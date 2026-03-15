/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code
*/
/**
   f-extract can extract individual files from a fossil repository
   db.
*/
#include "libfossil.h"

// Only for testing/debugging..
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

// Global app state.
struct App_ {
  bool overwrite;
  bool dryRun;
  int verbose;
  char const * sym;
  char const * targetDir;
  fsl_list globs;
  fsl_list xfiles;
  fsl_list fnArgs;
  struct {
    fsl_buffer b;
    fsl_size_t tgtLen;
  } fn;
} App = {
false,//overwrite
false,//dryRun
0/*verbose*/,
"trunk",//sym
"./",//targetDir
fsl_list_empty_m/*globs*/,
fsl_list_empty_m/*xfiles*/,
fsl_list_empty_m/*fnArgs*/,
{/*fn*/fsl_buffer_empty_m, 0}
};


/**
   fsl_repo_extract_f() impl. for extracting specific files from a
   repo.
*/
static int fsl_repo_extract_f_one( fsl_repo_extract_state const * xs ){
  int rc = 0;
  fsl_int_t ndx;
  /**
     Potential FIXMEs: filename comparisons below against
     xs->fCard->name are case-sensitive, so do not honor the repo's
     case-insensitivity setting.
  */
  if(!xs->content){
    /* First pass: just collect the list of names we want to process so
       that we can do related error handling before writing any files. */
    bool fcheck = false;
    if(fsl_glob_list_matches(&App.globs, xs->fCard->name)){
      fsl_list_append(&App.xfiles, fsl_strdup(xs->fCard->name));
      fcheck = true;
    }
    ndx = fsl_list_index_of_cstr( &App.fnArgs, xs->fCard->name );
    if(ndx>=0){
      App.fnArgs.list[ndx] = NULL;
      fsl_list_append(&App.xfiles, fsl_strdup(xs->fCard->name));
      fcheck = true;
    }
    if(fcheck && !App.overwrite){
      /* Report overwrite violation in the first pass so that we can
         report this before writing any files. Yes, this introduces a
         race condition between this and the second pass, where the
         file might not exist when checked here but does in the second
         pass and gets blindly overwritten, but it's neglible and
         probably irrelevant.

         Doing this check in dry-run mode is probably arguable but if
         we don't then we have to do the check again in the second
         pass.
      */
      App.fn.b.used = App.fn.tgtLen;
      fsl_buffer_append(&App.fn.b, xs->fCard->name, -1);
      char const * abs = fsl_buffer_cstr(&App.fn.b);
      rc = fsl_stat(abs, NULL, false);
      if(rc) rc = 0;
      else{
        rc = fcli_err_set(FSL_RC_ALREADY_EXISTS,
                          "File already exists and --overwrite flag "
                          "not specified: %s", abs);
        goto end;
      }
    }
  }else if(0<=(ndx = fsl_list_index_of_cstr(&App.xfiles, xs->fCard->name))){
    /* Second pass: write out the file if it's in the App.xfiles list. */
    fsl_free(App.xfiles.list[ndx]);
    App.xfiles.list[ndx] = NULL;
    App.fn.b.used = App.fn.tgtLen;
    fsl_buffer_append(&App.fn.b, xs->fCard->name, -1);
    char const * abs = fsl_buffer_cstr(&App.fn.b);
    if(!App.dryRun){
      rc = fsl_mkdir_for_file(abs, false);
      if(rc){
        rc = fcli_err_set(rc, "Error creating directory for file: %s", abs);
        goto end;
      }
    }
    if(App.dryRun) f_out("%s\n", abs);
    else{
      if(App.verbose) f_out("%s", abs);
      rc = fsl_buffer_to_filename(xs->content, abs);
      if(App.verbose) f_out("\n");
      if(rc){
        rc = fcli_err_set(rc, "Could not write file: %s", abs);
        goto end;
      }
    }
  }
  end:
  return rc;
}

static int do_everything(void){
  int rc = 0;
  char const * fn;
  fsl_deck d = fsl_deck_empty;
  fsl_cx * const f = fcli_cx();
  fsl_repo_extract_opt xopt = fsl_repo_extract_opt_empty;

  rc = fsl_cx_txn_begin(f);
  if(rc) return rc;

  rc = fsl_sym_to_rid(f, App.sym, FSL_SATYPE_CHECKIN, &xopt.checkinRid);
  if(rc) goto end;

  // Collect list of non-glob names to extract...
  while( NULL != (fn = fcli_next_arg(true)) ){
    fsl_list_append(&App.fnArgs, (void *)fn);
  }
  if(!App.fnArgs.used){
    rc = fcli_err_set(FSL_RC_MISUSE,
                      "No files specified. "
                      "To extract everything, pass --glob '*'.");
    goto end;
  }

  { // Set up target dir.
    fsl_buffer_reserve(&App.fn.b, 1024);
    fsl_buffer_appendf(&App.fn.b, "%//", App.targetDir);
    App.fn.b.used =
      fsl_file_simplify_name(fsl_buffer_str(&App.fn.b),
                             (fsl_int_t)App.fn.b.used, true);
    App.fn.tgtLen = App.fn.b.used;
  }

  /* Extract repo, pass 1: collect list of matching names and
     validate that App.fnArgs entries refer to real things. */
  xopt.callback = fsl_repo_extract_f_one;
  xopt.extractContent = false/*for the first pass, no content*/;
  rc = fsl_repo_extract(f, &xopt);
  if(rc) goto end;

  // Report any App.fnArgs entries which do not match:
  for(fsl_size_t i = 0, x = 0; i < App.fnArgs.used; ++i ){
    fn = (char const *)App.fnArgs.list[i];
    if(fn){
      if(1==++x){
        f_out("Error: not found in repository:\n");
      }
      rc = fcli_err_set(FSL_RC_NOT_FOUND,
                        "%" FSL_SIZE_T_PFMT " %s not found "
                        "in repository.", x, x==1 ? "file" : "files");
      f_out("    %s\n", fn);
    }
  }
  if(rc) goto end;

  if(0){
    f_out("Files to extract:\n");
    for(fsl_size_t i = 0; i < App.xfiles.used; ++i ){
      fn = (char const *)App.xfiles.list[i];
      f_out("  %b%s\n", &App.fn.b, fn);
    }
  }

  if(!App.dryRun){
    // Create target dir, if needed...
    fsl_buffer_append(&App.fn.b,"x", 1)/*dummy filename*/;
    rc = fsl_mkdir_for_file(fsl_buffer_cstr(&App.fn.b), false);
    App.fn.b.used = App.fn.tgtLen;
    App.fn.b.mem[App.fn.tgtLen] = 0;
    if(rc){
      App.fn.b.mem[--App.fn.tgtLen] = 0/*strip trailing slash (cosmetic)*/;
      rc = fcli_err_set(rc, "Could not create directory: %b",
                        &App.fn.b);
      goto end;
    }
  }

  xopt.extractContent = true;
  rc = fsl_repo_extract(f, &xopt);
  if(rc) goto end;

  if(App.dryRun && App.verbose){
    f_out("** DRY-RUN MODE: nothing was actually extracted. **\n");
  }
  end:
  fsl_cx_txn_end(f, true);
  fsl_deck_finalize(&d);
  return rc;
}

static int fcli_flag_callback_f_glob(fcli_cliflag const *f){
  char const * v = *((char const **)f->flagValue);
  //f_out("GLOB: %s\n", v);
  return fsl_glob_list_parse(&App.globs, v);
}

int main(int argc, const char * const * argv ){
  char const * dummy/*needed for -g flag callback*/;
  const fcli_cliflag FCliFlags[] = {
    FCLI_FLAG_BOOL("w","overwrite",&App.overwrite,
                   "Overwrite target files which already exist "
                   "instead of triggering an error."),
    FCLI_FLAG_CSTR_X("g","glob","glob(s)", &dummy,
                     fcli_flag_callback_f_glob,
                     "Glob or comma-separated list of globs of "
                     "filenames to extract. May be used multiple "
                     "times. Be sure to quote them to protect them "
                     "from shell expansion."),
    FCLI_FLAG("v", "version", "version", &App.sym,
              "Version to extract (default=trunk)."),
    FCLI_FLAG("d", "directory", "directory", &App.targetDir,
              "Directory to extract to (default=./). "
              "Will be created if needed."),
    /* ^^^^ TODO: add some basic %X flags to that which expand to the
       project name and/or the version being extracted (either with
       the -r NAME value or the resulting expanded hash). */
    FCLI_FLAG_BOOL("n", "dry-run", &App.dryRun,
                   "Output what would be extracted but do not "
                   "create any directories nor write any files."),
    fcli_cliflag_empty_m // list MUST end with this (or equivalent)
  };
  const fcli_help_info FCliHelp = {
    "Extracts individual files from a repository database.",
    "file1 [...fileN]",
    NULL // optional callback which outputs app-specific help
  };
  fcli.config.checkoutDir =
    NULL/*suppress automatic opening of checkout*/;
  /**
     Using fsl_malloc() before calling fcli_setup() IS VERBOTEN. fcli
     swaps out the allocator with a fail-fast one, meaning that if an
     allocation fails, the app crashes. This frees up the client app
     from much of the tedium of dealing with allocation errors (which,
     in practice, "never happen").
  */
  int rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  else if(!fsl_needs_repo(fcli_cx())) goto end;
  else if((rc=fcli_has_unused_flags(false))) goto end;
  App.verbose = fcli_is_verbose();
  rc = do_everything();
  end:
  fsl_list_visit_free(&App.globs, true);
  fsl_list_visit_free(&App.xfiles, true);
  fsl_list_reserve(&App.fnArgs, 0);
  fsl_buffer_clear(&App.fn.b);
  return fcli_end_of_main(rc);
}
