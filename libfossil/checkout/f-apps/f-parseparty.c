/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).

  *****************************************************************************
  A test/demo app for doing mass parsing tests on all artifacts in a
  repository.
*/

#include "libfossil.h"
#include <time.h>

static struct App_{
  bool failFast;
  bool crosslink;
  bool quiet;
  bool wellFormed;
  bool randomOrder;
  bool clearLinks;
  bool rebuildLeaves;
  const char * eventTypes;
} App = {
false/*failFast*/,
false/*crosslink*/,
false/*quiet*/,
true/*wellFormed*/,
false/*randomOrder*/,
true/*clearLinks*/,
true/*rebuildLeaves*/,
NULL/*eventTypes*/
};


static int my_xlink_f(fsl_deck * const d, void * state){
  if(fcli_is_verbose()>1){
    FCLI_VN(2,("Crosslinking %s rid %"FSL_ID_T_PFMT" ...\n",
               fsl_satype_cstr(d->type), d->rid));
  }else if(!App.quiet){
    f_out("x");
  }
  (void)state;
  return 0;
}

static int test_parse_all(void){
  fsl_buffer content = fsl_buffer_empty;
  fsl_deck mf = fsl_deck_empty;
  fsl_cx * const f = fcli_cx();
  fsl_buffer q = fsl_buffer_empty;
  fsl_stmt q1 = fsl_stmt_empty;
  fsl_db * const db = fsl_cx_db_repo(f);
  int rc = 0;
  int counter = 0;
  int errCount = 0;
  int counters[FSL_SATYPE_count] = {0,0,0,0,0,0,0,0,0};
  bool got[FSL_SATYPE_count] =     {0,0,0,0,0,0,0,0,0};
  fsl_buffer eventTypesIn = fsl_buffer_empty;
  fsl_timer timer = fsl_timer_empty;
  uint64_t runtimeC = 0/*fsl_content_get()*/,
    runtimeP = 0/*fsl_deck_parse()*/,
    runtimeX = 0/*crosslinking*/
    /* TODO: break those into separate counters for
       each artifact type. */;
  unsigned short const verbose = fcli_is_verbose();
  if(!db){
    return fsl_cx_err_set(f, FSL_RC_MISUSE,
                          "This app requires a repository db.");
  }
  if(App.crosslink){
    rc = fsl_xlink_listener( f, "parseparty", my_xlink_f, 0 );
    if(!rc) rc = fsl__crosslink_begin(f);
    if(rc) goto end;
  }
  rc = fsl_db_txn_begin(db);
  if(rc) goto end;
#define RC if(rc) goto end
  if(App.eventTypes && *App.eventTypes){
    const char * c = App.eventTypes;
    fsl_buffer_append(&q, "SELECT e.objid, b.uuid FROM event e, blob b "
                      "WHERE e.objid=b.rid AND type in ", -1);
    for(; *c; ++c){
      const char * eType = 0;
      switch(*c){
        case 'c': eType = "ci"; got[FSL_SATYPE_CHECKIN] = true; break;
        case 'g': eType = "g";  got[FSL_SATYPE_CONTROL] = true; break;
        case 't': eType = "t";  got[FSL_SATYPE_TICKET] = true; break;
        case 'n': eType = "e";  got[FSL_SATYPE_TECHNOTE] = true; break;
        case 'w': eType = "w";  got[FSL_SATYPE_WIKI] = true; break;
        case 'f': eType = "f";  got[FSL_SATYPE_FORUMPOST] = true; break;
        default:
          fsl_cx_err_set(f, FSL_RC_MISUSE, "Unknown --types value '%c'.", *c);
          fsl_buffer_clear(&q);
          goto end;
      }
      fsl_buffer_append(&eventTypesIn, (c==App.eventTypes ? "(" : ","), 1);
      fsl_buffer_appendf(&eventTypesIn, "%Q", eType);
    }
    fsl_buffer_append(&eventTypesIn, ")", 1);
    fsl_buffer_appendf(&q, "%b ORDER BY ", &eventTypesIn);
    fsl_buffer_append(&q, App.randomOrder
                      ? "RANDOM()" : "mtime DESC", -1);
    rc = fsl_db_prepare(db, &q1, fsl_buffer_cstr(&q));
  }else{
    int i;
    for(i = 0; i < (int)(sizeof(got)/sizeof(got[0])); ++i){
      got[i] = true;
    }
    got[FSL_SATYPE_CHECKIN] = true;
    fsl_buffer_append(&q,
                      "SELECT e.objid, b.uuid FROM event e, blob b "
                      "WHERE e.objid=b.rid ORDER BY ", -1);
    fsl_buffer_append(&q, App.randomOrder
                      ? "RANDOM()" : "mtime", -1);
    rc = fsl_db_prepare(db, &q1, fsl_buffer_cstr(&q));
  }
  //FCLI_V(("Query = %b\n", &q));
  fsl_buffer_clear(&q);
  if(rc){
    rc = fsl_cx_uplift_db_error2(f, db, rc);
    goto end;
  }
  if(App.crosslink){
    if(0 && App.crosslink && eventTypesIn.used){
      /* If we do this, the for-each-entry loop fails because it has
         no data, of course. */
      f_out("Deleting timeline entries for re-crosslinking types: %b\n",
            &eventTypesIn);
      fsl_db_exec(db, "DELETE FROM event WHERE type IN (%b)", &eventTypesIn);
    }
    if(got[FSL_SATYPE_CHECKIN]){
      FCLI_V(("Deleting mlink/plink entries! If crosslinking "
              "breaks, use 'fossil rebuild' to recover.\n"));
      rc = fsl_db_exec_multi(db, "DELETE FROM mlink; "
                             "DELETE FROM plink;");
      if(!rc && App.rebuildLeaves){
        FCLI_V(("Deleting leaf entries (will be rebuilt).\n"));
        rc = fsl_db_exec_multi(db, "DELETE FROM leaf;");
      }
      if(!rc && got[FSL_SATYPE_CONTROL]){
        FCLI_V(("Deleting most tags!"));
        rc = fsl_db_exec_multi(db,
                               "CREATE TEMP TABLE X AS "
                               "SELECT tagid FROM TAG WHERE tagid>%d "
                               "AND tagname NOT LIKE 'wiki-%%' "
                               "AND tagname NOT LIKE 'tkt-%%' "
                               "AND tagname NOT LIKE 'event-%%'; "
                               "DELETE FROM tagxref WHERE tagid IN X; "
                               "DELETE FROM tag WHERE tagid IN X; "
                               "DROP TABLE X;"
                               "",
                               FSL_TAGID_NOTE);
      }
      /*fsl_db_each( db, fsl_stmt_each_f_dump, f,
        "SELECT tagname from tag" );*/
    }
    if(!rc && got[FSL_SATYPE_WIKI]){
      FCLI_V(("Deleting wiki-* tags entries! If crosslinking "
              "breaks, use 'fossil rebuild' to recover.\n"));
      rc = fsl_db_exec_multi(db,
                             "DELETE FROM tagxref WHERE tagid IN("
                             "SELECT tagid FROM tag "
                             "WHERE tagname LIKE 'wiki-%%'"
                             "); "
                             "DELETE FROM tag "
                             "WHERE tagname LIKE 'wiki-%%';"
                             );
    }
    if(!rc && got[FSL_SATYPE_EVENT]){
      FCLI_V(("Deleting event-* tags entries! If crosslinking "
              "breaks, use 'fossil rebuild' to recover.\n"));
      rc = fsl_db_exec_multi(db,
                             "DELETE FROM tagxref WHERE tagid IN("
                             "SELECT tagid FROM tag "
                             "WHERE tagname LIKE 'event-%%'"
                             ");"
                             "DELETE FROM tag "
                             "WHERE tagname LIKE 'event-%%';"
                             );
    }
    if(rc){
      rc = fsl_cx_uplift_db_error2(f, db, rc);
      goto end;
    }
  }/*App.crosslink*/
  f_out("Here we go...\n");
  mf.f = f;
  while(FSL_RC_STEP_ROW==fsl_stmt_step(&q1)){
    fsl_id_t const rid = fsl_stmt_g_id(&q1, 0);
    const char * zUuid = fsl_stmt_g_text(&q1, 1, 0);
    const char * lineBreak = verbose ? "" : "\n";
    bool wellFormedCheck = true;
    assert(rid>0);
    if(1) fsl_deck_clean2(&mf,&content)
            /* Saves only a tiny amount of allocation, largely because
               fsl_content_get() has to create many temporary buffers
               for de-deltification and we don't currently have a
               mechanism for reusing those. */;
    else fsl_deck_clean(&mf);
    fsl_timer_start(&timer);
    rc = fsl_content_get(f, rid, &content);
    runtimeC += fsl_timer_stop(&timer);
    RC;
    if(App.wellFormed && !fsl_might_be_artifact(&content)){
      wellFormedCheck = 0;
    }
    assert(mf.f);
    fsl_timer_start(&timer);
    rc = fsl_deck_parse2(&mf, &content, rid);
    runtimeP += fsl_timer_stop(&timer);
    if(fcli_is_verbose()<2 && !App.quiet){
      f_out(".");
      fflush(stdout);
    }
    if(rc){
      ++errCount;
      f_out("%sparse-offending artifact: %d / %s\n",
            lineBreak, (int)rid, zUuid);
      if(App.failFast){
        goto end;
      }
      fcli_err_report(1);
      continue;
    }else{
      assert(!content.mem
             && "API says that deck takes over memory on success.");
    }
    assert(mf.rid);
    assert(mf.type>=0 && mf.type<FSL_SATYPE_count);
    if(!wellFormedCheck){
      f_out("\nWARNING: fsl_might_be_artifact() says that this "
            "is NOT an artifact: #%" FSL_ID_T_PFMT"\n", mf.rid);
    }
#if 0
    /* These assertions are wrong for phantom artifact cases. The
       libfossil tree contains some artifacts, for testing purposes,
       from the main fossil tree, which results in phantoms (the
       hashes those artifacts reference but which we don't have). */
    assert(mf.rid);
    assert(mf.uuid);
#endif
    FCLI_VN(2,("Parsed rid %d\n", (int)mf.rid));
    if(App.crosslink){
      fsl_timer_start(&timer);
      rc = fsl__deck_crosslink(&mf);
      runtimeX += fsl_timer_stop(&timer);
      if(rc){
        if(FSL_RC_NOT_FOUND==rc){
          /* Assume this is an artifact, like 4b05c2c59fa61f1240d41949b305173c76d1395d,
             which exists as an artifact file but is not part of this project. */
          f_out("%sFAILED NON-FATALLY crosslinking rid %d w/ rc=%s\n",
                lineBreak, (int)mf.rid, fsl_rc_cstr(rc));
          fcli_err_report(1);
          rc = 0;
        }else{
          f_out("%sFAILED crosslinking rid %d w/ rc=%s\n",
                lineBreak, (int)mf.rid, fsl_rc_cstr(rc));
          fcli_err_report(1);
          if(App.failFast){
            break;
          }
        }
      }
    }
    ++counter;
    ++counters[mf.type];
    /* TODO? Optionally check R-card calculation if
       mf.type==FSL_SATYPE_CHECKIN and mf.R is not NULL. */
  }

  f_out("\nSuccessfully processed %d artifact(s):\n", counter);
  assert(0==counters[FSL_SATYPE_ANY]);
#define CAT(T) if(counters[T]){f_out("%-22s = %d\n", #T, counters[T]);} (void)0
  CAT(FSL_SATYPE_CHECKIN);
  CAT(FSL_SATYPE_CLUSTER);
  CAT(FSL_SATYPE_CONTROL);
  CAT(FSL_SATYPE_WIKI);
  CAT(FSL_SATYPE_TICKET);
  CAT(FSL_SATYPE_ATTACHMENT);
  CAT(FSL_SATYPE_EVENT);
  CAT(FSL_SATYPE_FORUMPOST);
#undef CAT
  if(errCount){
    f_out("ERROR count: %d\n", errCount);
  }
  end:
  fsl_buffer_clear(&eventTypesIn);
  if(App.crosslink){
    if(!rc) fsl_cx_err_reset(f);
    f_out("Ending crosslinking process (might take a few ms)...\n");
    fsl_timer_start(&timer);
    rc = fsl__crosslink_end(f, rc);
    runtimeX += fsl_timer_stop(&timer);
    f_out("\nIf crosslinking made a mess of things, use "
          "'fossil rebuild' to recover.\n");
    if(!rc && App.rebuildLeaves
       && (counters[FSL_SATYPE_CHECKIN]
           || counters[FSL_SATYPE_CONTROL])){
      f_out("Rebuilding leaves after mass-crosslink of "
            "checkins and/or tags. This might take a moment...\n");
      rc = fsl_repo_leaves_rebuild(f);
    }
  }
  f_out("Processing times in ms:\n");
  f_out("    fsl_content_get():    %f\n", (double)(runtimeC / 1000.0));
  f_out("    fsl_deck_parse():     %f\n", (double)(runtimeP / 1000.0));
  if(App.crosslink){
    f_out("    fsl__deck_crosslink(): %f\n", (double)(runtimeX / 1000.0));
  }
#undef RC
  fsl_stmt_finalize(&q1);
  fsl_buffer_clear(&content);
  fsl_deck_finalize(&mf);
  if(fsl_db_txn_level(db)){
    if(rc){
      f_out("Something failed. Rolling back crosslink-started "
            "transaction.\n");
    }
    int const rc2 = fsl_db_txn_end(db, rc ? true : false);
    if(!rc) rc = rc2;
  }
  return rc;
}

int main(int argc, char const * const * argv ){
  int rc = 0;
  bool skipUnknown = true;
  bool useMCache = true;
  bool dryRun = false;
  fsl_timer timer = fsl_timer_empty;
  fcli_cliflag FCliFlags[] = {
    FCLI_FLAG("t","types","list",&App.eventTypes,
              "List of letters of artifact types to parse: "
              "c=checkin, g=control (tags), w=wiki, "
              "t=ticket, n=technote, f=forum"),
    FCLI_FLAG_BOOL("F","fail-fast",&App.failFast,
                   "Stop processing at the first error."),
    FCLI_FLAG_BOOL("c","crosslink",&App.crosslink,
                   "Crosslink all parsed artifacts."),
    FCLI_FLAG_BOOL("q","quiet",&App.quiet,
                   "Disables certain output."),
    FCLI_FLAG_BOOL("n","dry-run",&dryRun,
                   "Rolls back any changes."),
    FCLI_FLAG_BOOL_INVERT("w","no-well-formed",&App.wellFormed,
                   "Disable comparing results of fsl_might_be_artifact() "
                   "with the parsing results to ensure that they "
                   "agree with each other."),
    FCLI_FLAG_BOOL_INVERT(0,"no-mcache",&useMCache,
                          "Disable use of the manifest cache."),
    FCLI_FLAG_BOOL("r","random",&App.randomOrder,
                   "Processes artifacts in a random order, instead of "
                   "their time order."),
    FCLI_FLAG_BOOL_INVERT("l","no-rebuild-leaves",&App.rebuildLeaves,
                          "After crosslinking manifests and/or tags, "
                          "do NOT rebuild the leaves list. For testing "
                          "the occasionally-stray-leaf bug."),
    FCLI_FLAG_BOOL_INVERT("s","no-skip-unknown",&skipUnknown,
                   "Disable skipping of \"known failures\" for "
                   "crosslinking as-yet-unsupported/unknown artifact "
                   "types."),
    fcli_cliflag_empty_m
  };
  fcli_help_info FCliHelp = {
  "Tests the libfossil artifact parser by parsing, "
  "and optionally crosslinking, in-repo artifacts. "
  "If it makes a mess, use 'fossil rebuild' to recover.",
  NULL, NULL
  };
  rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  else if(fcli_has_unused_flags(0)) goto end;
  fsl_cx * const f = fcli_cx();
  fsl_cx_flag_set(f, FSL_CX_F_MANIFEST_CACHE, useMCache);
  fsl_cx_flag_set(f, FSL_CX_F_SKIP_UNKNOWN_CROSSLINKS, skipUnknown);
  fsl_timer_start(&timer);
  if(dryRun) rc = fsl_cx_txn_begin(f);
  if(0==rc) rc = test_parse_all();
  {
    fsl_timer_stop(&timer);
    f_out("Total work time: %lf ms wall clock, %lf ms CPU.\n",
          (double)timer.wall/1000.0, (double)(timer.system+timer.user)/1000.0);
    fcli_dump_cache_metrics();
  }
  if(dryRun){
    f_out("Dry-run mode: rolling back transaction.\n");
    fsl_cx_txn_end(f, true);
  }
  end:
  return fcli_end_of_main(rc);
}
