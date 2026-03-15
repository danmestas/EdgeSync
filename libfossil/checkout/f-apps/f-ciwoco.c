/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code
*/
/**
   f-ciwoco is a libfossil app to create checkins in a repository without
   requiring a checkout. ciwoco = CheckIn WithOut CheckOut.
*/
#ifdef NDEBUG
/* Force assert() to always be in effect. */
#undef NDEBUG
#endif
#include "libfossil.h"
#include <string.h>

// Only for testing/debugging..
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

// Global app state.
struct App_ {
  char const * sym;
  char const * branch;
  char const * comment;
  char const * commentFile;
  char const * stripPrefix;
  fsl_size_t nStripPrefix;
  bool addRCard;
  bool dryRun;
  bool checkIgnoreGlobs;
  bool noChangesNoCry;
} App = {
.sym = "trunk",
.branch = NULL,
.comment = NULL,
.commentFile = NULL,
.stripPrefix = NULL,
.nStripPrefix = 0,
.addRCard = true,
.dryRun = false,
.checkIgnoreGlobs = true,
.noChangesNoCry = false
};

/**
   Takes a filename in the form NAME[:REPO-NAME] and dissects it into
   two parts:

   *localName is the NAME part.

   *repoName is the REPO-NAME part, if set, else the NAME part.

   The returned strings are owned by fcli and survive until app
   shutdown.
*/
static void parse_fn(char const *full, char const ** localName,
                    char const ** repoName){
  char * z = fcli_fax( fsl_strdup(full) );
  *localName = *repoName = z;
  if('.'==z[0] && '/'==z[1]){
    /* For usability convenience, e.g. passing along input from:

       find . -type f

       Strip any leading ./ on the reponame part.
    */
    z+=2;
    *repoName = z;
  }
  while( *z && *z!=':' ) ++z;
  if(':'==*z){
    *z++ = 0;
    *repoName = z;
  }
  if(App.stripPrefix){
    if(0==fsl_strncmp(App.stripPrefix, *repoName, App.nStripPrefix)){
      *repoName += App.nStripPrefix;
    }
  }
}

/**
   Checks whether d has changes compared to its parent (if any).
   Returns 0 on succcess (any error is considered fatal). Sets *rv to
   true if there are changes, else *rv to false.
*/
static int deck_check_changes(fsl_deck * const d, bool * rv){
  int rc = 0;
  fsl_cx * const f = d->f;
  fsl_deck p = fsl_deck_empty;
  *rv = false;
  if(!d->P.used){
    /* New root deck. */
    *rv = true;
    return 0;
  }
  rc = fsl_deck_load_sym(f, &p, (char const *)d->P.list[0], d->type);
  if(rc) goto end;
  if(p.F.used != d->F.used
     /* ^^^ that isn't necessarily a correct heuristic if the parent
        is a delta manifest. */
     || (p.R && d->R && fsl_strcmp(p.R,d->R))){
    goto change;
  }
  for(fsl_size_t i = 0; i < p.F.used; ++i ){
    fsl_card_F const * const fL = &p.F.list[i];
    fsl_card_F const * const fR = &d->F.list[i];
    if(fR->priorName
       || fsl_strcmp(fL->name, fR->name)
       || fsl_strcmp(fL->uuid, fR->uuid)
       || fL->perm != fR->perm){
      goto change;
    }
  }
  end:
  fsl_deck_finalize(&p);
  return rc;
  change:
  *rv = true;
  goto end;
}

/**
   Does... everything. Returns 0 on success (or non-error).
*/
static int do_everything(void){
  int rc = 0;
  fsl_cx * const f = fcli_cx();
  fsl_deck d = fsl_deck_empty;
  char const *fname = 0;
  fsl_buffer fcontent = fsl_buffer_empty;
  int const verbose = fcli_is_verbose();
  bool isNewRoot = false;
  char * dBranch = NULL;
  rc = fsl_cx_txn_begin(f);
  if(rc) return rc;

  if(0==fsl_strcmp("-", App.sym)){
    isNewRoot = true;
    fsl_deck_init(f, &d, FSL_SATYPE_CHECKIN);
    if(!App.branch || !*App.branch){
      // We "need" a default branch, so...
      App.branch = "ciwoco";
    }
  }else{
    rc = fsl_deck_load_sym(f, &d, App.sym, FSL_SATYPE_CHECKIN);
    if(rc) goto end;
    assert(f==d.f);
    f_out("Deriving from checkin [%s] (RID %"FSL_ID_T_PFMT")\n",
          App.sym, d.rid);
    rc = fsl_deck_derive(&d);
    if(rc) goto end;
  }
  if(App.branch){
    if(!isNewRoot){
      rc = fsl_branch_of_rid(f, d.rid, true, &dBranch);
      if(rc) goto end;
    }
    if(isNewRoot || 0!=fsl_strcmp(dBranch, App.branch)){
      /* Only set the branch if it would really be a change. In that case,
         we need to cancel the previous branch's tag. */
      rc = fsl_deck_branch_set(&d, App.branch);
      if(!rc && dBranch){
        dBranch = fsl_mprintf("sym-%z", dBranch);
        rc = fsl_deck_T_add(&d, FSL_TAGTYPE_CANCEL, NULL, dBranch,
                            "Cancelled by branch.");
      }
      if(rc) goto end;
    }
  }
  if(App.commentFile){
    rc = fsl_buffer_fill_from_filename(&fcontent, App.commentFile);
    if(rc){
      rc = fcli_err_set(rc, "Cannot read comment file: %s", App.commentFile);
      goto end;
    }
    char * c = fsl_buffer_take(&fcontent);
    fcli_fax(c);
    App.comment = c;
  }
  if(!App.comment){
    rc = fcli_err_set(FSL_RC_MISUSE, "Missing required checkin comment.");
    goto end;
  }

  rc = fsl_deck_C_set(&d, App.comment, -1);
  if(0==rc){
    char const * u = fsl_cx_user_get(f);
    if(!u){
      rc = fcli_err_set(FSL_RC_MISUSE,
                        "Cannot determine user name. "
                        "Try using --user NAME.");
    }else{
      rc = fsl_deck_U_set(&d, u);
    }
  }
  if(rc) goto end;

  /**
     Look for args in the form:

     FILENAME[:REPO-NAME]

     Where FILENAME is the local filesystem name and REPO-NAME is the
     name of that file within the repository, defaulting to FILENAME.

     Potential TODOs:

     - If FILENAME is a directory, process it recursively. If so, possibly
     offer a flag which tells it to only process files which already exist.
  */
  while((fname = fcli_next_arg(true))){
    /* Handle input files... */
    char const * nameLocal = 0;
    char const * nameRepo = 0;
    fsl_fstat fst = fsl_fstat_empty;
    parse_fn(fname, &nameLocal, &nameRepo);
    if(!*nameLocal || !*nameRepo){
      rc = fcli_err_set(FSL_RC_MISUSE,
                        "Only Chuck Norris may use empty filenames.");
      goto end;
    }
    rc = fsl_stat(nameLocal, &fst, false);
    if(rc){
      rc = fcli_err_set(rc, "Error stat()'ing file: %s", nameLocal);
      goto end;
    }
    switch(fst.type){
      case FSL_FSTAT_TYPE_DIR:
        rc = fcli_err_set(FSL_RC_TYPE,
                          "Cannot currently CiWoCo a whole directory: %s",
                          nameLocal);
        break;
      case FSL_FSTAT_TYPE_LINK:
        rc = fcli_err_set(FSL_RC_TYPE,
                          "Symlinks are currently unsupported: %s",
                          nameLocal);
        break;
      case FSL_FSTAT_TYPE_UNKNOWN:
        rc = fcli_err_set(FSL_RC_TYPE, "Unknown file type: %s", nameLocal);
        break;
      case FSL_FSTAT_TYPE_FILE: break;
    }
    if(rc) goto end;
    rc = fsl_buffer_fill_from_filename(&fcontent, nameLocal);
    if(rc){
      rc = fcli_err_set(rc, "Error reading file: %s", nameLocal);
      goto end;
    }
    if(App.checkIgnoreGlobs){
      if(fsl_cx_glob_matches(f, FSL_GLOBS_IGNORE, nameRepo)){
        if(verbose){
          f_out("Skipping due to ignore-glob: %s\n", nameLocal);
        }
        continue;
      }
    }
    fsl_card_F const * fc = fsl_deck_F_search(&d, nameRepo);
    if(NULL==fc){
      /* New file: make sure its name is not reserved. If it's reserved
         but already in the repo then the damage is already done and
         we won't enforce it retroactively. */
      rc = fsl_reserved_fn_check(f, nameRepo, -1, false);
      f_out("reserved name check: %s %s\n", fsl_rc_cstr(rc), nameRepo);
      if(rc) goto end;
    }
    if(verbose){
      char const * const action = fc ? "Updating" : "Adding";
      if(nameLocal!=nameRepo){
        f_out("%s [%s] as [%s]\n", action,nameLocal, nameRepo);
      }else{
        f_out("%s [%s]\n", action, nameLocal);
      }
    }
    rc = fsl_deck_F_set_content(&d, nameRepo, &fcontent,
                                fst.perm==FSL_FSTAT_PERM_EXE
                                ? FSL_FILE_PERM_EXE
                                : FSL_FILE_PERM_REGULAR,
                                NULL);
    if(rc) goto end;
  }/*next-arg loop*/

  if(App.addRCard || (isNewRoot && !d.F.used)
     /* A checkin artifact with no P/F/Q/R-cards cannot be unambiguously
        recognized as a checkin, so we'll add an R-card if someone
        creates an empty root-level checkin. */
     ){
    if(verbose) f_out("Calculating R-card... ");
    rc = fsl_deck_R_calc(&d);
    if(verbose) f_out("\n");
    if(rc) goto end;
  }

  if(isNewRoot){
    f_out("Creating new root entry.\n");
  }else{
    bool gotChanges = false;
    rc = deck_check_changes(&d, &gotChanges);
    if(rc) goto end;
    else if(!gotChanges){
      if(App.noChangesNoCry){
        f_out("No changes were made. Exiting without an error.\n");
        goto end;
      }else{
        rc = fcli_err_set(FSL_RC_NOOP,
                          "No file changes were made from "
                          "the parent version. Use --no-change-no-error "
                          "to permit a checkin anyway.");
        goto end;
      }
    }
  }

  double const julian = true
    ? fsl_db_julian_now(fsl_cx_db_repo(f))
    /* ^^^^ ms precision */
    : fsl_julian_now() /* seconds precision */;
  rc = fsl_deck_D_set(&d, julian);
  if(rc) goto end;

  rc = fsl_deck_save(&d, false);
  if(rc) goto end;
  f_out("Saved checkin %z (RID %"FSL_ID_T_PFMT")\n",
        fsl_rid_to_uuid(d.f, d.rid), d.rid);
  if(App.dryRun){
    f_out("Dry-run mode. Rolling back.\n");
    if(fcli_is_verbose()){
      rc = fsl_deck_output(&d, NULL, NULL);
      if(rc) goto end;
    }
  }else{
    f_out("ACHTUNG: current checkouts of this repo will not "
          "automatically know about this commit: they'll need "
          "to 'update' to see the changes.\n");
  }

  assert(0==rc);
  assert(fsl_cx_txn_level(f)>0);
  rc = fsl_cx_txn_end(f, App.dryRun);
  end:
  fsl_free(dBranch);
  fsl_buffer_clear(&fcontent);
  fsl_deck_finalize(&d);
  if(fsl_cx_txn_level(f)){
    fsl_cx_txn_end(f, true);
  }
  return rc;
}

static void ciwoco_help(void){
  f_out("Each non-flag argument is expected to be a file name "
        "in the form LOCALNAME:REPONAME, e.g. '/etc/hosts:foo/hosts'. "
        "The REPONAME part must be a repository-friendly name, e.g. no "
        "leading slashes nor stray ./ components. The default value "
        "for the REPONAME part is the LOCALNAME part, but that will "
        "only be useful in limited cases. ");
  f_out("As a special case, any leading './' on the REPONAME part "
        "is elided\n");
}

int main(int argc, const char * const * argv ){
  const fcli_cliflag FCliFlags[] = {
    FCLI_FLAG("v", "version", "version", &App.sym,
              "Version to derive from (default='trunk'). "
              "Use the value '-' to create a new checkin "
              "without a parent checkin. In that case, the "
              "--branch flag is strongly recommended."),
    FCLI_FLAG("b", "branch", "branch-name", &App.branch,
              "Create a new branch for the new checkin."),
    FCLI_FLAG("m", "comment", "string", &App.comment,
              "Checkin comment."),
    FCLI_FLAG("M", "comment-file", "filename", &App.commentFile,
              "Checkin comment from a file (trumps -m)."),
    FCLI_FLAG("p", "strip-prefix", "string", &App.stripPrefix,
              "Any repository-side filenames which start with the "
              "given prefix get that prefix stripped from them."),
    FCLI_FLAG_BOOL("e","no-change-no-error", &App.noChangesNoCry,
                   "If no changes were made from the previous "
                   "version, exit without an error. New root entries "
                   "are always considered to be changes."),
    FCLI_FLAG_BOOL("n", "dry-run", &App.dryRun,
                   "Dry-run mode. If verbose mode is active, "
                   "the resuling manifest is sent to stdout."),
    FCLI_FLAG_BOOL_INVERT("i", "no-ignore-glob", &App.checkIgnoreGlobs,
                          "If set, do not honor the repo-level 'ignore-glob' "
                          "config setting to determine whether to skip a given "
                          "file."),
    FCLI_FLAG_BOOL_INVERT(NULL, "no-r-card", &App.addRCard,
                          "If set, do not add an R-card to commits. "
                          "Even with this flag, an R-card is always "
                          "added for cases where not having one may "
                          "produce an ambiguous artifact."),
    fcli_cliflag_empty_m // list MUST end with this (or equivalent)
  };
  const fcli_help_info FCliHelp = {
    "Creates a checkin into a fossil repository without an "
    "intermediary checkout.",
    "file1:file1-repo-name ... fileN:file-N-repo-name",
    // ^^^ very brief usage text, e.g. "file1 [...fileN]"
    ciwoco_help // optional callback which outputs app-specific help
  };
  fsl_cx * f = NULL;
  fcli.config.checkoutDir = NULL; // same effect as global --no-checkout flag.
  int rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  else if((rc=fcli_has_unused_flags(false))) goto end;
  f = fcli_cx();
  if(!fsl_needs_repo(f)){
    rc = FSL_RC_NOT_A_REPO;
    goto end;
  }
  fsl_cx_flag_set(f, FSL_CX_F_CALC_R_CARD, App.addRCard);
  if(App.stripPrefix) App.nStripPrefix = fsl_strlen(App.stripPrefix);
  rc = do_everything();
  end:
  return fcli_end_of_main(rc);
}
