/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2025 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2025 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/
/**
  This file contains the fsl_msg pieces of the API.
*/
#include "fossil-scm/core.h"
#include "fossil-scm/internal.h"
#include <assert.h>

const fsl_msg fsl_msg_empty = fsl_msg_empty_m;

const fsl_msg_listener fsl_msg_listener_empty =
  fsl_msg_listener_empty_m;

int fsl_msg_emit(fsl_msg const * msg,
                 fsl_msg_listener const * where){
  if( where && where->callback ){
    return where->callback(msg, where->state);
  }
  return 0;
}

int fsl_cx_emit(fsl_cx * const f, fsl_msg_e type, void const * p){
  if( f->cxConfig.listener.callback ){
    const fsl_msg msg = {
      .type = type, .payload = p, .f = f
    };
    return f->cxConfig.listener.callback(
      &msg, f->cxConfig.listener.state
    );
  }
  return 0;
}

static inline void const * fsl__msg_strbuf(fsl_msg_e type,
                                              fsl_buffer const * const b){
  switch( type & FSL_MSG_mask_type ){
    case FSL_MSG_type_string: return fsl_buffer_cstr(b);
    case FSL_MSG_type_buffer: return b;
    default: return 0;
  }
}

int fsl_cx_emitfv(fsl_cx *f, fsl_msg_e type, char const * zFmt, va_list args){
  int rc = 0;
  if( f->cxConfig.listener.callback ){
    switch( type & FSL_MSG_mask_type ){
      case FSL_MSG_type_string:
      case FSL_MSG_type_buffer: {
        fsl_buffer * const b = fsl_buffer_reuse(&f->scratchpads.emit);
        rc = fsl_buffer_appendfv(b, zFmt, args);
        if( 0==rc ){
          fsl_msg const m = {
            .type = type, .payload = fsl__msg_strbuf(type,b), .f = f
          };
          rc = f->cxConfig.listener.callback(&m, f->cxConfig.listener.state);
        }
        break;
      }
      default:
        assert(!"invalid message type id");
        fsl__fatal(FSL_RC_TYPE, "Invalid message type 0x%08x for %s(). "
                   "Must have type FSL_MSG_type_string|buffer",
                   type, __func__);
        break;
    }
  }
  return rc;
}

int fsl_cx_emitf(fsl_cx *f, fsl_msg_e type, char const * zFmt, ...){
  int rc = 0;
  if( f->cxConfig.listener.callback ){
    va_list args;
    va_start(args, zFmt);
    rc = fsl_cx_emitfv(f, type, zFmt, args);
    va_end(args);
  }
  return rc;
}

void fsl_cx_listener_replace(fsl_cx * f, fsl_msg_listener const *pNew,
                             fsl_msg_listener * pOld){
  if( pOld ){
    *pOld = f->cxConfig.listener;
  }
  f->cxConfig.listener = *pNew;
}
