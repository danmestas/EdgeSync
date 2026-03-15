/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  Copyright 2025 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/
/*
  This file houses the fsl_url APIs.
*/

#include "fossil-scm/repo.h"
#include "fossil-scm/internal.h"
#include <assert.h>
#include <string.h> /* strcspn() */

#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

const fsl_url fsl_url_empty = fsl_url_empty_m;

static inline bool fsl__is_scheme_ch(int i){
  return fsl_isalpha(i);
  //&& i!=(int)'-' && i!=(int)'+' && i!=(int)'.';
}

#ifndef NDEBUG
static void fsl__url_dump(fsl_url const * u){
#define out(M,FMT) MARKER(("u->%-10s = " FMT "\n", #M, u->M))
#define outS(M) if( u->M ) { out(M,"%s"); } (void)0
#define outI(M) if( u->M > 0 ) {out(M,"%d"); } (void)0
  MARKER(("URL: %s\n", u->raw));
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
#endif

void fsl_url_cleanup2(fsl_url * const u, bool clearBuffer){
  if( clearBuffer ){
    fsl_buffer_clear(&u->impl.b);
    *u = fsl_url_empty;
  }else{
    fsl_buffer const x = u->impl.b;
    *u = fsl_url_empty;
    u->impl.b = x;
    fsl_buffer_reuse(&u->impl.b);
  }
}

void fsl_url_cleanup(fsl_url * const u){
  fsl_url_cleanup2(u, true);
}

int fsl_url_parse(fsl_url * const u, char const * z){
  int rc = 0;
  enum ParseState {
    e_Start, e_Slashes, e_UserOrHost, e_Host, e_Port,
    e_Path, e_Query, e_Fragment, e_Done,
    e_Err
  };
  fsl_buffer * const b = &u->impl.b;
  char const *zLeft = z;
  fsl_size_t const nZ = fsl_strlen(z);
  char const * const zEnd = z + nZ;
  enum ParseState state = e_Start;
  bool isFile = false;

  fsl_url_cleanup2(u, false);
  fsl_buffer_reserve(b,
                     nZ * 2
                     + (9 /*NUL bytes*/)
                     + (('.'==*z || '/'==*z)
                        ? 5 /*synthetic "file\0" scheme*/
                        : 0))
    /* Must be large enough that we will not have to allocate
       again. */;
  fsl_buffer_append( b, z, (fsl_int_t)nZ );
  fsl_buffer_appendch(b, 0);
  if( b->errCode ) goto oom;
  u->raw = (char const *)b->mem;

#define bstr ((char const *)b->mem + b->used)
#define checkOom(P) if(!(P)) goto oom
#define setstr(MEMBER)                                 \
  assert( z>=zLeft );                                  \
  assert( b->capacity > (b->used + (z - zLeft)) && "Else buffer is too small" ); \
  u->MEMBER = bstr;                                    \
  fsl_buffer_append(b, zLeft, (fsl_int_t)(z - zLeft)); \
  fsl_buffer_appendch(b, 0);                           \
  if( b->errCode ) goto end

#define checkEnd(nEXTRA) if( (z + nEXTRA)>=zEnd ) goto syntax_err

start:
  assert( e_Done!=state );
  switch( (rc=b->errCode) ? e_Err : state ){
    case e_Err: break;

    case e_Start:
      if( '/' == *z || '.' == *z ){
        u->scheme = bstr;
        fsl_buffer_append(b, "file\0", 5);
        state = e_Slashes;
        isFile = true;
        break;
      }
      for( zLeft = z; z<zEnd && *z; ++z ){
        if( ':'==*z ){
          isFile = (4 == (z - zLeft))
            && 0==memcmp("file", zLeft, 4);
          setstr(scheme);
          ++z;
          state = e_Slashes;
          break;
        }else if( !fsl__is_scheme_ch(*z) ){
          goto syntax_err;
        }
      }
      checkEnd(0);
      break;

    case e_Slashes: {
      int nSlash = 0;
      checkEnd(0);
      for( zLeft = z; z+nSlash<zEnd && '/'==z[nSlash]; ++nSlash ){}
      checkEnd(nSlash);
      switch( nSlash ) {
        case 0:
        case 1:
          if( isFile ){
            z = zEnd;
            setstr(path);
            state = e_Done;
            break;
          }
          goto syntax_err;
        case 2:
          zLeft += nSlash;
          if( isFile ){
            z = zEnd;
            setstr(path);
            state = e_Done;
          }else{
            state = e_UserOrHost;
            z = zLeft;
          }
          break;
        default:
          if( 3==nSlash && isFile ){
            zLeft += 2;
            z = zEnd;
            setstr(path);
            state = e_Done;
            break;
          }
          goto syntax_err;
      }
      break;
    }

    case e_UserOrHost:{
      char const * zAt = strchr(z, '@');
      //MARKER(("state=%d zAt=%s\n",state, zAt));
      if( zAt ){
        if( zAt==z ) goto syntax_err;
        size_t const nColon = strcspn(z, ":");
        if( nColon && (z+nColon < zAt) ){ /* X:Y@Z */
          zLeft = z;
          z = z + nColon;
          setstr(username);
          zLeft = ++z;
          z = zAt;
          setstr(password);
          zLeft = ++z;
        }else{ /* X@Y */
          z = zAt;
          setstr(username);
          zLeft = ++z;
        }
      }
      state = e_Host;
      break;
    }

    case e_Host:{
      size_t const n = strcspn(z, "/:?");
      if( n ){
        zLeft = z;
        z += n;
        setstr(host);
        state = (':'==*z)
          ? e_Port
          : (('?'==*z) ? e_Query : e_Path);
      }else{
        zLeft = z;
        z = zEnd;
        setstr(host);
        state = e_Done;
      }
      //MARKER(("state=%d z=%s\n",state, z));
      break;
    }

    case e_Port:{
      if( ':'==*z ){
        zLeft = ++z;
        assert( 0==u->port );
        if( (1==sscanf(z, "%" SCNu16, &u->port)
             || 1==sscanf(z, "%" SCNu16 "/", &u->port))
            && u->port>0
            && u->port<=65535){
          while( z<zEnd && *z>='0' && *z<='9' ) ++z;
          state = (z==zEnd) ? e_Done : e_Path;
        }else{
          goto syntax_err;
        }
      }else{
        state = e_Path;
      }
      break;
    }

    case e_Path: {
      if( z>=zEnd ){
        assert( zLeft<=z );
        state = e_Done;
      }else{
        size_t const n = strcspn(z, "?#");
        if( n ){
          zLeft = z;
          z += n;
          setstr(path);
        }
        state = (z<zEnd) ? e_Query : e_Done;
      }
      break;
    }

    case e_Query:{
      assert( z<zEnd );
      assert( '?'==*z || '#'==*z );
      if( '#'==*z ){
        state = e_Fragment;
      }else{
        size_t const n = strcspn(z, "#");
        if( n ){
          zLeft = z;
          z += n;
          setstr(query);
          state = (z < zEnd) ? e_Fragment : e_Done;
        }else{
          /* Impossible? */
          goto syntax_err;
        }
      }
      break;
    }

    case e_Fragment:{
      assert(z<zEnd && '#'==*z);
      zLeft = z;
      z = zEnd;
      setstr(fragment);
      state = e_Done;
      break;
    }

    case e_Done:
      fsl__fatal(FSL_RC_CANNOT_HAPPEN, "This is impossible");
  }
#undef setstr
#undef checkEnd
#undef checkOom
#undef bstr

  if( e_Start==state ) goto syntax_err;
  if( 0==rc && e_Done!=state ){
    goto start;
  }

end:

  if( 0==rc ){
#ifndef NDEBUG
    if(0) fsl__url_dump(u);
#endif
    if( !u->scheme || !*u->scheme ) goto syntax_err;
    if( isFile ){
      assert( !u->host );
      if( !u->path || !*u->path ) goto syntax_err;
    }else{
      if( !u->host || !*u->host ) goto syntax_err;
    }
    assert( u->port >= 0 && u->port<=65535 );
    // TODO? Validate that each part contains only valid characters.
  }

  if( rc ){
    fsl_url_cleanup2(u, true);
  }
  return rc;

oom:
  rc = FSL_RC_OOM;
  goto end;

syntax_err:
  rc = FSL_RC_SYNTAX;
  goto end;
}

int fsl_url_render(fsl_url const * const u, fsl_buffer * const b, int flags){
  int rc = b->errCode;
  if( rc ) return rc;

#define ba(piece,N) if(piece) fsl_buffer_append( b, piece, N )
#define bac(CH) fsl_buffer_appendch( b, CH )
#define baf fsl_buffer_appendf
  if( 0==(flags & FSL_URL_RENDER_NO_SCHEME) ){
    ba(u->scheme, -1);
    ba("://", 3);
  }
  if( 0==(flags & FSL_URL_RENDER_NO_USER)
      && u->username ){
    ba(u->username, -1);
    if( u->password && 0==(FSL_URL_RENDER_NO_PASSWORD & flags) ){
      if( FSL_URL_RENDER_MASK_PASSWORD & flags ){
        ba(":****",5);
      }else{
        ba(u->password, -1);
      }
    }
    bac('@');
  }
  ba(u->host, -1);
  if( u->port>0 ){
    /* TODO: elide default ports for http(s) and ssh. */
    baf(b, ":%d", u->port);
  }
  ba(u->path,-1);
  if( 0==(flags & FSL_URL_RENDER_NO_QUERY) ){
    ba(u->query,-1);
    if( 0==(flags & FSL_URL_RENDER_NO_FRAGMENT) ){
      ba(u->fragment,-1);
    }
  }
#undef ba
#undef bac
#undef baf
  return b->errCode;
}

void fsl_url_swap(fsl_url * lhs, fsl_url *rhs){
  if( lhs!=rhs ){
    fsl_url const tmp = *lhs;
    *lhs = *rhs;
    *rhs = tmp;
  }
}

bool fsl_url_next_query_arg(fsl_url const * u, char const **zCursor,
                            fsl_buffer * bKey, fsl_buffer * bVal){
  char const * zBegin = *zCursor ? *zCursor : u->query;
  char const * const zEnd = u->fragment
    ? u->fragment : (char const *)(u->impl.b.mem + u->impl.b.used);
  fsl_buffer_reuse(bKey);
  fsl_buffer_reuse(bVal);
  if( !zBegin || zBegin>=zEnd ) return false;
  if( !*zCursor && '?'==*zBegin ) ++zBegin;
  char const * z = zBegin;
  for( ; z<zEnd && *z && *z=='&'; ++z ){/*Leading & from prev run*/}
  zBegin = z;
  for( ; z<zEnd && *z && *z!='&' && *z!='='; ++z ){/* next '=' or '&' */}
  if( z==zBegin ) return false;
  if( 0!=fsl_buffer_appendf(bKey, "%.*T", (int)(z-zBegin), zBegin) ){
    return false;
  }
  if( '='==*z ){
    char const * x = ++z;
    for( ; z<zEnd && *z && *z!='&'; ++z ){}
    if( z>x ){
      if( 0!=fsl_buffer_appendf(bVal, "%.*T", (int)(z - x), x) ){
        return false;
      }
    }
  }
  *zCursor = z;
  return true;
}

#undef MARKER
