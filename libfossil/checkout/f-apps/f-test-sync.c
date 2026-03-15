/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2025 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2025 The Libfossil Authors
  SPDX-FileType: Code
*/
/**
   This is a test app for fsl_sync_client() and friends.
*/
#include "fossil-scm/config.h"
#include "fossil-scm/core.h"
#include "fossil-scm/sync.h"
#include "fossil-scm/cli.h"
#include "fossil-scm/util.h"
#include "fossil-scm/internal.h"

#include <string.h>
#if FSL_PLATFORM_IS_UNIX
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#else
#  error "don't know which #include to use for open(2) and read(2)."
#endif

// Only for testing/debugging..
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

int dummy_status(fsl_sc *ch, fsl_sc_msg const *msg){
  (void)ch;
  switch( msg->type ){
    case FSL_SC_MSG_FROM_REMOTE:
      f_out("MSG_FROM_REMOTE: %s\n", msg->text);
      break;
    case FSL_SC_MSG_INFO:
      f_out("MSG_INFO: %s\n", msg->text);
      break;
    default:
      /*MARKER(("status() %s %s\n", fsl_sc_msg_type_cstr(msg->type),
        msg->text ? msg->text : ""));*/
      break;
  }
  return 0;
}

static void dummy_debug(fsl_sc * ch, char const *zMsg){
  (void)ch;
  f_out("%s"/*expecting \n via fsl_sc_debug()*/, zMsg);
}

struct {
  /* -u|--url flag */
  char const * zUrl;
  char const * zPopenImpl;
  bool debug;
  bool leaveTempFiles;
  bool traceChannel;
  bool shortClone;
  bool compressResponses;
  bool includeResponseHeaders;
  bool includeReqHeaders;
  bool useNoReadln;
  bool verboseXf;
  int cloneMode;
  int responseMode;
} App = {
  .zUrl = 0,//"http://localhost:8080",
  .zPopenImpl = "curl",
  .debug = false,
  .traceChannel = false,
  .leaveTempFiles = false,
  .shortClone = false,
  .compressResponses = false,
  .includeResponseHeaders = false,
  .includeReqHeaders = false,
  .useNoReadln = false,
  .verboseXf = false,
  .cloneMode = 1,
  .responseMode = fsl_sc_popen_e_filebuf
};

static void dump_xfer_metrics(fsl_xfer_metrics const * const m, void *state, int theRc){
  char const * cardNames[] ={
    NULL,
#define E(T,N) # T,
    fsl_xfcard_map(E)
#undef E
  };

  f_out("Sync transfer metrics:\n");
  if( m->bytesWritten ){
    f_out("Bytes written:   %," FSL_SIZE_T_PFMT "\n",
          m->bytesWritten);
  }
  f_out("Cards read:      %," FSL_SIZE_T_PFMT "\n",
        m->rcvdCard);
  if( m->rcvdCard ){
    for( int i = fsl_xfcard_e__unknown + 1; i < fsl_xfcard_e_COUNT; ++i){
      if( m->cardsRx[i] ){
        f_out("    %-12s %," FSL_SIZE_T_PFMT "\n",
              cardNames[i], m->cardsRx[i]);
      }
    }
  }
  if(m->bytesReadUncompressed){
    f_out("Bytes read uncompressed: %," FSL_SIZE_T_PFMT "\n",
          m->bytesReadUncompressed);
  }
  if(m->bytesReadCompressed){
    f_out("Bytes read compressed:   %," FSL_SIZE_T_PFMT "\n",
          m->bytesReadCompressed);
  }
  if( m->largestDecompressedResponse ){
    f_out("Largest decompr. buffer: %," FSL_SIZE_T_PFMT "\n",
          m->largestDecompressedResponse);
  }
  if( m->largestCardPayload ){
    f_out("Largest card payload:    %," FSL_SIZE_T_PFMT "\n",
          m->largestCardPayload);
  }
#if 0
  if( xf->clone.seqNo ){
    f_out("Clone seq. #:            %," FSL_SIZE_T_PFMT "\n",
          xf->clone.seqNo);
  }
#endif
  for( int j = 0; j < 3; ++j ){
    /* m->timer... */
    fsl_timer const * tm = 0;
    char const * zPre = 0;
    switch(j){
      case 0:
        tm = &m->timer.submit;
        zPre = "Wait on submit()";
        break;
      case 1:
        tm = &m->timer.read;
        zPre = "Wait on read()";
        break;
      case 2:
        tm = &m->timer.uncompress;
        zPre = "Wait on uncompress()";
        break;
    }
    assert( tm );
    if( tm->user || tm->system ){
      f_out( "%-24s %-10.3lf ms: %10.3lf user, %-10.3lf system\n",
             zPre,
             (double)((tm->user + tm->system) / 1000.0),
             (double)(tm->user / 1000.0),
             (double)(tm->system / 1000.0) );
    }
  }
}

static int play_with_clone(void){
  int rc;
  fsl__xfer xf = fsl__xfer_empty;
  fsl_sc ch = fsl_sc_popen_curl;
  fsl_sc_popen_state st;
  fsl_sc stTrace = fsl_sc_tracer_empty;
  fsl_size_t cloneSeqNo = 0;
  char const *zPC = 0; /* project-code */
  fsl_xfer_config xc = fsl_xfer_config_empty;

  if( 0==fsl_strcmp("curl",App.zPopenImpl) ){
    st = fsl_sc_popen_state_curl;
    assert( 0==fsl_strcmp(st.bin.name,"curl") );
  }else if( 0==fsl_strcmp("fth",App.zPopenImpl) ){
    st = fsl_sc_popen_state_fth;
  }else{
    rc = fcli_err_set(FSL_RC_MISUSE,
                      "Cannot figure out which popen impl to use.");
    goto end;
  }

  assert( st.impl.buildCommandLine );
  assert( !ch.state.p );
  assert( ch.state.type == &fsl_sc_popen );

  switch (App.responseMode) {
    case fsl_sc_popen_e_direct:
    case fsl_sc_popen_e_fd:
    case fsl_sc_popen_e_filebuf:
    case fsl_sc_popen_e_membuf:
    st.response.mode = (enum fsl_sc_popen_e)App.responseMode;
    break;
  default:
    return fcli_err_set(FSL_RC_RANGE, "Invalid buffer mode: %d",
                        App.responseMode);
  }
  //f_out("url: %s\n", App.zUrl);
  //f_out("response buffer mode: %d\n", st.response.mode);

  ch.f = fcli_cx();
  if( App.debug ){
    ch.debug.callback = dummy_debug;
  }
  //ch.status = { .callback = dummy_status, .state = 0 };

  ch.state.p = &st;
  if( App.compressResponses ) ch.flags |= FSL_SC_F_COMPRESSED;
  if( App.useNoReadln ) ch.flags |= FSL_SC_F_NO_READ_LINE;
  if( App.includeResponseHeaders ) ch.flags |= FSL_SC_F_RESPONSE_HEADERS;
  if( App.includeReqHeaders ) ch.flags |= FSL_SC_F_REQUEST_HEADERS;
  if( App.leaveTempFiles ) ch.flags |= FSL_SC_F_LEAVE_TEMP_FILES;
  if( App.zUrl ){
    rc = fsl_url_parse(&ch.url, App.zUrl);
    if( rc ){
      rc = fcli_err_set(rc, "Error parsing URL: %s", App.zUrl);
      goto end;
    }
  }

  if( App.traceChannel ){
    stTrace.f = ch.f;
    stTrace.flags = ch.flags;
    stTrace.state.p = &ch;
    assert( stTrace.debug.callback );
    stTrace.debug.callback = fsl_sc_debug_f_dummy;
    stTrace.debug.state = stdout;
  }

  xc.verbose = !!App.verboseXf;
  xc.metrics.callback = dump_xfer_metrics;

  rc = fsl_repo_project_code(ch.f, &zPC);
  if( rc ) goto end;
  assert( zPC );

  rc = fsl__xfer_setup(fcli_cx(), &xf, &xc,
                       App.traceChannel ? &stTrace : &ch);
  if( rc ) goto end;

#define sayOnce(fmtexpr) if( 1==xf.n.trips ){ f_out fmtexpr; }(void)0

again:
  cloneSeqNo = xf.clone.seqNo;
  fsl__xfer_start(&xf, FSL_SC_INIT_FULL);
  assert( App.compressResponses ? (FSL_SC_F_COMPRESSED & ch.flags) : 1 );
  if( App.cloneMode ){
    /* TODO: dynamically determine whether we need push and/or pull
       cards: push|pull servercode(IGNORED) project-code. */
    fsl__xfer_appendlnf(&xf, "push ignored-servercode %s", zPC);
  }else if( 0 ){
    fsl__xfer_appendlnf(&xf, "pull ignored-servercode %s", zPC);
  }
  if( !App.cloneMode ){
    fsl__xfer_appendln(&xf, "reqconfig /project");
    fsl__xfer_appendln(&xf, "reqconfig /skin");
    if( xc.op.verily ){
      fsl__xfer_appendln(&xf, "pragma send-catalog");
    }
    if( fsl_repo_forbids_delta_manifests(ch.f) ){
      fsl__xfer_appendln(&xf, "pragma avoid-delta-manifests");
    }
  }
  switch( xf.errCode ? 0 : App.cloneMode ){

    case 2:
    case 3:
      /* Response contains many "file" cards (v2) or "cfile" cards
         (v3). Protocol version 2 may compress the body. Level 3
         compresses the individual "cfile" cards instead. */
      xf.clone.protocolVersion = App.cloneMode;
      fsl__xfer_appendlnf(&xf, "clone %d %" PRIu32, App.cloneMode,
                          xf.clone.seqNo ? xf.clone.seqNo : 1);
      break;

    case 1:
      /** Response contains igot cards but not file/cfile, so no cards
          with bodies. */
      fsl__xfer_appendln(&xf, "clone");
      break;

    case 0:
      break;

    default:
      rc = fcli_err_set(FSL_RC_MISUSE, "Don't know what to do. Try --clone=0|1|2|3");
      goto end;
  }
  //fsl__xfer_appendln(&xf, "pragma uv-sync");
  rc = fsl__xfer_submit(&xf);
  if( 0==rc ){
    if( cloneSeqNo && !xf.clone.seqNo ){
      f_out("\nDone processing %d clone round-trip(s)\n", xf.n.trips);
    }else if( !App.shortClone
              && xf.clone.seqNo
              && cloneSeqNo<xf.clone.seqNo ){
      if( 1 ){
        f_out("Starting round-trip #%d...\n", xf.n.trips+1);
      }else{
        /* Nicer but gets hosed by any debug output */
        f_out("\rStarting round-trip #%d...", xf.n.trips+1);
        fsl_flush( xf.ch->f );
      }
      goto again;
    }
  }
#undef sayOnce
end:
  fsl__xfer_cleanup(&xf, rc);
  assert( !ch.url.raw );
  //assert( !st.response.b.mem );
  return rc;
}

int main(int argc, const char * const * argv ){
  const fcli_cliflag FCliFlags[] = {
    FCLI_FLAG_BOOL(NULL, "sc-debug", &App.debug,
                   "Enable fsl_sc debug output."),
    FCLI_FLAG_BOOL("t", "tmp", &App.leaveTempFiles,
                   "Do not delete temp files."),
    FCLI_FLAG_BOOL("tsc", "sc-trace", &App.traceChannel,
                   "Trace all fsl_sc method calls. "
                   "ACHTUNG: generates many mb of output."),
    FCLI_FLAG_BOOL("h", "response-headers", &App.includeResponseHeaders,
                   "Include HTTP response headers in response payloads."),
    FCLI_FLAG_BOOL("hh", "request-headers", &App.includeReqHeaders,
                   "Generate HTTP request headers (not honored by all impls)."),
    FCLI_FLAG_BOOL("1", "short-clone", &App.shortClone,
                   "Stop cloning after the first response."),
    FCLI_FLAG_BOOL("norl", "no-readln", &App.useNoReadln,
                   "Enable FSL_SC_F_NO_READ_LINE."),
    FCLI_FLAG_BOOL("vxf", "verbose-xf", &App.verboseXf,
                   "Enable verbose fsl__xfer output. May generate LOTS of output."),
    FCLI_FLAG("u", "url", "URL", &App.zUrl,
              "URL for sync test purposes."),
    FCLI_FLAG("pi", "popen-impl", "name", &App.zPopenImpl,
              "Short-form name of fsl_sc_popen_state impl to use"),
    FCLI_FLAG_I32("c", "clone", "0|1|2|3", &App.cloneMode,
                  "Clone mode to test: 0=none, 1=partial, 2-3=full"),
    FCLI_FLAG_I32("b", "buffer-mode", "0|1|2|3", &App.responseMode,
                  "Response buffering mode. Only 1-2 work."),
    FCLI_FLAG_BOOL("z", "compress-responses", &App.compressResponses,
                   "Request compressed responses"),
    fcli_cliflag_empty_m // list MUST end with this (or equivalent)
  };
  const fcli_help_info FCliHelp = {
    "A test app for libfossil's sync API",
    NULL, // very brief usage text, e.g. "file1 [...fileN]"
    NULL // optional callback which outputs app-specific help
  };
  //fcli.config.checkoutDir = NULL; // disable automatic checkout-open
  bool inTransaction = false;
  int rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc
     || (rc=fcli_has_unused_args(false))
     || !fsl_needs_repo(fcli_cx())){
    goto end;
  }

  if( 1 ){
    rc = fcli_err_set(FSL_RC_NYI,
                      "This app was broken by library-side refactoring. "
                      "It will be repaired when the need arises.");
    goto end;
  }

  rc = fsl_cx_txn_begin(fcli_cx());
  if( rc ) goto end;

  inTransaction = true;

  fsl_timer timer = fsl_timer_empty;
  fsl_timer_scope(&timer,{
      rc = play_with_clone();
    });

  f_out("Total CPU time: %.3lf ms: "
        "%.3lf user, %.3lf system\n",
        (timer.user + timer.system)/1000.0,
        timer.user / 1000.0,
        timer.system / 1000.0);

  if( 0 ){
#define SO(T) f_out("sizeof(" #T ") = %u\n", (unsigned)sizeof(T))
    SO(fsl_sc);
    SO(fsl__xfer);
    SO(fsl_xfer_config);
    SO(fsl_xfer_metrics);
#undef SO
  }

end:

  if( inTransaction ){
    fsl_cx_txn_end(fcli_cx(), true);
  }
  return fcli_end_of_main(rc);
}
