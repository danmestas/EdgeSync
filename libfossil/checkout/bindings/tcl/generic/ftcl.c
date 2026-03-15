/*
** 2025 April 6
**
** The author disclaims copyright to this source code.  In place of a
** legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
*/

#include "ftcl.h"
#include <string.h> /* memset() */
#if 1
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)
#else
#define MARKER(pfexp) (void)0
#endif

extern FTCX__METHOD_DECL(xDb);
static FTCX__METHOD_DECL(xCkout);
static FTCX__METHOD_DECL(xFile);
static FTCX__METHOD_DECL(xFslClose);
static FTCX__METHOD_DECL(xFslOpen);
static FTCX__METHOD_DECL(xInfo);
static FTCX__METHOD_DECL(xLibVersion);
extern FTCX__METHOD_DECL(xLs) /*in ls.c*/;
static FTCX__METHOD_DECL(xOption);
static FTCX__METHOD_DECL(xPeriment);
static FTCX__METHOD_DECL(xResolve);

/* Per-ftcl_cx-instance subcommands. */
static const th_subcmd ftcl__aFtclCxCmds[] = {
  /* Keep these sorted so that th_subcmd_...() can work
     faster. */
#define CMD(N,XF) th_subcmd_init(N, FTCX__METHOD_NAME(XF), 0)
  CMD("checkout",     xCkout),
  CMD("close",        xFslClose),
  CMD("db",           xDb),
  CMD("experiment",   xPeriment),
  CMD("file",         xFile),
  CMD("info",         xInfo),
  CMD("ls",           xLs),
  CMD("open",         xFslOpen),
  CMD("option",       xOption),
  CMD("resolve",      xResolve),
#undef CMD
};

#if 1
static int bogoMutex = 0;
#  define ftcl__mutex_t void
#  define ftcl__mutex_alloc(X) &bogoMutex
#  define ftcl__mutex_free(M) (void)(M)
#  define ftcl__mutex_enter(M) (void)(M)
#  define ftcl__mutex_leave(M) (void)(M)
#else
/* A current build limitation keeps this from linking. */
#  define ftcl__mutex_t sqlite3_mutex
#  define ftcl__mutex_alloc(TYPE) sqlite3_mutex_alloc(TYPE)
#  define ftcl__mutex_free(M) sqlite3_mutex_free(M)
#  define ftcl__mutex_enter(M) sqlite3_mutex_enter(M)
#  define ftcl__mutex_leave(M) sqlite3_mutex_leave(M)
/**
   This library's global (non-recursive) mutex. It doesn't block all
   that much stuff.
*/
#endif

/** Global state */
static struct {
  struct {
    ftcl__mutex_t * m;
    int locked;
  } mutex;
  const struct {
    const th_subcmd * const a;
    unsigned n;
  } cxCmd;
  const struct {
    void const * const fsl;
    void const * const db;
  } typeId;
  /**
     Enum of database names.
  */
  const th_enum eDbType;
  struct {
    /** Cache of lists used by command flag parsing (which is a
        frequent thing). Test scripts used up to 3 concurrently. */
    th_list list[4];
  } cache;
  struct {
    unsigned todo;
  } metrics;
} Ftcl = {
  .mutex = {
    .m = NULL,
    .locked = 0
  },
  .cxCmd = {
    .a = ftcl__aFtclCxCmds,
    .n = sizeof(ftcl__aFtclCxCmds)/sizeof(ftcl__aFtclCxCmds[0])
  },
  .typeId  = {
    "fsl_cx", "fsl_db"
  },
  .eDbType = {
    3, {
#define E(N,V) {N, sizeof(N)-1, V}
      E("repo",     FSL_DBROLE_REPO),
      E("ckout",    FSL_DBROLE_CKOUT),
      E("config",   FSL_DBROLE_CONFIG)
#undef E
    }
  },
  .cache = {
    .list = {
      th_list_empty_m, th_list_empty_m, th_list_empty_m,
      th_list_empty_m
    }
  },
  .metrics = {
    .todo = 0
  }
};

void ftcl__atexit(void){
  if( Ftcl.mutex.m ){
    ftcl__mutex_t * const p = Ftcl.mutex.m;
    Ftcl.mutex.m = 0;
    ftcl__mutex_free(p);
  }
  for(size_t i = 0;
      i < sizeof(Ftcl.cache.list)/sizeof(Ftcl.cache.list[0]);
      ++i ){
    th_list_reserve(&Ftcl.cache.list[i], 0);
  }
}

void * ftcl_realloc(void *p, size_t n){
  return fsl_realloc(p, (fsl_size_t)n);
}

void ftcl_free(void *p){
  return fsl_free(p);
}

void const * ftcl_typeid_fsl(void){
  return Ftcl.typeId.fsl;
}

void const * ftcl_typeid_db(void){
    return Ftcl.typeId.db;
}

fsl_cx * ftcl_cx_fsl(ftcl_cx * const fx){
  return Ftcl.typeId.fsl==fx->cmdo.type ? fx->sub : NULL;
}

fsl_db * ftcl_cx_db(ftcl_cx * const fx){
  if(Ftcl.typeId.db==fx->cmdo.type) return fx->sub;
  else if(Ftcl.typeId.fsl==fx->cmdo.type){
    assert(fx->sub);
    return fsl_cx_db(fx->sub);
  }
  return NULL;
}

#undef ftcl__cx_as

/** Tcl finalizer for ftcl_cx instances. */
static void ftcl_cx_finalizer(void *p);

ftcl_cx * ftcl_cx_alloc(Tcl_Interp *tcl, void const * type, void * p){
  ftcl_cx * fx = fsl_malloc(sizeof(ftcl_cx));
  memset(fx, 0, sizeof(ftcl_cx));
  fx->cmdo.type = type;
  fx->sub = p;
  fx->cmdo.p = fx;
  fx->cmdo.tcl = tcl;
  fx->cmdo.dtor = ftcl_cx_finalizer;
  fx->null.n = 0;
  fx->null.z = fsl_strndup("",(fsl_int_t)fx->null.n);
  assert(fx->null.z);
  return fx;
}

#if 0
static void ftcl_cx_free(ftcl_cx *fx){
  if( fx ) ftcl_cx_unref(fx);
}
#endif

ftcl_cx * ftcl_cx_ref(ftcl_cx *fx){
  if( fx ){
    assert( ftcl_cx_finalizer == fx->cmdo.dtor );
    th_cmdo_ref(&fx->cmdo);
  }
  return fx;
}

void ftcl_cx_unref(ftcl_cx *fx){
  if( fx ){
    assert( ftcl_cx_finalizer == fx->cmdo.dtor );
    th_cmdo_unref(&fx->cmdo);
  }
}

void ftcl_cx_unplug(ftcl_cx *fx){
  assert( ftcl_cx_finalizer == fx->cmdo.dtor );
  th_cmdo_unset(&fx->cmdo);
  ftcl_cx_unref(fx);
}

void ftcl_cx_finalizer(void *p){
  /* Reminder to self: we're being called via the fx->cpdo.dtor,
     so we're already unplugged. */
  ftcl_cx * const fx = p;
  assert(fx && "Else we got a NULL ptr via Tcl's GC");
  assert( !fx->cmdo.allocStamp );
  assert( fx == fx->cmdo.p || 0 == fx->cmdo.p );
  if( fx->flags.verbose>1 ){
    MARKER(("ftcl_cx_finalizer ftcl_cx@%p %s@%p\n",
            fx, (char const *)fx->cmdo.type, fx->sub));
  }
  assert( ftcl_cx_finalizer == fx->cmdo.dtor || 0==fx->cmdo.dtor );
  //MARKER(("Freeing fx@%p %s@%p\n", fx, (char const *)fx->cmdo.type, fx->sub));
  extern void QueryState_invalidate_queries(ftcl_cx *) /* in db.c */;
  QueryState_invalidate_queries(fx);
  if( fx->sub ){
    fsl_cx * f;
    fsl_db * db;
    assert( fx->cmdo.type );
    if( (f = ftcl_cx_fsl(fx)) ){
      if( fcli_cx()!=f ){
        fsl_cx_finalize(f);
      }
    }else if( (db = ftcl_cx_db(fx)) ){
      fsl_db_close(db);
    }else{
      assert(!"not possible");
    }
    fx->sub = NULL;
  }
  fsl_free(fx->null.z);
  th_unref(fx->cache.tKeyDb);
  th_unref(fx->cache.tKeyAggCx);
  th_unref(fx->cache.tApply);
  th_unref(fx->cache.tEmpty);
  th_unref(fx->cache.tStar);
  th_unref(fx->cache.tFossil);
  th_unref(fx->null.t);
  memset(fx, 0, sizeof(*fx));
  fsl_free(fx);
}

Tcl_Obj * ftcl_cx_apply(ftcl_cx * const fx){
  if( !fx->cache.tApply ){
    fx->cache.tApply = th_nso("apply",5);
    th_ref(fx->cache.tApply);
  }
  return fx->cache.tApply;
}

int ftcl_cx_rescan(ftcl_cx *fx){
  int rc = 0;
  fsl_cx * const ff = ftcl_cx_fsl(fx);
  if( ff ){
    if( (rc = fsl_ckout_changes_scan(ff)) ){
      rc = ftcl_rs_fsl_cx(fx->cmdo.tcl, ff, rc);
    }
  }else{
    rc = th_err(fx->cmdo.tcl, "No fossil instance is open");
  }
  return rc;
}

int ftcl_file_read(Tcl_Interp *tcl, char const * zFile, fsl_buffer * b){
  int rc;
  if( 0!=(rc = fsl_buffer_fill_from_filename( b, zFile )) ){
    rc = ftcl_err(tcl, "Error %s reading file %s",
                  fsl_rc_cstr(rc), zFile);
  }
  return rc;
}

int ftcl_rs_take(Tcl_Interp *tcl, int rc, char * z){
  Tcl_SetResult(tcl, (char *)z, TCL_VOLATILE);
  fsl_free(z);
  return rc;
}

int ftcl_rs_v(Tcl_Interp *tcl, int rc, char const * zFmt, va_list vargs ){
  char * z = fsl_mprintfv(zFmt, vargs);
  ftcl_rs_take(tcl, rc, z);
  return rc;
}

int ftcl_rs(Tcl_Interp *tcl, int rc, char const * zFmt, ...){
  va_list args;
  va_start(args,zFmt);
  ftcl_rs_v(tcl, rc, zFmt, args);
  va_end(args);
  return rc;
}

int ftcl_err(Tcl_Interp *tcl, char const * zFmt, ...){
  int rc;
  va_list args;
  va_start(args,zFmt);
  rc = ftcl_rs_v(tcl, TCL_ERROR, zFmt, args);
  va_end(args);
  return rc;
}

int ftcl_rs_fsl_cx(Tcl_Interp *tcl, fsl_cx * f, int fslrc){
  char const * zMsg = 0;
  int const rc2 = fsl_cx_err_get( f, &zMsg, NULL );
  int const rc = th_rs_c(tcl, ((fslrc || rc2) ? TCL_ERROR : TCL_OK),
                         zMsg);
  fsl_cx_err_reset(f);
  return rc;
}

int ftcl_rs_sqlite3(sqlite3 * db, Tcl_Interp *tcl, int triggeringRc){
  int rc = sqlite3_errcode(db);
  if(rc){
    char * z = fsl_mprintf("sqlite3 error #%d: %s",
                           rc, sqlite3_errmsg(db));
    rc = ftcl_rs_take(tcl, rc, z);
  }else{
    Tcl_ResetResult(tcl);
  }
  return (triggeringRc || rc) ? TCL_ERROR : TCL_OK;
}

int ftcl_rs_db(Tcl_Interp *tcl, fsl_db * db, int triggeringRc){
  char const * zMsg = 0;
  int rc = fsl_db_err_get(db, &zMsg, 0);
  if( rc ){
    rc = th_rs_c(tcl, rc ? rc : triggeringRc, zMsg);
    fsl_db_err_reset(db);
  }else{
    rc = ftcl_rs_sqlite3(db->dbh, tcl, triggeringRc);
  }
  return rc ? TCL_ERROR : TCL_OK;
}

int ftcl_rs_argc(
  Tcl_Interp *tcl,
  Tcl_Obj * const * argv,
  const char *zUsage
){
  if( argv ){
    Tcl_WrongNumArgs(tcl, 1, argv, zUsage);
  }else{
    ftcl_err(tcl, "Wrong # of args for unknown command%s%s",
             zUsage ? ": " : "",
             zUsage ? zUsage : NULL);
  }
  return TCL_ERROR;
}

#if 0
int ftcl_affirm_db_is_open(ftcl_cx * const fx){
  char const * const zErrIfNot =
    "This fossil context has no opened database";
  int rc = 0;
  if( !ftcl_cx_db(fx) || !ftcl_cx_fsl(fx) ){
    rc = zErrIfNot
      ? th_rs_c(fx->cmdo.tcl, TCL_ERROR, zErrIfNot)
      : TCL_ERROR;
  }
  return rc;
}
#endif

int ftcl__cx_dispatch( ftcl_cx *fx, int startAtArgc,
                       int argc, Tcl_Obj * const * argv,
                       unsigned nCmd, th_subcmd const * const aCmd,
                       int isSorted){
  if( argc<startAtArgc+1 ){
    return ftcl_rs_argc(fx->cmdo.tcl, argv, NULL);
  }
  return th_subcmd_dispatch(fx, fx->cmdo.tcl,
                             argc-startAtArgc,
                             argv+startAtArgc,
                             nCmd, aCmd, isSorted, NULL, ftcl_rs);
}

static ftcl__mutex_t * ftcl__mutex(void){
  if( !Ftcl.mutex.m ){
    ftcl__mutex_t * const m =
      ftcl__mutex_alloc(SQLITE_MUTEX_FAST);
    if( Ftcl.mutex.m ){
      ftcl__mutex_free(m);
    }else{
      Ftcl.mutex.m = m;
    }
  }
  return Ftcl.mutex.m;
}

void ftcl_mutex_enter(void){
  assert(Ftcl.mutex.m);
  assert(!Ftcl.mutex.locked
         && "Is only reliable with single-threaded use and is here "
         "to catch misuse of the mutex in such cases.");
  ftcl__mutex_enter(Ftcl.mutex.m);
  Ftcl.mutex.locked = 1;
}

void ftcl_mutex_leave(void){
  assert(Ftcl.mutex.m);
  assert(Ftcl.mutex.locked);
  Ftcl.mutex.locked = 0;
  ftcl__mutex_leave(Ftcl.mutex.m);
}

/**
   If p's has memory which can be put in the cache, it is
   taken from p and moved into the cache, else there are
   no side-effects.
*/
static void ftcl__cache_list_get( th_list * const p ){
  assert( Ftcl.mutex.locked );
  for(size_t i = 0;
      !p->list &&
      i < sizeof(Ftcl.cache.list)/sizeof(Ftcl.cache.list[0]);
      ++i ){
    th_list * const li = &Ftcl.cache.list[i];
    if( li->list ){
      if( p->capacity < li->capacity ){
        th_list_reserve(p, 0);
        *p = *li;
        *li = th_list_empty;
        /*MARKER(("Re-using cached list#%d with capacity %u\n",
          (int)i, (unsigned)p->capacity));*/
        return;
      }
    }
  }
}

/**
   If p has no the memory list and the cache does, move that memory
   from the cache to p's ownership, else there are no side-effects.
*/
static void ftcl__cache_list_put( th_list * const p ){
  assert( Ftcl.mutex.locked );
  for(size_t i = 0;
      p->list &&
      i < sizeof(Ftcl.cache.list)/sizeof(Ftcl.cache.list[0]);
      ++i ){
    th_list * const li = &Ftcl.cache.list[i];
    if( !li->list ){
      *li = *p;
      *p = th_list_empty;
      /*MARKER(("Caching list#%d with capacity %u\n",
        (int)i, (unsigned)li->capacity));*/
      return;
    }
  }
}

int ftcl__flags_parse(Tcl_Interp * tcl, int argc,
                      Tcl_Obj *const* argv, th_fp * fp){
  if( !fp->nonFlag.list ){
    ftcl_mutex_enter();
    ftcl__cache_list_get(&fp->nonFlag);
    ftcl_mutex_leave();
  }
  return th_flags_parse(tcl, argc, argv, fp);
}

void ftcl__flags_cleanup(th_fp * fp){
  if( fp->nonFlag.list ){
    ftcl_mutex_enter();
    ftcl__cache_list_put(&fp->nonFlag);
    ftcl_mutex_leave();
  }
  th_flags_parse_cleanup(fp);
}

/**
   The command object impl for fossil instances. This is distinct from
   the extension-provided command object, which has its own callback
   (Tclfossil_Cmd()) which may call into this one.

   This impl does no more than dispatch to one of the subcommands defined in
   Ftcl.aFtclCxCmds.
*/
static int ftcl__cx_ObjCmd(
  ClientData p,
  Tcl_Interp *tcl,
  int argc,
  Tcl_Obj * const * argv
){
  //th_dump_argv("ftcl__cx_ObjCmd", tcl, argc, argv);
  ftcl_cx * const fx = p;
  //assert( ftcl_cx_fsl(fx) );
  //fsl_cx_err_reset(ftcl_cx_fsl(fx));
  return ftcl__cx_dispatch(fx, 1, argc, argv, Ftcl.cxCmd.n, Ftcl.cxCmd.a, 1);
}

/*
** Adaptor that provides an objCmd interface to the NRE-enabled
** interface implementation.
*/
static int ftcl__cx_ObjCmdAdaptor(
  ClientData cd,
  Tcl_Interp *tcl,
  int argc,
  Tcl_Obj * const * argv
){
  return Tcl_NRCallObjProc(tcl, ftcl__cx_ObjCmd, cd, argc, argv);
}

/**
   Creates a new command object wrapping a new ftcl_cx instance which
   wraps f.

   If zCmdName is not NULL then it is used as a new proc name, else a
   name is synthesized. If zCmdName is NULL then a command name is
   synthesized.

   If zVarName is not NULL then Tcl_SetVar() is used to set a var with
   a value equal to the new command's name.

   If f is not NULL, ownership of f is taken over by the returned
   object, with the exception that if f is fcli_cx() then it takes
   "stewardship" instead of ownership.
*/
static ftcl_cx * ftcl__cx_create_cmd(Tcl_Interp *tcl, fsl_cx * f,
                                     Tcl_ObjCmdProc cmd,
                                     const char *zCmdName,
                                     //TObject * tVarName
                                     const char *zVarName){
  static unsigned counter = 0;
  ftcl_cx * fx = ftcl_cx_alloc(tcl, ftcl_typeid_fsl(), f);
  char * const z = zCmdName
    ? (char *)zCmdName
    : fsl_mprintf("fsl_cx#%u", ++counter);
  Tcl_Obj * tVarName = zVarName ? th_nso(zVarName,-1) : 0;
  assert( f == fx->sub );
  assert( ftcl_cx_finalizer == fx->cmdo.dtor );
  th_ref(tVarName);
  if( th_cmdo_plugin(&fx->cmdo, ftcl__cx_ObjCmdAdaptor, cmd,
                      z, tVarName) ){
    ftcl_cx_unref(fx);
    fx = 0;
  }else{
    assert( 1 == fx->cmdo.refCount );
  }
  th_unref(tVarName);
  if( z!=zCmdName ) fsl_free(z);
  return fx;
}

/**
   Copy/paste template for new ftcl_cx subcommands.
*/
/*static*/ FTCX__METHOD_DECL(xTemplate) {
  ftcl_cmd_rcfx;
  ftcl_cmd_init;
  ftcl_fp_flag("--foo",    "value", 0, fFoo);
  ftcl_fp_flag("-bar",     0, 0, fBar);
  ftcl_fp_parse("?flags?");
  ftcl_cmd_replace_argv;

  if( 1!=argc || !fp.nSeen ){
    rc = ftcl_err(tcl, "%s expects an argument",
                  th_gs1(argv[0]));
    goto end;
  }
  if( fFoo->nSeen ) {
    Tcl_Size n = 0;
    char const * const z = th_gs(fFoo->pVal, &n);
    (void)n; (void)z;
  }

end:
  ftcl_cmd_cleanup;
  return rc;
}

static FTCX__METHOD_DECL(xLibVersion) {
  return ftcl_rs(tcl, 0,
                "%s %s using libfossil %s",
                TEAISH_PKGNAME, TEAISH_VERSION,
                fsl_library_version_scm() );
}

static FTCX__METHOD_DECL(xResolve) {
  fsl_uuid_str zUuid = 0;
  fsl_satype_e eType = FSL_SATYPE_ANY;
  char const * const zUsage = "artifact-name";
  fsl_id_t rid = 0;

  ftcl_cmd_rcfx;
  fsl_cx * const f = ftcl_cx_fsl(fx);
  ftcl_fp_init;
  ftcl_fp_flag("-noerr", 0, 0, fNoFail);
  ftcl_fp_flag("-rid", 0, 0, fRid);
  ftcl_fp_parse(zUsage);

  if( 1!=th_flags_argc(&fp) ) {
    rc = ftcl_rs_argc(tcl, argv, zUsage);
    goto end;
  }
  rc = fsl_sym_to_uuid(f, th_flags_arg_cstr(&fp, 0, NULL),
                       eType, &zUuid, &rid);
  if( rc ){
    if( fNoFail->nSeen ){
      fsl_cx_err_reset(f);
      if( fRid->nSeen ){
        rc = ftcl_rs(tcl, 0, "0");
      }else{
        rc = th_rs_c(tcl, 0, "");
      }
    } else {
      rc = ftcl_rs_fsl_cx(tcl, f, rc);
    }
  }else{
    if( fRid->nSeen ){
      rc = ftcl_rs(tcl, 0, "%" FSL_ID_T_PFMT, rid);
    } else {
      rc = th_rs_c(tcl, 0, zUuid);
    }
  }
end:
  fsl_free(zUuid);
  ftcl_fp_cleanup;
  return rc;
}

/**
   Opens either fx or creates a new ftcl_cx and opens it.

   If called with no arguments, it returns a bitmask indicating which
   databases are open: 0 for none, 0x01 if a repo is opened, 0x02 if a
   checkout is opened, and 0x04 if the global config is opened.

   It normally requires 1 non-flag argument: either a directory name
   or a repository database name. In the former case, it looks for a
   fossil checkout in or above that directory. In the latter it opens
   the repository db. Opening a checkout necessarily opens its repo
   along with it, and some operations are available only from a
   checkout.

   It will fail if the repo is already opened.
*/
static FTCX__METHOD_DECL(xFslOpen) {
  int rc = 0;
  int dirCheck = 0;
  const char * zArg = 0;
  char const * zVarName = 0;
  char const * const zUsage = "repoFileOrCheckoutDir";
  th_decl_cmdflags_init;
  th_decl_cmdflag("-noparents", 0,  0, fNoParents );
  th_decl_cmdflag("--new-instance", "varName|-",  0, fNew );
  th_decl_cmdflag("-new-instance", 0,  0, fNew1 );
  ftcl_decl_cmdflags(fp, th_F_PARSE_CHECK_ALL);
  ftcl_cx * const fx = cx;
  fsl_cx * const f = ftcl_cx_fsl(fx);
  fsl_cx * ff = 0;   /* new instance for use with... */
  ftcl_cx * ffx = 0; /* new instance */
  fsl_cx * fxLink = 0; /* (ffx ? ffx : fx)->cx */
  assert( fx );

  if( 1==argc ){
    int const mask = 0 \
      | (fsl_cx_db_repo(f) ? 1 : 0)
      | (fsl_cx_db_ckout(f) ? 2 : 0)
      | (fsl_cx_db_config(f) ? 4 : 0);
    ftcl_rs(tcl, 0, "%d", mask);
    return 0;
  }
  //th_dump_argv("open argv",tcl,argc,argv);

  ftcl__cx_flags_parse(&fp, zUsage);
  if(0){
    f_out("argv[0]=%s argv[1]=%s fNoParents.nSeen=%u\n",
          th_gs1(argv[0]),
          th_gs1(argv[1]),
          fNoParents.nSeen);
  }
  //th_dump_flags("open flags", tcl, &fp);

  if(fNew.nSeen && fNew1.nSeen){
    rc = ftcl_err(tcl, "--new-instance X and -new-instance may "
                  "not be used together.");
  }else if(fNew1.nSeen){
    assert(!fNew.nSeen);
    fNew.nSeen = fNew1.nSeen;
  }

  if( 1!=th_flags_argc(&fp) ){
    if( !fNew.nSeen || th_flags_argc(&fp)>1 ){
      rc = ftcl_rs_argc(tcl, argv, zUsage);
      goto end;
    }
  }

  if( th_flags_argc(&fp) ){
    assert( fp.nonFlag.list );
    Tcl_Obj * po = fp.nonFlag.list[0];
    assert( po );
    zArg = th_gs1(po);
    //f_out("zArg=%s\n", zArg);
    dirCheck = fsl_dir_check(zArg);
    //f_out("zArg=%s dircheck=%d\n", zArg, dirCheck);
    if( 0==dirCheck ){
      rc = ftcl_err(tcl,
                    "Argument is neither a repository db "
                    "nor a directory: %s", zArg);
      goto end;
    }
  }

  if( fNew.nSeen ){
    zVarName = fNew.pVal ? th_gs1(fNew.pVal) : 0;
    //f_out("--as %s\n", zVarName);
    if( zVarName && '-'==zVarName[0] && !zVarName[1] ){
      zVarName = NULL;
    }
    assert(!ff);
    rc = fsl_cx_init( &ff, NULL );
    if( rc ){
      rc = TCL_ERROR;
      if( ff ){
        rc = ftcl_rs_fsl_cx(tcl, ff, rc);
        fsl_cx_finalize(ff);
        ff = 0;
      }
      goto end;
    }else{
      ffx = ftcl__cx_create_cmd(tcl, ff, ftcl__cx_ObjCmd,
                                NULL, zVarName);
      ffx->flags.verbose = fx->flags.verbose;
      th_rs_o(tcl, 0, th_cmdo_name(&ffx->cmdo, false)
               /* if zVarName is set with a leading :: prefix, it comes out
                  different than Tcl_GetCommandName()... */);
    }
  }/*--new-instance*/

  fxLink = ftcl_cx_fsl(ffx ? ffx : fx);
  if( fsl_cx_db_repo(fxLink) ){
    rc = ftcl_err( tcl, "This fossil instance is already open. "
                   "'close' it before opening something else.");
  }else{
    if( dirCheck ){
      /* We got a file/dir arg */
      if( dirCheck>0 ){
        // checkout dir
        rc = fsl_ckout_open_dir(fxLink, zArg, fNoParents.nSeen==0);
      }else{
        // repo db
        rc = fsl_repo_open(fxLink, zArg);
      }
      if( rc ){
        rc = ftcl_rs_fsl_cx(tcl, fxLink, rc);
      }
    }
  }

end:
  if(rc) {
    ftcl_cx_unref(ffx);
    ffx = 0;
    fsl_cx_finalize(ff);
    ff = 0;
  }
  ftcl__flags_cleanup(&fp);
  return rc;
}

static FTCX__METHOD_DECL(xFslClose){
  int rc;
  ftcl_cx * const fx = cx;
  fsl_cx * const f = ftcl_cx_fsl(fx);

  /* TODO: do not permit closing when db or stmt commands are
     actively running. */
  th_decl_cmdflags_init;
  th_decl_cmdflag("-keep", 0, 0, fKeep);
  ftcl_decl_cmdflags(fp, 0);
  ftcl__cx_flags_parse(&fp, NULL);
  if( 0==th_flags_argc(&fp) ){
    //MARKER(("xFslClose for %p@%s\n", f, fsl_cx_ckout_dir_name(f, 0)));
    assert( fx->cmdo.refCount );
    extern void QueryState_invalidate_queries(ftcl_cx *) /* in db.c */;
    QueryState_invalidate_queries(fx);
    fsl_close_dbs(f);
    //MARKER(("f=%p fcli cx=%p\n", f, fcli_cx()));
    //th_dump_flags("fsl close", tcl, &fp);
    if( !fKeep.nSeen && fcli_cx()!=f ){
      //MARKER(("xFslClose unplugging %p@%s\n", f, fsl_cx_ckout_dir_name(f, 0)));
      ftcl_cx_unplug(fx) /* might free fx/f */;
    }
  }else {
    rc = ftcl_rs_argc(tcl, argv, NULL);
  }
  end:
  ftcl__flags_cleanup(&fp);
  return rc;
}

static FTCX__METHOD_DECL(xOption){
  th_decl_cmdflags_init;
  th_decl_cmdflag("-verbose", 0, 0, fV);
  th_decl_cmdflag("--verbosity", "level", 0, fV2);
  th_decl_cmdflag("-get", 0, 0, fGet);
  th_decl_cmdflag("--test", "value", th_F_CMDFLAG_LIST, fTest);
  th_decl_cmdflag("---test", "arg1 arg2", th_F_CMDFLAG_LIST, fTest2);
  ftcl_decl_cmdflags(fp, 0);
  assert( 2==fTest2.consumes );
  Tcl_Obj * vGetList = 0;
  char * z = 0;
  char const *zc = 0;
  ftcl_cx * const fx = cx;
  int rc;

  ftcl__cx_flags_parse(&fp, NULL);
  if( th_flags_argc(&fp) ){
    //th_dump_flags("xOption flags", tcl, &fp);
    rc = ftcl_rs_argc(tcl, argv, "TODO: list -flags here");
    goto end;
  }else if( fGet.nSeen && fp.nSeen==1 ){
    rc = ftcl_err(tcl, "-get requires other flags");
    goto end;
  }
  if( fGet.nSeen && fp.nSeen>2 ){
    vGetList = Tcl_NewListObj(0, 0);
    th_ref(vGetList);
  }
#define AddResult \
  assert( z || zc );                                            \
  if(vGetList) {                                                \
    Tcl_ListObjAppendElement(tcl, vGetList,                 \
                             Tcl_NewStringObj(zc ? zc : z,-1)); \
  }else{                                                        \
    ftcl_rs(tcl, 0, "%s", zc ? zc : z);                     \
  }                                                             \
  fsl_free(z); z = 0; zc = 0;

  unsigned char beVerbose = fx->flags.verbose;
  if(fV.pVal){
    if( !fGet.nSeen ) beVerbose = 1;
    z = fsl_mprintf("%d", (int)beVerbose);
    AddResult;
  }
  if(fV2.pVal){
    int x = 0;
    if( 0==(rc = Tcl_GetIntFromObj(tcl, fV2.pVal, &x)) ){
      if( !fGet.nSeen ) beVerbose = x & 0xff;
      z = fsl_mprintf("%d", (int)(x & 0xff));
      AddResult;
    }else{
      goto end;
    }
  }
  if(fTest.pVal){
    zc = th_gs1(fTest.pVal);
    AddResult;
  }
  if(fTest2.pVal){
    zc = th_gs1(fTest2.pVal);
    AddResult;
  }
  fx->flags.verbose = beVerbose;
#undef AddResult
end:
  assert( !z && !zc );
  if( vGetList ){
    if( 0==rc ) ftcl_rs(tcl, 0, th_gs1(vGetList));
    th_unref(vGetList);
  }
  ftcl__flags_cleanup(&fp);
  return rc;
}

static FTCX__METHOD_DECL(xInfo){
  ftcl_cmd_rcfx;
  ftcl_cmd_init;
  ftcl_fp_flag("-repo-db", 0, 0, fRepoDb);
  ftcl_fp_flag("-checkout-db", 0, 0, fCkoutDb);
  ftcl_fp_flag("-checkout-dir", 0, 0, fCkoutDir);
  ftcl_fp_flag("-lib-version", 0, 0, fVersion);
  ftcl_fp_parse("-flag");

  if( th_flags_argc(&fp) ) {
    rc = ftcl_rs_argc(tcl, argv, NULL);
    goto end;
  }else if( 1 != fp.nSeen ){
    rc = ftcl_err(tcl, "Only one info flag may be provided at a time");
    goto end;
  }

  fsl_cx * const fcx = ftcl_cx_fsl(fx);
  if( fVersion->nSeen ){
    rc = ftcl_rs(tcl, 0,
                 "%s %s using libfossil %s",
                 TEAISH_PKGNAME, TEAISH_VERSION,
                 fsl_library_version_scm() );
    goto end;
  }else if(!fcx){
    rc = th_rs_c(tcl, 0, "");
    goto end;
  }

  char const *z = 0;
  fsl_size_t nZ = 0;
  if( fRepoDb->nSeen ){
    z = fsl_cx_db_file_repo(fcx, &nZ);
    rc = ftcl_rs(tcl, 0, "%.*s", (int)nZ, z);
    goto end;
  }else if( fCkoutDb->nSeen ){
    z = fsl_cx_db_file_ckout(fcx, &nZ);
    rc = ftcl_rs(tcl, 0, "%.*s", (int)nZ, z);
    goto end;
  }else if( fCkoutDir->nSeen ){
    z = fsl_cx_ckout_dir_name(fcx, &nZ);
    rc = ftcl_rs(tcl, 0, "%.*s", (int)nZ, z);
    goto end;
  }
  rc = ftcl_err(tcl, "Should not have gotten this far");

end:
  ftcl_cmd_cleanup;
  return rc;
}

static fsl_cx * ftck__ckout_opened(ftcl_cx * fx){
  fsl_cx * f = ftcl_cx_fsl(fx);
  if( !f ){
    th_err(fx->cmdo.tcl, "Missing fsl_cx instance (has it be closed?)");
    f = 0;
  }else if( !fsl_needs_ckout(f) ){
    ftcl_rs_fsl_cx(fx->cmdo.tcl, f, FSL_RC_NOT_A_CKOUT);
    f = 0;
  }
  return f;
}

static FTCX__METHOD_DECL(xFileStub) {
  ftcl_cmd_rcfx;
  ftcl_cmd_init;
  ftcl_fp_flag("-placeholder", 0, 0, fFoo);
  ftcl_fp_parse(NULL);
  ftcl_cmd_replace_argv;

  if( 1!=argc ){
    rc = ftcl_rs_argc(tcl, argv, NULL);
    goto end;
  }

  rc = th_err(tcl, "NYI");

end:
  ftcl_cmd_cleanup;
  return rc;
}

static FTCX__METHOD_DECL(xFile) {
  static const th_subcmd aSubs[] = {
    /* Keep these sorted by name! */
    th_subcmd_init("placeholder", FTCX__METHOD_NAME(xFileStub), 0)
    /* TODOS? size, mtime, relative, dirname, glob, touch,
       normalize tail... */
  };
  static const unsigned nSubs = sizeof(aSubs)/sizeof(aSubs[0]);
  ftcl_cx * const fx = cx;

  if(!ftck__ckout_opened(fx)) return TCL_ERROR;
  return ftcl__cx_dispatch(fx, 1, argc, argv, nSubs, aSubs, 1);
}


static FTCX__METHOD_DECL(xCkoutScan) {
  ftcl_cx * const fx = cx;
  assert( ftck__ckout_opened(fx) );
  if( 1!=argc ){
    return ftcl_rs_argc(tcl, argv, NULL);
  }
  return ftcl_cx_rescan(fx);
}

static FTCX__METHOD_DECL(xCkout) {
  static const th_subcmd aSubs[] = {
    /* Keep these sorted by name! */
    th_subcmd_init("scan", FTCX__METHOD_NAME(xCkoutScan), 0)
    /* TODOS? ls, update, revert, merge, status, checkin ...

       For checkins, because they're built up incrementally, we could
       use a new context type.
    */
  };
  static const unsigned nSubs = sizeof(aSubs)/sizeof(aSubs[0]);
  ftcl_cx * const fx = cx;

  if(!ftck__ckout_opened(fx)) return TCL_ERROR;
  return ftcl__cx_dispatch(fx, 1, argc, argv, nSubs, aSubs, 1);
}


static FTCX__METHOD_DECL(xPeriment) {
#define SO(T)                                   \
  printf("sizeof(%s)=%u\n", #T, (unsigned)sizeof(T))
  SO(Ftcl);
  SO(fsl_db);
  SO(fsl_stmt);
  SO(ftcl_cx);
  SO(ftcl_db);
  SO(th_cmdflag);
  SO(th_cmdo);
  SO(th_enum);
  SO(th_enum_entry);
  SO(th_fp);
  SO(th_list);
  SO(th_subcmd);
#undef SO
  extern void ftcl__dump_sizeofs_db(void)/*in db.c*/;
  extern void ftcl__dump_sizeofs_ls(void)/*in ls.c*/;
  extern void ftcl__dump_sizeofs_udf(void)/*in udf.c*/;
  ftcl__dump_sizeofs_db();
  ftcl__dump_sizeofs_ls();
  ftcl__dump_sizeofs_udf();
#if 0
  Tcl_SetObjResult(tcl, argv[0]);
  th_rs_c(tcl, 0, Tcl_GetVar(
              fx->tcl, th_gs1(argv[0]), 0
            ));
#else
  ftcl_rs(tcl, 0, "Nothing to see here");
#endif
  return 0;
}

/**
   Library-level command object impl.
*/
static int Tclfossil_Cmd(ClientData cx, Tcl_Interp *tcl, int argc,
                         Tcl_Obj *const* argv){
  /* Global subcommands via the ftcl object. */
  static const th_subcmd aSubs[] = {
    /* Keep these sorted by name! */
    th_subcmd_init("lib-version", FTCX__METHOD_NAME(xLibVersion), 0)
    /* FIXME: replace that with (info -lib-version) */,
    th_subcmd_init("option",      FTCX__METHOD_NAME(xOption), 0),
  };
  static const unsigned nSubs = sizeof(aSubs)/sizeof(aSubs[0]);

  int rc = 0;
  ftcl_cx * const fx = cx;
  th_decl_cmdflags_init;
  th_decl_cmdflag("--new-instance", "varName|-", 0, fNew);
  th_decl_cmdflag("-new-instance", 0, 0, fNew1);
  ftcl_decl_cmdflags(fp, 0);
  ftcl__cx_flags_parse(&fp, NULL);

  if( fNew.nSeen || fNew1.nSeen ){
    /* Behave as if `open` had been called. */
    rc = ftcl__cx_xFslOpen(cx, tcl, argc, argv);
    goto end;
  }

  fsl_cx_err_reset(ftcl_cx_fsl(fx));

  if( th_flags_argc(&fp) ){
    /* First try an extension-level command before falling back to
       instance-level commands. */
    Tcl_Obj** pvv = th_flags_argv(&fp);
    th_subcmd const * const ps =
      th_subcmd_search(pvv[0], nSubs, aSubs, 1);
    //MARKER(("subcommand %s search=%s\n", th_gs1(pvv[0]), ps ? ps->zName : "<null>"));

    /* TODO?: use different lists of subcommands based on whether they
       require an open repo? That would severely confuse the -?
       output. */
    if(ps){
      rc = ps->xFunc(fx, tcl, th_flags_argc(&fp), pvv);
      goto end;
    }
    rc = th_subcmd_dispatch(
      fx, tcl, th_flags_argc(&fp), pvv,
      Ftcl.cxCmd.n, Ftcl.cxCmd.a, 1,
      NULL, ftcl_rs
    );
  }else{
    rc = ftcl_rs_argc(tcl, argv, NULL);
  }
  end:
  ftcl__flags_cleanup(&fp);
  assert(!fp.nonFlag.list);
  assert(!fp.nonFlag.count);
  assert(!fp.nonFlag.capacity);
  return rc;
}

/**
   Tcl init routine for this extension.

   https://wiki.tcl-lang.org/page/Hello+World+as+a+C+extension
*/
#ifdef _WIN32
__declspec(dllexport)
#endif
int DLLEXPORT Libfossil_Init(Tcl_Interp *tcl){
  if( NULL == Tcl_InitStubs(tcl, TCL_VERSION, 0) ) {
    return TCL_ERROR;
  }

  /* Set up fcli so that it installs its fail-fast allocator. */
  const fcli_cliflag FCliFlags[] = {
    fcli_cliflag_empty_m // list MUST end with this (or equivalent)
  };
  const fcli_help_info FCliHelp = {
    "A libfossil-extended tclsh shell.",
    NULL, // very brief usage text, e.g. "file1 [...fileN]"
    NULL // optional callback which outputs app-specific help
  };
  fcli.config.checkoutDir = NULL;
  {
    char const * argv[] = {
      TEAISH_PKGNAME,
      "--no-checkout",
      NULL
    };
    /* TODO: set fcli.config.output to... something useful from
       script code. */
    if( fcli_setup_v2(1, argv, FCliFlags, &FCliHelp) ){
      return TCL_ERROR;
    }
  }
  (void)ftcl__mutex();
  (void)ftcl__cx_create_cmd(
    tcl, fcli_cx(), Tclfossil_Cmd, "::fossil", NULL
  );
  atexit( ftcl__atexit );
  return Tcl_PkgProvide(tcl, TEAISH_PKGNAME, TEAISH_VERSION);
}

#undef MARKER
#undef ftcl__mutex_alloc
#undef ftcl__mutex_enter
#undef ftcl__mutex_t
#undef ftcl__mutex_enter
#undef ftcl__mutex_leave
