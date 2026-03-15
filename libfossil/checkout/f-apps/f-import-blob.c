/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code
*/
/**
   This is a quick hack to import raw blobs into a repository. Do not
   use this without understanding the implications of doing so.
*/
#undef NDEBUG
#include "libfossil.h"
#include <string.h>

static int app_stuff(fsl_cx * f){
  int rc = 0;
  char const * z;
  fsl_buffer b = fsl_buffer_empty;
  fsl_id_t rid = 0;
  while( (z = fcli_next_arg(true)) ){
    rc = fsl_buffer_fill_from_filename(&b, z);
    assert( 0==rc );
    rc = fsl__content_put(f, &b, &rid);
    assert( 0==rc );
    f_out("imported: #%d: %s\n", (int)rid, z);
  }
  fsl_buffer_clear(&b);
  return rc;
}

int main(int argc, const char * const * argv ){
  bool fDryRun = false;
  fsl_cx * f = 0;
  const fcli_cliflag FCliFlags[] = {
    FCLI_FLAG_BOOL("n","dry-run", &fDryRun,"Dry-run mode."),
    fcli_cliflag_empty_m
  };
  const fcli_help_info FCliHelp = {
    "Imports raw blobs into a fossil repository.",
    "file1 [...fileN]",
    NULL
  };
  int rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  f = fcli_cx();
  fsl_cx_txn_begin(f);
  if(!fsl_needs_ckout(fcli_cx())) goto end;
  rc = app_stuff(f);
  end:
  if(f && fsl_cx_txn_level(f)){
    if( fDryRun ){
      f_out("Dry-run mode: rolling back transaction.\n");
    }
    fsl_cx_txn_end(f, fDryRun || 0!=rc);
  }
  return fcli_end_of_main(rc);
}
