/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/
/******************************************************************
  This file implements a basic timeline [test] app using the libfossil
  API.
*/

#include "libfossil.h"

static struct TLApp_ {
  int limit;
  bool showFiles;
  bool utc;
  char const * ckoutUuid;
  const char * filterTag;
  const char * filterBranch;
  const char * filterComment;
} TLApp = {
-1/*limit*/,
0/*showFiles*/,
0/*utc*/,
NULL/*ckoutUuid*/,
NULL/*filterTag*/,
NULL/*filterBranch*/,
NULL/*filterComment*/
};

/**
    fsl_stmt_each_f() implementation for a basic timeline view. The
    state parameter is ignored.
 */
static int stmt_each_f_timeline1( fsl_stmt * q, void * state fsl__unused ){
  fsl_cx * const f = fcli_cx();
  char const * x;
  char const * uuid = fsl_stmt_g_text(q,0/*uuid*/,NULL);
  char const * type = fsl_stmt_g_text(q,3/*type*/,NULL);
  bool const isCheckin = 'c'==*type;
  char const * zComment = fsl_stmt_g_text(q,5/*comment*/,NULL);
  char const * zPrefix = "";
  (void)state;
  switch(*type){
    case 'c': type = "checkin"; break;
    case 'w':{
      type = "wiki";
      if(zComment){
        switch(*zComment){
          case '+': zPrefix = "Added: "; ++zComment; break;
          case '-': zPrefix = "Deleted: "; ++zComment; break;
          case ':': zPrefix = "Edited: "; ++zComment; break;
          default: break;
        }
      }
      break;
    }
    case 'g': type = "tag"; break;
    case 'e': type = "technote"; break;
    case 't': type = "ticket"; break;
    case 'f': type = "forum"; break;
  };
  /* Tip to copy/pasters: q->rowCount can be used to determine what
     row number we're on. It starts counting at 1, not 0.
  */
  fsl_outputf(f, "%-9s[%.*s] @ %s by [%s]",
              type, 12, uuid,
              fsl_stmt_g_text(q,1/*time*/,NULL),
              fsl_stmt_g_text(q,2/*user*/,NULL)
              );
  if( (x = fsl_stmt_g_text(q,4/*branch*/,NULL)) ){
    fsl_outputf(f, " branch [%s]", x);
  }
  if(isCheckin && TLApp.ckoutUuid && 0==fsl_uuidcmp(uuid,TLApp.ckoutUuid)){
    fsl_outputf(f," *CURRENT*");
  }
  fsl_outputf(f, "\n\n\t%s%s\n",zPrefix, zComment);;
  if(isCheckin && TLApp.showFiles){
    fsl_stmt * st = NULL;
    fsl_db * db = fsl_cx_db_repo(f);
    int rc;
    char doneHead = 0;
    assert(db);
    rc = fsl_db_prepare_cached(db, &st,
                               "SELECT "
                               /*0*/"bf.uuid, "
                               /*1*/"filename.name fname, "
                               /*2*/"bf.size, "
                               /*3*/"mlink.pid, "
                               /*4*/"mlink.fid "
                               "FROM mlink, filename "
                               "LEFT JOIN blob bf -- FILE blob\n"
                               " ON bf.rid=mlink.fid "
                               "LEFT JOIN blob bm -- MANIFEST/checkin blob\n"
                               " ON bm.rid=mlink.mid "
                               "WHERE "
                               "bm.uuid = ? "
                               "AND filename.fnid=mlink.fnid "
                               /* "AND bf.rid=mlink.fid " */
                               /*"AND bm.rid=mlink.mid "*/
                               "ORDER BY filename.name %s",
                               fsl_cx_filename_collation(f));
    if(rc){
      return fsl_cx_uplift_db_error(f, db);
    }
    rc = fsl_stmt_bind_text(st, 1, uuid, -1, 0);
    assert(0==rc);
    while(FSL_RC_STEP_ROW==(rc=fsl_stmt_step(st))){
      char const * changeType;
      if(!doneHead){
        doneHead = 1;
        fsl_outputf(f,"\n\t%-11s%s %13s Name\n", "Change", "File UUID", "Size");
      }
      if(0==fsl_stmt_g_id(st, 4/*==>fid*/)){
        fsl_outputf(f, "\t%-35s%s\n", "REMOVED", 
                    fsl_stmt_g_text(st, 1, NULL));
        continue;
      }else if(0==fsl_stmt_g_id(st, 3/*==>pid*/)){
        changeType = "ADDED";
      }else{
        changeType = "MODIFIED";
      }
      fsl_outputf(f,"\t%-11s%.*s %10"PRIi64" %s\n",
                  changeType,
                  12, fsl_stmt_g_text(st, 0, NULL),
                  (int64_t)fsl_stmt_g_int64(st, 2),
                  fsl_stmt_g_text(st, 1, NULL));
    }
    fsl_stmt_cached_yield(st);
  }
  fsl_output(f, "\n", 1);
  return 0;
}

static int my_timeline(void){
  fsl_buffer sql = fsl_buffer_empty;
  fsl_cx * const f = fcli_cx();
  fsl_db * const db = fsl_cx_db_repo(f);
  int rc;
  fsl_id_t tagId = 0;
  fsl_buffer_appendf(&sql, "SELECT "
                     /*0*/"uuid AS uuid, "
                     /*1*/"datetime(event.mtime%s) AS timestampString, "
                     /*2*/"coalesce(euser, user) AS user, "
                     /*3*/"event.type AS eventType, "
                     /*4*/"(SELECT group_concat(substr(tagname,5), ',') FROM tag, tagxref "
                     "WHERE tagname GLOB 'sym-*' AND tag.tagid=tagxref.tagid "
                     "AND tagxref.rid=blob.rid AND tagxref.tagtype>0) as tags, "
                     /*5*/"coalesce(ecomment, comment) AS comment ",
                     TLApp.utc ? "" : ", 'localtime'");

  if(TLApp.filterBranch){
    fsl_buffer_append(&sql,
                     "FROM tag CROSS JOIN event CROSS JOIN blob"
                     " LEFT JOIN tagxref ON tagxref.tagid=tag.tagid "
                     " AND tagxref.tagtype>0 "
                     " AND tagxref.rid=blob.rid ",
                      -1);
  }else{
    fsl_buffer_append(&sql, "FROM event JOIN blob", -1);
  }
  fsl_buffer_append(&sql, " WHERE blob.rid=event.objid ", -1);

  /*
    TODO: filters:

    - timeframe
    - user
    - event type
  */
  bool tagErr = false;
  if(TLApp.filterBranch){
    char const * zBr = TLApp.filterBranch;
    char * zTag = fsl_mprintf("sym-%s", zBr);
    tagId = fsl_tag_id(f, zTag, false);
    tagErr = tagId<=0;
    fsl_free(zTag);
    zTag = NULL;
    if(tagId>0){
      // Adapted from: https://fossil-scm.org/home/info/32b11546c830e328
      fsl_buffer_appendf(&sql,
       " AND tag.tagname='branch'\n"
       " AND blob.rid IN (\n"/* Commits */
        "SELECT rid FROM tagxref NATURAL JOIN tag\n"
        "  WHERE tagtype>0 AND tagname='sym-%q'\n"
        "UNION\n"   /* Tags */
        "  SELECT srcid FROM tagxref WHERE origid IN (\n"
        "    SELECT rid FROM tagxref NATURAL JOIN tag\n"
        "    WHERE tagname='sym-%q')\n"
        "UNION\n" /* Branch wikis */
        "  SELECT objid FROM event WHERE comment LIKE '_branch/%q'\n"
        "UNION\n"  /* Checkin wikis */
        "  SELECT e.objid FROM event e\n"
        "  INNER JOIN blob b ON b.uuid=substr(e.comment, 10)\n"
        "    AND e.comment LIKE '_checkin/%%'\n"
        "  LEFT JOIN tagxref tx ON tx.rid=b.rid "
        "    AND tx.tagid=%d\n"
        "  WHERE tx.value=%Q\n"
        ")\n"
        /* No merge closures... */
        "AND (tagxref.value IS NULL OR tagxref.value='%q')",
        zBr, zBr, zBr, FSL_TAGID_BRANCH, zBr, zBr);
    }
  }
  if(TLApp.filterTag){
    tagId = fsl_tag_id(f, TLApp.filterTag, false);
    tagErr = tagId<=0;
    if(tagId>0){
      fsl_buffer_appendf(&sql,
                         " AND EXISTS(SELECT 1 FROM tagxref"
                         " WHERE tagid=%"FSL_ID_T_PFMT
                         " AND tagtype>0 AND rid=blob.rid)",
                         (fsl_id_t)tagId);
    }
  }
  if(tagErr){
    if(0==tagId){
      rc = fcli_err_set(FSL_RC_NOT_FOUND,"No such branch: %s",
                        TLApp.filterBranch);
    }else{
      rc = fsl_cx_err_get(f, NULL, NULL);
      assert(0!=rc);
    }
    goto end;
  }
  if(TLApp.filterComment){
    rc = fsl_buffer_appendf(&sql,
                            " AND lower(comment) GLOB lower('*%q*')",
                            TLApp.filterComment);
    if(rc) goto end;
  }

  fsl_buffer_appendf(&sql,
                     " ORDER BY event.mtime DESC");
  if(TLApp.limit > 0){
    fsl_buffer_appendf(&sql, " LIMIT %d", TLApp.limit);
  }
  rc = fsl_db_each(db, stmt_each_f_timeline1, NULL,
                   "%b", &sql);
  fsl_flush(f);
  end:
  fsl_buffer_clear(&sql);
  if(rc && db->error.code){
    rc = fsl_cx_uplift_db_error(f, db);
  }
  return rc;
}

int main(int argc, char const * const * argv ){
  int rc = 0;
  const char * zLimit = NULL;
  fsl_cx * f = NULL;
  fcli_cliflag FCliFlags[] = {
    FCLI_FLAG("n","limit","number",&zLimit,
              "Limit the results to this many. n=0 means unlimited. "
              "Negative values are ignored and use the default limit."),
    FCLI_FLAG_BOOL("f","files", &TLApp.showFiles,
                   "Include a list of files modified by each checkin."),
    FCLI_FLAG("c","comment", "text", &TLApp.filterComment,
              "Filters the list on the given comment string text."),
    FCLI_FLAG("b","branch","branch-name",&TLApp.filterBranch,
              "Filter results to those in the given branch."),
    FCLI_FLAG("t","tag","tag-name",&TLApp.filterTag,
              "Filter results to those with the given tag."),
    FCLI_FLAG_BOOL(0,"utc",&TLApp.utc,
                   "Use UTC time instead of local time."),
    fcli_cliflag_empty_m
  };
  fcli_help_info FCliHelp = {
  "Shows an overview of recent fossil repository change history.",
  NULL, NULL
  };

  rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  f = fcli_cx();

  if(zLimit){
    TLApp.limit = atoi(zLimit);
    zLimit = NULL;
  }
  if(TLApp.limit<0) TLApp.limit = 5;

  if((rc = fcli_has_unused_args(false))) goto end;
  rc = fcli_fingerprint_check(true);
  if(rc) goto end;

  fsl_ckout_version_info(f, NULL, &TLApp.ckoutUuid);

  if(!fsl_cx_db_repo(f)){
    rc = fsl_cx_err_set(f, FSL_RC_MISUSE, "Repo db required.");
    goto end;
  }
  rc = my_timeline();

  end:
  return fcli_end_of_main(rc);
}
