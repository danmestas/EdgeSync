/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code
*/
/**
   This app implements a feature analog to fossil(1)'s "rebuild"
   command. It destroys all transient repository state which can
   be reconstructed from the `blob` and `delta` tables and then
   recreates that state.
*/
#include "libfossil.h"
#include <string.h> /* memset() */

// Only for testing/debugging..
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)


/***

For reference (noting that there are no plans to to implement all of
these) options...

$ fossil help rebuild
Usage: fossil rebuild ?REPOSITORY? ?OPTIONS?

Reconstruct the named repository database from the core
records.  Run this command after updating the fossil
executable in a way that changes the database schema.

Options:
  --analyze         Run ANALYZE on the database after rebuilding
  --cluster         Compute clusters for unclustered artifacts
  --compress        Strive to make the database as small as possible
  --compress-only   Skip the rebuilding step. Do --compress only
  --deanalyze       Remove ANALYZE tables from the database
  --force           Force the rebuild to complete even if errors are seen
  --ifneeded        Only do the rebuild if it would change the schema version
  --index           Always add in the full-text search index
  --noverify        Skip the verification of changes to the BLOB table
  --noindex         Always omit the full-text search index
  --pagesize N      Set the database pagesize to N. (512..65536 and power of 2)
  --quiet           Only show output if there are errors
  --randomize       Scan artifacts in a random order
  --stats           Show artifact statistics after rebuilding
  --vacuum          Run VACUUM on the database after rebuilding
  --wal             Set Write-Ahead-Log journalling mode on the database

***/

typedef struct RebuildState {
  bool quiet;
  fsl_rebuild_metrics metrics;
  fsl_msg_listener orig;
} RebuildState;

static int fsl_msg_f_my(fsl_msg const * msg, void *state){
  RebuildState * const rs = state;
  switch(msg->type){
    default: break;
    case FSL_MSG_REBUILD_STEP:{
      if( rs->quiet ) return 0;
      break;
    }
    case FSL_MSG_REBUILD_DONE:{
      fsl_rebuild_step const * const step = msg->payload;
      rs->metrics = step->metrics;
      if( rs->quiet ) return 0;
      break;
    }
  }
  if( rs->orig.callback ){
    return rs->orig.callback(msg, rs->orig.state);
  }
  return 0;
}

static void dump_db_info(char const *header){
  f_out("%.50c\n%s\n", '=', header);
  char const * queries[] = {
  "SELECT count(*) '#' FROM plink",
  "SELECT count(*) '#' FROM mlink",
  "SELECT count(*) '#' FROM event",
  "SELECT count(*) '#' FROM filename",
  NULL
  };
  int i = 0;
  const char * q = queries[0];
  fsl_cx * const f = fcli_cx();
  for( ; q; q=queries[++i] ){
    f_out("%.50c\n%s\n", '~', q);
    fsl_db_each( fsl_cx_db(f), fsl_stmt_each_f_dump, f, q );
  }
  f_out("%.50c\n", '=');
}

int main(int argc, const char * const * argv ){
  fsl_rebuild_opt ropt = fsl_rebuild_opt_empty;
  bool blobCache = true;
  bool mcache = true;
  RebuildState rs;
  memset(&rs, 0, sizeof(RebuildState));
  rs.quiet = !fsl_isatty(1);
  const fcli_cliflag FCliFlags[] = {
    FCLI_FLAG_BOOL("r","randomize", &ropt.randomize,
                   "Randomize artifact order."),
    FCLI_FLAG_BOOL("q","quiet", &rs.quiet,
                   "Disable progress output. "
                   "(Implied when not running on a terminal.)"),
    FCLI_FLAG_BOOL_INVERT(NULL,"no-blob-cache", &blobCache,
                          "Disable fsl_cx blob cache. "
                          "For library testing/debugging."),
    FCLI_FLAG_BOOL_INVERT(NULL,"no-mcache", &mcache,
                          "Disable fsl_cx manifest cache. "
                          "For library testing/debugging."),
    FCLI_FLAG_BOOL("n","dry-run", &ropt.dryRun,
                   "Dry-run mode."),
    fcli_cliflag_empty_m // list MUST end with this (or equivalent)
  };
  const fcli_help_info FCliHelp = {
    "\"Rebuilds\" a fossil SCM repository database from its low-level data.",
    NULL, // very brief usage text, e.g. "file1 [...fileN]"
    NULL // optional callback which outputs app-specific help
  };
  int rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  if((rc=fcli_has_unused_args(false))) goto end;

  fsl_cx * const f = fcli_cx();
  if(!fsl_needs_repo(f)){
    rc = FSL_RC_NOT_A_REPO;
    goto end;
  }
  {
    fsl_msg_listener const ml = {
      .callback = fsl_msg_f_my,
      .state = &rs
    };
    fsl_cx_listener_replace(f, &ml, &rs.orig);
  }
  //fsl_cx_flag_set(f, FSL_CX_F_SKIP_UNKNOWN_CROSSLINKS, true);
  fsl_cx_flag_set(f, FSL_CX_F_BLOB_CACHE, blobCache);
  fsl_cx_flag_set(f, FSL_CX_F_MANIFEST_CACHE, mcache);
  if(fcli_is_verbose()){
    dump_db_info("Before rebuild...");
  }

  rc = fsl_repo_rebuild(fcli_cx(), &ropt);
  if( rc ) goto end;

  if(fcli_is_verbose()){
    dump_db_info("After rebuild...");
  }
  struct {
    int type;
    char const * label;
  } aType[] = {
    {0,"Non-artifacts"},
    {FSL_SATYPE_CHECKIN,"Checkin"},
    {FSL_SATYPE_CLUSTER,"Cluster"},
    {FSL_SATYPE_CONTROL,"Control (tag)"},
    {FSL_SATYPE_WIKI,"Wiki"},
    {FSL_SATYPE_TICKET,"Ticket"},
    {FSL_SATYPE_ATTACHMENT,"Attachment"},
    {FSL_SATYPE_TECHNOTE,"Technote"},
    {FSL_SATYPE_FORUMPOST,"Forum post"},
    {0,NULL}
  };
  f_out("\rCounts per blob type:\n");
  fsl_size_t n = 0, sz = 0;
  for(int i = 0; aType[i].label; ++i){
    if(rs.metrics.counts[i]){
      n += rs.metrics.counts[i];
      sz += rs.metrics.sizes[i];
      f_out("  %-13s: %,-9" FSL_SIZE_T_PFMT
            "bytes: %," FSL_SIZE_T_PFMT "\n",
            aType[i].label, rs.metrics.counts[i],
            rs.metrics.sizes[i]);
    }
  }
  if(rs.metrics.phantomCount){
    n += rs.metrics.phantomCount;
    f_out("  %-13s: %,-9" FSL_SIZE_T_PFMT "\n",
          "Phantoms", rs.metrics.phantomCount);
  }
  if(n){
    f_out("  %-13s: %,-9" FSL_SIZE_T_PFMT
          "bytes: %," FSL_SIZE_T_PFMT "\n\n",
          "Total", n, sz);
  }
  if(ropt.dryRun){
    f_out("DRY RUN MODE - was rolled back.\n");
  }else{
    f_out("If this made a mess of things, run (fossil rebuild) "
          "to get the repository back in a working state.\n");
  }

  fcli_dump_cache_metrics();

end:
  return fcli_end_of_main(rc);
}
