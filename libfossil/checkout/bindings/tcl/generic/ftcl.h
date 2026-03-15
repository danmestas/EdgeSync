#ifndef NET_FOSSILSCM_TCLFOSSIL_H_INCLUDED
#define NET_FOSSILSCM_TCLFOSSIL_H_INCLUDED
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


/**
   These are the public-facing pieces of the ftcl Tcl binding of
   libfossil.

   APIs which use two consecutive underscores in their names are
   considered internal, and are only exposed here to simplify access
   to them from this project's various source files.
*/

#include "libfossil.h"
#include <tcl.h>
#include <stdarg.h>
#include "th.h"

#if defined(__cplusplus)
extern "C" {
#endif

#ifndef Tcl_BounceRefCount
# define Tcl_BounceRefCount(X) \
  if(X) {Tcl_IncrRefCount(X); Tcl_DecrRefCount(X);} (void)0
   /* https://www.tcl-lang.org/man/tcl9.0/TclLib/Object.html */
#endif

/**
   Sets the tcl result to the error state from f. It returns TCL_ERROR
   if either fslrc is non-0 or f's error state is non-0. The intent is
   that this be called immediately after a fsl_cx operation which is
   documented as updating its error state.
*/
int ftcl_rs_fsl_cx(Tcl_Interp *tcl, fsl_cx * f, int fslrc);

// TODO: fetch either the fsl_cx error or fsl_db error,
// depending on fx->cmdo.type.
//int ftcl_rs_cx(ftcl_cx * fx, int fslrc);

/**
   Sets the tcl result to the error state from db, if any. It returns
   TCL_ERROR if either fslrc is non-0 or db's error state is non-0. The
   intent is that this be called immediately after an sqlite3
   operation which is documented as updating its error state.
*/
int ftcl_rs_sqlite3(sqlite3 * db, Tcl_Interp *tcl, int triggeringRc);

/**
   Works like ftcl_rs_sqlite3() except that it prefers any error state
   from fsl_db_err_get(), if available, before falling back to
   ftcl_rs_sqlite3(). The intent is that this be called immediately
   after a fsl_db operation which is documented as updating its error
   state.
*/
int ftcl_rs_db(Tcl_Interp *tcl, fsl_db * db, int triggeringRc);

/** Sets the result string to a copy of z then frees z (which must
    have been allocated via a libfossil API, e.g. fsl_mprintf() or via
    the fsl_buffer class. Returns rc. */
int ftcl_rs_take(Tcl_Interp *tcl, int rc, char * z);
/**
   Uses libfossil's string-formatting API to format a string
   which is set as the Tcl result value. Returns rc.
 */
int ftcl_rs_v(Tcl_Interp *tcl, int rc, char const * zFmt, va_list vargs );
/**
   Elipses counterpart of ftcl_rs_v().
*/
int ftcl_rs(Tcl_Interp *tcl, int rc, char const * zFmt, ...);

/** Convenience form of ftcl_rs(tcl, TCL_ERROR, zFmt, ...). */
int ftcl_err(Tcl_Interp *tcl, char const * zFmt, ...);

/**
   Flags for use with ftcl_cx::flags.
*/
enum ftcl_cx_e {
  /* Indicates that this ftcl_cx instance wraps the fcli fsl_cx. */
  ftcl__F_CX_IS_FCLI = 0x01
};

/**
   Generic finalizer interface used in several places under different
   names.
*/
typedef void (*ftcl_finalizer_f)(void *);

/**
   This is a scratchpad for potential future changes. This
   type is not currently used.
*/
struct ftcl_db {
  /**
     Underlying native object.
  */
  fsl_db db;

  /**
     The interpreter instance which instantiated this object.
  */
  Tcl_Interp * tcl;
  /**
     The Command object for this instance.
  */
  Tcl_CmdDeleteProc *pCmd;
  /**
     If installed with a var name, this object owns this copy of that
     name. The value of that name (i.e. the name of the installed
     proc) is available via Tcl_GetCommandName(this->tcl, this->pCmd).
  */
  char * zVarName;
  /** Mask of ... something. */
  unsigned char flags;

  /**
     String representation of SQL NULL. Default is an empty string.
  */
  struct {
    /** Memory owned by this object. */
    char * z;
    /** strlen() of this->z */
    unsigned int n;
  } null;
};

typedef struct ftcl_db ftcl_db;

/**
   Each fossil instance gets one of these. This is exposed here only
   for the sake of other ftcl-internal pieces which live in
   disparate files. Clients must treat this as opaque.

   Design note: this type is used for two disparate purposes (fossil
   instances and standalone db instances). It "really should" be split
   into separate fossil-centric objects and db-centric types, but they
   aren't "for historical reasons." The two would have a lot in
   common.
*/
struct ftcl_cx {
  /**
     cmdo.p == this
     cmdo.type == type of this->sub
     cmdo.dor = th_finalizer_f_ftcl_cx

     This MUST be the first struct member.
  */
  th_cmdo cmdo;
  /** A fsl_cx or a fsl_db */
  void * sub;
  struct {
    /** Mask of ftcl_cx_e values. */
    unsigned char client;
    unsigned char internal;
    /** If true, maybe emit some debugging info. */
    unsigned char verbose;
    /**
       A workaround for fsl_cx-managed db commands, to keep the
       (fossil close) command from closing the db.
    */
    unsigned int nKeepOpen;
  } flags;
  /**
     String representation of SQL NULL.
  */
  struct {
    /** Memory owned by this object. */
    char * z;
    /** strlen() of this->z */
    unsigned int n;
    /** Tcl form of this->z, to avoid having to make copies in some
        places. */
    Tcl_Obj * t;
  } null;
  struct {
    /** Key for db name in function metadata */
    Tcl_Obj * tKeyDb;
    /** Key for db aggregate context key in function metadata */
    Tcl_Obj * tKeyAggCx;
    /** The word "apply" which gets added in some callback
        contexts. */
    Tcl_Obj * tApply;
    /** An empty string. */
    Tcl_Obj * tEmpty;
    /** The string "*". */
    Tcl_Obj * tStar;
    /** The string "fossil". */
    Tcl_Obj * tFossil;
  } cache;
  /** A linked list of queries currently using this
      object's fsl_cx. If this context is closed,
      each of them gets its statement invalidated to avoid
      stepping on a stale db pointer later. */
  void * pQueries;
};

/**
   Convenience typedef.

   Each ftcl_cx is a wrapper object for a fsl_cx instance and related
   state. They are created via the "open" subcommand of this
   extension's main command object.
*/
typedef struct ftcl_cx ftcl_cx;

/**
   Sets the result to a "wrong arg count" error for command argv[0] and returns
   TCL_ERROR.
*/
int ftcl_rs_argc(Tcl_Interp *tcl, Tcl_Obj * const * argv,
                 const char *zUsage);

/**
   Uses fsl_buffer_fill_from_filename() to populate b from the given
   file. Returns 0 on success. On error, tcl's result is set to a
   descriptive error string and TCL_ERROR is returned.
*/
int ftcl_file_read(Tcl_Interp *tcl, char const * zFile, fsl_buffer * b);

/**
   Callback for use with ftcl_db_foreach(). It gets passed the 4th,
   1st, and 3rd argument to that function, plus the statement being
   processed and a sequential ID of how many times ftcl_db_foreach()
   has called this callback in this particular invocation of
   ftcl_db_foreach(), starting with 0.

   Implementations must do any processing of the given statement
   (e.g. stepping all of its rows) _except_ for fsl_stmt_finalize(),
   which ftcl_db_foreach() function will do.
*/
typedef int (*ftcl_db_foreach_f)(void * const cx, Tcl_Interp * const tcl,
                                 fsl_stmt * const stmt, unsigned id);

/**
   For each SQL statement in zSql, this function prepares a new
   fsl_stmt object and passes it to cb. nSql is the strlen() of
   zSql. If it is negative, fsl_strlen() will be used to calculate it.

   If cb returns TCL_BREAK, iteration over the SQL stops without an
   error, else if f returns non-0, that becomes the result of this
   function. On a db-related error, tcl will be updated with an error
   message from the db.

   If cb is NULL, this function prepares each statement but does not
   execute it.

   The opaque context pointer cx is passed as the first argument
   of the callback.

   Returns 0 on success, TCL_ERROR on error (or propagates some other
   value from f.
*/
int ftcl_db_foreach(Tcl_Interp * const tcl, fsl_db * const db,
                    ftcl_db_foreach_f cb, void * const cx,
                    char const * const zSql, Tcl_Size nSql);

/**
   A ftcl_db_foreach_f impl which simply calls fsl_stmt_step() on the
   statement until it returns something other than FSL_RC_ROW, and
   returns non-0 if any value other than one of (FSL_RC_ROW,
   FSL_RC_DONE) are returned from fsl_stmt_step() (in which case it
   sets tcl's result to an error).

   This implementation does not use its cx pointer, so the 4th
   argument to calls to its corresponding ftcl_db_foreach() may be
   NULL.
*/
int ftcl_db_foreach_f_stepall(void * const cx, Tcl_Interp * const tcl,
                              fsl_stmt * const q, unsigned id);

/**
   If fx has an opened db, 0 is returned, else the tcl result
   is set to a descriptive error message and TCL_ERROR is
   returned.
*/
int ftcl_needs_db(ftcl_cx * const fx);

#if 0
/**
   If fx has an opened db handle, returns 0, else:

   If zErrIfNot then fx->tcl's result state is set to that error
   string. TCL_ERROR is returned.
*/
int ftcl_affirm_db_is_open(ftcl_cx * const fx);
#define ftcl__affirm_repo_open \
  if( 0!=(rc=ftcl_affirm_db_is_open(fx)) ) goto end
#endif

/**
   Allocator for ftcl_cx instances. The 2nd and 3rd args may be NULL,
   but only in limited circumstances.  The returned object has a
   refcount of 0.  Use ftcl_cx_ref() and ftcl_cx_unref() to manage
   its lifetime.
*/
ftcl_cx * ftcl_cx_alloc(Tcl_Interp *tcl, void const * type,
                         void * p);

/**
   If fx has a var associated with it, this unsets that var.  It also
   releases one refcount point from fx (which may free fx).
*/
void ftcl_cx_unplug(ftcl_cx *fx);
ftcl_cx * ftcl_cx_ref(ftcl_cx *fx);
void ftcl_cx_unref(ftcl_cx *fx);

/**
   Looks for a subcommand in argv[startAtArgc] and dispatches it, else
   returns TCL_ERROR and updates the tcl result.
*/
int ftcl__cx_dispatch( ftcl_cx *fx, int startAtArgc,
                       int argc, Tcl_Obj * const * argv,
                       unsigned nCmd, th_subcmd const * const aCmd,
                       int isSorted );

/** Proxy for th_decl_cmdflags which injects a -? flag named fHelp
    and sets VARNAME.xResult to ftcl_rs. */
#define ftcl_decl_cmdflags(VARNAME,OPTIONS) \
  th_decl_cmdflag("-?", 0, 0, fHelp);       \
  th_decl_cmdflags(VARNAME,OPTIONS);        \
  VARNAME.xResult = ftcl_rs


/** Internal helper to check for a -? help flag. */
#define ftcl__check_help(TCL,OBJ,FLAG,FP,USAGE)              \
  rc = th_check_help(TCL, OBJ, FLAG, FP, USAGE);            \
  if( rc ){ if(TCL_BREAK==rc){ rc = 0; } goto end; } (void)0

/** Internal helper to do common flags-parsing work. */
#define ftcl__cx_flags_parse(FP,USAGE)                   \
  rc = ftcl__flags_parse(tcl, argc, argv, FP);           \
  if(rc) goto end;                                       \
  ftcl__check_help(tcl, argv[0], &fHelp, FP, USAGE)

/**
   Proxy for th_flags_parse() which uses a cache of memory for use by
   p. Calling this obligates the caller, regardless of success or
   failure, to eventually pass p to ftcl__flags_cleanup().
*/
int ftcl__flags_parse(Tcl_Interp *tcl, int argc,
                      Tcl_Obj *const* argv, th_fp * fp);

/**
   Cleanup counterpart of ftcl__flags_parse().
*/
void ftcl__flags_cleanup(th_fp * fp);

/** Internal helper to standardized param names and allow easy ctrl-s
    (emacs) navigation around these functions. */
#define FTCX__METHOD_NAME(NAME) ftcl__cx_ ## NAME
#define FTCX__METHOD_DECL(NAME)                                              \
  int FTCX__METHOD_NAME(NAME) (void *cx, Tcl_Interp *tcl, int argc, Tcl_Obj *const* argv)

/**
   SQL binding types to supplement SQLITE_TEXT and friends.
*/
enum ftcl__SQL_TYPE_e {
  /**
     Indicates that a value should be SQL-bound as NULL
     if it's an empty string.
  */
  ftcl__SQL_TYPE_NULLABLE = SQLITE_NULL * 2,
  /**
     Indicates that we should guess the value's type.
  */
  ftcl__SQL_TYPE_GUESS = -1
};

/**
   A th_cmdo.type ID for fsl_cx-type objects.
*/
void const * ftcl_typeid_fsl(void);

/**
   A th_cmdo.type ID for db connection objects.
*/
void const * ftcl_typeid_db(void);

/**
   If fx has a fsl_cx then it is returned, else NULL is returned.
*/
fsl_cx * ftcl_cx_fsl(ftcl_cx * const fx);

/**
   If is-a db connection, it is returned, else if it's a fsl_cx and
   that object has a db, that is returned, else NULL is returned. */
fsl_db * ftcl_cx_db(ftcl_cx * const fx);

/**
   realloc() proxy which (A) uses fsl_realloc() and (B) uses a size_t
   2nd arg type so that it matches more conventional allocator
   requirements. It is likely configured to abort() on OOM.
*/
void * ftcl_realloc(void *p, size_t n);
/**
   free() proxy for fsl_free().
*/
void ftcl_free(void *p);

void ftcl_mutex_enter(void);
void ftcl_mutex_leave(void);

/**
   Returns a pointer to fx's cached copy of the word "apply".
*/
Tcl_Obj * ftcl_cx_apply(ftcl_cx * fx);

/**
   If fx has an opened fossil checkout, scan it for changes.
   returns a Tcl non-0 on error, including not having an
   opened checkout, and populates fx->cmdo.tcl's result
   with the error info.
*/
int ftcl_cx_rescan(ftcl_cx *fx);

/** The 3 types of SQLite User-defined Functions (UDFs). */
enum ftcl__udf_type_e {
  /** Sentinel value. Must be 0. */
  ftcl__udf_TYPE_INVALID = 0,
  ftcl__udf_TYPE_SCALAR,
  ftcl__udf_TYPE_AGGREGATE,
  ftcl__udf_TYPE_WINDOW
};

/**
   Flags for use with ftcl__udf::flags.
*/
enum ftcl__udf_e {
  /**
     Tells the UDF code that when numeric type is specifically
     requested and result values are invalid numbers, they are to be
     treated as zero instead of the default of SQL NULL.
  */
  ftcl__udf_F_BADNUM_ZERO = 0x01,

  /**
     If the UDF returns an empty value, treat it as SQL NULL.
  */
  ftcl__udf_F_EMPTY_AS_NULL = 0x02
};

/**
   State for a single Tcl binding of a single callback (lambda) for an
   SQL UDF.
*/
struct ftcl__udf_lambda {
  /** This func's Tcl impl (a lambda expression). This object owns
      it. */
  Tcl_Obj * o;
  /** Minimum number of args, or negative for any number. */
  short arityMin;
  /** Maximum number of args, or negative for any number. */
  short arityMax;
};
typedef struct ftcl__udf_lambda ftcl__udf_lambda;

/**
   State for a Tcl-side SQLite UDF (User-defined [SQL] Function).
*/
struct ftcl__udf {
  /**
     The fsl_db or fsl_cx instance this UDF is bound to.
  */
  ftcl_cx * fx;
  /** This func's SQL name. Used in error reporting. */
  Tcl_Obj * oName;
  /** Failed experiment. Might revisit later.
      Name of this UDF's call-local metadata array/object. */
  Tcl_Obj * tMetaName;
  enum ftcl__udf_type_e eType;
  /** From the ftcl__SQL_TYPE_e enum or one of the SQLITE_... type
      values. */
  short sqlResultType;
  /** Flags from the ftcl__udf_e enum. */
  unsigned short flags;
  /**
     Bindings to Tcl impls of SQLite's various UDF callbacks.
  */
  struct {
    /* Scalar. */
    ftcl__udf_lambda xFunc;
    /* Aggregate, window. */
    ftcl__udf_lambda xStep;
    /* Aggregate, window. */
    ftcl__udf_lambda xFinal;
    /* Window. */
    ftcl__udf_lambda xValue;
    /* Window. */
    ftcl__udf_lambda xInverse;
  } func;
#if 0
  struct {
    /** Current sqlite3 context of the currently-running method of
        this object */
    sqlite3_context * cx3;
    /** Currently-running method of this UDF. */
    //ftcl__udf_lambda const * xRunning;
  } sqlite;
#endif
#if 0
  /* This wouldn't actually save us more than
     sizeof(ftcl__udf_lambda)-sizeof(someTypeIdFlag), so something
     like 2 words. */
  union {
    /* We don't need to tag the type because the context
       makes it clear which entry to use. */
    struct {
      ftcl__udf_lambda xFunc;
    } scalar;
    struct {
      ftcl__udf_lambda xStep;
      ftcl__udf_lambda xFinal;
    } aggregate;
    struct {
      ftcl__udf_lambda xStep;
      ftcl__udf_lambda xFinal;
      ftcl__udf_lambda xValue;
      ftcl__udf_lambda xInverse;
    } window;
  } f;
#endif
};
typedef struct ftcl__udf ftcl__udf;

/**
   Helper macros (again) for setting up, parsing, and cleaning up
   command flags.
*/
#define ftcl_fp_init_(FPVAR,LIMIT)                 \
  enum { _max_ ## FPVAR = LIMIT };                 \
  th_cmdflag _a ## FPVAR[_max_ ## FPVAR] = {{0}};  \
  th_cmdflag * _ap ## FPVAR[_max_ ## FPVAR] = {0}; \
  th_fp FPVAR = th_fp_empty; \
  memset(&_a ## FPVAR, 0, sizeof(_a ## FPVAR));    \
  FPVAR.pFlags = _ap ## FPVAR

#define ftcl_fp_flag_(FPVAR, FLAG, USAGE, OPTIONS, FVAR)        \
  assert( FPVAR.nFlags < _max_ ## FPVAR );                      \
  th_cmdflag * const FVAR = &_a ## FPVAR[FPVAR.nFlags];         \
  _ap ## FPVAR[FPVAR.nFlags++] = FVAR;                          \
  FVAR->zFlag = FLAG; FVAR->nFlag = sizeof(FLAG)-1;             \
  FVAR->pVal = 0; FVAR->options = OPTIONS;                      \
  FVAR->zUsage = USAGE; FVAR->nSeen = 0;                        \
  FVAR->consumes = (FLAG[1]=='-' ? (FLAG[2]=='-' ? \
                                    (FLAG[3]=='-' ? (FLAG[4]=='-' ? 4 : 3) : 2) : 1) : 0)
  //doesn't work: *FVAR = th_cmdflag_initFUO(FLAG,USAGE,OPTIONS)

#define ftcl_fp_parse_(FPVAR, USAGE)                              \
  ftcl_fp_flag("-?", 0, 0, fHelp);                                \
  rc = ftcl__flags_parse(tcl, argc, argv, &FPVAR);                \
  if(rc) goto end;                                                \
  ftcl__check_help(tcl, argv[0], fHelp, &FPVAR, USAGE)

#define ftcl_fp_cleanup_(FPVAR) ftcl__flags_cleanup(&FPVAR)

/** th_fp-type var name used by the ftcl_fp_... macros. */
#define ftcl_fp_VarName fp
/** th_cmdflags[] size used by the ftcl_fp_... macros. */
#define ftcl_fp_FlagLimit 16

#define ftcl_fp_init ftcl_fp_init_(ftcl_fp_VarName,ftcl_fp_FlagLimit)
#define ftcl_fp_flag(FLAG, USAGE, OPTIONS, FVAR)  \
  ftcl_fp_flag_(ftcl_fp_VarName, FLAG, USAGE, OPTIONS, FVAR)
#define ftcl_fp_parse(USAGE) ftcl_fp_parse_(ftcl_fp_VarName, USAGE)
#define ftcl_fp_cleanup ftcl_fp_cleanup_(ftcl_fp_VarName)

#define ftcl_cmd_replace_argv                    \
  Tcl_Obj* const *origArgv = argv; (void)origArgv; \
  int const origArgc = argc; (void)origArgc;       \
  argc=th_flags_argc(&ftcl_fp_VarName);            \
  argv=th_flags_argv(&ftcl_fp_VarName)

#define ftcl_cmd_rcfx int rc = 0; ftcl_cx * const fx = cx; (void)fx
#define ftcl_cmd_init_(FLAG_LIMIT)  \
  ftcl_fp_init_(ftcl_fp_VarName, FLAG_LIMIT)

#define ftcl_cmd_init ftcl_cmd_init_(ftcl_fp_FlagLimit)
#define ftcl_cmd_cleanup ftcl_fp_cleanup_(ftcl_fp_VarName)

#define ftcl__db_open_check \
  if( ftcl_needs_db(fx) ) {rc = TCL_ERROR; goto end;} (void)0

#if defined(__cplusplus)
} /*extern "C"*/
#endif
#endif /* NET_FOSSILSCM_TCLFOSSIL_H_INCLUDED */
