/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */ 
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2022 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2013-2022 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/

/************************************************************************
  This file houses the "dibu" (Diff Builder) routines.
*/
#include "libfossil.h"
#include <assert.h>
#include <memory.h>
#include <stdlib.h>
#include <string.h> /* for memmove()/strlen() */


#include <stdio.h>
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)


const fsl_dibu_opt fsl_dibu_opt_empty = fsl_dibu_opt_empty_m;
const fsl_dibu fsl_dibu_empty = fsl_dibu_empty_m;


fsl_dibu * fsl_dibu_alloc(fsl_size_t extra){
  fsl_dibu * rc =
    (fsl_dibu*)fsl_malloc(sizeof(fsl_dibu) + extra);
  if(rc){
    *rc = fsl_dibu_empty;
    if(extra){
      rc->pimpl = ((unsigned char *)rc)+sizeof(fsl_dibu);
    }
  }
  return rc;
}

static int fdb__out(fsl_dibu *const b,
                    char const *z, fsl_int_t n){
  if(n<0) n = (fsl_int_t)fsl_strlen(z);
  return b->opt->out(b->opt->outState, z, (fsl_size_t)n);
}
static int fdb__outf(fsl_dibu * const b,
                     char const *fmt, ...){
  int rc = 0;
  va_list va;
  assert(b->opt->out);
  va_start(va,fmt);
  rc = fsl_appendfv(b->opt->out, b->opt->outState, fmt, va);
  va_end(va);
  return rc;
}


/**
   Column indexes for DiffCounter::cols.
*/
enum DiffCounterCols {
DICO_NUM1 = 0, DICO_TEXT1,
DICO_MOD,
DICO_NUM2, DICO_TEXT2,
DICO_count
};
/**
   Internal state for the text-mode split diff builder. Used for
   calculating column widths in the builder's first pass so that
   the second pass can output everything in a uniform width.
*/
struct DiffCounter {
  /**
     Largest column width we've yet seen. These are only updated for
     DICO_TEXT1 and DICO_TEXT2. The others currently have fixed widths.

     FIXME: these are in bytes, not text columns. The current code may
     truncate multibyte characters.
  */
  uint32_t maxWidths[DICO_count];
  /**
     Max line numbers seen for the LHS/RHS input files. This is likely
     much higher than the number of diff lines.

     This can be used, e.g., to size and allocate a curses PAD in the
     second pass of the start() method.
  */
  uint32_t lineCount[2];
  /**
     The actual number of lines needed for rendering the file.
  */
  uint32_t displayLines;
};
typedef struct DiffCounter DiffCounter;
static const DiffCounter DiffCounter_empty = {{1,25,3,1,25},{0,0},0};
#define DICOSTATE(VNAME) DiffCounter * const VNAME = (DiffCounter *)b->pimpl

static int maxColWidth(fsl_dibu const * const b,
                       DiffCounter const * const sst,
                       int mwIndex){
  static const short minColWidth =
    10/*b->opt.columnWidth values smaller than this are treated as
        this value*/;
  switch(mwIndex){
    case DICO_NUM1:
    case DICO_NUM2:
    case DICO_MOD: return sst->maxWidths[mwIndex];
    case DICO_TEXT1: case DICO_TEXT2: break;
    default:
      assert(!"internal API misuse: invalid column index.");
      break;
  }
  int const y =
    (b->opt->columnWidth>0
     && b->opt->columnWidth<=sst->maxWidths[mwIndex])
    ? (int)b->opt->columnWidth
    : (int)sst->maxWidths[mwIndex];
  return minColWidth > y ? minColWidth : y;
}

static void fdb__dico_update_maxlen(DiffCounter * const sst,
                                    int col,
                                    char const * const z,
                                    uint32_t n){
  if(sst->maxWidths[col]<n){
#if 0
    sst->maxWidths[col] = n;
#else
    n = (uint32_t)fsl_strlen_utf8(z, (fsl_int_t)n);
    if(sst->maxWidths[col]<n) sst->maxWidths[col] = n;
#endif
  }
}

static int fdb__debug_start(fsl_dibu * const b){
  int rc = fdb__outf(b, "DEBUG builder starting pass #%d.\n",
                     b->passNumber);
  if(1==b->passNumber){
    DICOSTATE(sst);
    *sst = DiffCounter_empty;
    if(b->opt->nameLHS) ++sst->displayLines;
    if(b->opt->nameRHS) ++sst->displayLines;
    if(b->opt->hashLHS) ++sst->displayLines;
    if(b->opt->hashRHS) ++sst->displayLines;
    ++b->fileCount;
    return rc;
  }
  if(0==rc && b->opt->nameLHS){
    rc = fdb__outf(b,"LHS: %s\n", b->opt->nameLHS);
  }
  if(0==rc && b->opt->nameRHS){
    rc = fdb__outf(b,"RHS: %s\n", b->opt->nameRHS);
  }
  if(0==rc && b->opt->hashLHS){
    rc = fdb__outf(b,"LHS hash: %s\n", b->opt->hashLHS);
  }
  if(0==rc && b->opt->hashRHS){
    rc = fdb__outf(b,"RHS hash: %s\n", b->opt->hashRHS);
  }
  return rc;
}


static int fdb__debug_chunkHeader(fsl_dibu* const b,
                                  uint32_t lnnoLHS, uint32_t linesLHS,
                                  uint32_t lnnoRHS, uint32_t linesRHS ){
#if 1
  if(1==b->passNumber){
    DICOSTATE(sst);
    ++sst->displayLines;
    return 0;
  }
  if(b->lnLHS+1==lnnoLHS && b->lnRHS+1==lnnoRHS){
    fdb__outf(b, "<<<Unfortunate chunk separator."
              "Ticket 746ebbe86c20b5c0f96cdadd19abd8284770de16.>>>\n");
  }
  //fdb__outf(b, "lnLHS=%d, lnRHS=%d\n", (int)b->lnLHS, (int)b->lnRHS);
  return fdb__outf(b, "@@ -%" PRIu32 ",%" PRIu32
                " +%" PRIu32 ",%" PRIu32 " @@\n",
                lnnoLHS, linesLHS, lnnoRHS, linesRHS);
#else
  return 0;
#endif
}

static int fdb__debug_skip(fsl_dibu * const b, uint32_t n){
  if(1==b->passNumber){
    DICOSTATE(sst);
    b->lnLHS += n;
    b->lnRHS += n;
    ++sst->displayLines;
    return 0;
  }
  const int rc = fdb__outf(b, "SKIP %u (%u..%u left and %u..%u right)\n",
                           n, b->lnLHS+1, b->lnLHS+n, b->lnRHS+1, b->lnRHS+n);
  b->lnLHS += n;
  b->lnRHS += n;
  return rc;
}
static int fdb__debug_common(fsl_dibu * const b, fsl_dline const * pLine){
  DICOSTATE(sst);
  ++b->lnLHS;
  ++b->lnRHS;
  if(1==b->passNumber){
    ++sst->displayLines;
    fdb__dico_update_maxlen(sst, DICO_TEXT1, pLine->z, pLine->n);
    fdb__dico_update_maxlen(sst, DICO_TEXT2, pLine->z, pLine->n);
    return 0;
  }
  return fdb__outf(b, "COMMON  %8u %8u %.*s\n",
                   b->lnLHS, b->lnRHS, (int)pLine->n, pLine->z);
}
static int fdb__debug_insertion(fsl_dibu * const b, fsl_dline const * pLine){
  DICOSTATE(sst);
  ++b->lnRHS;
  if(1==b->passNumber){
    ++sst->displayLines;
    fdb__dico_update_maxlen(sst, DICO_TEXT1, pLine->z, pLine->n);
    return 0;
  }
  return fdb__outf(b, "INSERT           %8u %.*s\n",
                   b->lnRHS, (int)pLine->n, pLine->z);
}
static int fdb__debug_deletion(fsl_dibu * const b, fsl_dline const * pLine){
  DICOSTATE(sst);
  ++b->lnLHS;
  if(1==b->passNumber){
    ++sst->displayLines;
    fdb__dico_update_maxlen(sst, DICO_TEXT2, pLine->z, pLine->n);
    return 0;
  }
  return fdb__outf(b, "DELETE  %8u          %.*s\n",
                   b->lnLHS, (int)pLine->n, pLine->z);
}
static int fdb__debug_replacement(fsl_dibu * const b,
                                  fsl_dline const * lineLhs,
                                  fsl_dline const * lineRhs) {
#if 0
  int rc = b->deletion(b, lineLhs);
  if(0==rc) rc = b->insertion(b, lineRhs);
  return rc;
#else    
  DICOSTATE(sst);
  ++b->lnLHS;
  ++b->lnRHS;
  if(1==b->passNumber){
    ++sst->displayLines;
    fdb__dico_update_maxlen(sst, DICO_TEXT1, lineLhs->z, lineLhs->n);
    fdb__dico_update_maxlen(sst, DICO_TEXT2, lineRhs->z, lineRhs->n);
    return 0;
  }
  int rc = fdb__outf(b, "REPLACE %8u          %.*s\n",
                     b->lnLHS, (int)lineLhs->n, lineLhs->z);
  if(!rc){
    rc = fdb__outf(b, "            %8u %.*s\n",
                   b->lnRHS, (int)lineRhs->n, lineRhs->z);
  }
  return rc;
#endif
}
                 
static int fdb__debug_edit(fsl_dibu * const b,
                           fsl_dline const * lineLHS,
                           fsl_dline const * lineRHS){
#if 0
  int rc = b->deletion(b, lineLHS);
  if(0==rc) rc = b->insertion(b, lineRHS);
  return rc;
#else    
  int rc = 0;
  DICOSTATE(sst);
  ++b->lnLHS;
  ++b->lnRHS;
  if(1==b->passNumber){
    sst->displayLines += 4
      /* this is actually 3 or 4, but we don't know that from here */;
    fdb__dico_update_maxlen(sst, DICO_TEXT1, lineLHS->z, lineLHS->n);
    fdb__dico_update_maxlen(sst, DICO_TEXT2, lineRHS->z, lineRHS->n);
    return 0;
  }
  int i, j;
  int x;
  fsl_dline_change chng = fsl_dline_change_empty;
#define RC if(rc) goto end
  rc = fdb__outf(b, "EDIT    %8u          %.*s\n",
                 b->lnLHS, (int)lineLHS->n, lineLHS->z);
  RC;
  fsl_dline_change_spans(lineLHS, lineRHS, &chng);
  for(i=x=0; i<chng.n; i++){
    int ofst = chng.a[i].iStart1;
    int len = chng.a[i].iLen1;
    if( len ){
      char c = '0' + i;
      if( x==0 ){
        rc = fdb__outf(b, "%*s", 26, "");
        RC;
      }
      while( ofst > x ){
        if( (lineLHS->z[x]&0xc0)!=0x80 ){
          rc = fdb__out(b, " ", 1);
          RC;
        }
        x++;
      }
      for(j=0; j<len; j++, x++){
        if( (lineLHS->z[x]&0xc0)!=0x80 ){
          rc = fdb__out(b, &c, 1);
          RC;
        }
      }
    }
  }
  if( x ){
    rc = fdb__out(b, "\n", 1);
    RC;
  }
  rc = fdb__outf(b, "                 %8u %.*s\n",
                 b->lnRHS, (int)lineRHS->n, lineRHS->z);
  RC;
  for(i=x=0; i<chng.n; i++){
    int ofst = chng.a[i].iStart2;
    int len = chng.a[i].iLen2;
    if( len ){
      char c = '0' + i;
      if( x==0 ){
        rc = fdb__outf(b, "%*s", 26, "");
        RC;
      }
      while( ofst > x ){
        if( (lineRHS->z[x]&0xc0)!=0x80 ){
          rc = fdb__out(b, " ", 1);
          RC;
        }
        x++;
      }
      for(j=0; j<len; j++, x++){
        if( (lineRHS->z[x]&0xc0)!=0x80 ){
          rc = fdb__out(b, &c, 1);
          RC;
        }
      }
    }
  }
  if( x ){
    rc = fdb__out(b, "\n", 1);
  }
  end:
#undef RC
  return rc;
#endif
}

static int fdb__debug_finish(fsl_dibu * const b){
  DICOSTATE(sst);
  if(1==b->passNumber){
    sst->lineCount[0] = b->lnLHS;
    sst->lineCount[1] = b->lnRHS;
    return 0;
  }
  int rc = fdb__outf(b, "END with %u LHS file lines "
                     "and %u RHS lines (max. %u display lines)\n",
                     b->lnLHS, b->lnRHS, sst->displayLines);
  if(0==rc){
    rc = fdb__outf(b,"Col widths: num left=%u, col left=%u, "
                   "modifier=%u, num right=%u, col right=%u\n",
                   sst->maxWidths[0], sst->maxWidths[1],
                   sst->maxWidths[2], sst->maxWidths[3],
                   sst->maxWidths[4]);
  }
  return rc;
}

void fsl_dibu_finalizer(fsl_dibu * const b){
  *b = fsl_dibu_empty;
  fsl_free(b);
}

static fsl_dibu * fsl__diff_builder_debug(void){
  fsl_dibu * rc =
    fsl_dibu_alloc((fsl_size_t)sizeof(DiffCounter));
  if(rc){
    rc->chunkHeader = fdb__debug_chunkHeader;
    rc->start = fdb__debug_start;
    rc->skip = fdb__debug_skip;
    rc->common = fdb__debug_common;
    rc->insertion = fdb__debug_insertion;
    rc->deletion = fdb__debug_deletion;
    rc->replacement = fdb__debug_replacement;
    rc->edit = fdb__debug_edit;
    rc->finish = fdb__debug_finish;
    rc->finalize = fsl_dibu_finalizer;
    rc->twoPass = true;
    assert(0!=rc->pimpl);
    DiffCounter * const sst = (DiffCounter*)rc->pimpl;
    *sst = DiffCounter_empty;
    assert(0==rc->implFlags);
    assert(0==rc->lnLHS);
    assert(0==rc->lnRHS);
    assert(NULL==rc->opt);
  }
  return rc;
}

/******************** json1 diff builder ********************/
/* Description taken verbatim from fossil(1): */
/*
** This formatter generates a JSON array that describes the difference.
**
** The Json array consists of integer opcodes with each opcode followed
** by zero or more arguments:
**
**   Syntax        Mnemonic    Description
**   -----------   --------    --------------------------
**   0             END         This is the end of the diff
**   1  INTEGER    SKIP        Skip N lines from both files
**   2  STRING     COMMON      The line show by STRING is in both files
**   3  STRING     INSERT      The line STRING is in only the right file
**   4  STRING     DELETE      The STRING line is in only the left file
**   5  SUBARRAY   EDIT        One line is different on left and right.
**
** The SUBARRAY is an array of 3*N+1 strings with N>=0.  The triples
** represent common-text, left-text, and right-text.  The last string
** in SUBARRAY is the common-suffix.  Any string can be empty if it does
** not apply.
*/

static int fdb__outj(fsl_dibu * const b,
                     char const *zJson, int n){
  return n<0
    ? fdb__outf(b, "%!j", zJson)
    : fdb__outf(b, "%!.*j", n, zJson);
}

static int fdb__json1_start(fsl_dibu * const b){
  int rc = fdb__outf(b, "{\"hashLHS\": %!j, \"hashRHS\": %!j, ",
                     b->opt->hashLHS, b->opt->hashRHS);
  if(0==rc && b->opt->nameLHS){
    rc = fdb__outf(b, "\"nameLHS\": %!j, ", b->opt->nameLHS);
  }
  if(0==rc && b->opt->nameRHS){
    rc = fdb__outf(b, "\"nameRHS\": %!j, ", b->opt->nameRHS);
  }
  if(0==rc){
    rc = fdb__out(b, "\"diff\":[", 8);
  }
  return rc;
}

static int fdb__json1_skip(fsl_dibu * const b, uint32_t n){
  return fdb__outf(b, "1,%" PRIu32 ",\n", n);
}
static int fdb__json1_common(fsl_dibu * const b, fsl_dline const * pLine){
  int rc = fdb__out(b, "2,",2);
  if(!rc) {
    rc = fdb__outj(b, pLine->z, (int)pLine->n);
    if(!rc) rc = fdb__out(b, ",\n",2);
  }
  return rc;
}
static int fdb__json1_insertion(fsl_dibu * const b, fsl_dline const * pLine){
  int rc = fdb__out(b, "3,",2);
  if(!rc){
    rc = fdb__outj(b, pLine->z, (int)pLine->n);
    if(!rc) rc = fdb__out(b, ",\n",2);
  }
  return rc;
}
static int fdb__json1_deletion(fsl_dibu * const b, fsl_dline const * pLine){
  int rc = fdb__out(b, "4,",2);
  if(!rc){
    rc = fdb__outj(b, pLine->z, (int)pLine->n);
    if(!rc) rc = fdb__out(b, ",\n",2);
  }
  return rc;
}
static int fdb__json1_replacement(fsl_dibu * const b,
                              fsl_dline const * lineLhs,
                              fsl_dline const * lineRhs) {
  int rc = fdb__out(b, "5,[\"\",",6);
  if(!rc) rc = fdb__outf(b,"%!.*j", (int)lineLhs->n, lineLhs->z);
  if(!rc) rc = fdb__out(b, ",", 1);
  if(!rc) rc = fdb__outf(b,"%!.*j", (int)lineRhs->n, lineRhs->z);
  if(!rc) rc = fdb__out(b, ",\"\"],\n",6);
  return rc;
}
                 
static int fdb__json1_edit(fsl_dibu * const b,
                           fsl_dline const * lineLHS,
                           fsl_dline const * lineRHS){
  int rc = 0;
  int i,x;
  fsl_dline_change chng = fsl_dline_change_empty;

#define RC if(rc) goto end
  rc = fdb__out(b, "5,[", 3); RC;
  fsl_dline_change_spans(lineLHS, lineRHS, &chng);
  for(i=x=0; i<(int)chng.n; i++){
    if(i>0){
      rc = fdb__out(b, ",", 1); RC;
    }
    rc = fdb__outj(b, lineLHS->z + x, (int)chng.a[i].iStart1 - x); RC;
    x = chng.a[i].iStart1;
    rc = fdb__out(b, ",", 1); RC;
    rc = fdb__outj(b, lineLHS->z + x, (int)chng.a[i].iLen1); RC;
    x += chng.a[i].iLen1;
    rc = fdb__out(b, ",", 1); RC;
    rc = fdb__outj(b, lineRHS->z + chng.a[i].iStart2,
                   (int)chng.a[i].iLen2); RC;
  }
  rc = fdb__out(b, ",", 1); RC;
  rc = fdb__outj(b, lineLHS->z + x, (int)(lineLHS->n - x)); RC;
  rc = fdb__out(b, "],\n",3); RC;
  end:
  return rc;
#undef RC
}

static int fdb__json1_finish(fsl_dibu * const b){
  return fdb__out(b, "0]}", 3);
}

static fsl_dibu * fsl__diff_builder_json1(void){
  fsl_dibu * rc = fsl_dibu_alloc(0);
  if(rc){
    rc->chunkHeader = NULL;
    rc->start = fdb__json1_start;
    rc->skip = fdb__json1_skip;
    rc->common = fdb__json1_common;
    rc->insertion = fdb__json1_insertion;
    rc->deletion = fdb__json1_deletion;
    rc->replacement = fdb__json1_replacement;
    rc->edit = fdb__json1_edit;
    rc->finish = fdb__json1_finish;
    rc->finalize = fsl_dibu_finalizer;
    assert(!rc->pimpl);
    assert(0==rc->implFlags);
    assert(0==rc->lnLHS);
    assert(0==rc->lnRHS);
    assert(NULL==rc->opt);
  }
  return rc;
}

/**
   State for the text-mode unified(-ish) diff builder.  We do some
   hoop-jumping here in order to combine runs of delete/insert pairs
   into a group of deletes followed by a group of inserts. It's a
   cosmetic detail only but it makes for more readable output.
*/
struct DiBuUnified {
  /** True if currently processing a block of deletes, else false. */
  bool deleting;
  /** Buffer for insertion lines which are part of delete/insert
      pairs. */
  fsl_buffer bufIns;
};
typedef struct DiBuUnified DiBuUnified;

#define UIMPL(V) DiBuUnified * const V = (DiBuUnified*)b->pimpl
/**
   If utxt diff builder b has any INSERT lines to flush, this
   flushes them. Sets b->impl->deleting to false. Returns non-0
   on output error.
*/
static int fdb__utxt_flush_ins(fsl_dibu * const b){
  int rc = 0;
  UIMPL(p);
  p->deleting = false;
  if(p->bufIns.used>0){
    rc = fdb__out(b, fsl_buffer_cstr(&p->bufIns), p->bufIns.used);
    fsl_buffer_reuse(&p->bufIns);
  }
  return rc;
}

static int fdb__utxt_start(fsl_dibu * const b){
  int rc = 0;
  UIMPL(p);
  p->deleting = false;
  if(p->bufIns.mem) fsl_buffer_reuse(&p->bufIns);
  else fsl_buffer_reserve(&p->bufIns, 1024 * 2);
  if(0==(FSL_DIFF2_NOINDEX & b->opt->diffFlags)){
    rc = fdb__outf(b,"Index: %s\n%.66c\n",
                   b->opt->nameLHS/*RHS?*/, '=');
  }
  if(0==rc){
    rc = fdb__outf(b, "--- %s\n+++ %s\n",
                   b->opt->nameLHS, b->opt->nameRHS);
  }
  return rc;
}

static int fdb__utxt_chunkHeader(fsl_dibu* const b,
                                 uint32_t lnnoLHS, uint32_t linesLHS,
                                 uint32_t lnnoRHS, uint32_t linesRHS ){
  /*
    Ticket 746ebbe86c20b5c0f96cdadd19abd8284770de16:

    Annoying cosmetic bug: the libf impl of this diff will sometimes
    render two directly-adjecent chunks with a separator, e.g.:
  */

  // $ f-vdiff --format u 072d63965188 a725befe5863 -l '*vdiff*' | head -30
  // Index: f-apps/f-vdiff.c
  // ==================================================================
  // --- f-apps/f-vdiff.c
  // +++ f-apps/f-vdiff.c
  // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  //     36     36    fsl_buffer fname;
  //     37     37    fsl_buffer fcontent1;
  //     38     38    fsl_buffer fcontent2;
  //     39     39    fsl_buffer fhash;
  //     40     40    fsl_list globs;
  //            41 +  fsl_dibu_opt diffOpt;
  //            42 +  fsl_diff_builder * diffBuilder;
  //     41     43  } VDiffApp = {
  //     42     44  NULL/*glob*/,
  //     43     45  5/*contextLines*/,
  //     44     46  0/*sbsWidth*/,
  //     45     47  0/*diffFlags*/,
  // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  //     46     48  0/*brief*/,
  //     47     49  fsl_buffer_empty_m/*fname*/,
  //     48     50  fsl_buffer_empty_m/*fcontent1*/,

  /*
    Note now the chunks before/after the second ~~~ line are
    consecutive lines of code. In fossil(1) that case is accounted for
    in the higher-level diff engine, which can not only collapse
    adjacent blocks but also does the rendering of chunk headers in
    that main algorithm (something we cannot do in the library because
    we need the fsl_dibu to be able to output to arbitrary
    destinations). We can only _partially_ account for it here,
    eliminating the extraneous ~~~ line when we're in line-number
    mode. In non-line-number mode we have to output the chunk header
    as-is. If we skip it then the _previous_ chunk header, if any,
    will contain incorrect numbers for the chunk, invaliding the diff
    for purposes of tools which import unified-format diffs.
  */
  int rc = fdb__utxt_flush_ins(b);
  if(0==rc){
    if(FSL_DIFF2_LINE_NUMBERS & b->opt->diffFlags){
      rc = (lnnoLHS == b->lnLHS+1 && lnnoRHS == b->lnRHS+1)
        ? 0
        : fdb__outf(b, "%.40c\n", '~');
    }else{
      rc = fdb__outf(b, "@@ -%" PRIu32 ",%" PRIu32
                     " +%" PRIu32 ",%" PRIu32 " @@\n",
                     lnnoLHS, linesLHS, lnnoRHS, linesRHS);
    }
  }
  return rc;
}


static int fdb__utxt_skip(fsl_dibu * const b, uint32_t n){
  //MARKER(("SKIP\n"));
  int rc = fdb__utxt_flush_ins(b);
  b->lnLHS += n;
  b->lnRHS += n;
  return rc;
}

/**
   Outputs line numbers, if configured to, to b->opt->out.

   - 2 line numbers = common lines
   - lnL only = deletion
   - lnR only = insertion
*/
static int fdb__utxt_lineno(fsl_dibu * const b, uint32_t lnL, uint32_t lnR){
  int rc = 0;
  if(FSL_DIFF2_LINE_NUMBERS & b->opt->diffFlags){
    UIMPL(p);
    if(lnL){ // common or delete
      rc = fdb__outf(b, "%s%6" PRIu32 "%s ",
                     (lnR ? "" : b->opt->ansiColor.deletion),
                     lnL,
                     (lnR ? "" : b->opt->ansiColor.reset));
    }else if(p->deleting){ // insert during deletion grouping
      rc = fsl_buffer_append(&p->bufIns, "       ", 7);
    }else{ // insert w/o deleting grouping
      rc = fdb__out(b, "       ", 7);
    }
    if(0==rc){
      if(!lnL && lnR && p->deleting){ // insert during deletion grouping
        rc = fsl_buffer_appendf(&p->bufIns, "%s%6" PRIu32 "%s ",
                                b->opt->ansiColor.insertion,
                                lnR, b->opt->ansiColor.reset);
      }else if(lnR){ // common or insert w/o deletion grouping.
        rc = fdb__outf(b, "%s%6" PRIu32 "%s ",
                       (lnL ? "" : b->opt->ansiColor.insertion),
                       lnR,
                       (lnL ? "" : b->opt->ansiColor.reset));
      }else{ // deletion
        rc = fdb__out(b, "       ", 7);
      }
    }
  }
  return rc;
}

static int fdb__utxt_common(fsl_dibu * const b, fsl_dline const * pLine){
  //MARKER(("COMMON\n"));
  int rc = fdb__utxt_flush_ins(b);
  if(0==rc){
    ++b->lnLHS;
    ++b->lnRHS;
    rc = fdb__utxt_lineno(b, b->lnLHS, b->lnRHS);
  }
  return rc ? rc : fdb__outf(b, " %.*s\n", (int)pLine->n, pLine->z);
}

static int fdb__utxt_insertion(fsl_dibu * const b, fsl_dline const * pLine){
  //MARKER(("INSERT\n"));
  int rc;
  ++b->lnRHS;
  rc = fdb__utxt_lineno(b, 0, b->lnRHS);
  if(0==rc){
    UIMPL(p);
    if(p->deleting){
      rc = fsl_buffer_appendf(&p->bufIns, "%s+%.*s%s\n",
                              b->opt->ansiColor.insertion,
                              (int)pLine->n, pLine->z,
                              b->opt->ansiColor.reset);
    }else{
      rc = fdb__outf(b, "%s+%.*s%s\n",
                     b->opt->ansiColor.insertion,
                     (int)pLine->n, pLine->z,
                     b->opt->ansiColor.reset);
    }
  }
  return rc;
}
static int fdb__utxt_deletion(fsl_dibu * const b, fsl_dline const * pLine){
  //MARKER(("DELETE\n"));
  UIMPL(p);
  int rc = p->deleting ? 0 : fdb__utxt_flush_ins(b);
  if(0==rc){
    p->deleting = true;
    ++b->lnLHS;
    rc = fdb__utxt_lineno(b, b->lnLHS, 0);
  }
  return rc ? rc : fdb__outf(b, "%s-%.*s%s\n",
                             b->opt->ansiColor.deletion,
                             (int)pLine->n, pLine->z,
                             b->opt->ansiColor.reset);
}
static int fdb__utxt_replacement(fsl_dibu * const b,
                                 fsl_dline const * lineLhs,
                                 fsl_dline const * lineRhs) {
  //MARKER(("REPLACE\n"));
  int rc = b->deletion(b, lineLhs);
  if(0==rc) rc = b->insertion(b, lineRhs);
  return rc;
}
static int fdb__utxt_edit(fsl_dibu * const b,
                           fsl_dline const * lineLhs,
                           fsl_dline const * lineRhs){
  //MARKER(("EDIT\n"));
  int rc = b->deletion(b, lineLhs);
  if(0==rc) rc = b->insertion(b, lineRhs);
  return rc;
}

static int fdb__utxt_finish(fsl_dibu * const b){
  int rc = fdb__utxt_flush_ins(b);
  UIMPL(p);
  fsl_buffer_reuse(&p->bufIns);
  return rc;
}

static void fdb__utxt_finalize(fsl_dibu * const b){
  UIMPL(p);
  fsl_buffer_clear(&p->bufIns);
  fsl_free(b);
}

static fsl_dibu * fsl__diff_builder_utxt(void){
  const DiBuUnified DiBuUnified_empty = {
  false, fsl_buffer_empty_m
  };
  fsl_dibu * rc = fsl_dibu_alloc(sizeof(DiBuUnified));
  if(!rc) return NULL;
  assert(NULL!=rc->pimpl);
  assert(NULL==rc->finally);
  *((DiBuUnified*)rc->pimpl) = DiBuUnified_empty;
  rc->chunkHeader = fdb__utxt_chunkHeader;
  rc->start = fdb__utxt_start;
  rc->skip = fdb__utxt_skip;
  rc->common = fdb__utxt_common;
  rc->insertion = fdb__utxt_insertion;
  rc->deletion = fdb__utxt_deletion;
  rc->replacement = fdb__utxt_replacement;
  rc->edit = fdb__utxt_edit;
  rc->finish = fdb__utxt_finish;
  rc->finalize = fdb__utxt_finalize;
  return rc;
}
#undef UIMPL

struct DiBuTcl {
  /** Buffer for TCL-format string conversion */
  fsl_buffer str;
};
typedef struct DiBuTcl DiBuTcl;
static const DiBuTcl DiBuTcl_empty = {fsl_buffer_empty_m};

#define BR_OPEN if(FSL_DIBU_TCL_BRACES & b->implFlags) \
    rc = fdb__out(b, "{", 1)
#define BR_CLOSE if(FSL_DIBU_TCL_BRACES & b->implFlags) \
    rc = fdb__out(b, "}", 1)

#define DTCL_BUFFER(B) &((DiBuTcl*)(B)->pimpl)->str
static int fdb__outtcl(fsl_dibu * const b,
                       char const *z, unsigned int n,
                       char chAppend ){
  int rc;
  fsl_buffer * const o = DTCL_BUFFER(b);
  fsl_buffer_reuse(o);
  rc = fsl_buffer_append_tcl_literal(o,
                                     (b->implFlags & FSL_DIBU_TCL_BRACES_ESC),
                                     z, n);
  if(0==rc) rc = fdb__out(b, (char const *)o->mem, o->used);
  if(chAppend && 0==rc) rc = fdb__out(b, &chAppend, 1);
  return rc;
}

static int fdb__tcl_start(fsl_dibu * const b){
  int rc = 0;
  fsl_buffer_reuse(DTCL_BUFFER(b));
  if(1==++b->fileCount &&
     FSL_DIBU_TCL_TK==(b->implFlags & FSL_DIBU_TCL_TK)){
    rc = fdb__out(b, "set difftxt {\n", -1);
  }
  if(0==rc && b->fileCount>1) rc = fdb__out(b, "\n", 1);
  if(0==rc && b->opt->nameLHS){
    char const * zRHS =
      b->opt->nameRHS ? b->opt->nameRHS : b->opt->nameLHS;
    BR_OPEN;
    if(0==rc) rc = fdb__out(b, "FILE ", 5);
    if(0==rc) rc = fdb__outtcl(b, b->opt->nameLHS,
                               (unsigned)fsl_strlen(b->opt->nameLHS), ' ');
    if(0==rc) rc = fdb__outtcl(b, zRHS,
                               (unsigned)fsl_strlen(zRHS), 0);
    if(0==rc) {BR_CLOSE;}
    if(0==rc) rc = fdb__out(b, "\n", 1);
  }
  return rc;
}

static int fdb__tcl_skip(fsl_dibu * const b, uint32_t n){
  int rc = 0;
  BR_OPEN;
  if(0==rc) rc = fdb__outf(b, "SKIP %" PRIu32, n);
  if(0==rc) {BR_CLOSE;}
  if(0==rc) rc = fdb__outf(b, "\n", 1);
  return rc;
}

static int fdb__tcl_common(fsl_dibu * const b, fsl_dline const * pLine){
  int rc = 0;
  BR_OPEN;
  if(0==rc) rc = fdb__out(b, "COM  ", 5);
  if(0==rc) rc= fdb__outtcl(b, pLine->z, pLine->n, 0);
  if(0==rc) {BR_CLOSE;}
  if(0==rc) rc = fdb__outf(b, "\n", 1);
  return rc;
}
static int fdb__tcl_insertion(fsl_dibu * const b, fsl_dline const * pLine){
  int rc = 0;
  BR_OPEN;
  if(0==rc) rc = fdb__out(b, "INS  ", 5);
  if(0==rc) rc = fdb__outtcl(b, pLine->z, pLine->n, 0);
  if(0==rc) {BR_CLOSE;}
  if(0==rc) rc = fdb__outf(b, "\n", 1);
  return rc;
}
static int fdb__tcl_deletion(fsl_dibu * const b, fsl_dline const * pLine){
  int rc = 0;
  BR_OPEN;
  if(0==rc) rc = fdb__out(b, "DEL  ", 5);
  if(0==rc) rc = fdb__outtcl(b, pLine->z, pLine->n, 0);
  if(0==rc) {BR_CLOSE;}
  if(0==rc) rc = fdb__outf(b, "\n", 1);
  return rc;
}
static int fdb__tcl_replacement(fsl_dibu * const b,
                                fsl_dline const * lineLhs,
                                fsl_dline const * lineRhs) {
  int rc = 0;
  BR_OPEN;
  if(0==rc) rc = fdb__out(b, "EDIT \"\" ", 8);
  if(0==rc) rc = fdb__outtcl(b, lineLhs->z, lineLhs->n, ' ');
  if(0==rc) rc = fdb__outtcl(b, lineRhs->z, lineRhs->n, 0);
  if(0==rc) {BR_CLOSE;}
  if(0==rc) rc = fdb__outf(b, "\n", 1);
  return rc;
}

static int fdb__tcl_edit(fsl_dibu * const b,
                         fsl_dline const * lineLHS,
                         fsl_dline const * lineRHS){
  int rc = 0;
  int i, x;
  fsl_dline_change chng = fsl_dline_change_empty;
#define RC if(rc) goto end
  BR_OPEN;
  rc = fdb__out(b, "EDIT", 4); RC;
  fsl_dline_change_spans(lineLHS, lineRHS, &chng);
  for(i=x=0; i<chng.n; i++){
    rc = fdb__out(b, " ", 1); RC;
    rc = fdb__outtcl(b, lineLHS->z + x, chng.a[i].iStart1 - x, ' '); RC;
    x = chng.a[i].iStart1;
    rc = fdb__outtcl(b, lineLHS->z + x, chng.a[i].iLen1, ' '); RC;
    x += chng.a[i].iLen1;
    rc = fdb__outtcl(b, lineRHS->z + chng.a[i].iStart2,
                     chng.a[i].iLen2, 0); RC;
  }
  assert(0==rc);
  if( x < lineLHS->n ){
    rc = fdb__out(b, " ", 1); RC;
    rc = fdb__outtcl(b, lineLHS->z + x, lineLHS->n - x, 0); RC;
  }
  BR_CLOSE; RC;
  rc = fdb__out(b, "\n", 1);
  end:
#undef RC
  return rc;
}

static int fdb__tcl_finish(fsl_dibu * const b fsl__unused){
  int rc = 0;
  (void)b;
#if 0
  BR_CLOSE;
  if(0==rc && FSL_DIBU_TCL_BRACES & b->implFlags){
    rc = fdb__out(b, "\n", 1);
  }
#endif
  return rc;
}
static int fdb__tcl_finally(fsl_dibu * const b){
  int rc = 0;
  if(FSL_DIBU_TCL_TK==(b->implFlags & FSL_DIBU_TCL_TK)){
    extern char const * fsl_difftk_cstr;
    if(!b->fileCount) rc = fdb__out(b,"set difftxt {\n",-1);
    if(0==rc) rc = fdb__out(b, "}\nset fossilcmd {}\n", -1);
    if(0==rc) rc = fdb__out(b, fsl_difftk_cstr, -1);
  }
  return rc;
}

#undef BR_OPEN
#undef BR_CLOSE

static void fdb__tcl_finalize(fsl_dibu * const b){
  fsl_buffer_clear( &((DiBuTcl*)b->pimpl)->str );
  *b = fsl_dibu_empty;
  fsl_free(b);
}

static fsl_dibu * fsl__diff_builder_tcl(void){
  fsl_dibu * rc =
    fsl_dibu_alloc((fsl_size_t)sizeof(DiBuTcl));
  if(rc){
    rc->chunkHeader = NULL;
    rc->start = fdb__tcl_start;
    rc->skip = fdb__tcl_skip;
    rc->common = fdb__tcl_common;
    rc->insertion = fdb__tcl_insertion;
    rc->deletion = fdb__tcl_deletion;
    rc->replacement = fdb__tcl_replacement;
    rc->edit = fdb__tcl_edit;
    rc->finish = fdb__tcl_finish;
    rc->finally = fdb__tcl_finally;
    rc->finalize = fdb__tcl_finalize;
    assert(0!=rc->pimpl);
    DiBuTcl * const dbt = (DiBuTcl*)rc->pimpl;
    *dbt = DiBuTcl_empty;
    if(fsl_buffer_reserve(&dbt->str, 120)){
      rc->finalize(rc);
      rc = 0;
    }
  }
  return rc;
}

static int fdb__splittxt_mod(fsl_dibu * const b, char ch){
  assert(2==b->passNumber);
  return fdb__outf(b, " %c ", ch);
}

static int fdb__splittxt_lineno(fsl_dibu * const b,
                                DiffCounter const * const sst,
                                bool isLeft, uint32_t n){
  assert(2==b->passNumber);
  int const col = isLeft ? DICO_NUM1 : DICO_NUM2;
  return n
    ? fdb__outf(b, "%*" PRIu32 " ", sst->maxWidths[col], n)
    : fdb__outf(b, "%.*c ", sst->maxWidths[col], ' ');
}

static int fdb__splittxt_start(fsl_dibu * const b){
  int rc = 0;
  if(1==b->passNumber){
    DICOSTATE(sst);
    *sst = DiffCounter_empty;
    ++b->fileCount;
    return rc;
  }
  if(b->fileCount>1){
    rc = fdb__out(b, "\n", 1);
  }
  if(0==rc){
    fsl_dibu_opt const * const o = b->opt;
    if(o->nameLHS || o->nameRHS
       || o->hashLHS || o->hashRHS){
      rc = fdb__outf(b, "--- %s%s%s\n+++ %s%s%s\n",
                     o->nameLHS ? o->nameLHS : "",
                     (o->nameLHS && o->hashLHS) ? " " : "",
                     o->hashLHS ? o->hashLHS : "",
                     o->nameRHS ? o->nameRHS : "",
                     (o->nameRHS && o->hashRHS) ? " " : "",
                     o->hashRHS ? o->hashRHS : "");
    }
  }
  return rc;
}

static int fdb__splittxt_skip(fsl_dibu * const b, uint32_t n){
  b->lnLHS += n;
  b->lnRHS += n;
  if(1==b->passNumber) return 0;
  DICOSTATE(sst);
  int const maxWidth1 = maxColWidth(b, sst, DICO_TEXT1);
  int const maxWidth2 = maxColWidth(b, sst, DICO_TEXT2);
  return fdb__outf(b, "%.*c %.*c   %.*c %.*c\n",
                 sst->maxWidths[DICO_NUM1], '~',
                 maxWidth1, '~',
                 sst->maxWidths[DICO_NUM2], '~',
                 maxWidth2, '~');
}

static int fdb__splittxt_color(fsl_dibu * const b,
                               int modType){
  char const *z = 0;
  switch(modType){
    case (int)'i': z = b->opt->ansiColor.insertion; break;
    case (int)'d': z = b->opt->ansiColor.deletion; break;
    case (int)'r'/*replacement*/: 
    case (int)'e': z = b->opt->ansiColor.edit; break;
    case 0: z = b->opt->ansiColor.reset; break;
    default:
      assert(!"invalid color op!");
  }
  return z&&*z ? fdb__outf(b, "%s", z) : 0;
}

static int fdb__splittxt_side(fsl_dibu * const b,
                              DiffCounter * const sst,
                              bool isLeft,
                              fsl_dline const * const pLine){
  int rc = fdb__splittxt_lineno(b, sst, isLeft,
                                pLine ? (isLeft ? b->lnLHS : b->lnRHS) : 0U);
  if(0==rc){
    uint32_t const w = maxColWidth(b, sst, isLeft ? DICO_TEXT1 : DICO_TEXT2);
    if(pLine){
      fsl_size_t const nU =
        /* Measure column width in UTF8 characters, not bytes! */
        fsl_strlen_utf8(pLine->z, (fsl_int_t)pLine->n);
      rc = fdb__outf(b, "%#.*s", (int)(w < nU ? w : nU), pLine->z);
      if(0==rc && w>nU){
        rc = fdb__outf(b, "%.*c", (int)(w - nU), ' ');
      }
    }else{
      rc = fdb__outf(b, "%.*c", (int)w, ' ');
    }
    if(0==rc && !isLeft) rc = fdb__out(b, "\n", 1);
  }
  return rc;
}

static int fdb__splittxt_common(fsl_dibu * const b,
                                fsl_dline const * const pLine){
  int rc = 0;
  DICOSTATE(sst);
  ++b->lnLHS;
  ++b->lnRHS;
  if(1==b->passNumber){
    fdb__dico_update_maxlen(sst, DICO_TEXT1, pLine->z, pLine->n);
    fdb__dico_update_maxlen(sst, DICO_TEXT2, pLine->z, pLine->n);
    return 0;
  }
  rc = fdb__splittxt_side(b, sst, true, pLine);
  if(0==rc) rc = fdb__splittxt_mod(b, ' ');
  if(0==rc) rc = fdb__splittxt_side(b, sst, false, pLine);
  return rc;
}

static int fdb__splittxt_insertion(fsl_dibu * const b,
                                   fsl_dline const * const pLine){
  int rc = 0;
  DICOSTATE(sst);
  ++b->lnRHS;
  if(1==b->passNumber){
    fdb__dico_update_maxlen(sst, DICO_TEXT1, pLine->z, pLine->n);
    return rc;
  }
  rc = fdb__splittxt_color(b, 'i');
  if(0==rc) rc = fdb__splittxt_side(b, sst, true, NULL);
  if(0==rc) rc = fdb__splittxt_mod(b, '>');
  if(0==rc) rc = fdb__splittxt_side(b, sst, false, pLine);
  if(0==rc) rc = fdb__splittxt_color(b, 0);
  return rc;
}

static int fdb__splittxt_deletion(fsl_dibu * const b,
                                  fsl_dline const * const pLine){
  int rc = 0;
  DICOSTATE(sst);
  ++b->lnLHS;
  if(1==b->passNumber){
    fdb__dico_update_maxlen(sst, DICO_TEXT2, pLine->z, pLine->n);
    return rc;
  }
  rc = fdb__splittxt_color(b, 'd');
  if(0==rc) rc = fdb__splittxt_side(b, sst, true, pLine);
  if(0==rc) rc = fdb__splittxt_mod(b, '<');
  if(0==rc) rc = fdb__splittxt_side(b, sst, false, NULL);
  if(0==rc) rc = fdb__splittxt_color(b, 0);
  return rc;
}

static int fdb__splittxt_replacement(fsl_dibu * const b,
                                     fsl_dline const * const lineLhs,
                                     fsl_dline const * const lineRhs) {
#if 0
  int rc = b->deletion(b, lineLhs);
  if(0==rc) rc = b->insertion(b, lineRhs);
  return rc;
#else    
  int rc = 0;
  DICOSTATE(sst);
  ++b->lnLHS;
  ++b->lnRHS;
  if(1==b->passNumber){
    fdb__dico_update_maxlen(sst, DICO_TEXT1, lineLhs->z, lineLhs->n);
    fdb__dico_update_maxlen(sst, DICO_TEXT2, lineRhs->z, lineRhs->n);
    return 0;
  }
  rc = fdb__splittxt_color(b, 'e');
  if(0==rc) rc = fdb__splittxt_side(b, sst, true, lineLhs);
  if(0==rc) rc = fdb__splittxt_mod(b, '|');
  if(0==rc) rc = fdb__splittxt_side(b, sst, false, lineRhs);
  if(0==rc) rc = fdb__splittxt_color(b, 0);
  return rc;
#endif
}

static int fdb__splittxt_finish(fsl_dibu * const b){
  int rc = 0;
  if(1==b->passNumber){
    DICOSTATE(sst);
    uint32_t ln = b->lnLHS;
    /* Calculate width of line number columns. */
    sst->maxWidths[DICO_NUM1] = sst->maxWidths[DICO_NUM2] = 1;
    for(; ln>=10; ln/=10) ++sst->maxWidths[DICO_NUM1];
    ln = b->lnRHS;
    for(; ln>=10; ln/=10) ++sst->maxWidths[DICO_NUM2];
  }
  return rc;
}

static void fdb__splittxt_finalize(fsl_dibu * const b){
  *b = fsl_dibu_empty;
  fsl_free(b);
}

static fsl_dibu * fsl__diff_builder_splittxt(void){
  fsl_dibu * rc =
    fsl_dibu_alloc((fsl_size_t)sizeof(DiffCounter));
  if(rc){
    rc->twoPass = true;
    rc->chunkHeader = NULL;
    rc->start = fdb__splittxt_start;
    rc->skip = fdb__splittxt_skip;
    rc->common = fdb__splittxt_common;
    rc->insertion = fdb__splittxt_insertion;
    rc->deletion = fdb__splittxt_deletion;
    rc->replacement = fdb__splittxt_replacement;
    rc->edit = fdb__splittxt_replacement;
    rc->finish = fdb__splittxt_finish;
    rc->finalize = fdb__splittxt_finalize;
    assert(0!=rc->pimpl);
    DiffCounter * const sst = (DiffCounter*)rc->pimpl;
    *sst = DiffCounter_empty;
  }
  return rc;
}

int fsl_dibu_factory( fsl_dibu_e type,
                              fsl_dibu **pOut ){
  int rc = FSL_RC_TYPE;
  fsl_dibu * (*factory)(void) = NULL;
  switch(type){
    case FSL_DIBU_DEBUG:
      factory = fsl__diff_builder_debug;
      break;
    case FSL_DIBU_JSON1:
      factory = fsl__diff_builder_json1;
      break;
    case FSL_DIBU_UNIFIED_TEXT:
      factory = fsl__diff_builder_utxt;
      break;
    case FSL_DIBU_TCL:
      factory = fsl__diff_builder_tcl;
      break;
    case FSL_DIBU_SPLIT_TEXT:
      factory = fsl__diff_builder_splittxt;
      break;
    case FSL_DIBU_INVALID: break;
  }
  if(NULL!=factory){
    *pOut = factory();
    rc = *pOut ? 0 : FSL_RC_OOM;
  }
  return rc;
}

#undef MARKER
#undef DICOSTATE
#undef DTCL_BUFFER
