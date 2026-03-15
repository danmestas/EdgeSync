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
  *****************************************************************************
  This file implements the code to open and checkout a Fossil repository.
*/

#include "libfossil.h"

#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

static int verbose = 0;

typedef struct {
  int  extracted;
  int  kept;
  bool keep;
  bool quiet;
} extract_state;

static int
fsl_ckup_f_my(fsl_ckup_state const *cuState){
  enum {
    DATE_SHORT = 16
  };
  fsl_repo_extract_state const * xs = cuState->extractState;
  fsl_cx        *f = xs->f;
  extract_state  *state = (extract_state *)cuState->callbackState;
  static char    tbuf[DATE_SHORT];
  int            rc = 0;
  char mode = '?';
  assert(f);
  assert(xs->fCard->uuid);
  switch(cuState->fileChangeType){
    case FSL_CKUP_FCHANGE_UPDATED:
      mode = '+';
      ++state->extracted;
      break;
    case FSL_CKUP_FCHANGE_NONE:
      mode = ' ';
      ++state->kept;
      break;
    default:
      MARKER(("Unexpected FSL_CKUP_FCHANGE value for ckout op: #%d\n",
              cuState->fileChangeType));
      break;
  }
  if(!state->quiet){
    fsl_strftime_unix(tbuf, DATE_SHORT, "%d %b %Y", cuState->mtime, 1);
    f_out("[%c] %8"FSL_SIZE_T_PFMT"   %s  %s\n",
          mode, cuState->size, tbuf, xs->fCard->name);
  }
  return rc;
}

int
main(int argc, char const * const *argv)
{
  fsl_buffer buf = fsl_buffer_empty;
  fsl_db *db = 0;
  fsl_db *dbRepo = 0;
  fsl_cx *f = 0;
  fsl_repo_open_ckout_opt opOpt =
    fsl_repo_open_ckout_opt_empty;
  fsl_id_t rv = 0, prev_ckout = 0;
  const char *repodir =0, *repository =0, *workdir = 0;
  const char *sym = NULL;
  char cwd[FILENAME_MAX];
  bool empty = false, force = false, keep = false,
    mtime = false, nested = false, q = false,
    uri = false;
  int rc = 0;
  extract_state ex;
  memset(&ex, 0, sizeof(extract_state));
  ex.quiet = true;
  fcli_cliflag       const fcli_flags[] = {
    FCLI_FLAG_BOOL("E", "empty", &empty,
     "Initialise checkout as being empty. The checkout will be connected to "
     "the local repository but no files will be written to disk."),
    FCLI_FLAG_BOOL("f", "force", &force, "Continue opening the <repository> if "
     "the working directory is not empty. Files present on disk with the same "
     "name as files from the requested <version> being checked out will be "
     "overwritten, other files will remain untouched."),
    FCLI_FLAG_BOOL(0, "keep", &keep, "Only checkout files from the requested "
     "<version> that do not have a file of the same name already present on "
     "disk. Files with the same name as those from the requested <version> will"
     " remain unmodified irrespective of whether their content is consistent "
     "with that of the requested <version>. In such a case, the checkout will "
     "immediately be in a changed state, which 'f-status' will report."),
#if 0
    // Not implemented
    FCLI_FLAG_BOOL(0, "manifest", &manifest,
     "Only modify the manifest and manifest.uuid files."),
#endif
    FCLI_FLAG_BOOL(0, "nested", &nested,
     "Allow opening the <repository> inside an opened checkout."),
    FCLI_FLAG_BOOL("q", "quiet", &q,
     "Suppress non-error output unless --verbose is used."),
    /*FCLI_FLAG(0, "repodir", "<dir>", &repodir,
     "If <repository> is a URI that will be cloned, store the clone in <dir> "
     "rather than the current working directory."),
    */
    FCLI_FLAG_BOOL(0, "setmtime", &mtime,
                   "Set timestamps of all files to that "
                   "of the last check-in in which they were modified "
                   "(i.e., manifest time)."),
    FCLI_FLAG(0, "workdir", "<dir>", &workdir,
     "Open the checkout into <dir> instead of the current working directory. "
     "This option will create <dir> if it does not already exist."),
    fcli_cliflag_empty_m
  };
  fcli_help_info const fcli_help = {
    "Open a new connection to the <repository>. A checkout of the most recent "
    "check-in is created if no <version> is specified.",
    "<repository> [<version>]",
    NULL
  };

  fcli.config.checkoutDir = NULL
    /* Cannot open checkout yet; we're making the db right now. */;
  rc = fcli_setup_v2(argc, argv, fcli_flags, &fcli_help);
  if(rc) goto end;
  f = fcli_cx();

  verbose = fcli_is_verbose();
  if(!verbose){
    verbose = !q;
  }

  if(fcli_has_unused_flags(0)){
    goto end;
  }

  dbRepo = fsl_cx_db_repo(f);
  if(dbRepo){
    // Was opened via -R flag.
    repository = fsl_cx_db_file_repo(f, 0);
  }else{
    repository = fcli_next_arg(1);
    if (!repository) {
      rc = fcli_err_set(FSL_RC_MISUSE,
                        "Usage: %s [options] <repository> [<version>]\n"
                        "Or try --help", fcli.appName);
      goto end;
    }
    if (fsl_str_glob("http://*", repository)
        || fsl_str_glob("https://*", repository)
        || fsl_str_glob("ssh:*", repository)
        || fsl_str_glob("file:*", repository)){
      uri = true;
      rc = fcli_err_set(FSL_RC_UNSUPPORTED,"URI-style names are "
                        "not currently supported.");
      goto end;
    }
  }
  assert(repository);

  if(workdir && dbRepo){
    MARKER(("FIXME: --workdir is currently incompatible with -R <REPO> "
            "handling because of a mismatch in path translation.\n"))
    /* ^^^ specifically, we need -R to be able to use a relative path
       so that the relative path gets stored in the ckout.vvar.repository
       setting. --workdir will, however, change how a relative path
       is interpreted. */;
  }else if(workdir) {
    char * zTmp = 0;
    assert(!uri && "URI handling is still far away.");
    if (!uri) {
      rc = fsl_file_canonical_name(repository, &buf, 0);
      if(rc) goto end;
      zTmp = fsl_buffer_take(&buf);
      fcli_fax(zTmp);
      repository = zTmp;
    }else if(repodir) {
      assert(!"Not handled until URI support is added");
      rc = fsl_file_canonical_name(repodir, &buf, 0);
      if(rc) goto end;
      zTmp = fsl_buffer_take(&buf);
      fcli_fax(zTmp);
      repodir = zTmp;
    }
    if(fsl_dir_check(workdir) < 1) {
      rc = fsl_mkdir_for_file(workdir, 0);
      if(rc) {
        rc = fcli_err_set(rc, "Cannot create directory [%s].", workdir);
        goto end;
      }
    }
    if ((rc = fsl_chdir(workdir))) {
      rc = fcli_err_set(rc, "Cannot chdir to [%s].",
                        workdir);
      goto end;
    }
  }

  if((rc = fsl_getcwd(cwd, FILENAME_MAX, NULL))){
    rc = fcli_err_set(rc, "Rather unexpectedly cannot get the cwd!\n");
    goto end;
  }
  if (!keep && !force) {
    rc = fsl_dir_is_empty(cwd);
    assert(rc>=0)/*"cannot" be <0 because fsl_getcwd() succeeded a
                   few nanoseconds ago.*/;
    if(rc>0){
      rc = fcli_err_set(FSL_RC_IO, "Directory [%s] is not empty\n"
                        "use the -f|--force option to override", cwd);
      goto end;
    }
  }

  opOpt.checkForOpenedCkout = !nested;
  opOpt.targetDir = cwd;
  opOpt.fileOverwritePolicy = keep ? FSL_OVERWRITE_NEVER : FSL_OVERWRITE_ALWAYS;
  opOpt.dbOverwritePolicy = false;

  if (uri) {
    /*
     * TODO: Implement clone and open when repository arg is a URI.
     */
  }

  if(rc) goto end;
  if(dbRepo){
    db = dbRepo;
  }else{
    FCLI_V(("Opening repository [%s]\n", repository));
    rc = fsl_repo_open(f, repository);
    if(rc) goto end;
    db = dbRepo = fsl_cx_db_repo(f);
    assert(db && "Can't be NULL if fsl_repo_open() succeeded, but..."
           "TODO: add API which confirms that the db handle is-a repo "
           "db in terms of schema.");
  }
  rc = fsl_cx_txn_begin(f);
  if(rc) goto end;
  assert(!fsl_cx_db_ckout(f));
  if(empty){
    if((rc = fcli_has_unused_args(false))) goto end;
  }else{
    /* Check which version to open... */
    sym = fcli_next_arg(true);
    if((rc = fcli_has_unused_args(false))) goto end;
    else if(sym){
      rc = fsl_sym_to_rid(f, sym, FSL_SATYPE_CHECKIN,
                          &rv);
      if(rc) goto end;
    }else{
      /* Try to determine which version to open... */
      char * verCheck = 0;
      char * branch = fsl_config_get_text(f, FSL_CONFDB_REPO,
                                          "main-branch", 0);
      rc = fsl_sym_to_uuid(f, branch ? branch : "trunk",
                           FSL_SATYPE_CHECKIN,
                           &verCheck, &rv);
      fsl_free(branch);
      branch = 0;
      if(rc==FSL_RC_NOT_FOUND){
        assert(!verCheck);
        rc = fsl_sym_to_uuid(f, "tip", FSL_SATYPE_CHECKIN,
                             &verCheck, &rv);
      }
      if(rc==FSL_RC_NOT_FOUND){
        // No checkins in this repo.
        fcli_err_reset();
        assert(0==rv);
        rc = 0;
        empty = true;
        sym = 0;
      }
      if(verCheck){
        fcli_fax(verCheck);
        sym = verCheck;
      }
      if(rc) goto end;
    }
  }
  if((rc = fsl_repo_open_ckout(f, &opOpt))){
    goto end;
  }
  if(empty){
    goto end;
  }
  if(!keep){
    prev_ckout = 0;
  } else{
    fsl_ckout_version_info(f, &prev_ckout, 0);
  }
  assert(dbRepo && dbRepo->dbh);
  if(!fsl_db_exists(dbRepo,"SELECT 1 FROM %s.event WHERE type='ci'",
                    fsl_db_role_name(FSL_DBROLE_REPO))){
    // Assume there are no checkins in this repo, so nothing for us to do.
    f_out("Repo contains no checkins, so there is nothing to check out.\n");
    goto end;
  }
  rc = fsl_sym_to_rid(f, sym, FSL_SATYPE_CHECKIN, &rv);
  if(rc){
    if(FSL_RC_NOT_FOUND==rc){
      f_out("No checkout found for '%s'. This is normal if the repo "
            "has no checkins, else it is an error.\n", sym);
    }
    goto end;
  }
  if(prev_ckout == rv){
    keep = true;  /* Why rewrite to disk? If user wants this they can use -f. */
  }
  ex.keep = keep;
  ex.extracted = 0;
  ex.kept = 0;
  ex.quiet = q;
  fsl_ckup_opt cOpt = fsl_ckup_opt_empty;
  cOpt.checkinRid = rv;
  cOpt.callbackState = &ex;
  cOpt.callback = fsl_ckup_f_my;
  //cOpt.fileOverwritePolicy = keep ? -1 : 1;
  cOpt.setMtime = mtime;
  rc = fsl_repo_ckout(f, &cOpt);
  if(rc) goto end;
  f_out("\nChecked out %d file(s) from %s [RID: %"FSL_ID_T_PFMT"] %z.\n",
        ex.extracted, sym, rv, fsl_rid_to_uuid(f, rv));
  if(ex.kept){
    f_out("%d file(s) left unchanged on disk\n", ex.kept);
  }
  if(rv){
    rc = fcli_ckout_show_info(false);
  }
  end:
  if(f && fsl_cx_txn_level(f)){
    if(rc) fsl_cx_txn_end(f, 1);
    else{
      rc = fsl_cx_txn_end(f, 0);
      rc = fsl_cx_uplift_db_error2(f, db, rc);
    }
  }
  fsl_buffer_clear(&buf);
  return fcli_end_of_main(rc);
}

