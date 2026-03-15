/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code
*/
/**
   This app implements renaming (a.k.a. moving) files within a
   fossil checkout.
*/
#include "libfossil.h"
#include <string.h>

#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

static struct {
  bool quiet;
} App = {
false
};


static int fsl_ckout_rename_f_my(fsl_cx * const f,
                                 fsl_ckout_rename_opt const * opt,
                                 char const * zSrcName, char const *zDestName){
  if(f || opt){/*unused*/}
  if(!App.quiet){
    f_out("%s: %s ==> %s\n", opt->doFsMv ? "MOVE": "RENAME",
          zSrcName, zDestName);
  }
  ++(*(int*)opt->callbackState);
  return 0;
}

int main(int argc, const char * const * argv ){
  fsl_list flist = fsl_list_empty;
  fsl_ckout_rename_opt opt = fsl_ckout_rename_opt_empty;
  const fcli_cliflag FCliFlags[] = {
    FCLI_FLAG_BOOL("n","dry-run", &opt.dryRun,"Dry-run mode."),
    FCLI_FLAG_BOOL("mv","move", &opt.doFsMv,
                   "Move the files in the filesystem "
                   "except in --dry-run mode. Defaults to true if "
                   "app is invoked as f-mv or f-move, else false."),
    FCLI_FLAG_BOOL("q","quiet", &App.quiet,
                   "Do not list each file as it's renamed."),
    fcli_cliflag_empty_m // list MUST end with this (or equivalent)
  };
  const fcli_help_info FCliHelp = {
    "Queues up file renames within a checkout.",
    "FILE NEW-NAME || FILEs... DIRECTORY",
    NULL // optional callback which outputs app-specific help
  };
  fsl_cx * f = NULL;
  char * zDest = NULL;
  int rc;

  opt.dryRun = false;
  opt.doFsMv = 0!=strstr(argv[0], "-move") || 0!=strstr(argv[0], "-mv");
  rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  f = fcli_cx();
  if(!fsl_needs_ckout(f)){
    goto end;
  }
  rc = fsl_cx_txn_begin(f);
  if(rc) goto end;
  if((rc=fcli_has_unused_flags(false))) goto end;
  while(true){
    char const * zArg = fcli_next_arg(true);
    if(fcli_next_arg(false)){
      fsl_list_append(&flist, (void*)zArg);
    }else{
      rc = fsl_filename_preferred_case(false, ".",
                                       zArg, &zDest);
      if(rc) goto end;
      break;
    }
  }
  if(!flist.used){
    rc = fcli_err_set(FSL_RC_MISUSE,
                      "Expecting (OLDNAME NEWNAME) or "
                      "(OLDNAME(s) DIRECTORY) arguments.");
    goto end;
  }
  assert(zDest);

  int counter = 0;
  opt.dest = zDest;
  opt.relativeToCwd = true;
  opt.callback = fsl_ckout_rename_f_my;
  opt.callbackState = &counter;
  opt.src = &flist;
  rc = fsl_ckout_rename(f, &opt);

  if(rc) goto end;
  f_out("%sRenamed %d file(s).\n",
        App.quiet ? "" : "\n", counter);

  if(fcli_is_verbose()){
    f_out("All pending renames:\n");
    fsl_db_each( fsl_cx_db_ckout(f), fsl_stmt_each_f_dump, f,
                 "SELECT origname as 'Original name', "
                 "pathname as '==> New name'"
                 "FROM vfile WHERE origname IS NOT NULL AND "
                 "origname<>pathname");
  }

  end:
  fsl_free(zDest);
  fsl_list_reserve(&flist, 0);
  if(f && fsl_cx_txn_level(f)){
    if(opt.dryRun){
      f_out("Dry-run mode. Rolling back transaction.\n");
    }
    if(opt.dryRun || rc) fsl_cx_txn_end(f, true);
    else rc = fsl_cx_txn_end(f, false);
  }
  return fcli_end_of_main(rc)
    /* Will report any pending error state and return either
       EXIT_SUCCESS or EXIT_FAILURE. */;
}
