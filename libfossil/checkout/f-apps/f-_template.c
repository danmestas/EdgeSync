/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code
*/
/**
   This is a template application for libfossil fcli client apps, with
   commentary explaining how to do various common things with the
   API. Copy/paste this and modify it to suit.
*/
#include "libfossil.h"

// Only for testing/debugging..
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

// Global app state.
struct App_ {
  bool flag1;
  bool flag2;
  char const * flag3;
  char const * fileArg1;
  int32_t flagInt;
} App = {
false,//flag1
true,//flag2
NULL,//flag3
NULL,//fileArg1
-1//flagInt
};

static int app_stuff(void){
  int rc = 0;
  // do... app... stuff...
  MARKER(("App.flag1=%d\n", App.flag1));
  MARKER(("App.flag2=%d\n", App.flag2));
  MARKER(("App.flag3=%s\n", App.flag3));
  MARKER(("App.flagInt=%"PRIi32"\n", App.flagInt));
  MARKER(("App.fileArg1=%s\n", App.fileArg1));
  return rc;
}

static int fcli_flag_callback_f_int(fcli_cliflag const *f){
  (void)f;
  MARKER(("Flag callback: App.flagInt = %"PRIi32"\n", App.flagInt));
  return 0;
}

int main(int argc, const char * const * argv ){
  /**
     Set up flag handling, which is used for processing
     basic CLI flags and generating --help text output.
  */
  const fcli_cliflag FCliFlags[] = {
    // FCLI_FLAG_xxx macros are convenience forms for initializing
    // these members...
    FCLI_FLAG_BOOL("1","flag1",&App.flag1,"Flag 1."),
    FCLI_FLAG_BOOL_INVERT(NULL,"2",&App.flag2,"Flag 2."),
    FCLI_FLAG("3",NULL,"string",&App.flag3,"Flag 3"),
    FCLI_FLAG("f","file","filename",NULL,
              // NULL 4th argument  ^^^^ means this flag is only
              // used for --help text generation and handling
              // the flag is left to the app (see below).
              "Input file. May optionally be passed as the first "
              "non-flag argument."),
    FCLI_FLAG_I32_X("i","int",NULL,&App.flagInt,
                    fcli_flag_callback_f_int,
                    "Integer flag"),
    fcli_cliflag_empty_m // list MUST end with this (or equivalent)
  };
  const fcli_help_info FCliHelp = {
    "Replace this text with a brief description of the app.",
    NULL, // very brief usage text, e.g. "file1 [...fileN]"
    NULL // optional callback which outputs app-specific help
  };
  //fcli.config.checkoutDir = NULL; // disable automatic checkout-open
  //Invoke this app with -? -? to see the global options.

  /**
     Using fsl_malloc() before calling fcli_setup() IS VERBOTEN. fcli
     swaps out the allocator with a fail-fast one, meaning that if an
     allocation fails, the app crashes. This frees up the client app
     from much of the tedium of dealing with allocation errors (which,
     in practice, "never happen").
  */
  int rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;

  /**
     If you need to do any custom flags handling, do it after
     fcli_setup() here...
  */
  if(!fcli_flag_or_arg("f","file", &App.fileArg1)){
    rc = fcli_err_set(FSL_RC_MISUSE,
                      "Missing required filename argument. Try --help.");
    goto end;
  }

  /**
     After all args processing, check for extra/unused flags
     with:
  */
  // if((rc=fcli_has_unused_flags(false))) goto end;
  // Or, more generally:
  if((rc=fcli_has_unused_args(false))) goto end;

  /** Check for a repository (if needed) with: */
  if(!fsl_needs_repo(fcli_cx())){
    /* Sets the context's error state and will produce an appropriate
       error message from fcli_end_of_main(). */
    goto end;
    /**
       Similarly, fsl_needs_ckout() checks for an opened checkout db
       (which also implies a repository: we never open a checkout
       without its corresponding repo).
    */
  }

  /** Perform your app-specific work... */
  rc = app_stuff();

  end:
  return fcli_end_of_main(rc)
    /* Will report any pending error state and return either
       EXIT_SUCCESS or EXIT_FAILURE. */;
  /* Sidebar: all of the memory allocated by this demo app is
     owned by the fcli internals and will be properly cleaned up
     during the at-exit phase. Running this app through valgrind
     should report no memory leaks. */
}
