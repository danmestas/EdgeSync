/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/
/***********************************************************************
  This file implements most of the fsl_repo_xxx() APIs.
*/
#include "fossil-scm/internal.h"
#include "fossil-scm/repo.h"
#include "fossil-scm/checkout.h"
#include "fossil-scm/hash.h"
#include "fossil-scm/confdb.h"
#include "fossil-scm/deprecated.h"
#include <assert.h>
#include <memory.h> /* memcpy() */
#include <time.h> /* time() */
#include <errno.h>

/* Only for debugging */
#include <stdio.h>
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

/**
   Calculate the youngest ancestor of the given blob.rid value that is a member of
   branch zBranch.

   Returns the blob.id value of the matching record, 0 if not found,
   or a negative value on error.

   Potential TODO: do we need this in the public API?
*/
static fsl_id_t fsl__youngest_ancestor_in_branch(fsl_cx * f, fsl_id_t rid,
                                                 const char *zBranch){
  fsl_db * const db = fsl_needs_repo(f);
  if(!db) return (fsl_id_t)-1;
  return fsl_db_g_id(db, 0,
    "WITH RECURSIVE "
    "  ancestor(rid, mtime) AS ("
    "    SELECT %"FSL_ID_T_PFMT", "
    "      mtime FROM event WHERE objid=%"FSL_ID_T_PFMT
    "    UNION "
    "    SELECT plink.pid, event.mtime"
    "      FROM ancestor, plink, event"
    "     WHERE plink.cid=ancestor.rid"
    "       AND event.objid=plink.pid"
    "     ORDER BY mtime DESC"
    "  )"
    "  SELECT ancestor.rid FROM ancestor"
    "   WHERE EXISTS(SELECT 1 FROM tagxref"
                    " WHERE tagid=%d AND tagxref.rid=ancestor.rid"
                    "   AND value=%Q AND tagtype>0)"
    "  ORDER BY mtime DESC"
    "  LIMIT 1",
    rid, rid, FSL_TAGID_BRANCH, zBranch
  );
}

int fsl_branch_of_rid(fsl_cx * const f, fsl_id_t rid,
                      bool doFallback, char ** zOut ){
  char *zBr = 0;
  fsl_db * const db = fsl_cx_db_repo(f);
  fsl_stmt st = fsl_stmt_empty;
  int rc;
  if(!fsl_needs_repo(f)) return FSL_RC_NOT_A_REPO;
  assert(db);
  rc = fsl_cx_prepare(f, &st,
      "SELECT value FROM tagxref "
      "WHERE rid=%" FSL_ID_T_PFMT " AND tagid=%d "
      "AND tagtype>0 "
      "/*%s()*/", rid, FSL_TAGID_BRANCH,__func__);
  if(rc) return rc;
  if( fsl_stmt_step(&st)==FSL_RC_STEP_ROW ){
    zBr = fsl_strdup(fsl_stmt_g_text(&st,0,0));
    if(!zBr) rc = FSL_RC_OOM;
  }
  fsl_stmt_finalize(&st);
  if( !rc ){
    if( zBr==0 && doFallback ){
      zBr = fsl_config_get_text(f, FSL_CONFDB_REPO, "main-branch", 0);
      if(!zBr){
        fsl_cx_err_reset(f)/* assume FSL_RC_NOT_FOUND */;
        zBr = fsl_strdup("trunk");
        if(!zBr) rc = FSL_RC_OOM;
      }
    }
    if(0==rc) *zOut = zBr;
  }
  return rc;
}

/**
   mrewt ==> most recent event with tag

   Comments from original fossil implementation:

   Find the RID of the most recent object with symbolic tag zTag and
   having a type that matches zType.

   Return 0 if there are no matches.

   This is a tricky query to do efficiently.  If the tag is very
   common (ex: "trunk") then we want to use the query identified below
   as Q1 - which searching the most recent EVENT table entries for the
   most recent with the tag.  But if the tag is relatively scarce
   (anything other than "trunk", basically) then we want to do the
   indexed search show below as Q2.
*/
static fsl_id_t fsl__mrewt(fsl_cx * const f, const char *zTag, fsl_satype_e type){
  char const * zType = fsl_satype_event_cstr(type);
  return fsl_db_g_id(fsl_cx_db_repo(f), 0,
    "SELECT objid FROM ("
      /* Q1:  Begin by looking for the tag in the 30 most recent events */
      "SELECT objid"
       " FROM (SELECT * FROM event ORDER BY mtime DESC LIMIT 30) AS ex"
      " WHERE type GLOB '%q'"
        " AND EXISTS(SELECT 1 FROM tagxref, tag"
                     " WHERE tag.tagname='sym-%q'"
                       " AND tagxref.tagid=tag.tagid"
                       " AND tagxref.tagtype>0"
                       " AND tagxref.rid=ex.objid)"
      " ORDER BY mtime DESC LIMIT 1"
    ") UNION ALL SELECT * FROM ("
      /* Q2: If the tag is not found in the 30 most recent events, then using
      ** the tagxref table to index for the tag */
      "SELECT event.objid"
       " FROM tag, tagxref, event"
      " WHERE tag.tagname='sym-%q'"
        " AND tagxref.tagid=tag.tagid"
        " AND tagxref.tagtype>0"
        " AND event.objid=tagxref.rid"
        " AND event.type GLOB '%q'"
      " ORDER BY event.mtime DESC LIMIT 1"
    ") LIMIT 1;",
    zType, zTag, zTag, zType
  );
}

/**
   Modes for fsl__start_of_branch().

   These values are hard-coded and must retain these values,
   else queries will break.
*/
enum fsl__stobr_type {
/**
   The check-in of the parent branch off of which
   the branch containing RID originally diverged.
*/
FSL__STOBR_ORIGIN = 0,
/**
   The first check-in of the branch that contains RID.
*/
FSL__STOBR_FIRST_CI = 1,
/**
   The youngest ancestor of RID that is on the branch from which the
   branch containing RID diverged.
*/
FSL__STOBR_YOAN = 2
};

/*
** Return the RID that is the "root" of the branch that contains
** check-in "rid".  Details depending on eType. If not found, rid is
** returned.
*/
static fsl_id_t fsl__start_of_branch(fsl_cx * const f, fsl_id_t rid,
                                     enum fsl__stobr_type eType){
  fsl_stmt q = fsl_stmt_empty;
  int rc;
  fsl_id_t ans = rid;
  char * zBr = 0;
  rc = fsl_branch_of_rid(f, rid, true, &zBr);
  if(rc) return -1;
  rc = fsl_cx_prepare(f, &q,
    "WITH RECURSIVE"
    "  par(pid, ex, cnt) as ("
    "    SELECT pid, EXISTS(SELECT 1 FROM tagxref"
    "                        WHERE tagid=%d AND tagtype>0"
    "                          AND value=%Q AND rid=plink.pid), 1"
    "    FROM plink WHERE cid=%"FSL_ID_T_PFMT" AND isprim"
    "    UNION ALL "
    "    SELECT plink.pid, EXISTS(SELECT 1 FROM tagxref "
    "                              WHERE tagid=%d AND tagtype>0"
    "                                AND value=%Q AND rid=plink.pid),"
    "           1+par.cnt"
    "      FROM plink, par"
    "     WHERE cid=par.pid AND isprim AND par.ex "
    "     LIMIT 100000 "
    "  )"
    " SELECT pid FROM par WHERE ex>=%d ORDER BY cnt DESC LIMIT 1",
    FSL_TAGID_BRANCH, zBr, ans, FSL_TAGID_BRANCH, zBr, eType%2
  );
  fsl_free(zBr);
  zBr = 0;
  if(rc){
    ans = -2;
    MARKER(("Internal error: fsl_db_prepare() says: %s\n", fsl_rc_cstr(rc)));
    goto end;
  }
  if( FSL_RC_STEP_ROW == fsl_stmt_step(&q) ) {
    ans = fsl_stmt_g_id(&q, 0);
  }
  fsl_stmt_finalize(&q);
  end:
  if( ans>0 && eType==FSL__STOBR_YOAN ){
    rc = fsl_branch_of_rid(f, ans, true, &zBr);
    if(rc) goto oom;
    else{
      ans = fsl__youngest_ancestor_in_branch(f, rid, zBr);
      fsl_free(zBr);
    }
  }
  return ans;
  oom:
  if(!f->error.code){
    fsl_cx_err_set(f, FSL_RC_OOM, NULL);
  }/* Else assume the OOM is really a misleading
      side-effect of another failure. */
  return -1;
}

int fsl_sym_to_rid( fsl_cx * const f, char const * sym,
                    fsl_satype_e type, fsl_id_t * const rv ){
  fsl_id_t rid = 0;
  fsl_id_t vid;
  fsl_size_t symLen;
  /* fsl_int_t i; */
  fsl_db * const dbR = fsl_cx_db_repo(f);
  fsl_db * const dbC = fsl_cx_db_ckout(f);
  bool startOfBranch = 0;
  int rc = 0;

  if(!sym || !*sym || !rv) return FSL_RC_MISUSE;
  else if(!dbR) return FSL_RC_NOT_A_REPO;

  if(FSL_SATYPE_BRANCH_START==type){
    /* The original implementation takes a (char const *) for the
       type, and treats "b" (branch?) as a special case of
       FSL_SATYPE_CHECKIN, resets the type to "ci", then sets
       startOfBranch to 1. We introduced the FSL_SATYPE_BRANCH
       pseudo-type for that purpose. That said: the original code
       base does not, as of this writing (2021-02-15) appear to actually
       use this feature anywhere. */
    type = FSL_SATYPE_CHECKIN;
    startOfBranch = 1;
  }

  /* special keyword: "tip" */
  if( 0==fsl_strcmp(sym,"tip")
      && (FSL_SATYPE_ANY==type || FSL_SATYPE_CHECKIN==type)){
    rid = fsl_db_g_id(dbR, 0,
                      "SELECT objid FROM event"
                      " WHERE type='ci'"
                      " ORDER BY event.mtime DESC"
                      " LIMIT 1");
    if(rid>0) goto gotit;
  }
  /* special keywords: "prev", "previous", "current", and "next".
     These require a checkout.
  */
  vid = dbC ? f->db.ckout.rid : 0;
  //MARKER(("has vid=%"FSL_ID_T_PFMT"\n", vid));
  if( vid>0){
    if( 0==fsl_strcmp(sym, "current") ){
      rid = vid;
    }
    else if( 0==fsl_strcmp(sym, "prev")
             || 0==fsl_strcmp(sym, "previous") ){
      rid = fsl_db_g_id(dbR, 0,
                        "SELECT pid FROM plink WHERE "
                        "cid=%"FSL_ID_T_PFMT" AND isprim",
                        (fsl_id_t)vid);
    }
    else if( 0==fsl_strcmp(sym, "next") ){
      rid = fsl_db_g_id(dbR, 0,
                        "SELECT cid FROM plink WHERE "
                        "pid=%"FSL_ID_T_PFMT
                        " ORDER BY isprim DESC, mtime DESC",
                        (fsl_id_t)vid);
    }
    if(rid>0) goto gotit;
  }

  /* Date and times */
  if( 0==memcmp(sym, "date:", 5) ){
    rid = fsl_db_g_id(dbR, 0,
                      "SELECT objid FROM event"
                      " WHERE mtime<=julianday(%Q,'utc')"
                      " AND type GLOB '%q'"
                      " ORDER BY mtime DESC LIMIT 1",
                      sym+5, fsl_satype_event_cstr(type));
    *rv = rid;
    return 0;
  }
  if( fsl_str_is_date(sym) ){
    rid = fsl_db_g_id(dbR, 0,
                      "SELECT objid FROM event"
                      " WHERE mtime<=julianday(%Q,'utc')"
                      " AND type GLOB '%q'"
                      " ORDER BY mtime DESC LIMIT 1",
                      sym, fsl_satype_event_cstr(type));
    if(rid>0) goto gotit;
  }

  /* Deprecated time formats elided: local:..., utc:... */

  /* "tag:" + symbolic-name */
  if( memcmp(sym, "tag:", 4)==0 ){
    rid = fsl__mrewt(f, sym+4, type);
    if(rid>0 && startOfBranch){
      rid = fsl__start_of_branch(f, rid, FSL__STOBR_FIRST_CI);
    }
    goto gotit;
  }

  /* root:TAG -> The origin of the branch */
  if( memcmp(sym, "root:", 5)==0 ){
    rc = fsl_sym_to_rid(f, sym+5, type, &rid);
    if(!rc && rid>0){
      rid = fsl__start_of_branch(f, rid, FSL__STOBR_ORIGIN);
    }
    goto gotit;
  }

  /* start:TAG -> The first check-in on branch named TAG */
  if( strncmp(sym, "start:", 6)==0 ){
    rc = fsl_sym_to_rid(f, sym+6, type, &rid);
    if(!rc && rid>0){
      rid = fsl__start_of_branch(f, rid, FSL__STOBR_FIRST_CI);
    }
    goto gotit;
  }

  /* merge-in:TAG -> Most recent merge-in for the branch */
  if( memcmp(sym, "merge-in:", 9)==0 ){
    rc = fsl_sym_to_rid(f, sym+9, type, &rid);
    if(!rc){
      rid = fsl__start_of_branch(f, rid, FSL__STOBR_YOAN);
    }
    goto gotit;
  }

  symLen = fsl_strlen(sym);
  /* SHA1/SHA3 hash or prefix */
  if( symLen>=4
      && symLen<=FSL_STRLEN_K256
      && fsl_validate16(sym, symLen) ){
    fsl_stmt q = fsl_stmt_empty;
    char zUuid[FSL_STRLEN_K256+1];
    memcpy(zUuid, sym, symLen);
    zUuid[symLen] = 0;
    fsl_canonical16(zUuid, symLen);
    rid = 0;
    /* Reminder to self: caching these queries would be cool. */
    if( FSL_SATYPE_ANY==type ){
      fsl_db_prepare(dbR, &q,
                       "SELECT rid FROM blob WHERE uuid GLOB '%s*'",
                       zUuid);
    }else{
      fsl_db_prepare(dbR, &q,
                     "SELECT blob.rid"
                     "  FROM blob, event"
                     " WHERE blob.uuid GLOB '%s*'"
                     "   AND event.objid=blob.rid"
                     "   AND event.type GLOB '%q'",
                     zUuid, fsl_satype_event_cstr(type) );
    }
    if( fsl_stmt_step(&q)==FSL_RC_STEP_ROW ){
      int64_t r64 = 0;
      fsl_stmt_get_int64(&q, 0, &r64);
      if( fsl_stmt_step(&q)==FSL_RC_STEP_ROW ) rid = -1
        /* Ambiguous results */
        ;
      else rid = (fsl_id_t)r64;
    }
    fsl_stmt_finalize(&q);
    if(rid<0){
      fsl_cx_err_set(f, FSL_RC_AMBIGUOUS,
                     "Symbolic name is ambiguous: %s",
                     sym);
    }
    goto gotit
      /* None of the further checks against the sym can pass. */
      ;
  }

  if(FSL_SATYPE_WIKI==type){
    rid = fsl_db_g_id(dbR, 0,
                    "SELECT event.objid, max(event.mtime)"
                    "  FROM tag, tagxref, event"
                    " WHERE tag.tagname='sym-%q' "
                    "   AND tagxref.tagid=tag.tagid AND tagxref.tagtype>0 "
                    "   AND event.objid=tagxref.rid "
                    "   AND event.type GLOB '%q'",
                    sym, fsl_satype_event_cstr(type)
    );
  }else{
    rid = fsl__mrewt(f, sym, type);
    //MARKER(("mrewt(%s,%s) == %d\n", sym, fsl_satype_cstr(type), (int)rid));
  }

  if( rid>0 ){
    if(startOfBranch) rid = fsl__start_of_branch(f, rid,
                                                 FSL__STOBR_FIRST_CI);
    goto gotit;
  }

  /* Undocumented: rid:### ==> validate that ### is a known rid */
  if(symLen>4 && 0==fsl_strncmp("rid:",sym,4)){
    int i;
    char const * oldSym = sym;
    sym += 4;
    for(i=0; fsl_isdigit(sym[i]); i++){}
    if( sym[i]==0 ){
      /* It's an integer. */
      if( FSL_SATYPE_ANY==type ){
        rid = fsl_db_g_id(dbR, 0,
                          "SELECT rid"
                          "  FROM blob"
                          " WHERE rid=%s",
                          sym);
      }else{
        rid = fsl_db_g_id(dbR, 0,
                          "SELECT event.objid"
                          "  FROM event"
                          " WHERE event.objid=%s"
                          "   AND event.type GLOB '%q'",
                          sym, fsl_satype_event_cstr(type));
      }
      if( rid>0 ) goto gotit;
    }
    sym = oldSym;
  }

  gotit:
  if(rid<=0){
    return f->error.code
      ? f->error.code
      : fsl_cx_err_set(f, FSL_RC_NOT_FOUND,
                       "Could not resolve symbolic name "
                       "'%s' as artifact type '%s'.",
                       sym, fsl_satype_event_cstr(type) );
  }
  assert(0==rc);
  *rv = rid;
  return rc;
}

fsl_id_t fsl__uuid_to_rid2(fsl_cx * const f, fsl_uuid_cstr uuid,
                           fsl__phantom_e mode){
    if(!f) return -1;
    else if(!fsl_is_uuid(uuid)){
      fsl_cx_err_set(f, FSL_RC_MISUSE,
                     "fsl__uuid_to_rid2() requires a "
                     "full UUID. Got: %s", uuid);
      return -2;
    }else{
      fsl_id_t rv = fsl_uuid_to_rid(f, uuid);
      if((0==rv) && (FSL_PHANTOM_NONE!=mode)
         && 0!=fsl__phantom_new(f, uuid,
                                (FSL_PHANTOM_PRIVATE==mode),
                                &rv)){
        assert(f->error.code);
        rv = -3;
      }
      return rv;
    }
}

int fsl_sym_to_uuid( fsl_cx * const f, char const * sym, fsl_satype_e type,
                     fsl_uuid_str * const rv, fsl_id_t * const rvId ){
  fsl_id_t rid = 0;
  fsl_db * dbR = fsl_needs_repo(f);
  fsl_uuid_str rvv = NULL;
  int rc = dbR
    ? fsl_sym_to_rid(f, sym, type, &rid)
    : FSL_RC_NOT_A_REPO;
  if(!rc){
    if(rvId) *rvId = rid;
    rvv = fsl_rid_to_uuid(f, rid)
      /* TODO: use a cached "exists" check if !rv, to avoid allocating
         rvv if we don't need it.
      */;
    if(!rvv){
      if(!f->error.code){
        rc = fsl_cx_err_set(f, FSL_RC_NOT_FOUND,
                            "Cannot find UUID for RID %"FSL_ID_T_PFMT".",
                            rid);
      }
    }
    else if(rv){
      *rv = rvv;
    }else{
      fsl_free( rvv );
    }
  }
  return rc;
}

fsl_id_t fsl_uuid_to_rid( fsl_cx * const f, char const * uuid ){
  fsl_db * const db = fsl_needs_repo(f);
  fsl_size_t const uuidLen = (uuid && db) ? fsl_strlen(uuid) : 0;
  if(!uuid || !uuidLen) return -1;
  else if(!db){
    /* f's error state has already been set */
    assert(FSL_RC_NOT_A_REPO == f->error.code);
    return -2;
  }else if(!fsl_validate16(uuid, uuidLen)){
    fsl_cx_err_set(f, FSL_RC_RANGE, "Invalid UUID (prefix): %s", uuid);
    return -3;
  }else if(uuidLen>FSL_STRLEN_K256){
    fsl_cx_err_set(f, FSL_RC_RANGE, "UUID is too long: %s", uuid);
    return -4;
  }else{
    fsl_id_t rid = -5;
    fsl_stmt qGlob = fsl_stmt_empty;
    fsl_stmt * q = NULL;
    int rc = 0;
    bool const isGlob = !fsl_is_uuid_len((int)uuidLen);
    if(isGlob){
      q = &qGlob;
      /* Maintenance reminder: rumor has it that GLOB '%q*' is more
         efficient than GLOB %Q || '*', the latter, rumor says,
         requiring a full table scan in some cases which the former
         does not. i.e. don't cache this in f->stmt with the latter
         formulation. */
      rc = fsl_cx_prepare(f, q, "SELECT rid FROM blob"
                          " WHERE uuid GLOB '%q*' /*%s()*/",
                          uuid, __func__);
    }else{
      /* Optimization for the common internally-used case.*/
      q = fsl__cx_stmt(f, fsl__cx_stmt_e_uuidToRid);
      if( !q ) rc = f->error.code;
    }
    if(rc){
      rid = -10;
      goto end;
    }
    rc = (&qGlob==q)
      ? fsl_stmt_step(q)
      : fsl_stmt_bind_step_v2(q, "s", uuid);
    switch(rc){
      case FSL_RC_STEP_ROW:
        rc = 0;
        rid = fsl_stmt_g_id(q, 0);
        if(isGlob){
          /* Check for an ambiguous result. We don't need this for
             the !isGlob case because that one does an exact match
             on a unique key. */
          rc = fsl_stmt_step(q);
          switch(rc){
            case FSL_RC_STEP_ROW:
              rc = 0;
              fsl_cx_err_set(f, FSL_RC_AMBIGUOUS,
                             "UUID prefix is ambiguous: %s",
                             uuid);
              rid = -6;
              break;
            case FSL_RC_STEP_DONE:
              /* Unambiguous UUID */
              rc = 0;
              break;
            default:
              assert(q->db->error.code);
              break;
          }
        }
        break;
      case FSL_RC_STEP_DONE:
        rid = 0;
        rc = 0;
        break;
      default:
        assert(q->db->error.code);
        rid = -7;
        break;
    }
    if(rc && q->db->error.code && !f->error.code){
      fsl_cx_uplift_db_error(f, q->db);
    }
  end:
    if( &qGlob != q ) fsl__cx_stmt_yield(f, q);
    fsl_stmt_finalize(&qGlob);
    return rid;
  }
}

fsl_id_t fsl_repo_filename_fnid( fsl_cx * f, char const * fn ){
  fsl_id_t rv = 0;
  int const rc = fsl__repo_filename_fnid2(f, fn, &rv, false);
  return rv>=0 ? rv : (rc>0 ? -rc : rc);
}

int fsl__repo_filename_fnid2( fsl_cx * f, char const * fn, fsl_id_t * rv, bool createNew ){
  fsl_db * db = fsl_cx_db_repo(f);
  fsl_id_t fnid = 0;
  fsl_stmt * qSel = NULL;
  int rc;
  assert(f);
  assert(db);
  assert(rv);
  if(!fn || !fsl_is_simple_pathname(fn, 1)){
    return fsl_cx_err_set(f, FSL_RC_RANGE,
                          "Filename is not a \"simple\" path: %s",
                          fn);
  }
  *rv = 0;
  rc = fsl_db_prepare_cached(db, &qSel,
                             "SELECT fnid FROM filename "
                             "WHERE name=? "
                             "/*%s()*/",__func__);
  if(rc){
      fsl_cx_uplift_db_error(f, db);
      return rc;
  }
  rc = fsl_stmt_bind_text(qSel, 1, fn, -1, 0);
  if(rc){
    fsl_stmt_cached_yield(qSel);
  }else{
    rc = fsl_stmt_step(qSel);
    if( FSL_RC_STEP_ROW == rc ){
      rc = 0;
      fnid = fsl_stmt_g_id(qSel, 0);
      assert(fnid>0);
    }else if(FSL_RC_STEP_DONE == rc){
      rc = 0;
    }
    fsl_stmt_cached_yield(qSel);
    if(!rc && (fnid==0) && createNew){
      fsl_stmt * qIns = NULL;
      rc = fsl_db_prepare_cached(db, &qIns,
                                 "INSERT INTO filename(name) "
                                 "VALUES(?) /*%s()*/",__func__);
      if(!rc){
        rc = fsl_stmt_bind_text(qIns, 1, fn, -1, 0);
        if(!rc){
          rc = fsl_stmt_step(qIns);
          if(FSL_RC_STEP_DONE==rc){
            rc = 0;
            fnid = fsl_db_last_insert_id(db);
          }
        }
        fsl_stmt_cached_yield(qIns);
      }
    }
  }
  if(!rc){
    assert(!createNew || (fnid>0));
    *rv = fnid;
  }else if(db->error.code){
    fsl_cx_uplift_db_error(f, db);
  }
  return rc;
}

static int fsl__delta_id(fsl_cx * const f, fsl_stmt * const q,
                         fsl_id_t theId, fsl_id_t * const pOther){
  int rc;
  assert( pOther );
  if( !q ) return f->error.code;
#if FSL_API_ARMOR
  if(theId<=0) return FSL_RC_RANGE;
#endif
  rc = fsl_stmt_bind_step_v2(q, "R", theId);
  switch(rc){
    case FSL_RC_STEP_ROW:
      rc = 0;
      *pOther = fsl_stmt_g_id(q, 0);
      fsl_stmt_reset(q);
      break;
    case FSL_RC_STEP_DONE:
      rc = 0;
      *pOther = 0;
      break;
    default:
      fsl_cx_uplift_db_error2(f, q->db, rc);
      break;
  }
  fsl__cx_stmt_yield(f, q);
  return rc;
}

int fsl_delta_s2r( fsl_cx * const f, fsl_id_t deltaId,
                   fsl_id_t * const rv ){
  return fsl__delta_id(f, fsl__cx_stmt(f, fsl__cx_stmt_e_deltaS2R),
                       deltaId, rv);
}

int fsl_delta_r2s( fsl_cx * const f, fsl_id_t deltaId,
                   fsl_id_t * const rv ){
  return fsl__delta_id(f, fsl__cx_stmt(f, fsl__cx_stmt_e_deltaR2S),
                       deltaId, rv);
}

int fsl__delta_replace(fsl_cx * const f, fsl_id_t rid,
                       fsl_id_t srcid){
  fsl_stmt * const q = fsl__cx_stmt(f, fsl__cx_stmt_e_replaceDelta);
  assert( rid>0 );
  assert( srcid>0 );
  assert( srcid!=rid );
  assert( fsl_cx_txn_level(f) );
  if( !q ) return f->error.code;
  int const rc = fsl_stmt_bind_step_v2(q, "RR", rid, srcid);
  fsl__cx_stmt_yield(f, q);
  return (FSL_RC_STEP_DONE==rc)
    ? 0
    : fsl_cx_uplift_db_error2(f, q->db, rc);
}

int fsl__repo_verify_before_commit( fsl_cx * const f, fsl_id_t rid ){
  if(0){
    /*
       v1 adds a commit hook here on the first entry, but it only
       seems to ever use one commit hook, so the infrastructure seems
       like overkill here. Thus this final verification is called from
       the commit (that's where v1 calls the hook).

       If we eventually add commit hooks, this is the place to do it.
    */
  }
  assert( fsl_cx_db_repo(f)->impl.txn.level > 0 );
  assert(rid>0);
  return rid>0
    ? fsl__cx_ptl_insert(f, fsl__ptl_e_toVerify, rid)
    : FSL_RC_RANGE;
}

void fsl__repo_verify_cancel( fsl_cx * const f ){
  fsl_id_bag_reuse( fsl__cx_ptl_bag(f, fsl__ptl_e_toVerify) );
}

int fsl_rid_to_uuid2(fsl_cx * const f, fsl_id_t rid, fsl_buffer *uuid){
  int rc;
  fsl_db * const db = f ? fsl_needs_repo(f) : NULL;
  if( !db ){
    rc = FSL_RC_NOT_A_REPO;
  }else if( rid<=0 ){
    rc = fsl_cx_err_set(f, FSL_RC_MISUSE,
                        "fsl_rid_to_uuid2() requires "
                        "a positive RID value. rid=%" FSL_ID_T_PFMT,
                        rid);
  }else{
    fsl_stmt * const q = fsl__cx_stmt(f, fsl__cx_stmt_e_ridToUuid);
    if( q ){
      rc = fsl_stmt_bind_id(q, 1, rid);
      if(!rc){
        rc = fsl_stmt_step(q);
        if(FSL_RC_STEP_ROW==rc){
          fsl_size_t len = 0;
          char const * x = fsl_stmt_g_text(q, 0, &len);
          fsl_buffer_reuse(uuid);
          rc = fsl_buffer_append(uuid, x, (fsl_int_t)len);
        }else if(FSL_RC_STEP_DONE){
          rc = fsl_cx_err_set(f, FSL_RC_NOT_FOUND,
                              "No blob found for rid %" FSL_ID_T_PFMT ".",
                              rid);
        }
      }
      fsl__cx_stmt_yield(f, q);
      if(rc && !f->error.code){
        if(q->db->error.code){
          fsl_cx_uplift_db_error(f, q->db);
        }else{
          fsl_cx_err_set(f, rc, NULL);
        }
      }
    }else{
      rc = f->error.code;
    }
    fsl_stmt_reset(q);
  }
  return rc;
}

fsl_uuid_str fsl_rid_to_uuid(fsl_cx * const f, fsl_id_t rid){
  fsl_buffer uuid = fsl_buffer_empty;
  fsl_rid_to_uuid2(f, rid, &uuid);
  return fsl_buffer_take(&uuid);
}

fsl_uuid_str fsl_rid_to_artifact_uuid(fsl_cx * const f, fsl_id_t rid, fsl_satype_e type){
  fsl_db * db = f ? fsl_cx_db_repo(f) : NULL;
  if(!f || !db || (rid<=0)) return NULL;
  else{
    char * rv = NULL;
    fsl_stmt * st = NULL;
    int rc;
    rc = fsl_db_prepare_cached(db, &st,
                               "SELECT uuid FROM blob "
                               "WHERE rid=?1 AND EXISTS "
                               "(SELECT 1 FROM event"
                               " WHERE event.objid=?1 "
                               " AND event.type GLOB %Q)"
                               "/*%s()*/",
                               fsl_satype_event_cstr(type),
                               __func__);
    if(!rc){
      rc = fsl_stmt_bind_id(st, 1, rid);
      if(!rc){
        rc = fsl_stmt_step(st);
        if(FSL_RC_STEP_ROW==rc){
          fsl_size_t len = 0;
          char const * x = fsl_stmt_g_text(st, 0, &len);
          rv = x ? fsl_strndup(x, (fsl_int_t)len ) : NULL;
          if(x && !rv){
            fsl_cx_err_set(f, FSL_RC_OOM, NULL);
          }
        }else if(FSL_RC_STEP_DONE){
          fsl_cx_err_set(f, FSL_RC_NOT_FOUND,
                         "No %s artifact found with rid %"FSL_ID_T_PFMT".",
                         fsl_satype_cstr(type), (fsl_id_t) rid);
        }
      }
      fsl_stmt_cached_yield(st);
      if(rc && !f->error.code){
        fsl_cx_uplift_db_error(f, db);
      }
    }
    return rv;
  }
}

/**
   Load the record identified by rid. Make sure we can reproduce it
   without error.

   Return non-0 and set f's error state if anything goes wrong.  If
   this procedure returns 0 it means that everything looks OK.
 */
static int fsl__repo_verify_rid(fsl_cx * f, fsl_id_t rid){
  fsl_uuid_str uuid = NULL;
  fsl_buffer hash = fsl_buffer_empty;
  fsl_buffer content = fsl_buffer_empty;
  int rc;
  fsl_db * db;
  if( fsl_content_size(f, rid)<0 ){
    return 0 /* No way to verify phantoms */;
  }
  db = fsl_cx_db_repo(f);
  assert(db);
  uuid = fsl_rid_to_uuid(f, rid);
  if(!uuid){
    rc = fsl_cx_err_set(f, FSL_RC_NOT_FOUND,
                        "Could not find blob record for "
                        "rid #%"FSL_ID_T_PFMT".",
                        rid);
  }else{
    int const uuidLen = fsl_is_uuid(uuid);
    fsl_flag32_t const oldFlags = f->flags;
    f->flags &= ~FSL_CX_F_BLOB_CACHE
      /* Reading from the blob cache here would bypass the whole
         reason for this validation */;
    if(!uuidLen){
      rc = fsl_cx_err_set(f, FSL_RC_RANGE,
                          "Invalid uuid for rid #%"FSL_ID_T_PFMT": %s",
                          (fsl_id_t)rid, uuid);
    }else if( 0==(rc=fsl_content_get(f, rid, &content)) ){
      /* This test can fail for artifacts which have an SHA1 hash in a
         repo with an SHA3 policy. A test case from the main fossil
         repo: c7dd1de9f9539a5a859c2b41fe4560604a774476

         This test hashes it (in that repo) as SHA3. As a workaround,
         if the hash is an SHA1 the we will temporarily force the hash
         policy to SHA1, and similarly for SHA3. Lame, but nothing
         better currently comes to mind.

         TODO: change the signature of fsl_cx_hash_buffer() to
         optionally take a forced policy, or supply a similar function
         which does what we're doing below.

         2025-08-05: what was this even for?
      */
      fsl_hashpolicy_e const oldHashP = f->cxConfig.hashPolicy;
      f->cxConfig.hashPolicy = (uuidLen==FSL_STRLEN_SHA1)
        ? FSL_HPOLICY_SHA1 : FSL_HPOLICY_SHA3;
      rc = fsl_cx_hash_buffer(f, 0, &content, &hash);
      f->cxConfig.hashPolicy = oldHashP;
      if( !rc && 0!=fsl_uuidcmp(uuid, fsl_buffer_cstr(&hash)) ){
        fsl__bprc(rc);
        rc = fsl_cx_err_set(f, FSL_RC_CONSISTENCY,
                            "Hash (%b) of rid %" FSL_ID_T_PFMT
                            " does not match its uuid (%s)",
                            &hash, rid, uuid);
#if 1
        /* This output is specifically to assist in debugging sync
           code. */
        MARKER(("Offending blob expecting\n%s\nbut got\n%s\nContent:\n%.*s\n",
                uuid, fsl_buffer_cstr(&hash),
                (int)content.used, content.mem));
#endif
      }
    }
    f->flags = oldFlags;
  }
  fsl_free(uuid);
  fsl_buffer_clear(&hash);
  fsl_buffer_clear(&content);
  return rc;
}


int fsl__repo_verify_at_commit( fsl_cx * const f ){
  fsl_id_t rid;
  int rc = 0;
  fsl_id_bag * const bag = fsl__cx_ptl_bag(f, fsl__ptl_e_toVerify);
  fsl_id_bag * const debag = fsl__cx_ptl_bag(f, fsl__ptl_e_dephantomize);

  assert( f->cache.ptl.level>0 );

  extern int fsl__after_dephantomize(fsl_cx *, fsl_id_t, bool) /* content.c */;
  if( debag->entryCount ){
    rid = fsl_id_bag_first(debag);
    for( ; !rc && rid>0; rid = fsl_id_bag_next(debag, rid) ){
      rc = fsl__after_dephantomize(f, rid, false);
    }
    fsl_id_bag_reuse(debag);
    if( rc ) return rc;
  }

  if( !bag->entryCount ) return 0;
  //fsl__id_bag_dump(&f->cache.toVerify, NULL, __func__);
  //assert(!"here");

  assert( f->dbMain );
  rid = fsl_id_bag_first(bag);
  if( rid>0 ){
    assert( !f->cache.inFinalVerify );
    f->cache.inFinalVerify = 1;
    if( f->cxConfig.traceSql ){
      fsl_db_exec(f->dbMain,
                  "SELECT 'Starting verify-at-commit.'");
    }
    while( !rc && rid>0 ){
#if 0
      MARKER(("verifying RID %d at txn level %d\n", (int)rid,
              f->cache.ptl.level));
#endif
      rc = fsl__repo_verify_rid(f, rid);
      if(!rc) rid = fsl_id_bag_next(bag, rid);
    }
    if( 0!=rc ){
      fsl__cx_content_caches_clear(f)
        /* This is unfortunate but we don't yet have a way to
           cherry-pick failed items from that cache and this failure
           may have just invalidated some. */;
    }
    fsl_id_bag_reuse(bag);
    if( f->cxConfig.traceSql ){
      fsl_db_exec(f->dbMain,
                  "SELECT 'Ending verify-at-commit. rc=%R'", rc);
    }
    assert( 1==f->cache.inFinalVerify );
    if( rc && !f->error.code ){
      fsl_cx_err_set(f, rc,
                     "Error %R in %s() for rid %" FSL_ID_T_PFMT,
                     rc, __func__, rid);
    }
    f->cache.inFinalVerify = 0;
  }
  return rc;
}


static int fsl__repo_create_default_users(fsl_db * db, bool addOnlyUser,
                                          char const * defaultUser ){
  int rc = fsl_db_exec(db,
                       "INSERT OR IGNORE INTO user(login, info) "
                       "VALUES(%Q,'')", defaultUser);
  if(!rc){
    rc = fsl_db_exec(db,
                     "UPDATE user SET cap='s', pw=lower(hex(randomblob(3)))"
                     " WHERE login=%Q", defaultUser);
    if( !rc && !addOnlyUser ){
      fsl_db_exec_multi(db,
                        "INSERT OR IGNORE INTO user(login,pw,cap,info)"
                        "   VALUES('anonymous',hex(randomblob(8)),'hz',"
                        "          'Anon');"
                        "INSERT OR IGNORE INTO user(login,pw,cap,info)"
                        "   VALUES('nobody','','gjor','Nobody');"
                        "INSERT OR IGNORE INTO user(login,pw,cap,info)"
                        "   VALUES('developer','','dei','Dev');"
                        "INSERT OR IGNORE INTO user(login,pw,cap,info)"
                        "   VALUES('reader','','kptw','Reader');"
                        );
    }
  }
  return rc;
}

int fsl_repo_create(fsl_cx * f, fsl_repo_create_opt const * opt ){
  fsl_db * db = 0;
  fsl_cx F = fsl__cx_empty /* used if !f */;
  int rc = 0;
  char const * userName = 0;
  fsl_time_t const unixNow = (fsl_time_t)time(0);
  bool fileExists;
  bool inTrans = 0;
  if(!opt || !opt->filename) return FSL_RC_MISUSE;
  fileExists = 0 == fsl_file_access(opt->filename,0);
  if(fileExists && !opt->allowOverwrite){
    return f
      ? fsl_cx_err_set(f, FSL_RC_ALREADY_EXISTS,
                       "File already exists and "
                       "allowOverwrite is false: %s",
                       opt->filename)
      : FSL_RC_ALREADY_EXISTS;
  }
  if(f){
    rc = fsl_close_scm_dbs(f)
      /* Will fail if a transaction is active! */;
    switch(rc){
      case 0:
        break;
      default:
        return rc;
    }
  }else{
    f = &F;
    rc = fsl_cx_init( &f, NULL );
    if(rc){
      fsl_cx_finalize(f);
      return rc;
    }
  }
  /* We probably should truncate/unlink the file here
     before continuing, to ensure a clean slate.
  */
  if(fileExists){
    rc = fsl_file_unlink(opt->filename);
    if(rc){
      rc = fsl_cx_err_set(f, rc, "Cannot unlink existing repo file: %s",
                          opt->filename);
      goto end2;
    }
  }
  rc = fsl__cx_attach_role(f, opt->filename, FSL_DBROLE_REPO, true);
  //MARKER(("attach role rc=%s\n", fsl_rc_cstr(rc)));
  if(rc){
    goto end2;
  }
  db = fsl_cx_db(f);
  if(!f->db.repo.user){
    f->db.repo.user = fsl_user_name_guess()
      /* Ignore OOM error here - we'll use 'root'
         by default (but if we're really OOM here then
         the next op will fail).
      */;
  }
  userName = opt->username;

  rc = fsl_cx_txn_begin(f);
  if(rc) goto end1;
  inTrans = 1;
  /* Install the schemas... */
  rc = fsl_db_exec_multi(db, "%s; %s; %s; %s",
                         fsl_schema_repo1(),
                         fsl_schema_repo2(),
                         fsl_schema_ticket(),
                         fsl_schema_ticket_reports());
  if(rc) goto end1;

  if(1){
    /*
      Set up server-code and project-code...

      in fossil this is optional, so we will presumably eventually
      have to make it so here as well. Not yet sure where this routine
      is used in fossil (i.e. whether the option is actually
      exercised).
    */
    rc = fsl_db_exec_multi(db,
                           "INSERT OR IGNORE INTO %q.config (name,value,mtime) "
                           "VALUES ('server-code',"
                           "lower(hex(randomblob(20))),"
                           "%"PRIi64");",
                           db->name, (int64_t)unixNow);
    if( 0==rc && !opt->elideProjectCode ){
      rc = fsl_db_exec_multi(db,
                           "INSERT OR IGNORE INTO %q.config (name,value,mtime) "
                           "VALUES ('project-code',"
                           "lower(hex(randomblob(20))),"
                           "%"PRIi64");",
                           db->name, (int64_t)unixNow,
                           db->name, (int64_t)unixNow);
    }
    if(rc) goto end1;
  }

  /* Set some config vars ... */
  {
    fsl_stmt st = fsl_stmt_empty;
    rc = fsl_db_prepare(db, &st,
                        "INSERT INTO %q.config (name,value,mtime) "
                        "VALUES (?,?,%"PRIi64")",
                        db->name, (int64_t)unixNow);
    if(!rc){
      fsl_stmt_bind_int64(&st, 3, unixNow);
#define DBSET_STR(KEY,VAL) \
      fsl_stmt_bind_text(&st, 1, KEY, -1, 0);    \
      fsl_stmt_bind_text(&st, 2, VAL, -1, 0); \
      fsl_stmt_step(&st); \
      fsl_stmt_reset(&st)
      DBSET_STR("content-schema",FSL_CONTENT_SCHEMA);
      DBSET_STR("aux-schema",FSL_AUX_SCHEMA);
#undef DBSET_STR

#define DBSET_INT(KEY,VAL) \
      fsl_stmt_bind_text(&st, 1, KEY, -1, 0 );    \
      fsl_stmt_bind_int32(&st, 2, VAL); \
      fsl_stmt_step(&st); \
      fsl_stmt_reset(&st)

      DBSET_INT("autosync",1);
      DBSET_INT("localauth",0);
      DBSET_INT("timeline-plaintext", 1);

#undef DBSET_INT
      fsl_stmt_finalize(&st);
    }
  }

  rc = fsl__repo_create_default_users(db, false, userName);
  if(rc) goto end1;

  end1:
  if(db->error.code && !f->error.code){
    rc = fsl_cx_uplift_db_error(f, db);
  }
  if(inTrans){
    if( 0==rc ) rc = fsl_cx_txn_end_v2(f, true, false);
    else fsl_cx_txn_end_v2(f, false, false);
    inTrans = 0;
  }
  fsl_close_scm_dbs(f);
  db = 0;
  if(rc) goto end2;

  /**
      In order for injection of the first commit to go through
      cleanly (==without any ugly kludging of f->dbMain), we
      need to now open the new db so that it gets connected
      to f properly...
   */
  rc = fsl_repo_open( f, opt->filename );
  if(rc) goto end2;
  db = fsl_cx_db_repo(f);
  assert(db);
  assert(db == f->dbMain);

  if(!userName || !*userName){
    userName = fsl_cx_user_get(f);
    if(!userName || !*userName){
      userName = "root" /* historical value */;
    }
  }

  /*
    Copy config...

    This is done in the second phase because...

    "cannot ATTACH database within transaction"

    and installing the initial schemas outside a transaction is
    horribly slow.
  */
  if( opt->configRepo && *opt->configRepo ){
    bool inTrans2 = false;
    char * inopConfig = fsl__config_inop_rhs(FSL_CONFIGSET_ALL);
    char * inopDb = inopConfig ? fsl__db_setting_inop_rhs() : NULL;
    if(!inopConfig || !inopDb){
      fsl_free(inopConfig);
      rc = FSL_RC_OOM;
      goto end2;
    }
    rc = fsl_db_attach(db, opt->configRepo, "settingSrc");
    if(rc){
      fsl_cx_uplift_db_error(f, db);
      goto end2;
    }
    rc = fsl_db_txn_begin(db);
    if(rc){
      fsl_cx_uplift_db_error(f, db);
      goto detach;
    }
    inTrans2 = 1;
    /*
       Copy all settings from the supplied template repository.
    */
    rc = fsl_db_exec(db,
                     "INSERT OR REPLACE INTO %q.config"
                     " SELECT name,value,mtime FROM settingSrc.config"
                     "  WHERE (name IN %s OR name IN %s)"
                     "    AND name NOT GLOB 'project-*';",
                     db->name, inopConfig, inopDb);
    if(rc) goto detach;
    rc = fsl_db_exec(db,
                     "REPLACE INTO %q.reportfmt "
                     "SELECT * FROM settingSrc.reportfmt;",
                     db->name);
    if(rc) goto detach;

    /*
       Copy the user permissions, contact information, last modified
       time, and photo for all the "system" users from the supplied
       template repository into the one being setup.  The other
       columns are not copied because they contain security
       information or other data specific to the other repository.
       The list of columns copied by this SQL statement may need to be
       revised in the future.
    */
    rc = fsl_db_exec(db, "UPDATE %q.user SET"
      "  cap = (SELECT u2.cap FROM settingSrc.user u2"
      "         WHERE u2.login = user.login),"
      "  info = (SELECT u2.info FROM settingSrc.user u2"
      "          WHERE u2.login = user.login),"
      "  mtime = (SELECT u2.mtime FROM settingSrc.user u2"
      "           WHERE u2.login = user.login),"
      "  photo = (SELECT u2.photo FROM settingSrc.user u2"
      "           WHERE u2.login = user.login)"
      " WHERE user.login IN ('anonymous','nobody','developer','reader');",
      db->name);

    detach:
    fsl_free(inopConfig);
    fsl_free(inopDb);
    if(inTrans2){
      if(!rc) rc = fsl_db_txn_end(db,0);
      else fsl_db_txn_end(db,1);
    }
    fsl_db_detach(db, "settingSrc");
    if(rc) goto end2;
  }

  if(opt->commitMessage && *opt->commitMessage){
    /* Set up initial commit. */
    fsl_deck d = fsl_deck_empty;
    fsl_cx_err_reset(f);
    fsl_deck_init(f, &d, FSL_SATYPE_CHECKIN);
    rc = fsl_deck_C_set(&d, opt->commitMessage, -1);
    if(!rc) rc = fsl_deck_D_set(&d, fsl_db_julian_now(db));
    if(!rc) rc = fsl_deck_R_set(&d, FSL_MD5_INITIAL_HASH);
    if(!rc && opt->commitMessageMimetype && *opt->commitMessageMimetype){
      rc = fsl_deck_N_set(&d, opt->commitMessageMimetype, -1);
    }
    /* Reminder: setting tags in "wrong" (unsorted) order to
       test/assert that the sorting gets done automatically. */
    if(!rc) rc = fsl_deck_T_add(&d, FSL_TAGTYPE_PROPAGATING, NULL,
                                "sym-trunk", NULL);
    if(!rc) rc = fsl_deck_T_add(&d, FSL_TAGTYPE_PROPAGATING, NULL,
                                "branch", "trunk");
    if(!rc) rc =fsl_deck_U_set(&d, userName);
    if(!rc){
      rc = fsl_deck_save(&d, 0);
    }
    fsl_deck_finalize(&d);
  }

  end2:
  if(f == &F){
    fsl_cx_finalize(f);
    if(rc) fsl_file_unlink(opt->filename);
  }
  return rc;
}

static int fsl_repo_dir_names_rid( fsl_cx * const f, fsl_id_t rid,
                                   fsl_list * const tgt,
                                   bool addSlash){
  fsl_db * dbR = fsl_needs_repo(f);
  fsl_deck D = fsl_deck_empty;
  fsl_deck * d = &D;
  int rc = 0;
  fsl_stmt st = fsl_stmt_empty;
  fsl_buffer tname = fsl_buffer_empty;
  int count = 0;
  fsl_card_F const * fc;
  /*
    This is a poor-man's impl. A more efficient one would calculate
    the directory names without using the database.
  */
  assert(rid>0);
  assert(dbR);
  rc = fsl_deck_load_rid( f, d, rid, FSL_SATYPE_CHECKIN);
  if(rc){
    fsl_deck_clean(d);
    return rc;
  }
  rc = fsl_buffer_appendf(&tname,
                          "tmp_filelist_for_rid_%d",
                          (int)rid);
  if(rc) goto end;
  rc = fsl_deck_F_rewind(d);
  while( !rc && !(rc=fsl_deck_F_next(d, &fc)) && fc ){
    assert(fc->name && *fc->name);
    if(!st.stmt){
      rc = fsl_db_exec(dbR, "CREATE TEMP TABLE IF NOT EXISTS "
                       "\"%b\"(n TEXT UNIQUE ON CONFLICT IGNORE)",
                       &tname);
      if(!rc){
        rc = fsl_db_prepare(dbR, &st,
                            "INSERT INTO \"%b\"(n) "
                            "VALUES(fsl_dirpart(?,%d))",
                            &tname, addSlash ? 1 : 0);
      }
      if(rc) goto end;
      assert(st.stmt);
    }
    rc = fsl_stmt_bind_text(&st, 1, fc->name, -1, 0);
    if(!rc){
      rc = fsl_stmt_step(&st);
      if(FSL_RC_STEP_DONE==rc){
        ++count;
        rc = 0;
      }
    }
    fsl_stmt_reset(&st);
    fc = 0;
  }

  if(!rc && (count>0)){
    fsl_stmt_finalize(&st);
    rc = fsl_db_prepare(dbR, &st,
                        "SELECT n FROM \"%b\" WHERE n "
                        "IS NOT NULL ORDER BY n %s",
                        &tname,
                        fsl_cx_filename_collation(f));
    while( !rc && (FSL_RC_STEP_ROW==(rc=fsl_stmt_step(&st))) ){
      fsl_size_t nLen = 0;
      char const * name = fsl_stmt_g_text(&st, 0, &nLen);
      rc = 0;
      if(name){
        char * cp;
        assert(nLen);
        cp = fsl_strndup( name, (fsl_int_t)nLen );
        if(!cp){
          rc = FSL_RC_OOM;
          break;
        }
        rc = fsl_list_append(tgt, cp);
        if(rc){
          fsl_free(cp);
          break;
        }
      }
    }
    if(FSL_RC_STEP_DONE==rc) rc = 0;
  }

  end:
  if(rc && !f->error.code && dbR->error.code){
    fsl_cx_uplift_db_error(f, dbR);
  }
  fsl_stmt_finalize(&st);
  fsl_deck_clean(d);
  if(tname.used){
    fsl_db_exec(dbR, "DROP TABLE IF EXISTS \"%b\"", &tname);
  }
  fsl_buffer_clear(&tname);
  return rc;
}

int fsl_repo_dir_names( fsl_cx * const f, fsl_id_t rid, fsl_list * const tgt,
                        bool addSlash ){
  fsl_db * const db = fsl_needs_repo(f);
  if(!db) return FSL_RC_NOT_A_REPO;
  else if(!tgt) return FSL_RC_MISUSE;
  int rc = fsl_cx_txn_begin(f);
  if(rc) return rc;
  if(rid>=0){
    if( 0==rid ){
      /* Dir list for current checkout version */
      fsl__ckout_rid(f, &rid);
      if( !rid ){
        rc = fsl_cx_err_set(f, FSL_RC_RANGE,
                            "The rid argument is 0 (indicating "
                            "the current checkout), but there is "
                            "no opened checkout.");
        goto end;
      }
    }
    assert(rid>0);
    rc = fsl_repo_dir_names_rid(f, rid, tgt, addSlash);
  }else{
    /* Dir list across all versions */
    fsl_stmt s = fsl_stmt_empty;
    rc = fsl_db_prepare(db, &s,
                        "SELECT DISTINCT(fsl_dirpart(name,%d)) dname "
                        "FROM filename WHERE dname IS NOT NULL "
                        "ORDER BY dname", addSlash ? 1 : 0);
    if(rc){
      fsl_cx_uplift_db_error(f, db);
      assert(!s.stmt);
      goto end;
    }
    while( !rc && (FSL_RC_STEP_ROW==(rc=fsl_stmt_step(&s)))){
      fsl_size_t len = 0;
      char const * col = fsl_stmt_g_text(&s, 0, &len);
      char * cp = fsl_strndup( col, (fsl_int_t)len );
      if(!cp){
        rc = FSL_RC_OOM;
        break;
      }
      rc = fsl_list_append(tgt, cp);
      if(rc) fsl_free(cp);
    }
    if(FSL_RC_STEP_DONE==rc) rc = 0;
    fsl_stmt_finalize(&s);
  }
end:
  fsl_cx_txn_end_v2(f, false, false)/*we only read*/;
  return rc;
}

/* UNTESTED */
bool fsl_repo_is_readonly(fsl_cx const * f){
  if(!f || !f->dbMain) return false;
  else{
    int const roleId = f->db.ckout.db.dbh ? FSL_DBROLE_MAIN : FSL_DBROLE_REPO
      /* If CKOUT is attached, it is the main DB and REPO is ATTACHed. */
      ;
    char const * zRole = fsl_db_role_name(roleId);
    return sqlite3_db_readonly(f->dbMain->dbh, zRole) ? true : false;
  }
}

int fsl__repo_record_filename(fsl_cx * const f){
  fsl_db * dbR = fsl_needs_repo(f);
  fsl_db * dbC;
  fsl_db * dbConf;
  char const * zCDir;
  char const * zName = dbR ? dbR->filename : NULL;
  int rc;
  if(!dbR) return FSL_RC_NOT_A_REPO;
  fsl_buffer * const full = fsl__cx_scratchpad(f);
  assert(zName);
  assert(f);
  rc = fsl_file_canonical_name(zName, full, 0);
  if(rc){
    fsl_cx_err_set(f, rc, "Error %s canonicalizing filename: %s", zName);
    goto end;
  }

  /*
    If global config is open, write the repo db's name to it.
  */
  dbConf = fsl_cx_db_config(f);
  if(dbConf){
    int const dbRole = (f->dbMain==&f->db.gconfig.db)
      ? FSL_DBROLE_MAIN : FSL_DBROLE_CONFIG;
    rc = fsl_db_exec(dbConf,
                     "INSERT OR IGNORE INTO %s.global_config(name,value) "
                     "VALUES('repo:%q',1)",
                     fsl_db_role_name(dbRole),
                     fsl_buffer_cstr(full));
    if(rc){
      fsl_cx_uplift_db_error(f, dbConf);
      goto end;
    }
  }

  dbC = fsl_cx_db_ckout(f);
  if(dbC && (zCDir=f->db.ckout.dir)){
    /* If we have a checkout, update its repo's list of checkouts... */
    /* Assumption: if we have an opened checkout, dbR is ATTACHed with
       the role REPO. */
    int ro;
    assert(dbR);
    ro = sqlite3_db_readonly(dbR->dbh,
                             fsl_db_role_name(FSL_DBROLE_REPO));
    assert(ro>=0);
    if(!ro){
      fsl_buffer localRoot = fsl_buffer_empty;
      rc = fsl_file_canonical_name(zCDir, &localRoot, 1);
      if(0==rc){
        if(dbConf){
          /*
            If global config is open, write the checkout db's name to it.
          */
          int const dbRole = (f->dbMain==&f->db.gconfig.db)
            ? FSL_DBROLE_MAIN : FSL_DBROLE_CONFIG;
          rc = fsl_db_exec(dbConf,
                           "REPLACE INTO INTO %s.global_config(name,value) "
                           "VALUES('ckout:%q',1)",
                           fsl_db_role_name(dbRole),
                           fsl_buffer_cstr(&localRoot));
        }
        if(0==rc){
          /* We know that repo is ATTACHed to ckout here. */
          assert(dbR == dbC);
          rc = fsl_db_exec(dbR,
                           "REPLACE INTO %s.config(name, value, mtime) "
                           "VALUES('ckout:%q', 1, now())",
                           fsl_db_role_name(FSL_DBROLE_REPO),
                           fsl_buffer_cstr(&localRoot));
        }
      }
      fsl_buffer_clear(&localRoot);
    }
  }

  end:
  if(rc && !f->error.code && f->dbMain->error.code){
    fsl_cx_uplift_db_error(f, f->dbMain);
  }
  fsl__cx_scratchpad_yield(f, full);
  return rc;

}

char fsl_rid_is_a_checkin(fsl_cx * f, fsl_id_t rid){
  fsl_db * db = f ? fsl_cx_db_repo(f) : NULL;
  if(!db || (rid<0)) return 0;
  else if(0==rid){
    /* Corner case: empty repo */
    return !fsl_db_exists(db, "SELECT 1 FROM blob WHERE rid>0");
  }
  else{
    fsl_stmt * st = 0;
    char rv = 0;
    int rc = fsl_db_prepare_cached(db, &st,
                                   "SELECT 1 FROM event WHERE "
                                   "objid=? AND type='ci' "
                                   "/*%s()*/",__func__);
    if(!rc){
      rc = fsl_stmt_bind_id( st, 1, rid);
      if(!rc){
        rc = fsl_stmt_step(st);
        if(FSL_RC_STEP_ROW==rc){
          rv = 1;
        }
      }
      fsl_stmt_cached_yield(st);
    }
    if(db->error.code){
      fsl_cx_uplift_db_error(f, db);
    }
    return rv;
  }
}

int fsl_repo_extract( fsl_cx * const f, fsl_repo_extract_opt const * const opt_ ){
  if(!f || !opt_->callback) return FSL_RC_MISUSE;
  else if(!fsl_needs_repo(f)) return FSL_RC_NOT_A_REPO;
  else if(opt_->checkinRid<=0){
    return fsl_cx_err_set(f, FSL_RC_RANGE, "RID must be positive.");
  }else{
    int rc;
    fsl_deck mf = fsl_deck_empty;
    fsl_buffer * const content = opt_->extractContent
      ? fsl__cx_content_buffer(f)
      : NULL;
    fsl_id_t fid;
    fsl_repo_extract_state xst = fsl_repo_extract_state_empty;
    fsl_card_F const * fc = NULL;
    fsl_repo_extract_opt const opt = *opt_
      /* Copy in case the caller modifies it via their callback. If we
         find an interesting use for such modification then we can
         remove this copy. */;
    rc = fsl_deck_load_rid(f, &mf, opt.checkinRid, FSL_SATYPE_CHECKIN);
    if(rc) goto end;
    assert(mf.f==f);
    xst.f = f;
    xst.checkinRid = opt.checkinRid;
    xst.callbackState = opt.callbackState;
    xst.content = opt.extractContent ? content : NULL;
    /* Calculate xst.count.fileCount... */
    assert(0==xst.count.fileCount);
    if(mf.B.uuid){/*delta. The only way to count this reliably
                   is to walk though the whole card list. */
      rc = fsl_deck_F_rewind(&mf);
      while( !rc && !(rc=fsl_deck_F_next(&mf, &fc)) && fc){
        ++xst.count.fileCount;
      }
      if(rc) goto end;
      fc = NULL;
    }else{
      xst.count.fileCount = mf.F.used;
    }
    assert(0==xst.count.fileNumber);
    rc = fsl_deck_F_rewind(&mf);
    while( !rc && !(rc=fsl_deck_F_next(&mf, &fc)) && fc){
      assert(fc->uuid
             && "We shouldn't get F-card deletions via fsl_deck_F_next()");
      ++xst.count.fileNumber;
      fid = fsl_uuid_to_rid(f, fc->uuid);
      if(fid<0){
        assert(f->error.code);
        rc = f->error.code;
      }else if(!fid){
        rc = fsl_cx_err_set(f, FSL_RC_NOT_FOUND,
                            "Could not resolve RID for UUID: %s",
                            fc->uuid);
      }else if(opt.extractContent){
        fsl_buffer_reuse(content);
        rc = fsl_content_get(f, fid, content);
        //assert(FSL_RC_RANGE!=rc);
      }
      if(!rc){
        /** Call the callback. */
        xst.fCard = fc;
        assert(fid>0);
        xst.content = content;
        xst.fileRid = fid;
        rc = opt.callback( &xst );
        if(FSL_RC_BREAK==rc){
          rc = 0;
          break;
        }
      }
    }/* for-each-F-card loop */
    end:
    if(content) fsl__cx_content_buffer_yield(f);
    fsl_deck_finalize(&mf);
    return rc;
  }
}

int fsl__repo_import_blob( fsl_cx * f, fsl_input_f in, void * inState,
                           fsl_id_t * rid, fsl_uuid_str * uuid ){
  fsl_db * db = f ? fsl_needs_repo(f) : NULL;
  if(!f || !in) return FSL_RC_MISUSE;
  else if(!db) return FSL_RC_NOT_A_REPO;
  else{
    int rc;
    fsl_buffer buf = fsl_buffer_empty;
    rc = fsl_buffer_fill_from(&buf, in, inState);
    if(rc){
      rc = fsl_cx_err_set(f, rc,
                          "Error filling buffer from input source.");
    }else{
      fsl_id_t theRid = 0;
      rc = fsl__content_put_ex( f, &buf, NULL, 0, 0, 0, &theRid);
      if(!rc){
        if(rid) *rid = theRid;
        if(uuid){
          *uuid = fsl_rid_to_uuid(f, theRid);
          if(!uuid) rc = FSL_RC_OOM;
        }
      }
    }
    fsl_buffer_clear(&buf);
    return rc;
  }
}

int fsl__repo_import_buffer( fsl_cx * f, fsl_buffer const * in,
                            fsl_id_t * rid, fsl_uuid_str * uuid ){
  if(!f || !in) return FSL_RC_MISUSE;
  else{
    /* Workaround: input ptr is const and input needs to modify
       (only) the cursor. So we'll cheat rather than require a non-const
       input...
    */
    fsl_buffer cursorKludge = fsl_buffer_empty;
    fsl_buffer_external(&cursorKludge, in->mem, in->used);
    int const rc = fsl__repo_import_blob(f, fsl_input_f_buffer, &cursorKludge,
                                         rid, uuid );
    assert(cursorKludge.mem == in->mem);
    return rc;
  }
}


int fsl_repo_blob_lookup( fsl_cx * const f, fsl_buffer const * const src,
                          fsl_id_t * const ridOut, fsl_uuid_str * hashOut ){
  int rc;
  fsl_buffer hash_ = fsl_buffer_empty;
  fsl_buffer * hash;
  fsl_id_t rid = 0;
  if(!fsl_cx_db_repo(f)) return FSL_RC_NOT_A_REPO;
  hash = hashOut ? &hash_ : fsl__cx_scratchpad(f);
  /* First check the auxiliary hash to see if there is already an artifact
     that uses the auxiliary hash name */
  rc = fsl_cx_hash_buffer(f, true, src, hash);
  if(FSL_RC_UNSUPPORTED==rc){
    /* The auxiliary hash option is incompatible with our hash policy.
       We'll just try again with the preferred policy. */
    rc = 0;
  }else if(rc){
    goto end;
  }
  rid = hash->used ? fsl_uuid_to_rid(f, fsl_buffer_cstr(hash)) : 0;
  if( rid < 0 ){
    assert(f->error.code);
    rc = f->error.code;
    goto end;
  }else if( !rid ){
    /* No existing artifact with the auxiliary hash name.  Therefore, use
       the primary hash name. */
    fsl_buffer_reuse(hash);
    rc = fsl_cx_hash_buffer(f, false, src, hash);
    if(rc) goto end;
    rid = fsl_uuid_to_rid(f, fsl_buffer_cstr(hash));
    if( !rid ){
      rc = FSL_RC_NOT_FOUND;
    }else if(rid<0){
      rc = f->error.code;
    }
  }
  end:
  if(!rc || rc==FSL_RC_NOT_FOUND){
    if(hashOut){
      assert(hash == &hash_);
      *hashOut = fsl_buffer_take(hash)/*transfer*/;
    }
  }
  if(!rc && ridOut){
    *ridOut = rid;
  }
  if(hash == &hash_){
    fsl_buffer_clear(hash);
  }else{
    assert(!hash_.mem);
    fsl__cx_scratchpad_yield(f, hash);
  }
  return rc;
}

int fsl__repo_fingerprint_search( fsl_cx * const f, fsl_id_t rcvid,
                                 char ** zOut ){
  int rc = 0;
  fsl_db * const db = fsl_needs_repo(f);
  if(!db) return FSL_RC_NOT_A_REPO;
  fsl_buffer * const sql = fsl__cx_scratchpad(f);
  fsl_stmt q = fsl_stmt_empty;
  int version = 1 /* Fingerprint version to check: 0 or 1 */;
  try_again:
  /*
   * We check both v1 and v0 fingerprints, in that order. From Fossil
   * db.c:
   *
   * The original fingerprint algorithm used "quote(mtime)".  But this could
   * give slightly different answers depending on how the floating-point
   * hardware is configured.  For example, it gave different answers on
   * native Linux versus running under valgrind.
   */
  if(0==version){
    fsl_stmt_finalize(&q);
    fsl_buffer_append(sql,
                      "SELECT rcvid, quote(uid), quote(mtime), "
                      "quote(nonce), quote(ipaddr) "
                      "FROM rcvfrom ", -1);
  }else{
    assert(1==version);
    fsl_buffer_append(sql,
                      "SELECT rcvid, quote(uid), datetime(mtime), "
                      "quote(nonce), quote(ipaddr) "
                      "FROM rcvfrom ", -1);
  }
  rc = (rcvid>0)
    ? fsl_buffer_appendf(sql, "WHERE rcvid=%" FSL_ID_T_PFMT, rcvid)
    : fsl_buffer_append(sql, "ORDER BY rcvid DESC LIMIT 1", -1);
  if(rc) goto end;
  rc = fsl_db_prepare(db, &q, "%b", sql);
  if(rc) goto end;
  rc = fsl_stmt_step(&q);
  switch(rc){
    case FSL_RC_STEP_ROW:{
      fsl_md5_cx hash = fsl_md5_cx_empty;
      fsl_size_t len = 0;
      fsl_id_t const rvid = fsl_stmt_g_id(&q, 0);
      unsigned char digest[16] = {0};
      char hex[FSL_STRLEN_MD5+1] = {0};
      for(int i = 1; i <= 4; ++i){
        char const * z = fsl_stmt_g_text(&q, i, &len);
        fsl_md5_update(&hash, z, len);
      }
      fsl_md5_final(&hash, digest);
      fsl_md5_digest_to_base16(digest, hex);
      *zOut = fsl_mprintf("%" FSL_ID_T_PFMT "/%s", rvid, hex);
      rc = *zOut ? 0 : FSL_RC_OOM;
      break;
    }
    case FSL_RC_STEP_DONE:
      if(1==version){
        version = 0;
        fsl_buffer_reuse(sql);
        goto try_again;
      }
      rc = FSL_RC_NOT_FOUND;
      break;
    default:
      rc = fsl_cx_uplift_db_error2(f, db, rc);
      break;
  }
  end:
  fsl__cx_scratchpad_yield(f, sql);
  fsl_stmt_finalize(&q);
  return rc;
}

int fsl_repo_manifest_write(fsl_cx * const f,
                            fsl_id_t manifestRid,
                            fsl_buffer * const pManifest,
                            fsl_buffer * const pHash,
                            fsl_buffer * const pTags) {
  fsl_db * const db = fsl_needs_repo(f);
  if(!db) return FSL_RC_NOT_A_REPO;
  if(manifestRid<=0){
    manifestRid = f->db.ckout.rid;
    if(manifestRid<=0){
      return fsl_cx_err_set(f, 0==f->db.ckout.rid
                            ? FSL_RC_RANGE
                            : FSL_RC_NOT_A_CKOUT,
                            "%s(): no checkin version was specified "
                            "and %s.", __func__,
                            0==f->db.ckout.rid
                            ? "checkout has no version"
                            : "no checkout is opened");
    }
  }
  int rc = 0;
  char * str = 0;
  fsl_uuid_str ridHash = 0;
  fsl_buffer * bHash = 0;
  assert(manifestRid>0);

  if(pManifest){
    fsl_buffer_reuse(pManifest);
    rc = fsl_content_get(f, manifestRid, pManifest);
    if(rc) goto end;
  }
  if(pHash){
    if(f->db.ckout.rid!=manifestRid){
      bHash = fsl__cx_scratchpad(f);
      rc = fsl_rid_to_uuid2(f, manifestRid, bHash);
      if(rc) goto end;
      ridHash = (char *)bHash->mem;
    }else{
      ridHash = f->db.ckout.uuid;
    }
    assert(ridHash);
    fsl_buffer_append(pHash, ridHash, -1);
    rc = fsl_buffer_append(pHash, "\n", 1);
  }
  if(pTags){
    fsl_stmt q = fsl_stmt_empty;
    fsl_db * const db = fsl_cx_db_repo(f);
    assert(db && "We can't have a checkout w/o a repo.");
    str = fsl_db_g_text(db, NULL, "SELECT VALUE FROM tagxref "
                        "WHERE rid=%" FSL_ID_T_PFMT
                        " AND tagid=%d /*%s()*/",
                        f->db.ckout.rid, FSL_TAGID_BRANCH, __func__);
    rc = fsl_buffer_appendf(pTags, "branch %z\n", str);
    str = 0;
    if(rc) goto end;
    rc = fsl_db_prepare(db, &q,
                        "SELECT substr(tagname, 5)"
                        "  FROM tagxref, tag"
                        " WHERE tagxref.rid=%" FSL_ID_T_PFMT
                        "   AND tagxref.tagtype>0"
                        "   AND tag.tagid=tagxref.tagid"
                        "   AND tag.tagname GLOB 'sym-*'"
                        " /*%s()*/",
                        f->db.ckout.rid, __func__);
    while( 0==rc && FSL_RC_STEP_ROW==fsl_stmt_step(&q) ){
      const char *zName = fsl_stmt_g_text(&q, 0, NULL);
      rc = fsl_buffer_appendf(pTags, "tag %s\n", zName);
    }
    fsl_stmt_finalize(&q);
  }
  end:
  if(bHash) fsl__cx_scratchpad_yield(f, bHash);
  return rc;
}

/**
   Internal state for the rebuild process.
*/
struct FslRebuildState {
  fsl_cx * f;
  fsl_db * db;
  fsl_rebuild_opt const * opt;
  fsl_stmt qSize;
  fsl_stmt qChild;
  fsl_id_bag idsDone;
  fsl_rebuild_step step;
};
typedef struct FslRebuildState FslRebuildState;
const FslRebuildState FslRebuildState_empty = {
  .f = NULL, .db = NULL, .opt = NULL,
  .qSize = fsl_stmt_empty_m,
  .qChild = fsl_stmt_empty_m,
  .idsDone = fsl_id_bag_empty_m,
  .step = fsl_rebuild_step_empty_m
};

static int fsl__rebuild_update_schema(FslRebuildState * const frs){
  int rc = 0;
  char * zBlobSchema = NULL;

  /* Verify that the PLINK table has a new column added by the
  ** 2014-11-28 schema change.  Create it if necessary.  This code
  ** can be removed in the future, once all users have upgraded to the
  ** 2014-11-28 or later schema.
  */
  if(!fsl_db_table_has_column(frs->db, "plink", "baseid")){
    rc = fsl_cx_exec(frs->f,
                     "ALTER TABLE repository.plink ADD COLUMN baseid");
    if(rc) goto end;

  }

  /* Verify that the MLINK table has the newer columns added by the
  ** 2015-01-24 schema change.  Create them if necessary.  This code
  ** can be removed in the future, once all users have upgraded to the
  ** 2015-01-24 or later schema.
  */
  if( !fsl_db_table_has_column(frs->db,"mlink","isaux") ){
    rc = fsl_cx_exec_multi(frs->f,
      "ALTER TABLE repo.mlink ADD COLUMN pmid INTEGER DEFAULT 0;"
      "ALTER TABLE repo.mlink ADD COLUMN isaux BOOLEAN DEFAULT 0;"
    );
    if(rc) goto end;
  }

  /* We're going to skip several older (2011) schema updates for the
     time being on the grounds of YAGNI. */

  /**
     Update the repository schema for Fossil version 2.0.  (2017-02-28)
     (1) Change the CHECK constraint on BLOB.UUID so that the length
     is greater than or equal to 40, not exactly equal to 40.
  */
  zBlobSchema =
    fsl_db_g_text(frs->db, NULL, "SELECT sql FROM %!Q.sqlite_schema"
                  " WHERE name='blob'", fsl_db_role_name(FSL_DBROLE_REPO));
  if(!zBlobSchema){
    /* ^^^^ reminder: fossil(1) simply ignores this case, silently
       doing nothing instead. */
    rc = fsl_cx_uplift_db_error(frs->f, frs->db);
    if(!rc){
      rc = fsl_cx_err_set(frs->f, FSL_RC_DB,
                          "Unknown error fetching blob table schema.");
    }
    goto end;
  }
  /* Search for:  length(uuid)==40
  **              0123456789 12345   */
  for(int i=10; zBlobSchema[i]; i++){
    if( zBlobSchema[i]=='='
        && fsl_strncmp(&zBlobSchema[i-6],"(uuid)==40",10)==0 ){
      int rc2 = 0;
      zBlobSchema[i] = '>';
      sqlite3_db_config(frs->db->dbh, SQLITE_DBCONFIG_DEFENSIVE, 0, &rc2);
      rc = fsl_cx_exec_multi(frs->f,
           "PRAGMA writable_schema=ON;"
           "UPDATE %!Q.sqlite_schema SET sql=%Q WHERE name LIKE 'blob';"
           "PRAGMA writable_schema=OFF;",
           fsl_db_role_name(FSL_DBROLE_REPO), zBlobSchema
      );
      sqlite3_db_config(frs->db->dbh, SQLITE_DBCONFIG_DEFENSIVE, 1, &rc2);
      break;
    }
  }
  if(rc) goto end;
  rc = fsl_cx_exec(frs->f,
    "CREATE VIEW IF NOT EXISTS "
    "  %!Q.artifact(rid,rcvid,size,atype,srcid,hash,content) AS "
    "    SELECT blob.rid,rcvid,size,1,srcid,uuid,content"
    "      FROM blob LEFT JOIN delta ON (blob.rid=delta.rid);",
    fsl_db_role_name(FSL_DBROLE_REPO)
  );

  end:
  fsl_free(zBlobSchema);
  return rc;
}

#define INTCHECK frs->f->interrupted ? frs->f->interrupted :
/**
   Inserts rid into frs->idsDone and calls frs->opt->callback. Returns
   0 on success.
*/
static int fsl__rebuild_step_done(FslRebuildState * const frs, fsl_id_t rid){
  assert( !fsl_id_bag_contains(&frs->idsDone, rid) );
  assert( (int)frs->step.artifactType >= (int)FSL_SATYPE_INVALID );
  assert( (int)frs->step.artifactType < (int)FSL_SATYPE_count );
  int rc = fsl_id_bag_insert(&frs->idsDone, rid);
  ++frs->step.stepNumber;
  frs->step.rid = rid;
  if( FSL_SATYPE_INVALID==frs->step.artifactType ){
    frs->step.artifactType = FSL_SATYPE_ANY;
  }
  ++frs->step.metrics.counts[frs->step.artifactType];
  if( frs->step.blobSize<0 ){
    ++frs->step.metrics.phantomCount;
  }else{
    frs->step.metrics.sizes[frs->step.artifactType] += frs->step.blobSize;
  }
  /* TODO (2025-08-09). Need to fix f-rebuild.c at the same time. */
  if( 0==rc && frs->f->cxConfig.listener.callback ){
    rc = fsl_cx_emit(frs->f, FSL_MSG_REBUILD_STEP, &frs->step);
  }
  return rc ? rc : (INTCHECK 0);
}

/**
   Rebuilds cross-referencing state for the given RID and its content,
   recursively on all of its descendents. The contents of the input
   buffer are taken over by this routine.

   If other artifacts are deltas based off of the given artifact, they
   are processed as well.

   Returns 0 on success.
*/
static int fsl__rebuild_step(FslRebuildState * const frs, fsl_id_t rid,
                             int64_t blobSize, fsl_buffer * const content){
  fsl_deck deck = fsl_deck_empty;
  fsl_buffer deckContent = fsl_buffer_empty;
  fsl_id_bag idsChildren = fsl_id_bag_empty;
  int rc = 0;
  if(rc) goto end;
  assert(rid>0);
  assert( fsl_cx_txn_level(frs->f)>0 );
  if(!frs->qSize.stmt){
    rc = fsl_cx_prepare(frs->f, &frs->qSize,
                        "UPDATE blob SET size=?1 WHERE rid=?2/*%s()*/",
                        __func__);
    if(rc) goto end;
  }else{
    fsl_stmt_reset(&frs->qSize);
  }
  while(0==rc && rid>0){
    //MARKER(("TODO: %s(rid=%d)\n", __func__, (int)rid));
    if(blobSize != (int64_t)content->used){
      /* Fix [blob.size] field if needed. (Why would this ever be
         needed?) Much later: the blob.size gets out of sync with
         expected semantics sometimes, _presumably_ involving syncing
         of deltas, where their size is their deltified size instead
         of their fully-expanded size. It is needed, and fossil(1) has
         this same step. */
      rc = fsl_stmt_bind_step(&frs->qSize, "IR", (int64_t)content->used, rid);
      if(rc){
        fsl_cx_uplift_db_error(frs->f, frs->qSize.db);
        break;
      }
      blobSize = (int64_t)content->used;
    }
    /* Find all deltas based off of rid... */
    fsl_stmt * const qDeltaS2R =
      fsl__cx_stmt(frs->f, fsl__cx_stmt_e_deltaS2R);
    if( qDeltaS2R ){
      rc = fsl_stmt_bind_fmt(qDeltaS2R, "R", rid);
      if(rc){
        fsl_cx_uplift_db_error2(frs->f, qDeltaS2R->db, rc);
        break;
      }
      fsl_id_bag_reuse(&idsChildren);
      while(0==rc && FSL_RC_STEP_ROW==fsl_stmt_step(qDeltaS2R)){
        fsl_id_t const cid = fsl_stmt_g_id(qDeltaS2R, 0);
        if(!fsl_id_bag_contains(&frs->idsDone, cid)){
          rc = fsl_id_bag_insert(&idsChildren, cid);
        }
      }
      fsl_stmt_reset(qDeltaS2R);
      fsl__cx_stmt_yield(frs->f, qDeltaS2R);
    }else{
       rc = frs->f->error.code;
    }
    rc = INTCHECK rc;
    if(rc) break;

    fsl_size_t const nChild = fsl_id_bag_count(&idsChildren);
    if(fsl_id_bag_contains(&frs->idsDone, rid)){
      /* Kludge! This check should not be necessary. Testing with the
         libfossil repo, this check is not required on the main x86
         dev machine but is on a Raspberry Pi 4, for reasons
         as-yet-unknown. On the latter, artifact
         1ee529429e2aa6ffbeffb0cf73bb51a34a8547b8 (an opaque file, not
         a fossil artifact) makes it into frs->idsDone
         unexpectedly(?), triggering an assert() in
         fsl__rebuild_step_done().

         2025-08-09: is that still the case?
      */
      goto doChildren;
    }
    if(nChild){
      /* Parsing the deck will mutate the buffer, so we need a copy of
         the input content to apply the next delta to. */
      rc = fsl_buffer_copy(&deckContent, content);
      if(rc) break;
    }else{
      /* We won't be applying any deltas, so use the deck content the
         user passed in. */
      fsl_buffer_swap(content, &deckContent);
      fsl_buffer_clear(content);
    }
    frs->step.blobSize = blobSize;
    /* At this point fossil(1) decides between rebuild and
       deconstruct, performing different work for each. We're skipping
       the deconstruct option for now but may want to add it
       later. See fossil's rebuild.c:rebuild_step(). Note that
       deconstruct is not a capability intended for normal client
       use. It's primarily for testing of fossil itself. */
    fsl_deck_init(frs->f, &deck, FSL_SATYPE_ANY);
    //MARKER(("rid=%d\n", (int)rid));
    rc = INTCHECK fsl_deck_parse2(&deck, &deckContent, rid)
      /* But isn't it okay if rid is not an artifact? */;
    switch(rc){
      case FSL_RC_SYNTAX:
        /* Assume deck is not an artifact. Fall through and continue
           processing the delta children. */
        fsl_cx_err_reset(frs->f);
        rc = 0;
        frs->step.artifactType = FSL_SATYPE_INVALID;
        break;
      case 0:
        frs->step.artifactType = deck.type;
        rc = INTCHECK fsl__deck_crosslink(&deck);
        break;
      default:
#if 0
        MARKER(("err=%s for rid=%d content=\n%.*s\n", fsl_rc_cstr(rc), (int)rid,
                (int)deckContent.used, (char const *)deckContent.mem));
#endif
        break;
    }
    fsl_buffer_clear(&deckContent);
    fsl_deck_finalize(&deck);
    rc = INTCHECK 0;
    if(0==rc && 0==(rc = frs->f->interrupted)){
      rc = fsl__rebuild_step_done( frs, rid );
    }
    if(rc) break;
    /* Process all dependent deltas recursively... */
    doChildren:
    rid = 0;
    fsl_size_t i = 1;
    for(fsl_id_t cid = fsl_id_bag_first(&idsChildren);
         0==rc && cid!=0; cid = fsl_id_bag_next(&idsChildren, cid), ++i){
      int64_t sz;
      if(!frs->qChild.stmt){
        rc = fsl_cx_prepare(frs->f, &frs->qChild,
                            "SELECT content, size "
                            "FROM blob WHERE rid=?1/*%s()*/",
                            __func__);
        if(rc) break;
      }else{
        fsl_stmt_reset(&frs->qChild);
      }
      fsl_stmt_bind_id(&frs->qChild, 1, cid);
      if( FSL_RC_STEP_ROW==fsl_stmt_step(&frs->qChild) &&
          (sz = fsl_stmt_g_int64(&frs->qChild, 1))>=0 ){
        fsl_buffer next = fsl_buffer_empty;
        fsl_buffer delta = fsl_buffer_empty;
        void const * blob = 0;
        fsl_size_t deltaBlobSize = 0;
        rc = INTCHECK fsl_stmt_get_blob(&frs->qChild, 0, &blob, &deltaBlobSize);
        if(rc) goto outro;
        fsl_buffer_external(&delta, blob, (fsl_int_t)deltaBlobSize);
        rc = INTCHECK fsl_buffer_uncompress(&delta, &delta);
        if(rc) goto outro;
        rc = INTCHECK fsl_buffer_delta_apply(content, &delta, &next);
        fsl_stmt_reset(&frs->qChild);
        fsl_buffer_clear(&delta);
        if(rc){
          if(FSL_RC_OOM!=rc){
            rc = fsl_cx_err_set(frs->f, rc,
                                "Error applying delta #%" FSL_ID_T_PFMT
                                " to parent #%" FSL_ID_T_PFMT, cid, rid);
          }
          goto outro;
        }
        if(i<nChild){
          rc = INTCHECK fsl__rebuild_step(frs, cid, sz, &next);
          assert(!next.mem);
        }else{
          rid = cid;
          blobSize = sz;
          fsl_buffer_clear(content);
          *content = next/*transfer ownership*/;
        }
        if(0==rc) continue;
        outro:
        assert(0!=rc);
        fsl_stmt_reset(&frs->qChild);
        fsl_buffer_clear(&delta);
        fsl_buffer_clear(&next);
        break;
      }else{
        fsl_stmt_reset(&frs->qChild);
        fsl_buffer_clear(content);
      }
    }
  }
  end:
  fsl_deck_finalize(&deck);
  fsl_buffer_clear(content);
  fsl_buffer_clear(&deckContent);
  fsl_id_bag_clear(&idsChildren);
  return rc ? rc : (INTCHECK 0);
}
#undef INTCHECK
/**
   Check to see if the "sym-trunk" tag exists.  If not, create it and
   attach it to the very first check-in. Returns 0 on success.
*/
static int fsl__rebuild_tag_trunk(FslRebuildState * const frs){
  fsl_id_t const tagid =
    fsl_db_g_id(frs->db, 0,
                "SELECT 1 FROM tag WHERE tagname='sym-trunk'");
  if(tagid>0) return 0;
  fsl_id_t const rid =
    fsl_db_g_id(frs->db, 0,
                "SELECT pid FROM plink AS x WHERE NOT EXISTS"
                "(SELECT 1 FROM plink WHERE cid=x.pid)");
  if(rid==0) return 0;

  /* Add the trunk tag to the root of the whole tree */
  int rc = 0;
  fsl_buffer * const b = fsl__cx_scratchpad(frs->f);
  rc = fsl_rid_to_uuid2(frs->f, rid, b);
  switch(rc){
    case FSL_RC_NOT_FOUND:
      rc = 0/*fossil ignores this case without an error*/;
      break;
    case 0: {
      fsl_deck d = fsl_deck_empty;
      char const * zUuid = fsl_buffer_cstr(b);
      fsl_deck_init(frs->f, &d, FSL_SATYPE_CONTROL);
      rc = fsl_deck_T_add(&d, FSL_TAGTYPE_PROPAGATING,
                          zUuid, "sym-trunk", NULL);
      if(0==rc) rc = fsl_deck_T_add(&d, FSL_TAGTYPE_PROPAGATING,
                                    zUuid, "branch", "trunk");
      if(0==rc){
        char const * userName = fsl_cx_user_guess(frs->f);
        if(!userName){
          rc = fsl_cx_err_set(frs->f, FSL_RC_NOT_FOUND,
                              "Cannot determine user name for "
                              "control artifact.");
        }else{
          rc = fsl_deck_U_set(&d, userName);
        }
      }
      if(0==rc){
        rc = fsl_deck_save(&d, fsl_content_is_private(frs->f, rid));
      }
      fsl_deck_finalize(&d);
      break;
    }
    default: break;
  }
  fsl__cx_scratchpad_yield(frs->f, b);
  return rc;
}

static int fsl__rebuild(fsl_cx * const f, fsl_rebuild_opt const * const opt){
  fsl_stmt s = fsl_stmt_empty;
  fsl_stmt q = fsl_stmt_empty;
  fsl_db * const db = fsl_cx_db_repo(f);
  int rc;
  FslRebuildState frs = FslRebuildState_empty;
  fsl_buffer * const sql = fsl__cx_scratchpad(f);
  assert(db);
  assert(opt);
  frs.f = frs.step.f = f;
  frs.db = db;
  frs.opt = frs.step.opt = opt;
  rc = fsl__rebuild_update_schema(&frs);
  if(!rc) rc = fsl_buffer_reserve(sql, 1024 * 4);
  if(rc) goto end;

  fsl__cx_content_caches_clear(f);

  /* DROP all tables which are not part of our One True Vision of the
     repo db...

     2022-07-31: we might want to stop doing this because: if fossil
     adds new tables, there may be a lag in getting them into
     libfossil and we don't necessarily want to nuke those. OTOH, all
     such tables would be transient/rebuildable state, so if we nuke
     them then a rebuild from fossil(1) would correct it.

     2024-12-06: https://fossil-scm.org/home/info/aeec557e897f
     reimplements this slightly but the full implications of doing it
     that way are not yet clear.
 */
  rc = fsl_cx_prepare(f, &q,
     "SELECT name FROM %!Q.sqlite_schema /*scan*/"
     " WHERE type='table'"
     " AND name NOT IN ('admin_log', 'blob','delta','rcvfrom','user','alias',"
                       "'config','shun','private','reportfmt',"
                       "'concealed','accesslog','modreq',"
                       "'purgeevent','purgeitem','unversioned',"
                      "'ticket','ticketchng',"
                       "'subscriber','pending_alert','chat'"
                      ")"
     " AND name NOT GLOB 'sqlite_*'"
     " AND name NOT GLOB 'fx_*'"
     " AND name NOT GLOB 'ftsidx_*'"
     " AND name NOT GLOB 'chatfts1_*'",
     fsl_db_role_name(FSL_DBROLE_REPO)
  );
  while( 0==rc && FSL_RC_STEP_ROW==fsl_stmt_step(&q) ){
    rc = fsl_buffer_appendf(sql, "DROP TABLE IF EXISTS %!Q;\n",
                            fsl_stmt_g_text(&q, 0, NULL));
  }
  fsl_stmt_finalize(&q);
  if(0==rc && fsl_buffer_size(sql)){
    rc = fsl_cx_exec_multi(f, "%b", sql);
  }
  if(rc) goto end;

  rc = fsl_cx_exec_multi(f, "%s", fsl_schema_repo2());
  if(0==rc) rc = fsl__cx_ticket_create_table(f);
  if(0==rc) rc = fsl__shunned_remove(f);
  if(0==rc){
    rc = fsl_cx_exec_multi(f,
      "INSERT INTO unclustered"
      " SELECT rid FROM blob EXCEPT SELECT rid FROM private;"
      "DELETE FROM unclustered"
      " WHERE rid IN (SELECT rid FROM shun JOIN blob USING(uuid));"
      "DELETE FROM config WHERE name IN ('remote-code', 'remote-maxid');"
      "UPDATE user SET mtime=now() WHERE mtime IS NULL;"
    );
  }
  if(rc) goto end;

  /* The following should be count(*) instead of max(rid). max(rid) is
  ** an adequate approximation, however, and is much faster for large
  ** repositories. */
  if(1){
    frs.step.artifactCount =
      (uint32_t)fsl_db_g_id(db, 0, "SELECT count(*) FROM blob");
  }else{
    frs.step.artifactCount =
      (uint32_t)fsl_db_g_id(db, 0, "SELECT max(rid) FROM blob");
  }

  //totalSize += incrSize*2;
  rc = fsl_cx_prepare(f, &s,
     "SELECT rid, size FROM blob /*scan*/"
     " WHERE NOT EXISTS(SELECT 1 FROM shun WHERE uuid=blob.uuid)"
     "   AND NOT EXISTS(SELECT 1 FROM delta WHERE rid=blob.rid)"
     "%s", opt->randomize ? " ORDER BY RANDOM()" : ""
  );
  if(rc) goto end;
  rc = fsl__crosslink_begin(f)
    /* Maintenace reminder: if this call succeeds, BE SURE that
       we do not skip past the fsl__crosslink_end() call via
       (goto end). Doing so would get the transaction stack out
       of sync. */;
  if(rc) goto end /*to skip fsl__crosslink_end() call!*/;
  while( 0==rc && FSL_RC_STEP_ROW==fsl_stmt_step(&s) ){
    fsl_id_t const rid = fsl_stmt_g_id(&s, 0);
    int64_t const size = fsl_stmt_g_int64(&s, 1);
    if( size>=0 ){
      fsl_buffer content = fsl_buffer_empty;
      rc = fsl_content_get(f, rid, &content);
      if(0==rc){
        rc = fsl__rebuild_step(&frs, rid, size, &content);
        assert(!content.mem);
      }
      fsl_buffer_clear(&content);
    }
  }
  fsl_stmt_finalize(&s);
  if(rc) goto crosslink_end;
  rc = fsl_cx_prepare(f, &s,
     "SELECT rid, size FROM blob"
     " WHERE NOT EXISTS(SELECT 1 FROM shun WHERE uuid=blob.uuid)"
     "%s", opt->randomize ? " ORDER BY RANDOM()" : ""
  );
  while( 0==rc && FSL_RC_STEP_ROW==fsl_stmt_step(&s) ){
    fsl_id_t const rid = fsl_stmt_g_id(&s, 0);
    int64_t const size = fsl_stmt_g_int64(&s, 1);
    if( size>=0 ){
      if( !fsl_id_bag_contains(&frs.idsDone, rid) ){
        fsl_buffer content = fsl_buffer_empty;
        rc = fsl_content_get(f, rid, &content);
        if(0==rc){
          rc = fsl__rebuild_step(&frs, rid, size, &content);
          assert(!content.mem);
        }
        /*
          2021-12-17: hmmm... while debugging the problem reported here:

          https://fossil-scm.org/forum/forumpost/f4cc31863179f843

          It was discovered that fossil will simply skip any content
          it cannot read in this step, even if it's skipped over
          because of a broken blob-to-delta mapping (whereas fossil's
          test-integrity command will catch that case). If such a case
          happens to us, fsl_content_get() fails with FSL_RC_PHANTOM.
          That seems to me to be the right thing to do, as such a case
          is indicative of db corruption. However, if we skip over
          these then we cannot rebuild a repo which has such (invalid)
          state.

          Feature or bug?

          For now let's keep it strict and fail if we can't fetch the
          content. We can reevaluate that decision later if needed. We
          can add a fsl_rebuild_opt::ignorePhantomFailure (better name
          pending!) flag which tells us how the user would prefer to
          deal with this.
        */
        fsl_buffer_clear(&content);
      }
    }else{
      rc = fsl_cx_exec_multi(f, "INSERT OR IGNORE INTO phantom "
                             "VALUES(%" FSL_ID_T_PFMT ")", rid);
      if(0==rc){
        frs.step.blobSize = -1;
        frs.step.artifactType = FSL_SATYPE_INVALID;
        rc = fsl__rebuild_step_done(&frs, rid);
      }
    }
  }
  fsl_stmt_finalize(&s);
  crosslink_end:
  rc = fsl__crosslink_end(f, rc);
  if(rc) goto end;
  rc = fsl__rebuild_tag_trunk(&frs);
  if(rc) goto end;
  //if( opt->createClusters ) rc = fsl__create_cluster(f);
  rc = fsl_cx_exec_multi(f,
     "REPLACE INTO config(name,value,mtime) VALUES('content-schema',%Q,now());"
     "REPLACE INTO config(name,value,mtime) VALUES('aux-schema',%Q,now());"
     "REPLACE INTO config(name,value,mtime) "
       "VALUES('rebuilt','libfossil %q %q',now());",
     FSL_CONTENT_SCHEMA, FSL_AUX_SCHEMA,
     fsl_buildinfo(FSL_BUILDINFO_VERSION_HASH),
     fsl_buildinfo(FSL_BUILDINFO_VERSION_TIMESTAMP)
  );
  end:
  fsl__cx_scratchpad_yield(f, sql);
  fsl_stmt_finalize(&s);
  fsl_stmt_finalize(&frs.qSize);
  fsl_stmt_finalize(&frs.qChild);
  fsl_id_bag_clear(&frs.idsDone);

  if( frs.step.stepNumber ){
    frs.step.errCode = rc;
    int const rc2 = fsl_cx_emit(frs.f, FSL_MSG_REBUILD_DONE, &frs.step);
    if( rc2 && !rc ) rc = rc2;
  }
  return rc;
}

int fsl_repo_rebuild(fsl_cx * const f, fsl_rebuild_opt const * opt){
  int rc = 0;
  fsl_db * const db = fsl_needs_repo(f);
  if( !db ) return rc;
  if( !opt ) opt = &fsl_rebuild_opt_empty;
#if 0
  rc = fsl_cx_txn_begin(f);
  if(0==rc){
    rc = fsl__rebuild(f, opt);
    int const rc2 =
      fsl_cx_txn_end_v2(f, !opt->dryRun && 0==rc, false);
    if(0==rc && 0!=rc2) rc = rc2;
  }
#else
  fsl__cx_txn_local(f, rc, !opt->dryRun, {
      rc = fsl__rebuild(f, opt);
    });
#endif
  fsl_cx_interrupt(f, 0, NULL);
  return rc;
}


int fsl_cidiff(fsl_cx * const f, fsl_cidiff_opt const * const opt){
  fsl_deck d1 = fsl_deck_empty;
  fsl_deck d2 = fsl_deck_empty;
  fsl_card_F const * fc1;
  fsl_card_F const * fc2;
  int rc;
  fsl_cidiff_state cst = fsl_cidiff_state_empty;
  if(!fsl_needs_repo(f)) return FSL_RC_NOT_A_REPO;
  rc = fsl_deck_load_rid(f, &d1, opt->v1, FSL_SATYPE_CHECKIN);
  if(rc) goto end;
  rc = fsl_deck_load_rid(f, &d2, opt->v2, FSL_SATYPE_CHECKIN);
  if(rc) goto end;
  rc = fsl_deck_F_rewind(&d1);
  if(0==rc) rc = fsl_deck_F_rewind(&d2);
  if(rc) goto end;
  fsl_deck_F_next(&d1, &fc1);
  fsl_deck_F_next(&d2, &fc2);
  cst.f = f;
  cst.opt = opt;
  cst.d1 = &d1;
  cst.d2 = &d2;
  rc = opt->callback(&cst);
  cst.stepType = FSL_RC_STEP_ROW;
  while(0==rc && (fc1 || fc2)){
    int nameCmp;
    cst.changes = FSL_CIDIFF_NONE;
    if(!fc1) nameCmp = 1;
    else if(!fc2) nameCmp = -1;
    else{
      nameCmp = fsl_strcmp(fc1->name, fc2->name);
      if(fc2->priorName){
        if(0==nameCmp){
          cst.changes |= FSL_CIDIFF_FILE_RENAMED;
        }else if(0==fsl_strcmp(fc1->name, fc2->priorName)){
          /**
             Treat these as being the same file for this purpose.

             We ostensibly know that fc1 was renamed to fc2->name here
             BUT there's a corner case we can't sensibly determine
             here: file A renamed to B and file C renamed to A. If
             both of those F-cards just happen to align at this point
             in this loop, we're mis-informing the user. Reliably
             catching that type of complex situation requires
             significant hoop-jumping, as can be witness in
             fsl_ckout_merge() (which still misses some convoluted
             cases).
          */
          nameCmp = 0;
          cst.changes |= FSL_CIDIFF_FILE_RENAMED;
        }
      }
      if(fc1->perm!=fc2->perm){
        cst.changes |= FSL_CIDIFF_FILE_PERMS;
      }
    }
    if(nameCmp<0){
      nameCmp = -1/*see below*/;
      assert(fc1);
      cst.changes |= FSL_CIDIFF_FILE_REMOVED;
      cst.fc1 = fc1; cst.fc2 = NULL;
    }else if(nameCmp>0){
      nameCmp = 1/*see below*/;
      cst.changes |= FSL_CIDIFF_FILE_ADDED;
      cst.fc1 = NULL; cst.fc2 = fc2;
    }else{
      cst.fc1 = fc1; cst.fc2 = fc2;
    }
    if(fc1 && fc2 && 0!=fsl_strcmp(fc1->uuid, fc2->uuid)){
      cst.changes |= FSL_CIDIFF_FILE_MODIFIED;
    }
    rc = opt->callback(&cst);
    switch(rc ? 2 : nameCmp){
      case  2: break;
      case -1: rc = fsl_deck_F_next(&d1, &fc1); break;
      case  1: rc = fsl_deck_F_next(&d2, &fc2); break;
      case  0:
        rc = fsl_deck_F_next(&d1, &fc1);
        if(0==rc) rc = fsl_deck_F_next(&d2, &fc2);
        break;
      default:
        fsl__fatal(FSL_RC_MISUSE,"Internal API misuse.");
    }
  }/*while(f-cards)*/
  if(0==rc){
    cst.fc1 = cst.fc2 = NULL;
    cst.stepType = FSL_RC_STEP_DONE;
    rc = opt->callback(&cst);
  }
  end:
  fsl_deck_finalize(&d1);
  fsl_deck_finalize(&d2);
  return rc;
}


bool fsl_repo_forbids_delta_manifests(fsl_cx * const f){
  return fsl_config_get_bool(f, FSL_CONFDB_REPO, false,
                             "forbid-delta-manifests");
}

int fsl_tkt_id_to_rids(fsl_cx * const f, char const * tktId,
                       fsl_id_t ** ridList){
  fsl_db * const dbR = fsl_needs_repo(f);
  if(!dbR) return FSL_RC_NOT_A_REPO;
  fsl_stmt q = fsl_stmt_empty;
  int rc;
  fsl_id_t * rids = 0;
  int const isFullId = fsl_is_uuid(tktId);
  unsigned int n = 0;
  if(FSL_STRLEN_SHA1<isFullId){
    return fsl_cx_err_set(f, FSL_RC_RANGE,
                          "Ticket ID is not valid. Expecting <=%d bytes of "
                          "lower-case hex values.", FSL_STRLEN_SHA1);
  }
  fsl_db_err_reset(dbR);
  rc = fsl_cx_txn_begin(f);
  if(rc) return rc;
  if(isFullId){
    // Expect an exact match...
    assert(FSL_STRLEN_SHA1==isFullId);
    rc = fsl_cx_prepare(f, &q,
                        "SELECT b.rid, b.uuid FROM blob b, tagxref x, tag t "
                        "WHERE t.tagname = 'tkt-'||%Q "
                        "AND t.tagid=x.tagid AND x.rid=b.rid "
                        "ORDER BY x.mtime, x.rowid",
                        tktId);
  }else{
    /* Check for an ambiguous match of an ID prefix... */
    int32_t const c =
      fsl_db_g_int32(dbR, -1,
                     "SELECT COUNT(distinct tagname) FROM tag "
                     "WHERE tagname GLOB 'tkt-%q*'", tktId);
    if( c>1 ){
      rc = fsl_cx_err_set(f, FSL_RC_AMBIGUOUS,
                          "Ticket ID prefix is ambiguous: %s",
                          tktId);
      goto end;
    }else{
      if( c<0 && dbR->error.code ){
        rc = fsl_cx_uplift_db_error(f, dbR);
        goto end;
      }
      if( c<=0 ){
        assert( 0==n );
        goto not_found;
      }
    }
    rc = fsl_cx_prepare(f, &q,
                        "SELECT b.rid, b.uuid FROM blob b, tagxref x, tag t "
                        "WHERE t.tagname GLOB 'tkt-%q*' "
                        "AND t.tagid=x.tagid AND x.rid=b.rid "
                        "ORDER BY x.mtime, x.rowid",
                        tktId);
  }
  if(rc) return rc;
  // Count how many we have to allocate for...
  while(FSL_RC_STEP_ROW==fsl_stmt_step(&q)) ++n;
  fsl_stmt_reset(&q);
  not_found:
  if(!n){
    rc = fsl_cx_err_set(f, FSL_RC_NOT_FOUND,
                        "No ticket found with ID%s %s.",
                        isFullId ? "" : " prefix",
                        tktId);
    goto end;
  }
  // Populate the result list...
  rids = (fsl_id_t*)fsl_malloc(sizeof(fsl_id_t)*(n+1));
  if(!rids){
    rc = FSL_RC_OOM;
    goto end;
  }
  unsigned i = 0;
  while(FSL_RC_STEP_ROW==fsl_stmt_step(&q)){
    assert(i<n);
    rids[i++] = fsl_stmt_g_id(&q, 0);
  }
  assert(i==n);
  rids[i] = 0;
  end:
  if(0==rc) *ridList = rids;
  else fsl_free(rids);
  fsl_stmt_finalize(&q);
  fsl_cx_txn_end_v2(f,false,false);
  return rc;
}

int fsl_branch_main(fsl_cx *f, char const **zOut, bool forceReload){
  if( !forceReload && f->db.repo.mainBranch ){
    *zOut = f->db.repo.mainBranch;
    return 0;
  }
  if( !fsl_needs_repo(f) ) return FSL_RC_NOT_A_REPO;
  fsl_buffer * const b = fsl__cx_scratchpad(f);
  char const * z = 0;
  int rc;
  rc = fsl_config_get_buffer(f, FSL_CONFDB_REPO, "main-branch", b);
  switch( rc ){
    case 0:
      z = fsl_buffer_cstr(b);
      break;
    case FSL_RC_NOT_FOUND:
      fsl_cx_err_reset(f);
      rc = 0;
      z = "trunk";
      break;
    default:
      break;
  }
  if( 0==rc ){
    char * const zz = fsl_strdup(z);
    if( zz ){
      fsl_free( f->db.repo.mainBranch );
      *zOut = f->db.repo.mainBranch = zz;
    }else{
      fsl_report_oom;
      rc = FSL_RC_OOM;
    }
  }
  fsl__cx_scratchpad_yield(f, b);
  return rc;
}


static int fsl__uperm_add(fsl_cx * f, fsl_uperm * p, char const *zCap,
                          unsigned level, int chFromPerm){
  int rc = 0;
  for( ; 0==rc && *zCap; ++zCap ){
    switch( (int)*zCap ){
      case (int)' ': case (int)'\t': break;
      case 's':   p->setup = 1;
        FSL_SWITCH_FALL_THROUGH /* into Admin */;
      case 'a':   p->admin = p->readTicket = p->writeTicket = p->zip =
                             p->readWiki = p->writeWiki = p->newWiki =
                             p->appendWiki = p->hyperlink = p->clone =
                             p->newTicket = p->password = p->readAddr =
                             p->ticketReport = p->attach = p->appendTicket =
                             p->moderateWiki = p->moderateTicket =
                             p->readForum = p->writeForum = p->moderateForum =
                             p->trustedForum = p->adminForum = p->chat =
                             p->emailAlert = p->announce = p->debug = 1;
        FSL_SWITCH_FALL_THROUGH /* into Read/Write */;
      case 'i':   p->read = p->write = 1;  break;
      case 'o':   p->read = 1;             break;
      case 'z':   p->zip = 1;              break;

      case 'h':   p->hyperlink = 1;        break;
      case 'g':   p->clone = 1;            break;
      case 'p':   p->password = 1;         break;

      case 'j':   p->readWiki = 1;         break;
      case 'k':   p->writeWiki = p->readWiki =
                  p->appendWiki =1;        break;
      case 'm':   p->appendWiki = 1;       break;
      case 'f':   p->newWiki = 1;          break;
      case 'l':   p->moderateWiki = 1;     break;

      case 'e':   p->readAddr = 1;         break;
      case 'r':   p->readTicket = 1;       break;
      case 'n':   p->newTicket = 1;        break;
      case 'w':   p->writeTicket = p->readTicket =
                  p->newTicket = p->appendTicket = 1; break;
      case 'c':   p->appendTicket = 1;     break;
      case 'q':   p->moderateTicket = 1;   break;
      case 't':   p->ticketReport = 1;     break;
      case 'b':   p->attach = 1;           break;
      case 'x':   p->privateContent = 1;   break;
      case 'y':   p->writeUnversioned = 1; break;

      case '6':   p->adminForum = 1;    FSL_SWITCH_FALL_THROUGH;
      case '5':   p->moderateForum = 1; FSL_SWITCH_FALL_THROUGH;
      case '4':   p->trustedForum = 1;  FSL_SWITCH_FALL_THROUGH;
      case '3':   p->writeForum = 1;    FSL_SWITCH_FALL_THROUGH;
      case '2':   p->readForum = 1;        break;

      case '7':   p->emailAlert = 1;       break;
      case 'A':   p->announce = 1;         break;
      case 'C':   p->chat = 1;             break;
      case 'D':   p->debug = 1;            break;

      case (int)'u':
      case (int)'v': {
        if( chFromPerm==(int)*zCap || level>1 ){
          /* Don't recurse. level>1 catches indirect recursion like
             u=>v=>u. */
          break;
        }
        bool * const pB = 'u'==*zCap ? &p->xReader : &p->xDeveloper;
        if( *pB ) break;
        fsl_db * const db = f ? fsl_needs_repo(f) : 0;
        if( !db ){
          rc = f ? FSL_RC_NOT_A_REPO : FSL_RC_MISUSE;
          break;
        }
        char * z = 0;
        rc = fsl_db_get_text(db, &z, NULL,
                             "SELECT cap FROM %!Q.user WHERE login=%Q /*%s()*/",
                             fsl_db_role_name(FSL_DBROLE_REPO),
                             ('u'==*zCap) ? "reader" : "developer",
                             __func__);
        if( rc ){
          rc = fsl_cx_uplift_db_error(f, db);
          assert( !z );
          break;
        }
        if( z ){
          rc = fsl__uperm_add(f, p, z, level+1, (int)*zCap);
          fsl_free(z);
        }
        if( 0==rc ) *pB = true;
        break;
      }
      default:
        rc = f
          ? fsl_cx_err_set(f, FSL_RC_RANGE, "Invalid permissions char '%c' (0x%d)",
                           *zCap, (int)*zCap)
          : FSL_RC_RANGE;
        break;
    }
  }
  return rc;
}

int fsl_uperm_add(fsl_cx * f, fsl_uperm * pTgt, char const *zCap){
  return fsl__uperm_add(f, pTgt, zCap, 0, 0);
}

int fsl_repo_project_code( fsl_cx * f, char const **pOut ){
  if(f->cache.projectCode){
    *pOut = f->cache.projectCode;
    return 0;
  }else{
    fsl_buffer b = fsl_buffer_empty;
    int const rc = fsl_config_get_buffer(f, FSL_CONFDB_REPO,
                                         "project-code", &b);
    if( 0==rc ){
      *pOut = f->cache.projectCode = (char*)b.mem/*transfer ownership*/;
    }else{
      fsl_buffer_clear(&b);
    }
    return rc;
  }
}


#undef MARKER
#undef fsl__bprc
