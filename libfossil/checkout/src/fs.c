/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2013-25 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/
#ifdef _WIN32
# undef __STRICT_ANSI__ /* Needed for _wfopen */
#endif
#include "fossil-scm/internal.h"
#include "fossil-scm/util.h"

#include <assert.h>
#include <string.h> /* strlen() */
#include <stddef.h> /* NULL on linux */
#include <errno.h>
#include "xdirent.h"
#if FSL_PLATFORM_IS_WINDOWS
# if !defined(ELOOP)
#  define ELOOP 114 /* Missing in MinGW */
# endif
#else
# include <unistd.h> /* access(2), readlink(2) */
# include <sys/types.h>
# include <sys/time.h>
#endif
#include <sys/stat.h>
#include <stdlib.h> /* getenv() */

const fsl_path_splitter fsl_path_splitter_empty = fsl_path_splitter_empty_m;

/* Only for debugging */
#include <stdio.h>
#define MARKER(pfexp)                                               \
  do{ printf("MARKER: %s:%d:%s():\t",__FILE__,__LINE__,__func__);   \
    printf pfexp;                                                   \
  } while(0)

FILE *fsl_fopen(const char *zName, const char *zMode){
  FILE *f;
  if(zName && ('-'==*zName && !zName[1])){
    f = (strchr(zMode, 'w') || strchr(zMode,'+'))
      ? stdout
      : stdin
      ;
  }else{
#ifdef _WIN32
    wchar_t *uMode = (wchar_t *)fsl_utf8_to_unicode(zMode);
    wchar_t *uName = (wchar_t *)fsl_utf8_to_filename(zName);
    f = _wfopen(uName, uMode);
    fsl_os_str_free(uName);
    fsl_unicode_free(uMode);
#else
    f = fopen(zName, zMode);
#endif
  }
  return f;
}


void fsl_fclose( FILE * f ){
  if(f && (stdin!=f) && (stdout!=f) && (stderr!=f)){
    fclose(f);
  }
}

/*
   Wrapper around the access() system call.
*/
int fsl_file_access(const char *zFilename, int flags){
  /* FIXME: port in fossil(1) win32_access() */
#ifdef _WIN32
  wchar_t *zMbcs = (wchar_t *)fsl_utf8_to_filename(zFilename);
#define ACC _waccess
#else
  char *zMbcs = (char*)fsl_utf8_to_filename(zFilename);
#define ACC access
#endif
  int rc = zMbcs ? ACC(zMbcs, flags) : FSL_RC_OOM;
  if(zMbcs) fsl_os_str_free(zMbcs);
  return rc;
#undef ACC
}


int fsl_getcwd(char *zBuf, fsl_size_t nBuf, fsl_size_t * outLen){
#ifdef _WIN32
  /* FIXME: port in fossil(1) win32_getcwd() */
  char *zPwdUtf8;
  fsl_size_t nPwd;
  fsl_size_t i;
  wchar_t zPwd[2000];
  if(!zBuf) return FSL_RC_MISUSE;
  else if(!nBuf) return FSL_RC_RANGE;
  /*
    https://msdn.microsoft.com/en-us/library/windows/desktop/aa365247(v=vs.85).aspx

    It says:

    Note File I/O functions in the Windows API convert "/" to "\" as
    part of converting the name to an NT-style name, except when using
    the "\\?\" prefix as detailed in the following sections.

    So the path-demangling bits below might do more damage they
    fix?
  */
  else if( _wgetcwd(zPwd, sizeof(zPwd)/sizeof(zPwd[0])-1)==0 ){
    /* FIXME: how to determine if FSL_RC_RANGE is a better
       return value?
    */
    return FSL_RC_IO;
  }
  zPwdUtf8 = fsl_filename_to_utf8(zPwd);
  if(!zPwdUtf8) return FSL_RC_OOM;
  nPwd = strlen(zPwdUtf8);
  if( nPwd > nBuf-1 ){
    fsl_os_str_free(zPwdUtf8);
    return FSL_RC_RANGE;
  }
  for(i=0; zPwdUtf8[i]; i++) if( zPwdUtf8[i]=='\\' ) zPwdUtf8[i] = '/';
  memcpy(zBuf, zPwdUtf8, nPwd+1);
  fsl_os_str_free(zPwdUtf8);
  if(outLen) *outLen = nPwd;
  return 0;
#else
  if(!zBuf) return FSL_RC_MISUSE;
  else if(!nBuf) return FSL_RC_RANGE;
  else if( NULL==getcwd(zBuf,nBuf) ){
    return fsl_errno_to_rc(errno, FSL_RC_IO);
  }else{
    if(outLen) *outLen = fsl_strlen(zBuf);
    return 0;
  }
#endif
}

/*
   The file status information from the most recent stat() call.

   Use _stati64 rather than stat on windows, in order to handle files
   larger than 2GB.
*/
#if defined(_WIN32) && (defined(__MSVCRT__) || defined(_MSC_VER))
# undef stat
# define stat _stati64
#endif
/*
   On Windows S_ISLNK always returns FALSE.
*/
#if !defined(S_ISLNK)
# define S_ISLNK(x) (0)
#endif

/* Reminder: the semantics of the 3rd parameter are
   reversed from v1's fossil_stat().
*/
int fsl_stat(const char *zFilename, fsl_fstat * fst,
             bool derefSymlinks){
  /* FIXME: port in fossil(1) win32_stat() */
#if FSL_API_ARMOR
  if(!zFilename) return FSL_RC_MISUSE;
  else if(!*zFilename) return FSL_RC_RANGE;
#endif
  int rc;
  struct stat buf;
#if !defined(_WIN32)
  char *zMbcs = (char *)fsl_utf8_to_filename(zFilename);
  if(!zMbcs) rc = FSL_RC_OOM;
  else{
    if( derefSymlinks ){
      rc = stat(zMbcs, &buf);
    }else{
      rc = lstat(zMbcs, &buf);
    }
  }
#else
  wchar_t *zMbcs = (wchar_t *)fsl_utf8_to_filename(zFilename);
  /*trailing pathseps are forbidden in Windows stat fxns, as per doc; sigh*/
  int nzmbcslen = wcslen ( zMbcs );
  while ( nzmbcslen > 0 && ( L'\\' == zMbcs[nzmbcslen-1] ||
                             L'/' == zMbcs[nzmbcslen-1] ) ) {
    zMbcs[nzmbcslen-1] = 0;
    --nzmbcslen;
  }
  rc = zMbcs ? _wstati64(zMbcs, &buf) : FSL_RC_OOM;
#endif
  if(zMbcs) fsl_os_str_free(zMbcs);
  if(fst && (0==rc)){
    *fst = fsl_fstat_empty;
    fst->ctime = (fsl_time_t)buf.st_ctime;
    fst->mtime = (fsl_time_t)buf.st_mtime;
    fst->size = (fsl_size_t)buf.st_size;
    if(S_ISDIR(buf.st_mode)) fst->type = FSL_FSTAT_TYPE_DIR;
#if !defined(_WIN32)
    else if(S_ISLNK(buf.st_mode)) fst->type = FSL_FSTAT_TYPE_LINK;
#endif
    else /* if(S_ISREG(buf.st_mode)) */{
      fst->type = FSL_FSTAT_TYPE_FILE;
#if defined(_WIN32)
#  ifndef S_IXUSR
#    define S_IXUSR  _S_IEXEC
#  endif
      if(((S_IXUSR)&buf.st_mode)!=0){
        fst->perm |= FSL_FSTAT_PERM_EXE;
      }
#else
      if( ((S_IXUSR|S_IXGRP|S_IXOTH)&buf.st_mode)!=0 ){
        fst->perm |= FSL_FSTAT_PERM_EXE;
      }
#if 0
      /* Porting artifact: something to consider... */
      else if( g.allowSymlinks && S_ISLNK(buf.st_mode) )
        return PERM_LNK;
#endif
#endif
    }
  }else if(rc){
    rc = fsl_errno_to_rc(errno, FSL_RC_IO);
  }
  return rc;
}

fsl_int_t fsl_file_size(const char *zFilename){
  fsl_fstat fst;
  return (0 == fsl_stat(zFilename, &fst, 1))
    ? (fsl_int_t)fst.size
    : -1;
}

fsl_time_t fsl_file_mtime(const char *zFilename){
  fsl_fstat fst;
  return (0==fsl_stat(zFilename, &fst, 1))
    ? (fsl_time_t)fst.mtime
    : -1;
}


bool fsl_is_file(const char *zFilename){
  fsl_fstat fst;
  return (0==fsl_stat(zFilename, &fst, 1))
    ? (FSL_FSTAT_TYPE_FILE == fst.type)
    : false;
}

bool fsl_is_symlink(const char *zFilename){
#if FSL_PLATFORM_IS_WINDOWS
  (void)zFilename;
  return false;
#else
  fsl_fstat fst;
  return (0 == fsl_stat(zFilename, &fst, 0))
    ? (FSL_FSTAT_TYPE_LINK == fst.type)
    : false;
#endif
}

bool fsl_is_absolute_path(const char *zPath){
  if( zPath && ((zPath[0]=='/')
#if defined(_WIN32) || defined(__CYGWIN__)
      || (zPath[0]=='\\')
      || (fsl_isalpha(zPath[0]) && zPath[1]==':'
          && (zPath[2]=='\\' || zPath[2]=='/'))
#endif
    )
  ){
    return true;
  }else{
    return false;
  }
}

bool fsl_is_simple_pathname(const char *z, bool bStrictUtf8){
  int i;
  unsigned char c = (unsigned char) z[0];
  char maskNonAscii = bStrictUtf8 ? 0x80 : 0x00;
  if( c=='/' || c==0 ) return 0;
  if( c=='.' ){ /* Common cases: ./ and ../ */
    if( z[1]=='/' || z[1]==0 ) return 0;
    if( z[1]=='.' && (z[2]=='/' || z[2]==0) ) return 0;
  }
  for(i=0; (c=(unsigned char)z[i])!=0; i++){
    if( c & maskNonAscii ){
      if( (z[++i]&0xc0)!=0x80 ){
        /* Invalid first continuation byte */
        return 0;
      }
      if( c<0xc2 ){
        /* Invalid 1-byte UTF-8 sequence, or 2-byte overlong form. */
        return 0;
      }else if( (c&0xe0)==0xe0 ){
        /* 3-byte or more */
        int unicode;
        if( c&0x10 ){
          /* Unicode characters > U+FFFF are not supported.
           * Windows XP and earlier cannot handle them.
           */
          return 0;
        }
        /* This is a 3-byte UTF-8 character */
        unicode = ((c&0x0f)<<12) + ((z[i]&0x3f)<<6) + (z[i+1]&0x3f);
        if( unicode <= 0x07ff ){
          /* overlong form */
          return 0;
        }else if( unicode>=0xe000 ){
          /* U+E000..U+FFFF */
          if( (unicode<=0xf8ff) || (unicode>=0xfffe) ){
            /* U+E000..U+F8FF are for private use.
             * U+FFFE..U+FFFF are noncharacters. */
            return 0;
          } else if( (unicode>=0xfdd0) && (unicode<=0xfdef) ){
            /* U+FDD0..U+FDEF are noncharacters. */
            return 0;
          }
        }else if( (unicode>=0xd800) && (unicode<=0xdfff) ){
          /* U+D800..U+DFFF are for surrogate pairs. */
          return 0;
        }
        if( (z[++i]&0xc0)!=0x80 ){
          /* Invalid second continuation byte */
          return 0;
        }
      }
    }else if( bStrictUtf8 && (c=='\\') ){
      return 0;
    }
    if( c=='/' ){
      if( z[i+1]=='/' ) return 0;
      if( z[i+1]=='.' ){
        if( z[i+2]=='/' || z[i+2]==0 ) return 0;
        if( z[i+2]=='.' && (z[i+3]=='/' || z[i+3]==0) ) return 0;
        if( z[i+3]=='.' ) return 0;
      }
    }
  }
  if( z[i-1]=='/' ) return 0;
  return 1;
}


/*
   If the last component of the pathname in z[0]..z[j-1] is something
   other than ".." then back it out and return true.  If the last
   component is empty or if it is ".." then return false.
*/
static bool fsl_backup_dir(const char *z, fsl_int_t *pJ){
  fsl_int_t j = *pJ;
  fsl_int_t i;
  if( !j ) return 0;
  for(i=j-1; i>0 && z[i-1]!='/'; i--){}
  if( z[i]=='.' && i==j-2 && z[i+1]=='.' ) return 0;
  *pJ = i-1;
  return 1;
}



fsl_size_t fsl_file_simplify_name(char *z, fsl_int_t n_, bool slash){
  fsl_size_t i;
  fsl_size_t n = (n_<0) ? fsl_strlen(z) : (fsl_size_t)n_;
  fsl_int_t j;
  bool const hadSlash = n && (z[n-1]=='/');
  /* On windows and cygwin convert all \ characters to / */
#if defined(_WIN32) || defined(__CYGWIN__)
  for(i=0; i<n; i++){
    if( z[i]=='\\' ) z[i] = '/';
  }
#endif
  /* Removing trailing "/" characters */
  while( n>1 && z[n-1]=='/' ){--n;}

  /* Remove duplicate '/' characters.  Except, two // at the beginning
     of a pathname is allowed since this is important on windows. */
  for(i=j=1; i<n; i++){
    z[j++] = z[i];
    while( z[i]=='/' && i<n-1 && z[i+1]=='/' ) i++;
  }
  n = j;
  /* Skip over zero or more initial "./" sequences */
  for(i=0; i<n-1 && z[i]=='.' && z[i+1]=='/'; i+=2){}

  /* Begin copying from z[i] back to z[j]... */
  for(j=0; i<n; i++){
    if( z[i]=='/' ){
      /* Skip over internal "/." directory components */
      if( z[i+1]=='.' && (i+2==n || z[i+2]=='/') ){
        i += 1;
        continue;
      }

      /* If this is a "/.." directory component then back out the
         previous term of the directory if it is something other than ".."
         or "."
      */
      if( z[i+1]=='.' && i+2<n && z[i+2]=='.' && (i+3==n || z[i+3]=='/')
       && fsl_backup_dir(z, &j)
      ){
        i += 2;
        continue;
      }
    }
    if( j>=0 ) z[j] = z[i];
    j++;
  }
  if( j==0 ) z[j++] = '.';
  if(slash && hadSlash && '/'!=z[j-1]) z[j++] = '/';
  z[j] = 0;
  return (fsl_size_t)j;
}

int fsl_file_canonical_name2(const char *zRoot,
                             const char *zOrigName,
                             fsl_buffer * const pOut, bool slash){
  int rc;
  if(!zOrigName || !pOut) return FSL_RC_MISUSE;
  else if( fsl_is_absolute_path(zOrigName) || (zRoot && !*zRoot)){
    rc = fsl_buffer_append( pOut, zOrigName, -1 );
#if defined(_WIN32) || defined(__CYGWIN__)
    if(!rc){
      char *zOut;
      /*
         On Windows/cygwin, normalize the drive letter to upper case.
      */
      zOut = fsl_buffer_str(pOut);
      if( fsl_islower(zOut[0]) && zOut[1]==':' ){
        zOut[0] = fsl_toupper(zOut[0]);
      }
    }
#endif
  }else if(!zRoot){
    char zPwd[2000];
    fsl_size_t nOrig = fsl_strlen(zOrigName);
    assert(nOrig < sizeof(zPwd));
    rc = fsl_getcwd(zPwd, sizeof(zPwd)-nOrig, NULL);
    if(!rc){
#if defined(_WIN32)
      /*
         On Windows, normalize the drive letter to upper case.
      */
      if( !rc && fsl_islower(zPwd[0]) && zPwd[1]==':' ){
        zPwd[0] = fsl_toupper(zPwd[0]);
      }
#endif
      rc = fsl_buffer_appendf(pOut, "%//%/", zPwd, zOrigName);
    }
  }else{
    rc = fsl_buffer_appendf(pOut, "%/%s%/", zRoot,
                            *zRoot ? "/" : "",
                            zOrigName);
  }
  if(!rc){
    fsl_size_t const newLen = fsl_file_simplify_name(fsl_buffer_str(pOut),
                                                     (int)pOut->used, slash);
    /* Reminder to self: do NOT resize pOut to the new,
       post-simplification length because pOut is almost always a
       fsl_cx::scratchpad buffer and doing so forces all sorts of
       downstream reallocs. */
    pOut->used = newLen;
  }
  return rc;
}


int fsl_file_canonical_name(const char *zOrigName,
                            fsl_buffer * const pOut,
                            bool slash){
  return fsl_file_canonical_name2(NULL, zOrigName, pOut, slash);
}

int fsl_file_dirpart(char const * zFilename,
                     fsl_int_t nLen,
                     fsl_buffer * const pOut,
                     bool leaveSlash){
  if(!zFilename || !*zFilename || !pOut) return FSL_RC_MISUSE;
  else if(!nLen) return FSL_RC_RANGE;
  else{
    fsl_size_t n = (nLen>0) ? (fsl_size_t)nLen : fsl_strlen(zFilename);
    char const * z = zFilename + n;
    char doBreak = 0;
    if(!n) return FSL_RC_RANGE;
    else while( !doBreak && (--z >= zFilename) ){
      switch(*z){
#if defined(_WIN32)
        case '\\':
#endif
        case '/':
          if(!leaveSlash) --z;
          doBreak = 1;
          break;
      }
    }
    if(z<=zFilename){
      return (doBreak && leaveSlash)
        ? fsl_buffer_append(pOut, zFilename, 1)
        : fsl_buffer_append(pOut, "", 0) /* ensure a NUL terminator */;
    }else{
      return fsl_buffer_append(pOut, zFilename, z-zFilename + 1);
    }
  }
}

const char *fsl_file_tail(const char *z){
  const char *zTail = z;
  if( !zTail ) return 0;
  while( z[0] ){
    if( '/'==z[0] || '\\'==z[0] ) zTail = &z[1];
    z++;
  }
  return zTail;
}


int fsl_find_home_dir( fsl_buffer * const tgt, bool requireWriteAccess ){
  char * zHome = NULL;
  int rc = 0;
  fsl_buffer_reuse(tgt);
#if defined(_WIN32) || defined(__CYGWIN__)
  zHome = fsl_getenv("LOCALAPPDATA");
  if( zHome==0 ){
    zHome = fsl_getenv("APPDATA");
    if( zHome==0 ){
      char *zDrive = fsl_getenv("HOMEDRIVE");
      zHome = fsl_getenv("HOMEPATH");
      if( zDrive && zHome ){
        tgt->used = 0;
        rc = fsl_buffer_appendf(tgt, "%s", zDrive);
        fsl_os_str_free(zDrive);
        if(rc){
          fsl_os_str_free(zHome);
          return rc;
        }
      }
    }
  }
  if(NULL==zHome){
    rc = fsl_buffer_append(tgt,
                           "Cannot locate home directory - "
                           "please set the LOCALAPPDATA or "
                           "APPDATA or HOMEPATH "
                           "environment variables.",
                           -1);
    return rc ? rc : FSL_RC_NOT_FOUND;
  }
  rc = fsl_buffer_appendf( tgt, "%/", zHome );
#else
  /* Unix... */
  zHome = fsl_getenv("HOME");
  if( zHome==0 ){
    rc = fsl_buffer_append(tgt,
                           "Cannot locate home directory - "
                           "please set the HOME environment "
                           "variable.",
                           -1);
    return rc ? rc : FSL_RC_NOT_FOUND;
  }
  rc = fsl_buffer_appendf( tgt, "%s", zHome );
#endif

  fsl_os_str_free(zHome);
  if(rc) return rc;
  assert(0<tgt->used);
  zHome = fsl_buffer_str(tgt);

  if( fsl_dir_check(zHome)<1 ){
    /* assert(0==tgt->used); */
    fsl_buffer tmp = fsl_buffer_empty;
    rc = fsl_buffer_appendf(&tmp,
                            "Invalid home directory: %s",
                            zHome);
    fsl_buffer_swap_free(&tmp, tgt, -1);
    return rc ? rc : FSL_RC_TYPE;
  }

#if !(defined(_WIN32) || defined(__CYGWIN__))
  /* Not sure why, but the is-writable check is historically only done
     on Unix platforms?

     TODO: this was subsequently changed in fossil(1) to only require
     that the global db dir be writable. Port the newer logic in.
  */
  if( requireWriteAccess &&
      (0 != fsl_file_access(zHome, W_OK)) ){
    fsl_buffer tmp = fsl_buffer_empty;
    rc = fsl_buffer_appendf(&tmp,
                            "Home directory [%s] must "
                            "be writeable.",
                            zHome);
    fsl_buffer_swap_free(&tmp, tgt, -1);
    return rc ? rc : FSL_RC_ACCESS;
  }
#endif

  return rc;
}

int fsl_errno_to_rc(int errNo, int dflt){
  switch(errNo){
    /* Plese expand on this as tests/use cases call for it... */
    case 0:
      return 0;
    case EINVAL:
      return FSL_RC_MISUSE;
    case ENOMEM:
      return FSL_RC_OOM;
    case EROFS:
    case EACCES:
    case EBUSY:
    case EPERM:
    case EDQUOT:
    case EAGAIN:
    case ETXTBSY:
      return FSL_RC_ACCESS;
    case EISDIR:
    case ENOTDIR:
      return FSL_RC_TYPE;
    case ENAMETOOLONG:
    case ELOOP:
    case ERANGE:
      return FSL_RC_RANGE;
    case ENOENT:
    case ESRCH:
      return FSL_RC_NOT_FOUND;
    case EEXIST:
    case ENOTEMPTY:
      return FSL_RC_ALREADY_EXISTS;
    case EIO:
      return FSL_RC_IO;
    default:
      return dflt;
  }
}

int fsl_file_unlink(const char *zFilename){
  int rc;
#ifdef _WIN32
  wchar_t *z = (wchar_t*)fsl_utf8_to_filename(zFilename);
  rc = _wunlink(z) ? errno : 0;
#else
  char *z = (char *)fsl_utf8_to_filename(zFilename);
  rc = unlink(zFilename) ? errno : 0;
#endif
  fsl_os_str_free(z);
  return rc ? fsl_errno_to_rc(errno, FSL_RC_IO) : 0;
}

int fsl_mkdir(const char *zName, bool forceFlag){
  int rc =
    /*file_wd_dir_check(zName)*/
    fsl_dir_check(zName)
    ;
  if( rc<0 ){
    if( !forceFlag ) return FSL_RC_TYPE;
    rc = fsl_file_unlink(zName);
    if(rc) return rc;
  }else if( 0==rc ){
#if defined(_WIN32)
    typedef wchar_t char_t;
#define mkdir(F,P) _wmkdir(F)
#else
    typedef char char_t;
#endif
    char_t *zMbcs = (char_t*)fsl_utf8_to_filename(zName);
    if(!zMbcs) return FSL_RC_OOM;
    rc = mkdir(zMbcs, 0755);
    fsl_os_str_free(zMbcs);
    return rc ? fsl_errno_to_rc(errno, FSL_RC_IO) : 0;
#if defined(_WIN32)
#undef mkdir
#endif
  }
  return 0;
}

int fsl_mkdir_for_file(char const *zName, bool forceFlag){
  int rc;
  fsl_buffer b = fsl_buffer_empty /* we copy zName to
                                     simplify traversal */;
  fsl_size_t n = fsl_strlen(zName);
  fsl_size_t i;
  char * zCan;
  if(n==0) return FSL_RC_RANGE;
  else if(n<2) return 0/*no dir part*/;
#if 1
  /* This variant does more work (checks dirs we know already
     exist) but transforms the path into something platform-neutral.
     If we use fsl_file_simplify_name() instead then we end up
     having to do the trailing-slash logic here.
  */
  rc = fsl_file_canonical_name(zName, &b, true);
#else
  rc = fsl_buffer_append(&b, zName, n);
#endif
  zCan = fsl_buffer_str(&b);
  n = b.used;
  for( i = 1; 0==rc && i<n; ++i ){
    if( '/'==zCan[i] ){
      zCan[i] = 0;
#if defined(_WIN32) || defined(__CYGWIN__)
      /*
         On Windows, local path looks like: C:/develop/project/file.txt
         The if stops us from trying to create a directory of a drive letter
         C: in this example.
      */
      if( !(i==2 && zCan[1]==':') ){
#endif
        rc = fsl_dir_check(zCan);
#if 0
        if(rc<0){
          if(forceFlag) rc = fsl_file_unlink(zCan);
          else rc = FSL_RC_TYPE;
        }
#endif
        /* MARKER(("dir_check rc=%d, zCan=%s\n", rc, zCan)); */
        if(0>=rc){
          rc = fsl_mkdir(zCan, forceFlag);
          /* MARKER(("mkdir(%s) rc=%s\n", zCan, fsl_rc_cstr(rc))); */
        }else{
          rc = 0;
          /* Nothing to do. */
        }
#if defined(_WIN32) || defined(__CYGWIN__)
      }
#endif
      zCan[i] = '/';
    }
  }
  fsl_buffer_clear(&b);
  return rc;
}

#if defined(_WIN32)
/* Taken verbatim from fossil(1), just renamed */
/*
** Returns non-zero if the specified name represents a real directory, i.e.
** not a junction or symbolic link.  This is important for some operations,
** e.g. removing directories via _wrmdir(), because its detection of empty
** directories will (apparently) not work right for junctions and symbolic
** links, etc.
*/
static int w32_file_is_normal_dir(wchar_t *zName){
  /*
  ** Mask off attributes, applicable to directories, that are harmless for
  ** our purposes.  This may need to be updated if other attributes should
  ** be ignored by this function.
  */
  DWORD dwAttributes = GetFileAttributesW(zName);
  if( dwAttributes==INVALID_FILE_ATTRIBUTES ) return 0;
  dwAttributes &= ~(
    FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_COMPRESSED |
    FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_NORMAL |
    FILE_ATTRIBUTE_NOT_CONTENT_INDEXED
  );
  return dwAttributes==FILE_ATTRIBUTE_DIRECTORY;
}
#endif


bool fsl_file_isexec(const char *zFilename){
  fsl_fstat st = fsl_fstat_empty;
  int const s = fsl_stat(zFilename, &st, true);
  return 0==s ? (st.perm & FSL_FSTAT_PERM_EXE) : false;
}

int fsl_rmdir(const char *zFilename){
  int rc = fsl_dir_check(zFilename);
  if(rc<1) return rc ? FSL_RC_TYPE : FSL_RC_NOT_FOUND;
#ifdef _WIN32
  wchar_t *z = (wchar_t*)fsl_utf8_to_filename(zFilename);
  if(w32_file_is_normal_dir(z)){
    rc = _wunlink(z) ? errno : 0;
  }else{
    rc = ENOTDIR;
  }
#else
  char *z = (char *)fsl_utf8_to_filename(zFilename);
  rc = rmdir(zFilename) ? errno : 0;
#endif
  fsl_os_str_free(z);
  if(rc){
    int const eno = errno;
    switch(eno){
      /* ENOENT normally maps to FSL_RC_NOT_FOUND,
         but in this case that's ambiguous. */
      case ENOENT: rc = FSL_RC_ACCESS; break;
      default: rc = fsl_errno_to_rc(errno, FSL_RC_IO);
        break;
    }
  }
  return rc;
}

int fsl_dir_check(const char *zFilename){
  fsl_fstat fst;
  int rc;
  if( zFilename ){
#if 1
    rc = fsl_stat(zFilename, &fst, 1);
#else
    char *zFN = fsl_strdup(zFilename);
    if(!zFN) rc = FSL_RC_OOM;
    else{
      fsl_file_simplify_name(zFN, -1, 0);
      rc = fsl_stat(zFN, &fst, 1);
      fsl_free(zFN);
    }
#endif
  }else{
    rc = -1 /*fsl_stat(zFilename, &fst, 1) historic: used static stat cache*/;
  }
  return rc ? 0 : ((FSL_FSTAT_TYPE_DIR == fst.type) ? 1 : -1);
}

int fsl_chdir(const char *zChDir){
  int rc;
#ifdef _WIN32
  wchar_t *zPath = fsl_utf8_to_filename(zChDir);
  errno = 0;
  rc = (int)!SetCurrentDirectoryW(zPath);
  fsl_os_str_free(zPath);
  if(rc) rc = FSL_RC_IO;
#else
  char *zPath = fsl_utf8_to_filename(zChDir);
  errno = 0;
  rc = chdir(zPath);
  fsl_os_str_free(zPath);
  if(rc) rc = fsl_errno_to_rc(errno, FSL_RC_IO);
#endif
  return rc;
}

#if 0
/*
  The family of 'wd' functions is historical in nature and not really
  needed(???) at the library level. 'wd' == 'working directory'
  (i.e. checkout).  Ideally the library won't have to do any _direct_
  manipulation of directory trees, e.g. checkouts. That is essentially
  app-level logic, though we'll need some level of infrastructure for
  the apps to build off of.  When that comes, the "wd" family of
  functions (or something similar) might come back into play.

  [Edit much later: the library most definintely needs to manipulate
  the checkout on behalf of apps, but we've so far never needed the wd
  variants to do any of that.]
*/
/*
   Same as dir_check(), but takes into account symlinks.
*/
int file_wd_dir_check(const char *zFilename){
  if(!zFilename || !*zFilename) return FSL_RC_MISUSE;
  else{
    int rc;
    fsl_fstat fst = fsl_fstat_empty;
    char *zFN = fsl_strdup(zFilename);
    if(!zFN) rc = FSL_RC_OOM;
    else{
      fsl_file_simplify_name(zFN, -1, 0);
      rc = fsl_stat(zFN, &fst, 0);
      fsl_free(zFN);
    }
    return rc ? 0 : ((FSL_FSTAT_TYPE_DIR == fst.type) ? 1 : 2);
  }
}
#endif

#if 0
/* This block requires permissions flags from v1's manifest.c. */

/*
   Return TRUE if the named file is an executable.  Return false
   for directories, devices, fifos, symlinks, etc.
*/
int fsl_wd_isexe(const char *zFilename){
  return fsl_wd_perm(zFilename)==PERM_EXE;
}

/*
   Return TRUE if the named file is a symlink and symlinks are allowed.
   Return false for all other cases.

   On Windows, always return False.
*/
int file_wd_islink(const char *zFilename){
  return file_wd_perm(zFilename)==PERM_LNK;
}
#endif

#if 0
/**
    Same as fsl_is_file(), but takes into account symlinks.
 */
bool fsl_wd_isfile(const char *zFilename);
bool fsl_wd_isfile(const char *zFilename){
  fsl_fstat fst;
  return ( 0 != fsl_stat(zFilename, &fst, 0) )
    ? 0
    : (FSL_FSTAT_TYPE_FILE == fst.type);
}
#endif
#if 0
/**
    Same as fsl_file_mtime(), but takes into account symlinks.
 */
fsl_time_t fsl_wd_mtime(const char *zFilename);
fsl_time_t fsl_wd_mtime(const char *zFilename){
  fsl_fstat fst;
  return ( 0 != fsl_stat(zFilename, &fst, 0) )
    ? -1
    : (fsl_time_t)fst.mtime;
}

bool fsl_wd_isfile_or_link(const char *zFilename){
  fsl_fstat fst;
  return ( 0 != fsl_stat(zFilename, &fst, 0) )
    ? 0
    : ((FSL_FSTAT_TYPE_LINK == fst.type)
       || (FSL_FSTAT_TYPE_FILE == fst.type))
    ;
}
#endif

#if 0
/**
    Same as fsl_file_size(), but takes into account symlinks.
 */
fsl_size_t fsl_wd_size(const char *zFilename);
fsl_size_t fsl_wd_size(const char *zFilename){
  fsl_fstat fst;
  return ( 0 != fsl_stat(zFilename, &fst, 0) )
    ? -1
    : fst.size;
}
#endif

int fsl_file_mtime_set(const char *zFilename, fsl_time_t newMTime){
  if(!zFilename || !*zFilename) return FSL_RC_MISUSE;
  else{
    int rc;
    void * zMbcs;
#if !defined(_WIN32)
    struct timeval tv[2];
    if(newMTime < 0) newMTime = (fsl_time_t)time(0);
    zMbcs = fsl_utf8_to_filename(zFilename);
    if(!zMbcs) return FSL_RC_OOM;
    memset(tv, 0, sizeof(tv[0])*2);
    tv[0].tv_sec = newMTime;
    tv[1].tv_sec = newMTime;
    rc = utimes((char const *)zMbcs, tv);
#else
    struct _utimbuf tb;
    if(newMTime < 0) newMTime = (fsl_time_t)time(0);
    zMbcs = fsl_utf8_to_filename(zFilename);
    if(!zMbcs) return FSL_RC_OOM;
    tb.actime = newMTime;
    tb.modtime = newMTime;
    rc = _wutime((wchar_t const *)zMbcs, &tb);
#endif
    fsl_os_str_free(zMbcs);
    return rc ? fsl_errno_to_rc(errno, FSL_RC_IO) : 0;
  }
}



void fsl_pathfinder_clear(fsl_pathfinder * const pf){
  if(pf){
    fsl_list_visit_free(&pf->ext, 1);
    fsl_list_visit_free(&pf->dirs, 1);
    fsl_buffer_clear(&pf->buf);
    *pf = fsl_pathfinder_empty;
  }
}

static int fsl__pathfinder_add_impl(fsl_list * const li, char const * str,
                                   fsl_int_t strLen){
  char * cp = fsl_strndup(str, strLen);
  int rc;
  if(!cp) rc = FSL_RC_OOM;
  else{
    rc = fsl_list_append(li, cp);
    if(rc) fsl_free(cp);
  }
  return rc;
}

int fsl_pathfinder_dir_add(fsl_pathfinder * const pf, char const * const dir){
  return dir
    ? fsl__pathfinder_add_impl(&pf->dirs, dir, -1)
    : FSL_RC_MISUSE;
}

int fsl_pathfinder_dir_add2(fsl_pathfinder * const pf, char const * const dir,
                            fsl_int_t strLen){
  return dir
    ? fsl__pathfinder_add_impl(&pf->dirs, dir, strLen)
    : FSL_RC_MISUSE;
}

int fsl_pathfinder_ext_add(fsl_pathfinder * const pf, char const * const ext){
  return (pf && ext)
    ? fsl__pathfinder_add_impl(&pf->ext, ext, -1)
    : FSL_RC_MISUSE;
}

int fsl_pathfinder_ext_add2(fsl_pathfinder * const pf, char const * const ext,
                           fsl_int_t strLen){
  return (pf && ext)
    ? fsl__pathfinder_add_impl(&pf->ext, ext, strLen)
    : FSL_RC_MISUSE;
}

int fsl_pathfinder_search(fsl_pathfinder * const pf,
                          char const * const base,
                          char const ** pOut,
                          fsl_size_t * const outLen ){
  fsl_buffer * const buf = &pf->buf;
  fsl_list * ext;
  fsl_list * dirs;
  int rc = 0;
  fsl_size_t d, x, nD, nX, resetLen = 0;
  fsl_size_t baseLen;
  static char const pathSep =
#if defined(_WIN32)
    '\\' /* TODO: confirm whether we can always use '/', and do so if
            we can. */
#else
    '/'
#endif
    ;
  if(!base || !*base) return FSL_RC_MISUSE;
  else if(0==fsl_file_access( base, 0 )){
    /* Special case: if base is found as-is, without a path search,
       use it. */
    if(pOut) *pOut = base;
    if(outLen) *outLen = fsl_strlen(base);
    return 0;
  }
  baseLen = fsl_strlen(base);
  ext = &pf->ext;
  dirs = &pf->dirs;
  nD = dirs->used;
  nX = ext->used;
  for( d = 0; !rc && (d < nD); ++d ){
    char const * vD = (char const *)dirs->list[d];
    /*
      Search breadth-first for a file/directory named by vD/base
    */
    buf->used = 0;
    if(vD){
      fsl_size_t const used = buf->used;
      rc = fsl_buffer_append(buf, vD, -1);
      if(rc) return rc;
      if(used != buf->used){
        /* Only append separator if vD is non-empty. */
        rc = fsl_buffer_append(buf, &pathSep, 1);
        if(rc) return rc;
      }
    }
    rc = fsl_buffer_append(buf, base, (fsl_int_t)baseLen);
    if(rc) return rc;
    if(0==fsl_file_access( (char const *)buf->mem, 0 )) goto gotone;
    resetLen = buf->used;
    for( x = 0; !rc && (x < nX); ++x ){
      char const * vX = (char const *)ext->list[x];
      if(vX){
        buf->used = resetLen;
        rc = fsl_buffer_append(buf, vX, -1);
        if(rc) return rc;
      }
      assert(buf->used < buf->capacity);
      buf->mem[buf->used] = 0;
      if(0==fsl_file_access( (char const *)buf->mem, 0 )){
        goto gotone;
      }
    }
  }
  return FSL_RC_NOT_FOUND;
  gotone:
  if(outLen) *outLen = buf->used;
  if(pOut) *pOut = (char const *)buf->mem;
  return 0;
}

void fsl_path_splitter_init( fsl_path_splitter * pt, char const * path, fsl_int_t len ){
  *pt = fsl_path_splitter_empty;
  pt->pos = pt->begin = path;
  pt->end = pt->begin + ((len>=0) ? (fsl_size_t)len : fsl_strlen(path));
}

int fsl_path_splitter_next( fsl_path_splitter * const pt, char const ** token,
                            fsl_size_t * const len ){
  if(!pt->pos || pt->pos>=pt->end) return FSL_RC_RANGE;
  else if(!pt->separators || !*pt->separators) return FSL_RC_MISUSE;
  else{
    char const * pos = pt->pos;
    char const * t;
    char const * sep;
    for( sep = pt->separators; *sep; ++sep){
      if(*sep & 0x80) return FSL_RC_MISUSE;
      /* non-ASCII */
    }
    for( ; pos<pt->end; ){
      /*skip leading separators*/
      for( sep = pt->separators;
           *sep && *pos!=*sep; ++sep ){
      }
      if(*pos == *sep) ++pos;
      else break;
    }
    t = pos;
    for( ; pos<pt->end; ){
      /*skip until the next separator*/
      for( sep = pt->separators;
           *sep && *pos!=*sep; ++sep ){
      }
      if(*pos == *sep) break;
      else ++pos;
    }
    pt->pos = pos;
    if(pos>t){
      *token = t;
      *len = (fsl_size_t)(pos - t);
      return 0;
    }
    return FSL_RC_NOT_FOUND;
  }
}

int fsl_pathfinder_split( fsl_pathfinder * const tgt,
                          bool isDirs,
                          char const * path,
                          fsl_int_t pathLen ){
  int rc = 0;
  char const * t = 0;
  fsl_size_t tLen = 0;
  fsl_path_splitter pt = fsl_path_splitter_empty;
  fsl_path_splitter_init(&pt, path, pathLen);
  while(0==rc && 0==fsl_path_splitter_next(&pt, &t, &tLen)){
    rc = isDirs
      ? fsl_pathfinder_dir_add2(tgt, t, (fsl_int_t)tLen)
      : fsl_pathfinder_ext_add2(tgt, t, (fsl_int_t)tLen);
  }
  return rc;
}

char * fsl__file_without_drive_letter(char * zIn){
#ifdef _WIN32
  if( zIn && fsl_isalpha(zIn[0]) && zIn[1]==':' ) zIn += 2;
#endif
  return zIn;
}

int fsl_dir_is_empty(const char *path){
  struct dirent *ent;
  int            retval = 0;
  DIR *d = opendir(path);
  if(!d){
    return -1;
  }
  while((ent = readdir(d))) {
    const char * z = ent->d_name;
    if('.'==*z &&
       (!z[1] || ('.'==z[1] && !z[2]))){
      // Skip "." and ".." entries
      continue;
    }
    retval = 1;
    break;
  }
  closedir(d);
  return retval;
}

int fsl_file_exec_set(const char *zFilename, bool isExe){
#if FSL_PLATFORM_IS_WINDOWS
  return 0;
#else
  int rc = 0, err;
  struct stat sb;
  err = stat(zFilename, &sb);
  if(0==err){
    if(!S_ISREG(sb.st_mode)) return 0;
    else if(isExe){
      if( 0==(sb.st_mode & 0100) ){
        int const mode = (sb.st_mode & 0444)>>2
          /* This impl is from fossil, which is known to work, but...
             what is the >>2 for?*/;
        err = chmod(zFilename, (mode_t)(sb.st_mode | mode));
      }
    }else if( 0!=(sb.st_mode & 0100) ){
      err = chmod(zFilename, sb.st_mode & ~0111);
    }
  }
  if(err) rc = fsl_errno_to_rc(errno, FSL_RC_IO);
  return rc;
#endif
}

/**
   fsl_dircrawl() part for handling a single directory. fst must be
   valid state from a freshly-fsl_stat()'d DIRECTORY and this
   function will re-use it for its own stat()ing.
*/
static int fsl_dircrawl_impl(fsl_buffer * const dbuf, fsl_fstat * const fst,
                             fsl_dircrawl_f cb,
                             fsl_dircrawl_state * const dst,
                             unsigned int depth){
  int rc = 0;
  DIR *dir = opendir(fsl_buffer_cstr(dbuf));
  struct dirent * dent = 0;
  fsl_size_t const dPos = dbuf->used;
  if(!dir){
    return fsl_errno_to_rc(errno, FSL_RC_IO);
  }
  if(depth>20/*arbitrary limit to try to avoid stack overflow*/){
    return FSL_RC_RANGE;
  }
  while(!rc && (dent = readdir(dir))){
    const char * z = dent->d_name;
    if('.'==*z &&
       (!z[1] || ('.'==z[1] && !z[2]))){
      // Skip "." and ".." entries
      continue;
    }
    dbuf->used = dPos;
    rc = fsl_buffer_appendf(dbuf, "/%s", z);
    if(rc) break;
    fsl_size_t const newLen = dbuf->used;
    if(fsl_stat((char const *)dbuf->mem, fst, false)){
      // Simply skip stat errors. i was once bitten by an app which did
      // not do so. Scarred for life. Too soon.
      continue;
    }
    switch(fst->type){
      case FSL_FSTAT_TYPE_LINK:
      case FSL_FSTAT_TYPE_DIR:
      case FSL_FSTAT_TYPE_FILE:
        break;
      default: continue;
    }
    dbuf->mem[dbuf->used = dPos] = 0;
    dst->absoluteDir = (char const *)dbuf->mem;
    dst->entryName = z;
    dst->entryType = fst->type;
    dst->depth = depth;
    rc = cb( dst );
    if(!rc){
      dbuf->mem[dbuf->used] = '/';
      dbuf->used = newLen;
      if(FSL_FSTAT_TYPE_DIR==fst->type){
        rc = fsl_dircrawl_impl( dbuf, fst, cb, dst, depth+1 );
      }
    }else if(FSL_RC_NOOP == rc){
      rc = 0;
    }
  }
  closedir(dir);
  return rc;
}

int fsl_dircrawl(char const * dirName, fsl_dircrawl_f callback,
                 void * cbState){
  fsl_buffer dbuf = fsl_buffer_empty;
  fsl_fstat fst = fsl_fstat_empty;
  int rc = fsl_file_canonical_name(dirName, &dbuf, false);
  fsl_dircrawl_state dst;
  if(!rc && '/' == dbuf.mem[dbuf.used-1]){
    dbuf.mem[--dbuf.used] = 0;
  }
  memset(&dst, 0, sizeof(dst));
  dst.callbackState = cbState;
  while(!rc){
    rc = fsl_stat((char const *)dbuf.mem, &fst, false);
    if(rc) break;
    else if(FSL_FSTAT_TYPE_DIR!=fst.type){
      rc = FSL_RC_TYPE;
      break;
    }
    rc = fsl_dircrawl_impl(&dbuf, &fst, callback, &dst, 1);
    if(FSL_RC_BREAK==rc) rc = 0;
    break;
  }
  fsl_buffer_clear(&dbuf);
  return rc;
}

bool fsl_is_file_or_link(const char *zFilename){
  fsl_fstat fst = fsl_fstat_empty;
  return fsl_stat(zFilename, &fst, false)
    ? false
    : (fst.type==FSL_FSTAT_TYPE_FILE
       || fst.type==FSL_FSTAT_TYPE_LINK);
}

fsl_size_t fsl_strip_trailing_slashes(char * name, fsl_int_t nameLen){
  fsl_size_t rc = 0;
  if(nameLen < 0) nameLen = (fsl_int_t)fsl_strlen(name);
  if(nameLen){
    char * z = name + nameLen - 1;
    for( ; (z>=name) && ('/'==*z); --z){
      *z = 0;
      ++rc;
    }
  }
  return rc;
}

void fsl_buffer_strip_slashes(fsl_buffer * const b){
  b->used -= fsl_strip_trailing_slashes((char *)b->mem,
                                        (fsl_int_t)b->used);
}

int fsl_file_rename(const char *zFrom, const char *zTo){
  int rc;
#if defined(_WIN32)
  /** 2021-03-24: fossil's impl of this routine has 2 additional
      params (bool isFromDir, bool isToDir), which are passed on to
      fsl_utf8_to_filename(), only used on Windows platforms, and are
      only to allow for 12 bytes of edge case in MAX_PATH handling.
      We don't need them. */
  wchar_t *zMbcsFrom = fsl_utf8_to_filename(zFrom);
  wchar_t *zMbcsTo = zMbcsFrom ? fsl_utf8_to_filename(zTo) : 0;
  rc = zMbcsTo ? _wrename(zMbcsFrom, zMbcsTo) : FSL_RC_OOM;
#else
  char *zMbcsFrom = fsl_utf8_to_filename(zFrom);
  char *zMbcsTo = zMbcsFrom ? fsl_utf8_to_filename(zTo) : 0;
  rc = zMbcsTo ? rename(zMbcsFrom, zMbcsTo) : FSL_RC_OOM;
#endif
  fsl_os_str_free(zMbcsTo);
  fsl_os_str_free(zMbcsFrom);
  return -1==rc ? fsl_errno_to_rc(errno, FSL_RC_IO) : rc;
}

char ** fsl_temp_dirs_get(void){
#if FSL_PLATFORM_IS_WINDOWS
  const char *azDirs[] = {
     ".",
     NULL
  };
  unsigned int const nDirs = 4
    /* GetTempPath(), $TEMP, $TMP, azDirs */;
#else
  const char *azDirs[] = {
     "/var/tmp",
     "/usr/tmp",
     "/tmp",
     "/temp",
     ".", NULL
  };
  unsigned int const nDirs = 6
    /* $TMPDIR, azDirs */;
#endif
  char *z;
  char const *zC;
  char ** zDirs = NULL;
  unsigned int i, n = 0;

  zDirs = (char **)fsl_malloc(sizeof(char*) * (nDirs + 1));
  if(!zDirs) return NULL;
  for(i = 0; i<=nDirs; ++i) zDirs[i] = NULL;
#define DOZ \
  if(z && fsl_dir_check(z)>0) zDirs[n++] = z;   \
  else if(z) fsl_os_str_free(z)

#if FSL_PLATFORM_IS_WINDOWS
  wchar_t zTmpPath[MAX_PATH];

  if( GetTempPathW(MAX_PATH, zTmpPath) ){
    z = fsl_filename_to_utf8(zTmpPath);
    DOZ;
  }
  z = fsl_getenv("TEMP");
  DOZ;
  z = fsl_getenv("TMP");
  DOZ;
#else /* Unix-like */
  z = fsl_getenv("TMPDIR");
  DOZ;
#endif
  for( i = 0; (zC = azDirs[i]); ++i ){
    z = fsl_filename_to_utf8(azDirs[i]);
    DOZ;
  }
#undef DOZ
  /* Strip any trailing slashes unless the only character is a
     slash. Note that we ignore the root-dir case on Windows here,
     mainly because this developer can't test it and secondarily
     because it's a highly unlikely case. */
  for(i = 0; i < n; ++i ){
    fsl_size_t len;
    z = zDirs[i];
    len = fsl_strlen(z);
    while(len>1 && (z[len-1]=='/' || z[len-1]=='\\')){
      z[--len] = 0;
    }
  }
  return zDirs;
}

void fsl_temp_dirs_free(char **aDirs){
  if(aDirs){
    char * z;
    for(unsigned i = 0; (z = aDirs[i]); ++i){
      fsl_os_str_free(z);
      aDirs[i] = NULL;
    }
    fsl_free(aDirs);
  }
}

int fsl_file_tempname(fsl_buffer * const tgt, char const *zPrefix,
                      char * const * const dirs){
  int rc = 0;
  unsigned int tries = 0;
  const unsigned char zChars[] =
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "0123456789_-";
  enum { RandSize = 16 };
  char zRand[RandSize + 1];
  int i;
  char const * zDir = "";

  if( tgt->errCode ) return tgt->errCode;
  if(dirs){
    for(i = 0; (zDir=dirs[i++]); ){
      /* We repeat this check, performed in fsl_temp_dirs_get(), for the
         sake of long-lived apps where a given temp dir might disappear
         at some point. */
      if(fsl_dir_check(zDir)>0) break;
    }
    if(!zDir) return FSL_RC_NOT_FOUND;
  }
  if(!zPrefix) zPrefix = "libfossil";
  /* Pre-fill buffer to allocate it in advance and remember the length
     of the base filename part so that we don't have to re-write the
     prefix each iteration below. */
  rc = fsl_buffer_appendf(tgt, "%/%s%s%s%.*cZ",
                          zDir, *zDir ? "/" : "",
                          zPrefix, *zPrefix ? "~" : "",
                          (int)RandSize, 'X');
  fsl_size_t const baseLen = rc ? 0 : (tgt->used - RandSize - 1);
  do{
    if(++tries == 20){
      rc = FSL_RC_RANGE;
      break;
    }
    fsl_randomness(RandSize, zRand);
    for( i=0; i < RandSize; ++i ){
      zRand[i] = (char)zChars[ ((unsigned char)zRand[i])%(sizeof(zChars)-1) ];
    }
    zRand[RandSize] = 0;
    tgt->used = baseLen;
    rc = fsl_buffer_append(tgt, zRand, (fsl_int_t)RandSize);
    assert(0==rc && "We pre-allocated the buffer above.");
  }while(0==rc && fsl_file_size(fsl_buffer_cstr(tgt)) >= 0);
  return rc;
}

int fsl_file_copy(char const *zFrom, char const *zTo){
  FILE * in = 0, *out = 0;
  int rc;
  in = fsl_fopen(zFrom, "rb");
  if(!in) return fsl_errno_to_rc(errno, FSL_RC_IO);
  rc = fsl_mkdir_for_file(zTo, false);
  if(rc) goto end;
  out = fsl_fopen(zTo, "wb");
  rc = out
    ? fsl_stream(fsl_input_f_FILE, in, fsl_output_f_FILE, out)
    : fsl_errno_to_rc(errno, FSL_RC_IO);
  end:
  if(in) fsl_fclose(in);
  if(out) fsl_fclose(out);
  if(0==rc && fsl_file_isexec(zFrom)){
    fsl_file_exec_set(zTo, true);
  }
  return rc;
}

int fsl_symlink_read(fsl_buffer * const tgt, char const * zFilename){
#if FSL_PLATFORM_IS_WINDOWS
  fsl_buffer_reuse(tgt);
  return 0;
#else
  enum { BufLen = 1024 * 2 };
  char buf[BufLen];
  int rc;
  ssize_t const len = readlink(zFilename, buf, BufLen-1);
  if(len<0) rc = fsl_errno_to_rc(errno, FSL_RC_IO);
  else{
    fsl_buffer_reuse(tgt);
    rc = fsl_buffer_append(tgt, buf, (fsl_size_t)len);
  }
  return rc;
#endif
}

int fsl_symlink_create(const char *zTargetFile, const char *zLinkFile,
                       bool realLink){
  int rc;
#if FSL_PLATFORM_IS_WINDOWS
  (void)realLink;
#else
  if( realLink ){
    char *zName, zBuf[1024 * 2];
    fsl_size_t nName = fsl_strlen(zLinkFile);
    if( nName>=sizeof(zBuf) ){
      zName = fsl_mprintf("%s", zLinkFile);
      if(!zName) return FSL_RC_OOM;
    }else{
      zName = zBuf;
      memcpy(zName, zLinkFile, nName+1);
    }
    nName = fsl_file_simplify_name(zName, (fsl_int_t)nName, false);
    rc = fsl_mkdir_for_file(zName, false);
    if(0==rc && 0!=symlink(zTargetFile, zName) ){
      rc = fsl_errno_to_rc(errno, FSL_RC_IO);
    }
    if( zName!=zBuf ) fsl_free(zName);
  }else
#endif
  {
    rc = fsl_mkdir_for_file(zLinkFile, false);
    if(0==rc){
      fsl_buffer content = fsl_buffer_empty;
      fsl_buffer_external(&content, zTargetFile, -1);
      fsl_file_unlink(zLinkFile)
        /* in case it's already a symlink, we don't want the following
           to overwrite the symlinked-to file */;
      rc = fsl_buffer_to_filename(&content, zLinkFile);
    }
  }
  return rc;
}

int fsl_symlink_copy(char const *zFrom, char const *zTo, bool realLink){
  int rc;
  fsl_buffer b = fsl_buffer_empty;
  rc = fsl_symlink_read(&b, zFrom);
  if(0==rc){
    rc = fsl_symlink_create(fsl_buffer_cstr(&b), zTo, realLink);
  }
  fsl_buffer_clear(&b);
  return rc;
}

char const * fsl_last_path_sep(char const * str, fsl_int_t slen ){
  if(slen<0) slen = (fsl_int_t)fsl_strlen(str);
  unsigned char const * pos = (unsigned char const *)str + slen;
  while( --pos >= (unsigned char const *)str ){
    if('/'==*pos || '\\'==*pos){
      return (char const *)pos;
    }
  }
  return NULL;
}

int fsl_filename_preferred_case(bool isCaseSensitive, const char *zDir,
                                const char *zPath, char **zOut){
  /* Adapted from: https://fossil-scm.org/home/timeline?r=preserve-case-on-add */
  /* 2024-10-15: Florian added a Windows-specific variant of that:
     https://fossil-scm.org/home/info/9919dfbbaa2019e7eb7b
     but we'll need a Windows-using developer to integrate that here.
  */
  DIR *d;
  int i;
  char *zResult = 0;
  void *zNative = 0;
  int rc = 0;
  struct dirent *pEntry;

  if( isCaseSensitive ){
    if( (zResult = fsl_strdup(zPath))!=0 ) *zOut = zResult;
    else rc = FSL_RC_OOM;
    return rc;
  }
  for( i = 0; zPath[i] && zPath[i]!='/' && zPath[i]!='\\'; ++i){}
  /* potential TODO: reimplement this using fsl_dircrawl().

     alternative potential TODO: add an outer loop which adjusts zDir
     and zPath on each iteration instead of recursing.
  */
#define CKERR(P,RC) if( !(P) ){ rc = RC; assert(!"!"); goto end_err; }(void)0
#define CKOOM(P) CKERR(P,FSL_RC_OOM)
  zNative = fsl_utf8_to_unicode(zDir);
  CKOOM(zNative);
  d = opendir(zNative);
  CKERR(d,FSL_RC_IO);
  while( (pEntry = readdir(d)) != 0 ){
    char *zUtf8 = fsl_filename_to_utf8(pEntry->d_name);
    CKOOM(zUtf8);
    if( fsl_strnicmp(zUtf8, zPath, i)==0 && zUtf8[i]==0 ){
      if( zPath[i]==0 ){
        zResult = fsl_strdup(zUtf8);
        CKOOM(zResult);
      }else{
        char *zSubDir = fsl_mprintf("%s/%s", zDir, zUtf8);
        char *zSubPath = 0;
        rc = zSubDir
          ? fsl_filename_preferred_case(false, zSubDir, &zPath[i+1], &zSubPath)
          : FSL_RC_OOM;
        zResult = rc ? 0 : fsl_mprintf("%s/%s", zUtf8, zSubPath);
        fsl_free(zSubPath);
        fsl_free(zSubDir);
        CKERR(zResult, (rc ? rc : FSL_RC_OOM));
      }
      fsl_os_str_free(zUtf8);
      break;
    }
    fsl_os_str_free(zUtf8);
  }
  closedir(d);
  if( !zResult ){
    zResult = fsl_strdup(zPath);
    CKOOM(zResult);
  }
  fsl_free(zNative);
  *zOut = zResult;
  return 0;
#undef CKOOM
#undef CKERR
end_err:
  fsl_free(zNative);
  fsl_free(zResult);
  return rc;
}

int fsl_chdir2(const char *zTo, char * zCurrent, fsl_size_t nCurrent){
  int rc = fsl_getcwd(zCurrent, nCurrent, NULL);
  if( 0==rc ){
    rc = fsl_chdir(zTo);
  }
  return rc;
}

int fsl__find_bin( char const *zPath, char const *zBinName, char **zOut ){
  fsl_pathfinder pf = fsl_pathfinder_empty;
  int rc = 0;
  if( !zPath ) zPath = getenv("PATH");
  if( !zPath ){
#if FSL_PLATFORM_IS_UNIX
    zPath = "/usr/local/bin:/usr/bin";
#else
    return FSL_RC_NOT_FOUND;
#endif
  }
  rc = fsl_pathfinder_split(&pf, true, zPath, -1);
  if( rc ) goto end;
#if FSL_PLATFORM_IS_WINDOWS
  rc = fsl_pathfinder_ext_add2(&pf, ".exe", 4);
  if( rc ) goto end;
#endif
  fsl_size_t nGot = 0;
  char const * got = 0;
  rc = fsl_pathfinder_search(&pf, zBinName, &got, &nGot);
  if( got ){
    *zOut = fsl_strndup(got, (fsl_int_t)nGot);
    if( !*zOut ){
      rc = FSL_RC_OOM;
    }
  }
end:
  fsl_pathfinder_clear(&pf);
  return rc;
}

#if 0
int fsl_file_relative_name( char const * zRoot, char const * zPath,
                            fsl_buffer * pOut, char retainSlash ){
  int rc = FSL_RC_NYI;
  char * zPath;
  fsl_size_t rootLen;
  fsl_size_t pathLen;
  if(!zPath || !*zPath || !pOut) return FSL_RC_MISUSE;

  return rc;
}
#endif


#undef MARKER
#ifdef _WIN32
#  undef DIR
#  undef dirent
#  undef opendir
#  undef readdir
#  undef closedir
#endif
