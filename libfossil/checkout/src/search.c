/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */ 
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/

/*
  This file houses some FTS-search-related functionality.

  libfossil does not aim to reproduce all search functionality
  provided by fossil. Initially, at least, the only planned feature
  parity is that of updating the search index as content is
  added/updated.
*/
#include "fossil-scm/internal.h"
#include "fossil-scm/confdb.h"
#include <assert.h>
#include <memory.h> /* memcmp() */

/* Only for debugging */
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

/**
   Returns 0 if f has no search index or a repo is not opened, else 4
   for an FTS4 index and 5 for an FTS5. If forceRecheck is false then
   a cached values is returned after the first call. If forceRecheck
   is true, the cached value is updated.
 */
static int fsl__search_ndx_exists(fsl_cx * const f, bool forceRecheck){
  fsl_db * const db = fsl_cx_db_repo(f);
  if(db && (f->cache.searchIndexExists<0 || forceRecheck) ){
    const bool b = fsl_db_table_exists(db, FSL_DBROLE_REPO, "ftsdocs");
    if(b){
      f->cache.searchIndexExists =
        fsl_db_table_has_column(db, "ftsdocs", "rowid" )
        ? 5 // FTS5 (fossil as of 2023-01-24)
        : (fsl_db_table_has_column(db, "ftsdocs", "docid" ) ? 4 : 0);
      assert(f->cache.searchIndexExists==4 || f->cache.searchIndexExists==5);
    }else{
      f->cache.searchIndexExists = 0;
    }
  }
  return f->cache.searchIndexExists;
}

static char fsl_satype_letter(fsl_satype_e t){
  switch(t){
    case FSL_SATYPE_CHECKIN: return 'c';
    case FSL_SATYPE_WIKI: return 'w';
    case FSL_SATYPE_TICKET: return 't';
    case FSL_SATYPE_FORUMPOST: return 'f';
    case FSL_SATYPE_TECHNOTE: return 'e';
    default:
      assert(!"Internal misuse of fsl_satype_letter()");
      return 0;
  }
}

int fsl__search_doc_touch(fsl_cx * const f, fsl_satype_e saType,
                         fsl_id_t rid, const char * docName){
  const int ftsVers = fsl__search_ndx_exists(f, false);
  if(!ftsVers || fsl_content_is_private(f, rid)) return 0;
  assert(ftsVers==4 || ftsVers==5 || !"If this fails then our search-index-exists check is wrong.");
  char zType[2] = {0,0};
  zType[0] = fsl_satype_letter(saType);
#if 0
  /* See MARKER() call in the #else block */
  assert(*zType);
  return *zType ? 0 : FSL_RC_MISUSE;
#else
  /* Reminder: fossil(1) does some once-per-connection init here which
     installs UDFs used by the search process. Those will be significant
     for us if we add the search features to the library. */
  assert(zType[0] && "Misuse of fsl__search_doc_touch()'s 2nd parameter.");
  fsl_db * const db = fsl_cx_db_repo(f);
  int rc = 0;
  char const * zDocId = 4==ftsVers ? "docid" : "rowid";
  rc = fsl_db_exec(db,
       "DELETE FROM ftsidx WHERE %s IN"
       "    (SELECT rowid FROM ftsdocs WHERE type=%Q AND rid=%"FSL_ID_T_PFMT" AND idxed)",
       zDocId, zType, rid );
  if(rc){
    // For reasons i don't understand, this query fails with "SQL logic error"
    // when run from here, but succeeds fine in fossil and fossil's SQL shell.
    MARKER(("type=%s rid=%d rc=%s\n",zType, (int)rid, fsl_rc_cstr(rc)));
    goto end;
  }
  rc = fsl_db_exec(db,
       "REPLACE INTO ftsdocs(type,rid,name,idxed)"
       " VALUES(%Q,%"FSL_ID_T_PFMT",%Q,0)",
       zType, rid, docName );
  if(rc) goto end;
  if( FSL_SATYPE_WIKI==saType || FSL_SATYPE_TECHNOTE==saType ){
    rc = fsl_db_exec(db,
        "DELETE FROM ftsidx WHERE %s IN"
        "    (SELECT rowid FROM ftsdocs WHERE type=%Q AND name=%Q AND idxed)",
        zDocId, zType, docName );
    if(!rc) rc = fsl_db_exec(db,
        "DELETE FROM ftsdocs WHERE type=%Q AND name=%Q AND rid!=%"FSL_ID_T_PFMT,
        zType, docName, rid );
  }
  /* All forum posts are always indexed */
  end:
  return rc;
#endif
}

#undef MARKER
