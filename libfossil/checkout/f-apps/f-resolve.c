/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).

  *****************************************************************************
  A simple tool for resolving symbolic names to UUIDs and RIDs.
*/

#include "libfossil.h"

int main(int argc, char const * const * argv ){
  int rc = 0;
  const char * sym = NULL;
  fsl_cx * f;
  fsl_db * db;
  int count = 0;
  fsl_id_t rid;
  fsl_uuid_str uuid;
  bool startOfBranch = 0;
  fcli_cliflag FCliFlags[] = {
    FCLI_FLAG_BOOL("sb","start-of-branch",&startOfBranch,
                   "Expects the symbolic name to be a branch name "
                   "and resolves to the first checkin in that branch, rather than the "
                   "most recent. Results are unspecified if the branch name is used by "
                   "multiple branches."),
    fcli_cliflag_empty_m
  };
  fcli_help_info FCliHelp = {
    "Resolves symbolic version names to full-length hash IDs.",
    "symbol [...symbol]",
  NULL
  };
  rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  f = fcli_cx();
  db = fsl_cx_db_repo(f);
  if(!db){
    rc = fsl_cx_err_set(f, FSL_RC_MISUSE,
                        "This app requires a repository db.");
    goto end;
  }
  startOfBranch = fcli_flag2("sb", "start-of-branch",0);
  if(fcli_has_unused_flags(0)) goto end;

  while( (sym = fcli_next_arg(1)) ){
    uuid = NULL;
    rid = 0;
    ++count;
    rc = fsl_sym_to_uuid(f, sym,
                         startOfBranch ? FSL_SATYPE_BRANCH_START : FSL_SATYPE_ANY,
                         &uuid, &rid);
    switch(rc){
      case 0:
        f_out("%s %7"FSL_ID_T_PFMT" %s\n",
              uuid, rid, sym);
        fsl_free(uuid);
        break;
      case FSL_RC_AMBIGUOUS:
        fcli_list_ambiguous_artifacts(NULL,sym);
        break;
      default: break;
    }
    if(rc) goto end;
  }
  if(!count){
    rc = fcli_err_set(FSL_RC_MISUSE,
                      "No symbolic names were provided. See --help.");
  }
  end:
  return fcli_end_of_main(rc);
}
