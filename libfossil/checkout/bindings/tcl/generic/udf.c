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


ftcl__udf * ftcl__udf_alloc(ftcl_cx * fx){
  ftcl__udf * rv = fsl_malloc(sizeof(ftcl__udf));
  memset(rv, 0, sizeof(ftcl__udf));
  rv->fx = fx;
#define M1(FF) rv->func.FF.arityMin = -1
  M1(xFunc); M1(xStep); M1(xFinal);
  M1(xValue); M1(xInverse);
#undef M1
  return rv;
}

void ftcl__udf_free(ftcl__udf * const u){
  if(u){
    th_unref(u->oName);
    th_unref(u->tMetaName);
#define unref(F) th_unref(u->func.F.o)
    unref(xFunc);
    unref(xStep);
    unref(xFinal);
    unref(xValue);
    unref(xInverse);
#undef unref
    fsl_free(u);
  }
}

// ftcl__udf finalizer for sqlite3_create_function()
static void ftcl__udf_free_v(void * const u){
  //MARKER(("ftcl__udf freeing via sqlite cleanup\n"));
  ftcl__udf_free(u);
}

/**
https://www.sqlite.org/c3ref/create_function.html

int sqlite3_create_function_v2(
  sqlite3 *db,
  const char *zFunctionName,
  int nArg,
  int eTextRep,
  void *pApp,
  void (*xFunc)(sqlite3_context*,int,sqlite3_value**),
  void (*xStep)(sqlite3_context*,int,sqlite3_value**),
  void (*xFinal)(sqlite3_context*),
  void(*xDestroy)(void*)
);

int sqlite3_create_window_function(
  sqlite3 *db,
  const char *zFunctionName,
  int nArg,
  int eTextRep,
  void *pApp,
  void (*xStep)(sqlite3_context*,int,sqlite3_value**),
  void (*xFinal)(sqlite3_context*),
  void (*xValue)(sqlite3_context*),
  void (*xInverse)(sqlite3_context*,int,sqlite3_value**),
  void(*xDestroy)(void*)
);
*/

static Tcl_Obj * ftcl__sq3val_to_tcl(
  ftcl_cx * const fx, sqlite3_value * sv
){
  Tcl_Obj * rv = 0;
  switch( sqlite3_value_type(sv) ){
    case SQLITE_BLOB: {
      int const bytes = sqlite3_value_bytes(sv);
      rv = Tcl_NewByteArrayObj(sqlite3_value_blob(sv), bytes);
      break;
    }
    case SQLITE_INTEGER: {
      sqlite_int64 const v = sqlite3_value_int64(sv);
      rv = th_obj_for_int64(v);
      break;
    }
    case SQLITE_FLOAT: {
      double const r = sqlite3_value_double(sv);
      rv = Tcl_NewDoubleObj(r);
      break;
    }
    case SQLITE_NULL: {
      extern Tcl_Obj * ftcl_cx__get_null(ftcl_cx * const fx)/*in db.c*/;
      rv = ftcl_cx__get_null(fx);
      break;
    }
    default: {
      int const bytes = sqlite3_value_bytes(sv);
      rv = Tcl_NewStringObj((char const *)sqlite3_value_text(sv), bytes);
      break;
    }
  }
  return rv;
}

static void ftcl__ptr_base64(void const * p, char *zOut, size_t nOut){
  /** Adapted from code in the Fossil SCM */
  static const char zDigits[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz~";
  /*  123456789 123456789 123456789 123456789 123456789 123456789 123 */
  unsigned long long v = (unsigned long long)p;
  char zBuf[20];
  int i;
  assert( nOut >= sizeof(void*)+1 );
  if( v==0 ){
    *zOut++ = '0';
    *zOut = 0;
    return;
  }
  for(i=0; v>0; i++, v>>=6){
    zBuf[i] = zDigits[v&0x3f];
  }
  for(int j=i-1; j>=0 && nOut--!=0; j--){
    *zOut++ = zBuf[j];
  }
  *zOut = 0;
}

/**
   Writes a unique key for cx3's current aggregate context to pOut,
   which must be at least (sizeof(void*)+1) bytes. isFinal must only
   be true for calls via xFinal().
*/
static int ftcl__udf_aggc(ftcl__udf const * const u,
                          sqlite3_context *cx3,
                          int isFinal,
                          char * pOut, size_t nOut ){
  void const * p = sqlite3_aggregate_context(cx3, isFinal ? 0 : 1);
  if( !p ){
    if( isFinal ){
      /* This means that xFinal was called without a preceding
         xStep. */
      //MARKER(("isFinal\n"));
      *pOut = 0;
      return 0;
    }
    return th_rs_oom_here(u->fx->cmdo.tcl);
  }
  /*p is owned by sqlite*/
#if 0
  assert( nOut >= sizeof(void*)*2+3/* 0x..\0 */ );
  int const rc = snprintf(pOut, nOut, "%p", p);
  assert( rc>= && rc<=(int)nOut ); (void)rc;
  pOut[sizeof(void*)*2] = 0;
#else
  assert( nOut >= sizeof(void*)+1 );
  ftcl__ptr_base64(p, pOut, nOut);
#endif
  //MARKER(("agg cx=%s\n", pOut));
  return 0;
}

static const char * ftcl__udf_xName(ftcl__udf const * const u,
                                    ftcl__udf_lambda const * const f){
  switch( u->eType ){
    case ftcl__udf_TYPE_WINDOW:
      if( f==&u->func.xValue ) return "xValue";
      else if( f==&u->func.xInverse ) return "xInverse";
      FSL_SWITCH_FALL_THROUGH;
    case ftcl__udf_TYPE_AGGREGATE:
      if( f==&u->func.xStep ) return "xStep";
      else if( f==&u->func.xFinal ) return "xFinal";
      FSL_SWITCH_FALL_THROUGH;
    case ftcl__udf_TYPE_SCALAR:
      return "xFunc";
    case ftcl__udf_TYPE_INVALID:
      break;
  }
  assert(!"<internal error - mismatch UDF type/func>");
  return "<internal error - mismatch UDF type/func>";
}

/**
   Appends the items "apply" and fn->o to tgt, then appends a
   translation of each entry in (argc,argv) to tgt. On success returns
   0 and writes the eval-able object to call the UDF in *ppTgt (it
   will have a refcount of 0).  If argc is out of range for the lambda,
   the tcl result is set and TCL_ERROR is returned. On error it calls
   sqlite3_result_error() to report it.
*/
static int ftcl__udf_xlate_args(
  ftcl__udf * const u, ftcl__udf_lambda const * const fn,
  Tcl_Obj ** ppTgt, sqlite3_context* cx3, int argc, sqlite3_value **argv
){
  int rc = 0;
  ftcl_cx * const fx = u->fx;
  Tcl_Obj * tgt = 0;
  short const isFinal = (fn==&u->func.xFinal);
  short const nFudge = /* Number of args we will prefix to the call. */
    u->tMetaName
    ? 0
    : ((fn==&u->func.xFunc) ? 1 /* db */ : 2 /* db, aggregate context */);
  assert( ppTgt );
  if( ((short)argc + nFudge < fn->arityMin)
      || (fn->arityMax>=0 && ((short)argc + nFudge > fn->arityMax)) ){
    char buf[256] = {0};
    rc = snprintf(buf, sizeof(buf),
                  "%s.%s() argument count (%d) is out of range (%d..%d)",
                  th_gs1(u->oName), ftcl__udf_xName(u, fn),
                  argc, fn->arityMin, fn->arityMax);
    /*MARKER(("argc check failed %d vs (%d..%d) snprintf=%d: %s\n", argc,
      fn->arityMin, fn->arityMax, rc, buf));*/
    if( rc>=0 && (unsigned)rc<sizeof(buf) ){
      sqlite3_result_error(cx3, buf, rc);
    }else{
      sqlite3_result_error(cx3, "UDF argument count is out of range and "
                           "its name is too long to output here", -1);
    }
    rc = TCL_ERROR;
  }else{
    Tcl_Interp * const tcl = fx->cmdo.tcl;
    tgt = Tcl_NewObj() /* fixme: we need a way to cache this higher
                          up.  The problem is that i don't see any Tcl
                          APIs with which we can truncate a list, so
                          we can't just re-use the first 2 elements
                          over and over. */;
    th_loae(tcl, tgt, ftcl_cx_apply(u->fx));
    th_loae(tcl, tgt, fn->o);
    if( nFudge ){
      assert( 1==nFudge || 2==nFudge );
      /* Add db command name arg... */
      th_loae(tcl, tgt, th_cmdo_name(&u->fx->cmdo, false));
      if( nFudge>1 ){
        /* Add aggregate context arg */
        char buf[sizeof(void*)+1]; //*2+3/* 0x..\0 */];
        if( 0==(rc = ftcl__udf_aggc(u, cx3, isFinal, buf, sizeof(buf))) ){
          th_loae(tcl, tgt, th_nso(buf,-1));
        }
      }
    }
    for( int i = 0; 0==rc && i < argc; ++i ){
      sqlite3_value * const sv = argv[i];
      Tcl_Obj * const tv = ftcl__sq3val_to_tcl(fx, sv);
      th_ref(tv);
      rc = th_loae(tcl, tgt, tv);
      th_unref_c(tv);
    }
    if(rc){
      th_bounce(tgt);
    }else{
      *ppTgt = tgt;
    }
  }
  return rc;
}

/**
   Sets the sqlite_result_...() for the just-completed eval of u's
   f method.
*/
static void ftcl__udf_rs(sqlite3_context *cx3, ftcl__udf * const u,
                         ftcl__udf_lambda const * const f, int rc){
  Tcl_Size nZ = 0;
  Tcl_Obj * const rv = Tcl_GetObjResult(u->fx->cmdo.tcl);
  char const * const z = th_gs(rv, &nZ);

  if( rc ){
    //MARKER(("udf result = %d = %u bytes: %.*s\n", rc, (unsigned)nZ, (int)nZ, z));
    char * zz = fsl_mprintf("UDF \"%s.%s()\": %.*s",
                            th_gs1(u->oName), ftcl__udf_xName(u, f),
                            (int)nZ, z);
    sqlite3_result_error(cx3, zz, -1);
    fsl_free(zz);
  }else if(f == &u->func.xStep
           || f == &u->func.xInverse ){
    /* These methods don't set an SQLite result. */
    return;
  }else{
    switch( (0==nZ && (ftcl__udf_F_EMPTY_AS_NULL & u->flags))
            ? SQLITE_NULL
            : u->sqlResultType ){
      case SQLITE_NULL:
        sqlite3_result_null(cx3);
        break;
      case SQLITE_TEXT:
        sqlite3_result_text(cx3, z, (int)nZ, SQLITE_TRANSIENT);
        break;
      case SQLITE_BLOB:
        sqlite3_result_blob(cx3, z, (int)nZ, SQLITE_TRANSIENT);
        break;
      case SQLITE_FLOAT:{
        double v = 0;
        if( 0==Tcl_GetDoubleFromObj(NULL, rv, &v)
            || (ftcl__udf_F_BADNUM_ZERO & u->flags) ){
          sqlite3_result_double(cx3, v);
        }else{
          sqlite3_result_null(cx3);
        }
        break;
      }
      case ftcl__SQL_TYPE_GUESS:
      case SQLITE_INTEGER:{
        Tcl_WideInt vw = 0;
        int vi = 0;
        double vd = 0;
        int rc;
        if( 0==Tcl_GetWideIntFromObj(NULL, rv, &vw) ){
          sqlite3_result_int64(cx3, (sqlite3_int64)vw);
        }else if( 0==Tcl_GetIntFromObj(NULL, rv, &vi) ){
          sqlite3_result_int(cx3, vi);
        }else if( 0==(rc=Tcl_GetDoubleFromObj(NULL, rv, &vd))
                  || (SQLITE_INTEGER == u->sqlResultType) ){
          if(SQLITE_INTEGER == u->sqlResultType){
            if( !rc || (ftcl__udf_F_BADNUM_ZERO & u->flags) ){
              sqlite3_result_int64(cx3, (sqlite3_int64)vd);
            }else{
              assert(rc);
              sqlite3_result_null(cx3);
            }
          }else{
            if( !rc || (ftcl__udf_F_BADNUM_ZERO & u->flags) ){
              sqlite3_result_double(cx3, vd);
            }else{
              sqlite3_result_null(cx3);
            }
          }
        }else{
          sqlite3_result_text(cx3, z, (int)nZ, SQLITE_TRANSIENT);
        }
        break;
      }
    }
  }
} /* ftcl__udf_rs() */

/** Wrapper for UDF xFunc... tcl impls. */
static void ftcl__udf_xAdaptor(ftcl__udf * const u,
                               ftcl__udf_lambda * const f,
                               sqlite3_context* cx3,
                               int argc, sqlite3_value**argv){
  //MARKER(("xFunc argc=%d\n", argc));
  Tcl_Obj * tArgs = 0;
  Tcl_Interp * const tcl = u->fx->cmdo.tcl;
  if( ftcl__udf_xlate_args(u, f, &tArgs, cx3, argc, argv) ){
    /*the error state was already set*/
    return;
  }
  th_ref(tArgs);
  //ftcl__udf_lambda const * const oldL = u->sqlite.xRunning;
  //sqlite3_context * const oldCx3 = u->sqlite.cx3;
  //u->sqlite.xRunning = f;
  //u->sqlite.cx3 = oldCx3;
  int const rc = Tcl_EvalObjEx(tcl, tArgs, 0);
  //u->sqlite.xRunning = oldL;
  //u->sqlite.cx3 = oldCx3;
  th_unref(tArgs);
  ftcl__udf_rs(cx3, u, f, rc);
}

static void ftcl__udf_xFunc(sqlite3_context* cx3, int argc,
                            sqlite3_value**argv){
  ftcl__udf * const u = sqlite3_user_data(cx3);
  ftcl__udf_xAdaptor(u, &u->func.xFunc, cx3, argc, argv);
}

static void ftcl__udf_xStep(sqlite3_context* cx3, int argc,
                            sqlite3_value**argv){
  ftcl__udf * const u = sqlite3_user_data(cx3);
  ftcl__udf_xAdaptor(u, &u->func.xStep, cx3, argc, argv);
}

static void ftcl__udf_xFinal(sqlite3_context* cx3){
  ftcl__udf * const u = sqlite3_user_data(cx3);
  ftcl__udf_xAdaptor(u, &u->func.xFinal, cx3, 0, 0);
}

static void ftcl__udf_xValue(sqlite3_context* cx3){
  ftcl__udf * const u = sqlite3_user_data(cx3);
  ftcl__udf_xAdaptor(u, &u->func.xValue, cx3, 0, 0);
}

static void ftcl__udf_xInverse(sqlite3_context* cx3, int argc,
                               sqlite3_value**argv){
  ftcl__udf * const u = sqlite3_user_data(cx3);
  ftcl__udf_xAdaptor(u, &u->func.xInverse, cx3, argc, argv);
}

/**
   Ensure that fn->o is-a lambda expression ({{args} {body}}) and
   calculate a minimum/maximum arity based on the first
   element. Returns 0 on success, TCL_ERROR (or similar) on eror.
*/
static int ftcl__udf_lambda_init(ftcl__udf * const u,
                                 ftcl__udf_lambda * const fn,
                                 char const * zMethodName){
  Tcl_Obj ** argv = 0;
  Tcl_Size argc = 0;
  Tcl_Interp * const tcl = u->fx->cmdo.tcl;
  int rc = Tcl_ListObjGetElements(tcl, fn->o, &argc, &argv);

  if( rc ) return rc;
  if( 2!=argc ){
    return ftcl_err(tcl, "Expecting a lambda-style object (two-entry list)");
  }
  /* Separate out the parameters... */
  rc = Tcl_ListObjGetElements(tcl, argv[0], &argc, &argv);
  if( rc ) return rc;
  /*
    Now guestimate how many args the lambda can take. This counting
    does not take into account the automatically-passed $db and (for
    non-scalars) $aggregateContext.
   */
  short iMin = argc ? 0 : -1;
  short iMax = 0;
  int seenDflt = 0;
  for( int i = 0; i < argc; ++i ){
    Tcl_Size iLen = 0;
    Tcl_Obj * const arg = argv[i];
    if( !seenDflt && 0==Tcl_ListObjLength(tcl, arg, &iLen) ){
      if(iLen>1){
        /* parameter entry {name dflt} */
        seenDflt = 1;
      }
    }
    if( i==argc-1 ){
      /* If the final var is "args" then... */
      Tcl_Size n = 0;
      char const * z = Tcl_GetStringFromObj(argv[i], &n);
      if( 4==n && 0==fsl_strncmp(z, "args", (fsl_size_t)n) ){
        iMax = -1;
        break;
      }
    }
    if( !seenDflt ){
      ++iMin;
    }
    ++iMax;
  }
  if( 0==iMax ){
    /* Pedantic cosmetic tweak */
    assert( -1==iMin );
    iMin = 0;
  }else if(iMax>0 && iMax<iMin){
    iMax = iMin;
  }
  int const nRequiredMin = ( ftcl__udf_TYPE_SCALAR==u->eType )
    ? (u->tMetaName ? 0 : 1)
    : (u->tMetaName ? 0 : 2);
  if( nRequiredMin && (iMin>=0 && iMin<nRequiredMin) ){
    return ftcl_err(tcl, /*Non-aggregate/non-window functions*/
                    "The %s method requires at least %d argument%s",
                    zMethodName, nRequiredMin,
                    1==nRequiredMin
                    ? " (db)" : "s (db, aggregateContext)");
  }

  fn->arityMin = iMin;
  fn->arityMax = iMax;
  /*MARKER(("arity: %s argc=%d min-max=%d..%d\n%s\n", th_gs1(u->oName), argc,
    iMin, iMax, th_gs1(fn->o)));*/
  return rc;
}

/**
   Sets up one UDF binding from its corresponding (query
   --scalar/---aggragate/-----window) flag. TCL_ERROR on error and all
   that.

   Sets *pSqlOwnsUdf to 1 if, after this returns, sqlite3 owns the udf
   object.
*/
int ftcl__udf_init(ftcl_cx * const fx, enum ftcl__udf_type_e eType,
                   Tcl_Obj * const tName, th_cmdflag * const pFlag,
                   ftcl__udf * const udf, int funcFlags,
                   int * pSqlOwnsUdf){
  char const * z;
  int rc = 0;
  ftcl__udf_lambda * aUdf[4] = {0,0,0,0};
  char const * aMethodNames[sizeof(aUdf)/sizeof(aUdf[0])] = {0,0,0,0};
  unsigned short aUdfPos = 0;
  udf->oName = tName;
  th_ref(udf->oName);
  udf->eType = eType;
  switch(eType){

#define DO(FM,TObj)                  \
    udf->func.FM.o = TObj;           \
    assert( udf->func.FM.o );        \
    th_ref(udf->func.FM.o);          \
    aMethodNames[aUdfPos] = # FM;    \
    aUdf[aUdfPos++] = &udf->func.FM; \
    assert(aUdfPos <= 5)

    case ftcl__udf_TYPE_SCALAR:
      DO(xFunc, pFlag->pVal);
      break;
    case ftcl__udf_TYPE_AGGREGATE:
    case ftcl__udf_TYPE_WINDOW:{
      /** Extract 2 or 4 args from pFlag */
      Tcl_Obj ** argv = 0;
      Tcl_Size argc = 0;
      Tcl_Interp * const tcl = fx->cmdo.tcl;
      int rc = Tcl_ListObjGetElements(tcl, pFlag->pVal, &argc, &argv);
      if( rc ) goto end;
      else if( (ftcl__udf_TYPE_AGGREGATE==eType ? 2 : 4)!=argc ){
        rc = ftcl_err(tcl, "%s expects %d lambda expressions",
                      pFlag->zFlag,
                      (ftcl__udf_TYPE_AGGREGATE==eType ? 2 : 4));
        goto end;
      }
      if( ftcl__udf_TYPE_AGGREGATE==eType ){
        DO(xStep, argv[0]);
        DO(xFinal, argv[1]);
      }else{
        DO(xStep, argv[0]);
        DO(xFinal, argv[1]);
        DO(xValue, argv[2]);
        DO(xInverse, argv[3]);
      }
      break;
    }
#undef DO

    case ftcl__udf_TYPE_INVALID:
    default:
      assert(!"impossible!");
  }

  /** Validate and initialize u->func.* */
  for( unsigned short i = 0; i < aUdfPos;++i ){
    ftcl__udf_lambda * const ff = aUdf[i];
    assert(ff);
    if( (rc = ftcl__udf_lambda_init(udf, ff, aMethodNames[i])) ){
      goto end;
    }
  }
  z = th_gs1(tName);
  fsl_db * const fdb = ftcl_cx_db(fx);
  assert(fdb);
  switch(eType){
    case ftcl__udf_TYPE_SCALAR:
      rc = sqlite3_create_function_v2(fdb->dbh, z, -1, funcFlags, udf,
                                      ftcl__udf_xFunc, 0, 0,
                                      ftcl__udf_free_v);
      break;
    case ftcl__udf_TYPE_AGGREGATE:
      rc = sqlite3_create_function_v2(fdb->dbh, z, -1, funcFlags, udf,
                                      0, ftcl__udf_xStep, ftcl__udf_xFinal,
                                      ftcl__udf_free_v);
      break;
    case ftcl__udf_TYPE_WINDOW:
      rc = sqlite3_create_window_function(fdb->dbh, z, -1, funcFlags, udf,
                                          ftcl__udf_xStep,
                                          ftcl__udf_xFinal,
                                          ftcl__udf_xValue,
                                          ftcl__udf_xInverse,
                                          ftcl__udf_free_v);
      break;
    case ftcl__udf_TYPE_INVALID:
    default:
      assert(!"impossible!");
  }
  *pSqlOwnsUdf = 1
    /* sqlite3_create_function_v2() and friends call the finalizer on
       failure, so we have transferred ownership of u to sqlite. */;
  if( rc ){
    rc = ftcl_rs_sqlite3(ftcl_cx_db(fx)->dbh, fx->cmdo.tcl, rc);
  }
end:
  return rc;
}

void ftcl__dump_sizeofs_udf(void){
#define SO(T)                                   \
  printf("sizeof(%s)=%u\n", #T, (unsigned)sizeof(T))
  SO(ftcl__udf);
  SO(ftcl__udf_lambda);
#undef SO
}

#undef MARKER
