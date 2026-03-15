/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
** 2025-07-27
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
*/
/**
   This app has nothing at all to do with libfossil. It's just a convenient
   place for me to test out some code.
*/
/* Force assert() to always be in effect. */
#undef NDEBUG
#include <assert.h>
#include <string.h> /* memset() */
#include <stdio.h>  /* FILE* */
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>

/* Optional */
#include <unistd.h> /* read(), write() */

#include "libfossil.h"
#include "fossil-scm/internal.h"

// Only for testing/debugging..
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)


static int app_stuff(void){
  int rc = 0;
  fsl_buffer b_ = fsl_buffer_empty;
  fsl_buffer * const b = &b_;
  fsl_cx * const f = fcli_cx();
#define RC \
  if(rc) {MARKER(("rc=%s\n", fsl_rc_cstr(rc))); }   \
  assert( 0==rc )

  char const * zIn =
    "hi\n"
    "unknown={{unknown keyword}}\n"
    "repo.db={{repo.db}}\n"
    "ckout.dir={{ckout.dir}}\n"
    "ckout.dir/={{ckout.dir/}}\n"
    "user.name={{user.name}}\n"
    "nope={{nope} malformed tag\n"
    "repo.db={{repo.db}}\n"
    //"bye\n"
    ;

  f_out("Raw input string:\n%s\n", zIn);
  rc = fsl_cx_format_buffer(f, b, zIn);
  RC;
  f_out("Formatted to a buffer=\n%b\n", b);

  f_out("Via fsl_outputf():\n");
  f_out(zIn);


#undef RC
  fsl_buffer_clear(b);
  return rc;
}

int main(int argc, const char * const * argv ){
  const fcli_cliflag FCliFlags[] = {
    fcli_cliflag_empty_m // list MUST end with this (or equivalent)
  };
  const fcli_help_info FCliHelp = {
    "A random experiment.",
    NULL, // very brief usage text, e.g. "file1 [...fileN]"
    NULL // optional callback which outputs app-specific help
  };

  //fcli.config.checkoutDir = NULL; // same effect as global -C flag.
  int rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  if((rc=fcli_has_unused_args(false))) goto end;

  MARKER(("This is an incomplete scratchpad, not a working program\n"));

  rc = app_stuff();

end:
  return fcli_end_of_main(rc);

}
