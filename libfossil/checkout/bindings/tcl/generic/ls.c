#include "ftcl.h"
#include <string.h> /*memset()*/

#if 1
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)
#else
#define MARKER(pfexp) (void)0
#endif

/** State for FTCX__METHOD_NAME(xLs). */
typedef struct {
  ftcl_cx * fx;
  Tcl_Obj * tOutList;
  fsl_id_t rid;
  struct {
    bool bCollectResult;
    bool bVerbose;
    bool bSize;
    /** potential todo: 0 = leave out renames, 1 = set them, 2 = unset
        them if empty */
    char iRenames;
    /** 0 = no file times, 1 = UTC, 2 = local */
    char iTime;
  } flag;
  struct {
    /** --glob patterns. */
    fsl_list *pGlob;
    /** Number of --match patterns. Owned by Tcl. */
    Tcl_Obj **aPattern;
    /* Number of items in this->aPattern */
    Tcl_Size nPattern;
  } m;
  struct {
    Tcl_Obj * tVarName;
    Tcl_Obj * tScript;
  } eval;
  struct {
    Tcl_Obj *tLambda;
    Tcl_Obj *tDict;
  } apply;
  struct {
    unsigned int total;
    unsigned int matched;
  } count;
  struct {
    Tcl_Obj * tName;
    Tcl_Obj * tPriorName;
    Tcl_Obj * tUuid;
    Tcl_Obj * tPerm;
    Tcl_Obj * tSize;
    Tcl_Obj * tTime;
    Tcl_Obj * tPermNorm;
    Tcl_Obj * tPermX;
    Tcl_Obj * tPermL;
  } cache;
} LsState;

static const LsState LsState_empty = {
  .fx = 0,
  .tOutList = 0,
  .rid = -1,
  .flag = {
    .bCollectResult = false,
    .bVerbose = false,
    .bSize = false,
    .iRenames = 0,
    .iTime = 0,
  },
  .m = {0,0,0},
  .eval = {0,0},
  .apply = {0},
  .count = {0U,0U},
  .cache = {0,0,0,0,0,0,0,0,0}
};

/**
   Basically a proxy for an F-card, but abstracted by one level to
   allow us to consolidate some code which deals with checkouts rather
   than in-repo files.
*/
struct LsFile {
  fsl_uuid_cstr uuid;
  char const * name;
  char const * rename;
  fsl_fileperm_e perm;
};

typedef struct LsFile LsFile;
/*static*/const LsFile LsFile_empty = {
  0,0,0,
  FSL_FILE_PERM_REGULAR
};

static void LsState_cleanup(LsState * const ls){
  if(ls->m.pGlob){
    fsl_glob_list_clear(ls->m.pGlob);
  }
  th_unref(ls->tOutList);
  th_unref(ls->eval.tVarName);
  th_unref(ls->eval.tScript);
  th_unref(ls->apply.tLambda);
  th_unref(ls->apply.tDict);
  th_unref(ls->cache.tName);
  th_unref(ls->cache.tPriorName);
  th_unref(ls->cache.tUuid);
  th_unref(ls->cache.tSize);
  th_unref(ls->cache.tTime);
  th_unref(ls->cache.tPermNorm);
  th_unref(ls->cache.tPermX);
  th_unref(ls->cache.tPermL);
  memset(ls, 0, sizeof(*ls));
}

static bool LsState_matches(LsState * const ls, char const * zName){
  ++ls->count.total;
  if( ls->m.pGlob &&
      fsl_glob_list_matches(ls->m.pGlob, zName) ){
    ++ls->count.matched;
    return true;
  }
  if( ls->m.nPattern ){
    Tcl_Size n = 0;
    char const * z;
    bool gotMatch = false;
    for( Tcl_Size i = 0; !gotMatch && i < ls->m.nPattern; ++i ){
      z = th_gs(ls->m.aPattern[i], &n);
      assert( z );
      if( Tcl_StringMatch(zName, z) ){
        ++ls->count.matched;
        return true;
      }
    }
  }
  return (ls->m.pGlob || ls->m.nPattern)
    ? false
    : (++ls->count.matched,true);
}

static inline Tcl_Obj * LsState_outList(LsState * const ls,
                                        bool initKeys){
  if( ls->flag.bCollectResult && !ls->tOutList ){
    ls->tOutList = Tcl_NewListObj(0,0);
    th_ref(ls->tOutList);
  }
  if( initKeys && !ls->cache.tName ){
    Tcl_Obj * k;
#define KEY(K,M) k=th_nso(K,-1); ls->cache.M = th_ref(k)
    KEY("name", tName);
    KEY("uuid", tUuid);
    KEY("perm", tPerm);
    KEY("-", tPermNorm);
    KEY("x", tPermX);
    KEY("l", tPermL);
    if(ls->flag.iRenames) {KEY("rename", tPriorName);}
    if(ls->flag.bSize) {KEY("size", tSize);}
    if(ls->flag.iTime) {KEY("time", tTime);}
#undef KEY
    if(!ls->fx->cache.tEmpty){
      ls->fx->cache.tEmpty = th_nso("",0);
      th_ref(ls->fx->cache.tEmpty);
    }
  }
  return ls->tOutList;
}

static int LsState_inject_kv(LsState * ls, Tcl_Obj *tTgtList,
                             Tcl_Obj *k, Tcl_Obj *v){
  Tcl_Interp * const tcl = ls->fx->cmdo.tcl;
  int rc = 0;
  Tcl_Obj * tMp = 0;
  if( ls->flag.bVerbose ){
    rc = th_loae(tcl, tTgtList, k);
    if( rc ) {
      th_bounce(tMp);
      goto end;
    }
  }
  tMp = (v) ? (v) : ls->fx->cache.tEmpty;
  th_ref(tMp);
  if( ls->flag.bVerbose ){
    rc = th_loae(tcl, tTgtList, tMp);
  }
  if( !rc && ls->eval.tVarName ){
    if( !Tcl_ObjSetVar2(
          tcl, ls->eval.tVarName, k, v,
          TCL_LEAVE_ERR_MSG) ) {
      rc = TCL_ERROR;
    }
  }
end:
  th_unref(tMp);
  return rc;
}

/**
   Visits one file for the given LsState, doing whatever's appropriate
   for ls's configuration. Returns a libfossil result code on error,
   which needs to be translated to a Tcl code (i.e. TCL_ERROR) by the
   caller.
*/
static int LsState_visit_file(LsState * const ls, LsFile const * const lf){
  int rc = 0 /* Tcl result code */, frc = 0 /* libfossil result code */;
  Tcl_Interp * const tcl = ls->fx->cmdo.tcl;
  fsl_cx * const fcx = ftcl_cx_fsl(ls->fx);

  if( !LsState_matches( ls, lf->name) ){
    return 0;
  }

  Tcl_Obj * tArg = 0;
  int const inject = ls->flag.bVerbose || !!ls->eval.tVarName
    /* True if we need to build up the map of keys to
       file info */;
  Tcl_Obj * const pOut = LsState_outList(ls, !!inject);

  if( inject ){
    char buf[32] = {0}/*timestamp and blob size buffer*/;
    char const * const zBuf = &buf[0];
    Tcl_Obj * tMp = 0;
    tArg = Tcl_NewListObj(0,0);
    th_ref(tArg);

    /* Put the given Tcl_Obj* key and value where they need to be. */
#define PUTKV(K,V) \
    rc = LsState_inject_kv(ls, tArg, K,V); \
    if( rc ) goto end

#define PUTCSTR(K,V)                 \
    tMp = (V) ? th_nso((V), -1) : 0; \
    PUTKV(K, tMp)

    assert(ls->cache.tUuid);
    PUTCSTR(ls->cache.tName, lf->name);
    PUTCSTR(ls->cache.tUuid, lf->uuid);
    if( ls->flag.iRenames ){
      PUTCSTR(ls->cache.tPriorName, lf->rename);
    }
    /* TODO: consider using "-" instead of an empty string for "normal
       file" perms to simplify consistent rendering. */
    PUTKV(ls->cache.tPerm,
             (FSL_FILE_PERM_EXE==lf->perm
              ? ls->cache.tPermX
              : (FSL_FILE_PERM_LINK==lf->perm
                 ? ls->cache.tPermL
                 : ls->cache.tPermNorm)));

    if( ls->flag.bSize ){
      fsl_id_t const frid = fsl_uuid_to_rid(fcx, lf->uuid);
      fsl_int_t sz = -1;

      assert(ls->cache.tSize);
      frc = fsl_content_size_v2(fcx, frid, &sz);
      if( frc ) goto end;
      assert(sz >= 0);
      (void)snprintf(buf, sizeof(buf), "%" FSL_INT_T_PFMT, sz);
      PUTCSTR(ls->cache.tSize, zBuf);
    }

    if( ls->flag.iTime ){
      fsl_time_t ftm = 0;
      fsl_id_t const fid = fsl_uuid_to_rid(fcx, lf->uuid);
      assert(fid>0);
      assert(ls->cache.tTime);
      frc = fsl_mtime_of_manifest_file(fcx, ls->rid, fid, &ftm);
      if( frc ) goto end;
      fsl_strftime_unix(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S",
                        ftm, ls->flag.iTime>1);
      PUTCSTR(ls->cache.tTime, zBuf);
    }
#undef PUTCSTR
#undef PUTKV

    if( pOut ){
      rc = th_loae(tcl, pOut, tArg);
      if( rc ) goto end;
    }
  }else{
    /* Non-verbose... */
    tArg = th_nso(lf->name, -1);
    th_ref(tArg);
    if( pOut ){
      rc = th_loae(tcl, pOut, tArg);
      if( rc ) goto end;
    }
  }

  if( ls->eval.tVarName ){
    assert( 0==rc );
    assert( ls->eval.tScript );
    /* Local vars were set up above */
    rc = Tcl_EvalObjEx(tcl, ls->eval.tScript, 0 );
    //if( rc ) goto end;
  }

end:
  if( frc && !rc ){
    ftcl_rs_fsl_cx(tcl, fcx, frc);
  }
  if( ls->eval.tVarName ){
    Tcl_UnsetVar2(tcl, th_gs1(ls->eval.tVarName), 0, 0);
  }
  th_unref(tArg);
  return frc ? frc : (rc ? FSL_RC_ERROR : rc);
}

static int LsState_visitor_F_card(fsl_card_F const * const fc,
                                  void * state ){
  LsFile const lf = {
    fc->uuid, fc->name, fc->priorName, fc->perm
  };
  return LsState_visit_file(state, &lf);
}

/**
   Lists files from a repo or checkout.
*/
FTCX__METHOD_DECL(xLs) {
  char const *zSym = 0;
  char * zUuid = 0;
  fsl_list liGlobs = fsl_list_empty;
  fsl_deck d = fsl_deck_empty;
  int frc = 0;/* libfossil-specific rc */
  bool isCkout = false;
  LsState ls = LsState_empty;

  ftcl_cmd_rcfx;
  ftcl_cmd_init;
  fsl_cx * const fcx = ftcl_cx_fsl(fx);
  assert(fcx);

  ftcl_fp_flag("--glob", "glob", th_F_CMDFLAG_LIST, fGlob);
  ftcl_fp_flag("--match", "pattern", th_F_CMDFLAG_LIST, fStrMatch);
  ftcl_fp_flag("--version", "artifact-name|:checkout:",
               0, fVersion);
  ftcl_fp_flag("-ckout", 0, 0, fCkout);
  ftcl_fp_flag("-not-found-ok", 0, 0, fNotFound);
  ftcl_fp_flag("-verbose", 0, 0, fVerbose);
  ftcl_fp_flag("-renames", 0, 0, fRename);
  ftcl_fp_flag("-size", 0, 0, fSize);
  ftcl_fp_flag("-time", 0, 0, fTime);
  ftcl_fp_flag("-localtime", 0, 0, fLtime);
  ftcl_fp_flag("-rescan-ckout", 0, 0, fRescan);
  ftcl_fp_flag("---eval", "varName script", 0, fEval)
    /* TODO: support varName of -$ to set call-local vars named $name,
       $uuid, $perm, $rename. Or maybe support  a dict like
       {name => localName uuid => localUuid ...}
       to configure the eval-time var names. */;

  /*ftcl_fp_flag("-dict", 0, 0, fEval);
    ^^^ TODO: tell ---eval to use a dict instead of array.
  */
  /*ftcl_fp_flag("--apply", "lambda", 0, fEval);
    ^^^ what to pass to it? */
  ftcl__db_open_check;
  assert(fp.xResult==th_rs);
  ftcl_fp_parse("?flags?");

  assert(2==fEval->consumes);
  //th_dump_flags("ls flags", tcl, &fp);

  ls.fx = fx;
  if( th_flags_argc(&fp) ){
    rc = ftcl_rs_argc(tcl, argv, NULL);
    goto end;
  }

  if( fVersion->nSeen && fCkout->nSeen ){
    rc = th_err(tcl, "Cannot use %s and %s together",
                fVersion->zFlag, fCkout->zFlag);
    goto end;
  }else{
    isCkout = !!fCkout->nSeen;
  }

  if( fVersion->nSeen ){
    assert( !fCkout->nSeen );
    Tcl_Size n = 0;
    zSym = th_gs(fVersion->pVal, &n);
    isCkout = (10==n && 0==fsl_strncmp(":checkout:", zSym, (fsl_size_t)n));
  }else{
    zSym = isCkout
      ? "current"
      : "trunk" /*FIXME: pull prefered branch name from the "main-branch"
                  repo settings*/;
  }

  /*MARKER(("zSym=%s\n", zSym));*/
  if( fGlob->nSeen ){
    Tcl_Size gargc = 0;
    Tcl_Obj ** gargv = 0;
    rc = Tcl_ListObjGetElements(tcl, fGlob->pVal, &gargc, &gargv);
    for(Tcl_Size i = 0; 0==rc && i < gargc; ++i ){
      frc = fsl_glob_list_parse( &liGlobs, th_gs1(gargv[i]) );
      if( frc ){
        rc = th_rs_oom_here(tcl);
      }
    }
    if(rc) goto end;
    ls.m.pGlob = &liGlobs;
  }

  if( fStrMatch->nSeen ){
    rc = Tcl_ListObjGetElements(tcl, fStrMatch->pVal,
                                &ls.m.nPattern, &ls.m.aPattern);
    if(rc) goto end;
  }


  ls.flag.bSize = !!fSize->nSeen;
  ls.flag.iTime = (fLtime->nSeen ? 2 : !!fTime->nSeen);
  ls.flag.bVerbose = /* -time, -localtime, or -size imply -verbose */
    (ls.flag.bSize || ls.flag.iTime)
    || fVerbose->nSeen;
  int skipResultVal = 0 /* true if we should not collect the
                           result value from the listing */;
  if( fEval->nSeen ){
    Tcl_Size narg = 0;
    Tcl_Obj **gargv = 0;
    Tcl_ListObjGetElements(tcl, fEval->pVal, &narg, &gargv);
    assert(fEval->consumes==narg && "flags parser ensures this");
    ls.eval.tVarName = th_ref(gargv[0]);
    ls.eval.tScript = th_ref(gargv[1]);
    //??? (void)th_cmdo_name(&fx->cmdo, true);
    ++skipResultVal;
  }

  ls.flag.bCollectResult = !skipResultVal;
  ls.flag.iRenames = !!fRename->nSeen;

  if( isCkout ){
    if( fRescan->nSeen ){
      rc = ftcl_cx_rescan(fx);
    }
    if( 0==rc ) {
      rc = th_err(tcl, "ls of the checkout is TODO");
    }
    goto end;
  }else{
    frc = fsl_deck_load_sym(fcx, &d, zSym, FSL_SATYPE_CHECKIN);
    switch(frc){
      case 0: break;
      case FSL_RC_NOT_FOUND:
        if( fNotFound->nSeen ){
          frc = 0;
        }
        goto end;
      default:
        goto end;
    }
    assert(!frc);
    ls.rid = d.rid;
    frc = fsl_deck_F_foreach(&d, LsState_visitor_F_card, &ls);
    /*MARKER(("frc=%s counters: total=%u matched=%u\n",
      fsl_rc_cstr(frc), ls.count.total, ls.count.matched));*/
    if( frc ) goto end;
  }

end:
  if( frc && !rc ){
    rc = ftcl_rs_fsl_cx(tcl, fcx, frc);
  }
  if(0 && rc){
    MARKER(("rc=%d <<<%s>>>\n", rc, Tcl_GetStringResult(tcl)));
  }
  if( !rc && ls.tOutList ){
    Tcl_SetObjResult(tcl, ls.tOutList);
  }
  LsState_cleanup(&ls);
  fsl_free(zUuid);
  fsl_deck_clean(&d);
  ftcl_cmd_cleanup;
  return rc;
}

void ftcl__dump_sizeofs_ls(void){
#define SO(T)                                   \
  printf("sizeof(%s)=%u\n", #T, (unsigned)sizeof(T))
  SO(LsState);
#undef SO
}

#undef MARKER
