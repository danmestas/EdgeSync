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

int main(int argc, const char * const * argv ){
  fsl_buffer buf = fsl_buffer_empty;
  const fcli_cliflag FCliFlags[] = {
    fcli_cliflag_empty_m // list MUST end with this (or equivalent)
  };
  const fcli_help_info FCliHelp = {
    "Hashes input files",
    "file1 [...fileN]",
    NULL // optional callback which outputs app-specific help
  };
  fcli.config.checkoutDir = NULL;
  int rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;
  char const * zFlag;

  if( !fcli_next_arg(false) ){
    rc = fcli_err_set(FSL_RC_MISUSE, "Usage: %s file1 [...fileN]",
                      argv[0]);
    goto end;
  }
#define RC(HASH)                                 \
    if( rc ){ f_out("ERROR: %R\n", rc); break; } \
    f_out( "%-4s %-64b %s\n", #HASH, &buf, zFlag ); \
    fsl_buffer_reuse(&buf)
  while( 0!=(zFlag = fcli_next_arg(true)) ){
    rc = fsl_md5sum_filename(zFlag, &buf);
    RC(md5);
    rc = fsl_sha1sum_filename(zFlag, &buf);
    RC(sha1);
    rc = fsl_sha3sum_filename(zFlag, &buf);
    RC(sha3);
  }
#undef RC

end:
  fsl_buffer_clear(&buf);
  return fcli_end_of_main(rc)
    /* Will report any pending error state and return either
       EXIT_SUCCESS or EXIT_FAILURE. */;
  /* Sidebar: all of the memory allocated by this demo app is
     owned by the fcli internals and will be properly cleaned up
     during the at-exit phase. Running this app through valgrind
     should report no memory leaks. */
}
