/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */ 
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/
/*************************************************************************
  This file implements ticket-related parts of the library.
*/
#include "fossil-scm/internal.h"
#include <assert.h>
#include <string.h> /* memcmp() */

int fsl__cx_ticket_create_table(fsl_cx * const f){
  fsl_db * const db = fsl_needs_repo(f);
  int rc;
  if(!db) return FSL_RC_NOT_A_REPO;
  rc = fsl_cx_exec_multi(f,
                         "DROP TABLE IF EXISTS ticket;"
                         "DROP TABLE IF EXISTS ticketchng;"
                         );
  if(!rc){
    fsl_buffer * const buf = fsl__cx_content_buffer(f);
    rc = fsl_cx_schema_ticket(f, buf);
    if(!rc) rc = fsl_cx_exec_multi(f, "%b", buf);
    fsl__cx_content_buffer_yield(f);
  }
  return rc;
}

static int fsl__tkt_field_id(fsl_list const * jli, const char *zFieldName){
  int i;
  fsl_card_J const * jc;
  for(i=0; i<(int)jli->used; ++i){
    jc = (fsl_card_J const *)jli->list[i];
    if( !fsl_strcmp(zFieldName, jc->field) ) return i;
  }
  return -1;
}

int fsl__cx_ticket_load_fields(fsl_cx * const f, bool forceReload){
  fsl_list * const li = &f->ticket.customFields;
  if(li->used){
    if(!forceReload) return 0;
    fsl__card_J_list_free(li, false);
    /* Fall through and reload ... */
  }else if( !fsl_needs_repo(f) ){
    return FSL_RC_NOT_A_REPO;
  }
  fsl_card_J * jc;
  fsl_stmt q = fsl_stmt_empty;
  int i;
  int rc = fsl_cx_prepare(f, &q, "PRAGMA table_info(ticket)");
  if(!rc) while( FSL_RC_STEP_ROW==fsl_stmt_step(&q) ){
    char const * zFieldName = fsl_stmt_g_text(&q, 1, NULL);
    if(!zFieldName){
      rc = FSL_RC_OOM;
      break;
    }
    f->ticket.hasTicket = 1;
    if( 0==memcmp(zFieldName,"tkt_", 4)){
      if( 0==fsl_strcmp(zFieldName,"tkt_ctime")) f->ticket.hasCTime = 1;
      /* These are core field names, part of every fossil ticket
         table. */
      continue;
    }
    jc = fsl_card_J_malloc(0, zFieldName, NULL);
    if(!jc){
      rc = FSL_RC_OOM;
      break;
    }
    jc->flags = FSL_CARD_J_TICKET;
    rc = fsl_list_append(li, jc);
    if(rc){
      fsl_card_J_free(jc);
      break;
    }
  }
  fsl_stmt_finalize(&q);
  if(rc) goto end;

  rc = fsl_cx_prepare(f, &q, "PRAGMA table_info(ticketchng)");
  if(!rc) while( FSL_RC_STEP_ROW==fsl_stmt_step(&q) ){
    char const * zFieldName = fsl_stmt_g_text(&q, 1, NULL);
    if(!zFieldName){
      rc = FSL_RC_OOM;
      break;
    }
    f->ticket.hasChng = 1;
    if( 0==memcmp(zFieldName,"tkt_", 4)){
      if( 0==fsl_strcmp(zFieldName,"tkt_rid")) f->ticket.hasChngRid = 1;
      /* These are core field names, part of every fossil ticketchng
         table. */
      continue;
    }
    if( (i=fsl__tkt_field_id(li, zFieldName)) >= 0){
      jc = (fsl_card_J*)li->list[i];
      jc->flags |= FSL_CARD_J_CHNG;
      continue;
    }
    jc = fsl_card_J_malloc(0, zFieldName, NULL);
    if(!jc){
      rc = FSL_RC_OOM;
      break;
    }
    jc->flags = FSL_CARD_J_CHNG;
    rc = fsl_list_append(li, jc);
    if(rc){
      fsl_card_J_free(jc);
      break;
    }
  }
  fsl_stmt_finalize(&q);
  end:
  if(!rc){
    fsl_list_sort(li, fsl__qsort_cmp_J_cards);
  }
  return rc;
}

static int fsl__ticket_insert(fsl_deck * const d, fsl_id_t tktId,
                              fsl_id_t * const tgtId){
  /* Derived from fossil(1) tkt.c:ticket_insert() */;
  fsl_cx * const f = d->f;
  fsl_id_t const rid = d->rid;
  int rc = 0;
  fsl_buffer * const sql1 = fsl__cx_scratchpad(f);
  fsl_buffer * const sql2 = fsl__cx_scratchpad(f);
  fsl_buffer * const sql3 = fsl__cx_scratchpad(f);
  fsl_db * const db = fsl_cx_db_repo(f);
  fsl_list const * const cf = &f->ticket.customFields;
  fsl_size_t i;
  //char const * zMimetype = NULL;
  fsl_stmt q = fsl_stmt_empty;
  char aUsed[cf->used];
  assert(rid>0 && f!=NULL && db);
  if(0==tktId){
    rc = fsl_cx_exec_multi(f, "INSERT INTO ticket(tkt_uuid, tkt_mtime) "
                           "VALUES(%Q, 0)", d->K);
    if(rc) goto end;
    tktId = fsl_db_last_insert_id(db);
  }
  rc = fsl_buffer_append(sql1, "UPDATE OR REPLACE ticket SET tkt_mtime=?1", -1);
  if(0==rc && f->ticket.hasCTime){
    rc = fsl_buffer_append(sql1, ", tkt_ctime=coalesce(tkt_ctime,?1)", -1);
  }
  if(rc) goto end;
  memset(aUsed, 0, cf->used);
  for(i = 0; 0==rc && i < d->J.used; ++i){
    fsl_card_J const * const dJC = (fsl_card_J*)d->J.list[i];
    int const j = fsl__tkt_field_id(cf, dJC->field);
    if(j<0){
      /* Ticket has a field which this repo does not have. Skip it. */
      continue;
    }
    aUsed[j] = FSL_CARD_J_TICKET;
    fsl_card_J const * const rJC = (fsl_card_J*)cf->list[j];
    if(rJC->flags & FSL_CARD_J_TICKET){
      if(dJC->append){
        rc = fsl_buffer_appendf(sql1, ", %!Q=coalesce(%!Q,'') || %Q",
                                dJC->field, dJC->field, dJC->value);
      }else{
        rc = fsl_buffer_appendf(sql1, ", %!Q=%Q",
                                dJC->field, dJC->value);
      }
      if(rc) break;
    }
    if(rJC->flags & FSL_CARD_J_CHNG){
      rc = fsl_buffer_appendf(sql2, ",%!Q", dJC->field);
      if(0==rc) rc = fsl_buffer_appendf(sql3, ",%Q", dJC->value);
      if(rc) break;
    }
#if 0
    if(0==fsl_strcmp(dJC->field, "mimetype")){
      zMimetype = dJC->value;
    }
#endif
  }
  if(rc) goto end;
  /* MISSING: a block from fossil(1) tkt.c which extracts backlinks:

  if( rid>0 ){
    for(i=0; i<p->nField; i++){
      const char *zName = p->aField[i].zName;
      const char *zBaseName = zName[0]=='+' ? zName+1 : zName;
      j = fieldId(zBaseName);
      if( j<0 ) continue;
      backlink_extract(p->aField[i].zValue, zMimetype, rid, BKLNK_TICKET,
                       p->rDate, i==0);
    }
  }

  That's not critical for core ticket functionality.
  */
  rc = fsl_buffer_appendf(sql1, " WHERE tkt_id=%" FSL_ID_T_PFMT, tktId);
  if(rc) goto end;
  rc = fsl_cx_prepare(f, &q, "%b", sql1);
  if(rc) goto end;
  rc = fsl_stmt_bind_step(&q, "f", d->D);
  fsl_stmt_finalize(&q);
  if(rc) goto end;
  fsl_buffer_reuse(sql1);
  if(f->ticket.hasChngRid || sql2->used){
    bool fromTkt = false;
    if(f->ticket.hasChngRid){
      rc = fsl_buffer_append(sql2, ",tkt_rid", -1);
      if(0==rc) rc = fsl_buffer_appendf(sql3, ",%" FSL_ID_T_PFMT, d->rid);
      if(rc) goto end;
    }
    for(i = 0; 0==rc &&  i < cf->used; ++i){
      fsl_card_J const * const rJC = (fsl_card_J*)cf->list[i];
      if(0==aUsed[i] && (rJC->flags & FSL_CARD_J_BOTH)==FSL_CARD_J_BOTH){
        fromTkt = true;
        rc = fsl_buffer_appendf(sql2, ",%!Q", rJC->field);
        if(0==rc) rc = fsl_buffer_appendf(sql3, ",%!Q", rJC->field);
      }
    }
    if(rc) goto end;
    if(fromTkt){
      rc = fsl_cx_prepare(f, &q, "INSERT INTO ticketchng(tkt_id,tkt_mtime%b)"
                          "SELECT %"FSL_ID_T_PFMT",?1%b "
                          "FROM ticket WHERE tkt_id=%"FSL_ID_T_PFMT,
                          sql2, tktId, sql3, tktId);
    }else{
      rc = fsl_cx_prepare(f, &q, "INSERT INTO ticketchng(tkt_id,tkt_mtime%b)"
                          "VALUES(%"FSL_ID_T_PFMT",?1%b)",
                          sql2, tktId, sql3);
    }
    if(0==rc) rc = fsl_stmt_bind_step(&q, "f", d->D);
  }
  end:
  fsl_stmt_finalize(&q);
  fsl__cx_scratchpad_yield(f, sql1);
  fsl__cx_scratchpad_yield(f, sql2);
  fsl__cx_scratchpad_yield(f, sql3);
  *tgtId = tktId;
  return rc;
}

static int fsl__ticket_timeline_entry(fsl_deck * const d, bool isNew, fsl_id_t tagId){
  /* Derived from fossil(1) manifest.c:mainfest_ticket_event() */;
  int rc;
  fsl_buffer * const comment = fsl__cx_scratchpad(d->f);
  fsl_buffer * const brief = fsl__cx_scratchpad(d->f);
  char * zTitle = 0;
  char * zNewStatus = 0;
  fsl_db * const db = fsl_cx_db_repo(d->f);
  fsl_cx * const f = d->f;
  if(!f->ticket.titleColumn){
    assert(!f->ticket.statusColumn);
    rc = fsl_db_get_text(db, &f->ticket.titleColumn, NULL,
             "SELECT coalesce("
             "(SELECT value FROM config WHERE name='ticket-title-expr'),"
             "'title')");
    if(0==rc){
      rc = fsl_db_get_text(db, &f->ticket.statusColumn, NULL,
               "SELECT coalesce("
               "(SELECT value FROM config WHERE name='ticket-status-column'),"
               "'status')");
    }
    if(rc) return fsl_cx_uplift_db_error( f, db );
  }
  rc = fsl_db_get_text(db, &zTitle, NULL,
                       "SELECT coalesce(%!Q,'unknown') "
                       "FROM ticket WHERE tkt_uuid=%Q",
                       f->ticket.titleColumn, d->K);
  if(rc){
    fsl_cx_uplift_db_error(d->f, db);
    goto end;
  }
  if(isNew){
    rc = fsl_buffer_appendf(comment, "New ticket [%!S|%S] <i>%h</i>.",
                            d->K, d->K, zTitle);
    if(0==rc){
      rc = fsl_buffer_appendf(brief, "New ticket [%!S|%S].",
                              d->K, d->K);
    }
    if(rc) goto end;
  }else{
    // Update an existing ticket...
    char * zNewStatus = 0;
    for(fsl_size_t i = 0; i < d->J.used; ++i){
      fsl_card_J const * const jc = (fsl_card_J*)d->J.list[i];
      if(0==fsl_strcmp(jc->field, f->ticket.statusColumn)){
        zNewStatus = jc->value;
        break;
      }
    }
    if(zNewStatus){
      rc = fsl_buffer_appendf(comment, "%h ticket [%!S|%S]: <i>%h</i>",
                              zNewStatus, d->K, d->K, zTitle);
      if(!rc && d->J.used>1){
        rc = fsl_buffer_appendf(comment, " plus %d other change%s",
                                (int)d->J.used-1, d->J.used==2 ? "" : "s");
      }
      if(0==rc) rc = fsl_buffer_appendf(brief, "%h ticket [%!S|%S].",
                                        zNewStatus, d->K, d->K);
      if(rc) goto end;
    }else{
      rc = fsl_db_get_text(db, &zNewStatus, NULL,
                           "SELECT coalesce(%!Q,'unknown') "
                           "FROM ticket WHERE tkt_uuid=%Q",
                           f->ticket.statusColumn, d->K);
      if(rc){
        rc = fsl_cx_uplift_db_error2(f, db, rc);
        goto end;
      }
      rc = fsl_buffer_appendf(comment, "Ticket [%!S|%S] <i>%h</i> "
                              "status still %h with %d other change%s",
                              d->K, d->K, zTitle, zNewStatus, (int)d->J.used,
                              1==d->J.used ? "" : "s");
      fsl_free(zNewStatus);
      if(rc) goto end;
      rc = fsl_buffer_appendf(brief, "Ticket [%!S|%S]: %d change%s",
                              d->K, d->K, (int)d->J.used,
                              1==d->J.used ? "" : "s");
      if(rc) goto end;
    }
  }
  assert(0==rc);
  // MISSING: manifest_create_event_triggers()
  rc = fsl_cx_exec(d->f,
                   "REPLACE INTO event"
                   "(type, tagid, mtime, objid, user, comment, brief) "
                   "VALUES('t', %"FSL_ID_T_PFMT", %"FSL_JULIAN_T_PFMT", "
                   "%"FSL_ID_T_PFMT",%Q,%B,%B)",
                   tagId, d->D, d->rid, d->U, comment, brief);
  end:
  fsl_free(zTitle);
  fsl_free(zNewStatus);
  fsl__cx_scratchpad_yield(d->f, comment);
  fsl__cx_scratchpad_yield(d->f, brief);
  return rc;
}

int fsl__ticket_rebuild(fsl_cx * const f, char const * zTktKCard){
  int rc;
  fsl_id_t tktId;
  fsl_id_t tagId;
  fsl_db * const db = fsl_needs_repo(f);
  fsl_stmt q = fsl_stmt_empty;
  if(!db) return FSL_RC_NOT_A_REPO;
  assert(!f->cache.isCrosslinking);
  rc = fsl__cx_ticket_load_fields(f, false);
  if(rc) goto end;
  else if(!f->ticket.hasTicket) return 0;
  char * const zTag = fsl_mprintf("tkt-%s", zTktKCard);
  if(!zTag){
    rc = FSL_RC_OOM;
    goto end;
  }
  tagId = fsl_tag_id(f, zTag, true);
  fsl_free(zTag);
  if(tagId<0){
    rc = f->error.code;
    assert(0!=rc);
    goto end;
  }
  tktId = fsl_db_g_id(db, 0, "SELECT tkt_id FROM ticket "
                      "WHERE tkt_uuid=%Q", zTktKCard);
  if(tktId>0){
    if(f->ticket.hasChng){
      rc = fsl_cx_exec(f, "DELETE FROM ticketchng "
                       "WHERE tkt_id=%" FSL_ID_T_PFMT,
                       tktId);
    }
    if(!rc) rc = fsl_cx_exec(f, "DELETE FROM ticket "
                             "WHERE tkt_id=%" FSL_ID_T_PFMT,
                             tktId);
    if(rc) goto end;
  }
  tktId = 0;
  rc = fsl_cx_prepare(f, &q, "SELECT rid FROM tagxref "
                      "WHERE tagid=%" FSL_ID_T_PFMT
                      " ORDER BY mtime", tagId);
  int counter = 0;
  /* Potential TODO (fossil does not do this):
     DELETE FROM EVENT WHERE tagid=${tagId} */
  while(0==rc && FSL_RC_STEP_ROW==fsl_stmt_step(&q)){
    fsl_deck deck = fsl_deck_empty;
    fsl_id_t const rid = fsl_stmt_g_id(&q, 0);
    rc = fsl_deck_load_rid(f, &deck, rid, FSL_SATYPE_TICKET);
    if(rc) goto outro;
    assert(deck.rid==rid);
    rc = fsl__ticket_insert(&deck, tktId, &tktId);
    if(0==rc){
      rc = fsl__ticket_timeline_entry(&deck, 0==counter++, tagId);
      if(0==rc) rc = fsl__call_xlink_listeners(&deck);
    }
    outro:
    fsl_deck_finalize(&deck);
  }
  end:
  fsl_stmt_finalize(&q);
  return rc;
}
