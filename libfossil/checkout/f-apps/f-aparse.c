/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).

  *****************************************************************************
  A test/demo app for working with the libfossil "deck" API.
*/

#include "libfossil.h" /* Fossil App mini-framework */
#include <time.h>

#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

static struct App_{
  bool doCrosslink;
  bool checkRCard;
  char const * mfile;
  char const * oFile;
  bool saveDeck;
  bool emitJson;
} App = {
false/*doCrosslink*/,
false/*checkRCard*/,
0/*mfile*/,
0/*oFile*/,
false/*saveDeck*/,
false/*emitJson*/
};

/* Just for testing default crosslinker replacement */
#define MY_OVERRIDE_XLINK_CHECKIN 0

/**
    Just experimenting with fsl_xlink_listener() and friends.
 */
static int my_xlink_f(fsl_deck * d, void * state){
  FCLI_V(("Crosslink callback for %s artifact RID %" FSL_ID_T_PFMT "\n",
           fsl_satype_cstr(d->type), d->rid));
  if( *((char const *)state) ){
    return fsl_cx_err_set(d->f, FSL_RC_IO,
                          "Demonstrating what happens when crosslinking fails.");
  }
#if !MY_OVERRIDE_XLINK_CHECKIN
  return fsl_db_exec(fsl_cx_db_repo(d->f),
       "UPDATE event SET ecomment="
       "'f-aparse: '||coalesce(ecomment,comment) "
       "WHERE objid=%"FSL_ID_T_PFMT,
       d->rid
  );
#else
  if(FSL_SATYPE_CHECKIN!=d->type) return 0;
  return fsl_db_exec(fsl_cx_db_repo(d->f),
       "REPLACE INTO event(type,mtime,objid,user,comment,"
       "bgcolor,euser,ecomment,omtime)"
       "VALUES('ci',"
       "  coalesce(" /*mtime*/
       "    (SELECT julianday(value) FROM tagxref "
       "      WHERE tagid=%d AND rid=%"FSL_ID_T_PFMT
       "    ),"
       "    %"FSL_JULIAN_T_PFMT""
       "  ),"
       "  %"FSL_ID_T_PFMT","/*objid*/
       "  %Q," /*user*/
       "  '%q:%d: %q'," /*comment. No, the comment _field_. */
       "  (SELECT value FROM tagxref " /*bgcolor*/
       "    WHERE tagid=%d AND rid=%"FSL_ID_T_PFMT
       "    AND tagtype>0"
       "  ),"
       "  (SELECT value FROM tagxref " /*euser*/
       "    WHERE tagid=%d AND rid=%"FSL_ID_T_PFMT
       "  ),"
       "  (SELECT value FROM tagxref " /*ecomment*/
       "    WHERE tagid=%d AND rid=%"FSL_ID_T_PFMT
       "  ),"
       "  %"FSL_JULIAN_T_PFMT/*omtime*/
       /* RETURNING coalesce(ecomment,comment)
          see comments below about zCom */
       ")",
       /* The casts here are to please the va_list. */
       (int)FSL_TAGID_DATE, d->rid, d->D,
       d->rid, d->U,
       __FILE__, __LINE__, d->C,
       (int)FSL_TAGID_BGCOLOR, d->rid,
       (int)FSL_TAGID_USER, d->rid,
       (int)FSL_TAGID_COMMENT, d->rid, d->D
  );
#endif
}

#define RCEND if(rc) goto end

static int test_parse_1( char const * mfile ){
  fsl_buffer buf = fsl_buffer_empty;
  fsl_buffer bout = fsl_buffer_empty;
  int rc;
  fsl_id_t rid = 0;
  fsl_deck mf = fsl_deck_empty;
  fsl_cx * const f = fcli_cx();
  char const * ofile = App.oFile;
  rc = fsl_buffer_fill_from_filename(&buf, mfile);
  if(rc){
    return fcli_err_set(rc, "Reading input file failed with %s: %s",
                        fsl_rc_cstr(rc), mfile);
  }
  assert(buf.used);
  { /* See if we can find an in-repo match... */
    fsl_buffer hash = fsl_buffer_empty;
    fsl_sha3sum_buffer(&buf, &hash);
    rid = fsl_uuid_to_rid(f, fsl_buffer_cstr(&hash));
    if(rid<=0){
      fsl_buffer_reuse(&hash);
      fsl_sha1sum_buffer(&buf, &hash);
      rid = fsl_uuid_to_rid(f, fsl_buffer_cstr(&hash));
    }
    fsl_buffer_clear(&hash);
    fcli_err_reset();
  }

  f_out("Parsing this manifest: %s\n",mfile);
  mf.f = f /* this allows fsl_deck_parse() to populate mf with more
              data. */;
  rc = fsl_deck_parse(&mf, &buf);
  assert(0==mf.rid);
  RCEND;
  if(rid>0) mf.rid = rid;
  assert(f == mf.f);
  f_out("Artifact type=%s\n", fsl_satype_cstr(mf.type));
  if(App.saveDeck){
    rc = fsl_deck_save(&mf, false);
    MARKER(("save rc=%s\n",fsl_rc_cstr(rc)));
    RCEND;
  }

  if(mf.B.uuid){
    f_out("Trying to fetch baseline manifest [%s]\n", mf.B.uuid);
    rc = fsl_deck_baseline_fetch(&mf);
    f_out("rc=%s, Baseline@%p\n", fsl_rc_cstr(rc), (void const *)mf.B.baseline);
    if(0){
      fsl_deck_output( mf.B.baseline, fsl_output_f_FILE, stdout);
    }
    fcli_err_reset(/*in case baseline was not in our repo*/);
  }
  if(App.checkRCard && mf.R){
    char _rCheck[FSL_STRLEN_MD5+1] = {0};
    char * rCheck = 0 ? NULL : _rCheck;
    assert(mf.R);
    f_out("Trying to re-calculate R-card: original=[%s]\n", mf.R);
    rc = fsl_deck_R_calc2(&mf, &rCheck);
    fcli_err_report(1);
    assert(!rc);
    f_out("Re-calculated R-card: [%s]\n", rCheck);
    f_out("R-card match? %s\n",
          (0==fsl_strcmp(rCheck, mf.R)) ? "yes" : "NO!");
    if(rCheck != _rCheck) fsl_free(rCheck);
  }
  if( App.emitJson ){
    if(ofile){
      rc = fsl_deck_to_json(&mf, fsl_output_f_buffer, &bout);
      fcli_err_report(1);
      RCEND;
      f_out("Dumping artifact to file [%s]\n", ofile);
      rc = fsl_buffer_to_filename(&bout, ofile);
      RCEND;
    }else{
      rc = fsl_deck_to_json(&mf, fsl_output_f_FILE, stdout);
      RCEND;
      f_out("\n");
    }
  }else{
    rc = fsl_deck_output(&mf, fsl_output_f_buffer, &bout);
    fcli_err_report(1);
    RCEND;
    f_out("Round-trip re-generated artifact (type=%s) from input file:\n",
          fsl_satype_cstr(mf.type));
    if(bout.used<2000){
      f_out("%b", &bout);
    }else{
      f_out("Rather large - not dumping to console.\n");
    }
    if( ofile ){
      f_out("Dumping artifact to file [%s]\n", ofile);
      rc = fsl_buffer_to_filename(&bout, ofile);
      RCEND;

      fsl_buffer sha = fsl_buffer_empty;
      rc = fsl_cx_hash_filename(f, 0, ofile, &sha);
      assert(!rc);
      f_out("SHA of [%s] = [%b]\n", ofile, &sha);
      fsl_buffer_clear(&sha);
    }
  }

  if(App.doCrosslink){
    fsl_cx_flag_set(f, FSL_CX_F_SKIP_UNKNOWN_CROSSLINKS, 1);
    f_out("Disabling errors for currently-unhandled crosslink types.\n");
    if(mf.rid){
      f_out("Crosslinking manifest #%d ...\n", (int)mf.rid);
      rc = fsl__deck_crosslink_one( &mf );
      f_out("Crosslink says: %s\n", fsl_rc_cstr(rc));
      fcli_err_report(1);
    }
  }

  end:
  fsl_buffer_clear(&buf);
  fsl_buffer_clear(&bout);
  fsl_deck_finalize(&mf);
  return rc;

}

static void fcli_local_help(void){
  puts("If -f is not used, it reads from the first non-flag argument. "
       "It will read from stdin if stdin is not a terminal and "
       "neither -f nor any non-flag arguments are provided.");
}

int main(int argc, char const * const * argv ){
  int rc = 0;
  fsl_cx * f = 0;
  fsl_db * db = 0;
  bool failCrosslink = 0;
  bool fDryRun = false;
  const char * mfile = 0;
  fcli_cliflag FCliFlags[] = {
    FCLI_FLAG("f","file","filename",&mfile,
              "Read artifact from this file. "
              "Default is the first non-flag argument."),
    FCLI_FLAG_BOOL("n","dry-run",&fDryRun, "Dry-run mode."),
    FCLI_FLAG("o","output", "filename", &App.oFile,
              "Optional file to write round-trip-generated artifact "
              "to."),
    FCLI_FLAG_BOOL("j","json",&App.emitJson,
                   "With -o, emit the resulting artifact as JSON."),
    FCLI_FLAG_BOOL("c","crosslink",&App.doCrosslink,
                   "Crosslink the parsed artifact."),
    FCLI_FLAG_BOOL("fx","fail-xlink",&failCrosslink,
                   "Causes crosslinking to forcibly fail. Implies -c."),
    FCLI_FLAG_BOOL("r","r-card",&App.checkRCard,
                   "Confirms the R-card value (if any) on the "
                   "parsed artifact."),
    FCLI_FLAG_BOOL(0,"save",&App.saveDeck,
                   "Save the deck into the current repo. "
                   "USE WITH MUCH CAUTION!"),
    fcli_cliflag_empty_m
  };
  fcli_help_info FCliHelp = {
  "Parses fossil structural artifacts for libfossil testing purposes.",
  "-f FILE (or first non-flag argument)",
  fcli_local_help
  };

  rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  RCEND;
  if(failCrosslink) App.doCrosslink = true;
  if(!mfile){
    mfile = fcli_next_arg(1);
    if(!mfile){
      if(!fsl_isatty(0)){
        mfile = "-";
      }else if(!mfile){
        rc = fcli_err_set(FSL_RC_MISUSE,
                          "No input file specified. Try --help.");
        goto end;
      }
    }
  }
  f = fcli_cx();
  db = fsl_cx_db_repo(f);
  if(!db){
    rc = fsl_cx_err_set(f, FSL_RC_MISUSE,
                        "This app requires a repository db.");
    goto end;
  }
  rc = fsl_cx_txn_begin(f);

  fsl_xlink_listener( f,
#if !MY_OVERRIDE_XLINK_CHECKIN
                      fcli.appName,
#else
                      "fsl/checkin/timeline",
#endif
                      my_xlink_f, &failCrosslink );
  if(fcli_has_unused_flags(0)) goto end;

  rc = test_parse_1(mfile);

  end:
  if(f && fsl_cx_txn_level(f)){
    if(!rc && fDryRun){
      f_out("Dry-run mode. Rolling back transaction.\n");
    }
    fsl_cx_txn_end(f, rc||fDryRun);
  }
  return fcli_end_of_main(rc);
}
#undef RCEND
