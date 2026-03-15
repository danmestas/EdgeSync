/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/

/*****************************************************************************
  This file implements a basic libfossil client which queues new files for
  addition into a fossil repo.
*/

#include "libfossil.h"

#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

static struct App_ {
  fsl_buffer absPath;
  fsl_buffer coRelPath;
  fsl_ckout_manage_opt addOpt;
  bool quiet;
} App = {
fsl_buffer_empty_m,
fsl_buffer_empty_m,
fsl_ckout_manage_opt_empty_m,
false/*quiet*/
};

static int fsl_ckout_manage_f_my(fsl_ckout_manage_state const * mst,
                                 bool *include){
  *include = true;
  if(!App.quiet){
    f_out("Queuing: %s\n", mst->filename);
  }
  return 0;
}

int main(int argc, char const * const * argv ){
  int rc = 0;
  fsl_cx * f = 0;
  char inTrans = 0;
  fsl_db * db;
  char *zCasedFile = 0;
  bool useIgnoreGlobs = true;
  bool fDryRun = false;
  fsl_ckout_manage_opt addOpt =
    fsl_ckout_manage_opt_empty;
  fcli_cliflag FCliFlags[] = {
    FCLI_FLAG_BOOL("q","quiet", &App.quiet,
                   "Do not output the list of files added."),
    FCLI_FLAG_BOOL("n","dry-run",&fDryRun, "Dry-run mode."),
    FCLI_FLAG_BOOL_INVERT(0,"no-ignore", &useIgnoreGlobs,
                          "Do not check the ignore-globs config "
                          "setting to determine whether to include "
                          "files or not."),
    fcli_cliflag_empty_m
  };
  fcli_help_info FCliHelp = {
  "Queues up files to be added at the next commit.",
  "file1-or-dir [...fileN-or-dir]",
  NULL
  };
  rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  f = fcli_cx();
  db = fsl_needs_ckout(f);
  if(!db) goto end;

  if(fcli_has_unused_flags(0)) goto end;
  else if(!fcli_next_arg(false)){
    rc = fcli_err_set(FSL_RC_MISUSE,"No file names provided.");
    goto end;
  }

  rc = fsl_db_txn_begin(db);
  if(rc){
    fsl_cx_uplift_db_error(f, db);
    goto end;
  }
  inTrans = 1;

  addOpt.relativeToCwd = true;
  addOpt.checkIgnoreGlobs = useIgnoreGlobs;
  addOpt.callback = fsl_ckout_manage_f_my;
  const bool fCaseSensitive = false
    //fsl_cx_is_case_sensitive(f)
    /* force filename case check */;
  while((addOpt.filename = fcli_next_arg(true))){
    rc = fsl_filename_preferred_case(fCaseSensitive, ".",
                                     addOpt.filename, &zCasedFile);
    if(rc){
      fcli_err_set(rc, "Filename case check failed.");
      goto end;
    }
    FCLI_V(("%s ==case-adjusted==> %s\n",addOpt.filename, zCasedFile));
    addOpt.filename = zCasedFile;
    rc = fsl_ckout_manage(f, &addOpt);
    addOpt.filename = 0;
    fsl_free(zCasedFile);
    if(rc) goto end;
  }
  if(fDryRun && fcli_is_verbose()>1){
    f_out("vfile changed, removed, or renamed files:\n");
    fsl_db_each( fsl_cx_db_ckout(f), fsl_stmt_each_f_dump, f,
                 "SELECT * from vfile "
                 "WHERE (chnged OR deleted "
                 "      OR (origname IS NOT NULL AND origname<>pathname)"
                 " )"
                 " ORDER BY pathname");
  }
  f_out("File(s) added:   %u\n", addOpt.counts.added);
  f_out("File(s) updated: %u\n", addOpt.counts.updated);
  f_out("File(s) skipped: %u\n", addOpt.counts.skipped);

  if(fDryRun){
    f_out("Dry-run mode. Rolling back transaction.\n");
    fsl_db_txn_rollback(db);
  }else{
    rc = fsl_db_txn_end(db, 0);
  }
  inTrans = 0;

  end:
  fsl_buffer_clear(&App.absPath);
  fsl_buffer_clear(&App.coRelPath);
  if(inTrans){
    int const rc2 = fsl_db_txn_rollback(db);
    if(!rc) rc = rc2;
  }
  return fcli_end_of_main(rc);
}
