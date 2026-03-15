/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).

  *****************************************************************************
  A simple tool for checking the change status of a checkout.
*/

#include "libfossil.h"

#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

static char const * fsl_ckout_change_label(fsl_ckout_change_e change){
  switch(change){
    case FSL_CKOUT_CHANGE_NONE: return NULL;
    case FSL_CKOUT_CHANGE_MOD: return "MODIFIED";
    case FSL_CKOUT_CHANGE_MERGE_MOD: return "MERGE-MOD";
    case FSL_CKOUT_CHANGE_MERGE_ADD: return "MERGE-ADD";
    case FSL_CKOUT_CHANGE_INTEGRATE_MOD: return "INTEGRATE-MOD";
    case FSL_CKOUT_CHANGE_INTEGRATE_ADD: return "INTEGRATE-ADD";
    case FSL_CKOUT_CHANGE_ADDED: return "ADDED";
    case FSL_CKOUT_CHANGE_REMOVED: return "REMOVED";
    case FSL_CKOUT_CHANGE_MISSING: return "MISSING";
    case FSL_CKOUT_CHANGE_RENAMED: return "RENAMED";
    case FSL_CKOUT_CHANGE_IS_EXEC: return "+EXEC";
    case FSL_CKOUT_CHANGE_NOT_EXEC: return "-EXEC";
    case FSL_CKOUT_CHANGE_BECAME_SYMLINK: return "+SYMLINK";
    case FSL_CKOUT_CHANGE_NOT_SYMLINK: return "-SYMLINK";
    default:
      return "?!NO WAY!?";
  }
}

/**
    A fsl_ckout_changes_f() impl which outputs change status to f_out().
 */
static int fsl_ckout_changes_fapp(void * state, fsl_ckout_change_e change,
                                     char const * filename, char const * origName){
  if(1==++*((int*)state)){
    f_out("\nLocal changes compared to this version:\n\n");
  }
  f_out("%-15s", fsl_ckout_change_label(change));
  if(FSL_CKOUT_CHANGE_RENAMED==change){
    assert(origName && *origName);
    f_out("%s\n%14s %s\n", origName, "->", filename);
  }else{
    f_out("%s\n", filename);
  }
  return 0;
}


static int fapp_show_local_changes(fsl_cx * f, fsl_id_t vid, bool doScan){
  int rc = 0;
  if(doScan) rc = fsl_ckout_changes_scan(f);
  if(!rc){
    int counter = 0;
    rc = fsl_ckout_changes_visit(f, vid, 0,
                                    fsl_ckout_changes_fapp, &counter);
    if(!rc){
      f_out("%s\n", counter ? "" : "\nNo local changes.");
    }
  }
  return rc;
}

int main(int argc, char const * const * argv ){
  int rc = 0;
  fsl_cx * f = 0;
  fsl_db * db = 0;
  fsl_id_t rid = 0;
  bool skipFiles = false;
  bool useUtc = false;
  const char * zVersion = NULL;
  fsl_id_t ckoutRid = 0;
  bool skipVfileReset = false;
  fcli_cliflag FCliFlags[] = {
    FCLI_FLAG_BOOL("f","no-files",&skipFiles,
              "Disables the status check for local files."),
    FCLI_FLAG_BOOL(0,"utc",&useUtc,
                   "Enables UTC timestamps (default is local time)."),
    FCLI_FLAG("v","version","version", &zVersion,
              "Shows info for the given version instead of the checkout."),
    FCLI_FLAG_BOOL(0,"no-reset", &skipVfileReset,
                   "Is an internal debugging flag which tells this app "
                   "not to re-set the vfile contents to be those of the current checkout. "
                   "For compatibility with fossil(1), do not use it. It is only for testing."),
    fcli_cliflag_empty_m
  };
  fcli_help_info FCliHelp = {
  "Outputs status info for the current checkout.",
  "",
  NULL
  };

  rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  f = fcli_cx();
  db = fsl_needs_ckout(f);
  if(!db){
    rc = FSL_RC_NOT_A_CKOUT;
    goto end;
  }

  if(fcli_has_unused_args(false)) goto end;

  rc = fcli_fingerprint_check(true);
  if(rc) goto end;

  if(zVersion){
    rc = fsl_sym_to_rid(f, zVersion, FSL_SATYPE_CHECKIN, &rid);
    if(rc) goto end;
  }
  if(!rid){
    fsl_ckout_version_info(f, &ckoutRid, NULL);
    rid = ckoutRid;
  }
  if( !fsl_rid_is_a_checkin(f, rid) ) {
    rc = fcli_err_set(FSL_RC_RANGE,
                      "%s does not refer to a checkin version.",
                      zVersion ? zVersion : "RID");
    goto end;
  }

  rc = fcli_ckout_show_info(useUtc);
  if(!rc && !skipFiles){
    rc = fapp_show_local_changes(f, rid, !skipVfileReset);
  }
  end:
  return fcli_end_of_main(rc);
}
