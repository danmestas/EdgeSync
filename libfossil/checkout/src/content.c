/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2025 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/
/**************************************************************************
  This file houses the code for the fsl_content_xxx() APIS.
*/
#include "fossil-scm/core.h"
#include "fossil-scm/internal.h"
#include "fossil-scm/hash.h"
#include "fossil-scm/checkout.h"
#include <assert.h>
#include <memory.h> /* memcmp() */

/* Only for debugging */
#include <stdio.h>
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

fsl_int_t fsl_content_size( fsl_cx * const f, fsl_id_t blobRid ){
  if(blobRid<=0) return -3;
  else if(!fsl_needs_repo(f)) return -4;
  else{
    int rc = 0;
    fsl_int_t rv = -2;
    fsl_stmt * const q = fsl__cx_stmt(f, fsl__cx_stmt_e_contentSize);
    if( q ){
      rc = fsl_stmt_bind_step_v2(q, "R", blobRid);
      if(FSL_RC_STEP_ROW==rc){
        rv = (fsl_int_t)fsl_stmt_g_int64(q, 0);
      }
      fsl__cx_stmt_yield(f, q);
    }else{
       rv = -6;
    }
    return rv;
  }
}

int fsl_content_size_v2(fsl_cx * const f, fsl_id_t rid, fsl_int_t *pOut){
  int rc;
  fsl_db * const db = fsl_needs_repo(f);
  fsl_stmt * const q = db ? fsl__cx_stmt(f, fsl__cx_stmt_e_contentSize) : 0;
  if( !q ) return f->error.code;
  rc = fsl_stmt_bind_step_v2(q, "R", rid);
  switch( rc ){
    case FSL_RC_STEP_ROW:
      *pOut = fsl_stmt_g_int32(q, 0);
      rc = 0;
      break;
    case FSL_RC_STEP_DONE:
      rc = fsl_cx_err_set(f, FSL_RC_NOT_FOUND,
                          "No such blob.rid: %"FSL_ID_T_PFMT,
                          rid);
      break;
    default:
      fsl_cx_uplift_db_error(f, db);
      break;
  }
  fsl__cx_stmt_yield(f, q);
  return rc;
}

#undef fsl__content_size_sql


static bool fsl__content_is_available(fsl_cx * const f, fsl_id_t rid){
  fsl_id_t srcid = 0;
  int rc = 0, depth = 0 /* Limit delta recursion depth */;
  while( depth++ < 100000 ){
    if( fsl__cx_ptl_has(f, fsl__ptl_e_missing, rid) ){
      return false;
    }else if( fsl__cx_ptl_has(f, fsl__ptl_e_available, rid) ){
      return true;
    }else if( fsl_content_size(f, rid)<0 ){
      fsl__cx_ptl_insert(f, fsl__ptl_e_missing, rid)
        /* ignore possible OOM error - not fatal */;
      f->cache.ptl.rc = 0;
      return false;
    }
    rc = fsl_delta_r2s(f, rid, &srcid);
    if(rc) break;
    else if( 0==srcid ){
      fsl__cx_ptl_insert(f, fsl__ptl_e_available, rid)
        /* ignore possible OOM error - not fatal */;
      f->cache.ptl.rc = 0;
      return true;
    }
    rid = srcid;
  }
  if(0==rc){
    /* This "cannot happen" (never has historically, and would be
       indicative of what amounts to corruption in the repo). */
    fsl__fatal(FSL_RC_RANGE,"delta-loop in repository");
  }
  return false;
}


int fsl_content_raw( fsl_cx * const f, fsl_id_t blobRid, fsl_buffer * const tgt ){
  if(!fsl_needs_repo(f)) return FSL_RC_NOT_A_REPO;
  else if(blobRid<=0){
    return fsl_cx_err_set(f, FSL_RC_RANGE,
                          "Invalid RID for %s().", __func__);
  }
  int rc = 0;
  fsl_stmt * const q = fsl__cx_stmt(f, fsl__cx_stmt_e_contentBlob);
  if(!q) return f->error.code;
  rc = fsl_stmt_bind_step_v2(q, "R", blobRid);
  if( FSL_RC_STEP_ROW==rc ){
    void const * mem = NULL;
    fsl_size_t memLen = 0;
    int64_t const sz = fsl_stmt_g_int64(q, 1);
    if(sz<0){
      rc = fsl_cx_err_set(f, FSL_RC_PHANTOM,
                          "Cannot fetch content for phantom "
                          "blob #%"FSL_ID_T_PFMT".",
                          blobRid);
    }else if(sz){
      rc = fsl_stmt_get_blob(q, 0, &mem, &memLen);
      if(rc){
        rc = fsl_cx_err_set(f, rc,
                            "Error %R fetching blob content for "
                            "blob #%"FSL_ID_T_PFMT".", rc, blobRid);
      }else{
        fsl_buffer bb = fsl_buffer_empty;
        assert(memLen>0);
        fsl_buffer_external(&bb, mem, memLen);
        rc = fsl_buffer_uncompress(&bb, tgt);
      }
    }else{
      rc = 0;
      fsl_buffer_reuse(tgt);
    }
    fsl__cx_stmt_yield(f, q);
  }else if( FSL_RC_STEP_DONE==rc ){
    rc = fsl_cx_err_set(f, FSL_RC_NOT_FOUND,
                        "No blob found for rid %"FSL_ID_T_PFMT".",
                        blobRid);
  }else{
    rc = fsl_cx_uplift_db_error2(f, q->db, rc);
  }
  return rc;
}

bool fsl_content_is_private(fsl_cx * const f, fsl_id_t rid){
  fsl_stmt * const q = fsl__cx_stmt(f, fsl__cx_stmt_e_ridIsPrivate);
  bool rv = false;
  if( q ){
    rv = FSL_RC_STEP_ROW==fsl_stmt_bind_step(q, "R", rid);
    fsl__cx_stmt_yield(f, q);
  }
  return rv;
}

/**
   Helper for f->cache.stmt members in the form (INSERT [OR IGNORE]
   INTO t VALUES(?1)), where ?1 is an RID.

   MUST only be passed q values which come from fsl__cx_stmt().
*/
static int fsl__rid_insert(fsl_cx * const f, fsl_id_t rid, fsl_stmt * const q){
  assert( fsl_cx_txn_level(f) );
  int rc;
  if( q ){
    assert( q->stmt );
    rc = fsl_stmt_bind_step(q, "R", rid);
    fsl__cx_stmt_yield(f, q);
  }else{
    rc = f->error.code;
  }
  return rc;
}

int fsl__private_rid_add(fsl_cx * const f, fsl_id_t rid){
  return fsl__rid_insert(
    f, rid, fsl__cx_stmt(f, fsl__cx_stmt_e_insertPrivateRid)
  );
}

int fsl__unclustered_rid_add(fsl_cx * const f, fsl_id_t rid){
  return fsl__rid_insert(
    f, rid, fsl__cx_stmt(f, fsl__cx_stmt_e_insertUnclusteredRid)
  );
}

int fsl__phantom_rid_add(fsl_cx * const f, fsl_id_t rid){
  return fsl__rid_insert(
    f, rid, fsl__cx_stmt(f, fsl__cx_stmt_e_insertPhantomRid)
  );
}

int fsl__unsent_rid_add(fsl_cx * const f, fsl_id_t rid){
  return fsl__rid_insert(
    f, rid, fsl__cx_stmt(f, fsl__cx_stmt_e_insertUnsentRid)
  );
}

int fsl__rid_delete(fsl_cx * const f, fsl_dbrole_e dbRole,
                    char const *zTable, fsl_id_t rid){
  assert( fsl_cx_txn_level(f) );
  return fsl_cx_exec(f, "DELETE FROM %!Q.%!Q WHERE rid=%"FSL_ID_T_PFMT
                     " /* %s() */",
                     fsl_db_role_name(dbRole), zTable, rid, __func__);
}

int fsl__delta_delete(fsl_cx * const f, fsl_id_t rid){
  return fsl__rid_delete(f, FSL_DBROLE_REPO, "delta", rid);
}

int fsl__private_rid_delete(fsl_cx * const f, fsl_id_t rid){
  return fsl__rid_delete(f, FSL_DBROLE_REPO, "private", rid);
}

static int fsl__phantom_rid_delete(fsl_cx * const f, fsl_id_t rid){
  return fsl__rid_delete(f, FSL_DBROLE_REPO, "phantom", rid);
}


int fsl_content_get( fsl_cx * const f, fsl_id_t rid,
                     fsl_buffer * const tgt ){
  fsl_db * const db = fsl_cx_db_repo(f);
  if( !tgt ) return FSL_RC_MISUSE;
  else if( rid<=0 ){
    return fsl_cx_err_set(f, FSL_RC_RANGE,
                          "RID %"FSL_ID_T_PFMT" is out of range.",
                          rid);
  }else if( !db ){
    return fsl_cx_err_set(f, FSL_RC_NOT_A_REPO,
                          "Fossil has no repo opened.");
  }
  else{
    int rc;
    bool gotIt = false;
    fsl_id_t nextRid;
    fsl__bccache * const ac = &f->cache.blobContent;
    if( fsl__cx_ptl_has(f, fsl__ptl_e_missing, rid) ){
      /* Early out if we know the content is not available */
      return FSL_RC_NOT_FOUND;
    }
    /* Look for the artifact in the cache first */
    if(0!=(FSL_CX_F_BLOB_CACHE & f->flags)
       && fsl_id_bag_contains(&ac->inCache, rid) ){
      fsl_size_t i;
      for(i=0; i<ac->used; ++i){
        fsl__bccache_line * const line = &ac->list[i];
        if( line->rid==rid ){
          ++ac->metrics.hits;
          rc = fsl_buffer_copy(tgt, &line->content);
          line->age = ac->nextAge++;
          return rc;
        }
      }
    }
    fsl_buffer_reuse(tgt);
    ++ac->metrics.misses;
    nextRid = 0;
    rc = fsl_delta_r2s(f, rid, &nextRid);
    /* MARKER(("rc=%d, nextRid=%"FSL_ID_T_PFMT"\n", rc, nextRid)); */
    if(rc) return rc;
    if( nextRid == 0 ){
      /* This is not a delta, so get its raw content. */
      rc = fsl_content_raw(f, rid, tgt);
      gotIt = 0==rc;
    }else{
      /* Looks like a delta, so let's expand it... */
      fsl_int_t n           /* number of used entries in 'a' */;
      fsl_int_t mx;
      fsl_id_t * a = NULL;
      //fsl_buffer D = fsl_buffer_empty;
      fsl_buffer * const delta = &f->cache.deltaContent;
      fsl_buffer next = fsl_buffer_empty  /* delta-applied content */ ;
      assert(nextRid>0);
      unsigned int nAlloc = 20;
      a = (fsl_id_t*)fsl_malloc(sizeof(fsl_id_t) * nAlloc);
      if(!a){
        fsl_report_oom;
        rc = FSL_RC_OOM;
        goto end_delta;
      }
      a[0] = rid;
      a[1] = nextRid;
      n = 1;
      while( !fsl_id_bag_contains(&ac->inCache, nextRid)
             && 0==(rc=fsl_delta_r2s(f, nextRid, &nextRid))
             && (nextRid>0)){
        /* Figure out how big n needs to be... */
        ++n;
        if( n >= (fsl_int_t)nAlloc ){
          /* Expand 'a' */
          void * remem;
          if( n > fsl_db_g_int64(db, 0,
                                "SELECT max(rid) FROM blob")){
            rc = fsl_cx_err_set(f, FSL_RC_RANGE,
                                "Infinite loop in delta table.");
            goto end_delta;
          }
          unsigned int const nAlloc2 = nAlloc * 2;
          remem = fsl_realloc(a, nAlloc2 * sizeof(fsl_id_t));
          if(!remem){
            rc = FSL_RC_OOM;
            goto end_delta;
          }
          a = (fsl_id_t*)remem;
          nAlloc = nAlloc2;
          /*MARKER(("deltaIds allocated = %u\n", nAlloc));*/
        }
        a[n] = nextRid;
      }
      if( rc ) goto end_delta;
      /**
         Recursively expand deltas to get the content...
      */
      mx = n;
      rc = fsl_content_get( f, a[n], tgt );
      /* MARKER(("Getting content for rid #%"FSL_ID_T_PFMT", rc=%d\n", a[n], rc)); */
      --n;
      for( ; !rc && (n>=0); --n){
        rc = fsl_content_raw(f, a[n], delta);
        /* MARKER(("Getting/applying delta rid #%"FSL_ID_T_PFMT", rc=%d\n", a[n], rc)); */
        if(rc) break;
        if(!delta->used){
          assert(!"Is this possible? The fossil tree has a similar "
                 "condition but i naively don't believe it's necessary.");
          continue;
        }
        rc = fsl_buffer_delta_apply2(tgt, delta, &next, &f->error);
        //assert(FSL_RC_RANGE!=rc);
        if(rc) break;
#if 1
        /*
           2021-03-24: in a debug build, running:

           f-parseparty -t c -c -q

           (i.e.: parse and crosslink all checkin artifacts)

           on the libfossil repo with 2003 checkins takes:

           10.5s without this cache
           5.2s with this cache

           We shave another 0.5s if we always cache instead of using
           this mysterious (mx-n)%8 heuristic.

           Later testing with f-rebuild gives much different results:
           the (mx-n)%8 heuristic provides the best results of the
           variations tested, including always caching.
        */
        //MARKER(("mx=%d, n=%d, (mx-n)%%8=%d\n",
        //(int)mx, (int)n, (int)(mx-n)%8));
        //MARKER(("nAlloc=%d\n", (int)nAlloc));
        if( (mx-n)%8==0 ){
          //MARKER(("Caching artifact %d\n", (int)a[n+1]));
          rc = fsl__bccache_insert( ac, a[n+1], tgt );
          assert(!tgt->mem && "Passed to artifact cache (even on failure).");
          if( rc ) break;
        }else{
          fsl_buffer_clear(tgt);
        }
#else
        if(mx){/*unused var*/}
        fsl_buffer_clear(tgt);
#endif
        fsl_buffer_swap(tgt, &next);
        fsl_buffer_reuse(&next);
      }/*for 0..n*/
      end_delta:
      fsl_free(a);
      fsl_buffer_reuse(delta);
      fsl_buffer_clear(&next);
      gotIt = 0==rc;
    }

    if(!rc){
      rc = fsl__cx_ptl_insert(f, gotIt
                              ? fsl__ptl_e_available
                              : fsl__ptl_e_missing,
                              rid);
    }
    return rc;
  }
}

int fsl_content_get_sym( fsl_cx * const f, char const * sym,
                         fsl_buffer * const tgt ){
  int rc;
  fsl_db * db = f ? fsl_needs_repo(f) : NULL;
  fsl_id_t rid = 0;
  if(!f || !sym || !tgt) return FSL_RC_MISUSE;
  else if(!db) return FSL_RC_NOT_A_REPO;
  rc = fsl_sym_to_rid(f, sym, FSL_SATYPE_ANY, &rid);
  return rc ? rc : fsl_content_get(f, rid, tgt);
}

/**
    Mark artifact rid as being available now. Update f's cache to show
    that everything that was formerly unavailable because rid was
    missing is now available. Returns 0 on success. f must have an
    opened repo and rid must be valid. On error (allocation failure)
    the state of the cache is not well-defined.
*/
static int fsl__content_mark_available(fsl_cx * const f, fsl_id_t rid){
  assert(f);
  assert(rid>0);
  if( fsl__cx_ptl_has(f, fsl__ptl_e_available, rid) ) return 0;
#if 1
  int rc = 0;
  while( 0==rc && rid>0 ){
    rc = fsl__cx_ptl_insert(f, fsl__ptl_e_available, rid);
    if(rc) break;
    //MARKER(("rid %" FSL_ID_T_PFMT "\n", rid));
    fsl__cx_ptl_remove(f, fsl__ptl_e_missing, rid);
    rc = fsl_delta_s2r(f, rid, &rid);
  }
#else
  /* Pre-2025-08-14 impl */
  fsl_id_bag * const pending =
    fsl_id_bag_reuse(&f->cache.markAvailableCache);
  int rc = fsl_id_bag_insert(pending, rid);
  while( 0==rc && (rid = fsl_id_bag_first(pending))!=0 ){
    fsl_id_bag_remove(pending, rid);
    rc = fsl__cx_ptl_insert(f, fsl__ptl_e_available, rid);
    if(rc) break;
    //MARKER(("Marking available %" FSL_ID_T_PFMT "\n", rid));
    fsl__cx_ptl_remove(f, fsl__ptl_e_missing, rid);
    fsl_id_t ridOther = 0;
    rc = fsl_delta_s2r(f, rid, &ridOther);
    if( ridOther>0 ){
      rc = fsl_id_bag_insert(pending, ridOther);
    }
  }
#endif
  return rc;
}

/**
   Attempt to parse the given RID as a deck and crosslink it. If the RID
   is not a deck, set *isDeck=false and return 0.
*/
static int fsl__attempt_crosslink(fsl_cx *f, fsl_id_t rid, bool *isADeck){
  fsl_buffer * const b = fsl__cx_content_buffer(f);
  int rc = fsl_content_get(f, rid, b);
  if( 0==rc ){
    fsl_deck deck = fsl_deck_empty;
    fsl_deck_init(f, &deck, FSL_SATYPE_ANY);
    rc = fsl_deck_parse2(&deck, b, rid);
    assert( FSL_RC_NOT_FOUND!=rc );
    if( 0==rc ){
      *isADeck = true;
      rc = fsl__deck_crosslink(&deck);
#if 0
      if( FSL_RC_NOT_FOUND==rc && (deck.P.used || deck.Q.used || deck.B.uuid) ){
        /* Assume a parent is still a phantom. */
        /*
          We have an architectural issue here: since the beginning,
          libfossil has, in routines which push a transaction level,
          put the transaction in rollback for any (or just about any)
          failure which happens within the scope of that transaction
          level. That's all fine and good for how it's been used up
          until Aug 2025.  The addition of the sync pieces introduces
          phantoms, something the lib was designed to account for but
          which it had never actually had to deal with in practice
          (and certainly not as part of fsl__content_put_ex()).
          Phantoms can trigger errors, like FSL_RC_NOT_FOUND, which
          may, depending on where they're triggered, put the
          transaction into a rollback state.
          We might need a completely
          different (from fossil(1)'s) approach to
          post-dephantomization. e.g. we could queue up all phantoms
          into a new table, figure out an order to handle them (via
          their delta chains) and dephantomize them in that order. No
          idea if that's really feasible.
        */
        rc = 0;
        //MARKER(("Skipping failed crosslink, presumably due to phantom pieces\n"));
      }
#endif
      assert( FSL_RC_NOT_FOUND!=rc );
    }else if( FSL_RC_SYNTAX==rc ){
      /* This can't be a manifest. The rule is "if it doesn't parse
         as a manifest, it's not one," and we specifically want to
         ignore any it's-not-a-manifest parsing errors here. */
      *isADeck = false;
      rc = 0;
      //assert( !f->dbMain->doRollback );
      fsl_cx_err_reset(f);
    }
    fsl_deck_finalize(&deck);
  }else if( FSL_RC_NOT_FOUND==rc ){
    /* Assume it's a phantom. Ignore it. */;
    rc = 0;
    fsl_cx_err_reset(f);
    assert(!f->error.code);
  }
  assert( FSL_RC_NOT_FOUND!=rc );
  fsl__cx_content_buffer_yield(f);
  return rc;
}

/**
   When a record is converted from a phantom to a real record, if that
   record has other records that are derived by delta, then call
   fsl__deck_crosslink() on those other records.

   If the formerly phantom record or any of the other records derived
   by delta from the former phantom are a baseline manifest, then also
   invoke fsl__deck_crosslink() on the delta-manifests associated with
   that baseline.

   Returns 0 on success, any number of non-0 results on error.

   The 3rd argument must always be 0 except in recursive
   calls to this function.
*/
//static
int fsl__after_dephantomize(fsl_cx * f, fsl_id_t rid, bool tryCrosslink){
  int rc = 0;
  unsigned nChildAlloc = 0;
  fsl_id_t * aChild = 0;
  fsl_buffer bufChild = fsl_buffer_empty;
  fsl_db * const db = fsl_cx_db_repo(f);
  fsl_stmt q = fsl_stmt_empty;
  char const * const zRepoDbName = fsl_db_role_name(FSL_DBROLE_REPO);

  MARKER(("rid %d tryCrosslink=%d\n", (int)rid, (int)tryCrosslink));
  assert( db );
  assert( fsl_cx_txn_level(f)>0 );
  if(f->cache.ignoreDephantomizations) return 0;
  while(rid){
    unsigned nChildUsed = 0;
    unsigned i = 0;

    /* See if we can parse it as a manifest */
    if( tryCrosslink ) {
      rc = fsl__attempt_crosslink(f, rid, &tryCrosslink);
      assert( FSL_RC_NOT_FOUND!=rc );
      if( rc ){
#if 1
        if( FSL_RC_PHANTOM==rc ){
          MARKER(("We need(?) to delay this step until the end of "
                  "sync due to phantoms\n"));
        }
#endif
        fsl__bprc(rc);
        break;
      }
    }

    if( tryCrosslink ){
      /* Parse all delta-manifests that depend on baseline-manifest rid */
      /* 2025-08-15: libfossil does not yet inject anything into the
         orphan table */
      rc = fsl_cx_prepare(f, &q,
                          "SELECT rid FROM %!Q.orphan "
                          "WHERE baseline=%" FSL_ID_T_PFMT "/*%s()*/",
                          zRepoDbName, rid, __func__);
      if(rc) break;
      while(FSL_RC_STEP_ROW==fsl_stmt_step(&q)){
        fsl_id_t const child = fsl_stmt_g_id(&q, 0);
        if(nChildUsed>=nChildAlloc){
          nChildAlloc = nChildAlloc*2 + 10;
          rc = fsl_buffer_reserve(&bufChild, sizeof(fsl_id_t)*nChildAlloc);
          if(rc) goto end;
          aChild = (fsl_id_t*)bufChild.mem;
        }
        aChild[nChildUsed++] = child;
      }
      fsl_stmt_finalize(&q);
      for(i=0; i<nChildUsed; ++i){
        fsl_deck deck = fsl_deck_empty;
        rc = fsl_deck_load_rid(f, &deck, aChild[i], FSL_SATYPE_CHECKIN);
        if(!rc){
          assert(aChild[i]==deck.rid);
          rc = fsl__deck_crosslink(&deck);
        }
        fsl_deck_finalize(&deck);
        if(rc){
          fsl__bprc(rc);
          goto end;
        }
      }
      if( nChildUsed ){
        rc = fsl_cx_exec_multi(f,
                               "DELETE FROM %!Q.orphan "
                               "WHERE baseline=%" FSL_ID_T_PFMT "/*%s()*/",
                               zRepoDbName, rid, __func__);
        break;
      }
      nChildUsed = 0;
    }

    /* Recursively dephantomize all artifacts that are derived by
    ** delta from artifact rid and which have not already been
    ** cross-linked.  */
    rc = fsl_cx_prepare(f, &q,
                        "SELECT rid FROM %!Q.delta "
                        "WHERE srcid=%" FSL_ID_T_PFMT " "
                        "AND NOT EXISTS("
                          "SELECT 1 FROM %!Q.mlink "
                          "WHERE mid=%!Q.delta.rid"
                        ") /*%s()*/",
                        zRepoDbName, rid, zRepoDbName, zRepoDbName,
                        __func__);
    if(rc) break;
    while( FSL_RC_STEP_ROW==fsl_stmt_step(&q) ){
      fsl_id_t const child = fsl_stmt_g_id(&q, 0);
      if(nChildUsed>=nChildAlloc){
        nChildAlloc = nChildAlloc*2 + 10;
        rc = fsl_buffer_reserve(&bufChild, sizeof(fsl_id_t)*nChildAlloc);
        if(rc) goto end;
        aChild = (fsl_id_t*)bufChild.mem;
      }
      aChild[nChildUsed++] = child;
    }
    fsl_stmt_finalize(&q);
    for(i=1; 0==rc && i<nChildUsed; ++i){
      rc = fsl__after_dephantomize(f, aChild[i], true);
    }
    if(rc) break;
    rid = nChildUsed>0 ? aChild[0] : 0;
  }
  end:
  fsl_stmt_finalize(&q);
  fsl_buffer_clear(&bufChild);
  return rc;
}

int fsl__content_put_ex( fsl_cx * const f,
                         fsl_buffer const * pBlob,
                         fsl_uuid_cstr zUuid,
                         fsl_id_t srcId,
                         fsl_size_t uncompSize,
                         bool isPrivate,
                         fsl_id_t * outRid){
  fsl_buffer cmpr = fsl_buffer_empty;
  fsl_buffer hash = fsl_buffer_empty;
  bool markAsUnclustered = false;
  bool markAsUnsent = true;
  bool isDephantomize = false;
  bool inTrans = false;
  int const zUuidLen = zUuid ? fsl_is_uuid(zUuid) : 0;
  int rc = 0;
  fsl_size_t size = 0;
  fsl_id_t rid = 0;
  assert(f);
  assert(pBlob);
  assert(srcId==0 || zUuid!=NULL);
  assert(!zUuid || zUuidLen);
  assert( fsl_cx_txn_level(f)>0 );
  assert( f->cache.ptl.level>0 );
  if(!fsl_cx_db_repo(f)) return FSL_RC_NOT_A_REPO;
  static const fsl_size_t MaxSize = 0x70000000
    /* Do we want FSL_BLOB_MAX_SIZE here instead? */;
  if(pBlob->used>=MaxSize || uncompSize>=MaxSize){
    /* fossil(1) uses int for all blob sizes, and therefore has a
       hard-coded limit of 2GB max size per blob. That property of the
       API is well-entrenched, and correcting it properly, including
       all algorithms which access blobs using integer indexes, would
       require a large coding effort with a non-trivial risk of
       lingering, difficult-to-trace bugs.

       Even if we could exceed that limit, SQLite would stop us, as
       it's a limit there, too.

       For compatibility, we limit ourselves to 2GB, but to ensure a
       bit of leeway, we set our limit slightly less than 2GB.
    */
    return fsl_cx_err_set(f, FSL_RC_RANGE,
                          "For compatibility with fossil(1), "
                          "blobs may not exceed %d bytes in size.",
                          (int)MaxSize);
  }
  rc = fsl_cx_txn_begin(f);
  if(rc){
    fsl__bprc(rc);
    goto end;
  }
  inTrans = true;
  if(!zUuid){
    assert(0==uncompSize);
    /* "auxiliary hash" bits from:
       https://fossil-scm.org/fossil/file?ci=c965636958eb58aa&name=src%2Fcontent.c&ln=527-537
    */
    /* First check the auxiliary hash to see if there is already an artifact
    ** that uses the auxiliary hash name */
    /* 2021-04-13: we can now use fsl_repo_blob_lookup() to do this,
       but the following code is known to work, so touching it is a
       low priority. */
    rc = fsl_cx_hash_buffer(f, true, pBlob, &hash);
    if(rc){
      if(FSL_RC_UNSUPPORTED==rc) rc = 0;
      else goto end;
    }
    assert(hash.used==0 || hash.used>=FSL_STRLEN_SHA1);
    rid = hash.used ? fsl_uuid_to_rid(f, fsl_buffer_cstr(&hash)) : 0;
    assert(rid>=0 && "Cannot have malformed/ambiguous UUID at this point.");
    if(!rid){
      /* No existing artifact with the auxiliary hash name.  Therefore, use
      ** the primary hash name. */
      hash.used = 0;
      rc = fsl_cx_hash_buffer(f, false, pBlob, &hash);
      if(rc) goto end;
      assert(hash.used>=FSL_STRLEN_SHA1);
    }else if(rid<0){
      assert( f->error.code );
      rc = f->error.code;
    }
  }else{
    assert( zUuidLen );
    fsl_buffer_external(&hash, zUuid, (fsl_int_t)zUuidLen);
  }
  assert(!rc);

  if( f->cxConfig.hashPolicy==FSL_HPOLICY_AUTO && hash.used>FSL_STRLEN_SHA1 ){
    fsl_cx_err_reset(f);
    fsl_cx_hash_policy_set(f, FSL_HPOLICY_SHA3);
    if((rc = f->error.code)) goto end;
  }

  if(uncompSize){
    /* pBlob is assumed to be compressed. */
    assert(fsl_buffer_is_compressed(pBlob));
    size = uncompSize;
  }else{
    size = pBlob->used;
    if(srcId>0){
      rc = fsl_delta_applied_size(pBlob->mem, pBlob->used, &size);
      if(rc) goto end;
    }
  }

  {
    /* Check to see if the entry already exists and if it does whether
       or not the entry is a phantom. */
    fsl_stmt * const q = fsl__cx_stmt(f, fsl__cx_stmt_e_blobRidSize);
    if( !q ){
      rc = f->error.code;
      assert( rc );
      fsl__bprc(rc);
      goto end;
    }
    rc = fsl_stmt_bind_step(q, "b", &hash);
    switch(rc){
      case FSL_RC_STEP_ROW:
        rc = 0;
        rid = fsl_stmt_g_id(q, 0);
        if( fsl_stmt_g_int64(q, 1)>=0 ){
          /* The entry is not a phantom. There is nothing for us to do
             other than return the RID. */
          /*
            Reminder: the do-nothing-for-empty-phantom behaviour is
            arguable (but historical). There may(?) be(?) a corner
            case there involving an empty file. So far, so good,
            though. After all...  all empty files have the same hash.
          */
          assert(inTrans);
          fsl__cx_stmt_yield(f, q);
          fsl_cx_txn_end_v2(f,true,false);
          if(outRid) *outRid = rid;
          fsl_buffer_clear(&hash);
          return 0;
        }
        break;
      case 0:
        /* No entry with the same UUID currently exists */
        rid = 0;
        markAsUnclustered = true;
        break;
      default:
        fsl_cx_uplift_db_error(f, NULL);
        break;
    }
    fsl__cx_stmt_yield(f, q);
    if( rc ){
      fsl__bprc(rc);
      goto end;
    }
  }/*blob-exists check*/

#if 0
  /* Requires app-level data. We might need a client hook mechanism or
     other metadata here.
  */
  /* Construct a received-from ID if we do not already have one */
  if( f->cache.rcvId <= 0 ){
    /* FIXME: use cached statement. */
    rc = fsl_db_exec(dbR,
       "INSERT INTO %!Q.rcvfrom(uid, mtime, nonce, ipaddr)"
       "VALUES(%d, julianday('now'), %Q, %Q)",
       fsl_db_role_name(FSL_DBROLE_REPO),
       g.userUid, g.zNonce, g.zIpAddr
    );
    f->cache.rcvId = fsl_db_last_insert_id(dbR);
  }
#endif

  if( uncompSize ){
    fsl_buffer_external(&cmpr, pBlob->mem, (fsl_int_t)pBlob->used);
  }else{
    rc = fsl_buffer_compress(pBlob, &cmpr);
    if(rc){
      fsl__bprc(rc);
      goto end;
    }
  }

  bool const srcIdIsZeroOrAvailable =
    (srcId==0 || 0==fsl__bccache_check_available(f, srcId));
  if( rid>0 ){
    /* We are just adding data to a phantom */
    fsl_stmt * const q = fsl__cx_stmt(f, fsl__cx_stmt_e_phantomPopulate);
    if( !q ){
      rc = f->error.code;
      assert( rc );
      fsl__bprc(rc);
      goto end;
    }
    rc = fsl_stmt_bind_step_v2(q, "RIBR", f->cache.rcvId, (int64_t)size,
                               &cmpr, rid);
    if( FSL_RC_STEP_DONE==rc ){
      rc = fsl__phantom_rid_delete(f, rid);
      if( !rc && srcIdIsZeroOrAvailable ){
        isDephantomize = true;
        rc = fsl__content_mark_available(f, rid);
        fsl__bprc(rc);
      }
    }else{
      assert( FSL_RC_STEP_ROW!=rc );
      fsl_cx_uplift_db_error2(f, q->db, rc);
    }
    fsl__cx_stmt_yield(f, q);
    if(rc) goto end;
  }else{
    /* We are creating a new entry */
    fsl_stmt * const q = fsl__cx_stmt(f, fsl__cx_stmt_e_blobInsertFull);
    if( !q ){
      rc = f->error.code;
      assert( rc );
      fsl__bprc(rc);
      goto end;
    }
    rc = fsl_stmt_bind_step_v2(q, "RIbB", f->cache.rcvId, (int64_t)size,
                               &hash, &cmpr);
    if( FSL_RC_STEP_ROW==rc ){
      rc = 0;
      rid = fsl_stmt_g_id(q, 0);
      fsl_stmt_reset(q);
      assert( rid>0 );
      if(!pBlob){
        assert(!"This cannot happen");
        fsl__fatal(FSL_RC_CANNOT_HAPPEN,"This cannot happen.");
        //rc = fsl__phantom_rid_add(f, rid);
        //markAsUnsent = false;
      }
      if( !rc && (f->cache.markPrivate || isPrivate) ){
        rc = fsl__private_rid_add(f, rid);
        markAsUnclustered = false;
        //markAsUnsent = false;
      }
    }else{
      assert( FSL_RC_STEP_DONE!=rc );
      fsl_cx_uplift_db_error2(f, q->db, rc);
    }
    fsl__cx_stmt_yield(f, q);
    if(rc){
      fsl__bprc(rc);
      goto end;
    }
  }

  /* If the srcId is specified, then the data we just added is
     really a delta. Record this fact in the delta table.
  */
  if( srcId ){
    rc = fsl__delta_replace(f, rid, srcId);
    fsl__bprc(rc);
  }
  if( 0==rc
      && !isDephantomize
      && fsl__cx_ptl_has(f, fsl__ptl_e_missing, rid)
      && srcIdIsZeroOrAvailable ){
    /*
      TODO: document what this is for.
      TODO: figure out what that is.
    */
    rc = fsl__content_mark_available(f, rid);
    fsl__bprc(rc);
  }
  if( 0==rc && isDephantomize ){
#if 1
    /* 2025-08-18: This approach (delaying d14n until
       content-validation time) surives much further, but still
       eventually chokes on a not-found blob in one of the major test
       repos. It's presumably a SHUNned blob, but xfer.c doesn't yet
       read the SHUN list, so we cannot yet differentiate between
       shunned and does-not-exist. We need to side-track and add that
       support before continuing here. */
    rc = fsl__cx_ptl_insert(f, fsl__ptl_e_dephantomize, rid);
#else
    rc = fsl__after_dephantomize(f, rid, false);
#endif
    fsl__bprc(rc);
  }
  /* Add the element to the unclustered table if has never been
     previously seen.
  */
  if( 0==rc && markAsUnclustered ){
    rc = fsl__unclustered_rid_add(f, rid);
    fsl__bprc(rc);
  }
#if 0
  /* Why did we ever do this? fossil(1) does not */
  if( 0==rc && markAsUnsent ){
    rc = fsl__unsent_rid_add(f, rid);
    fsl__bprc(rc);
  }
#else
  (void)markAsUnsent;
#endif
  if( 0==rc ){
    /* Trivia: fossil(1) does this _after_ committing
       the current transaction level. */
    rc = fsl__repo_verify_before_commit(f, rid);
    fsl__bprc(rc);
  }
  if( 0==rc ){
    assert( inTrans );
    if( f->cache.ptl.validateRidLevel < 0 ){
      assert( f->cache.ptl.level>0 );
      f->cache.ptl.validateRidLevel = f->cache.ptl.level - 1
        /* This is a bit of a hack which delays RID content validation
           until after the COMMIT below.  That validation requires
           having complete content and we may have just injected delta
           content deriving from a parent we don't yet have, so
           running content validation in response to _this_ COMMIT is
           doomed to fail during sync (when we get lots of deltas). If
           validateRidLevel>=0 then another transaction level has
           already set it.

           This has to be set before txn_end is called below or else
           we'll end up validating the content as a side-effect of
           this txn_end call.
        */;
    }
    rc = fsl_cx_txn_end_v2(f, true, false);
    fsl__bprc(rc);
    inTrans = false;
    if( 0==rc && outRid ) *outRid = rid;
  }

end:
  assert( FSL_RC_STEP_DONE!=rc );
  if(inTrans){
    assert( rc );
    fsl_cx_txn_end_v2(f,false, false);
  }
  fsl_buffer_clear(&hash);
  fsl_buffer_clear(&cmpr);
  return rc;
}

int fsl__content_put( fsl_cx * const f, fsl_buffer const * pBlob,
                      fsl_id_t * newRid){
  return fsl__content_put_ex(f, pBlob, NULL, 0, 0, 0, newRid);
}

bool fsl_is_shunned_uuid(fsl_cx * const f, fsl_uuid_cstr zUuid){
  fsl_stmt * const q = fsl__cx_stmt(f, fsl__cx_stmt_e_uuidIsShunned);
  fsl_size_t nUuid;
  if( !q || zUuid==0 || zUuid[0]==0 ) return false;
  else if(FSL_STRLEN_SHA1==(nUuid=fsl_is_uuid(zUuid))
          && FSL_HPOLICY_SHUN_SHA1==f->cxConfig.hashPolicy){
    return true;
  }
  bool rv = false;
  switch( fsl_stmt_bind_step_v2(q, "s", zUuid, (fsl_int_t)nUuid, false) ){
    case FSL_RC_STEP_DONE: break;
    case FSL_RC_STEP_ROW:
      rv = true;
      break;
    default:
      fsl_cx_uplift_db_error(f, q->db);
      break;
  }
  fsl__cx_stmt_yield(f, q);
  return rv;
}

int fsl__phantom_new( fsl_cx * const f, fsl_uuid_cstr uuid,
                      bool isPrivate, fsl_id_t * const newId ){
  fsl_id_t rid = 0;
  int rc;
  int const uuidLen = uuid ? fsl_is_uuid(uuid) : 0;
  fsl_stmt * const q = fsl__cx_stmt(f, fsl__cx_stmt_e_contentNew);
#if FSL_API_ARMOR
  if(!uuid) return FSL_RC_MISUSE;
#endif
  assert(f);
  assert(uuid);
  assert(uuidLen);
  assert( fsl_cx_txn_level(f)>0 );
  if( !q ) return f->error.code;
  if( !uuidLen ) return FSL_RC_RANGE;
  rc = fsl_cx_txn_begin(f);
  if(rc) return rc;
  if( fsl_is_shunned_uuid(f, uuid) ){
    rc = fsl_cx_err_set(f, FSL_RC_SHUNNED,
                        "UUID is shunned: %s", uuid);
    goto end;
  }
  rc = fsl_stmt_bind_step_v2(q, "s", uuid);
  if( FSL_RC_STEP_ROW==rc ){
    rid = fsl_stmt_g_id(q, 0);
    assert( rid>0 );
    rc = 0;
  }else{
    assert( FSL_RC_STEP_DONE!=rc );
    assert( 0!=rc );
    fsl_cx_uplift_db_error2(f, q->db, rc);
    goto end;
  }

  rc = fsl__phantom_rid_add(f, rid);
  if( 0==rc ){
    rc = (isPrivate || f->cache.markPrivate)
      ? fsl__private_rid_add(f, rid)
      : fsl__unclustered_rid_add(f, rid);
  }

  if(!rc){
    rc = fsl__cx_ptl_insert(f, fsl__ptl_e_missing, rid);
  }

  end:
  fsl__cx_stmt_yield(f, q);
  if( rc ){
    if( !f->error.code ){
      fsl_cx_err_set(f, rc, NULL);
    }
    fsl_cx_txn_end_v2(f, false, false);
  }else{
    rc = fsl_cx_txn_end_v2(f, true, false);
    if( 0==rc && newId ) *newId = rid;
  }
  return rc;
}

int fsl__content_undeltify(fsl_cx * const f, fsl_id_t rid){
  int rc;
  fsl_id_t srcid = 0;
  fsl_buffer x = fsl_buffer_empty;
  fsl_stmt * const q = fsl__cx_stmt(f, fsl__cx_stmt_e_contentUndeltify);
  if(!q) return f->error.code;
  if(rid<=0) return FSL_RC_RANGE;
  rc = fsl_cx_txn_begin(f);
  if(rc) return rc;
  /* Reminder: the original impl does not do this in a
     transaction, _possibly_ because it's only done from places
     where a transaction is active (that's unconfirmed).
     Nested transactions are very cheap, though. */
  rc = fsl_delta_r2s( f, rid, &srcid );
  if(rc || srcid<=0) goto end;
  rc = fsl_content_get(f, rid, &x);
  if( rc || !x.used ) goto end;
  rc = fsl_buffer_compress(&x, &x);
  if( 0==rc ){
    rc = fsl_stmt_bind_step_v2(q, "BIR", &x, (int64_t)x.used, rid);
    if(FSL_RC_STEP_DONE==rc){
      rc = fsl__delta_delete(f, rid);
    }else{
      assert( rc );
      goto dberr;
    }
  }
#if 0
  /*
    fossil does not do this, but that seems like an inconsistency.

    On that topic Richard says:

    "When you undelta an artifact, however, it is then stored as
    plain text.  (Actually, as zlib compressed plain text.)  There
    is no possibility of delta loops or bugs in the delta encoder or
    missing source artifacts.  And so there is much less of a chance
    of losing content.  Hence, I didn't see the need to verify the
    content of artifacts that are undelta-ed."

    Potential TODO: f->flags FSL_CX_F_PEDANTIC_VERIFICATION, which
    enables the R-card and this check, and any similarly
    possibly-superfluous ones.
  */
  if(!rc) fsl__repo_verify_before_commit(f, rid);
#endif
end:
  fsl__cx_stmt_yield(f, q);
  fsl_buffer_clear(&x);
  if(rc) fsl_cx_txn_end(f, true);
  else rc = fsl_cx_txn_end(f, false);
  return rc;
dberr:
  assert(rc);
  rc = fsl_cx_uplift_db_error2(f, NULL, rc);
  goto end;
}

int fsl__content_deltify(fsl_cx * f, fsl_id_t rid,
                        fsl_id_t srcid, bool force){
  fsl_id_t s;
  fsl_buffer data = fsl_buffer_empty;
  fsl_buffer src = fsl_buffer_empty;
  fsl_buffer * delta = 0;
  int rc = 0;
  enum { MinSizeThreshold = 50 };
#if FSL_API_ARMOR
  if(rid<=0 || srcid<=0) return FSL_RC_RANGE;
  else if(!fsl_needs_repo(f)) return FSL_RC_NOT_A_REPO;
  else if( srcid==rid ) return 0;
#else
  assert( rid>0 );
  assert( srcid>0 );
  assert( srcid!=rid );
  assert( fsl_needs_repo(f) );
#endif
  if(!fsl__content_is_available(f, rid)){
    return 0;
  }
  //TODO (2025-08-06): assert( fsl_cx_txn_level(f)>0 );
  if(!force){
    fsl_id_t tmpRid = 0;
    rc = fsl_delta_r2s(f, rid, &tmpRid);
    if( tmpRid>0 || 0!=rc ){
      /* if 0==rc then we already have a delta, it seems. Nothing left
         to do :-D. Should we return FSL_RC_ALREADY_EXISTS here? (Much
         later: no, as that would only complicate where this is used.)
      */
      return rc;
    }
  }

  if( fsl_content_is_private(f, srcid)
      && !fsl_content_is_private(f, rid) ){
    /*
      See API doc comments about crossing the private/public
      boundaries. Do we want to report okay here or FSL_RC_ACCESS?
      Since delitifying is an internal optimization/implementation
      detail, it seems best to return 0 for this case.
    */
    return 0;
  }
  /* Compute all ancestors of srcid and make sure rid is not one of them.
  ** If rid is an ancestor of srcid, then making rid a decendent of srcid
  ** would create a delta loop. */
  s = srcid;
  while( (0==(rc=fsl_delta_r2s(f, s, &s)))
         && (s>0) ){
    if( s==rid ){
      rc = fsl__content_undeltify(f, srcid);
      break;
    }
  }
  if( rc || 0!=s ){
    return rc;
  }

  /******************************************************************
   ** As of here, don't return on error. Use (goto end) instead
   ** because buffers might need cleaning.
   ******************************************************************/
  rc = fsl_content_get(f, srcid, &src);
  if(rc || (src.used < MinSizeThreshold)
     /* See API doc comments about minimum size to delta/undelta. */
  ){
    goto end;
  }

  rc = fsl_content_get(f, rid, &data);
  if(rc || (data.used < MinSizeThreshold)) goto end;
  delta = fsl_buffer_reuse(&f->cache.deltaContent)
    /* reminder: this buffer is used by fsl_content_get() and this
       routine may recurse into fsl_content_get() before this line. */;
  rc = fsl_buffer_delta_create(&src, &data, delta);
  if( !rc && (delta->used <= (data.used * 3 / 4 /* 75% */))){
    fsl_stmt * const q = fsl__cx_stmt(f, fsl__cx_stmt_e_blobUpdateContent);
    if( q ){
      rc = fsl_buffer_compress(delta, &data);
      if( !rc ){
        rc = fsl_stmt_bind_step_v2(q, "BR", &data, rid);
        if( FSL_RC_STEP_DONE==rc ){
          rc = fsl__delta_replace(f, rid, srcid);
        }else{
          fsl_cx_uplift_db_error2(f, q->db, rc);
        }
      }
      if(!rc) fsl__repo_verify_before_commit(f, rid);
      fsl__cx_stmt_yield(f, q);
    }else{
      rc = f->error.code;
    }
  }

end:
  fsl_buffer_clear(&src);
  fsl_buffer_clear(&data);
  if( delta ) fsl_buffer_reuse(delta);
  return rc;
}

/**
   Removes all entries from the repo's blob table which are listed in
   the shun table.
*/
int fsl__repo_shun_artifacts(fsl_cx * const f){
  fsl_stmt q = fsl_stmt_empty;
  int rc;
  fsl_db * db = f ? fsl_cx_db_repo(f) : NULL;
  if(!f) return FSL_RC_MISUSE;
  else if(!db) return FSL_RC_NOT_A_REPO;
  rc = fsl_db_txn_begin(db);
  if(rc) return rc;
  rc = fsl_db_exec_multi(db,
                         "CREATE TEMP TABLE IF NOT EXISTS "
                         "toshun(rid INTEGER PRIMARY KEY); "
                         "DELETE FROM toshun; "
                         "INSERT INTO toshun SELECT rid FROM blob, shun "
                         "WHERE blob.uuid=shun.uuid;"
  );
  if(rc) goto end;
  /* Ensure that deltas generated from the to-be-shunned data
     are unpacked into non-delta form...
  */
  rc = fsl_db_prepare(db, &q,
                      "SELECT rid FROM delta WHERE srcid IN toshun"
                      );
  if(rc) goto end;
  while( !rc && (FSL_RC_STEP_ROW==fsl_stmt_step(&q)) ){
    fsl_id_t const srcid = fsl_stmt_g_id(&q, 0);
    rc = fsl__content_undeltify(f, srcid);
  }
  fsl_stmt_finalize(&q);
  if(!rc){
    rc = fsl_db_exec_multi(db,
            "DELETE FROM delta WHERE rid IN toshun;"
            "DELETE FROM blob WHERE rid IN toshun;"
            "DELETE FROM toshun;"
            "DELETE FROM private "
            "WHERE NOT EXISTS "
            "(SELECT 1 FROM blob WHERE rid=private.rid);"
    );
  }
  end:
  if(!rc) rc = fsl_db_txn_commit(db);
  else fsl_db_txn_rollback(db);
  if(rc && db->error.code && !f->error.code){
    rc = fsl_cx_uplift_db_error(f, db);
  }
  return rc;
}

int fsl_content_make_public(fsl_cx * const f, fsl_id_t rid){
#if FSL_API_ARMOR
  if(!f) return FSL_RC_MISUSE;
  else if(!fsl_cx_db_repo(f)) return FSL_RC_NOT_A_REPO;
#else
  assert( f );
  assert( fsl_cx_db_repo(f) );
#endif
  return fsl__private_rid_delete(f, rid);
}

/**
    Load the record ID rid and up to N-1 closest ancestors into
    the "fsl_computed_ancestors" table.
*/
static int fsl__compute_ancestors( fsl_db * const db, fsl_id_t rid,
                                   int N, bool directOnly ){
  fsl_stmt st = fsl_stmt_empty;
  int rc = fsl_db_prepare(db, &st,
    "WITH RECURSIVE "
    "  ancestor(rid, mtime) AS ("
    "    SELECT ?1, mtime "
    "      FROM event WHERE objid=?2 "
    "    UNION "
    "    SELECT plink.pid, event.mtime"
    "      FROM ancestor, plink, event"
    "     WHERE plink.cid=ancestor.rid"
    "       AND event.objid=plink.pid %s"
    "     ORDER BY mtime DESC LIMIT ?3"
    "  )"
    "INSERT INTO fsl_computed_ancestors"
    "  SELECT rid FROM ancestor;",
    directOnly ? "AND plink.isPrim" : ""
  );
  if(!rc){
    rc = fsl_stmt_bind_step(&st, "RRi", rid, rid, (int32_t)N);
  }
  fsl_stmt_finalize(&st);
  return rc;
}

int fsl_mtime_of_F_card(fsl_cx * const f, fsl_id_t vid,
                        fsl_card_F const * const fc,
                        fsl_time_t * const pMTime){
  if(!f || !fc) return FSL_RC_MISUSE;
  else if(vid<=0) return FSL_RC_RANGE;
  else if(!fc->uuid){
    if(pMTime) *pMTime = 0;
    return 0;
  }else{
    fsl_id_t const fid = fsl_uuid_to_rid(f, fc->uuid);
    if(fid<=0){
      assert(f->error.code);
      return f->error.code;
    }else{
      return fsl_mtime_of_manifest_file(f, vid, fid, pMTime);
    }
  }
}

int fsl_mtime_of_manifest_file(fsl_cx * const f, fsl_id_t vid, fsl_id_t fid,
                               fsl_time_t * const pMTime){
  fsl_db * const db = fsl_needs_repo(f);
  fsl_stmt * q = NULL;
  int rc = 0;
  if(!db) return FSL_RC_NOT_A_REPO;

  if(fid<=0){
    /* Only fetch the manifest's time... */
    int64_t i = -1;
    rc = fsl_db_get_int64(db, &i,
                          "SELECT fsl_j2u(mtime) "
                          "FROM event WHERE objid=%" FSL_ID_T_PFMT,
                          (fsl_id_t)vid);
    if(!rc){
      if(i<0) rc = FSL_RC_NOT_FOUND;
      else if(pMTime) *pMTime = (fsl_time_t)i;
    }
    return rc;
  }

  if( f->cache.mtimeManifest != vid ){
    /*
      Computing (and keeping) ancestors is relatively costly, so we
      keep only the copy associated with f->cache.mtimeManifest
      around. For the general case, we will be feeding this function
      files from the same manifest.
    */
    f->cache.mtimeManifest = vid;
    rc = fsl_db_exec_multi(db, "CREATE TEMP TABLE IF NOT EXISTS "
                           "fsl_computed_ancestors"
                           "(x INTEGER PRIMARY KEY); "
                           "DELETE FROM fsl_computed_ancestors;");
    if(!rc){
      rc = fsl__compute_ancestors(db, vid, 1000000, 1);
    }
    if(rc){
      fsl_cx_uplift_db_error(f, db);
      return rc;
    }
  }
  rc = fsl_db_prepare_cached(db, &q,
    "SELECT fsl_j2u(max(event.mtime)) FROM mlink, event"
    " WHERE mlink.mid=event.objid"
    "   AND mlink.fid=?"
    "   AND +mlink.mid IN fsl_computed_ancestors"
  );
  if(!rc){
    fsl_stmt_bind_id(q, 1, fid);
    rc = fsl_stmt_step(q);
    if( FSL_RC_STEP_ROW==rc ){
      rc = 0;
      if(pMTime) *pMTime = (fsl_time_t)fsl_stmt_g_int64(q, 0);
    }else{
      assert(rc);
      if(FSL_RC_STEP_DONE==rc) rc = FSL_RC_NOT_FOUND;
    }
    fsl_stmt_cached_yield(q);
    /* Reminder: DO NOT clean up fsl_computed ancestors here. Doing so
       is not only costly later on but also breaks test code. */
  }
  return rc;
}

int fsl_card_F_content( fsl_cx * f, fsl_card_F const * fc,
                        fsl_buffer * const dest ){
  if(!f || !fc || !dest) return FSL_RC_MISUSE;
  else if(!fc->uuid){
    return fsl_cx_err_set(f, FSL_RC_RANGE,
                          "Cannot fetch content of a deleted file "
                          "because it has no UUID.");
  }
  else if(!fsl_needs_repo(f)) return FSL_RC_NOT_A_REPO;
  else{
    fsl_id_t const rid = fsl_uuid_to_rid(f, fc->uuid);
    if(!rid) return fsl_cx_err_set(f, FSL_RC_NOT_FOUND,
                                   "UUID not found: %s",
                                   fc->uuid);
    else if(rid<0){
      assert(f->error.code);
      return f->error.code;
    }else{
      return fsl_content_get(f, rid, dest);
    }
  }
}

fsl_hash_types_e fsl_validate_hash(const char *zHash, fsl_size_t nHash){
  /* fossil(1) counterpart: hname_validate() */
  fsl_hash_types_e rc;
  switch(nHash){
    case FSL_STRLEN_SHA1: rc = FSL_HTYPE_SHA1; break;
    case FSL_STRLEN_K256: rc = FSL_HTYPE_K256; break;
    default: return FSL_HTYPE_ERROR;
  }
  return fsl_validate16(zHash, nHash) ? rc : FSL_HTYPE_ERROR;
}

const char * fsl_hash_type_name(fsl_hash_types_e h, const char *zUnknown){
  /* fossil(1) counterpart: hname_alg() */
  switch(h){
    case FSL_HTYPE_SHA1: return "SHA1";
    case FSL_HTYPE_K256: return "SHA3-256";
    default: return zUnknown;
  }
}

fsl_hash_types_e fsl_verify_blob_hash(fsl_buffer const * pIn,
                                      const char *zHash, int nHash){
  fsl_hash_types_e id = FSL_HTYPE_ERROR;
  switch(nHash){
    case FSL_STRLEN_SHA1:{
      fsl_sha1_cx cx;
      char hex[FSL_STRLEN_SHA1+1] = {0};
      fsl_sha1_init(&cx);
      fsl_sha1_update(&cx, pIn->mem, (unsigned)pIn->used);
      fsl_sha1_final_hex(&cx, hex);
      if(0==memcmp(hex, zHash, FSL_STRLEN_SHA1)){
        id = FSL_HTYPE_SHA1;
      }
      break;
    }
    case FSL_STRLEN_K256:{
      fsl_sha3_cx cx;
      unsigned char const * hex;
      fsl_sha3_init(&cx);
      fsl_sha3_update(&cx, pIn->mem, (unsigned)pIn->used);
      hex = fsl_sha3_end(&cx);
      if(0==memcmp(hex, zHash, FSL_STRLEN_K256)){
        id = FSL_HTYPE_K256;
      }
      break;
    }
    default:
      break;
  }
  return id;
}


int fsl__shunned_remove(fsl_cx * const f){
  fsl_stmt q = fsl_stmt_empty;
  int rc;
  assert(fsl_cx_db_repo(f));
  rc = fsl_cx_exec_multi(f,
     "CREATE TEMP TABLE toshun(rid INTEGER PRIMARY KEY);"
     "INSERT INTO toshun SELECT rid FROM blob, shun WHERE blob.uuid=shun.uuid;"
  );
  if(rc) goto end;
  rc = fsl_cx_prepare(f, &q,
     "SELECT rid FROM delta WHERE srcid IN toshun"
  );
  while( 0==rc && FSL_RC_STEP_ROW==fsl_stmt_step(&q) ){
    rc = fsl__content_undeltify(f, fsl_stmt_g_id(&q, 0));
  }
  fsl_stmt_finalize(&q);
  if(rc) goto end;
  rc = fsl_cx_exec_multi(f,
     "DELETE FROM delta WHERE rid IN toshun;"
     "DELETE FROM blob WHERE rid IN toshun;"
     "DROP TABLE toshun;"
     "DELETE FROM private "
     " WHERE NOT EXISTS (SELECT 1 FROM blob WHERE rid=private.rid);"
  );
  end:
  fsl_stmt_finalize(&q);
  return rc;
}

#undef MARKER
#undef fsl__bprc
