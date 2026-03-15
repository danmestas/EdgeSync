/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/
/*
  This file creates ZIP files from fossil repository content.
*/

#include "libfossil.h"

static int vbose = 0;

static int fsl_card_F_visitor_progress(fsl_card_F const * fc,
                                  void * state){
  ++(*((int*)state));
  if(vbose>1){
    f_out("Adding: %.12s %s\n", fc->uuid, fc->name);
  }
  return 0;
}

int main(int argc, char const * const * argv ){
  int rc = 0;
  const char * sym = NULL;
  const char * fileName = NULL;
  const char * rootDir = NULL;
  fsl_cx * f;
  fsl_db * db;
  bool noRoot = false;
  fsl_uuid_str uuid = NULL;
  int fileCounter = 0;
  bool fQuiet = false;
  fcli_cliflag FCliFlags[] = {
    FCLI_FLAG_BOOL("q","quiet",&fQuiet,
                "Suppress non-error output."),
    FCLI_FLAG("r","root","dir-name",&rootDir,
              "Use the given name for the zip file's top-most "
              "directory. The default is based on the project's name "
              "and exported version."),
    FCLI_FLAG_BOOL("V", "verbose", NULL,
                   "List each file as it's added. Trumped by --quiet."),
    fcli_cliflag_empty_m
  };
  fcli_help_info FCliHelp = {
    "Creates ZIP files from fossil repository checkins.",
    "CHECKIN_VERSION OUTPUT_FILE",
    NULL
  };

  rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  noRoot = fcli_flag("no-root",NULL) /* undocumented flag, for testing only.*/;
  f = fcli_cx();
  db = fsl_needs_repo(f);
  if(!db){
    rc = FSL_RC_NOT_A_REPO;
    goto end;
  }
  vbose = fcli_is_verbose();
  if(!vbose) vbose = fQuiet ? 0 : 1;
  else vbose = fQuiet ? 0 : 2;
  if(fcli_has_unused_flags(0)) goto end;

  sym = fcli_next_arg(1);
  fileName = fcli_next_arg(1);
  if(!sym || !*sym || !fileName || !*fileName){
    rc = fcli_err_set(FSL_RC_MISUSE,
                      "Usage: %s [options] CHECKIN_VERSION OUTPUT_FILE"
                      "\nOr try --help.",
                      fcli.appName);
    goto end;
  }

  rc = fsl_sym_to_uuid(f, sym, FSL_SATYPE_CHECKIN, &uuid, NULL);
  if(rc) goto end;

  if(!noRoot && !rootDir){
    char * pname;
    char * pz;
    pz = pname = fsl_config_get_text(f, FSL_CONFDB_REPO,
                                     "project-name", NULL);
    if(!pz){
      pz = pname = fsl_mprintf("Unnamed Project");
    }
    /* Translate unusual characters to underscores... */
    for( ; *pz; ++pz ){
      if('_'!=*pz && '-'!=*pz
         && !fsl_isalnum(*pz)){
        *pz = '_';
      }
    }
    pz = fsl_mprintf("%s-%.12s", pname, uuid);
    fcli_fax(pz);
    rootDir = pz;
    fsl_free(pname);
  }

  if(vbose){
    f_out("Extracting repository version %.12s to file %s...\n",
          uuid, fileName);
  }
  rc = fsl_repo_zip_sym_to_filename(f, sym, noRoot ? NULL : rootDir,
                                    fileName,
                                    vbose ? fsl_card_F_visitor_progress : NULL,
                                    &fileCounter);
  if(vbose && !rc){
    f_out("%d file(s) zipped to %s\n", fileCounter, fileName);
  }
  end:
  fsl_free(uuid);
  return fcli_end_of_main(rc);
}
