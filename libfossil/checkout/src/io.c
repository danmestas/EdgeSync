/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/
/*************************************************************************
  This file implements the generic i/o-related parts of the library.
*/
#include "fossil-scm/internal.h"
#include <assert.h>
#include <errno.h>
#include <string.h> /* memcmp() */

#if FSL_PLATFORM_IS_UNIX
#  include <unistd.h>
#else
#  error "don't know which #include to use for read(2)."
#endif

/* Only for debugging */
#include <stdio.h>
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

/**
    fsl_appendf_f() impl which sends its output to fsl_output(). state
    must be a (fsl_cx*).
 */
static int fsl_output_f_fsl_output( void * state, void const * s,
                                    fsl_size_t n ){
  return fsl_output( (fsl_cx *)state, s, n );
}


int fsl_outputfv( fsl_cx * const f, char const * fmt, va_list args ){
#if FSL_API_ARMOR
  if(!f || !fmt) return FSL_RC_MISUSE;
  else if(!*fmt) return FSL_RC_RANGE;
#endif
  return fsl_cx_formatv( f, fsl_output_f_fsl_output, f, fmt, args );
}

int fsl_outputf( fsl_cx * const f, char const * fmt, ... ){
#if FSL_API_ARMOR
  if(!f || !fmt) return FSL_RC_MISUSE;
  else if(!*fmt) return FSL_RC_RANGE;
#endif
  int rc;
  va_list args;
  va_start(args,fmt);
  rc = fsl_outputfv( f, fmt, args );
  va_end(args);
  return rc;
}

int fsl_output( fsl_cx * const cx, void const * const src, fsl_size_t n ){
  if(!n || !cx->output.out) return 0;
  else return cx->output.out( cx->output.state, src, n );
}

int fsl_flush( fsl_cx * const f ){
  return f->output.flush
       ? f->output.flush(f->output.state)
       : 0;
}


int fsl_flush_f_FILE(void * _FILE){
  return fflush((FILE*)_FILE) ? fsl_errno_to_rc(errno, FSL_RC_IO) : 0;
}

int fsl_output_f_FILE( void * state,
                       void const * src, fsl_size_t n ){
  if(!n) return 0;
  else return (1 == fwrite(src, n, 1, state ? (FILE*)state : stdout))
         ? 0 : FSL_RC_IO;
}


int fsl_output_f_fd( void * state, void const * src, fsl_size_t n ){
  if( !n ) return 0;
#if FSL_API_ARMOR
  else if( !state ) return FSL_RC_MISUSE;
#endif
  int const fd = *((int*)state);
  ssize_t const wn = write( fd, src, n );
  return wn<0
    ? fsl_errno_to_rc(errno, FSL_RC_IO)
    :  0;
}


int fsl_input_f_FILE( void * state, void * dest, fsl_size_t * n ){
  if( !*n ) return FSL_RC_RANGE;
  FILE * f = (FILE*) state;
  fsl_size_t const rn = *n;
  *n = (size_t)fread( dest, 1, rn, f );
  return *n==rn ? 0 : (feof(f) ? 0 : FSL_RC_IO);
}

int fsl_input_f_fd( void * state, void * dest, fsl_size_t * n ){
  if( !*n ) return FSL_RC_RANGE;
  int const fd = *((int*)state);
  ssize_t const rn = read( fd, dest, *n );
  if( rn<0 ){
    return fsl_errno_to_rc(errno, FSL_RC_IO);
  }else{
    *n = (fsl_size_t)rn;
    return 0;
  }
}

void fsl_finalizer_f_FILE( void * state fsl__unused, void * mem ){
  (void)state;
  if(mem){
    fsl_fclose((FILE*)mem);
  }
}

int fsl_stream( fsl_input_f inF, void * inState,
                fsl_output_f outF, void * outState ){
#if FSL_API_ARMOR
  if(!inF || !outF) return FSL_RC_MISUSE;
#endif
  int rc = 0;
  enum { BufSize = 1024 * 4 };
  unsigned char buf[BufSize];
  fsl_size_t rn = BufSize;
  while( 0==rc
         && (rn==BufSize)
         && (0==(rc=inF(inState, buf, &rn))) ){
    if(rn) rc = outF(outState, buf, rn);
  }
  return rc;
}

int fsl_stream_compare( fsl_input_f in1, void * in1State,
                        fsl_input_f in2, void * in2State ){
  enum { BufSize = 1024 * 2 };
  unsigned char buf1[BufSize];
  unsigned char buf2[BufSize];
  fsl_size_t rn1 = BufSize;
  fsl_size_t rn2 = BufSize;
  int rc;
  while(1){
    rc = in1(in1State, buf1, &rn1);
    if(rc) return -1;
    rc = in2(in2State, buf2, &rn2);
    if(rc) return 1;
    else if(rn1!=rn2){
      rc = (rn1<rn2) ? -1 : 1;
      break;
    }
    else if(0==rn1 && 0==rn2) return 0;
    rc = memcmp( buf1, buf2, rn1 );
    if(rc) break;
    rn1 = rn2 = BufSize;
  }
  return rc;
}

#undef MARKER
