/*
** 2025 May 20
**
** The author disclaims copyright to this source code.  In place of a
** legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
*/

#include "th.h"
#include <string.h> /* memset() */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#if 1
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)
#else
#define MARKER(pfexp) (void)0
#endif

const th_list th_list_empty = th_list_empty_m;
const th_cmdflag th_cmdflag_empty = th_cmdflag_empty_m;
const th_fp th_fp_empty =
  th_fp_empty_m;

#if (defined(TH_F_FREE) && !defined(TH_F_REALLOC))  \
  || (defined(TH_F_REALLOC) && !defined(TH_F_FREE))
#  error "If TH_F_FREE is defined then TH_F_REALLOC must also be"
#endif

#ifndef TH_F_FREE
#  define TH_F_FREE free
#endif

#ifndef TH_F_REALLOC
#  define TH_F_REALLOC realloc
#endif

#if 0
#ifndef TH_F_MALLOC
#  define TH_F_MALLOC alloc
#else
  extern void  TH_F_MALLOC(void*);
#endif
#endif

extern void* TH_F_REALLOC(void*,size_t);
extern void  TH_F_FREE(void*);

#ifndef TH_OOM_ABORT
#  define TH_OOM_ABORT 0
#endif
void * th_realloc(void *p, size_t n){
#if (TH_OOM_ABORT+0)
  void * const x = n
    ? TH_F_REALLOC(p, n)
    : (TH_F_FREE(p), NULL);
  if( n && !x ){
    fprintf(stderr,"Allocation of %llu byte(s) failed",
            (unsigned long long)n);
    abort();
  }
  return x;
#else
  return n
    ? TH_F_REALLOC(p, n)
    : (TH_F_FREE(p), NULL);
#endif
}

void th_free(void * p){
  th_realloc(p, 0);
}

#if 0
extern void* TH_F_MALLOC(size_t);
void * th_malloc(size_t n){
  return TH_F_MALLOC(n);
}
#undef TH_F_MALLOC
#endif
#undef TH_F_FREE
#undef TH_F_REALLOC
#undef TH_OOM_ABORT

int th_rs_c(Tcl_Interp *tcl, int rc, char const * z){
  if(z && *z){
    Tcl_SetResult(tcl, (char *)z, TCL_VOLATILE);
  }else{
    Tcl_ResetResult(tcl);
  }
  return rc;
}

int th_rs_v(Tcl_Interp *tcl, int rc, char const * zFmt, va_list args){
  enum { BufSize = 2048 };
  char buf[BufSize] = {0};
  vsnprintf(buf, BufSize, zFmt, args);
  buf[BufSize-1] = 0;
  Tcl_SetResult(tcl, buf, TCL_VOLATILE);
  return rc;
}

int th_rs(Tcl_Interp *tcl, int rc, char const * zFmt, ...){
  if( zFmt && *zFmt ){
    va_list args;
    va_start(args,zFmt);
    rc = th_rs_v(tcl, rc, zFmt, args);
    va_end(args);
  }else{
    Tcl_ResetResult(tcl);
  }
  return rc;
}

int th_err_v(Tcl_Interp *tcl, char const * zFmt, va_list vargs){
  int rc;
  if( zFmt && *zFmt ){
    rc = th_rs_v(tcl, TCL_ERROR, zFmt, vargs);
  }else{
    Tcl_ResetResult(tcl);
    rc = TCL_ERROR;
  }
  return rc;
}

int th_err(Tcl_Interp *tcl, char const * zFmt, ...){
  int rc;
  va_list args;
  va_start(args,zFmt);
  rc = th_err_v(tcl, zFmt, args);
  va_end(args);
  return rc;
}

int th_rs_oom_at(Tcl_Interp *tcl, char const * zFile, int line ){
  return th_err(tcl, "Allocation error @%s:%d", zFile, line);
}

Tcl_Obj * th_obj_for_int64(Tcl_WideInt v){
  if( v>=-2147483647 && v<=2147483647 ){
    return Tcl_NewIntObj((int)v);
  }else{
    return Tcl_NewWideIntObj(v);
  }
}

int th_list_reserve( th_list * const self, th_size_t n ){
  assert(self);
  if(0 == n){
    if(self->list){
      //MARKER(("Freeing th_list memory\n"));
      th_free(self->list);
      *self = th_list_empty;
    }
    return 0;
  }else if( self->capacity >= n ){
    return 0;
  }else{
    size_t const sz = sizeof(void*) * n;
    void* * m = th_realloc( self->list, sz );
    if( !m ) return -1 /* OOM */;
    memset( m + self->capacity, 0, (sizeof(void*)*(n-self->capacity)));
    self->capacity = n;
    self->list = m;
    return 0;
  }
}

int th_list_append( th_list * const self, void* cp ){
  assert(self && self->count <= self->capacity);
  if(self->count == self->capacity){
    int rc;
    th_size_t const cap = self->capacity
      ? (self->capacity * 3 / 2)
      : 10;
    rc = th_list_reserve(self, cap);
    if(rc) return rc;
  }
  self->list[self->count++] = cp;
  if(self->count<self->capacity) self->list[self->count]=NULL;
  return 0;
}

void th_dump_cmdflag(
  char const * zPrefix,
  th_cmdflag const * const cf
){
  fprintf(stderr,
          "%s%sflag %-15s len=%-2u options=0x%02x "
          "consumes=%u seen=%u\n",
          zPrefix ? zPrefix : "",
          zPrefix ? " " : "",
          cf->zFlag, cf->nFlag, cf->options,
          (unsigned)cf->consumes, cf->nSeen);
}

void th_dump_cmdflags(
  char const * zPrefix,
  unsigned n,
  th_cmdflag * const * const aCF
){
  unsigned int i;
  for(i = 0; i < n; ++i) {
    th_dump_cmdflag(zPrefix, aCF[i]);
  }
}

void th_dump_argv(
  char const * zPrefix,
  Tcl_Interp *tcl,
  int argc,
  Tcl_Obj * const * argv
){
  int i;
  for(i = 0; i < argc; ++i) {
    Tcl_Obj * const po = argv[i];
    char const * const zArg = Tcl_GetStringFromObj(po, 0);
    fprintf(stderr, "%s arg #%d = %s\n", zPrefix, i, zArg);
  }
}

#if 0
static void th_dump_largv(
  char const * zPrefix,
  Tcl_Interp *tcl,
  th_list const * li
){
  th_dump_argv(zPrefix, tcl, (int)li->count, th_list_argv(li) );
}
#endif

void th_dump_flags(
  char const * zPrefix,
  Tcl_Interp *tcl,
  th_fp const * const fp
){
  th_dump_cmdflags(zPrefix, fp->nFlags, fp->pFlags);
  if( fp->nonFlag.count ){
    th_dump_argv("  non-flag", tcl, th_flags_argcv(fp));
  }
}

void th_cmdflag_set(th_cmdflag * const p, Tcl_Obj *v){
  th_ref(v);
  th_unref(p->pVal);
  p->pVal = v;
}

Tcl_Obj * th_cmdflag_get( th_cmdflag const * f, int at ){
  if( at < 0 || !f->pVal) return f->pVal;
  Tcl_Obj * e = 0;
  Tcl_Size n = 0;
  int rc = Tcl_ListObjLength(NULL, f->pVal, &n);
  if(!rc && at<n){
    Tcl_ListObjIndex(NULL, f->pVal, at, &e);
  }
  return e;
}

Tcl_Obj * th_flags_arg( th_fp const * fp, unsigned at ){
  return at < fp->nonFlag.count ? fp->nonFlag.list[at] : NULL;
}

char const * th_flags_arg_str( th_fp const * fp, unsigned at,
                                Tcl_Size *nOut ) {
  Tcl_Obj * const t = th_flags_arg(fp, at);
  return t ? th_gs(t, nOut) : 0;
}


char const * th_flags_arg_cstr( th_fp const * fp, unsigned at,
                                 Tcl_Size *nOut ){
  Tcl_Obj * const o = th_flags_arg(fp, at);
  return o ? th_gs(o,nOut) : NULL;
}

void th_flags_parse_cleanup2(th_fp * const p, int freeListMem){
  th_unref(p->arg0);
  p->arg0 = 0;
  for(th_size_t i = 0; i < p->nonFlag.count; ++i){
    Tcl_Obj * const o = p->nonFlag.list[i];
    th_unref_c(o);
  }
  if( freeListMem ){
    th_list_reserve(&p->nonFlag, 0);
    assert(!p->nonFlag.list);
    assert(!p->nonFlag.count);
    assert(!p->nonFlag.capacity);
  }else{
    p->nonFlag.count = 0;
  }
  for( unsigned i = 0; i < p->nFlags; ++i ){
    th_cmdflag_set(p->pFlags[i], NULL);
  }
  p->ddIndex = -1;
  p->nSeen = 0;
  /* Keep p->pFlags intact. The way this type is used, those are all
     gauranteed to outlive this object. */
}

void th_flags_parse_cleanup(th_fp * const p){
  th_flags_parse_cleanup2(p, 1);
}

int th_flags_parse( Tcl_Interp *tcl,
                     int argc, Tcl_Obj * const * argv,
                     th_fp * const fp ){
  th_cmdflag * f;
  int rc = 0;
  int i = 0;
  char const * zArg = 0;
  Tcl_Obj * pArg = 0;
  int stillLooking = 1;

  assert( fp->nFlags > 0 );
  th_flags_parse_cleanup2(fp, 0);
  fp->arg0 = argv[0];
  th_ref(fp->arg0);
  for(int j=0; j<fp->nFlags; ++j ){
    /* Ensure no stale state when these objects are reused. */
    f = fp->pFlags[j];
    f->nSeen = 0;
  }

  /* Add pArg to the fp->nonFlag list if configured to do so, else do
     nothing. */
#define CollectArg \
  assert(pArg);                                     \
  if(0==(th_F_PARSE_NO_COLLECT & fp->options)) {    \
    if( th_list_append(&fp->nonFlag, pArg) ) {      \
      rc = TCL_ERROR; goto end;                     \
    } else {                                        \
      th_ref(pArg);                                 \
    }                                               \
  } (void)0

  /* Set/append (Tcl_Obj*) V as/to f->pVal. */
#define SetFlagVal(V)                                   \
  if(f->consumes>1 || th_F_CMDFLAG_LIST & f->options){  \
    if(!f->pVal){                                       \
      th_cmdflag_set(f, Tcl_NewListObj(0,0));           \
      if(!f->pVal){ rc = TCL_ERROR; goto end; }         \
    }                                                   \
    Tcl_ListObjAppendElement(tcl, f->pVal, V);          \
    /* not a list: Tcl_AppendObjToObj */                \
  }else{                                                \
    th_cmdflag_set(f, V);                               \
  } (void)0

  for( i = 1; i < argc; ++i ){
    Tcl_Size nArg = 0;
    pArg = argv[i];
    if( !stillLooking ){
      CollectArg;
      continue;
    }
    zArg = th_gs(pArg, &nArg);
    if( zArg && zArg[0]=='-' ) {
      int j = 0;
      if( '-'==zArg[1] && 0==zArg[2] ){
        /* "--" flag */
        stillLooking = (th_F_PARSE_CHECK_ALL & fp->options);
        if( fp->ddIndex<0 ){
          fp->ddIndex = i;
        }
        continue;
      }
      f = fp->pFlags[0];
      /* Look for a matching -flag... */
      for( j=0; f; f = j+1<fp->nFlags ? fp->pFlags[++j] : NULL ){
        if( f->nFlag!=(unsigned)nArg || strncmp(zArg, f->zFlag, nArg) ) continue;
        //th_dump_cmdflag("...checking flag", f);
        if( 1==++f->nSeen ){
          ++fp->nSeen;
        }
        //th_dump_cmdflag("...seen flag", f); printf("i=%d zArg=%s\n", i, zArg);
        if( f->nSeen>1 &&
            !(f->options & (th_F_CMDFLAG_LIST|th_F_CMDFLAG_LAST_WINS)) ){
          rc = TCL_ERROR;
          if( fp->xResult ){
            rc = fp->xResult(tcl, rc,
                             "%s flag %s was used more than once",
                             Tcl_GetStringFromObj(argv[0], NULL),
                             zArg);
          }
          goto end;
        }else if( f->consumes ){
          for( unsigned short k = 0; k<f->consumes; ++k ){
            if( ++i >= argc ){
              goto missing_arg;
            }
            SetFlagVal(argv[i]);
          }
        }else{
          SetFlagVal(pArg);
        }
        break;
      }
      if( !f/* no flag found */ || !f->nSeen ){
        stillLooking = (th_F_PARSE_CHECK_ALL & fp->options);
        CollectArg;
      }
    } else {
      stillLooking = (th_F_PARSE_CHECK_ALL & fp->options);
      CollectArg;
    }
  }
  for( ; i<argc && 0==(th_F_PARSE_NO_COLLECT & fp->options); ++i ){
    pArg = argv[i];
    CollectArg;
#undef CollectArg
#undef SetFlagVal
  }
end:
  return rc;
missing_arg:
  rc = TCL_ERROR;
  if( fp->xResult ){
    rc = fp->xResult(tcl, rc, "Missing argument for flag %s", zArg);
  }
  return TCL_ERROR;
}


static int th_subcmd_cmp(const void *lhs, const void *rhs){
  return strcmp(((th_subcmd const *)lhs)->zName,
                ((th_subcmd const *)rhs)->zName);
}

void th_subcmd_sort( unsigned nCmd, th_subcmd * aCmd ){
  qsort( aCmd, nCmd, sizeof(th_subcmd), th_subcmd_cmp);
}

th_subcmd const * th_subcmd_search( Tcl_Obj * const pArg,
                                      unsigned nCmd,
                                      th_subcmd const * aCmd,
                                      int isSorted ){
  unsigned i = 0;
  Tcl_Size nArg = 0;
  char const * const zArg = th_gs(pArg, &nArg);
  assert( zArg );
  if( !isSorted || nCmd<4 ){
    for( ; zArg && i < nCmd; ++i ){
      th_subcmd const * c = &aCmd[i];
      if( c->zName && 0==strcmp(zArg, c->zName) ){
        return c;
      }
    }
    return NULL;
  }else{
    th_subcmd const key = {zArg, 0, 0};
    return bsearch( &key, aCmd, nCmd, sizeof(th_subcmd), th_subcmd_cmp);
  }
}

int th_subcmd_dispatch( void *cx,
                         Tcl_Interp *tcl,
                         int argc, Tcl_Obj * const * argv,
                         unsigned nCmd,
                         th_subcmd const * const aCmd,
                         int isSorted,
                         char const *zUsage,
                         th_result_set_f xResult ){
  assert(argc>0);
  int rc;
  th_subcmd const * const ps = argc
    ? th_subcmd_search(argv[0], nCmd, aCmd, isSorted)
    : 0;
  //MARKER(("subcommand %s search=%s\n", th_gs1(argv[0]), ps ? ps->zName : "<null>"));
  if( ps ){
    rc = ps->xFunc(cx, tcl, argc, argv);
  } else {
    char buf[1024];
    if( !zUsage && 0==th_subcmd_generate_list(nCmd, aCmd, buf, sizeof(buf)) ){
      zUsage = &buf[0];
    }
    rc = TCL_ERROR;
    if( xResult ) {
      rc = xResult(tcl, rc,
                   "Unknown subcommand \"%s\".%s%s",
                   argc ? th_gs1(argv[0]) : "<null>",
                   zUsage ? " Try: " : "",
                   zUsage ? zUsage : "");
    }
  }
  return rc;
}

int th_subcmd_dispatch_flags( void *cx, Tcl_Interp *tcl,
                               th_fp const * const args,
                               unsigned nCmd, th_subcmd const * const aCmd,
                               int isSorted,
                               char const *zUsage ) {
  int const argc = (int)args->nonFlag.count;
  assert( args->arg0 );
  if( !argc ){
    Tcl_WrongNumArgs(tcl, 1, &args->arg0, zUsage);
    return TCL_ERROR;
  }
  return th_subcmd_dispatch(cx, tcl, argc, th_flags_argv(args),
                             nCmd, aCmd, isSorted, zUsage,
                             args->xResult);
}

th_enum_entry const * th_enum_search(th_enum const * const E,
                                       char const * z, int nZ){
  th_enum_entry const * ee = 0;
  if( nZ<0 ) nZ = (int)strlen(z);
  for( int i = 0; i<E->n && (ee = &E->e[i]); ++i ){
    if( nZ==(int)ee->nName && 0==strcmp(z, ee->zName) ){
      return ee;
    }
  }
  return NULL;
}

int th_flags_generate_help(th_fp const * fp,
                            char * z, size_t nDest){

  for(unsigned i = 0; i < fp->nFlags; ++i){
    th_cmdflag const * const f = fp->pFlags[i];
    char const * const zU = (f->zUsage && *f->zUsage) ? f->zUsage : 0;
    char const * const zMulti = (f->options & th_F_CMDFLAG_LIST)
      ? "*" : "";
    int const n = snprintf(z, nDest, "%s%s%s%s%s",
                           i==0 ? "" : " ",
                           f->zFlag, zMulti,
                           zU ? " " : "",
                           zU ? zU : "");
    if(n<0 || n>=(int)nDest){
      *z = 0;
      return TCL_ERROR;
    }
    nDest -= n;
    z += n;
    *z = 0;
  }
  return 0;
}

int th_check_help(Tcl_Interp * const tcl, Tcl_Obj * const cmd,
                   th_cmdflag const * const fHelp,
                   th_fp const * const fp,
                   char const *zNonFlags){
  if( fHelp->nSeen ){
    char buf[1024];
    int rc = th_flags_generate_help(fp, buf, sizeof(buf));
    return rc
      ? rc
      : th_rs(tcl, TCL_BREAK, "%s %s%s%s",
               th_gs1(cmd), buf,
               zNonFlags ? " " : "",
               zNonFlags ? zNonFlags : "");
  }
  return 0;
}

int th_subcmd_generate_list(unsigned nSubs,
                             th_subcmd const * const aSubs,
                             char * z, size_t nDest){
  for(unsigned i = 0; i < nSubs; ++i){
    th_subcmd const * sc = &aSubs[i];
    int const n = snprintf(z, nDest, "%s%s", i==0 ? "" : " ", sc->zName);
    if(n<0 || n>=(int)nDest){
      *z = 0;
      return TCL_ERROR;
    }
    nDest -= n;
    z += n;
    *z = 0;
  }
  return 0;
}

int th_enum_generate_list(th_enum const * const E,
                           int which,
                           char * z, unsigned nOut,
                           char const * zSep){
  th_enum_entry const * ee = 0;
  int const mode = (which<0 ? -1 : (which ? 1 : 0));
  if( !zSep ) zSep = " ";
  for( int i = 0; i<E->n && (ee = &E->e[i]); ++i ){
    int n = 0;
#define range_check \
    if(n<0 || n>=(int)nOut){ *z = 0; return TCL_ERROR; } \
    nOut -=n; z += n; *z = 0
    if( zSep && *zSep && i>0 ){
      n = snprintf(z, nOut, "%s", zSep);
      range_check;
    }
    switch(mode){
      case -1:
        n = snprintf(z, nOut, "%.*s", (int)ee->nName, ee->zName);
        break;
      case 0:
        n = snprintf(z, nOut, "%.*s %d", (int)ee->nName,
                     ee->zName, ee->value);
        break;
      case 1:
        n = snprintf(z, nOut, "%d", ee->value);
        break;
    }
    range_check;
#undef range_check
  }
  return 0;
}

static const int thAllocStamp = 0;
th_cmdo * th_cmdo_alloc(Tcl_Interp *tcl, void const * type, void * p,
                          th_finalizer_f dtor){
  th_cmdo * const cm = th_realloc(NULL,sizeof(th_cmdo));
  if(!cm) return NULL;
  memset(cm, 0, sizeof(th_cmdo));
  cm->type = type;
  cm->p = p;
  cm->dtor = dtor;
  cm->tcl = tcl;
  cm->allocStamp = &thAllocStamp;
  return cm;
}

th_cmdo * th_cmdo_ref(th_cmdo * cm){
  ++cm->refCount;
  return cm;
}

#if 0
static int th_cmdo_subcmd_dispatch_ObjCmd(
  ClientData p,
  Tcl_Interp *tcl,
  int argc,
  Tcl_Obj * const * argv
){
  //th_dump_argv("ftcl__cx_ObjCmd", tcl, argc, argv);
  th_cmdo * const cm = p;
  assert( th_cmdo_fsl(cm) );
  fsl_cx_err_reset(th_cmdo_fsl(cm));
  return ftcl__cx_dispatch(cm, 1, argc, argv, Ftcl.cxCmd.n, Ftcl.cxCmd.a, 1);
}
#endif


void th_cmdo_unset(th_cmdo * const cm){
  //MARKER(("Unsetting varname@%s\n", cm->tVarName ? th_gs1(cm->tVarName) : "<null>"));
  if( cm->tVarName ){
    assert( cm->tcl );
    //MARKER(("Unsetting varname=%s\n", th_gs1(cm->tVarName)));
    Tcl_UnsetVar(cm->tcl, th_gs1(cm->tVarName), 0);
    th_unref(cm->tVarName);
    assert( !cm->tVarName );
  }
}

static const unsigned short nonRefCount = (unsigned short)-1;
void th_cmdo_unref(th_cmdo * cm){
  /*MARKER(("unreffing (refc=%d) tCmdName=%s\n", (int)cm->refCount,
    Tcl_GetCommandName(cm->tcl, cm->pCmd)));*/
  if( cm->refCount>1 ){
    if( nonRefCount == cm->refCount ){
      /*MARKER(("Avoiding recursive free()\n"));*/
    }else{
      --cm->refCount;
    }
    return;
  }
  assert( cm->tcl );
  assert( nonRefCount != cm->refCount );
  /*MARKER(("Freeing (refc=%d) tCmdName=%s\n", (int)cm->refCount,
    Tcl_GetCommandName(cm->tcl, cm->pCmd)));*/
  int const doFree = &thAllocStamp == cm->allocStamp;
  th_cmdo_unset(cm);
  cm->refCount = nonRefCount
    /* Reminder to self: this finalizer can be triggered via an
       explicit Tcl_DeleteCommandFromToken() (see below) or from
       renaming of the proc script-side. In the latter case, or when
       th_cmdo_unref() is explicitly called by a client, we have to
       guard against recursively invoking this function. We do that by
       setting the refcount to a sentinel value. */;
  if( cm->tcl ){
    if( cm->tCmdName ){
      Tcl_Command c1 = Tcl_FindCommand(cm->tcl, th_gs1(cm->tCmdName),
                                       0, 0)
        /* we don't(?) know that pCmd is still valid. */;
      if( c1 ){
        /*MARKER(("Deleting cmd via tCmdName=%s\n",
          Tcl_GetCommandName(cm->tcl, c1)));*/
        Tcl_DeleteCommandFromToken(cm->tcl, c1);
      }else if( cm->pCmd ){
        /*MARKER(("Deleting cmd via pCmd=%s\n",
          Tcl_GetCommandName(cm->tcl, cm->pCmd)));*/
        Tcl_DeleteCommandFromToken(cm->tcl, cm->pCmd);
      }
      th_unref(cm->tCmdName);
    }else if(cm->pCmd){
      /*MARKER(("Deleting cmd via pCmd=%s\n",
        Tcl_GetCommandName(cm->tcl, cm->pCmd)));*/
      Tcl_DeleteCommandFromToken(cm->tcl, cm->pCmd);
    }
    cm->pCmd = 0;
    cm->tcl = 0;
    cm->refCount = 0;
  }
  if( cm->dtor ){
    void * const p = cm->p;
    th_finalizer_f const dtor = cm->dtor;
    cm->dtor = 0;
    cm->p = 0;
    dtor(p) /* If cm lives in p, cm may have just been destroyed. In
               that case, cm->allocStamp must have been set to 0 */;
  }
  if( doFree ){
    //assert(!"Should not be hit in these test");
    /*MARKER(("Freeing th_cmdo@%p\n", cm));*/
    memset(cm, 0, sizeof(th_cmdo));
    th_free(cm);
  }
}

Tcl_Obj * th_cmdo_name(th_cmdo * const cm, bool recheck){
  if( (recheck || !cm->tCmdName) && cm->pCmd ){
    th_unref(cm->tCmdName);
    cm->tCmdName = th_nso(Tcl_GetCommandName(cm->tcl, cm->pCmd), -1);
    th_ref(cm->tCmdName);
  }
  return cm->tCmdName;
}

/** th_cmdo finalizer for use with Tcl_CreateObjCommand(). */
static void th_cmdo_finalizer(void *p){
  assert(p && "Tcl should never be passing us a NULL here");
  if(p && nonRefCount!=((th_cmdo*)p)->refCount) th_cmdo_unref(p);
}

int th_cmdo_plugin(th_cmdo * const cm, Tcl_ObjCmdProc nrAdapter,
                   Tcl_ObjCmdProc cmd, char const *zCmdName,
                   Tcl_Obj * const tVarName){
  char buf[64] = {0};
  char const * zCN = 0;
  int nz = -1;
  if( zCmdName ) zCN = zCmdName;
  else{
    static unsigned counter = 0;
    nz = snprintf(buf, sizeof(buf), "th_cmdo#%u@%llu",
                  counter, (unsigned long long)time(0));
    if( nz<0 || nz>=(int)sizeof(buf) ){
      assert(!"cannot happen");
      return TCL_ERROR;
    }
    zCN = buf;
  }
  if( nrAdapter ){
    cm->pCmd = Tcl_NRCreateCommand(cm->tcl, zCN, nrAdapter, cmd,
                                   cm, th_cmdo_finalizer);
  }else{
    cm->pCmd = Tcl_CreateObjCommand(cm->tcl, zCN, cmd, cm,
                                    th_cmdo_finalizer);
  }
  if( !cm->pCmd ) return TCL_ERROR;
  cm->tCmdName = th_nso(Tcl_GetCommandName(cm->tcl, cm->pCmd), -1);
  th_ref(cm->tCmdName);
  if( tVarName ){
    Tcl_SetVar(cm->tcl, th_gs1(tVarName),
               1 ? zCN : Tcl_GetCommandName(cm->tcl, cm->pCmd),
               0);
    //MARKER(("setting varname=%s to %s\n", th_gs1(tVarName), zCN));
    cm->tVarName = th_ref(tVarName);
  }
  th_cmdo_ref(cm);
  return 0;
}

#undef MARKER
