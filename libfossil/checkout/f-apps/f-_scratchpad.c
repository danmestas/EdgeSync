/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code
*/
/**
   This is a "scratchpad" app for developing and testing out new code which
   doesn't otherwise have a proper home.
*/
/* Force assert() to always be in effect. */
#undef NDEBUG
#include "libfossil.h"
#include "fossil-scm/internal.h"
#include <string.h>

// Only for testing/debugging..
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

// Global app state.
struct App_ {
  bool flag1;
} App = {
false//flag1
};

static int app_stuff(void){
  int rc = 0;
  fsl_deck d = fsl_deck_empty;
  fsl_cx * const f = fcli_cx();
  rc = fsl_deck_load_sym(f, &d, "f2f1612a0ca08146", FSL_SATYPE_CHECKIN);
  assert(0==rc);
  assert(d.f == f);
  if(fsl_repo_forbids_delta_manifests(f)){
    rc = fsl_deck_save(&d, false);
    assert(FSL_RC_ACCESS==rc && "Cannot save delta manifests.");
    f_out("Confirmed that we cannot save a delta in this repo.\n");
    fcli_err_reset();
  }

  assert(d.B.uuid && "We know this to be a delta manifest with 1 F-card.");
  assert(1==d.F.used);
  rc = fsl_deck_derive(&d);
  assert(0==rc);
  assert(f == d.f);
  assert(!d.B.uuid);
  assert(!d.B.baseline);
  assert(292 == d.F.used);
  for( uint32_t i = 0; i < d.F.used; ++i ){
    assert(d.F.list[i].uuid);
    assert(d.F.list[i].name);
    assert(!d.F.list[i].priorName);
  }
  MARKER(("Derived deck has %d F-cards\n", (int)d.F.used));
  fsl_deck_finalize(&d);
  return rc;
}

int main(int argc, const char * const * argv ){
  fsl_cx * f = 0;
  /**
     Set up flag handling, which is used for processing
     basic CLI flags and generating --help text output.
  */
  const fcli_cliflag FCliFlags[] = {
    fcli_cliflag_empty_m // list MUST end with this (or equivalent)
  };
  const fcli_help_info FCliHelp = {
    "A scratchpad app for developing new code.",
    NULL, // very brief usage text, e.g. "file1 [...fileN]"
    NULL // optional callback which outputs app-specific help
  };
  //fcli.config.checkoutDir = NULL; // same effect as global -C flag.
  //Invoke this app with -? -? to see the global options.
  int rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  f = fcli_cx();
  fsl_cx_txn_begin(f);
  if((rc=fcli_has_unused_args(false))) goto end;
  else if(!fsl_needs_ckout(fcli_cx())) goto end;
  rc = app_stuff();
  end:
  if(f && fsl_cx_txn_level(f)){
    fsl_cx_txn_end(f, true);
  }
  return fcli_end_of_main(rc);
}
