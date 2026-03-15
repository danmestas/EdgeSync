/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */ 
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/
/************************************************************************
  This file implements technote (formerly known as event)-related
  parts of the library.
*/
#include "fossil-scm/internal.h"
#include <assert.h>


/**
   Fetches all "technote" (formerly "event") IDs from the repository
   and appends each one to the given list in the form of a
   (`char*`). This function relies on the `event-` tag prefix being
   reserved for technotes and that the technote IDS are all exactly 40
   bytes long.
   
   Returns 0 on success, FSL_RC_NOT_A_REPO if f has no repository db
   opened, FSL_RC_OOM if allocation of a new list entry fails, or
   propagates db-related code on any other error. Results are
   undefined if either argument is NULL.

   TODO? Reformulate this to be like fsl_tkt_id_to_rids(), returning
   the list as RIDs?
*/
/*FSL_EXPORT*/ int fsl_technote_ids_get(fsl_cx * const f, fsl_list * const tgt );

int fsl_technote_ids_get( fsl_cx * const f, fsl_list * const tgt ){
  fsl_db * const db = fsl_needs_repo(f);
  if(!db) return FSL_RC_NOT_A_REPO;
  int rc = fsl_db_select_slist( db, tgt,
                                "SELECT substr(tagname,7) AS n "
                                "FROM tag "
                                "WHERE tagname GLOB 'event-*' "
                                "AND length(tagname)=46 "
                                "ORDER BY n");
  if(rc && db->error.code && !f->error.code){
    fsl_cx_uplift_db_error(f, db);
  }
  return rc;
}
