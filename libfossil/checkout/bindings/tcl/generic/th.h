#ifndef NET_WANDERINGHORSE_TH_H_INCLUDED
#define NET_WANDERINGHORSE_TH_H_INCLUDED
/*
** 2025 May 20
**
** The author disclaims copyright to this source code.  In place of a
** legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
************************************************************************
**
** This file houses utility code for working with the Tcl C APIs. It's
** intended to be project-agnostic (but also serve my own projects'
** needs). Its API prefix is "th", short for "Tcl Helpers".
**
*/

#include <stdarg.h>
#include <stdbool.h>
#include <tcl.h>

#if TCL_MAJOR_VERSION<9 && !defined(Tcl_Size)
typedef int Tcl_Size;
#endif

#if defined(__cplusplus)
extern "C" {
#endif

#define th_gs Tcl_GetStringFromObj
#define th_gs1(X) Tcl_GetStringFromObj((X), NULL)
#define th_ref(X) ((X) ? (Tcl_IncrRefCount(X),(X)) : 0)
#define th_unref_c(X) if(X) Tcl_DecrRefCount(X)
#define th_unref(X) th_unref_c(X); X = 0
#define th_bounce_c(X) if(X) {Tcl_IncrRefCount(X); Tcl_DecrRefCount(X);} (void)0
#define th_bounce(X) th_bounce_c(X); X = 0
#define th_nso Tcl_NewStringObj
#define th_nlo Tcl_NewListObj
#define th_loae Tcl_ListObjAppendElement

/**
   For use with th_cmdflag::options.
*/
enum th_cmdflag_e {
  /**
     Indicates that this flag may be used multiple times but that only
     the last one provided is retained.
  */
  th_F_CMDFLAG_LAST_WINS = 0x01,

  /**
     Indicates that the flag may be specified multiple times and that
     the result should be set as a list object in th_cmdflag::pVal.
     Each use of the flag appends another entry to that list.
  */
  th_F_CMDFLAG_LIST     = 0x02
};

/**
   Utility for use in processing tcl proc flags from Tcl extension
   functions.
*/
struct th_cmdflag {
  /**
     Flag name. By convention of this framework, flags consume a
     number of following arguments equal to their dash count minus 1.
     Clients may override that by manipulating this->consumes.
  */
  char const *zFlag;

  /** strlen() of zFlag */
  unsigned nFlag;

  /** Brief usage text. May be NULL. */
  char const *zUsage;

  /**
     If this his flag consumes an argument, this holds that argument.
     If it's a non-consuming flag, the flag argument itself gets set
     here.

     Always use th_cmdflag_set() to set this or clear this, as it
     manages refcounts in a way compatible with
     th_flags_parse_cleanup().

     It is legal to pre-set this to a value, using
     th_cmdflag_set(), before calling th_parse_flags(), in
     which case the flag will have a default value if the flag is not
     passed to the function whose flags are being parsed.
  */
  Tcl_Obj * pVal;

  /** Mask of th_cmdflag_e flags */
  unsigned short options;

  /** How many times it was seen in parsing. */
  unsigned short nSeen;

  /**
     The number of arguments this flag consumes.
  */
  unsigned char consumes;
};

/** Convenience typedef. */
typedef struct th_cmdflag th_cmdflag;

#define th_cmdflag_empty_m {0,0,0,0,0,0}
extern const th_cmdflag th_cmdflag_empty;

/**
   Evaluates to a struct initializer for a th_cmdflag object with the
   given flag name and options. If FNAME starts with two dashes then
   its consume member is set to 1. If it has three dashes, it's set to
   2.
*/
#define th_cmdflag_initFUO(FNAME,USAGE,OPTIONS) { \
  .zFlag = FNAME, .nFlag = sizeof(FNAME)-1,       \
  .zUsage = USAGE, .pVal = NULL,                  \
  .options = (OPTIONS),                           \
  .nSeen = 0U,                                    \
  .consumes =                                     \
  (FNAME[1]=='-' ? (FNAME[2]=='-' ? 2 : 1) : 0)   \
}
#define th_cmdflag_initFO(FNAME, OPTIONS) \
  th_cmdflag_initFO(FNAME,NULL,OPTIONS)

/**
   Assigns p->pVal, decrementing any prior value's reference count and
   incrementing v's (if v is not NULL, which it may be).
*/
void th_cmdflag_set(th_cmdflag * const p, Tcl_Obj *v);

/**
   Returns a value from f. If at<0 then f->pVal is returned, else
   f->pVal is assumed to be a list and at'th element of that list is
   returned (or NULL if at is out of range).
*/
Tcl_Obj * th_cmdflag_get( th_cmdflag const * f, int at );

/**
   To redefine the general-purpose allocator this API uses, define
   TH_F_FREE and TH_F_REALLOC to the names of functions compatible
   with free(3) and realloc(3), respectively.
*/
void th_free(void *);

/**
   See th_free().

   If built with TH_OOM_ABORT defined to a non-0 integer, th_realloc()
   will abort() if a (re)allocation fails.
*/
void * th_realloc(void *, size_t n);

/**
   Unsigned int type used by th_list for holding sizes and capacities.
*/
typedef unsigned int th_size_t;

/**
   A very basic generic list type. In this API it's almost exclusively
   used for holding (Tcl_Obj*), but the interface is type-agnostic.
*/
struct th_list {
  /**
     Array of entries. It contains this->capacity entries,
     this->count of which are "valid" (in use).
  */
  void ** list;
  /**
     Number of "used" entries in the list.
  */
  th_size_t count;
  /**
     Number of slots allocated in this->list. Use th_list_reserve()
     to modify this. Doing so might move the this->list pointer but
     the values it points to will stay stable.
  */
  th_size_t capacity;
};
typedef struct th_list th_list;

/** A th_list initializer with all values set to 0. */
#define th_list_empty_m { NULL, 0, 0 }

/** A th_list instance with all values set to 0. Use this to ensure
    that new instances have a clean slate. */
extern const th_list th_list_empty;

/**
   Possibly reallocates self->list, changing its size. This function
   ensures that self->list has at least n entries. If n is 0 then
   the list is deallocated (but the self object is not), BUT THIS
   DOES NOT DO ANY TYPE-SPECIFIC CLEANUP of the items. If n is less
   than or equal to self->capacity then there are no side effects. If
   n is greater than self->capacity, self->list is reallocated and
   self->capacity is adjusted to be at least n (it might be bigger -
   this function may pre-allocate a larger value).

   Passing an n of 0 when self->capacity is 0 is a no-op.

   Newly-allocated slots will be initialized with NULL pointers.

   Returns 0 on success, TCL_ERROR on reallocation error.

   Results are undefined if self was not cleanly initialized before
   passing it here. Copy th_list_empty or (depending on the context)
   th_list_empty_m to cleanly initialize th_list instances.
*/
int th_list_reserve( th_list * const self, th_size_t n );

/**
   Appends a bitwise copy of cp to self->list, expanding the list as
   necessary and adjusting self->used.

   Ownership of cp is unchanged by this call. cp may not be NULL.

   Returns 0 on success, non-0 on allocation error.
*/
int th_list_append( th_list * const self, void* cp );

/**
   Flags for use with th_fp::options.
*/
enum th_flags_parse_e {
  /**
     Tells th_flags_parse() to not stop parsing flags when it
     encounters a non-flag.
  */
  th_F_PARSE_CHECK_ALL = 0x01,
  /**
     Tells th_flags_parse() not to accumulate non-flags in
     th_fp::nonFlag.
  */
  th_F_PARSE_NO_COLLECT = 0x02
};

/**
   Callback type for error logging. Some pieces of this API emit error
   results through these. It calls them, passing the interpreter, the
   triggering error code (from one of TCL's, e.g. TCL_ERROR),
   and a printf-formatted string.

   Abstractly, implementations must set tcl's result value to a string
   using the printf-formatted one provided. It must then return rc. It
   is legal to return another, transformed code provided the client is
   ready to accept arbitrary codes.
*/
typedef int (*th_result_set_f)(Tcl_Interp *tcl, int rc, char const * zFmt, ...);

/**
   A th_result_set_f() impl which uses vsnprintf() to format the
   result. If zFmt is NULL or "", this is equivalent to
   Tcl_ResetResult().
*/
int th_rs(Tcl_Interp *tcl, int rc, char const * zFmt, ...);

/** va_list variant of th_rs(). */
int th_rs_v(Tcl_Interp *tcl, int rc, char const * zFmt, va_list args);

/**
   Attempts to report, via Tcl_SetResult(), that at allocation error
   occurred at the given file and line. Returns TCL_ERROR.
*/
int th_rs_oom_at(Tcl_Interp *tcl, char const * zFile, int line );
#define th_rs_oom_here(TCL) th_rs_oom_at(TCL, __FILE__, __LINE__)

/**
   Sets the Tcl result string to a copy of z and returns rc. If z is
   NULL or empty, this is equivalent to Tcl_ResetResult().
*/
int th_rs_c(Tcl_Interp *tcl, int rc, char const * z);

static inline int th_rs_o(Tcl_Interp *tcl, int rc, Tcl_Obj *o){
  Tcl_SetObjResult(tcl, o);
  return rc;
}

/** Convenience form of th_rs(tcl, TCL_ERROR, zFmt, ...). */
int th_err(Tcl_Interp *tcl, char const * zFmt, ...);
int th_err_v(Tcl_Interp *tcl, char const * zFmt, va_list vargs);

/**
   Returns either a new int or wide-int value, depending on the
   value of v.
*/
Tcl_Obj * th_obj_for_int64(Tcl_WideInt v);

/**
   State for th_flags_parse().
*/
struct th_fp {
  /** Number of flags in this->pFlags. */
  unsigned int nFlags;

  /** All flags registered with this object. */
  th_cmdflag ** pFlags;

  /** Mask of th_flags_parse_e bits. */
  unsigned int options;

  /**
     Index in the original argv where the first "--" was seen, else a
     negative value.
  */
  int ddIndex;

  /** Number of distinct flags seen during th_flags_parse(). */
  unsigned int nSeen;

  /**
     Gets set by th_flags_parse() to the 0th argument in its argv
     (typically a (sub)command name). This object manages a reference
     point to this member.
  */
  Tcl_Obj * arg0;

  /**
     Non-flag arguments get placed in this unless this->options has
     the th_F_PARSE_NO_COLLECT flag. This object manages a reference
     count point to each (Tcl_Obj*) in the list.
  */
  th_list nonFlag;

  /**
     Optional output function to report th_flags_parse() errors to.
     Without this, good luck trying to decipher any errors that
     function throws.
  */
  th_result_set_f xResult;
};
/** Convenience typedef. */
typedef struct th_fp th_fp;

#define th_fp_empty_m {0U, 0, 0U, -1, 0U, 0, th_list_empty_m, th_rs}
extern const th_fp th_fp_empty;

#if 1
#define th_list_argv(LIST) (LIST)->count ? (Tcl_Obj**)(LIST)->list : NULL
#define th_list_argc(LIST) (int)(LIST)->count
#define th_flags_argv(FPPTR) th_list_argv(&(FPPTR)->nonFlag)
#define th_flags_argc(FPPTR) (FPPTR)->nonFlag.count
#define th_flags_argcv(FPPTR) th_flags_argc(FPPTR), th_flags_argv(FPPTR)
#else
/* Getting linking errors with inline. */
inline Tcl_Obj ** th_list_argv( th_list const * const li ){
  return li->count ? (Tcl_Obj**)li->list : NULL;
}

inline int th_list_argc( th_list const * const li ){
  return (int)li->count;
}

inline Tcl_Obj ** th_flags_argv( th_fp const * const fp ){
  return th_list_argv(&fp->nonFlag);
}
inline int th_flags_argc( th_fp const * const fp ){
  return (int)fp->nonFlag.count;
}
#endif

/**
   If fp has a no-flag argument at the given fp->nonFlag.list index,
   it's returned, else NULL is returned.
 */
Tcl_Obj * th_flags_arg( th_fp const * fp, unsigned at );

/**
   If th_flags_arg(fp,at) returns non-null, this function returns
   th_gs(thatObject,nOut), else it returns NULL. nOut may be NULL.
*/
char const * th_flags_arg_str( th_fp const * fp, unsigned at,
                                Tcl_Size *nOut );

/**
   If th_flags_arg(fp,at) returns non-null, the returns the result of
   calling Tcl_GetStringFromObj(thatPtr,nOut), else it returns NULL.
*/
char const * th_flags_arg_cstr( th_fp const * fp, unsigned at,
                                 Tcl_Size *nOut );

/**
  th_decl_cmdflags_init or init0 Must be used immediately before
  th_decl_cmdflag() and th_decl_cmdflags(), as it sets up state
  used by those macros.

  The 0-suffixed members of this family all have an ID argument, which
  is a context-specific name suffix to apply to the scope-local
  "private" var names. They are only needed if multiple
  fsl_flages_parse_t instances will be used in a given function.

  Example use:

  @code
  th_decl_cmdflags_init;
  th_decl_cmdflag("-noerr", 0, 0, fNoFail);
  th_decl_cmdflag("-rid",   0, 0, fRid);
  th_decl_cmdflags(fp, 0);
  int rc = th_flags_parse(tcl, argc, argv, &fp);
  ...
  th_flags_parse_cleanup(&fp); // even if th_flags_parse() fails!
  @endcode
*/
#define th_decl_cmdflags_init0(ID,LIMIT)                        \
  enum { NMaxFlags ## ID = LIMIT };                              \
  th_cmdflag * cmdflag__list ## ID [NMaxFlags ## ID + 1] = {0}; \
  unsigned cmdflag__list_pos ## ID = 0

/**
  Starts the process of setting up proc flags.
  See th_decl_cmdflags_init0() for example usage.
*/
#define th_decl_cmdflags_init \
  th_decl_cmdflags_init0(dflt, 16)

/** Internal detail. */
#define th__decl_cmdflag_push0(ID,VARNAME)             \
  assert(cmdflag__list_pos ## ID < NMaxFlags ## ID); \
  cmdflag__list ## ID [cmdflag__list_pos ## ID ++] = &VARNAME

/** Internal detail. */
#define th__decl_cmdflag_push(VARNAME) \
  th__decl_cmdflag_push0(dflt,VARNAME)

/**
   Declares a proc flag in the context of
   ctcl_decl_cmdflags_init0(ID), using the given scope-local var name
   for the th_cmdflag object and a bitmask of th_cmdflag_e values
   to configure the flag.
*/
#define th_decl_cmdflag0(ID,FLAG,USAGE,OPT,VARNAME)               \
  th_cmdflag VARNAME = th_cmdflag_initFUO(FLAG,USAGE,OPT); \
  th__decl_cmdflag_push0(ID,VARNAME)

/**
   Declares a proc flag in the context of ctcl_decl_cmdflags_init,
   using the given scope-local var name for the th_cmdflag object
   and a bitmask of th_cmdflag_e values to configure the flag.
*/
#define th_decl_cmdflag(FLAG,USAGE,OPT,VARNAME)        \
  th_decl_cmdflag0(dflt,FLAG,USAGE,OPT,VARNAME)

/**
   Set up a th_fp instance named VARNAME with the given
   th_fp::options, and wrapping all flags which were
   locally declared using th_decl_cmdflag0(ID,...).
*/
#define th_decl_cmdflags0(ID,VARNAME,OPTIONS)     \
  th_fp VARNAME =                      \
    {cmdflag__list_pos ## ID, cmdflag__list ## ID, \
     OPTIONS, -1, 0U, NULL, th_list_empty_m, th_rs}

/**
   Set up a th_fp instance named VARNAME with the given
   th_fp::options, and wrapping all flags which were
   locally declared using th_decl_cmdflag.
*/
#define th_decl_cmdflags(VARNAME,OPTIONS) \
  th_decl_cmdflags0(dflt,VARNAME,OPTIONS)

/**
   Equivalent to th_flags_parse_cleanup2(p,1).
*/
void th_flags_parse_cleanup(th_fp * const p);

/**
   Must be called to free up resources assigned via th_flags_parse()
   (which may happen even if it fails).  Because of the way this is
   typically used, p->pFlags and p->nFlags are left intact - they
   invariably refer to objects which are guaranteed to outlive p. It
   is legal to re-use p with th_flags_parse(), but this function must
   be called at some point after each run of that functions.

   The second argument should be 1 unless you know this object will be
   used again with th_flags_parse(), in which case keeping that
   memory around may save an allocation or two.
*/
void th_flags_parse_cleanup2(th_fp * const p, int freeListMem);

/**
   A helper for parsing flags from proc argv lists.

   Before starting work, it passes fp to
   th_flags_parse_cleanup2(p,0).

   It inspects each of argv, starting at element 1 (skipping the
   command name), looking for flags which match those defined in fp,
   and updates fp and/or the individual fp->pFlags and/or
   fp->nonFlag with the results.

   By default it stops looking at the first non-flag argument (that
   is, the first arg which is not in fp), but if fp->options has
   th_F_PARSE_CHECK_ALL then it will continue to traverse all of
   argv, even continuing past a "--" (but see below).

   All non-flag arguments are appended to fp->nonFlag in their
   original order, with the exception of the first instance of "--",
   which is unconditionally elided from the list. To avoid
   populating fp->nonFlag, add th_F_PARSE_NO_COLLECT to fp->options.

   If a "--" flag is encountered in argv then all further arguments
   are considered to be non-flags. The first "--" flag is elided from
   fp->nonFlag, but any further "--" flags are retained in that list. If
   "--" was encountered, fp->ddIndex will be set to the original argv
   index in which it was found. fp->ddIndex will be negative if "--"
   was never encountered.

   Regardless of whether this call succeeds or fails, the caller must
   eventually pass fp to th_flags_parse_cleanup() to clean it up.

   Returns 0 on success or one of the TCL_... values on error
   (hypothetically always TCL_ERROR).
*/
int th_flags_parse( Tcl_Interp *tcl,
                     int argc, Tcl_Obj * const * argv,
                     th_fp * const fp );

/**
   Emits a basic help message text from fp->pFlags to the given string
   buffer. Returns 0 on success, TCL_ERROR if the buffer is not large
   enough for the text. The output is intended to be appended to the
   corresponding command's name and looks something like:

   --foo value -bar -verbose*

   Where:

   - "value" comes from the flag's zUsage

   - "*" after a flag name (before its zUsage, if any), indicates that
     the flag has the th_F_CMDFLAG_LIST flag set (i.e. it can be used
     multiple times).
*/
int th_flags_generate_help(th_fp const * fp,
                           char * dest, size_t nDest);

/**
   If fHelp->nSeen is non-0 then...

   th_flags_generate_help() is used to generate help text from fp,
   then that text is sandwitched between cmd's string form and
   zNonFlags (which may be NULL).

   Returns 0 if !fHelp->nSeen, TCL_BREAK if !!fHelp->nSeen, TCL_ERROR
   on error (the help text was larger than some static internal buffer
   size). If it returns TCL_BREAK, it will have set the Tcl result to
   the help text string.
*/
int th_check_help(Tcl_Interp * const tcl,
                   Tcl_Obj * const cmd,
                   th_cmdflag const * const fHelp,
                   th_fp const * const fp,
                   char const *zNonFlags);


/** Dumps the given flag to stderr with an optional prefix string. */
void th_dump_cmdflag(
  char const * zPrefix,
  th_cmdflag const * const cf
);

/** Dumps the first n flags in aCF to stderr with an optional prefix
    string. */
void th_dump_cmdflags(
  char const * zPrefix,
  unsigned n,
  th_cmdflag * const * const aCF
);

/** Dumps all of fp's flags to stderr with an optional prefix
    string. */
void th_dump_flags(
  char const * zPrefix,
  Tcl_Interp *tcl,
  th_fp const * const fp
);

/** Dumps the first argc objects in argv to stderr with an optional
    prefix string. */
void th_dump_argv(
  char const * zPrefix,
  Tcl_Interp *tcl,
  int argc,
  Tcl_Obj * const * argv
);

/** Typedef for th_subcmd callbacks. */
typedef int (*th_subcmd_f)(void *, Tcl_Interp *, int argc, Tcl_Obj *const* argv);

/**
   State for organizing and working with subcommands.
*/
struct th_subcmd {
  /** Subcommand name. */
  char const *zName;
  /** strlen() of this->zName. We store this only because it can be
      used to speed up searching. */
  unsigned short nName;
  /** Flags for client use. */
  unsigned short flags;
  /** Callback impl. */
  th_subcmd_f xFunc;
};
/** Convenience typedef. */
typedef struct th_subcmd th_subcmd;

/**
   Resolves to a th_subcmd struct initializer with all of its fields
   set.
*/
#define th_subcmd_init(NAME, FUNC, FLAGS) \
  {NAME, sizeof(NAME)-1, FLAGS, FUNC}


/**
   Compares pArg against (up to) the first nCmd entries in aCmd.  On a
   match, the matching object is returned, else NULL is returned.

   If isSorted is true and nCmd is greater than some small number,
   it can do a faster search. Only pass 1 if aCmd is sorted by
   their zName field.
*/
th_subcmd const * th_subcmd_search( Tcl_Obj * const pArg,
                                    unsigned nCmd,
                                    th_subcmd const * aCmd,
                                    int isSorted);

/**
   Sorts the given array of nCmd entries on their zName entries.
*/
void th_subcmd_sort( unsigned nCmd, th_subcmd * aCmd );

/**
   Emits a list of all subcommands in the given aray to the given
   output buffer.  Returns 0 on success, TCL_ERROR if the buffer is
   not large enough for the text.
*/
int th_subcmd_generate_list(unsigned nSubs,
                             th_subcmd const * const aSubs,
                             char * dest, size_t nDest);

/**
   Uses th_subcmd_search() to search for a subcommand matching
   argv[0]. If no match is found, an error message is sent to xResult
   (if it's not NULL) describing the lack of a match, then it returns
   TCL_ERROR.  If it finds a match then it returns the result of
   calling theSubcmd->xFunc(), passing it the (cx, tcl, argc, argv)
   arguments.

   zUsage is an optional string to use in error reports. It may be
   NULL. If it is NULL then th_subcmd_generate_list() will be used on
   the subcommand list to generate a list of available commands.
*/
int th_subcmd_dispatch( void *cx,
                         Tcl_Interp *tcl,
                         int argc, Tcl_Obj * const * argv,
                         unsigned nCmd,
                         th_subcmd const * const aCmd,
                         int isSorted,
                         const char *zUsage,
                         th_result_set_f xResult);

/**
   A proxy for th_subcmd_dispatch() which first ensures that
   args has arguments before calling that function, passing on
   args's argument list.
*/
int th_subcmd_dispatch_flags( void *cx,
                               Tcl_Interp *tcl,
                               th_fp const * const args,
                               unsigned nCmd, th_subcmd const * const aCmd,
                               int isSorted, const char *zUsage );

/**
   A type for mapping an "enum" string value to an integer value.
*/
struct th_enum_entry {
  char const *zName;
  /* strlen() of zName. */
  unsigned nName;
  /* Enum value. */
  int value;
};

/** Convenience typedef. */
typedef struct th_enum_entry th_enum_entry;

enum {
  /* Hard upper limit on the number of entries a th_enum may
     contain. This limit was essentially arbitrily chosen. */
  th_enum_MAX_ENTRIES = 12
};
/**
   A type for holding a list of th_enum_entry objects.  These are
   structured differently than their conceptually-similar counterpart
   th_fp because individual entries in enums do not need
   readily-accessible symbolic names (for how they're used in this
   API), whereas flags-parsing requires being able to readily access
   the individual flags.
*/
struct th_enum {
  /** Number of entries in this->e which are populated. */
  unsigned n;
  /** The invidiual enum entries, this->n of which are populated. */
  th_enum_entry e[th_enum_MAX_ENTRIES];
};

/** Convenience typedef. */
typedef struct th_enum th_enum;

/**
   Declares and zeroes out a th_enum instance named VARNAME,
   referring to the default enum initialized by th_enum_init().
*/
#define th_enum_decl(VARNAME) \
  th_enum VARNAME; memset(&VARNAME,0,sizeof(th_enum))

#if 0
/**
   Pushes enum entry (ENAME,VALUE) to the scope-local enum named EnumName.
*/
#define th_enum_e(EnumName,EntryName,VALUE)          \
  assert(EnumName.n < th_enum_MAX_ENTRIES);          \
  EnumName.e[EnumName.n].zName = EntryName;           \
  EnumName.e[EnumName.n].nName = sizeof(EntryName)-1; \
  EnumName.e[EnumName.n++].value = VALUE;
#endif

/**
   Searches E->e for an entry matching the given string.  nZ is
   the strlen() of z. If nZ is negative, strlen() is used to calculate
   it.

   If a match is found, it is returned, else NULL is returned.

   The returned memory is owned by E.
*/
th_enum_entry const * th_enum_search(th_enum const * const E,
                                       char const * z, int nZ);

/**
   Emits a list of E's names and/or values to zOut.

   If which is <0 then only the keys are emitted. If which is 0 then
   both the keys and (integer) values are emitted. If which is >0 then
   only the values are emitted.

   If zSeparator is not NULL and not empty, it is inserted between
   each result. If it is NULL, " " is used.

   Returns 0 if the output fits in nOut bytes of zOut, else returns
   TCL_ERROR. On error it will have modified the contents of zOut.
*/
int th_enum_generate_list(th_enum const * const E,
                           int which,
                           char * zOut, unsigned nOut,
                           char const * zSeparator );



/**
   Generic finalizer interface used in several places under different
   names.
*/
typedef void (*th_finalizer_f)(void *);

/**
   The th_cmdo struct (pronounced "commando") holds state for binding
   a Tcl proc to an arbitrary native C object, which this object can
   optionally take over the lifetime of. Its purpose is to simplify
   management of lifetimes of objects bound to Tcl_ObjCmdProc
   implementations.
*/
struct th_cmdo {
  /**
     Some arbitrary, client-defined ID for the type of this->p. It is
     up to the client to confirm that the objects they are passed have
     the type(s) they're expecting.
  */
  void const * type;

  /**
     Underlying native object. Ostensibly owned by this object, but
     ownership is such a fluid term here.
  */
  void * p;

  /** Finalizer for this->p. */
  th_finalizer_f dtor;

  /**
     The interpreter instance which instantiated this object.
     Presumably that's the one it will always be used with. If not,
     all bets are off.
  */
  Tcl_Interp * tcl;

  /**
     The Command object for this instance.
  */
  Tcl_Command pCmd;

  /**
     Name of this->cmdo.pCmd. ACHTUNG: if the command is renamed in a
     script, this will be out of sync! Use wh_cmdo_name() to fetch
     it, optionally re-checking it.
  */
  Tcl_Obj *tCmdName;

  /**
     If installed with a var name (see th_cmdo_plugin()), this object
     manages a reference count for that name. The value of that name
     (i.e. the name of the installed proc) is available via
     tCmdName;
  */
  Tcl_Obj *tVarName;

  /**
     For internal use so that th_cmdo_free() can know whether
     th_cmdo_alloc() allocated it.
  */
  void const * allocStamp;

  /** The bottom 24 bits are for client-side use. */
  unsigned int flags;

  /** Reference count, which starts at 0. */
  unsigned short refCount;
};

typedef struct th_cmdo th_cmdo;

/**
   Returns cm's command's name from cm->tCmdName. If recheck is true
   or cm->tCmdName is 0 then this destroys any previous value (if any)
   and re-populates it from cm->pCmd. This whole thing is to work
   around the eventuality that a script renames the command.

   If cm->pCmd is 0 then 0 is returned.
*/
Tcl_Obj * th_cmdo_name(th_cmdo * cm, bool recheck);

/**
   Allocates a new th_cmdo and initializes its main state with the
   arguments (all but the first of which may be NULL). It must
   eventually be freed using th_cmdo_free(), but if th_cmdo_plugin()
   is used, that ownership will transfer to Tcl.

   The returned object starts out with a refcount of 0.
*/
th_cmdo * th_cmdo_alloc(Tcl_Interp *tcl, void const * type, void * p,
                        th_finalizer_f dtor);

/**
   Increases fx's refcount by 1 and returns fx.
*/
th_cmdo * th_cmdo_ref(th_cmdo * cm);

/**
   Decrements cm's refcount by 1, freeing it if it is at, or drops to,
   zero.  If cm was not allocated by th_cmdo_alloc() then this
   function won't actually free it, but will still clean up. The
   clean-up entails deleting its underlying Tcl command (if any),
   th_cmdo_unset() (if necessary), freeing its native resources, and
   clearing out most of its state.
*/
void th_cmdo_unref(th_cmdo * cm);

/**
   If cm is not 0 then this calls th_cmdo_unset() to clean up,
   then frees cm's memory _if_ cm was allocated using th_cmdo_alloc().
   If it was not allocated using th_cmdo_alloc() then it is cleaned up,
   and all state cleared except for cm->allocStamp, which is retained.

   ACHTUNG: this must never be called after th_cmdo_plugin() has
   succeeded. At that point, Tcl owns the memory.
*/
//void th_cmdo_free(th_cmdo * cm);

/**
   Plugs in a new command object to cm->tcl which wraps the given
   command implementation(s).  If nrAdapter is NULL then
   Tcl_CreateObjCommand() is used, else Tcl_NRCreateCommand() is used.

   The command gets installed with the name zCmdName. If that's NULL
   then a unique name is synthesized.  The command's name can later be
   fetched using Tcl_GetCommandName(cm->pCmd).  This API makes no
   guarantees about the form of synthesized command names, nor their
   stability across versions. They are only intended to be used for a
   single Tcl_Interp run, not serialized except for debugging
   purposes.

   If tVarName is not NULL, it also sets a tcl-frame-local var with
   that name to the name of the new proc, cm->tVarName is set that
   pointer, and this object manages a reference point to it.

   On success, returns 0 and this function increments cm's refcount to
   reflect that Tcl (co-)owns it via the installed Command object.

   Returns TCL_ERROR on error, but that can only happen for OOM
   conditions.  Results are undefined if any of the 1st or 3rd
   arguments are NULL.

   th_cmdo_unset() will unset the variable name, if any, set via
   tVarName. Use th_cmdo_unref() to release the reference count point
   this function installs (potentially freeing the object).
*/
int th_cmdo_plugin(th_cmdo * const cm,
                   Tcl_ObjCmdProc nrAdapter, Tcl_ObjCmdProc cmdProc,
                   const char *zCmdName, Tcl_Obj * const tVarName);

/**
   This undoes some of what th_cmdo_plugin().

   If cm has an associated variable name, that var is unset, with a
   caveat: it's unset in the current Tcl frame, which may differ from
   the frame the var was set in.

   This does not free the associated command object, nor does it
   modify cm's refcount.
*/
void th_cmdo_unset(th_cmdo * const cm);


#if defined(__cplusplus)
} /*extern "C"*/
#endif
#endif /* NET_WANDERINGHORSE_TH_H_INCLUDED */
