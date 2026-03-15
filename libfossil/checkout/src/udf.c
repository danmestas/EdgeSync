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
  This file houses fsl_cx-related sqlite3 User Defined Functions (UDFs).
*/
#include "fossil-scm/internal.h"
#if !defined(FSL_ENABLE_SQLITE_REGEXP)
#  define FSL_ENABLE_SQLITE_REGEXP 0
#endif
#if FSL_ENABLE_SQLITE_REGEXP
#  include "ext_regexp.h"
#endif
#include "sqlite3.h"
#include <assert.h>

/* Only for debugging */
#include <stdio.h>
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)


/**
   SQL function for debugging.

   The print() function writes its arguments to fsl_output()
   if the bound fsl_cx->cxConfig.sqlPrint flag is true.
*/
static void fsl__udf_print(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  fsl_cx * f = (fsl_cx*)sqlite3_user_data(context);
  assert(f);
  if( f->cxConfig.sqlPrint ){
    int i;
    for(i=0; i<argc; i++){
      char c = i==argc-1 ? '\n' : ' ';
      fsl_outputf(f, "%s%c", sqlite3_value_text(argv[i]), c);
    }
  }
}

/**
   SQL function to return the number of seconds since 1970.  This is
   the same as strftime('%s','now') but is more compact.
*/
static void fsl__udf_now(
  sqlite3_context *context,
  int argc fsl__unused,
  sqlite3_value **argv fsl__unused
){
  (void)argc;
  (void)argv;
  sqlite3_result_int64(context, (sqlite3_int64)time(0));
}

/**
   SQL function to convert a Julian Day to a Unix timestamp.
*/
static void fsl__udf_j2u(
  sqlite3_context *context,
  int argc fsl__unused,
  sqlite3_value **argv fsl__unused
){
  double const jd = (double)sqlite3_value_double(argv[0]);
  (void)argc;
  (void)argv;
  sqlite3_result_int64(context, (sqlite3_int64)fsl_julian_to_unix(jd));
}

/**
   SQL function FSL_CKOUT_DIR([bool includeTrailingSlash=1]) returns
   the top-level checkout directory, optionally (by default) with a
   trailing slash. Returns NULL if the fsl_cx instance bound to
   sqlite3_user_data() has no checkout.
*/
static void fsl__udf_chkout_dir(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  fsl_cx * f = (fsl_cx*)sqlite3_user_data(context);
  int const includeSlash = argc
    ? sqlite3_value_int(argv[0])
    : 1;
  if(f && f->db.ckout.dir && f->db.ckout.dirLen){
    sqlite3_result_text(context, f->db.ckout.dir,
                        (int)f->db.ckout.dirLen
                        - (includeSlash ? 0 : 1),
                        SQLITE_TRANSIENT);
  }else{
    sqlite3_result_null(context);
  }
}


/**
    SQL Function to return the check-in time for a file.
    Requires (vid,fid) RID arguments, as described for
    fsl_mtime_of_manifest_file().
 */
static void fsl__udf_checkin_mtime(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  fsl_cx * f = (fsl_cx*)sqlite3_user_data(context);
  fsl_time_t mtime = 0;
  int rc;
  fsl_id_t vid, fid;
  assert(f);
  assert(2<=argc);
  vid = (fsl_id_t)sqlite3_value_int(argv[0]);
  fid = (fsl_id_t)sqlite3_value_int(argv[1]);
  rc = fsl_mtime_of_manifest_file(f, vid, fid, &mtime);
  if( rc==0 ){
    sqlite3_result_int64(context, mtime);
  }else{
    sqlite3_result_error(context, "fsl_mtime_of_manifest_file() failed", -1);
  }
}


/**
   SQL UDF binding for fsl_content_get().

   FSL_CONTENT(RID INTEGER | SYMBOLIC_NAME) returns the
   undeltified/uncompressed content of the [blob] record identified by
   the given RID or symbolic name.
*/
static void fsl__udf_blob_content(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  fsl_cx * const f = sqlite3_user_data(context);
  fsl_id_t rid = 0;
  char const * arg;
  int rc;
  fsl_buffer b = fsl_buffer_empty;
  assert(f);
  if(1 != argc){
    sqlite3_result_error(context, "Expecting one argument", -1);
    return;
  }
  if(SQLITE_INTEGER==sqlite3_value_type(argv[0])){
    rid = (fsl_id_t)sqlite3_value_int64(argv[0]);
    arg = NULL;
  }else{
    arg = (const char*)sqlite3_value_text(argv[0]);
    if(!arg){
      sqlite3_result_error(context, "Invalid argument", -1);
      return;
    }
    rc = fsl_sym_to_rid(f, arg, FSL_SATYPE_ANY, &rid);
    if(rc) goto cx_err;
    else if(!rid){
      sqlite3_result_error(context, "No blob found", -1);
      return;
    }
  }
  rc = fsl_content_get(f, rid, &b);
  if( 0==rc ){
    /* Curiously, i'm seeing no difference in allocation counts here whether
       we copy the blob here or pass off ownership... */
    sqlite3_result_blob(context, b.mem, (int)b.used, fsl_free
                        /* transfer b.mem ownership  ^^^^^^^^ */);
    return;
  }
  cx_err:
  fsl_buffer_clear(&b);
  if(FSL_RC_OOM==rc){
    sqlite3_result_error_nomem(context);
  }else if( f->error.msg.used ){
    assert(f->error.msg.used);
    sqlite3_result_error(context, (char const *)f->error.msg.mem,
                         (int)f->error.msg.used);
  }else{
    sqlite3_result_error(context, fsl_rc_cstr(rc), -1);
  }
}



/**
   Impl for SQL UDF binding for fsl_uncompress(X) and fsl_compress(X)

   If its argument appears to be fossil-(un)compressed, it is
   (un)compressed and that value is returned, else the argument is
   returned as-is.
*/
static void fsl__udf_press(
  bool isCompress,
  sqlite3_context *cx, int argc, sqlite3_value **argv
){
  sqlite3_value * const arg = argc ? argv[0] : NULL;
  void const * const mem = arg ? sqlite3_value_blob(arg) : NULL;
  int const nMem = mem ? sqlite3_value_bytes(arg) : 0;
  if( !mem || !nMem ){
    sqlite3_result_value(cx, arg);
    return;
  }
  if( isCompress==fsl_data_is_compressed(mem, (fsl_size_t)nMem) ){
    sqlite3_result_value(cx, arg);
    return;
  }

  fsl_buffer b = fsl_buffer_empty;
  fsl_buffer_external(&b, mem, (fsl_int_t)nMem);
  int const rc = isCompress
    ? fsl_buffer_compress(&b, &b)
    : fsl_buffer_uncompress(&b, &b);
  switch( rc ){
    case 0:
      sqlite3_result_blob(cx, b.mem, (int)b.used, SQLITE_TRANSIENT);
      break;
    case FSL_RC_OOM:
      sqlite3_result_error_nomem(cx);
      break;
    default:
      sqlite3_result_error(cx, fsl_rc_cstr(rc), -1);
      break;
  }
  fsl_buffer_clear(&b);
}

static void fsl__udf_uncompress(
  sqlite3_context *cx, int argc, sqlite3_value **argv
){
  fsl__udf_press(false, cx, argc, argv);
}
static void fsl__udf_compress(
  sqlite3_context *cx, int argc, sqlite3_value **argv
){
  fsl__udf_press(true, cx, argc, argv);
}

/**
   SQL UDF FSL_SYM2RID(SYMBOLIC_NAME) resolves the name to a [blob].[rid]
   value or triggers an error if it cannot be resolved.

   Maintenance note: fossil(1) has since expanded this UDF to take 2
   args: (name [, type='ci']). where "type" is the conventional
   short-hand form of the artifact type (ci, t, etc.). libfossil,
   otoh, has always accepted only 1 argument and assumed "*" for the
   artifact type. It also supports a "+" suff
*/
static void fsl__udf_sym2rid(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  fsl_cx * const f = (fsl_cx*)sqlite3_user_data(context);
  char const * arg;
  assert(f);
  if(1 != argc){
    sqlite3_result_error(context, "Expecting one argument", -1);
    return;
  }
  arg = (const char*)sqlite3_value_text(argv[0]);
  if(!arg){
    sqlite3_result_error(context, "Expecting a STRING argument", -1);
  }else{
    fsl_id_t rid = 0;
    int const rc = fsl_sym_to_rid(f, arg, FSL_SATYPE_ANY, &rid);
    if(rc){
      if(FSL_RC_OOM==rc){
        sqlite3_result_error_nomem(context);
      }else{
        assert(f->error.msg.used);
        sqlite3_result_error(context, (char const *)f->error.msg.mem,
                             (int)f->error.msg.used);
      }
      fsl_cx_err_reset(f)
        /* This is arguable but keeps this error from poluting
           down-stream code (seen it happen in unit tests).  The irony
           is, it's very possible/likely that the error will propagate
           back up into f->error at some point.
        */;
    }else{
      /* fossil(1) translates rid==0 to NULL here. */
      assert(rid>0);
      sqlite3_result_int64(context, rid);
    }
  }
}

static void fsl__udf_dirpart(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  char const * arg;
  int rc;
  fsl_buffer b = fsl_buffer_empty;
  int fSlash = 0;
  if(argc<1 || argc>2){
    sqlite3_result_error(context,
                         "Expecting (string) or (string,bool) arguments",
                         -1);
    return;
  }
  arg = (const char*)sqlite3_value_text(argv[0]);
  if(!arg){
    sqlite3_result_error(context, "Invalid argument", -1);
    return;
  }
  if(argc>1){
    fSlash = sqlite3_value_int(argv[1]);
  }
  rc = fsl_file_dirpart(arg, -1, &b, fSlash ? 1 : 0);
  if(!rc){
    if(b.used && *b.mem){
#if 0
      sqlite3_result_text(context, (char const *)b.mem,
                          (int)b.used, SQLITE_TRANSIENT);
#else
      sqlite3_result_text(context, (char const *)b.mem,
                          (int)b.used, fsl_free);
      b = fsl_buffer_empty /* we passed ^^^^^ on ownership of b.mem */;
#endif
    }else{
      sqlite3_result_null(context);
    }
  }else{
    if(FSL_RC_OOM==rc){
      sqlite3_result_error_nomem(context);
    }else{
      sqlite3_result_error(context, "fsl_dirpart() failed!", -1);
    }
  }
  fsl_buffer_clear(&b);
}


/*
   Implement the user() SQL function.  user() takes no arguments and
   returns the user ID of the current user.
*/
static void fsl__udf_user(
  sqlite3_context *context,
  int argc fsl__unused,
  sqlite3_value **argv fsl__unused
){
  fsl_cx * f = (fsl_cx*)sqlite3_user_data(context);
  assert(f);
  (void)argc;
  (void)argv;
  if(f->db.repo.user){
    sqlite3_result_text(context, f->db.repo.user, -1, SQLITE_STATIC);
  }else{
    sqlite3_result_null(context);
  }
}

/**
   SQL function:

   fsl_is_enqueued(vfile.id)
   fsl_if_enqueued(vfile.id, X, Y)

   On the commit command, when filenames are specified (in order to do
   a partial commit) the vfile.id values for the named files are
   loaded into the fsl_cx state.  This function looks at that state to
   see if a file is named in that list.

   In the first form (1 argument) return TRUE if either no files are
   named (meaning that all changes are to be committed) or if id is
   found in the list.

   In the second form (3 arguments) return argument X if true and Y if
   false unless Y is NULL, in which case always return X.
*/
static void fsl__udf_selected_for_checkin(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  int rc = 0;
  fsl_cx * f = (fsl_cx*)sqlite3_user_data(context);
  fsl_id_bag * bag = &f->ckin.selectedIds;
  assert(argc==1 || argc==3);
  if( bag->entryCount ){
    fsl_id_t const iId = (fsl_id_t)sqlite3_value_int64(argv[0]);
    rc = iId ? (fsl_id_bag_contains(bag, iId) ? 1 : 0) : 0;
  }else{
    rc = 1;
  }
  if(1==argc){
    sqlite3_result_int(context, rc);
  }else{
    assert(3 == argc);
    assert( rc==0 || rc==1 );
    if( sqlite3_value_type(argv[2-rc])==SQLITE_NULL ) rc = 1-rc;
    sqlite3_result_value(context, argv[2-rc]);
  }
}

/**
   fsl_match_vfile_or_dir(p1,p2)

   A helper for resolving expressions like:

   WHERE pathname='X' C OR
      (pathname>'X/' C AND pathname<'X0' C)

   i.e. is 'X' a match for the LHS or is it a directory prefix of
   LHS?

   C = empty or COLLATE NOCASE, depending on the case-sensitivity
   setting of the fsl_cx instance associated with
   sqlite3_user_data(context). p1 is typically vfile.pathname or
   vfile.origname, and p2 is the string being compared against that.

   Resolves to NULL if either argument is NULL, 0 if the comparison
   shown above is false, 1 if the comparison is an exact match, or 2
   if p2 is a directory prefix part of p1.
*/
static void fsl__udf_match_vfile_or_dir(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  fsl_cx * f = (fsl_cx*)sqlite3_user_data(context);
  char const * p1;
  char const * p2;
  fsl_buffer * b = 0;
  int rc = 0;
  assert(f);
  if(2 != argc){
    sqlite3_result_error(context, "Expecting two arguments", -1);
    return;
  }
  p1 = (const char*)sqlite3_value_text(argv[0]);
  p2 = (const char*)sqlite3_value_text(argv[1]);
  if(!p1 || !p2){
    sqlite3_result_null(context);
    return;
  }
  int (*cmp)(char const *, char const *) =
    fsl_cx_is_case_sensitive(f, false) ? fsl_stricmp : fsl_strcmp;
  if(0==cmp(p1, p2)){
    sqlite3_result_int(context, 1);
    return;
  }
  b = fsl__cx_scratchpad(f);
  rc = fsl_buffer_appendf(b, "%s/", p2);
  if(rc) goto oom;
  else if(cmp(p1, fsl_buffer_cstr(b))>0){
    b->mem[b->used-1] = '0';
    if(cmp(p1, fsl_buffer_cstr(b))<0)
    rc = 2;
  }
  assert(0==rc || 2==rc);
  sqlite3_result_int(context, rc);
  end:
  fsl__cx_scratchpad_yield(f, b);
  return;
  oom:
  sqlite3_result_error_nomem(context);
  goto end;
}

/**
   F(glob-list-name, filename)

   Returns 1 if the 2nd argument matches any glob in the fossil glob
   list named by the first argument. The first argument must be a name
   resolvable via fsl_glob_name_to_category() or an error is
   triggered. The second value is intended to be a string, but NULL is
   accepted (but never matches anything).

   If no match is found, 0 is returned. An empty glob list never matches
   anything.
*/
static void fsl__udf_cx_glob(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  fsl_cx * const f = (fsl_cx*)sqlite3_user_data(context);
  fsl_list * li = NULL;
  fsl_glob_category_e globType;
  char const * p1;
  char const * p2;
  assert(2<=argc);
  p2 = (const char*)sqlite3_value_text(argv[1])/*value to check*/;
  if(NULL==p2 || 0==p2[0]){
    sqlite3_result_int(context, 0);
    return;
  }
  p1 = (const char*)sqlite3_value_text(argv[0])/*glob set name*/;
  globType  = fsl_glob_name_to_category(p1);
  if(FSL_GLOBS_INVALID==globType){
    char buf[100] = {0};
    buf[sizeof(buf)-1] = 0;
    fsl_snprintf(buf, (fsl_size_t)sizeof(buf)-1,
                 "Unknown glob pattern name: %#.*s",
                 50, p1 ? p1 : "NULL");
    sqlite3_result_error(context, buf, -1);
    return;
  }
  fsl_cx_glob_list(f, globType, &li, false);
  assert(li);
  sqlite3_result_int(context, fsl_glob_list_matches(li, p2) ? 1 : 0);
}

static void fsl__udf_cx_a2j(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  fsl_cx * const f = (fsl_cx*)sqlite3_user_data(context);
  fsl_buffer * b = NULL;
  int rc;
  fsl_deck d = fsl_deck_empty;
  if(1 != argc){
    goto error_usage;
  }
  switch( sqlite3_value_type(argv[0]) ){
    case SQLITE_INTEGER:
      rc = fsl_deck_load_rid(f, &d, (fsl_id_t)sqlite3_value_int(argv[0]),
                             FSL_SATYPE_ANY);
      break;
    case SQLITE_TEXT:
      rc = fsl_deck_load_sym(f, &d,
                             (const char *)sqlite3_value_text(argv[0]),
                             FSL_SATYPE_ANY);
      break;
    default:
      goto error_usage;
  }
  switch(rc){
    case 0: break;
    case FSL_RC_OOM:
      sqlite3_result_error_nomem(context);
      goto cleanup;
    default:
      sqlite3_result_null(context);
      // ^^^ for compat with fossil(1)
      /*sqlite3_result_error(context, (char const *)f->error.msg.mem,
        (int)f->error.msg.used);*/
      goto cleanup;
  }
  b = fsl__cx_content_buffer(f);
  rc = fsl_deck_to_json(&d, fsl_output_f_buffer, b);
  if( rc ){
    if( FSL_RC_OOM==rc ){
      sqlite3_result_error_nomem(context);
    }else{
      sqlite3_result_null(context);
    }
  }else{
    fsl_size_t n = 0;
    const char * z = fsl_buffer_cstr2(b, &n);
    sqlite3_result_text(context, z, (int)n, SQLITE_TRANSIENT);
  }
cleanup:
  if(b) fsl__cx_content_buffer_yield(f);
  fsl_deck_finalize(&d);
  return;
error_usage:
  sqlite3_result_error(context, "Expecting one argument: blob.rid or "
                       "artifact symbolic name", -1);
}

/**
   Plug in fsl_cx-specific db functionality into the given db handle
   and sets it as f->dbMain. This must only be passed the MAIN db
   handle for the context.
*/
int fsl__cx_init_db(fsl_cx * const f, fsl_db * const db){
  int rc;
  assert(!f->dbMain);
  f->dbMain = db;
  db->role = FSL_DBROLE_MAIN;
  /* This all comes from db.c:db_open()... */
  /* FIXME: check result codes here. */
  sqlite3 * const dbh = db->dbh;
  sqlite3_busy_timeout(dbh, 5000 /* historical value */);
  sqlite3_wal_autocheckpoint(dbh, 1);  /* Set to checkpoint frequently */
  rc = fsl_cx_exec_multi(f,
                         "PRAGMA foreign_keys=OFF;"
                         // ^^^ vmerge table relies on this for its magical
                         // vmerge.id values.
                         //"PRAGMA main.temp_store=FILE;"
                         //"PRAGMA main.journal_mode=TRUNCATE;"
                         // ^^^ note that WAL is not possible on a TEMP db
                         // and OFF leads to undefined behaviour if
                         // ROLLBACK is used!
                         );
  if(rc) goto end;
#define FLAGS_D SQLITE_UTF8 | SQLITE_DETERMINISTIC
#define FLAGS_I SQLITE_UTF8 | SQLITE_INNOCUOUS
#define FLAGS_DI FLAGS_D | SQLITE_INNOCUOUS
  sqlite3_create_function(dbh, "now", 0, SQLITE_UTF8, 0,
                          fsl__udf_now, 0, 0);
  sqlite3_create_function(dbh, "fsl_ci_mtime", 2,
                          FLAGS_D, f,
                          fsl__udf_checkin_mtime, 0, 0);
  sqlite3_create_function(dbh, "fsl_user", 0,
                          FLAGS_DI, f,
                          fsl__udf_user, 0, 0);
  sqlite3_create_function(dbh, "fsl_print", -1,
                          SQLITE_UTF8
                          /* not strictly SQLITE_DETERMINISTIC
                             because it produces arbitrary output */,
                          f, fsl__udf_print,0,0);
  sqlite3_create_function(dbh, "fsl_content", 1,
                          FLAGS_D, f,
                          fsl__udf_blob_content, 0, 0);
  sqlite3_create_function(dbh, "fsl_sym2rid", 1,
                          FLAGS_I,
                          f,
                          fsl__udf_sym2rid, 0, 0);
  sqlite3_create_function(dbh, "fsl_dirpart", 1,
                          FLAGS_D, NULL,
                          fsl__udf_dirpart, 0, 0);
  sqlite3_create_function(dbh, "fsl_dirpart", 2,
                          FLAGS_D, NULL,
                          fsl__udf_dirpart, 0, 0);
  sqlite3_create_function(dbh, "fsl_j2u", 1,
                          FLAGS_DI, NULL,
                          fsl__udf_j2u, 0, 0);
  sqlite3_create_function(dbh, "fsl_artifact_to_json", 1,
                          SQLITE_UTF8, f, fsl__udf_cx_a2j, 0, 0 );

  sqlite3_create_function(dbh, "fsl_uncompress", 1,
                          FLAGS_D, NULL,
                          fsl__udf_uncompress, 0, 0);
  sqlite3_create_function(dbh, "fsl_compress", 1,
                          FLAGS_D, NULL,
                          fsl__udf_compress, 0, 0);

  /*
    fsl_i[sf]_selected() both require access to the f's list of
    files being considered for commit.
  */
  sqlite3_create_function(dbh, "fsl_is_enqueued", 1, SQLITE_UTF8, f,
                          fsl__udf_selected_for_checkin,0,0 );
  sqlite3_create_function(dbh, "fsl_if_enqueued", 3, SQLITE_UTF8, f,
                          fsl__udf_selected_for_checkin,0,0 );

  sqlite3_create_function(dbh, "fsl_ckout_dir", -1,
                          FLAGS_D,
                          f, fsl__udf_chkout_dir,0,0 );
  sqlite3_create_function(dbh, "fsl_match_vfile_or_dir", 2,
                          FLAGS_D,
                          f, fsl__udf_match_vfile_or_dir,0,0 );
  sqlite3_create_function(dbh, "fsl_glob", 2,
                          FLAGS_D,
                          /* noting that ^^^^^ it's only deterministic
                             for a given statement execution IF no SQL
                             triggers an effect which forces the globs to
                             reload. That "shouldn't ever happen." */
                          f, fsl__udf_cx_glob, 0, 0 );

#if FSL_ENABLE_SQLITE_REGEXP
  rc = sqlite3_regexp_init(dbh, NULL, NULL);
  if( rc ) { rc = FSL_RC_ERROR; goto end; }
#endif

  rc = fsl__foci_register(f, db);
  end:
#undef FLAGS_D
#undef FLAGS_I
#undef FLAGS_DI
  return rc;
}


#undef MARKER
