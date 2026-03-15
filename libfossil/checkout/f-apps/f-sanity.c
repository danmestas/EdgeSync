/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/
/********************************************************************
  This file holds some basic libfossil sanity test code.
*/

#include "fossil-scm/repo.h"
#ifdef NDEBUG
/* Force assert() to always be in effect. */
#undef NDEBUG
#endif

#ifdef _MSC_VER
#define THIS_SRC_FNAME "f-apps/f-sanity.c"
/*A peculiarity of msvc x64 compiler (and not x86!) is that __FILE__ will be
a relative path as seen at compile-time.  But since the cwd of the compiler is
not the source dir, this relative path will be incorrect at runtime, when our
cwd IS in the source dir, and tests will fail.*/
#else
// 2021-12-21: since moving to a top-level makefile, we can't use __FILE__ here...
#define THIS_SRC_FNAME "f-apps/f-sanity.c"
#endif

#include "libfossil.h"
#include "fossil-scm/internal.h"
#include <string.h>
#include <time.h>
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

#define TEST_HEADER \
  f_out(">>>START: %s()<<<\n", __func__); \
  assert( 0==fsl_db_txn_level(fsl_cx_db(fcli_cx())) )
#define TEST_FOOTER f_out("<<<DONE: %s()>>>\n", __func__)

static char dirBuffer[1024] = {0};
/*
  Workaround for many tests historically wanting to run from
  ./f-apps/., which were then rudely moved to the top dir in May
  2025.
*/
static void test_pushd(const char *z){
  int const rc = fsl_chdir2(z, dirBuffer, sizeof(dirBuffer));
  assert(!rc);
}

static void test_popd(void){
  assert(dirBuffer[0] && "Else test_push() was not called.");
  int rc = fsl_chdir(&dirBuffer[0]);
  assert(!rc);
  dirBuffer[0] = 0;
}

static int test_sanity_repo(void){
  fsl_cx * const f = fcli_cx();
  int rc = 0;
  fsl_buffer buf = fsl_buffer_empty;
  fsl_deck d = fsl_deck_empty;
  fsl_id_t rid;
  fsl_uuid_str uuid = NULL;
  char * str;
  fsl_db * const db = fsl_cx_db_repo(f);
  fsl_size_t slen, slen2;

  TEST_HEADER;
  if(0){
    /* Must exit the app using fsl__fatal()! */
    fsl_db_txn_begin(db);
    fsl_db_exec(db, "COMMIT");
  }
  assert( fsl_db_is_writeable(db, 0) );
  assert( !fsl_db_is_writeable(db, "nope") );
  f_out("Running basic uuid/rid tests against this repo...\n");
  assert( 1==fsl_uuid_to_rid(f, "99237c3636730f20ed07b227c5092c087aea8b0c") );
  assert( 1==fsl_uuid_to_rid(f, "99237c363673"));
  assert( 0>fsl_uuid_to_rid(f, "992") );
  assert(f->error.code == FSL_RC_AMBIGUOUS);
  assert( strstr(fsl_buffer_cstr(&f->error.msg), "ambig") );
  fcli_err_reset();
  assert(0==f->error.code);
  assert(0==f->error.msg.used);
  assert(0<f->error.msg.capacity);

  rc = fsl_sym_to_rid(f, "prev", FSL_SATYPE_CHECKIN, &rid);
  assert(!rc);
  assert(rid>0);
  rc = fsl_content_get(f, rid, &buf);
  assert(!rc);
  /* assert('B' == *fsl_buffer_cstr(&buf)); */
  d.f = f;
  rc = fsl_deck_parse2(&d, &buf, rid);
  assert(0==rc);
  fsl_buffer_clear(&buf);
  assert(FSL_SATYPE_CHECKIN==d.type);
  assert(rid==d.rid);
  uuid = fsl_rid_to_uuid(f, d.rid);
  assert(uuid);
  fsl_free(uuid);
  assert(d.D>0 && d.D<fsl_julian_now());
  fsl_deck_finalize(&d);
  assert(0==rc);

  slen = 0;
  str = fsl_db_g_text(db, &slen, "SELECT FSL_USER()");
  assert(str && "fsl_user() SQL func is broken");
  /* f_out("SELECT fsl_user()=%s\n", str); */
  assert(0==fsl_strcmp(str, fsl_cx_user_get(f)));
  fsl_free(str);

  slen = 0;
  str = fsl_db_g_text(db, &slen, "select fsl_content(1)");
  assert(str && slen && "fsl_content() SQL func is broken");
  fsl_free(str);
  assert(168 == slen) /* size of rid 1 in libfossil */;
  /* f_out("SELECT fsl_content() got %"FSL_SIZE_T_PFMT" bytes\n", (fsl_size_t)slen); */

  slen2 = 0;
  str = fsl_db_g_text(db, &slen2, "select fsl_content('rid:1')");
  assert(str && slen2 && "fsl_content(sym) SQL func is broken");
  assert(slen2==slen);
  fsl_free(str);
  /* f_out("SELECT fsl_content(rid) got %"FSL_SIZE_T_PFMT" bytes\n", (fsl_size_t)slen2); */

  rid = fsl_db_g_id(db, -1, "select fsl_sym2rid('root:7bfbc3dba6c65')");
  assert(1==rid);
  assert(!fsl_rid_is_leaf(f, rid));

  if(1){
    rid = 0;
    rc = fsl_sym_to_rid(f, "trunk", FSL_SATYPE_CHECKIN, &rid);
    assert(!rc);
    assert(rid>0);
    assert(fsl_rid_is_leaf(f, rid));
  }

  /* f_out("SELECT fsl_sym2rid(...) got %"FSL_ID_T_PFMT"\n", (fsl_id_t)rid); */

  rid = fsl_db_g_id(db, -666, "select fsl_sym2rid('abcdefabcdef')");
  assert(-666==rid && "Very unexpected RID.");
  assert(db->error.code);
  assert(db->error.msg.used);
  assert(!f->error.code && "fsl_sym2rid() did not clear f's error state");
  fcli_err_report(false)/* will move the error into f! */;
  f_out("Caught expected SQL-triggered error: %b\n", &f->error.msg);
  assert(f->error.code && "fcli_err_report() did not uplift the db error");
  fsl_db_err_reset(db);
#if 1
  fcli_err_reset() /* b/c fsl_sym2rid() (indirectly) sets error at the fsl_cx context level */;
#endif

  fsl_uuid_str zUuid = 0;
#if 0
  /* of course this only works when we have the central copy's shun
     list. How have i gotten by with this test all these years? */
  char const * zShunned = "28281415c8a79ef9a4040643a56c8ba9ad348b06";
  rid = 0;
  rc = fsl_db_get_id(fsl_needs_repo(f), &rid,
                     "SELECT 1 FROM shun WHERE uuid=%Q", zShunned);
  assert( 0==rc );
  assert( rid>0 );
  assert( fsl_is_shunned_uuid(f, zShunned ) );

  zUuid = fsl_rid_to_uuid(f, rid);
  assert( zUuid );
  fsl_free( zUuid );
#endif

  rid = 0;
  rc = fsl_sym_to_uuid(f, "tip", FSL_SATYPE_CHECKIN, &zUuid, &rid);
  assert(rid>0);
  assert( zUuid );
  assert( !fsl_is_shunned_uuid(f, zUuid) );
  fsl_free( zUuid );

  TEST_FOOTER;
  return 0;
}

static int test_sanity_delta(void){
  fsl_buffer f1 = fsl_buffer_empty,
    f2 = fsl_buffer_empty,
    d12 = fsl_buffer_empty,
    d21 = fsl_buffer_empty,
    a1 = fsl_buffer_empty,
    a2 = fsl_buffer_empty;
  int rc = 0;
  char const * F1 = THIS_SRC_FNAME;
  char const * F2 = "f-apps/f-sizeof.c";
  fsl_size_t len1 = 0, len2 = 0;
  TEST_HEADER;
  rc = fsl_buffer_fill_from_filename(&f1, F1);
  assert(!rc);
  rc = fsl_buffer_fill_from_filename(&f2, F2);
  assert(!rc);
  f_out("Input file sizes: f1: %"FSL_SIZE_T_PFMT", f2: %"FSL_SIZE_T_PFMT"\n",
        f1.used, f2.used);

  rc = fsl_buffer_delta_create(&f1, &f2, &d12);
  assert(!rc);
  rc = fsl_buffer_delta_create(&f2, &f1, &d21);
  assert(!rc);

  f_out("Delta sizes: f1=>f2: %"FSL_SIZE_T_PFMT", f2=>f1: %"FSL_SIZE_T_PFMT"\n",
        d12.used, d21.used);
  /* f_out("%b\n", &d12); */

  rc = fsl_delta_applied_size(d12.mem, d12.used, &len1);
  assert(!rc);
  assert(len1==f2.used);
  rc = fsl_delta_applied_size(d21.mem, d21.used, &len2);
  assert(!rc);
  assert(len2==f1.used);

  rc = fsl_buffer_delta_apply(&f1, &d12, &a2);
  assert(!rc);
  rc = fsl_buffer_delta_apply(&f2, &d21, &a1);
  assert(!rc);
  if( fsl_buffer_compare(&f1,&a1) || fsl_buffer_compare(&f2, &a2) ){
    fsl__fatal(FSL_RC_CONSISTENCY, "delta test failed");
  }
  fsl_buffer_clear(&f1);
  fsl_buffer_clear(&f2);
  fsl_buffer_clear(&a1);
  fsl_buffer_clear(&a2);
  fsl_buffer_clear(&d12);
  fsl_buffer_clear(&d21);
  TEST_FOOTER;
  return rc;
}

static void test_sanity_tkt_01(void){
  fsl_buffer sql = fsl_buffer_empty;
  fsl_cx * f = fcli_cx();
  char const * orig = fsl_schema_ticket();
  int rc = fsl_cx_schema_ticket(f, &sql);
  assert(!rc);
  assert(sql.used>200);
  if(0) f_out("Ticket schema size=%"FSL_SIZE_T_PFMT
              ", default schema size=%"FSL_SIZE_T_PFMT"\n",
              sql.used, fsl_strlen(orig));
  fsl_buffer_clear(&sql);
  TEST_FOOTER;
}

static void test_sanity_tkt_fields(void){
  fsl_cx * f = fcli_cx();
  int rc;
  f_out("Loading custom ticket fields...\n");
  rc = fsl__cx_ticket_load_fields(f, 0);
  assert(!rc);
  f_out("Ticket field count=%"FSL_SIZE_T_PFMT": ",
        f->ticket.customFields.used);
  for(rc = 0; rc < (int)f->ticket.customFields.used; ++rc){
    fsl_card_J const * jc = f->ticket.customFields.list[rc];
    f_out( "%s%s", rc ? ", " : "", jc->field);
  }
  f_out("\n");
  TEST_FOOTER;
}


static void test_fs_mkdir(void){
  int rc;
  char const * path = "_sanity/foo/bar/baz/";
  fsl_buffer b = fsl_buffer_empty;
  fsl_fstat fst = fsl_fstat_empty;
  f_out("fsl_mkdir_for_file(%s,...)...\n", path);
  rc = fsl_mkdir_for_file(path, 1);
  assert(0==rc);
  rc = fsl_stat( path, &fst, 1);
  assert(0==rc);
  assert(FSL_FSTAT_TYPE_DIR==fst.type);
  fsl_buffer_appendf(&b, "%//", THIS_SRC_FNAME);
  rc = fsl_mkdir_for_file(fsl_buffer_cstr(&b), 0);
  assert(FSL_RC_TYPE==rc);
  path = "f-apps/f-sanity.c";
  assert(fsl_is_file(path));
  rc = fsl_mkdir_for_file(path, 0);
  assert(0==rc) /* b/c no path component, nothing to do */;
  b.used = 0;
  fsl_buffer_appendf(&b, "%s/", path);
  rc = fsl_mkdir_for_file(fsl_buffer_cstr(&b), 0);
  assert(FSL_RC_TYPE==rc);
  fsl_buffer_clear(&b);
  TEST_FOOTER;
}

static void test_fs_cx_stat(void){
  fsl_cx * f = fcli_cx();
  int rc;
  fsl_fstat fst = fsl_fstat_empty;
  int64_t time1, time2;
  char const * fname = "src/cli.c";
  fsl_buffer buf = fsl_buffer_empty;
  fsl_time_t const now = time(0);
  f_out("fsl_cx_stat()...\n");
  rc = fsl_cx_stat( f, 1, "no-such-file", &fst );
  assert(FSL_RC_NOT_FOUND==rc);
  fcli_err_reset();
  rc = fsl_cx_stat( f, 0, fname, &fst );

  assert(0==rc);
  assert(FSL_FSTAT_TYPE_FILE==fst.type);
  assert(fst.ctime>0 && fst.ctime<now);
  assert(fst.mtime>0 && fst.mtime<now);
  assert(fst.size>0);


  fname = "src/cli.c";
  rc = fsl_cx_stat( f, 1, fname, &fst );
  assert(0==rc);

  fsl_buffer_reuse(&buf);
  rc = fsl_cx_stat2( f, true, fname, &fst, &buf, false );
  assert(0==rc);
  assert(FSL_FSTAT_TYPE_FILE==fst.type);
  f_out("rc=%s buf=[%b]\n", fsl_rc_cstr(rc), &buf);
  assert(0==memcmp(buf.mem,"src/cli.c",9));

  fname = "src/cli.c";
  fsl_buffer_reuse(&buf);
  rc = fsl_cx_stat2( f, false, fname, &fst, &buf, false );
  assert(0==rc);
  assert(FSL_FSTAT_TYPE_FILE==fst.type);
  f_out("rc=%s buf=[%b]\n", fsl_rc_cstr(rc), &buf);
  assert(0==memcmp(buf.mem,"src/cli.c",9));

  fsl_buffer_reuse(&buf);
  rc = fsl_cx_stat2( f, false, "src/", &fst, &buf, false );
  f_out("rc=%s buf=[%b]\n", fsl_rc_cstr(rc), &buf);
  assert(0==rc);
  assert(FSL_FSTAT_TYPE_DIR==fst.type);
  assert('/' == buf.mem[buf.used-1]);

  fsl_buffer_reuse(&buf);
  rc = fsl_cx_stat2( f, false, "src", &fst, &buf, false );
  f_out("rc=%s buf=[%b]\n", fsl_rc_cstr(rc), &buf);
  assert(0==rc);
  assert(FSL_FSTAT_TYPE_DIR==fst.type);
  assert('/' != buf.mem[buf.used-1]);


  fsl_buffer_reuse(&buf);
  rc = fsl_cx_stat2( f, false, "./", &fst, &buf, false );
  f_out("buf=[%b]\n", &buf);
  assert(0==rc);
  assert(FSL_FSTAT_TYPE_DIR==fst.type);
  assert('/' == buf.mem[buf.used-1]);

#if !defined(_WIN32)
  /*
    This next test will likely not work on Windows, but we need a file
    we can 'touch' without invaliding build dependencies
    (e.g. touching the Makefile or one of the sources).
  */
  fname = fcli.appName;
  rc = fsl_cx_stat(f, 1, fname, &fst);
  assert(0==rc);
  time1 = fst.mtime;
  rc = fsl_file_mtime_set(fname, now+1);
  assert(0==rc);
  f_out("old mtime=%"PRIi64"\n", time1);
  rc = fsl_cx_stat(f, 1, fname, &fst);
  assert(0==rc);
  time2 = fst.mtime;
  f_out("new mtime=%"PRIi64"\n", time2);
  assert(time2 > time1);
#endif

  fsl_buffer_clear(&buf);
  TEST_FOOTER;
}

static int fsl_dircrawl_f_test(fsl_dircrawl_state const *st){
  f_out("%.*cdir -> entry type %d = %s -> %s\n",
        ((int)st->depth-1)*4, ' ',
        st->entryType, st->absoluteDir, st->entryName);
  return 0;
}

static void test_fs_dircrawl(void){
  TEST_HEADER;
  char const * path = 0 ? "." : "_sanity"/*created by test_fs_mkdir()*/;
  int rc = fsl_dircrawl(path, fsl_dircrawl_f_test, NULL);
  if(rc){
    MARKER(("crawl rc=%s\n", fsl_rc_cstr(rc)));
  }
  assert(0==rc);
  TEST_FOOTER;
}

static void test_sanity_fs(void){
  char * s = NULL;
  fsl_buffer b = fsl_buffer_empty;
  int rc;
  fsl_fstat fst = fsl_fstat_empty;
  TEST_HEADER;
  rc = fsl_find_home_dir(&b, 1);
  f_out("Home dir: %b\n", &b);
  assert(0==rc);
  b.used = 0;
  rc = fsl_stat(THIS_SRC_FNAME, &fst, 1);
  assert(0==rc);
  assert(FSL_FSTAT_TYPE_FILE == fst.type);
  assert(fst.mtime>0);
  assert(fst.mtime<time(0));

  fsl_free(s);
  fsl_buffer_clear(&b);

  assert(fsl_dir_check(".") > 0);
  assert(fsl_dir_check("no-such-file") == 0);
  assert(fsl_dir_check(fcli.appName) < 0);

  test_fs_mkdir();
  test_fs_cx_stat();
  test_fs_dircrawl();
  TEST_FOOTER;
}

static void test_sanity_localtime(void){
  int64_t now = time(0);
  fsl_cx * f = fcli_cx();
  char * tsNow;
  char * ts2;
  fsl_db * db = fsl_cx_db(f);
  tsNow = fsl_db_unix_to_iso8601(db, now, 0);
#if 1
  ts2 = fsl_db_unix_to_iso8601(db, now, 1);
  f_out("now utc: %s\nlocal:   %s\n",
        tsNow, ts2);
#else
  {
    char * ts1;
    int64_t t1, t2;
    /* still very unsure about this. And it's broken. */
    fsl_cx_flag_set(f, FSL_CX_F_LOCALTIME_GMT, 0 );
    assert(!(f->flags & FSL_CX_F_LOCALTIME_GMT));
    t1 = fsl_cx_time_adj(f, now);
    ts1 = fsl_db_unix_to_iso8601(db, t1, 0);

    fsl_cx_flag_set(f, FSL_CX_F_LOCALTIME_GMT, 1 );
    assert(f->flags & FSL_CX_F_LOCALTIME_GMT);
    t2 = fsl_cx_time_adj(f, now);
    ts2 = fsl_db_unix_to_iso8601(db, t2, 0);

    fsl_cx_flag_set(f, FSL_CX_F_LOCALTIME_GMT, 0 );
    f_out("now=%s\nts1=%s\nts2=%s\n",
          tsNow, ts1, ts2);
    fsl_free(ts1);
  }
#endif
  fsl_free(tsNow);
  fsl_free(ts2);
  TEST_FOOTER;
}

static void test_julian(void){
  char const * ts;
  double tolerance = 0
    /* Tolerance, in Julian Days, for round-trip rounding errors. In
       my tests most time conversions are either exact or +/-1ms, but
       the exact range of error is probably at least partially
       platform-dependent. The differences between the reference JD
       values (taken from sqlite3) and the results are consistently
       (0, 0.000000001, and 0.000000011), which is quite close enough
       for what we do with these.
    */
    ? 1
    : 0.000000012;
  double eB;
  double eE;
  char buf[24];
  double j = 0;
  char rc;
  bool const vbose = 2==fcli_is_verbose();
  struct tester {
    /*
      ISO8601 time string.
    */
    char const * ts;
    /* Expected JD result. This value is normally taken
       from sqlite3's strftime('%J', this->ts).
    */
    double expect;
  } list[] = {
  {"2013-12-13T01:02:03.000", 2456639.543090278},
  {"2013-09-10T23:35:53.471", 2456546.483257755},
  {"2013-09-10T22:35:53.471", 2456546.441591088},
  {"2013-09-10T21:35:53.471", 2456546.399924421},
  {"2013-09-10T20:35:53.471", 2456546.358257755},
  {"2013-09-10T19:35:53.471", 2456546.316591088},
  {"2013-09-10T18:35:53.471", 2456546.274924421},
  {"2013-09-10T17:35:53.471", 2456546.233257755},
  {"2013-09-10T16:35:53.471", 2456546.191591088},
  {"2013-09-10T15:35:53.471", 2456546.149924421},
  {"2013-09-10T14:35:53.471", 2456546.108257755},
  {"2013-09-10T13:14:15.167", 2456546.051564422},
  {"2013-09-10T01:02:03.004", 2456545.543090324},
  {"2013-09-09T12:11:10.987", 2456545.007766053},
  {"2013-09-09T11:10:09.876", 2456544.965392072},
  {"2013-02-02T12:13:14.000", 2456326.009189815},
  {"2013-01-02T12:13:14.000", 2456295.009189815},
  {"2011-12-31T12:51:46.686", 2455927.035957014},
  {"2011-01-01T00:15:54.000", 2455562.511041666},
  {"2008-12-20T02:23:18.000", 2454820.599513888},
  {"2008-01-02T14:53:47.000", 2454468.12068287},
  {"2007-07-21T14:09:59.000", 2454303.090266198},
  {"2013-12-31T23:59:59.997", 2456658.499999954},
  {0,0}
  };
  struct tester * t;
  f_out("iso8601 <==> julian tests...\n");
  for( t = list; t->ts; ++t ){
    ts = t->ts;
    j = 0;
    if(vbose) f_out("\ninput : %s\n", ts);
    rc = fsl_iso8601_to_julian(ts, &j);
    assert(rc);
    if(vbose){
      f_out("julian: %"FSL_JULIAN_T_PFMT"\n", j);
      f_out("expect: %"FSL_JULIAN_T_PFMT"\n", t->expect);
    }
    buf[0] = 0;
    rc = fsl_julian_to_iso8601(j, buf, 1);
    assert(rc);
    if(vbose) f_out("j=>iso: %s\n", buf);
    if(0 != t->expect){
      eB = t->expect - tolerance;
      eE = t->expect + tolerance;
      assert( (j>=eB && j<=eE) );
    }
    if(0!=fsl_strncmp(ts, buf, 22/*not the final digit*/)){
      f_out("WARNING: round-trip mismatch at >ms level "
            "for:"
            "\n\t%"FSL_JULIAN_T_PFMT"\t%s"
            "\n\t%"FSL_JULIAN_T_PFMT"\t%s\n",
            t->expect, ts, j, buf);
    }else{
      rc = fsl_julian_to_iso8601(j, buf, 0);
      if(vbose) f_out("j=>iso: %s\n", buf);
      assert(rc);
      if(0!=fsl_strncmp(ts, buf, 19)){
        /* This is caused by:
           SS.000 ==> (SS-1).999
           in julian-to-iso8601
        */
        f_out("WARNING: mismatch:\n\t%s\n\t%s\n", ts, buf);
      }
    }
  }
  TEST_FOOTER;
}

static void test_julian2(void){
  fsl_cx * f = fcli_cx();
  fsl_db * db = fsl_cx_db_repo(f);
  double tolerance = 0
    /* Tolerance, in Julian Days, for round-trip rounding errors. In
       my tests most time conversions are either exact or +/-1ms, but
       the exact range of error is probably at least partially
       platform-dependent. The differences between the reference JD
       values (taken from sqlite3) and the results are consistently
       (0, 0.000000001, and 0.000000011), which is quite close enough
       for what we do with these.
    */
    ? 1
    : 0.000000012;
  double eB;
  double eE;
  char buf[24];
  int rc;
  bool const vbose = fcli_is_verbose()>1;
  fsl_stmt q = fsl_stmt_empty;
  int counter = 0, warnings = 0, diffBy1Ms = 0;
  f_out("Running all event.mtimes through the julian<==>iso converters...\n");
  rc = fsl_db_prepare(db, &q,
                      "SELECT mtime, strftime('%%Y-%%m-%%dT%%H:%%M:%%f',mtime) "
                      "FROM event ORDER BY mtime DESC "
                      /*"LIMIT 100"*/
                      );
  while( FSL_RC_STEP_ROW == fsl_stmt_step(&q) ){
    char const * ts = fsl_stmt_g_text(&q, 1, NULL);
    double const jexp = fsl_stmt_g_double(&q, 0);
    double j = 0;
    int const oldWarn = warnings;
    ++counter;
    if(vbose) f_out("\ninput : %s\n", ts);
    rc = fsl_iso8601_to_julian(ts, &j);
    assert(rc);
    if(vbose){
      f_out("julian: %"FSL_JULIAN_T_PFMT"\n", j);
      f_out("expect: %"FSL_JULIAN_T_PFMT"\n", jexp);
    }
    assert(j>0.0);
    buf[0] = 0;
    rc = fsl_julian_to_iso8601(j, buf, 1);
    assert(rc);
    if(vbose) f_out("j=>iso: %s\n", buf);
    eB = jexp - tolerance;
    eE = jexp + tolerance;
    assert( (j>=eB && j<=eE) );
    if(0!=fsl_strncmp(ts, buf, 22/*not the final digit!*/)){
      /* See if only the last three digits differ by 1 integer
         point (1ms), and don't warn if that's the case. There's a
         corner case there when N.x99 rolls over, and another for
         N.999, etc., but we'll punt on that problem.
      */
      int const f1 = atoi(ts+20);
      int const f2 = atoi(buf+20);
      int const d = f1 - f2;
      /* assert(f1 || f2); */
      if(d<-1 || d>1){
        f_out("WARNING: possible round-trip fidelity mismatch: "
              "\n\twant: %"FSL_JULIAN_T_PFMT"\t%s"
              "\n\tgot : %"FSL_JULIAN_T_PFMT"\t%s\n",
              jexp, ts, j, buf);
        if(1==++warnings){
          f_out("These are normally caused by, e.g., N.789 ==> N.790 "
                "or vice versa, which is still well within tolerance "
                "but is more difficult to test against in string form.\n"
                );
        }
      }else{
        if(0!=fsl_strncmp(ts, buf, 19)){
          f_out("Mismatch at YMDHmS level:\n\t%s\n\t%s\n", ts, buf);
        }
        ++diffBy1Ms;
      }
    }
    /* Try without fractional part. These should never mismatch. */
    rc = fsl_julian_to_iso8601(j, buf, 0);
    if(vbose) f_out("j=>iso: %s\n", buf);
    assert(rc);
    if(0!=fsl_strncmp(ts, buf, 19)){
      /* f_out("VERY UNPEXECTED MISMATCH:\n\t%.*s\n\t%.*s\n", 19, ts, 19, buf); */
      /* assert(!"These should never mismatch. "
         "Unless, of course, the SS.999 problem hits."); */
      if(oldWarn == warnings) ++warnings /* avoid double counting */;
      /* This is caused by:
         SS.000 ==> (SS-1).999
         in julian-to-iso8601. But that's "fixed" now - we shouldn't
         see this anymore. Still seeing it in the main fossil repo
         for about 0.27% of the records :/.
      */
    }
  }
  fsl_stmt_finalize(&q);
  f_out("%d Julian timestamps tested from event.mtime.\n", counter);
  if(diffBy1Ms>0){
    f_out("%d record(s) (%3.2f%%) differed round-trip by 1ms "
          "(this is \"normal\"/not unexpected).\n", diffBy1Ms,
          ((diffBy1Ms+0.0)/(counter+0.0)*100));
    assert((1.0 > ((diffBy1Ms+0.0)/(counter+0.0)*100))
          /*^^^ 1% was arbitrarily chosen! Increase if needed! */
           && "Suspiciously many Julian/ISO8601 off-by-1ms conversions.");
  }
  if(warnings>0){
    f_out("ACHTUNG: conversion warning count: %d (%3.2f%% of total)\n",
          warnings, ((warnings+0.0)/(counter+0.0)*100));
    assert((1.0 > ((warnings+0.0)/(counter+0.0)*100))
          /*^^^ 1% based on current rate of 0.51% of 779 records. */
           && "Suspiciously many Julian/ISO8601 conversion warnings.");
  }
  TEST_FOOTER;
}


static void test_pathfinder(void){
  fsl_pathfinder PF = fsl_pathfinder_empty;
  fsl_pathfinder * pf = &PF;
  char const * found = NULL;
  int rc;
  f_out("fsl_pathfinder sanity checks...\n");
  /* add() cannot fail here b/c of custom allocator, resp.
     it will abort() if it fails. */
  fsl_pathfinder_dir_add(pf, "src");
  fsl_pathfinder_ext_add(pf, ".c");

  rc = fsl_pathfinder_search(pf, "fs", &found, NULL);
  assert(0==rc);
  assert(strstr(found, "src/fs.c"));
  f_out("pathfinder found %s\n", found);
  rc = fsl_pathfinder_search(pf, "nono", &found, NULL);
  assert(FSL_RC_NOT_FOUND==rc);

  fsl_pathfinder_clear(pf);
  TEST_FOOTER;
}

static void test_fs_dirpart(void){
  fsl_buffer B = fsl_buffer_empty;
  fsl_buffer * b = &B;
  char const * cwd = "/foo/bar/baz";
  fsl_size_t n = fsl_strlen(cwd);
  int rc;
  f_out("fsl_file_dirpart() tests...\n");
  rc = fsl_file_dirpart(cwd, (fsl_int_t)n, b, 0);
  assert(0==rc);
  assert(0==fsl_strcmp("/foo/bar", fsl_buffer_cstr(b)));
  assert('/'!=b->mem[b->used-1]);

  b->used = 0;
  rc = fsl_file_dirpart(cwd, (fsl_int_t)n, b, 1);
  assert(0==rc);
  assert(0==fsl_strcmp("/foo/bar/", fsl_buffer_cstr(b)));
  assert('/'==b->mem[b->used-1]);

  b->used = 0;
  rc = fsl_file_dirpart("/", 1, b, 0);
  assert(0==rc);
  assert(0==b->used);

  rc = fsl_file_dirpart("/", 1, b, 1);
  assert(1==b->used);
  assert('/'==b->mem[0]);

  b->used = 0;
  rc = fsl_file_dirpart("foo", 3, b, 0);
  assert(0==b->used);

  fsl_buffer_clear(b);
  TEST_FOOTER;
}

static int fsl_list_visitor_f_dump_str(void * p, void * visitorState ){
  FCLI_V(("dir entry: %s\n", (char const *)p));
  ++*((int*)visitorState);
  return 0;
}

static void test_dir_names(void){
  fsl_list li = fsl_list_empty;
  int rc;
  fsl_id_t rid;
  int count = 0;
  rid =
#if 0
    2973 /* some commit rid with dirs in it. Not rid 1. */
#elif 0
    -1 /* all versions */
#else
    0 /* current checkout */
#endif
    ;

  TEST_HEADER;
  f_out("fsl_repo_dir_name()/fsl_dirpart() UDF tests "
        "for rid=%"FSL_ID_T_PFMT"...\n",(fsl_id_t)rid);

  rc = fsl_repo_dir_names( fcli_cx(), rid, &li, 1 );
  fcli_err_report(0);
  assert(!rc);
  assert(li.used>0);
  fsl_list_visit( &li, 0, fsl_list_visitor_f_dump_str, &count );
  assert(count==(int)li.used);
  f_out("%d dir entry(ies).\n", count);
  fsl_list_visit_free(&li, 1);
  assert(NULL == li.list);
  assert(0 == li.used);
  TEST_FOOTER;
}

static void test_tree_name(void){
  fsl_cx * f = fcli_cx();
  fsl_buffer buf = fsl_buffer_empty;
  int rc;
  char const * zName = "src/fsl.c";
  char const * zOrig = zName;
  f_out("Starting fsl_ckout_filename_check() test.\n");
  rc = fsl_ckout_filename_check(f, 0, zName, &buf);
  f_out("fsl_ckout_filename_check(%s) ==> %b\n", zName, &buf);
  assert(0==rc);
  assert(0==fsl_strcmp(zName, fsl_buffer_cstr(&buf)));
  fsl_buffer_reuse(&buf);

  zName = fsl_cx_db_file_repo(f,NULL);
  rc = fsl_ckout_filename_check(f, 0, zName, &buf);
  assert(FSL_RC_RANGE==rc && "The repo db IS outside the checkout tree, right?");
  fcli_err_reset();
  assert(0==buf.used);

  {
    fsl_buffer vroot = fsl_buffer_empty;
    char const * zRoot = fsl_cx_ckout_dir_name(f, NULL);
    char const * zName2;
    fsl_buffer_appendf(&vroot, "%/src/foo", zRoot);
    zName = "../../xyz/../f-apps/f-sanity.c";
    zRoot = fsl_buffer_cstr(&vroot);
    rc = fsl_file_canonical_name2(zRoot, zName, &buf, 0);
    f_out("fsl_file_canonical_name2(%s, %s) ==> %b\n", zRoot, zName, &buf);
    assert(0==rc);
    fsl_buffer_clear(&vroot);
    zRoot = NULL;
    zName2 = fsl_buffer_cstr(&buf);
    assert(fsl_is_absolute_path(zName2));
    /* f_out("zName2=%s\n", zName2); */
    assert(0==fsl_stat(zName2, NULL, 1));
    fsl_buffer_reuse(&buf);
  }

  zName = "/etc/hosts";
  rc = fsl_ckout_filename_check(f, 0, zName, &buf);
  assert(FSL_RC_RANGE == fcli_error()->code);
  /*fcli_err_report(1); assert(fcli_error()->code == 0); */
  fcli_err_reset(); assert(fcli_error()->code == 0);
  assert(FSL_RC_RANGE==rc);

  zName += 1;
  rc = fsl_ckout_filename_check(f, 0, zName, NULL);
  assert(0==rc);

  rc = fsl_ckout_filename_check(f, 0, zOrig, NULL);
  assert(0==rc);

  fcli_err_reset();

  test_pushd("f-apps");
  {
    zName = "..";
    fsl_buffer_reuse(&buf);
    rc = fsl_ckout_filename_check(f, 1, zName, &buf);
    assert(0 == rc);
    assert(1 == buf.used);
    assert('.' == buf.mem[0]);
    fsl_buffer_reuse(&buf);
    zName = "../";
    rc = fsl_ckout_filename_check(f, 1, zName, &buf);
    assert(0 == rc);
    assert(2 == buf.used);
    assert('.' == buf.mem[0]);
    assert('/' == buf.mem[1]);


    fsl_buffer_reuse(&buf);
    rc = fsl_ckout_filename_check(f, 0, "...../....", &buf);
    assert(0==rc && "Yes, that is actually a legal file path.");
#if 0
    /* See the ...../.... example above. Still looking for an error case
       which will trigger this particular error before re-enabling it
       in fsl_ckout.c.
    */
    /* fcli_err_report(1); */
    assert(FSL_RC_RANGE==rc);
    zName = NULL;
    fsl_error_get( fcli_error(), &zName, NULL);
    assert(NULL != zName);
    assert(NULL != strstr(zName, "resolve"));
    fcli_err_reset();
#endif
    fsl_buffer_clear(&buf);
    {
      fsl_id_t vfid = 0;
      rc = fsl_filename_to_vfile_id(f, 0, "f-apps/f-sanity.c", &vfid);
      assert(!rc);
      assert(vfid>0);

      rc = fsl_filename_to_vfile_id(f, 0, "no-such-file", &vfid);
      assert(!rc);
      assert(0==vfid);
    }
  }
  test_popd();
  TEST_FOOTER;
}

static void test_strftime(void){
  enum { BufSize = 256 };
  char buf[BufSize];
  fsl_size_t len;
  char const * str = buf;
  struct tm * tm;
  time_t timt;
  struct FmtInfo {
    char const * fmt;
    char const * label;
  } fmtList[] = {
  {"%Y-%m-%d %H:%M:%S", "YYYY-mm-dd HH:MM:SS"},
  {"%%", "Literal percent"},
  {"%a", "Abbr. weekday name"},
  {"%A", "Full weekday name"},
  {"%b", "Abbr. month name"},
  {"%e", "day of month, blank-padded"},
  {"%h", "Same as %b"},
  {"%B", "Full month name"},
  {"%c", "??? \"appropriate date/time representation\""},
  {"%C", "Century as two digits"},
  {"%d", "day of month, 01-31"},
  {"%D", "%m/%d/%y"},
  {"%E", "ignored"},
  {"%H", "hour, 00-23"},
  {"%I", "hour, 01-12"},
  {"%j", "day of year, 001-366"},
  {"%k", "hour, 0-24, blank-padded"},
  {"%l", "hour, 1-12, blank-padded"},
  {"%m", "month, 01-12"},
  {"%M", "minute, 00-59"},
  {"%n", "\\n"},
  {"%O", "ignored"},
  {"%p", "\"am\" or \"pm\""},
  {"%r", "%I:%M:%S %p"},
  {"%R", "%H:%M"},
  {"%S", "seconds, 00-61"},
  {"%t", "\\t"},
  {"%T", "%H:%M:%S"},
  {"%u", "ISO-8601 weeday as number 1-7, 1=Monday"},
  {"%U", "week of year, Sunday as first day"},
  {"%v", "dd-bbb-YYYY"},
  {"%V", "ISO-8601 week number"},
  {"%w", "weekday, 0-6, Sunday=0"},
  {"%W", "week of year, Monday as first day"},
  {"%x", "??? \"appropriate date representation\""},
  {"%X", "??? \"appropriate time representation\""},
  {"%y", "year, 00-99"},
  {"%Y", "year with century"},
  {NULL,NULL}
  };
  struct FmtInfo const * fi = fmtList;
  f_out("fsl_strftime() tests...\n");
  time(&timt);
  tm = gmtime(&timt);
  if(fcli_is_verbose()){
    for( ; fi->fmt; ++fi ){
      len = fsl_strftime(buf, BufSize, fi->fmt, tm);
      f_out("strftime: %s %s ==> %s\n", fi->fmt, fi->label, buf);
      assert(len>0);
    }
  }else{
    len = fsl_strftime(buf, BufSize, "%Y-%m-%d %H:%M:%S", tm);
    f_out("strftime: %s\n", str);
    assert(19==len);
  }

  {
    fsl_buffer bf = fsl_buffer_empty;
    const int rc = fsl_buffer_strftime(&bf, "%H:%M:%S", tm);
    assert(0==rc);
    assert(8==bf.used);
    fsl_buffer_clear(&bf);
  }
  TEST_FOOTER;
}

static void test_config_db(void){
  fsl_confdb_e mode = FSL_CONFDB_CKOUT;
  fsl_cx * const f = fcli_cx();
  char * str = 0;
  fsl_size_t slen;
  int rc;

  f_out("fsl_config_xxx() tests...\n");

  rc = fsl_config_global_preferred_name(&str);
  assert(!rc);
  f_out("fsl_config_global_preferred_name() = %s\n", str);
  fsl_free(str);
  str = 0;

  rc = fsl_config_transaction_begin(f, mode);
  assert(!rc);
  rc = fsl_config_set_text(f, mode, "sanity-text", "hi");
  assert(!rc);
  rc = fsl_config_set_blob(f, mode, "sanity-blob", "hi!!!", 3/*yes, 3!*/);
  assert(!rc);
  rc = fsl_config_set_int32(f, mode, "sanity-int32", 32);
  assert(!rc);
  rc = fsl_config_set_int64(f, mode, "sanity-int64", 64);
  assert(!rc);
  rc = fsl_config_set_id(f, mode, "sanity-id", 2345);
  assert(!rc);
  rc = fsl_config_set_double(f, mode, "sanity-double", -42.24);
  assert(!rc);
  rc = fsl_config_set_bool(f, mode, "sanity-bool", 1);
  assert(!rc);

  str = fsl_config_get_text(f, mode, "sanity-text", &slen);
  assert(0==fsl_strcmp("hi", str));
  assert(2==slen);
  fsl_free(str);

  str = fsl_config_get_text(f, mode, "sanity-blob", &slen);
  assert(0==fsl_strcmp("hi!", str));
  assert(3==slen);
  fsl_free(str);

  assert(32==fsl_config_get_int32(f, mode, -1, "sanity-int32"));
  assert(64==fsl_config_get_int64(f, mode, -1, "sanity-int64"));
  assert(2345==fsl_config_get_id(f, mode, -1, "sanity-id"));
  assert(true==fsl_config_get_bool(f, mode, false, "sanity-bool"));
  { /* portability problem: on my x64 box sanity-double==-42.24. On my i32
       box it prints out as -42.24 but does not compare == to -42.24. So
       we'll do a slightly different check...
    */
    double const dbl =
      fsl_config_get_double(f, mode, -1, "sanity-double");
    /* f_out("dbl=%f\n", dbl); */
    /* assert(-42.24==dbl); */
    assert(-42 == (int)dbl);
    assert(-24 == (int)(dbl*100) % -100);
  }

  assert(fsl_configs_get_bool(f, "vrc", false, "sanity-bool"));
  assert(fsl_configs_get_bool(f, "crv", false, "sanity-bool"));
  assert(!fsl_configs_get_bool(f, "r", false, "sanity-bool"));
  assert(32==fsl_configs_get_int32(f, "grc", -1, "sanity-int32"));
  assert(-1==fsl_configs_get_int32(f, "rg", -1, "sanity-int32"));
  assert(64==fsl_configs_get_int64(f, "grc", -1, "sanity-int64"));
  assert(-1==fsl_configs_get_int64(f, "rg", -1, "sanity-int64"));
  assert(2345==fsl_configs_get_int64(f, "grc", -1, "sanity-id"));
  assert(-1==fsl_configs_get_int64(f, "rg", -1, "sanity-id"));
  assert(-42.0 > fsl_configs_get_double(f, "grc", 1, "sanity-double"));
  assert(0.0==fsl_configs_get_double(f, "rg", 0.0, "sanity-double"));
  str = fsl_configs_get_text(f, "r", "sanity-text", NULL);
  assert(NULL==str);
  str = fsl_configs_get_text(f, "c", "sanity-text", &slen);
  assert(0==fsl_strcmp("hi", str));
  assert(2==slen);
  slen = 0;
  fsl_free(str);
  fsl_buffer check = fsl_buffer_empty;
  assert(FSL_RC_NOT_FOUND==fsl_configs_get_buffer(f,"r","sanity-text", &check));
  assert(0==fsl_configs_get_buffer(f,"rc","sanity-text", &check));
  assert(2==check.used);
  assert('h'==check.mem[0] && 'i'==check.mem[1]);

  rc = fsl_config_transaction_end(f, mode, true);
  assert(!rc);

  mode = FSL_CONFDB_VERSIONABLE;
  rc = fsl_config_set_int32(f, mode, "sanity-int32", 32);
  assert(!rc);

  test_pushd("f-apps");
  {
    char const *verFile = "../.fossil-settings/sanity-int32";
    fsl_buffer_reuse(&check);
    rc = fsl_buffer_fill_from_filename(&check, verFile);
    assert(!rc);
    assert(3==check.used);
    assert(0==memcmp(check.mem,"32\n",check.used));
    assert(0==fsl_stat(verFile,NULL,false));
    fsl_file_unlink(verFile);

    rc = fsl_config_set_bool(f, mode, "sanity-bool", false);
    assert(!rc);
    verFile = "../.fossil-settings/sanity-bool";
    fsl_buffer_reuse(&check);
    rc = fsl_buffer_fill_from_filename(&check, verFile);
    assert(!rc);
    assert(4==check.used);
    assert(0==memcmp(check.mem,"off\n",check.used));
    fsl_buffer_clear(&check);
    assert(0==fsl_stat(verFile,NULL,false));
    fsl_file_unlink(verFile);

    fsl_config_close(f);
  }
  test_popd();
  TEST_FOOTER;
}

static void test_mtime_of_manifest(void){
  fsl_cx * f = fcli_cx();
  char const * tag = "current";
  fsl_id_t mrid = 0;
  fsl_id_t fnid = 0;
  int rc;
  fsl_deck d = fsl_deck_empty;
  fsl_size_t i;
  fsl_time_t timey;
  fsl_db * dbR = fsl_cx_db_repo(f);
  struct tm * tm;
  enum { BufSize = 30 };
  char strBuf[BufSize];
  fsl_card_F const * fc;
  f_out("fsl_mtime_of_manifest() checks...\n");
  rc = fsl_sym_to_rid(f, tag, FSL_SATYPE_CHECKIN, &mrid);
  assert(!rc);
  rc = fsl_deck_load_rid(f, &d, mrid, FSL_SATYPE_CHECKIN);
  assert(!rc);
  //assert(fsl_rid_is_leaf(f, mrid));
  timey = 0;
  rc = fsl_mtime_of_manifest_file(f, mrid, 0, &timey);
  assert(!rc);
  assert(timey>0);
  timey = 0;

  /* Pedantic note: we are knowingly bypassing any lookup
     of files in the parent manifest here. Don't try this
     at home. */
  rc = fsl_deck_F_rewind(&d);
  for( i = 0; i < d.F.used; ++i){
    int64_t stm;
    fc = &d.F.list[i];
  /* if(!rc) while( !(rc=fsl_deck_F_next(&d, &fc)) && fc) { */
    if(!fc->uuid) continue;
    fnid = fsl_uuid_to_rid(f, fc->uuid);
    assert(fnid>0);
    rc = fsl_mtime_of_manifest_file(f, mrid, fnid, &timey);
    assert(0==rc);
    {
      time_t tt = (time_t)timey;
      tm = localtime(&tt);
    }
    strBuf[0] = 0;
    fsl_strftime(strBuf, BufSize, "%Y-%m-%d %H:%M:%S", tm);
    if(fcli_is_verbose()){
      f_out("%8"FSL_ID_T_PFMT" %.8s %"FSL_TIME_T_PFMT" => %s %s\n",
            (fsl_id_t)fnid, fc->uuid, (fsl_time_t)timey,
            strBuf, fc->name);
    }
    assert(19==fsl_strlen(strBuf));
    stm = fsl_db_g_int64(dbR, -1, "SELECT FSL_CI_MTIME("
                         "%"FSL_ID_T_PFMT",%"FSL_ID_T_PFMT")",
                         (fsl_id_t)mrid, (fsl_id_t)fnid);
    assert(stm == timey);
  }

  fsl_deck_finalize(&d);

  {
    /* Ensure that fsl_mtime_of_manifest_file() works for non-checkin
       artifacts if passed a file RID <= 0. */
    mrid = fsl_db_g_id(
      dbR, -1, "SELECT objid FROM event WHERE type<>'ci' ORDER BY mtime DESC"
    );
    assert( mrid>0 );
    fsl_time_t const mt1 = (fsl_time_t)fsl_db_g_int64(
      dbR, -1, "SELECT fsl_j2u(mtime) FROM event WHERE objid=%" FSL_ID_T_PFMT, mrid
    );
    assert( mt1>0 );
    fsl_time_t mt2 = 0;
    rc = fsl_mtime_of_manifest_file(f, mrid, 0, &mt2);
    assert( 0==rc );
    assert( mt2>0 );
    assert( mt1==mt2 && "Unexpected time mismatch" );
  }

  TEST_FOOTER;
}

static void test_deck_set(void){
  int rc = 0;
  fsl_deck d = fsl_deck_empty;
  fsl_cx * const f = fcli_cx();
  char const * fname = 0;
  TEST_HEADER;
  rc = fsl_deck_load_sym(f, &d, "trunk", FSL_SATYPE_CHECKIN);
  assert(0==rc);
  assert(d.f == f);
  fname = "auto.def";
  rc = fsl_deck_F_set(&d, fname, NULL, FSL_FILE_PERM_REGULAR, NULL);
  assert(FSL_RC_MISUSE==rc && "Cannot modify a saved deck (with an RID)");
  rc = 0;
  d.rid = 0;
  rc = fsl_deck_F_set(&d, fname, NULL, FSL_FILE_PERM_REGULAR, NULL);
  assert(0==rc && "Removing an entry is okay");
  rc = fsl_deck_F_set(&d, fname, NULL, FSL_FILE_PERM_REGULAR, NULL);
  assert(FSL_RC_NOT_FOUND==rc && "Because we removed it a moment ago.");
  fsl_deck_finalize(&d);
  f_out("%s() complete\n", __func__);
  fcli_err_reset();
  TEST_FOOTER;
}

static void test_stmt_cached(void){
  fsl_db * db = fsl_cx_db(fcli_cx());
  fsl_stmt * s1 = NULL, * s2 = NULL, * check = NULL;
  int rc;
  char const * sql = "SELECT 1 WHERE 3=?/*%s()*/";
  f_out("Statement caching tests...\n");
  assert(db);
  rc = fsl_db_prepare_cached(db, &s1, sql,__func__);
  assert(0==rc);
  assert(s1);
  check = s1;

  /* Concurrent use must fail to avoid that recursion
     bones us with a hard-to-track error... */
  rc = fsl_db_prepare_cached(db, &s2, sql,__func__);
  assert(FSL_RC_ACCESS==rc);
  assert(!s2);
  fcli_err_reset();

  rc = fsl_stmt_cached_yield(s1);
  assert(0==rc);

  /* Make sure we get the same pointer back... */
  s1 = NULL;
  rc = fsl_db_prepare_cached(db, &s1, sql,__func__);
  assert(0==rc);
  assert(s1);
  assert(check == s1);
  rc = fsl_stmt_cached_yield(s1);
  assert(0==rc);
  rc = fsl_stmt_cached_yield(s1);
  assert(FSL_RC_MISUSE==rc);
  fcli_err_reset();
  TEST_FOOTER;
}

static void test_buffer_compress(void){
  fsl_buffer buf = fsl_buffer_empty;
  int rc = 0;
  fsl_cx * f = fcli_cx();
  fsl_size_t sz, szOrig;
  char const * infile = THIS_SRC_FNAME;
  f_out("Buffer compression tests...\n");
  assert(f);
  rc = fsl_buffer_fill_from_filename(&buf, infile);
  assert(!rc);
  assert(0 > fsl_buffer_uncompressed_size(&buf));
  sz = szOrig = buf.used;
  rc = fsl_buffer_compress( &buf, &buf );
  assert(!rc);
  assert(buf.used < sz);
  assert((fsl_int_t)szOrig == fsl_buffer_uncompressed_size(&buf));
  /*f_out("Compressed [%s]. Size: %"FSL_SIZE_T_PFMT
    " => %"FSL_SIZE_T_PFMT"\n", infile, szOrig, buf.used);*/
  sz = buf.used;
  rc = fsl_buffer_uncompress(&buf, &buf);
  assert(!rc);
  /*f_out("Uncompressed [%s]. Size: %"FSL_SIZE_T_PFMT
    " => %"FSL_SIZE_T_PFMT"\n", infile, sz, buf.used);*/
  assert(szOrig == buf.used);

  if(0){
    fsl_buffer sql = fsl_buffer_empty;
    fsl_buffer_reuse(&buf);
    rc = fsl_buffer_appendf(&buf, "this isn't a quoted value.");

    fsl_buffer_appendf(&sql,"/*%%b*/ SELECT x FROM a WHERE a=%b\n",
                      &buf);
    fsl_output(f, sql.mem, sql.used );
    fsl_buffer_reuse(&sql);
    fsl_buffer_appendf(&sql,"/*%%B*/ SELECT x FROM a WHERE a=%B\n",
                       &buf);
    fsl_output(f, sql.mem, sql.used );
    rc = fsl_buffer_reserve(&sql, 0);
    assert(!rc);
  }

  rc = fsl_buffer_reserve(&buf, 0);
  assert(!rc);
  TEST_FOOTER;
}

static void test_buffer_count_lines(void){
  fsl_buffer to = fsl_buffer_empty;
  fsl_buffer from = fsl_buffer_empty;
  int rc;
  f_out("fsl_buffer_copy_lines() tests...\n");
  fsl_buffer_append(&from, "a\nb\nc\nd\n", -1);
  rc = fsl_buffer_copy_lines(&to, &from, 2);
  assert(0==rc);
  rc = fsl_strcmp("a\nb\n", fsl_buffer_cstr(&to));
  assert(0==rc);
  to.used = 0;
  rc = fsl_buffer_copy_lines(&to, &from,3);
  assert(0==rc);
  rc = fsl_strcmp("c\nd\n", fsl_buffer_cstr(&to));
  /* f_out("<<<%b>>>\n", &to); */
  assert(from.cursor==from.used);
  assert(0==rc);

  fsl_buffer_clear(&to);
  fsl_buffer_clear(&from);
  TEST_FOOTER;
}

static void test_buffer_streams(void){
  fsl_buffer bin = fsl_buffer_empty;
  fsl_buffer bout = fsl_buffer_empty;
  f_out("fsl_buffer_(input|output)_f() tests...\n");
  fsl_buffer_append(&bin, "Buffer stream.", -1);
  fsl_stream(fsl_input_f_buffer, &bin,
             fsl_output_f_buffer, &bout);
  assert(bin.used==bin.cursor);
  assert(0==fsl_buffer_compare(&bin, &bout));
  /* f_out("bout=<<<%b>>>\n", &bout); */
  fsl_buffer_clear(&bin);
  fsl_buffer_clear(&bout);
}

static void test_buffer_seek(void){
  fsl_buffer bin = fsl_buffer_empty;
  f_out("fsl_buffer_seek() and friends tests...\n");
  fsl_buffer_append(&bin, "0123456789", -1);
  assert(10==bin.used);
  assert(10==fsl_buffer_seek(&bin, 0, FSL_BUFFER_SEEK_END));
  assert(7==fsl_buffer_seek(&bin, -3, FSL_BUFFER_SEEK_END));
  assert(7==fsl_buffer_tell(&bin));
  assert(4==fsl_buffer_seek(&bin, -3, FSL_BUFFER_SEEK_CUR));
  assert(10==fsl_buffer_seek(&bin, 14, FSL_BUFFER_SEEK_CUR));
  assert(0==fsl_buffer_seek(&bin, 0, FSL_BUFFER_SEEK_SET));
  assert(0==fsl_buffer_seek(&bin, 0, FSL_BUFFER_SEEK_CUR));
  fsl_buffer_rewind(&bin);
  assert(0==bin.cursor);
  fsl_buffer_clear(&bin);
  TEST_FOOTER;
}

static void test_buffer_getdelim(void){
  static const char *t[] = {
    "",
    "0",
    "01",
    "012",
    "0123",
    "01234",
    "012345",
    "0123456",
    "01234567",
    "012345678",
    "0123456789",
    NULL
  };
  fsl_buffer buf = fsl_buffer_empty;
  size_t sz = 8;
  fsl_int_t len, i;
  char *d, *token, *delim = "\t\n";
  const char nodelim[] = "no matching delimiter\nno matching delimiter\n";
  int rc, prealloc = 1;

  TEST_HEADER;
  token = malloc(sz);
  assert(token!=NULL);
  f_out("fsl_buffer_getdelim() tests...\n");
  len = fsl_buffer_getdelim(&token, &sz, 'X', &buf);  /* empty buffer path */
  assert(len==-1);
  assert(buf.errCode==0);
  for(;;){  /* test with and without preallocated buffer */
    for(d = delim; *d != '\0'; ++d){  /* test with \t and \n delimiter */
      f_out("%sdelim: %s\n",
          prealloc ? "prealloc " : "", *d == '\t' ? "\\t" : "\\n");
      for(i=0; t[i]!=NULL; ++i){
        rc = fsl_buffer_appendf(&buf, "%s%c", t[i], *d);
        assert(rc==0);
      }
      buf.cursor = buf.used+1;  /* end-of-buffer code path */
      len = fsl_buffer_getdelim(&token, &sz, *d, &buf);
      assert(len==-1);
      assert(buf.errCode==0);
      i = 0;
      buf.cursor = 0;
      while((len = fsl_buffer_getdelim(&token, &sz, *d, &buf)) != -1){
        assert(buf.errCode==0);
        assert(len>0 && token[len - 1]==*d);
        assert(fsl_strlen(token)==(fsl_size_t)len);
        rc = fsl_strncmp(token, t[i], fsl_strlen(t[i]));
        assert(rc==0);
        ++i;
      }
      assert(buf.errCode==0);
      assert(buf.cursor==buf.used);
      fsl_buffer_reuse(&buf);
    }
    if(!prealloc) break;
    prealloc = 0;
    fsl_free(token);
    token = NULL;
    d = delim;
    sz = 0;
  }
  /* no matching delimiter code path */
  rc = fsl_buffer_append(&buf, nodelim, sizeof(nodelim)-1);
  assert(rc==0);
  len = fsl_buffer_getdelim(&token, &sz, '\t', &buf);
  assert(buf.errCode==0);
  assert(buf.cursor==buf.used);
  assert(len==sizeof(nodelim)-1);
  assert(fsl_strlen(token)==sizeof(nodelim)-1);
  rc = fsl_strcmp(token, nodelim);
  assert(rc==0);

  fsl_free(token);
  fsl_buffer_clear(&buf);
  TEST_FOOTER;
}

static int fsl_confirm_callback_f_test(fsl_confirm_detail const * d,
                                       fsl_confirm_response *answer,
                                       void * clientState){
  assert(0==fsl_strcmp("Hi via clientState", (char const *)clientState));
  switch(d->eventId){
    case FSL_CEVENT_OVERWRITE_MOD_FILE:
      f_out("Asking for confirmation to overwrite "
            "locally-modified file: %s\n", d->filename);
      answer->response =  FSL_CRESPONSE_CANCEL;
      break;
    case FSL_CEVENT_OVERWRITE_UNMGD_FILE:
      f_out("Asking for confirmation to overwrite "
            "an unmanaged file with a managed one: %s\n",
            d->filename);
      answer->response =  FSL_CRESPONSE_YES;
      break;
    case FSL_CEVENT_RM_MOD_UNMGD_FILE:
      f_out("Asking for confirmation to remove "
            "locally-modified file: %s\n", d->filename);
      answer->response = FSL_CRESPONSE_ALWAYS;
      break;
    default:
      fsl__fatal(FSL_RC_UNSUPPORTED,"Invalid fsl_confirm_event_e value.");
  }
  return 0;
}

static void test_confirmation(void){
  fsl_cx * f = fcli_cx();
  const char * message = "Hi via clientState";
  fsl_confirm_response answer = fsl_confirm_response_empty;
  int rc;
  fsl_confirm_detail deets = fsl_confirm_detail_empty;

  assert(NULL==f->confirmer.callback);
  deets.eventId = FSL_CEVENT_OVERWRITE_MOD_FILE;
  deets.filename = THIS_SRC_FNAME;
  rc = fsl_cx_confirm(f, &deets, &answer);
  assert(0==rc);
  assert(FSL_CRESPONSE_NEVER==answer.response
         && "API guaranty violation");
  answer.response = FSL_CRESPONSE_INVALID;

  deets.eventId = FSL_CEVENT_OVERWRITE_UNMGD_FILE;
  rc = fsl_cx_confirm(f, &deets, &answer);
  assert(0==rc);
  assert(FSL_CRESPONSE_NEVER==answer.response
          && "API guaranty violation");
  answer.response = FSL_CRESPONSE_INVALID;

  deets.eventId = FSL_CEVENT_RM_MOD_UNMGD_FILE;
  rc = fsl_cx_confirm(f, &deets, &answer);
  assert(0==rc);
  assert(FSL_CRESPONSE_NEVER==answer.response
          && "API guaranty violation");
  answer.response = FSL_CRESPONSE_INVALID;

  deets.eventId = FSL_CEVENT_MULTIPLE_VERSIONS;
  deets.filename = NULL;
  rc = fsl_cx_confirm(f, &deets, &answer);
  assert(0==rc);
  assert(FSL_CRESPONSE_CANCEL==answer.response
          && "API guaranty violation");
  answer.response = FSL_CRESPONSE_INVALID;

  fsl_confirmer oldConfimer = fsl_confirmer_empty;
  {
    fsl_confirmer fcon = fsl_confirmer_empty;
    fcon.callback = fsl_confirm_callback_f_test;
    fcon.callbackState = (void*)message;
    fsl_cx_confirmer(f, &fcon, &oldConfimer );
  }
  assert(fsl_confirm_callback_f_test==f->confirmer.callback);
  assert(message==f->confirmer.callbackState);
  deets.eventId = FSL_CEVENT_OVERWRITE_MOD_FILE;
  deets.filename = THIS_SRC_FNAME;
  rc = fsl_cx_confirm(f, &deets, &answer);
  assert(0==rc);
  assert(FSL_CRESPONSE_CANCEL==answer.response);
  answer.response = FSL_CRESPONSE_INVALID;

  deets.eventId = FSL_CEVENT_OVERWRITE_UNMGD_FILE;
  deets.filename = THIS_SRC_FNAME;
  rc = fsl_cx_confirm(f, &deets, &answer);
  assert(0==rc);
  assert(FSL_CRESPONSE_YES==answer.response);
  answer.response = FSL_CRESPONSE_INVALID;

  deets.eventId = FSL_CEVENT_RM_MOD_UNMGD_FILE;
  deets.filename = THIS_SRC_FNAME;
  rc = fsl_cx_confirm(f, &deets, &answer);
  assert(0==rc);
  assert(FSL_CRESPONSE_ALWAYS==answer.response);

  fsl_cx_confirmer(f, &oldConfimer, NULL );
  TEST_FOOTER;
}

static void test_glob_list(void){
  fsl_list gl = fsl_list_empty;
  int rc;
  fsl_size_t i;
  char const * str;
  char const * match;
  char const * zGlobs = "*.c *.h,'*.sql',,,\"*.sh\"";
  f_out("fsl_str_glob() and fsl_glob_list_xxx() tests...\n");

  assert(fsl_str_glob("*.c", THIS_SRC_FNAME));
  assert(!fsl_str_glob("*.h", THIS_SRC_FNAME));
  assert(fsl_str_glob("*.[a-d]", THIS_SRC_FNAME));

  rc = fsl_glob_list_parse( &gl, zGlobs );
  assert(!rc);
  rc = fsl_glob_list_append( &gl, "*.in");
  assert(!rc);
  if(fcli_is_verbose()){
    for(i = 0; i < gl.used; ++i){
      str = (char const *)gl.list[i];
      f_out("glob #%d: [%s]\n", (int)i+1, str);
    }
  }
  match = fsl_glob_list_matches(&gl, "foo.h");
  assert(0==fsl_strcmp("*.h",match));
  match = fsl_glob_list_matches(&gl, "foo.x");
  assert(!match);
  match = fsl_glob_list_matches(&gl, "Makefile.in");
  assert(0==fsl_strcmp("*.in",match));

  fsl_glob_list_clear(&gl);

  assert(FSL_GLOBS_IGNORE == fsl_glob_name_to_category("ignore-glob"));
  assert(FSL_GLOBS_BINARY == fsl_glob_name_to_category("binary-glob"));
  assert(FSL_GLOBS_CRNL == fsl_glob_name_to_category("crnl-glob"));
  assert(FSL_GLOBS_INVALID == fsl_glob_name_to_category(NULL));

  fsl_cx * const f = fcli_cx();
  fsl_list * gList = 0;
  assert(FSL_RC_RANGE==fsl_cx_glob_list(f, FSL_GLOBS_INVALID, &gList, false));
  assert(0==fsl_cx_glob_list(f, FSL_GLOBS_BINARY, &gList, false));
  assert(&f->cache.globs.binary == gList);
  assert(0==fsl_cx_glob_list(f, FSL_GLOBS_CRNL, &gList, false));
  assert(&f->cache.globs.crnl == gList);
  assert(0==fsl_cx_glob_list(f, FSL_GLOBS_IGNORE, &gList, false));
  assert(&f->cache.globs.ignore == gList);

  TEST_FOOTER;
}

static void test_vtime_check(void){
  fsl_cx * f = fcli_cx();
  fsl_id_t vid = 0;
  fsl_uuid_cstr uuid = NULL;
  int rc;
  int sigFlags =
    0
    /* | FSL_VFILE_CKSIG_SETMTIME */
    ;
  f_out("fsl_vtime_check_sig() tests...\n");
  fsl_ckout_version_info(f, &vid, &uuid);
  assert(vid>0);
  assert(uuid);
  rc = fsl_vfile_changes_scan(f, vid, sigFlags);
  fcli_err_report(0);
  assert(!rc);
  TEST_FOOTER;
}

static void test_buffer_compare(void){
  fsl_buffer b1 = fsl_buffer_empty;
  fsl_buffer b2 = fsl_buffer_empty;
  int rc;
  char const * inFile = "f-ls.c";
  FILE * file;
  f_out("fsl_buffer_compare() tests...\n");
  test_pushd("f-apps");
  {
    rc = fsl_buffer_fill_from_filename(&b1, inFile);
    assert(!rc);
    rc = fsl_buffer_compare_file(&b1, inFile);
    assert(0==rc);
    b1.mem[2] = '\0';
    rc = fsl_buffer_compare_file(&b1, inFile);
    assert(0!=rc);

    rc = fsl_buffer_fill_from_filename(&b2, inFile);
    assert(!rc);

    file = fsl_fopen(inFile, "r");
    assert(file);
    rc = fsl_stream_compare( fsl_input_f_FILE, file,
                             fsl_input_f_buffer, &b2);
    fsl_fclose(file);
    assert(0==rc);


    b1.cursor = 0;
    b2.cursor = 0;
    rc = fsl_stream_compare( fsl_input_f_buffer, &b1,
                             fsl_input_f_buffer, &b2);
    assert(0!=rc);

    b2.used = b2.used / 2;
    rc = fsl_buffer_compare_file(&b2, inFile);
    assert(0!=rc);
  }
  test_popd();
  fsl_buffer_clear(&b1);
  fsl_buffer_clear(&b2);
  TEST_FOOTER;
}

static void test_file_add(void){
  int rc;
  fsl_cx * f = fcli_cx();
  fsl_db * db = fsl_cx_db_ckout(f);
  f_out("fsl_ckout_manage() sanity check...\n");
  rc = fsl_db_txn_begin(db);
  assert(!rc);
  fsl_ckout_manage_opt addOpt =
    fsl_ckout_manage_opt_empty;
  addOpt.relativeToCwd = true;
  addOpt.filename = "f-sanity.c";
  test_pushd("f-apps");
  {
    rc = fsl_ckout_manage( f, &addOpt );
    assert(0==rc);
    /* fcli_err_report(0); */
    assert(0==addOpt.counts.added);
    assert(0==addOpt.counts.skipped);
    assert(1==addOpt.counts.updated);

    addOpt.filename = FSL_PLATFORM_IS_WINDOWS
      ? "../f-sanity.exe" : "../f-sanity";
    addOpt.checkIgnoreGlobs = false;
    addOpt.counts = fsl_ckout_manage_opt_empty.counts;
    rc = fsl_ckout_manage( f, &addOpt );
    assert(0==rc);
    /* fcli_err_report(0); */
    assert(1==addOpt.counts.added);
    assert(0==addOpt.counts.skipped);
    assert(0==addOpt.counts.updated);

    addOpt.filename = "no-such-file";
    addOpt.counts = fsl_ckout_manage_opt_empty.counts;
    rc = fsl_ckout_manage( f, &addOpt );
    assert(FSL_RC_NOT_FOUND==rc);
    /* fcli_err_report(0); */
    assert(0==addOpt.counts.added);
    assert(0==addOpt.counts.skipped);
    assert(0==addOpt.counts.updated);
    fsl_db_txn_rollback(db);
    fcli_err_reset();
  }
  test_popd();
  TEST_FOOTER;
}


static int fsl_ckout_unmanage_f_my(fsl_ckout_unmanage_state const * us){
  f_out("REMOVED    %s\n", us->filename);
  ++*((unsigned int*)us->opt->callbackState);
  return 0;
}

static void test_file_rm(void){
  int rc;
  fsl_cx * f = fcli_cx();
  char const * fname = "fsl.c";
  fsl_db * db = fsl_cx_db_ckout(f);
  enum { BufSize = 2000 };
  char cwd[BufSize];
  f_out("fsl_ckout_unmanage() sanity check...\n");

  rc = fsl_getcwd(cwd, BufSize, NULL);
  assert(!rc);
  rc = fsl_db_txn_begin(db);
  assert(!rc);
  rc = fsl_chdir("src");
  assert(0==rc);

  fsl_ckout_unmanage_opt ropt = fsl_ckout_unmanage_opt_empty;
  ropt.scanForChanges = false;
  ropt.relativeToCwd = true;
  ropt.filename = fname;
  rc = fsl_ckout_unmanage( f, &ropt );
  assert(0==rc);
  rc = fsl_chdir(cwd);
  assert(0==rc);
  /* fcli_err_report(0); */

  ropt.filename = "no-such-file";
  rc = fsl_ckout_unmanage( f, &ropt );
  assert(0==rc);

  unsigned int changes = 0;
  ropt.filename = "tools/";
  ropt.callback = fsl_ckout_unmanage_f_my;
  ropt.callbackState = &changes;
  ropt.relativeToCwd = false;
  rc = fsl_ckout_unmanage( f, &ropt );
  assert(0==rc);
  assert(changes>=2);

  fsl_db_each( fsl_cx_db(f), fsl_stmt_each_f_dump, f,
               "SELECT * FROM vfile WHERE deleted=1");

  fsl_db_txn_rollback(db);
  fcli_err_reset();
  TEST_FOOTER;
}

static void test_branch_create(void){
  int rc;
  fsl_branch_opt opt = fsl_branch_opt_empty;
  fsl_id_t ckoutRid = 0;
  fsl_cx * f = fcli_cx();
  fsl_db * db = fsl_cx_db_repo(f);
  fsl_id_t rid = 0;
  TEST_HEADER;
  assert(db);
  fsl_ckout_version_info(f, &ckoutRid, NULL);
  opt.name = "lib-generated-branch";
  opt.basisRid = ckoutRid;
  opt.bgColor = "#f0f0f0";
  fsl_cx_txn_begin(f);
  rc = fsl_branch_create(f, &opt, &rid);
  fcli_err_report(0);
  fsl_cx_txn_end_v2(f, false, false);
  assert(0==rc);
  assert(rid>0);
  TEST_FOOTER;
}

static void test_file_simplify_name(void){
  fsl_size_t n, n2;
  enum { BufSize = 512 };
  char cbuf[BufSize];
  char const * zName;
  TEST_HEADER;

#define STR(VAL,SLASH) zName=VAL; n=fsl_strlen(zName);   \
  memcpy( cbuf, zName, n+1 ); \
  n2 = fsl_file_simplify_name(cbuf, n, SLASH)

  STR("a///b/../c", true);
  assert(n2 < n);
  assert(3==n2);
  assert(0==fsl_strcmp("a/c", cbuf));

  STR(".",true);
  assert(1==n2);
  assert(0==fsl_strcmp(".", cbuf));

  STR(".///", true);
  assert(2==n2);
  assert(0==fsl_strcmp("./", cbuf));

  STR("./././",true);
  assert(2==n2);
  assert(0==fsl_strcmp("./", cbuf));

  STR("./././",false);
  assert(1==n2);
  assert(0==fsl_strcmp(".", cbuf));

  STR("/",true);
  assert(1==n2 /*special case*/);
  assert(0==fsl_strcmp("/", cbuf));

  STR("/",false);
  assert(1==n2 /*special case*/);
  assert(0==fsl_strcmp("/", cbuf));

#undef STR
  TEST_FOOTER;
}

typedef struct {
  uint32_t counter;
} ExtractState;

static int test_repo_extract_f(fsl_repo_extract_state const * xs){
  int rc = 0;
  fsl_cx * f = xs->f;
  ExtractState * st = (ExtractState*)xs->callbackState;
  enum { BufSize = 60 };
  static char tbuf[BufSize];
  assert(f);
  assert(++st->counter == xs->count.fileNumber);
  assert(xs->count.fileNumber <= xs->count.fileCount);
  assert(xs->fCard->uuid);
  if(fcli_is_verbose()){
    fsl_time_t mtime = 0;
    rc = fsl_mtime_of_manifest_file(xs->f, xs->checkinRid,
                                    xs->fileRid, &mtime);
    assert(0==rc);
    assert(mtime>0);
    fsl_strftime_unix(tbuf, BufSize, "%Y-%m-%d %H:%M:%S", mtime, 0);
    f_out("repo_extract: %-8u  %s  %s\n", (unsigned)xs->content->used,
          tbuf, xs->fCard->name);
  }
  return 0;
}

static void test_repo_extract(void){
  int rc;
  fsl_cx * f = fcli_cx();
  fsl_id_t vid = 0;
  ExtractState ex;
  f_out("test_repo_extract()...\n");
  fcli_err_reset();
  fsl_ckout_version_info(f, &vid, NULL);
  assert(vid>0);
  ex.counter = 0;
  fsl_repo_extract_opt reopt = fsl_repo_extract_opt_empty;
  reopt.checkinRid = vid;
  reopt.callback = test_repo_extract_f;
  reopt.callbackState = &ex;
  reopt.extractContent = true;
  rc = fsl_repo_extract(fcli_cx(), &reopt);
  fcli_err_report(0);
  assert(0==rc);
  assert(ex.counter > 200);
  f_out("Extracted %d file(s) from RID %"FSL_ID_T_PFMT".\n",
        ex.counter, (fsl_id_t)vid);
  TEST_FOOTER;
}

static void test_repo_filename_to_fnid(void){
  int rc;
  fsl_cx * f = fcli_cx();
  fsl_id_t fnid = 0;
  f_out("%s()...\n", __func__);

  rc = fsl__repo_filename_fnid2( f, "Makefile.in", &fnid, 0 );
  assert(!rc);
  assert(fnid>0);
  rc = fsl__repo_filename_fnid2(f, "NoSuchFile", &fnid, 0);
  assert(0==rc);
  assert(!fnid);
  TEST_FOOTER;
}

static int fsl_checkin_queue_f_my(const char * filename, void * state){
  ++(*((unsigned *)state));
  (void)filename;
  return 0;
}

static void test_checkin_file_list(void){
  int rc;
  fsl_cx * const f = fcli_cx();
  fsl_id_bag const * const fList = &f->ckin.selectedIds;
  f_out("%s()...\n", __func__);

  rc = fsl_cx_txn_begin(f);
  assert(0==rc);

  unsigned int counter = 0;
  fsl_checkin_queue_opt qOpt = fsl_checkin_queue_opt_empty;
  test_pushd("f-apps");
  {
    qOpt.callback = fsl_checkin_queue_f_my;
    qOpt.callbackState = &counter;
    qOpt.onlyModifiedFiles = false;
    qOpt.filename = "f-sanity.c";
    qOpt.relativeToCwd = true;
    rc = fsl_checkin_enqueue(f, &qOpt);
    assert(0==rc);
    assert(1 == fList->entryCount);
    assert(1 == counter);

    qOpt.filename = "src/fsl.c";
    qOpt.relativeToCwd = false;
    rc = fsl_checkin_enqueue(f, &qOpt);
    assert(0==rc);
    assert(2 == fList->entryCount);
    assert(2 == counter);

    qOpt.filename = "../fsl.c";
    qOpt.relativeToCwd = false;
    rc = fsl_checkin_enqueue(f, &qOpt);
    assert((FSL_RC_RANGE==rc) && "File is outside of checkout tree.");
    assert(2 == fList->entryCount);
    assert(2 == counter);
    fcli_err_reset();

    qOpt.filename = "../src/fsl.c";
    qOpt.relativeToCwd = true;
    rc = fsl_checkin_enqueue(f, &qOpt);
    assert(0==rc);
    assert((2 == fList->entryCount) && "This is a dupe.");
    assert(2==counter && "This is a dupe.");

    assert(fsl_checkin_is_enqueued(f, qOpt.filename, true));
    assert(!fsl_checkin_is_enqueued(f, qOpt.filename, false));

    qOpt.filename = "no-such.file";
    qOpt.relativeToCwd = false;
    rc = fsl_checkin_enqueue(f, &qOpt);
    assert(0==rc);
    assert(2==counter && "File was not queued");

    qOpt.filename = "../src/fsl.c";
    qOpt.relativeToCwd = true;
    rc = fsl_checkin_dequeue(f, &qOpt);
    assert(0==rc);
    assert(1==fList->entryCount);
    assert(3==counter);

    qOpt.filename = "../src";
    qOpt.relativeToCwd = true;
    rc = fsl_checkin_enqueue(f, &qOpt);
    assert(0==rc);
    assert(fsl_checkin_is_enqueued(f, "src/diff.c", false));
    assert(counter > 3);

    qOpt.filename = ".";
    qOpt.relativeToCwd = false;
    rc = fsl_checkin_dequeue(f, &qOpt);
    assert(0==rc);
    assert(0==fList->entryCount);

    /**
       We need at least one file which is NOT part of the SCM, or we
       require changes to one of the above SCM'd files, for this test to
       actually run. Checkin will refuse to do anything if no files
       are modified.
    */
    fsl_ckout_manage_opt aOpt =
      fsl_ckout_manage_opt_empty;
    aOpt.filename = "../Makefile"/*generated file*/;
    aOpt.relativeToCwd = true;
    aOpt.checkIgnoreGlobs = false;
    rc = fsl_ckout_manage(f, &aOpt);
    assert(!rc);
    assert(1==aOpt.counts.added);

    {
      fsl_checkin_opt opt = fsl_checkin_opt_empty;
      opt.message = "Blah";
      opt.messageMimeType = "text/plain";
      rc = fsl_checkin_commit( f, &opt, NULL, NULL );
      fcli_err_report(1);
#if 0
      /* Hmm. This assertion means we can't run f-sanity in a
         merge-changed checkout. */
      assert(!rc || (FSL_RC_RANGE==rc && "no files to commit"));
#endif
    }

  }
  test_popd();
  fcli_err_reset();
  fsl_cx_txn_end_v2(f, false, false);
  TEST_FOOTER;
}

static void test_gradient(void){
  unsigned int c1 = 0xFF0000;
  unsigned int c2 = 0x0000FF;
  unsigned int stepCount = 10;
  unsigned int i;
  for( i = 0; i <= stepCount; ++i ){
    f_out("#%06x\n", fsl_gradient_color( c1, c2, stepCount, i ));
  }
  TEST_FOOTER;
}

static void test_versionable_config(void){
  fsl_cx * f = fcli_cx();
  char * val = NULL;
  char const * key;

  f_out("test_versionable_config()...\n");
  key = "ignore-glob";
  val = fsl_config_get_text(f, FSL_CONFDB_VERSIONABLE,
                            key, NULL);
  assert(val && "Because i happen to know this tree "
         "has that versioned setting.");
  f_out("%s=%z\n", key, val);
  val = NULL /* %z freed it! */;
  TEST_FOOTER;
}


static void test_cx_glob_lists(void){
  fsl_cx * f = fcli_cx();
  char const * match;
  int globs = FSL_GLOBS_IGNORE;
  f_out("fsl_cx_glob_matches() tests...\n");
  match = fsl_cx_glob_matches(f, globs, "GNUmakefile");
  assert(match);
  assert(0==fsl_strcmp("GNUmakefile",match));

  match = fsl_cx_glob_matches(f, globs, "f-apps/f-sanity");
  assert(match);

  match = fsl_cx_glob_matches(f, globs, "no-such-glob");
  assert(!match);

  match = fsl_cx_glob_matches(f, globs, "foo~");
  assert(match);
  assert(0==fsl_strcmp("*~",match));
  TEST_FOOTER;
}

static void test_simplify_sql(void){
  fsl_buffer sql = fsl_buffer_empty;
  fsl_size_t len;
  char const * q =
    "SELECT   \n  1  ;    "
    "SELECT        'hi''ya,\n\n   world'\n\n\n\n;"
    ;
  fsl_buffer_append(&sql, q, -1);
  len = sql.used;
  fsl_simplify_sql_buffer( &sql );
  f_out("Simplified SQL: len %d=>%d: [%b]\n",
        (int)len, (int)sql.used, &sql);
  assert((int)len > (int)sql.used);
  fsl_buffer_clear(&sql);
  TEST_FOOTER;
}

static void test_import_blob(void){
  fsl_cx * f = fcli_cx();
  fsl_db * db = fsl_cx_db_repo(f);
  int rc;
  fsl_buffer buf = fsl_buffer_empty;
  char const * inFile = THIS_SRC_FNAME;
  fsl_uuid_str uuid = NULL;
  fsl_id_t rid = 0;
  assert(db);

  f_out("fsl_repo_import_blob()...\n");

  rc = fsl_cx_txn_begin(f);
  assert(!rc);
  rc = fsl_buffer_fill_from_filename(&buf, inFile);
  assert(!rc);

  rc = fsl__repo_import_buffer(f, &buf, &rid, &uuid);
  assert(!rc);
  assert(rid>0);
  assert(fsl_is_uuid(uuid));
  f_out("imported blob: %"FSL_ID_T_PFMT" %s\n", (fsl_id_t)rid, uuid);
  fsl_free(uuid);

  fsl_cx_txn_end(f, true);
  fsl_buffer_clear(&buf);
  TEST_FOOTER;
}

void test_date2(void){
  char const * d;
  f_out("fsl_str_is_date2() tests...\n");
  d = "02014-04-22";
  assert(fsl_str_is_date2(d)>0);
  ++d;
  assert(fsl_str_is_date2(d)<0);
  ++d;
  assert(!fsl_str_is_date2(d));
  TEST_FOOTER;
}

static void test_vpath_1(void){
  fsl_cx * f = fcli_cx();
  fsl_vpath p = fsl_vpath_empty;
  fsl_vpath_node * n;
  int rc;
  fsl_id_t v1, v2;
  char oneWayOnly, directOnly;
  char const * sym1 = "c10d7424ae4c";
  char const * sym2 = "d7927376fa9d";
  /* d7927376fa9d is 3 versions after c10d7424ae4c. */
  if(1){
    /* swap version order */
    char const * x = sym1; sym1 = sym2; sym2 = x;
  }
  TEST_HEADER;

  if(0){
    sym1 = "99237c363673";
    v1 = 1;
  }else{
    rc = fsl_sym_to_rid(f, sym1, FSL_SATYPE_CHECKIN, &v1);
    assert(0==rc);
  }
  rc = fsl_sym_to_rid(f, sym2, FSL_SATYPE_CHECKIN, &v2);
  assert(0==rc);
  assert(v1>0);
  assert(v2>0);

  oneWayOnly = 0;
  directOnly = 1;
  rc = fsl_vpath_shortest( f, &p, v1, v2, directOnly, oneWayOnly );
  assert(!rc);

  f_out("directOnly=%d, oneWayOnly=%d\n", directOnly, oneWayOnly );
  f_out("Versions %s (%d) to %s (%d): %d steps\n",
        sym1, (int)v1, sym2, (int)v2,
        p.nStep);
  {
#if 1
    int i;
    for( i = 1, n = fsl_vpath_first(&p);
         n;
         ++i, n = fsl_vpath_next(n)){
      f_out("\t#%d: %d", i, (int)n->rid);
      if(n->pFrom){
        if(n->rid > n->pFrom->rid){
          f_out(" derives from %d", (int)n->pFrom->rid);
        }else{
          f_out(" begat %d", (int)n->pFrom->rid);
        }
      }
      f_out("\n");
    }
#else
    /* fsl_vpath_reverse() does not reverse after the initial creation :/ */
    int x, i;
    for( x = 0; x < 2; ++x ){
      extern void fsl_vpath_reverse(fsl_vpath * path);
      if(x){
        fsl_vpath_reverse(&p);
        f_out("Reversed:\n");
      }
      for( i = 1, n = fsl_vpath_first(&p);
           n;
           ++i, n = fsl_vpath_next(n)){
        f_out("\t#%d: %d\n", i, (int)n->rid);
      }
    }
#endif
  }
  fsl_vpath_clear(&p);
  TEST_FOOTER;
}

static void test_vpath_2(void){
  fsl_cx * const f = fcli_cx();
  fsl_db * const db = fsl_cx_db(f);
  int rc;
  char const * sym1 = "c10d7424ae4c";
  char const * sym2 = "d7927376fa9d";
  /* d7927376fa9d is 3 versions after c10d7424ae4c. */
  fsl_id_t rid1 = 0, rid2 = 0;
  uint32_t steps = 0;

  TEST_HEADER;
  rc = fsl_sym_to_rid(f, sym1, FSL_SATYPE_CHECKIN, &rid1);
  assert(0==rc);
  rc = fsl_sym_to_rid(f, sym2, FSL_SATYPE_CHECKIN, &rid2);
  assert(0==rc);
  assert(rid1>0 && rid2>0);

  assert(!fsl_db_table_exists(db, FSL_DBROLE_TEMP, "ancestor"));

  rc = fsl_vpath_shortest_store_in_ancestor(f, rid2, rid1
                                            /*RIDs intentionally
                                              swapped*/,
                                            &steps);
  assert(0==rc);
  assert(fsl_db_table_exists(db, FSL_DBROLE_TEMP, "ancestor"));
  f_out("vpath ancestors...\n");
  rc = fsl_db_each(db, fsl_stmt_each_f_dump, f,
                   "SELECT * FROM ancestor");
  assert(0==rc);
  assert(4U==steps);
  assert((int32_t)steps==fsl_db_g_int32(db, 0, "SELECT COUNT(*) FROM ancestor"));
  TEST_FOOTER;
}

#define FSLPRINTF_ENABLE_JSON 1
#if FSLPRINTF_ENABLE_JSON
static void test_printf_json(void){
  fsl_buffer buf = fsl_buffer_empty;
  int rc;
  f_out("%s() %%j JSON output...\n", __func__);
#define jcheck(FMT,IN,OUT)                \
  fsl_buffer_reuse(&buf);                  \
  rc = fsl_buffer_appendf(&buf, FMT, IN);    \
  assert(0==rc);                           \
  f_out("%s [%s] expecting: [%s] got: [%b]\n", FMT, IN, OUT, &buf);  \
  assert(0==fsl_strcmp(fsl_buffer_cstr(&buf), OUT))
  jcheck("%j","hi\nworld", "hi\\nworld");
  jcheck("%j","hä\nwörld", "hä\\nwörld");
  jcheck("%#j","hä\nwörld", "h\\u00e4\\nw\\u00f6rld");
  jcheck("%#!j","hä wörld", "\"h\\u00e4 w\\u00f6rld\"");
  jcheck("%!#j","hä wörld", "\"h\\u00e4 w\\u00f6rld\"");
  unsigned char ubuf[] = {'s','u','r','r',
                          'o','g','a','t',
                          'e',' ',
                          0xF0, 0x9F, 0x92, 0xA9,
                          0};
  jcheck("%j",ubuf, "surrogate \\ud83d\\udca9");
  jcheck("%#j",ubuf, "surrogate \\ud83d\\udca9");
  jcheck("%j", NULL, "null");
  jcheck("%!j", NULL, "null");
  fsl_buffer_clear(&buf);
#undef jcheck
  TEST_FOOTER;
}
#endif /* FSLPRINTF_ENABLE_JSON */

static void test_deck_derive(void){
  int rc = 0;
  fsl_deck d = fsl_deck_empty;
  fsl_cx * const f = fcli_cx();
  rc = fsl_cx_txn_begin(f);
  assert(0==rc);
  if(!fsl_repo_forbids_delta_manifests(f)){
    rc = fsl_config_set_bool(f, FSL_CONFDB_REPO,
                             "forbid-delta-manifests", true);
    assert(0==rc);
  }
  assert(fsl_repo_forbids_delta_manifests(f));
  rc = fsl_deck_load_sym(f, &d,
                         "f2f1612a0ca081462b4021d8126f394b6d6d8772",
                         FSL_SATYPE_CHECKIN);
  assert(0==rc && "We know f2f1612a0 to be a delta manifest.");
  assert(d.f == f);
  assert(d.rid>0);
  {
    fsl_id_t const oldRid = d.rid;
    d.rid = 0;
    rc = fsl_deck_save(&d, false);
    d.rid = oldRid;
    assert(FSL_RC_ACCESS==rc && "Cannot save delta manifests.");
    f_out("Confirmed that we cannot save a delta in this repo.\n");
  }
  fsl_cx_err_reset(f);
  assert(d.B.uuid && "We know this to be a delta manifest with 1 F-card.");
  assert(1==d.F.used);
  rc = fsl_deck_derive(&d);
  fsl_cx_txn_end(f, true);
  assert(0==rc);
  assert(f == d.f);
  assert(!d.B.uuid);
  assert(!d.B.baseline);
  assert(292 == d.F.used);
  for( uint32_t i = 0; i < d.F.used; ++i ){
    assert(d.F.list[i].uuid);
    assert(d.F.list[i].name);
    assert(!d.F.list[i].priorName);
  }
  fsl_deck_finalize(&d);
  TEST_FOOTER;
}

static void test_appendf_comma(void){
  f_out("%s()...\n",__func__);
  f_out("%,15d\n%,15u\n", 1234567890, 2345678);
  TEST_FOOTER;
}

static void test_repo_fingerprint(void){
  int rc;
  fsl_cx * const f = fcli_cx();
  char * zPrint = 0;
  TEST_HEADER;
  rc = fsl__repo_fingerprint_search(f, 0, &zPrint);
  assert(0==rc);
  assert(zPrint);
  f_out("Repo fingerprint: %s\n", zPrint);
  fsl_free(zPrint);
  rc = fsl_ckout_fingerprint_check(f);
  //f_out("Fingerprint check result = %s\n", fsl_rc_cstr(rc));
  assert(0==rc);
  rc = fsl_cx_txn_begin(f);
  assert(0==rc);
  rc = fsl_config_set_text(f, FSL_CONFDB_CKOUT, "fingerprint",
                           "123/bogus");
  assert(0==rc);
  rc = fsl_ckout_fingerprint_check(f);
  assert(FSL_RC_REPO_MISMATCH==rc);
  fsl_cx_err_reset(f);
  rc = fsl_cx_txn_end_v2(f, false, false);
  fcli_err_report(false);
  assert(0==rc);
  TEST_FOOTER;
}

/* static */ void test_repo_is_readonly(void){
  /* TODO */
}

static void test_bind_fmt(void){
  fsl_cx * const f = fcli_cx();
  fsl_db * const db = fsl_cx_db(f);
  fsl_stmt q = fsl_stmt_empty;
  int rc;
  TEST_HEADER;
  f_out("%s() fsl_stmt_bind_fmt() tests\n", __func__);
  rc = fsl_db_prepare(db, &q, "SELECT ?, ?, ?, ?");
  assert(0==rc);
  rc = fsl_stmt_bind_fmt(&q, "ifs^s", (int)42, (double)42.24, "Fourty two",
                         "Fourty three");
  fcli_err_report(false);
  assert(0==rc);
  rc = fsl_stmt_each(&q, fsl_stmt_each_f_dump, f);
  assert(0==rc);
  fsl_stmt_finalize(&q);
  TEST_FOOTER;
}

static void test_appendf_utf8_precision(void){
  char const * zmb = "äaöoü";
  char buf[100];
  TEST_HEADER;
  assert(8==fsl_strlen(zmb));
  assert(5==fsl_strlen_utf8(zmb,8));
#define CHECK(FMT,LEN,LEN8,CMP)             \
  fsl_snprintf(buf, sizeof(buf), FMT, zmb); \
  if(0)f_out("\tFMT=[%s] for [%s]\n\tRES=[%s]\n", FMT, zmb, buf);   \
  assert(0==fsl_strcmp(buf,CMP));               \
  assert(LEN==fsl_strlen(buf)); \
  assert(LEN8==fsl_strlen_utf8(buf,-1))
  CHECK("%#.2s", 3, 2, "äa");
  CHECK("%#.3s", 5, 3, "äaö");
  CHECK("%#.5s", 8, 5, "äaöoü");
  CHECK("%#.10s", 8, 5, "äaöoü");
  CHECK("%.5s", 5, 3, "äaö");
  CHECK("%.2s", 2, 1, "ä");
  CHECK("%#10.3s", 12, 10, "       äaö");
  CHECK("%#-10.3s", 12, 10, "äaö       ");
  CHECK("%#2.3s", 5, 3, "äaö");
  CHECK("%#-2.3s", 5, 3, "äaö");
#undef CHECK
  TEST_FOOTER;
}

/**
   Test fix for %T range problems reported at:

   https://fossil-scm.org/forum/forumpost/cb564acd01
*/
static void test_urldecode(void){
  char buf[100] = {0};
  TEST_HEADER;
  fsl_snprintf(buf, sizeof(buf), "%T", "a%41b%42");
  assert(0==fsl_strcmp(buf,"aAbB"));
  fsl_snprintf(buf, sizeof(buf), "%T", "%41%4");
  assert(0==fsl_strcmp(buf,"A%4"));
  TEST_FOOTER;
}

/**
   Tests for "external" fsl_buffer behaviour.
*/
static void test_external_buffers(void){
  TEST_HEADER;
  fsl_buffer b1 = fsl_buffer_empty;
  fsl_buffer b2 = fsl_buffer_empty;
  char const * ext = "External memory.";
  fsl_size_t nExt = fsl_strlen(ext);
  int rc;

  fsl_buffer_external(&b1, ext, -1);
  assert(0==b1.capacity);
  assert(b1.used==nExt);
  assert((void *)ext == (void*)b1.mem);

  fsl_buffer_clear(&b1);
  assert(NULL==b1.mem);

  fsl_buffer_external(&b1, ext, -1);
  assert((void *)ext == (void*)b1.mem);
  rc = fsl_buffer_reserve(&b1, 0);
  assert(0==rc);
  assert(NULL==b1.mem);

  fsl_buffer_external(&b1, ext, -1);
  rc = fsl_buffer_materialize(&b1);
  assert(0==rc);
  assert(b1.mem);
  assert((void*)ext != (void*)b1.mem);
  assert(b1.used == nExt);
  assert(0==fsl_strcmp(fsl_buffer_cstr(&b1), ext));
  fsl_buffer_clear(&b1);

  fsl_buffer_external(&b1, ext, -1);
  assert((void *)ext == (void*)b1.mem);
  fsl_buffer_reuse(&b1);
  assert(NULL==b1.mem);
  rc = fsl_buffer_materialize(&b1);
  assert(0==rc);
  assert(NULL==b1.mem);

  fsl_buffer_external(&b1, ext, -1);
  assert((void *)ext == (void*)b1.mem);
  fsl_buffer_append(&b1, "abc", 3);
  assert((void *)ext != (void*)b1.mem);
  assert(nExt+3 == b1.used);
  assert(0==b1.mem[b1.used]);

  rc = fsl_buffer_materialize(&b2);
  assert(0==rc);
  assert(NULL==b2.mem);

  fsl_buffer_clear(&b1);
  fsl_buffer_clear(&b2);
  TEST_FOOTER;
}

static void test_stmt_returning(void){
  fsl_stmt q = fsl_stmt_empty;
  fsl_cx * const f = fcli_cx();
  int rc;
  TEST_HEADER;
  /* Verifying behavior of fsl_stmt in the face of
     the RETURNING clause... */
  rc = fsl_cx_exec(f, "CREATE TEMP TABLE RET("
                   "a INTEGER PRIMARY KEY, "
                   "b DATE DEFAULT CURRENT_TIMESTAMP,"
                   "c)");
  assert(0==rc);
  rc = fsl_cx_prepare(f, &q, "INSERT INTO RET(c) "
                      "VALUES(random())");
  assert(0==rc);
  assert(0==fsl_stmt_col_count(&q));
  rc = fsl_stmt_step(&q);
  assert(FSL_RC_STEP_DONE==rc);
  fsl_stmt_finalize(&q);
  // Now the same query with a RETURNING clause...
  rc = fsl_cx_prepare(f, &q, "INSERT INTO RET(c) "
                      "VALUES(random()) RETURNING *");
  assert(0==rc);
  assert(3==fsl_stmt_col_count(&q));
  rc = fsl_stmt_step(&q);
  assert(FSL_RC_STEP_ROW==rc);
  assert(fsl_stmt_g_int32(&q,0)>0);
  assert(fsl_stmt_g_text(&q,1,NULL)!=NULL);
  fsl_stmt_finalize(&q);
  fsl_cx_exec(f, "DROP TABLE RET");
  TEST_FOOTER;
}

static void  test_temp_dirs(void){
  TEST_HEADER;
  char *z;
  char ** dirs = fsl_temp_dirs_get();
  for(int i = 0; (z=dirs[i]); ++i){
    MARKER(("Temp dir #%d = %s\n", i+1, z));
  }

  fsl_buffer b = fsl_buffer_empty;
  fsl_buffer_reserve(&b, 128);
  for(int i = 0; i < 5; ++i){
    int const rc = fsl_file_tempname(&b, "f-sanity", dirs);
    assert(0==rc);
    MARKER(("fsl_file_tempname() #%d = %s\n",
            i+1, fsl_buffer_cstr(&b)));
    fsl_buffer_reuse(&b);
  }
  fsl_temp_dirs_free(dirs);
  fsl_buffer_clear(&b);
  TEST_FOOTER;
}

static void test_file_cp_mv(void){
  TEST_HEADER;
  int rc;
  char const *zFrom = THIS_SRC_FNAME;
  char const *zTo;
  char const *zTo2;
  fsl_buffer bTo = fsl_buffer_empty;
  fsl_buffer bTo2 = fsl_buffer_empty;

#if !FSL_PLATFORM_IS_WINDOWS
  assert(fsl_file_isexec("f-sanity"));
#endif
  rc = fsl_file_tempname(&bTo, "f-sanity", NULL);
  assert(0==rc);
  zTo = fsl_buffer_cstr(&bTo);
  f_out("Temp file = %s\n", zTo);
  rc = fsl_file_copy(zFrom, zTo);
  assert(0==rc && "Copy failed");
  rc = fsl_stat(zTo, NULL, false);
  assert(0==rc && "stat() failed");

  fsl_buffer_appendf(&bTo2, "%b2", &bTo);
  zTo2 = fsl_buffer_cstr(&bTo2);
  rc = fsl_file_rename(zTo, zTo2);
  assert(0==rc && "Rename failed");
  rc = fsl_stat(zTo2, NULL, false);
  assert(0==rc && "stat() failed");
  rc = fsl_stat(zTo, NULL, false);
  assert(FSL_RC_NOT_FOUND==rc);
  rc = fsl_file_unlink(zTo2);
  assert(0==rc && "Unlink failed");
  rc = fsl_stat(zTo2, NULL, false);
  assert(FSL_RC_NOT_FOUND==rc);

  fsl_buffer_clear(&bTo);
  fsl_buffer_clear(&bTo2);
  TEST_FOOTER;
}

static void test_is_top_of_ckout(void){
  TEST_HEADER;
  assert(!fsl_is_top_of_ckout(".."));
  assert(fsl_is_top_of_ckout("."));
  assert(!fsl_is_top_of_ckout("../.."));
  assert(!fsl_is_top_of_ckout("src"));
  TEST_FOOTER;
}

static void test_ticket_1(void){
  TEST_HEADER;
  fsl_deck d = fsl_deck_empty;
  assert( FSL_SATYPE_ANY == d.type );
  d.type = FSL_SATYPE_TICKET;
  fsl_deck_K_set(&d, NULL);
  assert(d.K);
  assert((fsl_size_t)FSL_STRLEN_SHA1==fsl_strlen(d.K));
  fsl_deck_finalize(&d);
  TEST_FOOTER;
}

static void  test_tkt_id(void){
  TEST_HEADER;
  int rc;
  fsl_cx * const f = fcli_cx();
  fsl_id_t * ridList = 0;

  rc = fsl_tkt_id_to_rids(f, "f319404ba616146654ab5ac9f7d0de3a38e6ed9f",
                          &ridList);
  assert(0==rc);
  assert(ridList && ridList[0]>0);
  assert(ridList[1]>0);
  fsl_free(ridList);
  ridList = 0;

  rc = fsl_tkt_id_to_rids(f, "f319404ba6", &ridList);
  //fcli_err_report(false);
  assert(0==rc);
  assert(ridList && ridList[0]>0);
  assert(ridList[1]>0);
  fsl_free(ridList);
  ridList = 0;

  rc = fsl_tkt_id_to_rids(f, "abcdef123456", &ridList);
  assert(FSL_RC_NOT_FOUND==rc);
  fcli_err_reset();
  assert(NULL==ridList);

  rc = fsl_tkt_id_to_rids(f, "ef8", &ridList);
  assert(FSL_RC_AMBIGUOUS==rc);
  fcli_err_reset();
  assert(NULL==ridList);

  TEST_FOOTER;
}

/* fsl_deck_visitor_f() impl */
static int fdv_test( fsl_cx * const f, fsl_deck * const d,
                     void * state ){
  ++(*((int*)state));
  (void)f;
  (void)d;
  return 0;
}

static void test_deck_foreach(void){
  TEST_HEADER;
  int rc;
  int state;
  fsl_cx * const f = fcli_cx();
  rc = fsl_deck_foreach(f, FSL_SATYPE_CLUSTER, fdv_test, &state);
  assert(FSL_RC_TYPE==rc);
  fcli_err_reset();
  //fcli_err_report(true);
  struct {
    fsl_satype_e type;
    char const * label;
  } types[] = {
  {FSL_SATYPE_WIKI, "Wiki pages"},
  {FSL_SATYPE_TICKET, "Ticket changes"},
  {FSL_SATYPE_CONTROL, "Control artifacts"},
  {FSL_SATYPE_CHECKIN, "Checkins"},
  {FSL_SATYPE_TECHNOTE, "Tech-notes changes"},
  {FSL_SATYPE_FORUMPOST, "Forum posts"}
  };
  for(unsigned int i = 0; i<sizeof(types)/sizeof(types[0]); ++i){
    state = 0;
    rc = fsl_deck_foreach(f, types[i].type, fdv_test, &state);
    fcli_err_report(true);
    assert(0==rc);
    if(FSL_SATYPE_FORUMPOST!=types[i].type){
      assert(state>0);
    }
    f_out("%s: %d\n", types[i].label, state);
  }

  TEST_FOOTER;
}

static void test_foci(void){
  TEST_HEADER;
  int rc;
  fsl_deck d = fsl_deck_empty;
  fsl_cx * const f = fcli_cx();
  fsl_stmt q = fsl_stmt_empty;
  rc = fsl_deck_load_sym(f, &d, "trunk", FSL_SATYPE_CHECKIN);
  assert(0==rc);
  fsl_cx_prepare(f, &q,
                 "SELECT filename, uuid FROM fsl_foci('trunk') "
                 "ORDER BY filename");
  int fCount = 0;
  while(1){
    fsl_card_F const * fcD = NULL;
    rc = fsl_deck_F_next(&d, &fcD);
    assert(0==rc);
    rc = fsl_stmt_step(&q);
    if(NULL==fcD){
      assert(FSL_RC_STEP_DONE==rc);
      break;
    }else{
      ++fCount;
      assert(FSL_RC_STEP_ROW==rc);
    }
    char const * str = fsl_stmt_g_text(&q, 0, NULL);
    assert(0==fsl_strcmp(str, fcD->name));
    str = fsl_stmt_g_text(&q, 1, NULL);
    assert(0==fsl_strcmp(str, fcD->uuid));
  }
  fsl_stmt_finalize(&q);
  fsl_deck_finalize(&d);
  f_out("Compared %d files using foci.\n", fCount);
  TEST_FOOTER;
}

static void test_ckout_rename(void){
  TEST_HEADER;
  int rc;
  fsl_cx * const f = fcli_cx();
  fsl_db * const db = fsl_cx_db(f);
  fsl_list flist = fsl_list_empty;
  char const * zOld = "../autosetup/README.autosetup";
  char const * zNew = "../autosetup/README.out";
  fsl_ckout_rename_opt opt = fsl_ckout_rename_opt_empty;
  test_pushd("f-apps");
  {
    opt.src = &flist;
    assert(!fsl_cx_txn_level(f));
    rc = fsl_cx_txn_begin(f);
    assert(0==rc);
    assert(fsl_cx_txn_level(f)==1);

#if 0
    rc = fsl_cx_exec_multi(f, "select fsl_print('hi'); "
                           "create table tt(a); COMMIT; "
                           "select fsl_print('bye');");
    fcli_err_report(false);
    assert(0==rc);
    assert(!"that should have failed fatally.");
#endif

    fsl_list_reserve(&flist, 5);

    int check = fsl_db_g_int32(db, 0,  "SELECT COUNT(*) FROM vfile "
                               "WHERE pathname='Makefile.in'");
    assert(1==check);
    assert(fsl_is_file(zOld));
    assert(!fsl_is_file(zNew));
    opt.relativeToCwd = true;
    opt.doFsMv = true;

#define SETSRC(X) flist.list[0] = (void*)X; flist.used = 1

    SETSRC(zOld);
    opt.dest = zNew;
    fcli_err_reset();
    rc = fsl_ckout_rename(f, &opt);
    //fcli_err_report(true);
    assert(0==rc);
    assert(!fsl_is_file(zOld));
    assert(fsl_is_file(zNew));

    opt.doFsMv = false;

    check = fsl_db_g_int32(db, 0,  "SELECT COUNT(*) FROM vfile "
                           "WHERE origname='autosetup/README.autosetup' "
                           "AND pathname='autosetup/README.out'");
    assert(1==check);

    SETSRC("f-add.c"); opt.dest = "f-sanity.c";
    fcli_err_reset();
    rc = fsl_ckout_rename(f, &opt);
    //fcli_err_report(false);
    assert(FSL_RC_ALREADY_EXISTS==rc);

    SETSRC("../src"); opt.dest = "../Makefile";
    /* Moving dir over a non-dir. */
    fcli_err_reset();
    rc = fsl_ckout_rename(f, &opt);
    //fcli_err_report(false);
    assert(FSL_RC_TYPE==rc);

    SETSRC("nopenope"); opt.dest = "nope";
    fcli_err_reset();
    rc = fsl_ckout_rename(f, &opt);
    //fcli_err_report(false);
    assert(FSL_RC_NOT_FOUND==rc);

    SETSRC("f-sanity.c"); opt.dest = "../src";
    fcli_err_reset();
    rc = fsl_ckout_rename(f, &opt);
    fcli_err_report(false);
    assert(0==rc);
    fsl_db_g_int32(db, 0,  "SELECT COUNT(*) FROM vfile "
                   "WHERE origname='f-apps/f-sanity.c' "
                   "AND pathname='src/f-sanity.c'");
    assert(1==check);

    // Try ^^^^ that ^^^ again w/ a trailing slash...
    SETSRC("f-add.c"); opt.dest = "../src/";
    rc = fsl_ckout_rename(f, &opt);
    fcli_err_report(false);
    assert(0==rc);
    fsl_db_g_int32(db, 0,  "SELECT COUNT(*) FROM vfile "
                   "WHERE origname='f-apps/f-add.c' "
                   "AND pathname='src/f-add.c'");
    assert(1==check);

    // Try to revert the original rename...
    bool didSomething = false;
    assert(fsl_is_file(zNew));
    assert(!fsl_is_file(zOld));
    //MARKER(("Revert 1... zNew=%s\n", zNew));
    rc = fsl_ckout_rename_revert(f, zNew, true, true, &didSomething);
    assert(0==rc);
    assert(didSomething);
    assert(!fsl_is_file(zNew));
    assert(fsl_is_file(zOld));
    //MARKER(("Revert 1 again... zNew=%s\n", zNew));
    didSomething = false;
    rc = fsl_ckout_rename_revert(f, zNew, true, true, &didSomething);
    assert(0==rc);
    assert(!didSomething);

#undef SETSRC
    fsl_list_reserve(&flist, 0);
    fsl_cx_err_reset(f);
    fsl_cx_txn_end(f, true);
  }
  test_popd();
  TEST_FOOTER;
}

static void test_fcli_sync(void){
  TEST_HEADER;
  f_out("Trying out a sync (if fossil is found "
        "and autosync is enabled). "
        "This can fail for any number of reasons.\n");
  fcli_sync( FCLI_SYNC_PULL | FCLI_SYNC_AUTO );
  fcli_err_report(true)/*can fail for any number of reasons.*/;
  fsl_cx * const f = fcli_cx();
  fsl_cx_txn_begin(f);
  int rc = fcli_sync( FCLI_SYNC_PULL );
  assert(FSL_RC_LOCKED==rc && "Transaction write-locks the db.");
  fsl_cx_txn_end(f, true);
  fcli_err_reset();
  TEST_FOOTER;
}

static void test_buffer_err(void){
  TEST_HEADER;
  fsl_buffer b1 = fsl_buffer_empty;
  int rc;

  rc = fsl_buffer_reserve(&b1, 1);
  assert(0==b1.errCode);
  assert(0==rc);
  assert(b1.mem);

  // non-0 reserve must propagate errCode....
  void const * m = b1.mem;
  b1.errCode = 3;
  rc = fsl_buffer_reserve(&b1, b1.capacity * 2);
  assert(3==b1.errCode);
  assert(b1.errCode==rc);
  assert(m == b1.mem);

  rc = fsl_buffer_compress(&b1, &b1);
  assert(b1.errCode==rc);

  // errCode must not interfere with freeing the buffer, which also
  // clears its error state.
  b1.errCode = FSL_RC_LOCKED;
  rc = fsl_buffer_reserve(&b1, 0);
  assert(!b1.mem);
  assert(0==b1.errCode);
  TEST_FOOTER;
}

static void test_buffer_esc_arg(void){
  TEST_HEADER;
  fsl_buffer b = fsl_buffer_empty;

  fsl_buffer_esc_arg(&b, "arg one", false);
  fsl_buffer_esc_arg(&b, "'arg 2'", false);
  fsl_buffer_esc_arg(&b, "-afile", true);
  assert( 0==b.errCode );
  //f_out("buffer=%b\n", &b);
  assert( fsl_buffer_eq(&b, "'arg one' \\'arg\\ 2\\' ./-afile", -1) );

  fsl_error err = fsl_error_empty;
  fsl_buffer_reuse(&b);
  fsl_buffer_esc_arg_v2(&b, &err, "a\tb", 0);
  assert( FSL_RC_SYNTAX==err.code );
  //f_out("Got expected error: %b\n", &err.msg);
  assert( err.code==b.errCode );

  fsl_buffer_reuse(&b);
  fsl_buffer_esc_arg_v2(&b, &err, "a b c", 0);
  assert( 0==err.code && "it gets reset by fsl_buffer_escape_arg()" );
  assert( 0==b.errCode );

  fsl_buffer b2 = fsl_buffer_empty;
  fsl_buffer_appendch(&b2, 'a');
  fsl_buffer_appendch(&b2, (char)0x80/*invalid*/);
  fsl_buffer_appendch(&b2, 'b');
  fsl_buffer_reuse(&b);
  fsl_buffer_esc_arg_v2(&b, &err, fsl_buffer_cstr(&b2), 0);
  fsl_buffer_clear(&b2);
  assert( FSL_RC_SYNTAX==err.code );
  //f_out("Got expected error: %b\n", &err.msg);
  assert( err.code==b.errCode );


  /* fsl_buffer_ensure_eol() and fsl_buffer_chomp() */
  fsl_buffer_reuse(&b);
  fsl_buffer_ensure_eol(&b);
  assert( fsl_buffer_eq(&b, "", -1) );
  fsl_buffer_appendch(&b, 'a');
  fsl_buffer_ensure_eol(&b);
  assert( fsl_buffer_eq(&b, "a\n", -1) );
  fsl_buffer_chomp(&b);
  assert( fsl_buffer_eq(&b, "a", -1) );
  fsl_buffer_chomp(&b);
  assert( fsl_buffer_eq(&b, "a", -1) );
  fsl_buffer_append(&b, "\r\nb", -1);
  fsl_buffer_chomp(&b);
  assert( fsl_buffer_eq(&b, "a\r\nb", -1) );
  b.mem[--b.used] = 0;
  assert( fsl_buffer_eq(&b, "a\r\n", -1) );
  fsl_buffer_chomp(&b);
  assert( fsl_buffer_eq(&b, "a", -1) );

  fsl_buffer_clear(&b);
  fsl_error_clear(&err);
  TEST_FOOTER;
}

static void test_branch_main(void){
  TEST_HEADER;
  char const * z = 0;
  char const * z2 = 0;
  fsl_cx * const f = fcli_cx();
  int rc = fsl_branch_main(f, &z, false);
  assert(0==rc);
  assert( z );
  rc = fsl_branch_main(f, &z2, false);
  assert(0==rc);
  assert( z2==z );
  assert( 0==fsl_strcmp(z, z2) );
  TEST_FOOTER;
}


/**
   Count permissions set in u. mode<0 => uninherited, mode>0 =>
   inherited, mode=0 => both.
*/
static unsigned upermCount(fsl_uperm const * u, int mode){
  unsigned rv = 0;
#define M(MEMBER,CH) if(u->MEMBER) ++rv;
  if( mode<=0 ){
    fsl_uperm_map_base(M);
  }
  if( mode>=0 ){
    fsl_uperm_map_inherited(M);
  }
#undef M
  return rv;
}

static void test_uperm(void){
  TEST_HEADER;
  fsl_cx * const f = fcli_cx();
  fsl_uperm up = fsl_uperm_empty;
  fsl_uperm * const u = &up;
  int rc = fsl_uperm_add(f, u, "!");
  fcli_err_reset();
  assert( FSL_RC_RANGE==rc );
  assert( 0==upermCount(u, 0) );

  rc = fsl_cx_txn_begin(f);
  assert(!rc);

  rc = fsl_cx_exec_multi(f,
                         "UPDATE user set cap='opu' where login='developer';"
                         "UPDATE user set cap='27' where login='reader';");
  assert(!rc);

  rc = fsl_uperm_add(f, u, "v");
  assert( 4==upermCount(u, -1)
          && "Expecting the 'developer' user to have a 'op27' perms" );
  assert( up.read );
  assert( up.password );
  assert( up.readForum );
  assert( up.emailAlert );

  up = fsl_uperm_empty;
  rc = fsl_uperm_add(f, u, "u");
  assert( 2==upermCount(u, -1)
          && "Expecting the 'reader' user to have '27' perms" );
  assert( up.readForum );
  assert( up.xReader );
  assert( up.emailAlert );

  up = fsl_uperm_empty;
  fsl_uperm_add(f, u, "C3");
  assert( 3==upermCount(u,0) /* '3'==>read/writeForum */ );
  assert( up.chat );
  assert( up.writeForum );

  fsl_cx_txn_end(f, true);
  TEST_FOOTER;
}

static void test_custom_formats(void){
  TEST_HEADER;
  fsl_buffer _b = fsl_buffer_empty;
  fsl_buffer * const b = &_b;
#define baf fsl_buffer_appendf
#define beq fsl_buffer_eq
#define bre fsl_buffer_reuse(b)

  baf(bre, "%R", FSL_RC_IO);
  //f_out("R=%b\n", b);
  assert( beq(b, "FSL_RC_IO", 9) );
  baf(bre, "%R", FSL_RC_end+1);
  //f_out("R=%b\n", b);
  assert( b->mem[0] == '#' );

#undef baf
#undef beq
  fsl_buffer_clear(b);
  TEST_FOOTER;
}

int main(int argc, char const * const * argv ){
  int rc = 0;
  fsl_cx * f;
  bool singleTest = false;
  bool dumpSqlCache = false;
  fsl_timer timer = fsl_timer_empty;
  assert( !timer.wall );
  fsl_timer_start(&timer);
  assert( timer.wall );
  fcli_cliflag FCliFlags[] = {
    FCLI_FLAG_BOOL("1","single",&singleTest,"Run only the currently "
                   "in-development test, not the whole suite."),
    FCLI_FLAG_BOOL(NULL,"cached-sql", &dumpSqlCache,
                   "Dump cached fsl_stmt objects to fcli_printf()."),
    fcli_cliflag_empty_m
  };
  fcli_help_info FCliHelp = {
  "Runs some sanity tests on the libfossil API. "
  "Requires a checkout of libfossil.",
  NULL, NULL
  };

  //test_popd(); // must assert()
  rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;

  if((rc = fcli_has_unused_flags(0))) goto end;
  f = fcli_cx();
  if(!f || !fsl_cx_db_ckout(f)){
    fcli_err_set(FSL_RC_MISUSE,
                 "This app requires a checkout db.");
    goto end;
  }
  f_out("fcli.appName = %s\n", fcli.appName);
  f_out("fcli_progname() = %s\n", fcli_progname());
  f_out("Checkout dir = %s\n", fsl_cx_ckout_dir_name(f, NULL));
  f_out("Checkout db = %s\n", fsl_cx_db_file_ckout(f, NULL));
  f_out("Repo db = %s\n", fsl_cx_db_file_repo(f, NULL));
  f_out("MAIN db ");
  fsl_db_each( fsl_cx_db(f), fsl_stmt_each_f_dump, f,
               "PRAGMA main.journal_mode");

  fsl_id_t ckoutRid = 0;
#define CKOUT_RID_CHECK \
  fsl_ckout_version_info(f, &ckoutRid, NULL); \
  assert(f->db.ckout.rid==ckoutRid)

  if(!singleTest){
    test_buffer_err();
    test_buffer_esc_arg();
    rc = test_sanity_repo();
    if(rc) goto end;
    rc = test_sanity_delta();
    if(rc) goto end;
    CKOUT_RID_CHECK;
    assert(fsl_rid_is_a_checkin(f, 1));
    assert(!fsl_rid_is_a_checkin(f, 2));
    test_repo_fingerprint();
    test_pathfinder();
    test_fs_dirpart();
    test_sanity_tkt_01();
    test_sanity_tkt_fields();
    test_sanity_fs();
    test_sanity_localtime();
    test_julian();
    test_julian2();
    test_dir_names();
    test_tree_name();
    CKOUT_RID_CHECK;
    test_strftime();
    test_config_db();
    test_mtime_of_manifest();
    test_buffer_count_lines();
    test_buffer_streams();
    test_buffer_seek();
    test_buffer_getdelim();
    test_glob_list();
    test_vtime_check();
    test_buffer_compare();
    test_file_add();
    test_file_rm();
    test_branch_create();
    CKOUT_RID_CHECK;
    test_file_simplify_name();
    test_stmt_cached();
    test_buffer_compress();
    test_repo_filename_to_fnid();
    test_repo_extract()
      /*
        A) VERY memory-hungry!

        B) fails if called after test_checkin_file_list()!
        (Last time i checked)
      */;
    test_checkin_file_list();
    CKOUT_RID_CHECK;
    test_versionable_config();
    test_cx_glob_lists();
    test_import_blob();
    test_date2();
#if FSLPRINTF_ENABLE_JSON
    test_printf_json();
#endif
    test_vpath_1();
    test_vpath_2();
    test_bind_fmt();
    test_confirmation();
    test_appendf_comma();
    test_deck_set();
    test_deck_derive();
    test_appendf_utf8_precision();
    test_urldecode();
    test_external_buffers();
    test_stmt_returning();
    if(0) test_simplify_sql();
    test_temp_dirs();
    test_file_cp_mv();
    test_is_top_of_ckout();
    test_ticket_1();
    test_tkt_id();
    test_deck_foreach();
    test_foci();
    CKOUT_RID_CHECK;
    if(1) test_ckout_rename();
    test_fcli_sync();
    test_branch_main();
    test_uperm();
    test_custom_formats();
    if(0) test_gradient();
  }else{
    /* placeholder for use while developing tests. */
    test_ckout_rename();
    CKOUT_RID_CHECK;
    test_dir_names();
    CKOUT_RID_CHECK;
    test_buffer_getdelim();
    test_fs_dircrawl();
  }
  f_out("Fossil binary: %s\n", fcli_fossil_binary(false, 0));
  {
    char const * ckoutUuid = NULL;
    fsl_ckout_version_info(f, &ckoutRid, &ckoutUuid);
    f_out("checkout UUID=%s (RID %"FSL_ID_T_PFMT")\n",
          ckoutUuid, (fsl_id_t)ckoutRid);
  }
  if(dumpSqlCache){
    fcli_dump_stmt_cache(true);
  }
  fcli_dump_cache_metrics();
  f_out("If you made it this far, no assertions were triggered. "
        "Now try again with valgrind.\n");
  end:

  {
    fsl_timer_stop(&timer);
    f_out("Total run time: %.3f ms wall, %.3f ms CPU time\n",
          timer.wall/1000.0, (timer.user+timer.system)/1000.0 );
  }
  return fcli_end_of_main(rc);
}

#undef MARKER
