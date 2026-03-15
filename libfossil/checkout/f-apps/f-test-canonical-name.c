/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code
*/
/**
   This is test app for fsl_file_canonical_name().
*/
#include "libfossil.h"

int main(int argc, const char * const * argv ){
  fsl_buffer buf = fsl_buffer_empty;
  const fcli_cliflag FCliFlags[] = {
    fcli_cliflag_empty_m // list MUST end with this (or equivalent)
  };
  const fcli_help_info FCliHelp = {
    "Runs its arguments through fsl_file_canonical_name().",
    "filename1 [...filenameN]",
    NULL
  };
  fcli.config.checkoutDir = NULL; // disable automatic checkout-open

  int rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  else if((rc=fcli_has_unused_flags(false))) goto end;

  const char * zName;
  while( 0==rc && (zName = fcli_next_arg(true)) ){
    fsl_buffer_reuse(&buf);
    rc = fsl_file_canonical_name(zName, &buf, false);
    if( 0==rc ){
      f_out("%s ==> %b\n", zName, &buf);
    }
  }
  if( !buf.mem ){
    rc = fsl_cx_err_set(fcli_cx(), FSL_RC_MISUSE,
                        "Usage: %s filename1 [...filenameN]", argv[0]);
  }

  end:
  fsl_buffer_clear(&buf);
  return fcli_end_of_main(rc);
}
