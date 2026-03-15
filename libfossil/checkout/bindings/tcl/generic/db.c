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

const struct {
  char const * zTypeQueryState;
} DbGlobal = {
  .zTypeQueryState = "QueryState"
};

int ftcl_db_foreach_f_stepall(void * const cx, Tcl_Interp * const tcl,
                              fsl_stmt * const q, unsigned id){
  int rc = 0;
  while( (rc = fsl_stmt_step(q)) == FSL_RC_STEP_ROW ) {}
  return FSL_RC_STEP_DONE==rc ? 0 : ftcl_rs_db(tcl, q->db, rc);
}

int ftcl_db_foreach(Tcl_Interp * const tcl, fsl_db * const db,
                    ftcl_db_foreach_f cb, void * const cx,
                    char const * const zSql, Tcl_Size nSql){
  if( nSql<0 ) nSql = zSql ? (int)fsl_strlen(zSql) : 0;
  if( nSql <=0 ) return 0;
  int rc = 0;
  unsigned id = 0;
  char const * zPos = zSql;
  char const * const zEnd = zPos ? (zPos + nSql) : NULL;
  fsl_stmt qf = fsl_stmt_empty;
  while( 0==rc && zPos<zEnd ){
    fsl_stmt_finalize(&qf);
    while( *zPos && fsl_isspace(*zPos) && zPos<zEnd ) {
      /*solely cosmetic: trim leading space*/
      ++zPos;
    }
    if( zPos>=zEnd ) break;
    rc = sqlite3_prepare_v2(db->dbh, zPos, (int)(zEnd - zPos), &qf.stmt, &zPos);
    if( 0!=rc ){
      rc = ftcl_rs_sqlite3(db->dbh, tcl, rc);
    }else if(!qf.stmt){
      /* Empty statement/whitespace */
      continue;
    }else if(cb) {
      qf.db = db;
      rc = cb(cx, tcl, &qf, id++);
      if( rc ){
        if( TCL_BREAK==rc ){
          //Tcl_ResetResult(tcl);
          rc = 0;
        }
      }
    }
  }
  fsl_stmt_finalize(&qf);
  assert(!qf.stmt);
  return rc;
}

/**
   Types of result value semantics for db eval, for use with
   eval --return ENUM_VALUE|#ColumnIndex|*ColumnIndex.
*/
enum ftcl__RESULT_e {
  /**
     No return value. Must have the value 0.
  */
  ftcl__RESULT_NONE = 0,

  /** Internal sentinel. */
  ftcl__RESULT_UNKNOWN,

  /**
    List of first result row's columns, with no names.
  */
  ftcl__RESULT_ROW1_VALS,

  /**
     A two-entry list containing 1) the column names and 2) the row
     values, as per ftcl__RESULT_ROW1_VALS.
  */
  ftcl__RESULT_ROW1_VALS2,

  /**
     list of lists:

     {{col-names...} {col1Val...} {col1Val...} ...}
  */
  ftcl__RESULT_ROWS_LISTS,

  /**
     dict-ish list: {{*} {col-names} col1Name col1Value...}
  */
  ftcl__RESULT_ROW1_DICT,

  /**
     The first row's value of a specific result column number.
  */
  ftcl__RESULT_ROW1_COLUMN,

  /**
     A list of the values from a specific result column number from
     all result rows.
  */
  ftcl__RESULT_ROWS_COLUMN,

  /**
     No result rows, only a list of column names.
  */
  ftcl__RESULT_COLNAMES,
  /**
     Result from --eval.
  */
  ftcl__RESULT_EVAL,

  /**
     Returns 1 if a row was found, else 0.
  */
  ftcl__RESULT_EXISTS

};

/**
   Modes of operation for binding Tcl lists to sqlite3_stmt
   parameters, via the eval command's (--bind, --bind-list,
   --bind-map) flags.
*/
enum ftcl__BIND_STYLE_e {
  /** Sentinel value. */
  ftcl__BIND_STYLE_NONE = 0,
  /**
     Bind a single value and add it to a list so that the --bind flag
     may be used multiple times to bind subsequent parameters.
  */
  ftcl__BIND_STYLE_LIST1 = 1,
  /**
     Bind a list of values in this form:

     {
       value1
       -type value2
       -- value3
     }

     The -type flags are defined via ftcl__bind_type_flag() except for
     the -- flag, which means "the next value is a value, even if it
     looks like a flag," and the value's type is determined
     automatically.
  */
  ftcl__BIND_STYLE_LIST2 = 2,
  /**
     Bind a map of values in this form:

     {
       :X ?-type? value1
       $X ?-type? value2
       @X ?--? value3
     }

     The -type flags are defined via ftcl__bind_type_flag(), and see
     ftcl__BIND_STYLE_LIST2 for the meaning of the -- flag.
  */
  ftcl__BIND_STYLE_MAP = 3
};

/**
   For communicating how (db query ---eval X Y) should handle the X
   part.
*/
enum fctl__EVAL_VAR_e {
  /** Sentinel value. */
  ftcl__EVAL_VAR_INVALID  = 0,
  /** Set row results in a scope-local array. */
  ftcl__EVAL_VAR_ARRAY    = 1,
  /** Set row results in a scope-local vars named after the result
      column names. */
  ftcl__EVAL_VAR_COLNAMES = 2,
  /** Set row results in a scope-local vars named after the result
      column indexes, i.e. $0..$n. In this mode, the list of column
      names is stored in ${*}. */
  ftcl__EVAL_VAR_COLNUMS  = 3
};

/**
   State for various db-related operations, namely the (fossil db
   query|batch) interfaces. These are also (through a quirk of
   evolution) the implementation class for prepared statements.
*/
struct QueryState {
  /**
     The native "this" object.
  */
  ftcl_cx * fx;
  /**
     String representation of SQL NULL. If it's NULL it will derive
     from fx->null. We separate it so that individual API calls can
     specify a per-call custom NULL string value.
  */
  Tcl_Obj * tNull;

  /** Command object manager. */
  th_cmdo * pCmdo;
  /** For packaged-as-command queries only. */
  fsl_stmt q;
  struct {
    /**
       How to bind values from fBindList;
    */
    enum ftcl__BIND_STYLE_e style;
    Tcl_Obj * tList;
  } bind;
  struct {
    /** fsl_malloc()'d list of Tcl-alloced Tcl_Obj with the names each
        column. */
    Tcl_Obj ** aObj;
    /** List of current query's column names, each one also available
        in this->a. For use with ftcl__EVAL_VAR_COLNAMES. */
    Tcl_Obj * tColNames;
    /** List of numbers of the column indexes, for use with
        ftcl__EVAL_VAR_COLNUMS. */
    Tcl_Obj * tColNums;
    /** Number of items in this->a. */
    unsigned n;
  } colInfo;
  /** State for db eval --result X */
  struct {
    /**
       ftcl__RESULT_... value.
    */
    enum ftcl__RESULT_e type;

    /**
       0-based result column number to return if this->type indicates
       to use it.
    */
    int column;

    /** Return value. */
    Tcl_Obj * rv;
  } result;
  struct {
    enum fctl__EVAL_VAR_e varMode;
    Tcl_Obj * tRowName;
    Tcl_Obj * tScript;
  } eval;
  /**
     Because a fsl_cx-managed db can be pulled out from under a query
     or stmt command object, we maintain a list of all queries open
     for a given ftcl_cx/fsl_cx combo in ftcl_cx::pQueries and
     invalide them when the fsl_cx's db is closed. This does not
     protect from C-level calls to fsl_close_scm_dbs() (or similar),
     only to equivalent calls from these Tcl pieces.

     We don't strictly need to do the same for fsl_db/ftcl_cx
     contexts, as we have a refcount and can keep the context opened
     so long as queries remain open. However, that discrepancy
     vis-a-vis fsl_cx instances is awkward and may lead to,
     e.g. locking problems, when a script tries to close and re-open a
     db, so we also invalidate all active QueryState objects when a db
     closes.
  */
  struct {
    struct QueryState * pPrev;
    struct QueryState * pNext;
  } link;
  struct {
    /**
       1 if it should process all statements in the SQL source. 0 if it
       should stop after the first.
    */
    unsigned char isMulti:1;
    /** 1 if (eval --bind -TYPEFLAG val) is legal. */
    unsigned char bindTypeFlags:1;
    /** 1 if empty strings should bind as SQL NULL. */
    unsigned char bindEmptyAsNull:1;
    unsigned char noStarColNames:1;
    /**
       If true, certain db eval ops will elide elements which
       have SQL NULL values, rather than emit them as an empty
       Tcl string.
    */
    unsigned char noNullColumnResults:1;
    /**
       If true, unset SQL NULL entries instead of setting them to the
       null-string value.
    */
    unsigned char unsetNulls:1;
    /**
       If true, return only the column names, not the query results.
    */
    unsigned char colNamesOnly:1;

    /**
       If true, do not look for data type flags in QueryState_bind_list().
    */
    unsigned char noBindTypeFlags:1;
    unsigned char bResetOnBind:1;
  } flags;
};

typedef struct QueryState QueryState;

static const QueryState QueryState_empty = {
  .fx = 0, .tNull = 0, .pCmdo = 0,
  .q = fsl_stmt_empty_m,
  .bind = { .style = ftcl__BIND_STYLE_NONE, .tList = 0 },
  .colInfo = {0, 0, 0, 0},
  .result = {ftcl__RESULT_NONE, 0, 0},
  .eval = {ftcl__EVAL_VAR_INVALID, 0, 0},
  .link = {.pPrev = 0, .pNext = 0},
  .flags = {0, 0, 0, 0, 0, 0, 0, 0}
};

/** Add qs to qs->fx->pQueries */
static void QueryState_qlist_add(QueryState * const qs){
  assert(!qs->link.pPrev);
  assert(!qs->link.pNext);
  assert(qs->fx);
  assert(qs->fx->pQueries != qs);
  qs->link.pNext = qs->fx->pQueries;
  if( qs->link.pNext ){
    assert( !qs->link.pNext->link.pPrev );
    qs->link.pNext->link.pPrev = qs;
  }
  qs->fx->pQueries = qs;
}

/** Remove qs from qs->fx->pQueries */
static void QueryState_qlist_snip(QueryState * const qs){
  assert(qs->fx);
  if( qs->link.pPrev ){
    assert( qs == qs->link.pPrev->link.pNext );
    qs->link.pPrev->link.pNext = qs->link.pNext;
  }
  if( qs->link.pNext ){
    assert( qs == qs->link.pNext->link.pPrev );
    qs->link.pNext->link.pPrev = qs->link.pPrev;
  }
  if( qs->fx->pQueries==(void const *)qs ){
    qs->fx->pQueries = qs->link.pPrev
      ? qs->link.pPrev : qs->link.pNext;
  }
  qs->link = QueryState_empty.link;
}

void QueryState_invalidate_queries(ftcl_cx * const fx){
  while( fx->pQueries ){
    QueryState * const qs = fx->pQueries;
    QueryState * const next = qs->link.pNext;
    assert( qs->fx && "Should not have a QueryState without an ->fx" );
    assert( fx == qs->fx );
    assert( !qs->link.pPrev );
    assert( next!= qs );
    QueryState_qlist_snip(qs);
    if(qs->q.stmt){
      /*MARKER(("Invaliding query due to db closing:\n%.*s\n",
        (int)qs->q.sql.used, fsl_buffer_cstr(&qs->q.sql)));*/
      fsl_stmt_finalize(&qs->q);
    }
    fx->pQueries = next;
  }
}

#if 0
static char const * QueryState_get_null(QueryState const * const qs){
  return qs->tNull
    ? th_gs1(qs->tNull)
    : qs->fx->null.z;
}
#endif

/** Cleans up e->colInfo. */
static void QueryState_colinfo_cleanup(QueryState * const qs){
  for( int i = 0; i < qs->colInfo.n; ++i ){
    th_unref(qs->colInfo.aObj[i]);
  }
  fsl_free( qs->colInfo.aObj );
  th_unref( qs->colInfo.tColNames );
  th_unref( qs->colInfo.tColNums );
  th_unref( qs->tNull );
  qs->colInfo = QueryState_empty.colInfo;
}

/** Cleans up all state owned by e but does not free e (it's assumed
    to be stack-allocated). */
static void QueryState_cleanup(QueryState * const qs){
  assert( (qs->pCmdo ? 0==qs->pCmdo->refCount : 1)
          && "This cleanup is either from a stack-allocated qs "
          "or the th_cmdo finalizer" );
  QueryState_qlist_snip(qs);
  fsl_stmt_finalize(&qs->q);
  th_unref(qs->eval.tRowName);
  th_unref(qs->eval.tScript);
  th_unref(qs->result.rv);
  th_unref(qs->bind.tList);
  QueryState_colinfo_cleanup(qs);
  /* do not th_cmdo_unref(qs->pCmdo) from here */
  ftcl_cx_unref(qs->fx)/* must be at the end */;
  *qs = QueryState_empty;
}

/**
   Initialize some of qs's state from fx, some of it shared. The
   latter object MUST outlive qs.
*/
static inline void QueryState_init_from_cx( QueryState * const qs,
                                            ftcl_cx * const fx ){
  assert( !qs->fx );
  qs->fx = ftcl_cx_ref(fx);
  //fx->flags.verbose = 3;
  QueryState_qlist_add(qs);
}

static QueryState * QueryState_alloc(ftcl_cx * fx){
  QueryState * const qs = fsl_malloc(sizeof(QueryState));
  *qs = QueryState_empty;
  QueryState_init_from_cx(qs, fx);
  return qs;
}

static void QueryState_cmdo_finalizer(void * p){
  //MARKER(("~QueryState @ %p via cmdo finalizer\n", p));
  QueryState_cleanup(p);
  fsl_free(p);
}


static inline void QueryState_setupStar(QueryState * const qs){
  if( !qs->fx->cache.tStar ){
    qs->fx->cache.tStar = th_nso("*", 1);
    th_ref( qs->fx->cache.tStar );
  }
}

/**
   If qs->colInfo.n is not 0, this returns 0 without side-effects.
   If qs->colInfo.n is zero, this sets up the result column name list
   based on q. Returns 0 on success, TCL_ERROR on error.
*/
static int QueryState_colinfo_setup(QueryState * const qs,
                                    fsl_stmt const * q ){

  if( qs->colInfo.n ) return 0;
  char intBuf[32] = {0};
  int rc = 0;
  Tcl_Interp * const tcl = qs->fx->cmdo.tcl;
  if( !q ) q = &qs->q;

  QueryState_colinfo_cleanup(qs);
  assert(qs->colInfo.n==0);
  assert(!qs->colInfo.aObj);
  assert(!qs->colInfo.tColNames);
  assert(!qs->colInfo.tColNums);
  qs->colInfo.n = sqlite3_column_count(q->stmt);
  if( !qs->flags.noStarColNames ){
    QueryState_setupStar(qs);
  }
  if( qs->colInfo.n>0 ){
    qs->colInfo.tColNames = th_nlo(0,0);
    th_ref(qs->colInfo.tColNames);
    if( qs->eval.varMode == ftcl__EVAL_VAR_COLNUMS ){
      /* TODO: figure out whether we need both lists, based on
         qs.result and qs.eval. */
      qs->colInfo.tColNums = th_nlo(0,0);
      th_ref(qs->colInfo.tColNums);
    }
    qs->colInfo.aObj = fsl_malloc(sizeof(Tcl_Obj*) * qs->colInfo.n);
    for(int i = 0; 0==rc && i < qs->colInfo.n; ++i){
      char const * z = sqlite3_column_name(q->stmt,i);
      if( !z ){
        rc = ftcl_rs(tcl, TCL_ERROR,
                     "Error fetching name for column #%d (out of bounds?)", i);
        break;
      }
      qs->colInfo.aObj[i] = th_nso(z, -1);
      th_ref(qs->colInfo.aObj[i]);
      rc = th_loae(tcl, qs->colInfo.tColNames, qs->colInfo.aObj[i]);
      if( 0==rc && qs->colInfo.tColNums ){
        int const n = snprintf(intBuf, sizeof(intBuf), "%d", i);
        assert(n>0);
        rc = th_loae(tcl, qs->colInfo.tColNums, th_nso(intBuf, n));
        assert(!rc);
      }
    }
  }
  return rc;
}


/**
   Try to determine the sqlite3 data type to bind a value to.

   If o is a legal int, sets *pI64 and returns SQLITE_INTEGER.

   Else if o is a legal double, sets *pDbl and returns SQLITE_FLOAT.

   Else returns SQLITE_TEXT.
*/
static int ftcl__sqlite3_type_for_obj(Tcl_Obj *o, Tcl_WideInt * pI64, double *pDbl){
  if( 0==Tcl_GetWideIntFromObj(NULL, o, pI64) ){
    return SQLITE_INTEGER;
  }
  if (0==Tcl_GetDoubleFromObj(NULL, o, pDbl)){
    return SQLITE_FLOAT;
  }
  return SQLITE_TEXT;
}

/**
   If the given string is one of the -type flags used by the binding
   APIs, its SQLite data type is returned, else ftcl__BINTE_TYPE_GUESS
   is returned.
*/
static int ftcl__bind_type_flag(char const * z, int nVal){
  assert( nVal>1 && nVal<10 );
#if 0
  /* Just testing out th_enum... */
  static const th_enum eFlags = {
    6, {
#define E(N,V) {N, sizeof(N)-1, V}
      E("-text",     SQLITE_TEXT),
      E("-blob",     SQLITE_BLOB),
      E("-null",     SQLITE_NULL),
      E("-real",     SQLITE_FLOAT),
      E("-integer",  SQLITE_INTEGER),
      E("-nullable", ftcl__SQL_TYPE_NULLABLE)
#undef E
    }
  };
  th_enum_entry const * const e = th_enum_search(&eFlags, z, nVal);
  return e ? e->value : ftcl__SQL_TYPE_GUESS;
#else
  /* Same thing, but performs better on average. */
  switch(nVal){
#define IFFLAG(F) if( 0==strncmp(z, F, nVal) )
    case 5:
      IFFLAG("-text")     return SQLITE_TEXT;
      IFFLAG("-real")     return SQLITE_FLOAT;
      IFFLAG("-blob")     return SQLITE_BLOB;
      IFFLAG("-null")     return SQLITE_NULL;
      break;
    case 8:
      IFFLAG("-integer")  return SQLITE_INTEGER;
      break;
    case 9:
      IFFLAG("-nullable") return ftcl__SQL_TYPE_NULLABLE;
      break;
#undef IFFLAG
  }
  return ftcl__SQL_TYPE_GUESS;
#endif
}

/**
   Handles sqlite3_bind() on q for the given "bind object", the
   semantics of which are appropriate for ftcl__cx_xDbQuery().

   See ftcl__BIND_STYLE_e for tcl-side syntax details.
*/
static int QueryState_bind_list(QueryState * const qs,
                           fsl_stmt * q){
  if( !q ) q = &qs->q;
  assert( qs->bind.tList );
  Tcl_Interp * const tcl = qs->fx->cmdo.tcl;
  Tcl_Size nCol = 0;
  int rc = Tcl_ListObjLength(tcl, qs->bind.tList, &nCol);
  int dbrc = 0;
  int bindCol = 1;
  int bindType = ftcl__SQL_TYPE_GUESS;
  char const * zFlag = 0;
  Tcl_WideInt pI64 = 0;
  double pDbl = 0;
  /*MARKER(("paramCount=%d, noBindTypeFlags=%d\n", q->paramCount,
    qs->flags.noBindTypeFlags));*/
  if( qs->flags.bResetOnBind ){
    /* If we bind while a statement is running, binding fails.*/
    fsl_stmt_reset(q);
  }
  for( int i = 0; 0==rc && i < nCol; ++i ){
    Tcl_Obj * e = 0;
    rc = Tcl_ListObjIndex(tcl, qs->bind.tList, i, &e);
    if( rc ) break;
    //assert( e );
    //MARKER(("nCol=%d i=%d value=%s\n", (int)nCol, (int)i, th_gs1(e)));
    if( !zFlag ){
      /* Check for a -type flag or $X/:X/@X binding name... */
      if( ftcl__BIND_STYLE_MAP==qs->bind.style ){
        /* Check for $X/:X/@X binding name... */
        Tcl_Size nKey = 0;
        char const *zKey = th_gs(e, &nKey);
        switch(*zKey){
          case (int)'$':
          case (int)':':
          case (int)'@':
            if( !zKey[1] ){
              rc = ftcl_err(tcl, "Missing name for --bind-map "
                            "key at bind list index %d", i);
              break;
            }
            bindCol = fsl_stmt_param_index(q, zKey);
            if( bindCol < 1 ){
              rc = ftcl_err(tcl, "Cannot find column binding "
                            "named %s", zKey);
              break;
            }
            break;
          default:
            rc = ftcl_err(tcl, "Bind map keys must start "
                          "with :, @, or $");
            break;
        }
        if( 0==rc ){
          /* Advance to the next argument */
          /* FIXME: catch trailing key with no value case. */
          //MARKER(("bind key: %s\n",zKey));
          if( i+1==nCol ){
            rc = ftcl_err(tcl, "Missing binding-map value for key %s",
                          zKey);
          }else{
            rc = Tcl_ListObjIndex(tcl, qs->bind.tList, ++i, &e);
            assert( rc || !!e );
            //MARKER(("binding value: %s = %s\n",zKey, th_gs1(e)));
          }
        }
        if( rc ) break;
      }/*ftcl__BIND_STYLE_MAP*/

      if(0==qs->flags.noBindTypeFlags
         && (ftcl__BIND_STYLE_LIST2==qs->bind.style
             || ftcl__BIND_STYLE_MAP==qs->bind.style) ){
        /* Check for --type flag or -- flag */
        Tcl_Size nVal = 0;
        char const * const z = th_gs(e, &nVal);
        if( z && '-'==z[0]
            && nVal>1 && nVal<=9 ){
          if( 2==nVal && '-'==z[1] ){ /* -- */
            assert( ftcl__SQL_TYPE_GUESS==bindType );
            zFlag = z;
            continue;
          }
          bindType = ftcl__bind_type_flag(z, nVal);
          if( bindType>0 ){ /* -type */
            zFlag = z;
            continue;
          }
        }
      }
      assert( 0==rc );
      /* Fall through and treat it like a value... */
    }/*end of check for a -type flag*/

/* Mimic some of the fsl_db_prepare() internals, to be called
   immediately after an sqlite3_prepare(). */
    zFlag = 0;
    if( bindCol > sqlite3_bind_parameter_count(q->stmt) ){
      rc = ftcl_err(tcl,
                    "Bind column #%d is out of bounds for SQL: %s",
                    bindCol, sqlite3_sql(q->stmt));
      break;
    }

    /*MARKER(("bindCol=%d bindType=%d q->paramCount=%d\n",
      bindCol, bindType, q->paramCount));*/
    if( e ){
      if( ftcl__SQL_TYPE_GUESS == bindType ){
        bindType = ftcl__sqlite3_type_for_obj(e, &pI64, &pDbl);
      }else{
        switch( bindType ){
          case SQLITE_INTEGER:
            Tcl_GetWideIntFromObj(NULL, e, &pI64);
            break;
          case SQLITE_FLOAT:
            Tcl_GetDoubleFromObj(NULL, e, &pDbl);
            break;
        }
      }
      switch( bindType ){
        case SQLITE_INTEGER:
          /*MARKER(("binding #%d: int %lld\n", bindCol, (long long)pI64));*/
          dbrc = fsl_stmt_bind_int64(q, bindCol, (sqlite3_int64)pI64);
          /*MARKER(("bind #%d: int %lld dbrc=%s\n", bindCol, (long long)pI64, fsl_rc_cstr(dbrc)));*/
          break;
        case SQLITE_FLOAT:
          //f_out("binding #%d: double %lf\n", bindCol, pDbl);
          dbrc = fsl_stmt_bind_double(q, bindCol, pDbl);
          break;
        case SQLITE_NULL:
          //f_out("binding #%d: null\n", bindCol);
          dbrc = fsl_stmt_bind_null(q, bindCol);
          break;
        default:{
          Tcl_Size nVal = 0;
          char const * const z = th_gs(e, &nVal);
          if( 0==nVal && (qs->flags.bindEmptyAsNull
                          || ftcl__SQL_TYPE_NULLABLE==bindType) ){
              //f_out("binding #%d: null by virtue of being empty\n", bindCol);
              dbrc = fsl_stmt_bind_null(q, bindCol);
          }else if( SQLITE_BLOB==bindType ){
            //f_out("binding #%d: blob of %d bytes\n", bindCol, nVal);
            unsigned char const * zz = Tcl_GetByteArrayFromObj(e, &nVal);
            dbrc = fsl_stmt_bind_blob(q, bindCol, zz, (fsl_int_t)nVal, true);
          }else{
            //MARKER(("binding #%d: string of %d bytes: %.*s\n", bindCol, nVal, nVal, z));
            dbrc = fsl_stmt_bind_text(q, bindCol, z, (fsl_int_t)nVal, true);
          }
        }
      }
    }else{
      //f_out("binding #%d: null\n", bindCol);
      dbrc = fsl_stmt_bind_null(q, bindCol);
    }
    if(dbrc){
      rc = ftcl_rs_db(tcl, q->db, dbrc);
      break;
    }
    bindType = ftcl__SQL_TYPE_GUESS;
    ++bindCol;
  }/* for-each bind argument */
  if( 0==rc && zFlag ){
    rc = ftcl_rs(tcl, TCL_ERROR,
                 "Missing value after bind flag %s", zFlag);
  }
  //MARKER(("leaving with dbrc=%s rc=%d\n", fsl_rc_cstr(dbrc), rc));
  return rc;
}

/**
   Returns fx's cached copy of its SQL NULL string representation,
   initializing it if needed.
*/
Tcl_Obj * ftcl_cx__get_null(ftcl_cx * const fx){
  if( !fx->null.t ){
    assert(fx->null.z);
    fx->null.t = th_nso(fx->null.z, (int)fx->null.n);
    th_ref(fx->null.t);
  }
  return fx->null.t;
}

/**
   Fetch the string value of column colIndex of query q, accounting for
   NULL-type values.
*/
static int QueryState_col_text(QueryState * const qs, fsl_stmt * const q,
                               int colIndex, char const **z,
                               fsl_size_t *nOut){
  int rc = 0;
  switch( sqlite3_column_type( q->stmt, colIndex ) ){
    case SQLITE_NULL: {
      if( qs->tNull ){
        Tcl_Size n = 0;
        *z = th_gs(qs->tNull, &n);
        *nOut = (fsl_size_t)n;
      }else{
        assert( qs->fx->null.z );
        *z = qs->fx->null.z;
        *nOut = (fsl_size_t)qs->fx->null.n;
      }
      break;
    }
    default:
      rc = fsl_stmt_get_text(q, colIndex, z, nOut);
      break;
  }
  if(rc){
    rc = ftcl_rs_db(qs->fx->cmdo.tcl, q->db, rc);
  }
  return rc;
}

static Tcl_Obj * QueryState_col(QueryState * const qs, fsl_stmt * const q,
                                int iCol, int nullAsNull ){
  sqlite3_stmt * const pStmt = q->stmt;
  assert( iCol>=0 );
  assert( iCol<sqlite3_column_count(q->stmt) );
  /* What follows was taken, almost verbatim, from SQLite's
     tcl extension. */
  switch( sqlite3_column_type(q->stmt, iCol) ){
    case SQLITE_BLOB: {
      const char *zBlob = sqlite3_column_blob(pStmt, iCol);
      int const bytes = zBlob ? sqlite3_column_bytes(pStmt, iCol) : 0;
      return Tcl_NewByteArrayObj((unsigned char *)zBlob, bytes);
    }
    case SQLITE_INTEGER: {
      return th_obj_for_int64( sqlite3_column_int64(pStmt, iCol) );
    }
    case SQLITE_FLOAT: {
      return Tcl_NewDoubleObj(sqlite3_column_double(pStmt, iCol));
    }
    case SQLITE_NULL: {
      if( nullAsNull ) return NULL;
      return qs->tNull ? qs->tNull : ftcl_cx__get_null(qs->fx);
    }
    default: {
      const char *zBlob = (char const *)sqlite3_column_text(pStmt, iCol);
      int const bytes = zBlob ? sqlite3_column_bytes(pStmt, iCol) : 0;
      return Tcl_NewStringObj(zBlob, bytes);
    }
  }
}

/**
   Depending on qs->result.type and q->colCount, this may or may not
   stuff q's current row data into qs->result.rv. Returns a Tcl error
   code on error.
*/
static int QueryState_return_result(QueryState * const qs,
                                    fsl_stmt * q,
                                    unsigned rowId){
  if( !q ) q = &qs->q;
  Tcl_Interp * const tcl = qs->fx->cmdo.tcl;
  int rc = 0;
  int const nCol = sqlite3_column_count(q->stmt);
  switch( nCol ? qs->result.type : ftcl__RESULT_NONE ){
    case ftcl__RESULT_EXISTS:
    case ftcl__RESULT_COLNAMES: /* Handled in ftcl__db_foreach_f_single() */
    case ftcl__RESULT_EVAL: /* Handled via QueryState_eval() */
    case ftcl__RESULT_NONE:
    case ftcl__RESULT_UNKNOWN:
      break;
    case ftcl__RESULT_ROW1_VALS:
    case ftcl__RESULT_ROW1_VALS2:{
      if( 0!=rowId ) break;
      assert( !qs->result.rv );
      Tcl_Obj * lrow = th_nlo(0,0);
      qs->result.rv = lrow;
      th_ref(qs->result.rv);
      if( ftcl__RESULT_ROW1_VALS2==qs->result.type ) {
        if( !qs->flags.noStarColNames ){
          if( 0==(rc = QueryState_colinfo_setup(qs, q)) ){
            rc = th_loae(tcl, qs->result.rv, qs->colInfo.tColNames);
          }
          if(0==rc){
            lrow = th_nlo(0,0);
          }
        }
      }
      for( int i = 0; 0==rc && i < nCol; ++i ){
        fsl_size_t n = 0;
        char const * z = 0;
        rc = QueryState_col_text(qs, q, i, &z, &n);
        if(!rc){
          rc = th_loae(tcl, lrow, th_nso(z, (int)n));
        }
      }
      if( !rc && lrow!=qs->result.rv ){
        rc = th_loae(tcl, qs->result.rv, lrow);
      }
      th_bounce( lrow );
      assert( !lrow );
      break;
    }
    case ftcl__RESULT_ROWS_LISTS:{
      if( !qs->result.rv ){
        assert( 0==rowId );
        qs->result.rv = th_nlo(0,0);
        th_ref(qs->result.rv);
        if( !qs->flags.noStarColNames ){
          if( 0==(rc = QueryState_colinfo_setup(qs, q)) ){
            rc = th_loae(tcl, qs->result.rv, qs->colInfo.tColNames);
          }
          if(rc) break;
        }
      }
      Tcl_Obj * lrow = th_nlo(0,0);
      for( int i = 0; 0==rc && i < nCol; ++i ){
        fsl_size_t n = 0;
        char const * z = 0;
        rc = QueryState_col_text(qs, q, i, &z, &n);
        if(!rc){
          rc = th_loae(tcl, lrow, th_nso(z, (int)n));
        }
      }
      if( !rc ){
        rc = th_loae(tcl, qs->result.rv, lrow);
      }
      th_bounce( lrow );
      assert( !lrow );
      break;
    }
    case ftcl__RESULT_ROW1_DICT:
      if(rowId) break;
      assert( !qs->result.rv );
      Tcl_Obj * const lrow = th_nlo(0,0);
      qs->result.rv = lrow;
      th_ref(qs->result.rv);
      if( 0==(rc = QueryState_colinfo_setup(qs, q)) ){
        assert( qs->colInfo.n == nCol );
        if( !qs->flags.noStarColNames ) {
          assert( qs->fx->cache.tStar );
          rc = th_loae(tcl, lrow, qs->fx->cache.tStar);
          if(0==rc){
            rc = th_loae(tcl, lrow, qs->colInfo.tColNames);
          }
        }
      }
      for( int i = 0; 0==rc && i < nCol; ++i ){
        fsl_size_t n = 0;
        char const * z = 0;
        rc = QueryState_col_text(qs, q, i, &z, &n);
        if(!rc){
          if( !*z ){
            if( qs->flags.noNullColumnResults ) continue;
            z = "";
            n = 0;
          }
          if( 0==(rc = th_loae(tcl, lrow, qs->colInfo.aObj[i])) ){
            rc = th_loae(tcl, lrow, th_nso(z, (int)n));
          }
        }
      }
      if(rc){
        th_unref(qs->result.rv);
        assert( !qs->result.rv );
      }
      break;
    case ftcl__RESULT_ROW1_COLUMN:{
      if( 0!=rowId ) break;
      assert( !qs->result.rv );
      assert( qs->result.column>=0 );
      if( qs->result.column<0 || qs->result.column>=nCol) {
        rc = ftcl_rs(tcl, TCL_ERROR,
                     "Column index %d is out of range",
                     qs->result.column);
      }else{
        fsl_size_t n = 0;
        char const * z = 0;
        rc = QueryState_col_text(qs, q, qs->result.column, &z, &n);
        if( !rc ){
          qs->result.rv = th_nso(z,(int)n);
          th_ref(qs->result.rv);
        }
      }
      break;
    }
    case ftcl__RESULT_ROWS_COLUMN:{
      /* Collect column #qs->result.colIndex for each row. */
      if( !qs->result.rv ){
        assert( 0==rowId );
        assert( qs->result.column>=0 );
        if( qs->result.column<0 || qs->result.column>=nCol) {
          rc = ftcl_rs(tcl, TCL_ERROR,
                       "Column index %d is out of range",
                       qs->result.column);
          break;
        }
        qs->result.rv = th_nlo(0,0);
        th_ref(qs->result.rv);
      }
      assert( qs->result.rv );
      fsl_size_t n = 0;
      char const * z = 0;
      rc = QueryState_col_text(qs, q, qs->result.column, &z, &n);
      if( !rc ){
        rc = th_loae(tcl, qs->result.rv, th_nso(z, (int)n));
      }
      break;
    }
  }
  return rc;
}

/**
   Handle (---eval varname {script}) on behalf of the query
   subcommand.
*/
static int QueryState_eval(QueryState * const qs, fsl_stmt * const q,
                           unsigned rowId){
  if( !qs->eval.tScript ) return 0;
  int rc = 0;
  Tcl_Interp * const tcl = qs->fx->cmdo.tcl;
  Tcl_Obj * const tRowName = qs->eval.tRowName;
  char const * zRowName = 0;
  Tcl_Obj ** tColNums = 0;
  Tcl_Size nColNums = 0;
  char intBuf[32] = {0};
  assert( ftcl__EVAL_VAR_INVALID != qs->eval.varMode );
  assert( tRowName ? qs->eval.varMode==ftcl__EVAL_VAR_ARRAY : 1);
  rc = QueryState_colinfo_setup(qs, q);

  switch( qs->eval.varMode ){
    default:
      break;
    case ftcl__EVAL_VAR_ARRAY:
      zRowName = th_gs1(tRowName);
      if( 0==rowId ){
        Tcl_UnsetVar2(tcl, zRowName, 0, 0);
      }
      break;
    case ftcl__EVAL_VAR_COLNUMS:
      assert( qs->colInfo.tColNums );
      if( 0!=(rc = Tcl_ListObjGetElements(tcl, qs->colInfo.tColNums,
                                          &nColNums, &tColNums)) ){
        goto end;
      }
      assert( nColNums == sqlite3_column_count(q->stmt) );
      if( 0==rowId ){
        Tcl_UnsetVar2(tcl, "*", 0, 0);
      }
      break;
  }

  if( !qs->flags.noStarColNames ){
    switch( qs->eval.varMode ){
      default: break;
      case ftcl__EVAL_VAR_ARRAY:
        assert( tRowName );
        QueryState_setupStar(qs);
        if( !Tcl_ObjSetVar2(tcl, tRowName,
                            qs->fx->cache.tStar,
                            qs->colInfo.tColNames,
                            TCL_LEAVE_ERR_MSG) ) {
          rc = TCL_ERROR;
          goto end;
        }
        break;
      case ftcl__EVAL_VAR_COLNUMS:
        assert( qs->colInfo.tColNums );
        QueryState_setupStar(qs);
        if( !Tcl_ObjSetVar2(tcl, qs->fx->cache.tStar, 0,
                            qs->colInfo.tColNums,
                            TCL_LEAVE_ERR_MSG) ) {
          rc = TCL_ERROR;
          goto end;
        }
        break;
    }
  }

  int const nCol = sqlite3_column_count(q->stmt);
  for( int i = 0; 0==rc && i < nCol; ++i ){
    Tcl_Obj * tCol = QueryState_col(qs, q, i, qs->flags.unsetNulls);
    if( tCol ){
      switch(qs->eval.varMode){
        case ftcl__EVAL_VAR_INVALID:
          assert(!"cannot happen");
          return TCL_ERROR;
        case ftcl__EVAL_VAR_ARRAY:
          assert( tRowName );
          if( !Tcl_ObjSetVar2(tcl, tRowName, qs->colInfo.aObj[i], tCol,
                              TCL_LEAVE_ERR_MSG) ) {
            rc = TCL_ERROR;
            goto end;
          }
          break;
        case ftcl__EVAL_VAR_COLNAMES:
          if( !Tcl_ObjSetVar2(tcl, qs->colInfo.aObj[i], 0, tCol,
                              TCL_LEAVE_ERR_MSG) ) {
            rc = TCL_ERROR;
            goto end;
          }
          break;
        case ftcl__EVAL_VAR_COLNUMS:{
          assert(tColNums);
          if( !Tcl_ObjSetVar2(tcl, tColNums[i], 0, tCol,
                              TCL_LEAVE_ERR_MSG) ) {
            rc = TCL_ERROR;
            goto end;
          }
          break;
        }
      }
    }else{/*SQL NULL value (or an OOM)*/
      assert(qs->flags.unsetNulls && "Else tCol would be an empty string");
      switch(qs->eval.varMode){
        case ftcl__EVAL_VAR_INVALID:
          assert(!"cannot happen");
          return TCL_ERROR;
        case ftcl__EVAL_VAR_ARRAY:
          assert( tRowName );
          if( !zRowName ) zRowName = th_gs1(tRowName);
          Tcl_UnsetVar2(tcl, zRowName, th_gs1(qs->colInfo.aObj[i]), 0);
          break;
        case ftcl__EVAL_VAR_COLNAMES:{
          Tcl_UnsetVar2(tcl, th_gs1(qs->colInfo.aObj[i]), 0, 0);
          break;
        }
        case ftcl__EVAL_VAR_COLNUMS:{
          assert(tColNums);
          int const n = snprintf(intBuf, sizeof(intBuf), "%d", i);
          assert(n>0);
          Tcl_UnsetVar2(tcl, intBuf, 0, 0);
          break;
        }
      }
    }
  }/*foreach sql result column*/

  /* Set the result value... */
  switch( qs->result.type ){
    case ftcl__RESULT_EVAL:
      /* Let Eval store a result */
      rc = Tcl_EvalObjEx(tcl, qs->eval.tScript, 0);
      break;
    default:{
      Tcl_InterpState tis = Tcl_SaveInterpState(tcl, 0)
        /* Without this, eval stomps on our --return values. */;
      assert( qs->eval.tScript );
      if( 0==(rc = Tcl_EvalObjEx(tcl, qs->eval.tScript, 0)) ){
        Tcl_RestoreInterpState(tcl, tis);
      }else{
        Tcl_DiscardInterpState(tis);
      }
      break;
    }
  }

end:
  /* We clean up via ftcl__db_foreach_f_single() */
  return rc;
}

/**
   Cleans up script-visible qs->eval state (if any). It does not clean
   up the non-script-visible state.
*/
static void QueryState_eval_epilog(QueryState * const qs, fsl_stmt * const q){
  Tcl_Interp * const tcl = qs->fx->cmdo.tcl;
  if( qs->eval.tRowName ){
    Tcl_UnsetVar2(tcl, th_gs1(qs->eval.tRowName), 0, 0);
  }
  Tcl_Size nColNums = 0;
  Tcl_Obj ** tColNums = 0;
  if( qs->colInfo.tColNums ){
    if( !qs->flags.noStarColNames && qs->fx->cache.tStar ){
      Tcl_UnsetVar2(tcl, th_gs1(qs->fx->cache.tStar), 0, 0);
    }
    if( 0==Tcl_ListObjGetElements(tcl, qs->colInfo.tColNums,
                                  &nColNums, &tColNums) ){
      assert( nColNums == qs->colInfo.n );
    }
  }
  if( qs->colInfo.aObj || qs->colInfo.tColNums ){
    int const nCol = sqlite3_column_count(q->stmt);
    for( int i = 0; i < nCol; ++i ){
      if( qs->colInfo.aObj ){
        Tcl_UnsetVar2(tcl, th_gs1(qs->colInfo.aObj[i]), 0, 0);
      }
      if( tColNums ){
        Tcl_UnsetVar2(tcl, th_gs1(tColNums[i]), 0, 0);
      }
    }
  }
}

/**
   ftcl_db_foreach_f() impl which processes only the first query in
   the set, returning TCL_BREAK on success each time it's called.

   px must be a (QueryState*). If it has any statement bindings to
   make, they are bound here, before looping over the query results
   (if any).
*/
static int ftcl__db_foreach_f_single(void * const px, Tcl_Interp * const tcl,
                                     fsl_stmt * const q, unsigned id){
  int rc = 0;
  QueryState * const qs = px;
  bool gotOne = false;
  const bool justExists = qs->result.type==ftcl__RESULT_EXISTS;

  if( qs->bind.tList ){
    rc = QueryState_bind_list(qs, q);
    if( rc ) return rc;
    //f_out("Bound list of %d param(s)\n", (int)q->paramCount);
  }

  {
    unsigned rowId = 0;
    int rcSq = 0;
    while( (rcSq = fsl_stmt_step(q)) == FSL_RC_STEP_ROW ) {
      if( justExists ){
        gotOne = true;
        rcSq = FSL_RC_STEP_DONE;
        break;
      }
      /* Reminder to self: we can hypothetically skip this if
         !(qs->fEval.nSeen || qs->fReturn.nSeen), but it's possible for
         the SQL to have side-effecs, so we need to run it even if we're
         not interested in the results. */
      rc = QueryState_return_result(qs, q, rowId);
      //MARKER(("post-return_result rc=%d rcSq=%d %s\n", rc, rcSq, fsl_rc_cstr(rcSq)));
      if( 0==rc ) rc = QueryState_eval(qs, q, rowId);
      if( rc ) break;
      ++rowId;
    }
    //MARKER(("post-step rc=%d, rcSq=%d %s\n", rc, rcSq, fsl_rc_cstr(rcSq)));
    if( !rc && rcSq ){
      if( FSL_RC_STEP_DONE!=rcSq ){
        rc = ftcl_rs_db(tcl, q->db, rcSq);
      }
    }
  }
  if( 0==rc ) QueryState_eval_epilog(qs, q);
  if( 0==rc ){
    if( justExists ){
      assert( !qs->result.rv );
      qs->result.rv = th_nso(gotOne ? "1" : "0", 1);
      th_ref(qs->result.rv);
    }else if( qs->result.type==ftcl__RESULT_COLNAMES ){
      if( 0==(rc = QueryState_colinfo_setup(qs, q)) ){
        qs->result.rv = qs->colInfo.tColNames;
        th_ref(qs->result.rv);
      }
    }
  }
  if(rc){
    //MARKER(("leaving with rc=%d\n", rc));
  }
  return rc ? rc : TCL_BREAK;
}

int ftcl_needs_db(ftcl_cx * const fx){
  fsl_db * const db = ftcl_cx_db(fx);
  char const * const zErrIfNot = "This database has been closed";
  return (db && db->dbh)
    ? 0
    : th_rs_c(fx->cmdo.tcl, TCL_ERROR, zErrIfNot);
}

static inline QueryState * QueryState_from_cmdo(th_cmdo * const cm){
  return DbGlobal.zTypeQueryState==cm->type ? cm->p : NULL;
}

static int QueryState_cmdo_check_db(Tcl_Interp * tcl,
                                    th_cmdo * const cm,
                                    bool reportErr){
  QueryState * const qs = QueryState_from_cmdo(cm);
  assert( qs && qs->fx && "Else severe internal mismanagement" );
  if( !qs->q.stmt ){
    return reportErr
      ? th_err(qs->fx->cmdo.tcl,
                  "This prepared statement was invalidated "
                  "because its db was closed")
      : TCL_ERROR;
  }else if( !ftcl_cx_db(qs->fx) ){
    return reportErr
      ? th_err(qs->fx->cmdo.tcl, "The database is closed")
      : TCL_ERROR;
  }
  return 0;
}

static FTCX__METHOD_DECL(xStmtBind){
  if(QueryState_cmdo_check_db(tcl, cx, true)) return TCL_ERROR;
  int rc = 0;
#if 0
  rc = th_err(tcl, "bind is TODO");
#else
  QueryState * const qs = QueryState_from_cmdo(cx);
  QueryState const qsOld = *qs;
  assert( !qs->bind.tList );

  ftcl_cmd_init;
  ftcl_fp_flag("--list", "{values...}", 0, fBindList);
  ftcl_fp_flag("--map", "{mappings...}", 0, fBindMap);
  ftcl_fp_flag("-no-type-flags", 0, 0, fNoType);
  ftcl_fp_flag("-bind-{}-as-null", 0, 0, fBindNulls);
  ftcl_fp_flag("-clear", 0, 0, fClear);
  ftcl_fp_flag("-count", 0, 0, fCount);
  ftcl_fp_flag("-reset", 0, 0, fReset);
  ftcl_fp_parse(NULL);

  int const seenList = (!!fBindList->nSeen + !!fBindMap->nSeen);
  if( seenList && (seenList>1
                   || th_flags_argc(&fp)
                   || fCount->nSeen) ){
    rc = ftcl_err(tcl, "None of (%s, %s, %s, or bind args) may be used "
                  "together", fBindList->zFlag, fBindMap->zFlag,
                  fCount->zFlag);
    goto end;
  }else if( th_flags_argc(&fp) > sqlite3_column_count(qs->q.stmt) ){
    rc = th_err(tcl, "Too many binding arguments for");
    goto end;
  }

  //MARKER(("-no-type-flags=%d\n", !!fNoType->nSeen));
  qs->flags.noBindTypeFlags = !!fNoType->nSeen;

  if( fCount->nSeen ){
    rc = th_rs(tcl, 0, "%d", sqlite3_column_count(qs->q.stmt));
  }
  if( 0==rc && fClear->nSeen ){
    sqlite3_clear_bindings(qs->q.stmt);
  }
  if( 0==rc && (seenList || th_flags_argc(&fp)) ){
    if( !seenList ){
      assert(th_flags_argc(&fp));
      assert(!fBindList->pVal);
      assert(!fBindMap->pVal);
      qs->bind.tList = Tcl_NewListObj(0,0);
      th_ref(qs->bind.tList);
      for( int i = 0; 0==rc && i < th_flags_argc(&fp); ++i ){
        rc = th_loae(tcl, qs->bind.tList, th_flags_arg(&fp, i));
      }
      if( rc ) goto end;
    }else if( fBindList->pVal ){
      qs->bind.tList = th_ref(fBindList->pVal);
      qs->bind.style = ftcl__BIND_STYLE_LIST1;
    }else {
      assert( fBindMap->pVal );
      qs->bind.tList = th_ref(fBindMap->pVal);
      qs->bind.style = ftcl__BIND_STYLE_MAP;
    }
    qs->flags.bResetOnBind = !!fReset->nSeen;
    qs->flags.bindEmptyAsNull = !!fBindNulls->nSeen;
    rc = QueryState_bind_list(qs, NULL);
  }

end:
  ftcl_cmd_cleanup;
  qs->flags = qsOld.flags;
  th_unref(qs->bind.tList);
#endif
  return rc;
}

static FTCX__METHOD_DECL(xStmtStep){
  if(QueryState_cmdo_check_db(tcl, cx, true)) return TCL_ERROR;
  QueryState * const qs = QueryState_from_cmdo(cx);
  int rc = 0;
  int const rcSq = fsl_stmt_step(&qs->q);
  if( FSL_RC_STEP_ROW==rcSq ) {
    rc = QueryState_eval(qs, &qs->q, 0);
    QueryState_eval_epilog(qs, &qs->q);
  }
  switch(rc ? 0 : rcSq){
    case 0: break;
    case FSL_RC_STEP_ROW:
      th_rs_c(tcl, 0, "1");
      rc = 0;
      break;
    case FSL_RC_STEP_DONE:
      th_rs_c(tcl, 0, "0");
      rc = 0;
      break;
    default:
      rc = ftcl_rs_db(tcl, qs->q.db, rcSq);
      break;
  }
  //ftcl_cmd_cleanup;
  return rc;
}

static FTCX__METHOD_DECL(xStmtReset){
  if(QueryState_cmdo_check_db(tcl, cx, true)) return TCL_ERROR;
  QueryState * const qs = QueryState_from_cmdo(cx);
  if( 1!=argc ){
    return th_err(tcl, "Unexpected arguments to %s",
                  th_gs1(argv[0]));
  }
  fsl_stmt_reset(&qs->q);
  return 0;
}

#if 0
static int ftcl_tstrcmp(char const * z, Tcl_Size nZ, Tcl_Obj * const t){
  Tcl_Size nT = 0;
  char const *zT = th_gs(t, &nT);
  return nZ==nT ? 0==fsl_strcmp(z, zT) : 0;
}
#endif

static FTCX__METHOD_DECL(xStmtGet){
  if(QueryState_cmdo_check_db(tcl, cx, true)) return TCL_ERROR;
  QueryState * const qs = QueryState_from_cmdo(cx);
  Tcl_Obj * const oldTNull = qs->tNull;

  int rc = 0;
  int ndx = 0;
  int nRequired = 1 /* number of required non-flag args */;
  th_ref(oldTNull);
  ftcl_cmd_init;
  ftcl_fp_flag("--null-string",  "value", 0, fNull);
  ftcl_fp_flag("-column-names", 0, 0, fNames);
  ftcl_fp_flag("-count", 0, 0, fCount);
  ftcl_fp_flag("-list", 0, 0, fList);
  ftcl_fp_flag("-dict", 0, 0, fDict);
  //ftcl_fp_flag("-no-column-names", 0, 0, fNoStar);
  ftcl_fp_parse("-flag|columnIndex");

  int const sawFlags =
    (!!fNames->nSeen + !!fList->nSeen + !!fDict->nSeen
     + !!fCount->nSeen
    )
    /* Flags which control the result type. */;

  if( sawFlags ){
    if( sawFlags>1 ){
      rc = th_err(tcl, "Cannot be used together: %s %s %s %s",
                  fCount->nSeen, fDict->zFlag, fList->zFlag,
                  fNames->zFlag);
      goto end;
    }
    nRequired = 0;
  }

  if( nRequired!=th_flags_argc(&fp) ){
    rc = th_err(tcl, "Unexpected arguments to %s", argv[0]);
    goto end;
  }

  if( fCount->nSeen ){
    rc = th_rs(tcl, 0, "%d", sqlite3_column_count(qs->q.stmt));
    goto end;
  }

  if( fNull->nSeen ){
    qs->tNull = th_ref(fNull->pVal);
  }

  ftcl_cmd_replace_argv;

  if( fNames->nSeen ){
    QueryState const qsOld = *qs;
    qs->flags.noStarColNames = 1;
    rc = QueryState_colinfo_setup(qs, NULL);
    qs->flags = qsOld.flags;
    if( rc ) goto end;
    assert( qs->colInfo.tColNames );
    assert( !Tcl_IsShared(qs->colInfo.tColNames) );
#if 0
    rc = th_rs_o(tcl, 0, qs->colInfo.tColNames);
    assert( Tcl_IsShared(qs->colInfo.tColNames) );
#elif 1
    rc = th_rs_c(tcl, 0, th_gs1(qs->colInfo.tColNames));
#else
    rc = th_rs_o(tcl, 0, Tcl_DuplicateObj(qs->colInfo.tColNames));
#endif
    goto end;
  }else if( fList->nSeen || fDict->nSeen ){
    /* Fetch the row as a list or dict-like list */
    th_unref(qs->result.rv);
    QueryState const qsOld = *qs;
    qs->flags.noStarColNames = 1;
    qs->result.column = -1;
    qs->result.type = fDict->nSeen
      ? ftcl__RESULT_ROW1_DICT
      : ftcl__RESULT_ROW1_VALS;
    rc = QueryState_return_result(qs, NULL, 0);
    Tcl_Obj * const rv = qs->result.rv;
    qs->flags = qsOld.flags;
    qs->result = qsOld.result;
    qs->result.rv = rv;
    if( rc ) goto end;
    assert( qs->result.rv );
    rc = th_rs_o(tcl, 0, qs->result.rv);
    th_unref(qs->result.rv);
  }else if( nRequired==argc ){
    assert( argc );
    /* Extract one column index */
    if( 0!=Tcl_GetIntFromObj(NULL, argv[0], &ndx) ){
      rc = th_err(tcl, "Not an integer value: %s",
                  th_gs1(argv[0]));
      goto end;
    }else if(ndx<0 || ndx>=sqlite3_column_count(qs->q.stmt)){
      rc = th_err(tcl, "Column index %d is out of range", ndx);
      goto end;
    }
    Tcl_Obj * const rv = QueryState_col(qs, &qs->q, ndx, 0);
    assert( rv );
    rc = th_rs_o(tcl, 0, rv);
    goto end;
  }else{
    rc = th_err(tcl, "'get' can't figure out what to do!");
  }

end:
  assert( oldTNull ? Tcl_IsShared(oldTNull) : 1 );
  th_unref(qs->tNull);
  qs->tNull = oldTNull;
  ftcl_cmd_cleanup;
  return rc;
}

static FTCX__METHOD_DECL(xStmtColCount){
  if(QueryState_cmdo_check_db(tcl, cx, true)) return TCL_ERROR;
  QueryState * const qs = QueryState_from_cmdo(cx);
  return th_rs(tcl, 0, "%d", sqlite3_column_count(qs->q.stmt));
}

#if 0
/* TODO: "parameter" subcommand:

   - count
   - index NAME
*/
static FTCX__METHOD_DECL(xStmtParam){
  if(QueryState_cmdo_check_db(tcl, cx, true)) return TCL_ERROR;
  QueryState * const qs = QueryState_from_cmdo(cx);
  return th_rs(tcl, "%d", qs->q.paramCount);
}
#endif

static FTCX__METHOD_DECL(xStmtFinalize){
  QueryState * const qs = QueryState_from_cmdo(cx);
  assert( qs->pCmdo );
  assert( qs->fx );
  th_cmdo * const cm = qs->pCmdo;
  assert( cm->p == qs );
  assert( cx == cm );
  assert( cm->refCount>0 );
  qs->pCmdo = 0;
  th_cmdo_unref( cm ) /* will free qs */;
  return 0;
}

static FTCX__METHOD_DECL(xDbStmt){
  static const th_subcmd aSub[] = {
    /* !!!! Maintenance note: keep these sorted !!!! */
    th_subcmd_init("bind",            FTCX__METHOD_NAME(xStmtBind), 0),
    th_subcmd_init("column-count",    FTCX__METHOD_NAME(xStmtColCount), 0),
    th_subcmd_init("finalize",        FTCX__METHOD_NAME(xStmtFinalize), 0),
    th_subcmd_init("get",             FTCX__METHOD_NAME(xStmtGet), 0),
    //th_subcmd_init("parameter",       FTCX__METHOD_NAME(xStmtParam), 0),
    th_subcmd_init("reset",           FTCX__METHOD_NAME(xStmtReset), 0),
    th_subcmd_init("step",            FTCX__METHOD_NAME(xStmtStep), 0),
    /* !!!! keep these sorted !!!! */
  };
  static const unsigned nSub =
    sizeof(aSub)/sizeof(aSub[0]);

  return ftcl__cx_dispatch( cx, 1, argc, argv, nSub, aSub, 1 );
}

/** NRE command adapter. */
static int ftcl__stmt_ObjCmdAdaptor(
  ClientData cd, Tcl_Interp *tcl, int argc, Tcl_Obj * const * argv
){
  return Tcl_NRCallObjProc(tcl, FTCX__METHOD_NAME(xDbStmt), cd, argc, argv);
}

/**
   Creates a Command object for qs (which must have been allocated
   using QueryState_alloc()). This is the basis of the xStmt... family
   of commands. Ownership of qs is passed to this function. On
   success, Tcl owns it via the installed Command object. On error qs
   is freed.
*/
static int QueryState_setup_cmdo(QueryState * const qs,
                                 Tcl_Obj * const tVarName){
  assert(qs->fx);
  assert(!qs->pCmdo);
  th_cmdo * const cm = th_cmdo_alloc(qs->fx->cmdo.tcl,
                                     DbGlobal.zTypeQueryState,
                                     qs, QueryState_cmdo_finalizer);
  if( cm ){
    int const rc = th_cmdo_plugin(cm, ftcl__stmt_ObjCmdAdaptor,
                                  FTCX__METHOD_NAME(xDbStmt),
                                  NULL, tVarName);
    if( rc ){
      th_cmdo_unref(cm);
    }else{
      qs->pCmdo = cm /* cm is now owned by Tcl. */;
      assert( 1==cm->refCount );
      return 0;
    }
  }
  QueryState_cmdo_finalizer(qs);
  return TCL_ERROR;
}

/**
   Executes a single arbitrary SQL statement, optionally binding SQL
   column values, and runs a user-defined script or callback function.

   Command flags:

   query -bind X -bind Y
   {SELECT ?1 WHERE ?2}

   query --bind-list {?-type? X ?-type? Y}
   {SELECT ?1 WHERE ?2}

   query --bind-map {
     :X ?-type? 1
     :Y ?-type? 2
   }
   {SELECT :X WHERE :Y}
*/
static FTCX__METHOD_DECL(xDbQuery){
  /* Valid values for --return X. */
  static const th_enum eReturn = {
    10, {
#define E(N,V) {N, sizeof(N)-1, V}
      E("none",         ftcl__RESULT_NONE),
      E("row1-list",    ftcl__RESULT_ROW1_VALS),
      E("*",            ftcl__RESULT_ROW1_VALS /*w/o column names*/),
      E("row1-lists",   ftcl__RESULT_ROW1_VALS2),
      E("rows-lists",   ftcl__RESULT_ROWS_LISTS),
      E("**",           ftcl__RESULT_ROWS_LISTS /*w/o column names*/),
      E("row1-dict",    ftcl__RESULT_ROW1_DICT),
      E("column-names", ftcl__RESULT_COLNAMES),
      E("eval",         ftcl__RESULT_EVAL),
      E("exists",       ftcl__RESULT_EXISTS),
#undef E
#if 0
      /* TODO? */
      E("**",           ftcl__RESULT_ROWS_LISTS w/o column names),
      E("*.*",          ftcl__RESULT_ROW1_VALS w/ column names),
      E("*.**",         ftcl__RESULT_ROWS_LISTS w/ column names)
#endif
    }
  };
  /* Help text for eReturn. */
  static char eReturnBuf[(sizeof(eReturn.e)/sizeof(eReturn.e[0]))
                         * 16] = {0};

  if( !eReturnBuf[0] ){
    th_enum_generate_list(&eReturn, -1, eReturnBuf,
                           sizeof(eReturnBuf), "|");
  }

  QueryState qs = QueryState_empty;
  char const * zUsage = "sqlString";
  ftcl_cmd_rcfx;
  QueryState_init_from_cx(&qs, fx);
  ftcl_cmd_init;
  ftcl_fp_flag("--bind", "value", th_F_CMDFLAG_LIST, fBindList1);
  ftcl_fp_flag("--bind-list", "{values...}", 0, fBindList2);
  ftcl_fp_flag("--bind-map", "{mappings...}", 0, fBindMap);
  ftcl_fp_flag("--null-string", "value", 0, fNull);
  ftcl_fp_flag("-bind-{}-as-null", 0, 0, fBindNulls);
  ftcl_fp_flag("--return", eReturnBuf, 0, fReturn);
  ftcl_fp_flag("-no-column-names", 0, 0, fNoStar);
  ftcl_fp_flag("---eval", "varName|-$ script", 0, fEval);
  ftcl_fp_flag("-unset-null", 0, 0, fEvalNoNulls);
  ftcl_fp_flag("-prepare", 0, 0, fPrepare);
  ftcl_fp_flag("--prepare", "varName", 0, fPrepareNamed);
  ftcl_fp_flag("-no-type-flags", 0, 0, fNoType);

  fp.options = th_F_PARSE_CHECK_ALL;
  ftcl_fp_parse(zUsage);

  fsl_db * const db = ftcl_cx_db(fx);
  ftcl__db_open_check;
  assert(db);

  //th_dump_argv("db query argv", tcl, argc, argv);
  //th_dump_flags("db query", tcl, &fp);
  if( 1 < !!fBindList1->nSeen + !!fBindList2->nSeen + !!fBindMap->nSeen ){
    rc = ftcl_err(tcl, "None of (--bind-dict, --bind-list, --bind) "
                  "may be used together");
    goto end;
  }else if( 1!=th_flags_argc(&fp) ){
    rc = ftcl_rs_argc(tcl, argv, zUsage);
    goto end;
  }else if(fPrepare->nSeen && fPrepareNamed->nSeen){
    rc = ftcl_err(tcl, "%s and %s cannot be used together",
                  fPrepare->zFlag, fPrepareNamed->zFlag);
    goto end;
  }else if( fReturn->nSeen &&
            (fPrepare->nSeen || fPrepareNamed->nSeen) ){
    rc = ftcl_err(tcl, "%s cannot be used with %s or %s",
                  fReturn->zFlag, fPrepare->zFlag,
                  fPrepareNamed->zFlag);
    goto end;

  }

  if( fReturn->nSeen ){
    th_enum_entry const * wee;
    Tcl_Size nZ = 0;
    char const * z = th_gs(fReturn->pVal, &nZ);

    assert( fReturn->pVal );
    assert(ftcl__RESULT_NONE==qs.result.type);
    qs.result.type = ftcl__RESULT_UNKNOWN;
    if( (wee = th_enum_search(&eReturn, z, (unsigned)nZ)) ){
      qs.result.type = wee->value;
      if( '*'==wee->zName[0] ){
        ++fNoStar->nSeen;
      }
    }else if( '#'==z[0] || '*'==z[0] ){
      /* If passed #int or *int, assume it's a result column index. */
      if( 1==sscanf(z+1, "%d", &qs.result.column) && qs.result.column>=0 ){
        qs.result.type = '#'==z[0]
          ? ftcl__RESULT_ROW1_COLUMN
          : ftcl__RESULT_ROWS_COLUMN;
      }
    }
    if( ftcl__RESULT_UNKNOWN == qs.result.type ){
      rc = ftcl_err(tcl, "Invalid value for the %s flag: %.*s",
                    fReturn->zFlag, nZ, z);
      goto end;
    }
  }else if( fEval->nSeen ){
    qs.result.type = ftcl__RESULT_EVAL;
  }

  if( fEval->pVal ){
    Tcl_Obj ** eargv = 0;
    Tcl_Size eargc = 0;
    rc = Tcl_ListObjGetElements(tcl, fEval->pVal, &eargc, &eargv);
    if(rc) goto end;
    else if (2!=eargc){
      assert(!"This should not have gotten past the parser");
      rc = ftcl_err(tcl, "Expecting two arguments for ---eval flag");
      goto end;
    }
    assert( ftcl__EVAL_VAR_INVALID == qs.eval.varMode );
    qs.eval.tScript = eargv[1];
    th_ref(qs.eval.tScript);
    Tcl_Obj * const e = eargv[0];
    Tcl_Size nZ = 0;
    char const * z = th_gs(e,&nZ);
    if( 2==nZ && '-'==z[0] && '$'==z[1] ){
      qs.eval.varMode = ftcl__EVAL_VAR_COLNAMES;
    }else if( 2==nZ && '-'==z[0] && '#'==z[1] ){
      qs.eval.varMode = ftcl__EVAL_VAR_COLNUMS;
    }else{
      qs.eval.varMode = ftcl__EVAL_VAR_ARRAY;
      qs.eval.tRowName = e;
      th_ref(qs.eval.tRowName);
    }
  }

  if( fNull->pVal ){
    qs.tNull = fNull->pVal;
    th_ref(qs.tNull);
  }

#define LI(F,BT)                             \
  if(F->pVal) {                              \
    qs.bind.tList = th_ref(F->pVal);         \
    qs.bind.style = ftcl__BIND_STYLE_ ## BT; \
  }
  LI(fBindList1, LIST1)
  else LI(fBindList2, LIST2)
  else LI(fBindMap, MAP)
#undef LI

  qs.flags.bindEmptyAsNull = !!fBindNulls->nSeen;
  qs.flags.noStarColNames = !!fNoStar->nSeen;
  qs.flags.unsetNulls = !!fEvalNoNulls->nSeen;
  qs.flags.noBindTypeFlags = !!fNoType->nSeen;

  Tcl_Size nSql = 0;
  char const * const zSql = th_flags_arg_cstr(&fp, 0, &nSql);
  if( fPrepare->nSeen || fPrepareNamed->nSeen ){
    /* Return a prepared statement command object */
    QueryState * const qqs = QueryState_alloc(fx);
    assert( !fReturn->nSeen );
    int rc3 = fsl_db_prepare(db, &qqs->q, "%.*s", (int)nSql, zSql);
    if( rc3 ){
      QueryState_cmdo_finalizer(qqs);
      rc = ftcl_rs_db(tcl, db, rc3);
      goto end;
    }
    assert( !qs.pCmdo );
    assert( !qs.result.rv );
    assert( !qs.colInfo.tColNames );
    /*transfer ownership...*/
#define XFER(F) qqs->F = qs.F; qs.F = QueryState_empty.F
    XFER(tNull);
    XFER(bind);
    XFER(eval);
    XFER(result);
    XFER(flags);
    /* NO: XFER(link) */
#undef XFER
    rc = QueryState_setup_cmdo(qqs, fPrepareNamed->pVal)
      /* On error, qqs was already freed. On success, Tcl owns qqs via
         th_cmdo (qqs->pCmdo). */;
    if( 0==rc ){
      if( qqs->bind.tList ){
        rc = QueryState_bind_list(qqs, NULL);
        th_unref(qqs->bind.tList);
        qqs->bind.style = ftcl__BIND_STYLE_NONE;
      }
      if( 0==rc ){
        rc = th_rs_o(tcl, 0, th_cmdo_name(qqs->pCmdo, false));
      }
    }
  }else{
    rc = ftcl_db_foreach(tcl, db, ftcl__db_foreach_f_single, &qs,
                         zSql, nSql);
  }

end:
  if( 0==rc && qs.result.rv ){
    Tcl_SetObjResult(tcl, qs.result.rv);
  }
  QueryState_cleanup(&qs);
  ftcl__flags_cleanup(&fp);
  return rc;
}

static int ftcl__db_foreach_f_batch(void * px, Tcl_Interp * tcl,
                                   fsl_stmt *q, unsigned id){
  QueryState * const qs = px;
  int rc = 0;
  while( (rc = fsl_stmt_step(q)) == FSL_RC_STEP_ROW ) {}
  return (rc && FSL_RC_STEP_DONE!=rc)
    ? ftcl_rs_db(tcl, q->db, rc)
    : (qs->flags.isMulti ? 0 : TCL_BREAK);
}

/**
   Executes all statements in a blob of arbitrary SQL.
*/
static FTCX__METHOD_DECL(xDbBatch){
  int rc;
  ftcl_cx * const fx = cx;
  fsl_db * const db = ftcl_cx_db(fx);
  fsl_buffer b = fsl_buffer_empty;
  char const * zSql = 0;
  Tcl_Size nSql = 0;
  QueryState qs = QueryState_empty;
  char const * zUsage = "sql-string";

  QueryState_init_from_cx(&qs, fx);
  th_decl_cmdflags_init;
  th_decl_cmdflag("-file", 0, 0, fFile);
  th_decl_cmdflag("-no-transaction", 0, 0, fNoTx);
  th_decl_cmdflag("-dry-run", 0, 0, fDryRun);
  ftcl_decl_cmdflags(fp, 0);
  ftcl__db_open_check;
  ftcl__cx_flags_parse(&fp, zUsage);

#if 0
  MARKER(("fx@%p p@%p type=%d\n", fx, fx->p, fx->eType));
  if( ftcl_cx_TYPE_FSL==fx->eType ){
    assert( fsl_cx_db(ftcl_cx_fsl(fx)) );
  }
  assert(db);
  MARKER(("db opened=%d\n", db->dbh ? 1 : (int)0));
  //assert(!"here");
#endif

  assert(db);

  //th_dump_argv("db exec argv", tcl, argc, argv);
  //th_dump_flags("db exec", tcl, &fp);
  if( 1!=th_flags_argc(&fp) ){
    rc = ftcl_rs_argc(tcl, argv, zUsage);
    goto end;
  }
  Tcl_Obj * const oArg =  th_flags_arg(&fp, 0);
  if( fFile.nSeen ){
    rc = ftcl_file_read(tcl, th_gs1(oArg), &b);
    if(rc) goto end;
    zSql = fsl_buffer_cstr(&b);
    nSql = (Tcl_Size)b.used;
  }else{
    zSql = th_flags_arg_cstr(&fp, 0, &nSql);
  }
  assert( zSql );
  qs.flags.isMulti = 1;
  assert(!qs.result.rv);
  const int useTx = fDryRun.nSeen || !fNoTx.nSeen;
  if( useTx ){
    rc = fsl_db_txn_begin(db);
    if(rc){
      rc = ftcl_rs_db(tcl, db, rc);
      goto end;
    }
  }
  rc = ftcl_db_foreach(tcl, db, ftcl__db_foreach_f_batch, &qs,
                       zSql, nSql);
  if( useTx ){
    int const rc2 = fsl_db_txn_end_v2(db, 0==rc && !fDryRun.nSeen, false );
    if( !rc && rc2 ){
      rc = ftcl_rs_db(tcl, db, rc2);
    }
  }
end:
  assert(!qs.result.rv);
  QueryState_cleanup(&qs);
  fsl_buffer_clear(&b);
  ftcl__flags_cleanup(&fp);
  return rc;
}

static FTCX__METHOD_DECL(xDbTransaction){
  char const * zUsage = "script";
  int rc = 0, rce = 0;
  ftcl_cx * const fx = cx;
  fsl_db * const db = ftcl_cx_db(fx);
  fsl_buffer b = fsl_buffer_empty;
  char const * zScript = 0;
  Tcl_Size nScript = 0;

  assert(db);

  th_decl_cmdflags_init;
  th_decl_cmdflag("-dry-run", 0, 0, fDryRun);
  th_decl_cmdflag("-sql", 0, 0, fSql);
  th_decl_cmdflag("-file", 0, 0, fFile);
  ftcl_decl_cmdflags(fp, 0);
  ftcl__db_open_check;
  ftcl__cx_flags_parse(&fp, zUsage);
  if( 0==th_flags_argc(&fp) && !fp.nSeen ){
    rc = ftcl_rs(tcl, 0, "%d", fsl_db_txn_level(db));
    goto end;
  }else if( 1!=th_flags_argc(&fp) ){
    rc = ftcl_rs_argc(tcl, argv, zUsage);
    goto end;
  }

  Tcl_Obj * const oArg = th_flags_arg(&fp, 0);
  if( fFile.nSeen ){
    rc = ftcl_file_read(tcl, th_gs1(oArg), &b);
    if(rc) goto end;
    zScript = fsl_buffer_cstr(&b);
    nScript = (Tcl_Size)b.used;
  }
  rc = fsl_db_txn_begin(db);
  if( rc ){
    rc = ftcl_rs_db(tcl, db, rc);
    goto end;
  }
  if( fSql.nSeen ){
    if( !zScript ){
      assert( !fFile.nSeen );
      zScript = th_gs(oArg, &nScript);
    }
    rc = ftcl_db_foreach(tcl, db, ftcl_db_foreach_f_stepall,
                         NULL, zScript, nScript);
  }else{
    rc = fFile.nSeen
      ? Tcl_EvalEx(tcl, zScript, nScript, 0)
      : Tcl_EvalObjEx(tcl, oArg, 0);
  }
  rce = fsl_db_txn_end_v2( db, 0==rc && !fDryRun.nSeen, false );
  if( rce && !rc ){
    rc = ftcl_rs_db(tcl, db, rc ? rc : rce );
  }

end:
  fsl_buffer_clear(&b);
  ftcl__flags_cleanup(&fp);
  return rc;
}

static ftcl_cx * ftcl__db_cx_create(Tcl_Interp *tcl, fsl_db * db,
                                        Tcl_Obj * const tVarName);

FTCX__METHOD_DECL(xDbOpen) {
  char const * zUsage = "?db-file?";
  int rc = 0;
  ftcl_cx * const fx = cx;
  ftcl_cx * nfx = 0;
  fsl_db * db = 0;
  char const * zFile = 0;

  if( 1==argc ){
    return ftcl_rs(tcl, 0, "%d", !!ftcl_cx_db(fx));
  }

  th_decl_cmdflags_init;
  th_decl_cmdflag("-no-create", 0, 0, fNoCreate);
  th_decl_cmdflag("-read-only", 0, 0, fRO);
  th_decl_cmdflag("-trace-sql", 0, 0, fTraceSql);
  th_decl_cmdflag("--set", "varName", 0, fVarName);
  ftcl_decl_cmdflags(fp, 0);

  ftcl__cx_flags_parse(&fp, zUsage);
  if( 1!=th_flags_argc(&fp) ){
    rc = ftcl_rs_argc(tcl, argv, zUsage);
    goto end;
  }
  zFile = th_gs1(th_flags_arg(&fp,0));
  int const openFlags = 0
    | (fNoCreate.nSeen ? 0 : FSL_DB_OPEN_CREATE)
    | (fRO.nSeen ? FSL_DB_OPEN_RO : FSL_DB_OPEN_RW)
    | (fTraceSql.nSeen ? FSL_DB_OPEN_TRACE_SQL : 0);
  db = fsl_db_malloc();
  rc = fsl_db_open(db, zFile, openFlags);
  if( rc ){
    rc = ftcl_rs_db(tcl, db, rc);
    fsl_db_close(db);
    db = 0;
    goto end;
  }
  sqlite3_extended_result_codes(db->dbh, 1);
  nfx = ftcl__db_cx_create(tcl, db, fVarName.pVal);
  nfx->flags.verbose = fx->flags.verbose;
  th_rs_c(tcl, 0, Tcl_GetCommandName(tcl, nfx->cmdo.pCmd));
end:
  ftcl__flags_cleanup(&fp);
  if( rc ){
    assert(!nfx && "Should not have allocated this");
  }else{
    assert( nfx->cmdo.type==ftcl_typeid_db() );
    assert(nfx && "We're not leaking it - it's owned by Tcl now");
  }
  return rc;
}

FTCX__METHOD_DECL(xDbClose){
  ftcl_cx * const fx = cx;
  fsl_db * const db = ftcl_cx_db(fx);

  assert(!ftcl_cx_fsl(cx) && "We're not expecting any fsl_cx instances here");
  th_rs(tcl, 0, "%d", !!db);
  if( db ){
    QueryState_invalidate_queries(fx);
    ftcl_cx_unplug(fx);
  }
  return 0;
}

static FTCX__METHOD_DECL(xDbFunction) {
  /* Values for the --return flag */
  static const th_enum eReturn = {
    6, {
#define E(N,V) {N, sizeof(N)-1, V}
      E("text",     SQLITE_TEXT),
      E("real",     SQLITE_FLOAT),
      E("integer",  SQLITE_INTEGER),
      E("blob",     SQLITE_BLOB),
      E("any",      ftcl__SQL_TYPE_GUESS),
      E("null",     SQLITE_NULL),
#undef E
    }
  };
  /* Help text buffer. */
  static char eReturnBuf[(sizeof(eReturn.e)/sizeof(eReturn.e[0]))
                         * 16] = {0};

  if( !eReturnBuf[0] ){
    th_enum_generate_list(&eReturn, -1, eReturnBuf,
                           sizeof(eReturnBuf), "|");
  }

  char const * zUsage = "functionName ?flags?";
  int rc = 0;
  ftcl_cx * const fx = cx;
  fsl_db * const db = ftcl_cx_db(fx);
  int funcFlags = 0;
  ftcl__udf * u = 0;
  ftcl__udf uPre /* buffer to handle some setup and validation before
                    we commit to allocating one */;
  int bSqliteOwnsUdf = 0 /* true once sqlite_create_function*()
                            succeeds */;

  assert(db);

  th_decl_cmdflags_init;
  th_decl_cmdflag("--scalar", "xFuncLambdaExpr", 0, fScalar);
  th_decl_cmdflag("---aggregate", "xStep xFinal", 0, fAgg);
  th_decl_cmdflag("--return", eReturnBuf, 0, fReturn);
  //th_decl_cmdflag("--meta-args", "varName", 0, fMetaArgs);
  th_decl_cmdflag("-return-{}-as-null", 0, 0, fEmptyNull);
  th_decl_cmdflag("-deterministic", 0, 0, fDeter);
  th_decl_cmdflag("-direct-only", 0, 0, fDirect);
  th_decl_cmdflag("-innocuous", 0, 0, fInnoc);
  th_decl_cmdflag("-bad-numbers-as-0", 0, 0, fZero);
  th_decl_cmdflag("-----window", "xStep xFinal xValue xInverse", 0, fWindow);
  fWindow.consumes = 4 /*!!!*/;
  ftcl_decl_cmdflags(fp, th_F_PARSE_CHECK_ALL);
  ftcl__cx_flags_parse(&fp, zUsage);

  if( 0 && fWindow.nSeen ){
    th_dump_argv("db function flags argv", tcl, th_flags_argc(&fp), th_flags_argv(&fp));
    th_dump_flags("db function flags", tcl, &fp);
  }

  if( 1!=th_flags_argc(&fp) ){
    rc = ftcl_rs_argc(tcl, argv, zUsage);
    goto end;
  }

  if( !fp.nSeen || (!fScalar.nSeen && !fAgg.nSeen && !fWindow.nSeen) ){
    rc = ftcl_err(tcl, "Use --scalar or ---aggregate or -----window to set "
                  "the UDF callback(s)");
    goto end;
  }else if( (fScalar.nSeen + fAgg.nSeen + fWindow.nSeen) > 1 ){
    rc = ftcl_err(tcl, "None of --scalar, ---aggregate, or -----window "
                  "may be used together");
    goto end;
  }

  memset(&uPre, 0, sizeof(uPre));
  uPre.sqlResultType = ftcl__SQL_TYPE_GUESS;
  funcFlags |= SQLITE_UTF8
    | (fDeter.nSeen ? SQLITE_DETERMINISTIC : 0)
    | (fDirect.nSeen ? SQLITE_DIRECTONLY : 0)
    | (fInnoc.nSeen ? SQLITE_INNOCUOUS : 0);

  if( fReturn.nSeen ){
    Tcl_Size nZ = 0;
    char const * z = th_gs(fReturn.pVal, &nZ);
    th_enum_entry const * const wee =
      th_enum_search(&eReturn, z, (unsigned)nZ);
    if( wee ){
      uPre.sqlResultType =  wee->value;
    }else{
      rc = ftcl_err(tcl, "Invalid value for the %s flag: %.*s. "
                    "Try one of: %s",
                    fReturn.zFlag, nZ, z, eReturnBuf);
      goto end;
    }
  }

  extern int ftcl__udf_init(ftcl_cx * const fx, enum ftcl__udf_type_e eType,
                            Tcl_Obj * const tName, th_cmdflag * const pFlag,
                            ftcl__udf * const udf, int funcFlags,
                            int * pSqlOwnsUdf)/*in udf.c*/;
  extern ftcl__udf * ftcl__udf_alloc(ftcl_cx * fx)/*in udf.c*/;
  extern void ftcl__udf_free(ftcl__udf * const u)/*in udf.c*/;

  u = ftcl__udf_alloc(fx);
  u->sqlResultType = uPre.sqlResultType;
  if( fZero.nSeen ) u->flags |= ftcl__udf_F_BADNUM_ZERO;
  if( fEmptyNull.nSeen ) u->flags |= ftcl__udf_F_EMPTY_AS_NULL;

  if( fScalar.nSeen ){
    rc = ftcl__udf_init(fx, ftcl__udf_TYPE_SCALAR, th_flags_arg(&fp, 0),
                        &fScalar, u, funcFlags, &bSqliteOwnsUdf);
  }else if( fAgg.nSeen ){
    rc = ftcl__udf_init(fx, ftcl__udf_TYPE_AGGREGATE, th_flags_arg(&fp, 0),
                        &fAgg, u, funcFlags, &bSqliteOwnsUdf);
  }else if( fWindow.nSeen ){
    rc = ftcl__udf_init(fx, ftcl__udf_TYPE_WINDOW, th_flags_arg(&fp, 0),
                        &fWindow, u, funcFlags, &bSqliteOwnsUdf);
  }
  if( rc ) goto end;

end:
  ftcl__flags_cleanup(&fp);
  if( rc && !bSqliteOwnsUdf && u ){
    ftcl__udf_free(u);
  }
  return rc;
}

FTCX__METHOD_DECL(xDbInfo){
  int rc = 0;
  ftcl_cx * const fx = cx;
  fsl_db * const db = ftcl_cx_db(fx);

  th_decl_cmdflags_init;
  th_decl_cmdflag("-file", 0, 0, fFile);
  th_decl_cmdflag("-name", 0, 0, fName);
  ftcl_decl_cmdflags(fp, 0);
  ftcl__cx_flags_parse(&fp, NULL);

  if( !db ){
    assert(ftcl_cx_fsl(fx) && "Else we couldn't have gotten to this point");
    rc = th_rs_c(tcl, 0, 0);
    goto end;
  }
  if( fp.nSeen>1 || th_flags_argc(&fp) ){
    char buf[128] = {0};
    th_flags_generate_help(&fp, buf, sizeof(buf));
    rc = ftcl_err(tcl, "info expects only a single flag: %s", buf);
    goto end;
  }
  if( fFile.nSeen ){
    fsl_size_t n = 0;
    char const * z = fsl_db_filename(db, &n);
    rc = ftcl_rs(tcl, 0, "%.*s", (int)n, z);
  }else if( fName.nSeen ){
    char const * z = fsl_db_name(db);
    if( !z ){
      z = sqlite3_db_name(db->dbh, 0);
      if( z ){
        MARKER(("warning: fossil/sqlite3 db name mismatch: %s", z));
      }
    }
    rc = th_rs_c(tcl, 0, z);
  }else{
    assert(!"cannot happen");
  }
end:
  ftcl__flags_cleanup(&fp);
  return rc;
}

static FTCX__METHOD_DECL(xDbChanges) {
  char const * zUsage = "?flags?";
  int rc = 0;
  ftcl_cx * const fx = cx;
  fsl_db * const db = ftcl_cx_db(fx);
  assert(db);

  th_decl_cmdflags_init;
  th_decl_cmdflag("-total", 0, 0, fTotal);
  ftcl_decl_cmdflags(fp, th_F_PARSE_CHECK_ALL);
  ftcl__db_open_check;
  ftcl__cx_flags_parse(&fp, zUsage);

  //th_dump_flags("db changes", tcl, &fp);

  if( fp.nonFlag.count ){
    rc = ftcl_err(tcl, "%s: unexpected argument: %s",
                  th_gs1(argv[0]), th_gs1(th_flags_arg(&fp,0)));
    goto end;
  }

  sqlite3_int64 const t =  fTotal.nSeen
    ? sqlite3_total_changes64(db->dbh)
    : sqlite3_changes64(db->dbh);

  Tcl_Obj * rv = th_obj_for_int64(t);
  th_ref(rv);
  rc = th_rs_o(tcl, 0, rv);
  th_unref(rv);

end:
  ftcl__flags_cleanup(&fp);
  return rc;
}

static FTCX__METHOD_DECL(xDbConfig) {
  char const * zUsage = "?flags?";
  int rc = 0;
  ftcl_cx * const fx = cx;
  fsl_db * const db = ftcl_cx_db(fx);
  assert(db);

  th_decl_cmdflags_init;
  th_decl_cmdflag("--busy-timeout", "time", 0, fBusy);
  th_decl_cmdflag("--null-string", "value", 0, fNullSet);
  th_decl_cmdflag("-null-string", 0, 0, fNullGet);
  ftcl_decl_cmdflags(fp, th_F_PARSE_CHECK_ALL);
  ftcl__db_open_check;
  ftcl__cx_flags_parse(&fp, zUsage);

  //th_dump_flags("db config", tcl, &fp);

  if( fp.nonFlag.count || !fp.nSeen ){
    rc = ftcl_err(tcl, "%s expects an argument or flag",
                  th_gs1(argv[0]));
    goto end;
  }

  if( fBusy.nSeen ){
    int vi = 0;
    if( 0==Tcl_GetIntFromObj(NULL, fBusy.pVal, &vi) ){
    }else{
      rc = ftcl_err(tcl, "%s flag: invalid value (%s) - expecting an integer",
                    fBusy.zFlag, th_gs1(fBusy.pVal));
      goto end;
    }
    sqlite3_busy_timeout(db->dbh, vi);
  }

  if( fNullGet.nSeen ){
    th_rs_c(tcl, 0, fx->null.z);
  }

  if( fNullSet.nSeen ){
    Tcl_Size n = 0;
    char const * z = th_gs(fNullSet.pVal, &n);
    th_rs_c(tcl, 0, fx->null.z ? fx->null.z : "");
    fsl_free(fx->null.z);
    fx->null.z = fsl_strndup(z, (fsl_int_t)n);
    fx->null.n = (unsigned)n;
    th_unref(fx->null.t);
    fx->null.t = 0 /* will be re-set on demand */;
    (void)ftcl_cx__get_null(fx);
  }

end:
  ftcl__flags_cleanup(&fp);
  return rc;
}

#if 1
FTCX__METHOD_DECL(xDbSelf){
  ftcl_cx * const fx = cx;
  if( ftcl_cx_db(fx) && fx->cmdo.pCmd ){
    char const * z = Tcl_GetCommandName(tcl, fx->cmdo.pCmd);
    th_rs_c(tcl, 0, z);
  }else{
    Tcl_ResetResult(tcl);
  }
  return 0;
}
#endif

/**
   The main db command handler. It simply dispatches to subcommands,
   with no logic of its own. If passed on subcommands then its Tcl
   result is this command object's name.

   Reminder to self: to add -? support to this we'd have to parse the
   flags on every invocation, which is relatively expensive
   considering that we don't otherwise need flags here and 99.9%+ of
   calls to this won't be passing -?. If we were parsing flags for
   other purposes we'd get -? along with that "for free."
*/
FTCX__METHOD_DECL(xDb){
  /**
     Subcommands available to ftcl_cx instances which have an
     associated fsl_cx instance. These use that context's fsl_cx_db()
     for all subcommands.
  */
  static const th_subcmd aSubFsl[] = {
    /* !!!!

       Maintenance note: keep these sorted and then fix aSubDb for
       new/removed entries!

       !!!! */
    th_subcmd_init("batch",       FTCX__METHOD_NAME(xDbBatch), 0),
    th_subcmd_init("changes",     FTCX__METHOD_NAME(xDbChanges), 0),
    th_subcmd_init("config",      FTCX__METHOD_NAME(xDbConfig), 0),
    th_subcmd_init("function",    FTCX__METHOD_NAME(xDbFunction), 0),
    th_subcmd_init("info",        FTCX__METHOD_NAME(xDbInfo), 0),
    th_subcmd_init("open",        FTCX__METHOD_NAME(xDbOpen), 0),
    th_subcmd_init("query",       FTCX__METHOD_NAME(xDbQuery), 0),
    th_subcmd_init("transaction", FTCX__METHOD_NAME(xDbTransaction), 0),
    /* !!!! SEE NOTES ABOVE !!!! */
  };
  static const unsigned nSubFsl =
    sizeof(aSubFsl)/sizeof(aSubFsl[0]);

  /**
     Subcommands available to ftcl_cx instances which are "standalone
     db instances" (no associated fsl_cx).
  */
  static th_subcmd aSubDb[] = {
    aSubFsl[0], aSubFsl[1], aSubFsl[2], aSubFsl[3],
    aSubFsl[4], aSubFsl[5], aSubFsl[6], aSubFsl[7],
    th_subcmd_init("close", FTCX__METHOD_NAME(xDbClose), 0),
  };
  static const unsigned nSubDb =
    sizeof(aSubDb)/sizeof(aSubDb[0]);

  static int once = 0;
  if( !once ){
    th_subcmd_sort(nSubDb, aSubDb);
    ++once;
  }

  if( 1==argc ){
    return FTCX__METHOD_NAME(xDbSelf)(cx, tcl, argc, argv);
  }else{
    ftcl_cx * const fx = cx;
    int const isFsl = ftcl_typeid_fsl()==fx->cmdo.type;
    return ftcl__cx_dispatch( cx, 1, argc, argv,
                            isFsl ? nSubFsl : nSubDb,
                              isFsl ? aSubFsl : aSubDb,
                              1);
  }
}

static int ftcl__cx_DbCmdAdaptor(
  ClientData cd,
  Tcl_Interp *tcl,
  int argc,
  Tcl_Obj * const * argv
){
  return Tcl_NRCallObjProc(tcl, ftcl__cx_xDb, cd, argc, argv);
}

ftcl_cx * ftcl__db_cx_create(
  Tcl_Interp *tcl, fsl_db * db, Tcl_Obj * const tVarName
){
  static unsigned counter = 0;
  ftcl_cx * fx = ftcl_cx_alloc(tcl, ftcl_typeid_db(), db);
  char * const z = fsl_mprintf("fsl_db#%u", ++counter);
  assert( db == fx->sub );
  assert( db == ftcl_cx_db(fx) );
  if( th_cmdo_plugin(&fx->cmdo, ftcl__cx_DbCmdAdaptor,
                      ftcl__cx_xDb, z, tVarName) ){
    ftcl_cx_unref(fx);
    fx = 0;
  }else{
    assert( 1 == fx->cmdo.refCount );
  }
  fsl_free(z);
  return fx;
}

void ftcl__dump_sizeofs_db(void){
#define SO(T)                                   \
  printf("sizeof(%s)=%u\n", #T, (unsigned)sizeof(T))
  SO(QueryState);
#undef SO
}

#if 0
/*
** A flag for sqlite3_prepare_multi() which tells it that the callback
** function has taken ownership of the statement and
** sqlite3_prepare_multi() must not finalize it.
*/
#define SQLITE_PREPARE_MULTI_TAKE 0x01

/*
** Callback typedef for use with sqlite3_prepare_multi(). Implementations
** are passed the current db and a freshly-prepared sqlite3_stmt
** object, plus the a copy of the pointer which was passed as the 5th
** argument to sqlite3_prepare_multi().
**
** Implementations must "process" the statement, performing any legal
** operations. By default, sqlite3_prepare_multi() will take over
** ownership of the statement, so callbacks must not pass it to
** sqlite3_finalize(). However, if the callback sets the
** SQLITE_PREPARE_MULTI_TAKE bit via the *pOut then ownership of the
** statement object is transferred to the callback. If the callback
** does not set this bit on *pOut, invoking sqlite3_finalize() from a
** callback will result in Undefined Behavior.
**
** Implementations must not keep a copy of the db, stmt, or pOut
** pointers - they may expire after any call to this function. The
** lifetime of the clientState pointer is managed by the client.
**
** Implementations must return 0 on success, else one of the
** SQLITE_... result codes.  If SQLITE_DONE is returned, the calling
** sqlite3_prepare_multi() will stop processing its input SQL and
** return 0 to its caller. If the callback returns any other non-0
** result, it aborts the loop and returns that result to the caller.
*/
typedef int (*sqlite3_prepare_multi_f)(sqlite3 *db, sqlite3_stmt *stmt,
                                       unsigned int *pOut, void * clientState);

/*
** For each SQL statement in zSql, this function prepares a new
** sqlite3_stmt object and passes it to the given callback.
**
** The 1st, 2nd, and 3rd arguments are as documented for
** sqlite3_prepare(), and the 4th as documented for preparation flags
** in sqlite3_prepare_v3().
**
** See sqlite3_prepare_multi_f for the semantics of the 6th and 7th
** parameters, as well as the semantics for its result value. The
** opaque context pointer specified for the 7th parameter is passed as
** the final argument of the callback.
**
** If cb is NULL, this function prepares each statement but does not
** execute it. That can be used to determine whether or not
** sqlite3_prepare() (or equivalent) can process the input (but not
** whether it's useful to do so - inputs which are empty or contain
** only SQL comments are also legal).
**
** Empty input (NULL or consisting only of whitespace or SQL comments) is
** not an error.
**
** Returns 0 on success or propagates a non-0 result on error.
*/
int sqlite3_prepare_multi(sqlite3 * const db, char const * const zSql, int nSql,
                          unsigned int prepareFlags, unsigned int *pOut,
                          sqlite3_prepare_multi_f callback, void * const clientState);

/*
** A sqlite3_prepare_multi_f() implementation which repeatedly calls
** sqlite3_step() on q until it returns any value other than
** SQLITE_ROW.  It returns 0 if no errors are encountered in stepping,
** else it propagates the error result from a failed sqlite3_step().
** This implementation does not use its 3rd and 4th arguments, so the
** corresponding sqlite3_prepare_multi() call's final argument may be
** NULL.
*/
int sqlite3_prepare_multi_f_stepall(sqlite3 *db, sqlite3_stmt *q, unsigned int *pOut,
                                    void * clientState);

#include <ctype.h>
int sqlite3_prepare_multi_f_stepall(sqlite3 *db, sqlite3_stmt *q,
                                    unsigned int *pOut, void * clientState){
  int rc = 0;
  (void)db;
  (void)pOut;
  (void)clientState;
  while( (rc = sqlite3_step(q)) == SQLITE_ROW ) {}
  return SQLITE_DONE==rc ? 0 : rc;
}

int sqlite3_prepare_multi(sqlite3 * const db, char const * const zSql, int nSql,
                          unsigned int prepFlags, unsigned int *pOut,
                          sqlite3_prepare_multi_f cb, void * const clientData){
  sqlite3_stmt * q = 0;
  char const * zPos = zSql;
  char const * zEnd;
  int rc = 0;
  if( nSql<0 ) nSql = zSql ? (int)strlen/*sqlite3Strlen30()*/(zSql) : 0;
  if( nSql <=0 ) return 0;
  zEnd = zPos ? (zPos + nSql) : 0;
  while( zPos<zEnd ){
    while( *zPos && zPos<zEnd && isspace/*sqlite3Isspace*/(*zPos) ) {
      /*solely cosmetic: trim leading space*/
      ++zPos;
    }
    if( zPos>=zEnd ) break;
    sqlite3_finalize(q);
    q = 0;
    rc = sqlite3_prepare_v3(db, zPos, (int)(zEnd - zPos), prepFlags, &q, &zPos);
    if( 0!=rc ) break;
    else if(!q){
      /* Empty statement/whitespace */
      continue;
    }else if(cb) {
      unsigned int pOut = 0;
      rc = cb(db, q, &pOut, clientData);
      if( SQLITE_PREPARE_MULTI_TAKE & pOut ){
        q = 0 /* ownership transferred - do not finalize */;
      }
      if( rc ){
        if( SQLITE_DONE==rc ) rc = 0;
        break;
      }
    }
  }
  sqlite3_finalize(q);
  return rc;
}
#endif /* if 0|1 */

#undef MARKER
