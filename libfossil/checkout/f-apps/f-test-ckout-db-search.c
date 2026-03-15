/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2024 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2024 The Libfossil Authors
  SPDX-FileType: Code
*/
#include "libfossil.h"
int main(int argc, const char * const * argv ){
  fsl_buffer buf = fsl_buffer_empty;
  bool checkParentDirs = false;
  char const * zDir = NULL;
  const fcli_cliflag FCliFlags[] = {
    FCLI_FLAG_BOOL("p","parent",&checkParentDirs,
                   "Check parent dirs if no db is found."),
    fcli_cliflag_empty_m
  };
  const fcli_help_info FCliHelp = {
    "Basic test app for fsl_ckout_db_search() and friends.",
    "dir-name", NULL
  };
  fcli.config.checkoutDir = NULL /* disable automatic checkout-open */;
  int rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  zDir = fcli_next_arg(true);
  if(!zDir){
    rc = fcli_err_set(FSL_RC_MISUSE,
                      "Missing required directory argument. Try --help.");
    goto end;
  }else if((rc=fcli_has_unused_args(false))){
    goto end;
  }

  rc = fsl_ckout_db_search( zDir, checkParentDirs, &buf );
  f_out("fsl_ckout_db_search(%s, %d) rc=%s buf=%b\n",
        zDir, (int)checkParentDirs, fsl_rc_cstr(rc), &buf);

  end:
  fsl_buffer_clear(&buf);
  return fcli_end_of_main(rc);
}
