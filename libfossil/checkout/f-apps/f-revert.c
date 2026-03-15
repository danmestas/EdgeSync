/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code
*/
/**
   This app implements a feature similar to (fossil revert).
*/
#include "libfossil.h"

/**
   Callback for use with fsl_ckout_revert().
*/
static int fsl_ckout_revert_f_mine( char const *zFilename,
                                    fsl_ckout_revert_e changeType,
                                    void * callbackState ){
  char const * type = 0;
  switch(changeType){
    case FSL_REVERT_UNMANAGE: type = "UNMANAGE"; break;
    case FSL_REVERT_REMOVE: type = "-REMOVE"; break;
    case FSL_REVERT_CONTENTS: type = "REVERT"; break;
    case FSL_REVERT_RENAME: type = "-RENAME"; break;
    case FSL_REVERT_PERMISSIONS: type = "PERMS"; break;
    default:
      return fcli_err_set(FSL_RC_ERROR, "Invalid FSL_REVERT_xxx value: %d",
                          changeType);
  }
  ++(*((unsigned int*)callbackState));
  f_out("%-12s %s\n", type, zFilename);
  return 0;
}

int main(int argc, const char * const * argv ){
  fsl_id_bag vfileIds = fsl_id_bag_empty;
  unsigned int counter = 0;
  fsl_cx * f = 0;
  const fcli_cliflag FCliFlags[] = {
    fcli_cliflag_empty_m // list MUST end with this (or equivalent)
  };
  const fcli_help_info FCliHelp = {
    "Reverts local changes to the current checked-out version.",
    "file-or-dir [...file-or-dir-N]",
    NULL
  };
  int rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  else if(!fcli_needs_ckout() || !fcli_needs_repo()) goto end;
  else if((rc=fcli_has_unused_flags(false))) goto end;
  else if(!fcli_next_arg(false)){
    f_out("To avoid potential data loss, this app requires at "
          "least one file or directory name to be provided. To "
          "revert everything under the current directory pass "
          "the name '.'.\n");
    rc = FSL_RC_MISUSE;
    goto end;
  }

  f = fcli_cx();
  rc = fsl_cx_txn_begin(f);
  if(rc) goto end;

  fsl_ckout_revert_opt opt = fsl_ckout_revert_opt_empty;
  rc = fsl_vfile_changes_scan(f, 0, 0);
  if(rc) goto end;
  opt.scanForChanges = false;
  opt.callback = fsl_ckout_revert_f_mine;
  opt.callbackState = &counter;
  opt.vfileIds = &vfileIds;
  for(char const * zName;
      0==rc && (zName = fcli_next_arg(true));
      ){
    fsl_id_bag_reuse(&vfileIds);
    rc = fsl_ckout_vfile_ids(fcli.f, 0, &vfileIds, zName, true, true);
    if(rc) break;
    else if(!fsl_id_bag_count(&vfileIds)){
      /* Ambiguous situation: we don't know whether the user passed
         in an unknown path or one which has no changes. To determine
         which it is, we have to try again and include unchanged
         files in the result... */
      rc = fsl_ckout_vfile_ids(f, 0, &vfileIds, zName, true, false);
      if(0==rc && !fsl_id_bag_count(&vfileIds)){
        rc = fcli_err_set(FSL_RC_UNKNOWN_RESOURCE,
                          "Unknown/unmanaged file: %s", zName);
      }
    }
    if(0==rc) rc = fsl_ckout_revert(f, &opt);
  }
  f_out("%u file(s) reverted.\n", counter);

  end:
  fsl_id_bag_clear(&vfileIds);
  if(f && fsl_cx_txn_level(f)){
    int const rc2 = fsl_cx_txn_end(f, !!rc);
    if(!rc) rc = rc2;
  }
  return fcli_end_of_main(rc);
}
