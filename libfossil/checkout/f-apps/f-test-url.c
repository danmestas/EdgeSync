/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2025 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code
*/
#include "libfossil.h"

// Only for testing/debugging..
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

// Global app state.
struct App_ {
  int32_t dummy;
} App = {
0
};

static void dump_url(fsl_url const * u){
#define out(M,FMT) f_out("u->%-10s = " FMT "\n", #M, u->M)
#define outS(M) if( u->M ) { out(M,"%s"); } (void)0
#define outI(M) if( u->M > 0 ) {out(M,"%d"); } (void)0
  f_out("************* %s\n", u->raw);
  outS(scheme);
  outS(username);
  outS(password);
  outS(host);
  outI(port);
  outS(path);
  outS(query);
  outS(fragment);
#undef out
}

static int app_stuff(void){
  int rc = 0;
  fsl_url u = fsl_url_empty;
  unsigned int n = 0;
  char const * z = 0;

#define check2(DUMP,URL)             \
  rc = fsl_url_parse(&u, (z = URL)); \
  if( rc ) {rc = fcli_err_set(rc, "Parsing URL failed: %s\n", z);  goto end; } \
  if(DUMP) dump_url(&u)
#define check(URL) check2(1,URL)

  while( (z = fcli_next_arg(true)) ){
    ++n;
    check(z);
  }
  if( n ){
    assert(0==rc);
    goto end;
  }

  check2(0, "file:///foo.bar/baz");
  assert( !u.host );
  assert( 0==fsl_strcmp(u.path, "/foo.bar/baz") );
  assert( 0==fsl_strcmp(u.scheme, "file") );

  check2(0, "file://foo.bar/baz");
  assert( !u.host );
  assert( 0==fsl_strcmp(u.path, "foo.bar/baz") );

  check2(0, "file:/foo.bar/baz");
  assert( !u.host );
  assert( 0==fsl_strcmp(u.path, "/foo.bar/baz") );

  check2(0, "file:foo.bar/baz");
  assert( !u.host );
  assert( 0==fsl_strcmp(u.path, "foo.bar/baz") );

  check2(0, "./foo.bar/baz");
  assert( !u.host );
  assert( 0==fsl_strcmp(u.path, "./foo.bar/baz") );
  assert( 0==fsl_strcmp(u.scheme, "file") );

  check2(0, "/foo.bar/baz");
  assert( !u.host );
  assert( 0==fsl_strcmp(u.path, "/foo.bar/baz") );
  assert( 0==fsl_strcmp(u.scheme, "file") );

  if( !n ){
    char const * aList[] = {
      "/foo/bar",
      "file:/foo/bar",
      "file://foo/bar",
      "file:///foo/bar",
      "http://foo.com",
      "http://foo.com:70",
      "http://foo.com:70/",
      "http://foo.com:70?x=y&z=a#fragment",
      "http://foo.com:70#fragment",
      "https://rando:pw@foo.com:70?x=y&z=a#fragment",
      "https://www.thingiverse.com/thing:7011381",
      "file:///c:/foo/bar.baz",
      "file:/c:/foo/bar.baz",
      "file:c:/foo/bar.baz"
    };
    for( size_t i = 0; i < sizeof(aList)/sizeof(aList[0]); ++i ){
      check(aList[i]);
    }
  }

#undef check
#undef check2
end:
  fsl_url_cleanup(&u);
  return rc;
}

int main(int argc, const char * const * argv ){
  const fcli_cliflag FCliFlags[] = {
    fcli_cliflag_empty_m
  };
  const fcli_help_info FCliHelp = {
    "Test app for the fsl_url API",
    "[urls...]",
    NULL
  };
  fcli.config.checkoutDir = NULL;
  int rc = fcli_setup_v2(argc, argv, FCliFlags, &FCliHelp);
  if(0==rc){
    rc = app_stuff();
  }
  return fcli_end_of_main(rc);
}
