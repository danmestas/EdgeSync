/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */ 
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2022 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2013-2022 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/

/************************************************************************
  This file houses Fossil's diff-generation routines (as opposed to
  the delta-generation). This code is a straight port of those
  algorithms from the Fossil SCM project, initially implemented by
  D. Richard Hipp, ported and the license re-assigned to this project
  with this consent.
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


const fsl_dline fsl_dline_empty = fsl_dline_empty_m;
const fsl_dline_change fsl_dline_change_empty = fsl_dline_change_empty_m;
const fsl__diff_cx fsl__diff_cx_empty = fsl__diff_cx_empty_m;

void fsl__diff_cx_clean(fsl__diff_cx * const cx){
  fsl_free(cx->aFrom);
  fsl_free(cx->aTo);
  fsl_free(cx->aEdit);
  cx->aFrom = cx->aTo = NULL;
  cx->aEdit = NULL;
  *cx = fsl__diff_cx_empty;
}

/* Fast isspace for use by diff */
static const char diffIsSpace[] = {
  0, 0, 0, 0, 0, 0, 0, 0,   1, 1, 1, 0, 1, 1, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
  1, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,

  0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0, 0,
};
#define diff_isspace(X)  (diffIsSpace[(unsigned char)(X)])


/**
   Length of a dline.
*/
#define LENGTH(X) ((X)->n)


/**
   Minimum of two values
*/
static int minInt(int a, int b){ return a<b ? a : b; }

/** @internal

   Compute the optimal longest common subsequence (LCS) using an
   exhaustive search. This version of the LCS is only used for
   shorter input strings since runtime is O(N*N) where N is the
   input string length.
*/
static void fsl__diff_optimal_lcs(
  fsl__diff_cx * const p,     /* Two files being compared */
  int iS1, int iE1,          /* Range of lines in p->aFrom[] */
  int iS2, int iE2,          /* Range of lines in p->aTo[] */
  int *piSX, int *piEX,      /* Write p->aFrom[] common segment here */
  int *piSY, int *piEY       /* Write p->aTo[] common segment here */
){
  int mxLength = 0;          /* Length of longest common subsequence */
  int i, j;                  /* Loop counters */
  int k;                     /* Length of a candidate subsequence */
  int iSXb = iS1;            /* Best match so far */
  int iSYb = iS2;            /* Best match so far */

  for(i=iS1; i<iE1-mxLength; i++){
    for(j=iS2; j<iE2-mxLength; j++){
      if( p->cmpLine(&p->aFrom[i], &p->aTo[j]) ) continue;
      if( mxLength && p->cmpLine(&p->aFrom[i+mxLength], &p->aTo[j+mxLength]) ){
        continue;
      }
      k = 1;
      while( i+k<iE1 && j+k<iE2 && p->cmpLine(&p->aFrom[i+k],&p->aTo[j+k])==0 ){
        k++;
      }
      if( k>mxLength ){
        iSXb = i;
        iSYb = j;
        mxLength = k;
      }
    }
  }
  *piSX = iSXb;
  *piEX = iSXb + mxLength;
  *piSY = iSYb;
  *piEY = iSYb + mxLength;
}

/**
    Compare two blocks of text on lines iS1 through iE1-1 of the aFrom[]
    file and lines iS2 through iE2-1 of the aTo[] file.  Locate a sequence
    of lines in these two blocks that are exactly the same.  Return
    the bounds of the matching sequence.
   
    If there are two or more possible answers of the same length, the
    returned sequence should be the one closest to the center of the
    input range.
   
    Ideally, the common sequence should be the longest possible common
    sequence.  However, an exact computation of LCS is O(N*N) which is
    way too slow for larger files.  So this routine uses an O(N)
    heuristic approximation based on hashing that usually works about
    as well.  But if the O(N) algorithm doesn't get a good solution
    and N is not too large, we fall back to an exact solution by
    calling fsl__diff_optimal_lcs().
*/
static void fsl__diff_lcs(
  fsl__diff_cx * const p,     /* Two files being compared */
  int iS1, int iE1,          /* Range of lines in p->aFrom[] */
  int iS2, int iE2,          /* Range of lines in p->aTo[] */
  int *piSX, int *piEX,      /* Write p->aFrom[] common segment here */
  int *piSY, int *piEY       /* Write p->aTo[] common segment here */
){
  int i, j, k;               /* Loop counters */
  int n;                     /* Loop limit */
  fsl_dline *pA, *pB;            /* Pointers to lines */
  int iSX, iSY, iEX, iEY;    /* Current match */
  int skew = 0;              /* How lopsided is the match */
  int dist = 0;              /* Distance of match from center */
  int mid;                   /* Center of the chng */
  int iSXb, iSYb, iEXb, iEYb;   /* Best match so far */
  int iSXp, iSYp, iEXp, iEYp;   /* Previous match */
  sqlite3_int64 bestScore;      /* Best score so far */
  sqlite3_int64 score;          /* Score for current candidate LCS */
  int span;                     /* combined width of the input sequences */
  int cutoff = 4;            /* Max hash chain entries to follow */
  int nextCutoff = -1;       /* Value of cutoff for next iteration */

  span = (iE1 - iS1) + (iE2 - iS2);
  bestScore = -9223300000*(sqlite3_int64)1000000000;
  score = 0;
  iSXb = iSXp = iS1;
  iEXb = iEXp = iS1;
  iSYb = iSYp = iS2;
  iEYb = iEYp = iS2;
  mid = (iE1 + iS1)/2;
  do{
    nextCutoff = 0;
    for(i=iS1; i<iE1; i++){
      int limit = 0;
      j = p->aTo[p->aFrom[i].h % p->nTo].iHash;
      while( j>0
        && (j-1<iS2 || j>=iE2 || p->cmpLine(&p->aFrom[i], &p->aTo[j-1]))
      ){
        if( limit++ > cutoff ){
          j = 0;
          nextCutoff = cutoff*4;
          break;
        }
        j = p->aTo[j-1].iNext;
      }
      if( j==0 ) continue;
      assert( i>=iSXb && i>=iSXp );
      if( i<iEXb && j>=iSYb && j<iEYb ) continue;
      if( i<iEXp && j>=iSYp && j<iEYp ) continue;
      iSX = i;
      iSY = j-1;
      pA = &p->aFrom[iSX-1];
      pB = &p->aTo[iSY-1];
      n = minInt(iSX-iS1, iSY-iS2);
      for(k=0; k<n && p->cmpLine(pA,pB)==0; k++, pA--, pB--){}
      iSX -= k;
      iSY -= k;
      iEX = i+1;
      iEY = j;
      pA = &p->aFrom[iEX];
      pB = &p->aTo[iEY];
      n = minInt(iE1-iEX, iE2-iEY);
      for(k=0; k<n && p->cmpLine(pA,pB)==0; k++, pA++, pB++){}
      iEX += k;
      iEY += k;
      skew = (iSX-iS1) - (iSY-iS2);
      if( skew<0 ) skew = -skew;
      dist = (iSX+iEX)/2 - mid;
      if( dist<0 ) dist = -dist;
      score = (iEX - iSX)*(sqlite3_int64)span - (skew + dist);
      if( score>bestScore ){
        bestScore = score;
        iSXb = iSX;
        iSYb = iSY;
        iEXb = iEX;
        iEYb = iEY;
      }else if( iEX>iEXp ){
        iSXp = iSX;
        iSYp = iSY;
        iEXp = iEX;
        iEYp = iEY;
      }
    }
  }while( iSXb==iEXb && nextCutoff && (cutoff=nextCutoff)<=64 );
  if( iSXb==iEXb && (sqlite3_int64)(iE1-iS1)*(iE2-iS2)<2500 ){
    fsl__diff_optimal_lcs(p, iS1, iE1, iS2, iE2, piSX, piEX, piSY, piEY);
  }else{
    *piSX = iSXb;
    *piSY = iSYb;
    *piEX = iEXb;
    *piEY = iEYb;
  }
}

void fsl__dump_triples(fsl__diff_cx const * const p,
                       char const * zFile, int ln ){
  // Compare this with (fossil xdiff --raw) on the same inputs
  fprintf(stderr,"%s:%d: Compare this with (fossil xdiff --raw) on the same inputs:\n",
          zFile, ln);
  for(int i = 0; p->aEdit[i] || p->aEdit[i+1] || p->aEdit[i+2]; i+=3){
    printf(" copy %6d  delete %6d  insert %6d\n",
           p->aEdit[i], p->aEdit[i+1], p->aEdit[i+2]);
  }
}

/** @internal
    Expand the size of p->aEdit array to hold at least nEdit elements.
 */
static int fsl__diff_expand_edit(fsl__diff_cx * const p, int nEdit){
  void * re = fsl_realloc(p->aEdit, nEdit*sizeof(int));
  if(!re) return FSL_RC_OOM;
  else{
    p->aEdit = (int*)re;
    p->nEditAlloc = nEdit;
    return 0;
  }
}

/**
    Append a new COPY/DELETE/INSERT triple.
   
    Returns 0 on success, FSL_RC_OOM on OOM.
 */
static int appendTriple(fsl__diff_cx *p, int nCopy, int nDel, int nIns){
  /* printf("APPEND %d/%d/%d\n", nCopy, nDel, nIns); */
  if( p->nEdit>=3 ){
    if( p->aEdit[p->nEdit-1]==0 ){
      if( p->aEdit[p->nEdit-2]==0 ){
        p->aEdit[p->nEdit-3] += nCopy;
        p->aEdit[p->nEdit-2] += nDel;
        p->aEdit[p->nEdit-1] += nIns;
        return 0;
      }
      if( nCopy==0 ){
        p->aEdit[p->nEdit-2] += nDel;
        p->aEdit[p->nEdit-1] += nIns;
        return 0;
      }
    }
    if( nCopy==0 && nDel==0 ){
      p->aEdit[p->nEdit-1] += nIns;
      return 0;
    }
  }
  if( p->nEdit+3>p->nEditAlloc ){
    int const rc = fsl__diff_expand_edit(p, p->nEdit*2 + 15);
    if(rc) return rc;
    else if( p->aEdit==0 ) return 0;
  }
  p->aEdit[p->nEdit++] = nCopy;
  p->aEdit[p->nEdit++] = nDel;
  p->aEdit[p->nEdit++] = nIns;
  return 0;
}

/*
** A common subsequene between p->aFrom and p->aTo has been found.
** This routine tries to judge if the subsequence really is a valid
** match or rather is just an artifact of an indentation change.
**
** Return non-zero if the subsequence is valid.  Return zero if the
** subsequence seems likely to be an editing artifact and should be
** ignored.
**
** This routine is a heuristic optimization intended to give more
** intuitive diff results following an indentation change it code that
** is formatted similarly to C/C++, Javascript, Go, TCL, and similar
** languages that use {...} for nesting.  A correct diff is computed
** even if this routine always returns true (non-zero).  But sometimes
** a more intuitive diff can result if this routine returns false.
**
** The subsequences consists of the rows iSX through iEX-1 (inclusive)
** in p->aFrom[].  The total sequences is iS1 through iE1-1 (inclusive)
** of p->aFrom[].
**
** Example where this heuristic is useful, see the diff at
** https://www.sqlite.org/src/fdiff?v1=0e79dd15cbdb4f48&v2=33955a6fd874dd97
**
** See also discussion at https://fossil-scm.org/forum/forumpost/9ba3284295
**
** ALGORITHM (subject to change and refinement):
**
**    1.  If the subsequence is larger than 1/7th of the original span,
**        then consider it valid.  --> return 1
**
**    2.  If no lines of the subsequence contains more than one
**        non-whitespace character,  --> return 0
**
**    3.  If any line of the subsequence contains more than one non-whitespace
**        character and is unique across the entire sequence after ignoring
**        leading and trailing whitespace   --> return 1
**
**    4.  Otherwise, it is potentially an artifact of an indentation
**        change. --> return 0
*/
static bool likelyNotIndentChngArtifact(
  fsl__diff_cx const * const p,     /* The complete diff context */
  int iS1,         /* Start of the main segment */
  int iSX,         /* Start of the subsequence */
  int iEX,         /* First row past the end of the subsequence */
  int iE1          /* First row past the end of the main segment */
){
  int i, j, n;

  /* Rule (1) */
  if( (iEX-iSX)*7 >= (iE1-iS1) ) return 1;

  /* Compute fsl_dline.indent and fsl_dline.nw for all lines of the subsequence.
  ** If no lines contain more than one non-whitespace character return
  ** 0 because the subsequence could be due to an indentation change.
  ** Rule (2).
  */
  n = 0;
  for(i=iSX; i<iEX; i++){
    fsl_dline *pA = &p->aFrom[i];
    if( pA->nw==0 && pA->n ){
      const char *zA = pA->z;
      const int nn = pA->n;
      int ii, jj;
      for(ii=0; ii<nn && diff_isspace(zA[ii]); ii++){}
      pA->indent = ii;
      for(jj=nn-1; jj>ii && diff_isspace(zA[jj]); jj--){}
      pA->nw = jj - ii + 1;
    }
    if( pA->nw>1 ) n++;
  }
  if( n==0 ) return 0;

  /* Compute fsl_dline.indent and fsl_dline.nw for the entire sequence */
  for(i=iS1; i<iE1; i++){
    fsl_dline *pA;
    if( i==iSX ){
      i = iEX;
      if( i>=iE1 ) break;
    }
    pA = &p->aFrom[i];
    if( pA->nw==0 && pA->n ){
      const char *zA = pA->z;
      const int nn = pA->n;
      int ii, jj;
      for(ii=0; ii<nn && diff_isspace(zA[ii]); ii++){}
      pA->indent = ii;
      for(jj=nn-1; jj>ii && diff_isspace(zA[jj]); jj--){}
      pA->nw = jj - ii + 1;
    }
  }

  /* Check to see if any subsequence line that has more than one
  ** non-whitespace character is unique across the entire sequence.
  ** Rule (3)
  */
  for(i=iSX; i<iEX; i++){
    const char *z = p->aFrom[i].z + p->aFrom[i].indent;
    const int nw = p->aFrom[i].nw;
    if( nw<=1 ) continue;
    for(j=iS1; j<iSX; j++){
      if( p->aFrom[j].nw!=nw ) continue;
      if( memcmp(p->aFrom[j].z+p->aFrom[j].indent,z,nw)==0 ) break;
    }
    if( j<iSX ) continue;
    for(j=iEX; j<iE1; j++){
      if( p->aFrom[j].nw!=nw ) continue;
      if( memcmp(p->aFrom[j].z+p->aFrom[j].indent,z,nw)==0 ) break;
    }
    if( j>=iE1 ) break;
  }
  return i<iEX;
}

/**
    Do a single step in the difference.  Compute a sequence of
    copy/delete/insert steps that will convert lines iS1 through iE1-1
    of the input into lines iS2 through iE2-1 of the output and write
    that sequence into the difference context.
   
    The algorithm is to find a block of common text near the middle of
    the two segments being diffed.  Then recursively compute
    differences on the blocks before and after that common segment.
    Special cases apply if either input segment is empty or if the two
    segments have no text in common.
 */
static int diff_step(fsl__diff_cx *p, int iS1, int iE1, int iS2, int iE2){
  int iSX, iEX, iSY, iEY;
  int rc = 0;
  if( iE1<=iS1 ){
    /* The first segment is empty */
    if( iE2>iS2 ){
      rc = appendTriple(p, 0, 0, iE2-iS2);
    }
    return rc;
  }
  if( iE2<=iS2 ){
    /* The second segment is empty */
    return appendTriple(p, 0, iE1-iS1, 0);
  }

  /* Find the longest matching segment between the two sequences */
  fsl__diff_lcs(p, iS1, iE1, iS2, iE2, &iSX, &iEX, &iSY, &iEY);
  if( iEX>iSX+5
      || (iEX>iSX && likelyNotIndentChngArtifact(p,iS1,iSX,iEX,iE1) )){
    /* A common segment has been found.
       Recursively diff either side of the matching segment */
    rc = diff_step(p, iS1, iSX, iS2, iSY);
    if(!rc){
      if(iEX>iSX){
        rc = appendTriple(p, iEX - iSX, 0, 0);
      }
      if(!rc) rc = diff_step(p, iEX, iE1, iEY, iE2);
    }
  }else{
    /* The two segments have nothing in common.  Delete the first then
       insert the second. */
    rc = appendTriple(p, 0, iE1-iS1, iE2-iS2);
  }
  return rc;
}

int fsl__diff_all(fsl__diff_cx * const p){
  int mnE, iS, iE1, iE2;
  int rc = 0;
  /* Carve off the common header and footer */
  iE1 = p->nFrom;
  iE2 = p->nTo;
  while( iE1>0 && iE2>0 && p->cmpLine(&p->aFrom[iE1-1], &p->aTo[iE2-1])==0 ){
    iE1--;
    iE2--;
  }
  mnE = iE1<iE2 ? iE1 : iE2;
  for(iS=0; iS<mnE && p->cmpLine(&p->aFrom[iS],&p->aTo[iS])==0; iS++){}

  /* do the difference */
  if( iS>0 ){
    rc = appendTriple(p, iS, 0, 0);
    if(rc) return rc;
  }
  rc = diff_step(p, iS, iE1, iS, iE2);
  //fsl__dump_triples(p, __FILE__, __LINE__);
  if(rc) return rc;
  else if( iE1<p->nFrom ){
    rc = appendTriple(p, p->nFrom - iE1, 0, 0);
    if(rc) return rc;
  }
  /* Terminate the COPY/DELETE/INSERT triples with three zeros */
  rc = fsl__diff_expand_edit(p, p->nEdit+3);
  if(0==rc){
    if(p->aEdit ){
      p->aEdit[p->nEdit++] = 0;
      p->aEdit[p->nEdit++] = 0;
      p->aEdit[p->nEdit++] = 0;
      //fsl__dump_triples(p, __FILE__, __LINE__);
    }
  }
  return rc;
}

void fsl__diff_optimize(fsl__diff_cx * const p){
  int r;       /* Index of current triple */
  int lnFrom;  /* Line number in p->aFrom */
  int lnTo;    /* Line number in p->aTo */
  int cpy, del, ins;

  //fsl__dump_triples(p, __FILE__, __LINE__);
  lnFrom = lnTo = 0;
  for(r=0; r<p->nEdit; r += 3){
    cpy = p->aEdit[r];
    del = p->aEdit[r+1];
    ins = p->aEdit[r+2];
    lnFrom += cpy;
    lnTo += cpy;

    /* Shift insertions toward the beginning of the file */
    while( cpy>0 && del==0 && ins>0 ){
      fsl_dline *pTop = &p->aFrom[lnFrom-1];  /* Line before start of insert */
      fsl_dline *pBtm = &p->aTo[lnTo+ins-1];  /* Last line inserted */
      if( p->cmpLine(pTop, pBtm) ) break;
      if( LENGTH(pTop+1)+LENGTH(pBtm)<=LENGTH(pTop)+LENGTH(pBtm-1) ) break;
      lnFrom--;
      lnTo--;
      p->aEdit[r]--;
      p->aEdit[r+3]++;
      cpy--;
    }

    /* Shift insertions toward the end of the file */
    while( r+3<p->nEdit && p->aEdit[r+3]>0 && del==0 && ins>0 ){
      fsl_dline *pTop = &p->aTo[lnTo];       /* First line inserted */
      fsl_dline *pBtm = &p->aTo[lnTo+ins];   /* First line past end of insert */
      if( p->cmpLine(pTop, pBtm) ) break;
      if( LENGTH(pTop)+LENGTH(pBtm-1)<=LENGTH(pTop+1)+LENGTH(pBtm) ) break;
      lnFrom++;
      lnTo++;
      p->aEdit[r]++;
      p->aEdit[r+3]--;
      cpy++;
    }

    /* Shift deletions toward the beginning of the file */
    while( cpy>0 && del>0 && ins==0 ){
      fsl_dline *pTop = &p->aFrom[lnFrom-1];     /* Line before start of delete */
      fsl_dline *pBtm = &p->aFrom[lnFrom+del-1]; /* Last line deleted */
      if( p->cmpLine(pTop, pBtm) ) break;
      if( LENGTH(pTop+1)+LENGTH(pBtm)<=LENGTH(pTop)+LENGTH(pBtm-1) ) break;
      lnFrom--;
      lnTo--;
      p->aEdit[r]--;
      p->aEdit[r+3]++;
      cpy--;
    }

    /* Shift deletions toward the end of the file */
    while( r+3<p->nEdit && p->aEdit[r+3]>0 && del>0 && ins==0 ){
      fsl_dline *pTop = &p->aFrom[lnFrom];     /* First line deleted */
      fsl_dline *pBtm = &p->aFrom[lnFrom+del]; /* First line past end of delete */
      if( p->cmpLine(pTop, pBtm) ) break;
      if( LENGTH(pTop)+LENGTH(pBtm-1)<=LENGTH(pTop)+LENGTH(pBtm) ) break;
      lnFrom++;
      lnTo++;
      p->aEdit[r]++;
      p->aEdit[r+3]--;
      cpy++;
    }

    lnFrom += del;
    lnTo += ins;
  }
  //fsl__dump_triples(p, __FILE__, __LINE__);
}


/**
   Counts the number of lines in the first n bytes of the given 
   string. If n<0 then fsl_strlen() is used to count it. 

   It includes the last line in the count even if it lacks the \n
   terminator. If an empty string is passed in, the number of lines
   is zero.

   For the purposes of this function, a string is considered empty if
   it contains no characters OR contains only NUL characters.

   If the input appears to be plain text it returns true and, if nOut
   is not NULL, writes the number of lines there. If the input appears
   to be binary, returns false and does not modify nOut.
*/
static bool fsl__count_lines(const char *z, fsl_int_t n, uint32_t * nOut ){
  uint32_t nLine;
  const char *zNL, *z2;
  if(n<0) n = (fsl_int_t)fsl_strlen(z);
  for(nLine=0, z2=z; (zNL = strchr(z2,'\n'))!=0; z2=zNL+1, nLine++){}
  if( z2[0]!='\0' ){
    nLine++;
    do{ z2++; }while( z2[0]!='\0' );
  }
  if( n!=(fsl_int_t)(z2-z) ) return false;
  if( nOut ) *nOut = nLine;
  return true;
}

int fsl_break_into_dlines(const char *z, fsl_int_t n,
                          uint32_t *pnLine,
                          fsl_dline **pOut, uint64_t diffFlags){
  uint32_t nLine, i, k, nn, s, x;
  uint64_t h, h2;
  fsl_dline *a = 0;
  const char *zNL;

  if(!z || !n){
    *pnLine = 0;
    *pOut = NULL;
    return 0;
  }
  if( !fsl__count_lines(z, n, &nLine) ){
    return FSL_RC_DIFF_BINARY;
  }
  assert( nLine>0 || z[0]=='\0' );
  if(nLine>0){
    a = fsl_malloc( sizeof(a[0])*nLine );
    if(!a) return FSL_RC_OOM;
    memset(a, 0, sizeof(a[0])*nLine);
  }else{
    *pnLine = 0;
    *pOut = a;
    return 0;
  }
  assert( a );
  i = 0;
  do{
    zNL = strchr(z,'\n');
    if( zNL==0 ) zNL = z+n;
    nn = (uint32_t)(zNL - z);
    if( nn>FSL__LINE_LENGTH_MASK ){
      fsl_free(a);
      *pOut = 0;
      *pnLine = 0;
      return FSL_RC_DIFF_BINARY;
    }
    a[i].z = z;
    k = nn;
    if( diffFlags & FSL_DIFF2_STRIP_EOLCR ){
      if( k>0 && z[k-1]=='\r' ){ k--; }
    }
    a[i].n = k;
    if( diffFlags & FSL_DIFF2_IGNORE_EOLWS ){
      while( k>0 && diff_isspace(z[k-1]) ){ k--; }
    }
    if( (diffFlags & FSL_DIFF2_IGNORE_ALLWS)
        ==FSL_DIFF2_IGNORE_ALLWS ){
      uint32_t numws = 0;
      for(s=0; s<k && z[s]<=' '; s++){}
      a[i].indent = s;
      a[i].nw = k - s;
      for(h=0, x=s; x<k; ++x){
        char c = z[x];
        if( diff_isspace(c) ){
          ++numws;
        }else{
          h = (h^c)*9000000000000000041LL;
        }
      }
      k -= numws;
    }else{
      uint32_t k2 = k & ~0x7;
      uint64_t m;
      for(h=x=s=0; x<k2; x += 8){
        memcpy(&m, z+x, 8);
        h = (h^m)*9000000000000000041LL;
      }
      m = 0;
      memcpy(&m, z+x, k-k2);
      h ^= m;
    }
    a[i].h = h = ((h%281474976710597LL)<<FSL__LINE_LENGTH_MASK_SZ) | (k-s);
    h2 = h % nLine;
    a[i].iNext = a[h2].iHash;
    a[h2].iHash = i+1;
    z += nn+1; n -= nn+1;
    i++;
  }while( zNL[0]!='\0' && zNL[1]!='\0' );
  assert( i==nLine );

  *pnLine = nLine;
  *pOut = a;
  return 0;
}

int fsl_dline_cmp(const fsl_dline * const pA,
                  const fsl_dline * const pB){
  if( pA->h!=pB->h ) return 1;
  return memcmp(pA->z,pB->z, pA->h&FSL__LINE_LENGTH_MASK);
}

int fsl_dline_cmp_ignore_ws(const fsl_dline * const pA,
                            const fsl_dline * const pB){
  if( pA->h==pB->h ){
    unsigned short a, b;
    if( memcmp(pA->z, pB->z, pA->h&FSL__LINE_LENGTH_MASK)==0 ) return 0;
    a = pA->indent;
    b = pB->indent;
    while( a<pA->n || b<pB->n ){
      if( a<pA->n && b<pB->n && pA->z[a++] != pB->z[b++] ) return 1;
      while( a<pA->n && diff_isspace(pA->z[a])) ++a;
      while( b<pB->n && diff_isspace(pB->z[b])) ++b;
    }
    return pA->n-a != pB->n-b;
  }
  return 1;
}

/*
** The two text segments zLeft and zRight are known to be different on
** both ends, but they might have  a common segment in the middle.  If
** they do not have a common segment, return 0.  If they do have a large
** common segment, return non-0 and before doing so set:
**
**   aLCS[0] = start of the common segment in zLeft
**   aLCS[1] = end of the common segment in zLeft
**   aLCS[2] = start of the common segment in zLeft
**   aLCS[3] = end of the common segment in zLeft
**
** This computation is for display purposes only and does not have to be
** optimal or exact.
*/
static int textLCS2(
  const char *zLeft,  uint32_t nA, /* String on the left */
  const char *zRight, uint32_t nB, /* String on the right */
  uint32_t *aLCS                   /* Identify bounds of LCS here */
){
  const unsigned char *zA = (const unsigned char*)zLeft;    /* left string */
  const unsigned char *zB = (const unsigned char*)zRight;   /* right string */
  uint32_t i, j, k;               /* Loop counters */
  uint32_t lenBest = 0;           /* Match length to beat */

  for(i=0; i<nA-lenBest; i++){
    unsigned char cA = zA[i];
    if( (cA&0xc0)==0x80 ) continue;
    for(j=0; j<nB-lenBest; j++ ){
      if( zB[j]==cA ){
        for(k=1; j+k<nB && i+k<nA && zB[j+k]==zA[i+k]; k++){}
        while( (zB[j+k]&0xc0)==0x80 ){ k--; }
        if( k>lenBest ){
          lenBest = k;
          aLCS[0] = i;
          aLCS[1] = i+k;
          aLCS[2] = j;
          aLCS[3] = j+k;
        }
      }
    }
  }
  return lenBest>0;
}

/*
** Find the smallest spans that are different between two text strings
** that are known to be different on both ends. Returns the number
** of entries in p->a which get populated.
*/
static unsigned short textLineChanges(
  const char *zLeft,  uint32_t nA, /* String on the left */
  const char *zRight, uint32_t nB, /* String on the right */
  fsl_dline_change * const p             /* Write results here */
){
  p->n = 1;
  p->a[0].iStart1 = 0;
  p->a[0].iLen1 = nA;
  p->a[0].iStart2 = 0;
  p->a[0].iLen2 = nB;
  p->a[0].isMin = 0;
  while( p->n<fsl_dline_change_max_spans-1 ){
    int mxi = -1;
    int mxLen = -1;
    int x, i;
    uint32_t aLCS[4] = {0,0,0,0};
    struct fsl_dline_change_span *a, *b;
    for(i=0; i<p->n; i++){
      if( p->a[i].isMin ) continue;
      x = p->a[i].iLen1;
      if( p->a[i].iLen2<x ) x = p->a[i].iLen2;
      if( x>mxLen ){
        mxLen = x;
        mxi = i;
      }
    }
    if( mxLen<6 ) break;
    x = textLCS2(zLeft + p->a[mxi].iStart1, p->a[mxi].iLen1,
                 zRight + p->a[mxi].iStart2, p->a[mxi].iLen2, aLCS);
    if( x==0 ){
      p->a[mxi].isMin = 1;
      continue;
    }
    a = p->a+mxi;
    b = a+1;
    if( mxi<p->n-1 ){
      memmove(b+1, b, sizeof(*b)*(p->n-mxi-1));
    }
    p->n++;
    b->iStart1 = a->iStart1 + aLCS[1];
    b->iLen1 = a->iLen1 - aLCS[1];
    a->iLen1 = aLCS[0];
    b->iStart2 = a->iStart2 + aLCS[3];
    b->iLen2 = a->iLen2 - aLCS[3];
    a->iLen2 = aLCS[2];
    b->isMin = 0;
  }
  return p->n;
}

/*
** Return true if the string starts with n spaces
*/
static int allSpaces(const char *z, int n){
  int i;
  for(i=0; i<n && diff_isspace(z[i]); ++i){}
  return i==n;
}

/*
** Try to improve the human-readability of the fsl_dline_change p.
**
** (1)  If the first change span shows a change of indentation, try to
**      move that indentation change to the left margin.
**
** (2)  Try to shift changes so that they begin or end with a space.
*/
static void improveReadability(
  const char *zA,  /* Left line of the change */
  const char *zB,  /* Right line of the change */
  fsl_dline_change * const p /* The fsl_dline_change to be adjusted */
){
  int j, n, len;
  if( p->n<1 ) return;

  /* (1) Attempt to move indentation changes to the left margin */
  if( p->a[0].iLen1==0
   && (len = p->a[0].iLen2)>0
   && (j = p->a[0].iStart2)>0
   && zB[0]==zB[j]
   && allSpaces(zB, j)
  ){
    for(n=1; n<len && n<j && zB[j]==zB[j+n]; n++){}
    if( n<len ){
      memmove(&p->a[1], &p->a[0], sizeof(p->a[0])*p->n);
      p->n++;
      p->a[0] = p->a[1];
      p->a[1].iStart2 += n;
      p->a[1].iLen2 -= n;
      p->a[0].iLen2 = n;
    }
    p->a[0].iStart1 = 0;
    p->a[0].iStart2 = 0;
  }else
  if( p->a[0].iLen2==0
   && (len = p->a[0].iLen1)>0
   && (j = p->a[0].iStart1)>0
   && zA[0]==zA[j]
   && allSpaces(zA, j)
  ){
    for(n=1; n<len && n<j && zA[j]==zA[j+n]; n++){}
    if( n<len ){
      memmove(&p->a[1], &p->a[0], sizeof(p->a[0])*p->n);
      p->n++;
      p->a[0] = p->a[1];
      p->a[1].iStart1 += n;
      p->a[1].iLen1 -= n;
      p->a[0].iLen1 = n;
    }
    p->a[0].iStart1 = 0;
    p->a[0].iStart2 = 0;
  }

  /* (2) Try to shift changes so that they begin or end with a
  ** space.  (TBD) */
}

void fsl_dline_change_spans(const fsl_dline *pLeft, const fsl_dline *pRight,
                            fsl_dline_change * const p){
  /* fossil(1) counterpart ==> diff.c oneLineChange() */
  int nLeft;           /* Length of left line in bytes */
  int nRight;          /* Length of right line in bytes */
  int nShort;          /* Shortest of left and right */
  int nPrefix;         /* Length of common prefix */
  int nSuffix;         /* Length of common suffix */
  int nCommon;         /* Total byte length of suffix and prefix */
  const char *zLeft;   /* Text of the left line */
  const char *zRight;  /* Text of the right line */
  int nLeftDiff;       /* nLeft - nPrefix - nSuffix */
  int nRightDiff;      /* nRight - nPrefix - nSuffix */

  nLeft = pLeft->n;
  zLeft = pLeft->z;
  nRight = pRight->n;
  zRight = pRight->z;
  nShort = nLeft<nRight ? nLeft : nRight;

  nPrefix = 0;
  while( nPrefix<nShort && zLeft[nPrefix]==zRight[nPrefix] ){
    nPrefix++;
  }
  if( nPrefix<nShort ){
    while( nPrefix>0 && (zLeft[nPrefix]&0xc0)==0x80 ) nPrefix--;
  }
  nSuffix = 0;
  if( nPrefix<nShort ){
    while( nSuffix<nShort
           && zLeft[nLeft-nSuffix-1]==zRight[nRight-nSuffix-1] ){
      nSuffix++;
    }
    if( nSuffix<nShort ){
      while( nSuffix>0 && (zLeft[nLeft-nSuffix]&0xc0)==0x80 ) nSuffix--;
    }
    if( nSuffix==nLeft || nSuffix==nRight ) nPrefix = 0;
  }
  nCommon = nPrefix + nSuffix;

  /* If the prefix and suffix overlap, that means that we are dealing with
  ** a pure insertion or deletion of text that can have multiple alignments.
  ** Try to find an alignment to begins and ends on whitespace, or on
  ** punctuation, rather than in the middle of a name or number.
  */
  if( nCommon > nShort ){
    int iBest = -1;
    int iBestVal = -1;
    int i;
    int nLong = nLeft<nRight ? nRight : nLeft;
    int nGap = nLong - nShort;
    for(i=nShort-nSuffix; i<=nPrefix; i++){
       int iVal = 0;
       char c = zLeft[i];
       if( diff_isspace(c) ){
         iVal += 5;
       }else if( !fsl_isalnum(c) ){
         iVal += 2;
       }
       c = zLeft[i+nGap-1];
       if( diff_isspace(c) ){
         iVal += 5;
       }else if( !fsl_isalnum(c) ){
         iVal += 2;
       }
       if( iVal>iBestVal ){
         iBestVal = iVal;
         iBest = i;
       }
    }
    nPrefix = iBest;
    nSuffix = nShort - nPrefix;
    nCommon = nPrefix + nSuffix;
  }

  /* A single chunk of text inserted */
  if( nCommon==nLeft ){
    p->n = 1;
    p->a[0].iStart1 = nPrefix;
    p->a[0].iLen1 = 0;
    p->a[0].iStart2 = nPrefix;
    p->a[0].iLen2 = nRight - nCommon;
    improveReadability(zLeft, zRight, p);
    return;
  }

  /* A single chunk of text deleted */
  if( nCommon==nRight ){
    p->n = 1;
    p->a[0].iStart1 = nPrefix;
    p->a[0].iLen1 = nLeft - nCommon;
    p->a[0].iStart2 = nPrefix;
    p->a[0].iLen2 = 0;
    improveReadability(zLeft, zRight, p);
    return;
  }

  /* At this point we know that there is a chunk of text that has
  ** changed between the left and the right.  Check to see if there
  ** is a large unchanged section in the middle of that changed block.
  */
  nLeftDiff = nLeft - nCommon;
  nRightDiff = nRight - nCommon;
  if( nLeftDiff >= 4
   && nRightDiff >= 4
   && textLineChanges(&zLeft[nPrefix], nLeftDiff,
                      &zRight[nPrefix], nRightDiff, p)>1
  ){
    int i;
    for(i=0; i<p->n; i++){
      p->a[i].iStart1 += nPrefix;
      p->a[i].iStart2 += nPrefix;
    }
    improveReadability(zLeft, zRight, p);
    return;
  }

  /* If all else fails, show a single big change between left and right */
  p->n = 1;
  p->a[0].iStart1 = nPrefix;
  p->a[0].iLen1 = nLeft - nCommon;
  p->a[0].iStart2 = nPrefix;
  p->a[0].iLen2 = nRight - nCommon;
  improveReadability(zLeft, zRight, p);
}


/*
** The threshold at which diffBlockAlignment transitions from the
** O(N*N) Wagner minimum-edit-distance algorithm to a less process
** O(NlogN) divide-and-conquer approach.
*/
#define DIFF_ALIGN_MX  1225

/**
   FSL_DIFF_SMALL_GAP=0 is a temporary(? as of 2022-01-04) patch for a
   cosmetic-only (but unsightly) quirk of the diff engine where it
   produces a pair of identical DELETE/INSERT lines. Richard's
   preliminary solution for it is to remove the "small gap merging,"
   but he notes (in fossil /chat) that he's not recommending this as
   "the" fix.

   PS: we colloquially know this as "the lineno diff" because it was first
   reported in a diff which resulted in:

```
-  int     lineno;
+  int     lineno;
```

   (No, there are no whitespace changes there.)

   To reiterate, though: this is not a "bug," in that it does not
   cause incorrect results when applying the resulting unfified-diff
   patches. It does, however, cause confusion for human users.


   Here are two inputs which, when diffed, expose the lineno behavior:

   #1:

```
struct fnc_diff_view_state {
  int     first_line_onscreen;
  int     last_line_onscreen;
  int     diff_flags;
  int     context;
  int     sbs;
  int     matched_line;
  int     current_line;
  int     lineno;
  size_t     ncols;
  size_t     nlines;
  off_t    *line_offsets;
  bool     eof;
  bool     colour;
  bool     showmeta;
  bool     showln;
};
```

   #2:

```
struct fnc_diff_view_state {
  int     first_line_onscreen;
  int     last_line_onscreen;
  int     diff_flags;
  int     context;
  int     sbs;
  int     matched_line;
  int     selected_line;
  int     lineno;
  int     gtl;
  size_t     ncols;
  size_t     nlines;
  off_t    *line_offsets;
  bool     eof;
  bool     colour;
  bool     showmeta;
  bool     showln;
};
```

   Result without this patch:

```
Index: X.0
==================================================================
--- X.0
+++ X.1
@@ -3,15 +3,16 @@
   int     last_line_onscreen;
   int     diff_flags;
   int     context;
   int     sbs;
   int     matched_line;
+  int     selected_line;
-  int     current_line;
-  int     lineno;
+  int     lineno;
+  int     gtl;
   size_t     ncols;
   size_t     nlines;
   off_t    *line_offsets;
   bool     eof;
   bool     colour;
   bool     showmeta;
   bool     showln;
 };
```

    And with the patch:

```
Index: X.0
==================================================================
--- X.0
+++ X.1
@@ -3,15 +3,16 @@
   int     last_line_onscreen;
   int     diff_flags;
   int     context;
   int     sbs;
   int     matched_line;
-  int     current_line;
+  int     selected_line;
   int     lineno;
+  int     gtl;
   size_t     ncols;
   size_t     nlines;
   off_t    *line_offsets;
   bool     eof;
   bool     colour;
   bool     showmeta;
   bool     showln;
 };
```

*/
#define FSL_DIFF_SMALL_GAP 0

#if FSL_DIFF_SMALL_GAP
/*
** R[] is an array of six integer, two COPY/DELETE/INSERT triples for a
** pair of adjacent differences.  Return true if the gap between these
** two differences is so small that they should be rendered as a single
** edit.
*/
static int smallGap2(const int *R, int ma, int mb){
  int m = R[3];
  ma += R[4] + m;
  mb += R[5] + m;
  if( ma*mb>DIFF_ALIGN_MX ) return 0;
  return m<=2 || m<=(R[1]+R[2]+R[4]+R[5])/8;
}
#endif

static unsigned short diff_opt_context_lines(fsl_dibu_opt const * opt){
  const unsigned short dflt = 5;
  unsigned short n = opt ? opt->contextLines : dflt;
  if( !n && (opt->diffFlags & FSL_DIFF2_CONTEXT_ZERO)==0 ){
    n = dflt;
  }
  return n;
}

/*
** Minimum of two values
*/
static int diffMin(int a, int b){ return a<b ? a : b; }

/****************************************************************************/
/*
** Return the number between 0 and 100 that is smaller the closer pA and
** pB match.  Return 0 for a perfect match.  Return 100 if pA and pB are
** completely different.
**
** The current algorithm is as follows:
**
** (1) Remove leading and trailing whitespace.
** (2) Truncate both strings to at most 250 characters
** (3) If the two strings have a common prefix, measure that prefix
** (4) Find the length of the longest common subsequence that is
**     at least 150% longer than the common prefix.
** (5) Longer common subsequences yield lower scores.
*/
static int match_dline2(fsl_dline * const pA, fsl_dline * const pB){
  const char *zA;            /* Left string */
  const char *zB;            /* right string */
  int nA;                    /* Bytes in zA[] */
  int nB;                    /* Bytes in zB[] */
  int nMin;
  int nPrefix;
  int avg;                   /* Average length of A and B */
  int i, j, k;               /* Loop counters */
  int best = 0;              /* Longest match found so far */
  int score;                 /* Final score.  0..100 */
  unsigned char c;           /* Character being examined */
  unsigned char aFirst[256]; /* aFirst[X] = index in zB[] of first char X */
  unsigned char aNext[252];  /* aNext[i] = index in zB[] of next zB[i] char */

  zA = pA->z;
  if( pA->nw==0 && pA->n ){
    for(i=0; i<pA->n && diff_isspace(zA[i]); i++){}
    pA->indent = i;
    for(j=pA->n-1; j>i && diff_isspace(zA[j]); j--){}
    pA->nw = j - i + 1;
  }
  zA += pA->indent;
  nA = pA->nw;

  zB = pB->z;
  if( pB->nw==0 && pB->n ){
    for(i=0; i<pB->n && diff_isspace(zB[i]); i++){}
    pB->indent = i;
    for(j=pB->n-1; j>i && diff_isspace(zB[j]); j--){}
    pB->nw = j - i + 1;
  }
  zB += pB->indent;
  nB = pB->nw;

  if( nA>250 ) nA = 250;
  if( nB>250 ) nB = 250;
  avg = (nA+nB)/2;
  if( avg==0 ) return 0;
  nMin = nA;
  if( nB<nMin ) nMin = nB;
  if( nMin==0 ) return 68;
  for(nPrefix=0; nPrefix<nMin && zA[nPrefix]==zB[nPrefix]; nPrefix++){}
  best = 0;
  if( nPrefix>5 && nPrefix>nMin/2 ){
    best = nPrefix*3/2;
    if( best>=avg - 2 ) best = avg - 2;
  }
  if( nA==nB && memcmp(zA, zB, nA)==0 ) return 0;
  memset(aFirst, 0xff, sizeof(aFirst));
  zA--; zB--;   /* Make both zA[] and zB[] 1-indexed */
  for(i=nB; i>0; i--){
    c = (unsigned char)zB[i];
    aNext[i] = aFirst[c];
    aFirst[c] = i;
  }
  for(i=1; i<=nA-best; i++){
    c = (unsigned char)zA[i];
    for(j=aFirst[c]; j<nB-best && memcmp(&zA[i],&zB[j],best)==0; j = aNext[j]){
      int limit = diffMin(nA-i, nB-j);
      for(k=best; k<=limit && zA[k+i]==zB[k+j]; k++){}
      if( k>best ) best = k;
    }
  }
  score = 5 + ((best>=avg) ? 0 : (avg - best)*95/avg);

#if 0
  fprintf(stderr, "A: [%.*s]\nB: [%.*s]\nbest=%d avg=%d score=%d\n",
  nA, zA+1, nB, zB+1, best, avg, score);
#endif

  /* Return the result */
  return score;
}

// Forward decl for recursion's sake.
static int diffBlockAlignment(
  fsl_dline * const aLeft, int nLeft,
  fsl_dline * const aRight, int nRight,
  fsl_dibu_opt const * pOpt,
  unsigned char **pResult,
  unsigned *pNResult
);

/*
** Make a copy of a list of nLine fsl_dline objects from one array to
** another.  Hash the new array to ignore whitespace.
*/
static void diffDLineXfer(
  fsl_dline *aTo,
  const fsl_dline *aFrom,
  int nLine
){
  int i, j, k;
  uint64_t h, h2;
  for(i=0; i<nLine; i++) aTo[i].iHash = 0;
  for(i=0; i<nLine; i++){
    const char *z = aFrom[i].z;
    int n = aFrom[i].n;
    for(j=0; j<n && diff_isspace(z[j]); j++){}
    aTo[i].z = &z[j];
    for(k=aFrom[i].n; k>j && diff_isspace(z[k-1]); k--){}
    aTo[i].n = n = k-j;
    aTo[i].indent = 0;
    aTo[i].nw = 0;
    for(h=0; j<k; j++){
      char c = z[j];
      if( !diff_isspace(c) ){
        h = (h^c)*9000000000000000041LL;
      }
    }
    aTo[i].h = h = ((h%281474976710597LL)<<FSL__LINE_LENGTH_MASK_SZ) | n;
    h2 = h % nLine;
    aTo[i].iNext = aTo[h2].iHash;
    aTo[h2].iHash = i+1;
  }
}

/*
** For a difficult diff-block alignment that was originally for
** the default consider-all-whitespace algorithm, try to find the
** longest common subsequence between the two blocks that involves
** only whitespace changes.
**
** Result is stored in *pOut and must be eventually fsl_free()d.
** Returns 0 on success, setting *pOut to NULL if no good match is
** found. Returns FSL_RC_OOM on allocation error.
*/
static int diffBlockAlignmentIgnoreSpace(
  fsl_dline * const aLeft, int nLeft,     /* Text on the left */
  fsl_dline * const aRight, int nRight,   /* Text on the right */
  fsl_dibu_opt const *pOpt,            /* Configuration options */
  unsigned char ** pOut,          /* OUTPUT: Result */
  unsigned *pNResult                /* OUTPUT: length of result */
){
  fsl__diff_cx dc;
  int iSX, iEX;                /* Start and end of LCS on the left */
  int iSY, iEY;                /* Start and end of the LCS on the right */
  unsigned char *a1, *a2;
  int n1, n2, nLCS, rc = 0;

  dc.aEdit = 0;
  dc.nEdit = 0;
  dc.nEditAlloc = 0;
  dc.nFrom = nLeft;
  dc.nTo = nRight;
  dc.cmpLine = fsl_dline_cmp_ignore_ws;
  dc.aFrom = fsl_malloc( sizeof(fsl_dline)*(nLeft+nRight) );
  if(!dc.aFrom) return FSL_RC_OOM;
  dc.aTo = &dc.aFrom[dc.nFrom];
  diffDLineXfer(dc.aFrom, aLeft, nLeft);
  diffDLineXfer(dc.aTo, aRight, nRight);
  fsl__diff_optimal_lcs(&dc,0,nLeft,0,nRight,&iSX,&iEX,&iSY,&iEY);
  fsl_free(dc.aFrom);
  nLCS = iEX - iSX;
  if( nLCS<5 ){
    /* No good LCS was found */
    *pOut = NULL;
    *pNResult = 0;
    return 0;
  }
  rc = diffBlockAlignment(aLeft,iSX,aRight,iSY,
                          pOpt,&a1, (unsigned *)&n1);
  if(rc) return rc;
  rc = diffBlockAlignment(aLeft+iEX, nLeft-iEX,
                          aRight+iEY, nRight-iEY,
                          pOpt, &a2, (unsigned *)&n2);
  if(rc){
    fsl_free(a1);
    return rc;
  }else{
    unsigned char * x = (unsigned char *)fsl_realloc(a1, n1+nLCS+n2);
    if(NULL==x){
      fsl_free(a1);
      fsl_free(a2);
      return FSL_RC_OOM;
    }
    a1 = x;
  }
  memcpy(a1+n1+nLCS,a2,n2);
  memset(a1+n1,3,nLCS);
  fsl_free(a2);
  *pNResult = (unsigned)(n1+n2+nLCS);
  *pOut = a1;
  return 0;
}

/*
** This is a helper route for diffBlockAlignment().  In this case,
** a very large block is encountered that might be too expensive to
** use the O(N*N) Wagner edit distance algorithm.  So instead, this
** block implements a less-precise but faster O(N*logN) divide-and-conquer
** approach.
**
** Result is stored in *pOut and must be eventually fsl_free()d.
** Returns 0 on success. Returns FSL_RC_OOM on allocation error.
*/
static int diffBlockAlignmentDivideAndConquer(
  fsl_dline * const aLeft, int nLeft,     /* Text on the left */
  fsl_dline * const aRight, int nRight,   /* Text on the right */
  fsl_dibu_opt const *pOpt,            /* Configuration options */
  unsigned char ** pOut,       /* OUTPUT: result */
  unsigned *pNResult                /* OUTPUT: length of result */
){
  fsl_dline *aSmall;               /* The smaller of aLeft and aRight */
  fsl_dline *aBig;                 /* The larger of aLeft and aRight */
  int nSmall, nBig;            /* Size of aSmall and aBig.  nSmall<=nBig */
  int iDivSmall, iDivBig;      /* Divider point for aSmall and aBig */
  int iDivLeft, iDivRight;     /* Divider point for aLeft and aRight */
  unsigned char *a1 = 0, *a2 = 0; /* Results of the alignments on two halves */
  int n1, n2;                  /* Number of entries in a1 and a2 */
  int score, bestScore;        /* Score and best score seen so far */
  int i;                       /* Loop counter */
  int rc;

  if( nLeft>nRight ){
    aSmall = aRight;
    nSmall = nRight;
    aBig = aLeft;
    nBig = nLeft;
  }else{
    aSmall = aLeft;
    nSmall = nLeft;
    aBig = aRight;
    nBig = nRight;
  }
  iDivBig = nBig/2;
  iDivSmall = nSmall/2;
  bestScore = 10000;
  for(i=0; i<nSmall; i++){
    score = match_dline2(aBig+iDivBig, aSmall+i) + abs(i-nSmall/2)*2;
    if( score<bestScore ){
      bestScore = score;
      iDivSmall = i;
    }
  }
  if( aSmall==aRight ){
    iDivRight = iDivSmall;
    iDivLeft = iDivBig;
  }else{
    iDivRight = iDivBig;
    iDivLeft = iDivSmall;
  }
  rc = diffBlockAlignment(aLeft,iDivLeft,aRight,iDivRight,
                          pOpt,&a1, (unsigned*)&n1);
  if(!rc){
    rc = diffBlockAlignment(aLeft+iDivLeft, nLeft-iDivLeft,
                            aRight+iDivRight, nRight-iDivRight,
                            pOpt, &a2, (unsigned*)&n2);
  }
  if(rc){
    fsl_free(a1);
    fsl_free(a2);
  }else{
    unsigned char * x = (unsigned char *)fsl_realloc(a1, n1+n2);
    if(!x) rc = FSL_RC_OOM;
    else{
      a1 = x;
      memcpy(a1+n1,a2,n2);
      *pNResult = (unsigned)(n1+n2);
      *pOut = a1;
    }
    fsl_free(a2);
  }
  return rc;
}


/*
** There is a change block in which nLeft lines of text on the left are
** converted into nRight lines of text on the right.  This routine computes
** how the lines on the left line up with the lines on the right.
**
** The return value is a buffer of unsigned characters, obtained from
** fsl_malloc().  (The caller needs to free the `*pResult` value using
** fsl_free().)  Entries in the returned array have values as follows:
**
**    1.  Delete the next line of pLeft.
**    2.  Insert the next line of pRight.
**    3.  The next line of pLeft changes into the next line of pRight.
**    4.  Delete one line from pLeft and add one line to pRight.
**
** The length of the returned array will be at most nLeft+nRight bytes.
** If the first bytes is 4, that means we could not compute reasonable
** alignment between the two blocks.
**
** Algorithm:  Wagner's minimum edit-distance algorithm, modified by
** adding a cost to each match based on how well the two rows match
** each other.  Insertion and deletion costs are 50.  Match costs
** are between 0 and 100 where 0 is a perfect match 100 is a complete
** mismatch.
*/
int diffBlockAlignment(
  fsl_dline * const aLeft, int nLeft,     /* Text on the left */
  fsl_dline * const aRight, int nRight,   /* Text on the right */
  fsl_dibu_opt const * pOpt,             /* Configuration options */
  unsigned char **pResult,         /* Raw result */
  unsigned *pNResult               /* OUTPUT: length of result */
){
  int i, j, k;                 /* Loop counters */
  int *a = 0;                  /* One row of the Wagner matrix */
  int *pToFree = 0;            /* Space that needs to be freed */
  unsigned char *aM = 0;       /* Wagner result matrix */
  int aBuf[100];               /* Stack space for a[] if nRight not to big */
  int rc = 0;

  if( nLeft==0 ){
    aM = fsl_malloc( nRight + 2 );
    if(!aM) return FSL_RC_OOM;
    memset(aM, 2, nRight);
    *pNResult = nRight;
    *pResult = aM;
    return 0;
  }
  if( nRight==0 ){
    aM = fsl_malloc( nLeft + 2 );
    if(!aM) return FSL_RC_OOM;
    memset(aM, 1, nLeft);
    *pNResult = nLeft;
    *pResult = aM;
    return 0;
  }

  /* For large alignments, try to use alternative algorithms that are
  ** faster than the O(N*N) Wagner edit distance. */
  if( (int64_t)nLeft*(int64_t)nRight>DIFF_ALIGN_MX
      && (pOpt->diffFlags & FSL_DIFF2_SLOW_SBS)==0 ){
    if( (pOpt->diffFlags & FSL_DIFF2_IGNORE_ALLWS)==0 ){
      *pResult = NULL;
      rc = diffBlockAlignmentIgnoreSpace(aLeft, nLeft,aRight,nRight,
                                         pOpt, pResult, pNResult);
      if(rc || *pResult) return rc;
    }
    return diffBlockAlignmentDivideAndConquer(aLeft, nLeft,aRight, nRight,
                                              pOpt, pResult, pNResult);
  }

  /* If we reach this point, we will be doing an O(N*N) Wagner minimum
  ** edit distance to compute the alignment.
  */
  if( nRight < (int)(sizeof(aBuf)/sizeof(aBuf[0]))-1 ){
    pToFree = 0;
    a = aBuf;
  }else{
    a = pToFree = fsl_malloc( sizeof(a[0])*(nRight+1) );
    if(!a){
      rc = FSL_RC_OOM;
      goto end;
    }
  }
  aM = fsl_malloc( (nLeft+1)*(nRight+1) );
  if(!aM){
    rc = FSL_RC_OOM;
    goto end;
  }

  /* Compute the best alignment */
  for(i=0; i<=nRight; i++){
    aM[i] = 2;
    a[i] = i*50;
  }
  aM[0] = 0;
  for(j=1; j<=nLeft; j++){
    int p = a[0];
    a[0] = p+50;
    aM[j*(nRight+1)] = 1;
    for(i=1; i<=nRight; i++){
      int m = a[i-1]+50;
      int d = 2;
      if( m>a[i]+50 ){
        m = a[i]+50;
        d = 1;
      }
      if( m>p ){
        int const score =
          match_dline2(&aLeft[j-1], &aRight[i-1]);
        if( (score<=90 || (i<j+1 && i>j-1)) && m>p+score ){
          m = p+score;
          d = 3 | score*4;
        }
      }
      p = a[i];
      a[i] = m;
      aM[j*(nRight+1)+i] = d;
    }
  }

  /* Compute the lowest-cost path back through the matrix */
  i = nRight;
  j = nLeft;
  k = (nRight+1)*(nLeft+1)-1;
  while( i+j>0 ){
    unsigned char c = aM[k];
    if( c>=3 ){
      assert( i>0 && j>0 );
      i--;
      j--;
      aM[k] = 3;
    }else if( c==2 ){
      assert( i>0 );
      i--;
    }else{
      assert( j>0 );
      j--;
    }
    k--;
    aM[k] = aM[j*(nRight+1)+i];
  }
  k++;
  i = (nRight+1)*(nLeft+1) - k;
  memmove(aM, &aM[k], i);
  *pNResult = i;
  *pResult = aM;

  end:
  fsl_free(pToFree);
  return rc;
}


/*
** Format a diff using a fsl_dibu object
*/
static int fdb__format(
  fsl__diff_cx * const cx,
  fsl_dibu * const pBuilder
){
  fsl_dline *A;        /* Left side of the diff */
  fsl_dline *B;        /* Right side of the diff */
  fsl_dibu_opt const * pOpt = pBuilder->opt;
  const int *R;          /* Array of COPY/DELETE/INSERT triples */
  unsigned int a;    /* Index of next line in A[] */
  unsigned int b;    /* Index of next line in B[] */
  unsigned int r;        /* Index into R[] */
  unsigned int nr;       /* Number of COPY/DELETE/INSERT triples to process */
  unsigned int mxr;      /* Maximum value for r */
  unsigned int na, nb;   /* Number of lines shown from A and B */
  unsigned int i, j;     /* Loop counters */
  unsigned int m, ma, mb;/* Number of lines to output */
  signed int skip;   /* Number of lines to skip */
  unsigned int contextLines; /* Lines of context above and below each change */
  unsigned short passNumber = 0;
  int rc = 0;
  
#define RC if(rc) goto end
#define METRIC(M) if(1==passNumber) ++pBuilder->metrics.M
  pass_again:
  contextLines = diff_opt_context_lines(pOpt);
  skip = 0;
  a = b = 0;
  A = cx->aFrom;
  B = cx->aTo;
  R = cx->aEdit;
  mxr = cx->nEdit;
  //MARKER(("contextLines=%u, nEdit = %d, mxr=%u\n", contextLines, cx->nEdit, mxr));
  while( mxr>2 && R[mxr-1]==0 && R[mxr-2]==0 ){ mxr -= 3; }

  pBuilder->lnLHS = pBuilder->lnRHS = 0;
  ++passNumber;
  if(pBuilder->start){
    pBuilder->passNumber = passNumber;
    rc = pBuilder->start(pBuilder);
    RC;
  }
  for(r=0; r<mxr; r += 3*nr){
    /* Figure out how many triples to show in a single block */
    for(nr=1; 3*nr<mxr && R[r+nr*3]>0 && R[r+nr*3]<(int)contextLines*2; nr++){}

#if 0
    /* MISSING: this "should" be replaced by a stateful predicate
       function, probably in the fsl_dibu_opt class. */
    /* If there is a regex, skip this block (generate no diff output)
    ** if the regex matches or does not match both insert and delete.
    ** Only display the block if one side matches but the other side does
    ** not.
    */
    if( pOpt->pRe ){
      int hideBlock = 1;
      int xa = a, xb = b;
      for(i=0; hideBlock && i<nr; i++){
        int c1, c2;
        xa += R[r+i*3];
        xb += R[r+i*3];
        c1 = re_dline_match(pOpt->pRe, &A[xa], R[r+i*3+1]);
        c2 = re_dline_match(pOpt->pRe, &B[xb], R[r+i*3+2]);
        hideBlock = c1==c2;
        xa += R[r+i*3+1];
        xb += R[r+i*3+2];
      }
      if( hideBlock ){
        a = xa;
        b = xb;
        continue;
      }
    }
#endif

    /* Figure out how many lines of A and B are to be displayed
    ** for this change block.
    */
    if( R[r]>(int)contextLines ){
      na = nb = contextLines;
      skip = R[r] - contextLines;
    }else{
      na = nb = R[r];
      skip = 0;
    }
    for(i=0; i<nr; i++){
      na += R[r+i*3+1];
      nb += R[r+i*3+2];
    }
    if( R[r+nr*3]>(int)contextLines ){
      na += contextLines;
      nb += contextLines;
    }else{
      na += R[r+nr*3];
      nb += R[r+nr*3];
    }
    for(i=1; i<nr; i++){
      na += R[r+i*3];
      nb += R[r+i*3];
    }

    //MARKER(("Chunk header... a=%u, b=%u, na=%u, nb=%u, skip=%d\n", a, b, na, nb, skip));
    if(pBuilder->chunkHeader
       /* The following bit is a kludge to keep from injecting a chunk
          header between chunks which are directly adjacent.

          The problem, however, is that we cannot skip _reliably_
          without also knowing how the next chunk aligns. If we skip
          it here, the _previous_ chunk header may well be telling
          the user a lie with regards to line numbers.

          Fossil itself does not have this issue because it generates
          these chunk headers directly in this routine, instead of in
          the diff builder, depending on a specific flag being set in
          builder->opt. Also (related), in that implementation, fossil
          will collapse chunks which are separated by less than the
          context distance into contiguous chunks (see below). Because
          we farm out the chunkHeader lines to the builder, we cannot
          reliably do that here.
       */
#if 0
       && !skip
#endif
       ){
      rc = pBuilder->chunkHeader(pBuilder,
                                 (uint32_t)(na ? a+skip+1 : a+skip),
                                 (uint32_t)na,
                                 (uint32_t)(nb ? b+skip+1 : b+skip),
                                 (uint32_t)nb);
      RC;
    }

    /* Show the initial common area */
    a += skip;
    b += skip;
    m = R[r] - skip;
    if( r ) skip -= contextLines;

    //MARKER(("Show the initial common... a=%u, b=%u, m=%u, r=%u, skip=%d\n", a, b, m, r, skip));
    if( skip>0 ){
      if( NULL==pBuilder->chunkHeader && skip<(int)contextLines ){
        /* 2021-09-27: BUG: this is incompatible with unified diff
           format. The generated header lines say we're skipping X
           lines but we then end up including lines which that header
           says to skip. As a workaround, we'll only run this when
           pBuilder->chunkHeader is NULL, noting that fossil's diff
           builder interface does not have that method (and thus
           doesn't have this issue, instead generating chunk headers
           directly in this algorithm).

           Without this block, our "utxt" diff builder can mimic
           fossil's non-diff builder unified diff format, except that
           we add Index lines (feature or bug?). With this block,
           the header values output above are wrong.
        */
        /* If the amount to skip is less that the context band, then
        ** go ahead and show the skip band as it is not worth eliding */
        //MARKER(("skip %d < contextLines %d\n", skip, contextLines));
        /* from fossil(1) from formatDiff() */
        for(j=0; 0==rc && j<(unsigned)skip; j++){
          //MARKER(("(A) COMMON\n"));
          rc = pBuilder->common(pBuilder, &A[a+j-skip]);
        }
      }else{
        rc = pBuilder->skip(pBuilder, skip);
      }
      RC;
    }
    for(j=0; 0==rc && j<m; j++){
      //MARKER(("(B) COMMON\n"));
      rc = pBuilder->common(pBuilder, &A[a+j]);
    }
    RC;
    a += m;
    b += m;
    //MARKER(("Show the differences... a=%d, b=%d, m=%d\n", a, b, m));

    /* Show the differences */
    for(i=0; i<nr; i++){
      unsigned int nAlign;
      unsigned char *alignment = 0;
      ma = R[r+i*3+1];   /* Lines on left but not on right */
      mb = R[r+i*3+2];   /* Lines on right but not on left */

#if FSL_DIFF_SMALL_GAP
  /* Try merging the current block with subsequent blocks, if the
      ** subsequent blocks are nearby and their result isn't too big.
      */
      while( i<nr-1 && smallGap2(&R[r+i*3],ma,mb) ){
        i++;
        m = R[r+i*3];
        ma += R[r+i*3+1] + m;
        mb += R[r+i*3+2] + m;
      }
#endif

      /* Try to find an alignment for the lines within this one block */
      rc = diffBlockAlignment(&A[a], ma, &B[b], mb, pOpt,
                              &alignment, &nAlign);
      RC;
      for(j=0; ma+mb>0; j++){
        assert( j<nAlign );
        switch( alignment[j] ){
          case 1: {
            /* Delete one line from the left */
            METRIC(deletions);
            rc = pBuilder->deletion(pBuilder, &A[a]);
            if(rc) goto bail;
            ma--;
            a++;
            break;
          }
          case 2: {
            /* Insert one line on the right */
            METRIC(insertions);
            rc = pBuilder->insertion(pBuilder, &B[b]);
            if(rc) goto bail;
            assert( mb>0 );
            mb--;
            b++;
            break;
          }
          case 3: {
            /* The left line is changed into the right line */
            if( 0==cx->cmpLine(&A[a], &B[b]) ){
              rc = pBuilder->common(pBuilder, &A[a]);
            }else{
              METRIC(edits);
              rc = pBuilder->edit(pBuilder, &A[a], &B[b]);
            }
            if(rc) goto bail;
            assert( ma>0 && mb>0 );
            ma--;
            mb--;
            a++;
            b++;
            break;
          }
          case 4: {
            /* Delete from left then separately insert on the right */
            METRIC(replacements);
            rc = pBuilder->replacement(pBuilder, &A[a], &B[b]);
            if(rc) goto bail;
            ma--;
            a++;
            mb--;
            b++;
            break;
          }
        }
      }
      assert( nAlign==j );
      fsl_free(alignment);
      if( i<nr-1 ){
        m = R[r+i*3+3];
        for(j=0; 0==rc && j<m; j++){
          //MARKER(("D common\n"));
          rc = pBuilder->common(pBuilder, &A[a+j]);
        }
        RC;
        b += m;
        a += m;
      }
      continue;
      bail:
      assert(rc);
      fsl_free(alignment);
      goto end;
    }

    /* Show the final common area */
    assert( nr==i );
    m = R[r+nr*3];
    if( m>contextLines ) m = contextLines;
    for(j=0; 0==rc && j<m && j<contextLines; j++){
      //MARKER(("E common\n"));
      rc = pBuilder->common(pBuilder, &A[a+j]);
    }
    RC;
  }
  if( R[r]>(int)contextLines ){
    rc = pBuilder->skip(pBuilder, R[r] - contextLines);
  }
  end:
#undef RC
#undef METRIC
  if(0==rc){
    if(pBuilder->finish) pBuilder->finish(pBuilder);
    if(pBuilder->twoPass && 1==passNumber){
      goto pass_again;
    }
  }
  return rc;
}

/* MISSING(?) fossil(1) converts the diff inputs into utf8 with no
   BOM. Whether we really want to do that here or rely on the caller
   to is up for debate. If we do it here, we have to make the inputs
   non-const, which seems "wrong" for a library API. */
#define blob_to_utf8_no_bom(A,B) (void)0

/**
   Performs a diff of version 1 (pA) and version 2 (pB). ONE of
   pBuilder or outRaw must be non-NULL. If pBuilder is not NULL, all
   output for the diff is emitted via pBuilder. If outRaw is not NULL
   then on success *outRaw is set to the array of diff triples,
   transfering ownership to the caller, who must eventually fsl_free()
   it. On error, *outRaw is not modified but pBuilder may have emitted
   partial output. That is not knowable for the general
   case. Ownership of pBuilder is not changed. If pBuilder is not NULL
   then pBuilder->opt must be non-NULL.
*/
static int fsl_diff2_text_impl(fsl_buffer const *pA,
                               fsl_buffer const *pB,
                               fsl_dibu * const pBuilder,
                               fsl_dibu_opt const * const opt_,
                               int ** outRaw){
  int rc = 0;
  fsl__diff_cx c = fsl__diff_cx_empty;
  bool ignoreWs = false;
  int ansiOptCount = 0;
  fsl_dibu_opt opt = *opt_
    /*we need a copy for the sake of the FSL_DIFF2_INVERT flag*/;
  if(!pA || !pB || (pBuilder && outRaw)) return FSL_RC_MISUSE;

  blob_to_utf8_no_bom(pA, 0);
  blob_to_utf8_no_bom(pB, 0);

  if( opt.diffFlags & FSL_DIFF2_INVERT ){
    char const * z;
    fsl_buffer const *pTemp = pA;
    pA = pB;
    pB = pTemp;
    z = opt.hashRHS; opt.hashRHS = opt.hashLHS; opt.hashLHS = z;
    z = opt.nameRHS; opt.nameRHS = opt.nameLHS; opt.nameLHS = z;
  }
#define AOPT(OPT) \
  if(opt.ansiColor.OPT) ansiOptCount += (*opt.ansiColor.OPT) ? 1 : 0; \
  else opt.ansiColor.OPT = ""
  AOPT(insertion);
  AOPT(edit);
  AOPT(deletion);
#undef AOPT
  if(0==ansiOptCount){
    opt.ansiColor.reset = "";
  }else if(!opt.ansiColor.reset || !*opt.ansiColor.reset){
    opt.ansiColor.reset = "\x1b[0m";
  }
  ignoreWs = (opt.diffFlags & FSL_DIFF2_IGNORE_ALLWS)!=0;
  if(FSL_DIFF2_IGNORE_ALLWS==(opt.diffFlags & FSL_DIFF2_IGNORE_ALLWS)){
    c.cmpLine = fsl_dline_cmp_ignore_ws;
  }else{
    c.cmpLine = fsl_dline_cmp;
  }
 
  rc = fsl_break_into_dlines(fsl_buffer_cstr(pA), (fsl_int_t)pA->used,
                             (uint32_t*)&c.nFrom, &c.aFrom, opt.diffFlags);
  if(rc) goto end;
  rc = fsl_break_into_dlines(fsl_buffer_cstr(pB), (fsl_int_t)pB->used,
                             (uint32_t*)&c.nTo, &c.aTo, opt.diffFlags);
  if(rc) goto end;

  /* Compute the difference */
  rc = fsl__diff_all(&c);
  if(rc) goto end;
  if( ignoreWs && c.nEdit==6 && c.aEdit[1]==0 && c.aEdit[2]==0 ){
    rc = FSL_RC_DIFF_WS_ONLY;
    goto end;
  }
  if( (opt.diffFlags & FSL_DIFF2_NOTTOOBIG)!=0 ){
    int i, m, n;
    int const * const a = c.aEdit;
    int const mx = c.nEdit;
    for(i=m=n=0; i<mx; i+=3){ m += a[i]; n += a[i+1]+a[i+2]; }
    if( !n || n>10000 ){
      rc = FSL_RC_RANGE;
      /* diff_errmsg(pOut, DIFF_TOO_MANY_CHANGES, diffFlags); */
      goto end;
    }
  }
  //fsl__dump_triples(&c, __FILE__, __LINE__);
  if( (opt.diffFlags & FSL_DIFF2_NOOPT)==0 ){
    fsl__diff_optimize(&c);
  }
  //fsl__dump_triples(&c, __FILE__, __LINE__);

  /**
     Reference:

     https://fossil-scm.org/home/file?ci=cae7036bb7f07c1b&name=src/diff.c&ln=2749-2804

     Noting that:

     - That function's return value is this one's *outRaw

     - DIFF_NUMSTAT flag is not implemented. For that matter,
     flags which result in output going anywhere except for
     pBuilder->out are not implemented here, e.g. DIFF_RAW.

     That last point makes this impl tiny compared to the original!
  */
  if(pBuilder){
    fsl_dibu_opt const * oldOpt = pBuilder->opt;
    pBuilder->opt = &opt;
    rc = fdb__format(&c, pBuilder);
    pBuilder->opt = oldOpt;
  }
  end:
  if(0==rc && outRaw){
    *outRaw = c.aEdit;
    c.aEdit = 0;
  }
  fsl__diff_cx_clean(&c);
  return rc;
}

int fsl_diff_v2(fsl_buffer const * pv1,
                fsl_buffer const * pv2,
                fsl_dibu * const pBuilder){
  return fsl_diff2_text_impl(pv1, pv2, pBuilder, pBuilder->opt, NULL);
}

int fsl_diff_v2_raw(fsl_buffer const * pv1,
                    fsl_buffer const * pv2,
                    fsl_dibu_opt const * const opt,
                    int **outRaw ){
  return fsl_diff2_text_impl(pv1, pv2, NULL,
                             opt ? opt : &fsl_dibu_opt_empty,
                             outRaw);
}


#undef DIFF_ALIGN_MX
#undef blob_to_utf8_no_bom
#undef FSL_DIFF_SMALL_GAP
#undef diff_isspace
#undef LENGTH
