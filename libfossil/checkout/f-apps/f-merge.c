/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code
*/
/**
   This applications implements operations merge operations, like
   (fossil merge) does.
*/
#include "libfossil.h"
#include <string.h> /* memset() */

// Only for testing/debugging..
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

// Global app state.
struct App_ {
  bool flagIntegrate;
  bool flagCherrypick;
  bool flagBackout;
  bool flagDebug;
  char const * flagBaseline;
  fsl_merge_opt mopt;
  struct AppCkout {
    fsl_id_t rid;
    fsl_uuid_cstr uuid;
  } ckout;
  unsigned int mCounts[FSL_MERGE_FCHANGE_count];
} App = {
false,//flagIntegrate
false,//flagCherrypick
false,//flagBackout
false,//flagDebug
NULL,//flagBaseline
fsl_merge_opt_empty_m,
{/*ckout*/0,NULL},
{/*mCounts*/0}
};

static char const * fsl__merge_fchange_label(fsl_merge_fchange_e fce){
  char const * rc = "???";
  switch(fce){
    case FSL_MERGE_FCHANGE_ADDED: rc="+"; break;
    case FSL_MERGE_FCHANGE_COPIED: rc="c"; break;
    case FSL_MERGE_FCHANGE_RM: rc="-"; break;
    case FSL_MERGE_FCHANGE_MERGED: rc="m"; break;
    case FSL_MERGE_FCHANGE_CONFLICT_MERGED: rc="!m"; break;
    case FSL_MERGE_FCHANGE_CONFLICT_ADDED_UNMANAGED: rc="!+"; break;
    case FSL_MERGE_FCHANGE_CONFLICT_SYMLINK: rc="!s"; break;
    case FSL_MERGE_FCHANGE_CONFLICT_BINARY: rc="!b"; break;
    case FSL_MERGE_FCHANGE_RENAMED: rc="r"; break;
    case FSL_MERGE_FCHANGE_count:
    case FSL_MERGE_FCHANGE_NONE: rc="!!!";
      fsl__fatal(FSL_RC_NYI,"Cannot happen: FSL_MERGE_FCHANGE_NONE");
      break;
  }
  return rc;
}

/**
   App-local fsl_merge_f() callback.
 */
static int fsl_merge_f_mine(fsl_merge_state const * const ms){
  int rc = 0;
  char const *zLabel = fsl__merge_fchange_label(ms->fileChangeType);
  ++App.mCounts[ms->fileChangeType];
  ++(*((int*)ms->opt->callbackState));
  if(ms->priorName){
    f_out("[%-3s] %s\n   -> %s\n", zLabel, ms->priorName, ms->filename);
  }else{
    f_out("[%-3s] %s\n", zLabel, ms->filename);
  }
  return rc;
}


int main(int argc, const char * const * argv ){
  fsl_cx * f = 0;
  bool fAutoSync = true;
  int rc = 0;
  char const *zMergeSym = 0;
  const fcli_cliflag FCliFlags[] = {
    FCLI_FLAG_BOOL(NULL,"integrate",&App.flagIntegrate,
                   "Perform an integrate merge."),
    FCLI_FLAG_BOOL(NULL,"cherrypick",&App.flagCherrypick,
                   "Perform a cherrypick merge."),
    FCLI_FLAG_BOOL(NULL,"backout",&App.flagBackout,
                   "Perform a backout (reverse cherrypick) merge."),
    FCLI_FLAG_BOOL("n", "dry-run", &App.mopt.dryRun,
                   "Enable dry-run mode."),
    FCLI_FLAG_BOOL(NULL, "debug", &App.mopt.debug,
                   "Maybe enable some debug output."),
    FCLI_FLAG(NULL,"baseline","version",&App.flagBaseline,
              "Use this version as the \"pivot\" of the merge "
              "instead of the nearest common ancestor."),
    FCLI_FLAG_BOOL_INVERT(NULL,"no-autosync", &fAutoSync,
                          "Disable using the system's fossil(1) binary "
                          "for autosync. The boolean config setting "
                          "'fcli.autosync' can be used to change the "
                          "default behavior."),
    fcli_cliflag_empty_m // list MUST end with this (or equivalent)
  };
  const fcli_help_info FCliHelp = {
    "Merges a repository-side version into the current checkout.",
    NULL, // very brief usage text, e.g. "file1 [...fileN]"
    NULL // optional callback which outputs app-specific help
  };

  rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  if(App.mopt.dryRun){
    f_out("DRY RUN MODE. Use --wet-run to disable dry-run mode.\n");
  }
  while(fcli_flag("debug", NULL)){
    ++App.mopt.debug;
  }
  if(fcli_has_unused_flags(false)) goto end;

  f = fcli_cx();
  if(!fsl_needs_ckout(f)) goto end;
  if(fAutoSync){
    rc = fcli_sync(FCLI_SYNC_PULL | FCLI_SYNC_AUTO);
    if(rc){
      f_out("Pre-merge autosync failed with code %s.\n",
            fsl_rc_cstr(rc));
      goto end;
    }
  }

  if(App.flagIntegrate + App.flagCherrypick + App.flagBackout > 1){
    rc = fcli_err_set(FSL_RC_MISUSE,
                      "Only one of --integrate, --cherrypick, or "
                      "--backout may be used.");
    goto end;
  }
  rc = fsl_cx_txn_begin(f);
  if(rc) goto end;

  fsl_ckout_version_info(f, &App.ckout.rid, &App.ckout.uuid);
  if(!App.ckout.rid){
    rc = fcli_err_set(FSL_RC_MISUSE,
                      "Cannot merge into empty top-level checkin.");
    goto end;
  }
  assert(App.ckout.rid>0 && "libfossil internal API misuse?");

  zMergeSym = fcli_next_arg(true);
  if(!zMergeSym){
    rc = fcli_err_set(FSL_RC_MISUSE, "Expecting version to merge from.");
    goto end;
  }
  if((rc=fcli_has_unused_args(false))) goto end;

  if(App.flagIntegrate) App.mopt.mergeType = FSL_MERGE_TYPE_INTEGRATE;
  else if(App.flagCherrypick) App.mopt.mergeType = FSL_MERGE_TYPE_CHERRYPICK;
  else if(App.flagBackout) App.mopt.mergeType = FSL_MERGE_TYPE_BACKOUT;
  else{
    assert(FSL_MERGE_TYPE_NORMAL == App.mopt.mergeType);
  }

  rc = fsl_sym_to_rid(f, zMergeSym, FSL_SATYPE_CHECKIN,
                      &App.mopt.mergeRid);
  symfail:
  switch(rc){
    case 0: break;
    case FSL_RC_AMBIGUOUS:
      /* In the libf tree, use zMergSym = c1d7a2 to trigger this. */
      fcli_list_ambiguous_artifacts(0, zMergeSym);
      goto end;
    default:
      goto end;
  }
  if(App.flagBaseline){
    rc = fsl_sym_to_rid(f, App.flagBaseline, FSL_SATYPE_CHECKIN,
                        &App.mopt.baselineRid);
    if(rc){
      zMergeSym = App.flagBaseline;
      goto symfail;
    }
  }

  f_out("Merging [%s] into [%S]...\n", zMergeSym, App.ckout.uuid);
  int callbackCount = 0;
  App.mopt.callback = fsl_merge_f_mine;
  App.mopt.callbackState = &callbackCount;
  memset(App.mCounts, 0, sizeof(App.mCounts));
  rc = fsl_ckout_merge(f, &App.mopt);
  if(0==rc && callbackCount){
    /* Output legend... */
    struct legend {
      fsl_merge_fchange_e const fct;
      char const * altSym;
      char const *desc;
    } leg[] = {
      {FSL_MERGE_FCHANGE_ADDED, 0, "Added"},
      {FSL_MERGE_FCHANGE_COPIED, 0, "Copied"},
      {FSL_MERGE_FCHANGE_MERGED, 0, "Merged"},
      {FSL_MERGE_FCHANGE_RM, 0, "Removed"},
      {FSL_MERGE_FCHANGE_RENAMED, 0, "Renamed"},
      {FSL_MERGE_FCHANGE_RENAMED, "->", "New name of rename"},
      {FSL_MERGE_FCHANGE_CONFLICT_ADDED_UNMANAGED, 0,
       "Added, overwriting an unmanaged file"},
      {FSL_MERGE_FCHANGE_CONFLICT_MERGED, 0,
       "Merged with conflicts"},
      {FSL_MERGE_FCHANGE_CONFLICT_SYMLINK, 0,
       "Symlink cannot be merged"},
      {FSL_MERGE_FCHANGE_CONFLICT_BINARY, 0,
       "Binary content cannot be merged"},
      {FSL_MERGE_FCHANGE_NONE,NULL,NULL}
    };
    f_out("\nLEGEND:\n");
    for(struct legend * l = &leg[0]; l->desc; ++l){
      if(App.mCounts[l->fct]){
        f_out("   %-3s    %s\n",
              l->altSym ? l->altSym : fsl__merge_fchange_label(l->fct),
              l->desc);
      }
    }
    f_out("\n");
  }
  //f_out("fsl_ckout_merge() rc=%s\n", fsl_rc_cstr(rc));
  if(0==rc){
    if(App.mopt.dryRun){
      f_out("Dry-run mode: rolling back transaction.\n");
    }
    rc = fsl_cx_txn_end(f, App.mopt.dryRun);
  }
  end:
  if(f && fsl_cx_txn_level(f)){
    f_out("Rolling back transaction.\n");
    fsl_cx_txn_end(f, true);
  }
  return fcli_end_of_main(rc);
}
