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

// Only for testing/debugging..
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

int main(int argc, const char * const * argv ){
  fsl_cx * f = 0;
  bool fromCwd = false;
  const fcli_cliflag FCliFlags[] = {
  FCLI_FLAG_BOOL(0,"cwd",&fromCwd,
                 "Treat filenames as relative to cwd "
                 "instead of checkout root. This is implicit if "
                 "no checkout is opened."),
    fcli_cliflag_empty_m // list MUST end with this (or equivalent)
  };
  const fcli_help_info FCliHelp = {
    "Test app for fsl_reserved_fn_check() and friends.",
    "filename1 [...filenameN]",
    NULL // optional callback which outputs app-specific help
  };
  //fcli.checkoutDir = NULL; // same effect as global -C flag.
  int rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;;
  if((rc=fcli_has_unused_flags(false))) goto end;
  if(!fcli_next_arg(false)){
    rc = fcli_err_set(FSL_RC_MISUSE,"Missing filename.");
    goto end;
  }
  f = fcli_cx();
  char const * fn;
  f_out("fromCwd = %d\n", fromCwd);
  while( (fn = fcli_next_arg(true)) ){
    f_out("Testing: %s\n", fn);
    rc = fsl_reserved_fn_check(f, fn, -1, fromCwd);
    f_out("\t%s\t", fsl_rc_cstr(rc));
    if(rc){
      char const * zErr = 0;
      fsl_cx_err_get(f, &zErr, NULL);
      f_out("%s", zErr);
    }
    f_out("\n");
    fsl_cx_err_reset(f);
    rc = 0;
  }
  end:
  return fcli_end_of_main(rc);
}
