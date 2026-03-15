/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).

  *****************************************************************************
  This file holds test code for EVENT control artifacts.
*/

#include "libfossil.h"

static struct App_ {
  bool crossLink;
  bool wetRun;
} App = {
0/*crossLink*/,
0/*wetRun*/
};

/* static */
int test_event_0(void){
  fsl_cx * f = fcli_cx();
  fsl_db * db = fsl_cx_db_repo(f);
  fsl_deck DECK = fsl_deck_empty;
  fsl_deck * d = &DECK;
  fsl_buffer dout = fsl_buffer_empty;
  int rc;
  double now = 0
    ? 2456525.3001276273 /* 2013-08-20T19:12:11.027 */
    : fsl_db_julian_now(db);
  if(!db){
    return fsl_cx_err_set(f, FSL_RC_MISUSE, "This app requires a repo.");
  }
  FCLI_V(("now=%"FSL_JULIAN_T_PFMT"\n", now));
  fsl_deck_init(f, d, FSL_SATYPE_EVENT);
  assert(f==d->f);
  assert(FSL_SATYPE_EVENT==d->type);
  assert(NULL==d->allocStamp);

  rc = fsl_deck_C_set(d, "Test event - automatically generated", -1);
  assert(!rc);

  rc = fsl_deck_D_set( d, now );
  assert(!rc);

#if 0
  {
    char * eventId = fsl_db_random_hex(db, FSL_UUID_STRLEN);
    assert(fsl_is_uuid(eventId));
    rc = fsl_deck_E_set( d, now, eventId );
    fsl_free(eventId);
  }
#else
  rc = fsl_deck_E_set( d, now,
                       "b82b583b2cf60075c99e2ee5accec41906d3e6a2");
#endif
  assert(!rc);

  rc = fsl_deck_T_add( d, FSL_TAGTYPE_ADD, NULL, "automated", NULL);
  assert(!rc);


  {
    char * u = fsl_user_name_guess();
    assert(u);
    rc = fsl_deck_U_set(d, u);
    fsl_free(u);
    assert(!rc);
  }


  rc = fsl_deck_W_set( d, "Test event content.", -1 );
  assert(!rc);


  /*
    Note to readers: we could simplify the following greatly by using
    fsl_deck_save() here, but this code was largely written before
    fsl_deck_save() existed.
  */
  rc = fsl_deck_output(d, fsl_output_f_buffer, &dout);
  fcli_err_report(1);
  assert(!rc);
  f_out("%b", &dout);

  if(App.crossLink){
    /* Write it! */
    fsl_db_txn_begin(db);
    rc = fsl_deck_save(d, false);
    assert(!rc);
    assert(d->rid>0);
    fcli_err_report(1);
    FCLI_V(("Event content RID: %"FSL_ID_T_PFMT"\n", d->rid));
    if(!App.wetRun){
      FCLI_V(("dry-run mode: rolling back transaction.\n"));
    }
    fsl_db_txn_end(db, rc || !App.wetRun);
  }

  fsl_buffer_clear(&dout);
  fsl_deck_finalize(d);
  return rc;
}

int main(int argc, char const * const * argv ){
  int rc = 0;
  fcli_cliflag FCliFlags[] = {
    FCLI_FLAG_BOOL("w","wet-run",&App.wetRun,
                   "is the opposite of the default dry-run."),
    FCLI_FLAG_BOOL("c","crosslink",&App.crossLink,
                   "Crosslink the generated event."),
    fcli_cliflag_empty_m
  };
  fcli_help_info FCliHelp = {
  "Test app for EVENT manifests", NULL, NULL
  };
  rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  if(fcli_has_unused_flags(0)) goto end;
  rc = test_event_0();
  end:
  return fcli_err_report(0)
    ? EXIT_FAILURE : rc;
}
