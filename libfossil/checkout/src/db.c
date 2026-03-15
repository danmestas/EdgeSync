/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).

  *****************************************************************************
  This file contains the fsl_db_xxx() and fsl_stmt_xxx() parts of the
  API.

  Maintenance reminders:

  When returning dynamically allocated memory to the client, it needs
  to come from fsl_malloc(), as opposed to sqlite3_malloc(), so that
  it is legal to pass to fsl_free().
*/
#include "fossil-scm/internal.h"
#include <assert.h>
#include <string.h> /* strchr() */
/* Only for debugging */
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

#if 0
/**
    fsl_list_visitor_f() impl which requires that obj be NULL or
    a (fsl_stmt*), which it passed to fsl_stmt_finalize().
 */
static int fsl_list_v_fsl_stmt_finalize(void * obj, void * visitorState ){
  if(obj) fsl_stmt_finalize( (fsl_stmt*)obj );
  return 0;
}
#endif


int fsl__db_errcode(fsl_db * const db, int sqliteCode){
  int rc = 0;
  if(!sqliteCode) sqliteCode = sqlite3_errcode(db->dbh);
  switch(sqliteCode & 0xff){
    case SQLITE_ROW:
    case SQLITE_DONE:
    case SQLITE_OK: rc = 0; break;
    case SQLITE_NOMEM: rc = FSL_RC_OOM; break;
    case SQLITE_INTERRUPT: rc = FSL_RC_INTERRUPTED; break;
    case SQLITE_TOOBIG:
    case SQLITE_FULL:
    case SQLITE_RANGE: rc = FSL_RC_RANGE; break;
    case SQLITE_NOTFOUND: rc = FSL_RC_NOT_FOUND; break;
    case SQLITE_PERM:
    case SQLITE_AUTH:
    case SQLITE_BUSY:
    case SQLITE_LOCKED: rc = FSL_RC_LOCKED; break;
    case SQLITE_READONLY: rc = FSL_RC_ACCESS; break;
    case SQLITE_CORRUPT: rc = FSL_RC_CONSISTENCY; break;
    case SQLITE_CANTOPEN:
    case SQLITE_IOERR: rc = FSL_RC_IO; break;
    case SQLITE_NOLFS: rc = FSL_RC_UNSUPPORTED; break;
    default:
      //MARKER(("sqlite3_errcode()=0x%04x\n", rc));
      rc = FSL_RC_DB; break;
  }
  return rc
    ? fsl_error_set(&db->error, rc,
                    "sqlite3 error #%d: %s",
                    /* use ^^^^ "error" instead of "result code" to
                       match sqlite3's own output. */
                    sqliteCode, sqlite3_errmsg(db->dbh))
    : (fsl_error_reset(&db->error), 0);
}

void fsl__db_clear_strings(fsl_db * const db, bool alsoBuffers){
  fsl_free(db->filename);
  db->filename = NULL;
  fsl_free(db->name);
  db->name = NULL;
  if(alsoBuffers){
    fsl_buffer_clear(&db->impl.buffer);
    fsl_error_clear(&db->error);
  }
}

int fsl_db_err_get( fsl_db const * const db, char const ** msg, fsl_size_t * len ){
  return fsl_error_get(&db->error, msg, len);
}

fsl_db * fsl_stmt_db( fsl_stmt * const stmt ){
  return stmt->db;
}

char const * fsl_stmt_sql( fsl_stmt * const stmt, fsl_size_t * const len ){
  return fsl_buffer_cstr2(&stmt->sql, len);
}

char const * fsl_db_filename(fsl_db const * db, fsl_size_t * len){
  if(len && db->filename) *len = fsl_strlen(db->filename);
  return db->filename;
}

fsl_id_t fsl_db_last_insert_id(fsl_db * const db){
  return (db && db->dbh)
    ? (fsl_id_t)sqlite3_last_insert_rowid(db->dbh)
    : -1;
}

/**
   Immediately cleans up all cached statements (if any) and returns
   the number of statements cleaned up. It is illegal to call this
   while any of the cached statements are actively being used (have
   not been fsl_stmt_cached_yield()ed), and doing so will lead to
   undefined results if the statement(s) in question are used after
   this function completes.

   @see fsl_db_prepare_cached()
   @see fsl_stmt_cached_yield()
*/
static fsl_size_t fsl_db_stmt_cache_clear(fsl_db * const db){
  fsl_size_t rc = 0;
  if(db && db->impl.stCache.head){
    fsl_stmt * st;
    fsl_stmt * next = 0;
    for( st = db->impl.stCache.head; st; st = next, ++rc ){
      next = st->impl.next;
      st->impl.next = 0;
      fsl_stmt_finalize( st );
    }
    db->impl.stCache.head = 0;
  }
  return rc;
}

static void fsl__db_finalize_sp( fsl_db * const db ){
#define STMT(MEMBER) sqlite3_finalize(db->impl.txn.MEMBER); db->impl.txn.MEMBER=0
  STMT(sSavepoint);
  STMT(sRelease);
  STMT(sRollback);
#undef STMT
}

static void fsl__db_err_dump2(fsl_db * const db, char const *zFile, int line){
  MARKER(( "%s:%d: rc=%s %s\n", zFile, line,
           fsl_rc_cstr(db->error.code),
           fsl_buffer_cstr(&db->error.msg) ));
}
#define fsl__db_err_dump(DB) fsl__db_err_dump2(DB, __FILE__, __LINE__)

static int fsl__db_fire(fsl_db * const db, fsl_db_event_e mode, void * payload){
  int rc = 0;
  if( db->impl.event.f &&
      (FSL_DB_EVENT_mask_id & db->impl.event.maskIds & mode)
  ){
    int const spLevel = db->impl.txn.level + (FSL_DB_EVENT_BEGIN==mode);
    fsl_db_event const ev = {
      .db = db,
      .state = db->impl.event.state,
      .type = mode,
      .savepointLevel = spLevel,
      .payload = payload
    };
    rc = db->impl.event.f(&ev);
    if( rc && !db->error.code ){
      char const *zMode = 0;
      switch(mode){
#define E(X,V) case X: zMode = # X ; break;
        fsl_db_event_map(E)
#undef E
      }
      rc = fsl_error_set(&db->error, rc,
                         "Transaction hook %s for level %d failed",
                         zMode, spLevel);
      fsl__db_err_dump(db);
    }
  }
  return rc;
}

void fsl_db_close( fsl_db * const db ){
  if(!db) return;
  while(db->impl.txn.level>0){
    /* We do this primarily for the sake of keeping downstream code's
       internals intact. */
    fsl_db_txn_end_v2(db, false, true);
  }
  fsl__db_fire(db, FSL_DB_EVENT_CLOSING, NULL);
  fsl_db_stmt_cache_clear(db);
  if(db->impl.openStatementCount>0){
    assert(db->dbh);
    MARKER(("WARNING: %d open statement(s) left on db [%s].\n",
            (int)db->impl.openStatementCount, db->filename));
    sqlite3_stmt * pq = NULL;
    while( (pq = sqlite3_next_stmt(db->dbh, pq)) ){
      MARKER(("Lingering stmt: %s\n", sqlite3_sql(pq)));
    }
    //assert(!"here");
  }
  fsl__db_finalize_sp(db);
  if(db->dbh){
    sqlite3_close_v2(db->dbh);
  }
  fsl__db_clear_strings(db, true);
  void const * const allocStamp = db->impl.allocStamp;
  *db = fsl_db_empty;
  if(&fsl_db_empty == allocStamp){
    fsl_free( db );
  }else{
    db->impl.allocStamp = allocStamp;
  }
  return;
}

void fsl_db_err_reset( fsl_db * const db ){
  if(db && (db->error.code||db->error.msg.used)){
    fsl_error_reset(&db->error);
  }
}

int fsl_db_attach(fsl_db * const db, const char *zDbName,
                  const char *zLabel){
  return (db && db->dbh && zDbName && *zDbName && zLabel && *zLabel)
    ? fsl_db_exec(db, "ATTACH DATABASE %Q AS %!Q /*%s()*/",
                  zDbName, zLabel, __func__)
    : FSL_RC_MISUSE;
}

int fsl_db_detach(fsl_db * const db, const char *zLabel){
  return (db && db->dbh && zLabel && *zLabel)
    ? fsl_db_exec(db, "DETACH DATABASE %!Q /*%s()*/", zLabel, __func__)
    : FSL_RC_MISUSE;
}

char const * fsl_db_name(fsl_db const * const db){
  return db ? db->name : NULL;
}

/**
    Returns the db name for the given role.
 */
const char * fsl_db_role_name(fsl_dbrole_e r){
  switch(r){
    case FSL_DBROLE_CONFIG:
      return "cfg";
    case FSL_DBROLE_REPO:
      return "repo";
    case FSL_DBROLE_CKOUT:
      return "ckout";
    case FSL_DBROLE_MAIN:
      return "main";
    case FSL_DBROLE_TEMP:
      return "temp";
    case FSL_DBROLE_NONE:
    default:
      return NULL;
  }
}

char * fsl_db_julian_to_iso8601( fsl_db * const db, double j,
                                 bool msPrecision,
                                 bool localTime){
  char * s = NULL;
  fsl_stmt * st = NULL;
  if(db && db->dbh && (j>=0.0)){
    char const * sql;
    if(msPrecision){
      sql = localTime
        ? "SELECT strftime('%%Y-%%m-%%dT%%H:%%M:%%f',?, 'localtime')"
        : "SELECT strftime('%%Y-%%m-%%dT%%H:%%M:%%f',?)";
    }else{
      sql = localTime
        ? "SELECT strftime('%%Y-%%m-%%dT%%H:%%M:%%S',?, 'localtime')"
        : "SELECT strftime('%%Y-%%m-%%dT%%H:%%M:%%S',?)";
    }
    fsl_db_prepare_cached(db, &st, sql);
    if(st){
      fsl_stmt_bind_double( st, 1, j );
      if( FSL_RC_STEP_ROW==fsl_stmt_step(st) ){
        s = fsl_strdup(fsl_stmt_g_text(st, 0, NULL));
      }
      fsl_stmt_cached_yield(st);
    }
  }
  return s;
}

char * fsl_db_unix_to_iso8601( fsl_db * const db, fsl_time_t t, bool localTime ){
  char * s = NULL;
  fsl_stmt * st = NULL;
  if(db && db->dbh && (t>=0)){
    char const * sql = localTime
      ? "SELECT datetime(?, 'unixepoch', 'localtime')/*%s()*/"
      : "SELECT datetime(?, 'unixepoch')/*%s()*/"
      ;
    int const rc = fsl_db_prepare_cached(db, &st, sql,__func__);
    if(!rc){
      fsl_stmt_bind_int64(st, 1, t);
      if( FSL_RC_STEP_ROW==fsl_stmt_step(st) ){
        fsl_size_t n = 0;
        char const * v = fsl_stmt_g_text(st, 0, &n);
        s = (v&&n) ? fsl_strndup(v, (fsl_int_t)n) : NULL;
      }
      fsl_stmt_cached_yield(st);
    }
  }
  return s;
}

enum fsl__stmt_flags_e {
  /**
     fsl_stmt::flags bit indicating that fsl_db_preparev_cached() has
     doled out this statement, effectively locking it until
     fsl_stmt_cached_yield() is called to release it.
  */
  FSL__STMT_F_CACHE_HELD = 0x01,

  /**
     Propagates our intent to "statically" prepare a given statement
     through various internal API calls.
  */
  FSL__STMT_F_PREP_CACHE = 0x10
};

/**
   For use with fsl_db::flags.
*/
enum fsl__db_flag_e {
  /**
     For communicating rollback state through the call stack. If this
     is set, the final fsl_db_txn_end() call in a stack will behave
     like a rollback regardless of the arguments to that
     function. i.e. it propagates a rollback through pseudo-nested
     transactions.
  */
  FSL__DB_F_BUBBLING_ROLLBACK = 0x01,
  /**
     Tells fsl__db_verify_begin_was_not_called() that the commit in
     question was triggered by this API instead of random input.
  */
  FSL__DB_F_IN_COMMIT = 0x02
};

int fsl_db_preparev( fsl_db  * const db, fsl_stmt * const tgt, char const * sql, va_list args ){
  if(!db || !tgt || !sql) return FSL_RC_MISUSE;
  else if(!db->dbh){
    return fsl_error_set(&db->error, FSL_RC_NOT_FOUND, "Db is not opened.");
  }else if(!*sql){
    return fsl_error_set(&db->error, FSL_RC_RANGE, "SQL is empty.");
  }else if(tgt->stmt){
    return fsl_error_set(&db->error, FSL_RC_ALREADY_EXISTS,
                         "Error: attempt to re-prepare "
                         "active statement.");
  }
  else{
    int rc;
    fsl_buffer buf = fsl_buffer_empty;
    fsl_stmt_t * liteStmt = NULL;
    rc = fsl_buffer_appendfv( &buf, sql, args );
    if(!rc){
#if 0
      /* Arguably improves readability of some queries.
         And breaks some caching uses. */
      fsl_simplify_sql_buffer(&buf);
#endif
      sql = fsl_buffer_cstr(&buf);
      if(!sql || !*sql){
        rc = fsl_error_set(&db->error, FSL_RC_RANGE,
                           "Input SQL is empty.");
      }else{
        /*
          Achtung: if sql==NULL here, or evaluates to a no-op
          (e.g. only comments or spaces), prepare_v2 succeeds but has
          a NULL liteStmt, which is why we handle the empty-SQL case
          specially. We don't want that specific behaviour leaking up
          through the API. Though doing so would arguably more correct
          in a generic API, for this particular API we have no reason
          to be able to handle empty SQL. Where we do let through
          through we'd have to add a flag to fsl_stmt to tell us
          whether it's really prepared or not, since checking of
          st->stmt would no longer be useful.
        */
        rc = sqlite3_prepare_v3(db->dbh, sql, (int)buf.used,
                                (FSL__STMT_F_PREP_CACHE & tgt->impl.flags)
                                ? SQLITE_PREPARE_PERSISTENT
                                : 0,
                                &liteStmt, NULL);
        if(rc){
          rc = fsl_error_set(&db->error, FSL_RC_DB,
                             "Db statement preparation failed. "
                             "Error #%d: %s. SQL: %.*s",
                             rc, sqlite3_errmsg(db->dbh),
                             (int)buf.used, (char const *)buf.mem);
        }else if(!liteStmt){
          /* SQL was empty. In sqlite this is allowed, but this API will
             disallow this because it leads to headaches downstream.
          */
          rc = fsl_error_set(&db->error, FSL_RC_RANGE,
                             "Input SQL is empty.");
        }
      }
    }
    if(!rc){
      assert(liteStmt);
      ++db->impl.openStatementCount;
      tgt->stmt = liteStmt;
      tgt->db = db;
      tgt->sql = buf /*transfer ownership*/;
    }else{
      assert(!liteStmt);
      fsl_buffer_clear(&buf);
    }
    return rc;
  }
}

int fsl_db_prepare( fsl_db * const db, fsl_stmt * const tgt, char const * sql, ... ){
  int rc;
  va_list args;
  va_start(args,sql);
  rc = fsl_db_preparev( db, tgt, sql, args );
  va_end(args);
  return rc;
}


int fsl_db_preparev_cached( fsl_db * const db, fsl_stmt ** rv,
                            char const * sql, va_list args ){
  int rc = 0;
  fsl_buffer * const buf = &db->impl.buffer;
  fsl_stmt * st = NULL;
  fsl_stmt * cs = NULL;
  if(!db || !rv || !sql) return FSL_RC_MISUSE;
  else if(!*sql) return FSL_RC_RANGE;
  if(!buf->capacity && fsl_buffer_reserve(buf, 1024*2)){
    return FSL_RC_OOM;
  }
  fsl_buffer_reuse(buf);
  rc = fsl_buffer_appendfv(buf, sql, args);
  if(rc) goto end;
  /**
     Hash buf's contents using a very primitive algo and stores the
     hash in buf->cursor. This is a blatant abuse of that member but
     we're otherwise not using it on these buffer instances. We use
     this to slightly speed up lookup of previously-cached entries and
     reduce the otherwise tremendous number of calls to
     fsl_buffer_compare() libfossil makes.
  */
  for(fsl_size_t i = 0; i < buf->used; ++i){
    //buf->cursor = (buf->cursor<<3) ^ buf->cursor ^ buf->mem[i];
    buf->cursor = 31 * buf->cursor + (buf->mem[i] * 307);
  }
  for( cs = db->impl.stCache.head; cs; cs = cs->impl.next ){
    if(cs->sql.cursor==buf->cursor/*hash value!*/
       && buf->used==cs->sql.used
       && 0==fsl_buffer_compare(buf, &cs->sql)){
      if(cs->impl.flags & FSL__STMT_F_CACHE_HELD){
        rc = fsl_error_set(&db->error, FSL_RC_ACCESS,
                           "Cached statement is already in use. "
                           "Do not use cached statements if recursion "
                           "involving the statement is possible, and use "
                           "fsl_stmt_cached_yield() to release them "
                           "for further (re)use. SQL: %b",
                           &cs->sql);
        goto end;
      }
      cs->impl.flags |= FSL__STMT_F_CACHE_HELD;
      ++cs->impl.cachedHits;
      *rv = cs;
      goto end;
    }
  }
  st = fsl_stmt_malloc();
  if(!st){
    rc = FSL_RC_OOM;
    goto end;
  }
  st->impl.flags |= FSL__STMT_F_PREP_CACHE;
  rc = fsl_db_prepare( db, st, "%b", buf );
  if(rc){
    fsl_free(st);
    st = 0;
  }else{
    st->sql.cursor = buf->cursor/*hash value!*/;
    st->impl.next = db->impl.stCache.head;
    st->role = db->role
      /* Pessimistic assumption for purposes of invalidating
         fsl__db_cached_clear_role(). */;
    db->impl.stCache.head = st;
    st->impl.flags = FSL__STMT_F_CACHE_HELD;
    *rv = st;
  }
  end:
  return rc;
}

int fsl_db_prepare_cached( fsl_db * const db, fsl_stmt ** st, char const * sql, ... ){
  int rc;
  va_list args;
  va_start(args,sql);
  rc = fsl_db_preparev_cached( db, st, sql, args );
  va_end(args);
  return rc;
}

int fsl_stmt_cached_yield( fsl_stmt * const st ){
  if(!st || !st->db || !st->stmt) return FSL_RC_MISUSE;
  else if(!(st->impl.flags & FSL__STMT_F_CACHE_HELD)) {
    return fsl_error_set(&st->db->error, FSL_RC_MISUSE,
                         "fsl_stmt_cached_yield() was passed a "
                         "statement which is not marked as cached. "
                         "SQL: %b",
                         &st->sql);
  }else{
    fsl_stmt_reset(st);
    fsl_stmt_clear_bindings(st);
    st->impl.flags &= ~FSL__STMT_F_CACHE_HELD;
    return 0;
  }
}

int fsl_stmt_finalize( fsl_stmt * const stmt ){
  if(!stmt) return FSL_RC_MISUSE;
  else{
    void const * allocStamp = stmt->impl.allocStamp;
    fsl_db * const db = stmt->db;
    if(db){
      if(stmt->sql.mem){
        /* ^^^ b/c that buffer is set at the same time
           that openStatementCount is incremented.
        */
        --stmt->db->impl.openStatementCount;
      }
      if(db->impl.stCache.head){
        /* It _might_ be cached - let's remove it. */
        fsl_stmt * s;
        fsl_stmt * prev = 0;
        for( s = db->impl.stCache.head; s; prev = s, s = s->impl.next ){
          if(s == stmt){
            if(prev){
              assert(prev->impl.next == s);
              prev->impl.next = s->impl.next;
            }else{
              assert(s == db->impl.stCache.head);
              db->impl.stCache.head = s->impl.next;
            }
            s->impl.next = 0;
            break;
          }
        }
      }
    }
    fsl_buffer_clear(&stmt->sql);
    if(stmt->stmt){
      sqlite3_finalize( stmt->stmt );
    }
    *stmt = fsl_stmt_empty;
    if(&fsl_stmt_empty==allocStamp){
      fsl_free(stmt);
    }else{
      stmt->impl.allocStamp = allocStamp;
    }
    return 0;
  }
}

int fsl__db_cached_clear_role(fsl_db * const db, fsl_flag32_t role){
  int rc = 0;
  fsl_stmt * s;
  fsl_stmt * prev = 0;
  fsl_stmt * next = 0;
  for( s = db->impl.stCache.head; s; s = next ){
    next = s->impl.next;
    if(0!=role && 0==(s->role & role)){
      prev = s;
      continue;
    }
    else if(FSL__STMT_F_CACHE_HELD & s->impl.flags){
      rc = fsl_error_set(&db->error, FSL_RC_MISUSE,
                         "Cannot clear cached SQL statement "
                         "for role #%d because it is currently "
                         "being held by a call to "
                         "fsl_db_preparev_cached(). SQL=%B",
                         &s->sql);
      break;
    }
    //MARKER(("Closing cached stmt: %s\n", fsl_buffer_cstr(&s->sql)));
    if(prev){
      prev->impl.next = next;
    }else if(s==db->impl.stCache.head){
      db->impl.stCache.head = next;
    }
    s->impl.next = 0;
    s->impl.flags = 0;
    s->role = FSL_DBROLE_NONE;
    fsl_stmt_finalize(s);
    break;
  }
  return rc;
}

int fsl_stmt_step( fsl_stmt * const stmt ){
  if(!stmt->stmt) return FSL_RC_MISUSE;
  else{
    int const rc = sqlite3_step(stmt->stmt);
    assert(stmt->db);
    switch( rc ){
      case SQLITE_ROW:
        ++stmt->rowCount;
        return FSL_RC_STEP_ROW;
      case SQLITE_DONE:
        return FSL_RC_STEP_DONE;
      default:
        return fsl__db_errcode(stmt->db, rc);
    }
  }
}

int fsl_db_eachv( fsl_db * const db, fsl_stmt_each_f callback,
                  void * callbackState, char const * sql, va_list args ){
  if(!db->dbh || !callback || !sql) return FSL_RC_MISUSE;
  else if(!*sql) return FSL_RC_RANGE;
  else{
    fsl_stmt st = fsl_stmt_empty;
    int rc;
    rc = fsl_db_preparev( db, &st, sql, args );
    if(!rc){
      rc = fsl_stmt_each( &st, callback, callbackState );
      fsl_stmt_finalize( &st );
    }
    return rc;
  }
}

int fsl_db_each( fsl_db * const db, fsl_stmt_each_f callback,
                 void * callbackState, char const * sql, ... ){
  int rc;
  va_list args;
  va_start(args,sql);
  rc = fsl_db_eachv( db, callback, callbackState, sql, args );
  va_end(args);
  return rc;
}

int fsl_stmt_each( fsl_stmt * const stmt, fsl_stmt_each_f callback,
                   void * callbackState ){
  if(!callback) return FSL_RC_MISUSE;
  else{
    int strc;
    int rc = 0;
    bool doBreak = false;
    while( !doBreak && (FSL_RC_STEP_ROW == (strc=fsl_stmt_step(stmt)))){
      rc = callback( stmt, callbackState );
      switch(rc){
        case 0: continue;
        case FSL_RC_BREAK:
          rc = 0;
          /* fall through */
        default:
          doBreak = true;
          break;
      }
    }
    return rc
      ? rc
      : ((FSL_RC_STEP_ERROR==strc)
         ? FSL_RC_DB
         : 0);
  }
}

int fsl_stmt_reset2( fsl_stmt * const stmt, bool resetRowCounter ){
  if(!stmt->stmt || !stmt->db) return FSL_RC_MISUSE;
  else{
    int const rc = sqlite3_reset(stmt->stmt);
    if(resetRowCounter) stmt->rowCount = 0;
    assert(stmt->db);
    return rc
      ? fsl__db_errcode(stmt->db, rc)
      : 0;
  }
}

int fsl_stmt_reset( fsl_stmt * const stmt ){
  return fsl_stmt_reset2(stmt, 0);
}

void fsl_stmt_clear_bindings( fsl_stmt * stmt ){
  if( stmt->stmt ){
    sqlite3_clear_bindings(stmt->stmt);
  }
}

int fsl_stmt_col_count( fsl_stmt const * const stmt ){
  return (!stmt || !stmt->stmt)
    ? -1
    : sqlite3_column_count(stmt->stmt);
}

char const * fsl_stmt_col_name(fsl_stmt * const stmt, int index){
  return (stmt && stmt->stmt
          && (index>=0 && index<sqlite3_column_count(stmt->stmt)))
    ? sqlite3_column_name(stmt->stmt, index)
    : NULL;
}

int fsl_stmt_param_count( fsl_stmt const * const stmt ){
  return (!stmt || !stmt->stmt)
    ? -1
    : sqlite3_bind_parameter_count(stmt->stmt);
}

int fsl_stmt_bind_fmtv( fsl_stmt * st, char const * fmt, va_list args ){
  int rc = 0, ndx;
  char const * pos = fmt;
  if(!fmt ||
     !(st && st->stmt && st->db && st->db->dbh)) return FSL_RC_MISUSE;
  else if(!*fmt) return FSL_RC_RANGE;
  int const nParam = fsl_stmt_param_count(st);
  assert(nParam>=0);
  for( ndx = 1; !rc && *pos; ++pos, ++ndx ){
    if(' '==*pos){
      --ndx;
      continue;
    }
    if(ndx > nParam){
      rc = fsl_error_set(&st->db->error, FSL_RC_RANGE,
                         "Column index %d is out of bounds.", ndx);
      break;
    }
#define checkMakeCopy bool const makeCopy = ('^'==pos[1] ? (++pos, true) : false)
    switch(*pos){
      case '-':
        (void)va_arg(args,void const *) /* skip arg */;
        rc = fsl_stmt_bind_null(st, ndx);
        break;
      case 'i':
        rc = fsl_stmt_bind_int32(st, ndx, va_arg(args,int32_t));
        break;
      case 'I':
        rc = fsl_stmt_bind_int64(st, ndx, va_arg(args,int64_t));
        break;
      case 'R':
        rc = fsl_stmt_bind_id(st, ndx, va_arg(args,fsl_id_t));
        break;
      case 'f':
        rc = fsl_stmt_bind_double(st, ndx, va_arg(args,double));
        break;
      case 's':{/* C-string as TEXT or NULL */
        checkMakeCopy;
        char const * s = va_arg(args,char const *);
        rc = s
          ? fsl_stmt_bind_text(st, ndx, s, -1, makeCopy)
          : fsl_stmt_bind_null(st, ndx);
        break;
      }
      case 'S':{ /* C-string as BLOB or NULL */
        checkMakeCopy;
        char const * s = va_arg(args,char const *);
        rc = s
          ? fsl_stmt_bind_blob(st, ndx, s, fsl_strlen(s), makeCopy)
          : fsl_stmt_bind_null(st, ndx);
        break;
      }
      case 'b':{ /* fsl_buffer as TEXT or NULL */
        checkMakeCopy;
        fsl_buffer const * b = va_arg(args,fsl_buffer const *);
        rc = (b && b->used)
          ? fsl_stmt_bind_text(st, ndx, (char const *)b->mem,
                               (fsl_int_t)b->used, makeCopy)
          : fsl_stmt_bind_null(st, ndx);
        break;
      }
      case 'B':{ /* fsl_buffer as BLOB or NULL */
        checkMakeCopy;
        fsl_buffer const * b = va_arg(args,fsl_buffer const *);
        rc = (b && b->used)
          ? fsl_stmt_bind_blob(st, ndx, b->mem, b->used, makeCopy)
          : fsl_stmt_bind_null(st, ndx);
        break;
      }
      default:
        rc = fsl_error_set(&st->db->error, FSL_RC_RANGE,
                           "Invalid format character: '%c'", *pos);
        break;
    }
  }
#undef checkMakeCopy
  return rc;
}

/**
   The elipsis counterpart of fsl_stmt_bind_fmtv().
*/
int fsl_stmt_bind_fmt( fsl_stmt * const st, char const * fmt, ... ){
  int rc;
  va_list args;
  va_start(args,fmt);
  rc = fsl_stmt_bind_fmtv(st, fmt, args);
  va_end(args);
  return rc;
}

int fsl_stmt_bind_stepv_v2( fsl_stmt * const st, char const * fmt,
                         va_list args ){
  int rc;
  fsl_stmt_reset(st);
  rc = fsl_stmt_bind_fmtv(st, fmt, args);
  if(!rc){
    rc = fsl_stmt_step(st);
    switch(rc){
      case FSL_RC_STEP_DONE: break;
      case FSL_RC_STEP_ROW: return rc
        /* Don't reset() for ROW b/c that clears the column data */;
      default:
        rc = fsl_error_set(&st->db->error, rc,
                           "Error stepping statement: %s",
                           sqlite3_errmsg(st->db->dbh));
        break;
    }
  }
  fsl_stmt_reset(st);
  return rc;
}

int fsl_stmt_bind_step_v2( fsl_stmt * st, char const * fmt, ... ){
  int rc;
  va_list args;
  va_start(args,fmt);
  rc = fsl_stmt_bind_stepv_v2(st, fmt, args);
  va_end(args);
  return rc;
}

int fsl_stmt_bind_stepv( fsl_stmt * const st, char const * fmt,
                         va_list args ){
  int const rc = fsl_stmt_bind_stepv_v2(st, fmt, args);
  return (FSL_RC_STEP_DONE==rc) ? 0 : rc;
}

int fsl_stmt_bind_step( fsl_stmt * st, char const * fmt, ... ){
  int rc;
  va_list args;
  va_start(args,fmt);
  rc = fsl_stmt_bind_stepv_v2(st, fmt, args);
  va_end(args);
  return (FSL_RC_STEP_DONE==rc) ? 0 : rc;
}


#define BIND_PARAM_CHECK \
  int const nParam = fsl_stmt_param_count(stmt); \
  if(nParam<0) return FSL_RC_MISUSE; else
#define BIND_PARAM_CHECK2 BIND_PARAM_CHECK \
  if(ndx<1 || ndx>nParam) return FSL_RC_RANGE; else

int fsl_stmt_bind_null( fsl_stmt * const stmt, int ndx ){
  BIND_PARAM_CHECK2 {
    int const rc = sqlite3_bind_null( stmt->stmt, ndx );
    return rc ? fsl__db_errcode(stmt->db, rc) : 0;
  }
}

int fsl_stmt_bind_int32( fsl_stmt * const stmt, int ndx, int32_t v ){
  BIND_PARAM_CHECK2 {
    int const rc = sqlite3_bind_int( stmt->stmt, ndx, (int)v );
    return rc ? fsl__db_errcode(stmt->db, rc) : 0;
  }
}

int fsl_stmt_bind_int64( fsl_stmt * const stmt, int ndx, int64_t v ){
  BIND_PARAM_CHECK2 {
    int const rc = sqlite3_bind_int64( stmt->stmt, ndx, (sqlite3_int64)v );
    return rc ? fsl__db_errcode(stmt->db, rc) : 0;
  }
}

int fsl_stmt_bind_id( fsl_stmt * const stmt, int ndx, fsl_id_t v ){
  BIND_PARAM_CHECK2 {
    int const rc = sqlite3_bind_int64( stmt->stmt, ndx, (sqlite3_int64)v );
    return rc ? fsl__db_errcode(stmt->db, rc) : 0;
  }
}

int fsl_stmt_bind_double( fsl_stmt * const stmt, int ndx, double v ){
  BIND_PARAM_CHECK2 {
    int const rc = sqlite3_bind_double( stmt->stmt, ndx, (double)v );
    return rc ? fsl__db_errcode(stmt->db, rc) : 0;
  }
}

int fsl_stmt_bind_blob( fsl_stmt * const stmt, int ndx, void const * src,
                        fsl_size_t len, bool makeCopy ){
  BIND_PARAM_CHECK2 {
    int rc;
    rc = sqlite3_bind_blob( stmt->stmt, ndx, src, (int)len,
                            makeCopy ? SQLITE_TRANSIENT : SQLITE_STATIC );
    return rc ? fsl__db_errcode(stmt->db, rc) : 0;
  }
}

int fsl_stmt_bind_text( fsl_stmt * const stmt, int ndx, char const * src,
                        fsl_int_t len, bool makeCopy ){
  BIND_PARAM_CHECK {
    int rc;
    if(len<0) len = fsl_strlen((char const *)src);
    rc = sqlite3_bind_text( stmt->stmt, ndx, src, len,
                            makeCopy ? SQLITE_TRANSIENT : SQLITE_STATIC );
    return rc ? fsl__db_errcode(stmt->db, rc) : 0;
  }
}

int fsl_stmt_bind_null_name( fsl_stmt * const stmt, char const * param ){
  BIND_PARAM_CHECK {
    return fsl_stmt_bind_null( stmt,
                               sqlite3_bind_parameter_index( stmt->stmt,
                                                             param) );
  }
}

int fsl_stmt_bind_int32_name( fsl_stmt * const stmt, char const * param, int32_t v ){
  BIND_PARAM_CHECK {
    return fsl_stmt_bind_int32( stmt,
                                sqlite3_bind_parameter_index( stmt->stmt,
                                                              param),
                                v);
  }
}

int fsl_stmt_bind_int64_name( fsl_stmt * const stmt, char const * param, int64_t v ){
  BIND_PARAM_CHECK {
    return fsl_stmt_bind_int64( stmt,
                                sqlite3_bind_parameter_index( stmt->stmt,
                                                              param),
                                v);
  }
}

int fsl_stmt_bind_id_name( fsl_stmt * const stmt, char const * param, fsl_id_t v ){
  BIND_PARAM_CHECK {
    return fsl_stmt_bind_id( stmt,
                             sqlite3_bind_parameter_index( stmt->stmt,
                                                           param),
                             v);
  }
}

int fsl_stmt_bind_double_name( fsl_stmt * const stmt, char const * param, double v ){
  BIND_PARAM_CHECK {
    return fsl_stmt_bind_double( stmt,
                                 sqlite3_bind_parameter_index( stmt->stmt,
                                                               param),
                                 v);
  }
}

int fsl_stmt_bind_text_name( fsl_stmt * const stmt, char const * param,
                             char const * v, fsl_int_t n,
                             bool makeCopy ){
  BIND_PARAM_CHECK {
    return fsl_stmt_bind_text(stmt,
                              sqlite3_bind_parameter_index( stmt->stmt,
                                                            param),
                              v, n, makeCopy);
  }
}

int fsl_stmt_bind_blob_name( fsl_stmt * const stmt, char const * param,
                             void const * v, fsl_int_t len,
                             bool makeCopy ){
  BIND_PARAM_CHECK {
    return fsl_stmt_bind_blob(stmt,
                         sqlite3_bind_parameter_index( stmt->stmt,
                                                       param),
                              v, len, makeCopy);
  }
}

int fsl_stmt_param_index( fsl_stmt * const stmt, char const * const param){
  return (stmt && stmt->stmt)
    ? sqlite3_bind_parameter_index( stmt->stmt, param)
    : -1;
}

#undef BIND_PARAM_CHECK
#undef BIND_PARAM_CHECK2

#define GET_CHECK                                                \
  int const colCount = sqlite3_column_count(stmt->stmt);         \
  if( !colCount ) return FSL_RC_MISUSE;                          \
  else if((ndx<0) || (ndx>=colCount)) return FSL_RC_RANGE; else

int fsl_stmt_get_int32( fsl_stmt * const stmt, int ndx, int32_t * v ){
  GET_CHECK {
    if(v) *v = (int32_t)sqlite3_column_int(stmt->stmt, ndx);
    return 0;
  }
}
int fsl_stmt_get_int64( fsl_stmt * const stmt, int ndx, int64_t * v ){
  GET_CHECK {
    if(v) *v = (int64_t)sqlite3_column_int64(stmt->stmt, ndx);
    return 0;
  }
}

int fsl_stmt_get_double( fsl_stmt * const stmt, int ndx, double * v ){
  GET_CHECK {
    if(v) *v = (double)sqlite3_column_double(stmt->stmt, ndx);
    return 0;
  }
}

int fsl_stmt_get_id( fsl_stmt * const stmt, int ndx, fsl_id_t * v ){
  GET_CHECK {
    if(v) *v = (4==sizeof(fsl_id_t))
      ? (fsl_id_t)sqlite3_column_int(stmt->stmt, ndx)
      : (fsl_id_t)sqlite3_column_int64(stmt->stmt, ndx);
    return 0;
  }
}

int fsl_stmt_get_text( fsl_stmt * const stmt, int ndx, char const **out,
                       fsl_size_t * outLen ){
  GET_CHECK {
    unsigned char const * t = (out || outLen)
      ? sqlite3_column_text(stmt->stmt, ndx)
      : NULL;
    if(out) *out = (char const *)t;
    if(outLen){
      int const x = sqlite3_column_bytes(stmt->stmt, ndx);
      *outLen = (x>0) ? (fsl_size_t)x : 0;
    }
    return t ? 0 : fsl__db_errcode(stmt->db, 0);
  }
}

int fsl_stmt_get_blob( fsl_stmt * const stmt, int ndx, void const **out,
                       fsl_size_t * outLen ){
  GET_CHECK {
    void const * t = (out || outLen)
      ? sqlite3_column_blob(stmt->stmt, ndx)
      : NULL;
    if(out) *out = t;
    if(outLen){
      if(!t) *outLen = 0;
      else{
        int sz = sqlite3_column_bytes(stmt->stmt, ndx);
        *outLen = (sz>=0) ? (fsl_size_t)sz : 0;
      }
    }
    return t ? 0 : fsl__db_errcode(stmt->db, 0);
  }
}

#undef GET_CHECK

fsl_id_t fsl_stmt_g_id( fsl_stmt * const stmt, int index ){
  fsl_id_t rv = -1;
  fsl_stmt_get_id(stmt, index, &rv);
  return rv;
}
int32_t fsl_stmt_g_int32( fsl_stmt * const stmt, int index ){
  int32_t rv = 0;
  fsl_stmt_get_int32(stmt, index, &rv);
  return rv;
}
int64_t fsl_stmt_g_int64( fsl_stmt * const stmt, int index ){
  int64_t rv = 0;
  fsl_stmt_get_int64(stmt, index, &rv);
  return rv;
}
double fsl_stmt_g_double( fsl_stmt * const stmt, int index ){
  double rv = 0;
  fsl_stmt_get_double(stmt, index, &rv);
  return rv;
}

char const * fsl_stmt_g_text( fsl_stmt * const stmt, int index,
                              fsl_size_t * outLen ){
  char const * rv = NULL;
  fsl_stmt_get_text(stmt, index, &rv, outLen);
  return rv;
}


/**
   This sqlite3_trace_v2() callback outputs tracing info using
   fprintf() or fsl__db_fire().
*/
static int fsl__db_sq3TraceV2(unsigned t,void*c,void*p,void*x){
  static unsigned int counter = 0;
  switch(t){
    case SQLITE_TRACE_STMT:{
      fsl_db * const db = c;
      assert( !(db->impl.event.maskIds & ~FSL_DB_EVENT_mask_id)
              /* Else internal mask mismanipulation */ );
      if( db->impl.event.maskIds &
          (FSL_DB_EVENT_TRACE_SQL | FSL_DB_EVENT_TRACE_SQLX) ){
        fsl_buffer * const b = fsl_buffer_reuse(&db->impl.buffer);
        char const * zSql = (char const *)x;
        bool const isExp = ((FSL_DB_EVENT_TRACE_SQLX
                             & FSL_DB_EVENT_mask_id)
                            & db->impl.event.maskIds);
        char * zExp = isExp
          ? sqlite3_expanded_sql((sqlite3_stmt*)p)
            : 0;
        //MARKER(("mask=%08x isExp=%d\n", db->impl.event.maskIds, isExp));
        fsl_buffer_appendf(b, "SQL TRACE #%u: %s\n",
                           ++counter,
                           zExp ? zExp : zSql);
        sqlite3_free(zExp);
        if( 0==b->errCode ){
          if( db->impl.event.fpTrace ){
            fwrite(b->mem, b->used, 1, db->impl.event.fpTrace);
            //fflush(db->impl.event.fpTrace);
          }else{
            fsl__db_fire(db, isExp
                         ? FSL_DB_EVENT_TRACE_SQLX
                         : FSL_DB_EVENT_TRACE_SQL, b);
          }
        }
      }
      break;
    }
  }
  return 0;
}

fsl_db * fsl_db_malloc(void){
  fsl_db * const rc = (fsl_db *)fsl_malloc(sizeof(fsl_db));
  if(rc){
    *rc = fsl_db_empty;
    rc->impl.allocStamp = &fsl_db_empty;
  }
  return rc;
}

fsl_stmt * fsl_stmt_malloc(void){
  fsl_stmt * const rc = (fsl_stmt *)fsl_malloc(sizeof(fsl_stmt));
  if(rc){
    *rc = fsl_stmt_empty;
    rc->impl.allocStamp = &fsl_stmt_empty;
  }
  return rc;
}

/**
   Callback for use with sqlite3_commit_hook(). The argument must be a
   (fsl_db*). This function returns 0 only if it surmises that
   fsl_db_txn_end() triggered the COMMIT. On error it might
   assert() or abort() the application, so this really is just a
   sanity check for something which "must not happen."

   2025-08-11: TIL that this hook is only called if something actually
   changes. It won't trigger for read-only transactions.
*/
static int fsl__db_verify_begin_was_not_called(void * db_fsl){
  fsl_db * const db = (fsl_db *)db_fsl;
  assert(db && "What else could it be?");
  assert(db->dbh && "Else we can't have been called by sqlite3");
  //MARKER(("level=%d flags=0x%04x\n", db->impl.txn.level, db->flags));
  if( !db->impl.txn.level ){
    /* COMMIT is okay when we're not managing transaction levels */
    return 0;
  }
  if(FSL__DB_F_IN_COMMIT != (FSL__DB_F_IN_COMMIT & db->impl.flags)){
    /* It turns out that we'll end up losing this message when this
       error code bubbles up and we get a more generic message from
       sqlite. */
#if 1
#  define doit(RC,MSG) fsl__fatal(RC,MSG)
#else
#  define doit(RC,MSG) fsl_error_set(&db->error,RC,MSG)
#endif
    doit(FSL_RC_MISUSE,"SQL: COMMIT was called from "
         "outside of fsl_db_txn_end_v2() while a "
         "fsl_db_txn_begin()-started transaction "
         "is pending. Don't do that.");
#undef doit
    fsl__db_err_dump(db);
    return 2;
  }
  return 0;
}

int fsl_db_open( fsl_db * const db, char const * dbFile,
                 fsl_flag32_t openFlags ){
  int rc;
  fsl_db_t * dbh = NULL;
  if(!db || !dbFile) return FSL_RC_MISUSE;
  else if(db->dbh) return FSL_RC_MISUSE;
  else if(!(FSL_DB_OPEN_CREATE & openFlags)
          &&  0  != *dbFile /* temp db */
          && ':' != *dbFile /*assume :memory:*/
#if !FSL_PLATFORM_IS_WINDOWS
          && (NULL!=strchr(&dbFile[1], ':')/*assume URL*/)
#else
          && (':'==dbFile[1] /* maybe drive letter */
              || NULL==strchr(&dbFile[2], ':'/*assume URL*/))
#endif
          && fsl_file_access(dbFile, 0)){
    return fsl_error_set(&db->error, FSL_RC_NOT_FOUND,
                         "DB file not found: %s", dbFile);
  }else{
    int sOpenFlags = 0;
    if(FSL_DB_OPEN_RO & openFlags){
      sOpenFlags |= SQLITE_OPEN_READONLY;
    }else{
      if(FSL_DB_OPEN_RW & openFlags){
        sOpenFlags |= SQLITE_OPEN_READWRITE;
      }
      if(FSL_DB_OPEN_CREATE & openFlags){
        sOpenFlags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
      }
      if(!sOpenFlags) sOpenFlags = SQLITE_OPEN_READONLY;
    }
    rc = sqlite3_open_v2( dbFile, &dbh, sOpenFlags, NULL );
    if(rc){
      if(dbh){
        /* By some complete coincidence, FSL_RC_DB==SQLITE_CANTOPEN. */
        rc = fsl_error_set(&db->error, FSL_RC_DB,
                           "Opening db file [%s] failed with "
                           "sqlite code #%d: %s",
                           dbFile, rc, sqlite3_errmsg(dbh));
      }else{
        rc = fsl_error_set(&db->error, FSL_RC_DB,
                           "Opening db file [%s] failed with "
                           "sqlite code #%d",
                           dbFile, rc);
      }
      /* MARKER(("Error msg: %s\n", (char const *)db->error.msg.mem)); */
      goto end;
    }else{
      assert(!db->filename);
      if(!*dbFile || ':'==*dbFile){
        /* assume "" or ":memory:" or some such: don't canonicalize it,
           but copy it nonetheless for consistency. */
        db->filename = fsl_strdup(dbFile);
      }else{
        fsl_buffer tmp = fsl_buffer_empty;
        rc = fsl_file_canonical_name(dbFile, &tmp, 0);
        if(!rc){
          db->filename = (char *)tmp.mem
            /* transfering ownership */;
        }else if(tmp.mem){
          fsl_buffer_clear(&tmp);
        }
      }
      if(rc){
        goto end;
      }else if(!db->filename){
        rc = FSL_RC_OOM;
        goto end;
      }
    }
    db->dbh = dbh;
    sqlite3_extended_result_codes(dbh, 1);
    sqlite3_commit_hook(dbh, fsl__db_verify_begin_was_not_called, db);
    if(FSL_DB_OPEN_TRACE_SQL & openFlags){
      fsl_db_sqltrace_enable(db, stdout, false);
    }
  }
  end:
  if(rc){
    if(dbh){
      sqlite3_close(dbh);
      db->dbh = NULL;
    }
  }else{
    assert(db->dbh);
  }
  return rc;
}


static int fsl__db_err_not_opened(fsl_db * const db){
  return fsl_error_set(&db->error, FSL_RC_MISUSE,
                       "DB is not opened.");
}
static int fsl__db_err_sql_empty(fsl_db * const db){
  return fsl_error_set(&db->error, FSL_RC_MISUSE,
                       "Empty SQL is not permitted.");
 }

int fsl_db_exec_multiv( fsl_db * const db, const char * sql, va_list args){
  if(!db->dbh) return fsl__db_err_not_opened(db);
  else if(!sql || !*sql) return fsl__db_err_sql_empty(db);
  else{
    fsl_buffer buf = fsl_buffer_empty;
    int rc = 0;
    char const * z;
    char const * zEnd = NULL;
    rc = fsl_buffer_appendfv( &buf, sql, args );
    if(rc){
      fsl_buffer_clear(&buf);
      return rc;
    }
    z = fsl_buffer_cstr(&buf);
    while( (SQLITE_OK==rc) && *z ){
      fsl_stmt_t * pStmt = NULL;
      rc = sqlite3_prepare_v2(db->dbh, z, buf.used, &pStmt, &zEnd);
      if( SQLITE_OK != rc ){
        rc = fsl__db_errcode(db, rc);
        break;
      }
      if(pStmt){
        while( SQLITE_ROW == sqlite3_step(pStmt) ){}
        rc = sqlite3_finalize(pStmt);
        if(rc) rc = fsl__db_errcode(db, rc);
      }
      buf.used -= (zEnd-z);
      z = zEnd;
    }
    fsl_buffer_reserve(&buf, 0);
    return rc;
  }
}

int fsl_db_exec_multi( fsl_db * const db, const char * sql, ...){
  if(!db->dbh) return fsl__db_err_not_opened(db);
  else if(!sql || !*sql) return fsl__db_err_sql_empty(db);
  else{
    int rc;
    va_list args;
    va_start(args,sql);
    rc = fsl_db_exec_multiv( db, sql, args );
    va_end(args);
    return rc;
  }
}

int fsl_db_execv( fsl_db * const db, const char * sql, va_list args){
  if(!db->dbh) return fsl__db_err_not_opened(db);
  else if(!sql || !*sql) return fsl__db_err_sql_empty(db);
  else{
    fsl_stmt st = fsl_stmt_empty;
    int rc = 0;
    rc = fsl_db_preparev( db, &st, sql, args );
    if(0==rc){
      //while(FSL_RC_STEP_ROW == (rc=fsl_stmt_step(&st))){}
      //^^^ why did we historically do this instead of:
      rc = fsl_stmt_step( &st );
      fsl_stmt_finalize(&st);
    }
    switch(rc){
      case FSL_RC_STEP_DONE:
      case FSL_RC_STEP_ROW: rc = 0; break;
    }
    return rc;
  }
}

int fsl_db_exec( fsl_db * const db, const char * sql, ...){
  if(!db->dbh) return fsl__db_err_not_opened(db);
  else if(!sql || !*sql) return fsl__db_err_sql_empty(db);
  else{
    int rc;
    va_list args;
    va_start(args,sql);
    rc = fsl_db_execv( db, sql, args );
    va_end(args);
    return rc;
  }
}

int fsl_db_changes_recent(fsl_db * const db){
  return db->dbh
    ? sqlite3_changes(db->dbh)
    : 0;
}

int fsl_db_changes_total(fsl_db * const db){
  return db->dbh
    ? sqlite3_total_changes(db->dbh)
    : 0;
}

static int fsl__db_step_sp(fsl_db * const db, sqlite3_stmt *q){
  fsl_flag32_t const oldFlags = db->impl.flags;
  db->impl.flags |= FSL__DB_F_IN_COMMIT;
  int const rc = sqlite3_step(q);
  sqlite3_reset(q);
  db->impl.flags = oldFlags;
  return SQLITE_DONE==rc
    ? 0
    : fsl__db_errcode(db, rc);
}

static int fsl__db_step_rollback(fsl_db * const db){
  int rc = fsl__db_step_sp(db, db->impl.txn.sRollback);
  if( 0==rc ){
    rc = fsl__db_step_sp(db, db->impl.txn.sRelease);
  }
  return rc;
}

int fsl_db_txn_begin(fsl_db * const db){
  if( !db || !db->dbh ) return FSL_RC_MISUSE;
#if 0
  /* 2025-08-06: this really should have been part of the db's
     behavior all along, and activating it now breaks tests. Currently
     those tests are running through certain APIs with this flag set
     and not triggering errors. That's not _entirely_ wrong because
     this flag means they'll get rolled back anyway, but we shouldn't
     be in those routines when this flag is set in the first place.

     TODO: enable this bit and sort out the carnage (mostly, perhaps
     entirely, in f-sanity.c).
  */
  else if( db->impl.flags & FSL__DB_F_BUBBLING_ROLLBACK ){
    return fsl_error_set(&db->error, FSL_RC_DB,
                         "Cowardly refusing to push a new transaction level "
                         "when a rollback is pending. "
                         "Current transaction level=%d",
                         db->impl.txn.level);
  }
#endif
  else {
    int rc = 0;
    if( !db->impl.txn.sSavepoint ){
#define P(MEMBER,SQL)                                         \
      if( 0==rc ) {                                           \
        rc = sqlite3_prepare_v3(db->dbh, SQL, -1,             \
                                SQLITE_PREPARE_PERSISTENT,    \
                                &db->impl.txn.MEMBER, NULL ); \
      }
#define FSL__DB_SP "SAVEPOINT fsl_db"
      /* __func__ is not a string literal */
      P(sSavepoint, FSL__DB_SP " /*" __FILE__ "*/");
      P(sRelease, "RELEASE " FSL__DB_SP " /*" __FILE__ "()*/");
      P(sRollback, "ROLLBACK TO " FSL__DB_SP " /*" __FILE__ "()*/")
#undef FSL__DB_SP
#undef P
      if( rc ){
        fsl__db_finalize_sp(db);
        return fsl__db_errcode(db, rc);
      }
    }
    rc = fsl__db_step_sp(db, db->impl.txn.sSavepoint);
    if( 0==rc ){
      fsl_error_reset(&db->error);
      if( db->impl.event.f ){
        rc = fsl__db_fire(db, FSL_DB_EVENT_BEGIN, NULL);
        if( rc ){
          fsl__db_step_rollback(db);
        }
      }
      if( 0==rc ){
        ++db->impl.txn.level;
      }
    }
    return rc;
  }
}

/**
   End the current savepoint level. If keepSavepoint is true, it is
   RELEASEd, else it is ROLLBACKed and RELEASEd. Asserts that
   db->impl.txn.level>0 and adjusts db->impl.txn.level when it's done.
*/
static int fsl__db_txn_sp_end(fsl_db * const db, bool keepSavepoint){
  int rc, rc2;
  fsl_flag32_t const oldFlags = db->impl.flags;
  assert( db->impl.txn.level>0 );
  db->impl.flags |= FSL__DB_F_IN_COMMIT;
  if( keepSavepoint ){
    rc2 = fsl__db_fire(db, FSL_DB_EVENT_COMMIT, NULL);
    if( 0==rc2 ){
      rc = fsl__db_step_sp(db, db->impl.txn.sRelease);
    }else{
      rc = fsl__db_step_rollback(db);
      if( 0==rc ) rc = rc2;
    }
  }else{
    rc2 = fsl__db_fire(db, FSL_DB_EVENT_ROLLING_BACK, NULL);
    rc = fsl__db_step_rollback(db);
    if( !rc ) rc = rc2;
    rc2 = fsl__db_fire(db, FSL_DB_EVENT_ROLLED_BACK, NULL);
    if( !rc ) rc = rc2;
  }
  db->impl.flags = oldFlags;
  if( 0==--db->impl.txn.level ){
    db->impl.flags &= ~FSL__DB_F_BUBBLING_ROLLBACK;
  }
  return rc;
}

int fsl_db_txn_level(fsl_db * const db){
  return (db->impl.flags & FSL__DB_F_BUBBLING_ROLLBACK)
    ? -db->impl.txn.level : db->impl.txn.level;
}

int fsl_db_txn_commit(fsl_db * const db){
  return db->dbh
    ? fsl_db_txn_end_v2(db, true, false)
    : FSL_RC_MISUSE;
}

int fsl_db_txn_rollback(fsl_db * const db){
  return db->dbh
    ? fsl_db_txn_end_v2(db, false, true/*historical behavior*/)
    : FSL_RC_MISUSE;
}

#if 0
int fsl_db_rollback_force( fsl_db * const db ){
  if(!db->dbh){
    return fsl__db_err_not_opened(db);
  }else{
    db->impl.flags |= FSL__DB_F_BUBBLING_ROLLBACK;
    while( db->impl.txn.level>0 ){
      fsl__db_txn_sp_end(db, false);
      --db->impl.txn.level;
    }
    db->impl.flags &= ~FSL__DB_F_BUBBLING_ROLLBACK;
    return 0;
  }
}
#endif

int fsl_db_txn_end_v2(fsl_db * const db, bool keepSavepoint,
                      bool bubbleRollback){
  if(!db->dbh){
    return fsl__db_err_not_opened(db);
  }else if (db->impl.txn.level<=0){
    return fsl_error_set(&db->error, FSL_RC_RANGE,
                         "No transaction is active.");
  }
  if(bubbleRollback){
    db->impl.flags |= FSL__DB_F_BUBBLING_ROLLBACK
      /* ACHTUNG: set before continuing so that if we return due to a
         non-0 txn.level that the rollback flag propagates through
         the transaction's stack. */;
  }
  return fsl__db_txn_sp_end(
    db, db->impl.txn.level>1
    ? keepSavepoint
    : (keepSavepoint &&
       !(db->impl.flags & FSL__DB_F_BUBBLING_ROLLBACK))
  );
}

int fsl_db_get_int32v( fsl_db * const db, int32_t * rv,
                       char const * sql, va_list args){
  /* Potential fixme: the fsl_db_get_XXX() funcs are 95%
     code duplicates. We "could" replace these with a macro
     or supermacro, though the latter would be problematic
     in the context of an amalgamation build.
  */
  if(!db || !db->dbh || !rv || !sql || !*sql) return FSL_RC_MISUSE;
  else{
    fsl_stmt st = fsl_stmt_empty;
    int rc = 0;
    rc = fsl_db_preparev( db, &st, sql, args );
    if(rc) return rc;
    rc = fsl_stmt_step( &st );
    switch(rc){
      case FSL_RC_STEP_ROW:
        *rv = sqlite3_column_int(st.stmt, 0);
        /* Fall through */
      case FSL_RC_STEP_DONE:
        rc = 0;
        break;
      default:
        assert(FSL_RC_STEP_ERROR==rc);
        break;
    }
    fsl_stmt_finalize(&st);
    return rc;
  }
}

int fsl_db_get_int32( fsl_db * const db, int32_t * rv,
                      char const * sql, ... ){
  int rc;
  va_list args;
  va_start(args,sql);
  rc = fsl_db_get_int32v(db, rv, sql, args);
  va_end(args);
  return rc;
}

int fsl_db_get_int64v( fsl_db * const db, int64_t * rv,
                       char const * sql, va_list args){
  if(!db || !db->dbh || !rv || !sql || !*sql) return FSL_RC_MISUSE;
  else{
    fsl_stmt st = fsl_stmt_empty;
    int rc = 0;
    rc = fsl_db_preparev( db, &st, sql, args );
    if(rc) return rc;
    rc = fsl_stmt_step( &st );
    switch(rc){
      case FSL_RC_STEP_ROW:
        *rv = sqlite3_column_int64(st.stmt, 0);
        /* Fall through */
      case FSL_RC_STEP_DONE:
        rc = 0;
        break;
      default:
        assert(FSL_RC_STEP_ERROR==rc);
        break;
    }
    fsl_stmt_finalize(&st);
    return rc;
  }
}

int fsl_db_get_int64( fsl_db * const db, int64_t * rv,
                      char const * sql, ... ){
  int rc;
  va_list args;
  va_start(args,sql);
  rc = fsl_db_get_int64v(db, rv, sql, args);
  va_end(args);
  return rc;
}


int fsl_db_get_idv( fsl_db * const db, fsl_id_t * rv,
                       char const * sql, va_list args){
  if(!db || !db->dbh || !rv || !sql || !*sql) return FSL_RC_MISUSE;
  else{
    fsl_stmt st = fsl_stmt_empty;
    int rc = 0;
    rc = fsl_db_preparev( db, &st, sql, args );
    if(rc) return rc;
    rc = fsl_stmt_step( &st );
    switch(rc){
      case FSL_RC_STEP_ROW:
        *rv = (fsl_id_t)sqlite3_column_int64(st.stmt, 0);
        /* Fall through */
      case FSL_RC_STEP_DONE:
        rc = 0;
        break;
      default:
        break;
    }
    fsl_stmt_finalize(&st);
    return rc;
  }
}

int fsl_db_get_id( fsl_db * const db, fsl_id_t * rv,
                      char const * sql, ... ){
  int rc;
  va_list args;
  va_start(args,sql);
  rc = fsl_db_get_idv(db, rv, sql, args);
  va_end(args);
  return rc;
}


int fsl_db_get_sizev( fsl_db * const db, fsl_size_t * rv,
                      char const * sql, va_list args){
  if(!db || !db->dbh || !rv || !sql || !*sql) return FSL_RC_MISUSE;
  else{
    fsl_stmt st = fsl_stmt_empty;
    int rc = 0;
    rc = fsl_db_preparev( db, &st, sql, args );
    if(rc) return rc;
    rc = fsl_stmt_step( &st );
    switch(rc){
      case FSL_RC_STEP_ROW:{
        sqlite3_int64 const i = sqlite3_column_int64(st.stmt, 0);
        if(i<0){
          rc = FSL_RC_RANGE;
          break;
        }
        *rv = (fsl_size_t)i;
        rc = 0;
        break;
      }
      case FSL_RC_STEP_DONE:
        rc = 0;
        break;
      default:
        assert(FSL_RC_STEP_ERROR==rc);
        break;
    }
    fsl_stmt_finalize(&st);
    return rc;
  }
}

int fsl_db_get_size( fsl_db * const db, fsl_size_t * rv,
                      char const * sql, ... ){
  int rc;
  va_list args;
  va_start(args,sql);
  rc = fsl_db_get_sizev(db, rv, sql, args);
  va_end(args);
  return rc;
}


int fsl_db_get_doublev( fsl_db * const db, double * rv,
                       char const * sql, va_list args){
  if(!db || !db->dbh || !rv || !sql || !*sql) return FSL_RC_MISUSE;
  else{
    fsl_stmt st = fsl_stmt_empty;
    int rc = 0;
    rc = fsl_db_preparev( db, &st, sql, args );
    if(rc) return rc;
    rc = fsl_stmt_step( &st );
    switch(rc){
      case FSL_RC_STEP_ROW:
        *rv = sqlite3_column_double(st.stmt, 0);
        /* Fall through */
      case FSL_RC_STEP_DONE:
        rc = 0;
        break;
      default:
        assert(FSL_RC_STEP_ERROR==rc);
        break;
    }
    fsl_stmt_finalize(&st);
    return rc;
  }
}

int fsl_db_get_double( fsl_db * const db, double * rv,
                      char const * sql,
                      ... ){
  int rc;
  va_list args;
  va_start(args,sql);
  rc = fsl_db_get_doublev(db, rv, sql, args);
  va_end(args);
  return rc;
}


int fsl_db_get_textv( fsl_db * const db, char ** rv,
                      fsl_size_t *rvLen,
                      char const * sql, va_list args){
  if(!db || !db->dbh || !rv || !sql || !*sql) return FSL_RC_MISUSE;
  else{
    fsl_stmt st = fsl_stmt_empty;
    int rc = 0;
    rc = fsl_db_preparev( db, &st, sql, args );
    if(rc) return rc;
    rc = fsl_stmt_step( &st );
    switch(rc){
      case FSL_RC_STEP_ROW:{
        char const * str = (char const *)sqlite3_column_text(st.stmt, 0);
        int const len = sqlite3_column_bytes(st.stmt,0);
        if(!str){
          *rv = NULL;
          if(rvLen) *rvLen = 0;
        }else{
          char * x = fsl_strndup(str, len);
          if(!x){
            rc = FSL_RC_OOM;
          }else{
            *rv = x;
            if(rvLen) *rvLen = (fsl_size_t)len;
            rc = 0;
          }
        }
        break;
      }
      case FSL_RC_STEP_DONE:
        *rv = NULL;
        if(rvLen) *rvLen = 0;
        rc = 0;
        break;
      default:
        break;
    }
    fsl_stmt_finalize(&st);
    return rc;
  }
}

int fsl_db_get_text( fsl_db * const db, char ** rv,
                     fsl_size_t * rvLen,
                     char const * sql, ... ){
  int rc;
  va_list args;
  va_start(args,sql);
  rc = fsl_db_get_textv(db, rv, rvLen, sql, args);
  va_end(args);
  return rc;
}

int fsl_db_get_blobv( fsl_db * const db, void ** rv,
                      fsl_size_t *rvLen,
                      char const * sql, va_list args){
  if(!db || !db->dbh || !rv || !sql || !*sql) return FSL_RC_MISUSE;
  else{
    fsl_stmt st = fsl_stmt_empty;
    int rc = 0;
    rc = fsl_db_preparev( db, &st, sql, args );
    if(rc) return rc;
    rc = fsl_stmt_step( &st );
    switch(rc){
      case FSL_RC_STEP_ROW:{
        fsl_buffer buf = fsl_buffer_empty;
        void const * str = sqlite3_column_blob(st.stmt, 0);
        int const len = sqlite3_column_bytes(st.stmt,0);
        if(!str){
          *rv = NULL;
          if(rvLen) *rvLen = 0;
        }else{
          rc = fsl_buffer_append(&buf, str, len);
          if(!rc){
            *rv = buf.mem;
            if(rvLen) *rvLen = buf.used;
          }
        }
        break;
      }
      case FSL_RC_STEP_DONE:
        *rv = NULL;
        if(rvLen) *rvLen = 0;
        rc = 0;
        break;
      default:
        assert(FSL_RC_STEP_ERROR==rc);
        break;
    }
    fsl_stmt_finalize(&st);
    return rc;
  }
}

int fsl_db_get_blob( fsl_db * const db, void ** rv,
                     fsl_size_t * rvLen,
                     char const * sql, ... ){
  int rc;
  va_list args;
  va_start(args,sql);
  rc = fsl_db_get_blobv(db, rv, rvLen, sql, args);
  va_end(args);
  return rc;
}

int fsl_db_get_bufferv( fsl_db * const db, fsl_buffer * const b,
                        bool asBlob, char const * sql,
                        va_list args){
  if(!sql || !*sql) return FSL_RC_MISUSE;
  else{
    fsl_stmt st = fsl_stmt_empty;
    int rc = 0;
    rc = fsl_db_preparev( db, &st, sql, args );
    if(rc) return rc;
    rc = fsl_stmt_step( &st );
    switch(rc){
      case FSL_RC_STEP_ROW:{
        void const * str = asBlob
          ? sqlite3_column_blob(st.stmt, 0)
          : (void const *)sqlite3_column_text(st.stmt, 0);
        int const len = sqlite3_column_bytes(st.stmt,0);
        if(len && !str){
          rc = FSL_RC_OOM;
        }else{
          rc = 0;
          rc = fsl_buffer_append( b, str, len );
        }
        break;
      }
      case FSL_RC_STEP_DONE:
        rc = 0;
        break;
      default:
        break;
    }
    fsl_stmt_finalize(&st);
    return rc;
  }
}

int fsl_db_get_buffer( fsl_db * const db, fsl_buffer * const b,
                       bool asBlob,
                       char const * sql, ... ){
  int rc;
  va_list args;
  va_start(args,sql);
  rc = fsl_db_get_bufferv(db, b, asBlob, sql, args);
  va_end(args);
  return rc;
}

int32_t fsl_db_g_int32( fsl_db * const db, int32_t dflt,
                        char const * sql, ... ){
  int32_t rv = dflt;
  va_list args;
  va_start(args,sql);
  fsl_db_get_int32v(db, &rv, sql, args);
  va_end(args);
  return rv;
}

int64_t fsl_db_g_int64( fsl_db * const db, int64_t dflt,
                            char const * sql,
                            ... ){
  int64_t rv = dflt;
  va_list args;
  va_start(args,sql);
  fsl_db_get_int64v(db, &rv, sql, args);
  va_end(args);
  return rv;
}

fsl_id_t fsl_db_g_id( fsl_db * const db, fsl_id_t dflt,
                            char const * sql,
                            ... ){
  fsl_id_t rv = dflt;
  va_list args;
  va_start(args,sql);
  fsl_db_get_idv(db, &rv, sql, args);
  va_end(args);
  return rv;
}

fsl_size_t fsl_db_g_size( fsl_db * const db, fsl_size_t dflt,
                        char const * sql,
                        ... ){
  fsl_size_t rv = dflt;
  va_list args;
  va_start(args,sql);
  fsl_db_get_sizev(db, &rv, sql, args);
  va_end(args);
  return rv;
}

double fsl_db_g_double( fsl_db * const db, double dflt,
                              char const * sql,
                              ... ){
  double rv = dflt;
  va_list args;
  va_start(args,sql);
  fsl_db_get_doublev(db, &rv, sql, args);
  va_end(args);
  return rv;
}

char * fsl_db_g_text( fsl_db * const db, fsl_size_t * len,
                      char const * sql,
                      ... ){
  char * rv = NULL;
  va_list args;
  va_start(args,sql);
  fsl_db_get_textv(db, &rv, len, sql, args);
  va_end(args);
  return rv;
}

void * fsl_db_g_blob( fsl_db * const db, fsl_size_t * len,
                      char const * sql,
                      ... ){
  void * rv = NULL;
  va_list args;
  va_start(args,sql);
  fsl_db_get_blob(db, &rv, len, sql, args);
  va_end(args);
  return rv;
}

double fsl_db_julian_now(fsl_db * const db){
  double rc = -1.0;
  if(db && db->dbh){
    /* TODO? use cached statement? So far not used often enough to
       justify it. */
    fsl_db_get_double( db, &rc, "SELECT julianday('now')");
  }
  return rc;
}

double fsl_db_string_to_julian(fsl_db * const db, char const * str){
  double rc = -1.0;
  if(db && db->dbh){
    /* TODO? use cached statement? So far not used often enough to
       justify it. */
    fsl_db_get_double( db, &rc, "SELECT julianday(%Q)",str);
  }
  return rc;
}

bool fsl_db_existsv(fsl_db * const db, char const * sql, va_list args ){
  if(!db || !db->dbh || !sql) return 0;
  else if(!*sql) return 0;
  else{
    fsl_stmt st = fsl_stmt_empty;
    bool rv = false;
    if(0==fsl_db_preparev(db, &st, sql, args)){
      rv = FSL_RC_STEP_ROW==fsl_stmt_step(&st) ? true : false;
    }
    fsl_stmt_finalize(&st);
    return rv;
  }

}

bool fsl_db_exists(fsl_db * const db, char const * sql, ... ){
  bool rc;
  va_list args;
  va_start(args,sql);
  rc = fsl_db_existsv(db, sql, args);
  va_end(args);
  return rc;
}

bool fsl_db_table_exists(fsl_db * const db,
                        fsl_dbrole_e whichDb,
                        const char *zTable
){
  const char *zDb = fsl_db_role_name( whichDb );
  int rc = db->dbh
    ? sqlite3_table_column_metadata(db->dbh, zDb, zTable, 0,
                                    0, 0, 0, 0, 0)
    : !SQLITE_OK;
  return rc==SQLITE_OK ? true : false;
}

bool fsl_db_table_has_column( fsl_db * const db, char const *zTableName, char const *zColName ){
  fsl_stmt q = fsl_stmt_empty;
  int rc = 0;
  bool rv = 0;
  if(!zTableName || !*zTableName || !zColName || !*zColName) return false;
  rc = fsl_db_prepare(db, &q, "PRAGMA table_info(%Q)", zTableName );
  if(!rc) while(FSL_RC_STEP_ROW==fsl_stmt_step(&q)){
    /* Columns: (cid, name, type, notnull, dflt_value, pk) */
    fsl_size_t colLen = 0;
    char const * zCol = fsl_stmt_g_text(&q, 1, &colLen);
    if(0==fsl_strncmp(zColName, zCol, colLen)){
      rv = true;
      break;
    }
  }
  fsl_stmt_finalize(&q);
  return rv;
}

char * fsl_db_random_hex(fsl_db * const db, fsl_size_t n){
  if(!db->dbh || !n) return NULL;
  else{
    fsl_size_t rvLen = 0;
    char * rv = fsl_db_g_text(db, &rvLen,
                              "SELECT lower(hex("
                              "randomblob(%"FSL_SIZE_T_PFMT")))",
                              (fsl_size_t)(n/2+1));
    if(rv){
      assert(rvLen>=n);
      rv[n]=0;
    }
    return rv;
  }
}


int fsl_db_select_slistv( fsl_db * const db, fsl_list * const tgt,
                          char const * fmt, va_list args ){
  if(!db->dbh) return fsl__db_err_not_opened(db);
  else if(!fmt || !*fmt) return fsl__db_err_sql_empty(db);
  else if(!*fmt) return FSL_RC_RANGE;
  else{
    int rc;
    fsl_stmt st = fsl_stmt_empty;
    fsl_size_t nlen;
    char const * n;
    char * cp;
    rc = fsl_db_preparev(db, &st, fmt, args);
    while( !rc && (FSL_RC_STEP_ROW==fsl_stmt_step(&st)) ){
      nlen = 0;
      n = fsl_stmt_g_text(&st, 0, &nlen);
      cp = n ? fsl_strndup(n, (fsl_int_t)nlen) : NULL;
      if(n && !cp) rc = FSL_RC_OOM;
      else{
        rc = fsl_list_append(tgt, cp);
        if(rc && cp) fsl_free(cp);
      }
    }
    fsl_stmt_finalize(&st);
    return rc;
  }
}

int fsl_db_select_slist( fsl_db * const db, fsl_list * const tgt,
                         char const * fmt, ... ){
  int rc;
  va_list va;
  va_start (va,fmt);
  rc = fsl_db_select_slistv(db, tgt, fmt, va);
  va_end(va);
  return rc;
}

void fsl_db_sqltrace_enable( fsl_db * const db, FILE * outStream, bool expandSql ){
  if(db->dbh){
    db->impl.event.fpTrace = outStream;
    fsl_flag32_t m = db->impl.event.maskIds;
    if( expandSql ){
      m &= ~FSL_DB_EVENT_TRACE_SQL;
      m |= FSL_DB_EVENT_TRACE_SQLX;
    }else{
      m &= ~FSL_DB_EVENT_TRACE_SQLX;
      m |= FSL_DB_EVENT_TRACE_SQL;
    }
    db->impl.event.maskIds = m & FSL_DB_EVENT_mask_id;
    sqlite3_trace_v2(db->dbh, SQLITE_TRACE_STMT,
                     fsl__db_sq3TraceV2, db);
  }
}

int fsl_db_init( fsl_error * err,
                 char const * zFilename,
                 char const * zSchema,
                 ... ){
  fsl_db DB = fsl_db_empty;
  fsl_db * db = &DB;
  char const * zSql;
  int rc;
  char inTrans = 0;
  va_list ap;
  rc = fsl_db_open(db, zFilename, 0);
  if(rc) goto end;
  rc = fsl_db_exec(db, "BEGIN EXCLUSIVE");
  if(rc) goto end;
  inTrans = 1;
  rc = fsl_db_exec_multi(db, "%s", zSchema);
  if(rc) goto end;
  va_start(ap, zSchema);
  while( !rc && (zSql = va_arg(ap, const char*))!=NULL ){
    rc = fsl_db_exec_multi(db, "%s", zSql);
  }
  va_end(ap);
  end:
  if(rc){
    if(inTrans) fsl_db_exec(db, "ROLLBACK");
  }else{
    rc = fsl_db_exec(db, "COMMIT");
  }
  if(err){
    if(db->error.code){
      fsl_error_propagate(&db->error, err);
    }else if(rc){
      err->code = rc;
      err->msg.used = 0;
    }
  }
  fsl_db_close(db);
  return rc;
}

int fsl_stmt_each_f_dump( fsl_stmt * const stmt, void * state ){
  int i;
  fsl_cx * const f = (fsl_cx*)state;
  char const * sep = "\t";
  assert( f && "Missing fsl_cx* arg to fsl_stmt_each_f_dump()" );
  if( !f->output.out ) return 0;
  int const colCount = fsl_stmt_col_count(stmt);
  if(1==stmt->rowCount){
    for( i = 0; i < colCount; ++i ){
      fsl_outputf(f, "%s%s", fsl_stmt_col_name(stmt, i),
            (i==colCount-1) ? "" : sep);
    }
    fsl_output(f, "\n", 1);
  }
  for( i = 0; i < colCount; ++i ){
    char const * val = fsl_stmt_g_text(stmt, i, NULL);
    fsl_outputf(f, "%s%s", val ? val : "NULL",
          (i==colCount-1) ? "" : sep);
  }
  fsl_output(f, "\n", 1);
  return 0;
}

void fsl_db_event_listener( fsl_db * const db, fsl_db_event_f f,
                            void * pState, fsl_flag32_t mask ){
  db->impl.event.f = f;
  db->impl.event.state = pState;
  db->impl.event.maskIds = mask & FSL_DB_EVENT_mask_id;
}

bool fsl_db_is_writeable( fsl_db * const db, char const * const zSchema ){
  return db->dbh
    && 0==sqlite3_db_readonly(db->dbh, zSchema ? zSchema : "main");
}

#undef MARKER
#undef fsl__db_err_dump
