/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2025 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2025 The Libfossil Authors
  SPDX-FileType: Code
*/
/**
   This app imlements a "clone" operation for remote fossil repositories.
*/

/*
  We currently rely on internal APIs. That will change.
*/
#include "fossil-scm/config.h"
#include "fossil-scm/core.h"
#include "fossil-scm/sync.h"
#include "fossil-scm/cli.h"
#include "fossil-scm/util.h"
#include "fossil-scm/internal.h"

#include <string.h>

// Only for testing/debugging..
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)


static void xfer_debug(fsl_xfer * xf, char const *zMsg){
  (void)xf;
  //f_out("%s"/*expecting \n via fsl_sc_debug()*/, zMsg);
  assert(zMsg);
  fwrite( zMsg, fsl_strlen(zMsg), 1, stdout );
}

struct {
  /* -u|--url flag */
  bool xferDebug;
  bool leaveTempFiles;
  bool compressResponses;
  bool verboseXf;
  bool traceChannel;
  int cloneMode;
  fsl_clone_config cc;
} App = {
  .xferDebug = false,
  .leaveTempFiles = false,
  .compressResponses = false,
  .verboseXf = false,
  .traceChannel = false,
  .cloneMode = 1,
  .cc = fsl_clone_config_empty_m
};

static int play_with_clone(void){
  fsl_xfer_config * const xc = &App.cc.xfer;
  fsl_outputer fout = {
    .state = stdout,
    .out = fsl_output_f_FILE
  };
  assert( xc->url );
  assert( App.cloneMode );
  App.cc.repo.commitMessage = NULL /* suppress initial commit for new repo */;
  xc->leaveTempFiles = App.leaveTempFiles;
  xc->op.clone = (short)App.cloneMode;
  xc->metrics.callback = fsl_xfer_metrics_f_outputer;
  xc->metrics.state = &fout;
  xc->compressTraffic = App.compressResponses;
  xc->listener.callback = fsl_msg_f_fcli;
  xc->password.save = true;
  //xc->password.callback = fsl_pw_f_getpass;
  if( App.traceChannel ){
    xc->trace.mask = FSL_SC_TRACER_default;
    xc->trace.outputer.out = fsl_output_f_FILE;
    xc->trace.outputer.flush = fsl_flush_f_FILE;
    xc->trace.outputer.state = stdout;
  }
  if( App.xferDebug ){
    xc->debug.callback = xfer_debug;
  }
  fsl_error * const err = fsl_cx_err_e(fcli_cx());
  return fsl_clone( &App.cc, err );
}

int main(int argc, const char * const * argv ){
  bool showSizeofs = false;
  const fcli_cliflag FCliFlags[] = {
    FCLI_FLAG("u", "url", "URL", &App.cc.xfer.url,
              "URL to clone. May optionally be the first non-flag argument."),
    FCLI_FLAG("o", "output", "FILE", &App.cc.repo.filename,
              "Target file. May optionally be the second non-flag argument."),
    FCLI_FLAG_BOOL(NULL, "force", &App.cc.repo.allowOverwrite,
                   "Overwrite target file if it exists"),
    FCLI_FLAG_BOOL(NULL, "sc-debug", &App.xferDebug,
                   "Enable fsl_sc debug output."),
    FCLI_FLAG_BOOL(NULL, "tmp", &App.leaveTempFiles,
                   "Do not delete temp files."),
    FCLI_FLAG_BOOL("t", "sc-trace", &App.traceChannel,
                   "Trace (some of) the fsl_sc method calls."),
    //FCLI_FLAG_BOOL("xfv", "xf-verbose", &App.verboseXf,
    //               "Enable verbose fsl__xfer output. May generate LOTS of output."),
    FCLI_FLAG_I32("c", "clone", "1|2|3", &App.cloneMode,
                  "Clone mode: 1=partial, 2-3=full"),
    FCLI_FLAG_BOOL("z", "compress-responses", &App.compressResponses,
                   "Request compressed responses"),
    FCLI_FLAG_BOOL("so", "sizeof", &showSizeofs,
                   "Show sizeof() for some sync-related types and exit"),
    fcli_cliflag_empty_m // list MUST end with this (or equivalent)
  };
  const fcli_help_info FCliHelp = {
    "A test app for libfossil's sync API",
    "URL outfile",
    NULL // optional callback which outputs app-specific help
  };
  fcli.config.checkoutDir = NULL; // disable automatic checkout-open
  fcli.config.listener.callback = fsl_msg_f_fcli;
  int rc;

  MARKER(("This is an under-construction app. Do not use it.\n"));

  rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if( rc || (rc=fcli_has_unused_flags(false)) ){
    goto end;
  }

  if( showSizeofs ) goto show_sizeofs;


  if( ! App.cc.xfer.url ){
    App.cc.xfer.url = fcli_next_arg(true);
    if( ! App.cc.xfer.url ){
      rc = fcli_err_set(FSL_RC_MISUSE, "No URL specified");
      goto end;
    }
  }

  if( ! App.cc.repo.filename ){
    App.cc.repo.filename = fcli_next_arg(true);
    if( ! App.cc.repo.filename ){
      rc = fcli_err_set(FSL_RC_MISUSE, "No output file specified");
      goto end;
    }
  }

  if( (rc=fcli_has_unused_flags(false)) ) goto end;

  rc = play_with_clone();

show_sizeofs:
  if( showSizeofs ){
#define SO(T) f_out("sizeof(" #T ") = %u\n", (unsigned)sizeof(T))
    SO(fsl_sc);
    SO(fsl_sc_popen_state);
    SO(fsl__xfer);
    SO(fsl_xfer_config);
    SO(fsl_xfer_metrics);
#undef SO
  }

end:

  if( fcli_cx() && fsl_cx_txn_level(fcli_cx()) ){
    f_out("This app is not ready for prime-time: rolling back all changes\n");
    fsl_cx_txn_end(fcli_cx(), true);
  }
  return fcli_end_of_main(rc);
}
