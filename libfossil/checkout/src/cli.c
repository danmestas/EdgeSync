/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/
#include "fossil-scm/cli.h"
#include "fossil-scm/core.h"
#include "fossil-scm/checkout.h"
#include "fossil-scm/confdb.h"
#include "fossil-scm/internal.h"
#include <string.h> /* for strchr() */
#if !defined(FSL_AMALGAMATION_BUILD)
/* When not in the amalgamation build, force assert() to always work... */
#  if defined(NDEBUG)
#    undef NDEBUG
#    undef DEBUG
#    define DEBUG 1
#  endif
#endif
#include <assert.h> /* for the benefit of test apps */

/* Only for debugging */
#include <stdio.h>
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

/** Convenience form of FCLI_VN for level-3 verbosity. */
#define FCLI_V3(pfexp) FCLI_VN(3,pfexp)
#define fcli_empty_m {    \
  .f = NULL,            \
  .argv = NULL,         \
  .argc = 0,            \
  .appName = NULL,      \
  .config = {           \
    .appHelp = NULL,      \
    .cliFlags = NULL,     \
    .checkoutDir = ".",      \
    .verbose = 0,                    \
    .traceSql = -1,                  \
    .outputer = fsl_outputer_empty_m,  \
    .listener = {.callback=fsl_msg_f_fcli, .state=0}  \
  },                         \
  .transient = {             \
    .repoDbArg = NULL,       \
    .userArg = NULL,         \
    .helpRequested = 0,       \
    .versionRequested = false \
  },                          \
  .paths = {fsl_pathfinder_empty_m/*bins*/}, \
  .err = fsl_error_empty_m,                  \
  .see = { .key = NULL, .nKey = 0, .type = FSL_SEE_KEYTYPE_PLAIN }   \
}

const fcli_t fcli_empty = fcli_empty_m;
fcli_t fcli = fcli_empty_m;
const fcli_cliflag fcli_cliflag_empty = fcli_cliflag_empty_m;
static fsl_timer fcliTimer = fsl_timer_empty_m;

void fcli_printf(char const * fmt, ...){
  va_list args;
  va_start(args,fmt);
  if(fcli.f){
    fsl_outputfv(fcli.f, fmt, args);
  }else{
    fsl_fprintfv(stdout, fmt, args);
  }
  va_end(args);
}

/**
   Outputs app-level help. How it does this depends on the state of
   the fcli object, namely fcli.config.cliFlags and the verbosity
   level. Normally this is triggered automatically by the CLI flag
   handling in fcli_setup().
*/
static void fcli_help(void);

unsigned short fcli_is_verbose(void){
  return fcli.config.verbose;
}

fsl_cx * fcli_cx(void){
  return fcli.f;
}

static int fcli_open(void){
  int rc = 0;
  fsl_cx * f = fcli.f;
  assert(f);
  if(fcli.transient.repoDbArg){
    FCLI_V3(("Trying to open repo db file [%s]...\n", fcli.transient.repoDbArg));
    rc = fsl_repo_open( f, fcli.transient.repoDbArg );
  }
  else if(fcli.config.checkoutDir){
    char const * dirName = fcli.config.checkoutDir;
    FCLI_V3(("Trying to open checkout from [%s]...\n",
             dirName));
    rc = fsl_ckout_open_dir(f, dirName, true);
    FCLI_V3(("checkout open rc=%s\n", fsl_rc_cstr(rc)));

    /* if(FSL_RC_NOT_FOUND==rc) rc = FSL_RC_NOT_A_CKOUT; */
    if(rc){
      if(!fsl_cx_err_get(f,NULL,NULL)){
        rc = fsl_cx_err_set(f, rc, "Opening of checkout under "
                            "[%s] failed with code %d (%s).",
                            dirName, rc, fsl_rc_cstr(rc));
      }
    }
    if(rc) return rc;
  }
  if(!rc){
    if(fcli.config.verbose>1){
      fsl_db * dbC = fsl_cx_db_ckout(f);
      fsl_db * dbR = fsl_cx_db_repo(f);
      if(dbC){
        FCLI_V3(("Checkout DB name: %s\n", f->db.ckout.db.filename));
      }
      if(dbR){
        FCLI_V3(("Opened repo db: %s\n", f->db.repo.db.filename));
        FCLI_V3(("Repo user name: %s\n", f->db.repo.user));
      }
    }
#if 0
    /*
      Only(?) here for testing purposes.

       We don't really need/want to update the repo db on each
       open of the checkout db, do we? Or do we?
     */
    fsl__repo_record_filename(f) /* ignore rc - not critical */;
#endif
  }
  return rc;
}


#define fcli__error (fcli.f ? &fcli.f->error : &fcli.err)
fsl_error * fcli_error(void){
  return fcli__error;
}

void fcli_err_reset(void){
  fsl_error_reset(fcli__error);
}


static struct TempFlags {
  bool traceSql;
  bool doTimer;
} TempFlags = {
false,
false
};

static struct {
  fsl_list list;
}  FCliFree = {
fsl_list_empty_m
};

static void fcli_shutdown(void){
  fsl_cx * const f = fcli.f;
  int rc = 0;

  fsl_error_clear(&fcli.err);
  fsl_free(fcli.argv)/*contents are in the FCliFree list*/;
  fsl_pathfinder_clear(&fcli.paths.bins);

  if(f){
    while(fsl_cx_txn_level(f)){
      MARKER(("WARNING: open db transaction at shutdown-time. "
              "Rolling back.\n"));
      fsl_cx_txn_end(f, true);
    }
    if(1 &&
       fsl_cx_db_ckout(f)){
      /* For testing/demo only: this is implicit
         when we call fsl_cx_finalize().
      */
      rc = fsl_close_scm_dbs(f);
      FCLI_V3(("Closed checkout/repo db(s). rc=%s\n", fsl_rc_cstr(rc)));
      //assert(0==rc);
    }
  }
  fsl_list_visit_free(&FCliFree.list, true);
  if(f){
    FCLI_V3(("Finalizing fsl_cx @%p\n", (void const *)f));
    fsl_cx_finalize( f );
  }
  fcli = fcli_empty;
  if(TempFlags.doTimer){
    fsl_timer_stop(&fcliTimer);
    f_out("Total fcli run time: %lf seconds of CPU time\n",
          (double)(fcliTimer.system + fcliTimer.user)/1000.0);
  }
}

static struct {
  fcli_cliflag const * flags;
} FCliHelpState = {
NULL
};


static int fcli_flag_f_nocheckoutDir(fcli_cliflag const *f){
  (void)f;
  fcli.config.checkoutDir = 0;
  return 0;
}
static int fcli_flag_f_verbose(fcli_cliflag const *f){
  (void)f;
  ++fcli.config.verbose;
  return FCLI_RC_FLAG_AGAIN;
}
static int fcli_flag_f_help(fcli_cliflag const *f){
  (void)f;
  ++fcli.transient.helpRequested;
  return FCLI_RC_FLAG_AGAIN;
}
static int fcli_flag_f_seekey(fcli_cliflag const *f){
  static int counter = 0;
  assert( fcli.see.key );
  if( ++counter != 1 ){
    return fcli_err_set(FSL_RC_MISUSE, "Only one of --see-key, --see-textkey, "
                        "and --see-hexkey may be used.");
  }
  if( strstr(f->flagLong, "textkey") ){
    fcli.see.type = FSL_SEE_KEYTYPE_TEXTKEY;
  }else if( strstr(f->flagLong, "hex") ){
    fcli.see.type = FSL_SEE_KEYTYPE_HEXKEY;
    if( fcli.see.nKey % 2 ){
      return fcli_err_set(FSL_RC_RANGE,"--%s must have an even number of digits.",
                          f->flagLong);
    }
  }else{
    fcli.see.type = FSL_SEE_KEYTYPE_PLAIN;
  }
  fcli.see.nKey = fsl_strlen(fcli.see.key);
  return 0;
}

static const fcli_cliflag FCliFlagsGlobal[] = {
  FCLI_FLAG_BOOL_X("?","help",NULL,
                   fcli_flag_f_help,
                   "Show app help. Also triggered if the first non-flag is \"help\"."),
  FCLI_FLAG_BOOL(0,"lib-version", &fcli.transient.versionRequested,
                 "Show libfossil version number."),
  FCLI_FLAG("R","repo","REPO-FILE",&fcli.transient.repoDbArg,
            "Selects a specific repository database, ignoring the one "
            "used by the current directory's checkout (if any)."),
  FCLI_FLAG(NULL,"user","username",&fcli.transient.userArg,
            "Sets the name of the fossil user name for this session."),
  FCLI_FLAG_BOOL_X(NULL, "no-checkout",NULL,fcli_flag_f_nocheckoutDir,
                   "Disable automatic attempt to open checkout."),
  FCLI_FLAG(NULL,"checkout-dir","DIRECTORY", &fcli.config.checkoutDir,
            "Open the given directory as a checkout, instead of the current dir."),
  FCLI_FLAG_BOOL_X("V","verbose",NULL,fcli_flag_f_verbose,
              "Increases the verbosity level by 1. May be used multiple times."),
  FCLI_FLAG_BOOL(NULL,"trace-sql",&TempFlags.traceSql,
                 "Enable SQL tracing."),
  FCLI_FLAG_BOOL(NULL,"timer",&TempFlags.doTimer,
                 "At the end of successful app execution, output how long it took "
                 "from the call to fcli_setup() until the end of main()."),
  FCLI_FLAG_X(NULL,"see-key","ENCRYPTION-KEY", &fcli.see.key, fcli_flag_f_seekey,
              "The SEE encryption key to use when opening the repo and checkout databases."),
  FCLI_FLAG_X(NULL,"see-hexkey","HEX-ENCODED-ENCRYPTION-KEY", &fcli.see.key, fcli_flag_f_seekey,
              "The hex-encoded SEE encryption key to use when opening the repo and checkout databases."),
  FCLI_FLAG_X(NULL,"see-textkey","ENCRYPTION-KEY", &fcli.see.key, fcli_flag_f_seekey,
              "The SEE encryption key to use when opening the repo and checkout databases."),
  fcli_cliflag_empty_m
};

void fcli_cliflag_help(fcli_cliflag const *defs){
  fcli_cliflag const * f;
  const char * tab = "  ";
  for( f = defs; f->flagShort || f->flagLong; ++f ){
    const char * s = f->flagShort;
    const char * l = f->flagLong;
    const char * fvl = f->flagValueLabel;
    const char * valLbl = 0;
    switch(f->flagType){
      case FCLI_FLAG_TYPE_BOOL:
      case FCLI_FLAG_TYPE_BOOL_INVERT: break;
      case FCLI_FLAG_TYPE_INT32: valLbl = fvl ? fvl : "int32"; break;
      case FCLI_FLAG_TYPE_INT64: valLbl = fvl ? fvl : "int64"; break;
      case FCLI_FLAG_TYPE_ID: valLbl = fvl ? fvl : "db-record-id"; break;
      case FCLI_FLAG_TYPE_DOUBLE: valLbl = fvl ? fvl : "double"; break;
      case FCLI_FLAG_TYPE_CSTR: valLbl = fvl ? fvl : "string"; break;
      default:
        break;
    }
    f_out("%s%s%s%s%s%s%s%s",
          tab,
          s?"-":"", s?s:"", (s&&l)?"|":"",
          l?"--":"",l?l:"",
          valLbl ? "=" : "", valLbl);
    if(f->helpText){
      f_out("\n%s%s%s", tab, tab, f->helpText);
    }
    f_out("\n\n");
  }
}

void fcli_help(void){
  if(fcli.config.appHelp){
    if(fcli.config.appHelp->briefUsage){
      f_out("Usage: %s [options] %s\n", fcli.appName, fcli.config.appHelp->briefUsage);
    }
    if(fcli.config.appHelp->briefDescription){
      f_out("\n%s\n", fcli.config.appHelp->briefDescription);
    }
  }else{
    f_out("Help for %s:\n", fcli.appName);
  }
  const int helpCount = fcli.transient.helpRequested
    + fcli.config.verbose;
  bool const showGlobal = helpCount>1;
  bool const showApp = (2!=helpCount);
  if(showGlobal){
    f_out("\nFCli global flags:\n\n");
    fcli_cliflag_help(FCliFlagsGlobal);
  }else{
    f_out("\n");
  }
  if(showApp){
    if(FCliHelpState.flags
       && (FCliHelpState.flags[0].flagShort || FCliHelpState.flags[0].flagLong)){
      f_out("App-specific flags:\n\n");
      fcli_cliflag_help(FCliHelpState.flags);
      //f_out("\n");
    }
    if(fcli.config.appHelp && fcli.config.appHelp->callback){
      fcli.config.appHelp->callback();
      f_out("\n");
    }
  }
  if(showGlobal){
    if(!showApp){
      f_out("Invoke --help three times (or --help -V -V) to list "
            "both the framework- and app-level options.\n");
    }else{
      f_out("Invoke --help once to list only the "
            "app-level flags.\n");
    }
  }else{
    f_out("Invoke --help twice (or --help -V) to list the "
          "framework-level options. Use --help three times "
          "to list both framework- and app-level options.\n");
  }
  f_out("\nFlags which require values may be passed as "
        "--flag=value or --flag value.\n\n");
}

int fcli_process_flags( fcli_cliflag const * defs ) {
  fcli_cliflag const * f;
  int rc = 0;
  /**
     TODO/FIXME/NICE-TO-HAVE: we "really should" process the CLI flags
     in the order they are provided on the CLI, as opposed to the
     order they're defined in the defs array. The current approach is
     much simpler to process but keeps us from being able to support
     certain useful flag-handling options, e.g.:

     f-tag -a artifact-id-1 --tag x=y --tag y=z -a artifact-id-2 --tag a=b...

     The current approach consumes the -a flags first, leaving us
     unable to match the --tag flags to their corresponding
     (left-hand) -a flag.

     Processing them the other way around, however, requires that we
     keep track of which flags we've already seen so that we can
     reject, where appropriate, duplicate invocations.

     We could, instead of looping on the defs array, loop over the
     head of fcli.argv. If it's a non-flag, move it out of the way
     temporarily (into a new list), else look over the defs array
     looking for a flag match. We don't know, until finding such a
     match, whether the current flag requires a value. If it does, we
     then have to check the current fcli.argv entry to see if it has a
     value (--x=y) or whether the next argv entry is its value (--x
     y). If the current tip has no matching defs entry, we have no
     choice but to skip over it in the hopes that the user can use
     fcli_flag() and friends to consume it, but we cannot know, from
     here, whether such a stray flag requires a value, which means we
     cannot know, for sure, how to process the _next_ argument. The
     best we could do is have a heuristic like "if it starts with a
     dash, assume it's a flag, otherwise assume it's a value for the
     previous flag and skip over it," but whether or not that's sane
     enough for daily use is as yet undetermined.

     If we change the CLI interface to require --flag=value for all
     flags, as opposed to optionally allowing (--flag value), the
     above becomes simpler, but CLI usage suffers. Hmmm. e.g.:

     f-ci -m="message" ...

     simply doesn't fit the age-old muscle memory of:

     svn ci -m ...
     cvs ci -m ...
     fossil ci -m ...
     f-ci -m ...
  */
  for( f = defs; f->flagShort || f->flagLong; ++f ){
    if(!f->flagValue && !f->callback){
      /* We accept these for purposes of generating the --help text,
         but we can't otherwise do anything sensible with them and
         assume the app will handle such flags downstream or ignore
         them altogether.*/
      continue;
    }
    char const * v = NULL;
    const char ** passV = f->flagValue ? &v : NULL;
    switch(f->flagType){
      case FCLI_FLAG_TYPE_BOOL:
      case FCLI_FLAG_TYPE_BOOL_INVERT:
        passV = NULL;
        break;
      default: break;
    };
    bool const gotIt = fcli_flag2(f->flagShort, f->flagLong, passV);
    if(fcli__error->code){
      /**
         Corner case. Consider:

         FCLI_FLAG("x","y","xy", &foo, "blah");

         And: my-app -x

         That will cause fcli_flag2() to return false, but it will
         also populate fcli__error for us.
      */
      rc = fcli__error->code;
      break;
    }
    //MARKER(("Got?=%d flag: %s/%s %s\n",gotIt, f->flagShort, f->flagLong, v ? v : ""));
    if(!gotIt){
      continue;
    }
    if(f->flagValue) switch(f->flagType){
      case FCLI_FLAG_TYPE_BOOL:
        *((bool*)f->flagValue) = true;
        break;
      case FCLI_FLAG_TYPE_BOOL_INVERT:
        *((bool*)f->flagValue) = false;
        break;
      case FCLI_FLAG_TYPE_CSTR:
        if(!v) goto missing_val;
        *((char const **)f->flagValue) = v;
        break;
      case FCLI_FLAG_TYPE_INT32:
        if(!v) goto missing_val;
        *((int32_t*)f->flagValue) = atoi(v);
        break;
      case FCLI_FLAG_TYPE_INT64:
        if(!v) goto missing_val;
        *((int64_t*)f->flagValue) = atoll(v);
        break;
      case FCLI_FLAG_TYPE_ID:
        if(!v) goto missing_val;
        if(sizeof(fsl_id_t)>32){
          *((fsl_id_t*)f->flagValue) = (fsl_id_t)atoll(v);
        }else{
          *((fsl_id_t*)f->flagValue) = (fsl_id_t)atol(v);
        }
        break;
      case FCLI_FLAG_TYPE_DOUBLE:
        if(!v) goto missing_val;
        *((double*)f->flagValue) = strtod(v, NULL);
        break;
      default:
        MARKER(("As-yet-unhandled flag type for flag %s%s%s.",
                f->flagShort ? f->flagShort : "",
                (f->flagShort && f->flagLong) ? "|" : "",
                f->flagLong ? f->flagLong : ""));
        rc = FSL_RC_MISUSE;
        break;
    }
    if(rc) break;
    else if(f->callback){
      rc = f->callback(f);
      if(rc==FCLI_RC_FLAG_AGAIN){
        rc = 0;
        --f;
      }else if(rc){
        break;
      }
    }
  }
  //MARKER(("fcli__error->code==%s\n", fsl_rc_cstr(fcli__error->code)));
  return rc;
  missing_val:
  rc = fcli_err_set(FSL_RC_MISUSE,"Missing value for flag %s%s%s.",
                    f->flagShort ? f->flagShort : "",
                    (f->flagShort && f->flagLong) ? "|" : "",
                    f->flagLong ? f->flagLong : "");
  return rc;
}

/**
   oldMode must be true if fcli.config.cliFlags is NULL, else false.
*/
static int fcli_process_argv( bool oldMode, int argc, char const * const * argv ){
  int i;
  int rc = 0;
  char * cp;
  fcli.appName = argv[0];
  fcli.argc = 0;
  fcli.argv = (char **)fsl_malloc( (argc + 1) * sizeof(char*));
  fcli.argv[argc] = NULL;
  for( i = 1; i < argc; ++i ){
    char const * arg = argv[i];
    if('-'==*arg){
      char const * flag = arg+1;
      while('-'==*flag) ++flag;
#define FLAG(F) if(0==fsl_strcmp(F,flag))
      if(oldMode){
        FLAG("help") {
          ++fcli.transient.helpRequested;
          continue;
        }
        FLAG("?") {
          ++fcli.transient.helpRequested;
          continue;
        }
        FLAG("V") {
          fcli.config.verbose += 1;
          continue;
        }
        FLAG("verbose") {
          fcli.config.verbose += 1;
          continue;
        }
      }
#undef FLAG
      /* else fall through */
    }
    cp = fsl_strdup(arg);
    if(!cp) return FSL_RC_OOM;
    fcli.argv[fcli.argc++] = cp;
    fcli_fax(cp);
  }
  if(!rc && !oldMode){
    rc = fcli_process_flags(FCliFlagsGlobal);
  }
  return rc;
}

bool fcli_flag(char const * opt, const char ** value){
  int i = 0;
  int remove = 0 /* number of items to remove from argv */;
  bool rc = false /* true if found, else 0 */;
  fsl_size_t optLen = fsl_strlen(opt);
  for( ; i < fcli.argc; ++i ){
    char const * arg = fcli.argv[i];
    char const * x;
    char const * vp = NULL;
    if(!arg || ('-' != *arg)) continue;
    rc = false;
    x = arg+1;
    if('-' == *x) { ++x;}
    if(0 != fsl_strncmp(x, opt, optLen)) continue;
    if(!value){
      if(x[optLen]) continue /* not exact match */;
      /* Treat this as a boolean. */
      rc = true;
      ++remove;
      break;
    }else{
      /* -FLAG VALUE or -FLAG=VALUE */
      if(x[optLen] == '='){
        rc = true;
        vp = x+optLen+1;
        ++remove;
      }
      else if(x[optLen]) continue /* not an exact match */;
      else if(i<(fcli.argc-1)){ /* -FLAG VALUE */
        vp = fcli.argv[i+1];
        if('-'==*vp && vp[1]/*allow "-" by itself!*/){
          // VALUE looks like a flag.
          fcli_err_set(FSL_RC_MISUSE, "Missing value for flag [%s].",
                       opt);
          rc = false;
          assert(!remove);
          break;
        }
        rc = true;
        remove += 2;
      }
      else{
        /*
          --FLAG is expecting VALUE but we're at end of argv.  Leave
          --FLAG in the args and report this as "not found."
        */
        rc = false;
        assert(!remove);
        fcli_err_set(FSL_RC_MISUSE,
                     "Missing value for flag [%s].",
                     opt);
        assert(fcli__error->code);
        //MARKER(("Missing flag value for [%s]\n",opt));
        break;
      }
      if(rc){
        *value = vp;
      }
      break;
    }
  }
  if(remove>0){
    int x;
    for( x = 0; x < remove; ++x ){
      fcli.argv[i+x] = NULL/*memory ownership==>FCliFree*/;
    }
    for( ; i < fcli.argc; ++i ){
      fcli.argv[i] = fcli.argv[i+remove];
    }
    fcli.argc -= remove;
    fcli.argv[i] = NULL;
  }
  //MARKER(("flag %s check rc=%s\n",opt,fsl_rc_cstr(fcli__error->code)));
  return rc;
}

bool fcli_flag2(char const * shortOpt,
                char const * longOpt,
                const char ** value){
  bool rc = 0;
  if(shortOpt) rc = fcli_flag(shortOpt, value);
  if(!rc && longOpt && !fcli__error->code) rc = fcli_flag(longOpt, value);
  //MARKER(("flag %s check rc=%s\n",shortOpt,fsl_rc_cstr(fcli__error->code)));
  return rc;
}

bool fcli_flag_or_arg(char const * shortOpt,
                      char const * longOpt,
                      const char ** value){
  bool rc = fcli_flag(shortOpt, value);
  if(!rc && !fcli__error->code){
    rc = fcli_flag(longOpt, value);
    if(!rc && value){
      const char * arg = fcli_next_arg(1);
      if(arg){
        rc = true;
        *value = arg;
      }
    }
  }
  return rc;
}


/**
    We copy fsl_lib_configurable.allocator as a base allocator.
 */
static fsl_allocator fslAllocOrig;

/**
    Proxies fslAllocOrig.f() and abort()s on OOM conditions.
*/
static void * fsl_realloc_f_failing(void * state,
                                    void * mem, fsl_size_t n){
  void * rv = fslAllocOrig.f(fslAllocOrig.state, mem, n);
  (void)state;
  if(n && !rv){
    fsl__fatal(FSL_RC_OOM, NULL)/*does not return*/;
  }
  return rv;
}

/**
    Replacement for fsl_memory_allocator() which abort()s on OOM.
    Why? Because fossil(1) has shown how much that can simplify error
    checking in an allocates-often API.
 */
static const fsl_allocator fcli_allocator = {
fsl_realloc_f_failing,
NULL/*state*/
};

#if !defined(FCLI_USE_SIGACTION)
#  if (defined(_POSIX_C_SOURCE) || defined(sa_sigaction/*BSD*/)) \
  && defined(HAVE_SIGACTION)
/* ^^^ on Linux, sigaction() is only available in <signal.h>
   if _POSIX_C_SOURCE is set */
#    define FCLI_USE_SIGACTION HAVE_SIGACTION
#  else
#    define FCLI_USE_SIGACTION 0
#  endif
#endif

#if FCLI_USE_SIGACTION
#include <signal.h> /* sigaction(), if our feature macros are set right */
/**
   SIGINT handler which calls fsl_cx_interrupt().
*/
static void fcli__sigc_handler(int s){
  static fsl_cx * f = 0;
  if(f) return/*disable concurrent interruption*/;
  f = fcli_cx();
  if(f && !fsl_cx_interrupted(f)){
    //f_out("^C\n"); // no - this would interfere with curses apps
    fsl_cx_interrupt(f, FSL_RC_INTERRUPTED,
                     "Interrupted by signal #%d.", s);
    f = NULL;
  }
}
#endif
/* ^^^ FCLI_USE_SIGACTION */

static int fcli_see_key_f(void * pState, const char *zDbFile,
                          fsl_buffer * pOut, int *keyMode){
  (void)pState;
  (void)zDbFile;
  assert( fcli.see.key && *fcli.see.key );
  *keyMode = fcli.see.type;
  return fsl_buffer_append(pOut, fcli.see.key, (fsl_int_t)fcli.see.nKey);
}


void fcli_pre_setup(void){
  static int run = 0;
  if(run++) return;
  fslAllocOrig = fsl_lib_configurable.allocator;
  fsl_lib_configurable.allocator = fcli_allocator
    /* This MUST be done BEFORE the fsl API allocates
       ANY memory! */;
  atexit(fcli_shutdown);
#if FCLI_USE_SIGACTION
  struct sigaction sigIntHandler;
  sigIntHandler.sa_handler = fcli__sigc_handler;
  sigemptyset(&sigIntHandler.sa_mask);
  sigIntHandler.sa_flags = 0;
  sigaction(SIGINT, &sigIntHandler, NULL);
#endif
}

/**
   oldMode must be true if fcli.config.cliFlags is NULL, else false.
*/
static int fcli_setup_common1(bool oldMode, int argc, char const * const *argv){
  static char once = 0;
  int rc = 0;
  fsl_timer_start(&fcliTimer);
  if( once++ ){
    fprintf(stderr,"MISUSE: fcli_setup() must "
            "not be called more than once.");
    return FSL_RC_MISUSE;
  }
  fcli_pre_setup();
  rc = fcli_process_argv(oldMode, argc, argv);
  if(!rc && fcli.argc && 0==fsl_strcmp("help",fcli.argv[0])){
    fcli_next_arg(1) /* strip argument */;
    ++fcli.transient.helpRequested;
  }
  return rc;
}

static int fcli_setup_common2(void){
  int rc = 0;
  fsl_cx_config fcfg = fsl_cx_config_empty;
  fsl_cx * f = 0;

  fcfg.listener = fcli.config.listener;
  fcfg.sqlPrint = 1;
  if( fcli.see.key && *fcli.see.key ){
    fcfg.see.getSEEKey = fcli_see_key_f;
  }
  if(fcli.config.outputer.out){
    fcfg.output = fcli.config.outputer;
    fcli.config.outputer = fsl_outputer_empty
      /* To avoid any confusion about ownership */;
  }else{
    fcfg.output = fsl_outputer_FILE;
    fcfg.output.state = stdout;
  }
  if(fcli.config.traceSql>0 || TempFlags.traceSql){
    fcfg.traceSql = fcli.config.traceSql;
  }

  rc = fsl_cx_init( &f, &fcfg );
  fcli.f = f;
#if 0
  /* Just for testing cache size effects... */
  f->cache.arty.szLimit = 1024 * 1024 * 20;
  f->cache.arty.usedLimit = 300;
#endif
  fsl_error_clear(&fcli.err);
  FCLI_V3(("Initialized fsl_cx @0x%p. rc=%s\n",
           (void const *)f, fsl_rc_cstr(rc)));
  if(!rc){
#if 0
    if(fcli.transient.gmtTime){
      fsl_cx_flag_set(f, FSL_CX_F_LOCALTIME_GMT, 1);
    }
#endif
    if(fcli.config.checkoutDir || fcli.transient.repoDbArg){
      rc = fcli_open();
      FCLI_V3(("fcli_open() rc=%s\n", fsl_rc_cstr(rc)));
      if(!fcli.transient.repoDbArg && fcli.config.checkoutDir
         && (FSL_RC_NOT_FOUND == rc)){
        /* If [it looks like] we tried an implicit checkout-open but
           didn't find one, suppress the error. */
        rc = 0;
        fcli_err_reset();
      }
    }
  }
  if(!rc){
    char const * userName = fcli.transient.userArg;
    if(userName){
      fsl_cx_user_set(f, userName);
    }else{
      fsl_cx_user_guess(f);
    }
  }
  return rc;
}

static int check_help_invoked(void){
  int rc = 0;
  if(fcli.transient.helpRequested){
    /* Do this last so that we can get the default user name and such
       for display in the help text. */
    fcli_help();
    rc = FCLI_RC_HELP;
  }else if(fcli.transient.versionRequested){
    f_out("libfossil version: %s\nCheckin: %s\nCheckin timestamp: %s\n",
          fsl_library_version(),
          fsl_buildinfo(FSL_BUILDINFO_VERSION_HASH),
          fsl_buildinfo(FSL_BUILDINFO_VERSION_TIMESTAMP));
    rc = FCLI_RC_HELP;
  }
  return rc;
}

static int fcli_setup2(int argc, char const * const * argv,
                       const fcli_cliflag * flags){
  int rc;
  FCliHelpState.flags = flags;
  rc = fcli_setup_common1(false, argc, argv);
  if(rc) return rc;
  assert(!fcli__error->code);
  rc = check_help_invoked();
  if(!rc){
    rc = fcli_process_flags(flags);
    if(rc) assert(fcli__error->msg.used);
    if(!rc){
      rc = fcli_setup_common2();
    }
  }
  return rc;
}

int fcli_setup_v2(int argc, char const * const * argv,
                  fcli_cliflag const * const cliFlags,
                  fcli_help_info const * const helpInfo ){
  if(NULL!=cliFlags) fcli.config.cliFlags = cliFlags;
  if(NULL!=helpInfo) fcli.config.appHelp = helpInfo;
  if(cliFlags || fcli.config.cliFlags){
    return fcli_setup2(argc, argv, cliFlags ? cliFlags : fcli.config.cliFlags);
  }
  /* Else do "old mode" setup... */
  int rc = fcli_setup_common1(true, argc, argv);
  if(!rc){
    rc = check_help_invoked();
    if(!rc){
      if( fcli_flag2(NULL, "no-checkout", NULL) ){
        fcli.config.checkoutDir = NULL;
      }
      fcli_flag2(NULL,"user", &fcli.transient.userArg);
      fcli.config.traceSql =  fcli_flag2(NULL,"trace-sql", NULL);
      fcli_flag2("R", "repo", &fcli.transient.repoDbArg);
      //--see-key and friends: do not support in "old mode"
      rc = fcli_setup_common2();
    }
  }
  return rc;
}

int fcli_setup(int argc, char const * const * argv ){
  return fcli_setup_v2(argc, argv, fcli.config.cliFlags, fcli.config.appHelp);
}

int fcli_err_report2(bool clear, char const * file, int line){
  int errRc = 0;
  char const * msg = NULL;
  errRc = fsl_error_get( fcli__error, &msg, NULL );
  if(FCLI_RC_HELP==errRc){
    errRc = 0;
  }else if( !errRc && fcli.f ){
    if( fcli.f->interrupted ){
      errRc = fcli.f->interrupted;
      msg = "Interrupted.";
    }else{
      fsl_db * const db = fsl_cx_db(fcli.f);
      if( db && db->error.code ){
        fsl_cx_uplift_db_error(fcli.f, db);
        errRc = fsl_error_get( &fcli.f->error, &msg, NULL);
      }
    }
  }
  if(errRc || msg){
    if(fcli.config.verbose>0){
      fcli_printf("%s %s:%d: ERROR #%d (%s): %s\n",
                  fcli.appName,
                  file, line, errRc, fsl_rc_cstr(errRc), msg);
    }else{
      fcli_printf("%s: ERROR #%d (%s): %s\n",
                  fcli.appName, errRc, fsl_rc_cstr(errRc), msg);
    }
  }
  if(clear){
    fcli_err_reset();
    if(fcli.f) fsl_cx_interrupt(fcli.f, 0, NULL);
  }
  return errRc;
}


const char * fcli_next_arg(bool remove){
  const char * rc = fcli.argc ? fcli.argv[0] : NULL;
  if(fcli.argc && remove){
    int i;
    --fcli.argc;
    for(i = 0; i < fcli.argc; ++i){
      fcli.argv[i] = fcli.argv[i+1];
    }
    fcli.argv[fcli.argc] = NULL/*owned by FCliFree*/;
  }
  return rc;
}

int fcli_has_unused_args(bool outputError){
  int rc = 0;
  if(fcli.argc){
    rc = fsl_cx_err_set(fcli.f, FSL_RC_MISUSE,
                        "Unhandled extra argument: %s",
                        fcli.argv[0]);
    if(outputError){
      fcli_err_report(false);
    }
  }
  return rc;
}

int fcli_has_unused_flags(bool outputError){
  int i;
  for( i = 0; i < fcli.argc; ++i ){
    char const * arg = fcli.argv[i];
    if('-'==*arg){
      int rc = fsl_cx_err_set(fcli.f, FSL_RC_MISUSE,
                              "Unhandled/unknown flag or missing value: %s",
                              arg);
      if(outputError){
        fcli_err_report(false);
      }
      return rc;
    }
  }
  return 0;
}

int fcli_err_set(int code, char const * fmt, ...){
  int rc;
  va_list va;
  va_start(va, fmt);
  rc = fsl_error_setv(fcli__error, code, fmt, va);
  va_end(va);
  return rc;
}

int fcli_end_of_main(int mainRc){
  if(FCLI_RC_HELP==mainRc){
    mainRc = 0;
  }
  if(fcli_err_report(true)){
    return EXIT_FAILURE;
  }else if(mainRc){
    fcli_err_set(mainRc,"Ending with unadorned end-of-app "
                 "error code %d/%s.",
                 mainRc, fsl_rc_cstr(mainRc));
    fcli_err_report(true);
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

int fcli_dispatch_commands( fcli_command const * cmd,
                            bool reportErrors){
  int rc = 0;
  const char * arg = fcli_next_arg(0);
  fcli_command const * orig = cmd;
  fcli_command const * helpPos = 0;
  int helpState = 0;
  if(!arg){
    return fcli_err_set(FSL_RC_MISUSE,
                        "Missing command argument. Try --help.");
  }
  assert(fcli.f);
  for(; arg && cmd->name; ++cmd ){
    if(cmd==orig && 0==fsl_strcmp(arg,"help")){
      /* Accept either (help command) or (command help) as help. */
      /* Except that it turns out that fcli_setup() will trump the
         former and doesn't have the fcli_command state, so can't do
         this. Maybe we can change that somehow. */
      helpState = 1;
      helpPos = orig;
      arg = fcli_next_arg(1); // consume it
    }else if(0==fsl_strcmp(arg,cmd->name) || 0==fcli_cmd_aliascmp(cmd,arg)){
      if(!cmd->f){
        rc = fcli_err_set(FSL_RC_NYI,
                               "Command [%s] has no "
                               "callback function.");
      }else{
        fcli_next_arg(1)/*consume it*/;
        if(helpState){
          assert(1==helpState);
          helpState = 2;
          helpPos = cmd;
          break;
        }
        const char * helpCheck = fcli_next_arg(false);
        if(helpCheck && 0==fsl_strcmp("help",helpCheck)){
          helpState = 3;
          helpPos = cmd;
          break;
        }else{
          rc = cmd->f(cmd);
        }
      }
      break;
    }
  }
  if(helpState){
    f_out("\n");
    fcli_command_help(helpPos, true, helpState>1);
    fcli.transient.helpRequested++;
  }else if(!cmd->name){
    fsl_buffer msg = fsl_buffer_empty;
    int rc2;
    if(!arg){
      rc2 = FSL_RC_MISUSE;
      fsl_buffer_appendf(&msg, "No command provided.");
    }else{
      rc2 = FCLI_RC_NO_CMD;
      fsl_buffer_appendf(&msg, "Command not found: %s.",arg);
    }
    fsl_buffer_appendf(&msg, " Available commands: ");
    cmd = orig;
    for( ; cmd && cmd->name; ++cmd ){
      fsl_buffer_appendf( &msg, "%s%s",
                          (cmd==orig) ? "" : ", ",
                          cmd->name);
    }
    rc = fcli_err_set(rc2, "%b", &msg);
    fsl_buffer_clear(&msg);
  }
  if(rc && reportErrors){
    fcli_err_report(0);
  }
  return rc;
}

int fcli_cmd_aliascmp(fcli_command const * cmd, char const * arg){
  char const * alias = cmd->aliases;
  while ( alias && *alias!=0 ){
    if( 0==fsl_strcmp(alias, arg) ){
      return 0;
    }
    alias = strchr(alias, 0) + 1;
  }
  return 1;
}

void fcli_command_help(fcli_command const * cmd, bool showUsage, bool onlyOne){
  fcli_command const * c = cmd;
  for( ; c->name; ++c ){
    f_out("[%s] command:\n\n", c->name);
    if(c->briefDescription){
      f_out("  %s\n", c->briefDescription);
    }
    if(c->aliases){
      fcli_help_show_aliases(c->aliases);
    }else{
      f_out("\n");
    }
    if(c->flags){
      f_out("\n");
      fcli_cliflag_help(c->flags);
    }
    if(showUsage && c->usage){
      c->usage();
    }
    if(onlyOne) break;
  }
}

void fcli_help_show_aliases(char const * aliases){
  char const * alias = aliases;
  f_out("  (aliases: ");
  while ( *alias!=0 ){
    f_out("%s%s", alias, *(strchr(alias, 0) + 1) ? ", " : ")\n");
    alias = strchr(alias, 0) + 1;
  }
}

void * fcli_fax(void * mem){
  if(mem){
    fsl_list_append( &FCliFree.list, mem );
  }
  return mem;
}

int fcli_ckout_show_info(bool useUtc){
  fsl_cx * const f = fcli_cx();
  int rc = 0;
  fsl_stmt st = fsl_stmt_empty;
  fsl_db * const dbR = fsl_cx_db_repo(f);
  fsl_db * const dbC = fsl_cx_db_ckout(f);
  int lblWidth = -20;
  if(!fsl_needs_ckout(f)){
    return FSL_RC_NOT_A_CKOUT;
  }
  assert(dbR);
  assert(dbC);

  fsl_id_t rid = 0;
  fsl_uuid_cstr uuid = NULL;
  fsl_ckout_version_info(f, &rid, &uuid);
  assert((uuid && (rid>0)) || (!uuid && (0==rid)));

  f_out("%*s %s\n", lblWidth, "repository-db:",
        fsl_cx_db_file_repo(f, NULL));
  f_out("%*s %s\n", lblWidth, "checkout-root:",
        fsl_cx_ckout_dir_name(f, NULL));

  rc = fsl_cx_prepare(f, &st, "SELECT "
                      /*0*/"datetime(event.mtime%s) AS timestampString, "
                      /*1*/"coalesce(euser, user) AS user, "
                      /*2*/"(SELECT group_concat(substr(tagname,5), ', ') FROM tag, tagxref "
                      "WHERE tagname GLOB 'sym-*' AND tag.tagid=tagxref.tagid "
                      "AND tagxref.rid=blob.rid AND tagxref.tagtype>0) as tags, "
                      /*3*/"coalesce(ecomment, comment) AS comment, "
                      /*4*/"uuid AS uuid "
                      "FROM event JOIN blob "
                      "WHERE "
                      "event.type='ci' "
                      "AND blob.rid=%"FSL_ID_T_PFMT" "
                      "AND blob.rid=event.objid "
                      "ORDER BY event.mtime DESC",
                      useUtc ? "" : ", 'localtime'",
                      rid);
  if(rc) goto end;
  if( FSL_RC_STEP_ROW != fsl_stmt_step(&st)){
    /* fcli_err_set(FSL_RC_ERROR, "Event data for checkout not found."); */
    f_out("\nNo 'event' data found. This is only normal for an empty repo.\n");
    goto end;
  }

  f_out("%*s %s %s %s (RID %"FSL_ID_T_PFMT")\n",
        lblWidth, "checkout-version:",
        fsl_stmt_g_text(&st, 4, NULL),
        fsl_stmt_g_text(&st, 0, NULL),
        useUtc ? "UTC" : "local",
        rid );

  {
    /* list parent(s) */
    fsl_stmt stP = fsl_stmt_empty;
    rc = fsl_cx_prepare(f, &stP, "SELECT "
                        "uuid, pid, isprim, "
                        "datetime(event.mtime%s) "
                        "FROM plink, blob, event "
                        "WHERE cid=%"FSL_ID_T_PFMT" "
                        "AND rid=pid "
                        "AND rid=event.objid "
                        "ORDER BY isprim DESC, event.mtime DESC /*sort*/",
                        useUtc ? "" : ", 'localtime'",
                        rid);
    if(rc) goto end;
    while( FSL_RC_STEP_ROW == fsl_stmt_step(&stP) ){
      char const * zLabel = fsl_stmt_g_int32(&stP,2)
        ? "parent:" : "merged-from:";
      f_out("%*s %s %s %s (RID %" FSL_ID_T_PFMT")\n", lblWidth, zLabel,
            fsl_stmt_g_text(&stP, 0, NULL),
            fsl_stmt_g_text(&stP, 3, NULL),
            useUtc ? "UTC" : "local",
            fsl_stmt_g_id(&stP, 1)
      );
    }
    fsl_stmt_finalize(&stP);
  }
  {
    /* list merge parent(s) */
    fsl_stmt stP = fsl_stmt_empty;
    rc = fsl_cx_prepare(f, &stP, "SELECT "
                        "mhash, id FROm vmerge WHERE id<=0");
    if(rc) goto end;
    while( FSL_RC_STEP_ROW == fsl_stmt_step(&stP) ){
      char const * zClass;
      int32_t const id = fsl_stmt_g_int32(&stP,1);
      switch(id){
        case FSL_MERGE_TYPE_INTEGRATE: zClass = "integrate-merge:"; break;
        case FSL_MERGE_TYPE_BACKOUT: zClass = "backout-merge:"; break;
        case FSL_MERGE_TYPE_CHERRYPICK: zClass = "cherrypick-merge:"; break;
        case FSL_MERGE_TYPE_NORMAL: zClass = "merged-with:"; break;
        default:
          fsl__fatal(FSL_RC_RANGE,
                     "Unexpected value %"PRIi32" in vmerge.id",id);
          break;
      }
      f_out("%*s %s\n", lblWidth, zClass,
            fsl_stmt_g_text(&stP, 0, NULL));
    }
    fsl_stmt_finalize(&stP);
  }
  {
    /* list children */
    fsl_stmt stC = fsl_stmt_empty;
    rc = fsl_cx_prepare(f, &stC, "SELECT "
                        "uuid, cid, isprim "
                        "FROM plink JOIN blob ON cid=rid "
                        "WHERE pid=%"FSL_ID_T_PFMT" "
                        "ORDER BY isprim DESC, mtime DESC /*sort*/",
                        rid);
    if(rc) goto end;
    while( FSL_RC_STEP_ROW == fsl_stmt_step(&stC) ){
      char const * zLabel = fsl_stmt_g_int32(&stC,2)
        ? "child:" : "merged-into:";
      f_out("%*s %s\n", lblWidth, zLabel,
            fsl_stmt_g_text(&stC, 0, NULL));
    }
    fsl_stmt_finalize(&stC);
  }

  f_out("%*s %s\n", lblWidth, "user:",
        fsl_stmt_g_text(&st, 1, NULL));

  f_out("%*s %s\n", lblWidth, "tags:",
        fsl_stmt_g_text(&st, 2, NULL));

  f_out("%*s %s\n", lblWidth, "comment:",
        fsl_stmt_g_text(&st, 3, NULL));

  end:
  fsl_stmt_finalize(&st);

  return rc;
}

static int fsl_stmt_each_f_ambiguous( fsl_stmt * stmt, void * state ){
  int rc;
  if(1==stmt->rowCount) stmt->rowCount=0
                          /* HORRIBLE KLUDGE to elide header. */;
  rc = fsl_stmt_each_f_dump(stmt, state);
  if(0==stmt->rowCount) stmt->rowCount = 1;
  return rc;
}

void fcli_list_ambiguous_artifacts(char const * label,
                                   char const *prefix){
  fsl_db * const db = fsl_cx_db_repo(fcli.f);
  assert(db);
  if(!label){
    f_out("Artifacts matching ambiguous prefix: %s\n",prefix);
  }else if(*label){
    f_out("%s\n", label);
  }
  /* Possible fixme? Do we only want to list checkins
     here? */
  int rc = fsl_db_each(db, fsl_stmt_each_f_ambiguous, fcli.f,
              "SELECT uuid, CASE "
              "WHEN type='ci' THEN 'Checkin' "
              "WHEN type='w'  THEN 'Wiki' "
              "WHEN type='g'  THEN 'Control' "
              "WHEN type='e'  THEN 'Technote' "
              "WHEN type='t'  THEN 'Ticket' "
              "WHEN type='f'  THEN 'Forum' "
              "ELSE '?'||'?'||'?' END " /* '???' ==> trigraph! */
              "FROM blob b, event e WHERE uuid LIKE %Q||'%%' "
              "AND b.rid=e.objid "
              "ORDER BY uuid",
              prefix);
  if(rc){
    fsl_cx_uplift_db_error(fcli.f, db);
    fcli_err_report(false);
  }
}

fsl_db * fcli_db_ckout(void){
  return fcli.f ? fsl_cx_db_ckout(fcli.f) : NULL;
}

fsl_db * fcli_db_repo(void){
  return fcli.f ? fsl_cx_db_repo(fcli.f) : NULL;
}

fsl_db * fcli_needs_ckout(void){
  if(fcli.f) return fsl_needs_ckout(fcli.f);
  fcli_err_set(FSL_RC_NOT_A_CKOUT,
               "No checkout db is opened.");
  return NULL;
}

fsl_db * fcli_needs_repo(void){
  if(fcli.f) return fsl_needs_repo(fcli.f);
  fcli_err_set(FSL_RC_NOT_A_REPO,
               "No repository db is opened.");
  return NULL;
}

int fcli_args_to_vfile_ids(fsl_id_bag * const tgt,
                           fsl_id_t vid,
                           bool relativeToCwd,
                           bool changedFilesOnly){
  if(!fcli.argc){
    return fcli_err_set(FSL_RC_MISUSE,
                        "No file/dir name arguments provided.");
  }
  int rc = 0;
  char const * zName;
  while( !rc && (zName = fcli_next_arg(true))){
    FCLI_V3(("Collecting vfile ID(s) for: %s\n", zName));
    rc = fsl_ckout_vfile_ids(fcli.f, vid, tgt, zName,
                             relativeToCwd, changedFilesOnly);
  }
  return rc;
}

int fcli_fingerprint_check(bool reportImmediately){
  int rc = fsl_ckout_fingerprint_check(fcli.f);
  if(rc && reportImmediately){
    f_out("ERROR: repo/checkout fingerprint mismatch detected. "
          "To recover from this, (fossil close) the current checkout, "
          "then re-open it. Be sure to store any modified files somewhere "
          "safe and restore them after re-opening the repository.\n");
  }
  return rc;
}

char const * fcli_progname(void){
  if(!fcli.appName || !*fcli.appName) return NULL;
  char const * z = fcli.appName;
  char const * zEnd = z + fsl_strlen(z) - 1;
  for( ; zEnd > z; --zEnd ){
    switch((int)*zEnd){
      case (int)'/':
      case (int)'\\':
        return zEnd+1;
      default: break;
    }
  }
  return zEnd;
}

void fcli_diff_colors(fsl_dibu_opt * const tgt, fcli_diff_colors_e theme){
  char const * zIns = 0;
  char const * zEdit = 0;
  char const * zDel = 0;
  char const * zReset = 0;
  switch(theme){
    case FCLI_DIFF_COLORS_RG:
          zIns = "\x1b[32m";
          zEdit = "\x1b[36m";
          zDel = "\x1b[31m";
          zReset = "\x1b[0m";
          break;
    case FCLI_DIFF_COLORS_NONE:
    default: break;
  }
  tgt->ansiColor.insertion = zIns;
  tgt->ansiColor.edit = zEdit;
  tgt->ansiColor.deletion = zDel;
  tgt->ansiColor.reset = zReset;
}

void fcli_dump_stmt_cache(bool forceVerbose){
  int i = 0;
  fsl_stmt * st;
  fsl_db * const db = fsl_cx_db(fcli_cx());
  assert(db);
  for( st = db->impl.stCache.head; st; st = st->impl.next ) ++i;
  f_out("%s(): Cached fsl_stmt count: %d\n", __func__, i);
  if(i>0 && (forceVerbose || fcli_is_verbose()>1)){
    for( i = 1, st = db->impl.stCache.head; st; ++i, st = st->impl.next ){
      f_out("CACHED fsl_stmt #%d (%d hit(s)): %b\n", i,
            (int)st->impl.cachedHits, &st->sql);
    }
  }
}

void fcli_dump_cache_metrics(void){
  fsl_cx * const f = fcli.f;
  if(!f) return;
  f_out("fsl_cx::cache::mcache hits = %u misses = %u\n",
        f->cache.mcache.hits,
        f->cache.mcache.misses);
  f_out("fsl_cx::cache::blobContent hits = %u misses = %u. "
        "Entry count=%u totaling %u byte(s).\n",
        f->cache.blobContent.metrics.hits,
        f->cache.blobContent.metrics.misses,
        f->cache.blobContent.used,
        f->cache.blobContent.szTotal);

#define DBN(MEMBER) \
  f_out("fsl_cx::db::" # MEMBER " = %-10d\n", (int)f->db.MEMBER)
  DBN(peakTxnLevel);
  DBN(nBegin);
  DBN(nCommit);
  DBN(nRollback);
#undef DBN

  if( f->metrics.content.nCached ){
    f_out("fsl_cx::cache::fileContent buffer reused %"
          FSL_SIZE_T_PFMT " times " "with a total of %"
          FSL_SIZE_T_PFMT " bytes reported used at "
          "buffer-relinquish time. Peak content buffer size=%"
          FSL_SIZE_T_PFMT "\n",
          f->metrics.content.nCached, f->metrics.content.nTotalUsed,
          f->metrics.content.nPeakBufSize);
    if( f->metrics.content.nCappedMaxSize ){
      f_out("fsl_cx::cache::fileContent was trimmed %" FSL_SIZE_T_PFMT
            " time(s)\n", f->metrics.content.nCappedMaxSize );
    }
  }
}

char const * fcli_fossil_binary(bool errIfNotFound, int reportPolicy){
  static bool once = false;
  if(!once){
    int rc = 0;
    char const * path = getenv("PATH");
    if(path && *path){
      rc = fsl_pathfinder_split(&fcli.paths.bins, true, path, -1);
    }
    if(0==rc){
      fsl_pathfinder_ext_add2(&fcli.paths.bins,".exe", 4);
    }
    once = true;
  }
  char const * z = NULL;
  fsl_pathfinder_search(&fcli.paths.bins, "fossil", &z, NULL);
  if(!z && errIfNotFound){
    fcli_err_set(FSL_RC_NOT_FOUND,
                 "Fossil binary not found in $PATH.");
    if(reportPolicy){
      fcli_err_report(reportPolicy>0);
    }
  }
  return z;
}

static int fcli__transaction_check(void){
  if(fsl_cx_txn_level(fcli.f)){
    return fcli_err_set(FSL_RC_LOCKED,
                        "Sync cannot succeed if a transaction "
                        "is opened. Close all transactions before "
                        "calling %s().", __func__);
  }
  return 0;
}

static bool fcli__autosync_setting(void){
  return fsl_configs_get_bool(fcli.f, "crg",
                              fsl_configs_get_bool(fcli.f, "crg",
                                                   false, "autosync"),
                              "fcli.autosync");
}

int fcli_sync( int ops ){
  int rc = 0;
  if((rc = fcli__transaction_check())) return rc;

  int doPush = -1;
  int doPull = -1;
  char const * zSuppressOut = "";
  fsl_db * const dbR = fsl_needs_repo(fcli.f);
  if(!dbR){
    return FSL_RC_NOT_A_REPO;
  }else if(!fsl_db_exists(dbR, "select 1 from config "
                          "where name='last-sync-url' or "
                          "name like 'syncwith:%%'" )){
    /* No remote, so nothing to do (and any attempt would fail). */
    return 0;
  }
  if(FCLI_SYNC_PULL & ops){
    doPull = 1;
  }
  if(FCLI_SYNC_PUSH & ops){
    doPush = 1;
  }
#if !FSL_PLATFORM_IS_WINDOWS
  if(FCLI_SYNC_NO_OUTPUT & ops){
    zSuppressOut = " >/dev/null 2>&1";
  }else if(FCLI_SYNC_NO_STDOUT & ops){
    zSuppressOut = " >/dev/null";
  }
#endif
  bool const autosync = fcli__autosync_setting();
  if(!autosync && (FCLI_SYNC_AUTO & ops)){
    return 0;
  }
  if(doPull<=0 && doPush<=0){
    return 0;
  }
  char const * zCmd;
  char const * fslBin;
  if(doPull>0 && doPush>0) zCmd = "sync";
  else if(doPull>0) zCmd = "pull";
  else{
    assert(doPush>0);
    zCmd = "push";
  }
  fslBin = fcli_fossil_binary(true, 0);
  if(!fslBin){
    assert(fcli__error->code);
    return fcli__error->code;
  }
  ;
  char * cmd = fsl_mprintf("%s %s%s", fslBin, zCmd, zSuppressOut);
  rc = fsl_system(cmd);
  if(rc){
    extern void fsl__cx_caches_reset(fsl_cx *, bool);
    fsl__cx_caches_reset(fcli.f, true);
    rc = fcli_err_set(rc, "Command exited with non-0 result: %s", cmd);
  }
  fsl_free(cmd);
  return rc;
}


int fsl_msg_f_fcli(fsl_msg const *msg, void *state){
  (void)state;
  char const * z = 0;
  fsl_size_t n = 0;
  char const *zType = 0;
  static enum fsl_msg_e prevType = FSL_MSG_INVALID;
  switch(msg->type){
#define E(N,V,P) case FSL_MSG_ ## N:  zType = "fsl_msg " # N; break;
      fsl_msg_map(E)
#undef E
  }

  switch(msg->type){
    default: break;
    case FSL_MSG_RCV_BLOB:
      /* We can get many thousands of these during sync */
      goto end;
    case FSL_MSG_REBUILD_STEP:{
      fsl_rebuild_step const * const step = msg->payload;
      if( FSL_MSG_REBUILD_STEP==prevType ){
        f_out("\r");
      }
      f_out("Rebuild %.2f%% of %" FSL_SIZE_T_PFMT " steps",
            (double)step->stepNumber / step->artifactCount * 100,
            step->artifactCount);
      goto end;
    }
    case FSL_MSG_REBUILD_DONE:{
      fsl_rebuild_step const * const step = msg->payload;
      if( step->errCode ){
        f_out("\rRebuild failed with code %R%.40c\n", step->errCode,
              ' '/*erase trailing REBUILD_STEP parts*/);
      }else{
        f_out("\rRebuild complete in %" FSL_SIZE_T_PFMT " steps.%20c\n",
              step->stepNumber,
              ' '/*erase trailing REBUILD_STEP parts*/);
      }
      goto end;
    }
    case FSL_MSG_TXN_BEGIN:
    case FSL_MSG_TXN_COMMIT:
    case FSL_MSG_TXN_ROLLBACK:
      if( fcli.config.verbose>2 ){
        f_out("%s%s level %" FSL_SIZE_T_PFMT "\n",
              (FSL_MSG_REBUILD_STEP==prevType)
            ? "\n" : "",
              zType,
              *((fsl_size_t const *)msg->payload));
      }
      return 0 /* don't record these in prevType or rebuild messages
                  get out of whack */;
  }

  switch(FSL_MSG_mask_type & msg->type){
    case FSL_MSG_type_string:
      z = (char const *)msg->payload;
      n = fsl_strlen(z);
      break;
    case FSL_MSG_type_buffer:
      z = fsl_buffer_cstr2(msg->payload, &n);
      break;
    case FSL_MSG_type_blob:{
      fsl_msg_blob const * const f = msg->payload;
      z = f->hash;
      n = fsl_strlen(z);
      break;
    }
    default:
      switch(FSL_MSG_mask_type & msg->type){
#define E(N,V,P) case FSL_MSG_ ## N:    \
        z = "unhandled message type: " # N; \
        n = fsl_strlen(z); \
        break;
      fsl_msg_map(E)
#undef E
      }
      break;
  }

  switch(FSL_MSG_mask_type & msg->type){
    case FSL_MSG_type_string:
    case FSL_MSG_type_buffer:
    case FSL_MSG_type_blob:
      f_out("%s: %.*s\n", zType, (int)n, z);
      break;
    default:
      switch( msg->type ){
        case FSL_MSG_CONNECT:
          f_out("%s: starting trip #%" PRIi64 "\n",
                zType, *((fsl_size_t*)msg->payload));
          break;
        case FSL_MSG_ERROR:
          f_out("%s: %.*s\n", zType, (int)n, z);
          break;
        default:
          f_out("MSG: UNHANDLED TYPE %s\n", zType);
      }
      break;
  }
  end:
  prevType = msg->type;
  return 0;
}

#undef FCLI_V3
#undef fcli_empty_m
#undef fcli__error
#undef MARKER
#undef FCLI_USE_SIGACTION
