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

  Potential TODO: the fsl_cidiff() API was added since this was
  written and uses essentially the same logic for its difference
  algorithm.  We can refactor this to use that API as its main loop
  driver.
*/

#include <string.h>
#include "libfossil.h"
#if defined(HAS_NCURSES)
#  include "fsl-ncurses.h"
#endif

/* Only for debugging */
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

static void fcli_local_help(void){
  puts("If neither --from nor --to are supplied (nor implied!), or "
       "both versions are '.', then it behaves as if it were passed "
       "\"--from=current --to=.\", where '.' is a symbolic name for "
       "the local checkout.\n");

  puts("All non-flag parameters, after flag processing is finished, are treated "
       "as filenames/globs and diffs are restricted to files matching those "
       "names/globs.");
}

static struct VDiffApp {
  char const * glob;
  short contextLines;
  short sbsWidth;
  int diffFlags;
  bool brief;
  //! File content for LHS version.
  fsl_buffer fcontent1;
  //! File content for RHS version.
  fsl_buffer fcontent2;
  //! Holds a hash of file content.
  fsl_buffer fhash;
  //! For canonicalizing filenames.
  fsl_buffer fCanon;
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
fsl_buffer_empty_m/*fCanon*/,
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

static bool file_ckout_is_deleted(char const * fname){
  return fsl_db_exists(fsl_cx_db(fcli_cx()),
                       "SELECT 1 FROM vfile WHERE deleted<>0 "
                       "AND pathname=%Q",
                       fname);
}

/**
   If zName matches a vfile.origname entry or (vfile.origname
   is NULL and vfile.pathname=zName), a copy of that string
   is returned, else NULL is returned. Returned string must
   be fsl_free()d.
*/
static char * file_ckout_effective_name(char const * zName){
  char *rv = fsl_db_g_text(fsl_cx_db(fcli_cx()), NULL,
                           "SELECT coalesce(pathname,origname) "
                           "FROM vfile WHERE origname=%Q "
                           "OR (origname IS NULL and pathname=%Q)",
                           zName, zName);
  //fprintf(stderr,"effective name: %s => %s\n", zName, rv);
  return rv;
}

/**
   Check two F-cards for differences and render any they have. vid1
   and vid2 are the checkin versions from which the given cards are
   from. ONE of vid1 or vid2 may be 0, indicating the current
   checkout, in which case we compare the checked-out copy of that
   file. Returns 0 on success, non-0 on fatal error.

   ONE of fc1 or fc2 may be NULL, indicating that the file was added
   in vid2 (if fc1 is NULL) or removed from vid2 (if fc2 is NULL).
*/
int f_vdiff_files(fsl_cx * const f,
                  fsl_id_t vid1,
                  fsl_card_F const * fc1,
                  fsl_id_t vid2,
                  fsl_card_F const * fc2){
  int rc = 0;
  char * name1 = NULL;
  char * name2 = NULL;
  fsl_buffer * fContent1 = &VDiffApp.fcontent1;
  fsl_buffer * fContent2 = &VDiffApp.fcontent2;
  fsl_buffer * fLocalHash = &VDiffApp.fhash
    /* hash of the local checked-out file */;
  fsl_time_t rmtime = 0;
  fsl_time_t fmtime = 0;
  int deleted = 0 /*-1=local LHS deleted, 1=local RHS deleted*/;
  fsl_card_F const * fcHashCmp = NULL
    /* The card against which we will compare fLocalHash.  If
       fLocalHash is the hash of vid1 then this will be fc2, and vice
       versa. */;
#if 0
  if(vid1>0 && vid2>0 &&
     fc1 && fc2 && !fsl_uuidcmp(fc1->uuid, fc2->uuid)){
    /* No diffs to check */
    return 0;
  }
#endif

  /**
     TODO: optimization: use vfile where we can to reduce the
     set of files we scan. This can only work if one version
     is the checked-out version and one is the local checkout
     changes.
  */

  /* Else different content in each version OR we have a local file
     and need to load it to see if it's changed. */
  fsl_buffer_reuse(fContent1);
  fsl_buffer_reuse(fContent2);
  fsl_buffer_reuse(fLocalHash);

  assert(vid1!=vid2);

  name1 = (fc1 && 0==vid1) ? file_ckout_effective_name(fc1->name) :
    fsl_strdup((fc1 ? fc1 : fc2)->name);
  assert(NULL!=name1 && "\"Cannot happen\"?");
  name2 = (fc2 && 0==vid2) ? file_ckout_effective_name(fc2->name) :
    fsl_strdup((fc2 ? fc2 : fc1)->name);
  if(!name2){
    MARKER(("BUG: got null name2 for %s\n", (fc2?fc2:fc1)->name));
    assert(NULL!=name2 && "\"Cannot happen\"?");
    abort();
  }

  if(NULL==fc1){
    assert(NULL!=fc2);
    deleted = -1;
    if(0==vid1) fsl_buffer_external(fLocalHash, "(missing)", -1);
    fsl_buffer_append(fContent2, "(file added in RHS)", -1);
  }else if(0==vid1){ /* vid1 is the current checkout version */
    assert(0 != vid2);
    if(file_ckout_is_deleted(name1)){
      deleted = -1;
      fsl_buffer_external(fLocalHash, "(locally deleted)", -1);
    }else{
      rc = fsl_ckout_file_content(f, 0, name1, fContent1);
      if(!rc){
        rc = f_vdiff_hash(fc1, fContent1, fLocalHash);
        if(!rc){
          fsl_file_canonical_name2(fsl_cx_ckout_dir_name(f, NULL), name1,
                                   fsl_buffer_reuse(&VDiffApp.fCanon),
                                   false);
          fcHashCmp = fc2;
          fmtime = fsl_file_mtime(fsl_buffer_cstr(&VDiffApp.fCanon));
          if(fmtime<0){
            rc = fcli_err_set(FSL_RC_NOT_FOUND,"Cannot stat file: %s",
                              name1);
          }
        }
      }
    }
  }else{
    rc = fsl_card_F_content(f, fc1, fContent1);
    if(!rc && (0==vid2)){
      /* Collect the repo-side mtime IF the other version==0. */
      rc = fsl_card_F_ckout_mtime(f, vid1, fc1, &rmtime, NULL);
    }
  }

  if(rc) goto end;

  /* Repeat for vid2. */
  if(NULL==fc2){
    assert(NULL!=fc1);
    deleted = -1;
    if(0==vid2) fsl_buffer_external(fLocalHash, "(missing)", -1);
    fsl_buffer_reuse(fContent1);
    fsl_buffer_append(fContent1, "(file removed from RHS)", -1);
    //fsl_buffer_append(fContent2, "(removed from RHS)", -1);
  }else if(0==vid2){ /* vid2 is the current checkout */
    assert(0 != vid1);
    if(file_ckout_is_deleted(name2)){
      deleted = 1;
      fsl_buffer_external(fLocalHash, "(locally deleted)", -1);
    }else{
      rc = fContent2->used ? 0 : fsl_ckout_file_content(f, 0, name2, fContent2);
      if(!rc){
        rc = f_vdiff_hash(fc2, fContent2, fLocalHash);
        if(!rc){
          fsl_file_canonical_name2(fsl_cx_ckout_dir_name(f, NULL), name2,
                                   fsl_buffer_reuse(&VDiffApp.fCanon),
                                   false);
          fcHashCmp = fc1;
          fmtime = fsl_file_mtime(fsl_buffer_cstr(&VDiffApp.fCanon));
          if(fmtime<0){
            rc = fcli_err_set(FSL_RC_NOT_FOUND,"Cannot stat file: %s",
                              name2);
          }
        }
      }
    }
  }else{
    // If LHS is locally deleted, don't bother loading content, as we won't
    // display it.
    rc = fContent2->used ? 0 : fsl_card_F_content(f, fc2, fContent2);
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
    char const * zUuid1 = (0==vid1) ? "checkout" : (fc1 ? fc1->uuid : "(null)");
    char const * zUuid2 = (0==vid2) ? "checkout" : (fc2 ? fc2->uuid : "(null)");
    if(0 && deleted){
      // We don't want to display the whole content of deleted files.
      fsl_buffer_reuse(fContent1);
      fsl_buffer_reuse(fContent2);
      if(deleted<0) fsl_buffer_append(fContent1, "Not in LHS version.",-1);
      else fsl_buffer_append(fContent2, "Not in RHS version.",-1);
    }
    if(VDiffApp.brief){
      fsl_outputf(f, "DIFF: %.8s ==> %.8s %s\n", zUuid1, zUuid2,
                  name2);
    }else{
      VDiffApp.diffOpt.hashLHS = zUuid1;
      VDiffApp.diffOpt.hashRHS = zUuid2;
      VDiffApp.diffOpt.nameLHS = fc1 ? name1 : name2;
      VDiffApp.diffOpt.nameRHS = fc2 ? name2 : name1;
      if(fsl_looks_like_binary(fContent1)
         || fsl_looks_like_binary(fContent2)){
        fsl_buffer_append(fsl_buffer_reuse(fContent1),
                          "(cannot diff binary content.)", -1);
        fsl_buffer_append(fsl_buffer_reuse(fContent2),
                          "[CANNOT DIFF BINARY CONTENT.]", -1);
      }
      assert(VDiffApp.diffBuilder->opt == &VDiffApp.diffOpt);
      rc = fsl_diff_v2(fContent1, fContent2, VDiffApp.diffBuilder);
      if(rc){
        fcli_err_set(rc, "Error %s generating diff.", fsl_rc_cstr(rc));
      }else if(VDiffApp.diffBuilder->opt->out){
        VDiffApp.diffBuilder->opt->out(VDiffApp.diffBuilder->opt->outState, "\n", 1);
        /* only for compat with fossil(1) */
      }
    }
    return rc;
  }
  end:
  fsl_free(name1);
  fsl_free(name2);
  return rc;
}

/**
   Outputs a diff of the two given checkin version RIDs. v1 is, for
   purposes of this algorithm, considered to be the older of the two.

   ONE of the versions may be 0 to indicate the current local
   checkout, which differs semantically from the checked-out version
   in that a version of 0 causes local copies of those files of be
   diffed instead of the checked-in version (of the checked-out
   version! (got that?)).

   That is, v1 of 0 and v2 of 999 might refer to the same checkin
   version, but 0 will cause diffs to be calculated based on
   the local checkout copies, whereas 999 will use the copies
   from the database.

   It sends all output to f_out() and takes its diff-level
   configuration from the VDiffApp global.

   Bugs/shortcomings: this algorithm cannot properly handle files
   added in the current checkout. To do a better job of comparing we
   should stop comparing the fsl_deck instances and instead load the
   list of files from both decks into a temp table and then walk over
   that. Aside from renaming, which we cannot catch in that case when
   v1 and v2 are more than one version apart, we could get overall
   better results.
*/
static int f_vdiff(fsl_id_t v1, fsl_id_t v2){
  int rc = 0;
  fsl_deck d1 = fsl_deck_empty;
  fsl_deck d2 = fsl_deck_empty;
  fsl_cx * f = fcli_cx();
  fsl_card_F const * fc1 = NULL;
  fsl_card_F const * fc2 = NULL;
  int nameCmp = 0;

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
    changed. We will need fsl_vfile_changes_scan() for that.
  */

#define GLOBMATCH(FC) (!VDiffApp.globs.used ? 1 : !!fsl_glob_list_matches(&VDiffApp.globs, (FC)->name))

  fsl_deck_F_next(&d1, &fc1);
  fsl_deck_F_next(&d2, &fc2);
  /**
     Outputing RENAMED/REMOVED/ADDED/etc here is completely incompatible with
     various diff builders.
  */
  while(fc1 || fc2){
    if(!fc1) nameCmp = 1;
    else if(!fc2) nameCmp = -1;
    else nameCmp = fsl_strcmp(fc1->name, fc2->name);
    if(nameCmp<0){
      assert(fc1);
      if(GLOBMATCH(fc1)){
        //f_out("REMOVED: %s\n", fc1->name);
        rc = f_vdiff_files(f, v1, fc1, v2, NULL);
        if(rc) goto end;
      }
      fsl_deck_F_next(&d1, &fc1);
    }else if(nameCmp>0){
      if(GLOBMATCH(fc2)){
        //f_out("ADDED: %s\n", fc2->name);
        rc = f_vdiff_files(f, v1, NULL, v2, fc2);
        if(rc) goto end;
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
  }/*while(f-cards)*/
#undef GLOBMATCH
  end:
  fsl_deck_finalize(&d1);
  fsl_deck_finalize(&d2);
  if(0==rc && VDiffApp.diffBuilder->finally){
    rc = VDiffApp.diffBuilder->finally(VDiffApp.diffBuilder);
  }
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
  bool flagLineNo = false;
  bool flagBW = false;
  bool flagColor = false;
  bool flagIgnoreSpaces = false;
  bool flagSaveFormat = false;
  int flagSbsWidth = -1;
  fsl_cx * f;
  fsl_id_t idFrom = -1, idTo = -1;
  char const * checkoutAlias = ".";
  fsl_buffer globBuf = fsl_buffer_empty;
  char const * builderName = NULL;
  char const * zConfigKey = "f-vdiff:format";
  unsigned int builderImplFlags = 0;
  fcli_cliflag cliFlags[] = {
    FCLI_FLAG("v1", "from", "version", &vFrom,
              "Version to diff from. May also be provided as "
              "the first non-flag argument. Use '.' for the "
              "checkout version."),
    FCLI_FLAG("v2", "to", "version", &vTo,
              "Version to diff to. May also be provided as "
              "the second non-flag argument. Use '.' for the "
              "checkout version. A '.' for both -v1 and -v2 is "
              "shorthand for: -v1 current -v2 ."),
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
                   "Disable color output for formats which use ANSI "
                   "colors. This is automatic if stdout is not a "
                   "tty."),
    FCLI_FLAG_BOOL(NULL,"color", &flagColor,
                   "Try to force ANSI color even if stdout is not "
                   "a terminal or --no-color is used. Does not work "
                   "with all diff formats."),
    FCLI_FLAG("f","format", "diff-builder-name", &builderName,
              "Specify diff builder name: [u]nified, [s]plit, [d]ebug, "
              "[j]son, [t]cl, tk, [n]curses (if available)."),
    FCLI_FLAG_BOOL(NULL, "save-format", &flagSaveFormat,
                   "Saves the selected format in fossil's global config "
                   "db, along with certain other flags, and will use "
                   "those as the defaults in future sessions. "
                   "Only saves if --format is provided, else it unsets "
                   "any stored option."),
    fcli_cliflag_empty_m
  };
  fcli_help_info FCliHelp = {
    "Generate diffs of different repository and/or the "
    "local checkout versions.",
    "version1 version2 [filenames or quoted globs...]",
    fcli_local_help
  };
  rc = fcli_setup_v2(argc, argv, cliFlags, &FCliHelp);
  if(rc) goto end;
  VDiffApp.diffOpt.out = fsl_output_f_FILE;
  VDiffApp.diffOpt.outState = stdout;

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
  fsl_dibu_e eB = FSL_DIBU_INVALID;
  if(!builderName && !flagSaveFormat && 0==fsl_config_open(f, NULL)){
    char * zVal = fcli_fax(fsl_config_get_text(f,FSL_CONFDB_GLOBAL,
                                               zConfigKey, NULL));
    if(zVal && strstr(zVal, "--line-numbers")){
      flagLineNo = true;
    }
    builderName = zVal;
  }
  char const * effectiveBuilderName =
    (builderName ? builderName : "u");
  switch(*effectiveBuilderName){
    case 's': eB = FSL_DIBU_SPLIT_TEXT; break;
#if defined(HAS_NCURSES)
    case 'n': VDiffApp.diffBuilder = fsl_dibu_ncu_alloc();
      VDiffApp.diffOpt.out = NULL;
      break;
#endif
    case 'j': eB = FSL_DIBU_JSON1; break;
    case 't': eB = FSL_DIBU_TCL;
      builderImplFlags |=
        ('k'==effectiveBuilderName[1])
        ? FSL_DIBU_TCL_TK
        : 0;//FSL_DIBU_TCL_BRACES;
      break;
    case 'd': eB = FSL_DIBU_DEBUG; break;
    case 'u': eB = FSL_DIBU_UNIFIED_TEXT; break;
    default: break;
  }
  if(!VDiffApp.diffBuilder){
    if(FSL_DIBU_INVALID==eB){
      eB = flagSbs
        ? FSL_DIBU_SPLIT_TEXT
        : FSL_DIBU_UNIFIED_TEXT;
    }
    rc = fsl_dibu_factory(eB, &VDiffApp.diffBuilder);
    if(rc) goto end;
  }
  if(!VDiffApp.diffBuilder){
    rc = fcli_err_set(FSL_RC_MISUSE,
                      "Cannot figure out which diff builder to use.");
    goto end;
  }
  if(flagLineNo) VDiffApp.diffOpt.diffFlags |= FSL_DIFF2_LINE_NUMBERS;
  if(flagSaveFormat && 0==fsl_config_open(f, NULL)){
    if(builderName){
      char * zVal = fsl_mprintf("%s%s", builderName,
                                flagLineNo ? " --line-numbers" : "");
      f_out("Saving builder format: %s=%s\n", zConfigKey, zVal);
      fsl_config_set_text(f, FSL_CONFDB_GLOBAL, zConfigKey, zVal);
      fsl_free(zVal);
    }else{
      f_out("Restoring saved config option to default: %s\n", zConfigKey);
      fsl_config_unset(f, FSL_CONFDB_GLOBAL, zConfigKey);
    }
  }
  fsl_config_close(f);
  VDiffApp.diffBuilder->opt = &VDiffApp.diffOpt;
  if(flagColor || (!flagBW && fsl_isatty(1))){
    fcli_diff_colors(&VDiffApp.diffOpt, FCLI_DIFF_COLORS_DEFAULT);
  }
  assert(idFrom>=0);
  assert(idTo>=0);
  //f_out("vFrom=%d %s, vTo=%d %s\n", (int)idFrom, vFrom, (int)idTo, vTo);
  VDiffApp.diffBuilder->implFlags |= builderImplFlags;
  rc = f_vdiff( idFrom, idTo );
  end:
  fsl_glob_list_clear(&VDiffApp.globs);
  fsl_buffer_clear(&VDiffApp.fcontent1);
  fsl_buffer_clear(&VDiffApp.fcontent2);
  fsl_buffer_clear(&VDiffApp.fhash);
  fsl_buffer_clear(&VDiffApp.fCanon);
  fsl_buffer_clear(&globBuf);
  if(VDiffApp.diffBuilder){
    VDiffApp.diffBuilder->finalize(VDiffApp.diffBuilder);
  }
  return fcli_end_of_main(rc);
}

#undef MARKER
