/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */ 
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/
/************************************************************************
  This file implements the annoate/blame/praise-related APIs.
*/
#include "fossil-scm/internal.h"
#include "fossil-scm/vpath.h"
#include "fossil-scm/checkout.h"
#include <assert.h>
#include <string.h>/*memset()*/

/* Only for debugging */
#include <stdio.h>
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

const fsl_annotate_opt fsl_annotate_opt_empty = fsl_annotate_opt_empty_m;

/*
** The status of an annotation operation is recorded by an instance
** of the following structure.
*/
typedef struct Annotator Annotator;
struct Annotator {
  fsl__diff_cx c;       /* The diff-engine context */
  fsl_buffer headVersion;/*starting version of the content*/
  struct AnnLine {  /* Lines of the original files... */
    const char *z;       /* The text of the line. Points into
                            this->headVersion. */
    short int n;         /* Number of bytes (omitting trailing \n) */
    short int iVers;     /* Level at which tag was set */
  } *aOrig;
  unsigned int nOrig;/* Number of elements in aOrig[] */
  unsigned int nVers;/* Number of versions analyzed */
  bool bMoreToDo;    /* True if the limit was reached */
  fsl_id_t origId;       /* RID for the zOrigin version */
  fsl_id_t showId;       /* RID for the version being analyzed */
  struct AnnVers {
    char *zFUuid;   /* File being analyzed */
    char *zMUuid;   /* Check-in containing the file */
    char *zUser;    /* Name of user who did the check-in */
    double mtime;   /* [event].[mtime] db entry */
  } *aVers;         /* For each check-in analyzed */
  unsigned int naVers; /* # of entries allocated in this->aVers */
  fsl_timer timer;
};

static const Annotator Annotator_empty = {
fsl__diff_cx_empty_m,
fsl_buffer_empty_m/*headVersion*/,
NULL/*aOrig*/,
0U/*nOrig*/, 0U/*nVers*/,
false/*bMoreToDo*/,
0/*origId*/,
0/*showId*/,
NULL/*aVers*/,
0U/*naVerse*/,
fsl_timer_empty_m
};

static void fsl__annotator_clean(Annotator * const a){
  unsigned i;
  fsl__diff_cx_clean(&a->c);
  for(i = 0; i < a->nVers; ++i){
    fsl_free(a->aVers[i].zFUuid);
    fsl_free(a->aVers[i].zMUuid);
    fsl_free(a->aVers[i].zUser);
  }
  fsl_free(a->aVers);
  fsl_free(a->aOrig);
  fsl_buffer_clear(&a->headVersion);
}

static uint64_t fsl__annotate_opt_difflags(fsl_annotate_opt const * const opt){
  uint64_t diffFlags = FSL_DIFF2_STRIP_EOLCR;
  if(opt->spacePolicy>0) diffFlags |= FSL_DIFF2_IGNORE_ALLWS;
  else if(opt->spacePolicy<0) diffFlags |= FSL_DIFF2_IGNORE_EOLWS;
  return diffFlags;
}

/**
   Initializes the annocation process by populating `a` from
   a->toAnnote, which must have been previously populated.  `a` must
   have already been cleanly initialized via copying from
   Annotator_empty and a->headVersion populated.  Returns 0 on success,
   else:

   - FSL_RC_RANGE if pInput is empty.
   - FSL_RC_OOM on OOM.

   Regardless of success or failure, `a` must eventually be passed
   to fsl__annotator_clean() to free up any resources.
*/
static int fsl__annotation_start(Annotator * const a,
                                 fsl_annotate_opt const * const opt){
  int rc;
  uint64_t const diffFlags = fsl__annotate_opt_difflags(opt);
  if(opt->spacePolicy>0){
    a->c.cmpLine = fsl_dline_cmp_ignore_ws;
  }else{
    assert(fsl_dline_cmp == a->c.cmpLine);
  }
  rc = fsl_break_into_dlines(fsl_buffer_cstr(&a->headVersion),
                             (fsl_int_t)a->headVersion.used,
                             (uint32_t*)&a->c.nTo, &a->c.aTo, diffFlags);
  if(rc) goto end;
  if(!a->c.nTo){
    rc = FSL_RC_RANGE;
    goto end;
  }
  a->aOrig = fsl_malloc( (fsl_size_t)(sizeof(a->aOrig[0]) * a->c.nTo) );
  if(!a->aOrig){
    rc = FSL_RC_OOM;
    goto end;
  }
  for(int i = 0; i < a->c.nTo; ++i){
    a->aOrig[i].z = a->c.aTo[i].z;
    a->aOrig[i].n = a->c.aTo[i].n;
    a->aOrig[i].iVers = -1;
  }
  a->nOrig = (unsigned)a->c.nTo;
  end:
  return rc;
}

/**
   The input pParent is the next most recent ancestor of the file
   being annotated.  Do another step of the annotation. On success
   return 0 and, if additional annotation is required, assign *doMore
   (if not NULL) to true.
*/
static int fsl__annotation_step(
  Annotator * const a,
  fsl_buffer const *pParent,
  int iVers,
  fsl_annotate_opt const * const opt
){
  int i, j, rc;
  int lnTo;
  uint64_t const diffFlags = fsl__annotate_opt_difflags(opt);

  /* Prepare the parent file to be diffed */
  rc = fsl_break_into_dlines(fsl_buffer_cstr(pParent),
                             (fsl_int_t)pParent->used,
                             (uint32_t*)&a->c.nFrom, &a->c.aFrom,
                             diffFlags);
  if(rc) goto end;
  else if( a->c.aFrom==0 ){
    return 0;
  }
  //MARKER(("Line #1: %.*s\n", (int)a->c.aFrom[0].n, a->c.aFrom[0].z));
  /* Compute the differences going from pParent to the file being
  ** annotated. */
  rc = fsl__diff_all(&a->c);
  if(rc) goto end;

  /* Where new lines are inserted on this difference, record the
  ** iVers as the source of the new line.
  */
  for(i=lnTo=0; i<a->c.nEdit; i+=3){
    int const nCopy = a->c.aEdit[i];
    int const nIns = a->c.aEdit[i+2];
    lnTo += nCopy;
    for(j=0; j<nIns; ++j, ++lnTo){
      if( a->aOrig[lnTo].iVers<0 ){
        a->aOrig[lnTo].iVers = iVers;
      }
    }
  }

  /* Clear out the diff results except for c.aTo, as that's pointed to
     by a->aOrig.*/
  fsl_free(a->c.aEdit);
  a->c.aEdit = 0;
  a->c.nEdit = 0;
  a->c.nEditAlloc = 0;

  /* Clear out the from file */
  fsl_free(a->c.aFrom);
  a->c.aFrom = 0;
  a->c.nFrom = 0;
  end:
  return rc;
}

/* MISSING(?) fossil(1) converts the diff inputs into utf8 with no
   BOM. Whether we really want to do that here or rely on the caller
   to is up for debate. If we do it here, we have to make the inputs
   non-const, which seems "wrong" for a library API. */
#define blob_to_utf8_no_bom(A,B) (void)0

static int fsl__annotate_file(fsl_cx * const f,
                              Annotator * const a,
                              fsl_annotate_opt const * const opt){
  int rc = FSL_RC_NYI;
  fsl_buffer step = fsl_buffer_empty /*previous revision*/;
  fsl_id_t cid = 0, fnid = 0; // , rid = 0;
  fsl_stmt q = fsl_stmt_empty;
  bool openedTransaction = false;
  fsl_db * const db = fsl_needs_repo(f);
  if(!db) return FSL_RC_NOT_A_REPO;
  rc = fsl_cx_txn_begin(f);
  if(rc) goto dberr;
  openedTransaction = true;

  fnid = fsl_db_g_id(db, 0,
                     "SELECT fnid FROM filename WHERE name=%Q %s",
                     opt->filename, fsl_cx_filename_collation(f));
  if(0==fnid){
    rc = fsl_cx_err_set(f, FSL_RC_NOT_FOUND,
                        "File not found in repository: %s",
                        opt->filename);
    goto end;
  }
  if(opt->versionRid>0){
    cid = opt->versionRid;
  }else{
    fsl_ckout_version_info(f, &cid, NULL);
    if(cid<=0){
      rc = fsl_cx_err_set(f, FSL_RC_NOT_A_CKOUT,
                          "Cannot determine version RID to "
                          "annotate from.");
      goto end;
    }
  }
  if(opt->originRid>0){
    rc = fsl_vpath_shortest_store_in_ancestor(f, cid, opt->originRid, NULL);
  }else{
    rc = fsl_compute_direct_ancestors(f, cid);
  }
  if(rc) goto end;

  rc = fsl_db_prepare(db, &q,
    "SELECT DISTINCT"
    "   (SELECT uuid FROM blob WHERE rid=mlink.fid),"
    "   (SELECT uuid FROM blob WHERE rid=mlink.mid),"
    "   coalesce(event.euser,event.user),"
    "   mlink.fid, event.mtime"
    "  FROM mlink, event, ancestor"
    " WHERE mlink.fnid=%" FSL_ID_T_PFMT
    "   AND ancestor.rid=mlink.mid"
    "   AND event.objid=mlink.mid"
    "   AND mlink.mid!=mlink.pid"
    " ORDER BY ancestor.generation;",
    fnid
  );
  if(rc) goto dberr;

  while(FSL_RC_STEP_ROW==fsl_stmt_step(&q)){
    if(a->nVers>=3){
      /* Process at least 3 rows before imposing any limit. That is
         historical behaviour inherited from fossil(1). */
      if(opt->limitMs>0 &&
         fsl_timer_cpu(&a->timer)/1000 >= opt->limitMs){
        a->bMoreToDo = true;
        break;
      }else if(opt->limitVersions>0 && a->nVers>=opt->limitVersions){
        a->bMoreToDo = true;
        break;
      }
    }
    char * zTmp = 0;
    char const * zCol = 0;
    fsl_size_t nCol = 0;
    fsl_id_t const rid = fsl_stmt_g_id(&q, 3);
    double const mtime = fsl_stmt_g_double(&q, 4);
    if(0==a->nVers){
      rc = fsl_content_get(f, rid, &a->headVersion);
      if(rc) goto end;
      blob_to_utf8_no_bom(&a->headVersion,0);
      rc = fsl__annotation_start(a, opt);
      if(rc) goto end;
      a->bMoreToDo = opt->originRid>0;
      a->origId = opt->originRid;
      a->showId = cid;
      assert(0==a->nVers);
      assert(NULL==a->aVers);
    }
    if(a->naVers==a->nVers){
      unsigned int const n = a->naVers ? a->naVers*3/2 : 10;
      void * const x = fsl_realloc(a->aVers, n*sizeof(a->aVers[0]));
      if(NULL==x){
        rc = FSL_RC_OOM;
        goto end;
      }
      a->aVers = x;
      a->naVers = n;
    }
#define AnnStr(COL,FLD) zCol = NULL; \
    rc = fsl_stmt_get_text(&q, COL, &zCol, &nCol);  \
    if(rc) goto end;                                \
    else if(!zCol){ goto end;                                           \
      /*zCol=""; nCol=0; //causes downstream 'RID 0 is invalid' error*/}  \
    zTmp = fsl_strndup(zCol, (fsl_int_t)nCol);  \
    if(!zTmp){ rc = FSL_RC_OOM; goto end; } \
    a->aVers[a->nVers].FLD = zTmp
    AnnStr(0,zFUuid);
    AnnStr(1,zMUuid);
    AnnStr(2,zUser);
#undef AnnStr
    a->aVers[a->nVers].mtime = mtime;
    if( a->nVers>0 ){
      rc = fsl_content_get(f, rid, &step);
      if(!rc){
        rc = fsl__annotation_step(a, &step, a->nVers-1, opt);
      }
      fsl_buffer_reuse(&step);
      if(rc) goto end;
    }
    ++a->nVers;
  }

  assert(0==rc);
  if(0==a->nVers){
    if(opt->versionRid>0){
      rc = fsl_cx_err_set(f, FSL_RC_NOT_FOUND,
                          "File [%s] does not exist "
                          "in checkin RID %" FSL_ID_T_PFMT,
                          opt->filename, opt->versionRid);
    }else{
      rc = fsl_cx_err_set(f, FSL_RC_NOT_FOUND,
                          "No history found for file: %s",
                          opt->filename);
    }
  }
  
  end:
  fsl_buffer_clear(&step);
  fsl_stmt_finalize(&q);
  if(openedTransaction) fsl_cx_txn_end(f, rc!=0);
  return rc;
  dberr:
  assert(openedTransaction);
  assert(rc!=0);
  fsl_stmt_finalize(&q);
  fsl_buffer_clear(&step);
  rc = fsl_cx_uplift_db_error2(f, db, rc);
  if(openedTransaction) fsl_cx_txn_end(f, rc!=0);
  return rc;
}

int fsl_annotate_step_f_fossilesque(void * state,
                                    fsl_annotate_opt const * const opt,
                                    fsl_annotate_step const * const step){
  static const int szHash = 10;
  fsl_outputer const * fout = (fsl_outputer*)state;
  int rc = 0;
  char ymd[24];
  if(step->mtime>0){
    fsl_julian_to_iso8601(step->mtime, &ymd[0], false);
    ymd[10] = 0;
  }
  switch(step->stepType){
    case FSL_ANNOTATE_STEP_VERSION:
      rc = fsl_appendf(fout->out, fout->state,
                       "version %3d: %s %.*s file %.*s\n",
                       step->stepNumber+1, ymd, szHash,
                       step->versionHash, szHash, step->fileHash);
      break;
    case FSL_ANNOTATE_STEP_FULL:
      if(opt->praise){
        rc = fsl_appendf(fout->out, fout->state,
                         "%.*s %s %13.13s: %.*s\n",
                         szHash,
                         opt->fileVersions ? step->fileHash : step->versionHash,
                         ymd, step->username,
                         (int)step->lineLength, step->line);
      }else{
        rc = fsl_appendf(fout->out, fout->state,
                         "%.*s %s %5d: %.*s\n",
                         szHash, opt->fileVersions ? step->fileHash : step->versionHash,
                         ymd, step->lineNumber,
                         (int)step->lineLength, step->line);
      }
      break;
    case FSL_ANNOTATE_STEP_LIMITED:
      if(opt->praise){
        rc = fsl_appendf(fout->out, fout->state,
                         "%*s %.*s\n", szHash+26, "",
                         (int)step->lineLength, step->line);
      }else{
        rc = fsl_appendf(fout->out, fout->state,
                         "%*s %5" PRIu32 ": %.*s\n",
                         szHash+11, "", step->lineNumber,
                         (int)step->lineLength, step->line);
      }
      break;
  }
  return rc;
}


int fsl_annotate( fsl_cx * const f, fsl_annotate_opt const * const opt ){
  int rc;
  Annotator ann = Annotator_empty;
  unsigned int i;
  fsl_buffer * const scratch = fsl__cx_scratchpad(f);
  fsl_annotate_step aStep;

  if(!opt->out){
    return fsl_cx_err_set(f, FSL_RC_MISUSE,
                          "fsl_annotate_opt is missing its output function.");
  }
  if(opt->limitMs>0) fsl_timer_start(&ann.timer);
  rc = fsl__annotate_file(f, &ann, opt);
  if(rc) goto end;
  memset(&aStep,0,sizeof(fsl_annotate_step));

  if(opt->dumpVersions){
    struct AnnVers *av;
    for(av = ann.aVers, i = 0;
        0==rc && i < ann.nVers; ++i, ++av){
      aStep.fileHash = av->zFUuid;
      aStep.versionHash = av->zMUuid;
      aStep.mtime = av->mtime;
      aStep.stepNumber = i;
      aStep.stepType = FSL_ANNOTATE_STEP_VERSION;
      rc = opt->out(opt->outState, opt, &aStep);
    }
    if(rc) goto end;
  }

  for(i = 0; 0==rc && i<ann.nOrig; ++i){
    short iVers = ann.aOrig[i].iVers;
    char const * z = ann.aOrig[i].z;
    int const n = ann.aOrig[i].n;
    if(iVers<0 && !ann.bMoreToDo){
      iVers = ann.nVers-1;
    }
    fsl_buffer_reuse(scratch);
    rc = fsl_buffer_append(scratch, z, n);
    if(rc) break;
    aStep.stepNumber = iVers;
    ++aStep.lineNumber;
    aStep.line = fsl_buffer_cstr(scratch);
    aStep.lineLength = (uint32_t)scratch->used;

    if(iVers>=0){
      struct AnnVers * const av = &ann.aVers[iVers];
      aStep.fileHash = av->zFUuid;
      aStep.versionHash = av->zMUuid;
      aStep.mtime = av->mtime;
      aStep.username = av->zUser;
      aStep.stepType = FSL_ANNOTATE_STEP_FULL;
    }else{
      aStep.fileHash = aStep.versionHash =
        aStep.username = NULL;
      aStep.stepType = FSL_ANNOTATE_STEP_LIMITED;
      aStep.mtime = 0.0;
    }
    rc = opt->out(opt->outState, opt, &aStep);
  }
  
  end:
  fsl__cx_scratchpad_yield(f, scratch);
  fsl__annotator_clean(&ann);
  return rc;
}

#undef MARKER
#undef blob_to_utf8_no_bom
