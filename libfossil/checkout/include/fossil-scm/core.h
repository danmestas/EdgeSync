/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
#if !defined(ORG_FOSSIL_SCM_FSL_CORE_H_INCLUDED)
#define ORG_FOSSIL_SCM_FSL_CORE_H_INCLUDED
/*
  Copyright 2013-2025 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/

#include <stdbool.h>

/** @file core.h

  This file declares the core SCM-related public APIs.
*/

#include "fossil-scm/util.h" /* MUST come first b/c of config macros */
#include <time.h> /* struct tm, time_t */
#if defined(__cplusplus)
/**
  The fsl namespace is reserved for an eventual C++ wrapper for the API.
*/
namespace fsl {}
extern "C" {
#endif

/**
    An internal helper macro to help localize OOM error reports (which
    are most often side effects of other issues, rather than being
    real OOM cases). It's intended to be used like:

    ```
    void * someAllocedMemory = fsl_malloc(...); // any allocating routine
    if(!someAllocedMemory){
      fsl_report_oom;
      rc = FSL_RC_OOM;
      goto end;
    }
    ```

    or:

    ```
    if(FSL_RC_OOM==fs){ fsl_report_oom; }
    ```

    Note that this macro _may_ expand to encompass multiple commands
    so must never be used as the RHS of an `if` without squiggly
    braces surrounding it (as in the above example).

    This macro is _not_ invoked in _every_ possible OOM case, so
    cannot simply be replaced with abort() to kill the app on OOM.
    For that, apply a custom allocator via fsl_lib_configurable.
*/
#if !defined(fsl_report_oom_impl)
#define fsl_report_oom_impl \
  fprintf(stderr,"OOM @ %s:%d\n", __FILE__, __LINE__)
#endif

/* older name fos fsl_report_oom */
#define FSL__WARN_OOM fsl_report_oom_impl
#define fsl_report_oom fsl_report_oom_impl

/**
   @struct fsl_cx

   The main Fossil "context" type. This is the first argument to
   many Fossil library API routines, and holds all state related
   to a checkout and/or repository and/or global fossil configuration
   database(s).

   An instance's lifetime looks something like this:

   ```
   int rc;
   fsl_cx * f = NULL; // ALWAYS initialize to NULL or results are undefined
   rc = fsl_cx_init( &f, NULL );
   assert(!rc);
   rc = fsl_repo_open( f, "myrepo.fsl" );
   ...use the context, and clean up when done...
   fsl_cx_finalize(f);
   ```

   The contents of an fsl_cx instance are strictly private, for use
   only by APIs in this library. Any client-side dependencies on
   them will lead to undefined behaviour at some point.
*/
typedef struct fsl_cx_config fsl_cx_config;
typedef struct fsl_db fsl_db;
typedef struct fsl_cx_init_opt fsl_cx_init_opt;
typedef struct fsl_stmt fsl_stmt;
//typedef struct fsl__cx fsl_cx;
typedef struct fsl_cx fsl_cx;

/**
   This enum defines type ID tags with which the API tags fsl_db
   instances so that the library can figure out which DB is
   which. This is primarily important for certain queries, which
   need to know whether they are accessing the repo or config db,
   for example.

   As of 2021-10, the fossil context db model has long been, and is
   very, very likely to remain:

   - When a context starts up, it creates a temporary db for use
     as its `main` db.

   - When a repo or checkout db are opened, they are `ATTACH`ed to the
     main db with the names `repo` resp. `ckout`. (Note that those
     differ from the names fossil(1) uses: `repository` and
     `localdb`.)

   A large amount of the library code is written _as if_ the library
   (roughly) followed fossil's model of using separate sqlite3
   objects, even though it does not do so. The reason is historical:
   when starting the libfossil port, it was clear that fossil's
   internal connection juggling was going to be painful for the
   library, so the internal API was shaped to permit two separate
   approaches:

   Approach 1: the first db which gets opened (repo, checkout, or
   config) becomes the `main` db and all others get attached using
   their API-standard alias. That is _mostly_ what fossil does, but it
   leads to fossil sometimes having to "swap" connections by closing
   the db and re-opening it so that the db names are in a state which
   fits the current use. That model led to the creation of this
   enum. However, it also leads to unpredictable database names for
   purposes of queries, as we're never sure (at the client level or
   higher library-level APIs) which db is `main` and which has an
   alias.

   Approach 2: open a temp (or in-memory) db and attach all of the
   others to that using well-defined names.

   The latter has proven to work well enough that returning to the
   first approach seems _extremely_ unlikely at this point. Thus some
   newer library-level code behaves as if the latter model is in effect
   (which it is).

   The long and the short of the above diversion is that APIs like
   fsl_cx_db_repo() and fsl_cx_db_ckout() will always return the same
   database handle, but they can tell whether a given role has been
   attached or not and will fail in their documented ways when the
   role's corresponding database has not yet been attached. e.g.
   `fsl_cx_db_repo()` will return `NULL` if a repo database is not
   attached.

   @see fsl_db_role_name()
   @see fsl_cx_db_file_for_role()
*/
enum fsl_dbrole_e {
/**
   Sentinel "no role" value.
*/
FSL_DBROLE_NONE = 0,
/**
   The global (per user) config db. Analog to fossil's "configdb".
*/
FSL_DBROLE_CONFIG = 0x01,
/**
   The repository db. Analog to fossil's "repository".
*/
FSL_DBROLE_REPO = 0x02,
/**
   The checkout db. Analog to fossil's "localdb".
*/
FSL_DBROLE_CKOUT = 0x04,
/**
   Analog to fossil's "main", which is an alias for the first db
   opened. In this API a fsl_cx instance has a temporary/transient
   main db opened even if it does not have a repository, checkout, or
   config db opened.
*/
FSL_DBROLE_MAIN = 0x08,
/**
   Refers to the "temp" database. This is only used by a very few APIs
   and is outright invalid for most.
*/
FSL_DBROLE_TEMP = 0x10
};
typedef enum fsl_dbrole_e fsl_dbrole_e;

/**
   Bitmask values specifying "configuration sets".  The values in
   this enum come directly from fossil(1), but they are not part of
   the db structure, so may be changed over time.

   It seems very unlikely that these will ever be used at the level of
   this library. They are a "porting artifact" and retained for the
   time being, but will very likely be removed.
*/
enum fsl_configset_e {
/** Sentinel value. */
FSL_CONFSET_NONE = 0x000000,
/** Style sheet only */
FSL_CONFIGSET_CSS = 0x000001,
/** WWW interface appearance */
FSL_CONFIGSET_SKIN = 0x000002,
/** Ticket configuration */
FSL_CONFIGSET_TKT = 0x000004,
/** Project name */
FSL_CONFIGSET_PROJ = 0x000008,
/** Shun settings */
FSL_CONFIGSET_SHUN = 0x000010,
/** The USER table */
FSL_CONFIGSET_USER = 0x000020,
/** The CONCEALED table */
FSL_CONFIGSET_ADDR = 0x000040,
/** Transfer configuration */
FSL_CONFIGSET_XFER = 0x000080,
/** Everything */
FSL_CONFIGSET_ALL = 0x0000ff,
/** Causes overwrite instead of merge */
FSL_CONFIGSET_OVERWRITE = 0x100000,
/** Use the legacy format */
FSL_CONFIGSET_OLDFORMAT = 0x200000
};
typedef enum fsl_configset_e fsl_configset_e;

/**
   Runtime-configurable flags for a fsl_cx instance.

   @see fsl_cx_flag_set()
   @see fsl_cx_flags_get()
*/
enum fsl_cx_flags_e {
/**
   The "no flags" value. Guaranteed to be 0 and this is the only entry
   in this enum which is guaranteed to have a stable value.
*/
FSL_CX_F_NONE = 0,
/**
   Tells us whether or not we want to calculate R-cards by default.
   Historically they were initially required but eventually made
   optional due largely to their memory costs.
*/
FSL_CX_F_CALC_R_CARD = 0x01,

/**
   When encounting artifact types in the crosslinking phase which
   the library does not currently support crosslinking for, skip over
   them instead of generating an error. For day-to-day use this is,
   perhaps counter-intuitively, generally desirable.

   As of 2021-12-04, crosslinking of all core fossil artifact types is
   supported, so this flag is effectively a no-op until/unless a new
   artifact type is added to fossil.
*/
FSL_CX_F_SKIP_UNKNOWN_CROSSLINKS = 0x02,

/**
   By default, fsl_reserved_fn_check() will fail if the given filename
   is reserved on Windows platforms because such filenames cannot be
   checked out on Windows.  This flag removes that limitation. It
   should only be used, if at all, for repositories which will _never_
   be used on Windows.
*/
FSL_CX_F_ALLOW_WINDOWS_RESERVED_NAMES = 0x04,

/**
   If on (the default) then an internal cache will be used for
   artifact loading to speed up operations which do lots of that.
   Disabling this will save memory but may hurt performance for
   certain operations.
*/
FSL_CX_F_MANIFEST_CACHE = 0x08,

/**
   If on (the default) then fsl_content_get() will use an internal
   cache to speed up loading of repeatedly-fetched artifacts.
   Disabling this can be costly.
*/
FSL_CX_F_BLOB_CACHE = 0x10,

/**
   Internal use only to prevent duplicate initialization of some
   bits.
*/
FSL_CX_F_IS_OPENING_CKOUT = 0x100,

/**
   Default flags for all fsl_cx instances.
*/
FSL_CX_F_DEFAULTS = FSL_CX_F_MANIFEST_CACHE | FSL_CX_F_BLOB_CACHE

};
typedef enum fsl_cx_flags_e fsl_cx_flags_e;

/**
    List of hash policy values. New repositories should generally use
    only SHA3 hashes, but older repos may contain SHA1 hashes (perhaps
    only SHA1), so we have to support those. Repositories may contain
    a mix of hash types.

    Maintenance ACHTUNG: this enum's values must align with those from
    fossil(1) because their integer values are used in the
    `repo.config` table.
*/
enum fsl_hashpolicy_e {
/** Use only SHA1 hashes. */
FSL_HPOLICY_SHA1 = 0,
/** Accept SHA1 hashes but auto-promote to SHA3. */
FSL_HPOLICY_AUTO = 1,
/** Use SHA3 hashes. */
FSL_HPOLICY_SHA3 = 2,
/** Use SHA3 hashes exclusively. */
FSL_HPOLICY_SHA3_ONLY = 3,
/** With this policy, fsl_uuid_is_shunned() will always return true
    for SHA1 hashes, making it "impossible" to get SHA1-hashed content
    into the repository (for a given value of "impossible"). */
FSL_HPOLICY_SHUN_SHA1 = 4
};
typedef enum fsl_hashpolicy_e fsl_hashpolicy_e;


/**
   A macro which invokes its argument (a macro name) to expand to all
   possible values of fsl_rc_e entries. The macro name passed to it is
   invoked once for each entry and passed 3 arguments: the enum
   entry's full name (FSL_RC_...), its integer value, and a help-text
   string.
*/
#define fsl_rc_e_map(E) \
  E(FSL_RC_OK, 0, \
    "The quintessential not-an-error value.")    \
  E(FSL_RC_ERROR, 100, \
    "Generic/unknown error.")                                           \
  E(FSL_RC_NYI, 101, \
    "A placeholder return value for not yet implemented functions.")    \
  E(FSL_RC_OOM, 102, \
    "Out of memory. Indicates that a resource allocation request failed.") \
  E(FSL_RC_MISUSE, 103, \
    "API misuse (invalid args).")                                       \
  E(FSL_RC_RANGE, 104, \
    "Some range was violated (function argument, UTF character, etc.).") \
  E(FSL_RC_ACCESS, 105, \
    "Indicates that access to or locking of a resource was denied "     \
    "by some security mechanism or other.")                             \
  E(FSL_RC_IO, 106, \
    "Indicates an I/O error. Whether it was reading or "                \
    "writing is context-dependent.")                                    \
  E(FSL_RC_NOT_FOUND, 107, \
    "Requested resource not found")                                     \
  E(FSL_RC_ALREADY_EXISTS, 108,                                         \
    "Indicates that a to-be-created resource already exists.")          \
  E(FSL_RC_CONSISTENCY, 109, \
    "Data consistency problem")                                         \
  E(FSL_RC_REPO_NEEDS_REBUILD, 110,                                     \
    "Indicates that the requested repo needs to be rebuilt.")           \
  E(FSL_RC_NOT_A_REPO, 111,                                             \
    "Indicates that the requested repo is not, in fact, a repo. "       \
    "Also used by some APIs to indicate that they require a "           \
    "repository db but none has been opened.")                          \
  E(FSL_RC_REPO_VERSION, 112,                                           \
    "Indicates an attempt to open a too-old or too-new repository db.") \
  E(FSL_RC_DB, 113, \
    "Indicates db-level error (e.g. statement prep failed). In such "   \
    "cases, the error state of the related db handle (fsl_db) or "      \
    "Fossil context (fsl_cx) will be updated to contain more "          \
    "information directly from the db driver.")                         \
  E(FSL_RC_BREAK, 114, \
    "Used by some iteration routines to indicate that "                 \
    "iteration should stop prematurely without an error.")              \
  E(FSL_RC_STEP_ROW, 115, \
    "Indicates that fsl_stmt_step() has fetched a row and "             \
    "the cursor may be used to access the current row state (e.g. using " \
    "fsl_stmt_get_int32() and friends). It is strictly illegal to use " \
    "the fsl_stmt_get_xxx() APIs unless fsl_stmt_step() has returned "  \
    "this code.")                                                       \
  E(FSL_RC_STEP_DONE, 116, \
    "Indicates that fsl_stmt_step() has reached the end of "            \
    "the result set and that there is no row data to process. This is also the " \
    "result for non-fetching queries (INSERT and friends). It is strictly " \
    "illegal to use the fsl_stmt_get_xxx() APIs after fsl_stmt_step() has " \
    "returned this code.")                                              \
  E(FSL_RC_STEP_ERROR, 117, \
    "Indicates that a db-level error occurred "                         \
    "during a fsl_stmt_step() iteration.")                              \
  E(FSL_RC_TYPE, 118,                                                   \
    "Indicates that some data type or logical type is incorrect "       \
    "(e.g. an invalid card type in conjunction with a given fsl_deck).") \
  E(FSL_RC_NOT_A_CKOUT, 119,                                            \
    "Indicates that an operation which requires a checkout does not "   \
    "have a checkout to work on.")                                      \
  E(FSL_RC_REPO_MISMATCH, 120, \
    "Indicates that a repo and checkout do not belong together.")       \
  E(FSL_RC_CHECKSUM_MISMATCH, 121,                                      \
    "Indicates that a checksum comparison failed, possibly indicating " \
    "that corrupted or unexpected data was just read.")                 \
  E(FSL_RC_LOCKED, 122, \
    "Indicates a resource-locking error of some sort, normally a db lock.") \
  E(FSL_RC_CONFLICT, 123,                                               \
    "Indicates that a merge conflict, or some other context-dependent " \
    "type of conflict, was detected.")                                  \
  E(FSL_RC_UNKNOWN_RESOURCE, 124, \
    "This is a special case of FSL_RC_NOT_FOUND, intended specifically " \
    "to differentiate from \"file not found in filesystem\" " \
    "(FSL_RC_NOT_FOUND) and \"fossil does not know about this file\" in " \
    "routines for which both might be an error case. An example is a " \
    "an operation which wants to update a repo file with contents " \
    "from the filesystem - the file might not exist or it might not be " \
    "in the current repo db. " \
    "That said, this can also be used for APIs which search for other " \
    "resources (UUIDs, tickets, etc.), but FSL_RC_NOT_FOUND is already " \
    "fairly well entrenched in those cases and is unambiguous, so this " \
    "code is only needed by APIs for which both cases described above " \
     "might happen." ) \
  E(FSL_RC_SIZE_MISMATCH, 125, \
    "Indicates that a size comparison check failed. " \
    "TODO: remove this if it is not used." )          \
  E(FSL_RC_DELTA_INVALID_SEPARATOR, 126, \
    "Indicates that an invalid separator was encountered while " \
    "parsing a delta." )                                         \
  E(FSL_RC_DELTA_INVALID_SIZE, 127, \
    "Indicates that an invalid size value was encountered while " \
    "parsing a delta." )                                          \
  E(FSL_RC_DELTA_INVALID_OPERATOR, 128, \
    "Indicates that an invalid operator was encountered while parsing " \
    "a delta.")                                             \
  E(FSL_RC_DELTA_INVALID_TERMINATOR, 129, \
    "Indicates that an invalid terminator was encountered while " \
    "parsing a delta.")                                        \
  E(FSL_RC_SYNTAX, 130, \
    "Indicates a generic syntax error in a structural artifact. Some " \
     "types of manifest-releated errors are reported with more specific " \
    "error codes, e.g. FSL_RC_RANGE if a given card type appears too " \
    "often.")                                                          \
  E(FSL_RC_AMBIGUOUS, 131, \
    "Indicates that some value or expression is ambiguous. Typically " \
    "caused by trying to resolve ambiguous symbolic names or hash " \
    "prefixes to their full hashes.")                               \
  E(FSL_RC_NOOP, 132, \
    "Used by fsl_checkin_commit(), and similar operations, to indicate " \
    "that they're failing because they would be no-ops. That would " \
    "normally indicate a \"non-error\", but a condition the caller " \
    "certainly needs to know about.")                                \
  E(FSL_RC_PHANTOM, 133, \
    "A special case of FSL_RC_NOT_FOUND which indicates that the " \
    "requested repository blob could not be loaded because it is a " \
    "phantom. That is, the record is found but its contents are not " \
    "available. Phantoms are blobs which fossil knows should exist, " \
    "because it's seen references to their hashes, but for which it does " \
    "not yet have any content.")                                        \
  E(FSL_RC_UNSUPPORTED, 134, \
    "Indicates that the requested operation is unsupported.")   \
  E(FSL_RC_MISSING_INFO, 135, \
    "Indicates that the requested operation is missing certain required " \
    "information.")                                         \
  E(FSL_RC_DIFF_BINARY, 136, \
    "Special case of FSL_RC_TYPE triggered in some diff APIs, indicating " \
    "that the API cannot diff what appears to be binary data.") \
  E(FSL_RC_DIFF_WS_ONLY, 137, \
    "Triggered by some diff APIs to indicate that only whitespace " \
    "changes we found and the diff was requested to ignore whitespace.") \
  E(FSL_RC_INTERRUPTED, 138, \
    "Intended to be used with fsl_cx_interrupt() by signal handlers " \
    "and UI threads.")                                                \
  E(FSL_RC_WOULD_FORK, 139, \
    "Intended to be used by operations which would cause what is " \
    "presumably an unintended fork. Fossil does not have any issues with " \
    "forking, but practice suggests that most forks (that is, checking " \
    "in to a non-leaf version) are unintentional.")                     \
  E(FSL_RC_CANNOT_HAPPEN, 140, \
    "This is intended only for internal use with fsl__fatal(), to " \
    "report conditions which \"cannot possibly happen\".") \
  E(FSL_RC_TIMEOUT, 141, \
    "Indicates that a context-specific timeout was triggered.") \
  E(FSL_RC_REMOTE, 142, \
    "Indicates that an error was received from a remote during " \
    "syncing.")                                                  \
  E(FSL_RC_SHUNNED, 143, \
    "Indicates that a given blob is in the SHUNNED list.")  \
  E(FSL_RC_end,999, \
    "Must be the final entry in the enum. Used for creating client-side " \
    "result codes which are guaranteed to live outside of this one's " \
    "range.")

/**
   Most functions in this library which return an int return result
   codes from the fsl_rc_e enum.  None of these entries are guaranteed
   to have a specific value across library versions except for
   FSL_RC_OK, which is guaranteed to always be 0 (and the API
   guarantees that no other code shall have a value of zero).

   The only reasons numbers are hard-coded to the values is to
   simplify debugging during development. Clients may use
   fsl_rc_cstr() to get some human-readable (or programmer-readable)
   form for any given value in this enum.
*/
enum fsl_rc_e {

#define E(N,V,H) N = V,
  fsl_rc_e_map(E)
#undef E

};
typedef enum fsl_rc_e fsl_rc_e;

/**
   File permissions flags supported by fossil manifests. Their numeric
   values are a hard-coded part of the Fossil architecture and must
   not be changed. Note that these refer to manifest-level permissions
   and not filesystem-level permissions (though they translate to/from
   filesystem-level meanings at some point).
*/
enum fsl_fileperm_e {
/** Indicates a regular, writable file. */
FSL_FILE_PERM_REGULAR = 0,
/** Indicates a regular file with the executable bit set. */
FSL_FILE_PERM_EXE = 0x1,
/**
   Indicates a symlink. Note that symlinks do not have the executable
   bit set separately on Unix systems. Also note that libfossil does
   NOT YET IMPLEMENT symlink support like fossil(1) does - it
   currently treats symlinks (mostly) as Unix treats symlinks.
*/
FSL_FILE_PERM_LINK = 0x2
};
typedef enum fsl_fileperm_e fsl_fileperm_e;

/**
   Returns a "standard" string form for a fsl_rc_e code.  The string
   is primarily intended for debugging purposes.  The returned bytes
   are guaranteed to be static and NUL-terminated. They are not
   guaranteed to contain anything useful for any purposes other than
   debugging and tracking down problems. If passed a code which is
   not in the fsl_rc_e enum, it returns NULL.
*/
FSL_EXPORT char const * fsl_rc_cstr(int rc);

/**
   Returns the value of FSL_LIBRARY_VERSION used to compile the
   library. If this value differs from the value the caller was
   compiled with, Chaos might ensue.

   The API does not yet have any mechanism for determining
   compatibility between library versions and it also currently does
   no explicit checking to disallow incompatible versions.
*/
FSL_EXPORT char const * fsl_library_version(void);

/**
   Returns the SCM-specific version info for libfossil (the hash and
   timestamp of it's own source's version).
*/
FSL_EXPORT char const * fsl_library_version_scm(void);

/**
   Returns true (non-0) if yourLibVersion compares lexically equal to
   FSL_LIBRARY_VERSION, else it returns false (0). It is intended to
   be passed the FSL_LIBRARY_VERSION string the client code was built
   with.
*/
FSL_EXPORT bool fsl_library_version_matches(char const * yourLibVersion);

/**
   This type, accessible to clients via the ::fsl_lib_configurable
   global, contains configuration-related data for the library
   which can be swapped out by clients.
*/
struct fsl_lib_configurable_t {
  /**
     Library-wide allocator. It may be replaced by the client IFF it
     is replaced before the library allocates any memory. The default
     implementation uses the C-standard de/re/allocators.  Modifying
     this member while any memory allocated through it is still "live"
     leads to undefined results. There is an exception: a "read-only"
     middleman proxy which does not change how the memory is allocated
     or intepreted can safely be swapped in or out at any time
     provided the underlying allocator stays the same and the client
     can ensure that there are no thread-related race
     conditions. e.g. it is legal to swap this out with a proxy which
     logs allocation requests and then forwards the call on to the
     original implementation, and it is legal to do so at essentially
     any time. The important thing this that all of the
     library-allocated memory goes through a single underlying
     (de)allocator for the lifetime of the application.
  */
  fsl_allocator allocator;
};
typedef struct fsl_lib_configurable_t fsl_lib_configurable_t;
FSL_EXPORT fsl_lib_configurable_t fsl_lib_configurable;

/**
   SQLite Encryption Extension (SEE) key types for use with
   fsl_see_key_f(). Each of these options corresponds to one
   of the key-entry modes supported by SEE via pragmas,
   e.g. PRAGMA key, PRAGMA textkey, and PRAGMA hexkey.
*/
enum fsl_see_keytype_e {
  /** Sentinel value indicating "not an SEE key". */
  FSL_SEE_KEYTYPE_NONE = 0,
  /** Corresponses to (PRAGMA key). */
  FSL_SEE_KEYTYPE_PLAIN = 1,
  /** Corresponses to (PRAGMA hexkey). Binary data to be hex-encoded
      by the library. */
  FSL_SEE_KEYTYPE_BINARY = 2,
  /** Corresponses to (PRAGMA hexkey). Encoded by the client as a
      series of hex digits. */
  FSL_SEE_KEYTYPE_HEXKEY = 3,
  /** Corresponses to (PRAGMA textkey). */
  FSL_SEE_KEYTYPE_TEXTKEY = 4
};
typedef enum fsl_see_keytype_e fsl_see_keytype_e;

/**
   EXPERIMENTAL and subject to change. But it seems to do its job
   reasonably well.

   A callback type for fetching an encryption key for use with the
   SQLite Encryption Extension API. This is only for use in
   SEE-capable builds.

   `pState` is the pointer provided by the client for use here
   (typically via fsl_cx_config::see::pState). It is opaque to the
   library, intended solely for the SEE client's use.

   `zDbFile` is the NUL-terminated name of the db for which the key is
   being requested.

   If a key is available, it must be appended to pOut using
   fsl_buffer_append() (or equivalent).

   On success, it must return 0. Any non-0 result value is treated by
   the library as failure to open the db, with one exception:
   FSL_RC_UNSUPPORTED indicates that decryption is not supported but
   opening of a database will not fail fatally for this result.
   Instead, if the db really is encrypted, it will fail later when the
   db is first used.

   If it populates pOut then it must set `*keyType` to one of the
   entries from the fsl_see_keytype_e enum, indicating the type of the
   key.

   If the implementation has no key to provide for the db, but does
   not know that to be an error, it should simply return 0 and not add
   any bytes to pOut. It need not set `*keyType` in this case, as the
   library guarantees that it will initially have the value
   FSL_SEE_KEYTYPE_NONE.

   The library does not cache the results of these calls and may need
   to call this multiple times for the same database during the life
   of a single context. (Ordinarily, though, the SCM databases are
   opened a single time and left that way.)

   A recommended convention, derived from fossil(1), is to name
   encrypted repositories with a suffix of ".efossil". Implementations
   of this callback could use such a convention to determine whether
   or not a key is necessary for the db (something this library cannot
   do on its own).
*/
typedef int (*fsl_see_key_f)(void * pState, const char *zDbFile,
                             fsl_buffer * pOut, int *keyType);

/**
   Expands to a mapping of all fsl_msg types by invoking
   E(NAME,VALUE,PayloadDescription) for each one. NAME is the suffix
   part of the name, e.g. mask_type instead of
   FSL_MSG_mask_type. VALUE is its integer
   value. PayloadDescription is a human-legible string typically
   noting the data type of the payload.

   Each entry encodes the data type corresponding to the associated
   fsl_msg object's payload member: mask the message type value
   against FSL_MSG_mask_type to get the payload's type.

   This mechanism was added for the sync subsystem and the goal is now
   to replace some of the library's disparate callbacks with this
   mechanism.
*/
#define fsl_msg_map(E)                                     \
  E(INVALID,           0, "Sentinel value")                \
  E(mask_type,         0x7F000000, "Data type mask")       \
  E(mask_reserved,     0x00FFFF00, "Reserved")             \
  E(mask_purpose,      0x000000FF, "Message purpose ID")   \
  E(type_null,         0x01000000, "NULL")                 \
  E(type_string,       0x02000000, "(char const *)")       \
  E(type_buffer,       0x03000000, "(fsl_buffer const *)") \
  E(type_size,         0x04000000, "(fsl_size_t const *)") \
  E(type_blob,         0x06000000, "(fsl_msg_blob const *)" ) \
  E(type_rebuild_step, 0x07000000, "(fsl_rebuild_step const *)" )  \
  E(ERROR,         0x01 | FSL_MSG_type_string, "Error string") \
  E(INFO,          0x02 | FSL_MSG_type_string, "Info string")  \
  E(DEBUG,         0x03 | FSL_MSG_type_string, "Debugging string") \
  E(TMPFILE,       0x04 | FSL_MSG_type_string, "value=name of temp file left over") \
  E(CONNECT,       0x05 | FSL_MSG_type_size, "value=trip number") \
  E(RCV_BLOB,      0x06 | FSL_MSG_type_blob, "Blob received")     \
  E(RCV_MESSAGE,   0x07 | FSL_MSG_type_string, "\"message\" card from remote") \
  E(RCV_CONFIG,    0x08 | FSL_MSG_type_string, "Config group name") \
  E(REBUILD_STEP,  0x09 | FSL_MSG_type_rebuild_step, "") \
  E(REBUILD_DONE,  0x0A | FSL_MSG_type_rebuild_step, "") \
  E(TXN_BEGIN,     0x0B | FSL_MSG_type_size, "value=transaction level") \
  E(TXN_COMMIT,    0x0C | FSL_MSG_type_size, "value=transaction level") \
  E(TXN_ROLLBACK,  0x0D | FSL_MSG_type_size, "value=transaction level")

/**
   Message type flags Indicators for use with fsl_sc::status,
   fsl_xfer_emit(), and friends.
*/
enum fsl_msg_e {
#define E(NAME,VAL,IGNORED) FSL_MSG_ ## NAME = VAL,
  fsl_msg_map(E)
#undef E
};

typedef enum fsl_msg_e fsl_msg_e;

/**
   Message payload type for FSL_MSG_type_blob.
*/
struct fsl_msg_blob {
  char const * hash;
  fsl_int_t size /* negative for phantom blobs */;
};
typedef struct fsl_msg_blob fsl_msg_blob;

/**
   Returns the C-string form of the given sync message type, or NULL
   if e is invalid.
*/
char const * fsl_msg_type_cstr(fsl_msg_e e);

/**
   A "transfer context" type used by many of the sync transfer
   APIs. This type is opaque to client code.
*/
typedef struct fsl__xfer fsl_xfer;

/**
   A message object type for use with fsl_sc::status().
*/
struct fsl_msg {
  /** The type of status message. */
  fsl_msg_e type;

  /**
     An optional bit of informative text. If set, the bytes are owned
     by the library and may be (and likely are) invalidated as soon as
     the message callback returns. The data type depends on this->type
     and it can be interpreted by deciphering fsl_msg_e.
  */
  void const * payload;

  /**
     This message's fsl_cx context, providing the callback with access
     to the associated SCM state.
  */
  fsl_cx * f;
};

typedef struct fsl_msg fsl_msg;

/** fsl_msg instance intended for const-copy initialization. */
#define fsl_msg_empty_m { \
    .type = FSL_MSG_INVALID, .payload = NULL, \
    .f = NULL \
  }

/** fsl_msg instance intended for non-const copy initialization. */
extern const fsl_msg fsl_msg_empty;

/**
   A callback for use with the library's message-passing subsystem, to
   provide feedback during potentially long-running work, in
   particular synchronization with a remote.

   Implementations are passed the a message object and any state
   associated with the callback.

   If the function returns non-0, the operation is aborted and that
   result is propagated back up through the API.  On error, this
   callback is expected to call either of fsl_cx_err_set() or the
   fsl_xfer_error() family of functions and return that code.

   Messages are synchronous - the library must wait on a response from
   this function before continuing. Ergo, clients should not perform
   long work in the status callback (or should do so in a separate
   thread which _does_not_ use msg->f (which is not
   thread-safe)).

   This method is intended primarily for logging or non-interactive
   feedback, with the exception of the ability to return non-0 to
   interrupt the sync. e.g. an implementation could return
   FSL_RC_BREAK in response to an application-side cancel/interrupt
   button being triggered.

   When triggered via a sync operation, the library guarantees that a
   db transaction will be in place when it calls this, a further
   incentive to keep implementations short and fast because the db is
   necessarily locked during that time.
*/
typedef int (*fsl_msg_f)(fsl_msg const * msg, void *state);

/**
   Client callback info for receiving various events during
   fossil sync processing.
*/
struct fsl_msg_listener {
  /** Callback function. */
  fsl_msg_f callback;
  /** State for the final argument to this->callback(). */
  void * state;
};
typedef struct fsl_msg_listener fsl_msg_listener;
#define fsl_msg_listener_empty_m { .callback=0, .state=0 }
extern const fsl_msg_listener fsl_msg_listener_empty;


/**
   A part of the configuration used by fsl_cx_init() and friends.
*/
struct fsl_cx_config {
  /**
     If true, all SQL which goes through the fossil engine will be
     traced to stdout. This is, of course, only intended for use in
     debugging.

     TODO: replace this with a fsl_outputer.
  */
  bool traceSql;
  /**
     If true, the fsl_print() SQL function will output its output to the
     fsl_output()-configured channel, else it is a no-op.
  */
  bool sqlPrint;

  /**
     Specifies the default hash policy.
  */
  fsl_hashpolicy_e hashPolicy;

  /**
     Listener for fsl_msg-type events. This is how the library
     provides status updates for potentially long-running operations
     like synchronization.
  */
  fsl_msg_listener listener;

  /**
     The output channel for the Fossil instance.

     Reminder to self: if this is not used at the library level then
     consider removing it. This addition may have been a piece of
     unnecessary overengineering. To the best of my recollection, the
     library internals do not use this, but the fcli API does. If that
     is indeed the case, we can move it into that API.
  */
  fsl_outputer output;

  /**
     Config state for use with the SQLite3 Encryption Extension (SEE).
     This state is only useful together with encrypted databases.
  */
  struct {
    /**
       If this is non NULL, it is called when the library needs to
       fetch the encryption key to use for a db. Its first
       argument will be the pState member of this struct.

       Note that the library does not actually know whether it is built
       with SEE support or not.
    */
    fsl_see_key_f getSEEKey;
    /**
       Optional state for use as the first argument to getSEEKey. This
       may be NULL, so long as the getSEEKey argument accepts
       NULL. Ownership of this pointer is the client's
       responsibility. It must survive as as long as any fsl_cx which
       it is assigned to, but the library cannot know when (or
       whether) to clean it up.
    */
    void * pState;
  } see;
};

/**
   fsl_cx_config instance initialized with defaults, intended for
   const-copy initialization.
*/
#define fsl_cx_config_empty_m {     \
    .traceSql = 0,                  \
    .sqlPrint = 0,                  \
    .hashPolicy = FSL_HPOLICY_SHA3, \
    .listener = fsl_msg_listener_empty_m, \
    .output = {.out=NULL, .state=NULL}, \
    .see = {.getSEEKey = NULL, .pState = NULL}  \
}

/**
   fsl_cx_config instance initialized with defaults, intended for
   non-const copy initialization.
*/
FSL_EXPORT const fsl_cx_config fsl_cx_config_empty;

/**
   Allocates a new fsl_cx instance, which must eventually
   be passed to fsl_cx_finalize() to clean it up.
   Normally clients do not need this - they can simply pass
   a pointer to NULL as the first argument to fsl_cx_init()
   to let it allocate an instance for them.
*/
FSL_EXPORT fsl_cx * fsl_cx_malloc(void);

/**
   Initializes a fsl_cx instance. tgt must be a pointer to NULL,
   e.g.:

   ```
   fsl_cxt * f = NULL; // NULL is important - see below
   int rc = fsl_cx_init( &f, NULL );
   ```

   It is very important that f be initialized to NULL _or_ to an
   instance which has been properly allocated and empty-initialized
   (e.g. via fsl_cx_malloc()). If *tgt is NULL, this routine
   allocates the context, else it assumes the caller did. If f
   points to unitialized memory then results are undefined.

   If the second parameter is NULL then default implementations are
   used for the context's output routine and other options. If it
   is not NULL then param->allocator and param->output must be
   initialized properly before calling this function. The contents
   of param are bitwise copied by this function and ownership of
   the returned value is transfered to *tgt in all cases except
   one:

   If passed a pointer to a NULL context and this function cannot
   allocate it, it returns FSL_RC_OOM and does not modify *tgt. In
   this one case, ownership of the context is not changed (as there's
   nothing to change!). On any other result (including errors),
   ownership of param's contents are transfered to *tgt and the client
   is responsible for passing *tgt ot fsl_cxt_finalize() when he is
   done with it. Note that (like in sqlite3), *tgt may be valid memory
   even if this function fails, and the caller must pass it to
   fsl_cx_finalize() whether or not this function succeeds unless it
   fails at the initial OOM (which the client can check by seeing if
   (*tgt) is NULL, but only if he set it to NULL before calling this).

   Returns 0 on success, FSL_RC_OOM on an allocation error,
   FSL_RC_MISUSE if (!tgt). If this function fails, it is illegal to
   use the context object except to pass it to fsl_cx_finalize(), as
   explained above.

   @see fsl_cx_finalize()
*/
FSL_EXPORT int fsl_cx_init( fsl_cx ** tgt, fsl_cx_config const * param );

/**
   Frees all memory associated with f, which must have been
   allocated/initialized using fsl_cx_malloc(), fsl_cx_init(), or
   equivalent, or created on the stack and properly initialized (via
   fsl_cx_init() or copy-constructed from fsl_cx_empty).  If it was
   allocated using fsl_cx_malloc(), this function frees f, else the
   memory is owned by someone else and it is not freed.

   This function triggers any finializers set for f's client state
   or output channel.

   This is a no-op if !f and is effectively a no-op if f has no
   state to destruct.
*/
FSL_EXPORT void fsl_cx_finalize( fsl_cx * f );


/**
   Sets or unsets one or more option flags on the given fossil
   context.  flags is the flag or a bitmask of flags to set (from the
   fsl_cx_flags_e enum).  If enable is true the flag(s) is (are) set,
   else it (they) is (are) unset. Returns the _previous_ set of flags
   (that is, the state they were in before this call was made).

   @see fsl_cx_flags_get()
*/
FSL_EXPORT fsl_flag32_t fsl_cx_flag_set( fsl_cx * f, fsl_flag32_t flags,
                                         bool enable );

/**
   Returns f's flags.

   @see fsl_cx_flag_set()
*/
FSL_EXPORT fsl_flag32_t fsl_cx_flags_get( fsl_cx const * f );

/**
   Sets the Fossil error state to the given error code and
   fsl_appendf()-style format string/arguments. On success it
   returns the code parameter. It does not return 0 unless code is
   0, and if it returns a value other than code then something went
   seriously wrong (e.g. allocation error: FSL_RC_OOM) or the
   arguments were invalid: !f results in FSL_RC_MISUSE.

   If !fmt then fsl_rc_cstr(code) is used to create the
   error string.

   As a special case, if code is FSL_RC_OOM, no error string is
   allocated (because it would likely fail, assuming the OOM
   is real).

   As a special case, if code is 0 (the non-error value) then fmt is
   ignored and any error state is cleared.
*/
FSL_EXPORT int fsl_cx_err_set( fsl_cx * f, int code, char const * fmt, ... );

/**
   va_list counterpart to fsl_cx_err_set().
*/
FSL_EXPORT int fsl_cx_err_setv( fsl_cx * f, int code, char const * fmt,
                                va_list args );

/**
   Fetches the error state from f. See fsl_error_get() for the semantics
   of the parameters and return value.
*/
FSL_EXPORT int fsl_cx_err_get( fsl_cx * f, char const ** str, fsl_size_t * len );


/**
   Returns f's error object. It is owned by f, and its address is
   stable as long as f is valid. It's provided as a semi-formal way
   for not-quite-core code to get access to f->error without having to
   reach into f directly.  It should only be preferred over
   fsl_cx_err_set()/get() when an arbitrary fsl_error object is needed
   for an API (like fsl_buffer_escape_arg()) and we know we'd like to
   unconditionally propagate that error via f's error state. In such
   cases, using this object can avoid a second allocation to copy the
   error message.
*/
FSL_EXPORT fsl_error const * fsl_cx_err_ec(fsl_cx const * f);

/** Non-const counterpart of fsl_cx_err_ec(). */
FSL_EXPORT fsl_error * fsl_cx_err_e(fsl_cx * f);

/**
   Resets's f's error state, basically equivalent to
   fsl_cx_err_set(f,0,NULL). This may be necessary for apps if they
   rely on looking at fsl_cx_err_get() at the end of their
   app/routine, because error state survives until it is cleared, even
   if the error held there was caught and recovered. This function
   might keep error string memory around for re-use later on.

   This does NOT reset the fsl_cx_interrupted() flag!
*/
FSL_EXPORT void fsl_cx_err_reset(fsl_cx * f);

/**
   Replaces f's error state with the contents of err, taking over
   any memory owned by err (but not err itself). Returns the new
   error state code (the value of err->code before this call) on
   success. The only error case is if !f (FSL_RC_MISUSE). If err is
   NULL then f's error state is cleared and 0 is returned. err's
   error state is cleared by this call.
*/
FSL_EXPORT int fsl_cx_err_set_e( fsl_cx * f, fsl_error * err );

/**
   If f has error state then it outputs its error state to its output
   channel and returns the result of fsl_output(). Returns 0 if f has
   no error state our output of the state succeeds. If addNewline is
   true then it adds a trailing newline to the output, else it does
   not.

   This is intended for testing and debugging only, and not as an
   error reporting mechanism for a full-fledged application.
*/
FSL_EXPORT int fsl_cx_err_report( fsl_cx * f, bool addNewline );

/**
   Unconditionally Moves db->error's state into f (without requiring
   any allocation). If db is NULL then f's primary db connection is
   used. Returns FSL_RC_MISUSE if (!db && f-is-not-opened), with the
   caveat f _always_ has a db connection under the current connection
   architecture. On success it returns f's new error code (which may be
   0).

   The main purpose of this function is to propagate db-level errors
   up to higher-level code which deals directly with the f object but
   not the underlying db(s).

   @see fsl_cx_uplift_db_error2()
*/
FSL_EXPORT int fsl_cx_uplift_db_error( fsl_cx * f, fsl_db * db );

/**
   If rc is not 0 and f has no error state but db does, this calls
   fsl_cx_uplift_db_error() and returns its result, else returns
   rc. If db is NULL, f's main db connection is used. It is intended
   to be called immediately after calling a db operation which might
   have failed, and passed that operation's result.

   As a special case, if rc is FSL_RC_OOM, this function has no side
   effects and returns rc. The intention of that is to keep a
   propagated db-level error (which may perhaps be stale by the time
   this is called) from hiding an OOM error.

   Results are undefined if db is NULL and f has no main db
   connection.
*/
FSL_EXPORT int fsl_cx_uplift_db_error2(fsl_cx * f, fsl_db * db, int rc);

/**
   Outputs the first n bytes of src to f's configured output
   channel. Returns 0 on success, 0 (without side effects) if !n, else
   it returns the result of the underlying output call. This is a
   harmless no-op if f is configured with no output channel.

   Results are undefined if f or src are NULL.

   @see fsl_outputf()
   @see fsl_flush()
*/
FSL_EXPORT int fsl_output( fsl_cx * f, void const * src,
                           fsl_size_t n );

/**
   Flushes f's output channel. Returns 0 on success. If the flush
   routine is NULL then this is a harmless no-op. Results are undefined
   if f is NULL.

   @see fsl_outputf()
   @see fsl_output()
*/
FSL_EXPORT int fsl_flush( fsl_cx * f );

/**
   Uses fsl_appendf() to append formatted output to the channel
   configured for use with fsl_output(). Returns 0 on success, and
   will otherwise return the result of the underlying fsl_cx_format()
   call. (Prior to 2025-08-11, it used fsl_appendf() instead.)

   Results are undefined if f or fmt are NULL or otherwise malformed.

   @see fsl_output()
   @see fsl_flush()
*/
FSL_EXPORT int fsl_outputf( fsl_cx * f, char const * fmt, ... );

/**
   va_list counterpart to fsl_outputf().
*/
FSL_EXPORT int fsl_outputfv( fsl_cx * f, char const * fmt, va_list args );


/**
   A proxy for fsl_appendfv() which pre-processes the format-specifier
   string (zFmt) to replace certain placeholders with information
   about the fsl_cx object. After doing so, it passes on the adjusted
   zFmt to fsl_appendfv().

   Placeholders are in the form {{X}}, where X is one of the words
   listed below. Any unknown X will cause the whole {{X}} to be
   emitted as-is.

   Known X values (with quotes for clarity, but the quotes are not
   part of X):

   - "chkout.dir": gets replaced with the path to the current checkout
     directory, without a trailing slash.

   - "chkout.dir/": as for ckout-dir but with a trailing slash.

   - "repo.db": gets replaced with the path to the current repository
     database.

   - "user.name": gets replaced with fsl_cx_user_guess().

   File and directory names which are empty are emitted as such.

   The parser is not robust enough to attempt to recover from failures
   related to the "{{" and "}}" parts, and malformed tags may cause it
   to skip expansion on the remaining parts of the input (it will be
   passed on as-is, warts and all).

   Returns 0 on success. Returns FSL_RC_OOM if allocation of the
   formatting buffer fails. Else returns the value of the
   fsl_appendfv() call.
*/
FSL_EXPORT int fsl_cx_formatv(fsl_cx * f, fsl_output_f out,
                              void * outState, char const *zFmt,
                              va_list args );

/**
   The elipsis counterpart of fsl_cx_formatv().
*/
FSL_EXPORT int fsl_cx_format(fsl_cx * f, fsl_output_f out,
                             void * outState, char const *zFmt, ... );

/**
   Convenience formof fsl_cx_format() which appends the formatted
   output to tgt.
*/
FSL_EXPORT int fsl_cx_format_buffer(fsl_cx * f, fsl_buffer * tgt,
                                    char const *zFmt, ... );

/**
   Convenience formof fsl_cx_format() which appends the formatted
   output to tgt.
*/
FSL_EXPORT int fsl_cx_format_FILE(fsl_cx * f, FILE * tgt,
                                  char const *zFmt, ... );


/**
   Sets or clears (if userName is NULL or empty) the default
   repository user name for operations which require one.

   Returns 0 on success, FSL_RC_MISUSE if f is NULL,
   FSL_RC_OOM if copying of the userName fails.

   Example usage:
   ```
   char * u = fsl_user_name_guess();
   int rc = fsl_cx_user_set(f, u);
   fsl_free(u);
   ```

   (Sorry about the extra string copy there, but adding a function
   which passes ownership of the name string seems like overkill.)

   @see fsl_cx_user_guess()
*/
FSL_EXPORT int fsl_cx_user_set( fsl_cx * f, char const * userName );

/**
   If f has a user name set (via fsl_cx_user_set()) then this function
   returns that value. If none has been set, this tries to guess one,
   using the first of:

   - The "default-user" value from the checkout db's vvar table.
   - The user entry from the repo db's user table with id 1.
   - fsl_user_name_guess()

   and then assigns it as f's current user name. Returns NULL only on
   allocation error or if an environment setup error prevents
   detection of the user name. Not having a user name is not normally
   a problem, but trying to create new artifacts will fail if they
   have no user name to apply to their U-cards.

   The returned bytes are owned by f and will be invalidated by
   any future calls to fsl_cx_user_set().

   Note that the user name is, by default, determined automatically
   when a repo or a checkout/repo combination is opened, so client
   code does not normally need to call this.
*/
FSL_EXPORT char const * fsl_cx_user_guess(fsl_cx * f);

/**
   Returns the name set by fsl_cx_user_set(), or NULL if f has no
   default user name set. The returned bytes are owned by f and will
   be invalidated by any future calls to fsl_cx_user_set().

   @see fsl_cx_user_guess()
*/
FSL_EXPORT char const * fsl_cx_user_get( fsl_cx const * f );

/**
   If f is not NULL and has a checkout db opened then this function
   returns its name. The bytes are valid until that checkout db
   connection is closed. If len is not NULL then *len is (on
   success) assigned to the length of the returned string, in
   bytes. The string is NUL-terminated, so fetching the length (by
   passing a non-NULL 2nd parameter) is optional.

   Returns NULL if !f or f has no checkout opened.

   @see fsl_ckout_open_dir()
   @see fsl_cx_ckout_dir_name()
   @see fsl_cx_db_file_config()
   @see fsl_cx_db_file_repo()
*/
FSL_EXPORT char const * fsl_cx_db_file_ckout(fsl_cx const * f,
                                             fsl_size_t * len);

/**
   Equivalent to fsl_ckout_db_file() except that
   it applies to the name of the opened repository db,
   if any.

   @see fsl_cx_db_file_ckout()
   @see fsl_cx_db_file_config()
*/
FSL_EXPORT char const * fsl_cx_db_file_repo(fsl_cx const * f,
                                            fsl_size_t * len);

/**
   Equivalent to fsl_ckout_db_file() except that
   it applies to the name of the opened config db,
   if any.

   @see fsl_cx_db_file_ckout()
   @see fsl_cx_db_file_repo()
*/
FSL_EXPORT char const * fsl_cx_db_file_config(fsl_cx const * f,
                                              fsl_size_t * len);

/**
   Similar to fsl_cx_db_file_ckout() and friends except that it
   applies to db file implied by the specified role (2nd
   parameter). If no such role is opened, or the role is invalid,
   NULL is returned.

   Note that the role of FSL_DBROLE_TEMP is invalid here.
*/
FSL_EXPORT char const * fsl_cx_db_file_for_role(fsl_cx const * f,
                                                fsl_dbrole_e r,
                                                fsl_size_t * len);

/**
   If f has an opened checkout db (from fsl_ckout_open_dir()) then
   this function returns the directory part of the path for the
   checkout, including (for historical and internal convenience
   reasons) a trailing slash. The returned bytes are valid until that
   db connection is closed. If len is not NULL then *len is (on
   success) assigned to the length of the returned string, in bytes.
   The string is NUL-terminated, so fetching the length by passing a
   non-NULL 2nd parameter is optional.

   Returns NULL if !f or f has no checkout opened.

   @see fsl_ckout_open_dir()
   @see fsl_ckout_db_file()
*/
FSL_EXPORT char const * fsl_cx_ckout_dir_name(fsl_cx const * f,
                                              fsl_size_t * len);

/**
   Returns a handle to f's main db (which may or may not have any
   relationship to the repo/checkout/config databases - that's
   unspecified!), or NULL if f has no opened repo or checkout db.  The
   returned object is owned by f and the client MUST NOT do any of the
   following:

   - Close the db handle.

   - Use transactions without using fsl_db_txn_begin()
   and friends.

   - Fiddle with the handle's internals. Doing so might confuse its
   owning context.

   Clients MAY add new user-defined functions, use the handle with
   fsl_db_prepare(), and other "mundane" db-related tasks.

   Note that the global config db uses a separate db handle accessible
   via fsl_cx_db_config().

   @see fsl_cx_db_repo()
   @see fsl_cx_db_ckout()
   @see fsl_cx_db_config()
*/
FSL_EXPORT fsl_db * fsl_cx_db( fsl_cx * f );

/**
   If f is not NULL and has had its repo opened via
   fsl_repo_open(), fsl_ckout_open_dir(), or similar, this
   returns a pointer to that database, else it returns NULL.

   @see fsl_cx_db()
*/
FSL_EXPORT fsl_db * fsl_cx_db_repo( fsl_cx * f );

/**
   If f is not NULL and has had a checkout opened via
   fsl_ckout_open_dir() or similar, this returns a pointer to that
   database, else it returns NULL.

   @see fsl_cx_db()
*/
FSL_EXPORT fsl_db * fsl_cx_db_ckout( fsl_cx * f );

/**
   A helper which fetches f's repository db. If f has no repo db
   then it sets f's error state to FSL_RC_NOT_A_REPO with a message
   describing the requirement, then returns NULL.  Returns NULL if
   !f.

   @see fsl_cx_db()
   @see fsl_cx_db_repo()
   @see fsl_needs_ckout()
   @see fsl_cx_has_ckout()
*/
FSL_EXPORT fsl_db * fsl_needs_repo(fsl_cx * f);

/**
   The checkout-db counterpart of fsl_needs_repo(). If no checkout is
   opened, f's error state is updated with a FSL_RC_NOT_A_CKOUT code
   and description of the problem.

   @see fsl_cx_db()
   @see fsl_needs_repo()
   @see fsl_cx_db_ckout()
   @see fsl_cx_has_ckout()
*/
FSL_EXPORT fsl_db * fsl_needs_ckout(fsl_cx * f);

/**
   Returns true if the given context has a checkout opened, else
   false.

   @see fsl_needs_ckout()
*/
FSL_EXPORT bool fsl_cx_has_ckout(fsl_cx const * f );

/**
   Opens the given database file as f's configuration database.

   If f already has a config database opened then:

   1) If passed a NULL dbName or dbName is an empty string then this
   function returns 0 without side-effects.

   2) If passed a non-NULL/non-empty dbName, any existing config db is
   closed before opening the named one. The database is created and
   populated with an initial schema if needed.

   If dbName is NULL or empty then it uses a default db name,
   "probably" under the user's home directory (see
   fsl_config_global_preferred_name()). To get the name of the
   database after it has been opened/attached, use
   fsl_cx_db_file_config().

   Results are undefined if f is NULL or not properly initialized.

   @see fsl_cx_db_config()
   @see fsl_config_close()
   @see fsl_config_global_preferred_name()
*/
FSL_EXPORT int fsl_config_open( fsl_cx * f, char const * dbName );

/**
   Closes/detaches the database connection opened by
   fsl_config_open(). If the config db is not opened, this
   is a harmless no-op. Note that it does not propagate db-closing
   errors because there is no sensible recovery strategy from
   such cases.

   This operation only fails if the config db is opened and has
   an active transaction, in which case f's error state is updated
   to reflect that cause of the error.

   ACHTUNG: it is imperative that any prepared statements compiled
   against the config db be finalized before closing the db. Any
   statements prepared using fsl_db_prepare_cached() against the
   config db will be automatically finalized by the closing process.

   Potential TODO: if a transaction is pending, force a rollback and
   close the db anyway. If we do that, this function will change to
   return void.

   @see fsl_cx_db_config()
   @see fsl_config_open()
*/
FSL_EXPORT int fsl_config_close( fsl_cx * f );

/**
   If f has an opened configuration db then its handle is returned,
   else 0 is returned.

   For API consistency's sake, the db handle's "MAIN" name is aliased
   to fsl_db_role_name(FSL_DBROLE_CONFIG).

   @see fsl_config_open()
   @see fsl_config_close()
*/
FSL_EXPORT fsl_db * fsl_cx_db_config( fsl_cx * f );

/**
   Convenience form of fsl_db_prepare() which uses f's main db.
   Returns 0 on success. On preparation error, any db error state is
   uplifted from the db object to the fsl_cx object.  Returns
   FSL_RC_RANGE if !*sql.

   Results are undefined if f or sql are NULL or otherwise
   invalid.
*/
FSL_EXPORT int fsl_cx_prepare( fsl_cx * f, fsl_stmt * tgt,
                               char const * sql, ... );

/**
   va_list counterpart of fsl_cx_prepare().
*/
FSL_EXPORT int fsl_cx_preparev( fsl_cx * f, fsl_stmt * tgt,
                                char const * sql, va_list args );

/**
   Convenience form of fsl_db_prepare_cached() which uses f's main db.
   Returns 0 on success. On preparation error, any db error state is
   uplifted from the db object to the fsl_cx object.  Returns
   FSL_RC_RANGE if !*sql.

   Results are undefined if f or sql are NULL or otherwise
   invalid.
*/
FSL_EXPORT int fsl_cx_prepare_cached( fsl_cx * f, fsl_stmt ** tgt,
                                      char const * sql, ... );

/**
   va_list counterpart of fsl_cx_prepare_cached().
*/
FSL_EXPORT int fsl_cx_preparev_cached( fsl_cx * f, fsl_stmt ** tgt,
                                       char const * sql, va_list args );

/**
   Convenience form of fsl_db_exec() which uses f's main db handle.
   Returns 0 on success. On statement preparation or execution error,
   the db's error state is uplifted into f and that result is
   returned. Results are undefined if f or sql are NULL or otherwise
   invalid.
*/
FSL_EXPORT int fsl_cx_exec( fsl_cx * f, char const * sql, ... );

/**
   va_list counterpart of fsl_cx_exec().
*/
FSL_EXPORT int fsl_cx_execv( fsl_cx * f, char const * sql, va_list args );

/**
   The fsl_db_exec_multi() counterpart of fsl_cx_exec(). Results are
   undefined if f or sql are NULL or otherwise invalid.
*/
FSL_EXPORT int fsl_cx_exec_multi( fsl_cx * f, char const * sql, ... );

/**
   va_list counterpart of fsl_cx_exec_multi().
*/
FSL_EXPORT int fsl_cx_exec_multiv( fsl_cx * f, char const * sql,
                                   va_list args );

/**
   Wrapper around fsl_db_last_insert_id() which uses f's main
   database. Returns -1 if !f or f has no opened db.

   @see fsl_cx_db()
*/
FSL_EXPORT fsl_id_t fsl_cx_last_insert_id(fsl_cx * f);


/**
   Works similarly to fsl_stat(), except that zName must refer to a
   path under f's current checkout directory. Note that this stats
   local files, not repository-level content.

   If relativeToCwd is true then the filename is
   resolved/canonicalized based on the current working directory (see
   fsl_getcwd()), otherwise f's current checkout directory is used as
   the virtual root. This makes a subtle yet important difference in
   how the name is resolved. Applications taking input from users
   (e.g. CLI apps) will normally want to resolve from the current
   working dir (assuming the filenames were passed in from the
   CLI). In a GUI environment, where the current directory is likely
   not the checkout root, resolving based on the checkout root
   (i.e. relativeToCwd=false) is probably saner.

   Returns 0 on success. Errors include, but are not limited to:

   - FSL_RC_MISUSE if !zName.

   - FSL_RC_NOT_A_CKOUT if f has no opened checkout.

   - If fsl_is_simple_pathname(zName) returns false then
   fsl_ckout_filename_check() is used to normalize the name. If
   that fails, its failure code is returned.

   - As for fsl_stat().

   See fsl_stat() for more details regarding the tgt parameter.

   TODO: fossil-specific symlink support. Currently it does not
   distinguish between symlinks and non-links.

   @see fsl_cx_stat2()
*/
FSL_EXPORT int fsl_cx_stat( fsl_cx * f, bool relativeToCwd,
                            char const * zName, fsl_fstat * tgt );

/**
   This works identically to fsl_cx_stat(), but provides more
   information about the file being stat'd.

   If nameOut is not NULL then the resolved/normalized path to to
   that file is appended to nameOut. If fullPath is true then an
   absolute path is written to nameOut, otherwise a
   checkout-relative path is written.

   Returns 0 on success. On stat() error, nameOut is not updated,
   but after stat()'ing, allocation of memory for nameOut's buffer
   may fail.

   If zName ends with a trailing slash, that slash is retained in
   nameOut.

   This function DOES NOT resolve symlinks, stat()ing the link instead
   of what it points to.

   @see fsl_cx_stat()
*/
FSL_EXPORT int fsl_cx_stat2( fsl_cx * f, bool relativeToCwd,
                             char const * zName,
                             fsl_fstat * tgt,
                             fsl_buffer * nameOut,
                             bool fullPath);

/**
   Sets the case-sensitivity flag for f to the given value. This flag
   alters how some filename-search/comparison operations operate. This
   option is only intended to have an effect on plaforms with
   case-insensitive filesystems.

   Note that this does not save the option in the config database
   (repo-level "case-sensitive" boolean config option). It arguably
   should, and this behavior may change in the future.

   @see fsl_cx_is_case_sensitive()
*/
FSL_EXPORT void fsl_cx_case_sensitive_set(fsl_cx * f, bool caseSensitive);

/**
   Returns true if f is set for case-sensitive filename
   handling, else false. This setting is cached when a repository
   is opened, but passing true for the second argument forces the
   config option to be re-loaded from the repository db.
   Results are undefined if !f.

   @see fsl_cx_case_sensitive_set()
*/
FSL_EXPORT bool fsl_cx_is_case_sensitive(fsl_cx *  const f, bool forceRecheck);

/**
   Compares two strings, represumably filenames, using either
   fsl_strcmp() or fsl_stricmp(), depending on f's filename
   case-sensitivity setting.
*/
FSL_EXPORT int fsl_cx_filename_cmp(fsl_cx * f, char const * z1, char const *z2);

/**
   If f is set to use case-sensitive filename handling,
   returns a pointer to an empty string, otherwise a pointer
   to the string "COLLATE nocase" is returned.
   Results are undefined if f is NULL. The returned bytes
   are static.

   @see fsl_cx_case_sensitive_set()
   @see fsl_cx_is_case_sensitive()
*/
FSL_EXPORT char const * fsl_cx_filename_collation(fsl_cx const * f);

/**
   Invokes macro E with 2 arguments one time for each of the core
   fsl_satype_e entries. Argument 1 is the entry's name (identifier)
   and argument 2 is its value (integer).
*/
#define fsl_satype_e_map(E) \
  E(FSL_SATYPE_INVALID, -1/** Sentinel value used for some error
                              reporting.*/)                    \
  E(FSL_SATYPE_ANY, 0/** Sentinel value used to mark a deck as being
                         "any" type. This is a placeholder on a deck's
                         way to completion. */) \
  E(FSL_SATYPE_CHECKIN, 1/** Indicates a "manifest" artifact (a checkin
                             record). */)                               \
  E(FSL_SATYPE_CLUSTER, 2/** Indicates a "cluster" artifact. These
                             are used during synchronization. */)   \
  E(FSL_SATYPE_CONTROL, 3/** Indicates a "control" artifact (a tag
                             change).*/) \
  E(FSL_SATYPE_WIKI, 4/** Indicates a "wiki" artifact. */) \
  E(FSL_SATYPE_TICKET, 5/** Indicates a "ticket" artifact. */) \
  E(FSL_SATYPE_ATTACHMENT, 6/** Indicates an "attachment" artifact
                                (used in the ticketing subsystem). */)  \
  E(FSL_SATYPE_TECHNOTE, 7/** Indicates a technote (formerly
                                "event") artifact (kind of like a blog
                                entry). */) \
  E(FSL_SATYPE_FORUMPOST, 8/** Indicates a forum post artifact (a
                               close relative of wiki pages).*/)

/**
   An enumeration of the types of structural artifacts used by
   Fossil. The numeric values of all entries before FSL_SATYPE_count,
   with the exception of FSL_SATYPE_INVALID, are a hard-coded part of
   the Fossil db architecture and must never be changed. Any after
   FSL_SATYPE_count are libfossil extensions.
*/
enum fsl_satype_e {
#define E(N,V) N = V,
  fsl_satype_e_map(E)
#undef E
  /**
     The number of non-pseudo artifact types in this enum.  It is used
     to size arrays and must have a value of the last entry in
     fsl_satype_e_map() plus 1.
  */
  FSL_SATYPE_count,

  /**  @deprecated
       Historical (deprecated) name for FSL_SATYPE_TECHNOTE.
  */
  FSL_SATYPE_EVENT = FSL_SATYPE_TECHNOTE,

  /**
     A pseudo-type for use with fsl_sym_to_rid() which changes the
     behavior of checkin lookups to return the RID of the start of the
     branch rather than the tip, with the caveat that the results are
     unspecified if the given symbolic name refers to multiple
     branches.

     fsl_satype_event_cstr() returns the same as FSL_SATYPE_CHECKIN for
     this entry.

     This entry IS NOT VALID for most APIs which require a fsl_satype_e
     value.
  */
  FSL_SATYPE_BRANCH_START = 100
};
typedef enum fsl_satype_e fsl_satype_e;

/**
   Returns some arbitrary but distinct string for the given
   fsl_satype_e. The returned bytes are static and
   NUL-terminated. Intended primarily for debugging and informative
   purposes, not actual user output.
*/
FSL_EXPORT char const * fsl_satype_cstr(fsl_satype_e t);

/**
   For a given artifact type, it returns the key string used in the
   event.type db table. Returns NULL if passed an unknown value or
   a type which is not used in the event table, otherwise the
   returned bytes are static and NUL-terminated.

   The returned strings for a given type are as follows:

   - FSL_SATYPE_ANY returns "*"
   - FSL_SATYPE_CHECKIN and FSL_SATYPE_BRANCH_START return "ci"
   - FSL_SATYPE_WIKI returns "w"
   - FSL_SATYPE_TAG returns "g"
   - FSL_SATYPE_TICKET returns "t"
   - FSL_SATYPE_EVENT returns "e"

   The other control artifact types do not have representations
   in the event table, and NULL is returned for them.

   All of the returned values can be used in comparison clauses in
   queries on the event table's 'type' field (but use GLOB instead
   of '=' so that the "*" returned by FSL_ATYPE_ANY can match).
   For example, to get the comments from the most recent 5 commits:

   ```
   SELECT
   datetime(mtime),
   coalesce(ecomment,comment),
   user
   FROM event WHERE type='ci'
   ORDER BY mtime DESC LIMIT 5;
   ```

   Where 'ci' in the SQL is the non-NULL return value from this
   function. When escaping this value via fsl_buffer_appendf() (or
   anything functionally similar), use the %%q/%%Q format
   specifiers to escape it.
*/
FSL_EXPORT char const * fsl_satype_event_cstr(fsl_satype_e t);

/**
   A collection of bitmaskable values indicating categories
   of fossil-standard glob sets. These correspond to the following
   configurable settings:

   ignore-glob, crnl-glob, binary-glob
*/
enum fsl_glob_category_e{
/** Sentinel entry. */
FSL_GLOBS_INVALID = 0,
/** Corresponds to the ignore-glob config setting. */
FSL_GLOBS_IGNORE = 0x01,
/** Corresponds to the crnl-glob config setting. */
FSL_GLOBS_CRNL = 0x02,
/** Corresponds to the binary-glob config setting. */
FSL_GLOBS_BINARY = 0x04,
/** A superset of all config-level glob categories. */
FSL_GLOBS_ANY = 0xFF
/*
  Potential TODO: add FSL_GLOBS_CURRENT_OP for use with SQL UDFs. The
  idea would be that SCM operations which could make use of
  op-specific glob lists, e.g., checkin/add/merge, could set a custom
  glob set as the current one and then access it via their SQL using
  `fsl_glob('_', ...)` or some such.
*/
};
typedef enum fsl_glob_category_e fsl_glob_category_e;

/**
   Checks one or more of f's configurable glob lists to see if str
   matches one of them. If it finds a match, it returns a pointer to
   the matching glob (as per fsl_glob_list_matches()), the bytes
   of which are owned by f and may be invalidated via modification
   or reloading of the underlying glob list. In generally the return
   value can be used as a boolean - clients generally do not need
   to know exactly which glob matched.

   gtype specifies the glob list(s) to check in the form of a
   bitmask of fsl_glob_category_e values. Note that the order of the
   lists is unspecified, so if that is important for you then be
   sure that gtype only specifies one glob list
   (e.g. FSL_GLOBS_IGNORE) and call it again (e.g. passing
   FSL_GLOBS_BINARY) if you need to distinguish between those two
   cases.

   str must be a non-NULL, non-empty empty string.

   Returns NULL !str, !*str, gtype does not specify any known
   glob list(s), or no glob match is found.

   Performance is, abstractly speaking, horrible, because we're
   comparing arbitrarily long lists of glob patterns against an
   arbitrary string. That said, it's fast enough for our purposes.
*/
FSL_EXPORT char const * fsl_cx_glob_matches( fsl_cx * f, int gtype,
                                             char const * str );

/**
   Converts a well-known fossil glob list configuration key to
   a fsl_glob_category_e value:

   - "ignore-glob" = FSL_GLOBS_IGNORE
   - "binary-glob" = FSL_GLOBS_BINARY
   - "crnl-glob" = FSL_GLOBS_CRNL
   - Anything else = FSL_GLOBS_INVALID

   To simplify this function's use via an SQL-accessible UDF, the
   `*-glob` names may be passed in without their `-glob` suffix,
   e.g. `"ignore"` instead of `"ignore-glob"`.
*/
FSL_EXPORT fsl_glob_category_e fsl_glob_name_to_category(char const * str);

/**
   Fetches f's glob list of the given category. If forceReload is true
   then the context will check whether the list has had any content
   added to its source since it was initially loaded.

   On success, returns 0 and assigns `*tgt` to the list (noting that
   it may be empty). On error `*tgt` is not modified.

   Returns FSL_RC_RANGE if gtype is not one of FSL_GLOBS_IGNORE,
   FSL_GLOBS_CRNL, or FSL_GLOBS_BINARY. Returns FSL_RC_OOM if there is
   an allocation error during list reloading. May return lower-level
   result codes from the filesystem or db layer if loading a given
   list fails.
*/
FSL_EXPORT int fsl_cx_glob_list( fsl_cx * f,
                                 fsl_glob_category_e gtype,
                                 fsl_list ** tgt,
                                 bool forceReload );

/**
   Sets f's hash policy and returns the previous value. If f has a
   repository db open then the setting is stored there and any error
   in setting it is placed into f's error state but otherwise ignored
   for purposes of this call.

   If p is FSL_HPOLICY_AUTO *and* the current repository contains any
   SHA3-format hashes, the policy is interpreted as FSL_HPOLICY_SHA3.

   This value is a *suggestion*, and may be trumped by various
   conditions, in particular in repositories containing older (SHA1)
   hashes.
*/
FSL_EXPORT fsl_hashpolicy_e fsl_cx_hash_policy_set(fsl_cx *f, fsl_hashpolicy_e p);

/**
   Returns f's current hash policy.
*/
FSL_EXPORT fsl_hashpolicy_e fsl_cx_hash_policy_get(fsl_cx const*f);

/**
   Returns a human-friendly name for the given policy, or NULL for an
   invalid policy value. The returned strings are the same ones used
   by fossil's hash-policy command.
*/
FSL_EXPORT char const * fsl_hash_policy_name(fsl_hashpolicy_e p);

/**
   Hashes all of pIn, appending the hash to pOut. Returns 0 on succes,
   FSL_RC_OOM if allocation of space in pOut fails. The hash algorithm
   used depends on the given fossil context's current hash policy and
   the value of the 2nd argument.

   If the 2nd argument is false, the hash is performed per the first
   argument's current hash policy. If the 2nd argument is true, the
   hash policy is effectively inverted. e.g. if the context prefers
   SHA3 hashes, the alternate form will use SHA1.

   Returns FSL_RC_UNSUPPORTED, without updating f's error state, if
   the hash is not possible due to conflicting values for the policy
   and its alternate. e.g. a context with policy FSL_HPOLICY_SHA3_ONLY
   will refuse to apply an SHA1 hash. Whether or not this result can
   be ignored is context-dependent, but it normally can be. This
   result is only possible when the 2nd argument is true.

   Returns 0 on success.
*/
FSL_EXPORT int fsl_cx_hash_buffer( const fsl_cx * f, bool useAlternate,
                                   fsl_buffer const * pIn,
                                   fsl_buffer * pOut);
/**
   The file counterpart of fsl_cx_hash_buffer(), behaving exactly the
   same except that its data source is a file and it may return
   various error codes from fsl_buffer_fill_from_filename(). Note that
   the contents of the file, not its name, are hashed.
*/
FSL_EXPORT int fsl_cx_hash_filename( fsl_cx * f, bool useAlternate,
                                     const char * zFilename, fsl_buffer * pOut);

/**
   Works like fsl_getcwd() but updates f's error state on error and
   appends the current directory's name to the given buffer. Returns 0
   on success.
*/
FSL_EXPORT int fsl_cx_getcwd(fsl_cx * f, fsl_buffer * pOut);

/**
   Returns the same as passing fsl_cx_db() to
   fsl_db_txn_level(), or 0 if f has no db opened.

   @see fsl_cx_db()
*/
FSL_EXPORT int fsl_cx_txn_level(fsl_cx * f);
/**
   Returns the same as passing fsl_cx_db() to
   fsl_db_txn_begin().
*/
FSL_EXPORT int fsl_cx_txn_begin(fsl_cx * f);
/**
   Returns the same as passing fsl_cx_db() to fsl_db_txn_end().
*/
FSL_EXPORT int fsl_cx_txn_end_v2(fsl_cx * f, bool keepSavepoint,
                                 bool bubbleRollback);

#if 0
/* Legacy behavior. */
#define fsl_cx_txn_end(F, RollbackMode) \
  fsl_cx_txn_end_v2((F), !(RollbackMode), !!(RollbackMode))
#else
/* What we probably should have been doing instead but
   now breaks stuff. */
#define fsl_cx_txn_end(F, RollbackMode) \
  fsl_cx_txn_end_v2((F), !(RollbackMode), false)
#endif
/**
   Installs or (if f is NULL) uninstalls a confirmation callback for
   use by operations on f which require user confirmation. The exact
   implications of *not* installing a confirmer depend on the
   operation in question: see fsl_cx_confirm().

   The 2nd argument bitwise copied into f's internal confirmer
   object. If the 2nd argument is NULL, f's confirmer is cleared,
   which will cause fsl_cx_confirm() to use certain default responses
   (see that function for details).

   If the final argument is not NULL then the previous confirmer is
   bitwise copied to it.

   @see fsl_confirm_callback_f
   @see fsl_cx_confirm()
   @see fsl_cx_confirmer_get()
*/
FSL_EXPORT void fsl_cx_confirmer(fsl_cx * f,
                                 fsl_confirmer const * newConfirmer,
                                 fsl_confirmer * prevConfirmer);
/**
   Stores a bitwise copy of f's current confirmer object into *dest. Can
   be used to save the confirmer before temporarily swapping it out.

   @see fsl_cx_confirmer()
*/
FSL_EXPORT void fsl_cx_confirmer_get(fsl_cx const * f, fsl_confirmer * dest);

/**
   If fsl_cx_confirmer() was used to install a confirmer callback in f
   then this routine calls that confirmer and returns its result code
   and its answer via *outAnswer. If no confirmer is currently
   installed, it responds with default answers, depending on the
   eventId:

   - FSL_CEVENT_OVERWRITE_MOD_FILE: FSL_CRESPONSE_NEVER

   - FSL_CEVENT_OVERWRITE_UNMGD_FILE: FSL_CRESPONSE_NEVER

   - FSL_CEVENT_RM_MOD_UNMGD_FILE: FSL_CRESPONSE_NEVER

   - FSL_CEVENT_MULTIPLE_VERSIONS: FSL_CRESPONSE_CANCEL

   Those are not 100% set in stone and are up for reconsideration.

   If a confirmer has been installed, this function does not modify
   outAnswer->response if the installed confirmer does not. Thus
   routines should set it to some acceptable default/sentinel value
   before calling this, to account for callbacks which ignore the
   given detail->eventId.

   If a confirmer callback responds with FSL_CRESPONSE_ALWAYS or
   FSL_CRESPONSE_NEVER, the code which is requesting confirmation must
   honor that by *NOT* calling the callback again for the current
   processing step of that eventId. e.g. if a loop asks for
   confirmation of FSL_CEVENT_RM_MOD_FILE and any response is one of
   the above, that one loop must not ask for confirmation again, and
   must instead accept that response for future queries within the
   same logical library operation (e.g. one checkout-update
   cycle). This is particularly important for applications which
   interactively present the question to the user for confirmation so
   that users have a way to *not* get spammed with a confirmation
   message showing up for each and every one of an arbitrary number of
   confirmations.

   @see fsl_confirm_callback_f
   @see fsl_cx_confirmer()
*/
FSL_EXPORT int fsl_cx_confirm(fsl_cx * f, fsl_confirm_detail const * detail,
                              fsl_confirm_response *outAnswer);

/**
   Sets f's is-interrupted flag and, if the 3rd argument is not NULL,
   its error state. The is-interrupted flag is separate from f's
   normal error state and is _not_ cleared by fsl_cx_err_reset(). To
   clear the interrupted flag, call this function with values of 0 and
   NULL for the 2nd and 3rd arguments, respectively. This flag is
   _not_ fetched by fsl_cx_err_get() but:

   1) If this function is passed a non-NULL 3rd argument, then the
   normal error state, as well as the is-interrupted flag, is updated
   and can be fetched normally via fsl_cx_err_get(). However...

   2) It is possible for any error message provided via this routine
   to be overwritten or reset by another routine before the
   interrupted flag can be acted upon, whereas the interrupted flag
   itself can only be modified by this routine.

   Returns its 2nd argument on success or FSL_RC_OOM if given a
   formatted string and allocation of it fails. In either case, the
   interrupted flag, as returned by fsl_cx_interrupted(), is _always_
   assigned to the passed-in code.

   If passed a code of 0, the is-interrupted flag is reset but the
   general error state is not modified.

   Results are undefined if this function is called twice concurrently
   with the same fsl_cx object. i.e. all calls for a given fsl_cx must
   come from a single thread. Results are also undefined if it is
   called while f is in its finalization phase (typically during
   application shutdown).

   ACHTUNG: this is new as of 2021-11-18 and is not yet widely honored
   within the API.

   Library maintenance notes:

   - Long-running actions which honor this flag should, if it is set,
   clear it before returning its error code. Also, they should prefer
   to pass on non-interruption errors if one has been set set, in
   addition to clearing the interruption flag. Only routines which
   honor this flag, or top-most routines in the application, should
   ever clear this flag.

   @see fsl_cx_interrupted()
   @see fsl_cx_interruptv()
*/
FSL_EXPORT int fsl_cx_interrupt(fsl_cx * f, int code,
                                const char * fmt, ...);

/**
   The va_list counterpart of fsl_cx_interrupt().
*/
FSL_EXPORT int fsl_cx_interruptv(fsl_cx * f, int code, char const * fmt, va_list args);

/**
   If f's is-interrupted flag is set, this function returns its
   value. Note that there is inherently a race condition when calling
   fsl_cx_interrupt() (to set the flag) from another thread (e.g. a
   UI thread while showing a progress indicator).
*/
FSL_EXPORT int fsl_cx_interrupted(fsl_cx const * f);

/**
   Returns true if f has the "allow-symlinks" repo-level configuration
   option set to a truthy value, else returns false. That setting is
   cached to avoid performing a db lookup on each call, but passing
   true for the second argument causes the repository to be
   re-checked.
*/
FSL_EXPORT bool fsl_cx_allows_symlinks(fsl_cx * f, bool forceRecheck);

/**
   Closes any opened repository and/or checkout database(s) opened by
   f, freeing any associated resources and clearing out f's current
   user name. Returns 0 on success or if no dbs are opened (noting
   that this does NOT close the separate global configuration db: see
   fsl_config_close()). Returns FSL_RC_MISUSE if the opened SCM db(s)
   have an opened transaction, but that behaviour may change in the
   future to force a rollback and close the database(s).
*/
FSL_EXPORT int fsl_close_scm_dbs(fsl_cx * f);

/**
   Shorthand for calling both fsl_close_scm_dbs() and, if it succeeds,
   fsl_config_close(). Returns the result of the former if it fails,
   else returns the result of the latter.
 */
FSL_EXPORT int fsl_close_dbs(fsl_cx * f);

/**
   Extracts the "last-sync-url" entry from f's repository
   configuration and appends it to tgt. On success, returns 0, else
   returns the result of fsl_config_get_buffer().
 */
FSL_EXPORT int fsl_cx_last_sync_url(fsl_cx * f, fsl_buffer *tgt);

/**
   Flags for use with fsl_buildinfo().
*/
enum fsl_buildinfo_e {
  FSL_BUILDINFO_VERSION,
  FSL_BUILDINFO_VERSION_HASH,
  FSL_BUILDINFO_VERSION_TIMESTAMP,
  FSL_BUILDINFO_CONFIG_TIMESTAMP
};

/**
   Fetches the piece of library-level build information specified by
   its argument. If no such info is available, NULL is returned.
*/
FSL_EXPORT char const * fsl_buildinfo(enum fsl_buildinfo_e what);

/**
   If both where and where->callback are not NULL, msg is passed to
   where->callback and its result is returned, else this is a no-op
   and 0 is returned.

   Results are undefined if msg is NULL or either argument is
   improperly populated.
*/
FSL_EXPORT int fsl_msg_emit(fsl_msg const * msg,
                            fsl_msg_listener const * where);

/**
   If f has a fsl_msg listener, a fsl_msg composed from the given type
   and payload is passed to the the listener and its result is
   returned, else this is a no-op and 0 is returned.
*/
FSL_EXPORT int fsl_cx_emit(fsl_cx * const f, fsl_msg_e type, void const * p);

/**
   va_list counterpart of fsl_cx_emit() which emits a
   fsl_appendf()-formatted message as a string or a buffer, depending
   on the message type.

   It may (and does) fatally fail if passed a message type for which
   (type & FSL_MSG_mask_type) is not one of FSL_MSG_type_string or
   FSL_MSG_type_buffer.
*/
int fsl_cx_emitfv(fsl_cx * f, fsl_msg_e type, char const * zFmt,
                  va_list args);

/** Elipsis counterpart of fsl_cx_emitfv(). */
int fsl_cx_emitf(fsl_cx *f, fsl_msg_e type, char const * zFmt, ...);

/**
   Replace's f's current fsl_msg listener with a bitwise copy of
   pNew. If pOld is not NULL then a bitwise copy of the previous
   listener is written there.
*/
void fsl_cx_listener_replace(fsl_cx * f, fsl_msg_listener const *pNew,
                             fsl_msg_listener * pOld);

#if 0
/**
   DO NOT USE - not yet tested and ready.

   Returns the result of either localtime(clock) or gmtime(clock),
   depending on f:

   - If f is NULL, returns localtime(clock).

   - If f has had its FSL_CX_F_LOCALTIME_GMT flag set (see
   fsl_cx_flag_set()) then returns gmtime(clock), else
   localtime(clock).

   If clock is NULL, NULL is returned.

   Note that fsl_cx instances default to using UTC for everything,
   which is the opposite of fossil(1).
*/
FSL_EXPORT struct tm * fsl_cx_localtime( fsl_cx const * f, const time_t * clock );

/**
   Equivalent to fsl_cx_localtime(NULL, clock).
*/
FSL_EXPORT struct tm * fsl_localtime( const time_t * clock );

/**
   DO NOT USE - not yet tested and ready.

   This function passes (f, clock) to fsl_cx_localtime(),
   then returns the result of mktime(3) on it. So...
   it adjusts a UTC Unix timestamp to either the same UTC
   local timestamp or to the local time.
*/
FSL_EXPORT time_t fsl_cx_time_adj(fsl_cx const * f, time_t clock);
#endif

#if defined(__cplusplus)
} /*extern "C"*/
#endif
#endif /* ORG_FOSSIL_SCM_FSL_CORE_H_INCLUDED */
