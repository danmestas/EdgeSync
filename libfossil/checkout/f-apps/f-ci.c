/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).

  *****************************************************************************
  This file implements a checkin [test] app using the libfossil API.
*/

#include "libfossil.h" /* Fossil App mini-framework */
#include <ctype.h>

/* Only for debugging */
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

static struct App_ {
  const char * tagArg;
  fsl_list liTags/* (fsl_card_T*) */;
} App = {
0,
fsl_list_empty_m
};

// Handler for --tag name[=value] flags
static int fcli_flag_callback_tag(fcli_cliflag const *f){
  assert(App.tagArg==*((char const **)f->flagValue));
  int rc = 0;
  char * t = 0;
  char * v = 0;
  char * arg = fsl_strdup(App.tagArg);
  const char * z = arg;
  App.tagArg = 0;
  for( ; *z && '='!=*z; ++z){}
  if('='==*z){
    fcli_fax(arg);
    t = fsl_strndup(arg, (fsl_int_t)(z-arg));
    if(z[1]) v = fsl_strdup(z+1);
  }else{
    t = arg;
  }
  char * tOrig = t;
  fsl_tagtype_e ttype = FSL_TAGTYPE_INVALID;
  switch(*t){
    case '-':
      rc = fcli_err_set(FSL_RC_MISUSE,
                        "Cannot use CANCEL-type tags in checkins.");
      goto end;
    case '*': ttype = FSL_TAGTYPE_PROPAGATING; ++t; break;
    case '+':
      ++t;
      /* fall through */
    default:
      ttype = FSL_TAGTYPE_ADD;
      break;
  }
#if 0
  if(0==fsl_strncmp("sym-",t, 4)){
    rc = fcli_err_set(FSL_RC_MISUSE,
                      "Do not set sym-XYZ tags (used for branching) "
                      "with the --tag flag: they will not be properly "
                      "processed as sym-tags. Use --branch=name to "
                      "add this checkin to a new branch and cancel "
                      "its previous branch tag.");
    goto end;
  }
#else
  if(0==fsl_strncmp("sym-",t, 4)){
    if(!t[4]){
      rc = fcli_err_set(FSL_RC_MISUSE,
                        "Missing suffix for sym- tag.");
    }else if(v){
      rc = fcli_err_set(FSL_RC_MISUSE,
                        "In a checkin, sym- tags may not "
                        "have a value.");
    }else if(FSL_TAGTYPE_PROPAGATING==ttype){
      /* We disallow propagating sym- tags here (1) for compatibility
         with how (fossil ci --tag) works and because it's unclear how
         they (mis?)interact with branch tags. */
      rc = fcli_err_set(FSL_RC_MISUSE,
                        "In a checkin, sym- tags may not "
                        "be propagating.");
    }
    if(rc) goto end;
 }
#endif
  {
    fsl_card_T * tt = fsl_card_T_malloc(ttype, 0, t, v);
    fsl_list_append(&App.liTags, tt)
      /*reminder: fcli will crash on alloc error*/;
  }
  end:
  fsl_free(tOrig);
  fsl_free(v);
  return rc ? rc : FCLI_RC_FLAG_AGAIN;
}

/**
    Just experimenting with fsl_xlink_listener() and friends.
*/
static int my_xlink_f(fsl_deck * d, void * state fsl__unused){
  (void)state;
  FCLI_V(("Crosslink callback for %s artifact RID %" FSL_ID_T_PFMT "\n",
           fsl_satype_cstr(d->type), d->rid));
  return 0;
}

/** fsl_checkin_queue_f callback */
static int fsl_checkin_queue_f_my(const char * filename, void * state){
  ++*((fsl_size_t*)state);
  f_out("QUEUED: %s\n", filename);
  return 0;
}

/*
Reminder: name of a file by its content RID:

SELECT fn.name
FROM filename fn, mlink ml
WHERE fn.fnid=ml.fnid
AND ml.fid=$contentRid
*/
int main(int argc, char const * const * argv ){
  int rc = 0;
  fsl_cx * f = 0;
  fsl_db * db = 0;
  fsl_id_t ckoutId = 0;
  const char * cMsg = NULL;
  const char * cBranch = NULL;
  const char * fname = NULL;
  const char * cMimeType = NULL;
  const char * cDumpMf = NULL;
  const char * cColor = NULL;
  const char * cTimestamp = "now";
  fsl_checkin_opt cOpt = fsl_checkin_opt_empty;
  fsl_id_t newRid = 0;
  fsl_uuid_str newUuid = NULL;
  fsl_size_t fileArgCount = 0;
  fsl_size_t enqueuedArgCount = 0;
  fsl_size_t i = 0;
  bool fNoRCard = false;
  bool fBaseline = false;
  bool fDryRun = false;
  bool fAutoSync = true;
  bool fAllowFork = false;
  bool fAllowOlder = false;
  fsl_id_bag bagCheck = fsl_id_bag_empty
    /* to verify that we know about any given filename/wildcard */;
  fcli_pre_setup()/*allocator setup we need for arg processing*/;
  fcli_cliflag FCliFlags[] = {
    FCLI_FLAG("m","message","text",&cMsg, "Commit message."),
    FCLI_FLAG("b","branch","branch-name",&cBranch, "New branch name."),
    FCLI_FLAG_BOOL(NULL,"private",&cOpt.isPrivate,
                   "Mark the checkin as private. This is only permitted when the "
                   "--branch flag is used, else it is ignored."),
    FCLI_FLAG_BOOL("n","dry-run", &fDryRun,"Dry-run mode."),
    FCLI_FLAG_BOOL("i","integrate",&cOpt.integrate,
                   "Close all merge-parent branches, not just "
                   "integrate-merge branches."),
    FCLI_FLAG_BOOL(0,"fork", &fAllowFork,
                   "Permit a checkin to a non-leaf version."),
    FCLI_FLAG_X("t","tag","name[=value]",&App.tagArg,
                fcli_flag_callback_tag,
                "Adds the given tag with an optional value. "
                "May be used multiple times. "
                "Prefix the name with * for a propagating tag. "
                "A prefix of + (add tag) is equivalent to no prefix. "
                "May NOT be used to remove flags, so tag names must "
                "not start with '-'."),
    FCLI_FLAG_BOOL_INVERT(NULL,"no-autosync", &fAutoSync,
                          "Disable using the system's fossil(1) binary "
                          "for autosync. The boolean config setting "
                          "'fcli.autosync' can be used to change the "
                          "default behavior."),
    FCLI_FLAG(NULL,"date-override","date-string", &cTimestamp,
              "Timestamp for the checkin (default=current time). "
              "Use any time format supported by SQLite3's julianday()"),
    FCLI_FLAG_BOOL(NULL,"allow-older", &fAllowOlder,
                   "Permit the checkin to have an older timestamp than its parent. "
                   "By default this is prohibited to avoid time warps."),
    FCLI_FLAG_BOOL(0,"baseline",&fBaseline,
                   "Force creation of a baseline checkin artifact, "
                   "not a delta. Default is to use automatic "
                   "determination."),
    FCLI_FLAG_BOOL("r","no-r-card",&fNoRCard,
                   "Disable calculation of the R-card."),
    FCLI_FLAG_BOOL(0,"allow-merge-conflict",&cOpt.allowMergeConflict,
                   "Permit the checkin even if a file contains a merge "
                   "conflict marker."),
    FCLI_FLAG("bg","bg-color","color", &cColor,
              "Timeline entry background color. It's generally best to let "
              "fossil decide this itself so that it can account for "
              "light vs dark site color schemes."),
    FCLI_FLAG("d","dump","filename",&cDumpMf,
              "Dump generated artifact to this file."),
    FCLI_FLAG("mt","mime-type","mimetype",&cMimeType,
              "Mime type of the checkin message. ONLY FOR TESTING: "
              "Fossil currently only supports fossil-wiki-format messages."),
    fcli_cliflag_empty_m
  };
  fcli_help_info FCliHelp = {
  "Performs a checkin of local changes.",
  "[file1...fileN]",
  NULL
  };

  rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  cOpt.calcRCard = !fNoRCard;
  f = fcli_cx();
  db = fsl_cx_db_ckout(f);
  if(!db){
    rc = fcli_err_set(FSL_RC_NOT_A_CKOUT,
                      "This app requires a checkout db.");
    goto end;
  }
  if( !cBranch && cOpt.isPrivate ){
    rc = fcli_err_set(
      FSL_RC_MISUSE,
      "--private may only be used with the --branch flag."
      /* We're being somewhat pedantic/restrictive here solely because
         my understanding of private checkins is too little to be sure
         of the implications of marking a checkin private in the
         middle of a public branch. Fossil, at a cursory glance,
         appears to default to a branch name of "private" if
         --branch=NAME is not used. */
    );
    goto end;
  }

  if(fcli_has_unused_flags(false)) goto end;

  if(!cMsg){
    rc = fcli_err_set(FSL_RC_MISUSE,
                      "Commit message (-m|--message MSG) is required.");
    goto end;
  }else{
    while(isspace(*cMsg)){
      /*
        This is part of a dumb workaround for:

        f-ci -m "-msg starting with a minus"

        Which generates an error:

        f-ci --dry-run -m "-msg with leading dash"
        ./f-ci: ERROR #103 (FSL_RC_MISUSE): Missing value for flag [m].

        We work around that by adding a space:

        f-ci -m " -msg starting with a minus"

        And then strip that space here.

        _Sigh_.
      */
      ++cMsg;
    }
  }
  fsl_xlink_listener( f, fcli.appName, my_xlink_f, NULL );
  cOpt.user = fsl_cx_user_get(f);
  if(!cOpt.user){
    rc = fcli_err_set(FSL_RC_MISUSE,
                      "Could not figure out user name to commit as.");
    goto end;
  }
  if(fBaseline) cOpt.deltaPolicy = 0;
  fsl_ckout_version_info(f, &ckoutId, NULL);
  //if(fDryRun) fAutoSync = false;
  if(ckoutId && fAutoSync){
    rc = fcli_sync(FCLI_SYNC_PULL | FCLI_SYNC_AUTO);
    if(rc){
      f_out("Pre-checkin autosync failed with code %s.\n",
            fsl_rc_cstr(rc));
      goto end;
    }
    /* TODO: the "multi-leaf" check, fail if we find one, and add a
       CLI flag to permit that case to succeed. */
  }

  rc = fsl_cx_txn_begin(f)
    /* recall that we can't sync if a transaction is open: the transaction
       locks the sync out. */;
  if(rc) goto end;
  rc = fcli_fingerprint_check(true);
  if(rc) goto end;
  fsl_ckout_version_info(f, &ckoutId, NULL)
    /* re-load ckout id now that we're in a transaction */;
  if(ckoutId && !fAllowFork && !cBranch
     && !fsl_rid_is_leaf(f, ckoutId)){
    rc = fcli_err_set(FSL_RC_WOULD_FORK,
                      "Attempting to check in to a non-leaf version. "
                      "Aborting to avoid what is presumably an "
                      "unintended fork. Use the --fork flag to skip "
                      "this check.");
    goto end;
  }
  rc = fsl_vfile_changes_scan(f, ckoutId, 0);
  if(rc) goto end;
  if(!ckoutId && !cBranch){
    /* When checking in to a repo which has no checkins, and no branch
       is specified, default to trunk branch. If we don't then we end
       up with a branchless checkin, leading to downstream confusion
       (but it's otherwise harmless). */
    cBranch = "trunk";
  }
  cOpt.message = cMsg;
  cOpt.messageMimeType = cMimeType;
  cOpt.dumpManifestFile = cDumpMf;
  cOpt.bgColor = cColor;
  cOpt.scanForChanges = false /*we'll do this ourselves*/;
  cOpt.branch = cBranch;
#if 0
  /*
    Does not yet work properly. Symptom is that dry-run commits
    end with something like:

    ERROR #107 (FSL_RC_NOT_FOUND): No blob found for rid 16663.
  */
  cOpt.dryRun = fDryRun;
#endif
  fsl_checkin_queue_opt qOpt = fsl_checkin_queue_opt_empty;
  qOpt.relativeToCwd = true;
  qOpt.callback = fsl_checkin_queue_f_my;
  qOpt.callbackState = &enqueuedArgCount;
  qOpt.scanForChanges = false /* we just did this */;
  qOpt.vfileIds = &bagCheck;
  while((fname = fcli_next_arg(true))){
    ++fileArgCount;
    /* Verify that fname maps to a file or directory we know about, to
       avoid potential confusion (and a commit) when a user mis-types
       a name... */
    fsl_id_bag_reuse(&bagCheck);
    rc = fsl_ckout_vfile_ids(f, ckoutId, &bagCheck, fname,
                             true, true);
    if(rc) goto end;
    else if(!fsl_id_bag_count(&bagCheck)){
      /* Ambiguous situation: we don't know whether the user passed
         in an unknown path or one which has no changes. Do determine
         which it is, we have to try again and include unchanged
         files in the result... */
      rc = fsl_ckout_vfile_ids(f, ckoutId, &bagCheck, fname,
                               true, false);
      if(0==rc && !fsl_id_bag_count(&bagCheck)){
        rc = fcli_err_set(FSL_RC_UNKNOWN_RESOURCE,
                          "Unknown/unmanaged file: %s", fname);
      }
      if(rc) goto end;
      /* This was a managed dir or file with no changes. Fall through
         and continue looping... but we need to reset the bag here
         in case this is the last loop iteration, in order to prevent
         enqueueing its contents. */
      fsl_id_bag_reuse(&bagCheck);
    }else{
      rc = fsl_checkin_enqueue( f, &qOpt );
      if(rc) goto end;
    }
  }
  if( !fileArgCount ){
    /* No file args provided - use the whole checkout */
    qOpt.filename = ".";
    qOpt.relativeToCwd = false;
    qOpt.vfileIds = NULL;
    rc = fsl_checkin_enqueue( f, &qOpt );
    if(rc) goto end;
  }

  if(fileArgCount && !enqueuedArgCount){
    rc = fcli_err_set(FSL_RC_NOOP,
                      "No files queued up. No changes to commit.");
    goto end;
  }else if(fcli_is_verbose()>1){
    f_out("vfile selected contents:\n");
    fsl_db_each( fsl_cx_db_ckout(f), fsl_stmt_each_f_dump, f,
                 "SELECT vf.id, substr(b.uuid,0,8) hash, chnged, "
                 "deleted, isexe, vf.pathname "
                 "FROM vfile vf LEFT JOIN blob b "
                 "ON b.rid=vf.rid "
                 "WHERE vf.vid=%"FSL_ID_T_PFMT" "
                 "AND (chnged<>0 OR pathname<>origname)"
                 "AND fsl_is_enqueued(vf.id) "
                 "ORDER BY vf.id", ckoutId);
    f_out("f->ckin.selectedIds count: %d\n",
          (int)f->ckin.selectedIds.entryCount);
  }

  for( i = 0; i < App.liTags.used; ++i ){
    fsl_card_T * tc = (fsl_card_T *)App.liTags.list[i];
    App.liTags.list[i] = 0;
    assert(!tc->uuid);
    if(rc){
      fsl_card_T_free(tc);
      continue;
    }
    rc = fsl_checkin_T_add2(f, tc);
    if(rc) fsl_card_T_free(tc);
  }
  if(rc) goto end;
  cOpt.julianTime = fsl_db_string_to_julian( fsl_cx_db(f), cTimestamp );
  if( cOpt.julianTime<=0 ){
    rc = fcli_err_set(FSL_RC_MISUSE, "Invalid checkin time: %s", cTimestamp);
    goto end;
  }
  if( ckoutId && !fAllowOlder ){
    fsl_deck parent = fsl_deck_empty;
    rc = fsl_deck_load_rid(f, &parent, ckoutId, FSL_SATYPE_CHECKIN);
    if( 0==rc && parent.D >= cOpt.julianTime ){
      char tParent[32];
      char tCi[32];
      fsl_julian_to_iso8601(parent.D, tParent, false);
      fsl_julian_to_iso8601(cOpt.julianTime, tCi, false);
      rc = fcli_err_set(FSL_RC_RANGE,
                        "Parent checkin is more recent (%s) than this checkin (%s). "
                        "Use --allow-older to permit this.",
                        tParent, tCi);
    }
    fsl_deck_finalize(&parent);
    if( rc ) goto end;
  }
  rc = fsl_checkin_commit(f, &cOpt, &newRid, &newUuid);
  if(rc) goto end;
  f_out("New version: %s (%"FSL_ID_T_PFMT")\n",
        newUuid, newRid);

  if(fcli_is_verbose()>1){
    f_out("Post-commit vfile changed contents:\n");
    fsl_db_each( fsl_cx_db_ckout(f), fsl_stmt_each_f_dump, f,
                 "SELECT vf.vid, vf.id, substr(b.uuid,0,8) hash, chnged, "
                 "deleted, vf.pathname "
                 "FROM vfile vf LEFT JOIN blob b "
                 "ON b.rid=vf.rid "
                 "WHERE vf.vid=%"FSL_ID_T_PFMT" "
                 "AND ("
                 "  chnged<>0 OR pathname<>origname OR deleted<>0"
                 "  OR vf.rid=0 "
                 "  OR (origname IS NOT NULL AND origname<>pathname)"
                 ")"
                 "ORDER BY vf.id", newRid);
  }
  //For testing merge-conflict marker detection, start a non-comment
  //line with:
  //<<<<<<< BEGIN MERGE CONFLICT: local copy shown first <<<<<<<<<<<<<<<
  if(0){
    f_out("vfile contents:\n");
    fsl_db_each( fsl_cx_db_ckout(f), fsl_stmt_each_f_dump, f,
                 "SELECT * from vfile "
                 "WHERE vid=%"FSL_ID_T_PFMT
                 " AND ("
                 "  chnged OR deleted "
                 "  OR (origname IS NOT NULL AND origname<>pathname)"
                 "  OR rid=0"
                 " )"
                 " ORDER BY pathname",
                 (fsl_id_t)newRid );
  }

  assert( 0==rc );
  rc = fsl_cx_txn_end_v2(f, !fDryRun, false);
  if(fDryRun){
    f_out("Dry-run mode. Rolling back transaction.\n");
    /* Roll back manifest file changes to avoid potential
       major confusion later... */
    //f_out("Re-writing manifest file(s) for dry-run mode.\n");
    //fsl_ckout_manifest_write(f, -1, -1, -1, 0);
  }

  end:
  fsl_id_bag_clear(&bagCheck);
  if(f && fsl_cx_txn_level(f)!=0){
    assert(rc);
    fsl_cx_txn_end_v2(f, false, true);
  }
  for( i = 0; i < App.liTags.used; ++i ){
    fsl_card_T_free((fsl_card_T *)App.liTags.list[i]);
  }
  fsl_list_reserve(&App.liTags, 0);
  fsl_free(newUuid);
  if(0==rc && fAutoSync && !fDryRun){
    rc = fcli_sync(FCLI_SYNC_FULLAUTO);
  }
  return fcli_end_of_main(rc);
}

#undef MARKER
