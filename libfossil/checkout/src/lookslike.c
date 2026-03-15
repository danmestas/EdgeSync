/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */ 
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/

#include "libfossil.h"
#include "fossil-scm/internal.h"
#include <string.h> /* memcmp() */


/* definitions for various UTF-8 sequence lengths, encoded as start value
 * and size of each valid range belonging to some lead byte*/
#define US2A  0x80, 0x01 /* for lead byte 0xC0 */
#define US2B  0x80, 0x40 /* for lead bytes 0xC2-0xDF */
#define US3A  0xA0, 0x20 /* for lead byte 0xE0 */
#define US3B  0x80, 0x40 /* for lead bytes 0xE1-0xEF */
#define US4A  0x90, 0x30 /* for lead byte 0xF0 */
#define US4B  0x80, 0x40 /* for lead bytes 0xF1-0xF3 */
#define US4C  0x80, 0x10 /* for lead byte 0xF4 */
#define US0A  0x00, 0x00 /* for any other lead byte */

/* a table used for quick lookup of the definition that goes with a
 * particular lead byte */
static const unsigned char lb_tab[] = {
  US0A, US0A, US0A, US0A, US0A, US0A, US0A, US0A,
  US0A, US0A, US0A, US0A, US0A, US0A, US0A, US0A,
  US0A, US0A, US0A, US0A, US0A, US0A, US0A, US0A,
  US0A, US0A, US0A, US0A, US0A, US0A, US0A, US0A,
  US0A, US0A, US0A, US0A, US0A, US0A, US0A, US0A,
  US0A, US0A, US0A, US0A, US0A, US0A, US0A, US0A,
  US0A, US0A, US0A, US0A, US0A, US0A, US0A, US0A,
  US0A, US0A, US0A, US0A, US0A, US0A, US0A, US0A,
  US2A, US0A, US2B, US2B, US2B, US2B, US2B, US2B,
  US2B, US2B, US2B, US2B, US2B, US2B, US2B, US2B,
  US2B, US2B, US2B, US2B, US2B, US2B, US2B, US2B,
  US2B, US2B, US2B, US2B, US2B, US2B, US2B, US2B,
  US3A, US3B, US3B, US3B, US3B, US3B, US3B, US3B,
  US3B, US3B, US3B, US3B, US3B, US3B, US3B, US3B,
  US4A, US4B, US4B, US4B, US4C, US0A, US0A, US0A,
  US0A, US0A, US0A, US0A, US0A, US0A, US0A, US0A
};

#undef US2A
#undef US2B
#undef US3A
#undef US3B
#undef US4A
#undef US4B
#undef US4C
#undef US0A

bool fsl_looks_like_binary(fsl_buffer const * const b){
  return (fsl_looks_like_utf8(b, FSL_LOOKSLIKE_BINARY) & FSL_LOOKSLIKE_BINARY)
    != FSL_LOOKSLIKE_NONE;
}

int fsl_looks_like_utf8(fsl_buffer const * const b, int stopFlags){
  fsl_size_t n = 0;
  const char *z = fsl_buffer_cstr2(b, &n);
  int j, c, flags = FSL_LOOKSLIKE_NONE;  /* Assume UTF-8 text, prove otherwise */

  if( n==0 ) return flags;  /* Empty file -> text */
  c = *z;
  if( c==0 ){
    flags |= FSL_LOOKSLIKE_NUL;  /* NUL character in a file -> binary */
  }else if( c=='\r' ){
    flags |= FSL_LOOKSLIKE_CR;
    if( n<=1 || z[1]!='\n' ){
      flags |= FSL_LOOKSLIKE_LONE_CR;  /* Not enough chars or next char not LF */
    }
  }
  j = (c!='\n');
  if( !j ) flags |= (FSL_LOOKSLIKE_LF | FSL_LOOKSLIKE_LONE_LF);  /* Found LF as first char */
  while( !(flags&stopFlags) && --n>0 ){
    int c2 = c;
    c = *++z; ++j;
    if( c==0 ){
      flags |= FSL_LOOKSLIKE_NUL;  /* NUL character in a file -> binary */
    }else if( c=='\n' ){
      flags |= FSL_LOOKSLIKE_LF;
      if( c2=='\r' ){
        flags |= (FSL_LOOKSLIKE_CR | FSL_LOOKSLIKE_CRLF);  /* Found LF preceded by CR */
      }else{
        flags |= FSL_LOOKSLIKE_LONE_LF;
      }
      if( j>FSL__LINE_LENGTH_MASK ){
        flags |= FSL_LOOKSLIKE_LONG;  /* Very long line -> binary */
      }
      j = 0;
    }else if( c=='\r' ){
      flags |= FSL_LOOKSLIKE_CR;
      if( n<=1 || z[1]!='\n' ){
        flags |= FSL_LOOKSLIKE_LONE_CR;  /* Not enough chars or next char not LF */
      }
    }
  }
  if( n ){
    flags |= FSL_LOOKSLIKE_SHORT;  /* The whole blob was not examined */
  }
  if( j>FSL__LINE_LENGTH_MASK ){
    flags |= FSL_LOOKSLIKE_LONG;  /* Very long line -> binary */
  }
  return flags;
}

unsigned char const *fsl_utf8_bom(unsigned int *pnByte){
  static const unsigned char bom[] = {
    0xef, 0xbb, 0xbf, 0x00, 0x00, 0x00
  };
  if( pnByte ) *pnByte = 3;
  return bom;
}

bool fsl_starts_with_bom_utf8(fsl_buffer const * const b,
                              unsigned int *pBomSize){
  unsigned int bomSize;
  const char * const z = fsl_buffer_cstr(b);
  const unsigned char * const bom = fsl_utf8_bom(&bomSize);
  if( pBomSize ) *pBomSize = bomSize;
  return fsl_buffer_size(b)<bomSize
    ? false
    : memcmp(z, bom, bomSize)==0;
}

bool fsl_invalid_utf8(fsl_buffer const * const b){
  fsl_size_t n = 0;
  const unsigned char *z = (unsigned char *) fsl_buffer_cstr2(b, &n);
  unsigned char c; /* lead byte to be handled. */
  if( n==0 ) return false;  /* Empty file -> OK */
  c = *z;
  while( --n>0 ){
    if( c>=0x80 ){
      const unsigned char *def; /* pointer to range table*/
      c <<= 1; /* multiply by 2 and get rid of highest bit */
      def = &lb_tab[c]; /* search fb's valid range in table */
      if( (unsigned int)(*++z-def[0])>=def[1] ){
        return false/*FSL_LOOKSLIKE_INVALID*/;
      }
      c = (c>=0xC0) ? (c|3) : ' '; /* determine next lead byte */
    } else {
      c = *++z;
    }
  }
  return c<0x80 /* Final lead byte must be ASCII. */;
}
