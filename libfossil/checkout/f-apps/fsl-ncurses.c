/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/
/*
  This file holds a custom fsl_dibu implementation which
  renders its output to an ncurses PAD
*/
#include "libfossil.h"
#include <ncurses.h>
#include <panel.h>
#include <stdarg.h>
#include <locale.h>

#include "fsl-ncurses.h"

/* Only for debugging */
#include <stdio.h>
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

static unsigned ncAttr[fsl_dibu_nc_attr__end
                       - fsl_dibu_nc_attr__start] = {0,0,0,0,0,0};

unsigned int fsl_dibu_nc_attr_get(fsl_dibu_nc_attr_e n){
  return ncAttr[n - fsl_dibu_nc_attr__start];
}
void fsl_dibu_nc_attr_set(fsl_dibu_nc_attr_e n, unsigned int v){
  ncAttr[n - fsl_dibu_nc_attr__start] = v;
}

static int _ncInitCount = 0;

void fnc_screen_init(void){
  ++_ncInitCount;
  if(NULL==stdscr){
    setlocale(LC_ALL, "")/*needed for ncurses w/ UTF8 chars*/;
    initscr();
    nonl(); keypad(stdscr,TRUE); cbreak();
    curs_set(0);
  }
  if(_ncInitCount>1) return;
  if(has_colors()){
    start_color();
#define CP(N,F,B,A) init_pair(N, F, B); \
    fsl_dibu_nc_attr_set(N, COLOR_PAIR(N) | (A))
    CP(fsl_dibu_nc_attr_Window, COLOR_WHITE, COLOR_BLACK, 0);
    CP(fsl_dibu_nc_attr_Index, COLOR_CYAN, COLOR_BLACK, A_BOLD);
    CP(fsl_dibu_nc_attr_ChunkSplitter, COLOR_YELLOW, COLOR_BLACK, 0);
    CP(fsl_dibu_nc_attr_Common, COLOR_WHITE, COLOR_BLACK, 0); //A_DIM
    CP(fsl_dibu_nc_attr_Insert, COLOR_GREEN, COLOR_BLACK, A_BOLD);
    CP(fsl_dibu_nc_attr_Delete, COLOR_RED, COLOR_BLACK, A_BOLD);
    CP(fsl_dibu_nc_attr_Help, COLOR_BLACK, COLOR_YELLOW, A_BOLD);
#undef CP
    fsl_dibu_nc_attr_set(fsl_dibu_nc_attr_EOF,
                         fsl_dibu_nc_attr_get(fsl_dibu_nc_attr_Window) | A_REVERSE);
    wbkgd(stdscr, fsl_dibu_nc_attr_get(fsl_dibu_nc_attr_Window));
  }
}

void fnc_screen_shutdown(void){
  if(0==--_ncInitCount && NULL!=stdscr){
    endwin();
    stdscr = NULL;
  }
}

/** @internal

   Column indexes for fsl_dibu_ncu::maxWidths and friends.  This is
   only public so that that array can be sized statically.
*/
enum FslDiffBuilderCols_e {
FDBCols_NUM1 = 0, FDBCols_TEXT1,
FDBCols_MOD,
FDBCols_NUM2, FDBCols_TEXT2,
FDBCols_count
};

/**
   Internal state for the ncurses unified-format-ish diff
   builder. All of this state is to be considered private.
*/
struct fsl_dibu_ncu {
  /**
     Largest column width we've yet seen.

     FIXME: these are in bytes, not text columns. The current code may
     truncate multibyte characters.
  */
  uint32_t maxWidths[FDBCols_count];
  /**
     Max line numbers seen for the LHS/RHS input files. This is likely
     higher than the number of diff lines.

     These are used for calculating the required width of the
     line-number output.
  */
  uint32_t lineCount[2];
  /**
     The actual number of lines needed for rendering the file,
     including diff-injected info like index and chunk splitter lines.
  */
  uint32_t displayLines;
  /**
     The column width, in bytes, needed for rendering the file
     content. The internal width calculation will count multi-byte
     characters as multiple columns. No harm done, it's just going to
     use more horizontal screen space.
  */
  uint32_t displayWidth;

  /**
     Rendering surface for the builder. Created and owned by this object.
  */
  WINDOW * pad;

  /**
     Records whether this instance has calls fnc_screen_init(). If so,
     its finalizer will call fnc_screen_shutdown().
  */
  bool initedScreen;
  /**
     Internal buffer.
  */
  fsl_buffer buf;
  /**
     Internal buffer for collapsing runs of adjacent DELETE/INSERT
     pairs from DIDIDI... to DDD/III...
  */
  fsl_buffer bufIns;

  /**
     Stores line numbers, relative to this->pad, at which Index lines
     are found. That's used to add a next/previous file keybinding in
     the diff view.
  */
  struct {
    /** Array of line numbers. */
    uint32_t * lineNumbers;
    /** Number entries allocated in this->lineNumbers */
    uint32_t alloced;
    /** Number of entries used in this->lineNumbers */
    uint32_t used;
  } indexes;
};
typedef struct fsl_dibu_ncu fsl_dibu_ncu;

static const fsl_dibu_ncu fsl_dibu_ncu_empty = {
{7,0,1,7,0},{0,0},
0/*displayLines*/,
0/*displayWidth*/,
NULL/*pad*/,false/*initedScreen*/,
fsl_buffer_empty_m/*buf*/,
fsl_buffer_empty_m/*bufIns*/,
{/*indexes*/ NULL/*lineNumbers*/, 0/*alloced*/, 0/*used*/
}
};

#define DSTATE(VNAME) fsl_dibu_ncu * const VNAME = (fsl_dibu_ncu *)b->pimpl

/**
   Returns true if its argument was allocated via fsl_dibu_ncu_alloc(),
   else false.
 */
bool fsl_dibu_is_ncu(fsl_dibu const * const b);

/**
   If fsl_dibu_is_ncu() returns true for the given builder, this
   returns that builder's type-specific state, else it returns NULL.
*/
fsl_dibu_ncu * fsl_dibu_ncu_pimpl(fsl_dibu * const b);

/**
   This is a very basic implementation of a diff viewer intended to be
   run after a fsl_dibu has processed its diffs and is ready
   to be rendered. This enters an input look which accepts navigation
   keys until the user taps 'q' to quit the loop. w must be the target
   window on top of which to render (e.g. stdscr).
*/
void fsl_dibu_ncu_basic_loop(fsl_dibu_ncu * const nc,
                             WINDOW * const w);

/**
   Characters used in rendering scrollbars.
*/
static const struct NCScrollbarConfig {
  char const * vTop;
  char const * vBottom;
  char const * hLeft;
  char const * hRight;
  char const * curPos;
  char const * filler;
} NCScrollbarConfig = {
"△",//vTop
"▽",//vBottom
"◁",//hLeft
"▷",//hRight
"▣",//curPos
"░"//filler
};

/**
   Renders a vertical scrollbar on window tgt at column x, running
   from the given top row to the row (top+barHeight). The scroll
   indicator is rendered depending on the final two arguments:
   lineCount specifies the number of lines in the scrollable
   input and currentLine specifies the top-most line number of the
   currently-displayed data. e.g. a lineCount of 100 and currentLine
   of 1 puts the indicator 1% of the way down the scrollbar. Likewise,
   a lineCount of 100 and currentLine of 90 puts the indicator at the
   90% point.
 */
void fnc_scrollbar_v( WINDOW * const tgt, int top, int x,
                         int barHeight, int lineCount,
                         int currentLine ){
  int y, bottom = top + barHeight - 1,
    dotPos = top + (int)((double)currentLine / lineCount * barHeight) + 1;
  struct NCScrollbarConfig const * const conf = &NCScrollbarConfig;
  unsigned int const attr = A_REVERSE;
  wattron(tgt, attr);
  mvwprintw(tgt, top, x, "%s", conf->vTop);
  for(y = top+1; y<bottom; ++y){
    mvwprintw(tgt, y, x, "%s", conf->filler);
  }
  wattroff(tgt, attr);
  mvwprintw(tgt, dotPos>=bottom?bottom-1:dotPos, x,
            "%s", conf->curPos);
  wattron(tgt, attr);
  mvwprintw(tgt, bottom, x, "%s", conf->vBottom);
  wattroff(tgt, attr);
}

/**
   Works like fnc_scrollbar_v() but renders a horizontal scrollbar
   at the given left/top coordinates of tgt.
*/
void fnc_scrollbar_h( WINDOW * const tgt, int top, int left,
                         int barWidth, int colCount,
                         int currentCol ){
  int x, right = left + barWidth - 1,
    dotPos = left + (int)((double)currentCol / colCount * barWidth) + 1;
  struct NCScrollbarConfig const * const conf = &NCScrollbarConfig;
  unsigned int const attr = A_REVERSE;
  wattron(tgt, attr);
  mvwprintw(tgt, top, left, "%s", conf->hLeft);
  for(x = left+1; x<right; ++x){
    mvwprintw(tgt, top, x, "%s", conf->filler);
  }
  wattroff(tgt, attr);
  mvwprintw(tgt, top, dotPos>=x?x-1:dotPos,
            "%s", conf->curPos);
  wattron(tgt, attr);
  mvwprintw(tgt, top, x, "%s", conf->hRight);
  wattroff(tgt, attr);
}

static int fdb__ncu_out(fsl_dibu_ncu *const sst, unsigned int ncattr,
                       char const *z, fsl_size_t n){
  int rc = 0;
  if(ncattr) wattron(sst->pad, ncattr);
#if 0
  /* Failing at some point i've yet to track down, even though
     rendering is okay. */
  rc = ERR==waddnstr(sst->pad, z, (int)n) ? FSL_RC_IO : 0;
#else
  waddnstr(sst->pad, z, (int)n);
#endif
  if(ncattr) wattroff(sst->pad, ncattr);
  return rc;
}

static int fdb__ncu_outf(fsl_dibu_ncu * const sst, unsigned int ncattr,
                        char const *fmt, ...){
  int rc = 0;
  if(NULL!=stdscr){
    va_list va;
    va_start(va,fmt);
    rc = fsl_buffer_appendfv(fsl_buffer_reuse(&sst->buf), fmt, va);
    if(0==rc) rc = fdb__ncu_out(sst, ncattr, fsl_buffer_cstr(&sst->buf), sst->buf.used);
    va_end(va);
  }
  return rc;
}

#if 0
static int maxColWidth(fsl_dibu const * const b,
                       fsl_dibu_ncu const * const sst,
                       int mwIndex){
  static const short minColWidth =
    10/*b->opt.columnWidth values smaller than this are treated as
        this value*/;
  switch(mwIndex){
    case FDBCols_NUM1:
    case FDBCols_NUM2:
    case FDBCols_MOD: return sst->maxWidths[mwIndex];
    case FDBCols_TEXT1: case FDBCols_TEXT2: break;
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
#endif

static void fdb__dico_update_maxlen(fsl_dibu_ncu * const sst,
                                    int col,
                                    char const * const z,
                                    uint32_t n){
  switch(col){
    case FDBCols_TEXT1: n+=2; break;
    default: break;
  }
  (void)z;
  if(sst->maxWidths[col]<n){
    sst->maxWidths[col] = n;
    //n = (uint32_t)fsl_strlen_utf8(z, (fsl_int_t)n);
    //if(sst->maxWidths[col]<n) sst->maxWidths[col] = n;
  }
}

static int fdb__ncu_reserve_indexes(fsl_dibu_ncu * const sst){
  int rc = 0;
  if(sst->indexes.used==sst->indexes.alloced){
    uint32_t const n = sst->indexes.alloced ?
      sst->indexes.alloced * 2 : 10;
    void * x = fsl_realloc(sst->indexes.lineNumbers,
                           sizeof(uint32_t)*n);
    assert(n>sst->indexes.used);
    if(!x) rc = FSL_RC_OOM;
    else{
      sst->indexes.lineNumbers = (uint32_t*)x;
      sst->indexes.alloced = n;
    }
  }
  return rc;
}

static int fdb__ncu_start(fsl_dibu * const b){
  int rc = 0;
  DSTATE(sst);
  b->pimplFlags = 0;
  if(sst->bufIns.mem) fsl_buffer_reuse(&sst->bufIns);
  else fsl_buffer_reserve(&sst->bufIns, 1024 * 2);
  if(1==b->passNumber){
    rc = fdb__ncu_reserve_indexes(sst);
    if(rc) return rc;
    sst->indexes.lineNumbers[sst->indexes.used++] =
      sst->displayLines;
    if(1==++b->fileCount){
      assert(!sst->pad);
    }
    if(!sst->initedScreen){
      sst->initedScreen = true;
      //MARKER(("initing curses screen.\n"));
      fnc_screen_init();
      mvwaddstr(stdscr,1,0,"Running diff... please wait...");
      wrefresh(stdscr);
    }
    if(0==(FSL_DIFF2_NOINDEX & b->opt->diffFlags)){
      sst->displayLines += 2;
      if(b->fileCount>1){
        ++sst->displayLines/*gap before 2nd+ index line*/;
      }
    }
    sst->displayLines += 2;
    fsl_size_t nName;
    if(b->opt->nameLHS &&
       (nName=fsl_strlen(b->opt->nameLHS)+5) >= sst->displayWidth){
      sst->displayWidth = nName;
    }
    if(b->opt->nameRHS &&
       (nName=fsl_strlen(b->opt->nameRHS)+5) >= sst->displayWidth){
      sst->displayWidth = nName;
    }
    return rc;
  }
  unsigned int attr = fsl_dibu_nc_attr_get(fsl_dibu_nc_attr_Index);
  if(0==(FSL_DIFF2_NOINDEX & b->opt->diffFlags)){
    if(b->fileCount>1){/*gap before 2nd+ index line*/
      fdb__ncu_out(sst, attr,"\n",1);
    }
    fdb__ncu_outf(sst, attr,"Index #%d: %s\n", (int)b->fileCount,b->opt->nameLHS/*RHS?*/);
    fdb__ncu_outf(sst, attr,"%.*c\n", (int)sst->displayWidth-2, '=');
  }
  fdb__ncu_outf(sst, attr,"--- %-17S %s\n", b->opt->hashLHS, b->opt->nameLHS);
  fdb__ncu_outf(sst, attr,"+++ %-17S %s\n", b->opt->hashRHS, b->opt->nameRHS);
  return rc;
}

/**
   If fsl_dibu_ncu diff builder b has any INSERT lines to flush, this
   flushes them. Sets b->impl->pimplFlags to 0.
*/
static int fdb__ncu_flush_ins(fsl_dibu * const b){
  int rc = 0;
  DSTATE(sst);
  b->pimplFlags = 0;
  if(sst->bufIns.used>0){
    rc = fdb__ncu_out(sst,
                      fsl_dibu_nc_attr_get(fsl_dibu_nc_attr_Insert),
                      fsl_buffer_cstr(&sst->bufIns),
                      sst->bufIns.used);
    fsl_buffer_reuse(&sst->bufIns);
  }
  return rc;
}

static int fdb__ncu_chunkHeader(fsl_dibu* const b,
                                  uint32_t lnnoLHS, uint32_t linesLHS,
                                  uint32_t lnnoRHS, uint32_t linesRHS ){
  DSTATE(sst);
  if(1==b->passNumber){
    if(FSL_DIFF2_LINE_NUMBERS & b->opt->diffFlags){
      ++sst->displayLines;
    }
    ++sst->displayLines;
    return 0;
  }
  unsigned int const attr =
    fsl_dibu_nc_attr_get(fsl_dibu_nc_attr_ChunkSplitter);
  if(FSL_DIFF2_LINE_NUMBERS & b->opt->diffFlags){
    fdb__ncu_outf(sst,attr, "%.40c\n", '~');
  }
  return fdb__ncu_outf(sst, attr, "@@ -%" PRIu32 ",%" PRIu32
                       " +%" PRIu32 ",%" PRIu32 " @@\n",
                       lnnoLHS, linesLHS, lnnoRHS, linesRHS);
}

static int fdb__ncu_skip(fsl_dibu * const b, uint32_t n){
  b->lnLHS += n;
  b->lnRHS += n;
  return 0;
}

/**
   Outputs line numbers, if configured to, to b->pimpl->pad.

   - lnL!=0, lnR!=0: common lines
   - lnL!=0, lnR==0: deletion
   - lnL==0, lnR!=0: insertion
*/
static int fdb__ncu_lineno(fsl_dibu * const b, uint32_t lnL, uint32_t lnR){
  int rc = 0;
  if(FSL_DIFF2_LINE_NUMBERS & b->opt->diffFlags){
    DSTATE(sst);
    unsigned int attr = 0;
    if(lnL){
      // common or delete
      attr = fsl_dibu_nc_attr_get(lnR
                                  ? fsl_dibu_nc_attr_Common
                                  : fsl_dibu_nc_attr_Delete);
      rc  = fdb__ncu_outf(sst, attr, "%*" PRIu32 " ",
                          (int)sst->maxWidths[FDBCols_NUM1], lnL);
    }else if(1==b->pimplFlags){
      // insert during deletion grouping
      rc = fsl_buffer_appendf(&sst->bufIns, "%.*c ",
                              (int)sst->maxWidths[FDBCols_NUM1], ' ');

    }else{
      // insert w/o deleting grouping
      rc = fdb__ncu_outf(sst, 0, "%.*c ",
                         (int)sst->maxWidths[FDBCols_NUM1], ' ');
    }
    if(0==rc){
      if(!lnL && lnR && 1==b->pimplFlags){
        // insert during deletion grouping
        rc = fsl_buffer_appendf(&sst->bufIns, "%*"PRIu32" ",
                                (int)sst->maxWidths[FDBCols_NUM2],
                                lnR);
      }else if(lnR){
        // common or insert w/o deletion grouping
        attr = fsl_dibu_nc_attr_get(lnL
                                    ? fsl_dibu_nc_attr_Common
                                    : fsl_dibu_nc_attr_Insert);
        rc = fdb__ncu_outf(sst, attr, "%*" PRIu32 " ",
                           (int)sst->maxWidths[FDBCols_NUM2], lnR);
      }else{
        // deletion
        rc = fdb__ncu_outf(sst, 0, "%.*c ",
                           (int)sst->maxWidths[FDBCols_NUM2], ' ');
      }
    }
  }
  return rc;
}

static int fdb__ncu_common(fsl_dibu * const b, fsl_dline const * pLine){
  DSTATE(sst);
  ++b->lnLHS;
  ++b->lnRHS;
  if(1==b->passNumber){
    ++sst->displayLines;
    fdb__dico_update_maxlen(sst, FDBCols_TEXT1, pLine->z, pLine->n);
    return 0;
  }
  int rc = fdb__ncu_flush_ins(b);
  if(0==rc) rc = fdb__ncu_lineno(b, b->lnLHS, b->lnRHS);
  return rc
    ? rc
    : fdb__ncu_outf(sst,
                    fsl_dibu_nc_attr_get(fsl_dibu_nc_attr_Common),
                    " %.*s\n", (int)pLine->n, pLine->z);
}
static int fdb__ncu_insertion(fsl_dibu * const b, fsl_dline const * pLine){
  DSTATE(sst);
  ++b->lnRHS;
  if(1==b->passNumber){
    ++sst->displayLines;
    fdb__dico_update_maxlen(sst, FDBCols_TEXT1, pLine->z, pLine->n);
    return 0;
  }
  int rc = fdb__ncu_lineno(b, 0, b->lnRHS);
  if(0==rc){
    if(1==b->pimplFlags){
      rc = fsl_buffer_appendf(&sst->bufIns,"+%.*s\n",
                              (int)pLine->n, pLine->z);
    }else{
      rc = fdb__ncu_outf(sst, fsl_dibu_nc_attr_get(fsl_dibu_nc_attr_Insert),
                         "+%.*s\n", (int)pLine->n, pLine->z);
    }
  }
  return rc;
}
static int fdb__ncu_deletion(fsl_dibu * const b, fsl_dline const * pLine){
  DSTATE(sst);
  ++b->lnLHS;
  if(1==b->passNumber){
    ++sst->displayLines;
    fdb__dico_update_maxlen(sst, FDBCols_TEXT1, pLine->z, pLine->n);
    return 0;
  }
  int rc = (1==b->pimplFlags) ? 0 : fdb__ncu_flush_ins(b);
  if(0==rc){
    b->pimplFlags = 1;
    rc = fdb__ncu_lineno(b, b->lnLHS, 0);
  }
  return rc ? rc
    : fdb__ncu_outf(sst, fsl_dibu_nc_attr_get(fsl_dibu_nc_attr_Delete),
                     "-%.*s\n", (int)pLine->n, pLine->z);
}
static int fdb__ncu_replacement(fsl_dibu * const b,
                                fsl_dline const * lineLhs,
                                fsl_dline const * lineRhs) {
  int rc = b->deletion(b, lineLhs);
  if(0==rc) rc = b->insertion(b, lineRhs);
  return rc;
}

static int fdb__ncu_edit(fsl_dibu * const b,
                           fsl_dline const * lineLHS,
                           fsl_dline const * lineRHS){
  int rc = b->deletion(b, lineLHS);
  if(0==rc) rc = b->insertion(b, lineRHS);
  return rc;
}

static int fdb__ncu_finish(fsl_dibu * const b){
  int rc = 0;
  DSTATE(sst);
  if(1==b->passNumber){
    if(sst->lineCount[0] < b->lnLHS) sst->lineCount[0] = b->lnLHS;
    if(sst->lineCount[1] < b->lnRHS) sst->lineCount[1] = b->lnRHS;
    // Calculate line number column widths...
    if(FSL_DIFF2_LINE_NUMBERS & b->opt->diffFlags){
      uint32_t x = 1, li = sst->lineCount[0];
      while( li >= 10 ){ li /= 10; ++x; }
      sst->maxWidths[FDBCols_NUM1] = x;
      x = 1;
      li = sst->lineCount[1];
      while( li >= 10 ){ li /= 10; ++x; }
      sst->maxWidths[FDBCols_NUM2] = x;
    }else{
      sst->maxWidths[FDBCols_NUM1]
        = sst->maxWidths[FDBCols_NUM2] = 4/*fudge value :/ */;
    }
    sst->displayWidth = 5/*semi-magic: avoids RHS truncation*/;
    for( int i = 0; i<FDBCols_count; ++i ){
      sst->displayWidth += sst->maxWidths[i];
    }
    if(sst->pad){
      int w, h, maxW;
      getmaxyx(sst->pad, h, w);
      if(h){/*unused but we need it for the getmaxyx() call*/}
      maxW = (int)sst->displayWidth>w
        ? (int)sst->displayWidth
        : w;
      if(wresize(sst->pad, sst->displayLines, maxW)){
        rc = FSL_RC_OOM;
      }
      sst->displayWidth = (unsigned)maxW;
    }else{
      ++sst->displayLines/* "END" marker */;
      sst->pad = newpad((int)sst->displayLines, (int)sst->displayWidth);
      if(!sst->pad) rc = FSL_RC_OOM;
      else{
        wbkgd(sst->pad, fsl_dibu_nc_attr_get(fsl_dibu_nc_attr_Window));
      }
    }
    if(FSL_RC_OOM==rc){
      rc = fcli_err_set(rc, "Could not (re)allocate PAD for "
                        "diff display.");
    }else{
      fsl_buffer_reserve(&sst->buf, sst->displayWidth*2);
    }
    return rc;
  }
  rc = fdb__ncu_flush_ins(b);
  fsl_buffer_reuse(&sst->bufIns);
  fsl_buffer_reuse(&sst->buf);
  return rc;
}

static int fdb__ncu_finally(fsl_dibu * const b){
  int rc;
  DSTATE(sst);
  b->fileCount = 0;
  rc = fdb__ncu_outf(sst,
                     fsl_dibu_nc_attr_get(fsl_dibu_nc_attr_EOF),
                     "(END)%.*c\n",
                     (int)sst->displayWidth-7, '=');
  if(0==rc){
    fnc_screen_init()/*needed for the no-diff-generated case!*/;
    fsl_dibu_ncu_basic_loop(sst, stdscr);
    fnc_screen_shutdown();
  }
  return rc;
}

static void fsl__diff_builder_ncu_finalizer(fsl_dibu * const b){
  DSTATE(sst);
  fsl_buffer_clear(&sst->buf);
  fsl_buffer_clear(&sst->bufIns);
  if(sst->pad){
    delwin(sst->pad);
  }
  if(sst->initedScreen){
    fnc_screen_shutdown();
  }
  fsl_free(sst->indexes.lineNumbers);
  *sst = fsl_dibu_ncu_empty;
  *b = fsl_dibu_empty;
  fsl_free(b);
}

bool fsl_dibu_is_ncu(fsl_dibu const * const b){
  return b && b->typeID==&fsl_dibu_ncu_empty;
}

fsl_dibu_ncu * fsl_dibu_ncu_pimpl(fsl_dibu * const b){
  return b->typeID==&fsl_dibu_ncu_empty
    ? (fsl_dibu_ncu*)b->pimpl : NULL;
}

fsl_dibu * fsl_dibu_ncu_alloc(void){
  fsl_dibu * rc =
    fsl_dibu_alloc((fsl_size_t)sizeof(fsl_dibu_ncu));
  if(rc){
    rc->typeID = &fsl_dibu_ncu_empty;
    rc->chunkHeader = fdb__ncu_chunkHeader;
    rc->start = fdb__ncu_start;
    rc->skip = fdb__ncu_skip;
    rc->common = fdb__ncu_common;
    rc->insertion = fdb__ncu_insertion;
    rc->deletion = fdb__ncu_deletion;
    rc->replacement = fdb__ncu_replacement;
    rc->edit = fdb__ncu_edit;
    rc->finish = fdb__ncu_finish;
    rc->finally = fdb__ncu_finally;
    rc->finalize = fsl__diff_builder_ncu_finalizer;
    rc->twoPass = true;
    assert(NULL!=rc->pimpl);
    fsl_dibu_ncu * const sst = (fsl_dibu_ncu*)rc->pimpl;
    *sst = fsl_dibu_ncu_empty;
    assert(0==rc->pimplFlags);
    assert(0==rc->lnLHS);
    assert(0==rc->lnRHS);
    assert(NULL==rc->opt);
  }
  return rc;
}

void fsl_dibu_ncu_basic_loop(fsl_dibu_ncu * const nc,
                             WINDOW * const w){
  int pTop = 0, pLeft = 0;
  int wW/*window width*/, wH/*window height*/,
    wTop/*window Y*/, wLeft/*window X*/,
    pW/*pad width*/, pH/*pad height*/,
    vH/*viewport height*/, vW/*viewport width*/,
    ch/*wgetch() result*/;
  uint32_t index = 0;
  int const hScroll = 2/*cols to shift for horizontal scroll*/;
  int showHeader = 1;
  wclear(w);
  wrefresh(w);
  noecho();
  cbreak();
  keypad(w,TRUE);
  if(!nc->indexes.used){
    unsigned int const attr =
      fsl_dibu_nc_attr_get(fsl_dibu_nc_attr_Help);
    wattron(w, attr);
    mvwprintw(w,1,1, "No diff output generated. Tap a key.");
    wattroff(w, attr);
    wrefresh(w);
    wgetch(w);
    return;
  }
  assert(nc->indexes.used>0);
  getyx(w,wTop,wLeft);
  getmaxyx(nc->pad,pH,pW);
  WINDOW * const wHelp = 1 ? nc->pad : w;
  while(1){
    getmaxyx(w,wH,wW)/*can change dynamically*/;
    //touchwin(w);
    bool const scrollV = wH<pH;
    bool const scrollH = wW<pW;
    vH = wH - scrollV;
    vW  = wW - wLeft - scrollH;
    if(vH>=LINES){
      vH = LINES - scrollH;
    }
    if(vW>=COLS-1){
      vW = COLS-wLeft-scrollH;
    }
    if(pTop + wH - scrollH >= pH) pTop = pH - wH + scrollH;
    if(pTop<0) pTop=0;
    if(pLeft<0) pLeft=0;
    else if(pW - pLeft < vW) pLeft = pW - vW;
    switch(showHeader){
      case 1:{
        unsigned int attr = fsl_dibu_nc_attr_get(fsl_dibu_nc_attr_Help);
        char const * zLabel =
          " scroll: arrows/pg/home/end "
          "| [q]uit "
          "| [n]ext/[p]rev file ";
        wattron(wHelp, attr);
        mvwprintw(wHelp,1, 2, "%s", zLabel);
        wattroff(wHelp, attr);
        ++showHeader;
        break;
      }
      case 2:
        if(nc->pad!=wHelp){
          wmove(wHelp,1,0);
          wclrtoeol(wHelp);
        }
        ++showHeader;
        break;
    }
    prefresh(nc->pad, pTop, pLeft, wTop, wLeft, vH,vW);
    if(scrollV) fnc_scrollbar_v(w, 0, wW-1, wH-1+!scrollH,
                                pH - vH, pTop);
    if(scrollH) fnc_scrollbar_h(w, wH-1, 0, wW-1+!scrollV,
                                pW - vW, pLeft);
    doupdate();
    ch = wgetch(w);
    if('q'==ch || 0==ch) break;
#define KEY_ctrl(x) ((x) & 0x1f)
    switch(ch){
      case 'n':
        if(++index==nc->indexes.used) index = 0;
        pTop = nc->indexes.lineNumbers[index];
        break;
      case 'p':
        if(0==index) index = nc->indexes.used-1;
        else --index;
        pTop = nc->indexes.lineNumbers[index];
        break;
      case KEY_HOME: pTop = 0; break;
      case KEY_END: pTop = pH - vH; break;
      case KEY_DOWN: ++pTop; break;
      case KEY_UP: --pTop; break;
      case KEY_NPAGE: pTop += vH; break;
      case KEY_PPAGE: pTop -= vH; break;
      case KEY_LEFT: pLeft -= hScroll; break;
      case KEY_RIGHT: pLeft += hScroll; break;
      case KEY_ctrl('l'):
        wclear(w);
        wrefresh(w);
        break;
    }
  }
#undef KEY_ctrl
}

#undef MARKER
