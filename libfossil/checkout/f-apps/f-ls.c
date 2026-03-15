/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).

  *****************************************************************************
  This file implements a basic 'ls' for in-repo content.
*/

#include "libfossil.h"

static struct LsApp {
  const char * glob;
  fsl_list globs;
  bool invertGlob;
} LsApp = {
  NULL,
  fsl_list_empty_m,
  false/*invertGlob*/
};


static char ls_matches(char const * name){
  if(!LsApp.globs.used) return 1;
  else{
    char const rc = fsl_glob_list_matches(&LsApp.globs, name) ? 1 : 0;
    return LsApp.invertGlob ? !rc : rc;
  }
}
/**
   A fsl_card_F_visitor_f() implementation which outputs
   state from fc to the fossil output channel.
*/
static int ls_F_card_v(fsl_card_F const * fc, void * state fsl__unused){
  char show;
  (void)state;
  if(!fc->uuid) return 0 /* was removed in this manifest */;
  show = ls_matches(fc->name);
  if(show){
    char perm;
    if(FSL_FILE_PERM_EXE == fc->perm) perm = 'x';
    else if(FSL_FILE_PERM_LINK == fc->perm) perm = 'L';
    else perm = '-';
    if(fcli_is_verbose()){
      f_out("%-8"FSL_ID_T_PFMT,
            fsl_uuid_to_rid(fcli_cx(), fc->uuid));
    }
    f_out("%.*s %c %s\n", 12, fc->uuid, perm, fc->name);
  }
  return 0;
}

int main(int argc, char const * const * argv ){
  int rc = 0;
  const char * lsVersion = NULL;
  bool showCheckouts = false;
  fsl_buffer buf = fsl_buffer_empty;
  fsl_cx * f;
  fsl_deck deck = fsl_deck_empty;
  fsl_deck * d = &deck;
  fcli_cliflag FCliFlags[] = {
    FCLI_FLAG("v","version","version",&lsVersion,
              "The version to list. Default is 'current' if there is a checkout, "
              "else 'trunk'."),
    FCLI_FLAG("g","glob","glob-list",&LsApp.glob,
              "List only filenames matching the given "
              "list of space-or-comma-separated glob patterns. All patterns must "
              "be provided as a single string argument, so be sure to quote them "
              "if your shell might resolve them as wildcards."),
    FCLI_FLAG_BOOL(0,"invert",&LsApp.invertGlob,
                   "Inverts the matching, such that only files not matching one of "
                   "the globs are listed."),
    FCLI_FLAG_BOOL(0,"checkouts", &showCheckouts,
                   "List all open checkouts to this repo instead of its files. "
                   "Note that checkouts opened via different paths (via symlinks) "
                   "are not detected here."),
    fcli_cliflag_empty_m
  };
  fcli_help_info FCliHelp = {
    "Lists files in a fossil repository.", NULL, NULL
  };
  rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;

  /* Set up/validate args... */
  f = fcli_cx();
  if(!fsl_cx_db_repo(f)){
    rc = fcli_err_set(FSL_RC_NOT_A_REPO,
                      "Requires a repository db. See --help.");
    goto end;
  }
  if(LsApp.glob){
    fsl_glob_list_parse(&LsApp.globs, LsApp.glob);
  }

  LsApp.invertGlob = fcli_flag2("v","invert", NULL);
  if(fcli_has_unused_flags(0)) goto end;

  if( showCheckouts ){
    int counter = 0;
    const char * zRepo = fsl_cx_db_file_repo(f, NULL);
    rc = fsl_config_open( f, NULL );
    if( rc ) goto end;
    rc = fsl_file_canonical_name( zRepo, &buf, false );
    if( rc ) goto end;
    FCLI_VN(1,("Repo filename: %s\n", zRepo));
    FCLI_VN(1,("Absolute repo filename: %b\n", &buf));
    fsl_db * const dbCfg = fsl_cx_db_config(f);
    assert( dbCfg );
    FCLI_VN(1,("Config db: %s\n", dbCfg->filename));
    fsl_stmt q = fsl_stmt_empty;
    rc = fsl_db_prepare(dbCfg, &q,
                        "SELECT substr(name,7) FROM global_config "
                        "WHERE value=%B ORDER BY value", &buf);
    if(rc) goto end_show;
    /* Potential TODO: iterate over all global_config.value entries
       matching 'ckout:%', canonicalize their names to resolve
       symlinks (do we have code for that? i don't think so.), and
       also show matches which open the repo via another (symlinked)
       path. */
    while( FSL_RC_STEP_ROW==(rc = fsl_stmt_step(&q)) ){
      f_out("%s\n", fsl_stmt_g_text(&q, 0, NULL));
      ++counter;
    }
    if( FSL_RC_STEP_DONE==rc ){
      rc = 0;
      if( 0==counter ){
        f_out("No checkouts found.\n");
      }
    }
  end_show:
    if( rc ){
      assert( dbCfg );
      fsl_cx_uplift_db_error(f, dbCfg);
    }
    fsl_stmt_finalize(&q);
  }else{ /* List files... */
    if(!lsVersion){
      lsVersion = fsl_cx_db_ckout(f) ? "current" : "trunk";
    }
    rc = fsl_deck_load_sym(f, d, lsVersion, FSL_SATYPE_CHECKIN);
    if(rc) goto end;
    f_out("File list from manifest version '%s' [%.*z] "
          "(RID %"FSL_ID_T_PFMT")...\n",
          lsVersion, 12, fsl_rid_to_uuid(f, d->rid),
          d->rid);
    if(d->B.uuid){
      f_out("This is a delta manifest from baseline [%.*s].\n",
            12, d->B.uuid);
    }
    if(fcli_is_verbose()) f_out("RID     ");
    f_out("%-12s P Name\n", "UUID");
    rc = fsl_deck_F_foreach(d, ls_F_card_v, NULL);
  }
  end:
  fsl_buffer_clear(&buf);
  fsl_glob_list_clear(&LsApp.globs);
  fsl_deck_finalize(d);
  rc = fcli_end_of_main(rc);
  return rc;
}
