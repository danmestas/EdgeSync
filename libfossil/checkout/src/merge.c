/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */ 
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/
#include "libfossil.h"
#include <assert.h>
#include <memory.h>
#include <stdlib.h>
#include <string.h> /* memmove()/strlen() */

#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

#define FSL__TABLE_PIVOT "aqueue"

/**
   Sets the primary version of a merge.  The primary version is
   one of the two files that have a common ancestor.  The other file
   is the secondary.  There can be multiple secondaries but only a
   single primary.  The primary must be set first.

   In the merge algorithm, the file being merged in is the primary.
   The current check-out or other files that have been merged into
   the current checkout are the secondaries.

   The act of setting the primary resets the pivot-finding algorithm.

   Returns 0 on success, non-0 on serious error.

   This works using a TEMP db, so does not strictly require
   a repo or checkout.

   @see fsl__pivot_add_secondary()
   @see fsl__pivot_find()
*/
static int fsl__pivot_set_primary(fsl_cx * const f, fsl_id_t rid){
  /* Set up table used to do the search */
  int rc = fsl_cx_exec_multi(f,
    "CREATE TEMP TABLE IF NOT EXISTS " FSL__TABLE_PIVOT "("
    "  rid INTEGER,"              /* The record id for this version */
    "  mtime REAL,"               /* Time when this version was created */
    "  pending BOOLEAN,"          /* True if we have not check this one yet */
    "  src BOOLEAN,"              /* 1 for primary.  0 for others */
    "  PRIMARY KEY(rid,src)"
    ") WITHOUT ROWID;"
    "DELETE FROM " FSL__TABLE_PIVOT ";"
    "CREATE INDEX IF NOT EXISTS " FSL__TABLE_PIVOT "_idx1 ON " FSL__TABLE_PIVOT "(pending, mtime);"
  );
  if(0==rc){
    /* Insert the primary record */
    rc = fsl_cx_exec(f,
      "INSERT INTO " FSL__TABLE_PIVOT "(rid, mtime, pending, src)"
      "  SELECT %" FSL_ID_T_PFMT ", mtime, 1, 1 "
      "  FROM event WHERE objid=%" FSL_ID_T_PFMT
      " AND type='ci' LIMIT 1",
      rid, rid );
  }
  return rc;
}

/**
   Set a secondary file of a merge. The primary file must be set
   first. There must be at least one secondary but there can be more
   than one if desired.

   Returns 0 on success, non-0 on db error.

   @see fsl__pivot_set_primary()
   @see fsl__pivot_find()
*/
static int fsl__pivot_add_secondary(fsl_cx * const f, fsl_id_t rid){
  return fsl_cx_exec(f,
    "INSERT OR IGNORE INTO " FSL__TABLE_PIVOT "(rid, mtime, pending, src)"
    "  SELECT %" FSL_ID_T_PFMT ", mtime, 1, 0 "
    "FROM event WHERE objid=%" FSL_ID_T_PFMT " AND type='ci'",
    rid, rid
  );
}


/**
   Searches for the most recent common ancestor of the primary and one of
   the secondaries in a merge.

   On success, *outRid is set to its value and 0 is returned. If no
   match is found, *outRid is set to 0 and 0 is returned. Returns
   non-0 on error, indicating either a lower-level db error or an
   allocation error.

   If ignoreMerges is true, it follows only "primary" parent links.

   @see fsl__pivot_set_primary()
   @see fsl__pivot_add_secondary()
*/
static int fsl__pivot_find(fsl_cx * const f, bool ignoreMerges, fsl_id_t *outRid){
  fsl_db * const db = fsl_cx_db(f);
  int rc;
  fsl_stmt q1 = fsl_stmt_empty, q2 = fsl_stmt_empty,
    u1 = fsl_stmt_empty, i1 = fsl_stmt_empty;
  fsl_id_t rid = 0;
  if(fsl_db_g_int32(db, 0, "SELECT COUNT(distinct src) FROM " FSL__TABLE_PIVOT "")<2){
    return fsl_cx_err_set(f, FSL_RC_MISUSE,
                          "Pivot list [" FSL__TABLE_PIVOT "] contains neither primary "
                          "nor secondary entries.");
  }
  /* Prepare queries we will be needing
  **
  ** The first query finds the oldest pending version on the
  ** FSL__TABLE_PIVOT. This will be next one searched.
  */
  rc = fsl_cx_prepare(f, &q1,
                      "SELECT rid FROM " FSL__TABLE_PIVOT " WHERE pending"
                      " ORDER BY pending DESC, mtime DESC");
  if(rc) goto end;

  /* Check to see if the record :rid is a common ancestor.  The result
  ** set contains one or more rows if it is and is the empty set if it
  ** is not.
  */
  rc = fsl_cx_prepare(f, &q2,
    "SELECT 1 FROM " FSL__TABLE_PIVOT " A, plink, " FSL__TABLE_PIVOT " B"
    " WHERE plink.pid=?1"
    "   AND plink.cid=B.rid"
    "   AND A.rid=?1"
    "   AND A.src!=B.src %s",
    ignoreMerges ? "AND plink.isprim" : ""
  );
  if(rc) goto end;

  /* Mark the :rid record has having been checked.  It is not the
  ** common ancestor.
  */
  rc = fsl_cx_prepare(f, &u1,
    "UPDATE " FSL__TABLE_PIVOT " SET pending=0 WHERE rid=?1"
  );
  if(rc) goto end;

  /* Add to the queue all ancestors of :rid.
  */
  rc = fsl_cx_prepare(f, &i1,
    "REPLACE INTO " FSL__TABLE_PIVOT " "
    "SELECT plink.pid,"
    " coalesce((SELECT mtime FROM event X WHERE X.objid=plink.pid), 0.0),"
    " 1,"
    " " FSL__TABLE_PIVOT ".src "
    "  FROM plink, " FSL__TABLE_PIVOT
    " WHERE plink.cid=?1"
    "   AND " FSL__TABLE_PIVOT ".rid=?1 %s",
    ignoreMerges ? "AND plink.isprim" : ""
  );
  if(rc) goto end;
  while(FSL_RC_STEP_ROW==(rc = fsl_stmt_step(&q1))){
    rid = fsl_stmt_g_id(&q1, 0);
    fsl_stmt_reset(&q1);
    rc = fsl_stmt_bind_step(&q2, "R", rid);
    if(rc) break/*error or found match*/;
    rc = fsl_stmt_bind_step(&i1, "R", rid);
    assert(FSL_RC_STEP_ROW!=rc);
    if(0==rc) rc = fsl_stmt_bind_step(&u1, "R", rid);
    if(rc) break;
    assert(FSL_RC_STEP_ROW!=rc);
    rid = 0;
  }
  switch(rc){
    case 0: break;
    case FSL_RC_STEP_ROW:
    case FSL_RC_STEP_DONE: rc = 0; break;
    default:
      rc = fsl_cx_uplift_db_error2(f, NULL, rc);
      break;
  }
  end:
  fsl_stmt_finalize(&q1);
  fsl_stmt_finalize(&q2);
  fsl_stmt_finalize(&u1);
  fsl_stmt_finalize(&i1);
  if(0==rc  && rid) *outRid = rid;
  return rc;
}

#if 0
// not currently used anywhere in libfossil.
/**
   Searches f's current repository for the nearest fork related to
   version vid.

   More specifically: this looks for the most recent leaf that is (1)
   not equal to vid and (2) has not already been merged into vid and
   (3) the leaf is not closed and (4) the leaf is in the same branch
   as vid.

   If checkVmerge is true then the current checkout
   database is also checked (via the vmerge table), in which case a
   checkout must be opened.

   On success, returns 0 and assigns *outRid to the resulting RID.

   On error:

   - FSL_RC_NOT_A_REPO or FSL_RC_NOT_A_CKOUT if called when no repo or
     (if checkVmerge is true) no checkout.

   - FSL_RC_NOT_FOUND: no closest merge was found.

   - FSL_RC_OOM: on allocation error

   - Any number of potential other errors from the db layer.

   f's error state will contain more information about errors reported
   here.
*/
static int fsl__find_nearest_fork(fsl_cx * const f,
                                  fsl_id_t vid, bool checkVmerge,
                                  fsl_id_t * outRid){
  fsl_db * const db = checkVmerge
    ? fsl_needs_ckout(f) : fsl_needs_repo(f);
  fsl_buffer * const sql = fsl__cx_scratchpad(f);
  fsl_stmt q;
  if(!db){
    return checkVmerge ? FSL_RC_NOT_A_CKOUT : FSL_RC_NOT_A_REPO;
  }
  q = fsl_stmt_empty;
  int rc = fsl_buffer_appendf(sql,
    "SELECT leaf.rid"
    "  FROM leaf, event"
    " WHERE leaf.rid=event.objid"
    "   AND leaf.rid!=%" FSL_ID_T_PFMT,  /* Constraint (1) */
    vid
  );
  if(rc) goto end;
  if( checkVmerge ){
    rc = fsl_buffer_append(sql,
      "   AND leaf.rid NOT IN (SELECT merge FROM vmerge)"
      /* Constraint (2) */, -1 );
    if(rc) goto end;
  }
  rc = fsl_buffer_appendf(sql,
    "   AND NOT EXISTS(SELECT 1 FROM tagxref" /* Constraint (3) */
                  "     WHERE rid=leaf.rid"
                  "       AND tagid=%d"
                  "       AND tagtype>0)"
    "   AND (SELECT value FROM tagxref"      /* Constraint (4) */
          "  WHERE tagid=%d AND rid=%" FSL_ID_T_PFMT " AND tagtype>0) ="
          " (SELECT value FROM tagxref"
          "  WHERE tagid=%d AND rid=leaf.rid AND tagtype>0)"
    " ORDER BY event.mtime DESC LIMIT 1",
                         FSL_TAGID_CLOSED,
                         FSL_TAGID_BRANCH,
                         vid, FSL_TAGID_BRANCH
  );
  if(rc) goto end;
  rc = fsl_db_prepare(db, &q, "%b", sql);
  if(rc){
    rc = fsl_cx_uplift_db_error2(f, db, rc);
    goto end;
  }
  rc = fsl_stmt_step(&q);
  switch(rc){
    case FSL_RC_STEP_ROW:
      rc = 0;
      *outRid = fsl_stmt_g_id(&q, 0);
      assert(*outRid>0);
      break;
    case FSL_RC_STEP_DONE:
      rc = fsl_cx_err_set(f, FSL_RC_NOT_FOUND,
                          "Cannot find nearest fork of RID #%"
                          FSL_ID_T_PFMT ".", vid);
      break;
    default:
      rc = fsl_cx_uplift_db_error2(f, db, rc);
      break;
  }
  end:
  fsl_stmt_finalize(&q);
  fsl__cx_scratchpad_yield(f, sql);
  return rc;
}
#endif /* fsl__find_nearest_fork() */

/**
   Name for the merge algo's version of the fv table, noting that it
   differs enough from the update algo's version that they do not
   map 1-to-1 without some degree of pain which might make sense
   once merge is in the library and working.
*/
#define FSL__TABLE_FVM "fvm"

static int fsl__renames_init(fsl_cx * const f){
  char const * const coll = fsl_cx_filename_collation(f);
  return fsl_cx_exec_multi(f,
    "CREATE TEMP TABLE IF NOT EXISTS " FSL__TABLE_FVM "(\n"
    "  fn TEXT UNIQUE %s,\n"       /* The filename */
    "  idv INTEGER DEFAULT 0,\n"   /* VFILE entry for current version */
    "  idp INTEGER DEFAULT 0,\n"   /* VFILE entry for the pivot */
    "  idm INTEGER DEFAULT 0,\n"   /* VFILE entry for version merging in */
    "  chnged BOOLEAN,\n"          /* True if current version has been edited */
    "  ridv INTEGER DEFAULT 0,\n"  /* Record ID for current version */
    "  ridp INTEGER DEFAULT 0,\n"  /* Record ID for pivot */
    "  ridm INTEGER DEFAULT 0,\n"  /* Record ID for merge */
    "  isexe BOOLEAN,\n"           /* Execute permission enabled */
    "  fnp TEXT UNIQUE %s,\n"      /* The filename in the pivot */
    "  fnm TEXT UNIQUE %s,\n"      /* The filename in the merged version */
    "  fnn TEXT UNIQUE %s,\n"      /* The filename in the name pivot */
    "  islinkv BOOLEAN,\n"         /* True if current version is a symlink */
    "  islinkm BOOLEAN\n"          /* True if merged version in is a symlink */
    ");"
    "DELETE FROM " FSL__TABLE_FVM ";",
    coll, coll, coll, coll);
}

static int fsl__renames_finalize(fsl_cx * const f){
  return fsl_cx_exec(f,
                     //"DROP TABLE IF EXISTS " FSL__TABLE_FVM
                     "DELETE FROM " FSL__TABLE_FVM);
}

static int fsl__renames_add(fsl_cx * const f,
                            char const * zFnCol,
                            fsl_id_t vid, fsl_id_t nid,
                            bool reverseOk){
  int rc;
  uint32_t i; /* loop counter */
  uint32_t nChng; /* # of entries in aChng */
  fsl_id_t * aChng = 0; /* Array of filename changes */
  fsl_stmt q1 = fsl_stmt_empty, q2 = fsl_stmt_empty;
  fsl_db * const db = fsl_cx_db_repo(f);
  rc = fsl__find_filename_changes(f, nid, vid, reverseOk, &nChng, &aChng);
  if(rc) goto end;
  else if(0==nChng) return 0;
  rc = fsl_cx_prepare(f, &q1,
                      "SELECT name FROM filename WHERE fnid=?1");
  if(0==rc){
    rc = fsl_cx_prepare(f, &q2,
                        "SELECT name FROM filename WHERE fnid=?1");
  }
  for(i=0; 0==rc && i < nChng; ++i){
    char const *zN;
    char const *zV;
    rc = fsl_stmt_bind_step(&q1, "R", aChng[i*2]);
    if(FSL_RC_STEP_ROW==rc){
      rc = fsl_stmt_bind_step(&q2, "R", aChng[i*2+1]);
      if(FSL_RC_STEP_ROW==rc) rc = 0;
    }
    if(rc){
      rc = fsl_cx_uplift_db_error2(f, NULL, rc);
      break;
    }
    rc = fsl_stmt_get_text(&q1, 0, &zN, NULL);
    if(0==rc) rc = fsl_stmt_get_text(&q2, 0, &zV, NULL);
    if(rc){
      rc = fsl_cx_uplift_db_error2(f, db, rc);
      break;
    }
    rc = fsl_cx_exec(f,"INSERT OR IGNORE INTO " FSL__TABLE_FVM
                     "(%s,fnn) VALUES(%Q,%Q)",
                     zFnCol, zV, zN);
    if(0==rc && 0==fsl_db_changes_recent(db)){
      rc = fsl_cx_exec_multi(f, "UPDATE " FSL__TABLE_FVM
                             " SET %s=%Q WHERE fnn=%Q",
                             zFnCol, zV, zN);
    }
  }
  end:
  fsl_stmt_finalize(&q1);
  fsl_stmt_finalize(&q2);
  fsl_free(aChng);
  return rc;
}

/**
   Part of fsl_ckout_merge() related to collecting filenames and
   setting up renames. Returns 0 on success.
*/
static int fsl__renames_tweak(fsl_cx * const f, fsl_id_t mid,
                              fsl_id_t pid, fsl_id_t vid,
                              fsl_id_t nid,
                              fsl_merge_opt const * const mOpt){
  int rc = 0;
  char vAncestor = 'p'; /* If P is an ancestor of V then 'p', else 'n' */

  rc = fsl__renames_init(f);
  if(0==rc) rc = fsl__renames_add(f, "fn", vid, nid, false);
  if(0==rc) rc = fsl__renames_add(f, "fnp", pid, nid, false);
  if(0==rc) rc = fsl__renames_add(f, "fnm", mid, nid,
                                  FSL_MERGE_TYPE_BACKOUT==mOpt->mergeType);
  /*
    It goes without saying that all of the SQL wizardry which follows
    was implemented by D. Richard Hipp. Its usage here does not imply
    any real understanding of it on the fossil-to-libfossil porter's
    part.
  */
  if(rc) goto end;
  else if(nid!=pid){
    /* See forum thread https://fossil-scm.org/forum/forumpost/549700437b
    **
    ** If a filename changes between nid and one of the other check-ins
    ** pid, vid, or mid, then it might not have changed for all of them.
    ** try to fill in the appropriate filename in all slots where the
    ** name is missing.
    **
    ** This does not work if
    **   (1) The filename changes more than once in between nid and vid/mid
    **   (2) Two or more filenames swap places - for example if A is renamed
    **       to B and B is renamed to A.
    ** The Fossil merge algorithm breaks down in those cases.  It will need
    ** to be completely rewritten to handle such complex cases.  Such cases
    ** appear to be rare, and also confusing to humans.
    */
    rc = fsl_cx_exec(f,
      "UPDATE OR IGNORE " FSL__TABLE_FVM
      " SET fnp=vfile.pathname FROM vfile"
      " WHERE fnp IS NULL"
      " AND vfile.pathname = " FSL__TABLE_FVM ".fnn"
      " AND vfile.vid=%" FSL_ID_T_PFMT,
      pid
    );
    if(rc) goto end;
    rc = fsl_cx_exec(f,
      "UPDATE OR IGNORE " FSL__TABLE_FVM
      " SET fn=vfile.pathname FROM vfile"
      " WHERE fn IS NULL"
      " AND vfile.pathname = "
      "   coalesce(" FSL__TABLE_FVM ".fnp," FSL__TABLE_FVM ".fnn)"
      " AND vfile.vid=%" FSL_ID_T_PFMT,
      vid
    );
    if(rc) goto end;
    rc = fsl_cx_exec(f,
      "UPDATE OR IGNORE " FSL__TABLE_FVM
      " SET fnm=vfile.pathname FROM vfile"
      " WHERE fnm IS NULL"
      " AND vfile.pathname ="
      "  coalesce(" FSL__TABLE_FVM ".fnp," FSL__TABLE_FVM ".fnn)"
      " AND vfile.vid=%" FSL_ID_T_PFMT,
      mid
    );
    if(rc) goto end;
    rc = fsl_cx_exec(f,
      "UPDATE OR IGNORE " FSL__TABLE_FVM
      " SET fnp=vfile.pathname FROM vfile"
      " WHERE fnp IS NULL"
      " AND vfile.pathname"
      "   IN (" FSL__TABLE_FVM ".fnm," FSL__TABLE_FVM ".fn)"
      " AND vfile.vid=%" FSL_ID_T_PFMT,
      pid
    );
    if(rc) goto end;
    rc = fsl_cx_exec(f,
      "UPDATE OR IGNORE " FSL__TABLE_FVM
      " SET fn=vfile.pathname FROM vfile"
      " WHERE fn IS NULL"
      " AND vfile.pathname = " FSL__TABLE_FVM ".fnm"
      " AND vfile.vid=%" FSL_ID_T_PFMT,
      vid
    );
    if(rc) goto end;
    rc = fsl_cx_exec(f,
      "UPDATE OR IGNORE " FSL__TABLE_FVM
      " SET fnm=vfile.pathname FROM vfile"
      " WHERE fnm IS NULL"
      " AND vfile.pathname = " FSL__TABLE_FVM ".fn"
      " AND vfile.vid=%" FSL_ID_T_PFMT,
      mid
    );
    if(rc) goto end;
  }
  assert(0==rc);
  if(mOpt->baselineRid>0){
    fsl_db * const db = fsl_cx_db_repo(f);
    fsl_db_err_reset(db);
    vAncestor = fsl_db_exists(db,
      "WITH RECURSIVE ancestor(id) AS ("
      "  VALUES(%" FSL_ID_T_PFMT ")"
      "  UNION"
      "  SELECT pid FROM plink, ancestor"
      "   WHERE cid=ancestor.id"
      "   AND pid!=%" FSL_ID_T_PFMT
      "   AND cid!=%" FSL_ID_T_PFMT ")"
      "SELECT 1 FROM ancestor"
      "  WHERE id=%" FSL_ID_T_PFMT " LIMIT 1",
      vid, nid, pid, pid
    ) ? 'p' : 'n';
    assert(0==fsl_db_err_get(db, NULL, NULL));
  }

  /*
  ** Add files found in V
  */
  rc = fsl_cx_exec_multi(f,
    "UPDATE OR IGNORE " FSL__TABLE_FVM
    " SET fn=coalesce(fn%c,fnn) WHERE fn IS NULL;"
    "REPLACE INTO "
    FSL__TABLE_FVM "(fn,fnp,fnm,fnn,idv,ridv,islinkv,isexe,chnged)"
    " SELECT pathname, fnp, fnm, fnn, id, rid, islink, vf.isexe, vf.chnged"
    "   FROM vfile vf"
    "   LEFT JOIN " FSL__TABLE_FVM " ON fn=coalesce(origname,pathname)"
    "    AND rid>0 AND vf.chnged NOT IN (3,5)"
    "  WHERE vid=%" FSL_ID_T_PFMT ";",
    vAncestor, vid
  );
  if(rc) goto end;
  /*
  ** Add files found in P
  */
  rc = fsl_cx_exec_multi(f,
    "UPDATE OR IGNORE " FSL__TABLE_FVM " SET fnp=coalesce(fnn,"
    "   (SELECT coalesce(origname,pathname) FROM vfile WHERE id=idv))"
    " WHERE fnp IS NULL;"
    "INSERT OR IGNORE INTO " FSL__TABLE_FVM "(fnp)"
    " SELECT coalesce(origname,pathname) FROM vfile WHERE vid=%" FSL_ID_T_PFMT ";",
    pid
  );
  if(rc) goto end;

  /*
  ** Add files found in M
  */
  rc = fsl_cx_exec_multi(f,
    "UPDATE OR IGNORE " FSL__TABLE_FVM " SET fnm=fnp WHERE fnm IS NULL;"
    "INSERT OR IGNORE INTO " FSL__TABLE_FVM "(fnm)"
    " SELECT pathname FROM vfile WHERE vid=%" FSL_ID_T_PFMT ";",
    mid
  );
  if(rc) goto end;

  /*
  ** Compute the file version ids for P and M
  */
  if( pid==vid ){
    rc = fsl_cx_exec_multi(f,
      "UPDATE " FSL__TABLE_FVM " SET idp=idv, ridp=ridv"
      " WHERE ridv>0 AND chnged NOT IN (3,5)"
    );
  }else{
    rc = fsl_cx_exec_multi(f,
      "UPDATE " FSL__TABLE_FVM
      " SET idp=coalesce(vfile.id,0), ridp=coalesce(vfile.rid,0)"
      " FROM vfile"
      " WHERE vfile.vid=%" FSL_ID_T_PFMT
      " AND " FSL__TABLE_FVM ".fnp=vfile.pathname",
      pid
    );
  }
  if(rc) goto end;
  rc = fsl_cx_exec_multi(f,
    "UPDATE " FSL__TABLE_FVM " SET"
    " idm=coalesce(vfile.id,0),"
    " ridm=coalesce(vfile.rid,0),"
    " islinkm=coalesce(vfile.islink,0),"
    " isexe=coalesce(vfile.isexe," FSL__TABLE_FVM ".isexe)"
    " FROM vfile"
    " WHERE vid=%" FSL_ID_T_PFMT " AND fnm=pathname",
    mid
  );
  if(rc) goto end;

  /*
  ** Update the execute bit on files where it's changed from P->M but
  ** not P->V
  */
  if(!mOpt->dryRun){
    fsl_stmt q = fsl_stmt_empty;
    rc = fsl_cx_prepare(f, &q,
      "SELECT idv, fn, " FSL__TABLE_FVM ".isexe "
      "FROM " FSL__TABLE_FVM ", vfile p, vfile v"
      " WHERE p.id=idp AND v.id=idv AND " FSL__TABLE_FVM ".isexe!=p.isexe"
      " AND v.isexe=p.isexe"
    );
    if(rc) goto end;
    fsl_buffer * const fnAbs =
      fsl__cx_scratchpad(f)/*absolute filenames*/;
    rc = fsl_buffer_reserve(fnAbs, f->db.ckout.dirLen + 256);
    if(0==rc){
      rc = fsl_buffer_append(fnAbs, f->db.ckout.dir, f->db.ckout.dirLen);
    }
    while( 0==rc && FSL_RC_STEP_ROW==fsl_stmt_step(&q) ){
      fsl_id_t const idv = fsl_stmt_g_id(&q, 0);
      int const isExe = fsl_stmt_g_int32(&q, 2);
      fsl_size_t nName = 0;
      const char *zName = 0;
      rc = fsl_stmt_get_text(&q, 1, &zName, &nName);
      if(rc) break;
      fnAbs->mem[f->db.ckout.dirLen] = 0;
      fnAbs->used = f->db.ckout.dirLen;
      rc = fsl_buffer_append(fnAbs, zName, (fsl_int_t)nName);
      if(rc) break;
      fsl_file_exec_set( fsl_buffer_cstr(fnAbs), !!isExe )
        /* Ignoring error */;
      rc = fsl_cx_exec(f, "UPDATE vfile SET isexe=%d "
                       "WHERE id=%" FSL_ID_T_PFMT,
                       isExe, idv);
    }
    fsl__cx_scratchpad_yield(f, fnAbs);
    fsl_stmt_finalize(&q);
  }
  end:
  return rc;
}/*fsl__renames_tweak()*/

/**
   Adds an an entry in the vmerge table for the given id and rid.
   Returns 0 on success, uplifts any db error into f's error state.
*/
static int fsl__vmerge_insert(fsl_cx * const f, fsl_id_t id, fsl_id_t rid){
  return fsl_cx_exec(f,
    "INSERT OR IGNORE INTO vmerge(id,merge,mhash)"
    "VALUES(%" FSL_ID_T_PFMT ",%" FSL_ID_T_PFMT
    ",(SELECT uuid FROM blob WHERE rid=%" FSL_ID_T_PFMT "))",
    id, rid, rid
  );
}

#define fsl_merge_state_empty_m {        \
  NULL/*f*/,                             \
  NULL/*opt*/,                           \
  NULL/*filename*/,                      \
  NULL/*priorName*/,                     \
  FSL_MERGE_FCHANGE_NONE/*fileChangeType*/,\
  FSL_CKUP_RM_NOT/* fileRmInfo */        \
}
/**
   Initialized-with-defaults fsl_merge_state instance,
   intended for use in non-const copy initialization.
*/
const fsl_merge_state fsl_merge_state_empty = fsl_merge_state_empty_m;

int fsl_ckout_merge(fsl_cx * const f, fsl_merge_opt const * const opt){
  /**
     Notation:

     V (vid)  The current checkout
     M (mid)  The version being merged in
     P (pid)  The "pivot" - the most recent common ancestor of V and M.
     N (nid)  The "name pivot" - for detecting renames

     What follows was initially based on:

     https://fossil-scm.org/home/file/src/merge.c?ci=e340af58a249dc09&ln=331-1065
  */
  int rc = 0;
  fsl_db * const db = fsl_needs_ckout(f);
  bool inTrans = false;
  fsl_id_t const vid = f->db.ckout.rid /* current checkout (V) */;
  fsl_id_t pid = 0 /* pivot RID (P): most recent common ancestor of V and M*/;
  fsl_id_t mid = opt->mergeRid /* merge-in version (M) */;
  fsl_id_t nid = 0 /* "name pivot" version (N) */;
  bool doIntegrate = FSL_MERGE_TYPE_INTEGRATE==opt->mergeType;
  fsl_stmt q = fsl_stmt_empty;
  fsl_buffer * const absPath = fsl__cx_scratchpad(f);
  fsl_merge_state mState = fsl_merge_state_empty;
  if(!db) rc = FSL_RC_NOT_A_CKOUT;
  else if(0==vid){
    rc = fsl_cx_err_set(f, FSL_RC_MISUSE,
                        "Cannot merge into empty top-level checkin.");
  }else if(FSL_MERGE_TYPE_CHERRYPICK==opt->mergeType
           && opt->baselineRid>0){
    rc = fsl_cx_err_set(f, FSL_RC_MISUSE,
                          "Cannot use the baselineRid option "
                          "with a cherry-pick merge.");
  }
  if(!rc) rc = fsl_cx_txn_begin(f);
  if(rc) goto end;
  inTrans = true;
  if((pid = opt->baselineRid)>0){
    if(!fsl_rid_is_version(f, pid)){
      rc = fsl_cx_err_set(f, FSL_RC_TYPE,
                          "Baseline RID #%" FSL_ID_T_PFMT
                          " does not refer to a checkin version.");
      goto end;
    }
  }
  if(FSL_MERGE_TYPE_CHERRYPICK==opt->mergeType
     || FSL_MERGE_TYPE_BACKOUT==opt->mergeType){
    pid = fsl_db_g_id(db, 0, "SELECT pid FROM plink WHERE cid=%"
                           FSL_ID_T_PFMT " AND isprim",
                           mid);
    if(0==pid){
      rc = fsl_cx_err_set(f, FSL_RC_NOT_FOUND, "Cannot find an ancestor "
                          "for to-merge RID #%" FSL_ID_T_PFMT ".",
                          mid);
      goto end;
    }
  }else{
    if(opt->baselineRid<=0){
      fsl_stmt q = fsl_stmt_empty;
      rc = fsl__pivot_set_primary(f, mid);
      if(0==rc) rc = fsl__pivot_add_secondary(f, vid);
      if(rc) goto end;
      rc = fsl_cx_prepare(f, &q, "SELECT merge FROM vmerge WHERE id=0");
      while( 0==rc && FSL_RC_STEP_ROW==fsl_stmt_step(&q) ){
        rc = fsl__pivot_add_secondary(f, fsl_stmt_g_id(&q, 0));
      }
      fsl_stmt_finalize(&q);
      if(0==rc) rc = fsl__pivot_find(f, false, &pid);
      if(rc) goto end;
      else if( pid<=0 ){
        rc = fsl_cx_err_set(f, FSL_RC_NOT_FOUND,
                            "Cannot find a common ancestor between "
                            "RID #%" FSL_ID_T_PFMT
                            " and RID #%" FSL_ID_T_PFMT ".",
                            pid, vid);
        goto end;
      }
    }
    rc = fsl__pivot_set_primary(f, mid);
    if(0==rc) rc = fsl__pivot_add_secondary(f, vid);
    if(0==rc) rc = fsl__pivot_find(f, true, &nid);
    if(rc) goto end;
    else if( nid!=pid ){
      rc = fsl__pivot_set_primary(f, nid);
      if(0==rc) rc = fsl__pivot_add_secondary(f, pid);
      if(0==rc) rc = fsl__pivot_find(f, true, &nid);
      if(rc) goto end;
    }
    /* ^^^ the above block is a great example of how much error
       checking intrudes on the library API compared to fossil(1). */
  }
  if( FSL_MERGE_TYPE_BACKOUT == opt->mergeType ){
    fsl_id_t const t = pid;
    pid = mid;
    mid = t;
  }
  if(0==nid) nid = pid;
  if(opt->debug){
    MARKER(("pid=%" FSL_ID_T_PFMT
            ", mid=%" FSL_ID_T_PFMT
            ", nid=%" FSL_ID_T_PFMT
            ", vid=%" FSL_ID_T_PFMT
            " integrate=%d\n",
            pid, mid, nid, vid, doIntegrate));
  }
  if(mid == pid){
    rc = fsl_cx_err_set(f, FSL_RC_RANGE, "Cowardly refusing to perform "
                        "no-op merge from/to RID #%" FSL_ID_T_PFMT ".",
                        mid);
    goto end;
  }else if(mid == vid){
    rc = fsl_cx_err_set(f, FSL_RC_RANGE, "Cowardly refusing to merge "
                        "version [%S] into itself.", f->db.ckout.uuid);
    goto end;
  }else if(!fsl_rid_is_version(f, pid)){
    rc = fsl_cx_err_set(f, FSL_RC_TYPE,
                        "RID #%" FSL_ID_T_PFMT " does not refer "
                        "to a checkin version.", pid);
    goto end;
  }else{
    uint32_t missing = 0;
    rc = fsl_vfile_load(f, mid, false, &missing);
    if(0==rc && 0==missing){
      rc = fsl_vfile_load(f, pid, false, &missing);
    }
    if(0==rc && missing){
      rc = fsl_cx_err_set(f, FSL_RC_PHANTOM,
                          "Cannot merge due to missing content in one "
                          "or more participating versions.");
    }
    if(rc) goto end;
  }
  if( doIntegrate && (fsl_content_is_private(f, mid)
                      || !fsl_rid_is_leaf(f, mid)) ){
    doIntegrate = false;
  }
  if(opt->scanForChanges){
    rc = fsl_vfile_changes_scan(f, vid, FSL_VFILE_CKSIG_ENOTFILE);
    if(rc) goto end;
  }
  rc = fsl__renames_tweak(f, mid, pid, vid, nid, opt);
  if(rc) goto end;
  if(opt->debug){
    MARKER(("pid=%" FSL_ID_T_PFMT
            ", mid=%" FSL_ID_T_PFMT
            ", nid=%" FSL_ID_T_PFMT
            ", vid=%" FSL_ID_T_PFMT
            " integrate=%d\n",
            pid, mid, nid, vid, doIntegrate));
    MARKER(("Contents of " FSL__TABLE_FVM ":\n"));
    if(1==opt->debug){
      fsl_db_each(db, fsl_stmt_each_f_dump, f,
                  "SELECT fn,fnp,fnm,chnged,ridv,ridp,ridm, "
                  " isexe,islinkv islinkm, fnn "
                  " FROM " FSL__TABLE_FVM
                  " WHERE chnged OR (ridv!=ridm AND ridm!=ridp)"
                  " ORDER BY fn, fnp, fnm ");
    }else{
      fsl_db_each(db, fsl_stmt_each_f_dump, f,
                  "SELECT * FROM " FSL__TABLE_FVM
                  " ORDER BY fn, fnp, fnm");
    }
    MARKER(("Contents of " FSL__TABLE_PIVOT ":\n"));
    fsl_db_each(db, fsl_stmt_each_f_dump, f,
                "SELECT * FROM " FSL__TABLE_PIVOT
                " ORDER BY src DESC, rid, pending, src");
    MARKER(("Contents of [vmerge]:\n"));
    fsl_db_each(db, fsl_stmt_each_f_dump, f,
                "SELECT * FROM vmerge order by merge");
  }
  mState.f = f;
  mState.opt = opt;
#define MCB(FCT,RMI,FN) \
  assert(opt->callback); \
  mState.fileChangeType = FCT; \
  mState.fileRmInfo = RMI; \
  mState.filename = FN; \
  rc = opt->callback(&mState); \
  mState.priorName = NULL
#define MCB2(FCT,FN) MCB(FCT,FSL_CKUP_RM_NOT,FN)
  /************************************************************************
  ** All of the information needed to do the merge is now contained in the
  ** FV table.  Starting here, we begin to actually carry out the merge.
  **
  ** First, find files that have changed from P->M but not P->V.  Copy
  ** the M content over into V.
  */
  rc = fsl_cx_prepare(f, &q,
    "SELECT idv, ridm, fn, islinkm FROM " FSL__TABLE_FVM
    " WHERE idp>0 AND idv>0 AND idm>0"
    "   AND ridm!=ridp AND ridv=ridp AND NOT chnged"
  );
  if(rc) goto end;
  while( 0==rc && (FSL_RC_STEP_ROW==fsl_stmt_step(&q)) ){
    fsl_id_t const idv = fsl_stmt_g_id(&q, 0);
    fsl_id_t const ridm = fsl_stmt_g_id(&q, 1);
    int const islinkm = fsl_stmt_g_int32(&q, 3);
    /* Copy content from idm over into idv.  Overwrite idv. */
    if(opt->debug){
      const char *zName = fsl_stmt_g_text(&q, 2, NULL);
      MARKER(("COPIED (M)=>(V) %s\n", zName));
    }
    if( !opt->dryRun ){
      //undo_save(zName);
      rc = fsl_cx_exec(f,
        "UPDATE vfile SET mtime=0, mrid=%" FSL_ID_T_PFMT
        ", chnged=%d, islink=%d,"
        " mhash=CASE WHEN rid<>%" FSL_ID_T_PFMT
        " THEN (SELECT uuid FROM blob WHERE blob.rid=%" FSL_ID_T_PFMT ") END"
        " WHERE id=%" FSL_ID_T_PFMT,
        ridm, doIntegrate ? 4 : 2, islinkm, ridm, ridm, idv
      );
      if(0==rc) rc = fsl__vfile_to_ckout(f, idv, NULL);
    }
    if(0==rc && opt->callback){
      const char *zName = 0;
      rc = fsl_stmt_get_text(&q, 2, &zName, NULL);
      if(0==rc){
        MCB2(FSL_MERGE_FCHANGE_COPIED,zName);
      }
    }
  }
  fsl_stmt_finalize(&q);
  switch(rc){
    case 0:
    case FSL_RC_STEP_DONE: rc = 0; break;
    default: goto end;
  }

  /*
  ** Do a three-way merge on files that have changes on both P->M and P->V.
  **
  ** Proceed even if the file doesn't exist on P, as if the common
  ** ancestor of M and V is an empty file. In this case, merge
  ** conflict marks will be added to the file and the user will be
  ** forced to handle them like any other conflict. The alternative is
  ** that we warn the user via opt->callback and let them decide on
  ** whether to keep the copy from P->V (historical default behavior)
  ** or error out.  As of 2023-04, fossil treats this as a normal
  ** merge conflict with an empty common ancestor version, so we'll do
  ** the same.
  */
  rc = fsl_cx_prepare(f, &q,
    "SELECT ridm, idv, ridp, ridv,"
    " FSL_GLOB('binary-glob'," FSL__TABLE_FVM ".fn),"
    " fn, isexe, islinkv, islinkm FROM " FSL__TABLE_FVM
    " WHERE /*idp>0 AND*/ idv>0 AND idm>0"
    /* -----^^^^^^^^^^^^^ https://fossil-scm.org/home/info/7c75e47b3c130ff1
       With that clause, entries with no common ancestor are filtered out. */
    "   AND ridm!=ridp AND (ridv!=ridp OR chnged)"
  );
  if(0==rc){
    rc = fsl_buffer_append( absPath, f->db.ckout.dir, (fsl_int_t)f->db.ckout.dirLen);
  }
  while( 0==rc && (FSL_RC_STEP_ROW==fsl_stmt_step(&q)) ){
    fsl_id_t const ridm = fsl_stmt_g_id(&q, 0);
    fsl_id_t const idv = fsl_stmt_g_id(&q, 1);
    fsl_id_t const ridp = fsl_stmt_g_id(&q, 2);
    fsl_id_t const  ridv = fsl_stmt_g_id(&q, 3);
    int32_t isBinary = fsl_stmt_g_int32(&q, 4);
    int32_t const isExe = fsl_stmt_g_int32(&q, 6);
    int32_t const islinkv = fsl_stmt_g_int32(&q, 7);
    int32_t const islinkm = fsl_stmt_g_int32(&q, 8);
    char const *zFullPath;
    const char *zName = NULL;
    fsl_size_t nName = 0;
    rc = fsl_stmt_get_text(&q, 5, &zName, &nName);
    if(rc){
      rc = fsl_cx_uplift_db_error2(f, NULL, rc);
      break;
    }
    /* Do a 3-way merge of idp->idm into idp->idv.  The results go into idv. */
    if(opt->debug){
      MARKER(("MERGE %s  (pivot=%d v1=%d v2=%d)\n",
              zName, (int)ridp, (int)ridm, (int)ridv));
    }
    if( islinkv || islinkm ){
      //MARKER(("***** Cannot merge symlink %s\n", zName));
      if(opt->callback){
        MCB2(FSL_MERGE_FCHANGE_CONFLICT_SYMLINK,zName);
      }
    }else if(isBinary){
      if(opt->callback){
        MCB2(FSL_MERGE_FCHANGE_CONFLICT_BINARY,zName);
      }
    }else{
      fsl_buffer m = fsl_buffer_empty;
      fsl_buffer p = fsl_buffer_empty;
      fsl_buffer r = fsl_buffer_empty;
      //if( !dryRunFlag ) undo_save(zName);
      if(opt->debug){
        MARKER(("Merge: %s\n", zName));
      }
      absPath->used = f->db.ckout.dirLen;
      rc = fsl_buffer_append(absPath, zName, (fsl_int_t)nName);
      if(rc) break;
      zFullPath = fsl_buffer_cstr(absPath);
      if(ridp) rc = fsl_content_get(f, ridp, &p);
      if(0==rc) rc = fsl_content_get(f, ridm, &m);
      if(0==rc){
        //unsigned mergeFlags = dryRunFlag ? MERGE_DRYRUN : 0;
        //if(keepMergeFlag!=0) mergeFlags |= MERGE_KEEP_FILES;
        //rc = merge_3way(&p, zFullPath, &m, &r, mergeFlags);
        unsigned int nConflict = 0;
        fsl_buffer * const contentLocal = fsl__cx_content_buffer(f);
        rc = fsl_buffer_fill_from_filename(contentLocal, zFullPath);
        if(0==rc){
          rc = fsl_buffer_merge3( &p, contentLocal, &m, &r, &nConflict );
          switch(rc){
            case 0:
              if(opt->debug){
                MARKER(("%swriting merged file w/ %u conflict(s): %s\n",
                        opt->dryRun ? "Not " : "", nConflict, zName));
              }
              if(!opt->dryRun){
                rc = fsl_buffer_to_filename(&r, zFullPath);
                if(0==rc) fsl_file_exec_set(zFullPath, !!isExe);
              }
              break;
            case FSL_RC_DIFF_BINARY:
            case FSL_RC_TYPE:
              /* 2021-12-15: fsl_buffer_merge3() currently returns
                 FSL_RC_TYPE for binary, but "should" return
                 FSL_RC_DIFF_BINARY.  Changing that is TODO. */
              rc = 0; isBinary = 1;
              break;
            default: break;
          }
        }
        fsl__cx_content_buffer_yield(f);
        if(0==rc && !isBinary){
          rc = fsl_cx_exec(f, "UPDATE vfile "
                           "SET mtime=0 WHERE id=%" FSL_ID_T_PFMT, idv);
        }
        if(0==rc && opt->callback){
          fsl_merge_fchange_e const fce =
            isBinary
            ? FSL_MERGE_FCHANGE_CONFLICT_BINARY
            : (nConflict
               ? FSL_MERGE_FCHANGE_CONFLICT_MERGED
               : FSL_MERGE_FCHANGE_MERGED);
          MCB2(fce,zName);
        }
      }
      fsl_buffer_clear(&p);
      fsl_buffer_clear(&m);
      fsl_buffer_clear(&r);
    }
    if(0==rc) rc = fsl__vmerge_insert(f, idv, ridm);
  }
  fsl_stmt_finalize(&q);
  switch(rc){
    case 0:
    case FSL_RC_STEP_DONE: rc = 0; break;
    default: goto end;
  }

  /*
  ** Drop files that are in P and V but not in M
  */
  rc = fsl_cx_prepare(f, &q,
    "SELECT idv, fn, chnged FROM " FSL__TABLE_FVM
    " WHERE idp>0 AND idv>0 AND idm=0"
  );
  while( 0==rc && FSL_RC_STEP_ROW==(rc=fsl_stmt_step(&q)) ){
    fsl_id_t const idv = fsl_stmt_g_id(&q, 0);
    int32_t const chnged = fsl_stmt_g_int32(&q, 2);
    const char *zName;
    fsl_size_t nName = 0;
    rc = fsl_stmt_get_text(&q, 1, &zName, &nName);
    if(rc) break;
    /* Delete the file idv */
    if(opt->debug){
      MARKER(("DELETE %s\n", zName));
    }
    if( chnged ){
      if(opt->debug){
        MARKER(("WARNING: local edits lost for %s", zName));
      }
    }
    //if( !dryRunFlag ) undo_save(zName);
    rc = fsl_cx_exec(f,
      "UPDATE vfile SET deleted=1 WHERE id=%" FSL_ID_T_PFMT, idv
    );
    if(!chnged && !opt->dryRun ){
      /* ^^^ this differs from fossil(1), which always deletes the
         local file regardless of whether it has local changes. */
      absPath->used = f->db.ckout.dirLen;
      rc = fsl_buffer_append(absPath, zName, (fsl_int_t)nName);
      if(0==rc) fsl_file_unlink(fsl_buffer_cstr(absPath));
    }
    if(opt->callback){
      fsl_ckup_rm_state_e const rme =
        chnged ? FSL_CKUP_RM_KEPT : FSL_CKUP_RM;
      MCB(FSL_MERGE_FCHANGE_RM,rme,zName);
    }
  }
  fsl_stmt_finalize(&q);
  switch(rc){
    case 0:
    case FSL_RC_STEP_DONE: rc = 0; break;
    default: goto end;
  }

  /* For certain sets of renames (e.g. A -> B and B -> A), a file that is
  ** being renamed must first be moved to a temporary location to avoid
  ** being overwritten by another rename operation. A row is added to the
  ** TMPRN table for each of these temporary renames.
  */
  rc = fsl_cx_exec_multi(f,
    "CREATE TEMP TABLE IF NOT EXISTS tmprn(fn UNIQUE, tmpfn);"
    "DELETE FROM tmprn;"
  );

  /*
  ** Rename files that have taken a rename on P->M but which keep the same
  ** name on P->V.  If a file is renamed on P->V only or on both P->V and
  ** P->M then we retain the V name of the file.
  */
  rc = fsl_cx_prepare(f, &q,
    "SELECT idv, fnp, fnm, isexe FROM " FSL__TABLE_FVM
    " WHERE idv>0 AND idp>0 AND idm>0 AND fnp=fn AND fnm!=fnp"
  );
  while( 0==rc && FSL_RC_STEP_ROW==(rc=fsl_stmt_step(&q)) ){
    fsl_id_t const idv = fsl_stmt_g_id(&q, 0);
    int32_t const isExe = fsl_stmt_g_int32(&q, 3);
    const char *zOldName;
    const char *zNewName;
    rc = fsl_stmt_get_text(&q, 1, &zOldName, NULL);
    if(0==rc) rc = fsl_stmt_get_text(&q, 2, &zNewName, NULL);
    if(rc) break;
    if(opt->debug){
      MARKER(("RENAME %s -> %s\n", zOldName, zNewName));
    }
    //if( !dryRunFlag ) undo_save(zOldName);
    //if( !dryRunFlag ) undo_save(zNewName);
    rc = fsl_cx_exec_multi(f,
      "UPDATE vfile SET pathname=NULL, origname=pathname"
      " WHERE vid=%" FSL_ID_T_PFMT " AND pathname=%Q;"
      "UPDATE vfile SET pathname=%Q, origname=coalesce(origname,pathname)"
      " WHERE id=%" FSL_ID_T_PFMT ";",
      vid, zNewName, zNewName, idv
    );
    if(rc) break;
    if( !opt->dryRun ){
      fsl_buffer * const bFullOld = fsl__cx_scratchpad(f);
      fsl_buffer * const bFullNew = fsl__cx_scratchpad(f);
      fsl_buffer * const bTmp = fsl__cx_scratchpad(f);
      char const *zFullOldPath;
      char const *zFullNewPath;
      bool const realSymlinks = fsl_cx_allows_symlinks(f, false);
      rc = fsl_db_get_buffer(db, bFullOld, false,
                             "SELECT tmpfn FROM tmprn WHERE fn=%Q", zOldName);
      if(!rc && !bFullOld->used){
        rc = fsl_buffer_appendf(bFullOld, "%s%s", f->db.ckout.dir, zOldName);
      }
      if(0==rc) rc = fsl_buffer_appendf(bFullNew, "%s%s", f->db.ckout.dir, zNewName);
      if(rc) goto merge_rename_end;
      zFullOldPath = fsl_buffer_cstr(bFullOld);
      zFullNewPath = fsl_buffer_cstr(bFullNew);
      if( fsl_file_size(zFullNewPath)>=0 ){
        assert(!bTmp->used);
        rc = fsl_file_tempname(bTmp, "", NULL);
        if(rc) goto merge_rename_end;
        rc = fsl_cx_exec(f, "INSERT INTO tmprn(fn,tmpfn) VALUES(%Q,%B)",
                         zNewName, bTmp);
        if(rc) goto merge_rename_end;
        rc = fsl_is_symlink(zFullNewPath)
          ? fsl_symlink_copy(zFullNewPath, fsl_buffer_cstr(bTmp), realSymlinks)
          : fsl_file_copy(zFullNewPath, fsl_buffer_cstr(bTmp));
        if(rc){
          rc = fsl_cx_err_set(f, rc, "Error copying file [%s].",
                              zFullNewPath);
        }
        if(rc) goto merge_rename_end;
      }
      rc = fsl_is_symlink(zFullOldPath)
        ? fsl_symlink_copy(zFullOldPath, zFullNewPath, realSymlinks)
        : fsl_file_copy(zFullOldPath, zFullNewPath);
      if(0==rc){
        fsl_file_exec_set(zFullNewPath, !!isExe);
        fsl_file_unlink(zFullOldPath);
        /* ^^^ Ignore errors: not critical here */
      }
      merge_rename_end:
      fsl__cx_scratchpad_yield(f, bFullOld);
      fsl__cx_scratchpad_yield(f, bFullNew);
      fsl__cx_scratchpad_yield(f, bTmp);
    }
    if(0==rc && opt->callback){
      mState.priorName = zOldName;
      MCB2(FSL_MERGE_FCHANGE_RENAMED,zNewName);
    }
  }
  fsl_stmt_finalize(&q);
  switch(rc){
    case 0:
    case FSL_RC_STEP_DONE: rc = 0; break;
    default: goto end;
  }
  /**
     TODO??? The above loop can leave temp files laying around. We
     should(?)  to (for each tmpfn in tmprn =>
     unlink(tmpfn)). fossil(1) does not do that, but that seems like a
     bug.
  */
  /* A file that has been deleted and replaced by a renamed file will have a
  ** NULL pathname. Change it to something that makes the output of "status"
  ** and similar commands make sense for such files and that will (most likely)
  ** not be an actual existing pathname.
  */
  rc = fsl_cx_exec(f,
    "UPDATE vfile SET pathname=origname || ' (overwritten by rename)'"
    " WHERE pathname IS NULL"
  );
  if(rc) goto end;
  /*
  ** Insert into V any files that are not in V or P but are in M.
  */
  rc = fsl_cx_prepare(f, &q,
    "SELECT idm, fnm FROM " FSL__TABLE_FVM
    " WHERE idp=0 AND idv=0 AND idm>0"
  );
  while( 0==rc && FSL_RC_STEP_ROW==(rc=fsl_stmt_step(&q)) ){
    fsl_id_t const idm = fsl_stmt_g_id(&q, 0);
    const char *zName;
    fsl_buffer * const bFullName = fsl__cx_scratchpad(f);
    fsl_merge_fchange_e fchange = FSL_MERGE_FCHANGE_NONE
      /* this initialization is bogus, but some gcc versions
         incorrectly report that it may be used uninitialized */;
    rc = fsl_cx_exec(f,
      "REPLACE INTO vfile(vid,chnged,deleted,rid,mrid,"
                         "isexe,islink,pathname,mhash)"
      "  SELECT %" FSL_ID_T_PFMT ",%d,0,rid,mrid,isexe,islink,pathname,"
            "CASE WHEN rid<>mrid"
            " THEN (SELECT uuid FROM blob WHERE blob.rid=vfile.mrid) END "
            "FROM vfile WHERE id=%" FSL_ID_T_PFMT,
      vid, doIntegrate
           ? FSL_VFILE_CHANGE_INTEGRATE_MOD
           : FSL_VFILE_CHANGE_MERGE_ADD,
      idm);
    if(0==rc) rc = fsl_stmt_get_text(&q, 1, &zName, NULL);
    if(0==rc) rc = fsl_buffer_appendf(bFullName, "%s%s", f->db.ckout.dir, zName);
    if(rc) goto merge_add_end;
    if( fsl_is_file_or_link(fsl_buffer_cstr(bFullName))
        && !fsl_db_exists(db, "SELECT 1 FROM fv WHERE fn=%Q", zName) ){
      if(opt->debug){
        MARKER(("ADDED %s (overwrites an unmanaged file)\n", zName));
      }
      fchange = FSL_MERGE_FCHANGE_CONFLICT_ADDED_UNMANAGED;
    }else{
      if(opt->debug){
        MARKER(("ADDED %s\n", zName));
      }
      fchange = FSL_MERGE_FCHANGE_ADDED;
    }
    if( !opt->dryRun ){
      //undo_save(zName);
      rc = fsl__vfile_to_ckout(f, idm, NULL);
    }
    merge_add_end:
    fsl__cx_scratchpad_yield(f, bFullName);
    if(0==rc && opt->callback){
      assert(FSL_MERGE_FCHANGE_CONFLICT_ADDED_UNMANAGED==fchange
             || FSL_MERGE_FCHANGE_ADDED==fchange);
      MCB2(fchange,zName);
    }
  }
  fsl_stmt_finalize(&q);
  switch(rc){
    case 0:
    case FSL_RC_STEP_DONE: rc = 0; break;
    default: goto end;
  }

  fsl_id_t vmergeWho = 0;
  switch(opt->mergeType){
    case FSL_MERGE_TYPE_CHERRYPICK:
      vmergeWho = mid;
      /* For a cherry-pick merge, make the default check-in comment the same
      ** as the check-in comment on the check-in that is being merged in. */
      if(0==rc){
        rc = fsl_cx_exec(f,
               "REPLACE INTO vvar(name,value)"
               " SELECT 'ci-comment', coalesce(ecomment,comment) FROM event"
               "  WHERE type='ci' AND objid=%" FSL_ID_T_PFMT,
               mid);
      }
      break;
    case FSL_MERGE_TYPE_BACKOUT:
      vmergeWho = pid;
      break;
    case FSL_MERGE_TYPE_INTEGRATE:
    case FSL_MERGE_TYPE_NORMAL:
      vmergeWho = mid;
      break;
  }
  if(0==rc){
    rc = fsl__vmerge_insert(f, (int)opt->mergeType, vmergeWho);
  }
#undef MCB
#undef MCB2
  end:
  if(opt->debug){
    MARKER(("fsl_ckout_merge() made it to the end with rc %s.\n",
            fsl_rc_cstr(rc)));
  }
  fsl__cx_scratchpad_yield(f, absPath);
  if(0==rc){
    fsl__renames_finalize(f);
  }
  fsl_stmt_finalize(&q);
  if(inTrans){
    if(0==rc){
      rc = fsl_vfile_unload_except(f, vid);
      // ^^^ not strictly needed unless we're NOT rolling back
    }
    int const rc2 = fsl_cx_txn_end_v2(f, 0==rc && !opt->dryRun, 0!=rc);
    if(rc2 && 0==rc) rc = rc2;
  }
  return rc;
}

#undef FSL__TABLE_PIVOT
#undef FSL__TABLE_FVM

#undef MARKER
