#include <tcl.h>
#include <assert.h>

#if defined(HAVE_LINENOISE) && (HAVE_LINENOISE+1>=1)
#define USE_LINENOISE
#include "th.h"
#endif

/* The main()-related code was taken directly from sqlite's public
   domain tclsqlite.c, modified only slightly to work here. */

/* This is the main routine for an ordinary TCL shell.  If there are
** arguments, run the first argument as a script.  Otherwise, read TCL
** commands from standard input
*/
static const char *tclsh_main_loop(void){
  static const char zMainloop[] =
    "if {[llength $argv]>=1} {\n"
#ifdef WIN32
      "set new [list]\n"
      "foreach arg $argv {\n"
        "if {[string match -* $arg] || [file exists $arg]} {\n"
          "lappend new $arg\n"
        "} else {\n"
          "set once 0\n"
          "foreach match [lsort [glob -nocomplain $arg]] {\n"
            "lappend new $match\n"
            "set once 1\n"
          "}\n"
          "if {!$once} {lappend new $arg}\n"
        "}\n"
      "}\n"
      "set argv $new\n"
      "unset new\n"
#endif
      "set argv0 [lindex $argv 0]\n"
      "set argv [lrange $argv 1 end]\n"
      "source $argv0\n"
    "} else {\n"
#if defined(USE_LINENOISE)
      "linenoise -history-load -noerr\n"
      "while {{<EOF>} ne [linenoise --var line]} {\n"
        "if {{} eq $line} continue\n"
        "if {[catch {uplevel #0 $line} result]} {\n"
          "puts stderr \"Error: $result\"\n"
        "} elseif {$result!=\"\"} {\n"
          "puts $result\n"
        "}\n"
      "}\n"
      "linenoise -history-save -noerr\n"
#else
      "set line {}\n"
      "while {![eof stdin]} {\n"
        "if {$line!=\"\"} {\n"
          "puts -nonewline \"> \"\n"
        "} else {\n"
          "puts -nonewline \"% \"\n"
        "}\n"
        "flush stdout\n"
        "append line [gets stdin]\n"
        "if {[info complete $line]} {\n"
          "if {[catch {uplevel #0 $line} result]} {\n"
            "puts stderr \"Error: $result\"\n"
          "} elseif {$result!=\"\"} {\n"
            "puts $result\n"
          "}\n"
          "set line {}\n"
        "} else {\n"
          "append line \\n\n"
        "}\n"
      "}\n"
#endif
    "}\n"
  ;
  return zMainloop;
}

#if defined(USE_LINENOISE)
/**
   The following linenoise binding was implemented as part of the
   libfossil project:

   https://fossil.wanderinghorse.net/r/libfossil

   And is based on Steve Bennett's variant of linenoise:

   https://github.com/msteveb/linenoise

   2025 August 12

   The author disclaims copyright to this source code.  In place of a
   legal notice, here is a blessing:

    May you do good and not evil.
    May you find forgiveness for yourself and forgive others.
    May you share freely, never taking more than you give.
*/
#include "linenoise.h"
#include <stdlib.h>
#include <stdio.h> /* snprintf() */

/**
   Name of a Tcl var to store the name of the linenoise
   history file in.
*/
#define LN_HISTORY_FILE_VAR "::linenoise(history-file)"

/**
   linenoise ?flags?

   Invoking it with no flags which "do work" is equivalent to invoking
   it with --prompt {> }.

   When prompting for input, it returns the input string, or "<EOF>"
   if ^D (EOF) was tapped.

   Flags:

   -eof: returns 1 if ^D (EOF) was tapped at the the prompt.  This
   gets reset if it's invoked again for input.

   --prompt STR: prompts for input with the given prompt string.

   --history-save FILE: saves the history and, on success, sets
   ::linenoise(history-file) to the given name.

   -history-save: saves the history to the current value of
   $::linenoise(history-file).

   --history-load FILE: loads the history from the given file and, on
   success, sets ::linenoise(history-file) to the given name.

   -history-save: saves the history to the current value of
   $::linenoise(history-file).

   --var varName: set the prompted-for value in the given var name.

   -noerr: do not throw an error if loading or saving of history fail.

   -clear: clear the history.

   -noadd: do not add the prompted-for value to the history.

   -?: return a brief usage overview.

   Flags can mostly be used together, except for -eof, which
   trumps all others.

   At startup it looks for a history file in:

   - The environment variable $LINENOISE_TCL_HISTORY_FILE.

   - The name of the current app with a ".history" extension.

   The name it decides upon is set in $::linenoise(history-file).  It
   does NOT automatically try to load history from that file, but that
   behavior is arguable.

   Example usage:

     linenoise -history-load -noerr
     while {{<EOF>} ne [linenoise -var line --prompt {foo> }]} {
       puts "got: $line"
     }
     linenoise -history-save -noerr

   That loop will end when the EOF sequence (typically ^D) is tapped
   while linenoise awaits input.
*/
static int TclLinenoise_Cmd(ClientData cx, Tcl_Interp *tcl, int argc,
                            Tcl_Obj *const* argv){
  static int isEof = 0;
  th_decl_cmdflags_init;
  th_decl_cmdflag("-eof", 0,  0, fEof );
  th_decl_cmdflag("--prompt", 0,  0, fPrompt );
  th_decl_cmdflag("-history-load", 0,  0, fLoad0 );
  th_decl_cmdflag("-history-save", 0,  0, fSave0 );
  th_decl_cmdflag("-noerr", 0,  0, fNoErr );
  th_decl_cmdflag("-clear", 0,  0, fClear );
  th_decl_cmdflag("-noadd", 0,  0, fNoAdd );
  th_decl_cmdflag("--var", "varName",  0, fVarName );
  th_decl_cmdflag("--history-load", "filename",  0, fLoad );
  th_decl_cmdflag("--history-save", "filename",  0, fSave );
  th_decl_cmdflag("-?", 0,  0, felp );
  th_decl_cmdflags(fp,0);
  char const *zPrompt = 0;
  char const *zHFile = 0;
  int didSomething = 0;
  int rc = th_flags_parse(tcl, argc, argv, &fp);
  if( rc ) goto end;
  rc = th_check_help(tcl, argv[0], &felp, &fp, NULL);
  if( TCL_BREAK==rc ){
    rc = 0;
    goto end;
  }

  if( fp.nonFlag.count ){
    char buf[1024];
    rc = snprintf(buf, sizeof(buf), "usage: %s ", th_gs1(argv[0]));
    th_flags_generate_help(&fp, &buf[rc], sizeof(buf)-rc);
    rc = th_err(tcl, buf);
    goto end;
  }

  if( fEof.nSeen ){
    th_rs(tcl, 0, isEof ? "1" : "0");
    goto end;
  }

  if( fSave.nSeen ){
    zHFile = th_gs1(fSave.pVal);
    if( linenoiseHistorySave(zHFile) && !fNoErr.nSeen ){
      rc = th_err(tcl, "Error saving history file %s", zHFile);
      goto end;
    }
    Tcl_SetVar(tcl, LN_HISTORY_FILE_VAR, zHFile, TCL_GLOBAL_ONLY);
    ++didSomething;
  }

  if( fLoad.nSeen ){
    char const * zHFile = th_gs1(fLoad.pVal);
    if( zHFile && linenoiseHistoryLoad(zHFile) && !fNoErr.nSeen ){
      rc = th_err(tcl, "Error loading history file %s", zHFile);
      goto end;
    }
    ++didSomething;
  }

  if( fClear.nSeen ){
    linenoiseHistoryFree();
    ++didSomething;
  }

  if( fLoad0.nSeen || fSave0.nSeen ){
    zHFile = Tcl_GetVar(tcl, LN_HISTORY_FILE_VAR, 0);
    if( zHFile ){
      if( fSave0.nSeen ){
        if( linenoiseHistorySave(zHFile)
            && !fNoErr.nSeen ){
          rc = th_err(tcl, "Error saving history file %s", zHFile);
          goto end;
        }
        Tcl_SetVar(tcl, LN_HISTORY_FILE_VAR, zHFile, TCL_GLOBAL_ONLY);
      }
      if( fLoad0.nSeen
          && linenoiseHistoryLoad(zHFile)
          && !fNoErr.nSeen ){
        rc = th_err(tcl, "Error loading history file %s", zHFile);
        goto end;
      }
      ++didSomething;
    }
  }

  if( fPrompt.nSeen || !didSomething ){
    isEof = 0;
    zPrompt = fPrompt.pVal ? th_gs1(fPrompt.pVal) : "> ";
    char * z = linenoise(zPrompt);
    isEof = NULL==z;
    th_rs_c(tcl, 0, isEof ? "<EOF>" : z);
    if( !isEof && !fNoAdd.nSeen ){
      linenoiseHistoryAdd(z);
    }
    if( fVarName.nSeen ){
      Tcl_SetVar(tcl, th_gs1(fVarName.pVal), z, 0);
    }
    free(z);
    ++didSomething;
  }

  if( didSomething ) goto end;
  Tcl_WrongNumArgs(tcl, 1, argv, "-?");
  rc = TCL_ERROR;
end:
  th_flags_parse_cleanup(&fp);
  return rc;
}

static int init_linenoise(Tcl_Interp *tcl, char const * argv0){
  int rc = Tcl_PkgProvide(tcl, "linenoise", "0.1");
  if( !rc ){
    Tcl_CreateObjCommand(tcl, "linenoise", TclLinenoise_Cmd, NULL, NULL);
  }
  if( 0==rc ){
    char buf[1024] = {0};
    char const * z = getenv("LINENOISE_TCL_HISTORY_FILE");
    if( !z ){
      rc = snprintf(buf, sizeof(buf), "%s.history", argv0);
      if( rc<(int)sizeof(buf)-1 ) {
        z = &buf[0];
      }
      rc = 0;
    }
    if( z ){
      Tcl_SetVar(tcl, LN_HISTORY_FILE_VAR, z, TCL_GLOBAL_ONLY);
      //linenoiseHistoryLoad(z);
    }
  }
  if( 0==rc ){
    linenoiseSetMultiLine(1);
  }
  return rc;
}
#ifdef _WIN32
__declspec(dllexport)
#endif

#if 0
/* To turn this into a loadable module... */
int DLLEXPORT Linenoise_Init(Tcl_Interp *tcl){
  if( NULL == Tcl_InitStubs(tcl, TCL_VERSION, 0) ) {
    return TCL_ERROR;
  }
  return init_linenoise(tcl, Tcl_GetNameOfExecutable());
}
#endif

#undef LN_HISTORY_FILE_VAR
#endif /* USE_LINENOISE */

#ifndef FTCL_MAIN
# define FTCL_MAIN main
#endif
int FTCL_MAIN(int argc, char **argv){
  Tcl_Interp *tcl;
  int rc = 0;
  int i;
  const char *zScript = 0;
  char zArgc[32];
#if defined(FTCL_INIT_PROC)
  extern const char *FTCL_INIT_PROC(Tcl_Interp*);
#endif

  Tcl_FindExecutable(argv[0]);
  Tcl_SetSystemEncoding(NULL, "utf-8");
  tcl = Tcl_CreateInterp();
  if( (rc = Tcl_Init(tcl)) ){
    goto end;
  }
  extern int Libfossil_Init(Tcl_Interp *tcl)/*in ftcl.c*/;
  if((rc = Libfossil_Init(tcl))){
    goto end;
  }

  snprintf(zArgc, sizeof(zArgc), "%d", argc-1);
  Tcl_SetVar(tcl,"argc", zArgc, TCL_GLOBAL_ONLY);
  Tcl_SetVar(tcl,"argv0",argv[0],TCL_GLOBAL_ONLY);
  Tcl_SetVar(tcl,"argv", "", TCL_GLOBAL_ONLY);
  for(i=1; i<argc; i++){
    Tcl_SetVar(tcl, "argv", argv[i],
        TCL_GLOBAL_ONLY | TCL_LIST_ELEMENT | TCL_APPEND_VALUE);
  }
#if defined(FTCL_INIT_PROC)
  zScript = FTCL_INIT_PROC(tcl);
#endif
#if defined(USE_LINENOISE)
  if( 0!=(rc = init_linenoise(tcl, argv[0])) ){
    goto end;
  }
#endif
  if( zScript==0 ){
    zScript = tclsh_main_loop();
  }

end:
  if( rc || Tcl_GlobalEval(tcl, zScript)!=TCL_OK ){
    const char *zInfo = Tcl_GetVar(tcl, "errorInfo", TCL_GLOBAL_ONLY);
    if( zInfo==0 ) zInfo = Tcl_GetStringResult(tcl);
    fprintf(stderr,"%s: %s\n", *argv, zInfo);
    rc = 1;
  }
  extern void ftcl__atexit()/*in ftcl.c*/;
  ftcl__atexit();
  Tcl_DeleteInterp(tcl);
  Tcl_Finalize() /* does not solve Tcl-level leaks */;
  //Tcl_Exit(rc) /* nor does that */; assert(!"not reached");
  return rc;
}
