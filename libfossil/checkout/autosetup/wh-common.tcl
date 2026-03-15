########################################################################
# 2024 September 25
#
# The author disclaims copyright to this source code.  In place of
# a legal notice, here is a blessing:
#
#  * May you do good and not evil.
#  * May you find forgiveness for yourself and forgive others.
#  * May you share freely, never taking more than you give.
#
########################################################################
# Routines for Steve Bennett's autosetup which are common to trees
# managed under the umbrella of wanderinghorse.net.
#
# In the interest of helping keep multiple copies of this file up to
# date:
#
# The canonical version of this file is the one in libfossil:
#
#   https://fossil.wanderinghorse.net/r/libfossil/dir/autosetup
#
# This file requires proj.tcl, which is co-maintained by the original
# author of this file and can be found with it.
########################################################################

use proj

########################################################################
# Looks for `bash` binary and dies if not found. On success, defines
# BIN_BASH to the full path to bash and returns that value. We
# _require_ bash because it's the SHELL value used in our makefiles.
proc wh-require-bash {} {
  set bash [proj-bin-define bash]
  if {"" eq $bash} {
    user-error "Our Makefiles require the bash shell."
  }
  return $bash
}

########################################################################
# Curses!
#
# Jumps through numerous hoops to try to find ncurses libraries and
# appropriate compilation/linker flags. Returns 0 on failure, 1 on
# success, and defines (either way) LDFLAGS_CURSES to the various linker
# flags and CFLAGS_CURSES to the various CFLAGS (both empty strings if
# no lib is found).
#
# If boolFlagForCheck is set, it is assumed to be the name of a
# boolean configure flag to enable/disable the ncurses check. If it's
# false then CFLAGS_CURSES and LDFLAGS_CURSES are set to "".
#
# This impl prefers to use pkg-config to find ncurses and libpanel
# because various platforms either combine, or not, the wide-char
# versions of those libs into the same library (or not). If no
# pkg-config is available, OR the platform looks like Mac, then we
# simply make an educated guess and hope it works. On Mac pkg-config
# is not sufficient because the core system and either brew or
# macports can contain mismatched versions of ncurses and iconv, so on
# that platform we simply guess from the core system level, ignoring
# brew/macports options.
proc wh-check-ncurses {{boolFlagForCheck ""}} {
  # TODO?: use autosetup's pkg-config module instead of
  # manually dealing with the binary.
  set pcBin [proj-bin-define pkg-config]
  msg-checking "Looking for \[n]curses... "
  set LDFLAGS_CURSES ""
  set CFLAGS_CURSES ""
  if {"" ne $boolFlagForCheck && ![opt-bool $boolFlagForCheck]} {
    msg-result "disabled via --disable-$boolFlagForCheck"
    define LDFLAGS_CURSES ""
    define CFLAGS_CURSES ""
    return [define HAVE_CURSES 0]
  }
  set rc 0
  if {"" ne $pcBin && $::tcl_platform(os)!="Darwin"} {
    # Some macOS pkg-config configurations alter library search paths, which make
    # the compiler unable to find lib iconv, so don't use pkg-config on macOS.
    set np ""
    foreach p {ncursesw ncurses} {
      if {[catch {exec $pcBin --exists $p}]} {
        continue
      }
      set np $p
      msg-result "Using pkg-config curses package \[$p]"
      break
    }
    if {"" ne $np} {
      set ppanel ""
      if {"ncursesw" eq $np} {
        if {![catch {exec $pcBin --exists panelw}]} {
          set ppanel panelw
        }
      }
      if {"" eq $ppanel && ![catch {exec $pcBin --exists panel}]} {
        set ppanel panel
      }
      set CFLAGS_CURSES [exec $pcBin --cflags $np]
      set LDFLAGS_CURSES [exec $pcBin --libs $np]
      if {"" eq $ppanel} {
        # Apparently Mac brew has pkg-config for ncursesw but not
        # panel/panelw, but hard-coding -lpanel seems to work on
        # that platform.
        append LDFLAGS_CURSES " -lpanel"
      } else {
        append LDFLAGS_CURSES " " [exec $pcBin --libs $ppanel]
        # append CFLAGS_CURSES " " [exec $pcBin --cflags $ppanel]
        # ^^^^ appending the panel cflags will end up duplicating
        # at least one -D flag from $np's cflags, leading to
        # "already defined" errors at compile-time. Sigh. Note, however,
        # that $ppanel's cflags have flags which $np's do not, so we
        # may need to include those flags anyway and manually perform
        # surgery on the list to remove dupes. Sigh.
      }
    }
  }

  if {"" eq $LDFLAGS_CURSES} {
    puts "Guessing curses location (will fail for exotic locations)..."
    define HAVE_CURSES_H [cc-check-includes curses.h]
    if {[get-define HAVE_CURSES_H]} {
      # Linux has -lncurses, BSD -lcurses. Both have <curses.h>
      msg-result "Found curses.h"
      if {[proj-check-function-in-lib waddnwstr ncursesw]} {
        msg-result "Found -lncursesw"
        set LDFLAGS_CURSES "-lncursesw -lpanelw"
      } elseif {[proj-check-function-in-lib initscr ncurses]} {
        msg-result "Found -lncurses"
        set LDFLAGS_CURSES "-lncurses -lpanel"
      } elseif {[proj-check-function-in-lib initscr curses]} {
        msg-result "Found -lcurses"
        set LDFLAGS_CURSES "-lcurses -lpanel"
      }
    }
  }
  if {"" ne $LDFLAGS_CURSES} {
    set rc 1
    puts {
      ************************************************************
      If your build fails due to missing ncurses functions such as
      waddwstr(), make sure you have the ncursesw (with a "w") development
      package installed. Some platforms combine the "w" and non-w curses
      builds and some don't. Similarly, it's easy to get a mismatch between
      libncursesw and libpanel, so make sure you have libpanelw (if
      appropriate for your platform).

      The package may have a name such as libncursesw5-dev or
      some such.
      ************************************************************}
  }
  define LDFLAGS_CURSES $LDFLAGS_CURSES
  define CFLAGS_CURSES $CFLAGS_CURSES
  return [define HAVE_CURSES $rc]
}
# /wh-check-ncurses
########################################################################

########################################################################
# Check for module-loading APIs (libdl/libltdl)...
#
# Looks for libltdl or dlopen(), the latter either in -ldl or built in
# to libc (as it is on some platforms). Returns the number of matches
# found: 0, 1, or 2. It [define]s:
#
#  - HAVE_LIBLTDL to 1 or 0 if libltdl is found/not found
#  - HAVE_LIBDL to 1 or 0 if dlopen() is found/not found
#  - LDFLAGS_LIBLTDL empty string or "-lltdl"
#  - LDFLAGS_LIBDL empty string or "-ldl", noting that -ldl may
#    legally be empty on some platforms even if HAVE_LIBDL is true
#    (indicating that dlopen() is available without extra link flags).
#
proc wh-check-module-loader {} {
  msg-checking "Looking for module-loader APIs... "
  set rc 0
  if {99 ne [get-define LDFLAGS_MODULE_LOADER 99]} {
    set msg ""
    if {1 eq [get-define HAVE_LIBLTDL 0]} {
      append msg " (cached) libltdl"
      incr rc
    }
    if {1 eq [get-define HAVE_LIBDL 0]} {
      append msg " (cached) libdl"
      incr rc
    }
    if {$rc} {
      msg-result $msg
      return $rc
    }
  }

  set HAVE_LIBLTDL 0
  set HAVE_LIBDL 0
  set LDFLAGS_LIBLTDL ""
  set LDFLAGS_LIBDL ""
  set msg ""
  if {[cc-check-includes ltdl.h] && [cc-check-function-in-lib lt_dlopen ltdl]} {
    set HAVE_LIBLTDL 1
    set LDFLAGS_LIBLTDL "-lltdl"
    append msg "- Got libltdl\n"
    incr rc
  }

  if {[cc-with {-includes dlfcn.h} {
    cctest -link 1 -declare "extern char* dlerror(void);" -code "dlerror();"}]} {
    append msg "- This system can use dlopen() without -ldl\n"
    set HAVE_LIBDL 1
    incr rc
  } elseif {[cc-check-includes dlfcn.h]} {
    set HAVE_LIBDL 1
    incr rc
    if {[cc-check-function-in-lib dlopen dl]} {
      append msg "- dlopen() needs libdl\n"
      set LDFLAGS_LIBDL  [get-define lib_dlopen]
    } else {
      append msg "- dlopen() not found in libdl. Assuming dlopen() is built-in.\n"
    }
  }
  puts -nonewline $msg
  define HAVE_LIBLTDL $HAVE_LIBLTDL
  define HAVE_LIBDL $HAVE_LIBDL
  define LDFLAGS_LIBDL $LDFLAGS_LIBDL
  define LDFLAGS_LIBLTDL $LDFLAGS_LIBLTDL
  define LDFLAGS_MODULE_LOADER ""; # for the check at the top of this function
  return $rc
}

########################################################################
# Sets all flags which would be set by wh-check-module-loader to
# empty/falsy values, as if those checks had failed to find a module
# loader. Intended to be called in place of that function when
# a module loader is explicitly not desired.
proc wh-no-check-module-loader {} {
  define HAVE_LIBDL 0
  define HAVE_LIBLTDL 0
  define LDFLAGS_MODULE_LOADER ""
}

########################################################################
# Checks for C99 (or greater) via (__STDC_VERSION__ >= 199901L).
# Returns 1 if so, 0 if not. It also define's CC_FLAG_C99 to either an
# empty string or one of the flags known to be able to induce certain
# compilers into C99 mode.
#
# If beStrict is 1 then a check for __STDC_VERSION__ is performed, else
# it isn't.
proc wh-cc-check-c99 {{beStrict 1}} {
  set flagStdc99 "-std=c99"
  define CC_FLAG_C99 ""
  set rc 0
  cc-with {} {
    if {[cc-check-flags $flagStdc99]} {
      define CC_FLAG_C99 $flagStdc99
      incr rc
    } elseif {[cc-check-flags "-Wc,-std=c99"]} {
      # QNX's qcc (gcc front-end)
      define CC_FLAG_C99 {-Wc,-std=c99}
      incr rc
    } else {
      define CC_FLAG_C99 {}
    }
  }
  if {1 == $beStrict} {
    msg-checking "Checking for C99 via __STDC_VERSION__... "
    if {[cctest -code {
      #if !defined(__STDC_VERSION__) || __STDC_VERSION__<199901L
      # error "Not C99"
      #endif
    }]} {
      msg-result "got C99"
      return 1
    }
    msg-result no
    return 0
  }
  return $rc
}

########################################################################
# Check for availability of libreadline.  Linking in readline varies
# wildly by platform and this check does not cover all known options.
#
# If flagToDisable is not empty, it is assumed to be an autosetup
# boolean config flag (--disable-$flagToDisable) and if [opt-bool
# $flagToDisable] returns false, this function skips the check but
# still defines the values described below.
#
# Defines the following vars:
#
# - HAVE_READLINE: 0 or 1
# - LDFLAGS_READLINE: "" or linker flags
# - READLINE_H "" or the full path to readline.h (if known)
# - CFLAGS_READLINE: "" or CFLAGS for finding readline.h (but see below)
#
# Quirks:
#
# - If readline.h is found in a directory name matching *line then the
#   resulting -I... flag points one directory _up_ from that, under
#   the assumption that client-side code will #include
#   <readline/readline.h>. READLINE_H, however, will be defined to the
#   full path to readline.h.
#
# Returns the value of HAVE_READLINE.
proc wh-check-readline {{flagToDisable ""}} {
  define HAVE_READLINE 0
  define LDFLAGS_READLINE ""
  define CFLAGS_READLINE ""
  define READLINE_H ""
  msg-checking "Looking for readline ... "
  if {"" ne $flagToDisable && ![opt-bool $flagToDisable]} {
    msg-result "disabled via --disable-$flagToDisable"
    return 0
  }
  msg-result ""

  # Don't use pkg-config for this because it returns ALL of the
  # --cflags output (-D_DEFAULT_SOURCE -D_XOPEN_SOURCE=600), which is
  # not really useful for readline because those are not flags clients
  # should be adding to their own code when #includ'ing readline.h

#  if {[pkg-config-init 0] && [pkg-config readline]} {
#    define HAVE_READLINE 1
#    define LDFLAGS_READLINE [get-define PKG_READLINE_LDFLAGS]
#    define-append LDFLAGS_READLINE [get-define PKG_READLINE_LIBS]
#    define CFLAGS_READLINE [get-define PKG_READLINE_CFLAGS]
#    msg-result "using info from pkg-config"
#    return 1
#  }

  # On OpenBSD on a Raspberry pi 4:
  #
  # $ pkg-config readline; echo $?
  # 0
  # $ pkg-config --cflags readline
  # Package termcap was not found in the pkg-config search path
  # $ echo $?
  # 1
  # $ pkg-config --print-requires readline; echo $?
  # 1
  #
  # i.e. there's apparently no way to find out that readline requires
  # termcap beyond parsing the error message.  It turns out it doesn't
  # want termcap, it wants -lcurses, but we don't get that info from
  # pkg-config either.

  # Look for readline.h
  set rlInc ""
  if {![proj-is-cross-compiling]} {
    # ^^^ this check is derived from SQLite's legacy configure script
    set rlInc [proj-search-for-header-dir readline.h \
                 -subdirs {include/readline include}]
    if {"" ne $rlInc} {
      define READLINE_H $rlInc/readline.h
      if {[string match */*line $rlInc]} {
        # Special case: if the path includes .../*line/readline.h", set
        # the -I to one dir up from that because our sources include
        # <readline/readline.h> or <editline/readline.h>. Reminder: if
        # auto.def is being run by jimsh0 then [file normalize] will not
        # work!
        set rlInc [file dirname $rlInc]
      }
      set rlInc "-I${rlInc}"
    }
  }

  # If readline.h was found, look for libreadline...
  set rlLib ""
  if {"" ne $rlInc} {
    set libTerm ""
    if {[proj-check-function-in-lib tgetent {readline ncurses curses termcap}]} {
      # ^^^ check extracted from an ancient autotools configure script.
      set libTerm [get-define lib_tgetent]
      undefine lib_tgetent
    }
    if {"readline" eq $libTerm} {
      set rlLib $libTerm
    } elseif {[proj-check-function-in-lib readline readline $libTerm]} {
      set rlLib [get-define lib_readline]
      lappend rlLib $libTerm
      undefine lib_readline
    }
  }

  if {"" ne $rlLib} {
    set rlLib [join $rlLib]
    define LDFLAGS_READLINE $rlLib
    define CFLAGS_READLINE $rlInc
    define HAVE_READLINE 1
    msg-result "Using readline with flags: $rlInc $rlLib"
    return 1
  }

  msg-result "libreadline not found."
  return 0
}

########################################################################
# Attempts to determine whether the given linenoise header file is of
# the "antirez" or "msteveb" flavor. It returns 1 for antirez, 2 for
# msteveb, and 0 if it's neither.
proc wh-which-linenoise {dotH} {
  set srcHeader [proj-file-content $dotH]
  set srcMain {
    int main(void) {
      linenoiseSetCompletionCallback(0$arg)
      /* antirez has only 1 arg, msteveb has 2 */;
      return 0;
    }
  }
  set arg ""
  append source $srcHeader [subst $srcMain]
  if {[cctest -nooutput 1 -source $source]} {
    return 1
  }
  set source {
    #include <stddef.h> /* size_t */
  }
  set arg ", 0"
  append source $srcHeader [subst $srcMain]
  if {[cctest -nooutput 1 -source $source]} {
    return 2
  }
  return 0
}

########################################################################
# Looks for -lpthread and defines LDFLAGS_PTHREAD to the linker flags
# needed for linking pthread (possibly an empty string). Defines
# HAVE_PTHREAD to its result value (0 or 1).
proc wh-check-pthread {} {
  define LDFLAGS_PTHREAD ""
  msg-checking "Checking for pthreads... "
  set enable 0
  if {[proj-check-function-in-lib pthread_create pthread]
      && [proj-check-function-in-lib pthread_mutexattr_init pthread]} {
    incr enable
    define LDFLAGS_PTHREAD [get-define lib_pthread_create]
    undefine lib_pthread_create
    undefine lib_pthread_mutexattr_init
  }
  define HAVE_PTHREAD $enable
}

########################################################################
# If the given platform identifier (defaulting to [get-define host])
# appears to be one of the Unix-on-Windows environments, returns a
# brief symbolic name for that environment, else returns an empty
# string.
#
# It does not distinguish between msys and msys2, returning msys for
# both. The build does not, as of this writing, specifically support
# msys v1.
proc wh-env-is-unix-on-windows {{envTuple ""}} {
  if {"" eq $envTuple} {
    set envTuple [get-define host]
  }
  set name ""
  switch -glob -- $envTuple {
    *-*-cygwin { set name cygwin }
    *-*-ming*  { set name mingw }
    *-*-msys   { set name msys }
  }
  return $name;
}

########################################################################
# @wh-check-options-flag theArg opts
#
# A helper to check a function's first flag for -options.  If set, it
# calls [options-add $opts] and returns 1, else returns 0.
#
# Example:
#
## proc foo {bar baz} {
##   if {[wh-check-options-flag $bar {
##     with-foo:path|auto|tree
##       => {Look for foo in the given path, automatically, or in the source tree.}
##   }]} {
##     return
##   }
##  ...else do the other thing...
## }
#
proc wh-check-options-flag {theArgs opts} {
  set rc 0
  if {"-options" eq [lindex $theArgs 0]} {
    incr rc
    options-add $opts
  }
  return $rc
}

########################################################################
# @wh-handle-dll-basename ?-options? libname {versionSuffix 0}
#
# Handles the --dll-basename configure flag. [define]'s
# ${libname}_DLL_BASENAME to the DLL's preferred base name (minus
# extension). If --dll-basename is not provided then this is always
# "lib${libname}", otherwise it may use a different value based on the
# value of [get-define host]. $versionSuffix is only used on
# "Unix-on-Windows" platforms (Cygwin and friends), and shows up as a
# file base name suffix of -${versionSuffix} on those environments.
# Returns the value it defines to ${libname}_DLL_BASENAME.
#
# If passed a libname of -options, it uses [options-add] to add
# a configure flag named --dll-basename then returns.
proc wh-handle-dll-basename {args} {
  if {[wh-check-options-flag $args {
      dll-basename:=auto
        => {Specifies the base name of the resulting DLL file.
          If not provided, libFOO is usually assumed but on some platforms
          a platform-dependent default is used. On some platforms this flag
          gets automatically enabled if it is not provided. Use "default" to
          explicitly disable platform-dependent activation on such systems.}
  }]} {
    return
  }
  lassign $args libname versionSuffix
  if {"" eq $versionSuffix} {
    set versionSuffix 0
  }
  if {[proj-opt-was-provided dll-basename]} {
    set dn [join [opt-val dll-basename] ""]
    if {$dn in {none default}} { set dn lib$libname }
  } else {
    set dn auto
  }
  if {$dn in {auto ""}} {
    switch -glob -- [get-define host] {
      *-*-cygwin  { set dn cyg${libname}-${versionSuffix} }
      *-*-ming*   { set dn lib${libname}-${versionSuffix} }
      *-*-msys    { set dn msys-${libname}-${versionSuffix} }
      default     { set dn lib${libname} }
    }
  }
  define ${libname}_DLL_BASENAME $dn
}

########################################################################
# @wh-handle-out-implib {args}
#
# [define]s LDFLAGS_OUT_IMPLIB to either an empty string or to a
# -Wl,... flag for the platform-specific --out-implib flag, which is
# used for building an "import library .dll.a" file on some platforms
# (e.g. msys2, mingw). Returns 1 if supported, else 0.
#
# If the configure flag --out-implib is not used then this is a no-op.
# If that flag is used but the capability is not available, a fatal
# error is triggered.
#
# The name of the import library is [define]d in ${libname}_OUT_IMPLIB
# and is initially based off of either --out-implib or the
# ${libname}_DLL_BASENAME define (see [wh-handle-dll-basename]).
#
# This feature is specifically opt-in because it's supported on far
# more platforms than actually need it and enabling it causes creation
# of libX.so.a files which are unnecessary in most environments.
#
# Added in response to: https://sqlite.org/forum/forumpost/0c7fc097b2
#
# Platform notes:
#
# - cygwin sqlite packages historically install no .dll.a file.
#
# - msys2 and mingw sqlite packages historically install
#   /usr/lib/libsqlite3.dll.a despite the DLL being in
#   /usr/bin/msys-sqlite3-0.dll.
#
# If passed -options, it uses [options-add] to add a configure flag
# named --out-implib then returns.
proc wh-handle-out-implib {args} {
  if {[wh-check-options-flag $args {
    out-implib:=auto
        => {Enable use of --out-implib linker flag to generate an
          "import library" for the DLL. The output's base name name is
          specified by the value, with "auto" meaning to figure out a
          name automatically. On some platforms this flag gets
          automatically enabled if it is not provided. Use "none" to
          explicitly disable this feature on such platforms.}
  }]} {
    return
  }
  lassign $args libname
  define LDFLAGS_OUT_IMPLIB ""
  define ${libname}_OUT_IMPLIB ""
  set rc 0
  if {[proj-opt-was-provided out-implib]} {
    set olBaseName [join [opt-val out-implib] ""]
    if {$olBaseName in {auto ""}} {
      set olBaseName [get-define ${libname}_DLL_BASENAME]
      # Based on discussions with mingw/msys users, the import lib
      # should always be called libsqlite3.dll.a even on platforms
      # which rename libsqlite3.dll to something else.
    }
    if {$olBaseName ne "none"} {
      cc-with {-link 1} {
        set dll "${olBaseName}[get-define TARGET_DLLEXT]"
        set flags [proj-cc-check-Wl-flag --out-implib ${dll}.a]
        if {"" ne $flags} {
          define LDFLAGS_OUT_IMPLIB $flags
          define ${libname}_OUT_IMPLIB ${dll}.a
          incr rc
        }
      }
      if {!$rc} {
        user-error "--out-implib is not supported on this platform"
      }
    }
  }
  return $rc
}

########################################################################
# @wh-check-cc-deps-gen ?c|c++?
#
# If the compiler claims to support -MD -MF X.d to generate
# dependencies, this function returns 1, else 0. Defines
# CC_DEPS_GEN_FLAG (if its first argument is "c") or CXX_DEPS_GEN_FLAG
# (if the arg is "C++") to an empty string or {-MD -MF} pr similar (which
# can hypothetically differ across compilers but that currently isn't
# handled).
#
# For C++, pass "c++" as the first argument.
#
proc wh-check-cc-deps-gen {{lang c}} {
  switch -exact -- $lang {
    c {
      set def CC_DEPS_GEN_FLAG
    }
    c++ {
      set def CXX_DEPS_GEN_FLAG
    }
    default {
      proj-error
    }
  }
  define $def ""
  set rc 0
  set msg "no"
  set flags "-MMD -MF"
  set tmpfile .deps-flags-check.tmp
  msg-checking "Checking [string toupper $lang] for compiler support of $flags ... "
  if {[cctest -lang $lang -cflags [list /dev/null {*}$flags $tmpfile] -source {}]} {
    proj-assert {[file exists $tmpfile]}
    set msg "yes"
    define $def $flags
    incr rc
  }
  file delete -force $tmpfile
  msg-result $msg
  return $rc
}

########################################################################
# @wh-handle-with-sqlite ?-options|localSrcDir?
#
# --with-sqlite=PATH checks for the first it finds of the following...
#
# - PATH/sqlite3.c and PATH/sqlite3.h
# - PATH/sqlite3.o (and assumes sqlite3.h is with it)
# - PATH/lib/libsqlite3* and PATH/include/sqlite3.h
# - PATH/libsqlite3* (and assumes sqlite3.h is with it)
#
# If PATH is empty or "auto" then it does a search for:
#
# - Library under $prefix/lib and headers in $prefix/include
# - System-level installation (no extra dir searches)
#
# Its argument is an optional source-tree-local directory where either
# sqlite3-see.c or sqlite3.c lives. It will prefer sqlite3-see.c over
# sqlite3.c. $localSrcDir may be relative to the current dir or to
# $::autosetup(srcdir).
#
# Returns the new value of HAVE_SQLITE3: 0 or 1.
#
# If passed "-options" this installs an autosetup configure flag
# named --with-sqlite and returns.
#
# Define:
#
# - SQLITE3_ORIGIN:
#   0 = sqlite3 not found
#   1 = local source tree ($localSrcDir)
#   2 = external sqlite3.c
#   3 = external sqlite3.o
#   4 = lib/headers from PATH/{lib,include}
#   5 = lib and headers from PATH/.
#   6 = use lib/headers under $prefix/{lib,include}
#   7 = use system-level installation
#
# - SQLITE3_SRC path to sqlite3.c (or sqlite3-see.c) or empty
#   string.
#
# - SQLITE3_OBJ path to sqlite3.o (with SQLITE3_ORIGIN=3) or empty
#   string.
#
# - CFLAGS_sqlite3 to CFLAGS needed for building against sqlite.
#
# - LDFLAGS_sqlite3 to LDFLAGS needed for building against sqlite,
#   including any external deps like -lpthread and -lm.  This is not
#   necessarily relevant for SQLITE3_ORIGIN of 1-3, as this file
#   cannot know which libs to link for those (so it guesses). Sidebar:
#   this should arguably contain SQLITE3_OBJ but doing so causes a
#   build-time grief in the distinction between a shared lib and a
#   static one.
#
# Noting that SQLITE3_ORIGIN is just informational - all of the
# necessary build pieces are stored in the other vars.
#
proc wh-handle-with-sqlite {{localSrcDir ""}} {
  if {[wh-check-options-flag $localSrcDir {
    with-sqlite:path|auto|tree|none
      => {Look for sqlite in the given path, automatically, or in the source tree.}
  }]} {
    return
  }
  set sq3path [opt-val with-sqlite]
  define SQLITE3_ORIGIN 0
  define SQLITE3_SRC {}
  define SQLITE3_OBJ {}
  define-append CFLAGS_sqlite3 {}
  define-append LDFLAGS_sqlite3 {}
  set ldfExtern {-lpthread -lm}; # conservative estimates for -lsqlite3 deps
  set rc 0
  set msg "SQLite not found"
  if {"none" eq $sq3path} {
    set msg "SQLite support specifically disabled"
    set ldfExtern {}
  } elseif {"" ne $localSrcDir && $sq3path in {tree ""}} {
    define SQLITE3_ORIGIN 1
    set src [lindex [glob -nocomplain \
                       ${localSrcDir}/sqlite3{-see,}.c \
                       $::autosetup(srcdir)/${localSrcDir}/sqlite3{-see,}.c] 0]
    if {$::autosetup(srcdir) eq $::autosetup(builddir)} {
      # Cosmetic
      set src [string map [list $::autosetup(srcdir)/ ""] $src]
    }
    #puts "sq3path=$sq3path src=$src"; exit
    set msg "SQLite: using $src from this source tree."
    define SQLITE3_SRC $src
    incr rc
  } else {
    if {$sq3path ni {auto {}}} {
      set srcC [lindex [glob -nocomplain ${sq3path}/sqlite3{-see,}.c] 0]
      if {[file exists $srcC] &&
          [file exists $sq3path/sqlite3.h] } {
        # Prefer sqlite3[-see].c and its accompanying sqlite3.h if
        # found.
        define SQLITE3_ORIGIN 2
        define-append SQLITE3_SRC $srcC
        define-append CFLAGS_sqlite3 -I$sq3path
        # ^^^ additional -lXXX flags are conservative estimates
        set msg "SQLite: using [file tail $srcC] and sqlite3.h from $sq3path"
        incr rc
      } elseif {[file exists $sq3path/sqlite3.o]} {
        # Use sqlite3.o if found.
        define SQLITE3_ORIGIN 3
        define SQLITE3_OBJ $sq3path/sqlite3.o
        define-append CFLAGS_sqlite3 -I$sq3path
        #define-append LDFLAGS_sqlite3 $sq3path/sqlite3.o
        # ^^^ don't do this, as it complicates differentiating between
        # building a shared lib and a static lib.
        set msg "SQLite: using sqlite3.o from $sq3path"
        incr rc
      } elseif { ([llength [glob -nocomplain -directory $sq3path/lib libsqlite3*]] != 0) \
                   && ([file exists $sq3path/include/sqlite3.h]) } {
        # e.g. --with-sqlite=/usr/local. Try $sq3path/lib/libsqlite3*
        # and $sq3path/include/sqlite3.h
        define SQLITE3_ORIGIN 4
        define-append CFLAGS_sqlite3 -I$sq3path/include
        define-append LDFLAGS_sqlite3 -L$sq3path/lib -lsqlite3
        # ^^^ additional -lXXX flags are conservative estimates
        set msg "SQLite: using -lsqlite3 from $sq3path"
        incr rc
      } else {
        # Assume $sq3path holds both the lib and header
        define SQLITE3_ORIGIN 5
        define-append CFLAGS_sqlite3 -I$sq3path
        define-append LDFLAGS_sqlite3 -L$sq3path -lsqlite3
        set msg "SQLite: assuming that -I$sq3path and -L$sq3path will work"
        incr rc
      }
    } else {
      # Look under --prefix. This should arguably come become a check for
      # a system-level one.
      set p [get-define prefix]
      set ccwopt [subst {
        -link 1
        -cflags -I${p}/include
        -libs {[list -L${p}/lib -lsqlite3]}
      }]
      #puts "prefix=$p opt=$ccwopt"
      cc-with $ccwopt {
        if {[cc-check-includes sqlite3.h] &&
            [msg-quiet cc-check-function-in-lib sqlite3_open_v2 sqlite3]} {
          # -----------^^^ says "no libs needed"?
          set msg "SQLite: using installation from --prefix=$p"
          define SQLITE3_ORIGIN 6
          define-append CFLAGS_sqlite3 -I$p/include
          define-append LDFLAGS_sqlite3 -L$p/lib -lsqlite3
          incr rc
        }
      }
      if {0 == $rc
          && [cc-check-includes sqlite3.h]
          && [cc-check-function-in-lib sqlite3_open_v2 sqlite3]} {
        # system-level installation
        define LDFLAGS_sqlite3 -lsqlite3
        define SQLITE3_ORIGIN 7
        incr rc
      }
    }
  }
  msg-result $msg
  define-append LDFLAGS_sqlite3 $ldfExtern
  return [define HAVE_SQLITE $rc]
}; # wh-handle-with-sqlite

# @wh-cc-check-flag flag defname ...flagN defnameN
#
# If [cc-check-flags $flag] returns true, [define $defname $flag] is
# performed, else [define $defname ""]. This function does not update
# the global CFLAGS. Returns the number of flags it set.
proc wh-cc-check-flag {args} {
  set i 0
  cc-with {} {
    foreach {flag defname} $args {
      if {[cc-check-flags $flag]} {
        define $defname "$flag"
        incr i
      } else {
        define $defname ""
      }
    }
  }
  return $i
}

#
# @wh-make-append targetVarName args...
#
# Appends makefile code to $targetVarName. Each arg may be any of:
#
# -space: enter spacing mode (see below) and emit no output
# -nospace: leave spacing mode and emit no output
# -tab: emit a literal tab
# -nl: emit a literal newline
# -nltab: short for -nl -tab
# -bnl: emit a backslash-escaped end-of-line
# -bnltab: short for -bnl -tab
#
# Anything else is appended verbatim except that empty arguments are
# treated as if they're not there. This function adds no additional
# spacing between each argument nor between subsequent invocations
# unless -space is used, which causes a space to be emitted before
# each following argument, and this remains active until the next flag
# is encountered. (It turns out that we almost invariably want to
# disable -space mode after emitting tabs and newlines.). As a special
# case, the value immediately after the -space flag will not get a
# space injected before it.
#
# Generally speaking, a series of calls to this function need to be
# sure to end the series with a newline.
proc wh-make-append {var args} {
  upvar $var out
  set space ""
  foreach a $args {
    switch -exact -- $a {
      ""      {continue}
      -bnl    { set a " \\\n"; set space "" }
      -bnltab { set a " \\\n\t"; set space "" }
      -tab    { set a "\t"; set space "" }
      -nl     { set a "\n"; set space ""  }
      -nltab  { set a "\n\t"; set space "" }
      -space    { set space " "; continue }
      -nospace  { set space ""; continue }
    }
    append out $space$a
  }
}

#proc wh-make-rule {opt} {
#  proj-parse-simple-flags opts f {
#   -target => {}
#    -deps => {}
#    -recipe => {}
#  }
#  if {"" eq $f(-target)} {
#    proj-error "-target NAME must be set"
#  }
#}


########################################################################
# Internal helper for [wh-check-line-editing]. Returns a list of
# potential locations under which readline.h might be found.
#
# On some environments this function may perform extra work to help
# wh-check-line-editing figure out how to find libreadline and
# friends. It will communicate those results via means other than the
# result value, e.g. by modifying configure --flags.
proc wh-get-readline-dir-list {} {
  # Historical note: the dirs list, except for the inclusion of
  # $prefix and some platform-specific dirs, originates from the
  # legacy configure script.
  set dirs [list [get-define prefix]]
  switch -glob -- [get-define host] {
    *-linux-android {
      # Possibly termux
      lappend dirs /data/data/com.termux/files/usr
    }
    *-mingw32 {
      lappend dirs /mingw32 /mingw
    }
    *-mingw64 {
      lappend dirs /mingw64 /mingw
    }
    *-haiku {
      lappend dirs /boot/system/develop/headers
      if {[opt-val with-readline-ldflags] in {auto ""}} {
        # If the user did not supply their own --with-readline-ldflags
        # value, hijack that flag to inject options which are known to
        # work on Haiku OS installations.
        if {"" ne [glob -nocomplain /boot/system/lib/libreadline*]} {
          proj-opt-set with-readline-ldflags {-L/boot/system/lib -lreadline}
        }
      }
    }
  }
  lappend dirs /usr /usr/local /usr/local/readline /usr/contrib
  set rv {}
  foreach d $dirs {
    if {[file isdir $d]} {lappend rv $d}
  }
  #proc-debug "dirs=$rv"
  return $rv
}

########################################################################
# wh-check-line-editing ?-options?
#
# If passed -options, this adds autoset CLI flag options for this
# feature and returns without doing any other work, otherwise...
#
# It jumps through proverbial hoops to try to find a working
# line-editing library, setting:
#
#   - HAVE_READLINE to 0 or 1
#   - HAVE_LINENOISE to 0, 1, or 2
#   - HAVE_EDITLINE to 0 or 1
#
# Only one of ^^^ those will be set to non-0.
#
#   - LDFLAGS_READLINE = linker flags or empty string
#
#   - CFLAGS_READLINE = compilation flags for clients or empty string.
#
#   - LINENOISE_C = empty string or path to linenoise*.c.
#
# Note that LDFLAGS_READLINE and CFLAGS_READLINE may refer to
# linenoise or editline, not necessarily libreadline.  In some cases
# it will set HAVE_READLINE=1 when it's really using editline, for
# reasons described in this function's comments.
#
# Returns true if it finds a match, else false.
#
# Linenoise notes: it will search the --with-linenoise=DIR for both
# linenoise-shipped.c and linenoise.c (the former being the name it
# generates in its own upstream build tree). If linenoise*.c are found
# then CFLAGS_READLINE holds the -I... part needed for building it and
# for clients which need to #include linenoise.h. LINENOISE_C will be
# defined to the full path to linenoise*.c. LDFLAGS_READLINE will be
# empty: it is up to the caller to arrange to compile and link
# LINENOISE_C into their app. (It does not require any external
# libraries.)
#
# Historical note: earlier versions included the path to linenoise*.c
# in CFLAGS_READLINE, but that breaks when passing them to specific
# build constructs. (cc -c x.c /path/to/linenoise.c) is not
# legal.
#
# Order of checks:
#
#  1) --with-linenoise trumps all others and skips all of the
#     complexities involved with the remaining options.
#
#  2) --editline trumps --readline
#
#  3) --disable-readline trumps --readline
#
#  4) Default to automatic search for optional readline
#
#  5) Try to find readline or editline. If it's not found AND the
#     corresponding --FEATURE flag was explicitly given then fail
#     fatally, else fail non-fatally.
proc wh-check-line-editing {args} {
  if {[wh-check-options-flag $args {
    readline=1  => {Disable readline support}
    with-readline-ldflags:=auto
      => {Readline LDFLAGS, e.g. -lreadline -lncurses}
    with-readline-cflags:=auto
      => {Readline CFLAGS, e.g. -I/path/to/includes}
    with-readline-header:PATH
      => {Full path to readline.h, from which --with-readline-cflags will be derived}
    with-linenoise:DIR
      => {Source directory for linenoise.c and linenoise.h}
    editline=0
      => {Enable BSD editline support}
  }]} {
    return
  }

  msg-result "Checking for line-editing capability..."
  define HAVE_READLINE 0
  define HAVE_LINENOISE 0
  define HAVE_EDITLINE 0
  define LDFLAGS_READLINE ""
  define CFLAGS_READLINE ""
  define LINENOISE_C ""
  set failIfNotFound 0 ; # Gets set to 1 for explicit --FEATURE requests
                         # so that we know whether to fail fatally or not
                         # if the library is not found.
  set libsForReadline {readline edit} ; # -l<LIB> names to check for readline().
                                        # The libedit check changes this.
  set editLibName "readline"     ; # "readline" or "editline"
  set editLibDef "HAVE_READLINE" ; # "HAVE_READLINE" or "HAVE_EDITLINE"
  set dirLn [opt-val with-linenoise]
  if {"" ne $dirLn} {
    # Use linenoise from a copy of its sources (not a library)...
    if {![file isdir $dirLn]} {
      proj-fatal "--with-linenoise value is not a directory"
    }
    set lnH $dirLn/linenoise.h
    if {![file exists $lnH] } {
      proj-fatal "Cannot find linenoise.h in $dirLn"
    }
    set lnC ""
    set lnCOpts {linenoise-ship.c linenoise.c}
    foreach f $lnCOpts {
      if {[file exists $dirLn/$f]} {
        set lnC $dirLn/$f
        break
      }
    }
    if {"" eq $lnC} {
      proj-fatal "Cannot find any of $lnCOpts in $dirLn"
    }
    set flavor ""
    set lnVal [proj-which-linenoise $lnH]
    switch -- $lnVal {
      1 { set flavor "antirez" }
      2 { set flavor "msteveb" }
      default {
        proj-fatal "Cannot determine the flavor of linenoise from $lnH"
      }
    }
    define CFLAGS_READLINE -I$dirLn
    define LINENOISE_C $lnC
    define HAVE_LINENOISE $lnVal
    msg-result "linenoise ($flavor)"
    return $lnVal
  } elseif {[opt-bool editline]} {
    # libedit mimics libreadline and on some systems does not have its
    # own header installed (instead, that of libreadline is used).
    #
    # shell.c historically expects HAVE_EDITLINE to be set for
    # libedit, but it then expects to see <editline/readline.h>, which
    # some system's don't actually have despite having libedit.  If we
    # end up finding <editline/readline.h> below, we will use
    # -DHAVE_EDITLINE=1, else we will use -DHAVE_READLINE=1. In either
    # case, we will link against libedit.
    set failIfNotFound 1
    set libsForReadline {edit}
    set editLibName editline
  } elseif {![opt-bool readline]} {
    msg-result "Readline support explicitly disabled with --disable-readline"
    msg-result "none"
    return 0
  } elseif {[proj-opt-was-provided readline]} {
    # If an explicit --[enable-]readline was used, fail if it's not
    # found, else treat the feature as optional.
    set failIfNotFound 1
  }

  # Transform with-readline-header=X to with-readline-cflags=-I...
  set v [opt-val with-readline-header]
  proj-opt-set with-readline-header ""
  if {"" ne $v} {
    if {"auto" eq $v} {
      proj-opt-set with-readline-cflags auto
    } else {
      set v [file dirname $v]
      if {[string match */readline $v]} {
        # Special case: if the path includes .../readline/readline.h,
        # set the -I to one dir up from that because our sources
        # #include <readline/readline.h> or <editline/readline.h>.
        set v [file dirname $v]
      }
      proj-opt-set with-readline-cflags "-I$v"
    }
  }

  # Look for readline.h
  set rlInc [opt-val with-readline-cflags auto]
  if {"auto" eq $rlInc} {
    set rlInc ""
    if {[proj-is-cross-compiling]} {
      # ^^^ this check is derived from the legacy configure script.
      proj-warn "Skipping check for readline.h because we're cross-compiling."
    } else {
      set dirs [wh-get-readline-dir-list]
      set subdirs [list \
                     include/$editLibName \
                     readline]
      if {"editline" eq $editLibName} {
        lappend subdirs include/readline
        # ^^^ editline, on some systems, does not have its own header,
        # and uses libreadline's header.
      }
      lappend subdirs include
      set rlInc [proj-search-for-header-dir readline.h \
                   -dirs $dirs -subdirs $subdirs]
      #proc-debug "rlInc=$rlInc"
      if {"" ne $rlInc} {
        if {[string match */readline $rlInc]} {
          set rlInc [file dirname $rlInc]; # CLI shell: #include <readline/readline.h>
        } elseif {[string match */editline $rlInc]} {
          set editLibDef HAVE_EDITLINE
          set rlInc [file dirname $rlInc]; # CLI shell: #include <editline/readline.h>
        }
        set rlInc "-I${rlInc}"
      }
    }
  } elseif {"" ne $rlInc && ![string match *-I* $rlInc]} {
    proj-fatal "Argument to --with-readline-cflags is intended to be CFLAGS and contain -I..."
  }

  # If readline.h was found/specified, look for lib(readline|edit)...
  #
  # This is not quite straightforward because both libreadline and
  # libedit typically require some other library which (according to
  # legacy autotools-generated tests) provides tgetent(3). On some
  # systems that's built into libreadline/edit, on some (most?) its in
  # lib[n]curses, and on some it's in libtermcap.
  set rlLib ""
  if {"" ne $rlInc} {
    set rlLib [opt-val with-readline-ldflags]
    #proc-debug "rlLib=$rlLib"
    if {$rlLib in {auto ""}} {
      set rlLib ""  ; # make sure it's not "auto", as we may append to it below
      set libTerm ""; # lib with tgetent(3)
      if {[proj-check-function-in-lib tgetent [list $editLibName ncurses curses termcap]]} {
        # ^^^ that libs list comes from the legacy configure script ^^^
        set libTerm [get-define lib_tgetent]
        undefine lib_tgetent
      }
      if {$editLibName eq $libTerm} {
        # tgetent(3) was found in the editing library
        set rlLib $libTerm
      } elseif {[proj-check-function-in-lib readline $libsForReadline $libTerm]} {
        # tgetent(3) was found in an external lib
        set rlLib [get-define lib_readline]
        lappend rlLib $libTerm
        undefine lib_readline
      }
    }
  }

  # If we found a library, configure the build to use it...
  if {"" ne $rlLib} {
    if {"editline" eq $editLibName && "HAVE_READLINE" eq $editLibDef} {
      # Alert the user that, despite outward appearances, we won't be
      # linking to the GPL'd libreadline. Presumably that distinction is
      # significant for those using --editline.
      proj-indented-notice {
        NOTE: the local libedit uses <readline/readline.h> so we
        will compile with -DHAVE_READLINE=1 but will link with
        libedit.
      }
    }
    set rlLib [join $rlLib]
    set rlInc [join $rlInc]
    define LDFLAGS_READLINE $rlLib
    define CFLAGS_READLINE $rlInc
    proj-assert {$editLibDef in {HAVE_READLINE HAVE_EDITLINE}}
    proj-assert {$editLibName in {readline editline}}
    msg-result "Using $editLibName flags: $rlInc $rlLib"
    # Check whether rl_completion_matches() has a signature we can use
    # and disable that sub-feature if it doesn't.
    if {![cctest -cflags "$rlInc -D${editLibDef}" -libs $rlLib -nooutput 1 \
            -source {
             #include <stdio.h>
             #ifdef HAVE_EDITLINE
             #include <editline/readline.h>
             #else
             #include <readline/readline.h>
             #endif
             static char * rcg(const char *z, int i){(void)z; (void)i; return 0;}
             int main(void) {
               char ** x = rl_completion_matches("one", rcg);
               (void)x;
               return 0;
             }
           }]} {
      proj-warn "readline-style completion disabled due to rl_completion_matches() signature mismatch"
    }
    msg-result "Using $editLibName"
    return 1
  }

  if {$failIfNotFound} {
    proj-fatal "Explicit --$editLibName failed to find a matching library."
  }
  msg-result "no line-editing support found"
  return 0
}; # wh-check-line-editing


########################################################################
# Check for the Emscripten SDK for building the web-based wasm
# components.
#
# Its one optional argument is either:
#
#   -options: adds the --with-emsdk flag to autosetup's flags and returns.
#
#   wrapperScript: the name of an input tempate for an emcc wrapper
#     script.  It gets queued for filtering by
#     [proj-dot-ins-process]. The extension will be strippped from the
#     filtered copy and it will be made executable.
#
# Much of the work is done via [proj-check-emsdk], then this function
# adds the following defines:
#
# - EMCC_WRAPPER = "" or ${wrapperScript}
# - BIN_WASM_OPT = "" or path to wasm-opt
# - BIN_WASM_STRIP = "" or path to wasm-strip
#
# Noting that:
#
# 1) Not finding the SDK is not fatal at this level, nor is failure to
#    find one of the related binaries.
#
# 2) wasm-strip is part of the wabt package:
#
#   https://github.com/WebAssembly/wabt
#
proc wh-handle-with-emsdk {args} {
  if {[wh-check-options-flag $args {
      with-emsdk:=auto
        => {Top-most dir of the Emscripten SDK installation.
            Default=EMSDK env var.}
  }]} {
    return
  }
  define EMCC_WRAPPER ""
  define BIN_WASM_STRIP ""
  define BIN_WASM_OPT ""
  set srcdir $::autosetup(srcdir)
  if {![get-define HAVE_WASI_SDK] && [proj-check-emsdk]} {
    set emsdkHome [get-define EMSDK_HOME ""]
    proj-assert {"" ne $emsdkHome}
    #define EMCC_WRAPPER ""; # just for testing
    proj-bin-define wasm-strip
    set wo $emsdkHome/upstream/bin/wasm-opt
    if {[file-isexec $wo]} {
      msg-result "Looking for wasm-opt ... $wo"
      # ^^^ for consistency with proj-bin-define
      define BIN_WASM_OPT $wo
    } else {
      # Maybe there's a copy in the path?
      proj-bin-define wasm-opt BIN_WASM_OPT
    }
    set emccSh [lindex $args 0]
    if {"" ne $emccSh} {
      set w [file root [file tail $emccSh]]
      define EMCC_WRAPPER $w
      proj-dot-ins-append $emccSh $w {
        catch {exec chmod u+x $dotInsOut}
      }
      file delete -force -- $w
    } else {
      define EMCC_WRAPPER ""
    }
  }
}

########################################################################
# wh-handle-soname ?-options? libBaseName
#
# "soname" for libsqlite3.so. See discussion at:
# https://sqlite.org/src/forumpost/5a3b44f510df8ded
#
# If passed the -options flag it adds the --soname option to the
# autosetup flags and returns an empty string. Its other argument is a
# library base name in the form of the part which conventionally goes
# after "lib" in "libfoo". ("foo" is in fact the default if one is not
# given).
#
# Sets [define]s as per [proj-check-soname] plus sets LDFLAGS_SONAME
# to the ldflags needed to set an soname for the given lib.
#
# Returns 0 if the feature is unsupported, 1 if it is is. If the
# --soname flag is explicitly provided by a user but it is not
# supported in this environment (and by this code) then it fails
# fatally instead.  If --soname was not explicitly provided then
# failure is not fatal.
proc wh-handle-soname {args} {
  if {[wh-check-options-flag $args {
    soname:=none
    => {Sets the SONAME for a shared library. "none", or not using this
        flag, sets no soname. A value matching the glob "lib*" sets it
        to that literal value. Any other value is assumed to be a suffix
        which gets applied to "libNAME.so.", e.g. --soname=9.10 equates
        to "libNAME.so.9.10".}
  }]} {
    return
  }
  lassign $args libname
  if {"" eq $libname} {
    set libname foo
  }
  define LDFLAGS_SONAME ""
  if {[proj-opt-was-provided soname]} {
    set soname [join [opt-val soname] ""]
  } else {
    # Enabling soname can break in-build-tree linking, so default to
    # none. Package maintainers, on the other hand, like to have an
    # soname.
    set soname none
  }
  switch -exact -- $soname {
    none - "" { return 0 }
    default {
      if {[string match lib* $soname]} {
        # use it as-is
      } else {
        # Assume it's a suffix
        set soname "lib${libname}.so.${soname}"
      }
    }
  }
  proc-debug "soname=$soname"
  if {[proj-check-soname $soname]} {
    define LDFLAGS_SONAME [get-define LDFLAGS_SONAME_PREFIX]$soname
    msg-result "Setting SONAME using: [get-define LDFLAGS_SONAME]"
  } elseif {[proj-opt-was-provided soname]} {
    # --soname was explicitly requested but not available, so fail fatally
    proj-fatal "This environment does not support SONAME."
  } else {
    # --soname was not explicitly requested but not available, so just warn
    msg-result "This environment does not support SONAME."
  }
}
