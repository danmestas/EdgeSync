/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).

  *****************************************************************************
  A test/demo app for creating new fossil repository DBs using the libfossil
  API.
*/

#include "libfossil.h"
#include <time.h>
#include <string.h>

static struct FNewApp_{
  const char * comment;
  const char * commentMimetype;
  const char * filename;
  const char * configRepo;
  const char *zHashPolicy;
  fsl_hashpolicy_e hashPolicy;
} FNewApp = {
NULL/*comment*/,
NULL/*commentMimetype*/,
NULL/*filename*/,
NULL/*configRepo*/,
NULL/*zHashPolicy*/,
FSL_HPOLICY_SHA3/*hashPolicy*/
};

static int f_create_repo(char force){
  int rc;
  fsl_cx * f = fcli_cx();
  fsl_repo_create_opt opt = fsl_repo_create_opt_empty;
  char const * mfile = FNewApp.filename;
  char * userName = 0;
  if(FNewApp.configRepo){
    f_out("Copying configuration from: %s\n", FNewApp.configRepo);
  }
  opt.allowOverwrite = force;
  opt.filename = mfile;
  opt.commitMessage = FNewApp.comment;
  opt.commitMessageMimetype = FNewApp.commentMimetype;
  opt.configRepo = FNewApp.configRepo;
  {
    /* Problem: when creating the repo, the fsl_cx gets closed, which will
       free its user string. Thus we need our own copy to ensure that it survives
       the repo setup process. */
    const char * u = fsl_cx_user_get(f)
      /* might be set up to -U|--user=name */;
    if(u){
      userName = fsl_strdup(u);
      fcli_fax(userName);
    }
  }
  opt.username = userName;
  rc = fsl_repo_create(f, &opt);
  if(!rc){
    fsl_db * db = fsl_cx_db_repo(f);
    fsl_stmt st = fsl_stmt_empty;
    char * s;
    f_out("Created repository: %s\n", mfile);
    assert(db);
    fsl_cx_hash_policy_set(f, FNewApp.hashPolicy);
#define CONF(KEY) s = fsl_config_get_text( f, FSL_CONFDB_REPO, KEY, NULL); \
    f_out("%-15s= %s\n", KEY, s); \
    fsl_free(s)
    CONF("server-code");
    CONF("project-code");
    CONF("hash-policy");
#undef CONF
    rc = fsl_db_prepare(db, &st,
                        "SELECT login,pw FROM user WHERE uid=1");
    assert(!rc);
    rc = fsl_stmt_step(&st);
    assert(FSL_RC_STEP_ROW==rc);
    if(FSL_RC_STEP_ROW==rc){
      rc = 0;
      f_out("%-15s= %s (password=%s)\n",
             "admin-user",
             fsl_stmt_g_text(&st, 0, NULL),
             fsl_stmt_g_text(&st, 1, NULL)
             );
    }
    fsl_stmt_finalize(&st);
    if(db->error.code){
      fsl_cx_uplift_db_error(f, db);
    }
  }
  /* fcli_err_report(0); */
  return rc;
}

static int handle_hash_policy(void){
  const char * zPol = FNewApp.zHashPolicy ? FNewApp.zHashPolicy : "sha3";
#define P(T) if(0==strcmp(fsl_hash_policy_name(T),zPol)){   \
    FNewApp.hashPolicy = T; return 0; \
  }
  P(FSL_HPOLICY_SHA3);
  P(FSL_HPOLICY_SHA3_ONLY);
  P(FSL_HPOLICY_AUTO);
  P(FSL_HPOLICY_SHA1);
  P(FSL_HPOLICY_SHUN_SHA1);
#undef P
  return fcli_err_set(FSL_RC_MISUSE,"Invalid --hash policy value: %s",zPol);
}

int main(int argc, char const * const * argv ){
  int rc = 0;
  fsl_cx * f;
  bool force;
  fcli_cliflag FCliFlags[] = {
    FCLI_FLAG("f","file","filename",&FNewApp.filename,
              "The repo to create. May optionally be the first non-flag "
              "argument."),
    FCLI_FLAG("m","message","filename",&FNewApp.comment,
              "Sets the commit message for the initial commit."),
    FCLI_FLAG("c","config","other-repo-filename", &FNewApp.configRepo,
              "Copies parts of the configuration from the given repo."),
    FCLI_FLAG_BOOL("F","force",&force,
                   "Force overwrite of existing file."),
    FCLI_FLAG("h","hash","policy-name",&FNewApp.zHashPolicy,
              "Sets the repo's hash policy: sha1, shun-sha1, "
              "sha3 (default), sha3-only, auto"),
#if 0
    FCLI_FLAG("N","mimetype","text",&FNewApp.commentMimetype,
              "Sets the commit message mimetype for the initial commit. "
              "This is permitted because fossil technically allows it, but fossil has never "
              "applied it when rendering, so its use is discouraged. (-N corresponds to the "
              "manifest's N-card.)\n"),
#endif
    fcli_cliflag_empty_m
  };
  fcli_help_info FCliHelp = {
  "Creates a new, empty fosisl repository db.",
  "repo-filename",
  NULL
  };
  fcli.config.checkoutDir = NULL
    /* Same effect as -C/--no-checkout */;
  rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  f = fcli_cx();
  assert(!fsl_cx_db_repo(f));
  rc = handle_hash_policy();
  if(rc) goto end;

  if(!FNewApp.filename){
    FNewApp.filename = fcli_next_arg(1);
    if(!FNewApp.filename){
      rc = fcli_err_set(FSL_RC_MISUSE,
                        "Missing filename argument. "
                        "Try --help.");
      goto end;
    }
  }
  if(fcli_has_unused_flags(0)) goto end;
  rc = f_create_repo(force);
  end:
  return fcli_end_of_main(rc);
}
