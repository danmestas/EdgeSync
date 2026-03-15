/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/
/**
   This application implements the libfossil counterpart of (fossil
   update).
*/
#include "libfossil.h"
#include <string.h> /*memset()*/

// Only for testing/debugging..
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

// Global app state.
struct App_ {
  int32_t dummp;
} App = {
-1//dummy
};

typedef struct {
  fsl_cx * f;
  int  upCount;
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
static int fsl_confirm_callback_f_my(fsl_confirm_detail const * detail,
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


/**
   fsl_repo_ckout() callback. Called once for each row of
   the checkout, including file removals.
*/
static int fsl_ckup_f_my(fsl_ckup_state const *cuState){
  enum {DATE_SHORT = 16};
  //fsl_repo_extract_state const * xs = cuState->extractState;
  extract_state  *state = (extract_state *)cuState->callbackState;
  char    tbuf[DATE_SHORT];
  char const * mode = "??";
  bool noteworthy = true;
  char const * nonTimestamp = 0;
  ++state->upCount;
  switch(cuState->fileChangeType){
    case FSL_CKUP_FCHANGE_NONE: mode = "  "; noteworthy = false;
      ++state->kept;
      break;
    case FSL_CKUP_FCHANGE_ADDED: mode = "+ ";
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
    case FSL_CKUP_FCHANGE_UPDATED: mode = "u ";
      ++state->written;
      break;
    case FSL_CKUP_FCHANGE_UPDATED_BINARY: mode = "ub";
      ++state->written;
      break;
    case FSL_CKUP_FCHANGE_MERGED: mode = "m ";
      ++state->written;
      break;
    case FSL_CKUP_FCHANGE_CONFLICT_MERGED: mode = "m!";
      ++state->written;
      break;
    case FSL_CKUP_FCHANGE_CONFLICT_ADDED: mode = "+M";
      ++state->kept;
      break;
    case FSL_CKUP_FCHANGE_CONFLICT_ADDED_UNMANAGED: mode = "+!";
      ++state->written;
      break;
    case FSL_CKUP_FCHANGE_CONFLICT_RM: mode = "-M";
      ++state->removed;
      ++state->kept;
      nonTimestamp = "<UNMANAGED>";
      break;
    case FSL_CKUP_FCHANGE_CONFLICT_SYMLINK: mode = "L!";
      ++state->written;
      break;
    case FSL_CKUP_FCHANGE_RENAMED: mode = "n ";
      ++state->written;
      break;
    case FSL_CKUP_FCHANGE_EDITED: mode = "e ";
      ++state->kept;
      break;
    case FSL_CKUP_FCHANGE_ADD_PROPAGATED: mode = "++";
      ++state->kept;
      break;
    case FSL_CKUP_FCHANGE_RM_PROPAGATED: mode = "--";
      ++state->kept;
      break;
    case FSL_CKUP_FCHANGE_INVALID:
    default:
      MARKER(("Unexpected FSL_CKUP_FCHANGE value: #%d\n",
              cuState->fileChangeType));
      //fsl__fatal(FSL_RC_UNSUPPORTED,
      //        "Invalid passing-on of FSL_CKUP_FCHANGE_INVALID.");
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


int main(int argc, const char * const * argv ){
  bool latest = false;
  bool forceMissing = false;
  bool setMTime = false;
  bool fDryRun = false;
  bool fAutoSync = true;
  char const * sym = NULL;
  fsl_id_t tgtRid = 0;
  fsl_id_t ckRid = 0;
  fsl_cx * f = 0;
  extract_state exs;
  memset(&exs, 0, sizeof(extract_state));
  exs.quiet = true;
  /**
     Set up flag handling, which is used for processing
     basic CLI flags and generating --help text output.
  */
  const fcli_cliflag FCliFlags[] = {
    // FCLI_FLAG_xxx macros are convenience forms for initializing
    // these members...
    FCLI_FLAG_BOOL(0,"latest",&latest,
                   "Pick the most recent version, regardless of branch. "
                   "Same as passing a version of 'tip'."),
    FCLI_FLAG_BOOL(0, "setmtime", &setMTime,
                   "Set timestamps of all updated files to that "
                   "of the last check-in in which they were modified "
                   "(i.e., manifest time)."),
    FCLI_FLAG_BOOL(0,"force-missing",&forceMissing,
                   "Perform update even if some content is missing."),
    FCLI_FLAG_BOOL("n", "dry-run", &fDryRun, "Dry-run mode."),
    FCLI_FLAG_BOOL_INVERT("Q","not-quiet",&exs.quiet,
                          "When updating, list all files, not just "
                          "noteworthy changes."),
    FCLI_FLAG_BOOL_INVERT(NULL,"no-autosync", &fAutoSync,
                          "Disable using the system's fossil(1) binary "
                          "for autosync. The boolean config setting "
                          "'fcli.autosync' can be used to change the "
                          "default behavior."),
    FCLI_FLAG("v","version","version",&sym,
              "Version to update to. Defaults to the current version of "
              "the current branch. May optionally be passed as the first "
              "non-flag argument."),
    fcli_cliflag_empty_m // list MUST end with this (or equivalent)
  };
  const fcli_help_info FCliHelp = {
    "Updates a fossil checkout to another version, merging any local "
    "changes into the updated-to version.",
    "[version]", // very brief usage text
    NULL // optional callback which outputs app-specific help
  };

  int rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  f = fcli_cx();
  if(!fsl_needs_ckout(f)){
    goto end;
  }
  if(fDryRun){
    f_out("Dry-run mode is ON.\n");
  }
  if(!sym) sym = fcli_next_arg(true);
  if((rc=fcli_has_unused_args(false))) goto end;
  //if(fDryRun) fAutoSync = false;
  if(fAutoSync){
    rc = fcli_sync(FCLI_SYNC_PULL | FCLI_SYNC_AUTO);
    if(rc){
      f_out("Pre-update autosync failed with code %s.\n",
            fsl_rc_cstr(rc));
      goto end;
    }
  }
  fsl_ckout_version_info(f, &ckRid, NULL);
  rc = fsl_cx_txn_begin(f);
  if(rc) goto end;

  if(sym){
    if(0==fsl_strcmp("current", sym)){
      /* Same as not passing a version */
      //sym = 0;
    }else if(0==fsl_strcmp("latest", sym)){
      /* Same as passing --latest */
      sym = "tip";
      latest = true;
    }else{
      rc = fsl_sym_to_rid(f, sym, FSL_SATYPE_CHECKIN, &tgtRid);
      switch(rc){
        case 0: break;
        case FSL_RC_AMBIGUOUS:
          fcli_list_ambiguous_artifacts(NULL,sym);
          goto end;
        default: goto end;
      }
    }
  }else{
    sym = "current";
  }

  if( !tgtRid ){
    rc = latest
      ? fsl_sym_to_rid(f, "tip", FSL_SATYPE_CHECKIN, &tgtRid)
      : fsl_ckout_calc_update_version(f, &tgtRid);
    if(rc) goto end;
    else if( !tgtRid ) tgtRid = ckRid;
  }

  if( !tgtRid ){
    rc = fcli_err_set(FSL_RC_RANGE,
                      "Cannot figure out which version "
                      "to update to. :/");
    goto end;
  }else if( tgtRid == ckRid ){
    f_out("Updating to checkout version.\n");
  }

  f_out("Version to update to: %.12z (RID %" FSL_ID_T_PFMT ")\n",
        fsl_rid_to_uuid(f, tgtRid), tgtRid);

  fsl_ckup_opt uOpt = fsl_ckup_opt_empty;
  fsl_confirmer fcon = fsl_confirmer_empty;
  assert(uOpt.scanForChanges);

  exs.f = f;
  uOpt.dryRun = fDryRun;
  uOpt.checkinRid = tgtRid;
  uOpt.callback = fsl_ckup_f_my;
  uOpt.callbackState = &exs;
  uOpt.setMtime = setMTime;
  fcon.callback = fsl_confirm_callback_f_my;
  fcon.callbackState = &exs;
  fsl_cx_confirmer(f, &fcon, NULL);

  rc = fsl_ckout_update(f, &uOpt);
  if(rc) goto end;
  const char * uuid = 0;
  fsl_ckout_version_info(f, NULL, &uuid);
  f_out("\nProcessed %u file(s) from [%s] [%.16s].\n",
        exs.upCount, sym, uuid);
  if(exs.written){
    f_out("%d SCM'd file(s) written to disk.\n", exs.written);
  }
  if(exs.kept){
    f_out("%d file(s) left unchanged on disk.\n", exs.kept);
  }
  if(exs.removed){
    f_out("%d file(s) removed from checkout.\n", exs.removed);
  }
  f_out("\n");
  fcli_ckout_show_info(false);
  if(fDryRun){
    f_out("Dry-run mode. Rolling back.\n");
    rc = fsl_cx_txn_end(f, true);
  }
  end:
  if(f && fsl_cx_txn_level(f)){
    int const rc2 =
      fsl_cx_txn_end(f, fDryRun || rc);
    if(!rc) rc = rc2;
  }
  return fcli_end_of_main(rc);
}
