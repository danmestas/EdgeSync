/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/

/******************************************************************
  This file implements some test/demo code for working with wiki
  pages.
*/

#include "libfossil.h"

static struct {
  const char * pageName;
  const char * outFilename;
  const char * inFilename;
  const char * mimetype;
  bool listNamesOnly;
  bool utcTime;
  bool allowNew;
  bool dryRun;
} WikiApp = {
NULL/*pageName*/,
NULL/*outFilename*/,
NULL/*inFilename*/,
NULL/*mimetype*/,
false/*lsNames*/,
false/*utcTime*/,
false/*allowNew*/,
false/*dryRun*/
};

static int fcli_flag_callback_f_mimetype(fcli_cliflag const * x){
  const char * m = *((char const **)x->flagValue);
  if(0==fsl_strcmp("markdown",m)) m = "text/x-markdown";
  else if(0==fsl_strcmp("fossil",m)) m = "text/x-fossil-wiki";
  else if(0==fsl_strcmp("plain",m)) m = "text/plain";
  if(0!=fsl_strcmp(m,"text/x-markdown")
     && 0!=fsl_strcmp(m,"text/x-fossil-wiki")
     && 0!=fsl_strcmp(m,"text/plain")){
    return fcli_err_set(FSL_RC_MISUSE,
                        "Unknown mimetype [%s]. Use one of: "
                        "text/x-fossil-wiki (alias=fossil), "
                        "text/x-markdown (alias=markdown), "
                        "or text/plain (alias=plain)",
                        m);
  }
  WikiApp.mimetype = m;
  return 0;
}


static fcli_cliflag cliFlagsLs[] = {
  FCLI_FLAG_BOOL("n","names",&WikiApp.listNamesOnly,
                 "Lists only the page names, not other info."),
  FCLI_FLAG_BOOL(0,"utc",&WikiApp.utcTime,
                 "Use UTC instead of local time."),
  fcli_cliflag_empty_m
};

static fcli_cliflag cliFlagsExport[] = {
  FCLI_FLAG("p","page","PageName",&WikiApp.pageName,
            "Optionally specified as first non-flag option."),
  FCLI_FLAG("o","output-file","file",&WikiApp.outFilename,
            "Optionally specified as second non-flag option."),
  fcli_cliflag_empty_m
};

static fcli_cliflag cliFlagsSave[] = {
  FCLI_FLAG("p","page","PageName",&WikiApp.pageName,
            "Optionally specified as first non-flag option."),
  FCLI_FLAG("f","input-file","file",&WikiApp.inFilename,
            "Optionally specified as second non-flag option."),
  FCLI_FLAG_BOOL("n","dry-run",&WikiApp.dryRun, "Dry-run mode."),
  FCLI_FLAG_BOOL(0,"new",&WikiApp.allowNew,
                 "Specifies that the page should be created if "
                 "it does not exist."),
  FCLI_FLAG_X("t","mimetype","mimetype-string",&WikiApp.mimetype,
              fcli_flag_callback_f_mimetype,
              "Specify mimetype as one of: "
              "text/x-fossil-wiki (default), text/x-markdown, text/plain"),
  fcli_cliflag_empty_m
};

int32_t wiki_page_count(void){
  return fsl_db_g_int32(fsl_cx_db_repo(fcli_cx()), -1,
                        "SELECT count(*) FROM tag "
                        "WHERE tagname GLOB 'wiki-*'");
}

/**
    A fsl_deck_visitor_f() impl which expects d to be a WIKI
    artifact, and it outputs various info about it.
 */
static int cb_f_wiki_list( fsl_cx * const f, fsl_deck * const d,
                           void * state ){
  int * counter = (int*)state;
  if(WikiApp.listNamesOnly){
    f_out("%s\n", d->L);
  }else{
    fsl_db * db = fsl_cx_db_repo(fcli_cx());
    char * ts = fsl_db_julian_to_iso8601(db, d->D, 0, !WikiApp.utcTime);
    unsigned short const vbose = fcli_is_verbose();
    assert(ts && 'T'==ts[10]);
    ts[10] = ' ';
    if(0 == (*counter)++){
      if(vbose){
        f_out("RID    ");
      }
      f_out("%-20s %-13s %-6s Name\n",
            (WikiApp.utcTime
             ? "Time (UTC)"
             : "Time (local time)"),
            "UUID", "Size");
    }
    if(vbose){
      f_out("%-6"FSL_ID_T_PFMT" ", d->rid);
    }
    f_out("%-20s %.*z  %-6"FSL_SIZE_T_PFMT" %s\n",
          ts, 12, fsl_rid_to_uuid(f, d->rid),
          (fsl_size_t)d->W.used, d->L);
    fsl_free(ts);
  }
#if defined(DEBUG)
  {
    int rc;
    fsl_id_t ridCheck = 0;
    rc = fsl_wiki_latest_rid(f, d->L, &ridCheck);
    assert(!rc);
    if(d->rid!=ridCheck){
      f_out("unexpected version mismatch in page [%s]: "
            "d->rid=%"FSL_ID_T_PFMT", ridCheck=%"FSL_ID_T_PFMT"\n",
            d->L, d->rid, ridCheck);
    }
    assert(d->rid==ridCheck);
  }
#endif

  return 0;
}

static int fcmd_wiki_list(fcli_command const *cmd){
  fsl_cx * f = fcli_cx();
  int counter = 0;
  int rc = fcli_process_flags(cmd->flags);
  if(rc || (rc = fcli_has_unused_flags(0))){
    return rc;
  }
  if(WikiApp.listNamesOnly){
    fsl_list li = fsl_list_empty;
    fsl_size_t i;
    rc = fsl_wiki_names_get(f, &li);
    for( i = 0; !rc && (i<li.used); ++i){
      char const * n = (char const *)li.list[i];
      f_out("%s\n", n);
    }
    fsl_list_clear(&li, fsl_list_v_fsl_free, NULL);
  }else{
    rc = fsl_wiki_foreach_page(f, cb_f_wiki_list, &counter );
  }
  return rc;
}

static int fcmd_wiki_export(fcli_command const *cmd){
  fsl_cx * f = fcli_cx();
  fsl_deck d = fsl_deck_empty;
  int rc = fcli_process_flags(cmd->flags);
  if(rc) return rc;
  const char * pName = WikiApp.pageName;
  if(!pName && !(pName = fcli_next_arg(1))){
    rc = fcli_err_set(FSL_RC_MISUSE,
                      "Expecting wiki page name argument.");
    goto end;
  }
  if(!WikiApp.outFilename){
    WikiApp.outFilename = fcli_next_arg(1);
  }
  if((rc = fcli_has_unused_flags(0))){
    goto end;
  }

  rc = fsl_wiki_load_latest(f, pName, &d);
  if(!rc){
    if(WikiApp.outFilename){
      FILE * of = fsl_fopen(WikiApp.outFilename, "w");
      if(!of){
        rc = fcli_err_set(FSL_RC_IO,
                          "Could not open [%s] for writing.",
                          WikiApp.outFilename);
      }else{
        fwrite(d.W.mem, d.W.used, 1, of);
        fsl_fclose(of);
      }
    }else{
      f_out("%b", &d.W);
      if(d.W.used && '\n'!=(char)d.W.mem[d.W.used-1]){
        f_out("\n");
      }
    }
  }
  end:
  fsl_deck_finalize(&d);
  return rc;
}

static int fcmd_wiki_save(fcli_command const *cmd){
  fsl_cx * f = fcli_cx();
  fsl_buffer buf = fsl_buffer_empty;
  fsl_db * db = fsl_cx_db_repo(f);
  char pageExists;
  char const * userName;
  int rc = fcli_process_flags(cmd->flags);
  if(rc) return rc;
  const char * pName = WikiApp.pageName;
  const char * iFile = WikiApp.inFilename;
  const char * mimeType = WikiApp.mimetype;
  if(!pName && !(pName = fcli_next_arg(1))){
    rc = fcli_err_set(FSL_RC_MISUSE,
                           "Expecting wiki page name argument.");
    goto end;
  }
  if(!iFile && !(iFile = fcli_next_arg(1))){
    rc = fcli_err_set(FSL_RC_MISUSE,
                           "Expecting input file name.");
    goto end;
  }
  if((rc = fcli_has_unused_flags(0))){
    goto end;
  }
  userName = fsl_cx_user_get(f);
  rc = fsl_buffer_fill_from_filename(&buf, iFile);
  if(rc){
    rc = fcli_err_set(rc, "Error opening file: %s", iFile);
    goto end;
  }
  pageExists = fsl_wiki_page_exists(f, pName);
  if(!pageExists && !WikiApp.allowNew){
    rc = fcli_err_set(FSL_RC_NOT_FOUND,
                           "Page [%s] does not exists. "
                           "Use --new to allow "
                           "creation of new pages.", pName);
    goto end;
  }
  fsl_db_txn_begin(db);
  rc = fsl_wiki_save(f, pName, &buf, userName,
                     mimeType, FSL_WIKI_SAVE_MODE_UPSERT);

  if(!rc){
    if(WikiApp.dryRun){
      f_out("Dry-run mode _not_ saving changes.\n");
    }
    rc = fsl_db_txn_end(db, WikiApp.dryRun);
  }else{
    fsl_db_txn_end(db, 1);
  }
  end:
  fsl_buffer_clear(&buf);
  return rc;
}



static fcli_command aCommandsWiki[] = {
{"ls", NULL, "Lists pages in the repository. Use --verbose for more info.",
 fcmd_wiki_list, NULL, cliFlagsLs},
{"export", "ex\0dump\0", "Exports the most recent version of the given page.",
 fcmd_wiki_export, NULL, cliFlagsExport},
{"save", NULL, "Saves or creates wiki pages.",
  fcmd_wiki_save, NULL, cliFlagsSave},
{NULL,NULL,NULL,NULL,NULL,NULL}/*empty sentinel is required by traversal algos*/
};

static void fcli_local_help(void){
  puts("Wiki commands and their options:\n");
  fcli_command_help(aCommandsWiki, false, false);
}

/**
    Just experimenting with fsl_xlink_listener() and friends.
 */
static int wiki_xlink_f(fsl_deck * d, void * state){
  FCLI_V(("Crosslink callback for %s artifact [%.*z] (RID %"FSL_ID_T_PFMT")\n",
          fsl_satype_cstr(d->type), 8,
          fsl_rid_to_uuid(d->f, d->rid), d->rid));
  return *((char const *)state) /* demonstrate what happens when crosslinking fails. */
    ? FSL_RC_NYI
    : 0;
}

int main(int argc, char const * const * argv ){
  int rc;
  fcli_cliflag const dummyFlags[] = {/*porting kludge*/fcli_cliflag_empty_m};
  fcli_help_info const FCliHelp = {
    "List, export, and import fossil repository wiki content.",
    "ls|save|export [options]",
    fcli_local_help
  };
  rc = fcli_setup_v2(argc, argv, dummyFlags, &FCliHelp);
  if(!rc){
    if(!fsl_cx_db_repo(fcli_cx())){
      rc = fcli_err_set(FSL_RC_MISUSE,
                             "This app requires a repository db.");
    }
    else{
      bool failCrosslink = fcli_flag2("fx", "fail-xlink", NULL);;
      fsl_xlink_listener( fcli_cx(), fcli.appName, wiki_xlink_f, &failCrosslink );
      rc = fcli_dispatch_commands(aCommandsWiki, 0);
    }
  }
  return fcli_end_of_main(rc);
}
