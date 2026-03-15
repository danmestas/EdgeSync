/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code
*/
/**
   This app displays various stats for a fossil repo db, analog to
   (fossil dbstat).
*/
#include "libfossil.h"

static int dump_repo_stats(bool brief, int check){
  int rc = 0;
  fsl_cx * const f = fcli_cx();
  fsl_db * const db = fsl_cx_db_repo(f);
  char * z = NULL;
  //char const * zC = NULL;
  int const colWidth = -19;
  fsl_int_t repoSize;
  int32_t n, m;

  rc = fsl_cx_txn_begin(f);
  if(rc) return rc;

  if( (z = fsl_config_get_text(f, FSL_CONFDB_REPO, "project-name", NULL))
      || (z = fsl_config_get_text(f, FSL_CONFDB_REPO, "short-project-name", NULL))){
    f_out("%*s%z\n", colWidth, "project-name:", z);
    z = NULL;
  }

  repoSize = fsl_file_size(fsl_cx_db_file_repo(f, NULL));
  f_out("%*s%,"FSL_INT_T_PFMT" bytes\n", colWidth,
        "repository-size:", repoSize);

  if(brief) goto end_nonbrief;

  int32_t a, b, szAvg, szMax;
  int64_t t;
  fsl_stmt q = fsl_stmt_empty;
  rc = fsl_cx_prepare(f, &q, "SELECT total(size), avg(size), max(size)"
                      " FROM blob WHERE size>0");
  if(rc) goto end;
  else{
    fsl_stmt_step(&q);
    t = fsl_stmt_g_int64(&q, 0);
    szAvg = fsl_stmt_g_int32(&q, 1);
    szMax = fsl_stmt_g_int32(&q, 2);
    fsl_stmt_finalize(&q);
  }

  f_out("%*s%,"PRIi32" average, %,"PRIi32" max, %,"PRIi64" total\n",
        colWidth, "artifact-sizes:", szAvg, szMax, t);
  if(t/repoSize < 5){
    b = 10;
    repoSize /= 10;
  }else{
    b = 1;
  }
  a = t / repoSize;
  f_out("%*s%"PRIi32":%"PRIi32"\n", colWidth, "compression-ratio:",
        a, b);

  n = fsl_db_g_int32(db, 0, "SELECT COUNT(*) FROM event e WHERE e.type='ci'");
  f_out("%*s%,d\n", colWidth, "check-ins:", n);
  n = fsl_db_g_int32(db, 0, "SELECT count(*) FROM filename /*scan*/");
  f_out("%*s%,"PRIi32" across all branches\n", colWidth, "files:", n);
  n = fsl_db_g_int32(db, 0,
                     "SELECT count(*) FROM ("
                     "SELECT DISTINCT substr(tagname,6) "
                     "FROM tag JOIN tagxref USING('tagid')"
                     " WHERE tagname GLOB 'wiki-*'"
                     " AND TYPEOF(tagxref.value+0)='integer'"
                     ")");
  m = fsl_db_g_int32(db, 0, "SELECT COUNT(*) FROM event WHERE type='w'");
  f_out("%*s%,"PRIi32" (%,"PRIi32" changes)\n", colWidth, "wiki-pages:", n, m);
  n = fsl_db_g_int32(db, 0, "SELECT count(*) FROM tag  /*scan*/"
                   " WHERE tagname GLOB 'tkt-*'");
  m = fsl_db_g_int32(db, 0, "SELECT COUNT(*) FROM event WHERE type='t'");
  f_out("%*s%,"PRIi32" (%,"PRIi32" changes)\n", colWidth, "tickets:", n, m);
  n = fsl_db_g_int32(db, 0, "SELECT COUNT(*) FROM event WHERE type='e'");
  f_out("%*s%,d\n", colWidth, "tech-notes:", n);

  if(fsl_db_table_exists(db, FSL_DBROLE_REPO, "forumpost")){
    n = fsl_db_g_int32(db, 0, "SELECT count(*) FROM forumpost/*scan*/");
    if( n>0 ){
      m = fsl_db_g_int32(db, 0, "SELECT count(*) FROM forumpost"
                         " WHERE froot=fpid");
      f_out("%*s%,"PRIi32" (on %,"PRIi32" threads)\n", colWidth,
            "forum-posts:", n, m);
    }
  }

  n = fsl_db_g_int32(db, 0, "SELECT COUNT(*) FROM event WHERE type='g'");
  f_out("%*s%,"PRIi32"\n", colWidth, "tag-changes:", n);

  z = fsl_db_g_text(db, NULL, "SELECT datetime(mtime) || ' UTC - about ' ||"
              " CAST(julianday('now') - mtime AS INTEGER)"
              " || ' days ago' FROM event "
              " ORDER BY mtime DESC LIMIT 1");
  f_out("%*s%z\n", colWidth, "latest-change:", z);
  z = NULL;

  end_nonbrief:
  n = fsl_db_g_int32(db, 0, "SELECT julianday('now') - "
                     "(SELECT min(mtime) FROM event) + 0.99");
  f_out("%*s%,"PRIi32" days, approximately %.2f years\n",
        colWidth, "project-age:", n, n/365.2425);
  if(!brief){
    z = fsl_config_get_text(f, FSL_CONFDB_REPO, "project-code", NULL);
    if( z ){
      f_out("%*s%z\n", colWidth, "project-id:", z);
      z = NULL;
    }
  }
  z = fsl_config_get_text(f, FSL_CONFDB_REPO, "aux-schema", NULL);
  f_out("%*s%s\n", colWidth, "schema-version:", z ? z : "???");
  fsl_free(z); z = NULL;
  f_out("%*s[%.16s] %.19s UTC (%s)\n",
        colWidth, "libfossil-version:",
        fsl_buildinfo(FSL_BUILDINFO_VERSION_HASH),
        fsl_buildinfo(FSL_BUILDINFO_VERSION_TIMESTAMP),
        fsl_library_version());
  f_out("%*s[%.16s] %.19s UTC (%s)\n",
        colWidth, "sqlite-version:",
        &sqlite3_sourceid()[20], sqlite3_sourceid(),
        sqlite3_libversion());

  f_out("%*s%,"PRIi32" pages, %,"PRIi32" bytes/pg, %,"PRIi32" free page(s), "
        "%z, %z mode\n",
        colWidth, "database-stats:",
        fsl_db_g_int32(db, 0, "PRAGMA repo.page_count"),
        fsl_db_g_int32(db, 0, "PRAGMA repo.page_size"),
        fsl_db_g_int32(db, 0, "PRAGMA repo.freelist_count"),
        fsl_db_g_text(db, NULL, "PRAGMA repo.encoding"),
        fsl_db_g_text(db, NULL, "PRAGMA repo.journal_mode"));

  if(check>0){
    z = fsl_db_g_text(db, NULL, "PRAGMA repo.quick_check(1)");
    f_out("%*s%z\n", colWidth, "database-check:", z);
    z = NULL;
  }
  /*
    Possible TODO from fossil dbstat:
    test-integrity (requires feature additional porting)
  */
  end:
  fsl_cx_txn_end(f, true);
  return rc;
}

int main(int argc, const char * const * argv ){
  bool brief = false;
  bool quickCheck = false;
  const fcli_cliflag FCliFlags[] = {
    FCLI_FLAG_BOOL("b","brief", &brief, "Show fewer stats."),
    FCLI_FLAG_BOOL("c","check", &quickCheck, "Perform quick db integrity check."),
    fcli_cliflag_empty_m // list MUST end with this (or equivalent)
  };
  const fcli_help_info FCliHelp = {
    "Shows various statistics for a fossil repository database.",
    NULL, // very brief usage text, e.g. "file1 [...fileN]"
    NULL // optional callback which outputs app-specific help
  };
  int rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  else if((rc=fcli_has_unused_args(false))) goto end;
  else if(!fsl_needs_repo(fcli_cx())){
    /* Sets the context's error state and will produce an appropriate
       error message from fcli_end_of_main(). */
    goto end;
  }

  /** Perform your app-specific work... */
  rc = dump_repo_stats(brief, quickCheck ? 1 : 0);

  end:
  return fcli_end_of_main(rc)
    /* Will report any pending error state and return either
       EXIT_SUCCESS or EXIT_FAILURE. */;
  /* Sidebar: all of the memory allocated by this demo app is
     owned by the fcli internals and will be properly cleaned up
     during the at-exit phase. Running this app through valgrind
     should report no memory leaks. */
}
