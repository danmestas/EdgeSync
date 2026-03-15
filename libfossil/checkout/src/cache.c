/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/
/*****************************************************************************
  This file some of the caching-related APIs.
*/
#include "fossil-scm/internal.h"
#include <assert.h>
#include <string.h> /* memmove() */

/* Only for debugging */
#include <stdio.h>
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

bool fsl__bccache_expire_oldest(fsl__bccache * const c){
  static uint16_t const sentinel = 0xFFFF;
  uint16_t i;
  fsl_uint_t mnAge = c->nextAge;
  uint16_t mn = sentinel;
  for(i=0; i<c->used; i++){
    if( c->list[i].age<mnAge ){
      mnAge = c->list[i].age;
      mn = i;
    }
  }
  if( mn<sentinel ){
    fsl_id_bag_remove(&c->inCache, c->list[mn].rid);
    c->szTotal -= (unsigned)c->list[mn].content.capacity;
    fsl_buffer_clear(&c->list[mn].content);
    --c->used;
    c->list[mn] = c->list[c->used];
  }
  return sentinel!=mn;
}

int fsl__bccache_insert(fsl__bccache * const c, fsl_id_t rid, fsl_buffer * const pBlob){
  fsl__bccache_line *p;
  if( c->used>c->usedLimit || c->szTotal>c->szLimit ){
    fsl_size_t szBefore;
    do{
      szBefore = c->szTotal;
      fsl__bccache_expire_oldest(c);
    }while( c->szTotal>c->szLimit && c->szTotal<szBefore );
  }
  if((!c->usedLimit || !c->szLimit)
     || (c->used+1 >= c->usedLimit)){
    fsl_buffer_clear(pBlob);
    return 0;
  }
  if( c->used>=c->capacity ){
    uint16_t const cap = c->capacity ? (c->capacity*2) : 10;
    void * remem = c->list
      ? fsl_realloc(c->list, cap*sizeof(c->list[0]))
      : fsl_malloc( cap*sizeof(c->list[0]) );
    assert((c->capacity && cap<c->capacity) ? !"Numeric overflow" : 1);
    if(c->capacity && cap<c->capacity){
      fsl__fatal(FSL_RC_RANGE,"Numeric overflow. Bump "
                 "fsl__bccache::capacity to a larger int type.");
    }
    if(!remem){
      fsl_buffer_clear(pBlob) /* for consistency */;
      return FSL_RC_OOM;
    }
    c->capacity = cap;
    c->list = (fsl__bccache_line*)remem;
  }
  int const rc = fsl_id_bag_insert(&c->inCache, rid);
  if(0==rc){
    p = &c->list[c->used++];
    p->rid = rid;
    p->age = c->nextAge++;
    c->szTotal += pBlob->capacity;
    ++c->changeMarker;
    p->content = *pBlob /* Transfer ownership */;
    *pBlob = fsl_buffer_empty;
  }else{
    fsl_buffer_clear(pBlob);
  }
  return rc;
}

#if 0 /* untested */
static bool fsl__bccache_remove(fsl__bccache * const c, fsl_id_t rid){
  static const fsl__bccache_line fsl__bccache_line_empty =
    fsl__bccache_line_empty_m;
  if( !fsl_id_bag_contains(&c->inCache, rid) ) return false;
  fsl__bccache_line *p = 0;
  uint16_t i;
  MARKER(("UNTESTED! removing rid%d from bccache\n", (int)rid));
  for( i = 0; i < c->used; ++i ){
    p = &c->list[i];
    if( rid == p->rid ){
      fsl_buffer_clear(&p->content);
      if( i<c->used-1 ){
        memmove(p, p+1, sizeof(fsl__bccache_line) * (c->used - i - 1));
      }
      --c->used;
      c->list[c->used] = fsl__bccache_line_empty;
      MARKER(("TESTED and seems to have worked\n"));
      return true;
    }
  }
  return false;
}
#endif

void fsl__bccache_clear(fsl__bccache * const c){
  static const fsl__bccache fsl__bccache_empty = fsl__bccache_empty_m;
#if 0
  while(fsl__bccache_expire_oldest(c)){}
#else
  fsl_size_t i;
  for(i=0; i<c->used; ++i){
    fsl_buffer_clear(&c->list[i].content);
  }
#endif
  fsl_free(c->list);
  fsl_id_bag_clear(&c->inCache);
  *c = fsl__bccache_empty;
}

void fsl__bccache_reuse(fsl__bccache * const c){
  static const fsl__bccache_line line_empty = fsl__bccache_line_empty_m;
  fsl_size_t i;
  for(i=0; i<c->used; ++i){
    fsl_buffer_clear(&c->list[i].content);
    c->list[i] = line_empty;
  }
  c->used = 0;
  c->szTotal = 0;
  c->nextAge = 0;
  fsl_id_bag_reuse(&c->inCache);
}


int fsl__bccache_check_available(fsl_cx * const f, fsl_id_t rid){
  fsl_id_t srcid;
  int depth = 0;  /* Limit to recursion depth */
  static const int limit = 100000 /* historical value=10M */;
  int rc;
  fsl__bccache * const c = &f->cache.blobContent;
  assert(f);
  assert(c);
  assert(rid>0);
  assert(fsl_cx_db_repo(f));
  while( depth++ < limit ){
    fsl_int_t cSize = -1;
    if( fsl__cx_ptl_has(f, fsl__ptl_e_missing, rid) ){
      return FSL_RC_NOT_FOUND;
    }else if( fsl__cx_ptl_has(f, fsl__ptl_e_available, rid) ){
      return 0;
    }
    rc = fsl_content_size_v2(f, rid, &cSize);
    if( rc ) return rc;
    else if( cSize<0 /*phantom*/ ){
      rc = fsl__cx_ptl_insert(f, fsl__ptl_e_missing, rid);
      return rc ? rc : FSL_RC_NOT_FOUND;
    }
    srcid = 0;
    rc = fsl_delta_r2s(f, rid, &srcid);
    if(rc) return rc;
    else if( srcid==0 ){
      return fsl__cx_ptl_insert(f, fsl__ptl_e_available, rid);
    }
    rid = srcid;
  }
  assert(!"apparent delta-loop in repository");
  return fsl_cx_err_set(f, FSL_RC_CONSISTENCY,
                        "Serious problem: apparent delta-loop in repository");
}

static void fsl__ptl_line_clear( fsl__ptl_line * cl ){
  cl->changeMarker = 0;
#define E(X) fsl_id_bag_clear(&cl->X);
  fsl__ptl_line_map(E)
#undef E
}

static void fsl__ptl_line_reuse( fsl__ptl_line * cl ){
  cl->changeMarker = 0;
#define E(X) fsl_id_bag_reuse(&cl->X);
  fsl__ptl_line_map(E)
#undef E
}

static int fsl__ptl_reserve( fsl__ptl * c, uint16_t n ){
  static const struct fsl__ptl_line fsl__ptl_line_empty = {
#define E(X) .X = fsl_id_bag_empty_m,
    fsl__ptl_line_map(E)
#undef E
    .changeMarker = 0
  };

  if( 0==n ){
    if( c->allocated ){
      for( uint16_t i = 0; i < c->allocated; ++i ){
        fsl__ptl_line_clear(&c->stack[i]);
      }
    }
    fsl_buffer_clear( &c->mem );
    c->allocated = 0;
    c->level = 0;
    c->stack = NULL;
    c->line = 0;
    c->rc = 0;
  }else if( 0==c->rc && n>c->allocated ){
    c->rc = fsl_buffer_reserve( &c->mem, sizeof(fsl__ptl_line) * n );
    if( 0==c->rc ){
      c->stack = (fsl__ptl_line*)c->mem.mem;
      for( uint16_t i = c->allocated; i < n; ++i ){
        c->stack[i] = fsl__ptl_line_empty;
      }
      c->allocated = n;
    }
  }
  return c->rc;
}

static inline fsl__ptl_line *fsl__ptl_ln( fsl__ptl * const c ){
  assert( c->mem.mem );
  return &c->stack[c->level];
}

static inline fsl__ptl_line * fsl__cx_ptl_line(fsl_cx * const f){
  return fsl__ptl_ln(&f->cache.ptl);
}

fsl_id_bag * fsl__cx_ptl_bag(fsl_cx *f, fsl__ptl_e where){
  fsl__ptl_line * const line = fsl__cx_ptl_line(f);
  switch(where){

#define E(X) case fsl__ptl_e_ ## X: return &line->X;
    fsl__ptl_line_map(E)
#undef E

    default:
      fsl__fatal(FSL_RC_CANNOT_HAPPEN,"Not possible");
      return NULL/*not reached*/;
  }
}

#define fsl__ptl_assert_clean(P)                            \
  assert(!P->level); assert(!P->line); assert(!P->stack);   \
  assert(!P->mem.mem); assert(!P->rc); assert(!P->allocated)

int fsl__ptl_init( fsl__ptl * c, uint16_t prealloc ){
  fsl__ptl_assert_clean(c);
  fsl__ptl_reserve( c, prealloc+1 );
  if( 0==c->rc ){
    c->line = fsl__ptl_ln(c);
  }
#if !defined(NDEBUG)
  else {
    assert( !c->stack );
    assert( !c->mem.mem );
  }
#endif
  return c->rc;
}

/**
   Frees all memory owned by c but does not free c.
*/
void fsl__ptl_clear( fsl__ptl * c ){
  fsl__ptl_reserve(c, 0);
  fsl__ptl_assert_clean(c);
}
#undef fsl__ptl_assert_clean

/**
   Pushes a level to the stack. Returns 0 on success, FSL_RC_OOM on
   allocation error. Returns c->rc and is a no-op if that's non-0 when
   this is called.
*/
static int fsl__ptl_push( fsl__ptl * c ){
  if( 0==c->rc ){
    assert( (int16_t)(c->level+1) > 0 );
    fsl__ptl_reserve( c, c->level+1 );
    if( 0==c->rc ){
      ++c->level;
      c->line = fsl__ptl_ln(c);
    }
  }
  return c->rc;
}

/**
   Pops a level from the stack.

   TODO: we need the fsl_cx instance and a flag telling us whether to
   clean up, e.g. f->cache.blobContent or remove RIDs from a given db
   table. Maybe a callback like:

   void (*callback)(fsl_cx * f, fsl__ptl_e where, fsl_id_bag *theRids)

   which gets called for every being-popped bag.
*/
static void fsl__ptl_pop( fsl__ptl * c ){
  assert( c->level>0 && "Don't pop level 0" );
  assert( c->line );
  fsl__ptl_line_reuse(c->line);
  --c->level;
  c->line = fsl__ptl_ln(c);
}

/**
   Searches all levels of the given cache type for rid, newest
   first. Returns true if found, else false.
*/
bool fsl__ptl_has( fsl__ptl const * c, fsl__ptl_e where, fsl_id_t rid ){
  assert( c->line );
  for( int16_t n = (int16_t)c->level; n >= 0; --n ){
    switch(where){

#define E(X)                                             \
      case fsl__ptl_e_ ## X:                             \
      return fsl_id_bag_contains(&c->stack[n].X, rid);
      fsl__ptl_line_map(E)
#undef E

      case fsl__ptl_e_dummy:
      fsl__fatal(FSL_RC_CANNOT_HAPPEN,"Not possible");
      break;
    }
  }
  return false;
}

/**
   Adds rid to the given cache for the current stack level. Returns
   c->rc and is a no-op if that's non-0 when this is called.
*/
int fsl__ptl_insert( fsl__ptl * c, fsl__ptl_e where, fsl_id_t rid ){
  if( 0==c->rc ){
    assert( c->line );
    switch(where){

#define E(X) case fsl__ptl_e_ ## X:                     \
      return c->rc = fsl_id_bag_insert(&c->line->X, rid);
      fsl__ptl_line_map(E)
#undef E

      case fsl__ptl_e_dummy:
        fsl__fatal(FSL_RC_CANNOT_HAPPEN,"Not possible");
      break;
    }
  }
  return c->rc;
}

/**
   Removes the rid from the first level of the stack which has it,
   starting at the oldest. Returns true if it removed an entry, else
   false.
*/
bool fsl__ptl_remove( fsl__ptl const * c, fsl__ptl_e where, fsl_id_t rid ){
  assert( c->line );
  bool rv = false;
#if 0
  /* (A) this can't happen, right? (B) we don't have access to the
     fsl_cx from here. */
  if( rid==f->db.ckout.rid ){
    fsl__cx_ckout_clear(f);
  }
#endif
  for( int16_t n = (int16_t)c->level; n >= 0; --n ){
    switch(where){

#define E(X) case fsl__ptl_e_ ## X:                           \
      if( fsl_id_bag_remove(&c->stack[n].X, rid) ) rv = true; \
      break;
      fsl__ptl_line_map(E)
#undef E
      /**
         We can very likely get by with stopping at the first found
         entry, but peace of mind requires that we make certain that
         explicitly-removed IDs, as opposed to a per-level wipe like
         fsl__ptl_pop() does, are not left in other levels.
      */
      case fsl__ptl_e_dummy:
      fsl__fatal(FSL_RC_CANNOT_HAPPEN,"Not possible");
      break;
    }
  }
  return rv;
}

int fsl__cx_ptl_push( fsl_cx * f ){
  fsl__ptl * const p = &f->cache.ptl;
  if( 0==fsl__ptl_push(p) ){
    fsl__ptl_ln(p)->changeMarker = f->cache.blobContent.changeMarker;
  }
  return p->rc;
}

int fsl__cx_ptl_pop( fsl_cx * f, bool isCommit ){
  fsl__ptl * const p = &f->cache.ptl;
  fsl__ptl_line * const line = fsl__ptl_ln(p);
  int rc = 0;
  assert( p->level && "Do not pop level 0" );
  fsl__cx_reset_for_txn(f, isCommit);
  if( isCommit ){
    /*
      Migrate all of line's entries into the previous line.  Any
      entries which needed to be cleaned up by now will have already
      been cleaned up if they need it. We always want the
      line->missing and line->available entries to bubble, as they're
      always so long as their corresponding blob records have not been
      rolled back.
    */
    fsl__ptl_line * const prev = &p->stack[p->level-1];

#define E(X)                                                            \
    if( 0==rc && line->X.entryCount ) {                                 \
      /*MARKER(("Uplifting RIDs from [" # X "] level %d\n",(int)p->level));*/ \
      if( prev->X.entryCount ) rc = fsl_id_bag_copy(&line->X, &prev->X);\
      else fsl_id_bag_swap(&prev->X, &line->X);                         \
    }

    fsl__ptl_line_map(E)

#undef E
  }else{
    if( f->cache.blobContent.changeMarker != line->changeMarker ){
      /**
         Entries were added to the content cache since this transaction
         level was started. Invalidate the cache.

         Potential TODO: use
         fsl__bccache_remove(&f->cache.blobContent,rid) for each rid
         in p->(toVerify, available, leafCheck). We know the blob
         cache does not map to any entries in p->missing. However,
         these caches may have arbitrarily many entries, and
         blobContent is an O(N) cache with potentially hundreds of
         entries, so that could become slow.
      */
      fsl__cx_content_caches_clear(f);
      fsl__bccache_reuse(&f->cache.blobContent);
    }
  }
  fsl__ptl_pop(p);
  return rc;
}

bool fsl__cx_ptl_remove( fsl_cx * f, fsl__ptl_e where, fsl_id_t rid ){
  fsl__ptl const * p = &f->cache.ptl;
  switch( where ){
    case fsl__ptl_e_available:
    case fsl__ptl_e_toVerify:
#if 0
      /* UNTESTED */
      fsl__bccache_remove(&f->cache.blobContent, rid);
#else
      fsl__cx_content_caches_clear(f)
        /* This RID could be in the blob cache or the deck cache. */;
#endif
      FSL_SWITCH_FALL_THROUGH;
    case fsl__ptl_e_dephantomize:
    case fsl__ptl_e_leafCheck:
    case fsl__ptl_e_missing:
      return fsl__ptl_remove(p, where, rid);
    case fsl__ptl_e_dummy:
      fsl__fatal(FSL_RC_CANNOT_HAPPEN,"Not possible") /* does not return */;
  }
  return false /* not reached */;
}

#undef MARKER
