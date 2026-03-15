/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/

#include "fossil-scm/core.h"
#include <assert.h>
#include <limits.h> /* LONG_MAX on darwin */
#include <string.h> /* strlen() */
#include <stddef.h> /* NULL on linux */
#include <errno.h>

#include <zlib.h>


#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

#define buf__is_external(b) (b->mem && 0==b->capacity)

#define buff__errcheck(B) if((B)->errCode) return (B)->errCode
/**
   Materializes external buffer b by allocating b->used+extra+1
   bytes, copying b->used bytes from b->mem to the new block,
   NUL-terminating the block, and replacing b->mem with the new
   block. Returns 0 on success, else FSL_RC_OOM.

   Asserts that b is an external buffer.
*/
static int fsl__buffer_materialize( fsl_buffer * const b, fsl_size_t extra ){
  assert(buf__is_external(b));
  buff__errcheck(b);
  fsl_size_t const n = b->used + extra + 1;
  unsigned char * x = (unsigned char *)fsl_malloc(n);
  if(!x) return b->errCode = FSL_RC_OOM;
  memcpy(x, b->mem, b->used);
  b->capacity = n;
  x[b->used] = 0;
  b->mem = x;
  return 0;
}

int fsl_buffer_err( fsl_buffer const * b ){
  return b->errCode;
}

void fsl_buffer_err_clear(fsl_buffer * const b){
  b->errCode = 0;
}

#define buf__materialize(B,N) (buf__is_external(B) ? fsl__buffer_materialize((B),(N)) : 0)

int fsl_buffer_materialize( fsl_buffer * const b ){
  buff__errcheck(b);
  if( !buf__is_external(b) ) return 0;
  return fsl__buffer_materialize(b, 0);
}

void fsl_buffer_external( fsl_buffer * const b, void const * mem, fsl_int_t n ){
  if(b->mem) fsl_buffer_clear(b);
  b->used = (n<0) ? fsl_strlen((char const *)mem) : (fsl_size_t)n;
  b->cursor = 0;
  b->errCode = 0;
  b->mem = (unsigned char *)mem;
  b->capacity = 0;
}

fsl_buffer * fsl_buffer_reuse( fsl_buffer * const b ){
  if(buf__is_external(b)){
    *b = fsl_buffer_empty;
  }else{
    if(b->capacity){
      assert(b->mem);
      b->mem[0] = 0;
      b->used = 0;
    }
    b->cursor = 0;
    b->errCode = 0;
  }
  return b;
}

void fsl_buffer_clear( fsl_buffer * const buf ){
  if(buf->capacity) fsl_free(buf->mem);
  *buf = fsl_buffer_empty;
}

int fsl_buffer_reserve( fsl_buffer * const buf, fsl_size_t n ){
  if( 0 == n ){
    if(!buf__is_external(buf)){
      fsl_free(buf->mem);
    }/* else if it has memory, it's owned elsewhere */
    *buf = fsl_buffer_empty;
    return 0;
  }
  else buff__errcheck(buf);
  else if( !buf__is_external(buf) && buf->capacity >= n ){
    assert(buf->mem);
    return 0;
  }else{
    unsigned char * x;
    bool const isExt = buf__is_external(buf);
    assert((buf->used < n) && "Buffer in-use greater than capacity!");
    if(isExt && n<=buf->used){
      /*For external buffers, always keep at least the initially-pointed-to
        size. */
      n = buf->used + 1;
    }
    x = (unsigned char *)fsl_realloc( isExt ? NULL : buf->mem, n );
    if( !x ) return buf->errCode = FSL_RC_OOM;
    else if(isExt){
      memcpy( x, buf->mem, buf->used );
      x[buf->used] = 0;
    }else{
      memset( x + buf->used, 0, n - buf->used );
    }
    buf->mem = x;
    buf->capacity = n;
    return 0;
  }
}

int fsl_buffer_resize( fsl_buffer * const b, fsl_size_t n ){
  buff__errcheck(b);
  else if(buf__is_external(b)){
    if(n==b->used) return 0;
    else if(n==0){
      b->capacity = 0;
      fsl_buffer_external(b, "", 0);
      return 0;
    }
    unsigned char * x = (unsigned char *)fsl_malloc( n+1/*NUL*/ );
    if( !x ) return b->errCode = FSL_RC_OOM;
    memcpy(x, b->mem, n < b->used ? n : b->used);
    x[n] = 0;
    b->mem = x;
    b->capacity = n+1;
    b->used = n;
    return 0;
  }else if(n && (b->capacity == n+1)){
    b->used = n;
    b->mem[n] = 0;
    return 0;
  }else{
    unsigned char * x = (unsigned char *)fsl_realloc( b->mem,
                                                      n+1/*NUL*/ );
    if( ! x ) return b->errCode = FSL_RC_OOM;
    if(n > b->capacity){
      /* zero-fill new parts */
      memset( x + b->capacity, 0, n - b->capacity +1/*NUL*/ );
    }
    b->capacity = n + 1 /*NUL*/;
    b->used = n;
    b->mem = x;
    b->mem[b->used] = 0;
    return 0;
  }
}

int fsl_buffer_compare(fsl_buffer const * const lhs, fsl_buffer const * const rhs){
  fsl_size_t const szL = lhs->used;
  fsl_size_t const szR = rhs->used;
  fsl_size_t const sz = (szL<szR) ? szL : szR;
  int rc = memcmp(lhs->mem, rhs->mem, sz);
  if(0 == rc){
    rc = (szL==szR)
      ? 0
      : ((szL<szR) ? -1 : 1);
  }
  return rc;
}

bool fsl_buffer_eq(fsl_buffer const * const b, char const * str,
                   fsl_int_t nStr){
  fsl_buffer const rhs = {
    .mem = (unsigned char *)str,
    .capacity = 0,
    .used = (nStr<0) ? fsl_strlen(str) : (fsl_size_t)nStr,
    .cursor = 0,
    .errCode = 0
  };
  return 0==fsl_buffer_compare(b, &rhs);
}

int fsl_buffer_compare_O1(fsl_buffer const * const lhs, fsl_buffer const * const rhs){
  fsl_size_t const szL = lhs->used;
  fsl_size_t const szR = rhs->used;
  fsl_size_t i;
  unsigned char const *buf1;
  unsigned char const *buf2;
  unsigned char rc = 0;
  if( szL!=szR || szL==0 ) return 1;
  buf1 = lhs->mem;
  buf2 = rhs->mem;
  for( i=0; i<szL; i++ ){
    rc = rc | (buf1[i] ^ buf2[i]);
  }
  return rc;
}


int fsl_buffer_append( fsl_buffer * const b,
                       void const * const data,
                       fsl_int_t len ){
  if(0==b->errCode){
    fsl_size_t sz = b->used;
    if(len<0) len = (fsl_int_t)fsl_strlen((char const *)data);
    if(buf__materialize(b, (fsl_size_t)len + 1)) return b->errCode;
    assert(b->capacity ? !!b->mem : !b->mem);
    assert(b->used <= b->capacity);
    sz += len + 1/*NUL*/;
    if(b->capacity<sz) fsl_buffer_reserve( b, sz );
    if(!b->errCode){
      assert(b->capacity >= sz);
      if(len>0) memcpy(b->mem + b->used, data, (size_t)len);
      b->used += len;
      b->mem[b->used] = 0;
    }
  }
  return b->errCode;
}

int fsl_buffer_appendfv( fsl_buffer * const b, char const * fmt,
                         va_list args){
  return fsl_appendfv( fsl_output_f_buffer, b, fmt, args );
}


int fsl_buffer_appendf( fsl_buffer * const b,
                        char const * fmt, ... ){
  buff__errcheck(b);
  else{
    va_list args;
    va_start(args,fmt);
    fsl_buffer_appendfv( b, fmt, args );
    va_end(args);
    return b->errCode;
  }
}

char const * fsl_buffer_cstr(fsl_buffer const * const b){
  return b->errCode ? NULL : (char const *)b->mem;
}

char const * fsl_buffer_cstr2(fsl_buffer const * const b, fsl_size_t * const len){
  char const * rc = NULL;
  if(0==b->errCode){
    rc = (char const *)b->mem;
    if(len) *len = b->used;
  }
  return rc;
}

char * fsl_buffer_str(fsl_buffer const * const b){
  return b->errCode ? NULL : (char *)b->mem;
}


#if 0
fsl_size_t fsl_buffer_size(fsl_buffer const * const b){
  return b->used;
}

fsl_size_t fsl_buffer_capacity(fsl_buffer const * const b){
  return b->capacity;
}
#endif

/** Decode a 4-byte big-endian value from MEM. */
#define decode__zsize(MEM) \
  (MEM[0] << 24 | MEM[1] << 16 | MEM[2] << 8 | MEM[3])

bool fsl_data_is_compressed(unsigned char const * const mem, fsl_size_t len){
  if(!mem || (len<6)) return 0;
#if 0
  else return ('x'==mem[4])
    && (0234==mem[5]);
  /*
    This check fails for one particular artifact in the tcl core.
    Notes gathered while debugging...

    https://core.tcl.tk/tcl/

    Delta manifest #5f37dcc3 while processing file #687
    (1-based):

    FSL_RC_RANGE: "Delta: copy extends past end of input"

    To reproduce from tcl repo:

    f-acat 5f37dcc3 | f-mfparse -r

    More details:

    Filename: library/encoding/gb2312-raw.enc
    Content: dba09c670f24d47b95d12d4bb9704391b81dda9a

    That artifact is a delta of bccc899015b688d5c426bc791c2fcde3a03a3eb5,
    which is actually two files:

    library/encoding/euc-cn.enc
    library/encoding/gb2312.enc

    When we go to apply the delta, the contents of bccc8 appear to
    be badly compressed data. They have the 'x' at byte offset
    4 but not the 0234 at byte offset 5.

    Turns out it is the fsl_buffer_is_compressed() impl which fails
    for that one.
  */
#else
  else{
    /**
       Adapted from:

       https://blog.2of1.org/2011/03/03/decompressing-zlib-images/

       Remember that fossil-compressed data has a 4-byte big-endian
       header holding the uncompressed size of the data, so we skip
       those first 4 bytes.

       See also:

       https://tools.ietf.org/html/rfc6713

       search for "magic number".
    */
    fsl_size_t const sz = decode__zsize(mem);
    int16_t const head = (((int16_t)mem[4]) << 8) | mem[5];
    /* MARKER(("isCompressed header=%04x\n", head)); */
    if( sz >= FSL_BLOB_MAX_SIZE ){
      return false;
    }
    /* The sz check will rule out _some_ false positives, but not all */
    switch(head){
      case 0x083c: case 0x087a: case 0x08b8: case 0x08f6:
      case 0x1838: case 0x1876: case 0x18b4: case 0x1872:
      case 0x2834: case 0x2872: case 0x28b0: case 0x28ee:
      case 0x3830: case 0x386e: case 0x38ac: case 0x38ea:
      case 0x482c: case 0x486a: case 0x48a8: case 0x48e6:
      case 0x5828: case 0x5866: case 0x58a4: case 0x58e2:
      case 0x6824: case 0x6862: case 0x68bf: case 0x68fd:
      case 0x7801: case 0x785e: case 0x789c: case 0x78da:
        return true;
      default:
        return false;
    }
  }
#endif
}

bool fsl_buffer_is_compressed(fsl_buffer const *buf){
  return fsl_data_is_compressed( buf->mem, buf->used );
}

fsl_int_t fsl_data_uncompressed_size(unsigned char const *mem,
                                     fsl_size_t len){
  return fsl_data_is_compressed(mem,len)
    ? decode__zsize(mem)
    : -1;
}

fsl_int_t fsl_buffer_uncompressed_size(fsl_buffer const * b){
  return fsl_data_uncompressed_size(b->mem, b->used);
}

int fsl_buffer_compress(fsl_buffer const *pIn, fsl_buffer * const pOut){
  buff__errcheck(pIn);
  else buff__errcheck(pOut);
  unsigned int nIn = pIn->used;
  unsigned int nOut = 13 + nIn + (nIn+999)/1000;
  fsl_buffer temp = fsl_buffer_empty;
  int rc = fsl_buffer_resize(&temp, nOut+4);
  if(rc) return rc;
  else{
    unsigned long int nOut2;
    unsigned char *outBuf;
    unsigned long int outSize;
    outBuf = temp.mem;
    outBuf[0] = nIn>>24 & 0xff;
    outBuf[1] = nIn>>16 & 0xff;
    outBuf[2] = nIn>>8 & 0xff;
    outBuf[3] = nIn & 0xff;
    nOut2 = (long int)nOut;
    rc = compress(&outBuf[4], &nOut2,
                  pIn->mem, pIn->used);
    if(rc){
      fsl_buffer_clear(&temp);
      return FSL_RC_ERROR;
    }
    outSize = nOut2+4;
    rc = fsl_buffer_resize(&temp, outSize);
    if(rc){
      fsl_buffer_clear(&temp);
    }else{
      fsl_buffer_swap_free(&temp, pOut, -1);
      assert(0==temp.used);
      assert(outSize==pOut->used);
    }
    return rc;
  }
}

int fsl_buffer_compress2(fsl_buffer const *pIn1,
                         fsl_buffer const *pIn2, fsl_buffer * const pOut){
  buff__errcheck(pIn1);
  else buff__errcheck(pIn2);
  else buff__errcheck(pOut);
  unsigned int nIn = pIn1->used + pIn2->used;
  unsigned int nOut = 13 + nIn + (nIn+999)/1000;
  fsl_buffer temp = fsl_buffer_empty;
  int rc;
  rc = fsl_buffer_resize(&temp, nOut+4);
  if(rc) return rc;
  else{
    unsigned char *outBuf;
    z_stream stream;
    outBuf = temp.mem;
    outBuf[0] = nIn>>24 & 0xff;
    outBuf[1] = nIn>>16 & 0xff;
    outBuf[2] = nIn>>8 & 0xff;
    outBuf[3] = nIn & 0xff;
    stream.zalloc = (alloc_func)0;
    stream.zfree = (free_func)0;
    stream.opaque = 0;
    stream.avail_out = nOut;
    stream.next_out = &outBuf[4];
    deflateInit(&stream, 9);
    stream.avail_in = pIn1->used;
    stream.next_in = pIn1->mem;
    deflate(&stream, 0);
    stream.avail_in = pIn2->used;
    stream.next_in = pIn2->mem;
    deflate(&stream, 0);
    deflate(&stream, Z_FINISH);
    rc = fsl_buffer_resize(&temp, stream.total_out + 4);
    deflateEnd(&stream);
    if(!rc){
      temp.used = stream.total_out + 4;
      if( pOut==pIn1 ) fsl_buffer_reserve(pOut, 0);
      else if( pOut==pIn2 ) fsl_buffer_reserve(pOut, 0);
      assert(!pOut->mem);
      *pOut = temp;
    }else{
      fsl_buffer_reserve(&temp, 0);
    }
    return rc;
  }
}

int fsl_buffer_uncompress(fsl_buffer const * const pIn, fsl_buffer * const pOut){
  buff__errcheck(pIn);
  else buff__errcheck(pOut);
  unsigned int nOut;
  unsigned char *inBuf;
  unsigned int const nIn = pIn->used;
  fsl_buffer temp = fsl_buffer_empty;
  int rc;
  unsigned long int nOut2;
  if(nIn<=4 || !fsl_data_is_compressed(pIn->mem, pIn->used)){
    if(pIn==pOut || !pIn->mem) rc = 0;
    else{
      fsl_buffer_reuse(pOut);
      rc = fsl_buffer_append(pOut, pIn->mem, pIn->used);
    }
    assert(pOut->errCode == rc);
    goto end;
  }
  inBuf = pIn->mem;
  nOut = decode__zsize(inBuf);
  /* MARKER(("decompress size: %u\n", nOut)); */
  if(pIn!=pOut && pOut->capacity>=nOut+1){
    assert(pIn->mem != pOut->mem);
#if 0
    /* why does this cause corruption (in the form of overwriting a
       buffer somewhere in the fsl_content_get() constellation)?
       fsl_repo_rebuild() works but fsl_repo_extract() can trigger
       it:

       (FSL_RC_RANGE): Delta: copy extends past end of input
    */
    fsl_buffer_external(&temp, pOut->mem, pOut->capacity);
#else
    fsl_buffer_swap(&temp, pOut);
#endif
  }else{
    rc = fsl_buffer_reserve(&temp, nOut+1);
    if(rc) goto end;
    temp.mem[nOut] = 0;
  }

  nOut2 = (long int)nOut;
  rc = uncompress(temp.mem, &nOut2, &inBuf[4], nIn - 4)
    /* In some libz versions (<1.2.4, apparently), valgrind says
       there's an uninitialized memory access somewhere under
       uncompress(), _presumably_ for one of these arguments, but i
       can't find it. fsl_buffer_reserve() always memsets() new bytes
       to 0.

       Turns out it's a known problem:

       https://www.zlib.net/zlib_faq.html#faq36
    */;
  switch(rc){
    case 0:
      /* this is always true but having this assert
         here makes me nervous: assert(nOut2 == nOut); */
      assert(nOut2<=nOut);
      temp.mem[nOut2] = 0;
      temp.used = (fsl_size_t)nOut2;
#if 1
      fsl_buffer_swap(&temp, pOut);
#else
      if(temp.mem!=pOut->mem){
        if(pOut->capacity>=temp.capacity){
          pOut->used = 0;
          MARKER(("uncompress() re-using target buffer.\n"));
          fsl_buffer_append(pOut, temp.mem, temp.capacity);
        }else{
          fsl_buffer_swap(pOut, &temp);
        }
      }
#endif
      break;
    case Z_DATA_ERROR: rc = FSL_RC_CONSISTENCY; break;
    case Z_MEM_ERROR: rc = FSL_RC_OOM; break;
    case Z_BUF_ERROR: rc = FSL_RC_RANGE; break;
    default: rc = FSL_RC_ERROR; break;
  }
end:
  if(temp.mem!=pOut->mem) fsl_buffer_clear(&temp);
  return pOut->errCode = rc;
}

#undef decode__zsize

int fsl_buffer_fill_from( fsl_buffer * const dest, fsl_input_f src,
                          void * const state ){
  buff__errcheck(dest);
  int rc;
  enum { BufSize = 512 * 8 };
  char rbuf[BufSize];
  fsl_size_t total = 0;
  fsl_size_t rlen = 0;
  fsl_buffer_reuse(dest);
  while(1){
    rlen = BufSize;
    rc = src( state, rbuf, &rlen );
    if( rc ) break;
    total += rlen;
    if(total<rlen){
      /* Overflow! */
      rc = FSL_RC_RANGE;
      break;
    }
    if( dest->capacity < (total+1) ){
      rc = fsl_buffer_reserve( dest,
                               total + ((rlen<BufSize) ? 1 : BufSize)
                               );
      if( 0 != rc ) break;
    }
    memcpy( dest->mem + dest->used, rbuf, rlen );
    dest->used += rlen;
    if( rlen < BufSize ) break;
  }
  if( !rc && dest->used ){
    assert( dest->used < dest->capacity );
    dest->mem[dest->used] = 0;
  }
  return rc;
}

int fsl_buffer_fill_from_FILE( fsl_buffer * const dest,
                               FILE * const src ){
  return fsl_buffer_fill_from( dest, fsl_input_f_FILE, src );
}


int fsl_buffer_fill_from_filename( fsl_buffer * const dest,
                                   char const * filename ){
  buff__errcheck(dest);
  int rc;
  FILE * src;
  fsl_fstat st = fsl_fstat_empty;
  /* This stat() is only an optimization to reserve all needed
     memory up front.
  */
  rc = fsl_stat( filename, &st, 1 );
  if(!rc && st.size>0){
    rc = fsl_buffer_reserve(dest, st.size +1/*NUL terminator*/);
    if(rc) return rc;
  } /* Else it might not be a real file, e.g. "-", so we'll try anyway... */
  src = fsl_fopen(filename,"rb");
  if(!src) rc = fsl_errno_to_rc(errno, FSL_RC_IO);
  else {
    rc = fsl_buffer_fill_from( dest, fsl_input_f_FILE, src );
    fsl_fclose(src);
  }
  return rc;
}

void fsl_buffer_swap( fsl_buffer * const left, fsl_buffer * const right ){
  fsl_buffer const tmp = *left;
  *left = *right;
  *right = tmp;
}

void fsl_buffer_swap_free( fsl_buffer * const left, fsl_buffer * const right, int clearWhich ){
  fsl_buffer_swap(left, right);
  if(0 != clearWhich) fsl_buffer_reserve((clearWhich<0) ? left : right, 0);
}

int fsl_buffer_copy( fsl_buffer * const dest,
                     fsl_buffer const * const src ){
  fsl_buffer_reuse(dest);
  return src->used
    ? fsl_buffer_append( dest, src->mem, src->used )
    : 0;
}

int fsl_buffer_delta_apply2( fsl_buffer const * const orig,
                             fsl_buffer const * const pDelta,
                             fsl_buffer * const pTarget,
                             fsl_error * const pErr){
  buff__errcheck(orig);
  else buff__errcheck(pDelta);
  else buff__errcheck(pTarget);
  int rc;
  fsl_size_t n = 0;
  fsl_buffer out = fsl_buffer_empty;
  rc = fsl_delta_applied_size( pDelta->mem, pDelta->used, &n);
  if(rc){
    if(pErr){
      fsl_error_set(pErr, rc, "fsl_delta_applied_size() failed.");
    }
    return rc;
  }
  rc = fsl_buffer_resize( &out, n );
  if(0==rc){
    rc = fsl_delta_apply2( orig->mem, orig->used,
                          pDelta->mem, pDelta->used,
                          out.mem, pErr);
    if(0==rc) fsl_buffer_swap(&out, pTarget);
  }
  fsl_buffer_clear(&out);
  return rc;
}

int fsl_buffer_delta_apply( fsl_buffer const * const orig,
                            fsl_buffer const * const pDelta,
                            fsl_buffer * const pTarget){
  return fsl_buffer_delta_apply2(orig, pDelta, pTarget, NULL);
}

void fsl_buffer_defossilize( fsl_buffer * const b ){
  fsl_bytes_defossilize( b->mem, &b->used );
}

int fsl_buffer_to_filename( fsl_buffer const * const b, char const * fname ){
  buff__errcheck(b);
  FILE * f;
  int rc = 0;
  if(!b || !fname) return FSL_RC_MISUSE;
  f = fsl_fopen(fname, "wb");
  if(!f) rc = fsl_errno_to_rc(errno, FSL_RC_IO);
  else{
    if(b->used) {
      size_t const frc = fwrite(b->mem, b->used, 1, f);
      rc = (1==frc) ? 0 : FSL_RC_IO;
    }
    fsl_fclose(f);
  }
  return rc;
}

int fsl_buffer_delta_create( fsl_buffer const * const src,
                             fsl_buffer const * const newVers,
                             fsl_buffer * const delta){
  if((src == newVers)
     || (src==delta)
     || (newVers==delta)) return FSL_RC_MISUSE;
  int rc = fsl_buffer_reserve( delta, newVers->used + 60 );
  if(!rc){
    delta->used = 0;
    rc = fsl_delta_create( src->mem, src->used,
                           newVers->mem, newVers->used,
                           delta->mem, &delta->used );
  }
  return rc;
}


int fsl_output_f_buffer( void * state, void const * src, fsl_size_t n ){
  return fsl_buffer_append((fsl_buffer*)state, src, n);
}

int fsl_finalizer_f_buffer( void * state fsl__unused, void * mem ){
  fsl_buffer * const b = (fsl_buffer*)mem;
  (void)state;
  fsl_buffer_reserve(b, 0);
  *b = fsl_buffer_empty;
  return 0;
}

int fsl_buffer_strftime(fsl_buffer * const b, char const * format,
                        const struct tm *timeptr){
  if(!b || !format || !*format || !timeptr) return FSL_RC_MISUSE;
  else{
    enum {BufSize = 128};
    char buf[BufSize];
    fsl_size_t const len = fsl_strftime(buf, BufSize, format, timeptr);
    return len ? fsl_buffer_append(b, buf, (fsl_int_t)len) : FSL_RC_RANGE;
  }
}

int fsl_buffer_stream_lines(fsl_output_f fTo, void * const toState,
                            fsl_buffer * const pFrom, fsl_size_t N){
  buff__errcheck(pFrom);
  char *z = (char *)pFrom->mem;
  fsl_size_t i = pFrom->cursor;
  fsl_size_t n = pFrom->used;
  fsl_size_t cnt = 0;
  int rc = 0;
  if( N==0 ) return 0;
  while( i<n ){
    if( z[i]=='\n' ){
      cnt++;
      if( cnt==N ){
        i++;
        break;
      }
    }
    i++;
  }
  if( fTo ){
    rc = fTo(toState, &pFrom->mem[pFrom->cursor], i - pFrom->cursor);
  }
  if(!rc){
    pFrom->cursor = i;
  }
  return rc;
}


int fsl_buffer_copy_lines(fsl_buffer * const pTo,
                          fsl_buffer * const pFrom,
                          fsl_size_t N){
#if 1
  if(pTo) buff__errcheck(pTo);
  return fsl_buffer_stream_lines( pTo ? fsl_output_f_buffer : NULL, pTo,
                                  pFrom, N );
#else
  char *z = (char *)pFrom->mem;
  fsl_size_t i = pFrom->cursor;
  fsl_size_t n = pFrom->used;
  fsl_size_t cnt = 0;
  int rc = 0;
  if( N==0 ) return 0;
  while( i<n ){
    if( z[i]=='\n' ){
      ++cnt;
      if( cnt==N ){
        ++i;
        break;
      }
    }
    ++i;
  }
  if( pTo ){
    rc = fsl_buffer_append(pTo, &pFrom->mem[pFrom->cursor], i - pFrom->cursor);
  }
  if(!rc){
    pFrom->cursor = i;
  }
  return rc;
#endif
}

int fsl_input_f_buffer( void * state, void * dest, fsl_size_t * n ){
  fsl_buffer * const b = (fsl_buffer*)state;
  buff__errcheck(b);
  fsl_size_t const from = b->cursor;
  fsl_size_t to;
  fsl_size_t c;
  if(from >= b->used){
    *n = 0;
    return 0;
  }
  to = from + *n;
  if(to>b->used) to = b->used;
  c = to - from;
  if(c){
    memcpy(dest, b->mem+from, c);
    b->cursor += c;
  }
  *n = c;
  return 0;
}

int fsl_buffer_compare_file( fsl_buffer const * b, char const * zFile ){
  int rc;
  fsl_fstat fst = fsl_fstat_empty;
  rc = fsl_stat(zFile, &fst, 1);
  if(rc || (FSL_FSTAT_TYPE_FILE != fst.type)) return -1;
  else if(b->used < fst.size) return -1;
  else if(b->used > fst.size) return 1;
  else{
#if 1
    FILE * f;
    f = fsl_fopen(zFile,"r");
    if(!f) rc = -1;
    else{
      fsl_buffer fc = *b /* so fsl_input_f_buffer() can manipulate its
                            cursor */;
      rc = fsl_stream_compare(fsl_input_f_buffer, &fc,
                              fsl_input_f_FILE, f);
      assert(fc.mem==b->mem);
      fsl_fclose(f);
    }

#else
    fsl_buffer fc = fsl_buffer_empty;
    rc = fsl_buffer_fill_from_filename(&fc, zFile);
    if(rc){
      rc = -1;
    }else{
      rc = fsl_buffer_compare(b, &fc);
    }
    fsl_buffer_clear(&fc);
#endif
    return rc;
  }
}

char * fsl_buffer_take(fsl_buffer * const b){
  char * z = NULL;
  if(0==buf__materialize(b,0)){
    z = (char *)b->mem;
    *b = fsl_buffer_empty;
  }
  return z;
}

fsl_size_t fsl_buffer_seek(fsl_buffer * const b, fsl_int_t offset,
                           fsl_buffer_seek_e  whence){
  int64_t c = (int64_t)b->cursor;
  switch(whence){
    case FSL_BUFFER_SEEK_SET: c = offset; break;
    case FSL_BUFFER_SEEK_CUR: c = (int64_t)b->cursor + offset; break;
    case FSL_BUFFER_SEEK_END:
      c = (int64_t)b->used + offset;
      /* ^^^^^ fossil(1) uses (used + offset - 1) but

         That seems somewhat arguable because (used + 0 - 1) is at the
         last-written byte (or 1 before the begining), not the
         one-past-the-end point (which corresponds to the
         "end-of-file" described by the fseek() man page). It then
         goes on, in other algos, to operate on that final byte using
         that position, e.g.  blob_read() after a seek-to-end would
         read that last byte, rather than treating the buffer as being
         at the end.

         So... i'm going to naively remove that -1 bit.

         About 12 years later (2025-08-21): that distinction has never
         been significant in practice, probably because SEEK_END
         support is "for completeness" but not actually used. Funnily
         enough, fossil(1) removed SEEK_END support from that class on
         2021-03-1: https://fossil-scm.org/home/info/6fc730e0c76d4bd2
      */
      break;
  }
  if(!b->used || c<0) b->cursor = 0;
  else if((fsl_size_t)c > b->used) b->cursor = b->used;
  else b->cursor = (fsl_size_t)c;
  return b->cursor;
}

fsl_size_t fsl_buffer_tell(fsl_buffer const * const b){
  return b->cursor;
}

void fsl_buffer_rewind(fsl_buffer * const b){
  b->cursor = 0;
}

/*
   Like getdelim(3) but for fsl_buffer structures.
   https://pubs.opengroup.org/onlinepubs/9799919799/functions/getdelim.html
 */
fsl_int_t fsl_buffer_getdelim(char **ret, size_t *retsz,
                              int delim, fsl_buffer *buf){
  char *p;
  unsigned char *sep;
  size_t psz, len = 0;
  static const uint8_t allocsz = 128;

  if(ret == NULL || retsz == NULL){
    buf->errCode = FSL_RC_MISUSE;
    return -1;
  }
  if(buf->errCode != 0 || buf->used == 0 || buf->cursor >= buf->used){
    return -1;  /* error or end-of-buffer */
  }
  if(*ret == NULL) *retsz = 0;

  sep = memchr(&buf->mem[buf->cursor], delim, buf->used - buf->cursor);
  if(sep == NULL) len = buf->used - buf->cursor;
  else len = (sep - &buf->mem[buf->cursor]) + 1;

  if(len > LONG_MAX){
    buf->errCode = FSL_RC_RANGE;
    return -1;
  }

  psz = len + 1;  /* NUL terminator */
  if(psz > *retsz){
    if(psz < allocsz){
      /* smallest power of 2 greater than common source code line length */
      psz = allocsz;
    }
    p = fsl_realloc(*ret, psz);
    if(p == NULL){
      buf->errCode = FSL_RC_OOM;
      return -1;
    }
    memset(&p[*retsz], 0, psz - *retsz);
    *ret = p;
    *retsz = psz;
  }

  if(*ret != NULL){
    memcpy(*ret, &buf->mem[buf->cursor], len);
    *(*ret + len) = '\0';
  }
  buf->cursor += len;
  return len;
}

/*
   Like getline(3) but for fsl_buffer structures.
   https://pubs.opengroup.org/onlinepubs/9799919799/functions/getline.html
 */
fsl_int_t fsl_buffer_getline(char **ret, size_t *retsz, fsl_buffer *buf){
  return fsl_buffer_getdelim(ret, retsz, '\n', buf);
}

int fsl_id_bag_to_buffer(fsl_id_bag const * bag, fsl_buffer * const b,
                         char const * separator){
  int i = 0;
  fsl_int_t const sepLen = (fsl_id_t)fsl_strlen(separator);
  fsl_buffer_reserve(b, b->used + (bag->entryCount * 7)
                     + (bag->entryCount * sepLen));
  for(fsl_id_t e = fsl_id_bag_first(bag);
      !b->errCode && e; e = fsl_id_bag_next(bag, e)){
    if(i++) fsl_buffer_append(b, separator, sepLen);
    fsl_buffer_appendf(b, "%" FSL_ID_T_PFMT, e);
  }
  return b->errCode;
}

int fsl_buffer_append_hex( fsl_buffer * const b, void const * m,
                           fsl_uint_t n, bool upperCase ){
  const char * zdigits = upperCase ? "0123456789ABCDEF" : "0123456789abcdef";
  unsigned char const * z = m;
  fsl_buffer_reserve(b, b->used + n*2+1);
  for(unsigned int i = 0; i < n && !b->errCode; ++i, ++z){
    fsl_buffer_appendch(b, zdigits[*z >> 4]);
    fsl_buffer_appendch(b, zdigits[*z & 0x0f]);
  }
  return b->errCode;
}

int fsl_buffer_append_tcl_literal(fsl_buffer * const b,
                                  bool escapeSquigglies,
                                  char const * z, fsl_int_t n){
  int rc;
  if(n<0) n = fsl_strlen(z);
  fsl_buffer_reserve(b, b->used + (fsl_size_t)n + 3);
  fsl_buffer_appendch(b, '"');
  for(fsl_int_t i=0; 0==b->errCode && i<n; ++i){
    char c = z[i];
    bool skipSlash = false;
    switch( c ){
      case '\r':  c = 'r'; goto slash;
      case '}': case '{': skipSlash = !escapeSquigglies;
        FSL_SWITCH_FALL_THROUGH;
      case '[':
      case ']':
      case '$':
      case '"':
      case '\\':
      slash:
        if(!skipSlash && (rc = fsl_buffer_appendch(b, '\\'))) break;
        FSL_SWITCH_FALL_THROUGH;
      default:
        fsl_buffer_appendch(b, c);
    }
  }
  fsl_buffer_appendch(b, '"');
  return b->errCode;
}

int fsl_buffer_appendch(fsl_buffer * const b, char c){
  buff__errcheck(b);
  else if( buf__materialize(b, 2) ){
    return b->errCode;
  }else if( b->used+1 < b->capacity ){
    assert( !b->errCode );
    b->mem[b->used++] = c;
    b->mem[b->used] = 0;
    return 0;
  }else{
    return fsl_buffer_append(b, &c, 1);
  }
}

int fsl_buffer_ensure_eol(fsl_buffer * const b){
  if(!b->errCode && b->used
     && b->mem[b->used-1] != '\n'){
    fsl_buffer_reserve(b, b->used+2)/*may materialize b*/;
    fsl_buffer_appendch(b, '\n');
  }
  return b->errCode;
}

void fsl_buffer_chomp(fsl_buffer * const b){
  if(!b->errCode && b->used && b->mem[b->used-1]=='\n'){
    --b->used;
    if( b->used && b->mem[b->used-1]=='\r' ){
      --b->used;
    }
    if( !buf__is_external(b) ){
      b->mem[b->used] = 0;
    }
  }
}

/*
  The following escaping code to support fsl_buffer_esc_arg_v2()
  was taken verbatim from fossil(1) blob.c on 2025-06-11.
*/
/*
** ASCII (for reference):
**    x0  x1  x2  x3  x4  x5  x6  x7  x8  x9  xa  xb  xc  xd  xe  xf
** 0x ^`  ^a  ^b  ^c  ^d  ^e  ^f  ^g  \b  \t  \n  ()  \f  \r  ^n  ^o
** 1x ^p  ^q  ^r  ^s  ^t  ^u  ^v  ^w  ^x  ^y  ^z  ^{  ^|  ^}  ^~  ^
** 2x ()  !   "   #   $   %   &   '   (   )   *   +   ,   -   .   /
** 3x 0   1   2   3   4   5   6   7   8   9   :   ;   <   =   >   ?
** 4x @   A   B   C   D   E   F   G   H   I   J   K   L   M   N   O
** 5x P   Q   R   S   T   U   V   W   X   Y   Z   [   \   ]   ^   _
** 6x `   a   b   c   d   e   f   g   h   i   j   k   l   m   n   o
** 7x p   q   r   s   t   u   v   w   x   y   z   {   |   }   ~   ^_
*/

/*
** Meanings for bytes in a filename:
**
**    0      Ordinary character.  No encoding required
**    1      Needs to be escaped
**    2      Illegal character.  Do not allow in a filename
**    3      First byte of a 2-byte UTF-8
**    4      First byte of a 3-byte UTF-8
**    5      First byte of a 4-byte UTF-8
*/
static const char aSafeChar[256] = {
#ifdef _WIN32
/* Windows
** Prohibit:  all control characters, including tab, \r and \n.
** Escape:    (space) " # $ % & ' ( ) * ; < > ? [ ] ^ ` { | }
*/
/*  x0  x1  x2  x3  x4  x5  x6  x7  x8  x9  xa  xb  xc  xd  xe  xf  */
     2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2, /* 0x */
     2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2, /* 1x */
     1,  0,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,  0,  0,  0,  0, /* 2x */
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  0,  1,  1, /* 3x */
     1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, /* 4x */
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  0,  1,  1,  0, /* 5x */
     1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, /* 6x */
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  0,  1, /* 7x */
#else
/* Unix
** Prohibit:  all control characters, including tab, \r and \n
** Escape:    (space) ! " # $ % & ' ( ) * ; < > ? [ \ ] ^ ` { | }
*/
/*  x0  x1  x2  x3  x4  x5  x6  x7  x8  x9  xa  xb  xc  xd  xe  xf  */
     2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2, /* 0x */
     2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2, /* 1x */
     1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  0,  0,  0,  0,  0, /* 2x */
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  0,  1,  1, /* 3x */
     1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, /* 4x */
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,  0, /* 5x */
     1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, /* 6x */
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  0,  1, /* 7x */
#endif
    /* all bytes 0x80 through 0xbf are unescaped, being secondary
    ** bytes to UTF8 characters.  Bytes 0xc0 through 0xff are the
    ** first byte of a UTF8 character and do get escaped */
     2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2, /* 8x */
     2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2, /* 9x */
     2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2, /* ax */
     2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2, /* bx */
     3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3, /* cx */
     3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3, /* dx */
     4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4, /* ex */
     5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5,  5  /* fx */
};


int fsl_buffer_esc_arg(fsl_buffer * const b, char const *zIn,
                      bool isFilename){
  return fsl_buffer_esc_arg_v2(b, NULL, zIn, isFilename);
}

int fsl_buffer_esc_arg_v2(fsl_buffer * const b,
                          fsl_error * const err,
                          char const *zIn,
                          bool isFilename){
  /* What follows was ported directly from fossil(1)'s
     blob.c:blob_append_escaped_arg(), modified only stylistically,
     not functionally. */
  int i;
  unsigned char c;
  bool needEsc = false;
  fsl_size_t nB;
  char const * zB;

  buff__errcheck(b);
  else if( buf__materialize(b, fsl_strlen(zIn) + 3 ) ){
    return b->errCode;
  }
  if( err ) fsl_error_reset(err);
  zB = fsl_buffer_cstr2(b, &nB);

  /* Look for illegal byte-sequences and byte-sequences that require
  ** escaping.  No control-characters are allowed.  All spaces and
  ** non-ASCII unicode characters and some punctuation characters require
  ** escaping. */
  for(i=0; (c = (unsigned char)zIn[i])!=0; i++){
    unsigned char const x = aSafeChar[c];
    if( x ){
      needEsc = true;
      if( 2==x ){
        if( err ){
          fsl_error_set(err, FSL_RC_SYNTAX,
                        "Input contains a character (ASCII 0x%02x) "
                        "that is not permitted by %s()",
                        c, __func__);
        }
        return b->errCode = FSL_RC_SYNTAX;
      }else if( x>2 ){
        if( (zIn[i+1] & 0xc0)!=0x80
            || (x>=4 && (zIn[i+2] & 0xc0)!=0x80)
            || (x>=5 && (zIn[i+3] & 0xc0)!=0x80) ){
          if( err ){
            fsl_error_set(err, FSL_RC_SYNTAX,
                          "The input contains an illegal UTF-8 "
                          "character");
          }
          return b->errCode = FSL_RC_SYNTAX;
        }
        i += x-2;
      }
      /* else it's handled below */
    }
  }

  /* Separate it from the previous argument with a space */
  if( nB>0 && !fsl_isspace(zB[nB-1]) ){
    fsl_buffer_appendch(b, ' ');
  }

  /* Check for characters that need quoting */
  if( !needEsc ){
    if( isFilename && zIn[0]=='-' ){
      fsl_buffer_appendch(b, '.');
#if defined(_WIN32)
      fsl_buffer_appendch(b, '\\');
#else
      fsl_buffer_appendch(b, '/');
#endif
    }
    fsl_buffer_append(b, zIn, -1);
  }else{
#if defined(_WIN32)
    /* Quoting strategy for windows:
    ** Put the entire name inside of "...".  Any " characters within
    ** the name get doubled. If input looks like a command flag,
    ** prefix it with ".\" so that it does not get mistaken for
    ** one. */
    */
    fsl_buffer_appendch(b, '"');
    if( isFilename && zIn[0]=='-' ){
      fsl_buffer_appendch(b, '.');
      fsl_buffer_appendch(b, '\\');
    }else if( zIn[0]=='/' ){
      fsl_buffer_appendch(b, '.');
    }
    for(i=0; 0==b->errCode
          && (c = (unsigned char)zIn[i])!=0; i++){
      fsl_buffer_appendch(b, (char)c);
      if( c=='"' ) fsl_buffer_appendch(b, '"');
      if( c=='\\' ) fsl_buffer_appendch(b, '\\');
      if( c=='%' && isFilename ) fsl_buffer_append(b, "%cd:~,%", 7);
    }
    fsl_buffer_appendch(b, '"');
#else
    /* Quoting strategy for unix:
    ** If the name does not contain ', then surround the whole thing
    ** with '...'.  If there is one or more ' characters within the
    ** name, then put \ before each special character. If input looks
    ** like a command flag, prefix it with "./" so that it does not
    ** get mistaken for one.
    */
    if( strchr(zIn,'\'') ){
      if( isFilename && zIn[0]=='-' ){
        fsl_buffer_appendch(b, '.');
        fsl_buffer_appendch(b, '/');
      }
      for(i=0; 0==b->errCode
            && (c = (unsigned char)zIn[i])!=0; i++){
        if( aSafeChar[c] && aSafeChar[c]!=2 ) fsl_buffer_appendch(b, '\\');
        fsl_buffer_appendch(b, (char)c);
      }
    }else{
      fsl_buffer_appendch(b, '\'');
      if( isFilename && zIn[0]=='-' ){
        fsl_buffer_appendch(b, '.');
        fsl_buffer_appendch(b, '/');
      }
      fsl_buffer_append(b, zIn, -1);
      fsl_buffer_appendch(b, '\'');
    }
#endif
  }
  if( err && !b->errCode ) b->errCode = err->code;
  return b->errCode;
}

#undef MARKER
#undef buf__is_external
#undef buf__errcheck
#undef buf__materialize
