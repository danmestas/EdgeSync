/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).

  *****************************************************************************
  A test/demo app for working with the libfossil config db API.
*/

#include "libfossil.h"

static struct ConfigApp {
  bool doGlobal;
  bool doLocal;
  bool doRepo;
  bool showValues;
  const char * lsGlob;
  fsl_confdb_e mode0;
} ConfigApp = {
0,0,0,
0/*showValues*/,
NULL/*lsGlob*/,
FSL_CONFDB_GLOBAL/*mode0*/
};

static fcli_cliflag cliFlagsGlobal[] = {
  FCLI_FLAG_BOOL("g", "global", &ConfigApp.doGlobal,
                 "Use the global config db."),
  FCLI_FLAG_BOOL("c", "checkout", &ConfigApp.doLocal,
                 "Use the checkout-local config."),
  FCLI_FLAG_BOOL("r", "repository", &ConfigApp.doRepo,
                 "Use the repository-local config."),
  fcli_cliflag_empty_m
};

static fcli_cliflag cliFlagsLs[] = {
  FCLI_FLAG(0, "glob", "string", &ConfigApp.lsGlob,
            "Only list properties matching this glob."),
  FCLI_FLAG_BOOL("v", "values", &ConfigApp.showValues,
                 "List property values."),
  fcli_cliflag_empty_m
};

static fcli_cliflag cliFlagsSet[] = {
  fcli_cliflag_empty_m
};

static fcli_cliflag cliFlagsUnset[] = {
  // TODO?: --glob to unset all matching a glob.
  fcli_cliflag_empty_m
};

static int fapp_ls(fsl_cx * f, fsl_confdb_e mode,
                   char const * nameGlob, char showVals){
  int rc;
  fsl_db * db = fsl_config_for_role(f, mode);
  fsl_stmt st = fsl_stmt_empty;
  int nameWidth = -30;
  if(!db){
    if(FSL_CONFDB_GLOBAL==mode){ /* special case: auto-open config db */
      rc = fsl_config_open(f, NULL);
      if(!rc){
        db = fsl_cx_db_config(f);
        assert(db);
      }
    }
    if(!db){
      return fcli_err_set(FSL_RC_MISUSE,
                          "Config db role #%d is not opened.",
                          mode);
    }
  }
  rc = fsl_db_prepare(db, &st, "SELECT name, value FROM %s ORDER BY name",
                      fsl_config_table_for_role(mode));
  while(FSL_RC_STEP_ROW==fsl_stmt_step(&st)){
    char const * name = fsl_stmt_g_text(&st, 0, NULL);
    if(nameGlob && !fsl_str_glob(nameGlob, name)) continue;
    if(showVals){
      f_out("%*s %s\n",
            nameWidth, name,
            fsl_stmt_g_text(&st,1,NULL));
    }else{
      f_out("%s\n", name);
    }
  }
  fsl_stmt_finalize(&st);
  return rc;
}

static int fcmd_ls(fcli_command const *fc){
  fsl_cx * f = fcli_cx();
  int didSomething = 0;
  int rc = fcli_process_flags(fc->flags);
  if(rc || (rc = fcli_has_unused_flags(0))){
    return rc;
  }


  /* f_out("Global config db: %s\n", fsl_cx_db_file_config(f, NULL)); */
#define DUMP(MODE) \
  ++didSomething; \
  rc = fapp_ls(f, MODE, ConfigApp.lsGlob, ConfigApp.showValues)

  if(ConfigApp.doGlobal){
    DUMP(FSL_CONFDB_GLOBAL);
  }
  if(!rc && ConfigApp.doRepo){
    DUMP(FSL_CONFDB_REPO);
  }
  if(!rc && ConfigApp.doLocal){
    DUMP(FSL_CONFDB_CKOUT);
  }
#undef DUMP
  assert(didSomething);
  return rc;
}


static int fcmd_set(fcli_command const *fc){
  const char * key = NULL;
  const char * val = NULL;
  fsl_cx * f = fcli_cx();
  int rc = fcli_process_flags(fc->flags);
  if(rc || (rc = fcli_has_unused_flags(0))){
    return rc;
  }
  key = fcli_next_arg(1);
  if(!key){
    return fcli_err_set(FSL_RC_MISUSE,"Missing property key argument.");
  }
  val = fcli_next_arg(1);
  if(val){
    FCLI_V(("Setting %s = %s\n", key, val));
    rc = fsl_config_set_text(f, ConfigApp.mode0, key, val);
  }else{
    char * v = fsl_config_get_text(f, ConfigApp.mode0, key, 0);
    if(v){
      f_out("%s\n", v);
      fsl_free(v);
    }else{
      rc = fcli_err_set(FSL_RC_NOT_FOUND,
                        "Config key not found: %s", key);
    }
  }
  return rc;
}

static int fcmd_unset(fcli_command const *fc){
  const char * key = NULL;
  fsl_cx * f = fcli_cx();
  unsigned int count = 0;
  int rc = fcli_process_flags(fc->flags);
  if(rc || (rc = fcli_has_unused_flags(0))){
    return rc;
  }
  rc = fsl_config_transaction_begin(f, ConfigApp.mode0);
  if(!rc){
    while( !rc && (key = fcli_next_arg(1)) ){
      ++count;
      FCLI_V(("Unsetting: %s\n", key));
      rc = fsl_config_unset(f, ConfigApp.mode0, key);
    }
    fsl_config_transaction_end(f, ConfigApp.mode0, rc ? 1 : 0);
  }
  if(!count){
    rc = fcli_err_set(FSL_RC_MISUSE,"Missing property key argument(s).");
  }
  return rc;
}


static const fcli_command ConfigCmds[] = {
{"ls", NULL, "List config properties.",
 fcmd_ls, NULL, cliFlagsLs},
{"set", NULL, "Get/set individual config properties.",
 fcmd_set, NULL, cliFlagsSet},
{"unset", "us\0", "Unset config properties. "
    "Pass it all property keys to unset.",
 fcmd_unset, NULL, cliFlagsUnset},
{NULL,NULL,NULL,NULL,NULL,NULL}
};

static void fcli_local_help(void){
  puts("Config commands and their options:\n");
  fcli_command_help(ConfigCmds, false, false);
}

static int fapp_main(fsl_cx * const f){
  /*
    Set the main config mode for commands which use only
    one config. Priority order: -c -r -g
  */
  if(ConfigApp.doGlobal){
    int rc;
    ConfigApp.mode0 = FSL_CONFDB_GLOBAL;
    rc = fsl_config_open( f, NULL );
    if(rc) return rc;
  }
  else if(ConfigApp.doLocal) ConfigApp.mode0 = FSL_CONFDB_CKOUT;
  else if(ConfigApp.doRepo) ConfigApp.mode0 = FSL_CONFDB_REPO;
  else{
    return fcli_err_set(FSL_RC_MISUSE,
                        "No config db specified. Use one of: "
                        "-g[lobal], -r[epo], -c[heckout]");
  }
  return fcli_dispatch_commands(ConfigCmds, 0);
}

int main(int argc, char const * const * argv ){
  int rc = 0;
  fcli_help_info FCliHelp = {
    "List and manipulate fossil configuration vars.",
    "ls|set|unset [options]",
    fcli_local_help
  };
  rc = fcli_setup_v2(argc, argv, cliFlagsGlobal, &FCliHelp);
  if(!rc){
    rc = fapp_main(fcli_cx());
  }
  return fcli_end_of_main(rc);
}
