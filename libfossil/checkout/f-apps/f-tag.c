/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/

/*******************************************************************
  This file implements a basic artifact tagging [test] app using the
  libfossil API.
*/

#include "libfossil.h"
#include <string.h>

#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

/**
    Just experimenting with fsl_xlink_listener() and friends.
*/
static int tag_xlink_f(fsl_deck * d, void * state){
  FCLI_V(("Crosslink callback for %s artifact RID %"FSL_ID_T_PFMT"\n",
           fsl_satype_cstr(d->type), d->rid));
  return *((char const *)state) /* demonstrate what happens when crosslinking fails. */
    ? FSL_RC_IO
    : 0;
}

static int MarkerA = 0;
static int MarkerT = 0;
static struct App_ {
  fsl_list liTgt;
  fsl_list liTags;
  const char * flagVal;
  const char * targetSym;
  void * MarkerA;
  void * MarkerT;
} App = {
fsl_list_empty_m,
fsl_list_empty_m,
NULL,NULL,&MarkerA,&MarkerT
};


static int f_is_reserved_tag_prefix(char const *zTag){
  if(zTag && *zTag){
    if(strncmp(zTag,"wiki-",5U)==0) return 5;
    else if(strncmp(zTag,"sym-",4U)==0) return 4;
    else if(strncmp(zTag,"tkt-",4U)==0) return 4;
    else if(strncmp(zTag,"event-",6U)==0) return 6;
  }
  return 0;
}

static int fcli_flag_callback_tag(fcli_cliflag const *f){
  if(!App.targetSym){
    return fcli_err_set(FSL_RC_MISUSE,"The --artifact flag "
                        "must be specified before --tag.");
  }
  assert(App.flagVal==*((char const **)f->flagValue));
  char * t = 0;
  char * v = 0;
  char * arg = fsl_strdup(App.flagVal);
  int rc = 0;
  const char * z = arg;
  App.flagVal = 0;
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
    case '-': ttype = FSL_TAGTYPE_CANCEL; ++t; break;
    case '*': ttype = FSL_TAGTYPE_PROPAGATING; ++t; break;
    case '+':
      ++t;
      /* fall through */
    default:
      ttype = FSL_TAGTYPE_ADD;
      break;
  }
  int const prefixCheck = f_is_reserved_tag_prefix(t);
  if(prefixCheck){
    rc = fcli_err_set(FSL_RC_MISUSE,
                      "Do not use the tag prefix '%.*s'. "
                      "It is reserved for fossil-internal use and will "
                      "not be properly processed as a '%.*s' tag.",
                      prefixCheck, t, prefixCheck, t);
    /* If we ever do allow sym- tags here, they must require
       a value. */
  }else{
    fsl_card_T * tt = fsl_card_T_malloc(ttype, 0, t, v);
    fsl_list_append(&App.liTags, tt);
    fsl_list_append(&App.liTgt, (void*)App.targetSym);
  }
  fsl_free(tOrig);
  fsl_free(v);
  return rc ? rc : FCLI_RC_FLAG_AGAIN;
}

int main(int argc, char const * const * argv ){
  int rc = 0;
  const char * symToTag = NULL;
  fsl_cx * f = 0;
  fsl_db * db = 0;
  char failCrosslink = 0;
  char const * userName = 0;
  fsl_deck mf = fsl_deck_empty;
  bool inTrans = 0;
  int vbose = 0;
  bool fDryRun = false;
  bool testMode = false;
  fsl_size_t i;
  fcli_pre_setup();
  fcli_cliflag FCliFlags[] = {
  /* Order is important: -a must come before -t so that args processing
     can DTRT. */
    FCLI_FLAG("a","artifact","artifact-id-to-tag", &App.targetSym,
              "Target artifact ID or symbolic name. "
              "Applies to all --tag flags."),
    FCLI_FLAG_X("t","tag","name[=value]",&App.flagVal,
                fcli_flag_callback_tag,
                "Adds the given tag with an optional value. "
                "May be used multiple times. "
                "Prefix the name with - for a cancellation tag or "
                "* for a propagating tag. A prefix of + (add tag) is "
                "equivalent to no prefix."
                ),
    FCLI_FLAG_BOOL("n","dry-run",&fDryRun,"Dry-run mode."),
    FCLI_FLAG_BOOL(0,"test", &testMode,
                   "Implies --dry-run and dumps artifact to stdout."),
    fcli_cliflag_empty_m
  };
  fcli_help_info FCliHelp = {
    "Creates control (tag) artifacts for a fossil repository.",
    "--artifact ID [...other options]",
    NULL
  };

  rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  f = fcli_cx();
  db = fsl_cx_db_repo(f);
  if(!db){
    rc = fsl_cx_err_set(f, FSL_RC_MISUSE,
                        "This app requires a repository db.");
    goto end;
  }
  vbose = fcli_is_verbose();
  failCrosslink = 0;//fcli_flag2("fx", "fail-xlink", NULL);
  fsl_xlink_listener( f, fcli.appName, tag_xlink_f, &failCrosslink );

  if(testMode) fDryRun = true;
  if(fcli_has_unused_flags(0)) goto end;
  else if(!App.liTags.used || !App.liTgt.used){
    rc = fcli_err_set(FSL_RC_MISUSE, "The --tag and --artifact "
                      "flags are required. Use --help for more info.");
    goto end;
  }

  userName = fsl_cx_user_get(f) /* set up by fcli */;
  if(!userName || !*userName){
    rc = fcli_err_set(FSL_RC_NOT_FOUND,
                      "Could not determine fossil user name. "
                      "Please specify %sone with --user|-U=name.",
                      userName ? "a non-empty " : "");
    goto end;
  }
  fsl_deck_init(f, &mf, FSL_SATYPE_CONTROL);

  rc = fsl_deck_U_set(&mf, userName);
  if(!rc) rc = fsl_deck_D_set(&mf, fsl_julian_now());
  if(rc) goto end;

  assert(App.liTags.used == App.liTgt.used);
  fsl_db_txn_begin(db);
  inTrans = 1;
  for( i=0; i<App.liTags.used; ++i ){
    fsl_card_T * tc = (fsl_card_T *)App.liTags.list[i];
    char const * sym = (char const *)App.liTgt.list[i];
    App.liTags.list[i] = NULL;
    assert(!tc->uuid);
    if(rc){
      fsl_card_T_free(tc);
      continue;
    }
    rc = fsl_sym_to_uuid(f, sym, FSL_SATYPE_ANY, &tc->uuid, NULL);
    if(!rc) rc = fsl_deck_T_add2(&mf, tc);
    if(rc) fsl_card_T_free(tc);
  }
  if(rc) goto end;

  if(testMode){
    rc = fsl_deck_unshuffle(&mf, 0);
    if(rc) goto end;
    rc = fsl_deck_output(&mf, fsl_output_f_FILE, stdout);
    if(rc) goto end;
  }else{
    rc = fsl_deck_save( &mf, 0 );
  }

  if(!rc && vbose){
    f_out("Applied tags to [%s] for user [%s]. New tag: RID %"FSL_ID_T_PFMT"\n",
          symToTag, userName, mf.rid);
  }

  end:
  if(inTrans){
    if(fDryRun && vbose) f_out("Dry-run mode. Rolling back transaction.\n");
    fsl_db_txn_end(db, fDryRun || rc);
    inTrans = 0;
  }
  fsl_deck_finalize(&mf);
  for( i=0; i<App.liTags.used; ++i ){
    fsl_card_T_free((fsl_card_T *)App.liTags.list[i]);
  }
  fsl_list_reserve(&App.liTags, 0);
  fsl_list_reserve(&App.liTgt, 0);
  return fcli_end_of_main(rc);
}
