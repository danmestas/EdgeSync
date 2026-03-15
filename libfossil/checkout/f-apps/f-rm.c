/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).

  *****************************************************************************
  This file is for testing the ability to "remove" files from a checkout.
*/

#include "libfossil.h"

static struct {
  bool dryRun;
  bool unlink;
  short int verbose;
  unsigned int counter;
  fsl_buffer fname;
} App = {
false, false, 0,
0, fsl_buffer_empty_m
};

static int fsl_ckout_unmanage_f_my(fsl_ckout_unmanage_state const * us){
  f_out("UNMANAGED    %s\n", us->filename);
  ++App.counter;
  if(App.unlink){
    fsl_buffer_reuse(&App.fname);
    if(0==fsl_cx_stat2(us->f, false, us->filename,
                       NULL, &App.fname, true)){
      if(App.verbose){
        if(App.dryRun){
          f_out("Dry-run: not unlinking: %b\n", &App.fname);
        }else{
          f_out("Unlinking: %b\n", &App.fname);
        }
      }
      if(!App.dryRun){
        int const rc = fsl_file_unlink(fsl_buffer_cstr(&App.fname));
        if(rc && App.verbose){
          f_out("Unlink failed: %s: %b\n", fsl_rc_cstr(rc), &App.fname);
        }
      }
    }else{
      /* Keep (unimportant) failed stat from propagating up. */
      fcli_err_reset();
    }
  }
  return 0;
}

int main(int argc, char const * const * argv ){
  int rc = 0;
  fsl_cx * f;
  bool inTrans = false;
  fsl_db * db;
  fsl_id_bag idBag = fsl_id_bag_empty;
  fcli_cliflag FCliFlags[] = {
    FCLI_FLAG_BOOL("n","dry-run", &App.dryRun,"Dry-run mode."),
    FCLI_FLAG_BOOL("u", "unlink", &App.unlink,
                   "Unlink (delete) newly-unmanaged files. "
                   "Will silently fail if the files cannot be "
                   "deleted, e.g. due to access rights. The "
                   "--verbose flag will cause it to report such "
                   "cases but they do not cause the unmanagement "
                   "to fail."),
    FCLI_FLAG_BOOL("rm", 0, &App.unlink, "Alias for --unlink."),
    fcli_cliflag_empty_m
  };
  fcli_help_info FCliHelp = {
  "Queues up files to be removed from future SCM history (unmanaged) "
  "at the next commit.",
  "file [...file]", NULL
  };
  rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  f = fcli_cx();
  db = fsl_cx_db_ckout(f);
  if(!db){
    rc = fsl_cx_err_set(f, FSL_RC_NOT_A_CKOUT,
                        "This app requires a checkout db.");
    goto end;
  }

  if(fcli_has_unused_flags(0)) goto end;
  else if(!fcli_next_arg(false)){
    rc = fcli_err_set(FSL_RC_MISUSE,"No file names provided.");
    goto end;
  }

  fsl_buffer_reserve(&App.fname, 1024);
  rc = fsl_cx_txn_begin(f);
  if(rc){
    goto end;
  }
  inTrans = true;

  rc = fsl_vfile_changes_scan(f, 0, 0);
  if(rc) goto end;
  rc = fcli_args_to_vfile_ids(&idBag, 0, true, false);
  if(rc) goto end;
  fsl_ckout_unmanage_opt ropt = fsl_ckout_unmanage_opt_empty;
  ropt.scanForChanges = false;
  ropt.vfileIds = &idBag;
  ropt.relativeToCwd = true;
  ropt.callback = fsl_ckout_unmanage_f_my;
  App.verbose = fcli_is_verbose();
  rc = fsl_ckout_unmanage(f, &ropt);
  if(rc) goto end;
  f_out("Total number of files rm'd: %u\n", App.counter);

  if(App.dryRun){
    f_out("Dry-run mode. Rolling back transaction.\n");
    fsl_cx_txn_end(f, true);
  }else{
    rc = fsl_cx_txn_end(f, false);
  }
  inTrans = 0;
  end:
  fsl_buffer_clear(&App.fname);
  fsl_id_bag_clear(&idBag);
  if(inTrans){
    fsl_cx_txn_end(f, true);
  }
  return fcli_end_of_main(rc);
}
