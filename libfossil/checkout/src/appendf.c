/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/************************************************************************
The printf-like implementation in this file is based on the one found
in the sqlite3 distribution is in the Public Domain.

This copy was forked for use with the clob API in Feb 2008 by Stephan
Beal (https://wanderinghorse.net/home/stephan/) and modified to send
its output to arbitrary targets via a callback mechanism. Also
refactored the %X specifier handlers a bit to make adding/removing
specific handlers easier.

All code in this file is released into the Public Domain.

The printf implementation (fsl_appendfv()) is pretty easy to extend
(e.g. adding or removing %-specifiers for fsl_appendfv()) if you're
willing to poke around a bit and see how the specifiers are declared
and dispatched. For an example, grep for 'etSTRING' and follow it
through the process of declaration to implementation.

LICENSE for the version in the libfossil source tree:

  This program is free software; you can redistribute it and/or
  modify it under the terms of the Simplified BSD License (also
  known as the "2-Clause License" or "FreeBSD License".)

  This program is distributed in the hope that it will be useful,
  but without any warranty; without even the implied warranty of
  merchantability or fitness for a particular purpose.
**********************************************************************/

#include "fossil-scm/util.h"
#include "fossil-scm/core.h" /* fsl_rc_e */
#include "fossil-scm/internal.h" /* fsl__cx_scratchpad() */
#include <string.h> /* strlen(), strcspn() */
#include <ctype.h>
#include <assert.h>

#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

/**
   SQLite eliminated all "long double" support in version 3.47, and
   all of this code originally (back in the 200x's) came from there,
   so we'll stop using "long double" here too.
*/
typedef double LONGDOUBLE_TYPE;

/*
  Most C compilers handle variable-sized arrays, so we enable
  that by default. Some (e.g. tcc) do not, so we provide a way
  to disable it: set FSLPRINTF_HAVE_VARARRAY to 0.

  One approach would be to look at:

  defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)

  but some compilers support variable-sized arrays even when not
  explicitly running in c99 mode.
*/
/*
  2022-05-17: apparently VLAs were made OPTIONAL in C11 and MSVC
  decided not to support them. So we'll go ahead and remove the VLA
  usage altogether.
*/
#define FSLPRINTF_HAVE_VARARRAY 0
#if 0
#if !defined(FSLPRINTF_HAVE_VARARRAY)
#  if defined(__TINYC__)
#    define FSLPRINTF_HAVE_VARARRAY 0
#  else
#    if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)
#        define FSLPRINTF_HAVE_VARARRAY 1 /*use 1 in C99 mode */
#    else
#        define FSLPRINTF_HAVE_VARARRAY 0
#    endif
#  endif
#endif
#endif

/*
  Conversion types fall into various categories as defined by the
  following enumeration.
*/
enum PrintfCategory {
  etRADIX = 1, /* Integer types.  %d, %x, %o, and so forth */
  etFLOAT = 2, /* Floating point.  %f */
  etEXP = 3, /* Exponentional notation. %e and %E */
  etGENERIC = 4, /* Floating or exponential, depending on exponent. %g */
  /* 5 can be reused. Formerly etSIZE (%n) */
  etSTRING = 6, /* Strings. %s */
  etDYNSTRING = 7, /* Dynamically allocated strings. %z */
  etPERCENT = 8, /* Percent symbol. %% */
  etCHARX = 9, /* Characters. %c */
  /* The rest are extensions, not normally found in printf() */
  etCHARLIT = 10, /* Literal characters.  %' */
  etSQLESCAPE = 11, /* Strings with '\'' doubled.  %q */
  etSQLESCAPE2 = 12, /* Strings with '\'' doubled and enclosed in '',
                        NULL pointers replaced by SQL NULL.  %Q */
  etSQLESCAPE3 = 13, /* %!Q -> SQL identifiers wrapped in \" with
                        inner '\"' doubled */
  etBLOBSQL = 14, /* %B -> Works like %Q,
                     but requires a (fsl_buffer*) argument. */
  etPOINTER = 15, /* The %p conversion */
  etORDINAL = 17, /* %r -> 1st, 2nd, 3rd, 4th, etc.  English only */
  etHTML = 18, /* %h -> basic HTML escaping. */
  etURLENCODE = 19, /* %t -> URL encoding. */
  etURLDECODE = 20, /* %T -> URL decoding. */
  etPATH = 21, /* %/ -> replace '\\' with '/' in path-like strings. */
  etBLOB = 22, /* Works like %s, but requires a (fsl_buffer*) argument. */
  etFOSSILIZE = 23, /* %F => like %s, but fossilizes it. */
  etSTRINGID = 24, /* String with length limit for a UUID prefix: %S */
  etJSONSTR = 25,  /* %j => JSON-escaped string. %!j adds outer
                       double-quotes to it. */
  etFSLRC = 26, /* %R => render as fsl_rc_cstr(int ARG) */
#if 0
  etSHELLESC = 27,
  /* %$ => shell-escapes a token, e.g. command name. That can only
     work if the output is all going to a single fsl_buffer, but this
     implementation is one layer of abstraction too far away to know
     that. */
#endif
  etPLACEHOLDER = 100
};

/**
   fsl_appendf_spec_handler is an almost-generic interface for farming
   work out of fsl_appendfv()'s code into external functions.  It doesn't
   actually save much (if any) overall code, but it makes the fsl_appendfv()
   code more manageable.


   REQUIREMENTS of implementations:

   - Expects an implementation-specific vargp pointer.
   fsl_appendfv() passes a pointer to the converted value of
   an entry from the format va_list. If it passes a type
   other than the expected one, undefined results.

   - If it calls pf it must do: pf( pfArg, D, N ), where D is
   the data to export and N is the number of bytes to export.
   It may call pf() an arbitrary number of times

   - If pf() successfully is called, the return value must be the
   accumulated totals of its return value(s), plus (possibly, but
   unlikely) an implementation-specific amount.

   - If it does not call pf() then it must return 0 (success)
   or a negative number (an error) or do all of the export
   processing itself and return the number of bytes exported.

   SIGNIFICANT LIMITATIONS:

   - Has no way of iterating over the format string, so handling
   precisions and such here can't work too well. (Nevermind:
   precision/justification is handled in fsl_appendfv().)
*/
typedef int (*fsl_appendf_spec_handler)( fsl_output_f pf,
                                         void * pfArg,
                                         unsigned int pfLen,
                                         void * vargp );

static int spech_string_to_html( fsl_output_f pf, void * pfArg,
                                 unsigned int pfLen, void * varg );
static int spech_urlencode( fsl_output_f pf, void * pfArg,
                            unsigned int pfLen, void * varg );
static int spech_urldecode( fsl_output_f pf, void * pfArg,
                            unsigned int pfLen, void * varg );
/*
  An "etByte" is an 8-bit unsigned value.
*/
typedef unsigned char etByte;

/*
  Each builtin conversion character (ex: the 'd' in "%d") is described
  by an instance of this struct.
*/
typedef struct et_info {   /* Information about each format field */
  char fmttype;            /* The format field code letter */
  etByte base;             /* The base for radix conversion */
  etByte flags;            /* One or more of FLAG_ constants below */
  etByte type;             /* Conversion paradigm */
  etByte charset;          /* Offset into aDigits[] of the digits string */
  etByte prefix;           /* Offset into aPrefix[] of the prefix string */
  fsl_appendf_spec_handler callback;
} et_info;

/*
  Allowed values for et_info.flags
*/
enum et_info_flags {
  FLAG_SIGNED = 1,    /* True if the value to convert is signed */
  FLAG_EXTENDED = 2,  /* True if for internal/extended use only. */
  FLAG_STRING = 4     /* Allow infinity precision */
};

/*
  Historically, the following table was searched linearly, so the most
  common conversions were kept at the front.

  Change 2008 Oct 31 by Stephan Beal: we reserve an array of ordered
  entries for all chars in the range [32..126]. Format character
  checks can now be done in constant time by addressing that array
  directly.  This takes more static memory, but reduces the time and
  per-call overhead costs of fsl_appendfv().
*/
static const char aDigits[] = "0123456789ABCDEF0123456789abcdef";
static const char aPrefix[] = "-x0\000X0";
static const et_info fmtinfo[] = {
/*
  These entries MUST stay in ASCII order, sorted on their fmttype
  member! They MUST start with fmttype==32 and end at fmttype==126.
*/
{' '/*32*/, 0, 0, 0, 0, 0, 0 },
{'!'/*33*/, 0, 0, 0, 0, 0, 0 },
{'"'/*34*/, 0, 0, 0, 0, 0, 0 },
{'#'/*35*/, 0, 0, 0, 0, 0, 0 },
{'$'/*36*/, 0, 0, 0, 0, 0, 0 },
{'%'/*37*/, 0, 0, etPERCENT, 0, 0, 0 },
{'&'/*38*/, 0, 0, 0, 0, 0, 0 },
{'\''/*39*/, 0, 0, 0, 0, 0, 0 },
{'('/*40*/, 0, 0, 0, 0, 0, 0 },
{')'/*41*/, 0, 0, 0, 0, 0, 0 },
{'*'/*42*/, 0, 0, 0, 0, 0, 0 },
{'+'/*43*/, 0, 0, 0, 0, 0, 0 },
{','/*44*/, 0, 0, 0, 0, 0, 0 },
{'-'/*45*/, 0, 0, 0, 0, 0, 0 },
{'.'/*46*/, 0, 0, 0, 0, 0, 0 },
{'/'/*47*/, 0, 0, etPATH, 0, 0, 0 },
{'0'/*48*/, 0, 0, 0, 0, 0, 0 },
{'1'/*49*/, 0, 0, 0, 0, 0, 0 },
{'2'/*50*/, 0, 0, 0, 0, 0, 0 },
{'3'/*51*/, 0, 0, 0, 0, 0, 0 },
{'4'/*52*/, 0, 0, 0, 0, 0, 0 },
{'5'/*53*/, 0, 0, 0, 0, 0, 0 },
{'6'/*54*/, 0, 0, 0, 0, 0, 0 },
{'7'/*55*/, 0, 0, 0, 0, 0, 0 },
{'8'/*56*/, 0, 0, 0, 0, 0, 0 },
{'9'/*57*/, 0, 0, 0, 0, 0, 0 },
{':'/*58*/, 0, 0, 0, 0, 0, 0 },
{';'/*59*/, 0, 0, 0, 0, 0, 0 },
{'<'/*60*/, 0, 0, 0, 0, 0, 0 },
{'='/*61*/, 0, 0, 0, 0, 0, 0 },
{'>'/*62*/, 0, 0, 0, 0, 0, 0 },
{'?'/*63*/, 0, 0, 0, 0, 0, 0 },
{'@'/*64*/, 0, 0, 0, 0, 0, 0 },
{'A'/*65*/, 0, 0, 0, 0, 0, 0 },
{'B'/*66*/, 0, 2, etBLOBSQL, 0, 0, 0 },
{'C'/*67*/, 0, 0, 0, 0, 0, 0 },
{'D'/*68*/, 0, 0, 0, 0, 0, 0 },
{'E'/*69*/, 0, FLAG_SIGNED, etEXP, 14, 0, 0 },
{'F'/*70*/, 0, 4, etFOSSILIZE, 0, 0, 0 },
{'G'/*71*/, 0, FLAG_SIGNED, etGENERIC, 14, 0, 0 },
{'H'/*72*/, 0, 0, 0, 0, 0, 0 },
{'I'/*73*/, 0, 0, 0, 0, 0, 0 },
{'J'/*74*/, 0, 0, 0, 0, 0, 0 },
{'K'/*75*/, 0, 0, 0, 0, 0, 0 },
{'L'/*76*/, 0, 0, 0, 0, 0, 0 },
{'M'/*77*/, 0, 0, 0, 0, 0, 0 },
{'N'/*78*/, 0, 0, 0, 0, 0, 0 },
{'O'/*79*/, 0, 0, 0, 0, 0, 0 },
{'P'/*80*/, 0, 0, 0, 0, 0, 0 },
{'Q'/*81*/, 0, FLAG_STRING, etSQLESCAPE2, 0, 0, 0 },
{'R'/*82*/, 0, 0, etFSLRC, 0, 0, 0 },
{'S'/*83*/, 0, FLAG_STRING, etSTRINGID, 0, 0, 0 },
{'T'/*84*/, 0, FLAG_STRING, etURLDECODE, 0, 0, spech_urldecode },
{'U'/*85*/, 0, 0, 0, 0, 0, 0 },
{'V'/*86*/, 0, 0, 0, 0, 0, 0 },
{'W'/*87*/, 0, 0, 0, 0, 0, 0 },
{'X'/*88*/, 16, 0, etRADIX,      0,  4, 0 },
{'Y'/*89*/, 0, 0, 0, 0, 0, 0 },
{'Z'/*90*/, 0, 0, 0, 0, 0, 0 },
{'['/*91*/, 0, 0, 0, 0, 0, 0 },
{'\\'/*92*/, 0, 0, 0, 0, 0, 0 },
{']'/*93*/, 0, 0, 0, 0, 0, 0 },
{'^'/*94*/, 0, 0, 0, 0, 0, 0 },
{'_'/*95*/, 0, 0, 0, 0, 0, 0 },
{'`'/*96*/, 0, 0, 0, 0, 0, 0 },
{'a'/*97*/, 0, 0, 0, 0, 0, 0 },
{'b'/*98*/, 0, 2, etBLOB, 0, 0, 0 },
{'c'/*99*/, 0, 0, etCHARX,      0,  0, 0 },
{'d'/*100*/, 10, FLAG_SIGNED, etRADIX,      0,  0, 0 },
{'e'/*101*/, 0, FLAG_SIGNED, etEXP,        30, 0, 0 },
{'f'/*102*/, 0, FLAG_SIGNED, etFLOAT,      0,  0, 0},
{'g'/*103*/, 0, FLAG_SIGNED, etGENERIC,    30, 0, 0 },
{'h'/*104*/, 0, FLAG_STRING, etHTML, 0, 0, spech_string_to_html },
{'i'/*105*/, 10, FLAG_SIGNED, etRADIX,      0,  0, 0},
{'j'/*106*/, 0, 0, etJSONSTR, 0, 0, 0 },
{'k'/*107*/, 0, 0, 0, 0, 0, 0 },
{'l'/*108*/, 0, 0, 0, 0, 0, 0 },
{'m'/*109*/, 0, 0, 0, 0, 0, 0 },
{'n'/*110*/, 0, 0, 0, 0, 0, 0 },
{'o'/*111*/, 8, 0, etRADIX,      0,  2, 0 },
{'p'/*112*/, 16, 0, etPOINTER, 0, 1, 0 },
{'q'/*113*/, 0, FLAG_STRING, etSQLESCAPE,  0, 0, 0 },
{'r'/*114*/, 10, (FLAG_EXTENDED|FLAG_SIGNED), etORDINAL,    0,  0, 0},
{'s'/*115*/, 0, FLAG_STRING, etSTRING,     0,  0, 0 },
{'t'/*116*/,  0, FLAG_STRING, etURLENCODE, 0, 0, spech_urlencode },
{'u'/*117*/, 10, 0, etRADIX,      0,  0, 0 },
{'v'/*118*/, 0, 0, 0, 0, 0, 0 },
#if 1
{'w'/*119*/, 0, 0, 0, 0, 0, 0 },
#else
/* This role is filled by %!Q. %w is not currently used/documented. */
{'w'/*119*/, 0, FLAG_STRING, etSQLESCAPE3, 0, 0, 0 },
#endif
{'x'/*120*/, 16, 0, etRADIX,      16, 1, 0  },
{'y'/*121*/, 0, 0, 0, 0, 0, 0 },
{'z'/*122*/, 0, FLAG_STRING, etDYNSTRING,  0,  0, 0 },
{'{'/*123*/, 0, 0, 0, 0, 0, 0 },
{'|'/*124*/, 0, 0, 0, 0, 0, 0 },
{'}'/*125*/, 0, 0, 0, 0, 0, 0 },
{'~'/*126*/, 0, 0, 0, 0, 0, 0 }
};
#define etNINFO  (sizeof(fmtinfo)/sizeof(fmtinfo[0]))

/*
  "*val" is a double such that 0.1 <= *val < 10.0
  Return the ascii code for the leading digit of *val, then
  multiply "*val" by 10.0 to renormalize.

  Example:
  input:     *val = 3.14159
  output:    *val = 1.4159    function return = '3'

  The counter *cnt is incremented each time.  After counter exceeds
  16 (the number of significant digits in a 64-bit float) '0' is
  always returned.
*/
static int et_getdigit(LONGDOUBLE_TYPE *val, int *cnt){
  int digit;
  LONGDOUBLE_TYPE d;
  if( (*cnt)++ >= 16 ) return '0';
  digit = (int)*val;
  d = digit;
  digit += '0';
  *val = (*val - d)*10.0;
  return digit;
}

/*
  On machines with a small(?) stack size, you can redefine the
  FSLPRINTF_BUF_SIZE to be less than 350.  But beware - for smaller
  values some %f conversions may go into an infinite loop.
*/
#ifndef FSLPRINTF_BUF_SIZE
#  define FSLPRINTF_BUF_SIZE 350  /* Size of the output buffer for numeric conversions */
#endif

#if defined(FSL_INT_T_PFMT)
/* int64_t is already defined. */
#else
#  if ! defined(__STDC__) && !defined(__TINYC__)
#    ifdef FSLPRINTF_INT64_TYPE
typedef FSLPRINTF_INT64_TYPE int64_t;
typedef unsigned FSLPRINTF_INT64_TYPE uint64_t;
#    elif defined(_MSC_VER) || defined(__BORLANDC__)
typedef __int64 int64_t;
typedef unsigned __int64 uint64_t;
#    else
typedef long long int int64_t;
typedef unsigned long long int uint64_t;
#    endif
#  endif
#endif /* Set up of int64 type */

static int spech_string_to_html( fsl_output_f pf, void * pfArg,
                                 unsigned int pfLen, void * varg ){
  char const * ch = (char const *) varg;
  unsigned int i;
  int rc = 0;
  if( ! ch ) return 0;
  rc = 0;
  for( i = 0; 0==rc && (i<pfLen) && *ch; ++ch, ++i )
  {
    switch( *ch )
    {
      case '<': rc = pf( pfArg, "&lt;", 4 );
        break;
      case '&': rc = pf( pfArg, "&amp;", 5 );
        break;
      default:
        rc = pf( pfArg, ch, 1 );
        break;
    };
  }
  return rc;
}

static int httpurl_needs_escape( int c ){
  /*
    Definition of "safe" and "unsafe" chars
    was taken from:

    https://www.codeguru.com/cpp/cpp/cpp_mfc/article.php/c4029/
  */
  return ( (c >= 32 && c <=47)
           || ( c>=58 && c<=64)
           || ( c>=91 && c<=96)
           || ( c>=123 && c<=126)
           || ( c<32 || c>=127)
           );
}

/**
   The handler for the etURLENCODE specifier.

   It expects varg to be a string value, which it will preceed to
   encode using an URL encoding algothrim (certain characters are
   converted to %XX, where XX is their hex value) and passes the
   encoded string to pf(). It returns the total length of the output
   string.
*/
static int spech_urlencode( fsl_output_f pf, void * pfArg,
                            unsigned int pfLen fsl__unused, void * varg ){
  char const * str = (char const *) varg;
  int rc = 0;
  char ch = 0;
  char const * hex = "0123456789ABCDEF";
#define xbufsz 10
  char xbuf[xbufsz];
  int slen = 0;
  (void)pfLen;
  if( ! str ) return 0;
  memset( xbuf, 0, xbufsz );
  ch = *str;
#define xbufsz 10
  slen = 0;
  for( ; 0==rc && ch; ch = *(++str) ){
    if( ! httpurl_needs_escape( ch ) ){
      rc = pf( pfArg, str, 1 );
      continue;
    }else{
      xbuf[0] = '%';
      xbuf[1] = hex[((ch>>4)&0xf)];
      xbuf[2] = hex[(ch&0xf)];
      xbuf[3] = 0;
      slen = 3;
      rc = pf( pfArg, xbuf, slen );
    }
  }
#undef xbufsz
  return rc;
}

/*
   hexchar_to_int():

   For 'a'-'f', 'A'-'F' and '0'-'9', returns the appropriate decimal
   number.  For any other character it returns -1.
*/
static int hexchar_to_int( int ch ){
  if( (ch>='0' && ch<='9') ) return ch-'0';
  else if( (ch>='a' && ch<='f') ) return ch-'a'+10;
  else if( (ch>='A' && ch<='F') ) return ch-'A'+10;
  else return -1;
}

/**
   The handler for the etURLDECODE specifier.

   It expects varg to be a ([const] char *), possibly encoded
   with URL encoding. It decodes the string using a URL decode
   algorithm and passes the decoded string to
   pf(). It returns the total length of the output string.
   If the input string contains malformed %XX codes then this
   function will return prematurely.
*/
static int spech_urldecode( fsl_output_f pf, void * pfArg,
                            unsigned int pfLen, void * varg ){
  char const * str = (char const *) varg;
  int rc = 0;
  char ch = 0;
  char ch2 = 0;
  char xbuf[4];
  int decoded;
  char const * end = str + pfLen;
  if( !str || !pfLen ) return 0;
  ch = *str;
  while(0==rc && ch && str<end){
    if( ch == '%' ){
      if(str+2>=end) goto outro/*invalid partial encoding - simply skip it*/;
      ch = *(++str);
      ch2 = *(++str);
      if( isxdigit((int)ch) &&
          isxdigit((int)ch2) )
      {
        decoded = (hexchar_to_int( ch ) * 16)
          + hexchar_to_int( ch2 );
        xbuf[0] = (char)decoded;
        xbuf[1] = 0;
        rc = pf( pfArg, xbuf, 1 );
        ch = *(++str);
        continue;
      }else{
        xbuf[0] = '%';
        xbuf[1] = ch;
        xbuf[2] = ch2;
        xbuf[3] = 0;
        rc = pf( pfArg, xbuf, 3 );
        ch = *(++str);
        continue;
      }
    }else if( ch == '+' ){
      xbuf[0] = ' ';
      xbuf[1] = 0;
      rc = pf( pfArg, xbuf, 1 );
      ch = *(++str);
      continue;
    }
    outro:
    xbuf[0] = ch;
    xbuf[1] = 0;
    rc = pf( pfArg, xbuf, 1 );
    ch = *(++str);
  }
  return rc;
}

/**
   Quotes the (char *) varg as an SQL string 'should' be quoted. The
   exact type of the conversion is specified by xtype, which must be
   one of etSQLESCAPE, etSQLESCAPE2, etSQLESCAPE3.

   Search this file for those constants to find the associated
   documentation.
*/
static int spech_sqlstring( int xtype, fsl_output_f pf,
                            void * pfArg, unsigned int pfLen,
                            void * varg ){
  enum { BufLen = 512 };
  char buf[BufLen];
  unsigned int i = 0, j = 0;
  int ch;
  char const q = xtype==etSQLESCAPE3 ?'"':'\''; /* Quote character */
  char const * escarg = (char const *) varg;
  bool const isnull = escarg==0;
  bool const needQuote =
    !isnull && (xtype==etSQLESCAPE2
                || xtype==etBLOBSQL
                || xtype==etSQLESCAPE3);
  if( isnull ){
    if(xtype==etSQLESCAPE2||xtype==etSQLESCAPE3){
      escarg = "NULL";
      pfLen = 4;
    }else{
      escarg = "(NULL)";;
      pfLen = 6;
    }
  }
  if( needQuote ) buf[j++] = q;
  for(i=0; (ch=escarg[i])!=0 && i<pfLen; ++i){
    buf[j++] = ch;
    if( ch==q ) buf[j++] = ch;
    if(j+2>=BufLen){
      int const rc = pf( pfArg, &buf[0], j );
      if(rc) return rc;
      j = 0;
    }
  }
  if( needQuote ) buf[j++] = q;
  buf[j] = 0;
  return j>0 ? pf( pfArg, &buf[0], j ) : 0;
}

/* TODO? Move these UTF8 bits into the public API? */
/*
** This lookup table is used to help decode the first byte of
** a multi-byte UTF8 character.
**
** Taken from sqlite3:
** https://www.sqlite.org/src/artifact?ln=48-61&name=810fbfebe12359f1
*/
static const unsigned char fsl_utfTrans1[] = {
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
  0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
  0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x00, 0x01, 0x02, 0x03, 0x00, 0x01, 0x00, 0x00
};
unsigned int fsl_utf8_read_char(
  const unsigned char *zIn,       /* First byte of UTF-8 character */
  const unsigned char *zTerm,     /* Pretend this byte is 0x00 */
  const unsigned char **pzNext    /* Write first byte past UTF-8 char here */
){
  /*
    Adapted from sqlite3:
    https://www.sqlite.org/src/artifact?ln=155-165&name=810fbfebe12359f1
  */
  unsigned c;
  if(zIn>=zTerm){
    *pzNext = zTerm;
    c = 0;
  }else{
    c = (unsigned int)*(zIn++);
    if( c>=0xc0 ){
      c = fsl_utfTrans1[c-0xc0];
      while( zIn!=zTerm && (*zIn & 0xc0)==0x80 )
        c = (c<<6) + (0x3f & *(zIn++));
      if( c<0x80
          || (c&0xFFFFF800)==0xD800
          || (c&0xFFFFFFFE)==0xFFFE ) c = 0xFFFD;
    }
    *pzNext = zIn;
  }
  return c;
}

static int fsl_utf8_char_to_cstr(unsigned int c, unsigned char *output, unsigned char length){
    /* Stolen from the internet, adapted from several variations which
      all _seem_ to have derived from librdf. */
    unsigned char size=0;

    /* check for illegal code positions:
     * U+D800 to U+DFFF (UTF-16 surrogates)
     * U+FFFE and U+FFFF
     */
    if((c > 0xD7FF && c < 0xE000)
       || c == 0xFFFE || c == 0xFFFF) return -1;

    /* Unicode 3.2 only defines U+0000 to U+10FFFF and UTF-8 encodings of it */
    if(c > 0x10ffff) return -1;

    if (c < 0x00000080) size = 1;
    else if (c < 0x00000800) size = 2;
    else if (c < 0x00010000) size = 3;
    else size = 4;
    if(!output) return (int)size;
    else if(size > length) return -1;
    else switch(size) {
      case 0:
          assert(!"can't happen anymore");
          output[0] = 0;
          return 0;
      case 4:
          output[3] = 0x80 | (c & 0x3F);
          c = c >> 6;
          c |= 0x10000;
          /* Fall through */
      case 3:
          output[2] = 0x80 | (c & 0x3F);
          c = c >> 6;
          c |= 0x800;
          /* Fall through */
      case 2:
          output[1] = 0x80 | (c & 0x3F);
          c = c >> 6;
          c |= 0xc0;
          /* Fall through */
      case 1:
        output[0] = (unsigned char)c;
          /* Fall through */
      default:
        return (int)size;
    }
}

struct SpechJson {
  char const * z;
  bool addQuotes;
  bool escapeSmallUtf8;
};

/**
   fsl_appendf_spec_handler for etJSONSTR. It assumes that varg is a
   SpechJson struct instance.
*/
static int spech_json( fsl_output_f pf, void * pfArg,
                       unsigned int pfLen, void * varg ){
  struct SpechJson const * state = (struct SpechJson *)varg;
  int pfRc = 0;
  const unsigned char *z = (const unsigned char *)state->z;
  const unsigned char *zEnd = z + pfLen;
  const unsigned char * zNext = 0;
  unsigned int c;
  unsigned char c1;

#define out(X,N) pfRc=pf(pfArg, (char const *)(X), N); \
  if(0!=pfRc) return pfRc
#define outc c1 = (unsigned char)c; out(&c1,1)
  if(!z){
    out("null",4);
    return pfRc;
  }
  if(state->addQuotes){
    out("\"", 1);
  }
  for( ; 0==pfRc && (z < zEnd)
         && (c=fsl_utf8_read_char(z, zEnd, &zNext));
       z = zNext ){
    if( c=='\\' || c=='"' ){
      out("\\", 1);
      outc;
    }else if( c<' ' ){
      out("\\",1);
      switch(c){
        case '\b': out("b",1); break;
        case '\f': out("f",1); break;
        case '\n': out("n",1); break;
        case '\t': out("t",1); break;
        case '\r': out("r",1); break;
        default:{
          unsigned char ubuf[5] = {'u',0,0,0,0};
          int i;
          for(i = 4; i>0; --i){
            ubuf[i] = "0123456789abcdef"[c&0xf];
            c >>= 4;
          }
          out(ubuf,5);
          break;
        }
      }
    }else if(c<128){
      outc;
    }/* At this point we know that c is part of a multi-byte
        character. We're assuming legal UTF8 input, which means
        emitting a surrogate pair if the value is > 0xffff. */
    else if(c<0xFFFF){
      unsigned char ubuf[6];
      if(state->escapeSmallUtf8){
        /* Output char in \u#### form. */
        fsl_snprintf((char *)ubuf, 6, "\\u%04x", c);
        out(ubuf, 6);
      }else{
        /* Output character literal. */
        int const n = fsl_utf8_char_to_cstr(c, ubuf, 4);
        if(n<0){
          out("?",1);
        }else{
          assert(n>0);
          out(ubuf, n);
        }
      }
    }else{
      /* Surrogate pair. */
      unsigned char ubuf[12];
      c -= 0x10000;
      fsl_snprintf((char *)ubuf, 12, "\\u%04x\\u%04x",
                   (0xd800 | (c>>10)),
                   (0xdc00 | (c & 0x3ff)));
      out(ubuf, 12);
    }
  }
  if(state->addQuotes){
    out("\"",1);
  }
  return pfRc;
#undef out
#undef outc
}

/*
   Find the length of a string as long as that length does not
   exceed N bytes.  If no zero terminator is seen in the first
   N bytes then return N.  If N is negative, then this routine
   is an alias for strlen().
*/
static int StrNLen32(const char *z, int N){
  int n = 0;
  while( (N-- != 0) && *(z++)!=0 ){ n++; }
  return n;
}

#if 0
/**
   Given the first byte of an assumed-to-be well-formed UTF8
   character, returns the length of that character. Returns 0 if the
   character appears to be an invalid UTF8 character, else returns its
   length, in bytes (1-4). Note that a NUL byte is a valid length-1
   character.
*/
static int utf8__char_length( unsigned char const * const c ){
  switch(0xF0 & *c) {
    case 0xF0: return (c[1]&0x80 && c[2]&0x80 && c[3]&0x80) ? 4 : 0;
    case 0xE0: return (c[1]&0x80 && c[2]&0x80) ? 3 : 0;
    case 0xC0: return (c[1]&0x80) ? 2 : 0;
    case 0x80: return 0;
    default: return 1;
      /* See also: https://stackoverflow.com/questions/4884656/utf-8-encoding-size */
  }
}
#endif

/**
   Internal helper for %#W.Ps format.
*/
static void appendf__utf8_altform(char const * z, int * pLength,
                                  int * pPrecision, int * pWidth){
  /* Treat %#W.Ps as a width/precision limit of W resp. P UTF8
     characters instead of bytes. */
  int pC = 0/*precision, chars*/, pB = 0/*precision, bytes*/,
    wC = 0/*width, chars*/, wB = 0/*width, bytes*/;
  char const * const zEnd = z + *pLength;
  int lc;
  while( z < zEnd ){
    switch(0xF0 & *z) {
      case 0xF0: lc = (z[1]&0x80 && z[2]&0x80 && z[3]&0x80) ? 4 : 0; break;
      case 0xE0: lc = (z[1]&0x80 && z[2]&0x80) ? 3 : 0; break;
      case 0xC0: lc = (z[1]&0x80) ? 2 : 0; break;
      case 0x80: lc = 0; break;
      default: lc = 1; break;
    }
    if(!lc) break;
    else if(wC<*pWidth && (*pPrecision<=0 || pC<*pPrecision)){ ++wC; wB+=lc;}
    if(pC<*pPrecision){ ++pC; pB+=lc;}
    z+=lc;
  }
  if(*pPrecision>0) *pLength = pB;
  if(*pWidth>0) *pWidth = *pWidth - wC + wB;
}

/*
  The root printf program.  All variations call this core.  It
  implements most of the common printf behaviours plus some extended
  ones.

  INPUTS:

  pfAppend : The is a fsl_output_f function which is responsible for
  accumulating the output. If pfAppend returns non-0 then processing
  stops immediately.

  pfAppendArg : is ignored by this function but passed as the first
  argument to pfAppend. pfAppend will presumably use it as a data
  store for accumulating its string.

  fmt : This is the format string, as in the usual printf().

  ap : This is a pointer to a list of arguments.  Same as in
  vprintf() and friends.

  OUTPUTS:

  The return value is 0 on success, non-0 on error. Historically it
  returned the total number bytes reported appended by pfAppend, but
  those semantics (A) are only very, very rarely useful and (B) they
  make sensibly reporting errors via the generic callback interface
  next to impossible. e.g. the callback may encounter I/O or allocation
  errors.

  Much of this code dates back to the early 1980's, supposedly.

  Known change history (most historic info has been lost):

  10 Feb 2008 by Stephan Beal: refactored to remove the 'useExtended'
  flag (which is now always on). Added the fsl_output_f typedef to
  make this function generic enough to drop into other source trees
  without much work.

  31 Oct 2008 by Stephan Beal: refactored the et_info lookup to be
  constant-time instead of linear.
*/
int fsl_appendfv(fsl_output_f pfAppend, /* Accumulate results here */
                 void * pfAppendArg,     /* Passed as first arg to pfAppend. */
                 const char *fmt,        /* Format string */
                 va_list ap              /* arguments */
                 ){
  /**
     HISTORIC NOTE (author and year unknown):

     Note that the order in which automatic variables are declared below
     seems to make a big difference in determining how fast this beast
     will run.
  */
  int pfrc = 0;              /* result from calling pfAppend */
  int c;                     /* Next character in the format string */
  char *bufpt = 0;           /* Pointer to the conversion buffer */
  int precision = 0;         /* Precision of the current field */
  int length;                /* Length of the field */
  int idx;                   /* A general purpose loop counter */
  int width;                 /* Width of the current field */
  etByte flag_leftjustify;   /* True if "-" flag is present */
  etByte flag_plussign;      /* True if "+" flag is present */
  etByte flag_blanksign;     /* True if " " flag is present */
  etByte flag_althash;       /* True if "#" flag is present */
  etByte flag_altbang;       /* True if "!" flag is present */
  etByte flag_zeropad;       /* True if field width constant starts with zero */
  etByte flag_long;          /* True if "l" flag is present */
  etByte flag_longlong;      /* True if the "ll" flag is present */
  etByte done;               /* Loop termination flag */
  etByte cThousand           /* Thousands separator for %d and %u */
    /* ported in from https://fossil-scm.org/home/info/2cdbdbb1c9b7ad2b */;
  uint64_t longvalue;        /* Value for integer types */
  LONGDOUBLE_TYPE realvalue; /* Value for real types */
  const et_info *infop = 0;      /* Pointer to the appropriate info structure */
  char buf[FSLPRINTF_BUF_SIZE];       /* Conversion buffer */
  char prefix;               /* Prefix character.  "+" or "-" or " " or '\0'. */
  etByte xtype = 0;          /* Conversion paradigm */
  char * zExtra = 0;         /* Extra memory used for conversions */
  int  exp, e2;              /* exponent of real numbers */
  double rounder;            /* Used for rounding floating point values */
  etByte flag_dp;            /* True if decimal point should be shown */
  etByte flag_rtz;           /* True if trailing zeros should be removed */
  etByte flag_exp;           /* True to force display of the exponent */
  int nsd;                   /* Number of significant digits returned */
  char bufSpaces[128] = {0}; /* Buffer for space-padded values */

/**
   FSLPRINTF_CHARARRAY is a helper to allocate variable-sized arrays.
   This exists mainly so this code can compile with the tcc compiler.
*/
#if FSLPRINTF_HAVE_VARARRAY
#  define FSLPRINTF_CHARARRAY(V,N) char V[N+1]; memset(V,0,N+1)
#  define FSLPRINTF_CHARARRAY_FREE(V)
#else
#  define FSLPRINTF_CHARARRAY(V,N)         \
  char * V;                                \
  if((int)(N)<((int)sizeof(bufSpaces))){   \
    V = &bufSpaces[0];                     \
  }else{                                   \
    V = (char *)fsl_malloc(N+1);           \
    if(!V) {FSLPRINTF_RETURN(FSL_RC_OOM);} \
  }
#  define FSLPRINTF_CHARARRAY_FREE(V) if(V!=&bufSpaces[0]) fsl_free(V)
#endif

  /* FSLPRINTF_RETURN, FSLPRINTF_CHECKERR, and FSLPRINTF_SPACES
     are internal helpers.
  */
#define FSLPRINTF_RETURN(RC) pfrc=RC; goto end
#define FSLPRINTF_CHECKERR if( 0!=pfrc ) goto end
#define FSLPRINTF_SPACES(N)                     \
  {                                             \
    FSLPRINTF_CHARARRAY(zSpaces,N);             \
    memset( zSpaces,' ',N);                     \
    pfrc = pfAppend(pfAppendArg, zSpaces, N);   \
    FSLPRINTF_CHARARRAY_FREE(zSpaces);          \
    FSLPRINTF_CHECKERR;                         \
  } (void)0

  length = 0;
  bufpt = 0;
  for(; (c=(*fmt))!=0; ++fmt){
    assert(0==pfrc);
    if( c!='%' ){
      int amt;
      bufpt = (char *)fmt;
      amt = 1;
      while( (c=(*++fmt))!='%' && c!=0 ) amt++;
      pfrc = pfAppend( pfAppendArg, bufpt, amt);
      FSLPRINTF_CHECKERR;
      if( c==0 ) break;
    }
    if( (c=(*++fmt))==0 ){
      pfrc = pfAppend( pfAppendArg, "%", 1);
      FSLPRINTF_CHECKERR;
      break;
    }
    /* Find out what flags are present */
    flag_leftjustify = flag_plussign = flag_blanksign = cThousand =
      flag_althash = flag_altbang = flag_zeropad = 0;
    done = 0;
    do{
      switch( c ){
        case '-':   flag_leftjustify = 1;  break;
        case '+':   flag_plussign = 1;     break;
        case ' ':   flag_blanksign = 1;    break;
        case '#':   flag_althash = 1;      break;
        case '!':   flag_altbang = 1;      break;
        case '0':   flag_zeropad = 1;      break;
        case ',':   cThousand = ',';       break;
        default:    done = 1;              break;
      }
    }while( !done && (c=(*++fmt))!=0 );
    /* Get the field width */
    width = 0;
    if( c=='*' ){
      width = va_arg(ap,int);
      if( width<0 ){
        flag_leftjustify = 1;
        width = width >= -2147483647 ? -width : 0;
      }
      c = *++fmt;
    }else{
      unsigned wx = 0;
      while( c>='0' && c<='9' ){
        wx = wx * 10 + c - '0';
        width = width*10 + c - '0';
        c = *++fmt;
      }
      width = wx & 0x7fffffff;
    }
    if( width > FSLPRINTF_BUF_SIZE-10 ){
      width = FSLPRINTF_BUF_SIZE-10;
    }
    /* Get the precision */
    if( c=='.' ){
      precision = 0;
      c = *++fmt;
      if( c=='*' ){
        precision = va_arg(ap,int);
        c = *++fmt;
        if( precision<0 ){
          precision = precision >= -2147483647 ? -precision : -1;
        }
      }else{
        unsigned px = 0;
        while( c>='0' && c<='9' ){
          px = px*10 + c - '0';
          c = *++fmt;
        }
        precision = px & 0x7fffffff;
      }
    }else{
      precision = -1;
    }
    /* Get the conversion type modifier */
    if( c=='l' ){
      flag_long = 1;
      c = *++fmt;
      if( c=='l' ){
        flag_longlong = 1;
        c = *++fmt;
      }else{
        flag_longlong = 0;
      }
    }else{
      flag_long = flag_longlong = 0;
    }
    /* Fetch the info entry for the field */
    infop = 0;
#define FMTNDX(N) (N - fmtinfo[0].fmttype)
#define FMTINFO(N) (fmtinfo[ FMTNDX(N) ])
    infop = ((c>=(fmtinfo[0].fmttype)) && (c<fmtinfo[etNINFO-1].fmttype))
      ? &FMTINFO(c)
      : 0;
    /*fprintf(stderr,"char '%c'/%d @ %d,  type=%c/%d\n",c,c,FMTNDX(c),infop->fmttype,infop->type);*/
    if( infop ) xtype = infop->type;
#undef FMTINFO
#undef FMTNDX
    if( (!infop) || (!infop->type) ){
      FSLPRINTF_RETURN(FSL_RC_RANGE);
    }
    assert( !zExtra );

    /* Limit the precision to prevent overflowing buf[] during conversion */
    if( precision>FSLPRINTF_BUF_SIZE-40 && (infop->flags & FLAG_STRING)==0 ){
      precision = FSLPRINTF_BUF_SIZE-40;
    }

    /*
      At this point, variables are initialized as follows:

      flag_althash                TRUE if a '#' is present.
      flag_altbang                TRUE if a '!' is present.
      flag_plussign               TRUE if a '+' is present.
      flag_leftjustify            TRUE if a '-' is present or if the
                                  field width was negative.
      flag_zeropad                TRUE if the width began with 0.
      flag_long                   TRUE if the letter 'l' (ell) prefixed
                                  the conversion character.
      flag_longlong               TRUE if the letter 'll' (ell ell) prefixed
                                  the conversion character.
      flag_blanksign              TRUE if a ' ' is present.
      width                       The specified field width.  This is
                                  always non-negative.  Default is 0.
      precision                   The specified precision.  The default
                                  is -1.
      xtype                       The class of the conversion.
      infop                       Pointer to the appropriate info struct.
    */
    switch( xtype ){
      case etPOINTER:
        flag_longlong = sizeof(char*)==sizeof(int64_t);
        flag_long = sizeof(char*)==sizeof(long int);
        FSL_SWITCH_FALL_THROUGH;
      case etORDINAL:
      case etRADIX:
        if( infop->flags & FLAG_SIGNED ){
          int64_t v;
          if( flag_longlong )   v = va_arg(ap,int64_t);
          else if( flag_long )  v = va_arg(ap,long int);
          else                  v = va_arg(ap,int);
          if( v<0 ){
            longvalue = -v;
            prefix = '-';
          }else{
            longvalue = v;
            if( flag_plussign )        prefix = '+';
            else if( flag_blanksign )  prefix = ' ';
            else                       prefix = 0;
          }
        }else{
          if( flag_longlong )   longvalue = va_arg(ap,uint64_t);
          else if( flag_long )  longvalue = va_arg(ap,unsigned long int);
          else                  longvalue = va_arg(ap,unsigned int);
          prefix = 0;
        }
        if( longvalue==0 ) flag_althash = 0;
        if( flag_zeropad && precision<width-(prefix!=0) ){
          precision = width-(prefix!=0);
        }
        bufpt = &buf[FSLPRINTF_BUF_SIZE-1];
        if( xtype==etORDINAL ){
          static const char zOrd[] = "thstndrd";
          int x = longvalue % 10;
          if( x>=4 || (longvalue/10)%10==1 ){
            x = 0;
          }
          buf[FSLPRINTF_BUF_SIZE-3] = zOrd[x*2];
          buf[FSLPRINTF_BUF_SIZE-2] = zOrd[x*2+1];
          bufpt -= 2;
        }
        {
          int const base = infop->base;
          const char *cset = &aDigits[infop->charset];
          do{                                           /* Convert to ascii */
            *(--bufpt) = cset[longvalue%base];
            longvalue = longvalue/base;
          }while( longvalue>0 );
        }
        length = &buf[FSLPRINTF_BUF_SIZE-1]-bufpt;
        while( precision>length ){
          *(--bufpt) = '0';                             /* Zero pad */
          ++length;
        }
        if( cThousand ){
          int nn = (length - 1)/3;  /* Number of "," to insert */
          int ix = (length - 1)%3 + 1;
          bufpt -= nn;
          for(idx=0; nn>0; idx++){
            bufpt[idx] = bufpt[idx+nn];
            ix--;
            if( ix==0 ){
              bufpt[++idx] = cThousand;
              nn--;
              ix = 3;
            }
          }
        }
        if( prefix ) *(--bufpt) = prefix;         /* Add sign */
        if( flag_althash && infop->prefix ){      /* Add "0" or "0x" */
          const char *pre;
          char x;
          pre = &aPrefix[infop->prefix];
          if( *bufpt!=pre[0] ){
            for(; (x=(*pre))!=0; pre++) *(--bufpt) = x;
          }
        }
        length = &buf[FSLPRINTF_BUF_SIZE-1]-bufpt;
        break;
      case etFLOAT:
      case etEXP:
      case etGENERIC:
        realvalue = va_arg(ap,double);
        if( precision<0 ) precision = 6;         /* Set default precision */
        if( precision>FSLPRINTF_BUF_SIZE/2-10 ) precision = FSLPRINTF_BUF_SIZE/2-10;
        if( realvalue<0.0 ){
          realvalue = -realvalue;
          prefix = '-';
        }else{
          if( flag_plussign )          prefix = '+';
          else if( flag_blanksign )    prefix = ' ';
          else                         prefix = 0;
        }
        if( xtype==etGENERIC && precision>0 ) precision--;
#if 0
        /* Rounding works like BSD when the constant 0.4999 is used.  Wierd! */
        for(idx=precision & 0xfff, rounder=0.4999; idx>0; idx--, rounder*=0.1);
#else
        /* It makes more sense to use 0.5 */
        for(idx=precision & 0xfff, rounder=0.5; idx>0; idx--, rounder*=0.1){}
#endif
        if( xtype==etFLOAT ) realvalue += rounder;
        /* Normalize realvalue to within 10.0 > realvalue >= 1.0 */
        exp = 0;
#if 1
        if( (realvalue)!=(realvalue) ){
          /* from sqlite3: #define sqlite3_isnan(X)  ((X)!=(X)) */
          /* This weird array thing is to avoid constness violations
             when assinging, e.g. "NaN" to bufpt.
          */
          static char NaN[4] = {'N','a','N','\0'};
          bufpt = NaN;
          length = 3;
          break;
        }
#endif
        if( realvalue>0.0 ){
          while( realvalue>=1e32 && exp<=350 ){ realvalue *= 1e-32; exp+=32; }
          while( realvalue>=1e8 && exp<=350 ){ realvalue *= 1e-8; exp+=8; }
          while( realvalue>=10.0 && exp<=350 ){ realvalue *= 0.1; exp++; }
          while( realvalue<1e-8 && exp>=-350 ){ realvalue *= 1e8; exp-=8; }
          while( realvalue<1.0 && exp>=-350 ){ realvalue *= 10.0; exp--; }
          if( exp>350 || exp<-350 ){
            if( prefix=='-' ){
              static char Inf[5] = {'-','I','n','f','\0'};
              bufpt = Inf;
              length = 4;
            }else if( prefix=='+' ){
              static char Inf[5] = {'+','I','n','f','\0'};
              bufpt = Inf;
              length = 4;
            }else{
              static char Inf[4] = {'I','n','f','\0'};
              bufpt = Inf;
              length = 3;
            }
            break;
          }
        }

        /* right around here somewhere we could port in
           https://sqlite.org/src/info/09e1d7c7b4615262dd03, which
           removes '-' signs from results containing only '0'
           digits. This impl has diverged far from that one since its
           original fork (in 2008-ish), so that change may not be
           straightforward here. */

        bufpt = buf;
        /*
          If the field type is etGENERIC, then convert to either etEXP
          or etFLOAT, as appropriate.
        */
        flag_exp = xtype==etEXP;
        if( xtype!=etFLOAT ){
          realvalue += rounder;
          if( realvalue>=10.0 ){ realvalue *= 0.1; exp++; }
        }
        if( xtype==etGENERIC ){
          flag_rtz = !flag_althash;
          if( exp<-4 || exp>precision ){
            xtype = etEXP;
          }else{
            precision = precision - exp;
            xtype = etFLOAT;
          }
        }else{
          flag_rtz = 0;
        }
        if( xtype==etEXP ){
          e2 = 0;
        }else{
          e2 = exp;
        }
        nsd = 0;
        flag_dp = (precision>0) | flag_althash | flag_altbang;
        /* The sign in front of the number */
        if( prefix ){
          *(bufpt++) = prefix;
        }
        /* Digits prior to the decimal point */
        if( e2<0 ){
          *(bufpt++) = '0';
        }else{
          for(; e2>=0; e2--){
            *(bufpt++) = et_getdigit(&realvalue,&nsd);
          }
        }
        /* The decimal point */
        if( flag_dp ){
          *(bufpt++) = '.';
        }
        /* "0" digits after the decimal point but before the first
           significant digit of the number */
        for(e2++; e2<0 && precision>0; precision--, e2++){
          *(bufpt++) = '0';
        }
        /* Significant digits after the decimal point */
        while( (precision--)>0 ){
          *(bufpt++) = et_getdigit(&realvalue,&nsd);
        }
        /* Remove trailing zeros and the "." if no digits follow the "." */
        if( flag_rtz && flag_dp ){
          while( bufpt[-1]=='0' ) *(--bufpt) = 0;
          /* assert( bufpt>buf ); */
          if( bufpt[-1]=='.' ){
            if( flag_altbang ){
              *(bufpt++) = '0';
            }else{
              *(--bufpt) = 0;
            }
          }
        }
        /* Add the "eNNN" suffix */
        if( flag_exp || (xtype==etEXP && exp) ){
          *(bufpt++) = aDigits[infop->charset];
          if( exp<0 ){
            *(bufpt++) = '-'; exp = -exp;
          }else{
            *(bufpt++) = '+';
          }
          if( exp>=100 ){
            *(bufpt++) = (exp/100)+'0';                /* 100's digit */
            exp %= 100;
          }
          *(bufpt++) = exp/10+'0';                     /* 10's digit */
          *(bufpt++) = exp%10+'0';                     /* 1's digit */
        }
        *bufpt = 0;

        /* The converted number is in buf[] and zero terminated. Output it.
           Note that the number is in the usual order, not reversed as with
           integer conversions. */
        length = bufpt-buf;
        bufpt = buf;

        /* Special case:  Add leading zeros if the flag_zeropad flag is
           set and we are not left justified */
        if( flag_zeropad && !flag_leftjustify && length < width){
          int i;
          int nPad = width - length;
          for(i=width; i>=nPad; i--){
            bufpt[i] = bufpt[i-nPad];
          }
          i = prefix!=0;
          while( nPad-- ) bufpt[i++] = '0';
          length = width;
        }
        break;
      case etPERCENT:
        buf[0] = '%';
        bufpt = buf;
        length = 1;
        break;
      case etCHARLIT:
      case etCHARX:
        c = buf[0] = (xtype==etCHARX ? va_arg(ap,int) : *++fmt);
        if( precision>=0 ){
          for(idx=1; idx<precision; idx++) buf[idx] = c;
          length = precision;
        }else{
          length =1;
        }
        bufpt = buf;
        break;
      case etPATH: {
        /* Sanitize path-like inputs, replacing \\ with /. */
        int i;
        int limit = flag_althash ? va_arg(ap,int) : -1;
        char const *e = va_arg(ap,char const*);
        if( e ){
          length = StrNLen32(e, limit);
          if( length >= FSLPRINTF_BUF_SIZE-1 ){
            zExtra = bufpt = fsl_malloc(length+1);
            if(!zExtra) return FSL_RC_OOM;
          }else{
            bufpt = buf;
          }
          for( i=0; i<length; i++ ){
            if( e[i]=='\\' ){
              bufpt[i]='/';
            }else{
              bufpt[i]=e[i];
            }
          }
          bufpt[length]='\0';
        }
        break;
      }
      case etSTRINGID: {
        precision = flag_altbang ? -1 : 16
          /* In fossil(1) this is configurable, but in this lib we
             don't have access to that state from here. Fossil also
             has the '!' flag_altbang, which indicates that it
             should be for a URL, and thus longer than the default.
             We are only roughly approximating that behaviour here. */;
        FSL_SWITCH_FALL_THROUGH;
      }
      case etDYNSTRING:
      case etSTRING: {
        bufpt = va_arg(ap,char*);
        length = bufpt
          ? StrNLen32(bufpt,
                      (precision>0 && flag_althash)
                      ? precision*4/*max bytes per char*/
                      : (precision>=0 ? precision : -1))
          : (int)0;
        if(flag_althash && length && (precision>0 || width>0)){
          appendf__utf8_altform(bufpt, &length, &precision, &width);
        }else if( length && precision>=0 && precision<length ){
          length = precision;
        }
        if( etDYNSTRING==xtype ){
          zExtra = bufpt /* will be freed below */;
        }
        break;
      }
      case etBLOB: {
        /* int const limit = flag_althash ? va_arg(ap, int) : -1; */
        fsl_buffer *pBlob = va_arg(ap, fsl_buffer*);
        bufpt = fsl_buffer_str(pBlob);
        length = (int)fsl_buffer_size(pBlob);
        if( precision>=0 && precision<length ) length = precision;
        /* if( limit>=0 && limit<length ) length = limit; */
        break;
      }
      case etFOSSILIZE:{
        int const limit = -1; /*flag_althash ? va_arg(ap,int) : -1;*/
        fsl_buffer fb = fsl_buffer_empty;
        int check;
        bufpt = va_arg(ap,char*);
        length = bufpt ? (int)fsl_strlen(bufpt) : 0;
        if((limit>=0) && (length>limit)) length = limit;
        check = fsl_bytes_fossilize((unsigned char const *)bufpt, length, &fb);
        if(check){
          fsl_buffer_reserve(&fb,0);
          FSLPRINTF_RETURN(check);
        }
        zExtra = bufpt = (char*)fb.mem /*transfer ownership*/;
        length = (int)fb.used;
        if( precision>=0 && precision<length ) length = precision;
        break;
      }
      case etJSONSTR: {
        struct SpechJson state;
        bufpt = va_arg(ap,char *);
        length = bufpt
          ? (precision>=0 ? precision : (int)fsl_strlen(bufpt))
          : 0;
        state.z = bufpt;
        state.addQuotes = flag_altbang ? true : false;
        state.escapeSmallUtf8 = flag_althash ? true : false;
        pfrc = spech_json( pfAppend, pfAppendArg, (unsigned)length, &state );
        FSLPRINTF_CHECKERR;
        bufpt = NULL;
        length = 0;
        break;
      }
      case etHTML:
      case etURLENCODE:
      case etURLDECODE:{
        bufpt = va_arg(ap,char*);
        length = bufpt ? (int)fsl_strlen(bufpt) : 0;
        pfrc = infop->callback( pfAppend, pfAppendArg,
                                (precision>=0 && precision<length) ? precision : length,
                                bufpt );
        FSLPRINTF_CHECKERR;
        bufpt = NULL;
        length = 0;
        break;
      }
      case etBLOBSQL:
      case etSQLESCAPE:
      case etSQLESCAPE2:
      case etSQLESCAPE3: {
        if(flag_altbang && etSQLESCAPE2==xtype){
          xtype = etSQLESCAPE3;
        }
        if(etBLOBSQL==xtype){
          fsl_buffer * const b = va_arg(ap,fsl_buffer*);
          bufpt = b ? fsl_buffer_str(b) : NULL;
          length = b ? (int)fsl_buffer_size(b) : 0;
          if(flag_altbang) xtype = etSQLESCAPE3;
        }else{
          /* TODO?: add flag_althash support, i.e. %#q. See
             appendf__utf8_altform()). */
          bufpt = va_arg(ap,char*);
          length = bufpt ? (int)strlen(bufpt) : 0;
        }
        pfrc = spech_sqlstring( xtype, pfAppend, pfAppendArg,
                                (precision>=0 && precision<length) ? precision : length,
                                bufpt );
        FSLPRINTF_CHECKERR;
        length = 0;
        break;
      }
      case etFSLRC:{
        int const i = va_arg(ap,int);
        bufpt = (char*)fsl_rc_cstr(i);
        if( bufpt ){
          length = (int)fsl_strlen(bufpt);
        }else{
          length = snprintf( buf, sizeof(buf), "#%d", i);
          bufpt = buf;
        }
        break;
      }
    }/* End switch over the format type */
    /*
      The text of the conversion is pointed to by "bufpt" and is
      "length" characters long.  The field width is "width".  Do
      the output.
    */
    if( !flag_leftjustify ){
      int nspace;
      nspace = width-length;
      if( nspace>0 ){
        FSLPRINTF_SPACES(nspace);
      }
    }
    if( length>0 ){
      pfrc = pfAppend( pfAppendArg, bufpt, length);
      FSLPRINTF_CHECKERR;
    }
    if( flag_leftjustify ){
      int nspace;
      nspace = width-length;
      if( nspace>0 ){
        FSLPRINTF_SPACES(nspace);
      }
    }
    if( zExtra ) fsl_free(zExtra);
    zExtra = 0;
  }/* End for loop over the format string */
end:
  if( zExtra ) fsl_free(zExtra);
  return pfrc;
} /* End of function */


#undef FSLPRINTF_CHARARRAY
#undef FSLPRINTF_CHARARRAY_FREE
#undef FSLPRINTF_SPACES
#undef FSLPRINTF_CHECKERR
#undef FSLPRINTF_RETURN
#undef FSLPRINTF_BUF_SIZE

int fsl_appendf(fsl_output_f pfAppend, void * pfAppendArg,
                const char *fmt, ... ){
  int ret;
  va_list vargs;
  va_start( vargs, fmt );
  ret = fsl_appendfv( pfAppend, pfAppendArg, fmt, vargs );
  va_end(vargs);
  return ret;
}

int fsl_appendn(fsl_output_f pfAppend, void * pfAppendArg,
                const char *str, fsl_int_t n){
  return pfAppend(pfAppendArg, (void const *)str,
                  n<0 ? fsl_strlen(str) : (fsl_size_t)n);
}

int fsl_fprintfv( FILE * fp, char const * fmt, va_list args ){
  return (fp && fmt)
    ? fsl_appendfv( fsl_output_f_FILE, fp, fmt, args )
    :  FSL_RC_MISUSE;
}

int fsl_fprintf( FILE * fp, char const * fmt, ... ){
  int ret;
  va_list vargs;
  va_start( vargs, fmt );
  ret = fsl_appendfv( fsl_output_f_FILE, fp, fmt, vargs );
  va_end(vargs);
  return ret;
}

char * fsl_mprintfv( char const * fmt, va_list vargs ){
  if( !fmt ) return 0;
  else if(!*fmt) return fsl_strndup("",0);
  else{
    fsl_buffer buf = fsl_buffer_empty;
    int const rc = fsl_buffer_appendfv( &buf, fmt, vargs );
    if(rc){
      fsl_buffer_reserve(&buf, 0);
      assert(0==buf.mem);
    }
    return (char*)buf.mem /*transfer ownership*/;
  }
}

char * fsl_mprintf( char const * fmt, ... ){
  char * ret;
  va_list vargs;
  va_start( vargs, fmt );
  ret = fsl_mprintfv( fmt, vargs );
  va_end( vargs );
  return ret;
}


/**
    Internal state for fsl_snprintfv().
 */
struct fsl_snp_state {
  /** Destination memory */
  char * dest;
  /** Current output position in this->dest. */
  fsl_size_t pos;
  /** Length of this->dest. */
  fsl_size_t len;
};
typedef struct fsl_snp_state fsl_snp_state;

static int fsl_output_f_snprintf( void * arg,
                                  void const * data_,
                                  fsl_size_t n ){
  char const * data = (char const *)data_;
  fsl_snp_state * st = (fsl_snp_state*) arg;
  if(n==0 || (st->pos >= st->len)) return 0;
  else if((n + st->pos) > st->len){
    n = st->len - st->pos;
  }
  memcpy(st->dest + st->pos, data, n);
  st->pos += n;
  assert(st->pos <= st->len);
  return 0;
}

int fsl_snprintfv( char * dest, fsl_size_t n,
                   char const * fmt, va_list args){
  fsl_snp_state st = {NULL,0,0};
  int rc = 0;
  if(!dest || !fmt) return FSL_RC_MISUSE;
  else if(!n || !*fmt){
    if(dest) *dest = 0;
    return 0;
  }
  st.len = n;
  st.dest = dest;
  rc = fsl_appendfv( fsl_output_f_snprintf, &st, fmt, args );
  if(st.pos < st.len){
    dest[st.pos] = 0;
  }
  return rc;
}

int fsl_snprintf( char * dest, fsl_size_t n, char const * fmt, ... ){
  int rc = 0;
  va_list vargs;
  va_start( vargs, fmt );
  rc = fsl_snprintfv( dest, n, fmt, vargs );
  va_end( vargs );
  return rc;
}

int fsl_cx_formatv(fsl_cx * f, fsl_output_f out, void * outState,
                   char const * const zFmt, va_list args ){
  int rc = 0;
  fsl_buffer bufLocal;
  fsl_buffer * const b = f->scratchpads.format.used
    ? &bufLocal
    : &f->scratchpads.format
    /* This bit protects us against the eventuality of recursion via
       out() without requiring a call-local allocation for
       non-recursive calls. */;
  char const * z = zFmt;
  char const * zLeft = z;
  char const * const zEnd =
    z ? (z + fsl_strlen(zFmt)) : NULL /* NULL+0 is UB */;

  assert( zFmt );
  //MARKER(("input=%.*s\n", (int)(zEnd - z), z));
  if( b==&f->scratchpads.format && !b->capacity ){
    fsl_buffer_reserve(b, 128);
  }
  while( z<zEnd && !b->errCode ){
    if( z<zEnd-5 /* 5 == length of {{x}} */
        && '{'==z[0] && '{'==z[1] ){
      /* Look for {{...}} */
      zLeft = z;
      z += 2;
      while( z<zEnd-1 && !('}'==z[0] && '}'==z[1]) ){
        ++z;
      }
      assert( z<=zEnd-1 );
      if( z==zEnd-1 ){
        /* Malformed. Pass it all through. */
        z = zEnd;
      passthru:
        fsl_buffer_append(b, zLeft, (z - zLeft));
        continue;
      passthru2:
        zLeft -= 2;
        goto passthru;
      }
      assert( z[0]=='}' && z[1]=='}' );
      zLeft += 2;
      fsl_int_t const n = (fsl_int_t)(z - zLeft);
      assert( n>1 );
      z += 2;
      //MARKER(("n=%d token=%.*s\n", (int)n, (int)n, zLeft));
      switch( n ){
        case 7:
          if( 0==memcmp(zLeft, "repo.db", n) ){
            fsl_buffer_append(b, f->db.repo.db.filename, -1);
            break;
          }
          goto passthru2;
        case 9:
          if( 0==memcmp(zLeft, "user.name", n) ){
            fsl_buffer_append(b, fsl_cx_user_guess(f), -1);
            break;
          }
          FSL_SWITCH_FALL_THROUGH;
        case 10:
          if( 0==memcmp(zLeft, "ckout.dir", 9) ){
            fsl_size_t nd = fsl_strlen(f->db.ckout.dir);
            if( nd && 0!=memcmp(zLeft, "ckout.dir/", 10) ){
              /* Remove trailing "/" */
              --nd;
            }
            fsl_buffer_append(b, f->db.ckout.dir, (fsl_int_t)nd);
            break;
          }
          goto passthru2;
        default:
          goto passthru2;
      }
    }else{
      fsl_buffer_appendch(b, *z);
      ++z;
    }
  }
  if( 0==(rc = b->errCode) ){
    rc = 0==strchr(zFmt,'%')
      ? out(outState, b->mem, b->used)
      : fsl_appendfv(out, outState, zFmt, args);
  }
  if( b==&f->scratchpads.format ) fsl_buffer_reuse(b);
  else fsl_buffer_clear(b);
  return rc;
}

int fsl_cx_format(fsl_cx * f, fsl_output_f out, void * outState,
                  char const *zFmt, ... ){
  int rc;
  va_list args;
  va_start(args,zFmt);
  rc = fsl_cx_formatv(f, out, outState, zFmt, args);
  va_end(args);
  return rc;
}

int fsl_cx_format_buffer(fsl_cx * f, fsl_buffer * tgt,
                         char const *zFmt, ... ){
  int rc;
  va_list args;
  va_start(args,zFmt);
  rc = fsl_cx_formatv(f, fsl_output_f_buffer, tgt, zFmt, args);
  va_end(args);
  return rc;
}

int fsl_cx_format_FILE(fsl_cx * f, FILE * tgt,
                       char const *zFmt, ... ){
  int rc;
  va_list args;
  va_start(args,zFmt);
  rc = fsl_cx_formatv(f, fsl_output_f_FILE, tgt, zFmt, args);
  va_end(args);
  return rc;
}


#undef MARKER
