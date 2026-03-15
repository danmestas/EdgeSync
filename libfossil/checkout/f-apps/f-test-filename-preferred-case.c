/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code
*/
/**
   This is a template application for libfossil fcli client apps, with
   commentary explaining how to do various common things with the
   API. Copy/paste this and modify it to suit.
*/
#include "libfossil.h"


int main(int argc, const char * const * argv ){
  bool fCaseSensitive = false;
  const fcli_cliflag FCliFlags[] = {
    FCLI_FLAG_BOOL("i","insenitive",&fCaseSensitive,
                   "Whether to assume case-sensitivity (default=false)."),
    fcli_cliflag_empty_m
  };
  const fcli_help_info FCliHelp = {
    "Tests the fsl_filename_preferred_case() function.",
    "root-dirname sub-path [...sub-path-N]",
    NULL // optional callback which outputs app-specific help
  };
  fcli.config.checkoutDir = NULL; // disable automatic checkout-open


  int rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;

  if((rc=fcli_has_unused_flags(false))) goto end;

  const char *zDir = fcli_next_arg(true);
  if( !zDir || !fcli_next_arg(false) ){
    rc = fcli_err_set(FSL_RC_MISUSE,"Usage: root-dirname sub-path [...sub-path-N].");
    goto end;
  }

  const char *zPath;
  while( (zPath = fcli_next_arg(true)) ){
    char * zOut = 0;
    rc = fsl_filename_preferred_case(fCaseSensitive, zDir, zPath, &zOut);
    if(rc) goto end;
    f_out("%s/%s ==> %s\n", zDir, zPath, zOut);
    fsl_free(zOut);
  }

  end:
  return fcli_end_of_main(rc);
}
