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
  This file implements a basic 'version diff' for in-repo or
  repo-vs-checkout content.
*/

#include "libfossil.h"

static int fsl_cidiff_f_mine(fsl_cidiff_state const *s){
  switch(s->stepType){
    case 0:
    case FSL_RC_STEP_DONE:
      assert(NULL==s->fc1);
      assert(NULL==s->fc2);
      return 0;
    default:
      assert(FSL_RC_STEP_ROW==s->stepType);
      break;
  }
  char label[5] = {' ',' ',' ',' ',0};
  if(0==s->changes){
    if(fcli_is_verbose()){
      f_out("[%s] %s\n", label, s->fc1->name);
    }
    return 0;
  }
  if(FSL_CIDIFF_FILE_ADDED & s->changes) label[0] = '+';
  else if(FSL_CIDIFF_FILE_REMOVED & s->changes) label[0] = '-';
  if(FSL_CIDIFF_FILE_MODIFIED & s->changes) label[1] = 'm';
  if(FSL_CIDIFF_FILE_RENAMED & s->changes) label[2] = 'r';
  if(FSL_CIDIFF_FILE_PERMS & s->changes) label[3] = 'p';

  f_out("[%s] %s\n", label, (s->fc2 ? s->fc2 : s->fc1)->name);
  return 0;
}


int main(int argc, char const * const * argv ){
  int rc = 0;
  const char * vFrom = NULL;
  const char * vTo = NULL;
  fsl_cx * f;
  fsl_id_t idFrom = -1, idTo = -1;

  fcli_cliflag cliFlags[] = {
    FCLI_FLAG("v1", "from", "version", &vFrom,
              "Version to diff from. May also be provided as "
              "the first non-flag argument. Default is 'prev'."),
    FCLI_FLAG("v2", "to", "version", &vTo,
              "Version to diff to. May also be provided as "
              "the second non-flag argument. Default is 'current'."),
    fcli_cliflag_empty_m
  };
  fcli_help_info FCliHelp = {
    "Lists the overall differences between any two checkins.",
    "[filenames or quoted globs...]",
    NULL/*callback*/
  };
  rc = fcli_setup_v2(argc, argv, cliFlags, &FCliHelp);
  if(rc) goto end;

  f = fcli_cx();
  if(!fsl_cx_db_repo(f)){
    rc = fcli_err_set(FSL_RC_NOT_A_REPO,
                      "Requires a repository db. See --help.");
    goto end;
  }

  if(fcli_has_unused_flags(0)) goto end;

  if(!vFrom && !(vFrom = fcli_next_arg(1))) vFrom = "prev";
  if(!vTo && !(vTo = fcli_next_arg(1))) vTo = "current";

  rc = fsl_sym_to_rid(f, vFrom, FSL_SATYPE_CHECKIN, &idFrom);
  if(!rc && idTo<0){
    rc = fsl_sym_to_rid(f, vTo, FSL_SATYPE_CHECKIN, &idTo);
  }
  if(rc) goto end;

  assert(idFrom>0);
  assert(idTo>0);
  f_out("Deck-level differences between\n#%-8d %s\n#%-8d %s\n",
        (int)idFrom, vFrom, (int)idTo, vTo);
  fsl_cidiff_opt copt = fsl_cidiff_opt_empty;
  copt.v1 = idFrom;
  copt.v2 = idTo;
  copt.callback = fsl_cidiff_f_mine;
  rc = fsl_cidiff( f, &copt );
  end:
  return fcli_end_of_main(rc);
}
