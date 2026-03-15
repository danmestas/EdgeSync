/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2025 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2025 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/
/**
  This file contains the library internals of the sync transport layer.
*/
#include "fossil-scm/sync.h"
#include "fossil-scm/core.h"
#include "fossil-scm/hash.h"
#include "fossil-scm/auth.h"
#include "fossil-scm/util.h"
#include "fossil-scm/confdb.h"
#include "fossil-scm/internal.h"

#include <stdbool.h>
#include <stdio.h> /* FILE class */
#include <assert.h>
#include <stdlib.h> /* bsearch() */
#include <string.h> /* memcpy() */

#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

const fsl_xfer_config fsl_xfer_config_empty =
  fsl_xfer_config_empty_m;

const fsl_sc fsl_sc_empty =
  fsl_sc_empty_m;

const fsl_xfer_dbg fsl_xfer_dbg_empty =
  fsl_xfer_dbg_empty_m;

#if 0
const fsl_sync_config fsl_sync_config_empty =
  fsl_sync_config_empty_m;
#endif

const fsl_clone_config fsl_clone_config_empty =
  fsl_clone_config_empty_m;

const fsl__xfer fsl__xfer_empty = {
  .f = 0,
  .ch = 0,
  .config = 0,
  .rc = 0,
  .line = {
    .b = fsl_buffer_empty_m,
    .bRaw = fsl_buffer_empty_m,
    .aToken  = {0,0,0,0,0,0},
    .aTokLen = {0,0,0,0,0,0},
    .nToken  = 0
  },
  .buf = {
    .uresp = fsl_buffer_empty_m,
    .hash = fsl_buffer_empty_m,
    .scratch = fsl_buffer_empty_m,
    .cardPayload = fsl_buffer_empty_m
  },
  .q = {
    .igotInsert   = fsl_stmt_empty_m,
    .igotHasRid   = fsl_stmt_empty_m,
    .gimmeInsert  = fsl_stmt_empty_m,
    //.gimmeDelete = fsl_stmt_empty_m,
    .remoteUnkUuid = fsl_stmt_empty_m,

  },
  .flag = {
    .configRcvMask = 0,
    .configSendMask = 0,
    .zAltPCode = 0,
    .syncPrivate = false,
    .nextIsPrivate = false
  },
  .remote = {
    .releaseVersion = 0,
    .manifestDate = 0,
    .manifestTime = 0,
    .clockSkewSeen = 0
  },
  .clientVersion = {
    .number = FOSSIL_RELEASE_VERSION_NUMBER,
    .date = FOSSIL_MANIFEST_NUMERIC_DATE,
    .time = FOSSIL_MANIFEST_NUMERIC_TIME,
    .projCodeMatch = -1
  },
  .clone = {
    .protocolVersion = 0,
    .seqNo = 0,
  },
  .user = {
    .name = 0, .password = 0,
    .passwordToFree = 0,
    .perm = fsl_uperm_empty_m
  },
  .chz = fsl_sc_empty_m,
  .n = fsl_xfer_metrics_empty_m,
  .request = {
    .sha1 = fsl_sha1_cx_empty_m,
    .loginCard = fsl_buffer_empty_m,
    .nBytes = 0
  },
  .response = {
    .httpVersion = 0,
    .httpCode = 0
  }
};

/** @internal

   Runs xf->ch->submit(). If a compressed response is expected then it
   fetches all of the content in a single go to keep the xf->ch impl
   from having to deal with compression.

   On success, before returning, it calls xf->ch->init() with a second
   value of FSL_SC_INIT_REQUEST so that xf can immediately be used for
   generating follow-up output.
*/
static int fsl__xfer_submit(fsl__xfer * const xf);

/** @internal

   Updates xf->count and returns the value of xf->ch->append(p,n).
*/
static int fsl__xfer_append(fsl__xfer * const xf, void const * p, fsl_size_t n);

#if 0
/** @internal

   Equivalent to fsl__sc_appendln() except that (A) it adds a newline
   to the output if it does not already have one and (B) it updates
   xf->count.
*/
static int fsl__xfer_appendln(fsl__xfer * const xf, char const * zMsg);
#endif

/** @internal

   Variadic form of fsl__xfer_appendln().
*/
static int fsl__xfer_appendlnf(fsl__xfer * const xf, char const * zFmt, ...);

/** @internal

   va_list form of fsl__xfer_appendln().
*/
static int fsl__xfer_appendlnv(fsl__xfer * const xf, char const * zFmt, va_list args);


/** @internal

   fsl__xfer_appendln() wrapper which prefixes the line with the appropriate
   card name and increments the outbound card count.
*/
static int fsl__xfer_appendcard(fsl__xfer * const xf, fsl_xfcard_e card,
                                char const * zFmt, ...);


/** @internal

   Reads n bytes from xf->ch into dest. On success it updates
   xf->count and returns 0. n must not be 0 (and that's assert()ed).
*/
static int fsl__xfer_read(fsl__xfer * const xf, fsl_buffer * dest, fsl_size_t n);

/** @internal

   Reads one line of input from xf->ch into xf->line.b and tokenizes
   it into the other xf->line state. On success it updates xf->line
   and xf->count then returns 0.
*/
static int fsl__xfer_readln(fsl__xfer * const xf);

/**
   If xf->rc is not 0 this is a proxy for fsl_cx_emit(), else it
   returns xf->rc without side-effects. This is a no-op if xf->f
   does not have a fsl_msg_listener installed.
*/
static int fsl__xfer_emit(fsl_xfer *xf, fsl_msg_e type, void const * payload){
  if( 0==xf->rc && xf->f->cxConfig.listener.callback ){
    xf->rc = fsl_cx_emit(xf->f, type, payload);
  }
  return xf->rc;
}

static int fsl__xfer_emitf(fsl_xfer *xf, fsl_msg_e type,
                           char const * zFmt, ...){
  if( 0==xf->rc && xf->f->cxConfig.listener.callback ){
    va_list args;
    va_start(args, zFmt);
    xf->rc = fsl_cx_emitfv( xf->f, type, zFmt, args);
    va_end(args);
  }
  return xf->rc;
}


void fsl_xfer_debug_f_FILE(fsl_xfer * const xf, char const * const zMsg){
  assert(xf->config->debug.state);
  (void)fprintf(xf->config->debug.state, "%s sync debug: %s", xf->ch->name, zMsg);
}

void fsl_xfer_debug_f_buffer(fsl_xfer * const xf, char const * const zMsg){
  assert(xf->config->debug.state);
  (void)fsl_buffer_appendf(fsl_buffer_reuse(xf->config->debug.state),
                           "%s sync debug: %s", xf->ch->name, zMsg);
}

fsl_cx * fsl_xfer_cx(fsl_xfer * xf){
  return xf->f;
}

void fsl_xfer_dbg_cleanup( fsl_xfer_dbg * const p ){
  fsl_buffer_clear(&p->b);
}

void fsl_xfer_config_cleanup( fsl_xfer_config * p ){
  fsl_xfer_dbg_cleanup(&p->debug);
}

int fsl_xfer_errorfv(fsl_xfer * const xf, int code,
                     char const * zFmt, va_list args){
  fsl_error * const err = fsl_cx_err_e(xf->f);
  fsl_buffer * const b = fsl_buffer_reuse(&err->msg);
  if( !code ){
    code = FSL_RC_ERROR /* Don't allow this routine to clear
                           xf->rc */;
  }
  fsl_buffer_appendf(b, "fsl_sc<%s> error %R: ", xf->ch->name, code);
  fsl_buffer_appendfv(b, zFmt, args);
  return xf->rc = err->code = code;
}

int fsl_xfer_errorf(fsl_xfer * const xf, int code, char const * zFmt, ...){
  va_list args;
  assert( xf->f );
  va_start(args,zFmt);
  fsl_xfer_errorfv(xf, code, zFmt, args);
  va_end(args);
  return xf->rc;
}

int fsl_xfer_error(fsl_xfer * const xf, int code, char const * zMsg){
  return fsl_xfer_errorf(xf, code, "%s", zMsg);
}

void fsl_xfer_debug(fsl_xfer * const xf, char const * zMsg){
  if( xf->config->debug.callback ){
    xf->config->debug.callback(xf, zMsg);
  }
}

int fsl_xfer_debugfv(fsl_xfer * const xf, char const * zFmt, va_list args){
  int rc = 0;
  assert( xf->f );
  if( xf->config->debug.callback ){
    fsl_buffer * const b = fsl_buffer_reuse(&xf->config->debug.b);
    if( b->used<128 ) fsl_buffer_reserve(b, 128);
    fsl_buffer_appendf(b, "# DEBUG fsl_sc<%s>: ", xf->ch->name);
    fsl_buffer_appendfv(b, zFmt, args);
    fsl_buffer_ensure_eol(b);
    assert( b->errCode || b->mem[b->used-1]=='\n' );
    //MARKER(("debugging buffer(rc=%d): len=%d <<%.*s>>\n", b->rc,
    //        (int)b->used, (int)b->used, (char const*)b->mem));
    if( !(rc = b->errCode) ){
      xf->config->debug.callback(xf, fsl_buffer_cstr(b));
      rc = xf->f->error.code;
    }
    fsl_buffer_reuse(b);
  }
  return rc;
}

int fsl_xfer_debugf(fsl_xfer * const xf, char const * zFmt, ...){
  int rc = 0;
  if( xf->config->debug.callback ){
    va_list args;
    va_start(args,zFmt);
    rc = fsl_xfer_debugfv(xf, zFmt, args);
    va_end(args);
  }
  return rc;
}

static char const * fsl__xfer_projcode(fsl__xfer * const xf){
  char const * z = 0;
  if( 0==xf->rc ){
    int const rc = fsl_repo_project_code(xf->f, &z);
    if( FSL_RC_NOT_FOUND==rc ){
      fsl_cx_err_reset(xf->f);
    }
  }
  return z;
}

/**
   Populates xf->buf.scratch with a saved-password lookup key for xf's
   URL. Returns &xf->buf.scratch on success, NULL on allocation error.
   If pUrl is not NULL, then on success *pUrl points to the part of
   the output buffer immediately after the "libfossil-sync-pw:"
   prefix.
*/
static fsl_buffer * fsl__xfer_pw_key(fsl__xfer * const xf, char const **pUrl){
  assert( !xf->rc );
  fsl_buffer * const b = fsl_buffer_reuse(&xf->buf.scratch);
  fsl_buffer_appendf(b, "libfossil-sync-pw:", b);
  fsl_url_render(&xf->ch->self(xf->ch)->url, b,
                 FSL_URL_RENDER_NO_PASSWORD);
  xf->rc = b->errCode;
  if( pUrl ){
    if( xf->rc ){
      *pUrl = 0;
    }else{
      *pUrl = strstr(fsl_buffer_cstr(b),":");
      assert( *pUrl );
      ++*pUrl /* skip the colon */;
    }
  }
  return b->errCode ? NULL : b;
}

/**
   Returns true if xf->user.password is either "" or "?", else returns
   false.
*/
static inline bool fsl__xfer_pw_wants_prompt(fsl_xfer const * const xf){
  return xf->user.password &&
    (!*xf->user.password || 0==fsl_strcmp("?", xf->user.password));
}

/**
   Tries to find a saved password for xf's URL. If found, it's
   assigned to xf->user.passwordToFree and xf->user.password, else
   xf->user.passwordToFree will be 0 and xf->user.password will be
   unmodified. It ignores db-level and search errors.
*/
static void fsl__xfer_pw_lookup(fsl__xfer * const xf, bool usePrompt){
  /** Look for a password hash in the login table. */
  if( xf->rc ) return;
  char const *zUrl = 0;
  fsl_buffer * const bKey = fsl__xfer_pw_key(xf, &zUrl);
  if( !bKey ) return;
  char *zPW = 0;

  fsl_free( xf->user.passwordToFree );
  xf->user.passwordToFree = 0;

  //MARKER(("xf->user.password=[%s] usePrompt=%d\n",
  //xf->user.password, (int)usePrompt));
  if( usePrompt ){
    if( !xf->config->password.callback ){
      fsl_xfer_errorf(xf, FSL_RC_MISUSE,
                      "Password prompt was requested for [%s] "
                      "but no password-collection callback was "
                      "provided.", zUrl);
      return;
    }
    goto prompt;
  }

  if( 0!=(xf->rc = fsl_config_open(xf->f, NULL)) ){
    return;
  }

  zPW = fsl_config_get_text(xf->f, FSL_CONFDB_GLOBAL,
                            fsl_buffer_cstr(bKey), NULL);
  if( !zPW ){
    fsl_cx_err_reset(xf->f)
      /* clear FSL_RC_NOT_FOUND. And any other error, for that matter,
         but a password lookup failure is not sync-fatal at this point
         in the process. */;
    fsl_db * const db = fsl_cx_db_repo(xf->f);
    zPW = db
      ? fsl_db_g_text(
          db, NULL,
          "SELECT pw FROM user WHERE login=%Q"
          " AND login NOT IN"
          " ('anonymous','nobody','developer','reader')",
          xf->user.name)
      : 0;
  }
  if( zPW ){
    xf->user.password = xf->user.passwordToFree = zPW;
    fsl__xfer_emitf(xf, FSL_MSG_INFO,
                   "Using saved password for [%b]", bKey);
    xf->config->password.save = false
      /* So fsl__xfer_pw_save() does not re-save it */;
  }else if( xf->config->password.callback ){
  prompt: ;
    fsl_buffer bpw = fsl_buffer_empty;
    xf->user.password = 0;
    xf->rc =
      xf->config->password.callback(xf->f, zUrl, &bpw,
                                    xf->config->password.state);
    if( 0==xf->rc ){
      xf->user.password = xf->user.passwordToFree =
        (char*)bpw.mem/*transfer ownership*/;
    }else{
      fsl_buffer_clear(&bpw);
    }
  }
  //MARKER(("xf->user.password=[%s]\n", xf->user.password));
}

/**
   If xf->user.name is not empty and xf->user.password is, try to
   fetch the password, else this is a no-op.
*/
static void fsl__xfer_pw_setup(fsl__xfer * const xf){
  if( 0==xf->rc && xf->user.name ){
    //char const * const zPc = fsl__xfer_projcode(xf);
    bool const usePrompt = fsl__xfer_pw_wants_prompt(xf);
    if( !xf->user.password || usePrompt ){
      fsl__xfer_pw_lookup(xf, usePrompt);
    }
  }
}

static int fsl__xfer_pw_save(fsl__xfer * const xf){
  if( 0==xf->rc
      && xf->config
      && xf->config->password.save
      && xf->user.name
      && xf->user.password
      && xf->ch
      && 0==fsl_config_open(xf->f, NULL) ){
    char const * myPC = fsl__xfer_projcode(xf);
    if( myPC ) {
      fsl_buffer * const b = fsl__xfer_pw_key(xf, NULL);
      char * zPW = b
        ? fsl_sha1_shared_secret2(xf->f, xf->user.name,
                                  xf->user.password, myPC)
        : NULL;
      if( zPW ){
        xf->rc = fsl_config_set_text(xf->f, FSL_CONFDB_GLOBAL,
                                     fsl_buffer_cstr(b), zPW);
        if( 0==xf->rc ){
          /* Reminder to self: in dry-run testing this won't
             be saved. */
          fsl__xfer_emitf(xf, FSL_MSG_INFO,
                         "Password saved under [%b]", b);
        }
      }/* else: we're going to treat this as innocuous for now */
      fsl_free(zPW);
    }/* else: without a project code we can't save the password, as
        the project code is part of the hash. */
  }
  return xf->rc;
}

static int fsl__xfer_init_url(fsl__xfer * const xf){
  fsl_xfer_config * const xc = xf->config;
  fsl_url * const url = &xf->ch->self(xf->ch)->url;
  assert( 0==xf->rc );
  xf->rc = fsl_url_parse( url, xc->url );
  if( xf->rc ){
    return fsl_xfer_errorf(xf, xf->rc, "Error %R parsing URL: %s",
                           xf->rc, xc->url);
  }
  if( 0==xf->rc && xf->config->listener.callback ){
    fsl_buffer * const bUrl = fsl_buffer_reuse(&xf->buf.scratch);
    fsl_url_render(url, bUrl, FSL_URL_RENDER_NO_PASSWORD);
    fsl__xfer_emitf(xf, FSL_MSG_INFO, "Sync URL: %b", bUrl);
  }
  xf->user.name = url->username;
  xf->user.password = url->password;
  return xf->rc;
}

/**
   (Re)creates temp tables used by xf.
*/
static int fsl__xfer_init_tables(fsl__xfer *xf){
#if 0
  if( 0==xf->rc ){
    xf->rc = fsl_cx_exec_multi(xf->f,
                               "DROP TABLE IF EXISTS temp.xfigot;"
                               "DROP TABLE IF EXISTS temp.xfgimme;"
                               "CREATE TEMP TABLE xfigot"
                               "(rid INTEGER PRIMARY KEY);"
                               "CREATE TEMP TABLE xfgimme"
                               "(uuid TEXT NOT NULL, CHECK( length(uuid)>=40 ));",
                               //"INSERT INTO temp.xfigot SELECT rid FROM %!Q.blob;",
                               /* TODO: delay the INSERT until needed
                                  (if it's needed at all). We don't need it unless
                                  we're going to deal with igots from the remote. */
                               fsl_db_role_name(FSL_DBROLE_REPO));
  }
#endif
  return xf->rc;
}

/**
   Initializes xf's core-most state. Returns 0 on success. On
   error, the sync must be aborted.

   Even on failure, fsl__xfer_cleanup() must eventually be called.

   For round-trip connections (cloning), this must be once at the
   onset and fsl__xfer_start() must be called at the start of each
   round-trip, before attempting to create a request payload.
*/
int fsl__xfer_setup(fsl_cx * const f,
                   fsl__xfer * const xf,
                   fsl_xfer_config * xc,
                   fsl_sc * const channel){
  assert( !channel->xfer );
  assert( channel->self );
  xf->f = f;
  xf->rc = 0;
  xf->ch = channel;
  channel->xfer = xf;
  xf->config = xc;
  fsl_sc * const chSelf = channel->self(channel);
  if( xc->compressTraffic ) chSelf->flags |= FSL_SC_F_COMPRESSED;
  else chSelf->flags &= ~FSL_SC_F_COMPRESSED;
  if( xc->leaveTempFiles ) chSelf->flags |= FSL_SC_F_LEAVE_TEMP_FILES;
  else chSelf->flags &= ~FSL_SC_F_LEAVE_TEMP_FILES;

  if( !xc->listener.callback ){
    xc->listener = f->cxConfig.listener;
  }

  fsl__xfer_init_url(xf);
  /*
    Reminder to self: we can't call fsl__xfer_init_tables(xf) from here
    because f is, in the case of a clone op, not yet opened.
  */
  if( 0==xf->rc ){
    /* Alloc errors here can be ignored. They'll be caught later if
       they continue to happen. */
    fsl_buffer_reserve(&xf->line.b, 128);
    fsl_buffer_reserve(&xf->line.bRaw, xf->line.b.capacity);
    fsl_buffer_reserve(
      &xf->ch->requestHeaders,
      (xf->ch->flags & FSL_SC_F_REQUEST_HEADERS) ? 1024 : 256
      /* We (ab)use this in fsl__xfer_append_prologue() even if
         !(xf->ch->flags & FSL_SC_F_REQUEST_HEADERS). */
    );
    fsl_buffer_reserve(&xf->line.bRaw, xf->line.b.capacity);
    fsl_buffer_reserve(&xf->buf.hash, FSL_STRLEN_SHA1+1);
    fsl_buffer_reserve(fsl_buffer_reuse(&xf->buf.emit), 160);
    if( fsl__xfer_projcode(xf) ){
      fsl__xfer_pw_setup(xf);
    }
  }
  return xf->rc
    ? xf->rc
    : (xf->rc = xf->ch->init(xf->ch, FSL_SC_INIT_INITIAL));
}

/** Modes for use with fsl__xfer_start(). */
enum fsl__xfer_e {
  /** (Re)Initialize the response-related state. */
  FSL__XFER_START_RESPONSE = 0x1000,
  /** (Re)Initialize the request-related state. */
  FSL__XFER_START_REQUEST  = 0x2000
};

#define fsl__xfer__check_rc if(xf->rc) return xf->rc

static int fsl__xfer_start(fsl__xfer * const xf, enum fsl__xfer_e initMode){
  fsl__xfer__check_rc;
  assert( xf->ch );
  assert( xf->f );

  switch( initMode ){
    case FSL__XFER_START_RESPONSE:
      xf->response.httpVersion = xf->response.httpCode = 0;
      fsl_buffer_reuse(&xf->buf.uresp);
      fsl_buffer_reserve(fsl_buffer_reuse(&xf->line.b), 160);
      fsl_buffer_reserve(fsl_buffer_reuse(&xf->line.bRaw),
                         xf->line.b.capacity);
      fsl_sc_cleanup(&xf->chz);
      xf->chz = fsl_sc_empty;
      break;
    case FSL__XFER_START_REQUEST:
      ++xf->n.trips;
      xf->request.nBytes = 0;
      fsl_sha1_init(&xf->request.sha1);
      fsl__xfer_init_tables(xf);
      break;
#if !defined(NDEBUG)
    default: /* can't happen */
      fsl__fatal(FSL_RC_ERROR, "Unhandled fsl__xfer_start() initMode");
#endif
  }
  //MARKER(("init mode=%02x rc=%d\n", mode, xf->rc));
  return xf->rc;
}


/** @internal

   Cleans up all state owned by xf (which must not be NULL).  If
   xf->ch is not 0, xf->ch->close() is called.
*/
static void fsl__xfer_cleanup(fsl__xfer * const xf, int theRc){
  if( 0==theRc ){
    fsl__xfer_pw_save(xf);
  }
  fsl_sc_cleanup(xf->ch);
#define fbc fsl_buffer_clear
  fbc(&xf->line.b);
  fbc(&xf->line.bRaw);
  fbc(&xf->buf.uresp);
  fbc(&xf->buf.hash);
  fbc(&xf->buf.cardPayload);
  fbc(&xf->buf.scratch);
  fbc(&xf->buf.emit);
  fbc(&xf->request.loginCard);
#undef fbc
  fsl_free( xf->user.passwordToFree );
#define ST(M) fsl_stmt_finalize(&xf->q.M)
  ST(igotInsert);
  ST(igotHasRid);
  ST(gimmeInsert);
  //ST(gimmeDelete);
  ST(remoteUnkUuid);
#undef ST
  if( xf->config && xf->config->metrics.callback ){
    xf->ch = 0;
    xf->user.name = xf->user.password = xf->user.passwordToFree = 0;
    xf->config->metrics.callback( &xf->n, xf->config->metrics.state,
                                  theRc ? theRc : xf->rc );
  }
  *xf = fsl__xfer_empty;
}

static inline void fsl__xfer_countRead( char mode, fsl_size_t * const pOut, fsl_size_t n ){
  /*MARKER(("update '%c' read: %u + %u = %u\n", mode, (unsigned) n, (unsigned)*pOut,
    (unsigned)(n + *pOut)));*/
  (void)mode;
  *pOut += n;
}

#define fsl__xfer__rch_decl \
  fsl_sc * const rch = xf->chz.xfer ? &xf->chz : xf->ch

static int fsl__xfer_read(fsl__xfer * const xf, fsl_buffer * const dest,
                          fsl_size_t n){
  if( 0==xf->rc ){
    assert( n > 0 );
    fsl__xfer__rch_decl;
    fsl_cx_err_reset(xf->f);
    fsl_timer_scope(&xf->n.timer.read,{
        xf->rc = rch->read(rch, dest, (fsl_int_t)n);
      });
    if( 0==xf->rc ){
      fsl__xfer_countRead('u', &xf->n.bytesReadUncompressed, n);
    }
  }
  return xf->rc;
}

static int fsl__xfer_readln(fsl__xfer * const xf){
  fsl_buffer * const bLn = &xf->line.b;
  fsl_buffer * const bLnRaw = &xf->line.bRaw;
  fsl__xfer__check_rc;
  fsl__xfer__rch_decl;
again: ;
  fsl_timer_scope(&xf->n.timer.read,{
      xf->rc = rch->read(rch, fsl_buffer_reuse(bLn), FSL_SC_READ_LINE);
    });

  /*MARKER(("rc=%d line=<<%.*s>>\n", rc,
    (int)(bLn->used ? bLn->used-1 : 0), (char const *)bLn->mem));*/
  if( 0==xf->rc ){
    fsl__xfer_countRead('u', &xf->n.bytesReadUncompressed, bLn->used);
    //assert( bLn->used );
    if( !bLn->used ){
      /* This shouldn't be possible at this point but we could account
         here for a broken fsl_sc impl which doesn't return EOF
         properly. */
      fsl_xfer_errorf(xf, FSL_RC_RANGE, "No more lines in input");
    }else if( '\n'!=bLn->mem[bLn->used-1] ){
      fsl_xfer_error(xf, FSL_RC_SYNTAX,
                     "Input does not end with a newline");
    }else if( 1==bLn->used ){
      /* Skip blank lines */
      assert( '\n'==bLn->mem[0] );
      /* Empty line - skip it */
      goto again;
    }else if( '#'==bLn->mem[0] ){
      /* Comment lines - skip it */
      if( 0 ){
        fsl_xfer_debugf(xf, "%s() got comment line:\n%b",
                      __func__, bLn);
      }
      goto again;
    }else{
      /** Populate xf->line.aToken and aTokLen */
      fsl__tokenizer src = {
        .z = bLn->mem, .zEnd = bLn->mem + bLn->used, .atEol = false
      };
      xf->line.nToken = 0;
      fsl_buffer_reuse(bLnRaw);
      fsl_buffer_append( bLnRaw, bLn->mem, (fsl_int_t)bLn->used )
        /* Alloc failure here should be benign. */;
      /*MARKER(("read line = <<%.*s>>\n",
        (int)bLn->used, (char const *)(bLn->mem)));*/
      while( (xf->line.nToken < fsl__xfer_max_tokens)
             && 0!=(xf->line.aToken[xf->line.nToken] =
                    fsl__tokenizer_next(
                      &src, &xf->line.aTokLen[xf->line.nToken]
                    )) ){
        ++xf->line.nToken;
      }
      assert( xf->line.nToken != fsl__xfer_max_tokens );
      if( !src.atEol ) {
        xf->rc = fsl_cx_err_set(xf->f, FSL_RC_RANGE,
                                     "Unexpected stuff after token #%d: %b",
                                     (int)xf->line.nToken+1,
                                     bLnRaw);
      }
    }
  }
  return xf->rc;
}

/**
   Internal helper which uses src as a fsl_sc read source.
*/
static int fsl__xfer_rdbuf(fsl_sc * const ch, fsl_buffer * const tgt,
                          fsl_buffer * const src, fsl_int_t howMuch){
  int rc = FSL_RC_CANNOT_HAPPEN;
  assert( 0==ch->xfer->rc );
  switch( howMuch ){

    case FSL_SC_READ_ALL:
      fsl_buffer_swap(src, tgt);
      rc = 0;
      break;

    case FSL_SC_READ_LINE:{
      fsl_size_t const cu = src->cursor;
      rc = fsl_buffer_copy_lines(tgt, src, 1);
      if( !rc && cu==src->cursor ){
        /* End of input */
        rc = FSL_RC_BREAK;
      }
      break;
    }

    default:
      assert( howMuch>0 );
      if( src->cursor + howMuch > src->used ){
        rc = fsl_xfer_errorf(ch->xfer, FSL_RC_RANGE,
                             "Not enough buffered to read %" FSL_INT_T_PFMT
                             " bytes");
      }else{
        rc = fsl_buffer_append(tgt, src->mem+src->cursor, (fsl_size_t)howMuch);
        if( 0==rc ){
          src->cursor += howMuch;
        }
      }
      break;
  }
  return rc;
}

static int fsl__rbuf_xRead(fsl_sc * const ch, fsl_buffer * tgt, fsl_int_t howMuch){
  return fsl__xfer_rdbuf(ch, tgt, &ch->xfer->buf.uresp, howMuch);
}

static fsl_sc * fsl__rbuf_xSelf(fsl_sc *ch){
  assert( ch->xfer->ch );
  assert( ch->xfer->ch != ch );
  return ch->xfer->ch;
}

/**
   A partial (read()-only) fsl_sc impl to support fsl__xfer's ability
   to read fossil-compressed responses.
 */
const fsl_sc fsl_sc__xfer_rbuf = {
  .xfer = 0,
  .impl = {.p = 0, .type= &fsl__xfer_empty},
  .name = "fsl__xfer:rbuf",
  .init  = 0,
  .append  = 0,
  .submit  = 0,
  .read = fsl__rbuf_xRead,
  .cleanup = 0,
  .self = fsl__rbuf_xSelf
};

/**
   Guesses whether xf's state is such that a pending request may
   include login info.
*/
static inline bool fsl__xfer_may_attempt_login(fsl__xfer * const xf){
  if(xf->rc
     || !xf->user.password
     || !xf->f->cache.projectCode
     || xf->clientVersion.projCodeMatch<=0
     || !xf->user.name || !*xf->user.name
     || 0==fsl__xfer_projcode(xf)
     || 0==fsl_strcmp("anonymous",xf->user.name)
     || 0==fsl_strcmp("nobody",xf->user.name)) return false;
  return true;
}

/**
   May construct a login card for xf->user, (re)populating
   xf->request.loginCard with the result. Returns 0 on success. On
   success it may or may not populate the login card, depending on
   $REASONS. It is not an error for no login card to be created, but
   it will mean that sync operations which require remote write acces
   won't work.

   Login cards look like:

     login LOGIN NONCE SIGNATURE

   The LOGIN is xf->user.name.  NONCE is the SHA1 checksum of all
   request payload (stored in xf->buf.hash).  SIGNATURE is the SHA1
   checksum of the NONCE followed by xf->user.password (or, if the
   password is not an itself an SHA1 hash, its fossil-encrypted form).
*/
static int fsl__xfer_build_login_card(fsl__xfer * const xf){
  fsl__xfer__check_rc;
  fsl_buffer const * const nonce = &xf->buf.hash;
  fsl_buffer * const sig = fsl_buffer_reuse(&xf->buf.scratch);
  char const * const zPw = xf->user.password;
  fsl_size_t nPw = 0;
  int rc = 0;

  fsl_buffer_reuse(&xf->request.loginCard);
#if !defined(NDEBUG)
  assert( fsl__xfer_may_attempt_login(xf) );
#else
  if( ! fsl__xfer_may_attempt_login(xf) ) return 0;
#endif
  assert( FSL_STRLEN_SHA1 == nonce->used );
  assert( zPw );
  nPw = fsl_strlen(zPw);
  /* At this point fossil(1) also returns 0 for ssh:// URLs. */
  assert( nonce->used == FSL_STRLEN_SHA1 /* payload hash */ );

  fsl_buffer_copy(sig, nonce);

  /* The login card wants the SHA1 hash of the password (as computed by
  ** fsl_sha1_shared_secret2()), not the original password.  So convert the
  ** password to its SHA1 encoding if it isn't already a SHA1 hash.
  **
  ** We assume that a hexadecimal string of exactly 40 characters is a
  ** SHA1 hash, not an original password.  If a user has a password which
  ** just happens to be a 40-character hex string, then this routine won't
  ** be able to distinguish it from a hash, the translation will not be
  ** performed, and the sync won't work.
  */
  if( sig->errCode ) goto end;
  assert( sig->used == FSL_STRLEN_SHA1 /* payload hash */ );
  if( (FSL_STRLEN_SHA1!=nPw
       || FSL_HTYPE_ERROR==fsl_validate_hash(zPw, FSL_STRLEN_SHA1)) ){
    char const *zPC = fsl__xfer_projcode(xf)
      /* TODO: a way to tell it to use some other project code,
         e.g. parent-project-code. */;
    assert( zPC );
    char * zSSS = fsl_sha1_shared_secret2(xf->f, xf->user.name, zPw, zPC);
    if( !zSSS ){
      if( xf->f->cache.projectCode ){
        fsl_report_oom;
      }else{
        assert(!"Not an OOM. Probably missing project-code");
      }
      rc = FSL_RC_OOM;
      goto end;
    }
    /* TODO: optionally save pw. See fossil(1)
       src/http.c:http_build_login_card(). We need a fsl__xfer flag
       to tell us whether this is okay. */
    assert( !zSSS[FSL_STRLEN_SHA1] && !!zSSS[FSL_STRLEN_SHA1-1] );
    fsl_buffer_append(sig, zSSS, FSL_STRLEN_SHA1);
    //fsl_buffer_append(sig, zSSS, -1);
    fsl_free( zSSS );
  }else{
    assert( zPw );
    fsl_buffer_append(sig, zPw, nPw);
  }

  if( 0==(rc = sig->errCode) ){
    fsl_sha1sum_buffer(sig, sig)
      /* Cannot fail - no alloc needed */;
    rc = fsl_buffer_appendf(&xf->request.loginCard, "login %F %b %b",
                            xf->user.name, nonce, sig);
  }

end:
  return xf->rc = rc;
}


static int fsl__xfer_prepare_req_headers(fsl__xfer * const xf){
  fsl__xfer__check_rc;
  unsigned char digest[FSL_STRLEN_SHA1/2];
  char hex[FSL_STRLEN_SHA1+1];
  bool const doLogin = fsl__xfer_may_attempt_login(xf);
  fsl_buffer * const bHash = fsl_buffer_reuse( &xf->buf.hash );
  fsl_buffer_reuse( &xf->request.loginCard );
  if( doLogin ){
    /* Add some entropy to the request, as a comment line, before
       submitting it so that the login-card nonce is different for
       otherwise identical requests. We don't do this for
       unauthenticated requests because they're not need to harden
       those this way, as the login nonce won't be used. */
    fsl_randomness(sizeof(digest), digest);
    fsl_to_hex(digest, sizeof(digest), hex);
    fsl__xfer_appendlnf(xf, "# entropy %s", hex);
  }

  { /* Add a # timestamp comment line. We really only need this in
       server mode, as it's the client who detects time skews.
       Potential TODO: simplify this in fossil to use a Unix-epoch
       time instead of ISO-8601. And it really should be its own
       card, even if it has to be a pragma for backwards compatibility
       (unknown pragmas are ignored). */
    char tmStamp[30];
    time_t const tt = time(0);
    fsl_strftime( tmStamp, sizeof(tmStamp), "%Y-%m-%dT%H:%M:%S", gmtime(&tt) );
    fsl__xfer_appendlnf(xf, "# timestamp %s", tmStamp);
    fsl__xfer_appendcard(xf, fsl_xfcard_e_pragma, "time %" FSL_TIME_T_PFMT, (fsl_time_t)tt);
  }

  if( doLogin && 0==xf->rc ){
    fsl_sha1_final(&xf->request.sha1, digest);
    xf->rc = fsl_buffer_reserve(bHash, FSL_STRLEN_SHA1+1);
    if( 0==xf->rc ){
      fsl_sha1_digest_to_base16(digest, (char *)bHash->mem);
      bHash->used = FSL_STRLEN_SHA1;
      bHash->mem[bHash->used] = 0;
    }
  }
  if( xf->rc ) goto end;
  assert( bHash->used == (doLogin ? FSL_STRLEN_SHA1 : 0) );
  if( 0 ){
     MARKER(( "Request payload size=%" FSL_SIZE_T_SFMT "  sha1=%s\n",
             xf->request.nBytes, fsl_buffer_cstr(bHash) ));
  }

  if( bHash->used && fsl__xfer_build_login_card(xf) ){
    goto end;
  }
  //MARKER(("Login card=%s\n", fsl_buffer_cstr(&xf->request.loginCard)));
  fsl_sc * const chSelf = xf->ch->self(xf->ch);
  if( FSL_SC_F_REQUEST_HEADERS & chSelf->flags ){
    /* Emit HTTP headers to chSelf->requestHeaders */
    fsl_url const * const url = &chSelf->url;
    fsl_buffer * const b = fsl_buffer_reuse( &chSelf->requestHeaders );
#define fba fsl_buffer_append
#define fbaf fsl_buffer_appendf
#define CRNL "\r\n"
    fbaf(b, "POST %s HTTP/1.0" CRNL,
         url->path && *url->path ? url->path : "/" );
    if( url->port>0 ){
      fbaf(b, "Host: %s:%d" CRNL, url->host ? url->host : "localhost",
           url->port);
    }else{
      fbaf(b, "Host: %s" CRNL, url->host ? url->host : "localhost" );
    }
    if( 1 ){
      fbaf(b, "X-Fossil-Client: libfossil %s" CRNL,
           FSL_LIBRARY_VERSION);
    }
    fba(b, (FSL_SC_F_COMPRESSED & xf->ch->flags)
        ? "Content-Type: application/x-fossil" CRNL
        : "Content-Type: application/x-fossil-uncompressed" CRNL,
        -1);
    if( xf->request.loginCard.used ){
      fbaf(b, "Cookie: x-f-l-c=%t\r\n",
           fsl_buffer_cstr(&xf->request.loginCard));

    }
    fbaf(b, "Content-Length: %" FSL_SIZE_T_PFMT CRNL,
         xf->request.nBytes);
    fba(b, CRNL, 2);
    if( xf->config->verbosity ){
      MARKER(("FSL_SC_F_REQUEST_HEADERS:\n"));
      fsl_output(xf->f, b->mem, b->used);
    }
    xf->rc = b->errCode;
  }
end:
  return xf->rc;
#undef CRNL
#undef fba
#undef fbaf
}

/**
   Parse xf's HTTP response headers. Write the Content-Length to
   *contentLength. Set *expectCompressedResponse to be true if the
   response body is expected to be compressed, false if it's not.

   0 on success and all that. This is a no-op if xf->rc
   and this always sets xf->rc on error.
*/
static int fsl__xfer_parse_http_headers( fsl__xfer * const xf,
                                         fsl_size_t * contentLength,
                                         /* Content-Length HTTP header value */
                                         bool * expectCompressedResponse ){
  /* Consume input until the end of the headers (\r\n\r\n).  Look
     for HTTP errors and the like. */
  fsl__xfer__check_rc;
  fsl__xfer__rch_decl;
  fsl_buffer * const bLine = &xf->line.b;
  fsl_size_t nLine = 0;
  int hc = 0;
  *contentLength = 0;
  while( 1 ){
    fsl_timer_scope(&xf->n.timer.read,{
        xf->rc = rch->read(rch, fsl_buffer_reuse(bLine),
                                FSL_SC_READ_LINE)
          /* reminder to self: don't use fsl__xfer_readln() because
             it does validation and empty line skipping which is not
             appropriate here. */;
      });
    if( FSL_RC_BREAK==xf->rc ){
      fsl_xfer_error(xf, FSL_RC_SYNTAX, "Empty HTTP response");
    }
    if( xf->rc ) break;
    ++nLine;
    fsl__xfer_countRead('u', &xf->n.bytesReadUncompressed,
                        bLine->used);
    if( 2==bLine->used
        && (unsigned char)'\r'==bLine->mem[0]
        && (unsigned char)'\n'==bLine->mem[1] ){
      break;
    }
    fsl_xfer_debugf(xf, "Response HTTP header #%d: %b",
                    (int)nLine, bLine);
    char const * const zLine = fsl_buffer_cstr(bLine);
    if( xf->config->verbosity ){
      fsl_xfer_debugf(xf, "Response header #%d: %b",
                    (int)nLine, bLine);
    }
    if( 1==nLine ){
      if( 2!=sscanf(zLine, "HTTP/1.%d %d",
                    &xf->response.httpVersion,
                    &xf->response.httpCode) ){
        fsl_xfer_error(xf, FSL_RC_SYNTAX,
                       "Expecting \"HTTP/1.X Y\" in the "
                       "first HTTP result line");
        break;
      }
      hc = xf->response.httpCode;
      if( hc!=200 && hc!=301 && hc!=302 && hc!=307 && hc!=308 ){
        int ii; /* Skip HTTP/1.X prefix */
        for(ii=7; zLine[ii] && zLine[ii]!=' '; ii++){}
        while( zLine[ii]==' ' ) ii++;
        fsl_xfer_errorf(xf, FSL_RC_ERROR,
                        "Error #%d from remote: %s",
                        hc, &zLine[ii]);
        break;
      }
      continue;
    }/*1==nLine*/
    if( hc==301 || hc==302 || hc==307 || hc==308 ){
      //fsl_strnicmp(zLine, "location:", 9)==0
      fsl_xfer_errorf(xf, FSL_RC_NYI,
                      "HTTP result #%d is not currently supported",
                      hc);
      break;
    }
    if(!*contentLength && bLine->used>16
       && 0==fsl_strnicmp("content-length: ", zLine, 16)
       && 1!=sscanf(zLine + 16, "%" FSL_SIZE_T_SFMT, contentLength) ){
      fsl_xfer_errorf(xf, FSL_RC_SYNTAX,
                      "Malformed content-length header: %s",
                      zLine);
      break;
    }
    if( bLine->used>=34
        && 0==fsl_strnicmp("content-type: application/x-fossil", zLine, 34) ){
      /* x-fossil-uncompressed or x-fossil-debug */
      *expectCompressedResponse = !(strstr( &zLine[34], "uncompressed" )
                                    || strstr( &zLine[34], "debug" ));
    }
  }/* while line */
  if( 0==xf->rc ){
    if( 200!=hc ){
      fsl_xfer_errorf(xf, FSL_RC_ERROR,
                      "HTTP error response #%d", hc);
    }else if( !*contentLength ){
      fsl_xfer_errorf(xf, FSL_RC_RANGE,
                           "No Content-Length found in response headers");
    }
  }
  return xf->rc;
}

/**
   fsl_buffer_uncompress(b) in place.
*/
static int fsl__xfer_unz(fsl__xfer * const xf, fsl_buffer * const b){
  if( 0==xf->rc ){
    int rc;
    fsl_timer_scope(&xf->n.timer.uncompress, {
        rc = fsl_buffer_uncompress(b, b);
      });
    if( rc ){
      fsl_xfer_errorf(xf, rc, "Error decompressing blob");
    }
  }
  return xf->rc;
}
/**
   Expects xf->ch to have responded with a compressed response.  We
   read the whole thing, decompress it, and install a small proxy
   fsl_sc to handle the reading from that point on so that concrete
   fsl_sc impls don't need to know how to deal with
   that. contentLength must be the response body length.
*/
static int fsl__xfer_response_unz(fsl__xfer * const xf,
                                  fsl_size_t contentLength){
  fsl_buffer * const b = fsl_buffer_reuse(&xf->buf.uresp);
  assert( 0==xf->rc );
  if( !contentLength ) return 0;
  /**
     Set up xf->chz with a partial fsl_sc impl which uses
     xf->buf.uresp as state and supports (only) the fsl_sc::read()
     API. Populate xf->buf.uresp with the uncompressed response
     body. fsl__xfer_read() and fsl__xfer_readln() will use
     xf->chz in place of xf->ch.
  */
  fsl_timer_scope(&xf->n.timer.read,{
      xf->rc = xf->ch->read(xf->ch, b, FSL_SC_READ_ALL);
    });
  //MARKER(("xf->rc=%s compressed size=%u\n",fsl_rc_cstr(xf->rc),
  //  (unsigned)b->used));

  if( 0==xf->rc ){
    fsl__xfer_countRead('c', &xf->n.bytesReadCompressed,
                        b->used);
    if( 0==fsl__xfer_unz(xf, b) ){
      if( xf->n.largestDecompressedResponse < b->capacity ){
        xf->n.largestDecompressedResponse = b->capacity;
      }
      /* Install a fsl_sc proxy which reads the response content from
         b. */
      xf->chz = fsl_sc__xfer_rbuf;
      xf->chz.xfer = xf;
      xf->chz.impl.p = b;
      xf->chz.impl.type = &fsl_sc__xfer_rbuf;
    }
  }
  return xf->rc;
}

static int fsl__xfer_parse_payload(fsl__xfer * const xf);

int fsl__xfer_submit( fsl__xfer * const xf ){
  fsl__xfer__check_rc;
  fsl_size_t contentLength = 0 /* Content-Length HTTP header value */;
  bool expectCompressedResponse = (3==xf->config->op.clone)
    ? false
    : 0!=(FSL_SC_F_COMPRESSED & xf->ch->flags);

  fsl__xfer_prepare_req_headers(xf);
  fsl__xfer_start(xf, FSL__XFER_START_RESPONSE);
  if( 0==xf->rc ){
    fsl_timer_scope(&xf->n.timer.submit,{
        xf->rc = xf->ch->submit(xf->ch, xf->request.loginCard.used
                                     ? &xf->request.loginCard : NULL);
      });
    fsl__xfer_parse_http_headers( xf, &contentLength,
                                  &expectCompressedResponse );
    if( 0==xf->rc && contentLength && expectCompressedResponse ){
      fsl__xfer_response_unz(xf, contentLength);
    }
    if( 0==fsl__xfer_start(xf, FSL__XFER_START_REQUEST) ){
      fsl_timer_scope(&xf->n.timer.process,{
          fsl__xfer_parse_payload(xf);
        });
    }
  }
  return xf->rc;
}

/**
   Write n bytes of p to xf->ch and update xf's related state.
   Is a no-op if called when xf->rc is non-0.
*/
static void fsl__xfer_append_direct(fsl__xfer * const xf, void const * const p,
                                    fsl_size_t n){
  if( 0==xf->rc ){
    xf->rc = xf->ch->append(xf->ch, p, n);
    if( 0==xf->rc ){
      xf->n.bytesWritten += n;
      xf->request.nBytes += n;
      fsl_sha1_update(&xf->request.sha1, p, n);
    }
  }
}

/**
   Appends a prologue series of common cards to the request if the
   request is empty, else this is a no-op.  Is a no-op if called when
   xf->rc is non-0.

   This must only be called from fsl__xfer_append() and only when no
   content has been submitted to the request body. An assert() to that
   effect is in place.
*/
static void fsl__xfer_append_prologue(fsl__xfer * const xf){
  assert( !xf->request.nBytes );
  if( 0==xf->rc ){
    fsl_buffer * const b = fsl_buffer_reuse(
      &xf->ch->requestHeaders
      /* using requestHeaders here is a bit of a hack. We can't use
         xf->buf.scratch because this call is made in a context where
         that buffer is already in use. requestHeaders may be
         re-used via fsl__xfer_submit(). */
    );
    /*
      Reminder: we cannot call back into the other
      fsl__xfer_append...() from here or we'll get in a loop until one
      of the buffers gets too big to grow further or we stack
      overflow. The one exception is fsl__xfer_append_direct(), which
      will not loop.
    */
    char const * zPC = fsl__xfer_projcode(xf);
    fsl_buffer_appendf(b, "pragma client-version %d %d %d\n",
                       xf->clientVersion.number,
                       xf->clientVersion.date,
                       xf->clientVersion.time);
    ++xf->n.cardsTx[fsl_xfcard_e_pragma];
    if( zPC ){
      fsl_buffer_appendf(b, "pull ignored-servercode %s\n", zPC)
        /* fossil(1) ignores gimme cards if no pull card was sent, so
           pull card needs to come before any gimme cards are
           emitted. */;
      ++xf->n.cardsTx[fsl_xfcard_e_pull];
      if( fsl__xfer_may_attempt_login(xf) ){
        fsl_buffer_appendf(b, "push ignored-servercode %s\n", zPC);
        ++xf->n.cardsTx[fsl_xfcard_e_push];
      }
      xf->rc = b->errCode;
      fsl__xfer_append_direct(xf, b->mem, b->used);
      fsl_buffer_reuse(b);
    }
  }
}

/**
   Write n bytes of p to xf->ch and update xf's related state.
   Returns xf->rc. Is a no-op if called when xf->rc is
   non-0. Initializes xf->ch if we've not yet sent any output.
*/
int fsl__xfer_append(fsl__xfer * const xf, void const * const p,
                     fsl_size_t n){
  if( 0==xf->rc ){
    if( !xf->request.nBytes ){
      /* Delay re-init of the request state until we know we're going to
         post another request (i.e. when we start writing the
         request). */
      xf->rc = xf->ch->init(xf->ch, FSL_SC_INIT_REQUEST);
      fsl__xfer_append_prologue(xf);
    }
    fsl__xfer_append_direct(xf, p, n);
  }
  return xf->rc;
}

#if 0
int fsl__xfer_appendln(fsl__xfer * const xf, char const * zMsg){
  if( 0==xf->rc ){
    fsl_buffer * const b = fsl_buffer_reuse(&xf->buf.scratch);
    fsl_buffer_append(b, zMsg, -1);
    fsl_buffer_ensure_eol(b);
    if( 0==(xf->rc = b->errCode) ){
      fsl__xfer_append(xf, b->mem, b->used);
    }
  }
  return xf->rc;
}
#endif

int fsl__xfer_appendlnv(fsl__xfer * const xf, char const * zFmt, va_list args){
  if( 0==xf->rc ){
    fsl_buffer * const b = fsl_buffer_reuse(&xf->buf.scratch);
    fsl_buffer_appendfv(b, zFmt, args);
    fsl_buffer_ensure_eol(b);
    if( 0==(xf->rc = b->errCode) ){
      fsl__xfer_append(xf, b->mem, b->used);
    }
  }
  return xf->rc;
}

int fsl__xfer_appendlnf(fsl__xfer * const xf, char const * zFmt, ...){
  if( 0==xf->rc ){
    va_list args;
    va_start(args,zFmt);
    fsl__xfer_appendlnv(xf, zFmt, args);
    va_end(args);
  }
  return xf->rc;
}

int fsl__xfer_appendcard(fsl__xfer * const xf, fsl_xfcard_e card,
                         char const * zFmt, ...){
  if( 0==xf->rc ){
    fsl_buffer * const b = fsl_buffer_reuse(&xf->buf.scratch);
    char const * z = 0;
    fsl_int_t n = 0;
    switch( (int)card ){
#define E(T,LEN) case fsl_xfcard_e_ ## T: z = #T; n = LEN; break;
      fsl_xfcard_map(E)
#undef E
    }
    assert( z );
    assert( n );
    fsl_buffer_append(b, z, n);
    if( zFmt && *zFmt ){
      va_list args;
      fsl_buffer_appendch(b, ' ');
      va_start(args,zFmt);
      fsl_buffer_appendfv(b, zFmt, args);
      va_end(args);
    }
    fsl_buffer_ensure_eol(b);
    ++xf->n.sentCards;
    ++xf->n.cardsTx[card];
    if( 0==(xf->rc = b->errCode) ){
      fsl__xfer_append(xf, b->mem, b->used);
    }
  }
  return xf->rc;
}

static void fsl__xfer_phantom_new(fsl__xfer *xf, fsl_uuid_cstr zUuid,
                                 bool isPrivate, fsl_id_t *ridOut){
  if( 0==xf->rc ){
    xf->rc = fsl__phantom_new(xf->f, zUuid, isPrivate, ridOut);
  }
}

/**

   Most of what follows will eventually be moved into the library, and
   is currently in this file to expedite development and testing.
*/

/**
   Returns true if z is parsable as a 32-bit integer, else false.  If
   it returns true, *pVal is set to the parsed value.
*/
static bool fsl__is_int32(char const * z, int *pVal){
  char * zEnd = 0;
  long const l = strtol(z, &zEnd, 10);
  if( zEnd && !*zEnd && l==(long)((int)l)/*else overflow*/ ){
    *pVal = (int)l;
    return true;
  }
  return false;
}

/**
   Confirms whether integer-shaped value z is consistent with
   size-typed card arguments. If so, returns true and sets *pVal to
   the size. On error, returns false.
*/
static bool fsl__is_card_size(unsigned char const * z, bool zeroIsLegal,
                              fsl_size_t *pVal){
  int i = 0;
  bool rv = false;
  if( fsl__is_int32((char const *)z, &i) &&
      (zeroIsLegal ? i>=0 : i>0) ){
    *pVal = (fsl_size_t)i;
    rv = true;
  }
  return rv;
}

/**
   Extract xf->line.aToken[tokNdx] for use with fsl__is_card_size().
   If tokNdx is in range and the token is-a size then returns 0 and
   sets *pOut to the size. On error, updates xf->f's error state
   and returns non-0.
*/
static int fsl__xfer_card_size(fsl__xfer * const xf,
                                unsigned char tokNdx, bool zeroIsLegal,
                                fsl_size_t * pOut){
  if( 0==xf->rc ){
    if( tokNdx>=xf->line.nToken ){
      fsl_xfer_errorf(xf, FSL_RC_SYNTAX, "Malformed card: %b",
                      &xf->line.bRaw);
    }else if( !fsl__is_card_size(xf->line.aToken[tokNdx], zeroIsLegal, pOut) ){
      fsl_xfer_errorf(xf, FSL_RC_SYNTAX,
                      "Not a valid %s card size: %s",
                      xf->line.aToken[0],
                      xf->line.aToken[tokNdx]);
    }
  }
  return xf->rc;
}


static inline unsigned char fsl__xfer_ntok(fsl__xfer const * const xf){
  return xf->line.nToken;
}

static inline unsigned char * fsl__xfer_toku(fsl__xfer const * const xf,
                                             unsigned char tokNdx){
  return (tokNdx<fsl__xfer_ntok(xf))
    ? xf->line.aToken[tokNdx] : NULL;
}

static inline char * fsl__xfer_tokn(fsl__xfer const * const xf,
                                    unsigned char tokNdx){
  return (char *)fsl__xfer_toku(xf, tokNdx);
}

static char const * fsl__xfer_uchomp(unsigned char *z, fsl_size_t *n){
  while( *n>0 && (z[*n - 1]=='\n') ) z[--*n] = 0;
  z[*n] = 0;
  return (char const *)z;
}

static char const * fsl__xfer_tokn_defos(fsl__xfer const * const xf,
                                         unsigned char tokNdx){
  unsigned char * z =  fsl__xfer_toku(xf, tokNdx);
  if( z ){
    fsl_size_t n = 0;
    fsl_bytes_defossilize(z, &n)
      /* Fossil-ized error card messages sometimes have a trailing \n
         encoded in them. */;
    return fsl__xfer_uchomp(z, &n);
  }
  return 0;
}

/**
   Returns true if 0==xf->rc and xf->line.aToken[tokNdx] matches the
   given string.
*/
static inline bool fsl__xfer_tok_match(fsl__xfer const * const xf,
                                       unsigned char tokNdx,
                                       char const * zStr){
  return 0==xf->rc && 0==fsl_strcmp(fsl__xfer_tokn(xf, tokNdx), zStr);
}

/**
   If xf->line.nToken is within the range of minArg..maxArg, return
   true, else set xf's error state and return false.  Returns false if
   xf->rc is non-0 when this is called.
*/
static bool fsl__xfer_argc_check(fsl__xfer * const xf,
                                 fsl_xfcard const * const card,
                                 unsigned char minArg,
                                 unsigned char maxArg){
  if( 0==xf->rc
      && (fsl__xfer_ntok(xf)<minArg || fsl__xfer_ntok(xf)>maxArg) ) {
    fsl_xfer_errorf(xf, FSL_RC_SYNTAX,
                    "Expecting %d-%d token(s) in %s card, but got %d",
                    (int)minArg, (int)maxArg, card->name,
                    (int)fsl__xfer_ntok(xf));
  }
  return xf->rc==0;
}

static bool fsl__xfer_is_hash(fsl__xfer * const xf, unsigned char tokNdx){
  char const * const z = fsl__xfer_tokn(xf, tokNdx);
  return z
    && FSL_HTYPE_ERROR!=fsl_validate_hash(z, xf->line.aTokLen[tokNdx]);
}

/**
   If xf's error state is not set and xf token #tokNdx is not a valid
   SHA1 or SHA3 hash, set xf's error state to describe the
   problem. Returns xf->rc.
*/
static int fsl__xfer_check_card_hash(fsl__xfer * const xf,
                                     fsl_xfcard const * const card,
                                     unsigned char tokNdx){
  if( 0==xf->rc && !fsl__xfer_is_hash(xf, tokNdx) ){
    fsl_xfer_errorf(xf, FSL_RC_SYNTAX, "Invalid hash in %s card index %d",
                    card->name, (int)tokNdx);
  }
  return xf->rc;
}

/**
   Uncompresses xf->buf.cardPayload, expecting it to have a size
   equal to that set in xf->line.aToken[uszNdx].
*/
static int fsl__payload_unz(fsl__xfer * const xf, unsigned char uszNdx){
  fsl_buffer * const b = &xf->buf.cardPayload;
  if( 0==xf->rc ){
    fsl_size_t usz = 0;
    assert( uszNdx );
    if( 0==fsl__xfer_card_size(xf, uszNdx, true, &usz) ){
      if( usz ){
        /* Reminder: fossil(1) artifact da39a3ee5e6b4b0d3255bfef95601890afd80709
           has a usz=0 and sz=12. */
        fsl__xfer_unz(xf, b);
      }else{
        fsl_buffer_reuse(b);
      }
    }
  }
  return xf->rc;
}

/**
   Reads the payload for the current card line. The payload's size
   must be encoded in xf->line.aToken[szNdx]. If uszNdx is not 0 then
   the payload is assumed to be compressed, its uncompressed size is
   assumed to be in xf->line.aToken[uszNdx], and it is uncompressed
   in-place.

   The payload is placed in xf->buf.cardPayload.

   Sidebar: it is legal for the uncompressed version to be smaller
   than the compressed one. This will happen for all cards below a
   certain size.
*/
static int fsl__payload_read(fsl__xfer * const xf, unsigned char szNdx,
                             unsigned char uszNdx){
  if( 0==xf->rc ){
    fsl_buffer * const b = fsl_buffer_reuse(&xf->buf.cardPayload);
    fsl_size_t sz = 0;
    fsl__xfer_card_size(xf, szNdx, true, &sz);
    if( sz ){
      fsl__xfer_read(xf, b, sz);
      if( xf->n.largestCardPayload < sz ){
        xf->n.largestCardPayload = sz;
      }
      if( uszNdx ){
        fsl__payload_unz(xf, uszNdx);
      }
    }
  }
  return xf->rc;
}

static int fsl__rcv_config(fsl__xfer * const xf,
                           fsl_xfcard const * const card){
  /* From fossil:
  **
  ** Config card:
  **
  **   config NAME SIZE \n CONTENT
  **
  ** NAME is one of:
  **
  **     "/config", "/user",  "/shun", "/reportfmt", "/concealed",
  **     "/subscriber"
  **
  ** NAME indicates the table that holds the configuration information being
  ** transferred.  pContent is a string that consist of alternating Fossil
  ** and SQL tokens.  The First token is a timestamp in seconds since 1970.
  ** The second token is a primary key for the table identified by NAME.  If
  ** The entry with the corresponding primary key exists and has a more recent
  ** mtime, then nothing happens.  If the entry does not exist or if it has
  ** an older mtime, then the content described by subsequent token pairs is
  ** inserted.  The first element of each token pair is a column name and
  ** the second is its value.
  **
  ** In overview, we have:
  **
  **    NAME        CONTENT
  **    -------     -----------------------------------------------------------
  **    /config     $MTIME $NAME value $VALUE
  **    /user       $MTIME $LOGIN pw $VALUE cap $VALUE info $VALUE photo $VALUE
  **    /shun       $MTIME $UUID scom $VALUE
  **    /reportfmt  $MTIME $TITLE owner $VALUE cols $VALUE sqlcode $VALUE jx $JSON
  **    /concealed  $MTIME $HASH content $VALUE
  **    /subscriber $SMTIME $SEMAIL suname $V ...
  **
  ** NAME-specific notes:
  **
  **  - /reportftm's $MTIME is in Julian, not the Unix epoch.
  */
  fsl__xfer__check_rc;
  if( !fsl__xfer_argc_check(xf, card, 3, 3) ) return xf->rc;
  char const *zName = fsl__xfer_tokn(xf,1);
  if( fsl__payload_read(xf, 2, 0) ) return xf->rc;

  assert( 3==fsl__xfer_ntok(xf) );
  /*
    This is going to be a beast to port from fossil(1) configure.c.
  */
  if( fsl__xfer_tok_match(xf, 1, "/config") ){
    /* /config: $MTIME $NAME value $VALUE */
  }else if( fsl__xfer_tok_match(xf, 1, "@user") ){
    /* /user: $MTIME $LOGIN pw $VALUE cap $VALUE info $VALUE photo $VALUE */
  }else if( fsl__xfer_tok_match(xf, 1, "@shun") ){
    /* /shun: $MTIME $UUID scom $VALUE */
  }else if( fsl__xfer_tok_match(xf, 1, "@reportfmt") ){
    /* /reportfmt: $MTIME $TITLE owner $VALUE cols $VALUE sqlcode $VALUE jx $JSON */
  }else if( fsl__xfer_tok_match(xf, 1, "@concealed") ){
    /* /concealed: $MTIME $HASH content $VALUE */
  }else if( fsl__xfer_tok_match(xf, 1, "@subscriber") ){
    /* /subscriber: $SMTIME $SEMAIL suname $V ... */
  }else{
    fsl_xfer_errorf(xf, FSL_RC_SYNTAX,
                    "Unhandled config card name: %s", zName);
  }
//end:
  fsl__xfer_emit( xf, FSL_MSG_RCV_CONFIG, zName );
  return xf->rc;
#if 0
  malformed:
  return fsl_xfer_errorf(xf, FSL_RC_SYNTAX,
                         "Malformed config %s card: %b", zName, b);
#endif
}

static bool fsl__xfer_version_check(fsl__xfer * const xf){
  /*MARKER(("version check %d %d %d\n", xf->remote.releaseVersion,
    xf->remote.manifestDate, xf->remote.manifestTime));*/
  return xf->remote.releaseVersion>=FOSSIL_RELEASE_VERSION_NUMBER
    && (xf->remote.manifestDate > 20250727
        || (xf->remote.manifestDate == 20250727
            && xf->remote.manifestTime >= 110738));
}

static int fsl__rcv_pragma(fsl__xfer * const xf){
  fsl__xfer__check_rc;
  if( fsl__xfer_ntok(xf)>2 ){
    if( fsl__xfer_tok_match(xf, 1, "server-version")
        || fsl__xfer_tok_match(xf, 1, "client-version") ){
      /* pragma server-version VERSION ?DATE? ?TIME? */
      /* pragma client-version VERSION ?DATE? ?TIME? */
      /* DATE and TIME are only missing from very old clients */
      xf->remote.releaseVersion = atoi(fsl__xfer_tokn(xf, 2));
      xf->remote.manifestDate = atoi(fsl__xfer_tokn(xf, 3));
      xf->remote.manifestTime = atoi(fsl__xfer_tokn(xf, 4));
      if( !fsl__xfer_version_check(xf) ){
        /* TODO: warn here but continue without the ability to
           authenticate, as that's the only sync feature we're missing
           in this case. */
        fsl_xfer_errorf(
          xf, FSL_RC_RANGE, "libfossil requires a remote fossil with "
          "version %d (%d@%06d) or higher. "
          "Got v%d %d %d",
          FOSSIL_RELEASE_VERSION_NUMBER,
          FOSSIL_MANIFEST_NUMERIC_DATE,
          FOSSIL_MANIFEST_NUMERIC_TIME,
          xf->remote.releaseVersion,
          xf->remote.manifestDate,
          xf->remote.manifestTime
        );
      }else if( 2==xf->n.trips  /* 2 == we've initialized the 2nd
                   request while reading the first response */ ){
        fsl__xfer_emitf(xf, FSL_MSG_INFO,
                       "Remote fossil version is %d %d %06d",
                       xf->remote.releaseVersion,
                       xf->remote.manifestDate,
                       xf->remote.manifestTime);
      }
      goto end;
    }
  }
  MARKER(("NYI. Doing nothing for %s\n", fsl_buffer_cstr(&xf->line.bRaw)));
end:
  return xf->rc;
}


/** Emits a FSL_MSG_RCV_BLOB message with the given hash as its
    payload. */
static int fsl__xfer_emit_blob(fsl_xfer * xf, char const * zHash,
                               fsl_size_t size){
  if( 0==xf->rc && xf->f->cxConfig.listener.callback ){
    fsl_msg_blob const blob = { .hash = zHash, .size = size };
    xf->rc = fsl_cx_emit( xf->f, FSL_MSG_RCV_BLOB, &blob );
  }
  return xf->rc;
}

/**
   Populates xf->buf.cardPayload from xf->line, which is known to be a
   cfile- or file-card, then processes that card.
*/
static void fsl__rcv_file(fsl__xfer * const xf,
                          fsl_xfcard const * const card){
  if( xf->rc ) return;
  char const * zUuid = fsl__xfer_tokn(xf,1);
  char const * zDeltaSrcUuid = 0;
  fsl_id_t deltaSrcRid = 0;
  fsl_size_t usz = 0 /* decompressed size */;
  unsigned char uszNdx = 0 /* arg index of usz */;
  unsigned char const nTok = fsl__xfer_ntok(xf);
  unsigned char const szNdx = nTok - 1 /* arg index of payload size */;
  bool const isPrivate = xf->flag.nextIsPrivate;

  assert( fsl_xfcard_e_cfile==card->type
          || fsl_xfcard_e_file==card->type );
  xf->flag.nextIsPrivate = false;
  if( !fsl__xfer_argc_check(xf, card, 3, 5) ) return;
  assert( zUuid );
  if( fsl_is_shunned_uuid(xf->f, zUuid) ) return;

  //fsl_xfer_debugf(ch, "Got %s: %b", card->name, &xf->line.bRaw);
  switch( nTok ) {
    case 3:
      /* file uuid size */
        break;
    case 4:
      /* file uuid delta-uuid size */
      /* cfile uuid usize size */
      if( fsl_xfcard_e_file==card->type ){
        zDeltaSrcUuid = fsl__xfer_tokn(xf,2);
      }else{
        uszNdx = nTok - 2;
      }
      break;
    case 5:
      /* cfile uuid delta-uuid usize size */
      assert( fsl_xfcard_e_cfile==card->type );
      uszNdx = nTok - 2;
      zDeltaSrcUuid = fsl__xfer_tokn(xf,2);
      break;
    default:
      fsl__fatal(FSL_RC_CANNOT_HAPPEN, "Malformed %s card", card->name);
  }
  fsl__payload_read(xf, szNdx, 0/*uszNdx*/);

  if( zDeltaSrcUuid && 0==xf->rc ){
    /* Ensure that we have a blob for zDeltaSrcUuid, even if it's
       just a phantom. */
    assert( fsl_is_uuid(zDeltaSrcUuid) );
    if( fsl_is_uuid(zDeltaSrcUuid) ){
      deltaSrcRid = fsl__uuid_to_rid2(xf->f, zDeltaSrcUuid, isPrivate
                                      ? FSL_PHANTOM_PRIVATE
                                      : FSL_PHANTOM_PUBLIC);
      assert( 0!=deltaSrcRid );
      if( deltaSrcRid<0 ){
        xf->rc = xf->f->error.code;
        assert( xf->rc );
      }
    }else{
      fsl_xfer_errorf(xf, FSL_RC_SYNTAX,
                      "Malformed UUID [%s] in %s card",
                      zDeltaSrcUuid, card->name);
    }
  }
  if( uszNdx ){
    fsl__xfer_card_size(xf, uszNdx, true, &usz);
  }
#if 1
  if( 0==xf->rc ){
    /* This part isn't working right. It outwardly appears to but no
       content is landing in the blob table. The verify-at-commit hook
       is failing (fsl__repo_verify_at_commit()) and that error is not
       propagating properly (this is the first time in 12+ years we've
       had an error to propagate from there!).*/
    xf->rc = fsl__content_put_ex(xf->f, &xf->buf.cardPayload,
                                 zUuid, deltaSrcRid, usz, false,
                                 /*     ^^^^ reminder: it's legal for
                                        deltaSrcRid to be a phantom */
                                 NULL/*new rid*/
    );
    if( xf->rc ){
      MARKER(("cx rc=%s\n", fsl_rc_cstr(fsl_cx_err_get(xf->f,NULL,NULL))));
      MARKER(("xf.rc=%s\n", fsl_rc_cstr(xf->rc)));
    }
  }
#else
  (void)deltaSrcRid;
  (void)zDeltaSrcUuid;
#endif
  fsl__xfer_emit_blob(xf, zUuid, usz ? usz : xf->buf.cardPayload.used);
}

#if 0
static void fsl__rcv_igot_add(fsl__xfer *xf, fsl_id_t rid){
  assert( rid > 0 );
  if( 0==xf->rc ){
    xf->rc = fsl__rid_insert(xf->f, &xf->q.igotInsert,
                             FSL_DBROLE_TEMP, "xfigot", rid);
  }
}
#endif

#if 0
static bool fsl__xfer_have_igot( fsl__xfer *xf, fsl_id_t rid ){
  assert( rid > 0 );
  fsl_stmt * const q = &xf->q.igotHasRid;
  bool rv = false;
  if( 0==xf->rc ){
    if( !q->stmt ){
      xf->rc = fsl_cx_prepare(xf->f, q,
                              "SELECT 1 FROM %!Q.blob "
                              "WHERE rid=?1 /*%s()*/",
                              fsl_db_role_name(FSL_DBROLE_REPO),
                              __func__);
    }
    if( 0==xf->rc ){
      int const rc = fsl_stmt_bind_step_v2(q, "R", rid);
      switch( rc ){
        case FSL_RC_STEP_ROW: rv = true; break;
        case FSL_RC_STEP_DONE: break;
        default:
          xf->rc = fsl_cx_uplift_db_error2(xf->f, NULL, rc);
          break;
      }
      fsl_stmt_reset(q);
    }
  }
  return rv;
}
#endif

static bool fsl__xfer_have_igot( fsl__xfer *xf, fsl_uuid_cstr zUuid ){
  assert( zUuid );
  bool rv = false;
  if( 0==xf->rc ){
    fsl_stmt * const q = fsl__cx_stmt(xf->f, fsl__cx_stmt_e_uuidToRid);
    if( q ){
      int const rc = fsl_stmt_bind_step_v2(q, "s", zUuid);
      switch( rc ){
        case FSL_RC_STEP_ROW:
          rv = true;
          fsl_stmt_reset(q);
          break;
        case FSL_RC_STEP_DONE: break;
        default:
          xf->rc = fsl_cx_uplift_db_error2(xf->f, q->db, rc);
          break;
      }
      fsl__cx_stmt_yield(xf->f, q);
    }else{
      xf->rc = xf->f->error.code;
    }
  }
  return rv;
}

static void fsl__gimme_queue(fsl__xfer *xf, fsl_uuid_cstr zUuid){
  assert(zUuid);
  if( !fsl__xfer_have_igot(xf, zUuid)
      && 0==xf->rc /* have_igot() can change it */){
    fsl__xfer_appendcard(xf, fsl_xfcard_e_gimme, zUuid);
  }
}

#if 0
/* Not useful unless we're going queue up gimmes in a table
   rather than emit gimme cards at the earliest opportunity. */
static void fsl__gimme_delete(fsl__xfer *xf, fsl__uuid_cstr zUuid){
  assert( rid > 0 );
  if( 0==xf->rc ){
    xf->rc = fsl_cx_exec(xf->f,
                         "DELETE FROM %!Q.xfgimme WHERE uuid=%Q",
                         fsl_db_role_name(FSL_DBROLE_TEMP),
                         zUuid);
  }
}
#endif

static void fsl__rcv_igot(fsl__xfer * const xf,
                         fsl_xfcard const * const card){
  assert( fsl_xfcard_e_igot==card->type );
  if( xf->rc || fsl__xfer_check_card_hash(xf, card, 1) ){
    return;
  }
  /* igot artifact-id ?flags? */
  assert( fsl__xfer_ntok(xf)>1 && fsl__xfer_ntok(xf)<4 );
  char const *zUuid = fsl__xfer_tokn(xf, 1);
  bool const isPrivate = fsl__xfer_ntok(xf)>2
    ? fsl__xfer_tok_match(xf, 2, "1")
    : false;
  fsl_id_t rid = fsl_uuid_to_rid(xf->f, zUuid);
  xf->flag.nextIsPrivate = isPrivate;
  if( rid>0 ){ /* We already have this */
    xf->rc = isPrivate
      ? fsl__private_rid_add(xf->f, rid)
      : fsl__private_rid_delete(xf->f, rid);
    if( -1==fsl_content_size(xf->f, rid) ){
      /* fossil(1) version 1 clone response #2 and later send
         file-cards for what it can, up to its limit, then sends igots
         to indicate that there's still more to fetch. */
      //fsl__xfer_appendcard(xf, fsl_xfcard_e_gimme, zUuid);
      /* However, we can get into a loop of receiving/sending
         the same igot/gimme cards, so we need to keep more
         precise track of them. */
    }
    //fsl__gimme_delete(xf, zUuid);
  }else if( isPrivate && !xf->user.perm.privateContent ){
    /* Ignore private files */
    //fsl__gimme_delete(xf, zUuid);
  }else if( xf->config->op.clone>0 || xf->config->op.pull ){
    //MARKER(("Adding phantom for igot %s\n", zUuid));
    fsl__xfer_phantom_new(xf, zUuid, isPrivate, &rid);
    //fsl__rcv_igot_add(xf, rid);
    fsl__gimme_queue(xf, zUuid);
  }
}

/**
   To be called immediately after xf->ch->submit() has been called, to
   parse its response.

   Reminder to self: we eventually need to distinguish between
   received-from-client and received-from-server payloads, as card
   handling can differ between the two (e.g. "igot" cards). We
   currently only handle the latter case.
*/
static int fsl__xfer_parse_payload(fsl__xfer * const xf){
  fsl__xfer__check_rc;
  unsigned nLine = 0;
  unsigned char * const * aTok = xf->line.aToken;
  //fsl_buffer * const bLn = &xf->line.b;
  fsl_buffer * const bLnRaw = &xf->line.bRaw;
  fsl_buffer * const bPay = &xf->buf.cardPayload;

  for( ; 0==xf->rc; ++nLine ){
    fsl_buffer_reuse(bPay);
    xf->flag.nextIsPrivate = false;
    fsl__xfer_readln(xf);
    if( xf->rc ){
      //MARKER(("xfer_readln() xf->rc=%s\n", fsl_rc_cstr(xf->rc)));
      if( FSL_RC_BREAK==xf->rc ) xf->rc = 0 /* EOF */;
      break;
    }
    if( !fsl__xfer_ntok(xf) ){
      if( 0==nLine ){
        fsl_xfer_error(xf, FSL_RC_ERROR, "Got an empty response");
      }else{
        assert(!"should not be possible");
        fsl_xfer_error(xf, FSL_RC_ERROR,
                       "FIXME: empty lines should be "
                       "skipped (elsewhere)");
      }
      break;
    }
    unsigned char const nTok = fsl__xfer_ntok(xf);

    /* Bails if nTok is not have been MIN and MAX
       arguments. If MAX<0 then any number, up to
       fsl__xfer_max_tokens, is fine. */
#define argcCheck2(MIN,MAX)                                             \
    if( !fsl__xfer_argc_check(xf, card, (MIN), (MAX)) ) break

#define argcCheck(N) argcCheck2(N,N)

    if( xf->config->verbosity>1 && xf->config->debug.callback ){
      MARKER(("line = %s", fsl_buffer_cstr(bLnRaw)));
      fsl_xfer_debugf( xf, "line #%u: %b", nLine, bLnRaw );
    }
    fsl_xfcard const * const card =
      fsl__xfcard_search(aTok[0]);
    if( 0 ){
      MARKER(("ntoken=%d search(%s) => %p\n", (int)nTok,
              (char const *)aTok[0], (void*)card));
    }
    xf->n.rcvdCards += !!card;

#define readPayload(N)                               \
    if( N ){                                            \
      fsl__xfer_read(xf, fsl_buffer_reuse(bPay), N);   \
      if( xf->rc ) break;                                \
      else if( xf->n.largestCardPayload < N ){                \
        xf->n.largestCardPayload = N;                         \
      } \
    }

#define readPayloadTok(TOKNDX,SZVAR)                                   \
    if( fsl__xfer_card_size(xf, TOKNDX, true, &SZVAR) ) break; \
    readPayload(SZVAR)

    if( card ){
      ++xf->n.cardsRx[card->type];
    }
    if( 0 ){
      MARKER(("line=%s", fsl_buffer_cstr(bLnRaw)));
    }

    switch( card ? card->type : fsl_xfcard_e_unknown ){
#define CASE(X) case fsl_xfcard_e_ ## X

      CASE(cfile): /* cfile uuid ?delta-uuid? usize size\n<PAYLOAD> */
      CASE(file):{ /* file uuid ?delta-uuid? size\n<PAYLOAD> */
        fsl__rcv_file(xf, card);
        xf->flag.nextIsPrivate = false;
        break;
      }

      CASE(clone):{
        /* clone
           clone protocol-version sequence-number */
        if( 3==nTok ){
          fsl_size_t n = 0;
          fsl__xfer_card_size(xf, 1, false, &n);
          if( 0==xf->rc ){
            xf->clone.protocolVersion = (int)n;
            fsl__xfer_card_size(xf, 2, true, &xf->clone.seqNo);
          }
        }else if( 1==nTok ){
          xf->clone.protocolVersion = 1;
        }else{
          fsl_xfer_errorf(xf, FSL_RC_SYNTAX,
                          "clone card expects 1 or 3 arguments "
                          "but got %d", (int)nTok);
        }
        goto nyi;
        break;
      }

      CASE(clone_seqno):{ /* clone_seqno number */
        fsl__xfer_card_size(xf, 1, true, &xf->clone.seqNo);
        /* a seqno of 0 is sent at the end of the cloning
           round-trips. */
        break;
      }

      CASE(config):{ /* config name size \n content */
        fsl__rcv_config(xf, card);
        break;
      }

      CASE(error):    /* error fossilized-msg */
      CASE(message):{ /* message fossilized-msg */
        argcCheck(2);
        if( fsl_xfcard_e_error==card->type ){
          char const * z = fsl__xfer_tokn_defos(xf, 1);
          fsl__xfer_emit(xf, FSL_MSG_ERROR, z);
          fsl_xfer_errorf(xf, FSL_RC_REMOTE,
                          "Error from remote: %s", z);
        }else if( xf->config->listener.callback ){
          fsl__xfer_emit(xf, FSL_MSG_RCV_MESSAGE,
                         fsl__xfer_tokn_defos(xf, 1));
        }
        break;
      }

      CASE(igot):{/* igot artifact-id ?flags? */
        argcCheck2(2,3);
        fsl__rcv_igot(xf, card);
        break;
      }

      CASE(gimme):{/* gimme artifact-id */
        argcCheck2(2,2);
        break;
      }

      CASE(uvigot) : { /* uvigot name mtime hash size */
        fsl_size_t sz = 0;
        argcCheck(5);
        readPayloadTok(4, sz);
        break;
      }

      CASE(pragma):{
        argcCheck2(2,-1);
        //fsl_xfer_debugf(ch, "Got pragma: %b", bLnRaw);
        fsl__rcv_pragma(xf);
        break;
      }

      CASE(private):{
        argcCheck(1);
        xf->flag.nextIsPrivate = true;
        break;
      }

      CASE(pull):  /* pull serverCode(IGNORED) projectCode */
      CASE(push):{ /* push serverCode(IGNORED) projectCode */
        argcCheck(3);
        char const * theirPC = fsl__xfer_tokn(xf, 2);
        char const * myPC = fsl__xfer_projcode(xf);
        //MARKER(("project code rc=%s %s\n", fsl_rc_cstr(rc), myPC));
        if( 0==xf->rc ){
          if( xf->clientVersion.projCodeMatch<0
              && xf->config->op.clone>0
              && !myPC ){
            /* Assume we just got our new project code via a clone */
            fsl_cx_err_reset(xf->f);
            fsl__xfer_emitf(xf, FSL_MSG_INFO,
                           "Got new project code: %s", theirPC);
            xf->clientVersion.projCodeMatch = 1;
            xf->rc = fsl_config_set_text(xf->f, FSL_CONFDB_REPO,
                                         "project-code", theirPC);
          }else{
            xf->clientVersion.projCodeMatch =
              (0==fsl_strcmp(theirPC, myPC));
            if( !xf->clientVersion.projCodeMatch ){
              fsl__xfer_emitf(xf, FSL_MSG_INFO,
                             "Project code mismatch: this=%s remote=%s",
                             myPC, theirPC);
            }
          }
        }
        break;
      }

      default:{
      nyi:
        fsl_xfer_errorf(xf, FSL_RC_NYI,
                        "Don't yet know how to handle card %s: %b",
                        card ? card->name : "", bLnRaw);
        break;
      }
    }/* switch( card->type ) */
  }/* while(0==rc) */
#undef CASE
#undef readPayload
#undef readPayloadTok
#undef argcCheck
#undef argcCheck2
  //end:
  return xf->rc;
//bad_card:
//  fsl_xfer_errorf(xf, FSL_RC_SYNTAX, "Malformed card: %b", bLnRaw);
//  goto end;
}

fsl_xfcard_hash_t fsl_xfcard_hash( unsigned const char * word ){
  fsl_xfcard_hash_t H = 0;
  uint8_t I = 0;
  for( unsigned char const * x = word; *x; ++x, ++I ){
#if 1
    if( *x < fsl_xfcard_LowestIdChar || I>fsl_xfcard_MaxCardNameLen ){
      /* Reminder to self: this is a use-case-specific optimization
         and may not be appropriate when carrying this code forward
         into other trees. You remind yourself of that because _this_
         copy (sans this block) was carried forward from the sewal
         experiment. */
      H = 0;
      break;
    }
#endif
    H = fsl_xfcard_hash_step_ch(H,I,*x);
  }
  return H;
}

/**
   qsort()/bsearch() cmp function for (fsl_xfcard*), ordering on their
   hash values.
*/
static int fsl__xfcard_cmp_hash(void const *lhs, void const *rhs){
  fsl_xfcard_hash_t const l = ((fsl_xfcard const *)lhs)->hash;
  fsl_xfcard_hash_t const r = ((fsl_xfcard const *)rhs)->hash;
  //assert( l==0 ? l==r : l!=r );
  return (l<r) ? -1 : l!=r;
}

fsl_xfcard const * fsl__xfcard_search(unsigned char const * z){
  /**
     All sync protocol card types, initially sorted by name but will
     be sorted (once) by hash the first time this is called.
  */
  static fsl_xfcard obh[fsl_xfcard_e_COUNT] = {
    /* !!! Must be in fsl_xfcard_e order !!! */
    {.type=fsl_xfcard_e_unknown, .name="<unknown>", .hash=0},
#define E(T,LEN) {.type=fsl_xfcard_e_ ## T, .name=#T, .hash=fsl_xfcard_hash_N(LEN,T) },
    fsl_xfcard_map(E)
#undef E
  };
  static bool once = false;
  if( !once ){
    /* obh = Ordered By Hash, so sort obh by hash value... */
    assert( fsl_xfcard_e_COUNT == sizeof(obh)/sizeof(obh[0]) );
    assert( fsl_xfcard_e_uvigot==obh[fsl_xfcard_e_COUNT-1].type );
    assert( 0==fsl_strcmp("clone_seqno", obh[fsl_xfcard_e_clone_seqno].name) );
    assert( fsl_xfcard_MaxCardNameLen == fsl_strlen(obh[fsl_xfcard_e_clone_seqno].name) );
    assert( fsl_xfcard_e_unknown==obh[0].type );
    assert( 0==obh[0].hash );

    qsort( obh, fsl_xfcard_e_COUNT, sizeof(obh[0]), fsl__xfcard_cmp_hash);

    assert( fsl_xfcard_e_unknown==obh[0].type );
    assert( 0==obh[0].hash );

    once = true;
#if !defined(NDEBUG)
    /* Assert that there are no duplicate hashes. If there are,
       we need to tweak the hash algo until there are none. */
    for(int i = 0; i < fsl_xfcard_e_COUNT; ++i){
      assert( fsl_strlen(obh[i].name) <= fsl_xfcard_MaxCardNameLen );
      assert( i ? (obh[i-1].hash < obh[i].hash) : 1 );
    }
    /** Assert that the fsl_xfcard_hash_#() macros produce the same results as
        fsl_xfcard_hash(). */
    assert( fsl_xfcard_hash_5(cfile) == fsl_xfcard_hashS("cfile"));
    assert( fsl_xfcard_hashS("clone_seqno") == fsl_xfcard_hash_11(clone_seqno) );
    assert( 0 == fsl_xfcard_hashS("abc!nope") );
#  if 0
    MARKER(("fsl_xfcards sorted by hash:\n"));
    for(int i = 0; i < fsl_xfcard_e_COUNT; ++i){
      printf("%-12s # %2d 0x%08" PRIx32 "\n",  obh[i].name, obh[i].type, obh[i].hash);
    }
#  endif
#endif
  }/*one-time setup*/
  if( !z ) return NULL;
  fsl_xfcard const key = {
    fsl_xfcard_e_unknown,
    (char const *)z,
    fsl_xfcard_hash(z)
  };
  fsl_xfcard const * scc = bsearch( &key, obh, fsl_xfcard_e_COUNT,
                                    sizeof(fsl_xfcard),
                                    fsl__xfcard_cmp_hash );
  /*MARKER(("Searching for 0x%08" PRIx32 " [%s] candidate=%p\n",
    key.hash, z, (void*)scc));*/
  return scc
    ? (fsl_strcmp((char const *)scc->name, (char const *)z)
       ? 0
       : (scc->type==fsl_xfcard_e_unknown ? 0 : scc))
    : 0;
}

/**
   Populate pSc and pPos based on the URL set in c->url. Report errors
   via the final argument.
*/
static int fsl__clone_sc_init(fsl_clone_config const * c,
                              fsl_sc * pSc,
                              fsl_sc_popen_state *pPos,
                              fsl_error * err){
  int rc = 0;
  fsl_url url = fsl_url_empty;

  assert(c->xfer.url);
  assert(c->repo.filename);
  if( fsl_file_size(c->repo.filename)>=0 ){
    if( c->repo.allowOverwrite ){
      if( 0!=fsl_file_unlink(c->repo.filename) ){
        rc = fsl_error_set(err, FSL_RC_ACCESS,
                           "Cannot delete file %s",
                           c->repo.filename);
      }
    }else{
      rc = fsl_error_set(err, FSL_RC_ALREADY_EXISTS,
                         "Cowardly refusing to overwrite %s",
                         c->repo.filename);
    }
  }
  if( 0==rc ) rc = fsl_url_parse(&url, c->xfer.url);
  if( 0==rc ){
    if(0==fsl_strcmp("http", url.scheme)
       || 0==fsl_strcmp("https", url.scheme) ){
      *pSc = fsl_sc_popen_curl;
      *pPos = fsl_sc_popen_state_curl;
    }else if(0==fsl_strcmp("file", url.scheme)){
      *pSc = fsl_sc_popen_fth;
      *pPos = fsl_sc_popen_state_fth;
    }else if(0==fsl_strcmp("ssh", url.scheme)){
      *pSc = fsl_sc_popen_ssh;
      *pPos = fsl_sc_popen_state_ssh;
    }else{
      rc = fsl_error_set(err, FSL_RC_TYPE,
                         "Cannot determine which sync channel to use "
                         "for URL ");
      fsl_url_render(&url, &err->msg,
                     FSL_URL_RENDER_MASK_PASSWORD);
    }
    if( 0==rc ){
      pSc->impl.p = pPos;
      fsl_url_swap(&url, &pSc->url);
    }
  }
  fsl_url_cleanup(&url);
  return rc;
}

/**
   To be called after fsl__xfer_submit(). Tries to guess whether another
   request should be sent as a follow-up.
*/
static bool fsl__xfer_should_continue(fsl__xfer const * const xf,
                                      fsl_xfer_metrics const * const prev,
                                      fsl_size_t nOldCloneSeq){
  if( prev->cardsTx[fsl_xfcard_e_gimme]
      < xf->n.cardsTx[fsl_xfcard_e_gimme] ){
    /* We can't just compare prev/xf->n.sentCard because some cards
       are innocuous and should not trigger a new trip. */
    return true;
  }
  if( xf->config->op.clone
      && xf->clone.seqNo
      && nOldCloneSeq<xf->clone.seqNo ) return true;
  return false;
}

int fsl_clone( fsl_clone_config const * const cc_, fsl_error * const err_ ){
  fsl_url url = fsl_url_empty;
  int rc = 0;
  fsl_cx f_ = fsl__cx_empty;
  fsl_cx * f = &f_;
  fsl_sc chBase;
  fsl_sc chTrace;
  fsl_sc *ch = &chBase;
  fsl_sc_popen_state st;
  fsl_clone_config ccc = *cc_;
  fsl_xfer_config * const cxf = &ccc.xfer;
  fsl_xfer_metrics prevMetrics /* gets assigned xf.n before reach trip */;
  fsl_cx_config fCfg = fsl_cx_config_empty_m;
  fsl__xfer xf = fsl__xfer_empty;
  uint32_t cloneSeqNo = 0;
  char const * zPC = 0;
  bool createdRepo = false;
  fsl_error * err = err_ ? err_ : &f_.error;

  MARKER(("This function and its infrastructure are largely TODO\n"));
  assert( cxf->op.clone );
  fsl_error_reset(err);

  fCfg.listener = cxf->listener;
  rc = fsl__clone_sc_init(&ccc, &chBase, &st, err);
  if( rc ) goto end;
  assert( chBase.impl.p == &st );
  assert( chBase.url.raw );
  //fCfg.traceSql = 1;
  rc = fsl_cx_init(&f, &fCfg);
  if( rc ) goto end;
  if( 1 ){
    f->output.out = fsl_output_f_FILE;
    f->output.flush = fsl_flush_f_FILE;
    f->output.state = stdout;
  }
  if( cxf->trace.mask ){
    if( fsl_sc_tracer_init(&chTrace, &chBase,
                           cxf->trace.mask,
                           &cxf->trace.outputer) ){
      ch = &chTrace;
      chBase.xfer = &xf;
      assert(chTrace.impl.p);
      assert(chTrace.impl.p!=&ch);
    }else{
      fsl_report_oom;
      rc = FSL_RC_OOM;
    }
  }
  assert( !xf.f );
  rc = fsl__xfer_setup(f, &xf, cxf, ch);
  assert( f==xf.f );
  assert( rc==xf.rc );
  if( 0==rc ){
    /* Create the repo */
    ccc.repo.elideProjectCode = true;
    if( !ccc.repo.username ) ccc.repo.username = xf.user.name;
    rc = fsl_repo_create(f, &ccc.repo);
    if( 0==rc ){
      createdRepo = true;
      if( 0==(rc = fsl_cx_txn_begin(f)) ){
        rc = fsl__xfer_start(&xf, FSL__XFER_START_REQUEST);
      }
    }else{
      createdRepo = FSL_RC_ALREADY_EXISTS!=rc;
    }
  }
  if( rc ) goto end;

  assert( rc || 1==xf.n.trips );
again:
  assert( !rc );
  prevMetrics = xf.n;
  rc = fsl__xfer_emit(&xf, FSL_MSG_CONNECT, &xf.n.trips);
  assert( xf.rc == rc );
  if( 0==rc ){
    fsl__xfer_pw_setup(&xf);
    if( 0==xf.rc){
      if( 1==xf.n.trips ){
        //fsl__xfer_appendcard(&xf, fsl_xfcard_e_reqconfig, "/project");
        //fsl__xfer_appendcard(&xf, fsl_xfcard_e_reqconfig, "/skin");
        //fsl__xfer_appendcard(&xf, fsl_xfcard_e_pragma, "send-catalog");
      }else{
        cloneSeqNo = xf.clone.seqNo;
        if( !zPC ){
          rc = xf.rc = fsl_repo_project_code(f, &zPC);
          assert( (zPC || rc) && "Expecting to have just received a project-code");
        }
      }
    }
    rc = xf.rc;
  }

  switch( rc ? 0 : xf.config->op.clone ){
    case 0: break;
    case 1:
      if( 1==xf.n.trips ){
        /* clone version 1 delivers all of its metadata in the initial
           response. */
        rc = fsl__xfer_appendcard(&xf, fsl_xfcard_e_clone, NULL);
      }
      break;
    case 2:
    case 3:
      /* Response contains many "file" cards (v2) or "cfile" cards
         (v3). Protocol version 2 may compress the body. Level 3
         compresses the individual "cfile" cards instead. */
      xf.clone.protocolVersion = xf.config->op.clone;
      if( 1==xf.n.trips || xf.clone.seqNo ){
        rc = fsl__xfer_appendcard(&xf, fsl_xfcard_e_clone,
                                  "%d %" FSL_SIZE_T_PFMT,
                                  xf.clone.protocolVersion,
                                  xf.clone.seqNo ? xf.clone.seqNo : 1);
      }
      break;
    default:
      rc = xf.rc = fsl_error_set(err, FSL_RC_MISUSE,
                                 "Don't know how to handle clone "
                                 "protocol version #%d",
                                 (int)xf.config->op.clone);
      break;
  }
  if( rc ) goto end;

  if( 0==fsl__xfer_submit(&xf) ){
    if( cloneSeqNo && !xf.clone.seqNo ){
      /* Done with the clone part */
      xf.config->op.clone = 0;
    }
    if( fsl__xfer_should_continue(&xf, &prevMetrics, cloneSeqNo) ){
      goto again;
    }
  }
  rc = xf.rc;
  if( 0==rc ){
    /* We need to commit before the rebuild is run so that content
       validation and dephantomization happen. If we run fsl_rebuild()
       while this level is active, that validation won't happen until
       after rebuild. */
    rc = fsl_cx_txn_end_v2(f, true, false);
    if( 0==rc ){
      rc = fsl_repo_rebuild(f, NULL);
    }
  }

end:
  if( !rc ) rc = xf.rc;
  if( f->error.code ){
    fsl_error_propagate(&f->error, err);
  }else if( !err->code && rc ){
    err->code = rc;
  }
  if( err->code && xf.config ){
    assert( xf.f );
    if( err->msg.used ){
      fsl__xfer_emit(&xf, FSL_MSG_ERROR,
                     fsl_buffer_cstr(&err->msg));
    }else{
      fsl__xfer_emitf(&xf, FSL_MSG_ERROR,
                     "Sync failed with code %R", err->code);
    }
  }
  fsl_url_cleanup(&url);
  fsl__xfer_cleanup(&xf, rc);
  while( fsl_cx_txn_level(f)!=0 ){
    fsl_cx_txn_end_v2(f, false, true);
  }
  if( 0 && 0==rc ){
    if( f->output.out ){
      fsl_db * const db = fsl_cx_db(f);
      fsl_outputf(f, "****** debugging counts:\n");
      fsl_db_each( db, fsl_stmt_each_f_dump,
                   f, "SELECT COUNT(*) nPhantom FROM blob WHERE size<0");
      fsl_db_each( db, fsl_stmt_each_f_dump,
                   f, "SELECT COUNT(*) nNonPhantom FROM blob WHERE size>=0");
      fsl_db_each( db, fsl_stmt_each_f_dump,
                   f, "SELECT COUNT(*) nDelta FROM delta");

#if 0
      MARKER(("All delta chains:\n"));
      fsl_db_each(
        db, fsl_stmt_each_f_dump, f,
        "SELECT d.rid, d.srcid, b.size "
        "from delta d, blob b where d.rid=b.rid "
        "order by d.rid"
      );
      MARKER(("Non-deltified roots of deltas:\n"));
      fsl_db_each(
        db, fsl_stmt_each_f_dump, f,
        "SELECT d.srcid, b.size "
        "FROM delta d, blob b WHERE d.srcid=b.rid "
        "AND d.srcid NOT IN (SELECT rid FROM delta) "
        "ORDER BY d.rid"
      );
#endif
#if 0
      MARKER(("Delta chains:\n"));
      fsl_db_each(
        db, fsl_stmt_each_f_dump, f,
        "WITH RECURSIVE "
        "roots(rid,bsz) AS ( "
        "  SELECT DISTINCT d.srcid rid, b.size bsz "
        "  FROM delta d, blob b "
        "  WHERE b.rid=d.srcid "
        "  AND d.srcid NOT IN (SELECT rid FROM delta) "
        "  ORDER BY d.rid"
        "), "
        "dchain(level,srcid,rid,bsz) AS ( "
        "  SELECT 0 level, 0 srcid, r.rid rid, r.bsz "
        "    FROM roots r "
        "  UNION ALL "
        "  SELECT dchain.level+1, delta.srcid, delta.rid, blob.size "
        "    FROM delta, dchain, blob "
        "    WHERE delta.srcid=dchain.rid "
        "    AND delta.rid=blob.rid "
        "  ORDER BY 1, 2 "
        ") "
        "SELECT "
        "substr('..........',1,level*2) || "
        "format('%%d [%%d]->%%d', rid, bsz, srcid) as 'Delta chains'"
        "FROM dchain"
      );
      if( db->error.code ){
        MARKER(("db says: %s: %s\n", fsl_rc_cstr(db->error.code),
                fsl_buffer_cstr(&db->error.msg)));
      }
#endif
      fsl_db_each( db, fsl_stmt_each_f_dump,
                   f, "SELECT rid,uuid,size, "
                   "octet_length(content), "
                   "octet_length(fsl_uncompress(content)) "
                   "from blob order by rid desc" );
    }
  }/*debug output*/
  fsl_cx_finalize(f);
  if( rc && createdRepo ){
    /* Has to wait until after fsl_cx_finalize() */
    fsl_file_unlink(ccc.repo.filename);
  }
  fsl_xfer_config_cleanup(cxf);
  return rc;
}

void fsl_xfer_metrics_f_dup(fsl_xfer_metrics const *m, void*dest, int theRc){
  (void)theRc;
  *((fsl_xfer_metrics *) dest) = *m;
}

void fsl_xfer_metrics_f_outputer(fsl_xfer_metrics const * const m, void *state,
                                 int theRc){
  if( theRc ) return;
  char const * cardNames[] ={
    NULL,
#define E(T,N) # T,
    fsl_xfcard_map(E)
#undef E
  };
  fsl_outputer * const fo = state;
  assert( fo );


#define out(msg) fo->out(fo->state, msg, sizeof(msg)-1)
#define outf(msg,...) fsl_appendf(fo->out, fo->state, msg, __VA_ARGS__)

  out("fsl_xfer metrics:\n");

  outf("Cards read:      %," FSL_SIZE_T_PFMT "\n",
        m->rcvdCards);
  if( m->rcvdCards ){
    for( int i = fsl_xfcard_e_unknown + 1; i < fsl_xfcard_e_COUNT; ++i){
      if( m->cardsRx[i] ){
        outf("    %-12s %," FSL_SIZE_T_PFMT "\n",
              cardNames[i], m->cardsRx[i]);
      }
    }
    out("\n");
  }
  if( m->sentCards ){
    outf("Cards written:   %," FSL_SIZE_T_PFMT "\n", m->sentCards);
    for( int i = fsl_xfcard_e_unknown + 1; i < fsl_xfcard_e_COUNT; ++i){
      if( m->cardsTx[i] ){
        outf("    %-12s %," FSL_SIZE_T_PFMT "\n",
             cardNames[i], m->cardsTx[i]);
      }
    }
    out("\n");
  }

  if( m->bytesWritten ){
    outf("Bytes written:           %," FSL_SIZE_T_PFMT "\n",
          m->bytesWritten);
  }
  if(m->bytesReadUncompressed){
    outf("Bytes read uncompressed: %," FSL_SIZE_T_PFMT "\n",
          m->bytesReadUncompressed);
  }
  if(m->bytesReadCompressed){
    outf("Bytes read compressed:   %," FSL_SIZE_T_PFMT "\n",
          m->bytesReadCompressed);
  }
  if( m->largestDecompressedResponse ){
    outf("Largest decompr. buffer: %," FSL_SIZE_T_PFMT "\n",
          m->largestDecompressedResponse);
  }
  if( m->largestCardPayload ){
    outf("Largest card payload:    %," FSL_SIZE_T_PFMT "\n",
          m->largestCardPayload);
  }

  fsl_timer tmt = {.user=0,.system=0,.wall=0};
  int nTimers = 0;
  char const * zTimerFmt =
    "    %-18s %-10.3lf %-10.3lf %-10.3lf %-10.3lf\n"
    /*   label  wall     CPU      user      system */
    ;

  for( unsigned j = 0; j < sizeof(m->timer)/sizeof(m->timer.submit); ++j ){
    /* m->timer... */
    fsl_timer const * tm = 0;
    char const * zPre = 0;
    switch(j){
      case 0:
        tm = &m->timer.submit;
        zPre = "fsl_sc::submit()";
        break;
      case 1:
        tm = &m->timer.read;
        zPre = "fsl_sc::read()";
        break;
      case 2:
        tm = &m->timer.uncompress;
        zPre = "blob uncompress()";
        break;
      case 3:
        tm = &m->timer.process;
        zPre = "processing";
        break;
    }
    assert( tm );
    if( tm->user || tm->system || tm->wall ){
      if( 1==++nTimers ){
        out("\nfsl_xfer timers (ms):\n");
        outf("    %-18s %-10s %-10s %-10s %-10s\n",
               "Purpose", "Wall", "CPU", "User", "System");
      }
      tmt.user += tm->user;
      tmt.system += tm->system;
      tmt.wall += tm->wall;
      outf( zTimerFmt, zPre,
             (double)(tm->wall / 1000.0),
             (double)((tm->user + tm->system) / 1000.0),
             (double)(tm->user / 1000.0),
             (double)(tm->system / 1000.0) );
    }
  }
  if( nTimers>1 ){
    outf( "    %.80c\n", '-');
    outf( zTimerFmt, "                 =",
            (double)(tmt.wall / 1000.0),
            (double)((tmt.user + tmt.system) / 1000.0),
            (double)(tmt.user / 1000.0),
            (double)(tmt.system / 1000.0) );
  }

#undef out
#undef outf
}

#undef MARKER
#undef fsl__sc_popen_init
#undef fsl__xfer__rch_decl
