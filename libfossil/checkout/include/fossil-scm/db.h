/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
#if !defined(ORG_FOSSIL_SCM_FSL_DB_H_INCLUDED)
#define ORG_FOSSIL_SCM_FSL_DB_H_INCLUDED
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).


  ******************************************************************************
  This file declares public APIs for working with fossil's database
  abstraction layer.
*/

#include "core.h" /* MUST come first b/c of config macros */
/*
  We don't _really_ want to include sqlite3.h at this point, but if we
  do not then we have to typedef the sqlite3 struct here and that
  breaks when client code includes both this file and sqlite3.h.
*/
#include "sqlite3.h"

#if defined(__cplusplus)
extern "C" {
#endif

#if 0
/* We can't do this because it breaks when clients include both
   this header and sqlite3.h. Is there a solution which lets us
   _not_ include sqlite3.h from this file and also compiles when
   clients include both?
*/
#if !defined(SQLITE_OK)
typedef struct sqlite3 sqlite3;
#endif
#endif

/**
   Flags for use with fsl_db_open() and friends.
*/
enum fsl_db_open_flags_e {
/**
   The "no flags" value.
*/
FSL_DB_OPEN_NONE = 0,

/**
   Flag for fsl_db_open() specifying that the db should be opened
   in read-only mode.
*/
FSL_DB_OPEN_RO = 0x01,

/**
   Flag for fsl_db_open() specifying that the db should be opened
   in read-write mode, but should not create the db if it does
   not already exist.
*/
FSL_DB_OPEN_RW = 0x02,

/**
   Flag for fsl_db_open() specifying that the db should be opened in
   read-write mode, creating the db if it does not already exist.

   ACHTUNG: this flag propagates from an OPEN'd db handle to the
   ATTACH SQL command run via that handle, such that ATTACHing a
   non-existing db file will fail if the FSL_DB_OPEN_CREATE flag is
   _not_ used. Historically (prior to 2022-01-01), fsl_db_open() would
   automatically apply this flag to DBs named ":memory:" or ""
   (unnamed temp dbs), but that ended up causing a full day of
   confusion, hair-pulling, and bug-hunting when lib-level code was
   migrated from an anonymous temp db to a "real" db and ATTACH
   suddenly failed. As of 2022-01-01, fsl_db_open() always takes the
   open-mode flags as provided by the client, regardless of the DB
   name, and never automatically rewrites them to include
   FSL_DB_OPEN_CREATE.
*/
FSL_DB_OPEN_CREATE = 0x04,

/**
   Shorthand for RW+CREATE flags.
*/
FSL_DB_OPEN_RWC = FSL_DB_OPEN_RW | FSL_DB_OPEN_CREATE,

/**
   Currently unused. It "should" be used to tell fsl_repo_open_xxx()
   to confirm that the db is a repository, but how to propagate
   that through the corresponding APIs is not currently clear.
*/
FSL_DB_OPEN_SCHEMA_VALIDATE = 0x20,

/**
   Used by fsl_db_open() to to tell the underlying db connection to
   trace all SQL to stdout. This is often useful for testing,
   debugging, and learning about what's going on behind the scenes.
*/
FSL_DB_OPEN_TRACE_SQL = 0x40
};

/**
   A level of indirection to "hide" the actual db driver
   implementation from the public API. It uses SQLite3 but this gives
   us a comforting level of abstraction.

   Sidebar: at the time this code was originally written, sqlite4 was
   in an experimental stage to explore a new storage engine which, it
   was hoped, would speed up sqlite considerably. It ended up never
   leaving that stage because the performance gains of the storage
   engine did not justify such a significant upheaval. Even so,
   sqlite4 "might" come around sometime within the lifetime of this
   project, so it behooves us to abstract away this type from the
   public API.
*/
typedef sqlite3 fsl_db_t;

/**
   Bitmasks for use with fsl_db_event_e.
*/
enum fsl_db_event_mask_e {
  FSL_DB_EVENT_mask_reserved     = 0x7ffff000,

  /**
     Describes a data type for fsl_db_event::payload.
  */
  FSL_DB_EVENT_mask_payload      = 0x00000f00,

  /** (fsl_buffer const *) payload. */
  FSL_DB_EVENT_payload_buffer    = 0x00000100,

  /**
     All concrete message IDs are masked with this.
  */
  FSL_DB_EVENT_mask_id           = 0x000000ff,

  /**
     Convenience entry for use with fsl_db_event_listener().
  */
  FSL_DB_EVENT_listen_all        = FSL_DB_EVENT_mask_id
};

/**
   A map of fsl_db_event types.
*/
#define fsl_db_event_map(E)          \
  E(FSL_DB_EVENT_BEGIN,        0x01) \
  E(FSL_DB_EVENT_COMMIT,       0x02) \
  E(FSL_DB_EVENT_ROLLING_BACK, 0x04) \
  E(FSL_DB_EVENT_ROLLED_BACK,  0x08) \
  E(FSL_DB_EVENT_CLOSING,      0x10) \
  E(FSL_DB_EVENT_TRACE_SQL,    0x20  \
    | FSL_DB_EVENT_payload_buffer    \
    /*unexpanded SQL*/)              \
  E(FSL_DB_EVENT_TRACE_SQLX,   0x40  \
    | FSL_DB_EVENT_payload_buffer    \
    /*expanded SQL*/)

/**
   A flag type for use with fsl_db_event_f() implementations.
*/
enum fsl_db_event_e {
#define E(X,V) X = V,
  fsl_db_event_map(E)
#undef E
};
typedef enum fsl_db_event_e fsl_db_event_e;

/**
   Convenience FSL_DB_EVENT_... entries which cannot be defined until
   after fsl_db_event_e is.
*/
enum fsl_db_event_mask2_e {
  /** Mask of the IDs of transaction-specific events. */
  FSL_DB_EVENT_group_txn = FSL_DB_EVENT_mask_id
  & (FSL_DB_EVENT_BEGIN | FSL_DB_EVENT_COMMIT
     | FSL_DB_EVENT_ROLLING_BACK | FSL_DB_EVENT_ROLLED_BACK),
  /** Mask of the IDs of SQL tracing events. */
  FSL_DB_EVENT_group_sql_trace = FSL_DB_EVENT_mask_id
  & (FSL_DB_EVENT_TRACE_SQL
     | FSL_DB_EVENT_TRACE_SQLX)
};

/**
   An "event" message type for use with fsl_db.
*/
struct fsl_db_event {
  /**
     The event-firing db.
  */
  fsl_db * db;
  /**
     The event listener state.
  */
  void * state;
  /**
     A message-type-specific payload. Its native type is encoded in
     this->type.
  */
  void const * payload;
  /**
     Event type.
  */
  fsl_db_event_e type;
  /**
     The db's current savepoint level (because most events deal with
     transactions and this info has proven useful in downstream code).
     This will usually have a value of 1 or higher, the exception
     being FSL_DB_EVENT_CLOSING, where a that level is 0.
  */
  int savepointLevel;
};
typedef struct fsl_db_event fsl_db_event;

/**
   An event callback for fsl_db objects, primarily for communicating
   on-transaction-start/end. Each fsl_db instance may have a single
   one of these installed and it gets called via at various points to
   fire well-defined events.

   When the docs below refer to BEGIN and COMMIT, they're actually
   referring to their SAVEPOINT counterparts. That's partly
   historical, from a time when this API uses BEGIN/COMMIT, but it's
   also thought to be clearer terminology here.

   Doc references below to fsl_db_txn_begin() and fsl_db_txn_end()
   also imply their v2 variants.

   ev->state holds the state registered with the callback in
   fsl_db_event_listener().

   ev->type explains the nature of the event:

   - FSL_DB_EVENT_CLOSING is fired early on in fsl_db_close(). If
   closing is forced to roll back savepoints, those events are fired
   first and their result codes are ignored. The result of this event
   is likewise also ignored. Though the callback may (technically
   speaking) still perform db operations, it must never start a new
   transaction (unless it also closes it before returning) nor may it
   close the db object. If either of those conditions is violated,
   results are undefined. This event is _not_ fired when a DETACH'ing
   database, as this API does not have any insight into when ATTACH
   and DETACH are used.

   - FSL_DB_TXN_BEGIN is passed immediately after fsl_db_txn_begin()
   starts the transaction. If this function returns non-0, the
   transaction is _immediately_ rolled back but the rollback event is
   not triggered for this particular failure. Unless the starting of a
   new transaction level actually fails, the callback's return value
   because the result of fsl_db_txn_begin().

   - FSL_DB_TXN_COMMIT is called immediately before fsl_db_txn_end()
   runs COMMIT on the db. If it returns non-0, the pending COMMIT is
   replaced with a ROLLBACK and that result code becomes the result of
   fsl_db_txn_end().  The rollback events are _not_ triggered in this
   case.

   - FSL_DB_TXN_ROLLING_BACK is called immediately before
   fsl_db_txn_end() runs ROLLBACK on the db. If it returns non-0 but
   the SQL-level succeeds, its result becomes the result of
   fsl_db_txn_end(), but it does not cancel the pending db-side
   ROLLBACK.  Only one of FSL_DB_TXN_COMMIT or FSL_DB_TXN_ROLLING_BACK
   is invoked for a given call to fsl_db_txn_end(). If
   FSL_DB_TXN_COMMIT returns non-zero, no FSL_DB_TXN_ROLLING_BACK
   invocation is made.

   - FSL_DB_TXN_ROLLED_BACK is called immediately after
   fsl_db_txn_end() has run a ROLLBACK. If this function returns non-0
   but both the the SQL-level rollback and the FSL_DB_TXN_ROLLING_BACK
   event succeededsucceeded, this event's result becames the result
   value of fsl_db_txn_end().

   To summarize, priority of determining which error code to return
   from fsl_db_txn_end(), in combination with the potential for errors
   when submitting the resulting SQL to commit/roll back, are:

   - Commit: COMMIT event (this callback) -> SQLite step()

   - Rollback: ROLLING_BACK event -> SQLite step() -> ROLLBACK event

   This hook is NOT called for IMPLICIT transactions. It is only
   called for explicit calls to fsl_db_txn_begin(), fsl_db_txn_end(),
   and fsl_db_rollback_force(). Clients must never execute SQL-level
   (BEGIN, COMMIT, ROLLBACK, SAVEPOINT) calls on fsl_db instances or
   they risk getting it out of sync. There are exceptions where
   they're safe but, generically speaking, they're not.

   @see fsl_db_event_listener()
*/
typedef int (*fsl_db_event_f)(fsl_db_event const * e);

/**
   Db handle wrapper class. Each instance wraps a single sqlite
   database handle. Though this type's interface is not opaque, it is
   never legal for clients to directly manipulate the members.

   Fossil is built upon sqlite3, but this abstraction is intended to
   hide that, insofar as possible, from clients so as to simplify an
   eventual port from v3 to v4. Clients should avoid relying on the
   underlying db being sqlite (or at least not rely on a specific
   version), but may want to register custom functions with the driver
   (or perform similar low-level operations) and the option is left
   open for them to access that handle via the fsl_db::dbh member. In
   practice, library clients do not ever need to use the underlying
   sqlite API.

   @see fsl_db_open();
   @see fsl_db_close();
   @see fsl_stmt
*/
struct fsl_db {

  /**
     Underlying db driver handle.
  */
  fsl_db_t * dbh;

  /**
     Holds error state from the underlying driver.  fsl_db and
     fsl_stmt operations which fail at the driver level "should"
     update this state to include error info from the driver.
     fsl_cx APIs which fail at the DB level uplift this (using
     fsl_error_swap()) so that they can pass it on up the call chain.
  */
  fsl_error error;

  /**
     Holds the file name used when opening this db. Might not refer to
     a real file (e.g. might be ":memory:" or "" (similar to
     ":memory:" but may swap to temp storage).

     Design note: we currently hold the name as it is passed to the
     db-open routine, without canonicalizing it. That is very possibly
     a mistake, as it makes it impossible to properly compare the name
     to another arbitrary checkout-relative name for purposes of
     fsl_reserved_fn_check(). For purposes of fsl_cx we currently
     (2021-03-12) canonicalize db's which we fsl_db_open(), but not
     those which we ATTACH (which includes the repo and checkout
     dbs). We cannot reasonably canonicalize the repo db filename
     because it gets written into the checkout db so that the checkout
     knows where to find the repository. History has shown that that
     path needs to be stored exactly as a user entered it, which is
     often relative.

     Memory is owned by this object.
  */
  char * filename;

  /**
     Holds the database name for use in creating queries.
     Might or might not be set/needed, depending on
     the context.

     Memory is owned by this object.
  */
  char * name;
  /**
     Describes what role(s) this db connection plays in fossil (if
     any). This is a bitmask of fsl_dbrole_e values, and a db
     connection may have multiple roles. This is only used by the
     fsl_cx-internal API.
  */
  fsl_flag16_t role;

  /**
     Private implementation details.
  */
  struct {
    /**
       Debugging/test counter. Closing a db with opened statements
       might assert() or trigger debug output when the db is closed.
    */
    int openStatementCount;

    /**
       Internal flags.
    */
    fsl_flag32_t flags;

    struct {
      /** Event listener callback. */
      fsl_db_event_f f;
      /** Impl-specific state for f. */
      void * state;
      /**
         The FSL_DB_EVENT_mask_id-masked part of event IDs to
         fire the callback for.
      */
      fsl_flag32_t maskIds;
      /** Older-style (pre-fsl_db_event) trace output channel. */
      FILE * fpTrace;
    } event;

    /** Transaction-related state. */
    struct {
      /** Counter for fsl_db_txn_begin/end(). */
      int level;
      /** Prepared statements for SAVEPOINT management. */
      sqlite3_stmt * sSavepoint;
      /** Prepared statements for SAVEPOINT RELEASE. */
      sqlite3_stmt * sRelease;
      /** Prepared statements for ROLLBACK TO SAVEPOINT. */
      sqlite3_stmt * sRollback;
    } txn;

    /** State for use with fsl_db_prepare_cached(). */
    struct {

      /**
         An internal cache of "static" queries - those which do not rely
         on call-time state unless that state can be bind()ed. Holds a
         linked list of (fsl_stmt*) instances, managed by the
         fsl_db_prepare_cached() and fsl_stmt_cached_yield() APIs.

         @see fsl_db_prepare_cached()
      */
      fsl_stmt * head;
    } stCache;
    /**
       Internal buffer to reduce reallocations caused via
       fsl_db_prepare_cached(), fsl_db_see_check(),
       and perhaps other internals.
    */
    fsl_buffer buffer;

    /**
       A marker which tells fsl_db_close() whether or not
       fsl_db_malloc() allocated this instance (in which case
       fsl_db_close() will fsl_free() it) or not (in which case it
       does not fsl_free() it).
    */
    void const * allocStamp;
  } impl;
};
/**
   Empty-initialized fsl_db structure, intended for const-copy
   initialization.
*/
#define fsl_db_empty_m {       \
  .dbh = NULL,                 \
  .error = fsl_error_empty_m,  \
  .filename = NULL,            \
  .name = NULL,                \
  .role = 0,                   \
  .impl = {                    \
    .openStatementCount = 0,   \
    .flags = 0,                \
    .event = {                 \
      .f=NULL,                 \
      .state=NULL,             \
      .maskIds=0,              \
      .fpTrace=NULL            \
    },                         \
    .txn = {                   \
      .level = 0,              \
      .sSavepoint=NULL,        \
      .sRelease=NULL,          \
      .sRollback=NULL          \
    },                         \
    .stCache = {               \
      .head = NULL             \
    },                         \
    .buffer = fsl_buffer_empty_m, \
    .allocStamp = NULL         \
  }                            \
}

/**
   Empty-initialized fsl_db structure, intended for copy
   initialization.
*/
FSL_EXPORT const fsl_db fsl_db_empty;

/**
   If db is not NULL then this function returns its name (the one used
   to fsl_db_open() it). The bytes are valid until the db connection
   is closed or until someone mucks with db->filename. If len is not
   NULL then *len is (on success) assigned to the length of the
   returned string, in bytes. The string is NUL-terminated, so
   fetching the length (by passing a non-NULL 2nd parameter) is
   optional but sometimes useful to eliminate a downstream call to
   fsl_strlen().

   Results are undefined if db is NULL or was improperly initialized.
   Will return NULL if db was properly initialized (via copying
   fsl_db_empty) but has not yet been opened.
*/
FSL_EXPORT char const * fsl_db_filename(fsl_db const * db, fsl_size_t * len);

typedef sqlite3_stmt fsl_stmt_t;
/**
   Represents a prepared statement handle.
   Intended usage:

   ```
   fsl_stmt st = fsl_stmt_empty;
   int rc = fsl_db_prepare( db, &st, "..." );
   if(rc){ // Error!
   assert(!st.stmt);
   // db->error might hold driver-level error details.
   }else{
   // use st and eventually finalize it:
   fsl_stmt_finalize( &st );
   }
   ```


   Script binding implementations can largely avoid exposing the
   statement handle (and its related cleanup ordering requirements)
   to script code. They need to have some mechanism for binding
   values to SQL (or implement all the escaping themselves), but
   that can be done without exposing all of the statement class if
   desired. For example, here's some hypothetical script code:

   ```
   var st = db.prepare(".... where i=:i and x=:x");
   // st is-a Statement, but we need not add script bindings for
   // the whole Statement.bind() API. We can instead simplify that
   // to something like:
   try {
   st.exec( {i: 42, x: 3} )
   // or, for a SELECT query:
   st.each({
   bind{i:42, x:3},
   rowType: 'array', // or 'object'
   callback: function(row,state,colNames){ print(row.join('\t')); },
   state: {...callback function state...}
   });
   } finally {
   st.finalize();
   // It is critical that st gets finalized before its DB, and
   // that'shard to guaranty if we leave st to the garbage collector!
   }
   // see below for another (less messy) alternative
   ```

   Ideally, script code should not have direct access to the
   Statement because managing lifetimes can be difficult in the
   face of flow-control changes caused by exceptions (as the above
   example demonstrates). Statements can be completely hidden from
   clients if the DB wrapper is written to support it. For example,
   in pseudo-JavaScript that might look like:

   ```
   db.exec("...where i=? AND x=?", 42, 3);
   db.each({sql:"select ... where id<?", bind:[10],
   rowType: 'array', // or 'object'
   callback: function(row,state,colNames){ print(row.join('\t')); },
   state: {...arbitrary state for the callback...}
   });
   ```
*/
struct fsl_stmt {
  /**
     The db which prepared this statement.
  */
  fsl_db * db;

  /**
     Underlying db driver-level statement handle. Clients should
     not rely on the specify concrete type if they can avoid it, to
     simplify an eventual port from sqlite3 to sqlite4.
  */
  fsl_stmt_t * stmt;

  /**
     Describes the database(s) used by this statement handle, in the
     form of a bitmask of fsl_dbrole_e values. This is a bit of a
     kludge used to allow the internals to flush cached statements
     from the fossil global config db when detaching that database.
     Code which requires this to be set must set it itself and must
     set it correctly. Hypothetically, no client-level code requires
     it but _some_ libfossil-internal code does.
  */
  fsl_flag16_t role;

  /**
     The SQL used to prepare this statement. We keep this, rather than
     simply fetching the SQL on demand, only for facilitate
     fsl_db_prepare_cached(). But since we have it, and it might be
     interesting, in client code, it's a public part of the interface.
  */
  fsl_buffer sql;

  /**
     The number of times this statement has fetched a row via
     fsl_stmt_step(). This is primarily intended for use by
     fsl_stmt_each_f() implementations so that they can keep track of
     when to emit a header (or similar pre-first-row setup).
  */
  fsl_size_t rowCount;

  /**
     Private implementation details.
  */
  struct {

    /**
       Internal state flags.
    */
    fsl_flag16_t flags;

    /**
       Internal use only: counts the number of times this query has
       been resolved as cached via fsl_db_prepare_cached().
    */
    uint32_t cachedHits;

    /**
       For _internal_ use in creating linked lists. Clients _must_not_
       modify this field.
    */
    fsl_stmt * next;

    /**
       A marker which tells fsl_stmt_finalize() whether or not
       fsl_stmt_malloc() allocated this instance (in which case
       fsl_stmt_finalize() will fsl_free() it) or not (in which case
       it does not free() it).
    */
    void const * allocStamp;
  } impl;
};

/**
   Empty-initialized fsl_stmt instance, intended for use as an
   in-struct initializer.
*/
#define fsl_stmt_empty_m {    \
  .db = NULL,                 \
  .stmt = NULL,               \
  .role = 0,                  \
  .sql = fsl_buffer_empty_m,  \
  .rowCount = 0,              \
  .impl = {                   \
    .flags = 0,               \
    .cachedHits = 0,          \
    .next = NULL,             \
    .allocStamp = NULL        \
  }                           \
}

/**
   Empty-initialized fsl_stmt instance, intended for
   copy-constructing.
*/
FSL_EXPORT const fsl_stmt fsl_stmt_empty;

/**
   Allocates a new, cleanly-initialized fsl_stmt instance using
   fsl_malloc(). The returned pointer must eventually be passed to
   fsl_stmt_finalize() to free it (whether or not it is ever passed
   to fsl_db_prepare()).

   Returns NULL on allocation error.
*/
FSL_EXPORT fsl_stmt * fsl_stmt_malloc(void);


/**
   Behaves like fsl_error_get(), using the db's underlying error
   state. Results are undefined if !db.
*/
FSL_EXPORT int fsl_db_err_get( fsl_db const * db,
                               char const ** msg, fsl_size_t * len );

/**
   Resets any error state in db, but might keep the string
   memory allocated for later use.
*/
FSL_EXPORT void fsl_db_err_reset( fsl_db * db );

/**
   Prepares an SQL statement for execution. On success it returns
   0, populates tgt with the statement's state, and the caller is
   obligated to eventually pass tgt to fsl_stmt_finalize(). tgt
   must have been cleanly initialized, either via allocation via
   fsl_stmt_malloc() or by copy-constructing fsl_stmt_empty
   resp. fsl_stmt_empty_m (depending on the context).

   On error non-0 is returned and tgt is not modified. If
   preparation of the statement fails at the db level then
   FSL_RC_DB is returned f's error state (fsl_cx_err_get())
   "should" contain more details about the problem. Returns
   FSL_RC_MISUSE if !db, !callback, or !sql. Returns
   FSL_RC_NOT_FOUND if db is not opened. Returns FSL_RC_RANGE if
   !*sql.

   The sql string and the following arguments get routed through
   fsl_appendf(), so any formatting options supported by that
   routine may be used here. In particular, the %%q and %%Q
   formatting options are intended for use in escaping SQL for
   routines such as this one.

   Compatibility note: in sqlite, empty SQL code evaluates
   successfully but with a NULL statement. This API disallows empty
   SQL because it uses NULL as a "no statement" marker and because
   empty SQL is arguably not a query at all.

   Tips:

   - fsl_stmt_col_count() can be used to determine whether a
   statement is a fetching query (fsl_stmt_col_count()>0) or not
   (fsl_stmt_col_count()==0) without having to know the contents
   of the query.

   - fsl_db_prepare_cached() can be used to cache often-used or
   expensive-to-prepare queries within the context of their parent
   db handle.
*/
FSL_EXPORT int fsl_db_prepare( fsl_db * db, fsl_stmt * tgt,
                               char const * sql, ... );

/**
   va_list counterpart of fsl_db_prepare().
*/
FSL_EXPORT int fsl_db_preparev( fsl_db * db, fsl_stmt * tgt,
                                char const * sql, va_list args );

/**
   A special-purpose variant of fsl_db_prepare() which caches
   statements based on their SQL code. This works very much like
   fsl_db_prepare() and friends except that it can return the same
   statement (via *st) multiple times (statements with identical
   SQL are considered equivalent for caching purposes). Clients
   need not explicitly pass the returned statement to
   fsl_stmt_finalize() - the db holds these statements and will
   finalize them when it is closed. It is legal to pass them to
   finalize, in which case they will be cleaned up immediately but
   that also invalidates _all_ pointers to the shared instances.

   If client code does not call fsl_stmt_finalize(), it MUST pass
   the statement pointer to fsl_stmt_cached_yield(st) after is done
   with it. That makes the query available for use again with this
   routine. If a cached query is not yielded via
   fsl_stmt_cached_yield() then this routine will return
   FSL_RC_ACCESS on subsequent requests for that SQL to prevent
   that recursive (mis)use of the statement causes problems.

   This routine is intended to be used in oft-called routines
   where the cost of re-creating statements on each execution could
   be prohibitive (or at least a bummer).

   Returns 0 on success, FSL_RC_MISUSE if any arguments are
   invalid. On error, *st is not written to.  On other errors
   db->error might be updated with more useful information.  See the
   Caveats section below for more details.

   Its intended usage looks like:

   ```
   fsl_stmt * st = NULL;
   int rc = fsl_db_prepare_cached(myDb, &st, "SELECT ...");
   if(rc) { assert(!st); ...error... }
   else {
   ...use it, and _be sure_ to yield it when done:...
   fsl_stmt_cached_yield(st);
   }
   ```

   Though this function allows a formatted SQL string, caching is
   generally only useful with statements which have "static" SQL,
   i.e. no call-dependent values embedded within the SQL. It _can_,
   however, contain bind() placeholders which get reset for each
   use. Note that fsl_stmt_cached_yield() resets the statement, so
   most uses of cached statements do not require that the client
   explicitly reset cached statements (doing so is harmless,
   however).

   Caveats:

   Cached queries must not be used in contexts where recursion might
   cause the same query to be returned from this function while it is
   being processed at another level in the execution stack. Results
   would be undefined. Caching is primarily intended for often-used
   routines which bind and fetch simple values, and not for queries
   which bind large inlined values or might invoke recursion. Because
   of the potential for recursive breakage, this function flags
   queries it doles out and requires that clients call
   fsl_stmt_cached_yield() to un-flag them for re-use. It will return
   FSL_RC_ACCESS if an attempt is made to (re)prepare a statement for
   which a fsl_stmt_cached_yield() is pending, and db->error will be
   populated with a (long) error string describing the problem and
   listing the SQL which caused the collision/misuse. Such SQL can
   often be disambiguated by adding a \%s, wrapped in a C-style
   comment, to the end and passing `__func__` as its value (which is
   also useful for debugging in conjunction with SQL tracing).

   Design note: for the recursion/parallel use case we "could"
   reimplement this to dole out a new statement (e.g. by appending
   " -- a_number" to the SQL to bypass the collision) and free it in
   fsl_stmt_cached_yield(), but that (A) gets uglier than it needs
   to be and (B) is not needed unless/until we really need cached
   queries in spots which would normally break them. The whole
   recursion problem is still theoretical at this point but could
   easily affect small, often-used queries without recursion.

   @see fsl_db_stmt_cache_clear()
   @see fsl_stmt_cached_yield()
*/
FSL_EXPORT int fsl_db_prepare_cached( fsl_db * db, fsl_stmt ** st,
                                      char const * sql, ... );

/**
   The va_list counterpart of fsl_db_prepare_cached().
*/
FSL_EXPORT int fsl_db_preparev_cached( fsl_db * db, fsl_stmt ** st,
                                       char const * sql, va_list args );

/**
   "Yields" a statement which was prepared with
   fsl_db_prepare_cached(), such that that routine can once again
   use/re-issue that statement. Statements prepared this way must be
   yielded in order to prevent that recursion causes
   difficult-to-track errors when a given cached statement is used
   concurrently in different code contexts.

   If st is not NULL then this also calls fsl_stmt_reset() and
   fsl_stmt_clear_bindings() on the statement.

   Returns 0 on success, FSL_RC_MISUSE if !st or if st does not
   appear to have been doled out from fsl_db_prepare_cached().

   @see fsl_db_prepare_cached()
   @see fsl_db_stmt_cache_clear()
*/
FSL_EXPORT int fsl_stmt_cached_yield( fsl_stmt * st );

/**
   Frees memory associated with stmt but does not free stmt unless
   it was allocated by fsl_stmt_malloc() (these objects are
   normally stack-allocated, and such object must be initialized by
   copying fsl_stmt_empty so that this function knows whether or
   not to fsl_free() them). Returns FSL_RC_MISUSE if !stmt or it
   has already been finalized (but was not freed).
*/
FSL_EXPORT int fsl_stmt_finalize( fsl_stmt * stmt );

/**
   "Steps" the given SQL cursor one time. The return values
   FSL_RC_STEP_ROW and FSL_RC_STEP_DONE are both success cases, the
   former indicating that one row has been fetched and the latter
   indicating that either no rows are left to fetch or the statement
   is a non-fetching query. On error some other non-zero code will be
   returned.  On a db error this will update the underlying db's error
   state.  This function increments stmt->rowCount by 1 if it returns
   FSL_RC_STEP_ROW.

   Returns FSL_RC_MISUSE if !stmt or stmt has not been prepared.

   It is only legal to call the fsl_stmt_g_xxx() and
   fsl_stmt_get_xxx() functions if this functon returns
   FSL_RC_STEP_ROW. FSL_RC_STEP_DONE is returned upon successfully
   ending iteration or if there is no iteration to perform
   (e.g. typically an UPDATE or INSERT, but see the next paragraph).

   Though the historical definition of non-fetching query was pretty
   clear, the addition of the RETURNING keyword to sqlite3's dialect
   means that even an INSERT or DELETE can return data.

   @see fsl_stmt_reset()
   @see fsl_stmt_reset2()
   @see fsl_stmt_each()
*/
FSL_EXPORT int fsl_stmt_step( fsl_stmt * stmt );

/**
   A callback interface for use with fsl_stmt_each() and
   fsl_db_each(). It will be called one time for each row fetched,
   passed the statement object and the state parameter passed to
   fsl_stmt_each() resp. fsl_db_each().  If it returns non-0 then
   iteration stops and that code is returned UNLESS it returns
   FSL_RC_BREAK, in which case fsl_stmt_each() stops iteration and
   returns 0. i.e. implementations may return FSL_RC_BREAK to
   prematurly end iteration without causing an error.

   This callback is not called for non-fetching queries or queries
   which return no results, though it might (or might not) be
   interesting for it to do so, passing a NULL stmt for that case.

   stmt->rowCount can be used to determine how many times the
   statement has called this function. Its counting starts at 1.

   It is strictly illegal for a callback to pass stmt to
   fsl_stmt_step(), fsl_stmt_reset(), fsl_stmt_finalize(), or any
   similar routine which modifies its state. It must only read the
   current column data (or similar metatdata, e.g. column names)
   from the statement, e.g. using fsl_stmt_g_int32(),
   fsl_stmt_get_text(), or similar.
*/
typedef int (*fsl_stmt_each_f)( fsl_stmt * stmt, void * state );

/**
   Calls the given callback one time for each result row in the
   given statement, iterating over stmt using fsl_stmt_step(). It
   applies no meaning to the callbackState parameter, which gets
   passed as-is to the callback. See fsl_stmt_each_f() for the
   semantics of the callback.

   Returns 0 on success. Returns FSL_RC_MISUSE if !stmt or
   !callback.
*/
FSL_EXPORT int fsl_stmt_each( fsl_stmt * stmt, fsl_stmt_each_f callback,
                              void * callbackState );

/**
   Resets the given statement, analog to sqlite3_reset(). Should be
   called one time between fsl_stmt_step() iterations when running
   multiple INSERTS, UPDATES, etc. via the same statement. If
   resetRowCounter is true then the statement's row counter
   (st->rowCount) is also reset to 0, else it is left
   unmodified. (Most use cases don't use the row counter.)

   Returns 0 on success, FSL_RC_MISUSE if stmt has not been prepared
   or has not been cleanly initialized via copying from fsl_stmt_empty
   or fsl_stmt_empty_m, FSL_RC_DB if the underlying reset fails (in
   which case the error state of the stmt->db handle is updated to
   contain the error information).

   @see fsl_stmt_db()
   @see fsl_stmt_reset()
*/
FSL_EXPORT int fsl_stmt_reset2( fsl_stmt * stmt, bool resetRowCounter );

/**
   Equivalent to fsl_stmt_reset2(stmt, false).
*/
FSL_EXPORT int fsl_stmt_reset( fsl_stmt * stmt );

/**
   Clears memory associtated with any bindings on this statement.
   This is a no-op if stmt is not prepared.
*/
FSL_EXPORT void fsl_stmt_clear_bindings(fsl_stmt * stmt);

/**
   Returns the db handle which prepared the given statement, or
   NULL if stmt has not been prepared.
*/
FSL_EXPORT fsl_db * fsl_stmt_db( fsl_stmt * stmt );

/**
   Returns the SQL string used to prepare the given statement, or NULL
   if stmt has not been prepared. If len is not NULL then *len is set
   to the length of the returned string (which is NUL-terminated). The
   returned bytes are owned by stmt and are invalidated when it is
   finalized.
*/
FSL_EXPORT char const * fsl_stmt_sql( fsl_stmt * stmt,
                                      fsl_size_t * len );

/**
   Returns the name of the given 0-based result column index, or
   NULL if !stmt, stmt is not prepared, or index is out out of
   range. The returned bytes are owned by the statement object and
   may be invalidated shortly after this is called, so the caller
   must copy the returned value if it needs to have any useful
   lifetime guarantees. It's a bit more complicated than this, but
   assume that any API calls involving the statement handle might
   invalidate the column name bytes.

   The API guarantees that the returned value is either NULL or
   NUL-terminated.

   @see fsl_stmt_param_count()
   @see fsl_stmt_col_count()
*/
FSL_EXPORT char const * fsl_stmt_col_name(fsl_stmt * stmt, int index);

/**
   Returns the result column count for the given statement, or -1 if
   !stmt or it has not been prepared. Note that this value is cached
   when the statement is created. Note that non-fetching queries
   (e.g. INSERT and UPDATE) have a column count of 0 unless they have
   a RETURNING clause. Some non-SELECT constructs, e.g. PRAGMA
   table_info(tname) and INSERT/UPDATE/DELETE with a RETURNING clause,
   behave like a SELECT and have a positive column count.

   @see fsl_stmt_param_count()
   @see fsl_stmt_col_name()
*/
FSL_EXPORT int fsl_stmt_col_count( fsl_stmt const * stmt );

/**
   Returns the bound parameter count for the given statement, or -1
   if !stmt or it has not been prepared. Note that this value is
   cached when the statement is created.

   @see fsl_stmt_col_count()
   @see fsl_stmt_col_name()
*/
FSL_EXPORT int fsl_stmt_param_count( fsl_stmt const * stmt );

/**
   Returns the index of the given named parameter for the given
   statement, or -1 if !stmt or stmt is not prepared.
*/
FSL_EXPORT int fsl_stmt_param_index( fsl_stmt * stmt, char const * param);

/**
   Binds NULL to the given 1-based parameter index.  Returns 0 on
   succcess. Sets the DB's error state on error.
*/
FSL_EXPORT int fsl_stmt_bind_null( fsl_stmt * stmt, int index );

/**
   Equivalent to fsl_stmt_bind_null_name() but binds to
   a named parameter.
*/
FSL_EXPORT int fsl_stmt_bind_null_name( fsl_stmt * stmt, char const * param );

/**
   Binds v to the given 1-based parameter index.  Returns 0 on
   succcess. Sets the DB's error state on error.
*/
FSL_EXPORT int fsl_stmt_bind_int32( fsl_stmt * stmt, int index, int32_t v );

/**
   Equivalent to fsl_stmt_bind_int32() but binds to a named
   parameter.
*/
FSL_EXPORT int fsl_stmt_bind_int32_name( fsl_stmt * stmt, char const * param, int32_t v );

/**
   Binds v to the given 1-based parameter index.  Returns 0 on
   succcess. Sets the DB's error state on error.
*/
FSL_EXPORT int fsl_stmt_bind_int64( fsl_stmt * stmt, int index, int64_t v );

/**
   Equivalent to fsl_stmt_bind_int64() but binds to a named
   parameter.
*/
FSL_EXPORT int fsl_stmt_bind_int64_name( fsl_stmt * stmt, char const * param, int64_t v );

/**
   Binds v to the given 1-based parameter index.  Returns 0 on
   succcess. Sets the Fossil context's error state on error.
*/
FSL_EXPORT int fsl_stmt_bind_double( fsl_stmt * stmt, int index, double v );

/**
   Equivalent to fsl_stmt_bind_double() but binds to a named
   parameter.
*/
FSL_EXPORT int fsl_stmt_bind_double_name( fsl_stmt * stmt, char const * param, double v );

/**
   Binds v to the given 1-based parameter index.  Returns 0 on
   succcess. Sets the DB's error state on error.
*/
FSL_EXPORT int fsl_stmt_bind_id( fsl_stmt * stmt, int index, fsl_id_t v );

/**
   Equivalent to fsl_stmt_bind_id() but binds to a named
   parameter.
*/
FSL_EXPORT int fsl_stmt_bind_id_name( fsl_stmt * stmt, char const * param, fsl_id_t v );

/**
   Binds the first n bytes of v as text to the given 1-based bound
   parameter column in the given statement. If makeCopy is true then
   the binding makes an copy of the data. Set makeCopy to false ONLY
   if you KNOW that the bytes will outlive the binding.

   Returns 0 on success. On error stmt's underlying db's error state
   is updated, hopefully with a useful error message.
*/
FSL_EXPORT int fsl_stmt_bind_text( fsl_stmt * stmt, int index,
                                   char const * v, fsl_int_t n,
                                   bool makeCopy );

/**
   Equivalent to fsl_stmt_bind_text() but binds to a named
   parameter.
*/
FSL_EXPORT int fsl_stmt_bind_text_name( fsl_stmt * stmt, char const * param,
                                        char const * v, fsl_int_t n,
                                        bool makeCopy );
/**
   Binds the first n bytes of v as a blob to the given 1-based bound
   parameter column in the given statement. See fsl_stmt_bind_text()
   for the semantics of the makeCopy parameter and return value.
*/
FSL_EXPORT int fsl_stmt_bind_blob( fsl_stmt * stmt, int index,
                                   void const * v, fsl_size_t len,
                                   bool makeCopy );

/**
   Equivalent to fsl_stmt_bind_blob() but binds to a named
   parameter.
*/
FSL_EXPORT int fsl_stmt_bind_blob_name( fsl_stmt * stmt, char const * param,
                                        void const * v, fsl_int_t len,
                                        bool makeCopy );

/**
   Gets an integer value from the given 0-based result set column,
   assigns *v to that value, and returns 0 on success.

   Returns FSL_RC_RANGE if index is out of range for stmt, FSL_RC_MISUSE
   if stmt has no result columns.
*/
FSL_EXPORT int fsl_stmt_get_int32( fsl_stmt * stmt, int index, int32_t * v );

/**
   Gets an integer value from the given 0-based result set column,
   assigns *v to that value, and returns 0 on success.

   Returns FSL_RC_RANGE if index is out of range for stmt, FSL_RC_MISUSE
   if stmt has no result columns.
*/
FSL_EXPORT int fsl_stmt_get_int64( fsl_stmt * stmt, int index, int64_t * v );

/**
   The fsl_id_t counterpart of fsl_stmt_get_int32(). Depending on
   the sizeof(fsl_id_t), it behaves as one of fsl_stmt_get_int32()
   or fsl_stmt_get_int64().
*/
FSL_EXPORT int fsl_stmt_get_id( fsl_stmt * stmt, int index, fsl_id_t * v );

/**
   Convenience form of fsl_stmt_get_id() which returns the value
   directly but cannot report errors. It returns -1 on error, but
   that is not unambiguously an error value.
*/
FSL_EXPORT fsl_id_t fsl_stmt_g_id( fsl_stmt * stmt, int index );

/**
   Convenience form of fsl_stmt_get_int32() which returns the value
   directly but cannot report errors. It returns 0 on error, but
   that is not unambiguously an error.
*/
FSL_EXPORT int32_t fsl_stmt_g_int32( fsl_stmt * stmt, int index );

/**
   Convenience form of fsl_stmt_get_int64() which returns the value
   directly but cannot report errors. It returns 0 on error, but
   that is not unambiguously an error.
*/
FSL_EXPORT int64_t fsl_stmt_g_int64( fsl_stmt * stmt, int index );

/**
   Convenience form of fsl_stmt_get_double() which returns the value
   directly but cannot report errors. It returns 0 on error, but
   that is not unambiguously an error.
*/
FSL_EXPORT double fsl_stmt_g_double( fsl_stmt * stmt, int index );

/**
   Convenience form of fsl_stmt_get_text() which returns the value
   directly but cannot report errors. It returns NULL on error, but
   that is not unambiguously an error because it also returns NULL
   if the column contains an SQL NULL value. If outLen is not NULL
   then it is set to the byte length of the returned string.
*/
FSL_EXPORT char const * fsl_stmt_g_text( fsl_stmt * stmt, int index, fsl_size_t * outLen );

/**
   Gets double value from the given 0-based result set column,
   assigns *v to that value, and returns 0 on success.

   Returns FSL_RC_RANGE if index is out of range for stmt, FSL_RC_MISUSE
   if stmt has no result columns.
*/
FSL_EXPORT int fsl_stmt_get_double( fsl_stmt * stmt, int index, double * v );

/**
   Gets a string value from the given 0-based result set column,
   assigns *out (if out is not NULL) to that value, assigns *outLen
   (if outLen is not NULL) to *out's length in bytes, and returns 0
   on success. Ownership of the string memory is unchanged - it is owned
   by the statement and the caller should immediately copy it if
   it will be needed for much longer.

   Returns FSL_RC_RANGE if index is out of range for stmt,
   FSL_RC_MISUSE if stmt has no result columns. Returns FSL_RC_OOM if
   fetching the text from the underlying statement handle fails due to
   an allocation error.
*/
FSL_EXPORT int fsl_stmt_get_text( fsl_stmt * stmt, int index, char const **out,
                       fsl_size_t * outLen );

/**
   The Blob counterpart of fsl_stmt_get_text(). Identical to that
   function except that its output result (3rd paramter) type
   differs, and it fetches the data as a raw blob, without any sort
   of string interpretation.

   Returns FSL_RC_RANGE if index is out of range for stmt,
   FSL_RC_MISUSE if stmt has no result columns. Returns FSL_RC_OOM if
   fetching the text from the underlying statement handle fails due to
   an allocation error.
*/
FSL_EXPORT int fsl_stmt_get_blob( fsl_stmt * stmt, int index, void const **out, fsl_size_t * outLen );

/**
   Executes multiple SQL statements, ignoring any results they might
   collect. Returns 0 on success, non-0 on error.  On error
   db->error might be updated to report the problem.
*/
FSL_EXPORT int fsl_db_exec_multi( fsl_db * db, const char * sql, ...);

/**
   va_list counterpart of db_exec_multi().
*/
FSL_EXPORT int fsl_db_exec_multiv( fsl_db * db, const char * sql, va_list args);

/**
   Executes a single SQL statement, skipping over any results it may
   have. Returns 0 on success. On error db's error state may be
   updated. Note that this function translates FSL_RC_STEP_DONE and
   FSL_RC_STEP_ROW to 0. For cases where those particular result codes
   are significant, use fsl_db_prepare() and fsl_stmt_step() (for
   which this function is just a proxy).
*/
FSL_EXPORT int fsl_db_exec( fsl_db * db, char const * sql, ... );

/**
   va_list counterpart of fs_db_exec().
*/
FSL_EXPORT int fsl_db_execv( fsl_db * db, char const * sql, va_list args );

/**
   Begins a transaction on the given db.

   This API historically used pseudo-nested transactions based off of
   BEGIN/COMMIT.  As of 2025-08-09 it uses SAVEPOINTs instead. The
   difference is subtle but was a necessary change for the libfossil
   SCM pieces to be able to behave sensibly in some cases. These docs
   use the term "transaction" to mean "savepoint".

   Returns FSL_RC_MISUSE if !db or the db is not connected, else the
   result of the underlying db call(s).

   Transactions are an easy way to implement "dry-run" mode for
   some types of applications. For example:

   ```
   bool dryRunMode = ...;
   fsl_db_txn_begin(db);
   ...do your stuff...
   fsl_db_txn_end_v2(db, dryRunMode, false);
   ```

   Here's a tip for propagating error codes when using
   transactions:

   ```
   ...
   if(rc) fsl_db_txn_end_v2(db, false, possiblyTrue);
   else rc = fsl_db_txn_end_v2(db, true, false);
   ```

   That ensures that we propagate rc in the face of a rollback but we
   also capture the rc for a commit (which might yet fail).

   Sidebar: this API, prior to 2025-08, used pseudo-nested
   transactions directly ported from fossil(1) (essentially a
   reference-counted BEGIN/END block, with each level representing one
   reference count point). As of 2025-08 it exclusively uses sqlite3's
   SAVEPOINT[^1] feature, which is essentially nested transactions.
   This change enables better support for dry-run modes and more
   flexible non-fatal-to-the-transaction error handling. e.g. we can
   fail certain opertations, e.g. lookup of a resources, without
   inherently invalidating the whole transaction stack.

   [^1]: https://sqlite.org/lang_savepoint.html
*/
FSL_EXPORT int fsl_db_txn_begin(fsl_db * db);

/**
   Equivalent to fsl_db_txn_end_v2(db, true, false).
*/
FSL_EXPORT int fsl_db_txn_commit(fsl_db * db);

/**
   Equivalent to fsl_db_txn_end_v2(db, false, true), noting that the
   final value of true there is for historical API compatibility and
   will be changed to false if we can get away with it elsewhere.
*/
FSL_EXPORT int fsl_db_txn_rollback(fsl_db * db);

#if 0
/**
   Forces a rollback of any pending transaction in db, regardless of
   the internal transaction begin/end counter. Returns FSL_RC_MISUSE
   if db is not opened, else returns the value of the underlying
   ROLLBACK call. This also re-sets/frees any transaction-related
   state held by db (e.g. db->beforeCommit).  Use with care, as this
   mucks about with db state in a way which is not all that pretty and
   it may confuse downstream code.

   Returns 0 on success.

   Never, ever use this. In 12+ years it has never proven necessary to
   use this function, and doing so can easily lead to a mismatch in
   transaction-using code and the transaction stack level.
*/
FSL_EXPORT int fsl_db_rollback_force(fsl_db * db);
#endif

/**
   Decrements the transaction counter incremented by
   fsl_db_txn_begin() and commits or rolls back the
   savepoint if the counter goes to 0.

   The behavior of keepSavepoint depends on the transaction depth:

   - If this is not the top-most savepoint, it is "released" (applied)
     if keepSavepoint is true or rolled back if keepSavepoint false,
     and that resulting db result code is returned. A value of true
     releases this savepoint regardless of the bubbleRollback flag
     (see below).

   - If this is the top-most savepoint then keepSavepoint will be
     considered to be false if bubbleRollback is true or if any call
     to this function in the same top-level transaction stack flag has
     passed true for that argument.

   If bubbleRollback is true, or any call to this function within the
   same top-level savepoint passed true as the bubbleRollback
   argument, the transaction is put into a persistent will-rollback
   mode.  In this mode, individual savepoints can still be applied by
   passing true as the second argument, but when the final one is
   popped, the whole thing will be rolled back instead of applied.

   To reiterate: if db fsl_db_txn_begin() is used in a nested manner
   and bubbleRollback is true for any one of the nested calls, then
   that value will be remembered, such that the downstream calls to
   this function within the same transaction will behave as if
   bubbleRollback were true even if they pass false for the third
   argument.

   Achtung: the bubbleRollback==true behavior is, as of 2025-08,
   considered "legacy" and should no longer be used, but much code
   still currently expects that behavior.

   The distinction been whether retaining a savepoint should put the
   transaction into a persistent rollback state is primarily of
   interest to operations which implement "dry-run" mode.  For
   dry-run, keepSavepoint should always ben false, while
   bubbleRollback should be true if the operation failed in a way
   which could impact further operations in the same transaction.

   Returns FSL_RC_MISUSE if db is not opened, 0 if the transaction
   counter is above 0, else the result of the (potentially many)
   underlying database operations.
*/
FSL_EXPORT int fsl_db_txn_end_v2(fsl_db * db, bool keepSavepoint,
                                 bool bubbleRollback);

/**
   Equivalent to fsl_db_txn_end_v2(DB, !DOROLLBACK, !!DOROLLBACK),
   which mimics the historical behavior of this API.
*/
#define fsl_db_txn_end(DB,DOROLLBACK) \
  fsl_db_txn_end_v2((DB), !(DOROLLBACK), !!(DOROLLBACK))

/**
   Returns the given db's current transaction depth. If the value is
   negative, its absolute value represents the depth but indicates
   that a rollback is pending. If it is positive, the transaction is
   still in a "good" state. If it is 0, no transaction is active.
*/
FSL_EXPORT int fsl_db_txn_level(fsl_db * db);

/**
   Runs the given SQL query on the given db and returns true if the
   query returns any rows, else false. Returns 0 for any error as
   well.
*/
FSL_EXPORT bool fsl_db_exists(fsl_db * db, char const * sql, ... );

/**
   va_list counterpart of fsl_db_exists().
*/
FSL_EXPORT bool fsl_db_existsv(fsl_db * db, char const * sql, va_list args );

/**
   Runs a fetch-style SQL query against DB and returns the first
   column of the first result row via *rv. If the query returns no
   rows, *rv is not modified. The intention is that the caller sets
   *rv to his preferred default (or sentinel) value before calling
   this.

   The format string (the sql parameter) accepts all formatting
   options supported by fsl_appendf().

   Returns 0 on success. On error db's error state is updated and
   *rv is not modified.

   Returns FSL_RC_MISUSE without side effects if !db, !rv, !sql,
   or !*sql.
*/
FSL_EXPORT int fsl_db_get_int32( fsl_db * db, int32_t * rv,
                                 char const * sql, ... );

/**
   va_list counterpart of fsl_db_get_int32().
*/
FSL_EXPORT int fsl_db_get_int32v( fsl_db * db, int32_t * rv,
                                  char const * sql, va_list args);

/**
   Convenience form of fsl_db_get_int32() which returns the value
   directly but provides no way of checking for errors. On error,
   or if no result is found, defaultValue is returned.
*/
FSL_EXPORT int32_t fsl_db_g_int32( fsl_db * db,
                                   int32_t defaultValue,
                                   char const * sql, ... );

/**
   The int64 counterpart of fsl_db_get_int32(). See that function
   for the semantics.
*/
FSL_EXPORT int fsl_db_get_int64( fsl_db * db, int64_t * rv,
                      char const * sql, ... );

/**
   va_list counterpart of fsl_db_get_int64().
*/
FSL_EXPORT int fsl_db_get_int64v( fsl_db * db, int64_t * rv,
                       char const * sql, va_list args);

/**
   Convenience form of fsl_db_get_int64() which returns the value
   directly but provides no way of checking for errors. On error,
   or if no result is found, defaultValue is returned.
*/
FSL_EXPORT int64_t fsl_db_g_int64( fsl_db * db, int64_t defaultValue,
                            char const * sql, ... );


/**
   The fsl_id_t counterpart of fsl_db_get_int32(). See that function
   for the semantics.
*/
FSL_EXPORT int fsl_db_get_id( fsl_db * db, fsl_id_t * rv,
                   char const * sql, ... );

/**
   va_list counterpart of fsl_db_get_id().
*/
FSL_EXPORT int fsl_db_get_idv( fsl_db * db, fsl_id_t * rv,
                    char const * sql, va_list args);

/**
   Convenience form of fsl_db_get_id() which returns the value
   directly but provides no way of checking for errors. On error,
   or if no result is found, defaultValue is returned.
*/
FSL_EXPORT fsl_id_t fsl_db_g_id( fsl_db * db, fsl_id_t defaultValue,
                      char const * sql, ... );


/**
   The fsl_size_t counterpart of fsl_db_get_int32(). See that
   function for the semantics. If this function would fetch a
   negative value, it returns FSL_RC_RANGE and *rv is not modified.
*/
FSL_EXPORT int fsl_db_get_size( fsl_db * db, fsl_size_t * rv,
                     char const * sql, ... );

/**
   va_list counterpart of fsl_db_get_size().
*/
FSL_EXPORT int fsl_db_get_sizev( fsl_db * db, fsl_size_t * rv,
                      char const * sql, va_list args);

/**
   Convenience form of fsl_db_get_size() which returns the value
   directly but provides no way of checking for errors. On error,
   or if no result is found, defaultValue is returned.
*/
FSL_EXPORT fsl_size_t fsl_db_g_size( fsl_db * db,
                                     fsl_size_t defaultValue,
                                     char const * sql, ... );


/**
   The double counterpart of fsl_db_get_int32(). See that function
   for the semantics.
*/
FSL_EXPORT int fsl_db_get_double( fsl_db * db, double * rv,
                                  char const * sql, ... );

/**
   va_list counterpart of fsl_db_get_double().
*/
FSL_EXPORT int fsl_db_get_doublev( fsl_db * db, double * rv,
                                   char const * sql, va_list args);

/**
   Convenience form of fsl_db_get_double() which returns the value
   directly but provides no way of checking for errors. On error,
   or if no result is found, defaultValue is returned.
*/
FSL_EXPORT double fsl_db_g_double( fsl_db * db, double defaultValue,
                                   char const * sql, ... );

/**
   The C-string counterpart of fsl_db_get_int32(). On success *rv
   will be set to a dynamically allocated string copied from the
   first column of the first result row. If rvLen is not NULL then
   *rvLen will be assigned the byte-length of that string. If no
   row is found, *rv is set to NULL and *rvLen (if not NULL) is set
   to 0, and 0 is returned. Note that NULL is also a legal result
   (an SQL NULL translates as a NULL string), The caller must
   eventually free the returned string value using fsl_free().
*/
FSL_EXPORT int fsl_db_get_text( fsl_db * db, char ** rv, fsl_size_t * rvLen,
                                char const * sql, ... );

/**
   va_list counterpart of fsl_db_get_text().
*/
FSL_EXPORT int fsl_db_get_textv( fsl_db * db, char ** rv, fsl_size_t * rvLen,
                      char const * sql, va_list args );

/**
   Convenience form of fsl_db_get_text() which returns the value
   directly but provides no way of checking for errors. On error,
   or if no result is found, NULL is returned. The returned string
   must eventually be passed to fsl_free() to free it.  If len is
   not NULL then if non-NULL is returned, *len will be assigned the
   byte-length of the returned string.
*/
FSL_EXPORT char * fsl_db_g_text( fsl_db * db, fsl_size_t * len,
                      char const * sql,
                      ... );

/**
   The Blob counterpart of fsl_db_get_text(). Identical to that
   function except that its output result (2nd paramter) type
   differs, and it fetches the data as a raw blob, without any sort
   of string interpretation. The returned *rv memory must
   eventually be passed to fsl_free() to free it. If len is not
   NULL then on success *len will be set to the byte length of the
   returned blob. If no row is found, *rv is set to NULL and *rvLen
   (if not NULL) is set to 0, and 0 is returned. Note that NULL is
   also a legal result (an SQL NULL translates as a NULL string),
*/
FSL_EXPORT int fsl_db_get_blob( fsl_db * db, void ** rv, fsl_size_t * len,
                     char const * sql, ... );


/**
   va_list counterpart of fsl_db_get_blob().
*/
FSL_EXPORT int fsl_db_get_blobv( fsl_db * db, void ** rv, fsl_size_t * stmtLen,
                      char const * sql, va_list args );

/**
   Convenience form of fsl_db_get_blob() which returns the value
   directly but provides no way of checking for errors. On error,
   or if no result is found, NULL is returned.
*/
FSL_EXPORT void * fsl_db_g_blob( fsl_db * db, fsl_size_t * len,
                      char const * sql,
                      ... );
/**
   Similar to fsl_db_get_text() and fsl_db_get_blob(), but writes
   its result to tgt, appending its results to the given buffer.

   If asBlob is true then the underlying BLOB API is used to
   populate the buffer, else the underlying STRING/TEXT API is
   used.  For many purposes there will be no difference, but if you
   know you might have binary data, be sure to pass a true value
   for asBlob to avoid any potential encoding-related problems.

   Results are undefined if any pointer argument is NULL. Returns
   FSL_RC_MISUSE if the SQL is an empty string.
*/
FSL_EXPORT int fsl_db_get_buffer( fsl_db * db, fsl_buffer * tgt,
                                  bool asBlob, char const * sql,
                                  ... );

/**
   va_list counterpart of fsl_db_get_buffer().
*/
FSL_EXPORT int fsl_db_get_bufferv( fsl_db * db, fsl_buffer * tgt,
                                   bool asBlob, char const * sql,
                                   va_list args );


/**
   Expects sql to be a SELECT-style query which (potentially)
   returns a result set. For each row in the set callback() is
   called, as described for fsl_stmt_each(). Returns 0 on success.
   The callback is _not_ called for queries which return no
   rows. If clients need to know if rows were returned, they can
   add a counter to their callbackState and increment it from the
   callback.

   Returns FSL_RC_MISUSE if db is not opened, !callback,
   !sql. Returns FSL_RC_RANGE if !*sql.
*/
FSL_EXPORT int fsl_db_each( fsl_db * db, fsl_stmt_each_f callback,
                            void * callbackState, char const * sql, ... );

/**
   va_list counterpart to fsl_db_each().
*/
FSL_EXPORT int fsl_db_eachv( fsl_db * db, fsl_stmt_each_f callback,
                             void * callbackState, char const * sql, va_list args );


/**
   Returns the given Julian date value formatted as an ISO8601
   string (with a fractional seconds part if msPrecision is true,
   else without it).  Returns NULL if !db, db is not connected, j
   is less than 0, or on allocation error. The returned memory must
   eventually be freed using fsl_free().

   If localTime is true then the value is converted to the local time,
   otherwise it is not.

   @see fsl_db_unix_to_iso8601()
   @see fsl_julian_to_iso8601()
   @see fsl_iso8601_to_julian()
*/
FSL_EXPORT char * fsl_db_julian_to_iso8601( fsl_db * db, double j,
                                            bool msPrecision, bool localTime );

/**
   Returns the given Julian date value formatted as an ISO8601
   string (with a fractional seconds part if msPrecision is true,
   else without it).  Returns NULL if !db, db is not connected, j
   is less than 0, or on allocation error. The returned memory must
   eventually be freed using fsl_free().

   If localTime is true then the value is converted to the local time,
   otherwise it is not.

   @see fsl_db_julian_to_iso8601()
   @see fsl_julian_to_iso8601()
   @see fsl_iso8601_to_julian()
*/
FSL_EXPORT char * fsl_db_unix_to_iso8601( fsl_db * db, fsl_time_t j,
                                          bool localTime );


/**
   Returns the current time in Julian Date format. Returns a negative
   value if !db or db is not opened.
*/
FSL_EXPORT double fsl_db_julian_now(fsl_db * db);

/**
   Uses the given db to convert the given time string to Julian Day
   format. If it cannot be converted, a negative value is returned.
   The str parameter can be anything suitable for passing to sqlite's:

   SELECT julianday(str)

   Note that this routine will escape str for use with SQL - the
   caller must not do so.

   @see fsl_julian_to_iso8601()
   @see fsl_iso8601_to_julian()
*/
FSL_EXPORT double fsl_db_string_to_julian(fsl_db * db, char const * str);

/**
   Opens the given db file and populates db with its handle.  db
   must have been cleanly initialized by copy-initializing it from
   fsl_db_empty (or fsl_db_empty_m) or by allocating it using
   fsl_db_malloc(). Failure to do so will lead to undefined
   behaviour.

   openFlags may be a mask of FSL_DB_OPEN_xxx values, but not all are
   used/supported here. If FSL_DB_OPEN_CREATE is _not_ set in
   openFlags and dbFile does not exist, it will return
   FSL_RC_NOT_FOUND. The existence of FSL_DB_OPEN_CREATE in the flags
   will cause this routine to try to create the file if needed. If
   conflicting flags are specified (e.g. FSL_DB_OPEN_RO and
   FSL_DB_OPEN_RWC) then which one takes precedence is unspecified and
   possibly unpredictable. If openFlags is 0 then FSL_DB_OPEN_RO is
   assumed.

   This routine normally looks for the given file before attempting to
   open it and will update db's error state if the file cannot be
   found. Those checks are skipped if (1) openFlags has the
   FSL_DB_OPEN_CREATE bit set, (2) dbName is empty (indicating a
   nameless temporary database), (3) the string contains a ":" in any
   byte position, in which case it is assumed to be a URL-style name
   or ":memory:". The exception to (3) is on Windows platforms, where
   it recognize drive letter prefixes in the form "X:".

   See this page for the differences between ":memory:" and "":

   https://www.sqlite.org/inmemorydb.html

   This routine does not handle URL-style file names.

   Returns FSL_RC_MISUSE if !db, !dbFile, or if db->dbh is not NULL
   (i.e. if it is already opened or its memory was default-initialized
   (use fsl_db_empty to cleanly copy-initialize new stack-allocated
   instances).

   On error db->dbh will be NULL, but db->error might contain error
   details.

   Regardless of success or failure, db should be passed to
   fsl_db_close() to free up all memory associated with it. It is
   not closed automatically by this function because doing so cleans
   up the error state, which the caller will presumably want to
   have.

   @see fsl_db_close()
   @see fsl_db_prepare()
   @see fsl_db_malloc()
*/
FSL_EXPORT int fsl_db_open( fsl_db * db, char const * dbFile,
                            fsl_flag32_t openFlags );

/**
   Closes the given db handle and frees any resources owned by
   db. This function is a no-op if db is NULL.

   If db was allocated using fsl_db_malloc() (as determined by
   examining db->allocStamp) then this routine also fsl_free()s it,
   otherwise it is assumed to either be on the stack or part of a
   larger struct and is not freed, but any resources it allocated are
   freed.

   If db has any pending transactions, they are rolled back by this
   function before a FSL_DB_EVENT_CLOSING event is fired.
*/
FSL_EXPORT void fsl_db_close( fsl_db * db );

/**
   If db is an opened db handle, this registers a debugging
   function with the db which traces all SQL to the given FILE
   handle. If outStream is NULL then tracing events will be
   emitted via FSL_DB_EVENT_TRACE_SQLX instead.

   If expandedSql is true then it uses sqlite3_expanded_sql() to
   expand each traced statement, else it uses unexpanded SQL (which is
   much faster).

   If outStream is not NULL then tracing will go there, otherwise it
   will go out via a FSL_DB_EVENT_TRACE_SQL or FSL_DB_EVENT_TRACE_SQLX
   event (depending on the value of expandedSql). Ownership of
   outStream is not modified by this call and outStream must outlive
   its association with db.

   This is a no-op if db is closed.

   This mechanism is only intended for debugging and exploration of
   how Fossil works. Tracing is often as easy way to ensure that a
   given code block is getting run.

   TODOs:

   - Expand this API to take a client-side callback and state
   object, rather than a FILE pointer.

   - Provide a toggle for the tracing level: with and without
   "expanded" SQL. Expanding the SQL to include its bound values is
   far more expensive (but also far more informative).
*/
FSL_EXPORT void fsl_db_sqltrace_enable( fsl_db * db, FILE * outStream,
                                        bool expandSql);

/**
   Returns the row ID of the most recent insertion,
   or -1 if !db, db is not connected, or 0 if no inserts
   have been performed.
*/
FSL_EXPORT fsl_id_t fsl_db_last_insert_id(fsl_db * db);

/**
   Returns non-0 (true) if the database (which must be open) table
   identified by zTableName has a column named zColName
   (case-sensitive), else returns 0.
*/
FSL_EXPORT bool fsl_db_table_has_column( fsl_db * db,
                                         char const *zTableName,
                                         char const *zColName );

/**
   If a db name has been associated with db then it is returned,
   otherwise NULL is returned. A db has no name by default, but
   fsl_cx-used ones get their database name assigned to them
   (e.g. "main" for the main db).
*/
FSL_EXPORT char const * fsl_db_name(fsl_db const * db);

/**
   Returns a db name string for the given fsl_db_role value. The
   string is static, guaranteed to live as long as the app.  It
   returns NULL if passed FSL_DBROLE_NONE or some value out of range
   for the enum.
*/
FSL_EXPORT const char * fsl_db_role_name(enum fsl_dbrole_e r);

/**
   Allocates a new fsl_db instance(). Returns NULL on allocation
   error. Note that fsl_db instances can often be used from the
   stack - allocating them dynamically is an uncommon case necessary
   for script bindings.

   Achtung: the returned value's allocStamp member is used for
   determining if fsl_db_close() should free the value or not.  Thus
   if clients copy over this value without adjusting allocStamp back
   to its original value, the library will likely leak the instance.
   Been there, done that.
*/
FSL_EXPORT fsl_db * fsl_db_malloc(void);

/**
   The fsl_stmt counterpart of fsl_db_malloc(). See that function
   for when you might want to use this and a caveat involving the
   allocStamp member of the returned value. fsl_stmt_finalize() will
   free statements created with this function.
*/
FSL_EXPORT fsl_stmt * fsl_stmt_malloc(void);

/**
   ATTACHes the file zDbName to db using the database name zLabel
   (which gets quoted as an SQL identifier). Returns 0 on
   success. Returns FSL_RC_MISUSE if any argument is NULL or any
   string argument starts with a NUL byte, else it returns the result
   of fsl_db_exec() which attaches the db. On db-level errors db's
   error state will be updated.
*/
FSL_EXPORT int fsl_db_attach(fsl_db * db, const char *zDbName,
                             const char *zLabel);

/**
   The converse of fsl_db_detach(). Must be passed the same arguments
   which were passed as the 1st and 3rd arguments to fsl_db_attach().
   Returns 0 on success, FSL_RC_MISUSE if !db, !zLabel, or !*zLabel,
   else it returns the result of the underlying fsl_db_exec()
   call.
*/
FSL_EXPORT int fsl_db_detach(fsl_db * db, const char *zLabel);

/**
   Expects fmt to be a SELECT-style query. For each row in the
   query, the first column is fetched as a string and appended to
   the tgt list.

   Returns 0 on success, FSL_RC_MISUSE if !fmt or fmt is empty, or any
   number of potential FSL_RC_OOM or db-related errors.

   Results rows with a NULL value (resulting from an SQL NULL) are
   added to the list as NULL entries.

   Each entry appended to the list is a (char *) which must
   be freed using fsl_free(). To easiest way to clean up
   the list and its contents is:

   ```
   fsl_list_visit_free(tgt,...);
   ```

   On error the list may be partially populated.

   Complete example:

   ```
   fsl_list li = fsl_list_empty;
   int rc = fsl_db_select_slist(db, &li,
            "SELECT uuid FROM blob WHERE rid<20");
   if(!rc){
     fsl_size_t i;
     for(i = 0;i < li.used; ++i){
       char const * uuid = (char const *)li.list[i];
       fsl_fprintf(stdout, "UUID: %s\n", uuid);
     }
   }
   fsl_list_visit_free(&li, 1);
   ```

   Of course fsl_list_visit() may be used to traverse the list as
   well, as long as the visitor expects (char [const]*) list
   elements.
*/
FSL_EXPORT int fsl_db_select_slist( fsl_db * db, fsl_list * tgt,
                                    char const * fmt, ... );

/**
   The va_list counterpart of fsl_db_select_slist().
*/
FSL_EXPORT int fsl_db_select_slistv( fsl_db * db, fsl_list * tgt,
                                     char const * fmt, va_list args );

/**
   Returns n bytes of random lower-case hexidecimal characters
   using the given db as its data source, plus a terminating NUL
   byte. The returned memory must eventually be freed using
   fsl_free(). Returns NULL if !n, db is not opened, or on a db-level error.
*/
FSL_EXPORT char * fsl_db_random_hex(fsl_db * db, fsl_size_t n);

/**
   Returns the "number of database rows that were changed or inserted
   or deleted by the most recently completed SQL statement" (to quote
   the underlying APIs). Returns 0 if db is not opened.

   See: https://sqlite.org/c3ref/changes.html
*/
FSL_EXPORT int fsl_db_changes_recent(fsl_db * db);

/**
   Returns "the number of row changes caused by INSERT, UPDATE or
   DELETE statements since the database connection was opened" (to
   quote the underlying APIs). Returns 0 if db is not opened.

   See; https://sqlite.org/c3ref/total_changes.html
*/
FSL_EXPORT int fsl_db_changes_total(fsl_db * db);

/**
   Initializes the given database file. zFilename is the name of
   the db file. It is created if needed, but any directory
   components are not created. zSchema is the base schema to
   install.  The following arguments may be (char const *) SQL
   code, each of which gets run against the db after the main
   schema is called.  The variadic argument list MUST end with NULL
   (0), even if there are no non-NULL entries.

   Returns 0 on success.

   On error, if err is not NULL then it is populated with any error
   state from the underlying (temporary) db handle.
*/
FSL_EXPORT int fsl_db_init( fsl_error * err, char const * zFilename,
                 char const * zSchema, ... );

/**
   A fsl_stmt_each_f() impl, intended primarily for debugging, which
   simply outputs row data in tabular form via fsl_output(). The
   state argument must be a valid fsl_cx pointer. On
   the first row, the column names are output.

   Achtung: this function's expectations were changed on 2024-09-16:
   prior to this, stmt's db handle had access to a fsl_cx instance,
   but that was factored out. It now requires the state argument to be
   a fsl_cx.
*/
FSL_EXPORT int fsl_stmt_each_f_dump( fsl_stmt * stmt, void * state );

/**
   Returns true if the table name specified by the final argument
   exists in the fossil database specified by the 2nd argument on the
   db connection specified by the first argument, else returns false.

   Trivia: this is one of the few libfossil APIs which makes use of
   FSL_DBROLE_TEMP.

   Potential TODO: this is a bit of a wonky interface. Consider
   changing it to eliminate the role argument, which is only really
   needed if we have duplicate table names across attached dbs or if
   we internally mess up and write a table to the wrong db.
*/
FSL_EXPORT bool fsl_db_table_exists(fsl_db * db, fsl_dbrole_e whichDb,
                                    const char *zTable);

/**
   The elipsis counterpart of fsl_stmt_bind_fmtv().
*/
FSL_EXPORT int fsl_stmt_bind_fmt( fsl_stmt * st, char const * fmt, ... );

/**
    Binds a series of values using a formatting string.

    The string may contain the following characters, each of which
    refers to the next argument in the args list:

    '-': binds a NULL and expects a NULL placeholder
         in the argument list (for consistency's sake).

    'i': binds an int32

    'I': binds an int64

    'R': binds a fsl_id_t ('R' as in 'RID' or 'ROWID')

    'f': binds a double

    's': binds a (char const *) as text or NULL. See below.

    'S': binds a (char const *) as a blob or NULL. See below.

    'b': binds a (fsl_buffer const *) as text or NULL. See below.

    'B': binds a (fsl_buffer const *) as a blob or NULL. See below.

    ' ': spaces are allowed for readability and are ignored.

    '^': An optional modifier for "sSbB". See below.

    Returns 0 on success, any number of other FSL_RC_xxx codes on
    error.

    About the sSbB bindings:

    - If their argument is NULL they bind as SQL NULL.

    - ACHTUNG: the "sSbB" bindings assume, because of how this API is
      normally used, that the memory pointed to by the given argument
      will outlive the pending step of the given statement, so that
      memory is NOT copied by this call. Thus results are undefined if
      such an argument's memory is invalidated before the statement is
      done with it. The default behavior can be overridden by adding
      the character '^' immediately after the "sSbB" entry (with no
      intervening spaces), which will tell it to make a copy of the
      content. e.g. "ss^i" would bind one string without making a
      copy, then one string with a copy, then an int32.

    Example:

    ```
    int rc = fsl_stmt_bind_fmt(&stmt, "ss^i", "hello", "world", (int32_t)12);
    ```

    Note the cast on the final argument to ensure that it's of the
    proper size for the 'i' type.
*/
FSL_EXPORT int fsl_stmt_bind_fmtv( fsl_stmt * st, char const * fmt,
                                   va_list args );

/**
   Works like fsl_stmt_bind_fmt() but:

   1) It calls fsl_stmt_reset() before binding the arguments.

   2) If binding succeeds then it steps the given statement a single
      time.

   3) If the result is _NOT_ FSL_RC_STEP_ROW then it also resets the
      statement before returning. It does not do so for
      FSL_RC_STEP_ROW because doing so would make it impossible to
      fetch the current row's data.

   Returns the result of the underlying step() call. It never returns
   0.

   Design note: the return value for FSL_RC_STEP_ROW, as opposed to
   returning 0, is necessary for proper statement use if the client
   wants to fetch any result data from the statement afterwards (which
   is only legal if FSL_RC_STEP_ROW was the result). This is also why
   it cannot reset the statement if that result is returned.
*/
FSL_EXPORT int fsl_stmt_bind_stepv_v2( fsl_stmt * st, char const * fmt,
                                       va_list args );

/**
   The elipsis counterpart of fsl_stmt_bind_step_v2().
*/
FSL_EXPORT int fsl_stmt_bind_step_v2( fsl_stmt * st, char const * fmt, ... );

/**
   Works exactly like fsl_stmt_bind_stepv_v2() except that it returns
   0 if stepping results in FSL_RC_STEP_DONE. That behavior is legacy
   and now consider to have been a poor design decision, but those
   semantics are currently heavily relied on throughout the library.

   To re-iterate: this routine NEVER returns FSL_RC_STEP_DONE. We say
   that because some folks (🙋‍♂️) keep breaking things by checking for
   that code instead of 0. That's why fsl_stmt_bind_step_v2() was
   introduced.

*/
FSL_EXPORT int fsl_stmt_bind_stepv( fsl_stmt * st, char const * fmt,
                                      va_list args );

/**
   The elipsis counterpart of fsl_stmt_bind_step().
*/
FSL_EXPORT int fsl_stmt_bind_step( fsl_stmt * st, char const * fmt, ... );

/**
   Sets the on-transaction hook for the given db, overwriting any
   prior one.  To clear the hook, pass NULL for the 2nd and 3rd
   arguments.

   The final argument specifies what events type to look for.  It may
   be an OR of any FSL_DB_EVENT_... events. Use
   FSL_DB_EVENT_listen_all to listen to all events. The stored mask is
   bitwise ANDed against FSL_DB_EVENT_mask_id, so only the concrete
   message IDs are recorded here.

   Achtung: fsl_cx instances install this hook on database handles
   they manage, so never call this on such a handle.
*/
FSL_EXPORT void fsl_db_event_listener( fsl_db * db, fsl_db_event_f f,
                                       void * pState, fsl_flag32_t mask);

/**
   Returns true if db is opened and sqlite3_db_readonly() returns 0
   for it, else returns false. If zSchema is NULL then "main" is
   assumed.
*/
FSL_EXPORT bool fsl_db_is_writeable( fsl_db * db, char const *zSchema );

#if defined(__cplusplus)
} /*extern "C"*/
#endif
#endif /* ORG_FOSSIL_SCM_FSL_DB_H_INCLUDED */
