/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/
/**
   This is a test app for fsl_buffer_merge3().
*/
#include "libfossil.h"

// Only for testing/debugging..
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

static int do_merge(const char *zPivot,
                    const char *zV1,
                    const char *zV2,
                    FILE * out){
  fsl_buffer bP = fsl_buffer_empty;
  fsl_buffer b1 = fsl_buffer_empty;
  fsl_buffer b2 = fsl_buffer_empty;
  fsl_buffer bOut = fsl_buffer_empty;
  int rc = 0;
  unsigned int conflictCount = 0;

#define fill(B,N) rc = fsl_buffer_fill_from_filename(&B, N); \
  if(rc) { rc = fcli_err_set(rc, "Cannot read file: %s\n", N); goto end; }(void)0
  fill(bP, zPivot);
  fill(b1, zV1);
  fill(b2, zV2);
#undef fill
  //MARKER(("Files loaded. Merging...\n"));
  rc = fsl_buffer_merge3(&bP, &b1, &b2, &bOut, &conflictCount);
  if(rc) goto end;
  fwrite(bOut.mem, bOut.used, 1, out);
  if(conflictCount){
    f_out("WARNING: %u merge conflict(s)\n", conflictCount);
    assert(fsl_buffer_contains_merge_marker(&bOut));
  }else{
    assert(!fsl_buffer_contains_merge_marker(&bOut));
  }
  end:
  fsl_buffer_clear(&bP);
  fsl_buffer_clear(&b1);
  fsl_buffer_clear(&b2);
  fsl_buffer_clear(&bOut);
  //MARKER(("rc=%s\n", fsl_rc_cstr(rc)));
  return rc;
}

int main(int argc, const char * const * argv ){
  char const * zPivot;
  char const * zV1;
  char const * zV2;
  char const * zFOut = "-";
  const fcli_cliflag FCliFlags[] = {
    FCLI_FLAG("o","outfile","filename",&zFOut,
              "Output file. Defaults to stdout."),
    fcli_cliflag_empty_m
  };
  const fcli_help_info FCliHelp = {
    "A test app for fsl_buffer_merge3().",
    "pivotFile v1File v2File",
    NULL
  };
  fcli.config.checkoutDir = NULL;

  int rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;;

  zPivot = fcli_next_arg(true);
  zV1 = fcli_next_arg(true);
  zV2 = fcli_next_arg(true);
  if(!zV2){
    rc = fcli_err_set(FSL_RC_MISUSE,
                      "Requires 3 input files: pivot v1 v2");
    goto end;
  }

  if((rc=fcli_has_unused_args(false))) goto end;

  FILE * fOut = fsl_fopen(zFOut, "wb");
  if(!fOut){
    rc = fcli_err_set(FSL_RC_IO,"Cannot open output file: %s",
                      zFOut);
    goto end;
  }
  rc = do_merge(zPivot, zV1, zV2, fOut);
  fsl_fclose(fOut);
  end:
  return fcli_end_of_main(rc);
}
