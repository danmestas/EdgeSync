/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2025 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2025 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/
/**
  This file contains the fsl_sc impls for the sync transport layer.
  This layer is effectively "client-level", in that fsl_sc is designed
  to be extensible by clients.
*/
#include "fossil-scm/sync.h"
#include "fossil-scm/core.h"
#include "fossil-scm/hash.h"
#include "fossil-scm/auth.h"
#include "fossil-scm/util.h"
#include "fossil-scm/confdb.h"
#include "fossil-scm/internal.h" /* an unfortunate dep */

#include <stdbool.h>
#include <stdio.h> /* FILE class */
#include <assert.h>
#include <stdlib.h> /* bsearch() */
#include <string.h> /* memcpy() */
#include <errno.h>

#if FSL_PLATFORM_IS_UNIX
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#else
#  error "don't know which #include to use for open(2) and read(2)."
#endif

#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

fsl_sc * fsl_sc_self(fsl_sc *ch){
  return ch;
}

char const * fsl_msg_type_cstr(fsl_msg_e e){
  switch(e){
#define E(N,V,D) case FSL_MSG_ ## N: return "FSL_MSG_" # N;
    fsl_msg_map(E)
#undef E
  }
  return NULL;
}

void fsl_sc_cleanup(fsl_sc * const ch){
  if(ch){
    if( ch->cleanup ) ch->cleanup(ch);
    fsl_url_cleanup(&ch->url);
    fsl_buffer_clear(&ch->requestHeaders);
    fsl_buffer_clear(&ch->scratch);
  }
}

#define fsl__sc_popen_decl \
  assert(ch->impl.type == fsl_sc_popen.impl.type); \
  fsl_sc_popen_state * const st = ch->impl.p; \
  assert( st )

static void fsl_sc_popen_unlinkResponseFile(fsl_sc *ch, bool freeIt){
  fsl__sc_popen_decl;
  if( st->response.fp ){
    fsl_fclose(st->response.fp);
    st->response.fp = 0;
    st->response.fd = -1;
  }else if( st->response.fd >= 0 ){
    close( st->response.fd );
    st->response.fd = -1;
  }
  if( st->response.filename ){
    if( ch->flags & FSL_SC_F_LEAVE_TEMP_FILES  ){
      fsl_cx_emit(fsl_xfer_cx(ch->xfer), FSL_MSG_TMPFILE,
                  st->response.filename);
    }else{
      fsl_file_unlink(st->response.filename);
    }
    if( freeIt ){
      fsl_free( st->response.filename );
      st->response.filename = 0;
    }
  }
}

static void fsl_sc_popen_unlinkRequestFile(fsl_sc *ch){
  fsl__sc_popen_decl;
  if( st->request.fp ){
    fsl_fclose( st->request.fp );
    st->request.fp = NULL;
  }
  if( st->request.filename ){
    if( ch->flags & FSL_SC_F_LEAVE_TEMP_FILES ){
      fsl_cx_emit(fsl_xfer_cx(ch->xfer), FSL_MSG_TMPFILE,
                  st->request.filename);
    }else{
      fsl_file_unlink(st->request.filename);
    }
    fsl_free(st->request.filename);
    st->request.filename = 0;
  }
}

static void fsl_sc_popen_pclose(fsl_sc * ch, bool unlinkTmpFiles){
  fsl__sc_popen_decl;
  if( unlinkTmpFiles ){
    fsl_sc_popen_unlinkRequestFile(ch);
    fsl_sc_popen_unlinkResponseFile(ch, true);
  }
  if( st->process.fdStdOut>=0 ){
    fsl__pclose2( st->process.fdStdOut, st->process.fStdIn, st->process.pid );
    st->process.fdStdOut = -1;
    st->process.fStdIn = NULL;
    st->process.pid = 0;
  }
}

#define popenTempFileName(LETTER) \
  fsl_mprintf("%s-%d.%c", st->misc.tmpfilePrefix, (int)st->request.count, LETTER)

static int fsl_sc_popen_initRequestFile(fsl_sc * const ch){
  fsl__sc_popen_decl;
  int rc = 0;

  fsl_sc_popen_unlinkRequestFile(ch);
  ++st->request.count;
  assert( st->misc.tmpfilePrefix );
  st->request.filename = popenTempFileName('q');
  if( st->request.filename ){
    st->request.fp = fsl_fopen(st->request.filename, "w+b");
    if( !st->request.fp ){
      rc = fsl_xfer_errorf(ch->xfer, FSL_RC_IO,
                           "%s: error opening temp POST data file "
                           "for writing", ch->name);
    }else{
      fsl__tmpchmod( st->request.filename );
    }
  }else{
    fsl_report_oom;
    rc = FSL_RC_OOM;
  }
  return rc;
}

static int fsl_sc_popen_initResponseFile(fsl_sc * const ch){
  fsl__sc_popen_decl;

  ++st->response.count;
  if( fsl_sc_popen_e_filebuf == st->response.mode
      || fsl_sc_popen_e_fd == st->response.mode ){
    fsl_sc_popen_unlinkResponseFile(ch, false);
    assert( st->misc.tmpfilePrefix );
    st->response.filename = popenTempFileName('a');
    if( !st->response.filename ){
      fsl_report_oom;
      return FSL_RC_OOM;
    }
  }
  return 0;
}

#undef popenTempFileName

static int fsl_sc_popen_xInit(fsl_sc * const ch, enum fsl_sc_init_e mode){
  fsl__sc_popen_decl;
  int rc = 0;

  assert( (FSL_SC_INIT_INITIAL | FSL_SC_INIT_REQUEST) & mode );
  if( !st->bin.location ){
    /* Find the binary, if needed... */
    assert( FSL_SC_INIT_INITIAL & mode );
    char const * zBinName = st->bin.name
      ? st->bin.name
      : ch->name;
    if( !zBinName ){
      rc = fsl_xfer_errorf(ch->xfer, FSL_RC_MISUSE,
                           "fsl_sc::name or fsl_sc_popen_state::bin::name "
                           "must be set before calling %s()",
                           __func__);
    }else{
      rc = fsl__find_bin( NULL, zBinName, &st->bin.location );
      switch( rc ){
        default: break;
        case 0: assert( st->bin.location ); break;
        case FSL_RC_NOT_FOUND:
          rc = fsl_xfer_errorf(ch->xfer, rc,
                               "Cannot find %s binary in $PATH", zBinName);
          break;
      }
    }
    if( 0==rc ){
      fsl_buffer b = fsl_buffer_empty;
      rc = fsl__tmpfile(&b, "libfossil-popen");
      if( b.mem ){
        st->misc.tmpfilePrefix = fsl_buffer_str(&b);
        b = fsl_buffer_empty;
      }
    }
    if( 0==rc && st->impl.init ){
      rc = st->impl.init(ch);
    }
    if( 0==rc && 0==fsl_strcmp("file", ch->url.scheme) ){
      fsl_fstat fst = fsl_fstat_empty;
      rc = fsl_stat(ch->url.path, &fst, false);
      if( rc ){
        rc = fsl_xfer_errorf(ch->xfer, rc, "fsl_stat(%s) failed with code %R",
                             ch->url.path, rc);
      }else if( FSL_FSTAT_TYPE_FILE!=fst.type ){
        rc = fsl_xfer_errorf(ch->xfer, FSL_RC_TYPE, "[%s] is not a file",
                             ch->url.path);
      }
    }
    if( rc ) goto end;
  }

  assert( 0==rc );
  assert( st->impl.buildCommandLine );
  if( FSL_SC_INIT_REQUEST & mode ){
    rc = fsl_sc_popen_initRequestFile(ch);
  }

end:
  return rc;
}

static int fsl_sc_popen_xAppend(fsl_sc * const ch, void const * src, fsl_size_t n){
  fsl__sc_popen_decl;
  assert( st->request.fp );
  //fsl_sc_debugf(ch, "append(): %.*s", (int)n, (char const *)src);
  return 1==fwrite(src, n, 1, st->request.fp)
    ? 0
    : fsl_xfer_error(ch->xfer, FSL_RC_IO, "Error writing to POST data file");
}

static int fsl_sc_popen_xSubmit(fsl_sc * const ch,
                                fsl_buffer const * bLoginCard){
  fsl__sc_popen_decl;
  int rc;
  assert( st->request.fp );
  if( 0 && st->request.filename ){
    //fsl_system("ls -latd /tmp/stephan/libfo*");
    fsl_fstat fs = fsl_fstat_empty;
    MARKER(("stat(%s)=%s\n", st->request.filename,
            fsl_rc_cstr(fsl_stat(st->request.filename, &fs, false))));
  }

  fsl_sc_popen_unlinkResponseFile(ch, true);
  assert( st->request.filename );
  assert( st->request.fp );
  if( ch->requestHeaders.used ){
    /* We need to prepend the request headers to the request. We use a
       separate temp file for this.  TODO: if st->request.fp is
       relatively small, stream it into ch->requestHeaders then
       replace its contents with that buffer. */
    fsl_buffer * const b = &ch->requestHeaders;
    //MARKER(("Request headers swap-around!\n"));
    fsl_int_t const nReq = fsl_file_size(st->request.filename);
    if( nReq >= 0 && nReq < (1024 * 512/*arbitrary*/) ){
      /* More memory-hungry, but simpler... */
      rewind( st->request.fp );
      rc = fsl_stream(fsl_input_f_FILE, st->request.fp,
                      fsl_output_f_buffer, b);
      if( 0==rc ){
        rewind( st->request.fp );
        fsl_buffer_rewind(b);
        rc = fsl_stream(fsl_input_f_buffer, b, fsl_output_f_FILE, st->request.fp);
        fflush( st->request.fp );
      }
      assert( rc || fsl_file_size(st->request.filename)==(fsl_int_t)b->used );
    }else{
      char * zTmpFile = fsl_mprintf("%s.2", st->request.filename);
      if( !zTmpFile ){
        fsl_report_oom;
        rc = FSL_RC_OOM;
      }else{
        FILE * const fNewReq = fsl_fopen(zTmpFile, "w+b");
        if( !fNewReq ){
          rc = fsl_xfer_error(ch->xfer, FSL_RC_IO,
                              "Error creating request payload temp file");
        }else if( 1==fwrite(b->mem, b->used, 1, fNewReq) ){
          rewind( st->request.fp );
          rc = fsl_stream(fsl_input_f_FILE, st->request.fp,
                          fsl_output_f_FILE, fNewReq);
        }else{
          rc = fsl_xfer_error(ch->xfer, fsl_errno_to_rc(errno, FSL_RC_IO),
                              "Error writing to request payload temp file");
        }
        fsl_fclose(fNewReq);
      }
      if( 0==rc ){
        fsl_buffer_reuse(b);
        fsl_sc_popen_unlinkRequestFile(ch);
        assert( !st->request.fp );
        st->request.filename = zTmpFile;
      }else{
        fsl_free(zTmpFile);
      }
    }
    if( rc ) goto end;
  }/* ch->requestHeaders */

  fsl_fclose( st->request.fp );
  st->request.fp = 0;
  assert( st->request.filename );

  rc = fsl_sc_popen_initResponseFile(ch);
  if( 0==rc ){
    rc = st->impl.buildCommandLine(ch, fsl_buffer_reuse( &st->cmd ),
                                   bLoginCard);
  }
  if( rc ) goto end;
  fsl_xfer_debugf(ch->xfer, "Command line: %b", &st->cmd);
  rc = fsl__popen2(fsl_buffer_cstr(&st->cmd), &st->process.fdStdOut,
                   &st->process.fStdIn, &st->process.pid);
  if( rc ){
    rc = fsl_xfer_errorf(ch->xfer, rc, "Could not run external command: %b",
                         &st->cmd);
    goto end;
  }
  assert( st->process.fdStdOut>0 );
  if( 0 && st->response.filename ){
    fsl_system("ls -latd /tmp/stephan/libfo*");
    fsl_fstat fs = fsl_fstat_empty;
    MARKER(("stat(%s)=%s\n", st->response.filename,
            fsl_rc_cstr(fsl_stat(st->response.filename, &fs, false))));
  }

  assert( 0==rc );

  switch( st->response.mode ){

    case fsl_sc_popen_e_membuf:
      /* Buffer the response. */
      fsl_buffer_reuse(&st->response.b);
      rc = fsl_buffer_fill_from(&st->response.b, fsl_input_f_fd,
                                &st->process.fdStdOut);
      if( rc ){
        rc = fsl_xfer_error(ch->xfer, FSL_RC_IO, "Error reading response");
      }
      fsl_sc_popen_pclose(ch, true);
      break;

    case fsl_sc_popen_e_filebuf:
      assert( st->response.filename );
      if( st->response.filename ){
        /* Buffer the response via st->response.filename, immediately
           piping all of the binary's stdout to there. We read the
           response from this new handle. See comments in
           case:fsl_sc_popen_e_direct. */
        //MARKER(("Buffering via %s\n", st->response.filename));
        assert( st->response.fd < 0 );
        assert( !st->response.fp );
#if 1
        FILE * fp = fsl_fopen(st->response.filename, "w+b");
        if( !fp ){
          rc = fsl_xfer_errorf(ch->xfer, fsl_errno_to_rc(errno,FSL_RC_IO),
                             "Cannot open file %s", st->response.filename);
          break;
        }
        fsl__tmpchmod( st->request.filename );
        rc = fsl_stream( fsl_input_f_fd, &st->process.fdStdOut,
                         fsl_output_f_FILE, fp );
        fsl_sc_popen_pclose(ch, false);
        if( 0==rc ){
          rewind(fp);
          st->response.fp = fp;
          //st->response.fd = fileno(fp);
        }else{
          fsl_fclose(fp);
        }
#else
        /* Same as above but use an unbuffered file descriptor */
        int fd = open(st->response.filename,
                      O_EXCL | O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
        if( fd<0 ){
          rc = fsl_xfer_errorf(ch->xfer, fsl_errno_to_rc(errno,FSL_RC_IO),
                             "Cannot open file %s", st->response.filename);
          break;
        }
        rc = fsl_stream( fsl_input_f_fd, &st->process.fdStdOut,
                         fsl_output_f_fd, &fd );
        fsl_sc_popen_pclose(st, false);
        if( rc ){
          close(fd);
          rc = fsl_xfer_errorf(ch->xfer, rc, "Error streaming response to %s",
                             st->response.filename);
        }else{
          st->response.fd = fd;
          lseek(fd, 0, SEEK_SET);
        }
        //MARKER(("rc=%d\n", rc));
#endif
      }
      break;

    case fsl_sc_popen_e_fd:
      /*  Buffer the response via st->response.filename so we can
          later stream input from the binary's stdout. For whatever
          reason, this mode is failing with "file not found" for the
          response file, whether we use (curl ... --output FILE) or
          (curl ... > FILE). */
      assert( st->response.filename );
      //fsl_system("ls -latd /tmp/stephan/libfo*");
      if( 0 ){
        fsl_fstat fs = fsl_fstat_empty;
        MARKER(("stat(%s)=%s\n", st->response.filename,
                fsl_rc_cstr(fsl_stat(st->response.filename, &fs, false))));
      }
      assert( st->response.fd<0 );
      st->response.fd = open(st->response.filename, O_RDONLY);
      if( st->response.fd<0 ){
        rc = fsl_xfer_errorf(ch->xfer, FSL_RC_IO,
                           "Error %R opening response file %s",
                           fsl_errno_to_rc(errno, FSL_RC_IO),
                           st->response.filename);
      }
      break;

    case fsl_sc_popen_e_direct:
      /* For some reason, reading the response directly from the
         binary's stdout st->process.fdStdOut, i.e. in
         fsl_sc_popen_e_direct mode, is misbehaving in a weird way: it
         seems to mostly work and then consumes only part of one
         cfile/file card, leaving the rest of the card for for the
         next read-line op, which then fails to parse the non-card
         input */
    default:
      break;
  }
end:
  return rc;
}

/**
   fsl_sc::read() impl for fsl_sc_popen.
*/
static int fsl_sc_popen_xRead(fsl_sc * const ch, fsl_buffer * tgt, fsl_int_t howMuch){
  int rc = 0;
  fsl__sc_popen_decl;
  switch( st->response.mode ){

    default:
      fsl__fatal(FSL_RC_MISUSE, "Impossible fsl_sc_popen_e_e value\n");
      break;

    case fsl_sc_popen_e_membuf:
      if( st->response.b.used ){
        fsl_buffer * const br = &st->response.b;
        fsl_size_t const cu = br->cursor;
        switch( howMuch ){
          case FSL_SC_READ_LINE:
            /* Read one line */
            rc = fsl_buffer_copy_lines(tgt, br, 1);
            if( !rc && cu==br->cursor ){
              /* End of input */
              rc = FSL_RC_BREAK;
            }
            break;

          case FSL_SC_READ_ALL:
            fsl_buffer_swap(&st->response.b, tgt);
            break;

          default:
            assert( howMuch > 0 );
            /* Read an absolute amount */
            fsl_buffer_err_clear(tgt);
            if( howMuch + cu > br->used ){
              rc = fsl_xfer_errorf(ch->xfer, FSL_RC_RANGE,
                                 "Not enough buffer (%u bytes) to serve %u bytes",
                                 (unsigned)(br->used - cu), (unsigned)howMuch);
            }else{
              rc = fsl_buffer_append(tgt, br->mem+cu, howMuch);
              if( 0==rc ){
                br->cursor += howMuch;
              }
            }
            break;
        }
      }else{
        rc = FSL_RC_BREAK;
      }
      break;

    case fsl_sc_popen_e_direct:
    case fsl_sc_popen_e_filebuf:
    case fsl_sc_popen_e_fd:{
      enum { Sz = 1024 * 2 /* input buffer size */ };
      unsigned char buf[Sz];
      FILE * const fp =
        1 ? st->response.fp : NULL
        /* Reading from st->response.fp is significantly faster than
           unbuffered reading from fileno(st->response.fp). */;
      int const fdIn = fp
        ? -1
        : (fsl_sc_popen_e_fd == st->response.mode
           ? st->response.fd
           : st->process.fdStdOut);
      assert(fsl_sc_popen_e_filebuf == st->response.mode
             || fsl_sc_popen_e_direct == st->response.mode
             || fsl_sc_popen_e_fd == st->response.mode);
      assert( fp ? (fsl_sc_popen_e_filebuf == st->response.mode)
              : (fsl_sc_popen_e_filebuf != st->response.mode) );
      assert( fp ? fdIn<0 : fdIn>0 );

      switch( howMuch ){

        case FSL_SC_READ_LINE:{
          fsl_size_t nLine = 0;
          char * line = 0;
          if( fp ){
            line = fgets((char *)buf, Sz, fp);
            if( !line ){
              rc = feof(fp)
                ? FSL_RC_BREAK
                : fsl_xfer_error(ch->xfer, FSL_RC_IO,
                               "Read from response failed");
            }else{
              /* fsl_strlen() here is a genuine performance hog. One
                 alternative would be to use getline(3) instead of
                 fgets(3), but that requires a malloc()-allocated buffer
                 which it may realloc(). */
              nLine = (fsl_size_t)strlen(line);//fsl_strlen(line)
              /* strlen() is far more performant than either
                 fsl_strlen() or a local loop. */;
            }
          }else{
            rc = fsl__fdgets(fdIn, buf, Sz, &line, &nLine);
            //MARKER(("xread %"FSL_INT_T_PFMT " rc=%d nLine=%u\n", howMuch, rc, (unsigned)nLine));
            if( rc ){
              rc = fsl_xfer_error(ch->xfer, rc,
                                "fgets() of child-process's stdout failed");
            }
          }
          //MARKER(("get-line: [%.*s]\n", (int)nLine, line));
          if( 0==rc ){
            if( !nLine ){
              rc = FSL_RC_BREAK;
            }
#if 0
            /* Leave this to the higher-up xfer APIs? */
            else if( '\n' != line[nLine-1] ){
              rc = fsl_xfer_error(ch->xfer, FSL_RC_SYNTAX,
                                "No newline found in input");
            }
#endif
            else{
              //MARKER(("get-line: [%.*s]\n", (int)nLine, line));
              rc = fsl_buffer_append(tgt, line, (fsl_int_t)nLine);
            }
          }
          break;
        }

        case FSL_SC_READ_ALL:
          if( fp ){
            rc = fsl_stream(fsl_input_f_FILE, fp, fsl_output_f_buffer, tgt);
          }else{
            rc = fsl_stream(fsl_input_f_fd, (void *)&fdIn,
                            fsl_output_f_buffer, tgt);
          }
          break;

        default:
          /* Read an absolute amount */
          assert( howMuch > 0 );
          fsl_buffer_err_clear(tgt);
          while( 0==rc && howMuch > 0 ){
            size_t const n = (howMuch>=Sz ? Sz : (size_t)howMuch);
            ssize_t const rd = fp
              ? (ssize_t)fread(buf, n, 1, fp)
              : read(fdIn, buf, n);
            if( fp ? 1!=rd : (ssize_t)-1==rd ){
              //MARKER(("errno=%s\n", strerror(errno)));
              rc = fsl_xfer_errorf(ch->xfer,
                                   fsl_errno_to_rc(errno, FSL_RC_IO),
                                   "error reading %u bytes",
                                   (unsigned)n);
            }else if( !fp && n==(fsl_size_t)howMuch && (size_t)rd != n ){
              rc = fsl_xfer_errorf(ch->xfer, FSL_RC_IO,
                                   "short read: %u of %u bytes",
                                   (unsigned)rd, (unsigned)n);
            }else{
              assert( (fsl_int_t)n > 0 && "Else overflow");
              assert( (size_t)howMuch >= n );
              howMuch -= n;
              assert( howMuch >= 0 );
              rc = fsl_buffer_append(tgt, buf, (fsl_int_t)n);
            }
          }
          break;
      }/*switch(howMuch)*/
      break;
    }/*case non-membuf*/
  }/* switch(st->response.mode) */
  return rc;
}

static void fsl_sc_popen_xCleanup(fsl_sc * const ch){
  fsl__sc_popen_decl;
  if(st->impl.cleanup){
    st->impl.cleanup(st);
  }
  fsl_sc_popen_pclose(ch, true);
  assert( st->response.fd < 0 );
  fsl_buffer_clear(&st->response.b);
  fsl_free(st->bin.location);
  fsl_free(st->misc.tmpfilePrefix);
  fsl_buffer_clear(&st->cmd);
  if( st->misc.allocStamp ){
    assert( st->misc.allocStamp == st );
    ch->impl = fsl_sc_popen.impl;
    *st = fsl_sc_popen_state_empty;
    fsl_free(st);
  }else{
    *st = fsl_sc_popen_state_empty;
  }
}

const fsl_sc_popen_state fsl_sc_popen_state_empty =
  fsl_sc_popen_state_init("<unnamed>", 0, 0, 0);


#define fsl__sc_popen_init(NAME,STATETYPE,SCFLAGS) { \
  .xfer = NULL, .name = NAME,                        \
  .url = fsl_url_empty_m,                            \
  .flags = SCFLAGS,                                  \
  .impl = {.p = 0, .type = &fsl_sc_popen},           \
  .init = fsl_sc_popen_xInit,                        \
  .append = fsl_sc_popen_xAppend,                    \
  .submit = fsl_sc_popen_xSubmit,                    \
  .read = fsl_sc_popen_xRead,                        \
  .cleanup = fsl_sc_popen_xCleanup,                  \
  .self = fsl_sc_self                                \
}

const fsl_sc fsl_sc_popen =
  fsl__sc_popen_init("popen", fsl_sc_popen_state_empty,0);

/**
   fsl_sc_popen::impl::buildCommandLine() impl for curl.
*/
static int fsl_sc_popen_curl_buildCommandLine(fsl_sc * const ch,
                                              fsl_buffer * const b,
                                              fsl_buffer const * bLoginCard){
  int rc = 0;
  fsl_error * const err = fsl_cx_err_e(ch->xfer->f);
  fsl_url const * const url = &ch->url;
  fsl__sc_popen_decl;
  char const * const curlArgs[] = {
    st->bin.location,
    "--silent",
    /*"-XPOST", is implied by --data-binary and curl -v warns about
      this. */
    "--include",
    "--user-agent", "libfossil " FSL_LIBRARY_VERSION,
    "-H", (FSL_SC_F_COMPRESSED & ch->flags)
    ? "Content-Type: application/x-fossil"
    : "Content-Type: application/x-fossil-uncompressed",
    "--data-binary" /* @st->request.filename */
  };
  assert( st->bin.location );
  assert( url->raw );
  assert( st->request.filename );
  assert( !st->request.fp );
  fsl_error_reset(err);
  for( unsigned int i = 0;
       0==rc &&i < sizeof(curlArgs)/sizeof(curlArgs[0]);
       ++i ){
    rc = fsl_buffer_esc_arg_v2(b, err, curlArgs[i], 0==i);
  }
  if( rc ) return rc;

  if( !b->errCode ){ /* --data-binary @filename */
    fsl_buffer * const bName = fsl_buffer_reuse(&ch->scratch);
    fsl_buffer_appendch(bName, '@');
    fsl_buffer_append(bName, st->request.filename, -1);
    fsl_buffer_esc_arg_v2(b, err, fsl_buffer_cstr(bName), true);
    if( bName->errCode ) return bName->errCode;
  }

  if( st->response.filename ){
    fsl_buffer_esc_arg_v2(b, err, "--output", false);
    fsl_buffer_esc_arg_v2(b, err, st->response.filename, true);
  }

  if( !b->errCode && bLoginCard ){
    fsl_buffer_appendf(b, " --cookie \"x-f-l-c=%b\"", bLoginCard);
  }

  if( url->username ){
    /* Strip username[:password] from the URL because curl will add a
       HTTP Basic Authentication header for it, which we don't
       want. */
    fsl_buffer * const bu = fsl_buffer_reuse(&ch->scratch);
    fsl_url_render(url, bu, FSL_URL_RENDER_NO_USER);
    if( bu->errCode ) b->errCode = bu->errCode;
    else{
      fsl_buffer_esc_arg_v2(b, err, fsl_buffer_cstr(bu), false);
    }
  }else{
    fsl_buffer_esc_arg_v2(b, err, url->raw, false);
  }

#if FSL_PLATFORM_IS_UNIX
  //stderr is useful for dianosing curl invocation errors
  //fsl_buffer_append(b, " 2>/dev/null", 12);
#endif
  return b->errCode ? b->errCode : err->code;
}

const fsl_sc_popen_state fsl_sc_popen_state_curl =
  fsl_sc_popen_state_init("curl",
                          0,
                          fsl_sc_popen_curl_buildCommandLine, 0);

const fsl_sc fsl_sc_popen_curl =
  fsl__sc_popen_init("popen<curl>", fsl_sc_popen_state_curl,0);

/**
   fsl_sc_popen::impl::buildCommandLine() impl for fossil(1)'s
   test-http command (which is how fossil implements local-file sync
   and SSH sync).
*/
static int fsl_sc_popen_fth_buildCommandLine(fsl_sc * ch,
                                             fsl_buffer * const b,
                                             fsl_buffer const * bLoginCard){
  fsl_error * const err = fsl_cx_err_e(fsl_xfer_cx(ch->xfer));
  fsl__sc_popen_decl;
  assert( st->bin.location );
  assert( ch->url.raw );
  assert( st->request.filename );
  assert( !st->request.fp );
  (void)bLoginCard;
  fsl_error_reset(err);
  fsl_buffer_esc_arg_v2(b, err, st->bin.location, true);
  fsl_buffer_esc_arg_v2(b, err, "test-http", false);
  fsl_buffer_esc_arg_v2(b, err, ch->url.path, true);
  fsl_buffer_append(b, " < ", 3);
  fsl_buffer_esc_arg_v2(b, err, st->request.filename, true);
  if( st->response.filename ){
    fsl_buffer_append( b, " > ", 3);
    fsl_buffer_esc_arg_v2(b, err, st->response.filename, true);
  }
#if FSL_PLATFORM_IS_UNIX
  if( FSL_SC_F_SUPPRESS_STDERR & ch->flags ){
    fsl_buffer_append(b, " 2>/dev/null", 12);
  }
#endif
  return b->errCode ? b->errCode : err->code;
}

const fsl_sc_popen_state fsl_sc_popen_state_fth =
  fsl_sc_popen_state_init("fossil",
                          0,
                          fsl_sc_popen_fth_buildCommandLine, 0);

const fsl_sc fsl_sc_popen_fth =
  fsl__sc_popen_init("popen<fossil test-http>",
                     fsl_sc_popen_state_fth,
                     FSL_SC_F_REQUEST_HEADERS);


/**
   fsl_sc_popen::impl::buildCommandLine() impl for an ssh-based impl
   which remotely invoke's fossil(1)'s test-http command (which is how
   fossil implements ssh sync).
*/
static int fsl_sc_popen_ssh_buildCommandLine(fsl_sc * ch,
                                             fsl_buffer * const b,
                                             fsl_buffer const * bLoginCard){
  fsl_error * const err = fsl_cx_err_e(fsl_xfer_cx(ch->xfer));
  char const *zRemoteFossil = "fossil" /* remote fossil binary */;
  fsl__sc_popen_decl;
  assert( st->bin.location );
  assert( ch->url.raw );
  assert( st->request.filename );
  assert( !st->request.fp );
  (void)bLoginCard;
  fsl_error_reset(err);
  fsl_buffer_esc_arg_v2(b, err, st->bin.location, true);
  //if( 0==(FSL_SC_F_COMPRESSED & ch->flags) ){
  //slow...
  //  fsl_buffer_esc_arg_v2(b, err, "-C", false);
  //}
  fsl_buffer_esc_arg_v2(b, err, "-T", false);
  fsl_buffer_esc_arg_v2(b, err, "--", false);
  if( 0==b->errCode ){
    /* Add user/host... */
    fsl_buffer * const bScratch = fsl_buffer_reuse(&ch->scratch);
    assert( !bScratch->errCode );
    if( ch->url.username ){
      fsl_buffer_appendf(bScratch, "%s@", ch->url.username);
    }
    fsl_buffer_append(bScratch, ch->url.host, -1);
    if( ch->url.port>0 ){
      fsl_buffer_appendf(bScratch, ":%d", ch->url.port );
    }
    b->errCode = bScratch->errCode;
    fsl_buffer_esc_arg_v2(b, err, fsl_buffer_cstr(bScratch), false);
    if( 0==b->errCode ){
      /* Check for fossil=... URL arg... */
      fsl_buffer bKey = fsl_buffer_empty;
      char const *zTail = 0;
      fsl_buffer_reserve(&bKey, 16);
      while( fsl_url_next_query_arg(&ch->url, &zTail, &bKey, bScratch) ){
        if( fsl_buffer_eq(&bKey, "fossil", 6) ){
          fsl_buffer_esc_arg_v2(b, err, fsl_buffer_cstr(bScratch), true);
          zRemoteFossil = 0;
          break;
        }
      }
      b->errCode = bKey.errCode ? bKey.errCode : bScratch->errCode;
      fsl_buffer_clear(&bKey);
      fsl_buffer_reuse(bScratch);
    }
  }
  if( zRemoteFossil ){
    fsl_buffer_esc_arg_v2(b, err, zRemoteFossil, true);
  }
  fsl_buffer_esc_arg_v2(b, err, "test-http", false);
  fsl_buffer_esc_arg_v2(b, err, ch->url.path, true);
  fsl_buffer_append(b, " < ", 3);
  fsl_buffer_esc_arg_v2(b, err, st->request.filename, true);
  if( st->response.filename ){
    fsl_buffer_append( b, " > ", 3);
    fsl_buffer_esc_arg_v2(b, err, st->response.filename, true);
  }
#if FSL_PLATFORM_IS_UNIX
  if( FSL_SC_F_SUPPRESS_STDERR & ch->flags ){
    fsl_buffer_append(b, " 2>/dev/null", 12);
  }
#endif
  return b->errCode ? b->errCode : err->code;
}

const fsl_sc_popen_state fsl_sc_popen_state_ssh =
  fsl_sc_popen_state_init("ssh",
                          0,
                          fsl_sc_popen_ssh_buildCommandLine, 0);

const fsl_sc fsl_sc_popen_ssh =
  fsl__sc_popen_init("popen<ssh>",
                     fsl_sc_popen_state_ssh,
                     FSL_SC_F_REQUEST_HEADERS
                     | FSL_SC_F_SUPPRESS_STDERR
  );

#undef fsl__sc_popen_decl


struct fsl_sc_tracer_state {
  fsl_sc * ch;
  fsl_outputer * out;
  fsl_flag32_t flags;
};
typedef struct fsl_sc_tracer_state fsl_sc_tracer_state;

#define tracer__decl \
  assert( &fsl_sc_tracer_empty == ch->impl.type ); \
  fsl_sc_tracer_state * const tst = ch->impl.p;    \
  fsl_sc * const orig = tst->ch;                   \
  assert(orig); assert(orig != ch)

#define origDebug(MASK) if( tst->flags & (MASK) )
#define rcOrEmpty rc ? fsl_rc_cstr(rc) : ""

static void tracer__trace(fsl_sc * const ch,
                          fsl_sc * const orig,
                          fsl_sc_tracer_state * const tst,
                          char const * zFmt, ...){
  assert( orig->xfer->config->listener.callback );
  va_list args;
  fsl_buffer * const b = fsl_buffer_reuse(&ch->scratch);
  va_start(args,zFmt);
  assert(tst->out);
  fsl_buffer_appendf(b, "fsl_sc<%s>::", orig->name);
  if( 0==fsl_buffer_appendfv(b, zFmt, args) ){
    if( b->mem[b->used-1] != '\n' ){
      fsl_buffer_appendch(b, '\n');
    }
    tst->out->out(tst->out->state, b->mem, b->used);
  }
  va_end(args);
}

static int tracer__init(fsl_sc * const ch, enum fsl_sc_init_e mode){
  tracer__decl;
  int const rc = orig->init(orig, mode);
  origDebug(FSL_SC_TRACER_init) {
    fsl_buffer_reserve(&ch->scratch, 128);
    tracer__trace(ch, orig, tst, "init(mode=0x%0x) %s",
                  mode, rcOrEmpty);
  }
  return rc;
}

static int tracer__read(fsl_sc * const ch, fsl_buffer * tgt,
                        fsl_int_t howMuch){
  tracer__decl;
  int const rc = orig->read(orig, tgt, howMuch);
  origDebug(FSL_SC_TRACER_read) {
    tracer__trace(ch, orig, tst, "read(%" FSL_INT_T_PFMT ") %s",
                  howMuch, rcOrEmpty);
  }
  return rc;
}

static int tracer__append(fsl_sc * const ch, void const * src,
                          fsl_size_t n){
  tracer__decl;
  int const rc = orig->append(orig, src, n);
  origDebug(FSL_SC_TRACER_append) {
    tracer__trace(ch, orig, tst, "append(%" FSL_SIZE_T_PFMT ") %s",
                  n, rcOrEmpty);
  }
  return rc;
}

static void tracer__cleanup(fsl_sc * const ch){
  tracer__decl;
  origDebug(FSL_SC_TRACER_cleanup) {
    tracer__trace(ch, orig, tst, "cleanup()");
  }
  fsl_sc_cleanup(orig);
  fsl_free( ch->impl.p );
  *ch = fsl_sc_tracer_empty;
}

static int tracer__submit(fsl_sc * const ch,
                          fsl_buffer const * bLoginCard){
  tracer__decl;
  int const rc = orig->submit(orig, bLoginCard);
  origDebug(FSL_SC_TRACER_submit) {
    tracer__trace(ch, orig, tst, "submit(%s) %s",
                  bLoginCard
                  ? fsl_buffer_cstr(bLoginCard)
                  : "",
                  rcOrEmpty);
  }
  return rc;
}

static fsl_sc * tracer__self(fsl_sc *ch){
  tracer__decl;
  fsl_sc * const rv = orig->self(orig);
  origDebug(FSL_SC_TRACER_self) {
    tracer__trace(ch, orig, tst, "self(%s) = %s",
                  ch->name, rv->name);
  }
  return rv;
}

#undef origDebug
#undef rcOrEmpty
#undef tracer__decl

const fsl_sc fsl_sc_tracer_empty = {
  .name = "tracer",
  .xfer = 0,
  .impl = {.p = NULL, .type = NULL},
  .init  = tracer__init,
  .append = tracer__append,
  .submit  = tracer__submit,
  .read = tracer__read,
  .cleanup = tracer__cleanup,
  .self = tracer__self
};

fsl_sc * fsl_sc_tracer_init(fsl_sc *chTrace, fsl_sc *ch,
                            fsl_flag32_t traceFlags,
                            fsl_outputer * out){
  fsl_sc_tracer_state * tst = fsl_malloc(sizeof(*tst));
  if( !tst ) return 0;
  tst->ch = ch;
  tst->flags = traceFlags;
  tst->out = out;
  *chTrace = fsl_sc_tracer_empty;
  chTrace->impl.p = tst;
  chTrace->impl.type = &fsl_sc_tracer_empty;
  chTrace->xfer = ch->xfer;
  assert( tracer__self==chTrace->self );
  fsl_buffer_reserve(&ch->scratch, 192);
  return chTrace;
}

#if 0
fsl_sc * fsl_sc_for_url( char const * zUrl ){
  fsl_sc * rv = NULL;
  fsl_url u = fsl_url_empty;
  if( 0!=fsl_url_parse(&u, zUrl) ) return NULL;
  else if( 0==fsl_strcmp("http",u.scheme)
      || 0==fsl_strcmp("https",u.scheme) ){
    rv = fsl_malloc(
      sizeof(*rv) + sizeof(fsl_sc_popen_state)
    );
    fsl_sc_popen_state * const st = rv
      ? (fsl_sc_popen_state *)(rv + 1)
      : 0;
    if( !st ) goto end;
    *rv = fsl_sc_popen;
    *st = fsl_sc_popen_state_curl;
    assert( &fsl_sc_popen_state_empty == rv->impl.type );
    rv->impl.p = st;
  }
end:
  fsl_url_cleanup(&u);
  return rv;
}
#endif

#undef MARKER
#undef fsl__sc_popen_init
