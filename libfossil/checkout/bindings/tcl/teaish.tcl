#
# A teaish configure script for the tclfossil extension (libfossil via tcl)
#
teaish-pkginfo-set {
  -name libfossil
  -version 0.1
  -vsatisfies {{Tcl 8.6-}}
  -url {https://fossil.wanderinghorse.net/r/libfossil}
  -pkgInit.tcl fossil.tcl
  -pragmas {no-full-dist}
  -options {
    f-f-d
    ftcl-full-dist => {Maintainer-level option.}
  }
}

# Gets called by teaish-configure-core after bootstrapping is complete.
proc teaish-options {} {
  use wh-common
  wh-handle-with-sqlite -options
}

# Gets called by teaish-configure-core after processing of --flags.
proc teaish-configure {} {
  proj-xfer-options-aliases {
    f-f-d  => ftcl-full-dist
  }

  if {![wh-cc-check-c99 0]} {
    user-error "Compiler doesn't claim to support C99."
  }

  use teaish/feature
  if {![teaish-check-libz]} {
    user-error "This build requires libz."
  }
  if {![teaish-check-libmath]} {
    user-error "This build requires libz."
  }

  set dx [teaish-get -dir]
  set df [file-normalize $dx/../..]
  define LIBFOSSIL_DIR $df

  set fls [define FTCL_LIBFOSSIL_SOURCE 0]
  if {[file exists $df/tools/text2c.c]} {
    # We're very probably in the canonical libfossil tree.
    set fls [define FTCL_LIBFOSSIL_SOURCE 1]
  }
  if {[opt-bool ftcl-full-dist]} {
    # We're ostensibly inside a checkout of the canonical libfossil
    # tree.
    proj-assert {$fls == 1} \
      "--ftcl-full-dist can only be used from within the libfossil source tree"
    set fls [define FTCL_LIBFOSSIL_SOURCE 2]

    # libfossil*.* are pulled into this tree via the makefile
    teaish-dist-add  \
      generic/libfossil.c \
      generic/libfossil.h \
      generic/libfossil-config.h

    teaish-pkginfo-set -pragmas full-dist
    proj-indented-notice {
      Enabling full-full dist mode. Remember that we do not
      yet dynamically locate sqlite3, so you must pass
      "LDFLAGS=-L/path -lsqlite3" to build this variant.
    }
  } elseif {[file exists $dx/generic/libfossil.c]} {
    # This is a dist build from a --t-f-d
    define LIBFOSSIL_DIR ""; #set df ""
    set fls [define FTCL_LIBFOSSIL_SOURCE 3]
  }
  switch $fls {
    0 {
      user-error "Could not determine which variant of the build to use."
    }
    default {
      if {![proj-opt-was-provided with-sqlite] && [file exists $df/extsrc/sqlite3.c]} {
        proj-opt-set with-sqlite $df/extsrc
        msg-result "Using sqlite from $df/extsrc"
      }
    }
  }

  if {![wh-handle-with-sqlite]} {
    user-error "libsqlite3 not found. Use --with-sqlite to specify one."
  }

  if {[file exists _config.tcl]} {
    source _config.tcl
    msg-result "Read in externally-provided _config.tcl"
  }

  if {"" eq [get-define CPPFLAGS_sqlite ""]} {
    define CPPFLAGS_sqlite ""
  }

  # TODO:
  # SQLITE3_ORIGIN (1, 2): build sqlite3.c
#  switch [get-define SQLITE3_ORIGIN] {
#    1 - 2 {
#    }
#  }

  switch $fls {
    1 {
      # Building in a checkout of the canonical tree
      teaish-cflags-add -I$dx/generic -I$df
      #if {[get-define SQLITE3_ORIGIN] ne {1 2}} {
      #  teaish-cflags-add -I$df/extsrc
      #}
    }
    2 {
      # Like 1, but in "ftcl-full-dist" mode
      teaish-cflags-add -I$dx/generic -I$df
    }
    3 {
      # building from generic/libfossil.[ch]
      teaish-cflags-add -I$dx/generic
    }
    default {
      user-error "An impossible thing has happened"
    }
  }

  teaish-cflags-add  \$(CFLAGS.sqlite3)
  teaish-ldflags-add \$(LDFLAGS.sqlite3)

  set srcLib {
    generic/th.c generic/th.h
    generic/ftcl.c generic/ftcl.h
    generic/db.c generic/ls.c generic/udf.c
  }
  set srcShell {
    generic/shell.c
  }
  teaish-dist-add {*}$srcLib {*}$srcShell
  # Per-file CFLAGS, CPPFLAGS, and/or LDFLAGS
  array set perFile {
    generic/shell.c.cflags {-DHAVE_LINENOISE=1}
  }

  # Create makefile rules for the .o files
  set mo {}
  set libObj {}
  set shellObj {}
  foreach f [list {*}$srcLib {*}$srcShell] {
    if {![string match *.c $f]} continue
    set fo [file tail [file rootname $f]].o
    set cpf {}
    if {[info exists perFile($f.cflags)]} {
      append cpf $perFile($f.cflags)
    }
    wh-make-append mo "$fo: $dx/$f \$(ftcl.src-deps-common)" -nltab \
      -space \$(CC.tcl) -c $dx/$f {*}$cpf -nl
    if {$f in $srcShell} {
      lappend shellObj $fo
    } else {
      lappend libObj $fo
    }
  }

  define FTCL_LIB_OBJ $libObj
  define FTCL_SHELL_OBJ $shellObj
  define RULES_FTCL_OBJ $mo

  teaish-define-to-cflag -quote TEAISH_PKGNAME TEAISH_VERSION
  teaish-define-to-cflag TEAISH_LOAD_PREFIX
  teaish-cflags-add \
    -g -Wall -Werror \
    [get-define CC_FLAG_C99 ""]
  if {1} {
    teaish-cflags-add -DTH_F_FREE=ftcl_free -DTH_F_REALLOC=ftcl_realloc
  } else {
    teaish-cflags-add -DTH_OOM_ABORT=1
  }

}; # teaish-configure
