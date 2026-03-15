/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).

  *****************************************************************************
   This file implements an "artifact diff" for in-repo content.
*/

#include "libfossil.h"
#include <stdlib.h> /* atoi() */

struct ADiffAppT {
  fsl_dibu_opt diffOpt;
  fsl_dibu * diffBuilder;
};
typedef struct ADiffAppT ADiffAppT;
ADiffAppT ADiffApp = {
fsl_dibu_opt_empty_m,
NULL
};

static int f_adiff(fsl_id_t v1, fsl_id_t v2){
  int rc = 0;
  fsl_cx * const f = fcli_cx();
  fsl_buffer lhs = fsl_buffer_empty;
  fsl_buffer rhs = fsl_buffer_empty;
  rc = fsl_content_get(f, v1, &lhs);
  if(0==rc) rc = fsl_content_get(f, v2, &rhs);
  if(0==rc) rc = fsl_diff_v2(&lhs, &rhs, ADiffApp.diffBuilder);
  fsl_buffer_clear(&lhs);
  fsl_buffer_clear(&rhs);
  return rc;
}

int main(int argc, char const * const * argv ){
  int rc = 0;
  const char * vFrom = NULL;
  const char * vTo = NULL;
  fsl_cx * f;
  fsl_id_t idFrom = 0, idTo = 0;
  bool flagBW = false;
  bool flagColor = false;
  bool flagInvert = false;
  bool flagLineNo = false;
  bool flagSbs = false;
  bool flagIgnoreSpaces = false;
  int flagSbsWidth = -1;
  int flagContextLines = -1;
  fcli_cliflag FCliFlags[] = {
    FCLI_FLAG("v1","from","from-version",&vFrom,
              "Name of version 1. Default is the first non-flag argument"),
    FCLI_FLAG("v2","to","to-version",&vTo,
              "Name of version 2. Default is the second non-flag argument"),
    FCLI_FLAG_BOOL("w","ignore-all-space",&flagIgnoreSpaces,
                   "Ignore all whitespace differences."),
    FCLI_FLAG_BOOL("y", "sbs", &flagSbs,
                   "Use side-by-side diff."),
    FCLI_FLAG_I32("W","sbs-width","max column width",&flagSbsWidth,
                  "Max side-by-side diff view width. Implies -y."),
    FCLI_FLAG_I32("c","context-lines","line-count",&flagContextLines,
                  "Number of lines of context."),
    FCLI_FLAG_BOOL("l","line-numbers",&flagLineNo,
                   "Add line numbers (unified diff only)."),
    FCLI_FLAG_BOOL("i","invert",&flagInvert,
                   "Invert the direction of the diff."),
    FCLI_FLAG_BOOL("bw","no-color",&flagBW,
                   "Disables ANSI color sequences."),
    FCLI_FLAG_BOOL(NULL,"color", &flagColor,
                   "Try to force ANSI color even if stdout is not "
                   "a terminal or --no-color is used. Does not work "
                   "with all diff formats."),
    fcli_cliflag_empty_m
  };
  fcli_help_info FCliHelp = {
  "Generates diff of individual blobs (not checkin versions!).",
  "fromArtifactUuid toArtifactUuid",
  NULL
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
  if(flagSbsWidth>0){
    ADiffApp.diffOpt.columnWidth = (unsigned short)flagSbsWidth;
    flagSbs = true;
  }
  rc = fsl_dibu_factory(flagSbs
                        ? FSL_DIBU_SPLIT_TEXT
                        : FSL_DIBU_UNIFIED_TEXT,
                        &ADiffApp.diffBuilder);
  if(rc) goto end;
  ADiffApp.diffBuilder->opt = &ADiffApp.diffOpt;
  ADiffApp.diffOpt.out = fsl_output_f_FILE;
  ADiffApp.diffOpt.outState = stdout;
  if(flagIgnoreSpaces){
    ADiffApp.diffOpt.diffFlags |= FSL_DIFF2_IGNORE_ALLWS;
  }
  if(flagInvert){
    ADiffApp.diffOpt.diffFlags |= FSL_DIFF2_INVERT;
  }
  if(flagLineNo){
    ADiffApp.diffOpt.diffFlags |= FSL_DIFF2_LINE_NUMBERS;
  }
  if(flagColor || (!flagBW && fsl_isatty(1))){
    fcli_diff_colors(&ADiffApp.diffOpt, FCLI_DIFF_COLORS_DEFAULT);
  }
  if(flagContextLines>=0){
    ADiffApp.diffOpt.contextLines = (unsigned short) flagContextLines;
  }
  if(fcli_has_unused_flags(0)) goto end;
  if(!vFrom) vFrom = fcli_next_arg(1);
  if(!vTo) vTo = fcli_next_arg(1);
  if(!vFrom || !vTo){
    rc = fcli_err_set(FSL_RC_MISUSE, "Both of -v1 UUID and -v2 UUID are required.");
    goto end;
  }

  rc = fsl_sym_to_rid(f, vFrom, FSL_SATYPE_ANY, &idFrom);
  if(!rc){
    rc = fsl_sym_to_rid(f, vTo, FSL_SATYPE_ANY, &idTo);
  }
  if(rc) goto end;
  ADiffApp.diffOpt.hashLHS = vFrom;
  ADiffApp.diffOpt.hashRHS = vTo; 

  rc = f_adiff( idFrom, idTo );

  end:
  if(ADiffApp.diffBuilder){
    ADiffApp.diffBuilder->finalize(ADiffApp.diffBuilder);
  }
  return fcli_end_of_main(rc);
}
