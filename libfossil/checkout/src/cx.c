/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/
/*************************************************************************
  This file houses most of the context-related APIs.
*/
#include "fossil-scm/internal.h"
#include "fossil-scm/checkout.h"
#include "fossil-scm/confdb.h"
#include "fossil-scm/hash.h"
#include "sqlite3.h"
#include <assert.h>

#if defined(_WIN32)
# include <windows.h>
# define F_OK 0
# define W_OK 2
#else
# include <unistd.h> /* F_OK */
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h> /* FILE class */
#include <errno.h>

/* Only for debugging */
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

/** Number of fsl_cx::scratchpads buffers. */
#define FSL_CX_NSCRATCH \
  ((int)(sizeof(fsl__cx_empty.scratchpads.buf) \
         /sizeof(fsl__cx_empty.scratchpads.buf[0])))
const int StaticAssert_scratchpadsCounts[
     (FSL_CX_NSCRATCH==
      ((int)(sizeof(fsl__cx_empty.scratchpads.used)
             /sizeof(fsl__cx_empty.scratchpads.used[0]))))
     ? 1 : -1
];

const fsl_cx fsl__cx_empty = {
  .dbMain = NULL,
  .allocStamp = NULL,
  .cxConfig = fsl_cx_config_empty_m ,
  .db = {
    .ckout = {
      .db = fsl_db_empty_m ,
      .dir = NULL,  .dirLen = 0,
      .rid = -1, .uuid = NULL, .mtime = 0
    },
    .repo = {
      .db = fsl_db_empty_m,
      .user = 0, .mainBranch = 0
    },
    .gconfig = { .db = fsl_db_empty_m },
    .peakTxnLevel = 0,
    .nBegin = 0,
    .nCommit = 0,
    .nRollback = 0
  },
  .ckin = {
    .selectedIds = fsl_id_bag_empty_m,
    .mf = fsl_deck_empty_m
  },
  .confirmer = fsl_confirmer_empty_m,
  .output = fsl_outputer_FILE_m,
  .clientState = fsl_state_empty_m,
  .error = fsl_error_empty_m,
  .scratchpads = {
    .buf = {
      fsl_buffer_empty_m,fsl_buffer_empty_m,
      fsl_buffer_empty_m,fsl_buffer_empty_m,
      fsl_buffer_empty_m,fsl_buffer_empty_m,
      fsl_buffer_empty_m,fsl_buffer_empty_m
    },
    .used = {
      false,false,false,false,
      false,false,false,false
    },
    .next = 0,
    .emit = fsl_buffer_empty_m,
    .format = fsl_buffer_empty_m
  },
  .flags = FSL_CX_F_DEFAULTS,
  .interrupted = 0,
  .xlinkers = fsl_xlinker_list_empty_m,
  .cache = {
    .ignoreDephantomizations = false,
    .markPrivate = false,
    .isCrosslinking = false,
    .xlinkClustersOnly = false,
    .inFinalVerify = false,
    .isSyncing = false,
    .caseInsensitive = -1,
    .allowSymlinks = -1,
    .seenDeltaManifest = -1,
    .searchIndexExists = -1,
    .manifestSetting = -1,
    .rcvId = 0,
    .fileContent = fsl_buffer_empty_m ,
    .deltaContent = fsl_buffer_empty_m ,
    .blobContent = fsl__bccache_empty_m,
    .ptl = fsl__ptl_empty_m,
    .markAvailableCache = fsl_id_bag_empty_m,
    .mtimeManifest = 0,
    .projectCode = NULL,
    .fstat = fsl_fstat_empty_m,
    .mcache = fsl__mcache_empty_m,
    .globs = {
      .ignore = fsl_list_empty_m,
      .binary = fsl_list_empty_m,
      .crnl = fsl_list_empty_m
    },
    .stmt = {
#define STMT(X,IGNORED) .X = fsl_stmt_empty_m,
      fsl__cx_cache_stmt_map(STMT)
#undef STMT
    },
    .tempDirs = NULL
  }/*cache*/,
  .ticket = {
    .customFields = fsl_list_empty_m,
    .hasTicket = 0,
    .hasCTime = 0,
    .hasChng = 0,
    .hasChngRid = 0,
    .titleColumn = NULL,
    .statusColumn = NULL
  },
  .metrics = {
    .content = {
      .nCached = 0, .nTotalUsed = 0, .nPeakBufSize = 0,
      .nCappedMaxSize = 0
    }
  }
}/*fsl__cx_empty*/;

int fsl__db_event_f_cx(fsl_db_event const *ev){
  fsl_cx * const f = ev->state;
  int rc = 0;
  fsl_size_t nLevel = (fsl_size_t)ev->savepointLevel;
  assert(f);

  //MARKER(("db transaction mode=%d rc=%d: %s\n", (int)mode, theRc, db->filename));
  switch( ev->type ){
    case FSL_DB_EVENT_TRACE_SQL:
    case FSL_DB_EVENT_TRACE_SQLX:{
      fsl_buffer const * const b = ev->payload;
      assert( b );
      fsl_output(f, b->mem, b->used);
      break;
    }
    case FSL_DB_EVENT_CLOSING:
      fsl__cx_caches_reset(f, false);
      //MARKER(("closing db: %s\n", ev->db->filename));
      if( ev->db == f->dbMain ){
        f->dbMain = NULL;
      }
      break;
    case FSL_DB_EVENT_BEGIN:
      assert(ev->savepointLevel>0);
      ++f->db.nBegin;
      rc = fsl_cx_emit(f, FSL_MSG_TXN_BEGIN, &nLevel);
      if( 0==rc ){
        rc = fsl__cx_ptl_push(f);
      }
      if( 0==rc && ev->savepointLevel>f->db.peakTxnLevel ){
        f->db.peakTxnLevel = ev->savepointLevel;
      }
      break;
    case FSL_DB_EVENT_ROLLING_BACK:
      break;
    case FSL_DB_EVENT_COMMIT:{
      assert(ev->savepointLevel>0);
      ++f->db.nCommit;
      if( 0==rc && f->cache.ptl.validateRidLevel == ev->savepointLevel ){
        f->cache.ptl.validateRidLevel = -1;
        rc = fsl__repo_verify_at_commit(f)
          /* Funny thing... if we unconditionally validate the current
             level's RIDs, we end up running validation in response to
             every inner transaction of fsl__content_put_ex(), which
             is precisely what we don't want to do (it can't work once
             delta content is involved). That's literally the ONE
             routine which is permitted to add non-phantom blob
             content to the repo (and only one other is allowed to add
             phantoms), but content validation has to be delayed until
             all other content (presumably from the same parent
             transaction(s)) is added so that we can resolve deltas
             during the validation.
          */;

        /* TODO (2025-08-14): here, i think, is where we need to
           handle post-dephantomization. Currently that's handled in
           fsl__content_put_ex(), just like fossil does, but that is
           not working out for me, so the current plan is to queue up
           dephantomization just like we do the verify-at-commit
           checks, delaying them until we "know" that we have all of
           the inbound content from a given transaction. It's not
           clear whether that would resolve all phantom-related
           problems we have at content-validation time.
        */
        if( 0==rc ) rc = fsl__leaf_do_pending_checks(f);
      }
      int const rc2 = fsl__cx_ptl_pop(f, 0==rc);
      if( rc2 && !rc ) rc = rc2;
      if( rc ){
        /* Returning non-0 will transform this pending commit
           to a rollback but the rollback trigger won't be
           called in that case. */
        fsl_cx_emit(f, FSL_MSG_TXN_ROLLBACK, &nLevel);
      }
      else{
        rc = fsl_cx_emit(f, FSL_MSG_TXN_COMMIT, &nLevel);
      }
      break;
    }
    case FSL_DB_EVENT_ROLLED_BACK:
      assert(ev->savepointLevel>0);
      ++f->db.nRollback;
      fsl__repo_verify_cancel(f);
      rc = fsl__cx_ptl_pop(f, false);
      if( f->cache.ptl.validateRidLevel == ev->savepointLevel ){
        f->cache.ptl.validateRidLevel = -1;
      }
      fsl_cx_emit(f, FSL_MSG_TXN_ROLLBACK, &nLevel)
        /* We ignore this particular result code because it could
           hypothetically make it look like a rollback failed,
           when in fact the rollback has already happened by this
           point. */;
      break;
  }
  if( rc && f->error.code ){
    fsl_error_copy( &f->error, &ev->db->error );
  }
  return rc;
}

/**
   Clears (most) dynamic state in f, but does not free f and does
   not free "static" state (that set up by the init process). If
   closeDatabases is true then any databases managed by f are
   closed, else they are kept open.

   Client code will not normally need this - it is intended for a
   particular potential memory optimization case. If (and only if)
   closeDatabases is true then after calling this, f may be legally
   re-used as a target for fsl_cx_init().

   This function does not trigger any finializers set for f's client
   state or output channel.

   Results are undefined if !f or f's memory has not been properly
   initialized.
*/
static void fsl__cx_reset( fsl_cx * const f, bool closeDatabases );

int fsl_cx_init( fsl_cx ** tgt, fsl_cx_config const * cfg ){
  fsl_cx_config paramDefaults = fsl_cx_config_empty_m;
  int rc = 0;
  fsl_cx * f;
  extern int fsl__cx_install_timeline_crosslinkers(fsl_cx * const f)
    /*in deck.c*/;
#if FSL_API_ARMOR
  if(!tgt) return FSL_RC_MISUSE;
#else
  assert( tgt );
#endif
  if(!cfg){
    paramDefaults.output.out = fsl_output_f_FILE;
    paramDefaults.output.state = stdout;
    cfg = &paramDefaults;
  }
  if(*tgt){
    void const * allocStamp = (*tgt)->allocStamp;
    fsl__cx_reset(*tgt, true) /* just to be safe */;
    f = *tgt;
    *f = fsl__cx_empty;
    f->allocStamp = allocStamp;
  }else{
    f = fsl_cx_malloc();
    if(!f) return FSL_RC_OOM;

    *tgt = f;
  }
  memset(&f->cache.mcache, 0, sizeof(f->cache.mcache));
  f->cxConfig = *cfg;

  if( fsl__ptl_init(&f->cache.ptl, 10) ){
    return f->cache.ptl.rc;
  }

  enum {
    /* Because testing shows a lot of re-allocs via some of the
       lower-level stat()-related bits, we pre-allocate this many
       bytes into f->scratchpads.buf[].  Curiously, there is almost no
       difference in (re)allocation behaviour until this size goes
       above about 200.

       We ignore allocation errors here, as they're not critical (but
       upcoming ops will fail when _they_ run out of memory).
    */
    InitialScratchCapacity = 256
  };
  assert(FSL_CX_NSCRATCH
         == (sizeof(f->scratchpads.used)/sizeof(f->scratchpads.used[0])));
  for(int i = 0; i < FSL_CX_NSCRATCH; ++i){
    f->scratchpads.buf[i] = fsl_buffer_empty;
    f->scratchpads.used[i] = false;
    fsl_buffer_reserve(&f->scratchpads.buf[i], InitialScratchCapacity);
  }
  /* We update f->error.msg often, so go ahead and pre-allocate that, too,
     also ignoring any OOM error at this point. */
  fsl_buffer_reserve(&f->error.msg, InitialScratchCapacity);

  if(!rc) rc = fsl__cx_install_timeline_crosslinkers(f);
  if(!rc){
    f->cache.tempDirs = fsl_temp_dirs_get();
    if(!f->cache.tempDirs) rc = FSL_RC_OOM;
  }
  return rc;
}

void fsl__cx_mcache_clear(fsl_cx * const f){
  static const unsigned cacheLen =
    (unsigned)(sizeof(fsl__mcache_empty.aAge)
               /sizeof(fsl__mcache_empty.aAge[0]));
  for(unsigned i = 0; i < cacheLen; ++i){
    fsl_deck_finalize(&f->cache.mcache.decks[i]);
  }
  f->cache.mcache = fsl__mcache_empty;
}

void fsl__cx_content_caches_clear(fsl_cx * const f){
  fsl__bccache_reuse(&f->cache.blobContent);
  fsl__cx_mcache_clear(f);
}

void fsl__cx_reset_for_txn(fsl_cx * const f, bool isCommit){
  if( !isCommit ){
    fsl_free( f->db.repo.mainBranch );
    f->db.repo.mainBranch = 0;
    f->cache.allowSymlinks =
      f->cache.caseInsensitive =
      f->cache.manifestSetting =
      f->cache.searchIndexExists =
      f->cache.seenDeltaManifest = -1;
    if(0) fsl__cx_ckout_clear(f)
      /* We're doing this way too often. Need to limit this to the
         places where we really can invalidate the cached chkout
         state. Plus... this breaks tests. */;
    fsl__cx_mcache_clear(f)
      /* This is unfortunate but we can't yet cherry-pick removing
         content from that cache */;
    /**
       The IDs stored in f->cache.blobContent are ensured not to be
       come stale via fsl__cx_ptl_pop().
    */
  }else{
    fsl__ckout_version_fetch(f)
      /* i don't like doing this here but much code depends on
         f->ckout.rid being up to date. */;
  }
}

void fsl__cx_caches_reset(fsl_cx * const f, bool isCommit){
  fsl__cx_content_caches_clear(f);
  /**
     Operations which might re-initialize f->cache.stmt entries
     should be skipped if repopSome is false.
  */
  fsl__cx_reset_for_txn(f, isCommit);
}

/** Passes each member of f->cache.stmt to fsl_stmt_finalize(). */
static void fsl__cx_finalize_cached_stmt(fsl_cx * const f){
#define STMT(X,IGNORED) fsl_stmt_finalize(&f->cache.stmt.X);
      fsl__cx_cache_stmt_map(STMT)
#undef STMT
}

/**
   Calls fsl_close_scm_dbs() and closes the global config.  If force
   is false and transactions are active on any db then non-0 is
   returned. If force is true, all transaction levels are forcibly
   rolled back before the db is closed.

   This force is false it will fail if any transactions are
   pending. Any databases which are already closed are silently
   skipped. This will fail if any cached statements are currently
   active for the being-closed db(s). "Active" means that
   fsl_db_prepare_cached() was used without a corresponding call to
   fsl_stmt_cached_yield().
*/
static int fsl__cx_close_dbs(fsl_cx * const f, bool force){
  fsl__cx_finalize_cached_stmt(f);
  if( fsl_cx_txn_level(f)
      || fsl_db_txn_level(&f->db.gconfig.db) ){
    if( !force ){
      /* Is this really necessary? Should we instead
         force rollback(s) and close the dbs? */
      return fsl_cx_err_set(f, FSL_RC_MISUSE,
                            "Cannot close the databases when a "
                            "transaction is pending.");
    }
    while( fsl_db_txn_level(&f->db.gconfig.db)!=0 ){
      fsl_db_txn_end_v2(&f->db.gconfig.db, false, true);
    }
    while( fsl_cx_txn_level(f) !=0 ){
      fsl_cx_txn_end_v2(f, false, true);
    }
  }
  fsl_db_close(&f->db.gconfig.db);
  return fsl_close_scm_dbs(f);
}

static void fsl__cx_reset(fsl_cx * const f, bool closeDatabases){
  fsl_checkin_discard(f);
#define SFREE(X) fsl_free(X); X = NULL
  if(closeDatabases){
    fsl__cx_close_dbs(f, true)
      /* Recall that any cached statements can prohibit its
         detachment, so those have to be finalized first. */;
    SFREE(f->db.ckout.dir);
    f->db.ckout.dirLen = 0;
    /* assert(NULL==f->dbMain); */
  }else{
    fsl__cx_finalize_cached_stmt(f);
  }
  SFREE(f->db.repo.user);
  fsl__cx_ckout_clear(f);
  SFREE(f->cache.projectCode);
  SFREE(f->ticket.titleColumn);
  SFREE(f->ticket.statusColumn);
#undef SFREE
  fsl_error_clear(&f->error);
  f->interrupted = 0;
  fsl__card_J_list_free(&f->ticket.customFields, true);
  fsl_buffer_clear(&f->cache.fileContent);
  fsl_buffer_clear(&f->cache.deltaContent);
  fsl_buffer_clear(&f->scratchpads.emit);
  fsl_buffer_clear(&f->scratchpads.format);
  for(int i = 0; i < FSL_CX_NSCRATCH; ++i){
    fsl_buffer_clear(&f->scratchpads.buf[i]);
    f->scratchpads.used[i] = false;
  }
  fsl__cx_caches_reset(f, false);
  fsl__bccache_clear(&f->cache.blobContent);
  fsl__ptl_clear( &f->cache.ptl );
  fsl_id_bag_clear(&f->cache.markAvailableCache);
  if(f->xlinkers.list){
    fsl_free(f->xlinkers.list);
    f->xlinkers = fsl_xlinker_list_empty;
  }
  fsl_list_visit_free(&f->cache.globs.ignore, 1);
  fsl_list_visit_free(&f->cache.globs.binary, 1);
  fsl_list_visit_free(&f->cache.globs.crnl, 1);
  f->cache = fsl__cx_empty.cache;
}

void fsl_state_finalize(fsl_state * const fst){
  if( fst ){
    if( fst->finalize.f ){
      fst->finalize.f( fst->finalize.state, fst->state );
    }
    *fst = fsl_state_empty;
  }
}

void fsl_cx_finalize( fsl_cx * const f ){
  void const * const allocStamp = f ? f->allocStamp : NULL;
  if(!f) return;
  fsl_state_finalize( &f->clientState );
  assert( !f->clientState.finalize.f );
  fsl_temp_dirs_free(f->cache.tempDirs);
  fsl__cx_reset(f, true);
  *f = fsl__cx_empty;
  if(&fsl__cx_empty == allocStamp){
    fsl_free(f);
  }else{
    f->allocStamp = allocStamp;
  }
}

fsl_error * fsl__cx_error( fsl_cx * const f ){
  return &f->error;
}

void fsl_cx_err_reset(fsl_cx * const f){
  //f->interrupted = 0; // No! ONLY modify this via fsl_cx_interrupt()
  fsl_error_reset(&f->error);
  fsl_db_err_reset(&f->db.repo.db);
  fsl_db_err_reset(&f->db.gconfig.db);
  fsl_db_err_reset(&f->db.ckout.db);
}

int fsl_cx_err_set_e( fsl_cx * const f, fsl_error * const err ){
  if(!err){
    return fsl_cx_err_set(f, 0, NULL);
  }else{
    fsl_error_propagate(err, &f->error);
    fsl_error_clear(err);
    return f->error.code;
  }
}

int fsl_cx_err_setv( fsl_cx * const f, int code, char const * fmt,
                     va_list args ){
  return fsl_error_setv( &f->error, code, fmt, args );
}

int fsl_cx_err_set( fsl_cx * const f, int code, char const * fmt,
                    ... ){
  int rc;
  va_list args;
  va_start(args,fmt);
  rc = fsl_error_setv( &f->error, code, fmt, args );
  va_end(args);
  return rc;
}

int fsl_cx_err_get( fsl_cx * const f, char const ** str, fsl_size_t * len ){
#if 1
  return fsl_error_get( &f->error, str, len );
#else
  /* For the docs:
   If fsl_cx_interrupted() has been called with an error code and the
   context has no other pending error state, that code is returned.
  */
  int const rc = fsl_error_get( &f->error, str, len );
  return rc ? rc : f->interrupted;
#endif
}

fsl_id_t fsl_cx_last_insert_id(fsl_cx * const f){
  return (f && f->dbMain && f->dbMain->dbh)
    ? fsl_db_last_insert_id(f->dbMain)
    : -1;
}

fsl_cx * fsl_cx_malloc(void){
  fsl_cx * rc = (fsl_cx *)fsl_malloc(sizeof(fsl_cx));
  if(rc) {
    *rc = fsl__cx_empty;
    rc->allocStamp = &fsl__cx_empty;
  }
  return rc;
}

int fsl_cx_err_report( fsl_cx * const f, bool addNewline ){
#if FSL_API_ARMOR
  if(!f) return FSL_RC_MISUSE;
#else
  assert( f );
#endif
  if(f->error.code){
    char const * msg = f->error.msg.used
      ? (char const *)f->error.msg.mem
      : fsl_rc_cstr(f->error.code)
      ;
    return fsl_outputf(f, "Error #%d: %s%s",
                       f->error.code, msg,
                       addNewline ? "\n" : "");
  }
  else return 0;
}

int fsl_cx_uplift_db_error( fsl_cx * const f, fsl_db * db ){
  assert(f);
  if(!db){
    db = f->dbMain;
    assert(db && "misuse: no DB handle to uplift error from!");
    if(!db) return FSL_RC_MISUSE;
  }
  fsl_error_propagate( &db->error, &f->error );
  return f->error.code;
}

int fsl_cx_uplift_db_error2(fsl_cx * const f, fsl_db * db, int rc){
  assert(f);
  if(!f->error.code && rc && rc!=FSL_RC_OOM){
    if(!db) db = f->dbMain;
    assert(db && "misuse: no DB handle to uplift error from!");
    if(db->error.code) rc = fsl_cx_uplift_db_error(f, db);
  }
  return rc;
}

fsl_db * fsl_cx_db_config( fsl_cx * const f ){
  return f->db.gconfig.db.dbh ? &f->db.gconfig.db : NULL;
}

fsl_db * fsl_cx_db_repo( fsl_cx * const f ){
  if(f->dbMain && (FSL_DBROLE_REPO & f->dbMain->role)) return f->dbMain;
  else if(f->db.repo.db.dbh) return &f->db.repo.db;
  else return NULL;
}

fsl_db * fsl_cx_db_ckout( fsl_cx * const f ){
  if(f->dbMain && (FSL_DBROLE_CKOUT & f->dbMain->role)) return f->dbMain;
  else if(f->db.ckout.db.dbh) return &f->db.ckout.db;
  else return NULL;
}

fsl_db * fsl_needs_repo(fsl_cx * const f){
  fsl_db * const db = fsl_cx_db_repo(f);
  if(!db){
    fsl_cx_err_set(f, FSL_RC_NOT_A_REPO,
                   "Fossil context has no opened repository db.");
  }
  return db;
}

fsl_db * fsl_needs_ckout(fsl_cx * const f){
  fsl_db * const db = fsl_cx_db_ckout(f);
  if(!db){
    fsl_cx_err_set(f, FSL_RC_NOT_A_CKOUT,
                   "Fossil context has no opened checkout db.");
  }
  return db;
}

fsl_db * fsl_cx_db( fsl_cx * const f ){
  return f->dbMain;
}

fsl_db * fsl__cx_db_for_role(fsl_cx * const f, fsl_dbrole_e r){
  switch(r){
    case FSL_DBROLE_CONFIG:
      return &f->db.gconfig.db;
    case FSL_DBROLE_REPO:
      return &f->db.repo.db;
    case FSL_DBROLE_CKOUT:
      return &f->db.ckout.db;
    case FSL_DBROLE_MAIN:
      return f->dbMain;
    case FSL_DBROLE_NONE:
    default:
      return NULL;
  }
}

/**
   Returns the "counterpart" role for the given db role.
*/
static fsl_dbrole_e fsl__dbrole_counterpart(fsl_dbrole_e r){
  switch(r){
    case FSL_DBROLE_REPO: return FSL_DBROLE_CKOUT;
    case FSL_DBROLE_CKOUT: return FSL_DBROLE_REPO;
    default:
      fsl__fatal(FSL_RC_MISUSE,
                 "Serious internal API misuse uncovered by %s().",
                 __func__);
      return FSL_DBROLE_NONE;
  }
}

/**
    Detaches/closes the given db role from f->dbMain and removes the
    role from f->dbMain->role. If r reflects the current primary db
    and a secondary db is attached, the secondary gets detached.  If r
    corresponds to the secondary db, only that db is detached. If
    f->dbMain is the given role then f->dbMain is set to NULL.
*/
static int fsl__cx_detach_role(fsl_cx * const f, fsl_dbrole_e r){
  assert(FSL_DBROLE_CONFIG!=r && "Config db now has its own handle.");
  assert(FSL_DBROLE_REPO==r || FSL_DBROLE_CKOUT==r);
  if(NULL==f->dbMain){
    assert(!"Internal API misuse: don't try to detach when dbMain is NULL.");
    return fsl_cx_err_set(f, FSL_RC_MISUSE,
                          "Cannot close/detach db: none opened.");
  }else if(!(r & f->dbMain->role)){
    assert(!"Misuse: cannot detach unattached role.");
    return fsl_cx_err_set(f, FSL_DBROLE_CKOUT==r
                          ? FSL_RC_NOT_A_CKOUT
                          : FSL_RC_NOT_A_REPO,
                          "Cannot close/detach unattached role: %s",
                          fsl_db_role_name(r));
  }else{
    fsl_db * const db = fsl__cx_db_for_role(f,r);
    int rc = 0;
    switch(r){
      case FSL_DBROLE_REPO:
      case FSL_DBROLE_CKOUT:
        break;
      default:
        fsl__fatal(FSL_RC_ERROR, "Cannot happen. Really. %s:%d",
                   __FILE__, __LINE__);
    }
    fsl__cx_caches_reset(f, false);
    fsl__cx_finalize_cached_stmt(f);
    assert( !f->cache.stmt.ridToUuid.stmt );
    fsl__db_cached_clear_role(f->dbMain, r)
      /* Make sure that we destroy any cached statements which are
         known to be tied to this db role. */;
    if(db->dbh){
      /* This is our MAIN db. CLOSE it. If we still have a
         secondary/counterpart db open, we'll detach it first. */
      fsl_dbrole_e const counterpart = fsl__dbrole_counterpart(r);
      assert(f->dbMain == db);
      if(db->role & counterpart){
        /* When closing the main db, detach the counterpart db first
           (if it's attached). We'll ignore any result code here, for
           sanity's sake. */
        assert(fsl__cx_db_for_role(f,counterpart)->filename &&
               "Inconsistent internal db handle state.");
        fsl__cx_detach_role(f, counterpart);
      }
      fsl_db_close(db);
      f->dbMain = NULL;
    }else{
      /* This is our secondary db. DETACH it. */
      assert(f->dbMain != db);
      rc = fsl_db_detach( f->dbMain, fsl_db_role_name(r) );
      //MARKER(("rc=%s %s %s\n", fsl_rc_cstr(rc), fsl_db_role_name(r),
      //        fsl_buffer_cstr(&f->dbMain->error.msg)));
      if(rc){
        fsl_cx_uplift_db_error(f, f->dbMain);
      }else{
        f->dbMain->role &= ~r;
        fsl__db_clear_strings(db, true);
      }
    }
    return rc;
  }
}

int fsl__cx_attach_role(fsl_cx * const f, const char *zDbName,
                         fsl_dbrole_e r, bool createIfNotExists){
  char const * const dbName =
    fsl_db_role_name(r)/* Reminder: these bytes MUST be static for
                          legal use with SQLITE_DBCONFIG_MAINDBNAME */;
  fsl_db * const db = fsl__cx_db_for_role(f, r);
  int rc;
  assert(db);
  assert(!db->dbh && "Internal API misuse: don't call this when db is connected.");
  assert(!db->filename && "Don't re-attach!");
  assert(!db->name && "Don't re-attach!");
  assert(dbName);
  assert(f->dbMain != db && "Don't re-attach the main db!");
  switch(r){
    case FSL_DBROLE_REPO:
    case FSL_DBROLE_CKOUT:
      break;
    case FSL_DBROLE_CONFIG:
    case FSL_DBROLE_MAIN:
    case FSL_DBROLE_NONE:
    default:
      assert(!"cannot happen/not legal");
      fsl__fatal(FSL_RC_RANGE, "Serious internal API misuse via %s().",
                 __func__);
      return FSL_RC_RANGE;
  }
  db->name = fsl_strdup(dbName);
  if(!db->name){
    rc = FSL_RC_OOM;
    goto end;
  }
  if(!f->dbMain){
    // This is our first/main db. OPEN it.
    rc = fsl_db_open( db, zDbName, createIfNotExists
                      ? FSL_DB_OPEN_RWC
                      : FSL_DB_OPEN_RW );
    if(rc){
      rc = fsl_cx_uplift_db_error2(f, db, rc);
      fsl_db_close(db);
      goto end;
    }
    if( f->cxConfig.see.getSEEKey ){
      fsl_buffer * const seeKey = fsl__cx_scratchpad(f);
      int keyType = FSL_SEE_KEYTYPE_NONE;
      rc = fsl__cx_see_key(f, zDbName, seeKey, &keyType);
      if( rc ){
        fsl__cx_scratchpad_yield(f, seeKey);
        fsl_db_close(db);
        goto end;
      }
      if( seeKey->used && (FSL_SEE_KEYTYPE_NONE!=keyType) ){
        /* Set the db's encryption key. Recall that we don't have to
           do this for subsequent ATTACHed databases because they use
           the same key as the main db by default. */
        switch( keyType ){
          case FSL_SEE_KEYTYPE_TEXTKEY:
          case FSL_SEE_KEYTYPE_PLAIN:
            rc = fsl_db_exec(db, "PRAGMA %s=%Q",
                             (FSL_SEE_KEYTYPE_TEXTKEY==keyType
                              ? "textkey" : "key"),
                             fsl_buffer_cstr(seeKey));
            if( rc ) rc = fsl_cx_uplift_db_error2(f, db, rc);
            break;
          case FSL_SEE_KEYTYPE_HEXKEY:
            rc = fsl_db_exec(db, "PRAGMA hexkey=%Q",
                             fsl_buffer_cstr(seeKey));
            if( rc ) rc = fsl_cx_uplift_db_error2(f, db, rc);
            break;
          case FSL_SEE_KEYTYPE_BINARY:{
            fsl_buffer * const buf = fsl__cx_scratchpad(f);
            rc = fsl_buffer_append_hex(buf, seeKey->mem, seeKey->used, false);
            if( 0==rc ){
              rc = fsl_db_exec(db, "PRAGMA hexkey=%B", buf);
              if( rc ) rc = fsl_cx_uplift_db_error2(f, db, rc);
            }else{
              rc = fsl_cx_err_set(f, rc, NULL/*OOM*/);
            }
            fsl__cx_scratchpad_yield(f, buf);
            break;
          }
          default:
            rc = fsl_cx_err_set(f, FSL_RC_UNSUPPORTED,
                                "Invalid or unsupported value (%d) for SEE key type.",
                                keyType);
            fsl_db_close(db);
            break;
        }
      }
      fsl__cx_scratchpad_yield(f, seeKey);
      if( rc ) goto end;
    }/* SEE setup */
    int const sqrc = sqlite3_db_config(db->dbh,
                                       SQLITE_DBCONFIG_MAINDBNAME,
                                       dbName);
    if(sqrc){
      rc = fsl__db_errcode(&f->db.gconfig.db, sqrc);
      fsl_cx_uplift_db_error2(f, db, rc);
      fsl_db_close(db);
    }else{
      if( 1 ){
        /* Delay setup of tracing until after the SEE init
           specifically to keep those pragmas from being traced. */
        fsl_flag32_t mask = FSL_DB_EVENT_group_txn
          | FSL_DB_EVENT_CLOSING;
        if( f->cxConfig.traceSql ){
          fsl_db_sqltrace_enable(db, NULL, false)
            /*workaround for older tracing API*/;
          assert( !db->impl.event.fpTrace );
          mask |= FSL_DB_EVENT_TRACE_SQLX
            /* TODO: a toggle for FSL_DB_EVENT_TRACE_SQLX
               vs FSL_DB_EVENT_TRACE_SQL. */;
        }
        fsl_db_event_listener(db, fsl__db_event_f_cx, f, mask);
      }
      rc = fsl__cx_init_db(f, db);
      db->role |= r;
      assert(db == f->dbMain)/*even on failure*/;
      /* Should we fsl__cx_detach_role() here? */
    }
  }else{
    // This is our secondary db. ATTACH it.
    assert(db != f->dbMain);
    db->filename = fsl_strdup(zDbName);
    if(!db->filename){
      rc = FSL_RC_OOM;
    }else{
      bool createdIt = false;
      if(createIfNotExists
         && 0!=fsl_file_access( zDbName, F_OK )){
        FILE * const cf = fsl_fopen(zDbName, "w");
        if(!cf){
          rc = fsl_cx_err_set(f, fsl_errno_to_rc(errno, FSL_RC_IO),
                              "Error creating new db file [%s].",
                              zDbName);
          goto end;
        }
        fsl_fclose(cf);
        createdIt = true;
      }
      rc = fsl_db_attach(f->dbMain, zDbName, dbName);
      if(rc){
        fsl_cx_uplift_db_error(f, f->dbMain);
        fsl_db_close(db)/*cleans up strings*/;
        if(createdIt) fsl_file_unlink(zDbName);
      }else{
        /*MARKER(("Attached %p role %d %s %s\n",
          (void const *)db, r, db->name, db->filename));*/
        f->dbMain->role |= r;
      }
    }
  }
  end:
  return rc;
}

int fsl_config_close( fsl_cx * const f ){
  if( fsl_db_txn_level(&f->db.gconfig.db) ){
    return fsl_cx_err_set(f, FSL_RC_MISUSE,
                          "Cannot close config db with an "
                          "opened transaction.");
  }
  fsl_db_close(&f->db.gconfig.db);
  return 0;
}

int fsl_close_scm_dbs(fsl_cx * const f){
  if(fsl_cx_txn_level(f)){
    /* TODO???: force a full rollback and close it */
    //if(f->db.repo.db.dbh) fsl_db_rollback_force(&f->db.repo.db);
    //if(f->db.ckout.db.dbh) fsl_db_rollback_force(&f->db.ckout.db);
    return fsl_cx_err_set(f, FSL_RC_MISUSE,
                          "Cannot close repo or checkout with an "
                          "opened transaction.");
  }
  int rc = 0;
  if(!f->dbMain){
    // Make sure that all string resources are cleaned up...
    fsl__cx_finalize_cached_stmt(f);
    fsl_db_close(&f->db.repo.db);
    fsl_db_close(&f->db.ckout.db);
  }else{
    fsl_db * const dbR = &f->db.repo.db;
    rc = fsl__cx_detach_role(f, f->dbMain == dbR
                             ? FSL_DBROLE_REPO
                             : FSL_DBROLE_CKOUT)
      /* Will also close the counterpart db. */;
  }
  fsl_cx_user_set(f, NULL);
  return rc;
}

int fsl_close_dbs(fsl_cx * f){
  int rc = fsl_close_scm_dbs(f);
  return rc ? rc : fsl_config_close(f);
}

int fsl_repo_close( fsl_cx * const f ){
  return fsl_close_scm_dbs(f);
}

int fsl_ckout_close( fsl_cx * const f ){
  return fsl_close_scm_dbs(f);
}

/**
   If zDbName is a valid checkout database file, open it and return 0.
   If it is not a valid local database file, return a non-0 code.
*/
static int fsl__cx_ckout_open_db(fsl_cx * f, const char *zDbName){
  /* char *zVFileDef; */
  int rc;
  fsl_int_t const lsize = fsl_file_size(zDbName);
  if( -1 == lsize  ){
    return FSL_RC_NOT_FOUND /* might be FSL_RC_ACCESS? */;
  }
  if( lsize%1024!=0 || lsize<4096 ){
    return fsl_cx_err_set(f, FSL_RC_RANGE,
                          "File's size is not correct for a "
                          "checkout db: %s",
                          zDbName);
  }
  rc = fsl__cx_attach_role(f, zDbName, FSL_DBROLE_CKOUT, false);
  return rc;
}


int fsl_cx_execv( fsl_cx * const f, char const * sql, va_list args ){
#if FSL_API_ARMOR
  if(!f->dbMain) return FSL_RC_MISUSE;
  if(!sql) return FSL_RC_MISUSE;
#else
  assert( f->dbMain );
  assert( sql );
#endif
  int const rc = fsl_db_execv(f->dbMain, sql, args);
  return rc ? fsl_cx_uplift_db_error2(f, f->dbMain, rc) : 0;
}

int fsl_cx_exec( fsl_cx * const f, char const * sql, ... ){
  int rc;
  va_list args;
  va_start(args,sql);
  rc = fsl_cx_execv( f, sql, args );
  va_end(args);
  return rc;
}

int fsl_cx_exec_multiv( fsl_cx * const f, char const * sql, va_list args ){
#if FSL_API_ARMOR
  if(!f->dbMain) return FSL_RC_MISUSE;
  if(!sql) return FSL_RC_MISUSE;
#else
  assert( f->dbMain );
  assert( sql );
#endif
  int const rc = fsl_db_exec_multiv(f->dbMain, sql, args);
  return rc ? fsl_cx_uplift_db_error2(f, f->dbMain, rc) : 0;
}

int fsl_cx_exec_multi( fsl_cx * const f, char const * sql, ... ){
  int rc;
  va_list args;
  va_start(args,sql);
  rc = fsl_cx_exec_multiv( f, sql, args );
  va_end(args);
  return rc;
}

int fsl_cx_preparev( fsl_cx * const f, fsl_stmt * const tgt, char const * sql,
                     va_list args ){
#if FSL_API_ARMOR
  if(!f->dbMain) return FSL_RC_MISUSE;
  if(!tgt) return FSL_RC_MISUSE;
#else
  assert( f->dbMain );
  assert( tgt );
#endif
  int const rc = fsl_db_preparev(f->dbMain, tgt, sql, args);
  return rc ? fsl_cx_uplift_db_error2(f, f->dbMain, rc) : 0;
}

int fsl_cx_prepare( fsl_cx * const f, fsl_stmt * const tgt, char const * sql,
                      ... ){
  int rc;
  va_list args;
  va_start(args,sql);
  rc = fsl_cx_preparev( f, tgt, sql, args );
  va_end(args);
  return rc;
}

int fsl_cx_preparev_cached( fsl_cx * const f, fsl_stmt ** tgt, char const * sql,
                            va_list args ){
#if FSL_API_ARMOR
  if(!f->dbMain) return FSL_RC_MISUSE;
  if(!tgt) return FSL_RC_MISUSE;
#else
  assert( f->dbMain );
  assert( tgt );
#endif
  int const rc = fsl_db_preparev_cached(f->dbMain, tgt, sql, args);
  return rc ? fsl_cx_uplift_db_error2(f, f->dbMain, rc) : 0;
}

int fsl_cx_prepare_cached( fsl_cx * const f, fsl_stmt ** tgt, char const * sql,
                           ... ){
  int rc;
  va_list args;
  va_start(args,sql);
  rc = fsl_cx_preparev_cached( f, tgt, sql, args );
  va_end(args);
  return rc;
}


/**
    Passes the fsl_schema_config() SQL code through a new/truncated
    file named dbName. If the file exists before this call, it is
    unlink()ed and fails if that operation fails.
 */
static int fsl_config_file_reset(fsl_cx * const f, char const * dbName){
  fsl_db DB = fsl_db_empty;
  fsl_db * db = &DB;
  int rc = 0;
  bool isAttached = false;
  const char * zPrefix = fsl_db_role_name(FSL_DBROLE_CONFIG);
  if(-1 != fsl_file_size(dbName)){
    rc = fsl_file_unlink(dbName);
    if(rc){
      return fsl_cx_err_set(f, rc,
                            "Error %s while removing old config file (%s)",
                            fsl_rc_cstr(rc), dbName);
    }
  }
  /**
     Hoop-jumping: because the schema file has a cfg. prefix for the
     table(s), and we cannot assign an arbitrary name to an open()'d
     db, we first open the db (making the the "main" db), then
     ATTACH it to itself to provide the fsl_db_role_name() alias.

     2024-09-16: we can use SQLITE_DBCONFIG_MAINDBNAME for this instead.
  */
  rc = fsl_db_open(db, dbName, FSL_DB_OPEN_RWC);
  if(rc) goto end;
  rc = fsl_db_attach(db, dbName, zPrefix);
  if(rc) goto end;
  isAttached = true;
  rc = fsl_db_exec_multi(db, "%s", fsl_schema_config());
  end:
  rc = fsl_cx_uplift_db_error2(f, db, rc);
  if(isAttached) fsl_db_detach(db, zPrefix);
  fsl_db_close(db);
  return rc;
}

int fsl_config_global_preferred_name(char ** zOut){
  char * zEnv = 0 /* from fsl_getenv(). Note the special-case free()
                     semantics!!! */;
  char * zRc = 0 /* `*zOut` result, from fsl_mprintf() */;
  int rc = 0;

#if FSL_PLATFORM_IS_WINDOWS
  zEnv = fsl_getenv("FOSSIL_HOME");
  if( zEnv==0 ){
    zEnv = fsl_getenv("LOCALAPPDATA");
    if( zEnv==0 ){
      zEnv = fsl_getenv("APPDATA");
      if( zEnv==0 ){
        zEnv = fsl_getenv("USERPROFILE");
        if( zEnv==0 ){
          char * const zDrive = fsl_getenv("HOMEDRIVE");
          char * const zPath = fsl_getenv("HOMEPATH");
          if( zDrive && zPath ){
            zRc = fsl_mprintf("%s%//_fossil", zDrive, zPath);
          }
          if(zDrive) fsl_os_str_free(zDrive);
          if(zPath) fsl_os_str_free(zPath);
        }
      }
    }
  }
  if(!zRc){
    if(!zEnv) rc = FSL_RC_NOT_FOUND;
    else{
      zRc = fsl_mprintf("%//_fossil", zEnv);
      if(!zRc) rc = FSL_RC_OOM;
    }
  }
#else
  fsl_buffer buf = fsl_buffer_empty;
  /* Option 1: $FOSSIL_HOME/.fossil */
  zEnv = fsl_getenv("FOSSIL_HOME");
  if(zEnv){
    zRc = fsl_mprintf("%s/.fossil", zEnv);
    if(!zRc) rc = FSL_RC_OOM;
    goto end;
  }
  /* Option 2: if $HOME/.fossil exists, use that */
  rc = fsl_find_home_dir(&buf, 0);
  if(rc) goto end;
  rc = fsl_buffer_append(&buf, "/.fossil", 8);
  if(rc) goto end;
  if(fsl_file_size(fsl_buffer_cstr(&buf))>1024*3){
    zRc = fsl_buffer_take(&buf);
    goto end;
  }
  /* Option 3: $XDG_CONFIG_HOME/fossil.db */
  fsl_os_str_free(zEnv);
  zEnv = fsl_getenv("XDG_CONFIG_HOME");
  if(zEnv){
    zRc = fsl_mprintf("%s/fossil.db", zEnv);
    if(!zRc) rc = FSL_RC_OOM;
    goto end;
  }
  /* Option 4: If $HOME/.config is a directory,
     use $HOME/.config/fossil.db */
  buf.used -= 8 /* "/.fossil" */;
  buf.mem[buf.used] = 0;
  rc = fsl_buffer_append(&buf, "/.config", 8);
  if(rc) goto end;
  if(fsl_dir_check(fsl_buffer_cstr(&buf))>0){
    zRc = fsl_mprintf("%b/fossil.db", &buf);
    if(!zRc) rc = FSL_RC_OOM;
    goto end;
  }
  /* Option 5: fall back to $HOME/.fossil */
  buf.used -= 8 /* "/.config" */;
  buf.mem[buf.used] = 0;
  rc = fsl_buffer_append(&buf, "/.fossil", 8);
  if(!rc) zRc = fsl_buffer_take(&buf);
  end:
  fsl_buffer_clear(&buf);
#endif
  if(zEnv) fsl_os_str_free(zEnv);
  if(!rc){
    if(zRc) *zOut = zRc;
    else rc = FSL_RC_OOM;
  }
  return rc;
}

int fsl_config_open( fsl_cx * const f, char const * openDbName ){
  int rc = 0;
  const char * zDbName = 0;
  char * zPrefName = 0;
  if(fsl_cx_db_config(f)){
    if(NULL==openDbName || 0==*openDbName) return 0/*nothing to do*/;
    fsl_config_close(f);
    assert(!f->db.gconfig.db.dbh);
  }
  if(openDbName && *openDbName){
    zDbName = openDbName;
  }else{
    rc = fsl_config_global_preferred_name(&zPrefName);
    if(rc) goto end;
    zDbName = zPrefName;
  }
  {
    fsl_int_t const fsize = fsl_file_size(zDbName);
    if( -1==fsize || (fsize<1024*3) ){
      rc = fsl_config_file_reset(f, zDbName);
      if(rc) goto end;
    }
  }
#if defined(_WIN32) || defined(__CYGWIN__)
  /* TODO: Jan made some changes in this area in fossil(1) in
     January(?) 2014, such that only the config file needs to be
     writable, not the directory. Port that in.
  */
  if( fsl_file_access(zDbName, W_OK) ){
    rc = fsl_cx_err_set(f, FSL_RC_ACCESS,
                        "Configuration database [%s] "
                        "must be writeable.", zDbName);
    goto end;
  }
#endif
  assert(NULL==fsl_cx_db_config(f));
  rc = fsl_db_open(&f->db.gconfig.db, zDbName,
                   FSL_DB_OPEN_RW | (f->cxConfig.traceSql
                                    ? FSL_DB_OPEN_TRACE_SQL
                                    : 0));
  if(0==rc){
    int const sqrc = sqlite3_db_config(f->db.gconfig.db.dbh,
                                       SQLITE_DBCONFIG_MAINDBNAME,
                                       fsl_db_role_name(FSL_DBROLE_CONFIG));
    if(sqrc) rc = fsl__db_errcode(&f->db.gconfig.db, sqrc);
  }
  if(rc){
    rc = fsl_cx_uplift_db_error2(f, &f->db.gconfig.db, rc);
    fsl_db_close(&f->db.gconfig.db);
  }
  end:
  fsl_free(zPrefName);
  return rc;
}

static int fsl_cx_load_glob_lists(fsl_cx * f){
  int rc;
  rc = fsl_config_globs_load(f, &f->cache.globs.ignore, "ignore-glob");
  if(!rc) rc = fsl_config_globs_load(f, &f->cache.globs.binary, "binary-glob");
  if(!rc) rc = fsl_config_globs_load(f, &f->cache.globs.crnl, "crnl-glob");
  return rc;
}

int fsl_cx_glob_list( fsl_cx * const f,
                      fsl_glob_category_e gtype,
                      fsl_list **tgt,
                      bool reload ){
  fsl_list * li = NULL;
  char const * reloadKey = NULL;
  switch(gtype){
    case FSL_GLOBS_IGNORE: li = &f->cache.globs.ignore;
      reloadKey = "ignore-glob"; break;
    case FSL_GLOBS_CRNL: li = &f->cache.globs.crnl;
      reloadKey = "crnl-glob"; break;
    case FSL_GLOBS_BINARY: li = &f->cache.globs.binary;
      reloadKey = "binary-glob"; break;
    default:
      return FSL_RC_RANGE;
  }
  int rc = 0;
  if(reload){
    assert(reloadKey);
    fsl_glob_list_clear(li);
    rc = fsl_config_globs_load(f, li, reloadKey);
  }
  if(0==rc) *tgt = li;
  return rc;
}

fsl_glob_category_e fsl_glob_name_to_category(char const * str){
  if(str){
#define CHECK(PRE,E) \
    if(*str==PRE[0] &&                          \
       (0==fsl_strcmp(PRE "-glob",str)          \
        || 0==fsl_strcmp(PRE,str))) return E;
    CHECK("ignore", FSL_GLOBS_IGNORE);
    CHECK("binary", FSL_GLOBS_BINARY);
    CHECK("crnl", FSL_GLOBS_CRNL);
#undef CHECK
  }
  return FSL_GLOBS_INVALID;
}


/**
   To be called after a repo or checkout/repo combination has been
   opened. This updates some internal cached info based on the
   checkout and/or repo.
*/
static int fsl__cx_after_open(fsl_cx * f){
  f->cache.searchIndexExists = -1;
  int rc = fsl__ckout_version_fetch(f);
  if( !rc ) rc = fsl_cx_load_glob_lists(f);
  if( !rc ) fsl_cx_user_guess(f);
  return rc;
}


static void fsl_cx_fetch_hash_policy(fsl_cx * f){
  int const iPol =
    fsl_config_get_int32( f, FSL_CONFDB_REPO,
                          FSL_HPOLICY_AUTO, "hash-policy");
  fsl_hashpolicy_e p;
  switch(iPol){
    case FSL_HPOLICY_SHA3: p = FSL_HPOLICY_SHA3; break;
    case FSL_HPOLICY_SHA3_ONLY: p = FSL_HPOLICY_SHA3_ONLY; break;
    case FSL_HPOLICY_SHA1: p = FSL_HPOLICY_SHA1; break;
    case FSL_HPOLICY_SHUN_SHA1: p = FSL_HPOLICY_SHUN_SHA1; break;
    default: p = FSL_HPOLICY_AUTO; break;
  }
  f->cxConfig.hashPolicy = p;
}

#if 0
/**
    Return true if the schema is out-of-date. db must be an opened
    repo db.
 */
static bool fsl__db_repo_schema_is_outofdate(fsl_db *db){
  return fsl_db_exists(db, "SELECT 1 FROM config "
                       "WHERE name='aux-schema' "
                       "AND value<>'%s'",
                       FSL_AUX_SCHEMA);
}

/*
   Returns 0 if db appears to have a current repository schema, 1 if
   it appears to have an out of date schema, and -1 if it appears to
   not be a repository.
*/
int fsl__db_repo_verify_schema(fsl_db * const db){
  if(fsl__db_repo_schema_is_outofdate(db)) return 1;
  else return fsl_db_exists(db,
                            "SELECT 1 FROM config "
                            "WHERE name='project-code'")
    ? 0 : -1;
}
int fsl_repo_schema_validate(fsl_cx * const f, fsl_db * const db){
  int rc = 0;
  int const check = fsl__db_repo_verify_schema(db);
  if(0 != check){
    rc = (check<0)
      ? fsl_cx_err_set(f, FSL_RC_NOT_A_REPO,
                      "DB file [%s] does not appear to be "
                      "a repository.", db->filename)
      : fsl_cx_err_set(f, FSL_RC_REPO_NEEDS_REBUILD,
                      "DB file [%s] appears to be a fossil "
                      "repsitory, but is out-of-date and needs "
                      "a rebuild.",
                      db->filename);
  }
  return rc;
}
#endif

int fsl_repo_open( fsl_cx * const f, char const * repoDbFile
                   /* , bool readOnlyCurrentlyIgnored */ ){
  if(fsl_cx_db_repo(f)){
    return fsl_cx_err_set(f, FSL_RC_ACCESS,
                          "Context already has an opened repository.");
  }else{
    int rc;
    if(0!=fsl_file_access( repoDbFile, F_OK )){
      rc = fsl_cx_err_set(f, FSL_RC_NOT_FOUND,
                          "Repository db [%s] not found or cannot be read.",
                          repoDbFile);
    }else{
      rc = fsl__cx_attach_role(f, repoDbFile, FSL_DBROLE_REPO, false);
      if(!rc && !(FSL_CX_F_IS_OPENING_CKOUT & f->flags)){
        rc = fsl__cx_after_open(f);
      }
      if(!rc){
        fsl_db * const db = fsl_cx_db_repo(f);
        /* 2024-09-02: why do we force-set allows_symlinks and
           is_case_sensitive here? */
        fsl_cx_allows_symlinks(f, true);
        fsl_cx_is_case_sensitive(f, true);
        f->cache.seenDeltaManifest =
          fsl_config_get_int32(f, FSL_CONFDB_REPO, -1,
                               "seen-delta-manifest");
        fsl_cx_fetch_hash_policy(f);
        if(f->cxConfig.hashPolicy==FSL_HPOLICY_AUTO){
          if(fsl_db_exists(db, "SELECT 1 FROM blob WHERE length(uuid)>40")
             || !fsl_db_exists(db, "SELECT 1 FROM blob WHERE length(uuid)==40")){
            f->cxConfig.hashPolicy = FSL_HPOLICY_SHA3;
          }
        }
      }
    }
    return rc;
  }
}

/**
    Tries to open the repository from which the current checkout
    derives. Returns 0 on success.
*/
static int fsl__repo_open_for_ckout(fsl_cx * f){
  char * repoDb = NULL;
  int rc;
  fsl_buffer nameBuf = fsl_buffer_empty;
  fsl_db * db = fsl_cx_db_ckout(f);
  assert(f);
  assert(f->db.ckout.dir);
  assert(db);
  rc = fsl_db_get_text(db, &repoDb, NULL,
                       "SELECT value FROM vvar "
                       "WHERE name='repository'");
  if(rc) fsl_cx_uplift_db_error( f, db );
  else if(repoDb){
    if(!fsl_is_absolute_path(repoDb)){
      /* Make it relative to the checkout db dir */
      rc = fsl_buffer_appendf(&nameBuf, "%s/%s", f->db.ckout.dir, repoDb);
      fsl_free(repoDb);
      if(rc) {
        fsl_buffer_clear(&nameBuf);
        return rc;
      }
      repoDb = (char*)nameBuf.mem /* transfer ownership */;
      nameBuf = fsl_buffer_empty;
    }
    rc = fsl_file_canonical_name(repoDb, &nameBuf, 0);
    fsl_free(repoDb);
    if(!rc){
      repoDb = fsl_buffer_str(&nameBuf);
      assert(repoDb);
      rc = fsl_repo_open(f, repoDb);
    }
    fsl_buffer_reserve(&nameBuf, 0);
  }else{
    /* This can only happen if we are not using a proper
       checkout db or someone has removed the repo link.
    */
    rc = fsl_cx_err_set(f, FSL_RC_NOT_FOUND,
                        "Could not determine this checkout's "
                        "repository db file.");
  }
  return rc;
}

static void fsl_ckout_mtime_set(fsl_cx * const f){
  f->db.ckout.mtime = f->db.ckout.rid>0
    ? fsl_db_g_double(fsl_cx_db_repo(f), 0.0,
                      "SELECT mtime FROM event "
                      "WHERE objid=%" FSL_ID_T_PFMT,
                      f->db.ckout.rid)
    : 0.0;
}

void fsl__cx_ckout_clear( fsl_cx * const f ){
  fsl_free(f->db.ckout.uuid);
  f->db.ckout.rid = -1;
  f->db.ckout.uuid = NULL;
  f->db.ckout.mtime = 0.0;
}

int fsl__ckout_version_fetch( fsl_cx * const f ){
  fsl_id_t rid = 0;
  int rc = 0;
  fsl_db * dbC = fsl_cx_db_ckout(f);
  fsl_db * dbR = dbC ? fsl_needs_repo(f) : NULL;
  fsl__cx_ckout_clear(f);
  if(!dbC) return 0;
  else if(!dbR) return FSL_RC_NOT_A_REPO;
  rid = fsl_config_get_id(f, FSL_CONFDB_CKOUT, -1, "checkout");
  //MARKER(("rc=%s rid=%d\n",fsl_rc_cstr(f->error.code), (int)rid));
  if(rid>0){
    assert( !f->db.ckout.uuid );
    f->db.ckout.uuid = fsl_rid_to_uuid(f, rid);
    if(!f->db.ckout.uuid){
      assert(f->error.code);
      if(!f->error.code){
        rc = fsl_cx_err_set(f, FSL_RC_NOT_FOUND,
                            "Could not load UUID for RID %"FSL_ID_T_PFMT,
                            (fsl_id_t)rid);
      }
    }else{
      assert(fsl_is_uuid(f->db.ckout.uuid));
    }
    f->db.ckout.rid = rid;
    fsl_ckout_mtime_set(f);
  }else if(rid==0){
    /* This is a legal case not possible before libfossil (and only
       afterwards possible in fossil(1)) - an empty repo without an
       active checkin. [Much later:] that capability has since been
       removed from fossil.
    */
    f->db.ckout.rid = 0;
  }else{
    rc = fsl_cx_err_set(f, FSL_RC_NOT_FOUND,
                        "Cannot determine checkout version.");
  }
  return rc;
}

/** @internal

    Sets f->db.ckout.rid to the given rid (which must be 0 or a valid
    RID) and f->db.ckout.uuid to a copy of the given uuid. If uuid is
    NULL and rid is not 0 then the uuid is fetched using
    fsl_rid_to_uuid(), else if uuid is not NULL then it is assumed to
    be the UUID for the given RID and is copies to f->db.ckout.uuid.

    Returns 0 on success, FSL_RC_OOM if copying uuid fails, or some
    error from fsl_rid_to_uuid() if that fails.

    Does not write the changes to disk. Use fsl__ckout_version_write()
    for that. That routine also calls this one, so there's no need to
    call both.
*/
static int fsl_cx_ckout_version_set(fsl_cx *f, fsl_id_t rid,
                                    fsl_uuid_cstr uuid){
  char * u = 0;
  assert(rid>=0);
  u = uuid
    ? fsl_strdup(uuid)
    : (rid ? fsl_rid_to_uuid(f, rid) : NULL);
  if(rid && !u){
    fsl_report_oom;
    return FSL_RC_OOM;
  }
  f->db.ckout.rid = rid;
  fsl_free(f->db.ckout.uuid);
  f->db.ckout.uuid = u;
  fsl_ckout_mtime_set(f);
  return 0;
}

int fsl__ckout_version_write( fsl_cx * const f, fsl_id_t vid,
                             fsl_uuid_cstr hash ){
  int rc = 0;
  if(!fsl_needs_ckout(f)) return FSL_RC_NOT_A_CKOUT;
  else if(vid<0){
    return fsl_cx_err_set(f, FSL_RC_MISUSE,
                          "Invalid vid for fsl__ckout_version_write()");
  }
  if(f->db.ckout.rid!=vid){
    rc = fsl_cx_ckout_version_set(f, vid, hash);
  }
  if(!rc){
    rc = fsl_config_set_id(f, FSL_CONFDB_CKOUT,
                           "checkout", f->db.ckout.rid);
    if(!rc){
      rc = fsl_config_set_text(f, FSL_CONFDB_CKOUT,
                               "checkout-hash", f->db.ckout.uuid);
    }
  }
  if(!rc){
    char * zFingerprint = 0;
    rc = fsl__repo_fingerprint_search(f, 0, &zFingerprint);
    if(!rc){
      rc = fsl_config_set_text(f, FSL_CONFDB_CKOUT,
                               "fingerprint", zFingerprint);
      fsl_free(zFingerprint);
    }
  }
  if(!rc){
    int const mode = vid ? -1 : 0;
    rc = fsl_ckout_manifest_write(f, mode, mode, mode, 0);
  }
  return rc;
}

void fsl_ckout_version_info(fsl_cx * const f, fsl_id_t * const rid,
                            fsl_uuid_cstr * const uuid ){
  if(uuid) *uuid = f->db.ckout.uuid;
  if(rid) *rid = f->db.ckout.rid>=0 ? f->db.ckout.rid : 0;
}

int fsl_ckout_db_search( char const * dirName, bool checkParentDirs,
                         fsl_buffer * const pOut ){
  int rc = 0;
  fsl_int_t dLen = 0, i;
  const char aDbName[][10] = { "_FOSSIL_", ".fslckout" };
  fsl_int_t const nDbName = (fsl_int_t)(sizeof(aDbName) / sizeof(aDbName[0]));
  fsl_fstat fst = fsl_fstat_empty;
  fsl_buffer Buf = fsl_buffer_empty;
  fsl_buffer * buf = &Buf;
  if(dirName){
    dLen = fsl_strlen(dirName);
    if(0==dLen) return FSL_RC_RANGE;
    fsl_buffer_reserve( buf, (fsl_size_t)(dLen + 10) );
    fsl_buffer_append( buf, dirName, dLen );
    if(buf->errCode){
      fsl_buffer_clear(buf);
      return buf->errCode;
    }
  }else{
    char zPwd[4000];
    fsl_size_t pwdLen = 0;
    rc = fsl_getcwd( zPwd, sizeof(zPwd)/sizeof(zPwd[0]), &pwdLen );
    if(rc){
      fsl_buffer_clear(buf);
#if 0
      return fsl_cx_err_set(f, rc,
                            "Could not determine current directory. "
                            "Error code %d (%s).",
                            rc, fsl_rc_cstr(rc));
#else
      return rc;
#endif
    }
    if(1 == pwdLen && '/'==*zPwd) *zPwd = '.'
      /* When in the root directory (or chroot) then change dir name
         name to something we can use.
      */;
    if(fsl_buffer_append(buf, zPwd, pwdLen)){
      fsl_buffer_clear(buf);
      return buf->errCode;
    }
    dLen = (fsl_int_t)pwdLen;
  }
  if(rc){
    fsl_buffer_clear(buf);
    return rc;
  }
  assert(buf->capacity>=buf->used);
  assert((buf->used == (fsl_size_t)dLen) || (1==buf->used && (int)'.'==(int)buf->mem[0]));
  assert(0==buf->mem[buf->used]);

  while(dLen>0){
    /*
      Loop over the list in aDbName, appending each one to
      the dir name in the search for something we can use.
    */
    fsl_int_t const lenMarker = dLen /* position to re-set to on each
                                        sub-iteration. */ ;
    /* trim trailing slashes on this part, so that we don't end up
       with multiples between the dir and file in the final output. */
    while( dLen && ((int)'/'==(int)buf->mem[dLen-1])) --dLen;
    for( i = 0; i < nDbName; ++i ){
      char const * zName;
      buf->used = (fsl_size_t)lenMarker;
      dLen = lenMarker;
      rc = fsl_buffer_appendf( buf, "/%s", aDbName[i]);
      if(rc){
        fsl_buffer_clear(buf);
        return rc;
      }
      zName = fsl_buffer_cstr(buf);
      if(0==fsl_stat(zName, &fst, false)
         && (0 == (fst.size % 512 /*else cannot be an sqlite db*/))
         && 0==fsl_file_access(zName, 0)
         /* ^^^ fsl_fstat does not store OS-level permissions, only
            fossil-relevant permissions, so we can't check this in
            fsl.perm. Maybe we should add fsl_fstat::osPerm.  Yes,
            there's a race condition here but it's wildly a
            hypothetical problem, not a real one. */){
        if(pOut){
          if( pOut->mem ){
            rc = fsl_buffer_append( pOut, buf->mem, buf->used );
            fsl_buffer_clear(buf);
          }else{
            fsl_buffer_swap( pOut, buf );
          }
        }
        return rc;
      }
      if(i==nDbName-1 && !checkParentDirs){
        dLen = 0;
        break;
      }else{
        /* Traverse up one dir and try again. */
        --dLen;
        while( dLen>0 && (int)buf->mem[dLen]!=(int)'/' ){ --dLen; }
        while( dLen>0 && (int)buf->mem[dLen-1]==(int)'/' ){ --dLen; }
        if(dLen>lenMarker){
          buf->mem[dLen] = 0;
        }
      }
    }
  }
  fsl_buffer_clear(buf);
  return FSL_RC_NOT_FOUND;
}

int fsl_cx_getcwd(fsl_cx * const f, fsl_buffer * const pOut){
  char cwd[FILENAME_MAX] = {0};
  fsl_size_t cwdLen = 0;
  int rc = fsl_getcwd(cwd, (fsl_size_t)sizeof(cwd), &cwdLen);
  if(rc){
    return fsl_cx_err_set(f, rc,
                          "Could not get current working directory!");
  }
  rc = fsl_buffer_append(pOut, cwd, cwdLen);
  return rc
    ? fsl_cx_err_set(f, rc/*must be an OOM*/, NULL)
    : 0;
}

int fsl_ckout_open_dir( fsl_cx * const f, char const * dirName,
                        bool checkParentDirs ){
  int rc;
  fsl_buffer * const buf = fsl__cx_scratchpad(f);
  fsl_buffer * const bufD = fsl__cx_scratchpad(f);
  char const * zName;
  if(fsl_cx_db_ckout(f)){
    rc = fsl_cx_err_set( f, FSL_RC_ACCESS,
                         "A checkout is already opened. "
                         "Close it before opening another.");
    goto end;
  }else if(!dirName){
    dirName = ".";
  }
  rc = fsl_file_canonical_name( dirName, bufD, false );
  if(rc) goto end;
  dirName = fsl_buffer_cstr(bufD);
  rc = fsl_ckout_db_search(dirName, checkParentDirs, buf);
  if(rc){
    if(FSL_RC_NOT_FOUND==rc){
      rc = fsl_cx_err_set(f, FSL_RC_NOT_FOUND,
                          "Could not find checkout under [%s].",
                          dirName ? dirName : ".");
    }
    goto end;
  }
  assert(buf->used>1 /* "/<FILENAME>" */);
  zName = fsl_buffer_cstr(buf);
  rc = fsl__cx_ckout_open_db(f, zName);
  if(0==rc){
    /* Checkout db is now opened. Fiddle some internal
       bits...
    */
    unsigned char * end = buf->mem+buf->used-1;
    /* Find dir part */
    while(end>buf->mem && (unsigned char)'/'!=*end) --end;
    assert('/' == (char)*end && "fsl_ckout_db_search() appends '/<DBNAME>'");
    fsl_free(f->db.ckout.dir);
    f->db.ckout.dirLen = end - buf->mem +1 /* for trailing '/' */ ;
    *(end+1) = 0; /* Rather than strdup'ing, we'll just lop off the
                     filename part. Keep the '/' for historical
                     conventions purposes - it simplifies path
                     manipulation later on. */
    f->db.ckout.dir = fsl_buffer_take(buf);
    assert(!f->db.ckout.dir[f->db.ckout.dirLen]);
    assert('/' == f->db.ckout.dir[f->db.ckout.dirLen-1]);
    f->flags |= FSL_CX_F_IS_OPENING_CKOUT;
    rc = fsl__repo_open_for_ckout(f);
    f->flags &= ~FSL_CX_F_IS_OPENING_CKOUT;
    if(!rc) rc = fsl__cx_after_open(f);
    if(rc){
      /* Is this sane? Is not doing it sane? */
      fsl_close_scm_dbs(f);
    }
  }
  end:
  fsl__cx_scratchpad_yield(f, buf);
  fsl__cx_scratchpad_yield(f, bufD);
  return rc;
}


char const * fsl_cx_db_file_for_role(fsl_cx const * f,
                                     fsl_dbrole_e r,
                                     fsl_size_t * len){
  fsl_db const * db = fsl__cx_db_for_role((fsl_cx*)f, r);
  char const * rc = db ? db->filename : NULL;
  if(len) *len = fsl_strlen(rc);
  return rc;
}

char const * fsl_cx_db_name_for_role(fsl_cx const * f,
                                     fsl_dbrole_e r,
                                     fsl_size_t * len){
  if(FSL_DBROLE_MAIN == r){
    if(f->dbMain){
      if(len) *len=4;
      return "main";
    }else{
      return NULL;
    }
  }else{
    fsl_db const * db = fsl__cx_db_for_role((fsl_cx*)f, r);
    char const * rc = db ? db->name : NULL;
    if(len) *len = rc ? fsl_strlen(rc) : 0;
    return rc;
  }
}

char const * fsl_cx_db_file_config(fsl_cx const * f,
                                   fsl_size_t * len){
  char const * rc = NULL;
  if(f && f->db.gconfig.db.filename){
    rc = f->db.gconfig.db.filename;
    if(len) *len = fsl_strlen(rc);
  }
  return rc;
}

char const * fsl_cx_db_file_repo(fsl_cx const * f,
                                 fsl_size_t * len){
  char const * rc = NULL;
  if(f && f->db.repo.db.filename){
    rc = f->db.repo.db.filename;
    if(len) *len = fsl_strlen(rc);
  }
  return rc;
}

char const * fsl_cx_db_file_ckout(fsl_cx const * f,
                                     fsl_size_t * len){
  char const * rc = NULL;
  if(f && f->db.ckout.db.filename){
    rc = f->db.ckout.db.filename;
    if(len) *len = fsl_strlen(rc);
  }
  return rc;
}

char const * fsl_cx_ckout_dir_name(fsl_cx const * f,
                                      fsl_size_t * len){
  char const * rc = NULL;
  if(f && f->db.ckout.dir){
    rc = f->db.ckout.dir;
    if(len) *len = f->db.ckout.dirLen;
  }
  return rc;
}

fsl_flag32_t fsl_cx_flags_get( fsl_cx const * const f ){
  return f->flags;
}

fsl_flag32_t fsl_cx_flag_set( fsl_cx * const f, fsl_flag32_t flags,
                              bool enable ){
  fsl_flag32_t const oldFlags = f->flags;
  if(enable) f->flags |= flags;
  else f->flags &= ~flags;
  return oldFlags;
}


fsl_xlinker * fsl_xlinker_by_name( fsl_cx * f, char const * name ){

  fsl_xlinker * rv = NULL;
  fsl_size_t i;
  for( i = 0; i < f->xlinkers.used; ++i ){
    rv = f->xlinkers.list + i;
    if(0==fsl_strcmp(rv->name, name)) return rv;
  }
  return NULL;
}

int fsl_xlink_listener( fsl_cx * const f, char const * name,
                        fsl_deck_xlink_f cb, void * cbState ){
  fsl_xlinker * x;
  if(!*name) return FSL_RC_MISUSE;
  x = fsl_xlinker_by_name(f, name);
  if(x){
    /* Replace existing entry */
    x->f = cb;
    x->state = cbState;
    return 0;
  }else if(f->xlinkers.used <= f->xlinkers.capacity){
    /* Expand the array */
    fsl_size_t const n = f->xlinkers.used ? f->xlinkers.used * 2 : 5;
    fsl_xlinker * re =
      (fsl_xlinker *)fsl_realloc(f->xlinkers.list,
                                 n * sizeof(fsl_xlinker));
    if(!re) return FSL_RC_OOM;
    f->xlinkers.list = re;
    f->xlinkers.capacity = n;
  }
  x = f->xlinkers.list + f->xlinkers.used++;
  *x = fsl_xlinker_empty;
  x->f = cb;
  x->state = cbState;
  x->name = name;
  return 0;
}

int fsl_cx_user_set( fsl_cx * const f, char const * userName ){
  if(!f) return FSL_RC_MISUSE;
  else if(!userName || !*userName){
    fsl_free(f->db.repo.user);
    f->db.repo.user = NULL;
    return 0;
  }else{
    char * u = fsl_strdup(userName);
    if(!u) return FSL_RC_OOM;
    else{
      fsl_free(f->db.repo.user);
      f->db.repo.user = u;
      return 0;
    }
  }
}

char const * fsl_cx_user_guess(fsl_cx * const f){
  if(!f->db.repo.user){
    char * u = fsl_config_get_text(f, FSL_CONFDB_CKOUT, "default-user", NULL);
    if( !u ){
      fsl_db * const dbR = fsl_cx_db_repo(f);
      u = dbR
        ? fsl_db_g_text(dbR, NULL, "SELECT login FROM user WHERE uid=1")
        : NULL;
    }
    if( !u ) u = fsl_user_name_guess();
    if( u ){
      assert( u!=f->db.repo.user );
      fsl_free(f->db.repo.user);
      f->db.repo.user = u;
      // don't use fsl_cx_user_set(f, u), to avoid another strdup()
    }
  }
  return f->db.repo.user;
}

char const * fsl_cx_user_get( fsl_cx const * const f ){
  return f->db.repo.user;
}

int fsl_cx_schema_ticket(fsl_cx * f, fsl_buffer * pOut){
  fsl_db * db = f ? fsl_needs_repo(f) : NULL;
  if(!f || !pOut) return FSL_RC_MISUSE;
  else if(!db) return FSL_RC_NOT_A_REPO;
  else{
    fsl_size_t const oldUsed = pOut->used;
    int rc = fsl_config_get_buffer(f, FSL_CONFDB_REPO,
                                   "ticket-table", pOut);
    if((FSL_RC_NOT_FOUND==rc)
       || (oldUsed == pOut->used/*found but it was empty*/)
       ){
      fsl_cx_err_reset(f);
      rc = fsl_buffer_append(pOut, fsl_schema_ticket(), -1);
    }
    return rc;
  }
}


int fsl_cx_stat2( fsl_cx * const f, bool relativeToCwd,
                  char const * zName, fsl_fstat * const tgt,
                  fsl_buffer * const nameOut, bool fullPath){
  int rc;
  fsl_buffer * b = 0;
  fsl_buffer * bufRel = 0;
  fsl_size_t n = 0;
  assert(f);
  if(!zName || !*zName) return FSL_RC_MISUSE;
  else if(!fsl_needs_ckout(f)) return FSL_RC_NOT_A_CKOUT;
  b = fsl__cx_scratchpad(f);
  bufRel = fsl__cx_scratchpad(f);
#if 1
  rc = fsl_ckout_filename_check(f, relativeToCwd, zName, bufRel);
  if(rc) goto end;
  zName = fsl_buffer_cstr2( bufRel, &n );
#else
  if(!fsl_is_simple_pathname(zName, 1)){
    rc = fsl_ckout_filename_check(f, relativeToCwd, zName, bufRel);
    if(rc) goto end;
    zName = fsl_buffer_cstr2( bufRel, &n );
    /* MARKER(("bufRel=%s\n",zName)); */
  }else{
    n = fsl_strlen(zName);
  }
#endif
  assert(n>0 &&
         "Will fail if fsl_ckout_filename_check() changes "
         "to return nothing if zName==checkout root");
  if(!n
     /* i don't like the "." resp "./" result when zName==checkout root */
     || (1==n && '.'==bufRel->mem[0])
     || (2==n && '.'==bufRel->mem[0] && '/'==bufRel->mem[1])){
    rc = fsl_buffer_appendf(b, "%s%s", f->db.ckout.dir,
                            (2==n) ? "/" : "");
  }else{
    rc = fsl_buffer_appendf(b, "%s%s", f->db.ckout.dir, zName);
  }
  if(!rc){
    rc = fsl_stat( fsl_buffer_cstr(b), tgt, false );
    if(rc){
      fsl_cx_err_set(f, rc, "Error %s from fsl_stat(\"%b\")",
                     fsl_rc_cstr(rc), b);
    }else if(nameOut){
      rc = fullPath
        ? fsl_buffer_append(nameOut, b->mem, b->used)
        : fsl_buffer_append(nameOut, zName, n);
    }
  }
  end:
  fsl__cx_scratchpad_yield(f, b);
  fsl__cx_scratchpad_yield(f, bufRel);
  return rc;
}

int fsl_cx_stat(fsl_cx * const f, bool relativeToCwd,
                char const * zName, fsl_fstat * const tgt){
  return fsl_cx_stat2(f, relativeToCwd, zName, tgt, NULL, false);
}

bool fsl_cx_allows_symlinks(fsl_cx * const f, bool forceRecheck){
  if(forceRecheck || f->cache.allowSymlinks<0){
    f->cache.allowSymlinks = fsl_config_get_bool(f, FSL_CONFDB_REPO,
                                                 false, "allow-symlinks");
  }
  return f->cache.allowSymlinks>0;
}

void fsl_cx_case_sensitive_set(fsl_cx * const f, bool caseSensitive){
  f->cache.caseInsensitive = caseSensitive ? 0 : 1;
}

bool fsl_cx_is_case_sensitive(fsl_cx * const f, bool forceRecheck){
  if(forceRecheck || f->cache.caseInsensitive<0){
    f->cache.caseInsensitive =
      fsl_config_get_bool(f, FSL_CONFDB_REPO,
                          true, "case-sensitive") ? 0 : 1;
  }
  return f->cache.caseInsensitive <= 0;
}

int fsl_cx_filename_cmp(fsl_cx * f, char const * z1, char const * z2){
  return fsl_cx_is_case_sensitive(f, false)
    ? fsl_strcmp(z1, z2)
    : fsl_stricmp(z1, z2);
}

char const * fsl_cx_filename_collation(fsl_cx const * f){
  return f->cache.caseInsensitive>0 ? "COLLATE nocase" : "";
}

fsl_buffer * fsl__cx_content_buffer(fsl_cx * const f){
  if(f->cache.fileContent.used){
    fsl__fatal(FSL_RC_MISUSE,
               "Called %s() while the content buffer has bytes in use.");
  }
  ++f->metrics.content.nCached;
  return &f->cache.fileContent;
}

void fsl__cx_content_buffer_yield(fsl_cx * const f){
  enum { MaxSize = 1024 * 1024 * 10 };
  fsl_buffer * const b = &f->cache.fileContent;
  assert(f);
  f->metrics.content.nTotalUsed += b->used;
  if( b->capacity > f->metrics.content.nPeakBufSize ){
    f->metrics.content.nPeakBufSize = b->capacity;
  }
  if(b->capacity>MaxSize){
    ++f->metrics.content.nCappedMaxSize;
    fsl_buffer_resize(b, MaxSize);
    assert( (b->capacity<=MaxSize+1) || b->errCode);
  }
  fsl_buffer_reuse(b);
}

fsl_error const * fsl_cx_err_ec(fsl_cx const * f){
  return &f->error;
}
fsl_error * fsl_cx_err_e(fsl_cx * f){
  return &f->error;
}

char const * fsl_cx_glob_matches( fsl_cx * const f, int gtype,
                                  char const * str ){
  int i, count = 0;
  char const * rv = NULL;
  fsl_list const * lists[] = {0,0,0};
  if(!f || !str || !*str) return NULL;
  if(gtype & FSL_GLOBS_IGNORE) lists[count++] = &f->cache.globs.ignore;
  if(gtype & FSL_GLOBS_CRNL) lists[count++] = &f->cache.globs.crnl;
  /*CRNL/BINARY together makes little sense, but why strictly prohibit
    it?*/
  if(gtype & FSL_GLOBS_BINARY) lists[count++] = &f->cache.globs.binary;
  for( i = 0; i < count; ++i ){
    if( (rv = fsl_glob_list_matches( lists[i], str )) ) break;
  }
  return rv;
}

int fsl_output_f_fsl_cx(void * state, void const * src, fsl_size_t n ){
  return (state && src && n)
    ? fsl_output((fsl_cx*)state, src, n)
    : (n ? FSL_RC_MISUSE : 0);
}

int fsl_cx_hash_buffer( fsl_cx const * f, bool useAlternate,
                        fsl_buffer const * pIn, fsl_buffer * pOut){
  /* fossil(1) counterpart: hname_hash() */
  if(useAlternate){
    switch(f->cxConfig.hashPolicy){
      case FSL_HPOLICY_AUTO:
      case FSL_HPOLICY_SHA1:
        return fsl_sha3sum_buffer(pIn, pOut);
      case FSL_HPOLICY_SHA3:
        return fsl_sha1sum_buffer(pIn, pOut);
      default: return FSL_RC_UNSUPPORTED;
    }
  }else{
    switch(f->cxConfig.hashPolicy){
      case FSL_HPOLICY_SHA1:
      case FSL_HPOLICY_AUTO:
        return fsl_sha1sum_buffer(pIn, pOut);
      case FSL_HPOLICY_SHA3:
      case FSL_HPOLICY_SHA3_ONLY:
      case FSL_HPOLICY_SHUN_SHA1:
        return fsl_sha3sum_buffer(pIn, pOut);
    }
  }
  assert(!"not reached");
  return FSL_RC_RANGE;
}

int fsl_cx_hash_filename( fsl_cx * f, bool useAlternate,
                          const char * zFilename, fsl_buffer * pOut){
  /* FIXME: reimplement this to stream the content in bite-sized
     chunks. That requires duplicating most of fsl_buffer_fill_from()
     and fsl_cx_hash_buffer(). */
  fsl_buffer * const content = fsl__cx_content_buffer(f);
  int rc;
  rc = fsl_buffer_fill_from_filename(content, zFilename);
  if(!rc){
    rc = fsl_cx_hash_buffer(f, useAlternate, content, pOut);
  }
  fsl__cx_content_buffer_yield(f);
  return rc;
}

char const * fsl_hash_policy_name(fsl_hashpolicy_e p){
  switch(p){
    case FSL_HPOLICY_SHUN_SHA1: return "shun-sha1";
    case FSL_HPOLICY_SHA3: return "sha3";
    case FSL_HPOLICY_SHA3_ONLY: return "sha3-only";
    case FSL_HPOLICY_SHA1: return "sha1";
    case FSL_HPOLICY_AUTO: return "auto";
    default: return NULL;
  }
}

fsl_hashpolicy_e fsl_cx_hash_policy_set(fsl_cx *f, fsl_hashpolicy_e p){
  fsl_hashpolicy_e const old = f->cxConfig.hashPolicy;
  fsl_db * const dbR = fsl_cx_db_repo(f);
  if(dbR){
    /* Write it regardless of whether it's the same as the old policy
       so that we're sure the db knows the policy. */
    if(FSL_HPOLICY_AUTO==p &&
       fsl_db_exists(dbR,"SELECT 1 FROM blob WHERE length(uuid)>40")){
      p = FSL_HPOLICY_SHA3;
    }
    fsl_config_set_int32(f, FSL_CONFDB_REPO, "hash-policy", p);
  }
  f->cxConfig.hashPolicy = p;
  return old;
}

fsl_hashpolicy_e fsl_cx_hash_policy_get(fsl_cx const*f){
  return f->cxConfig.hashPolicy;
}

int fsl_cx_txn_level(fsl_cx * const f){
   return f->dbMain
    ? fsl_db_txn_level(f->dbMain)
    : 0;
}

int fsl_cx_txn_begin(fsl_cx * const f){
  int const rc = fsl_db_txn_begin(f->dbMain);
  if( rc ) return fsl_cx_uplift_db_error2(f, f->dbMain, rc);
  return 0;
}

int fsl_cx_txn_end_v2(fsl_cx * f, bool keepSavepoint, bool bubbleRollback){
  assert( f->dbMain );
  assert( f->dbMain->impl.txn.level>0 );
  int const rc = fsl_db_txn_end_v2(f->dbMain, keepSavepoint,
                                   bubbleRollback);
  if( rc ) return fsl_cx_uplift_db_error2(f, f->dbMain, rc);
  return 0;
}

void fsl_cx_confirmer(fsl_cx * f,
                      fsl_confirmer const * newConfirmer,
                      fsl_confirmer * prevConfirmer){
  if(prevConfirmer) *prevConfirmer = f->confirmer;
  f->confirmer = newConfirmer ? *newConfirmer : fsl_confirmer_empty;
}

void fsl_cx_confirmer_get(fsl_cx const * f, fsl_confirmer * dest){
  *dest = f->confirmer;
}

int fsl_cx_confirm(fsl_cx * const f, fsl_confirm_detail const * detail,
                   fsl_confirm_response *outAnswer){
  if(f->confirmer.callback){
    return f->confirmer.callback(detail, outAnswer,
                                 f->confirmer.callbackState);
  }
  /* Default answers... */
  switch(detail->eventId){
    case FSL_CEVENT_OVERWRITE_MOD_FILE:
    case FSL_CEVENT_OVERWRITE_UNMGD_FILE:
      outAnswer->response =  FSL_CRESPONSE_NEVER;
      break;
    case FSL_CEVENT_RM_MOD_UNMGD_FILE:
      outAnswer->response = FSL_CRESPONSE_NEVER;
      break;
    case FSL_CEVENT_MULTIPLE_VERSIONS:
      outAnswer->response = FSL_CRESPONSE_CANCEL;
      break;
    default:
      assert(!"Unhandled fsl_confirm_event_e value");
      fsl__fatal(FSL_RC_UNSUPPORTED,
                "Unhandled fsl_confirm_event_e value: %d",
                detail->eventId)/*does not return*/;
  }
  return 0;
}

int fsl__cx_update_seen_delta_deck(fsl_cx * const f){
  int rc = 0;
  fsl_db * const d = fsl_cx_db_repo(f);
  if(d && f->cache.seenDeltaManifest <= 0){
    f->cache.seenDeltaManifest = 1;
    rc = fsl_config_set_bool(f, FSL_CONFDB_REPO,
                             "seen-delta-manifest", 1);
  }
  return rc;
}

int fsl_reserved_fn_check(fsl_cx * const f, const char *zPath,
                          fsl_int_t nPath, bool relativeToCwd){
  static const int errRc = FSL_RC_RANGE;
  int rc = 0;
  char const * z1 = 0;
  if(nPath<0) nPath = (fsl_int_t)fsl_strlen(zPath);
  if(fsl_is_reserved_fn(zPath, nPath)){
    return fsl_cx_err_set(f, errRc,
                        "Filename is reserved, not legal "
                        "for adding to a repository: %.*s",
                        (int)nPath, zPath);
  }
  if(!(f->flags & FSL_CX_F_ALLOW_WINDOWS_RESERVED_NAMES)
     && fsl__is_reserved_fn_windows(zPath, nPath)){
    return fsl_cx_err_set(f, errRc,
                          "Filename is a Windows reserved name: %.*s",
                          (int)nPath, zPath);
  }
  if((z1 = fsl_cx_db_file_for_role(f, FSL_DBROLE_REPO, NULL))){
    fsl_buffer * const c1 = fsl__cx_scratchpad(f);
    fsl_buffer * const c2 = fsl__cx_scratchpad(f);
    rc = fsl_file_canonical_name2(relativeToCwd ? NULL : f->db.ckout.dir/*NULL is okay*/,
                                  z1, c1, false);
    if(!rc) rc = fsl_file_canonical_name2(relativeToCwd ? NULL : f->db.ckout.dir,
                                          zPath, c2, false);
    //MARKER(("\nzPath=%s\nc1=%s\nc2=%s\n", zPath,
    //fsl_buffer_cstr(c1), fsl_buffer_cstr(c2)));
    if(!rc && c1->used == c2->used &&
       0==fsl_stricmp(fsl_buffer_cstr(c1), fsl_buffer_cstr(c2))){
      rc = fsl_cx_err_set(f, errRc, "File is the repository database: %.*s",
                          (int)nPath, zPath);
    }
    fsl__cx_scratchpad_yield(f, c1);
    fsl__cx_scratchpad_yield(f, c2);
    if(rc) return rc;
  }
  assert(!rc);
  while(true){
    /* Check the name against the repo's "manifest" setting and reject
       any filenames which that setting implies. */
    int manifestSetting = 0;
    fsl_ckout_manifest_setting(f, &manifestSetting);
    if(!manifestSetting) break;
    typedef struct {
      short flag;
      char const * fn;
    } MSetting;
    const MSetting M[] = {
    {FSL_MANIFEST_MAIN, "manifest"},
    {FSL_MANIFEST_UUID, "manifest.uuid"},
    {FSL_MANIFEST_TAGS, "manifest.tags"},
    {0,0}
    };
    fsl_buffer * const c1 = fsl__cx_scratchpad(f);
    if(f->db.ckout.dir){
      rc = fsl_ckout_filename_check(f, relativeToCwd, zPath, c1);
    }else{
      rc = fsl_file_canonical_name2("", zPath, c1, false);
    }
    if(rc) goto yield;
    char const * const z = fsl_buffer_cstr(c1);
    //MARKER(("Checking file against manifest setting 0x%03x: %s\n",
    //manifestSetting, z));
    for( MSetting const * m = &M[0]; m->fn; ++m ){
      if((m->flag & manifestSetting)
         && 0==fsl_strcmp(z, m->fn)){
        rc = fsl_cx_err_set(f, errRc,
                            "Filename is reserved due to the "
                            "'manifest' setting: %s",
                            m->fn);
        break;
      }
    }
    yield:
    fsl__cx_scratchpad_yield(f, c1);
    break;
  }
  return rc;
}

fsl_buffer * fsl__cx_scratchpad(fsl_cx * const f){
  fsl_buffer * rc = 0;
  int i = (f->scratchpads.next<FSL_CX_NSCRATCH)
    ? f->scratchpads.next : 0;
  for(; i < FSL_CX_NSCRATCH; ++i){
    if(!f->scratchpads.used[i]){
      rc = &f->scratchpads.buf[i];
      f->scratchpads.used[i] = true;
      ++f->scratchpads.next;
      //MARKER(("Doling out scratchpad[%d] w/ capacity=%d next=%d\n",
      //        i, (int)rc->capacity, f->scratchpads.next));
      break;
    }
  }
  if(!rc){
    assert(!"Fatal fsl_cx::scratchpads misuse.");
    fsl__fatal(FSL_RC_MISUSE,
              "Fatal internal fsl_cx::scratchpads misuse: "
              "too many unyielded buffer requests.");
  }else if(0!=rc->used){
    assert(!"Fatal fsl_cx::scratchpads misuse.");
    fsl__fatal(FSL_RC_MISUSE,
              "Fatal internal fsl_cx::scratchpads misuse: "
              "used buffer after yielding it.");
  }
  return rc;
}

void fsl__cx_scratchpad_yield(fsl_cx * const f, fsl_buffer * const b){
  int i;
  assert(b);
  for(i = 0; i < FSL_CX_NSCRATCH; ++i){
    if(b == &f->scratchpads.buf[i]){
      assert(f->scratchpads.next != i);
      assert(f->scratchpads.used[i] && "Scratchpad misuse.");
      f->scratchpads.used[i] = false;
      fsl_buffer_reuse(b);
      if(f->scratchpads.next>i) f->scratchpads.next = i;
      //MARKER(("Yielded scratchpad[%d] w/ capacity=%d, next=%d\n",
      //        i, (int)b->capacity, f->scratchpads.next));
      return;
    }
  }
  fsl__fatal(FSL_RC_MISUSE,
            "Fatal internal fsl_cx::scratchpads misuse: "
            "passed a non-scratchpad buffer.");
}


/** @internal

   Don't use this. Use fsl__ckout_rm_empty_dirs() instead.

   Attempts to remove empty directories from under a checkout,
   starting with tgtDir and working upwards until it either cannot
   remove one or it reaches the top of the checkout dir.

   The first argument must be the canonicalized absolute path to the
   checkout root. The second is the length of coRoot - if it's
   negative then fsl_strlen() is used to calculate it. The third must
   be the canonicalized absolute path to some directory under the
   checkout root. The contents of the buffer may, for efficiency's
   sake, be modified by this routine as it traverses the directory
   tree. It will never grow the buffer but may mutate its memory's
   contents.

   Returns the number of directories it is able to remove.

   Results are undefined if tgtDir is not an absolute path or does not
   have coRoot as its initial prefix.

   There are any number of valid reasons removal of a directory might
   fail, and this routine stops at the first one which does.
*/
static unsigned fsl__rm_empty_dirs(char const * const coRoot,
                                   fsl_int_t rootLen,
                                  fsl_buffer const * const tgtDir){
  if(rootLen<0) rootLen = (fsl_int_t)fsl_strlen(coRoot);
  char const * zAbs = fsl_buffer_cstr(tgtDir);
  char const * zCoDirPart = zAbs + rootLen;
  char * zEnd = fsl_buffer_str(tgtDir) + tgtDir->used - 1;
  unsigned rc = 0;
  assert(coRoot);
  if(0!=memcmp(coRoot, zAbs, (size_t)rootLen)){
    assert(!"Misuse of fsl__rm_empty_dirs()");
    return 0;
  }
  if(fsl_rmdir(zAbs)) return rc;
  ++rc;
  /** Now walk up each dir in the path and try to remove each,
      stopping when removal of one fails or we reach coRoot. */
  while(zEnd>zCoDirPart){
    for( ; zEnd>zCoDirPart && '/'!=*zEnd; --zEnd ){}
    if(zEnd==zCoDirPart) break;
    else if('/'==*zEnd){
      *zEnd = 0;
      assert(zEnd>zCoDirPart);
      if(fsl_rmdir(zAbs)) break;
      ++rc;
    }
  }
  return rc;
}

unsigned int fsl__ckout_rm_empty_dirs(fsl_cx * const f,
                                      fsl_buffer const * const tgtDir){
  int rc = f->db.ckout.dir ? 0 : FSL_RC_NOT_A_CKOUT;
  if(!rc){
    rc = fsl__rm_empty_dirs(f->db.ckout.dir, f->db.ckout.dirLen, tgtDir);
  }
  return rc;
}

int fsl__ckout_rm_empty_dirs_for_file(fsl_cx * const f, char const *zAbsPath){
  if(!fsl_is_rooted_in_ckout(f, zAbsPath)){
    assert(!"Internal API misuse!");
    return FSL_RC_MISUSE;
  }else{
    fsl_buffer * const p = fsl__cx_scratchpad(f);
    fsl_int_t const nAbs = (fsl_int_t)fsl_strlen(zAbsPath);
    int const rc = fsl_file_dirpart(zAbsPath, nAbs, p, false);
    if(!rc) fsl__rm_empty_dirs(f->db.ckout.dir, f->db.ckout.dirLen, p);
    fsl__cx_scratchpad_yield(f,p);
    return rc;
  }
}

int fsl_ckout_fingerprint_check(fsl_cx * const f){
  fsl_db * const db = fsl_cx_db_ckout(f);
  if(!db
     || !f->db.ckout.rid/*new repo - no fingerprint*/){
    return 0;
  }
  int rc = 0;
  char const * zCkout = 0;
  char * zRepo = 0;
  fsl_id_t rcvCkout = 0;
  fsl_buffer * const buf = fsl__cx_scratchpad(f);
  rc = fsl_config_get_buffer(f, FSL_CONFDB_CKOUT, "fingerprint", buf);
  if(FSL_RC_NOT_FOUND==rc){
    /* Older checkout with no fingerprint. Assume it's okay. */
    rc = 0;
    fsl_cx_err_reset(f);
    goto end;
  }else if(rc){
    goto end;
  }
  zCkout = fsl_buffer_cstr(buf);
#if 0
  /* Inject a bogus byte for testing purposes */
  buf->mem[6] = 'x';
#endif
  rcvCkout = (fsl_id_t)atoi(zCkout);
  rc = fsl__repo_fingerprint_search(f, rcvCkout, &zRepo);
  switch(rc){
    case FSL_RC_NOT_FOUND: goto mismatch;
    case 0:
      assert(zRepo);
      if(fsl_strcmp(zRepo,zCkout)){
        goto mismatch;
      }
      break;
    default:
      break;
  }
  end:
  fsl__cx_scratchpad_yield(f, buf);
  fsl_free(zRepo);
  return rc;
  mismatch:
  rc = fsl_cx_err_set(f, FSL_RC_REPO_MISMATCH,
                      "Mismatch found between repo/checkout "
                      "fingerprints.");
  goto end;
}

bool fsl_cx_has_ckout(fsl_cx const * const f ){
  return f->db.ckout.dir ? true : false;
}

int fsl_cx_interruptv(fsl_cx * const f, int code, char const * fmt, va_list args){
  f->interrupted = code;
  if(code && NULL!=fmt){
    code = fsl_cx_err_setv(f, code, fmt, args);
  }
  return code;
}

int fsl_cx_interrupt(fsl_cx * const f, int code, char const * fmt, ...){
  int rc;
  va_list args;
  va_start(args,fmt);
  rc = fsl_cx_interruptv(f, code, fmt, args);
  va_end(args);
  return rc;
}

int fsl_cx_interrupted(fsl_cx const * const f){
  return f->interrupted;
}

int fsl__cx_see_key(fsl_cx * const f, const char *zDbFile,
                    fsl_buffer *pOut, int *keyType){
  if( f->cxConfig.see.getSEEKey ){
    int const rc = f->cxConfig.see.getSEEKey( f->cxConfig.see.pState,
                                              zDbFile, pOut, keyType );
    switch( rc ){
      case 0:
      case FSL_RC_OOM: return rc;
      case FSL_RC_UNSUPPORTED: break;
      default: return fsl_cx_err_set(f, rc, "SEE key init failed.");
    }
  }
  *keyType = 0;
  return 0;
}

#if 0
struct tm * fsl_cx_localtime( fsl_cx const * f, const time_t * clock ){
  if(!clock) return NULL;
  else if(!f) return localtime(clock);
  else return (f->flags & FSL_CX_F_LOCALTIME_GMT)
         ? gmtime(clock)
         : localtime(clock)
         ;
}

struct tm * fsl_localtime( const time_t * clock ){
  return fsl_cx_localtime(NULL, clock);
}

time_t fsl_cx_time_adj(fsl_cx const * f, time_t clock){
  struct tm * tm = fsl_cx_localtime(f, &clock);
  return tm ? mktime(tm) : 0;
}
#endif

int fsl_cx_last_sync_url(fsl_cx * const f, fsl_buffer *tgt){
  return fsl_config_get_buffer(f, FSL_CONFDB_REPO, "last-sync-url", tgt);
}

#define STMT_YIELD_BIT 0x0200 /* must not colide with db.c:fsl__stmt_flags_e */

fsl_stmt * fsl__cx_stmt( fsl_cx * f, enum fsl__cx_stmt_e which ){
  fsl_stmt * q = 0;
  char const * zSql = 0;
  char const * zName = 0;

#if 0
#define E(NAME,SQL) \
  static int xx_ ## NAME = 0;
    fsl__cx_cache_stmt_map(E)
#undef E
#endif

  assert( f->dbMain->role & FSL_DBROLE_REPO );
  switch(which){
#define CASE(NAME,SQL) \
    case fsl__cx_stmt_e_ ## NAME: \
      q = &f->cache.stmt.NAME; zSql = SQL; zName = #NAME; \
      break;
    fsl__cx_cache_stmt_map(CASE)
#undef CASE
  }
  assert( q );
  assert( zSql );
  assert( zName );
#if !defined(NDEBUG)
  if( !*zSql ){
    fsl__fatal(FSL_RC_CANNOT_HAPPEN, "Missing SQL for %s(%d)",
               __func__, (int)which);
  }
#endif
  if( q->stmt ){
    if( STMT_YIELD_BIT & q->impl.flags ){
#if !defined(NDEBUG)
      assert( !(STMT_YIELD_BIT & q->impl.flags) && "Statement is already checked out");
      fsl__fatal(FSL_RC_MISUSE,
                 "Internal API misuse: %s() was used to fetch "
                 "cached statement '%s', but it's already checked out.",
                 __func__, zName) /* does not return */;
#else
      rc = fsl_cx_err_set(f, FSL_RC_MISUSE,
                          "Internal interal API misuse: %s() was used to fetch "
                          "cached statement '%s', but it's already checked out.",
                          __func__, zName);

#endif
    }else{
      q->impl.flags |= STMT_YIELD_BIT;
    }
  }else{
#if 0
    switch(which){
#define E(NAME,SQL) \
      case fsl__cx_stmt_e_ ##NAME: assert( ++xx_ ## NAME < 2 );
      fsl__cx_cache_stmt_map(E)
#undef E
    }
#endif
    if( fsl_cx_prepare(f, q, "%s /* %s(%s) */",
                       zSql, __func__, zName) ){
      q = 0;
    }else{
      q->impl.flags |= STMT_YIELD_BIT;
    }
  }
  return q;
}

void fsl__cx_stmt_yield( fsl_cx * f, fsl_stmt * q ){
  if( q ){
    (void)f;
    assert( STMT_YIELD_BIT & q->impl.flags && "Statement check-out misuse");
    q->impl.flags &= ~STMT_YIELD_BIT;
    fsl_stmt_reset(q);
    fsl_stmt_clear_bindings(q);
  }
}

int fsl__ckout_rid(fsl_cx * const f, fsl_id_t *rid){
  int rc = 0;
  if( (*rid=f->db.ckout.rid) <= 0 ){
    if( 0==(rc = fsl__ckout_version_fetch(f)) ){
      *rid = f->db.ckout.rid;
    }
  }
  return rc;
}

#undef MARKER
#undef FSL_CX_NSCRATCH
