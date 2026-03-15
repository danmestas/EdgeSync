/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).

  *****************************************************************************
  This file houses an application for creating Fossil-format deltas
  from, and applying them to, files.
*/

#include "libfossil.h" /* Fossil App mini-framework */
#include <string.h>

static void fcli_local_help(void){
  puts("Usages:\n");
  printf("\t%s file1 file2\n", fcli.appName);
  printf("\t%s -a|--apply delta_file fileVersion1\n\n", fcli.appName);

  puts("The first form outputs a fossil-format "
       "delta of two files.");
  puts("The second form applies a delta (the first file) "
       "to the second file.");
  puts("\nAll output goes to stdout.");
}

int main(int argc, char const * const * argv ){
  int rc = 0;
  const char * f1 = NULL;
  const char * f2 = NULL;
  bool doApply = false;
  fsl_error err = fsl_error_empty;
  fsl_buffer b1 = fsl_buffer_empty,
    b2 = fsl_buffer_empty,
    d12 = fsl_buffer_empty
    ;
  fcli_cliflag FCliFlags[] = {
    FCLI_FLAG_BOOL("a","apply",&doApply,
                   "Applies the first file as a delta to the second."),
    fcli_cliflag_empty_m
  };
  fcli_help_info FCliHelp = {
  "Creates or applies fossil-format deltas.",
  "file1 file2\nOR\n--apply delta-file source-file",
  fcli_local_help
  };
  rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;

  if(fcli_has_unused_flags(0)) goto end;
  f1 = fcli_next_arg(1);
  f2 = fcli_next_arg(1);
  if(!f2){
    rc = fcli_err_set(FSL_RC_MISUSE,
                      "Invalid arguments. Try --help.");
    goto end;
  }

  rc = fsl_buffer_fill_from_filename(&b1, f1);
  if(rc){
    rc = fcli_err_set(rc, "Could not open file: %s\n", f1);
    goto end;
  }
  rc = fsl_buffer_fill_from_filename(&b2, f2);
  if(rc){
    rc = fcli_err_set(rc, "Could not open file: %s\n", f2);
    goto end;
  }
  if(doApply){
    rc = fsl_buffer_delta_apply2(&b2, &b1, &b2, &err);
    if(err.code){
      f_out("DELTA ERROR: #%d (%s): %b\n", err.code,
            fsl_rc_cstr(err.code), &err.msg);
    }
    if(rc) goto end;
    f_out("%b", &b2);
  }else{
    rc = fsl_buffer_delta_create(&b1, &b2, &d12);
    if(rc) goto end;
    assert(!rc);
    if(1){
      /* extra verification */
      rc = fsl_buffer_delta_apply(&b1, &d12, &b1);
      assert(0==fsl_buffer_compare(&b1, &b2));
      if(rc) goto end;
    }
    f_out("%b\n", &d12);
  }

  end:
  fsl_error_clear(&err);
  fsl_buffer_clear(&b1);
  fsl_buffer_clear(&b2);
  fsl_buffer_clear(&d12);
  return fcli_end_of_main(rc);
}
