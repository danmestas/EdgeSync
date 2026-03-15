/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
 * Copyright 2022 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt
 *
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 * SPDX-FileCopyrightText: 2021 The Libfossil Authors
 * SPDX-FileType: Code
 *
 * Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
 */

/*
 * This file implements the files-of-checkin (foci) API used to construct a
 * SQLite3 virtual table via a table-valued function to aggregate all files
 * pertaining to a specific check-in. This table is used in repository
 * queries such as listing all files belonging to a specific version.
 *
 * Usage (from fossil(1) /src/foci.c:24):
 *
 *    SELECT * FROM fsl_foci('trunk');
 *
 * temp.foci table schema:
 *
 *     CREATE TABLE fsl_foci(
 *       checkinID    INTEGER,    -- RID for the check-in manifest
 *       filename     TEXT,       -- Name of a file
 *       uuid         TEXT,       -- hash of the file
 *       previousName TEXT,       -- Name of the file in previous check-in
 *       perm         TEXT,       -- Permissions on the file
 *       symname      TEXT HIDDEN -- Symbolic name of the check-in.
 *     );
 *
 * The hidden symname column is (optionally) used as a query parameter to
 * identify the particular check-in to parse.  The checkinID parameter
 * (such is a unique numeric RID rather than symbolic name) can also be used
 * to identify the check-in.  Example:
 *
 *    SELECT * FROM fsl_foci
 *     WHERE checkinID=fsl_sym2rid('trunk');
 *
 */
#include "fossil-scm/core.h"
#include "fossil-scm/repo.h"
#include "fossil-scm/internal.h"
#include <string.h>/*memset()*/
#include <assert.h>

/* Only for debugging */
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

enum {
FOCI_CHECKINID = 0,
FOCI_FILENAME = 1,
FOCI_UUID = 2,
FOCI_PREVNAME = 3,
FOCI_PERM = 4,
FOCI_SYMNAME = 5
};

typedef struct FociCursor FociCursor;
struct FociCursor {
  sqlite3_vtab_cursor base; /* Base class - must be first */
  fsl_deck d;           /* Current manifest */
  const fsl_card_F *cf;  /* Current file */
  int idx;                /* File index */
};

typedef struct FociTable FociTable;
struct FociTable {
  sqlite3_vtab base;        /* Base class - must be first */
  fsl_cx * f;               /* libfossil context */
};

/*
 * The schema for the virtual table:
 */
static const char zFociSchema[] =
  " CREATE TABLE fsl_foci("
  "  checkinID    INTEGER,    -- RID for the check-in manifest\n"
  "  filename     TEXT,       -- Name of a file\n"
  "  uuid         TEXT,       -- hash of the file\n"
  "  previousName TEXT,       -- Name of the file in previous check-in\n"
  "  perm         TEXT,       -- Permissions on the file\n"
  "  symname      TEXT HIDDEN -- Symbolic name of the check-in\n"
  " );";

/*
 * Connect to or create a foci virtual table.
 */
static int fociConnect(
  sqlite3 *db,
  void *pAux /*a (fsl_cx*) */,
  int argc fsl__unused,
  const char * const * argv fsl__unused,
  sqlite3_vtab **ppVtab,
  char **pzErr fsl__unused
){
  FociTable *pTab;
  int rc = SQLITE_OK;

  (void)argc;
  (void)argv;
  (void)pzErr;
  pTab = (FociTable *)sqlite3_malloc(sizeof(FociTable));
  if( !pTab ){
    return SQLITE_NOMEM;
  }
  memset(pTab, 0, sizeof(FociTable));
  rc = sqlite3_declare_vtab(db, zFociSchema);
  if( rc==SQLITE_OK ){
    pTab->f = (fsl_cx*)pAux;
    *ppVtab = &pTab->base;
  }
  return rc;
}

/*
 * Disconnect from or destroy a focivfs virtual table.
 */
static int fociDisconnect(sqlite3_vtab *pVtab){
  sqlite3_free(pVtab);
  return SQLITE_OK;
}

/*
 * Available scan methods:
 *
 *   (0)     A full scan.  Visit every manifest in the repo.  (Slow)
 *   (1)     checkinID=?.  visit only the single manifest specified.
 *   (2)     symName=?     visit only the single manifest specified.
 */
static int fociBestIndex(
  sqlite3_vtab *tab fsl__unused,
  sqlite3_index_info *pIdxInfo
){
  int i;

  (void)tab;
  pIdxInfo->estimatedCost = 1000000000.0;
  for( i=0; i<pIdxInfo->nConstraint; i++ ){
    if( !pIdxInfo->aConstraint[i].usable ) continue;
    if( pIdxInfo->aConstraint[i].op==SQLITE_INDEX_CONSTRAINT_EQ
     && (pIdxInfo->aConstraint[i].iColumn==FOCI_CHECKINID
            || pIdxInfo->aConstraint[i].iColumn==FOCI_SYMNAME)
    ){
      if( pIdxInfo->aConstraint[i].iColumn==FOCI_CHECKINID ){
        pIdxInfo->idxNum = 1;
      }else{
        pIdxInfo->idxNum = 2;
      }
      pIdxInfo->estimatedCost = 1.0;
      pIdxInfo->aConstraintUsage[i].argvIndex = 1;
      pIdxInfo->aConstraintUsage[i].omit = 1;
      break;
    }
  }
  return SQLITE_OK;
}

/*
 * Open a new focivfs cursor.
 */
static int fociOpen(sqlite3_vtab *pVTab, sqlite3_vtab_cursor **ppCursor){
  FociCursor *pCsr;
  pCsr = (FociCursor *)sqlite3_malloc(sizeof(FociCursor));
  if( !pCsr ){
    return SQLITE_NOMEM;
  }
  memset(pCsr, 0, sizeof(FociCursor));
  pCsr->d = fsl_deck_empty;
  pCsr->base.pVtab = pVTab;
  *ppCursor = (sqlite3_vtab_cursor *)pCsr;
  return SQLITE_OK;
}

/*
 * Close a focivfs cursor.
 */
static int fociClose(sqlite3_vtab_cursor *pCursor){
  FociCursor *pCsr = (FociCursor *)pCursor;
  fsl_deck_finalize(&pCsr->d);
  sqlite3_free(pCsr);
  return SQLITE_OK;
}

/*
 * Move a focivfs cursor to the next F card entry in the deck. If this fails,
 * pass the vtab cursor to fociClose and return the failing result code.
 */
static int fociNext(sqlite3_vtab_cursor *pCursor){
  int rc = SQLITE_OK;

  FociCursor *pCsr = (FociCursor *)pCursor;
  rc = fsl_deck_F_next(&pCsr->d, &pCsr->cf);
  if( !rc ){
    pCsr->idx++;
  }else{
    fociClose(pCursor);
  }
  return rc;
}

static int fociEof(sqlite3_vtab_cursor *pCursor){
  FociCursor *pCsr = (FociCursor *)pCursor;
  return pCsr->cf==0;
}

static int fociFilter(
  sqlite3_vtab_cursor *pCursor,
  int idxNum, const char *idxStr fsl__unused,
  int argc fsl__unused, sqlite3_value **argv
){
  int rc = SQLITE_OK;
  FociCursor *const pCur = (FociCursor *)pCursor;
  fsl_cx * const f = ((FociTable*)pCur->base.pVtab)->f;

  (void)idxStr;
  (void)argc;
  fsl_deck_finalize(&pCur->d);
  if( idxNum ){
    fsl_id_t rid;
    if( idxNum==1 ){
      rid = sqlite3_value_int(argv[0]);
    }else{
      rc = fsl_sym_to_rid(f, (const char *)sqlite3_value_text(argv[0]),
       FSL_SATYPE_CHECKIN, &rid);
      if( rc ){
        goto end;
      }
    }
    rc = fsl_deck_load_rid(f, &pCur->d, rid, FSL_SATYPE_CHECKIN);
    if( rc ){
      goto end;
    }
    if( pCur->d.rid ){
      rc = fsl_deck_F_rewind(&pCur->d);
      if( !rc ){
        rc = fsl_deck_F_next(&pCur->d, &pCur->cf);
      }
      if( rc ){
        goto end;
      }
    }
  }
  pCur->idx = 0;
end:
  if( rc ){
    fsl_deck_finalize(&pCur->d);
  }
  return rc;
}

static int fociColumn(
  sqlite3_vtab_cursor *pCursor,
  sqlite3_context *ctx,
  int i
){
  FociCursor *pCsr = (FociCursor *)pCursor;
  switch( i ){
    case FOCI_CHECKINID:
      sqlite3_result_int(ctx, pCsr->d.rid);
      break;
    case FOCI_FILENAME:
      sqlite3_result_text(ctx, pCsr->cf->name, -1, SQLITE_TRANSIENT);
      break;
    case FOCI_UUID:
      sqlite3_result_text(ctx, pCsr->cf->uuid, -1, SQLITE_TRANSIENT);
      break;
    case FOCI_PREVNAME:
      sqlite3_result_text(ctx, pCsr->cf->priorName, -1, SQLITE_TRANSIENT);
      break;
    case FOCI_PERM: {
      char *perm[3] = {"l", "w", "x"};
      int i = 1;
      switch( pCsr->cf->perm ){
        case FSL_FILE_PERM_LINK:
          i = 0; break;
        case FSL_FILE_PERM_EXE:
          i = 2; break;
        default:
          break;
      }
      sqlite3_result_text(ctx, perm[i], 1, SQLITE_TRANSIENT);
      break;
    }
    case FOCI_SYMNAME:
      break;
  }
  return SQLITE_OK;
}

static int fociRowid(sqlite3_vtab_cursor *pCursor, sqlite_int64 *pRowid){
  FociCursor *pCsr = (FociCursor *)pCursor;
  *pRowid = pCsr->idx;
  return SQLITE_OK;
}

int fsl__foci_register(fsl_cx * const f, fsl_db * const db){
  static sqlite3_module foci_module = {
    0,                            /* iVersion */
    fociConnect,                  /* xCreate */
    fociConnect,                  /* xConnect */
    fociBestIndex,                /* xBestIndex */
    fociDisconnect,               /* xDisconnect */
    fociDisconnect,               /* xDestroy */
    fociOpen,                     /* xOpen - open a cursor */
    fociClose,                    /* xClose - close a cursor */
    fociFilter,                   /* xFilter - configure scan constraints */
    fociNext,                     /* xNext - advance a cursor */
    fociEof,                      /* xEof - check for end of scan */
    fociColumn,                   /* xColumn - read data */
    fociRowid,                    /* xRowid - read data */
    0,                            /* xUpdate */
    0,                            /* xBegin */
    0,                            /* xSync */
    0,                            /* xCommit */
    0,                            /* xRollback */
    0,                            /* xFindMethod */
    0,                            /* xRename */
    0,                            /* xSavepoint */
    0,                            /* xRelease */
    0,                            /* xRollbackTo */
    0,                            /* xShadowName */
    0                             /* xIntegrity */
  };
  assert(f);
  assert(db);
  int rc = sqlite3_create_module(db->dbh, "fsl_foci",
                                 &foci_module, f);
  return fsl__db_errcode(db, rc);
}

#undef MARKER
