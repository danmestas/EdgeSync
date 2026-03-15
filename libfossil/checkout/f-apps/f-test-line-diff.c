/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code
*/
/**
   This is a template application for libfossil fcli client apps, with
   commentary explaining how to do various common things with the
   API. Copy/paste this and modify it to suit.
*/
#include "libfossil.h"

// Only for testing/debugging..
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

// Global app state.
struct App_ {
  char const * lineLHS;
  char const * lineRHS;
} App = {
NULL,//lineLHS
NULL//lineRHS
};

static int app_stuff(void){
  fsl_dline a = fsl_dline_empty;
  fsl_dline b = fsl_dline_empty;
  fsl_dline_change chng = fsl_dline_change_empty;
  int i, j, x;
  a.z = App.lineLHS;
  a.n = (int)fsl_strlen(a.z);
  b.z = App.lineRHS;
  b.n = (int)fsl_strlen(b.z);
  fsl_dline_change_spans(&a, &b, &chng);
  f_out("left:  [%s]\n", a.z);
  for(i=x=0; i<chng.n; i++){
    int ofst = chng.a[i].iStart1;
    int len = chng.a[i].iLen1;
    if( len ){
      if( x==0 ){ f_out("%*s", 8, ""); }
      while( ofst > x ){
        if( (a.z[x]&0xc0)!=0x80 ) f_out(" ");
        x++;
      }
      for(j=0; j<len; j++, x++){
        if( (a.z[x]&0xc0)!=0x80 ) f_out("%d",i);
      }
    }
  }
  if( x ) f_out("\n");
  f_out("right: [%s]\n", b.z);
  for(i=x=0; i<chng.n; i++){
    int ofst = chng.a[i].iStart2;
    int len = chng.a[i].iLen2;
    if( len ){
      if( x==0 ){ f_out("%*s", 8, ""); }
      while( ofst > x ){
        if( (b.z[x]&0xc0)!=0x80 ) f_out(" ");
        x++;
      }
      for(j=0; j<len; j++, x++){
        if( (b.z[x]&0xc0)!=0x80 ) f_out("%d",i);
      }
    }
  }
  if( x ) f_out("\n");
  return 0;
}

int main(int argc, const char * const * argv ){
  const fcli_cliflag FCliFlags[] = {
    fcli_cliflag_empty_m // list MUST end with this (or equivalent)
  };
  const fcli_help_info FCliHelp = {
    "Tests diff calculation between two lines "
    "using the 2021-era diff APIs.",
    "lineLHS lineRHS",
    NULL
  };
  fcli.config.checkoutDir = NULL; // same effect as global -C flag.

  int rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(rc) goto end;

  App.lineLHS = fcli_next_arg(true);
  App.lineRHS = fcli_next_arg(true);
  if(!App.lineRHS){
    rc =fcli_err_set(FSL_RC_MISUSE,
                     "Expecting two one-line strings to diff.");
    goto end;
  }

  if((rc=fcli_has_unused_args(false))) goto end;

  rc = app_stuff();

  end:
  return fcli_end_of_main(rc);
}
