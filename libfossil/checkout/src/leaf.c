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
  This file houses some of the "leaf"-related APIs.
*/
#include <assert.h>

#include "fossil-scm/internal.h"

/* Only for debugging */
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

int fsl_repo_leaves_rebuild(fsl_cx * const f){
  char const * zMainBranch = 0;
  int rc = fsl_branch_main(f, &zMainBranch, false);
  if( rc ) return rc;
  return fsl_cx_exec_multi(f,
    "DELETE FROM leaf;"
    "INSERT OR IGNORE INTO leaf"
    "  SELECT cid FROM plink"
    "  EXCEPT"
    "  SELECT pid FROM plink"
    "   WHERE coalesce((SELECT value FROM tagxref"
                       " WHERE tagid=%d AND rid=plink.pid),%Q)"
         " == coalesce((SELECT value FROM tagxref"
                       " WHERE tagid=%d AND rid=plink.cid),%Q)",
                         FSL_TAGID_BRANCH, zMainBranch,
                         FSL_TAGID_BRANCH, zMainBranch
  );
}

fsl_int_t fsl_count_nonbranch_children(fsl_cx * const f, fsl_id_t rid){
  int32_t rv = 0;
  fsl_db * const db = fsl_cx_db_repo(f);
  char const * zMainBranch = 0;
  int rc;
  if(!db || !db->dbh || (rid<=0)) return -1;
  rc = fsl_branch_main(f, &zMainBranch, false);
  if( 0==rc ){
    rc = fsl_db_get_int32(db, &rv,
                          "SELECT count(*) FROM plink "
                          "WHERE pid=%"FSL_ID_T_PFMT" "
                          "AND isprim "
                          "AND coalesce((SELECT value FROM tagxref "
                          "WHERE tagid=%d AND rid=plink.pid), %Q)"
                          "=coalesce((SELECT value FROM tagxref "
                          "WHERE tagid=%d AND rid=plink.cid), %Q)",
                          rid, FSL_TAGID_BRANCH, zMainBranch,
                          FSL_TAGID_BRANCH, zMainBranch);
  }
  return rc ? -2 : rv;
}

bool fsl_rid_is_leaf(fsl_cx * const f, fsl_id_t rid){
  if( rid<0 || !fsl_cx_db_repo(f) ) return false;
  fsl_stmt * const q = fsl__cx_stmt(f, fsl__cx_stmt_e_ridIsLeaf);
  bool rv = false;
  if( q ){
    char const * zMainBranch = 0;
    int rc = fsl_branch_main(f, &zMainBranch, false);
    if( 0==rc ){
      rc = fsl_stmt_bind_step_v2(q, "Rs", rid, zMainBranch);
      switch( rc ){
        case FSL_RC_STEP_DONE: rv = true; break;
        case FSL_RC_STEP_ROW: fsl_stmt_reset(q); break;
        default:
          fsl_cx_uplift_db_error2(f, q->db, rc);
          break;
      }
    }
    fsl__cx_stmt_yield(f, q);
  }
  return rv;
}

bool fsl_rid_is_version(fsl_cx * const f, fsl_id_t rid){
  fsl_db * const db = fsl_cx_db_repo(f);
  if(!db) return false;
  return 1==fsl_db_g_int32(db, 0,
                           "SELECT 1 FROM event "
                           "WHERE objid=%" FSL_ID_T_PFMT
                           " AND type='ci'", rid);
}

/** @internal

    Check to see if checkin "rid" is a leaf and either add it to the LEAF
    table if it is, or remove it if it is not.

    Returns 0 on success, FSL_RC_MISUSE if !f or f has no repo db
    opened, FSL_RC_RANGE if pid is <=0. Other errors
    (e.g. FSL_RC_DB) may indicate that db is not a repo. On error
    db's error state may be updated.
*/
static int fsl__leaf_check(fsl_cx * const f, fsl_id_t rid){
  if(rid<=0) return FSL_RC_RANGE;
  else {
    int rc = 0;
    bool isLeaf;
    isLeaf = fsl_rid_is_leaf(f, rid);
    fsl_cx_err_reset(f);
    fsl_stmt * const q = fsl__cx_stmt( f, isLeaf
                                       ? fsl__cx_stmt_e_leafInsert
                                       : fsl__cx_stmt_e_leafDelete );
    if( !q ) rc = f->error.code;
    else{
      rc = fsl_stmt_bind_step_v2(q, "R", rid);
      rc = (FSL_RC_STEP_DONE==rc)
        /* Reminder to self (2025-08-13): if we ever add (RETURNING
           rid) to leafInsert we'll need to handle FSL_RC_STEP_ROW
           here. */
        ? 0
        : fsl_cx_uplift_db_error2(f, q->db, rc);
      fsl__cx_stmt_yield(f, q);
    }
    return rc;
  }
}

int fsl__leaf_do_pending_checks(fsl_cx * const f){
  fsl_id_t rid;
  int rc = 0;
  fsl_id_bag * const bLeaf = fsl__cx_ptl_bag(f, fsl__ptl_e_leafCheck);
  for(rid=fsl_id_bag_first(bLeaf);
      !rc && rid; rid=fsl_id_bag_next(bLeaf,rid)){
    rc = fsl__leaf_check(f, rid);
  }
  fsl_id_bag_reuse(bLeaf);
  return rc;
}

int fsl__leaf_eventually_check( fsl_cx * const f, fsl_id_t rid){
  fsl_db * db = f ? fsl_cx_db_repo(f) : NULL;
  if(!f) return FSL_RC_MISUSE;
  else if(rid<=0) return FSL_RC_RANGE;
  else if(!db) return FSL_RC_NOT_A_REPO;
  else {
    fsl_id_bag * const bLeaf = fsl__cx_ptl_bag(f, fsl__ptl_e_leafCheck);
    fsl_stmt * q = fsl__cx_stmt(f, fsl__cx_stmt_e_parentsOf);
    int rc = q ? 0 : f->error.code;
    if( 0==rc ){
      assert( q );
      rc = fsl_stmt_bind_id(q, 1, rid);
      if( 0==rc ){
        rc = fsl_id_bag_insert(bLeaf, rid);
        while( !rc && (FSL_RC_STEP_ROW==fsl_stmt_step(q)) ){
          rc = fsl_id_bag_insert(bLeaf, fsl_stmt_g_id(q, 0));
        }
      }
      fsl__cx_stmt_yield(f,q);
    }
    return rc;
  }
}


int fsl_leaves_compute(fsl_cx * const f, fsl_id_t vid,
                       fsl_leaves_compute_e closeMode){
  fsl_db * const db = fsl_needs_repo(f);
  if(!db) return FSL_RC_NOT_A_REPO;
  int rc = 0;

  /* Create the LEAVES table if it does not already exist.  Make sure
  ** it is empty.
  */
  rc = fsl_db_exec_multi(db,
    "CREATE TEMP TABLE IF NOT EXISTS leaves("
    "  rid INTEGER PRIMARY KEY"
    ");"
    "DELETE FROM leaves;"
  );
  if(rc) goto dberr;
  if( vid <= 0 ){
    rc = fsl_db_exec_multi(db,
      "INSERT INTO leaves SELECT leaf.rid FROM leaf"
    );
    if(rc) goto dberr;
  }else{
    fsl_id_bag seen = fsl_id_bag_empty;     /* Descendants seen */
    fsl_id_bag pending = fsl_id_bag_empty;  /* Unpropagated descendants */
    fsl_stmt q1 = fsl_stmt_empty;      /* Query to find children of a check-in */
    fsl_stmt isBr = fsl_stmt_empty;    /* Query to check to see if a check-in starts a new branch */
    fsl_stmt * const qIns = fsl__cx_stmt(f, fsl__cx_stmt_e_leafInsert);
    char const * zMainBranch = 0;
    if( !qIns ){
      rc = f->error.code;
      goto end;
    }

    rc = fsl_branch_main(f, &zMainBranch, false);
    if( rc ) goto cleanup;

    /* Initialize the bags. */
    rc = fsl_id_bag_insert(&pending, vid);
    if(rc) goto cleanup;

    /* This query returns all non-branch-merge children of check-in
    ** RID (?1).
    **
    ** If a child is a merge of a fork within the same branch (?2), it
    ** is returned. Only merge children in different branches are
    ** excluded.
    */
    rc = fsl_db_prepare(db, &q1,
                        "SELECT cid FROM plink"
                        " WHERE pid=?1"
                        " AND (isprim"
                        "      OR coalesce("
                        "(SELECT value FROM tagxref"
                        " WHERE tagid=%d AND rid=plink.pid), ?2"
                        ")=coalesce("
                        "(SELECT value FROM tagxref"
                        "   WHERE tagid=%d AND rid=plink.cid), ?2)"
                        ")",
                        FSL_TAGID_BRANCH, FSL_TAGID_BRANCH
    );
    if(rc) goto cleanup;
    /* This query returns a single row if check-in RID (?1) is the
    ** first check-in of a new branch. */
    rc = fsl_db_prepare(db, &isBr,
       "SELECT 1 FROM tagxref"
       " WHERE rid=?1 AND tagid=%d AND tagtype=%d"
       "   AND srcid>0",
       FSL_TAGID_BRANCH, FSL_TAGTYPE_PROPAGATING
    );
    if(rc) goto cleanup;

    while( fsl_id_bag_count(&pending) ){
      fsl_id_t const rid = fsl_id_bag_first(&pending);
      unsigned cnt = 0;
      fsl_id_bag_remove(&pending, rid);
      rc = fsl_stmt_bind_fmt(&q1, "Rs", rid, zMainBranch);
      if( rc ) goto cleanup;
      while( FSL_RC_STEP_ROW==(rc = fsl_stmt_step(&q1)) ){
        int const cid = fsl_stmt_g_id(&q1, 0);
        rc = fsl_id_bag_insert(&seen, cid);
        if(rc) break;
        rc = fsl_id_bag_insert(&pending, cid);
        if(rc) break;
        fsl_stmt_bind_id(&isBr, 1, cid);
        if( FSL_RC_STEP_DONE==fsl_stmt_step(&isBr) ){
          ++cnt;
        }
        fsl_stmt_reset(&isBr);
      }
      if(FSL_RC_STEP_DONE==rc) rc = 0;
      else if(rc) break;
      fsl_stmt_reset(&q1);
      if( cnt==0 && !fsl_rid_is_leaf(f, rid) ){
        ++cnt;
      }
      if( cnt==0 ){
        fsl_stmt_bind_id(qIns, 1, rid);
        rc = fsl_stmt_step(qIns);
        if(FSL_RC_STEP_DONE!=rc) break;
        rc = 0;
        fsl_stmt_reset(qIns);
      }
    }
    cleanup:
    fsl__cx_stmt_yield(f,qIns);
    fsl_stmt_finalize(&isBr);
    fsl_stmt_finalize(&q1);
    fsl_id_bag_clear(&pending);
    fsl_id_bag_clear(&seen);
    if(rc) goto dberr;
  }
  assert(!rc);
  switch(closeMode){
    case FSL_LEAVES_COMPUTE_CLOSED:
    case FSL_LEAVES_COMPUTE_OPEN:
      rc =
        fsl_db_exec(db,
                    "DELETE FROM leaves WHERE rid %s IN"
                    "  (SELECT leaves.rid FROM leaves, tagxref"
                    "    WHERE tagxref.rid=leaves.rid "
                    "      AND tagxref.tagid=%d"
                    "      AND tagxref.tagtype>0)",
                    FSL_LEAVES_COMPUTE_CLOSED==closeMode ? "NOT " : "",
                    FSL_TAGID_CLOSED);
      if(rc) goto dberr;
      break;
    default: break;
  }

  end:
  return rc;
  dberr:
  assert(rc);
  rc = fsl_cx_uplift_db_error2(f, db, rc);
  goto end;
}

bool fsl_leaves_computed_has(fsl_cx * const f){
  return fsl_db_exists(fsl_cx_db_repo(f),
                       "SELECT 1 FROM leaves");
}

fsl_int_t fsl_leaves_computed_count(fsl_cx * const f){
  int32_t rv = -1;
  fsl_db * const db = fsl_cx_db_repo(f);
  int const rc = fsl_db_get_int32(db, &rv,
                                 "SELECT COUNT(*) FROM leaves");
  if(rc){
    fsl_cx_uplift_db_error2(f, db, rc);
    assert(-1==rv);
  }else{
    assert(rv>=0);
  }
  return rv;
}

fsl_id_t fsl_leaves_computed_latest(fsl_cx * const f){
  fsl_id_t rv = 0;
  fsl_db * const db = fsl_cx_db_repo(f);
  int const rc =
    fsl_db_get_id(db, &rv,
                  "SELECT rid FROM leaves, event"
                  " WHERE event.objid=leaves.rid"
                  " ORDER BY event.mtime DESC");
  if(rc){
    fsl_cx_uplift_db_error2(f, db, rc);
    assert(!rv);
  }else{
    assert(rv>=0);
  }
  return rv;
}

void fsl_leaves_computed_cleanup(fsl_cx * const f){
  if(fsl_cx_exec(f, "DROP TABLE IF EXISTS temp.leaves")){
    /**
       Naively assume that locking is keeping us from dropping it,
       and simply empty it instead. */
    fsl_cx_exec(f, "DELETE FROM temp.leaves");
  }
}

#undef MARKER
