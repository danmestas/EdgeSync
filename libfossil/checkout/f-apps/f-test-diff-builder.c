/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code
*/
/**
   This is a template application for libfossil fcli client apps, with
   commentary explaining how to do various common things with the
   API. Copy/paste this and modify it to suit.
*/
#include "libfossil.h"

// Only for testing/debugging..
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

// Global app state.
struct App_ {
  char const * file1;
  char const * file2;
  char const * builderName;
  uint32_t diffFlags;
  int contextLines;
  bool ansiColor;
} App = {
NULL,//file1
NULL,//file2
"unified",//builderName
0/*diffFlags*/,
-1/*contextLines*/,
false/*ansiColor*/
};

static int fdb__create(fsl_dibu **b){
  fsl_dibu_e t = FSL_DIBU_DEBUG;
  switch(*App.builderName){
    case (int)'d':
      break;
    case (int)'j': t = FSL_DIBU_JSON1;
      break;
    case (int)'t': t = FSL_DIBU_TCL;
      break;
    case (int)'u': t = FSL_DIBU_UNIFIED_TEXT;
      break;
    case (int)'s': t = FSL_DIBU_SPLIT_TEXT;
      break;
    default:
      return fcli_err_set(FSL_RC_MISUSE,
                          "Unknown diff builder name: %s",
                          App.builderName);
      break;
  }
  return fsl_dibu_factory(t, b);
}

static int app_stuff(void){
  fsl_buffer f1 = fsl_buffer_empty;
  fsl_buffer f2 = fsl_buffer_empty;
  fsl_buffer hash1 = fsl_buffer_empty;
  fsl_buffer hash2 = fsl_buffer_empty;
  fsl_dibu * b = 0;
  fsl_dibu_opt opt = fsl_dibu_opt_empty;
  int rc = 0;
  int * rawDiff = 0;
#define RC if(rc) goto end
  if('r'!=App.builderName[0]/*raw diff*/){
    rc = fdb__create(&b);
    if(rc) goto end;
    assert(b);
    b->opt = &opt;
  }
  if(App.ansiColor && fsl_isatty(1)){
    fcli_diff_colors(&opt, FCLI_DIFF_COLORS_DEFAULT);
  }
  opt.diffFlags = App.diffFlags;
  opt.contextLines = App.contextLines;
  assert(0==rc);
  opt.out = fsl_output_f_FILE;
  opt.outState = stdout;
  RC;
  rc = fsl_buffer_fill_from_filename(&f1, App.file1);
  RC;
  rc = fsl_buffer_fill_from_filename(&f2, App.file2);
  RC;
  if(1){
    fsl_sha3sum_buffer(&f1, &hash1);
    fsl_sha3sum_buffer(&f2, &hash2);
    opt.hashLHS = fsl_buffer_cstr(&hash1);
    opt.hashRHS = fsl_buffer_cstr(&hash2);
    opt.nameLHS = App.file1;
    opt.nameRHS = App.file2;
  }
  rc = b
    ? fsl_diff_v2(&f1, &f2, b)
    : fsl_diff_v2_raw(&f1, &f2, &opt, &rawDiff);
  RC;
  if(rawDiff){
    f_out("Raw diff triples:\n");
    for(int i = 0; rawDiff[i] || rawDiff[i+1] || rawDiff[i+2]; i+=3){
      f_out(" copy %6d  delete %6d  insert %6d\n",
            rawDiff[i], rawDiff[i+1], rawDiff[i+2]);
    }
  }
  end:
  fsl_free(rawDiff);
  if(b) b->finalize(b);
  fsl_buffer_clear(&f1);
  fsl_buffer_clear(&f2);
  fsl_buffer_clear(&hash1);
  fsl_buffer_clear(&hash2);
  return rc;
#undef RC
}

static int fcli_flag_callback_f_diffflags(fcli_cliflag const *f){
  switch(*f->flagShort){
    case (int)'#': App.diffFlags |= FSL_DIFF2_LINE_NUMBERS;
      break;
    case (int)'v': App.diffFlags |= FSL_DIFF2_INVERT;
      break;
    default: break;
  }
  return 0;
}

int main(int argc, const char * const * argv ){
  bool bogus;
  const fcli_cliflag FCliFlags[] = {
    FCLI_FLAG("b","builer","string",&App.builderName,
              "Builder name. One of: "
              "u (unified), s (split), r (raw), "
              "d (debug), j (json), t (tcl)"),
    FCLI_FLAG_I32("F","diff-flags", "integer", &App.diffFlags,
                  "Bitmask of flags for builder's config."),
    FCLI_FLAG_I32("c","context", "integer", &App.contextLines,
                  "Number of context lines for the diff."),
    FCLI_FLAG_BOOL_X("#", "line-numbers", &bogus,
                     fcli_flag_callback_f_diffflags,
                     "Enable line numbers for diff formats "
                     "which optionally support it."),
    FCLI_FLAG_BOOL_X("v", "invert", &bogus,
                     fcli_flag_callback_f_diffflags,
                     "Invert the diff's LHS and RHS."),
    FCLI_FLAG_BOOL("a","ansi", &App.ansiColor,
                   "Enable ANSI color if possible."),
    fcli_cliflag_empty_m
  };
  const fcli_help_info FCliHelp = {
    "Test app for fsl_dibu.",
    "file1 file2",
    NULL
  };
  fcli.config.checkoutDir = NULL; // same effect as global -C flag.

  int rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;

  App.file1 = fcli_next_arg(true);
  App.file2 = fcli_next_arg(true);
  if(!App.file2){
    rc =fcli_err_set(FSL_RC_MISUSE,
                     "Expecting two filenames.");
    goto end;
  }
  if((rc=fcli_has_unused_args(false))) goto end;

  if(0>App.contextLines) App.contextLines = fsl_dibu_opt_empty.contextLines;
  else if(0==App.contextLines) App.diffFlags |= FSL_DIFF2_CONTEXT_ZERO;
  rc = app_stuff();

  end:
  return fcli_end_of_main(rc);
}
