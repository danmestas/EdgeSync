/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).

  *****************************************************************************
  This file implements a basic SQL query tool using a libfossil-extended
  command set (new SQL-side functions for working with fossil data).
*/

#include "libfossil.h"

static struct {
  bool showHeaders;
  bool dumpDashE;
  const char * separator;
  const char * tmpFlag;
  fsl_list eList;
  fsl_buffer scriptBuf;
} QueryApp = {
  .showHeaders = true,
  .dumpDashE = false,
  .separator = NULL,
  .tmpFlag = NULL,
  .eList = fsl_list_empty_m,
  .scriptBuf = fsl_buffer_empty_m
};
struct EFlag {
  int type;
  char const * txt;
};
typedef struct EFlag EFlag;

static int fsl_stmt_each_f_row( fsl_stmt * stmt, void * state ){
  int i;
  char const * sep = QueryApp.separator
    ? QueryApp.separator : "\t";
  (void)state;
  if('\\'==*sep){
    /* Translate client-provided \t and \n */
    switch(sep[1]){
      case 't': sep = "\t"; break;
      case 'n': sep = "\n"; break;
    }
  }
  int const colCount = fsl_stmt_col_count(stmt);
  if((1==stmt->rowCount) && QueryApp.showHeaders){
    for( i = 0; i < colCount; ++i){
      f_out("%s%s", fsl_stmt_col_name(stmt, i),
            (i<(colCount-1)) ? sep : "\n");
    }
  }
  for( i = 0; i < colCount; ++i){
    char const * col = fsl_stmt_g_text(stmt, i, NULL);
    f_out("%s%s", col ? col : "NULL",
          (i<(colCount-1)) ? sep : "\n");
  }
  return 0;
}

static void new_eflag(bool bigE, char const *zTxt){
  EFlag * ef = (EFlag*)fsl_malloc(sizeof(EFlag));
  ef->txt = zTxt;
  ef->type = bigE ? 1 : 0;
  fsl_list_append(&QueryApp.eList, ef);
}
static int fcli_flag_callback_f_e(fcli_cliflag const * x){
  assert(QueryApp.tmpFlag==*((char const **)x->flagValue));
  new_eflag(false, QueryApp.tmpFlag);
  QueryApp.tmpFlag = 0;
  return FCLI_RC_FLAG_AGAIN;
}
static int fcli_flag_callback_f_EBig(fcli_cliflag const * x){
  assert(QueryApp.tmpFlag==*((char const **)x->flagValue));
  new_eflag(true, QueryApp.tmpFlag);
  QueryApp.tmpFlag = 0;
  return FCLI_RC_FLAG_AGAIN;
}

static int fsl_list_visitor_f_e(void * obj, void * visitorState ){
  EFlag const * e = (EFlag const *)obj;
  const char * sql = e->txt;
  int rc;
  fsl_cx * const f = fcli_cx();
  fsl_db * const db = (fsl_db*)visitorState;

  fsl_buffer_reuse(&QueryApp.scriptBuf);
  if(0==fsl_file_access(e->txt,0)){
    rc = fsl_buffer_fill_from_filename(&QueryApp.scriptBuf, sql);
    if(rc){
      return fcli_err_set(rc, "Error %d (%s) loading SQL from file [%s]",
                          rc, fsl_rc_cstr(rc), sql);
    }
    sql = (const char *)QueryApp.scriptBuf.mem;
  }
  if(e->type>0){
    if( QueryApp.dumpDashE ){
      f_out("-- Contents of %s:\n", e->txt);
      fsl_stream(fsl_input_f_buffer, &QueryApp.scriptBuf,
                 fsl_output_f_FILE, stdout);
    }
    rc = fsl_db_exec_multi(db, "%s", sql);
  }else{
    fsl_stmt st = fsl_stmt_empty;
    if( QueryApp.dumpDashE ){
      f_out(e->txt);
      fsl_stream(fsl_input_f_buffer, &QueryApp.scriptBuf,
                 fsl_output_f_FILE, stdout);
    }
    rc = fsl_db_prepare(db, &st, "%s", sql);
    if(!rc){
      int const colCount = fsl_stmt_col_count(&st);
      if( colCount>0 ){ /* SELECT-style query */
        rc = fsl_stmt_each( &st, fsl_stmt_each_f_row, NULL );
      }else if( 0==colCount ){
        rc = fsl_stmt_step(&st);
        if(FSL_RC_STEP_ROW==rc || FSL_RC_STEP_DONE==rc) rc = 0;
      }
    }
    fsl_stmt_finalize(&st);
  }
  if(rc) fsl_cx_uplift_db_error(f, db);
  return 0;
}

int main(int argc, char const * const * argv ){
  int rc = 0;
  bool noTransaction = false;
  bool fDryRun = false;
  bool fConfigDb = false;
  fsl_cx * f = NULL;
  fsl_db * db = NULL;
  fcli_cliflag FCliFlags[] = {
  /* FIXME: fcli processes flags based on their order in this list,
     e.g. all -E flags before any -e flags. That leads to unfortunate
     CLI flag usage ordering dependencies. We ought to improve it
     someday to process them in their argv order. */
    FCLI_FLAG_X("E",0,"SQL",&QueryApp.tmpFlag, fcli_flag_callback_f_EBig,
                "Treats its argument as a file or multiple SQL statements."),
    FCLI_FLAG_X("e",0,"SQL",&QueryApp.tmpFlag, fcli_flag_callback_f_e,
                "Treats its argument as a file or a single SQL statement. "
                "ACHTUNG: all -E flags are run before all -e flags, due to "
                "a quirk of the argument parser."),
    FCLI_FLAG_BOOL(0, "dump-e", &QueryApp.dumpDashE,
                   "Dump the contents of -e/-E flags before their output."),
    FCLI_FLAG("s", "separator", "string", &QueryApp.separator,
              "Separator for columns in SELECT results."),
    FCLI_FLAG_BOOL_INVERT("h", "no-header", &QueryApp.showHeaders,
                   "Disables output of headers on SELECT queries."),
    FCLI_FLAG_BOOL("t","no-transaction", &noTransaction,
                   "Disables the use of an SQL transaction. "
                   "Trumped by --dry-run."),
    FCLI_FLAG_BOOL("g","config", &fConfigDb,
                   "Opens the global config db instead of the "
                   "default of the current checkout or -R REPO db. "
                   "Trumped by the -R flag."),
    FCLI_FLAG_BOOL("n","dry-run", &fDryRun,"Dry-run mode."),
    fcli_cliflag_empty_m
  };
  fcli_help_info FCliHelp = { "Runs SQL against a fossil repository.", NULL, NULL };
  fcli.config.checkoutDir = NULL;

  rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  if(!QueryApp.eList.used){
    rc = fcli_err_set(FSL_RC_MISUSE,
                      "Missing -e/-E flag(s). Try --help.");
    goto end;
  }
  f = fcli_cx();
  //assert(!fsl_cx_db_ckout(f));
  db = fsl_cx_db_repo(f) /* from -R REPO or --checkout-dir flags */;
  if(!db){
    if(fConfigDb){
      rc = fsl_config_open(f, NULL);
      db = rc ? NULL : fsl_cx_db_config(f);
    }else{
      rc = fsl_ckout_open_dir(f, ".", true);
      db = rc ? NULL : fsl_cx_db_ckout(f);
    }
    if(rc) goto end;
  }
  if(!db){
    rc = fcli_err_set(FSL_RC_NOT_A_REPO,
                      "Requires an opened database. See --help.");
    goto end;
  }
  if(fDryRun) noTransaction = false;
  rc = noTransaction ? 0 : fsl_db_txn_begin(db);
  if(!rc){
    rc = fsl_list_visit(&QueryApp.eList, 0, fsl_list_visitor_f_e, db);
  }
  if(db->impl.txn.level>0){
    assert(!noTransaction);
    if(rc || fDryRun){
      FCLI_V(("Rolling back transaction.\n"));
      fsl_db_txn_rollback(db);
    }else{
      FCLI_V(("Committing transaction.\n"));
      rc = fsl_db_txn_commit(db);
      if(rc) fsl_cx_uplift_db_error(f, db);
    }
  }
  end:
  fsl_list_visit_free(&QueryApp.eList, true);
  fsl_buffer_clear(&QueryApp.scriptBuf);
  return fcli_end_of_main(rc);
}
