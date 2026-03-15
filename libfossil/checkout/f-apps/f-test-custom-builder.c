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
  This file is a scratchpad for implementing a custom diff builder. It is
  not a complete app, nor does it necessarily work.
*/
#include "libfossil.h"
#include "fsl-ncurses.h"
#include <locale.h>

static void fcli_local_help(void){
  puts("If neither --v1 nor --v2 are supplied (nor implied!) then it behaves as if "
       "it were passed \"--v1=current --v2=.\", where '.' is a symbolic "
       "name for the local checkout.\n");

  puts("All non-flag parameters, after flag processing is finished, are treated "
       "as filenames/globs and diffs are restricted to files matching those "
       "names/globs.\n");
}

static struct VDiffApp {
  char const * glob;
  short contextLines;
  short sbsWidth;
  int diffFlags;
  bool brief;
  fsl_buffer fcontent1;
  fsl_buffer fcontent2;
  fsl_buffer fhash;
  fsl_list globs;
  fsl_dibu_opt diffOpt;
  fsl_dibu * diffBuilder;
} VDiffApp = {
NULL/*glob*/,
5/*contextLines*/,
0/*sbsWidth*/,
0/*diffFlags*/,
0/*brief*/,
fsl_buffer_empty_m/*fcontent1*/,
fsl_buffer_empty_m/*fcontent2*/,
fsl_buffer_empty_m/*fhash*/,
fsl_list_empty_m/*globs*/,
fsl_dibu_opt_empty_m/*diffOpt*/,
NULL/*diffBuilder*/
};

static int f_vdiff_hash(fsl_card_F const * const fc,
                        fsl_buffer const * const content,
                        fsl_buffer * const hash){
  int rc = 0;
  fsl_buffer_reuse(hash);
  if(fc->uuid){
    rc = fc->uuid[FSL_STRLEN_SHA1]
      ? fsl_sha3sum_buffer(content, hash)
      : fsl_sha1sum_buffer(content, hash);
  }
  return rc;
}
#define f__out if(NULL==stdscr) f_out

/**
   Check two F-cards for differences and render any they have. vid1
   and vid2 are the checkin versions from which the given cards are
   from. ONE of vid1 or vid2 may be 0, indicating the current
   checkout, in which case we compare the checked-out copy of that
   file. Returns 0 on success, non-0 on fatal error.
*/
int f_vdiff_files(fsl_cx * const f,
                  fsl_id_t vid1,
                  fsl_card_F const * fc1,
                  fsl_id_t vid2,
                  fsl_card_F const * fc2){
  int rc = 0;
  fsl_buffer * fContent1 = &VDiffApp.fcontent1;
  fsl_buffer * fContent2 = &VDiffApp.fcontent2;
  fsl_buffer * fLocalHash = &VDiffApp.fhash
    /* hash of the local checked-out file */;
  fsl_time_t rmtime = 0;
  fsl_time_t fmtime = 0;
  fsl_card_F const * fcHashCmp = NULL
    /* The card against which we will compare fLocalHash.  If
       fLocalHash is the hash of vid1 then this will be fc2, and vice
       versa. */;
  if(vid1>0 && vid2>0 && !fsl_uuidcmp(fc1->uuid, fc2->uuid)){
    /* No diffs to check */
    return 0;
  }

  /**
     TODO: optimization: use vfile where we can to reduce the
     set of files we scan. This can only work if one version
     is the checked-out version and one is the local checkout
     changes.
  */

  /* Else different content in each version OR we have a local file
     and need to load it to see if it's changed. */
  fLocalHash->used = fContent2->used = fContent1->used = 0;

  assert(vid1!=vid2);

  if(0==vid1){ /* vid1 is the current checkout version */
    assert(0 != vid2);
    rc = fsl_ckout_file_content(f, 0, fc1->name, fContent1);
    if(!rc){
      rc = f_vdiff_hash(fc1, fContent1, fLocalHash);
      if(!rc){
        fcHashCmp = fc2;
        rc = fsl_card_F_ckout_mtime(f, vid1, fc1, NULL, &fmtime);
      }
    }
  }else{
    rc = fsl_card_F_content(f, fc1, fContent1);
    if(!rc && (0==vid2)){
      /* Collect the repo-side mtime IF the other version==0. */
      rc = fsl_card_F_ckout_mtime(f, vid1, fc1, &rmtime, NULL);
    }
  }

  if(rc) return rc;

  /* Repeat for vid2. */
  if(0==vid2){ /* vid2 is the current checkout */
    assert(0 != vid1);
    rc = fsl_ckout_file_content(f, 0, fc2->name, fContent2);
    if(!rc){
      rc = f_vdiff_hash(fc2, fContent2, fLocalHash);
      if(!rc){
        fcHashCmp = fc1;
        rc = fsl_card_F_ckout_mtime(f, vid2, fc2, NULL, &fmtime);
      }
    }
  }else{
    rc = fsl_card_F_content(f, fc2, fContent2);
    if(!rc && (0==vid1)){
      /* Collect the repo-side mtime IF the other version==0. */
      rc = fsl_card_F_ckout_mtime(f, vid2, fc2, &rmtime, NULL);
    }
  }

  if(rc) return rc;
  else if(fcHashCmp
          && (0==fsl_uuidcmp(fsl_buffer_cstr(fLocalHash),
                             fcHashCmp->uuid))
          ){
    /* repo-side content is unchanged from local copy. */
    return 0;
  }else if((fmtime>0) && (fmtime==rmtime)){
    /* One of the above is a local file and rmtime holds the repo-side
       mtime of the other. Assume naively that same time==same
       content, as that will be the case more often then not.
    */
    return 0;
  }else{
    char const * zUuid1 = (0==vid1) ? "checkout" : fc1->uuid;
    char const * zUuid2 = (0==vid2) ? "checkout" : fc2->uuid;
    if(VDiffApp.brief){
      f__out("DIFF: %.8s ==> %.8s %s\n", zUuid1, zUuid2,
             fc2->name);
    }else{
      VDiffApp.diffOpt.hashLHS = zUuid1;
      VDiffApp.diffOpt.hashRHS = zUuid2;
      VDiffApp.diffOpt.nameLHS = fc1->name;
      VDiffApp.diffOpt.nameRHS = fc2->name;
      assert(VDiffApp.diffBuilder->opt == &VDiffApp.diffOpt);
      rc = fsl_diff_v2(fContent1, fContent2, VDiffApp.diffBuilder);
      if(rc){
        fcli_err_set(rc, "Error %s generating diff.", fsl_rc_cstr(rc));
      }else{
        f__out("\n") /* only for compat with fossil(1) */;
      }
    }
    return rc;
  }
}

/**
   Outputs a diff of the two given checkin version RIDs. v1 is, for
   purposes of this algorithm, considered to be the older of the two.

   ONE of the versions may be 0 to indicate the current local
   checkout, which differs semantically from the checked-out version
   in that a version of 0 causes local copies of those files of be
   diffed instead of the checked-in version (of the checked-out
   version! (got that?)).

   That is, v1 of 0 and v1 of 999 might refer to the same checkin
   version, but 0 will cause diffs to be calculated based on
   the local checkout copies, whereas 999 will use the copies
   from the database.

   It sends all output to f_out() and takes its diff-level
   configuration from the VDiffApp global.
*/
static int f_vdiff(fsl_id_t v1, fsl_id_t v2){
  int rc = 0;
  fsl_deck d1 = fsl_deck_empty;
  fsl_deck d2 = fsl_deck_empty;
  fsl_cx * f = fcli_cx();
  fsl_card_F const * fc1 = NULL;
  fsl_card_F const * fc2 = NULL;
  int nameCmp = 0;
  fsl_buffer c1 = fsl_buffer_empty;
  fsl_buffer c2 = fsl_buffer_empty;

  rc = fsl_deck_load_rid(f, &d1, v1, FSL_SATYPE_CHECKIN);
  if(rc) goto end;
  rc = fsl_deck_load_rid(f, &d2, v2, FSL_SATYPE_CHECKIN);
  if(rc) goto end;
  rc = fsl_deck_F_rewind(&d1);
  if(!rc) rc = fsl_deck_F_rewind(&d2);
  if(rc) goto end;

  /*
    Reminder: if v1==0 or v2==0, we need slightly different semantics.
    fsl_deck_load_rid() equates 0 to the current checkout, which is
    half right.  We actually want the content of the current local
    checkout for that case.

    TODO: optimization: if v1==checkout version and v2==local changes,
    filter our result set based on vfile entries which have marked
    changes. We will need fsl_vfile_changes_scan() for that, which is
    current marked internal but should be moved into the public
    API anyway.
  */

#define GLOBMATCH(FC) (!VDiffApp.globs.used ? 1 : !!fsl_glob_list_matches(&VDiffApp.globs, (FC)->name))

  fsl_deck_F_next(&d1, &fc1);
  fsl_deck_F_next(&d2, &fc2);
  while(fc1 || fc2){
    if(!fc1) nameCmp = 1;
    else if(!fc2) nameCmp = -1;
    else{
      char const * zNameToCmp = fc2->priorName ? fc2->priorName : fc2->name;
      nameCmp = fsl_strcmp(fc1->name, zNameToCmp);
    }
    if(nameCmp<0){
      assert(fc1);
      if(GLOBMATCH(fc1)){
        f__out("REMOVED: %s\n", fc1->name);
      }
      fsl_deck_F_next(&d1, &fc1);
    }else if(nameCmp>0){
      if(GLOBMATCH(fc2)){
        f__out("ADDED: %s\n", fc2->name);
      }
      fsl_deck_F_next(&d2, &fc2);
    }else if(v1 && v2 && 0==fsl_strcmp(fc1->uuid, fc2->uuid)){
      fsl_deck_F_next(&d1, &fc1);
      fsl_deck_F_next(&d2, &fc2);
    }else{
      if(GLOBMATCH(fc2)){
        rc = f_vdiff_files(f, v1, fc1, v2, fc2);
        if(rc) goto end;
      }
      fsl_deck_F_next(&d1, &fc1);
      fsl_deck_F_next(&d2, &fc2);
    }
  }/*while(f-cards)*/;
#undef GLOBMATCH
  end:
  if(0==rc && VDiffApp.diffBuilder->finally){
    rc = VDiffApp.diffBuilder->finally(VDiffApp.diffBuilder);
  }
  fsl_deck_finalize(&d1);
  fsl_deck_finalize(&d2);
  fsl_buffer_clear(&c1);
  fsl_buffer_clear(&c2);
  return rc;
}


int main(int argc, char const * const * argv ){
  int rc = 0;
  const char * vFrom = NULL;
  const char * vTo = NULL;
  const char * glob = NULL;
  int32_t nContext = -1;
  bool flagSbs = false;
  bool flagInvert = false;
  bool flagDebug = false;
  bool flagLineNo = false;
  bool flagBW = false;
  bool flagColor = false;
  bool flagIgnoreSpaces = false;
  int flagSbsWidth = -1;
  fsl_cx * f;
  fsl_id_t idFrom = -1, idTo = -1;
  char const * checkoutAlias = ".";
  fsl_buffer globBuf = fsl_buffer_empty;

  fcli_cliflag cliFlags[] = {
    FCLI_FLAG("v1", "from", "version", &vFrom,
              "Version to diff from. May also be provided as "
              "the first non-flag argument"),
    FCLI_FLAG("v2", "to", "version", &vTo,
              "Version to diff to. May also be provided as "
              "the second non-flag argument"),
    FCLI_FLAG_BOOL("w","ignore-all-space",&flagIgnoreSpaces,
                   "Ignore all whitespace differences."),
    FCLI_FLAG_BOOL("y", "sbs", &flagSbs,
                   "Use side-by-side diff."),
    FCLI_FLAG_I32("W","sbs-width","max column width",&flagSbsWidth,
                  "Max side-by-side diff view width. Implies -y."),
    FCLI_FLAG_I32("c", "context", "integer", &nContext,
              "Number of context lines."),
    FCLI_FLAG("g", "glob", "string", &glob,
              "Lists only changes to filenames matching the given "
              "comma-separated globs. May be passed multiple times. "
              "All non-flag arguments after the versions are treated "
              "as globs."),
    FCLI_FLAG_BOOL("l","line-numbers",&flagLineNo,
                   "Add line numbers to unified diff output."),
    FCLI_FLAG_BOOL("i","invert",&flagInvert,
                   "Invert the direction of the diff."),
    FCLI_FLAG_BOOL("b","brief", &VDiffApp.brief,
                   "Elides actual diffs and only summarizes "
                   "the changes."),
    FCLI_FLAG_BOOL("bw", "no-color", &flagBW,
                   "Disable color output. This is automatic if stdout "
                   "is not a tty."),
    FCLI_FLAG_BOOL(NULL,"color", &flagColor,
                   "Try to force ANSI color even if stdout is not "
                   "a terminal or --no-color is used. Does not work "
                   "with all diff formats."),
    FCLI_FLAG_BOOL(NULL,"debug", &flagDebug,
                   "Use the debug/test diff builder."),
    fcli_cliflag_empty_m
  };
  fcli_help_info FCliHelp = {
    "Generate diffs of different repository and/or the "
    "local checkout versions.",
    "[filenames or quoted globs...]",
    fcli_local_help
  };
  assert(NULL==stdscr);
  rc = fcli_setup_v2(argc, argv, cliFlags, &FCliHelp);
  if(rc) goto end;

  f = fcli_cx();
  if(!fsl_cx_db_repo(f)){
    rc = fcli_err_set(FSL_RC_NOT_A_REPO,
                      "Requires a repository db. See --help.");
    goto end;
  }

  while(glob){
    fsl_glob_list_parse(&VDiffApp.globs, glob);
    glob = NULL;
    fcli_flag2("g","glob", &glob);
  }

  if(fcli_has_unused_flags(0)) goto end;

  if(flagIgnoreSpaces) VDiffApp.diffOpt.diffFlags |= FSL_DIFF2_IGNORE_ALLWS;
  if(flagInvert) VDiffApp.diffOpt.diffFlags |= FSL_DIFF2_INVERT;
  if(nContext>=0){
    VDiffApp.diffOpt.contextLines = (unsigned short)nContext;
    if(0==nContext){
      VDiffApp.diffOpt.diffFlags |= FSL_DIFF2_CONTEXT_ZERO;
    }
  }
  if(flagLineNo) VDiffApp.diffOpt.diffFlags |= FSL_DIFF2_LINE_NUMBERS;
  if(!vFrom) vFrom = fcli_next_arg(1);
  if(!vTo) vTo = fcli_next_arg(1);
  if(!vFrom && !vTo){
    /* Special case: compare current checkout repo version vs local copy. */
    vFrom = "current";
    idTo = 0;
    vTo = ".";
  }else if(vFrom && !vTo){
    /* Special case: permit (".") by itself as an alias for ("current" ".") */
    if(0==fsl_strcmp(vFrom,checkoutAlias)){
      vFrom = "current";
    }
    vTo = ".";
    idTo = 0;
  }else if(vFrom && vTo
           && 0==fsl_strcmp(vFrom,checkoutAlias)
           && 0==fsl_strcmp(vTo,checkoutAlias)){
    /* Special case: permit ("." ".") as an alias for ("current" ".") */
    vFrom = "current";
    idTo = 0;
  }else if(vTo && !vFrom){
    rc = fcli_err_set(FSL_RC_MISUSE, "Both of -v1 UUID and -v2 UUID are required.");
    goto end;
  }

  if(0==fsl_strcmp(vFrom, checkoutAlias)) idFrom = 0;
  else rc = fsl_sym_to_rid(f, vFrom, FSL_SATYPE_CHECKIN, &idFrom);
  if(!rc && idTo<0){
    if(0==fsl_strcmp(vTo, checkoutAlias)) idTo = 0;
    else rc = fsl_sym_to_rid(f, vTo, FSL_SATYPE_CHECKIN, &idTo);
  }
  if(rc) goto end;
  else if(idFrom==idTo){
    rc = fcli_err_set(FSL_RC_RANGE,
                      "Cowardly refusing to diff a version "
                      "against itself.");
    goto end;
  }

  while((glob = fcli_next_arg(true))){
    if(fsl_cx_has_ckout(f)){
      /* Check if each each entry looks like the name of an existing
         file. If so, add the repo-relative canonicalized name to
         the glob list instead of the literal glob argument. The end
         effect is that we accept filenames as well as globs. */
      fsl_buffer_reuse(&globBuf);
      if(fsl_ckout_filename_check(f, true, glob, &globBuf)){
        fcli_err_reset();
      }else{
        char const * z = fsl_buffer_cstr(&globBuf);
        if(fsl_cx_stat(f, false, z, NULL)){
          fcli_err_reset();
        }else{
          glob = z;
        }
      }
    }
    fsl_glob_list_parse(&VDiffApp.globs, glob);
  }

  if(!idTo || !idFrom){
    if(!fsl_cx_db_ckout(f)){
      rc = fcli_err_set(FSL_RC_NOT_A_CKOUT,
                        "Using the '.' (local checkout) version "
                        "alias requires a checkout.");
      goto end;
    }
  }
  if(flagSbsWidth>0){
    VDiffApp.diffOpt.columnWidth = (unsigned short)flagSbsWidth;
    flagSbs = true;
  }
  VDiffApp.diffBuilder = fsl_dibu_ncu_alloc();
  if(rc) goto end;
  VDiffApp.diffBuilder->opt = &VDiffApp.diffOpt;
  //VDiffApp.diffOpt.out = fsl_output_f_FILE;
  //VDiffApp.diffOpt.outState = stdout;
  assert(idFrom>=0);
  assert(idTo>=0);
  //f_out("vFrom=%d %s, vTo=%d %s\n", (int)idFrom, vFrom, (int)idTo, vTo);
  setlocale(LC_ALL, "")/*needed for ncurses w/ UTF8 chars*/;
  fnc_screen_init();
  nonl(); keypad(stdscr,TRUE); cbreak();
  if(has_colors()){
    start_color();
    init_pair(1, COLOR_RED,     COLOR_BLACK);
    init_pair(2, COLOR_GREEN,   COLOR_BLACK);
    init_pair(3, COLOR_YELLOW,  COLOR_BLACK);
    init_pair(4, COLOR_BLUE,    COLOR_BLACK);
    init_pair(5, COLOR_CYAN,    COLOR_BLACK);
    init_pair(6, COLOR_MAGENTA, COLOR_BLACK);
    init_pair(7, COLOR_WHITE,   COLOR_BLACK);
  }
  curs_set(0);
  mvwaddstr(stdscr,1,0,"Running diff... please wait...");
  wrefresh(stdscr);
  rc = f_vdiff( idFrom, idTo );
  clear();
  wrefresh(stdscr);
  end:
  fsl_glob_list_clear(&VDiffApp.globs);
  fsl_buffer_clear(&VDiffApp.fcontent1);
  fsl_buffer_clear(&VDiffApp.fcontent2);
  fsl_buffer_clear(&VDiffApp.fhash);
  fsl_buffer_clear(&globBuf);
  if(VDiffApp.diffBuilder){
    VDiffApp.diffBuilder->finalize(VDiffApp.diffBuilder);
  }
  fnc_screen_shutdown();
  return fcli_end_of_main(rc);
}
