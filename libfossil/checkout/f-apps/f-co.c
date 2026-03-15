/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/
/*****************************************************************************
  This file implements the code to checkout a Fossil repository.
*/

#include "libfossil.h"
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

typedef struct {
  fsl_cx * f;
  int  coCount;
  int  written;
  int  kept;
  int  removed;
  bool quiet;
  bool overwriteLocalMods;
} extract_state;

/**
   A basic fsl_confirm_callback_f() implementation which gets "asked
   questions" by the checkout process regarding how to handle certain
   situation which could either lead to data loss or extraneous files
   lying about.

   TODO: connect an interactive and/or flag-driven yes/no/all/cancel
   mechanism to this.
*/
static int
fsl_confirm_callback_f_my(fsl_confirm_detail const * detail,
                          fsl_confirm_response *answer,
                          void * clientState){
  extract_state  const *state = (extract_state const *)clientState;
  if(0 && fcli_is_verbose()){
    MARKER(("Asking for confirmation about event #%d: %s\n",
            detail->eventId, detail->filename));
  }
  switch(detail->eventId){
    case FSL_CEVENT_RM_MOD_UNMGD_FILE:
      // MODIFIED newly-unmanaged files are never removed
      answer->response = FSL_CRESPONSE_YES;
      break;
    case FSL_CEVENT_OVERWRITE_MOD_FILE:
    case FSL_CEVENT_OVERWRITE_UNMGD_FILE:
      answer->response = state->overwriteLocalMods
        ? FSL_CRESPONSE_YES//ALWAYS
        : FSL_CRESPONSE_NEVER
        /* The difference between FSL_CRESPONSE_YES and
           FSL_CRESPONSE_ALWAYS is that the latter will cause the
           checkout process to keep that answer and stop asking
           for purposes of the current phase of the checkout.
        */;
      break;
    default:
      return fsl_cx_err_set(state->f, FSL_RC_UNSUPPORTED,
                            "Unhanded event ID type %d\n",
                            detail->eventId);
  }
  if(0 && fcli_is_verbose()){
    MARKER(("eventId %d answer=%d\n", detail->eventId,
            answer->response));
  }
  return 0;
}

static int write_manifest(fsl_cx * f, int m, int mUuid, int mTags){
  int wrote = 0;
  int const rc = fsl_ckout_manifest_write(f, m, mUuid, mTags, &wrote);
  if(!rc){
    if(wrote) f_out("\n");
    if(0x001 & wrote) f_out("Wrote manifest.\n");
    if(0x010 & wrote) f_out("Wrote manifest.uuid.\n");
    if(0x100 & wrote) f_out("Wrote manifest.tags.\n");
  }
  return rc;
}

/**
   fsl_repo_ckout() callback. Called once for each row of
   the checkout, including file removals.
*/
static int fsl_ckup_f_my(fsl_ckup_state const *cuState){
  enum {DATE_SHORT = 16};
  //fsl_repo_extract_state const * xs = cuState->extractState;
  extract_state  *state = (extract_state *)cuState->callbackState;
  char    tbuf[DATE_SHORT];
  char const * mode = "? ";
  bool noteworthy = false;
  char const * nonTimestamp = 0;
  ++state->coCount;
  switch(cuState->fileChangeType){
    case FSL_CKUP_FCHANGE_NONE: mode = "  ";
      ++state->kept;
      break;
    case FSL_CKUP_FCHANGE_UPDATED: mode = "w ";
      ++state->written;
      break;
    case FSL_CKUP_FCHANGE_RM: mode = "- ";
      ++state->removed;
      switch(cuState->fileRmInfo){
        case FSL_CKUP_RM_KEPT:
          ++state->kept;
          mode = "-U";
          noteworthy = true;
          nonTimestamp = "<UNMANAGED>";
          break;
        case FSL_CKUP_RM:
          mode = "- ";
          assert(cuState->size<0);
          noteworthy = true;
          nonTimestamp = "<REMOVED>  ";
          break;
        default: break;
      }
      break;
    default:
      MARKER(("Unexpected FSL_CKUP_FCHANGE value for ckout op: #%d\n",
              cuState->fileChangeType));
      break;
  }
  if(noteworthy || !state->quiet){
    fsl_strftime_unix(tbuf, DATE_SHORT, "%d %b %Y", cuState->mtime, 1);
    f_out("[%s] %8"FSL_INT_T_PFMT"   %s  %s\n",
          mode, cuState->size,
          nonTimestamp ? nonTimestamp : tbuf,
          cuState->extractState->fCard->name);
  }
  return 0;
}

int main(int argc, char const * const *argv) {
  fsl_db *db = 0;
  fsl_cx *f = NULL;
  fsl_id_t prevId = 0, rid = 0;
  const char *sym = NULL;
  bool force = false, keep = false, manifest = false,
    mtime = false, fDryRun = false;
  int rc = 0;
  extract_state ex;
  memset(&ex, 0, sizeof(extract_state));
  ex.quiet = true;

  fcli_cliflag	         fcli_flags[] = {
    FCLI_FLAG_BOOL("f", "force", &force, "Continue with the checkout "
     "irrespective of any unsaved changes in the current checkout."),
#if 0
    FCLI_FLAG_BOOL(0, "keep", &keep,
     "Only checkout files from the requested "
     "<version> that do not have a file of the same name already present on "
     "disk. Files with the same name as those from the requested <version> will "
     "remain unmodified irrespective of whether their content is consistent "
     "with that of the requested <version>. In such a case, the checkout will "
                   "immediately be in a changed state, which 'f-status' will report."),
#else
    FCLI_FLAG_BOOL(0, "keep", &keep,
                   "Modified files in the checkout are retained in "
                   "the new checkout version, rather than being "
                   "overwritten. If a locally-modified file becomes "
                   "unmanaged in the new checkout version, it is kept "
                   "in the filesystem regardless of this flag."),
#endif
    FCLI_FLAG_BOOL(0, "manifest", &manifest,
                   "Only generate the manifest and manifest.uuid files "
                   "for the current checkout version."),
    FCLI_FLAG_BOOL_INVERT("Q","not-quiet",&ex.quiet,
                          "When checking out, list all files, not just "
                          "noteworthy changes."),
    FCLI_FLAG_BOOL(0, "setmtime", &mtime, "Set timestamps of all files to that "
                   "of the last check-in in which they were modified "
                   "(i.e., manifest time). FIXME: this does not apply to the "
                   "optional manifest file(s)."),
    FCLI_FLAG_BOOL("n","dry-run",&fDryRun, "Dry-run mode."),
#if 0
    FCLI_FLAG_BOOL_INVERT(0,"no-rm", &doRm,
                          "Do not delete files which were removed between the "
                          "original and new checkout versions. By default, only "
                          "locally-unmodified files will be removed."),
#endif
    fcli_cliflag_empty_m
  };
  fcli_help_info	 fcli_help = {
    "Change the current checkout to the requested <version> or to the tip of "
    "the trunk if no <version> is specified.",
    "[<version>]",
    NULL
  };

  rc = fcli_setup_v2(argc, argv, fcli_flags, &fcli_help);
  if(rc) goto end;

  f = fcli_cx();
  db = fsl_needs_ckout(f);
  if(!db){
    goto end;
  }

  if(fcli_has_unused_flags(0)){
    goto end;
  }

  rc = fsl_cx_txn_begin(f);
  if(rc) goto end;
  fsl_ckout_version_info(f, &prevId, NULL);

  if(manifest){
    if(!prevId){
      rc = fcli_err_set(FSL_RC_RANGE,"This checkout is completely empty, "
                        "so cannot generate manifest.");
      goto end;
    }
    assert(prevId>0);
    rc = write_manifest(f, 1, 1, -1);
    goto end;
  }

  if(prevId>0){
    fsl_vfile_changes_scan(f, prevId, mtime
                           ? FSL_VFILE_CKSIG_SETMTIME
                           : 0);
  }
  if(!force && fsl_ckout_has_changes(f)) {
    fcli_err_set(FSL_RC_MISUSE,
                 "The current checkout contains unsaved changes.");
    goto end;
  }

  if(!(sym = fcli_next_arg(1))){
    char * mainBranch = fsl_config_get_text(f, FSL_CONFDB_REPO,
                                            "main-branch", 0);
    if(mainBranch){
      fcli_fax(mainBranch);
      sym = mainBranch;
    }else{
      sym = "trunk";
    }
  }
  rc = fsl_sym_to_rid(f, sym, FSL_SATYPE_CHECKIN, &rid);
  if(rc) goto end;
  if(prevId == rid){
    f_out("Same version - nothing to do.\n");
    goto end;
  }else{
    fsl_confirmer fcon = fsl_confirmer_empty;
    fcon.callback = fsl_confirm_callback_f_my;
    fcon.callbackState = &ex;
    fsl_cx_confirmer(f, &fcon, NULL);
  }
  ex.f = f;
  ex.coCount = ex.written =
    ex.kept = ex.removed = 0;
  ex.overwriteLocalMods = !keep;
  fsl_ckup_opt cOpt = fsl_ckup_opt_empty;
  cOpt.dryRun = fDryRun;
  cOpt.scanForChanges = false/*we just did a scan*/;
  cOpt.checkinRid = rid;
  cOpt.callback = fsl_ckup_f_my;
  cOpt.callbackState = &ex;
  rc = fsl_repo_ckout(f, &cOpt);
  if(!rc) rc = write_manifest(f, -1, -1, -1);
  if(rc) goto end;

  const char * uuid = 0;
  fsl_ckout_version_info(f, NULL, &uuid);
  if(ex.written || ex.removed){
    f_out("\n");
  }
  f_out("Processed %d file(s) from [%s] [%.16s].\n",
        ex.coCount, sym, uuid);
  if(ex.written){
    f_out("%d SCM'd file(s) [w]ritten to disk.\n", ex.written);
  }
  if(ex.kept){
    f_out("%d file(s) left unchanged on disk.\n", ex.kept);
  }
  if(ex.removed){
    f_out("%d file(s) removed from checkout.\n", ex.removed);
  }
  f_out("\n");
  fcli_ckout_show_info(false);
  if(cOpt.dryRun){
    f_out("Dry-run mode. Rolling back transaction.\n");
    rc = fsl_cx_txn_end(f, true);
  }
  end:
  if(f && fsl_cx_txn_level(f)){
    int const rc2 = fsl_cx_txn_end(f, !!rc);
    rc = rc ? rc : fsl_cx_uplift_db_error2(f, db, rc2);
  }
  return fcli_end_of_main(rc);
}
