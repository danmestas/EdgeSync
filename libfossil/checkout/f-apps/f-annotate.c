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
  fsl_annotate_opt opt;
} App = {
fsl_annotate_opt_empty_m
};

#if 0
static int fsl_annotate_stepper(void * state,
                                fsl_annotate_opt const * const * opt,
                                fsl_annotate_step const * const * step){
  static const int szHash = 10;
  fsl_outputer const * fout = (fsl_outputer*)state;
  int rc = 0;
  char ymd[24];
  if(step->mtime>0){
    fsl_julian_to_iso8601(step->mtime, &ymd[0], false);
    ymd[10] = 0;
  }
  switch(step->stepType){
    case FSL_ANNOTATE_STEP_VERSION:
      rc = fsl_appendf(fout->out, fout->state,
                       "version %3d: %s %.*s file %.*s\n",
                       step->stepNumber+1, ymd, szHash,
                       step->versionHash, szHash, step->fileHash);
      break;
    case FSL_ANNOTATE_STEP_FULL:
      if(opt->praise){
        rc = fsl_appendf(fout->out, fout->state,
                         "%.*s %s %13.13s: %.*s\n",
                         szHash,
                         opt->fileVersions ? step->fileHash : step->versionHash,
                         ymd, step->username,
                         (int)step->lineLength, step->line);
      }else{
        rc = fsl_appendf(fout->out, fout->state,
                         "%.*s %s %5d: %.*s\n",
                         szHash, opt->fileVersions ? step->fileHash : step->versionHash,
                         ymd, step->lineNumber,
                         (int)step->lineLength, step->line);
      }
      break;
    case FSL_ANNOTATE_STEP_LIMITED:
      if(opt->praise){
        rc = fsl_appendf(fout->out, fout->state,
                         "%*s %.*s\n", szHash+26, "",
                         (int)step->lineLength, step->line);
      }else{
        rc = fsl_appendf(fout->out, fout->state,
                         "%*s %5" PRIu32 ": %.*s\n",
                         szHash+11, "", step->lineNumber,
                         (int)step->lineLength, step->line);
      }
      break;
  }
  return rc;
}
#endif

int main(int argc, const char * const * argv ){
  bool ignoreAllSpace = false;
  bool ignoreEOLSpace = false;
  char const * zRevision = 0;
  char const * zOrigin = 0;
  fsl_buffer fnamebuf = fsl_buffer_empty;

  /**
     Set up flag handling, which is used for processing
     basic CLI flags and generating --help text output.
  */
  const fcli_cliflag FCliFlags[] = {
    // FCLI_FLAG_xxx macros are convenience forms for initializing
    // these members...
    FCLI_FLAG_BOOL("p","praise",&App.opt.praise,
                   "Use praise/blame mode."),
    FCLI_FLAG_BOOL("f","file-versions",&App.opt.fileVersions,
                   "List file blob versions instead of checkin versions."),
    FCLI_FLAG_BOOL("w","ignore-all-space",&ignoreAllSpace,
                   "Ignore all whitespace changes."),
    FCLI_FLAG_BOOL("Z","ignore-trailing-space",&ignoreEOLSpace,
                   "Ignore end-of-line whitespace."),
    FCLI_FLAG("v","version", "string",&zRevision,
              "Checkin containing the input file (default=checkout)."),
    FCLI_FLAG("o","origin", "string", &zOrigin,
              "Origin checkin version (default root of "
              "the tree)."),
    FCLI_FLAG("f","file", "filename",&App.opt.filename,
              "Repo-relative file to annote. May be provided as the "
              "first non-flag argument."),
    FCLI_FLAG_I32("n","limit", "int>=0",(int32_t*)&App.opt.limitVersions,
                  "Limit the number of historical versions to check. "
                  "0=no limit."),
    FCLI_FLAG_I32(NULL,"ms", "int>=0",(int32_t*)&App.opt.limitMs,
                  "Limit annotation to approx. this number of "
                  "milliseconds of work. 0=no limit."),
    FCLI_FLAG_BOOL(NULL,"versions", &App.opt.dumpVersions,
                   "Prefix output with a list of all analyzed versions."),
    fcli_cliflag_empty_m // list MUST end with this (or equivalent)
  };
  const fcli_help_info FCliHelp = {
    "Outputs an annotated listing of a file's contents.",
    "FILENAME",
    NULL // optional callback which outputs app-specific help
  };

  int rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;

  fsl_cx * const f = fcli_cx();

  if(!App.opt.filename){
    App.opt.filename = fcli_next_arg(true);
    if(!App.opt.filename){
      rc = fcli_err_set(FSL_RC_MISUSE,
                        "Missing required filename argument. Try --help.");
      goto end;
    }
  }
  if(fsl_cx_db_ckout(f)
     && 0==fsl_stat(App.opt.filename, NULL, false)){
    // We're in a checkout and the file exists. Canonicalize
    // the filename relative to the repo root...
    rc = fsl_ckout_filename_check(f, true, App.opt.filename,
                                  &fnamebuf);
    if(rc) goto end;
    App.opt.filename = fsl_buffer_cstr(&fnamebuf);
  }

  if(zRevision){
    rc = fsl_sym_to_rid(f, zRevision, FSL_SATYPE_CHECKIN,
                        &App.opt.versionRid);
    if(rc) goto end;
  }else{
    fsl_ckout_version_info(f, &App.opt.versionRid, &zRevision);
    if(!zRevision){
      rc = fcli_err_set(FSL_RC_MISUSE,
                        "Cannot determine --revision value.");
      goto end;
    }
  }
  if(zOrigin){
    rc = fsl_sym_to_rid(f, zOrigin, FSL_SATYPE_CHECKIN,
                        &App.opt.originRid);
    if(rc) goto end;
  }
  if((rc=fcli_has_unused_args(false))) goto end;
  if(ignoreAllSpace) App.opt.spacePolicy = 1;
  else if(ignoreEOLSpace) App.opt.spacePolicy = -1;
  fsl_outputer myOut = fsl_outputer_FILE;
  myOut.state = stdout;
  App.opt.out = fsl_annotate_step_f_fossilesque;
  App.opt.outState = &myOut;
  rc = fsl_annotate(f, &App.opt);

  end:
  fsl_buffer_clear(&fnamebuf);
  return fcli_end_of_main(rc)
    /* Will report any pending error state and return either
       EXIT_SUCCESS or EXIT_FAILURE. */;
}
