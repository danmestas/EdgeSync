/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code
*/
/**
   This app is a quick hack for testing fsl_ckout_has_changes().
*/
#include "libfossil.h"


int main(int argc, const char * const * argv ){
  bool runScan = false;
  const fcli_cliflag FCliFlags[] = {
    FCLI_FLAG_BOOL("s","scan", &runScan,"Run vfile changes scan."),
    fcli_cliflag_empty_m // list MUST end with this (or equivalent)
  };
  const fcli_help_info FCliHelp = {
    "Tester for fsl_ckout_has_changes().", NULL, NULL
  };
  int rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  if((rc=fcli_has_unused_args(false))) goto end;

  fsl_cx * const f = fcli_cx();
  if(!fsl_needs_ckout(f)){
    goto end;
  }

  if( runScan ){
    rc = fsl_vfile_changes_scan(f, 0, 0);
    if(rc) goto end;
  }

  if( fsl_ckout_has_changes(f) ){
    f_out("Checkout has changes.\n");
  }else{
    f_out("Checkout has no changes.\n");
  }

  end:
  return fcli_end_of_main(rc);
}
