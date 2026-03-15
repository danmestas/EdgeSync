/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/*
** Copyright (c) 2010 D. Richard Hipp
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the Simplified BSD License (also
** known as the "2-Clause License" or "FreeBSD License".)
**
** This program is distributed in the hope that it will be useful,
** but without any warranty; without even the implied warranty of
** merchantability or fitness for a particular purpose.
**
** Author contact information:
**   drh@hwaci.com
**   http://www.hwaci.com/drh/
**
*******************************************************************************
**
** This file contains an implementation of a bi-directional popen().
*/
/*************************************************************************
  This copy has been modified slightly for use in the libfossil project.
*/
#undef __STRICT_ANSI__
#include "libfossil.h" /* MUST come first b/c of config macros */
#include <errno.h>

#if FSL_PLATFORM_IS_WINDOWS
#include <windows.h>
#include <fcntl.h>
#else
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#endif

/*
   The following macros are used to cast pointers to integers and
   integers to pointers.  The way you do this varies from one compiler
   to the next, so we have developed the following set of #if statements
   to generate appropriate macros for a wide range of compilers.

   The correct "ANSI" way to do this is to use the intptr_t type.
   Unfortunately, that typedef is not available on all compilers, or
   if it is available, it requires an #include of specific headers
   that vary from one machine to the next.

   This code is copied out of SQLite.
*/
#if defined(__PTRDIFF_TYPE__)  /* This case should work for GCC */
# define INT_TO_PTR(X)  ((void*)(__PTRDIFF_TYPE__)(X))
# define PTR_TO_INT(X)  ((int)(__PTRDIFF_TYPE__)(X))
#elif !defined(__GNUC__)       /* Works for compilers other than LLVM */
# define INT_TO_PTR(X)  ((void*)&((char*)0)[X])
# define PTR_TO_INT(X)  ((int)(((char*)X)-(char*)0))
#elif defined(HAVE_STDINT_H)   /* Use this case if we have ANSI headers */
# define INT_TO_PTR(X)  ((void*)(intptr_t)(X))
# define PTR_TO_INT(X)  ((int)(intptr_t)(X))
#else                          /* Generates a warning - but it always works */
# define INT_TO_PTR(X)  ((void*)(X))
# define PTR_TO_INT(X)  ((int)(X))
#endif


#if FSL_PLATFORM_IS_WINDOWS
/*
   On windows, create a child process and specify the stdin, stdout,
   and stderr channels for that process to use.

   Return the number of errors.
*/
static int win32_create_child_process(
  wchar_t *zCmd,       /* The command that the child process will run */
  HANDLE hIn,          /* Standard input */
  HANDLE hOut,         /* Standard output */
  HANDLE hErr,         /* Standard error */
  DWORD *pChildPid     /* OUT: Child process handle */
){
  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  BOOL rc;

  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  SetHandleInformation(hIn, HANDLE_FLAG_INHERIT, TRUE);
  si.hStdInput  = hIn;
  SetHandleInformation(hOut, HANDLE_FLAG_INHERIT, TRUE);
  si.hStdOutput = hOut;
  SetHandleInformation(hErr, HANDLE_FLAG_INHERIT, TRUE);
  si.hStdError  = hErr;
  rc = CreateProcessW(
     NULL,  /* Application Name */
     zCmd,  /* Command-line */
     NULL,  /* Process attributes */
     NULL,  /* Thread attributes */
     TRUE,  /* Inherit Handles */
     0,     /* Create flags  */
     NULL,  /* Environment */
     NULL,  /* Current directory */
     &si,   /* Startup Info */
     &pi    /* Process Info */
  );
  if( rc ){
    CloseHandle( pi.hProcess );
    CloseHandle( pi.hThread );
    *pChildPid = pi.dwProcessId;
  }else{
    //fsl__fatal(FSL_RC_ERROR, "cannot create child process");
    rc = FSL_RC_ERROR;
  }
  return rc!=0;
}
#endif

/**
   Create a child process running shell command "zCmd".  *ppOut gets
   assigned to a FILE that becomes the standard input of the child
   process.  (The caller writes to *ppOut in order to send text to the
   child.)  *pfdIn gets assigned to the stdout from the child process.
   (The caller reads from *pfdIn in order to receive input from the
   child.)  Note that *pfdIn is an unbuffered file descriptor, not a
   FILE.  The process ID of the child is written into *pChildPid.

   On success the values returned via *pfdIn, *ppOut, and *pChildPid
   must be passed to fsl_pclose2() to properly clean up.

   Return 0 on success, non-0 on error.
*/
int fsl__popen2(const char *zCmd, int *pfdIn, FILE **ppOut, int *pChildPid){
#if FSL_PLATFORM_IS_WINDOWS
  /* FIXME: port these win32_fatal_error() bits to error codes. */
  HANDLE hStdinRd, hStdinWr, hStdoutRd, hStdoutWr, hStderr;
  SECURITY_ATTRIBUTES saAttr;
  DWORD childPid = 0;
  int fd;

  saAttr.nLength = sizeof(saAttr);
  saAttr.bInheritHandle = TRUE;
  saAttr.lpSecurityDescriptor = NULL;
  hStderr = GetStdHandle(STD_ERROR_HANDLE);
  if( !CreatePipe(&hStdoutRd, &hStdoutWr, &saAttr, 4096) ){
    //win32_fatal_error("cannot create pipe for stdout");
    return FSL_RC_IO;
  }
  SetHandleInformation( hStdoutRd, HANDLE_FLAG_INHERIT, FALSE);

  if( !CreatePipe(&hStdinRd, &hStdinWr, &saAttr, 4096) ){
    //win32_fatal_error("cannot create pipe for stdin");
    return FSL_RC_IO;
  }
  SetHandleInformation( hStdinWr, HANDLE_FLAG_INHERIT, FALSE);

  win32_create_child_process(fsl_utf8_to_unicode(zCmd),
                             hStdinRd, hStdoutWr, hStderr,&childPid);
  *pChildPid = childPid;
  *pfdIn = _open_osfhandle(PTR_TO_INT(hStdoutRd), 0);
  fd = _open_osfhandle(PTR_TO_INT(hStdinWr), 0);
  *ppOut = _fdopen(fd, "w");
  CloseHandle(hStdinRd);
  CloseHandle(hStdoutWr);
  return 0;
#else
  int rc;
  int pin[2], pout[2];
  *pfdIn = 0;
  *ppOut = 0;
  *pChildPid = 0;

  if( pipe(pin)<0 ){
    return fsl_errno_to_rc(errno, FSL_RC_ERROR);
  }
  if( pipe(pout)<0 ){
    rc = fsl_errno_to_rc(errno, FSL_RC_ERROR);
    close(pin[0]);
    close(pin[1]);
    return rc;
  }
  *pChildPid = fork();
  if( *pChildPid<0 ){
    rc = fsl_errno_to_rc(errno, FSL_RC_ERROR);
    close(pin[0]);
    close(pin[1]);
    close(pout[0]);
    close(pout[1]);
    *pChildPid = 0;
    return rc;
  }
  signal(SIGPIPE,SIG_IGN);
  if( *pChildPid==0 ){
    int fd;
    /* This is the child process */
    close(0);
    fd = dup(pout[0]);
    if( fd!=0 ) { /* nop */ };
    close(pout[0]);
    close(pout[1]);
    close(1);
    fd = dup(pin[1]);
    if( fd!=1 ) { /* nop */ };
    close(pin[0]);
    close(pin[1]);
    execl("/bin/sh", "/bin/sh", "-c", zCmd, (char*)0)
      /* doesn't return on success */;
    return fsl_errno_to_rc(errno, FSL_RC_ERROR);
  }else{
    /* This is the parent process */
    close(pin[1]);
    *pfdIn = pin[0];
    close(pout[0]);
    *ppOut = fdopen(pout[1], "w");
    return 0;
  }
#endif
}

/**
   Close the connection to a child process previously created using
   fsl__popen2(). All 3 arguments are assumed to values returned via
   fsl__popen2()'s output parameters: the input file descriptor,
   output FILE handle, and child process PID.

   If childPid is not zero, that process is killed with SIGINT before
   the I/O channels are closed.

   On Windows platforms, killing of the child process is not
   implemented. (Patches are welcomed.)
*/
int fsl__pclose2(int fdIn, FILE *pOut, int childPid){
#if FSL_PLATFORM_IS_WINDOWS
  /* Not implemented. Patches welcomed. */
  close(fdIn);
  if(pOut) fclose(pOut);
  return 0;
#elif 1
  int wp, rc = 0;
  close(fdIn);
  if( pOut ) fclose(pOut);
  //if(childPid>0) kill(childPid, SIGINT);
  do{
    wp = waitpid((pid_t)childPid, &rc, WNOHANG);
    if( wp>0 ){
      if( WIFEXITED(rc) ){
        rc = WEXITSTATUS(rc);
      }else if( WIFSIGNALED(rc) ){
        rc = WTERMSIG(rc);
      }else{
        rc = 0/*???*/;
      }
    }
  } while( wp>0 );
  return rc;
#endif
}

#undef PTR_TO_INT
#undef INT_TO_PTR
