/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code
*/
/**
   This is a test app demonstrating creating a repository and checking
   in files without an associated checkout.
*/
#ifdef NDEBUG
/* Force assert() to always be in effect. */
#undef NDEBUG
#endif
#include "libfossil.h"

// Only for testing/debugging..
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

// Global app state.
struct App_ {
  char const * repoDbName;
  bool addEmptyCommit;
  bool addRCard;
} App = {
"_ciwoco.f",
true/*addEmptyCommit*/,
true/*addRCard*/
};

static int repo_create(bool addEgg){
  int rc;
  fsl_cx * const f = fcli_cx();
  fsl_repo_create_opt cOpt = fsl_repo_create_opt_empty;
  if(addEgg){
    cOpt.commitMessage =
      "This is a repo. There are many like it "
      "but this one is mine.";
  }else{
    cOpt.commitMessage = NULL;
  }
  cOpt.filename = App.repoDbName;
  fsl_file_unlink(cOpt.filename);
  rc = fsl_repo_create(f, &cOpt);
  //fcli_err_report(false);
  assert(0==rc);
  assert(fsl_cx_db_repo(f));
  return rc;
}

static int setup_deck(fsl_deck *d, char const * msg){
  double const julian = true
    ? fsl_db_julian_now(fsl_cx_db_repo(d->f))
    /* ^^^^ ms precision */
    : fsl_julian_now() /* seconds precision */;
  int rc = fsl_deck_D_set(d, julian);
  assert(0==rc);
  rc = fsl_deck_C_set(d, msg, -1);
  assert(0==rc);
  rc = fsl_deck_U_set(d, "ciwoco");
  assert(0==rc);
  return rc;
}

static int do_demo(void){
  int rc = 0;
  fsl_cx * const f = fcli_cx();
  fsl_buffer content = fsl_buffer_empty;
  fsl_deck d = fsl_deck_empty;
  char const *fname = 0;
  rc = repo_create(App.addEmptyCommit);
  if(rc) goto end;
  f_out("(Re)created repo: %s\n", fsl_cx_db_file_repo(f, NULL));
  if(App.addEmptyCommit){
    f_out("Empty initial commit was added.\n");
  }else{
    f_out("No empty initial commit was created.\n");
  }
  assert(fsl_needs_repo(f));

  fsl_cx_flag_set(f, FSL_CX_F_CALC_R_CARD, App.addRCard);
  f_out("R-card generation is %s\n", App.addRCard ? "ON" : "OFF");

  rc = fsl_cx_txn_begin(f);
  if(rc) goto end;

  //////////////////////////////////////////////////////////////
  // Step 1: initialize our deck...
  if(App.addEmptyCommit){
    rc = fsl_deck_load_sym(f, &d, "trunk", FSL_SATYPE_CHECKIN);
    assert(0==rc);
    assert(f==d.f);
    f_out("Deriving from initial trunk checkin #%"FSL_ID_T_PFMT"\n", d.rid);
    rc = fsl_deck_derive(&d);
    assert(0==rc);
  }else{
    f_out("Creating initial commit with files.\n");
    fsl_deck_init(f, &d, FSL_SATYPE_CHECKIN);
    /* If we don't set a branch, we cannot resolve the checkins
       via a branch name! */
    rc = fsl_deck_branch_set(&d, "trunk");
    assert(0==rc);
  }

  //////////////////////////////////////////////////////////////
  // Step 2: set up some commonly-required cards...
  rc = setup_deck(&d, "Files added w/o checkout.");
  assert(0==rc);

  //////////////////////////////////////////////////////////////
  // Step 3: add some files...
  char const * fnames[] = {
    "f-apps/f-test-ciwoco.c",
    "Makefile",
    NULL
  };
  for( int i = 0; (fname = fnames[i]); ++i ){
    rc = fsl_buffer_fill_from_filename(&content, fname);
    assert(0==rc);
    rc = fsl_deck_F_set_content(&d, fname, &content,
                                FSL_FILE_PERM_REGULAR, NULL);
    assert(0==rc);
    f_out("Added file: %s\n", fname);
  }

  //////////////////////////////////////////////////////////////
  // Step 4: save the deck...
  rc = fsl_deck_save(&d, false);
  assert(0==rc);
  f_out("Saved checkin #%"FSL_ID_T_PFMT"\n", d.rid);

  //////////////////////////////////////////////////////////////
  // Step 5: ...
  f_out("Now we'll try again so we can ensure that deltaing "
        "of parent file content works.\n");
  fsl_deck_derive(&d);
  setup_deck(&d, "Modified Makefile.");
  fname = "Makefile";
  rc = fsl_buffer_fill_from_filename(&content, fname);
  assert(0==rc);
  rc = fsl_buffer_append(&content,
                         "\n# This is an edit. There are many "
                         "like it, but this one is mine.\n", -1);
  assert(0==rc);
  rc = fsl_deck_F_set_content(&d, fname, &content,
                              FSL_FILE_PERM_REGULAR, NULL);
  assert(0==rc);
  f_out("Added file: %s\n", fname);
  rc = fsl_deck_save(&d, false);
  assert(0==rc);
  f_out("Saved checkin #%"FSL_ID_T_PFMT"\n", d.rid);
  f_out("You can confirm that the previous file version is delta'd "
        "by running:\n    f-acat -R %s --raw rid:X\n"
        "where X is the lowest-numbered entry in this list:\n",
        App.repoDbName);
  fsl_db_each( fsl_cx_db(f), fsl_stmt_each_f_dump, f,
               "SELECT m.fid FROM mlink m, filename f "
               "WHERE m.fnid=f.fnid and f.name=%Q",
               fname);

  assert(fsl_cx_txn_level(f)>0);
  rc = fsl_cx_txn_end(f, false);
  end:
  if(fsl_cx_txn_level(f)){
    fsl_cx_txn_end(f, true);
  }
  fsl_deck_finalize(&d);
  fsl_buffer_clear(&content);
  if(!rc){
    f_out("Results are in repo file %s\n", App.repoDbName);
  }
  return rc;
}

int main(int argc, const char * const * argv ){
  /**
     Set up flag handling, which is used for processing
     basic CLI flags and generating --help text output.
  */
  const fcli_cliflag FCliFlags[] = {
    FCLI_FLAG_BOOL_INVERT("e", "empty", &App.addEmptyCommit,
                          "If set, do not create the initial "
                          "empty checkin."),
    FCLI_FLAG_BOOL_INVERT("r", "no-r-card", &App.addRCard,
                          "If set, do not add an R-card to commits."),
    fcli_cliflag_empty_m // list MUST end with this (or equivalent)
  };
  const fcli_help_info FCliHelp = {
    "A demo of creating a new repo and checking in files "
    "without a checkout.",
    NULL, // very brief usage text, e.g. "file1 [...fileN]"
    NULL // optional callback which outputs app-specific help
  };
  fcli.config.checkoutDir = NULL; // same effect as global -C flag.
  int rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  else if((rc=fcli_has_unused_args(false))) goto end;
  else if(fsl_cx_db_repo(fcli_cx())){
    rc = fcli_err_set(FSL_RC_MISUSE,
                      "This app must be started WITHOUT a repo/checkout.");
    goto end;
  }

  rc = do_demo();
  end:
  return fcli_end_of_main(rc);
}
