/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
#if !defined(ORG_FOSSIL_SCM_FSL_INTERNAL_H_INCLUDED)
#define ORG_FOSSIL_SCM_FSL_INTERNAL_H_INCLUDED
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).

  *****************************************************************************
  This file declares library-level internal APIs which are shared
  across the library.
*/

#include "fossil-scm/repo.h" /* a fossil header MUST come first b/c of config macros */
#include "fossil-scm/sync.h"

/**
   Determine whether fsl__mutex_enter() and friends are no-ops.  If
   we'd require C11 then we could use C11's mutexes instead of
   sqlite3's, but we have those handy so we might as well use them.
*/
#if !defined(FSL_ENABLE_MUTEX)
#  if defined(SQLITE_THREADSAFE) && SQLITE_THREADSAFE>0
#    define FSL_ENABLE_MUTEX 1
#  else
#    define FSL_ENABLE_MUTEX 0
#  endif
#endif

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct fsl__bccache fsl__bccache;
typedef struct fsl__bccache_line fsl__bccache_line;
typedef struct fsl__pq fsl__pq;
typedef struct fsl__pq_entry fsl__pq_entry;

/** @internal

    Queue entry type for the fsl__pq class.

    Potential TODO: we don't currently use the (data) member. We can
    probably remove it.
*/
struct fsl__pq_entry {
  /** RID of the entry. */
  fsl_id_t id;
  /** Raw data associated with this entry. */
  void * data;
  /** Priority of this element. */
  double priority;
};
/** @internal
    Empty-initialized fsl__pq_entry structure.
*/
#define fsl__pq_entry_empty_m {0,NULL,0.0}

/** @internal

    A simple priority queue class. Instances _must_ be initialized
    by copying fsl__pq_empty or fsl__pq_empty_m (depending on where
    the instance lives).
*/
struct fsl__pq {
  /** Number of items allocated in this->list. */
  uint16_t capacity;
  /** Number of items used in this->list. */
  uint16_t used;
  /** The queue. It is kept sorted by entry->priority. */
  fsl__pq_entry * list;
};

/** @internal
    Empty-initialized fsl__pq struct, intended for const-copy initialization.
*/
#define fsl__pq_empty_m {0,0,NULL}

/** @internal
    Empty-initialized fsl__pq struct, intended for copy initialization.
*/
extern  const fsl__pq fsl__pq_empty;

/** @internal

    Clears the contents of p, freeing any memory it owns, but not
    freeing p. Results are undefined if !p.
*/
void fsl__pq_clear(fsl__pq * p);

/** @internal

    Insert element e into the queue. Returns 0 on success, FSL_RC_OOM
    on error. Results are undefined if !p. pData may be NULL.
*/
int fsl__pq_insert(fsl__pq *p, fsl_id_t e,
                  double v, void *pData);

/** @internal

    Extracts (removes) the first element from the queue (the element
    with the smallest value) and return its ID.  Return 0 if the queue
    is empty. If pp is not NULL then *pp is (on success) assigned to
    opaquedata pointer mapped to the entry.
*/
fsl_id_t fsl__pq_extract(fsl__pq *p, void **pp);

/** @internal

    Holds one "line" of a fsl__bccache cache.
*/
struct fsl__bccache_line {
  /**
     RID of the cached record.
  */
  fsl_id_t rid;
  /**
     Age. Newer is larger.
  */
  fsl_uint_t age;
  /**
     Content of the artifact.
  */
  fsl_buffer content;
};
/** @internal

    Empty-initialized fsl__bccache_line structure.
*/
#define fsl__bccache_line_empty_m { 0,0,fsl_buffer_empty_m }


/** @internal

    A cache for tracking the existence of blobs while the internal
    goings-on of fsl_content_get() and friends are going on.

    "bc" ==> blob cache.

    Historically fossil caches artifacts as their blob content, but
    libfossil will likely (at some point) to instead cache fsl_deck
    instances, which contain all of the same data in pre-parsed form.
    It cost more memory, though. That approach also precludes caching
    non-structural artifacts (i.e. opaque client blobs).

    Potential TODO: the limits of the cache size are currently
    hard-coded and changing them "by hand" won't have much effect.
    We should possibly have an API to tweak these limits.
*/
struct fsl__bccache {
  /**
     Total amount of buffer memory (in bytes) used by cached content.
     This does not account for memory held by this->list.
  */
  unsigned szTotal;
  /**
     Limit on the (approx.) amount of memory (in bytes) which can be
     taken up by the cached buffers at one time. Fossil's historical
     value is 50M.
  */
  unsigned szLimit;
  /**
     Number of entries "used" in this->list.
  */
  uint16_t used;
  /**
     Approximate upper limit on the number of entries in this->list.
     This limit may be violated slightly.

     This list gets searched linearly so this number should ideally be
     relatively small: 3 digits or less. Fossil's historical value is
     500.
  */
  uint16_t usedLimit;
  /**
     Number of allocated slots in this->list.
  */
  uint16_t capacity;
  /**
     Next cache counter age. Higher is newer.
  */
  fsl_uint_t nextAge;

  /**
     Gets incremented on each insert for purposes of invalidating
     this cache. See the notes for fsl__ptl::changeMarker. It's okay
     if this overflows.
  */
  unsigned changeMarker;

  /**
     List of cached content, ordered by age.
  */
  fsl__bccache_line * list;
  /**
     RIDs of all artifacts currently in the this->list
     cache.
  */
  fsl_id_bag inCache;

  /**
     Metrics solely for internal use in looking for
     optimizations. These are only updated by fsl_content_get().
  */
  struct {
    unsigned hits;
    unsigned misses;
  } metrics;
};
/** @internal

    Empty-initialized fsl__bccache structure, intended
    for const-copy initialization.
*/
#define fsl__bccache_empty_m {     \
  .szTotal = 0U,                   \
  .szLimit = 20000000U/*Historical fossil value=50M*/, \
  .used = 0U, .usedLimit = 400U/*Historical fossil value=500*/,\
  .capacity = 0,                   \
  .nextAge = 0,                    \
  .changeMarker = 0,               \
  .list = NULL,                    \
  .inCache = fsl_id_bag_empty_m,   \
  .metrics = {.hits = 0U, .misses = 0U}  \
}

/** @internal

   Very internal.

   "Manifest cache" for fsl_deck entries encountered during
   crosslinking. This type is intended only to be embedded in fsl_cx.

   The routines for managing this cache are static in deck.c:
   fsl__cx_mcache_insert() and fsl__cx_mcache_search().

   The array members in this struct MUST have the same length
   or results are undefined.
*/
struct fsl__mcache {
  /** Next age value. No clue how the cache will react once this
      overflows. */
  fsl_uint_t nextAge;
  /** Counts the number of cache hits. */
  unsigned hits;
  /** Counts the number of cache misses. */
  unsigned misses;
  /** The virtual age of each deck in the cache. They get evicted
      oldest first. */
  fsl_uint_t aAge[4];
  /**
     Stores bitwise copies of decks. Storing a fsl_deck_malloc() deck
     into the cache requires bitwise-copying is contents, wiping out
     its contents via assignment from fsl_deck_empty, then
     fsl_free()'ing it (as opposed to fsl_deck_finalize(), which would
     attempt to clean up memory which now belongs to the cache's
     copy).

     Array sizes of 6 and 10 do not appreciably change the hit rate
     compared to 4, at least not for current (2021-11-18) uses.
  */
  fsl_deck decks[4];
};

/** @internal
  Convenience typedef. */
typedef struct fsl__mcache fsl__mcache;

/** @internal
    Initialized-with-defaults fsl__mcache structure, intended for
    const-copy initialization. */
#define fsl__mcache_empty_m {                    \
    .nextAge=0, .hits=0, .misses=0,              \
    .aAge = {0,0,0,0},                           \
    .decks = {fsl_deck_empty_m,fsl_deck_empty_m, \
              fsl_deck_empty_m, fsl_deck_empty_m}\
}

/**@internal
  Initialized-with-defaults fsl__mcache structure, intended for
  non-const copy initialization. */
extern const fsl__mcache fsl__mcache_empty;

/**
   This will fail to compile if the arrays fsl__mcache::aAge and
   fsl__mcache::decks have different sizes. They must have the same
   size.
*/
typedef int fsl__mcache_StaticAssertArraySizes[
 ((sizeof(fsl__mcache_empty.aAge)
  /sizeof(fsl__mcache_empty.aAge[0]))
 == (sizeof(fsl__mcache_empty.decks)
     /sizeof(fsl__mcache_empty.decks[0])))
 ? 1 : -1
];

/**
   Expands to a list of the names of all fsl_id_bag members of the
   fsl__ptl_line struct by invoking macro E(NAME) once for each name.
*/
#define fsl__ptl_line_map(E)                        \
  E(toVerify/*RIDs of blobs added at this level*/)  \
  E(leafCheck/*Used during the processing of manifests to keep track
               of "leaf checks" which need to be done downstream.*/) \
  E(available/*RIDs of known-available content*/)   \
  E(missing/*RIDs of known-missing content*/) \
  E(dephantomize/*An experiment in queueing up dephantomization work*/)

/**
   A cache of fsl_cx state which applies per transaction level.  We
   need one of these per level in the transaction stack.
*/
struct fsl__ptl_line {
#define E(X) fsl_id_bag X;
  fsl__ptl_line_map(E)
#undef E
  /**
     A poor-man's approach to recognizing when to clear
     fsl_cx::cache::blobContent when a fsl__ptl stack level is popped.
     The problem is that fsl__bccache is global to a given fsl_cx
     instance but may hold RIDs which are invalidated when a
     transaction level pops. fsl__bccache is not structured such that
     removing individual entries by RID is easy or cheap, so we
     invalidate the whole cache if a given transaction level added
     anything to fsl_cx::cache::blobContent. When pushing a PTL level,
     we record fsl__bccache::changeMarker here and compare it when the
     transaction pops. If they differ, the blob content cache is
     cleared. An improvement to this would be to extend fsl__bccache
     to be able to remove entries by RID, but that's not on today's
     proverbial menu.
  */
  unsigned changeMarker;
};

typedef struct fsl__ptl_line fsl__ptl_line;

/**
   Enum entries corresponding to fsl__ptl_line members.
*/
enum fsl__ptl_e {
  /** Sentinel value needed in slot 0. */
  fsl__ptl_e_dummy = 0,
#define E(X) fsl__ptl_e_ ## X,
  fsl__ptl_line_map(E)
#undef E
};
typedef enum fsl__ptl_e fsl__ptl_e;

/**
   This object manages a stack of fsl__ptl_line objects for a
   fsl_cx.
*/
struct fsl__ptl {
  /** Persistent error code. */
  int rc;
  /** Number of levels currently allocated. */
  uint16_t allocated;
  /** 0-based level. 0 is for out-of-transaction stuff. */
  uint16_t level;
  /**
     The transaction level in which the first RIDs (in this
     transaction stack) were added. This is used for determining when
     to run certain checks. A negative value means no RIDs have been
     added.
   */
  int validateRidLevel;
  /** Memory living in this->mem. */
  fsl__ptl_line * stack;
  /** Current line in this->stack. */
  fsl__ptl_line * line;
  /** Memory for this->stack. */
  fsl_buffer mem;
};

typedef struct fsl__ptl fsl__ptl;

/** Empty-initialized fsl__ptl struct. */
#define fsl__ptl_empty_m { \
    .rc = 0, .allocated = 0, .level = 0, \
    .validateRidLevel = -1, \
    .stack = NULL, .line = NULL, \
    .mem = fsl_buffer_empty_m           \
  }

/*
  The fsl_cx class is documented in main public header. ALL of its
  members are to be considered private/internal.

  Reminder to self:

  i'd like to rename this to fsl__cx and have a public (typedef struct
  fsl__cx fsl_cx) like we do for fsl__xfer, but doing so breaks fnc,
  which uses (struct fsl_cx) in a few places and triggers an "is not a
  struct" error.
*/
struct fsl_cx {
  /**
     A pointer to the "main" db handle. Exactly which db IS the
     main db is, because we have three DBs, not generally knowble.

     The internal management of fsl_cx's db handles has changed
     a couple of times. As of 2022-01-01 the following applies:

     dbMain starts out NULL. When a repo or checkout is opened, dbMain
     is pointed at the first of those which is opened.  When its
     partner is opened, it is ATTACHed to dbMain.  dbMain->role holds
     a bitmask of fsl_dbrole_e values reflecting which roles are
     opened/attached to it.

     The db-specific separate handles (this->{repo,ckout}.db) are used
     to store the name and file path to each db. ONE of those will
     have a db->dbh value of non-NULL, and that one is the instance
     which dbMain points to.

     Whichever db is opened first gets its "main" schema aliased to
     the corresponding fsl_db_role_name() for is role so that SQL code
     need not care which db is "main" and which is "repo" or
     "ckout". (Sidebar: when this library was started, the ability to
     alias a db with another name did not exist, and thus we required
     a middle-man "main" db to which we ATTACHed the repo and checkout
     dbs.)

     As of 20211230, f->config.db is its own handle, not ATTACHed with
     the others. Its db gets aliased to
     fsl_db_role_name(FSL_DBROLE_CONFIG).

     Internal code should rely as little as possible on the actual
     arrangement of internal DB handles, and should use
     fsl_cx_db_repo(), fsl_cx_db_ckout(), and fsl_cx_db_config() to
     get a handle to the specific db they want. Whether or not they
     return the same handle or 3 different ones may change at some
     point, so the public API treats them as separate entities. That
     is especially important for the global config db, as that one
     is (for locking reason) almost certain to remain in its own
     db handle, independent of the repo/checkout dbs.

     In any case, the internals very much rely on the repo and
     checkout dbs being accessible via a single db handle because the
     checkout-related SQL requires both dbs for most queries. The
     internals are less picky about the genuine layout of those
     handles (e.g. which one, if either, is the MAIN db).
  */
  fsl_db * dbMain;

  /**
     Marker which tells us whether fsl_cx_finalize() needs
     to fsl_free() this instance or not.
  */
  void const * allocStamp;

  /**
     A bitwise copy of the config object passed to fsl_cx_init() (or
     some default).
  */
  fsl_cx_config cxConfig;

  struct {
    /**
       Holds info directly related to a checkout database.
    */
    struct {
      /**
         Holds the filename of the current checkout db and possibly
         related state.
      */
      fsl_db db;
      /**
         The directory part of an opened checkout db. This is currently
         only set by fsl_ckout_open_dir(). It contains a trailing slash,
         largely because that simplifies porting fossil(1) code and
         appending filenames to this directory name to create absolute
         paths (a frequently-needed option).

         Useful for doing absolute-to-relative path conversions for
         checking file lists.
      */
      char * dir;
      /**
         Optimization: fsl_strlen() of dir. Guaranteed to be set to
         dir's length if dir is not NULL, else it will be 0.
      */
      fsl_size_t dirLen;
      /**
         The rid of the current checkout. May be 0 for an empty
         repo/checkout. Must be negative if not yet known.
      */
      fsl_id_t rid;
      /**
         The UUID of the current checkout. Only set if this->rid is
         positive. Owned by the containing fsl_cx object.
      */
      fsl_uuid_str uuid;
      /**
         Julian mtime of the checkout version, as reported by the
         [event] table.
      */
      double mtime;
    } ckout;

    /**
       Holds info directly related to a repo database.
    */
    struct {
      /**
         Holds the filename of the current repo db and possibly related
         state.
      */
      fsl_db db;
      /**
         The default user name, for operations which need one.
         See fsl_cx_user_set().
      */
      char * user;

      /** The 'main-branch' repo config setting, defaulting to trunk. */
      char * mainBranch;
    } repo;

    /**
       Holds info directly related to a global config database.
    */
    struct {
      /**
         Holds the filename of the current global config db and possibly
         related state. This handle is managed separately from the
         repo/ckout handles because this db is shared by all fossil
         instances are we have to ensure that we don't lock it for
         longer than necessary, thus this db may get opened and closed
         multiple times within even within a short-lived application.
      */
      fsl_db db;
    } gconfig;

    int peakTxnLevel;
    fsl_size_t nBegin;
    fsl_size_t nCommit;
    fsl_size_t nRollback;
  } db;

  /**
     State for incrementally preparing a checkin operation.
  */
  struct {
    /**
       Holds a list of "selected files" in the form
       of vfile.id values.
    */
    fsl_id_bag selectedIds;

    /**
       The deck used for incrementally building certain parts of a
       checkin.
    */
    fsl_deck mf;
  } ckin;

  /**
     Confirmation callback. Used by routines which may have to
     interactively ask a user to confirm things.
  */
  fsl_confirmer confirmer;

  /**
     Output channel used by fsl_output() and friends.

     This was added primarily so that fossil client apps can share a
     single output channel which the user can swap out, e.g. to direct
     all output to a UI widget or a file.

     Though the library has adamantly avoided adding a "warning"
     output channel, features like:

     https://fossil-scm.org/home/info/52a389d3dbd4b6cc

     arguably call for one.
  */
  fsl_outputer output;

  /**
     Can be used to tie client-specific data to the context. Its
     finalizer is called when fsl_cx_finalize() cleans up. The library
     does not use this state. It is intended primarily for tying,
     e.g., scripting-engine information to the context, e.g. mapping a
     scripting engine context to this one for later use in fossil-side
     callbacks.
  */
  fsl_state clientState;

  /**
     Holds error state. As a general rule, this information is updated
     only by routines which need to return more info than a simple
     integer error code. e.g. this is often used to hold
     db-driver-provided error state. It is not used by "simple"
     routines for which an integer code always suffices. APIs which
     set this should denote it with a comment like "updates the
     context's error state on error."
  */
  fsl_error error;

  /**
     Reuseable scratchpads for low-level file canonicalization
     buffering and whatnot. Not intended for huge content: use
     this->fileContent for that. This list should stay relatively
     short, as should the buffers (a few kb each, at most).

     @see fsl__cx_scratchpad()
     @see fsl__cx_scratchpad_yield()
  */
  struct {
    /**
       Strictly-internal temporary buffers we intend to reuse many
       times, mostly for filename canonicalization, holding hash
       values, and small encoding/decoding tasks. These must never be
       used for values which will be long-lived, nor are they intended
       to be used for large content, e.g. reading files, with the
       possible exception of holding versioned config settings, as
       those are typically rather small. They must also not be held
       while recursion back into a function holding one of these is
       possible.

       If needed, the lengths of this->buf[] and this->used[] may be
       extended, but anything beyond 8, maybe 10, seems a bit extreme.
       They should only be increased if we find code paths which
       require it. As of this writing (2021-03-17), the peak
       concurrently used was 5. In any case fsl__cx_scratchpad() fails
       fatally if it needs more than it has, so we won't/can't fail to
       overlook such a case.
    */
    fsl_buffer buf[8];
    /**
       Flags telling us which of this->buf is currenly in use.
    */
    bool used[8];
    /**
       A cursor _hint_ to try to speed up fsl__cx_scratchpad() by about
       half a nanosecond, making it O(1) instead of O(small N) for the
       common case.
    */
    short next;
    /**
       A scratchpad specific to fsl__cx_emit() and friends.
    */
    fsl_buffer emit;
    /**
       A scratchpad specific to fsl_cx_format() and friends. This one
       has to account for potential recursion.
    */
    fsl_buffer format;
  } scratchpads;

  /**
     Flags, some (or one) of which is runtime-configurable by the
     client (see fsl_cx_flags_e). We can get rid of this and add the
     flags to the cache member along with the rest of them.
  */
  fsl_flag32_t flags;

  /**
     Error flag which is intended to be set via signal handlers or a
     UI thread to tell this context to cancel any currently
     long-running operation. Not all operations honor this check, but
     the ones which are prone to "exceedingly long" operation (at
     least a few seconds) do.
  */
  volatile int interrupted;

  /**
     List of callbacks for deck crosslinking purposes.
  */
  fsl_xlinker_list xlinkers;

  /**
     A place for caching generic things.
  */
  struct {
    /**
       If true, skip "dephantomization" of phantom blobs.  This is a
       detail from fossil(1) with as-yet-undetermined utility. It's
       apparently only used during the remote-sync process, which this
       API does not (as of 2021-10) yet have.
    */
    bool ignoreDephantomizations;

    /**
       Whether or not a running commit process should be marked as
       private. This member is used for communicating this flag
       through multiple levels of API.
    */
    bool markPrivate;

    /**
       True if fsl__crosslink_begin() has been called but
       fsl__crosslink_end() is still pending.
    */
    bool isCrosslinking;

    /**
       Flag indicating that only cluster control artifacts should be
       processed by manifest crosslinking. This will only be relevant
       if/when the sync protocol is implemented.
    */
    bool xlinkClustersOnly;

    /**
       Is used to tell the content-save internals that a "final
       verification" (a.k.a. verify-before-commit) is underway.
    */
    bool inFinalVerify;

    /**
       True when a sync is active, to prevent recursive attempts to
       sync.
    */
    bool isSyncing;

    /**
       Specifies whether SOME repository-level file-name
       comparisons/searches will work case-insensitively. <0 means
       not-yet-determined, 0 = no, >0 = yes.
    */
    short caseInsensitive;

    /**
       Cached copy of the allow-symlinks config option, because it is
       (hypothetically) needed on many stat() call. Negative
       value=="not yet determined", 0==no, positive==yes. The negative
       value means we need to check the repo config resp. the global
       config to see if this is on.

       As of late 2020, fossil(1) is much more restrictive with
       symlinks due to vulnerabilities which were discovered by a
       security researcher, and we definitely must not default any
       symlink-related features to enabled/on. As of Feb. 2021, my
       personal preference, and very likely plan of attack, is to only
       treat SCM'd symlinks as if symlinks support is disabled. It's
       very unlikely that i will implement "real" symlink support but
       would, *solely* for compatibility with fossil(1), be convinced
       to permit such changes if someone else wants to implement them.
       Patches are joyfully considered!
    */
    short allowSymlinks;

    /**
       Indicates whether or not this repo has ever seen a delta
       manifest. If none has ever been seen then the repository will
       prefer to use baseline (non-delta) manifests. Once a delta is
       seen in the repository, the checkin algorithm is free to choose
       deltas later on unless it's otherwise prohibited, e.g. by the
       `forbid-delta-manifests` config db setting.

       This article provides an overview to the topic delta manifests
       and essentially debunks their ostensible benefits:

       https://fossil-scm.org/home/doc/tip/www/delta-manifests.md

       Values: negative==undetermined, 0==no, positive==yes. This is
       updated when a repository is first opened and when new content
       is written to it.
    */
    short seenDeltaManifest;

    /**
       Records whether this repository has an FTS search
       index. <0=undetermined, 0=no, >0=yes.
    */
    short searchIndexExists;

    /**
       Cache for the `manifest` config setting, as used by
       fsl_ckout_manifest_setting(), with the caveat that
       if the setting changes after it is cached, we won't necessarily
       see that here!
    */
    short manifestSetting;

    /**
       Record ID of rcvfrom entry during commits. This is likely to
       remain unused in libf until/unless the sync protocol is
       implemented.

       2025-07-21: this will probably need to be manipulated by
       fsl__xfer. rcvId is used in fsl__content_put_ex() but it's
       historically (in this library, not fossil(1)) always been 0.
       Setting the rcvId requires high-level info that fossil(1) uses,
       and it's only really relevant for the sync layer.
    */
    fsl_id_t rcvId;

    /**
       A place for temporarily holding file content. We use this in
       places where we have to loop over files and read their entire
       contents, so that we can reuse this buffer's memory if
       possible.  The loop and the reading might be happening in
       different functions, though, and some care must be taken to
       avoid use in two functions concurrently.

       All access to this must be acquired by fsl__cx_content_buffer()
       and released by fsl__cx_content_buffer_yield().
    */
    fsl_buffer fileContent;

    /**
       Reusable buffer for creating and fetching deltas via
       fsl_content_get() and fsl__content_deltify(). The number of
       allocations this actually saves is pretty small.
    */
    fsl_buffer deltaContent;

    /**
       fsl_content_get() cache.
    */
    fsl__bccache blobContent;

    /**
       Per-transaction-level cache.
    */
    fsl__ptl ptl;

    /**
       An internal cache for fsl__content_mark_availble().  This is
       only used in that routine but it's called frequently during
       checkins and sync.
    */
    fsl_id_bag markAvailableCache;

    /**
       Infrastructure for fsl_mtime_of_manifest_file(). It
       remembers the previous RID so that it knows when it has to
       invalidate/rebuild its ancestry cache.
    */
    fsl_id_t mtimeManifest;
    /**
       The "project-code" config option. This is primarily for use
       with the sync protocol. It is managed by
       fsl_repo_project_code().
    */
    char * projectCode;

    /**
       Internal optimization to avoid duplicate fsl_stat() calls
       across two functions in some cases.
    */
    fsl_fstat fstat;

    /**
       Parsed-deck cache.
    */
    fsl__mcache mcache;

    /**
       Holds various glob lists. That said... these features are
       actually app-level stuff which the library itself does not
       resp. should not enforce. We can keep track of these for users
       but the library internals _generally_ have no business using
       them.

       _THAT_ said... these have enough generic utility that we can
       justify storing them and _optionally_ applying them. See
       fsl_checkin_opt for an example of where we do this.
    */
    struct {
      /**
         Holds the "ignore-glob" globs.
      */
      fsl_list ignore;
      /**
         Holds the "binary-glob" globs.
      */
      fsl_list binary;
      /**
         Holds the "crnl-glob" globs.
      */
      fsl_list crnl;
    } globs;

    /**
       Very-frequently-used SQL statements.
    */
    struct {
/*
  This macro greatly simplifies maintenance of the caches fsl_stmt
  objects elsewhere, believe it or not. The 1st argument to E is the
  name of the fsl_cx::cache::stmt member. The 2nd argument is the SQL
  of that statement.

  Potential TODO: add a fsl_dbrole_e value to each so that we
  centrally know which db to check for (for error-reporting purposes)
  before preparation.
*/
#define fsl__cx_cache_stmt_map(E)                 \
      E(deltaR2S,"SELECT srcid FROM delta WHERE rid=?1")  \
      E(deltaS2R,"SELECT rid FROM delta WHERE srcid=?1")  \
      E(uuidToRid,\
        "SELECT rid FROM blob WHERE uuid=?1")     \
      E(ridToUuid,\
        "SELECT uuid FROM blob WHERE rid=?1") \
      E(ridIsLeaf,\
        "SELECT 1 FROM plink "                             \
        "WHERE pid=?1 "                                    \
        "AND coalesce("                                    \
        "(SELECT value FROM tagxref "                      \
        "WHERE tagid=8/*FSL_TAGID_BRANCH*/ AND rid=?1), "  \
        "?2 /*main branch name*/)"                         \
        "=coalesce((SELECT value FROM tagxref "            \
        "WHERE tagid=8/*FSL_TAGID_BRANCH*/ "               \
        "AND rid=plink.cid), "                             \
        "?2 /*main branch name*/)")                        \
      E(ridIsPrivate,"SELECT 1 FROM private WHERE rid=?1") \
      E(contentSize,"SELECT size FROM blob WHERE rid=?1")  \
      E(contentBlob,"SELECT content, size FROM blob WHERE rid=?1") \
      E(uuidIsShunned,"SELECT 1 FROM shun WHERE uuid=?1")  \
      E(contentNew,\
        "INSERT INTO blob(rcvid,size,uuid,content) " \
        "VALUES(0,-1,?1,NULL) "                      \
        "RETURNING rid")                             \
      E(insertPhantomRid,"INSERT OR IGNORE INTO unclustered VALUES(?1)") \
      E(insertPrivateRid,"INSERT OR IGNORE INTO private VALUES(?1)") \
      E(insertUnclusteredRid,"INSERT OR IGNORE INTO unclustered VALUES(?1)") \
      E(insertUnsentRid,"INSERT OR IGNORE INTO unsent VALUES(?1)") \
      E(replaceDelta,\
        "REPLACE INTO delta(rid,srcid) VALUES(?1,?2)")  \
      E(contentUndeltify,\
        "UPDATE blob SET content=?1, size=?2 WHERE rid=?3")     \
      E(blobRidSize,"SELECT rid, size FROM blob WHERE uuid=?1") \
      E(blobUpdateContent,\
        "UPDATE blob SET content=?1 WHERE rid=?2")  \
      E(phantomPopulate,\
        "UPDATE blob SET rcvid=?1, size=?2, " \
        "content=?3 WHERE rid=?4")            \
      E(blobInsertFull,                               \
        "INSERT INTO blob (rcvid,size,uuid,content) " \
        "VALUES(?1,?2,?3,?4) RETURNING rid")          \
      E(leafInsert,                                \
        "INSERT OR IGNORE INTO leaf VALUES (?1)")  \
      E(leafDelete,                                \
        "DELETE FROM leaf WHERE rid=?1")           \
      E(parentsOf,                                     \
        "SELECT pid FROM plink WHERE cid=?1 AND pid>0") \
      /*
         TODO. Something akin to:

         WITH RECURSIVE
         dchain(rid,srcid,level) AS (
           SELECT ?1 rid, NULL srcid, 0 level
           UNION ALL
           SELECT delta.rid, delta.srcid, dchain.level+1
           FROM delta JOIN dchain ON delta.srcid=dchain.rid
           ORDER BY 2
         )
         SELECT rid, srcid FROM dchain WHERE level>0

       i.e.: for RID ?1, get a list of all deltas which
       depend on this. (Is it really all?).

       Remember that we can have multiple paths to a parent,
       as in this simulated delta table:

       drop table if exists dmap;
       create temp table dmap(rid INTEGER NOT NULL, srcid INTEGER NOT NULL);
       insert into dmap values(2,4),(4,6),(6,8),(8,10);
       insert into dmap values(1,11),(11,10);
       insert into dmap values(20,30),(30,0);

       For a ?1 of 10 that will emit:

       rid|srcid
       8|10
       6|8
       4|6
       2|4
       11|10 -- note that we re-visit the root here
       1|11
      */ \
      E(deltaChainForRid,"")

#define STMT(MEMBER,IGNORED) fsl_stmt MEMBER;
      fsl__cx_cache_stmt_map(STMT)
#undef STMT

    } stmt;

    /**
       Holds a list of temp-dir names. Must be allocated using
       fsl_temp_dirs_get() and freed using fsl_temp_dirs_free().
    */
    char **tempDirs;
  } cache;

  /**
     Ticket-related information.
  */
  struct {
    /**
       Holds a list of (fsl_card_J*) records representing custom
       ticket table fields available in the db.

       Each entry's flags member denotes (using fsl_card_J_flags)
       whether that field is used by the ticket or ticketchng
       tables.

       TODO, eventually: add a separate type for these entries.  We
       use fsl_card_J because the infrastructure is there and they
       provide what we need, but fsl_card_J::flags only exists for
       this list. A custom type would be smaller than fsl_card_J
       (only two members) but adding it requires adding some
       infrastructure which isn't worth the effort at the moment.
    */
    fsl_list customFields;

    /**
       Gets set to true (at some point) if the client has the
       ticket db table.
    */
    bool hasTicket;

    /**
       Gets set to true (at some point) if the client has the
       ticket.tkt_ctime db field.
    */
    bool hasCTime;

    /**
       Gets set to true (at some point) if the client has the
       ticketchng db table.
    */
    bool hasChng;

    /**
       Gets set to true (at some point) if the client has the
       ticketchng.rid db field.
    */
    bool hasChngRid;

    /**
       The name of the ticket-table field which refers to a ticket's
       title. Default = "title". The bytes are owned by this object.
    */
    char * titleColumn;

    /**
       The name of the ticket-table field which refers to a ticket's
       status. Default = "status". The bytes are owned by this object.
    */
    char * statusColumn;
  } ticket;

  struct {
    struct {
      /**
         Number of times the internal content cache was used.
      */
      fsl_size_t nCached;
      /**
         Total of the number of "used" bytes of the content cache
         each time it was relinquished.
      */
      fsl_size_t nTotalUsed;

      /**
         The maximum buffer capacity the content buffer had at
         relenquish-time.
      */
      fsl_size_t nPeakBufSize;

      /**
         The number of times the content cache buffer was trimmed
         below the built-in maximum size.
      */
      fsl_size_t nCappedMaxSize;
    } content;
  } metrics;
};

/** @internal
    Initialized-with-defaults fsl_cx instance.
*/
extern const fsl_cx fsl__cx_empty;

/**
   Ensure that fsl_cx::scratchpads::buf/used have the same array size.
*/
typedef int fsl__cx_StaticAssertScratchpadSizes[
 ((sizeof(fsl__cx_empty.scratchpads.buf)
  /sizeof(fsl__cx_empty.scratchpads.buf[0]))
 == (sizeof(fsl__cx_empty.scratchpads.used)
     /sizeof(fsl__cx_empty.scratchpads.used[0])))
 ? 1 : -1
];

/*
  TODO:

  int fsl_buffer_append_getenv( fsl_buffer * b, char const * env )

  Fetches the given env var and appends it to b. Returns FSL_RC_NOT_FOUND
  if the env var is not set. The primary use for this would be to simplify
  the Windows implementation of fsl_find_home_dir().
*/


/** @internal

    Expires the single oldest entry in c. Returns true if it removes
    an item, else false.
*/
bool fsl__bccache_expire_oldest(fsl__bccache * c);

/** @internal

    Add an entry to the content cache.

    This routines transfers the contents of pBlob over to c,
    regardless of success or failure.  The cache will deallocate the
    memory when it has finished with it.

    If the cache cannot add the entry due to cache-internal
    constraints, as opposed to allocation errors, it clears the buffer
    (for consistency's sake) and returns 0.

    Returns 0 on success, FSL_RC_OOM on allocation error. Has undefined
    behaviour if !c, rid is not semantically valid, !pBlob. An empty
    blob is normally semantically illegal but is not strictly illegal
    for this cache's purposes.
*/
int fsl__bccache_insert(fsl__bccache * c, fsl_id_t rid,
                       fsl_buffer * pBlob);

/** @internal

    Frees all memory held by c, and clears out c's state, but does
    not free c. Results are undefined if !c.
*/
void fsl__bccache_clear(fsl__bccache * c);

/** @internal

    Resets all bags associated with the given cache and frees all
    cached buffer memory, but keeps any fsl_id_bag memory intact for
    re-use. This does not reset the hit/miss metrics.
*/
void fsl__bccache_reuse(fsl__bccache * c);


/** @internal

    Checks f->cache.blobContent to see if rid is available in the
    repository opened by f.

    Returns 0 if the content for the given rid is available in the
    repo or the cache. Returns FSL_RC_NOT_FOUND if it is not in the
    repo nor the cache. Returns some other non-0 code for "real
    errors," e.g. FSL_RC_OOM if a cache allocation fails. This
    operation may update the cache's contents.

    If this function detects a loop in artifact lineage, it fails an
    assert() in debug builds and returns FSL_RC_CONSISTENCY in
    non-debug builds. That doesn't happen in real life, though.

    Reminder to self: it should arguably return FSL_RC_PHANTOM if it's
    a phantom but the next call for that same rid would pull from
    f->cache.blobContent.missing and lose the distinction between
    "wasn't found" and "is a phantom", so would have to return
    FSL_RC_NOT_FOUND. If we ever need another fsl_id_bag for tracking
    phantoms, we can adjust this to return FSL_RC_PHANTOM by checking
    that bag for the RID.
*/
int fsl__bccache_check_available(fsl_cx * f, fsl_id_t rid);

/** @internal

    This is THE ONLY ROUTINE IN THE LIBRARY which is permitted to add
    content to the blob table.  Only one other routine
    (fsl__phantom_new()) adds new blob entries, but that one does not
    add blobs with content.

    This writes the given buffer content into the repository
    database's blob tabe. It Returns the record ID via outRid (if it
    is not NULL).  If the content is already in the database (as
    determined by a lookup of its hash against blob.uuid), this
    routine fetches the RID (via *outRid) but has no side effects in
    the repo.

    If srcId is >0 then pBlob must contain delta content from
    the srcId record. srcId might be a phantom.

    pBlob is normally uncompressed text, but if uncompSize>0 then the
    pBlob value is assumed to be compressed (via fsl_buffer_compress()
    or equivalent) and uncompSize is its uncompressed size. If
    uncompSize>0 then zUuid must be valid and refer to the hash of the
    _uncompressed_ data (which is why this routine does not calculate
    it for the client).

    Sidebar 2025-08-04: it is legal for the uncompressed size to be 0
    when the content is compressed (and larger). That case can't be
    represented with the above semantics. Do we actually handle the
    empty-file case properly?

    Sidebar: we "could" use fsl_buffer_is_compressed() and friends
    to determine if pBlob is compressed and get its decompressed
    size, then remove the uncompSize parameter, but that would
    require that this function decompress the content to calculate
    the hash. Since the caller likely just compressed it, that seems
    like a huge waste.

    zUuid is the UUID of the artifact, if it is not NULL.  When
    srcId is specified then zUuid must always be specified.  If
    srcId is zero, and zUuid is zero then the correct zUuid is
    computed from pBlob.  If zUuid is not NULL then this function
    asserts (in debug builds) that fsl_is_uuid() returns true for
    zUuid.

    If isPrivate is true or if f->cache.markPrivate is true, the blob
    is created as a private record.

    If the record already exists but is a phantom, the pBlob content
    is inserted and the phatom becomes a real record.

    The original content of pBlob is not disturbed.  The caller continues
    to be responsible for pBlob.  This routine does *not* take over
    responsibility for freeing pBlob.

    If outRid is not NULL then on success *outRid is assigned to the
    RID of the underlying blob record.

    Returns 0 on success and there are too many potential error cases
    to name - this function is a massive beast.

    On success, the new blob's RID is queued up for pre-commit content
    validation. If this is the first call to this function in the
    current transaction stack, the RID is queued in the COMMIT handler
    for the _parent_ transaction. Subsequent calls will cause the RID
    to be queued in that same transaction level. We cannot validate in
    _this_ function because the validation would trigger at the end of
    the transaction level which _this_ function installs. Content
    validation has to be delayed until we believe we have all content
    which will be injected in the current transaction stack.

    @see fsl__content_put()
*/
int fsl__content_put_ex( fsl_cx * f,
                        fsl_buffer const * pBlob,
                        fsl_uuid_cstr zUuid, fsl_id_t srcId,
                        fsl_size_t uncompSize, bool isPrivate,
                        fsl_id_t * outRid);
/** @internal

    Equivalent to fsl__content_put_ex(f,pBlob,NULL,0,0,false,newRid).

    This must only be used for saving raw (non-delta) content.

    @see fsl__content_put_ex()
*/
int fsl__content_put( fsl_cx * f,
                     fsl_buffer const * pBlob,
                     fsl_id_t * newRid);


/** @internal

    If the given blob ID refers to deltified repo content, this routine
    undeltifies it and replaces its content with its expanded
    form.

    Returns 0 on success, FSL_RC_NOT_A_REPO if f has no opened
    repository, FSL_RC_RANGE if rid is not positive, and any number of
    other potential errors during the db and content operations. This
    function treats already unexpanded content as success.

    @see fsl__content_deltify()
*/
int fsl__content_undeltify(fsl_cx * f, fsl_id_t rid);


/** @internal

    The converse of fsl__content_undeltify(), this replaces the storage
    of the given blob record so that it is a delta of srcid.

    If rid is already a delta from some other place then no conversion
    occurs and this is a no-op unless force is true, which case the
    content is undeltified and re-delta'd against srcid.

    If rid's contents are not available because the the rid is a
    phantom or depends to one, no delta is generated and 0 is
    returned.

    It never generates a delta that carries a private artifact into
    a public artifact. Otherwise, when we go to send the public
    artifact on a sync operation, the other end of the sync will
    never be able to receive the source of the delta.  It is OK to
    delta private->private, public->private, and public->public.
    Just no private->public delta. For such cases this function
    returns 0, as opposed to FSL_RC_ACCESS or some similar code, and
    leaves the content untouched.

    If srcid is a delta that depends on rid, then srcid is
    converted to undelta'd text.

    If either rid or srcid contain less than some "small,
    unspecified number" of bytes (currently 50), or if the resulting
    delta does not achieve a compression of at least 25%, the rid is
    left untouched.

    Returns 0 if a delta is successfully made or none needs to be
    made, non-0 on error.

    Requires that fsl_cx_txn_level() be greater than 0.

    @see fsl__content_undeltify()
*/
int fsl__content_deltify(fsl_cx * f, fsl_id_t rid,
                        fsl_id_t srcid, bool force);


/** @internal

    Creates a new phantom blob with the given UUID and return its
    artifact ID via *newId. Returns 0 on success, any of many codes on
    error.  If isPrivate is true _or_ f has been flagged as being in
    "private mode" then the new content is flagged as private. newId
    may be NULL, but if it is then the caller will have to find the
    record id himself by using the UUID (see fsl_uuid_to_rid()).
*/
int fsl__phantom_new( fsl_cx * f, fsl_uuid_cstr uuid,
                      bool isPrivate, fsl_id_t * newId );

/** @internal

    Schedules a leaf check for "rid" and its parents. Returns 0 on
    success.
*/
int fsl__leaf_eventually_check( fsl_cx * f, fsl_id_t rid);

/** @internal

    Perform all pending leaf checks. Returns 0 on success or if it
    has nothing to do.
*/
int fsl__leaf_do_pending_checks(fsl_cx * f);

/** @internal

    Inserts a tag into f's repo db. It does not create the related
    control artifact - use fsl_tag_an_rid() for that.

    rid is the artifact to which the tag is being applied.

    srcId is the artifact that contains the tag. It is often, but
    not always, the same as rid. This is often the RID of the
    manifest containing tags added as part of the commit, in which
    case rid==srcId. A Control Artifact which tags a different
    artifact will have rid!=srcId.

    mtime is the Julian timestamp for the tag. Defaults to the
    current time if mtime <= 0.0.

    If outRid is not NULL then on success *outRid is assigned the
    record ID of the generated tag (the tag.tagid db field).

    If a more recent (compared to mtime) entry already exists for
    this tag/rid combination then its tag.tagid is returned via
    *outRid (if outRid is not NULL) and no new entry is created.

    Returns 0 on success, and has a huge number of potential error
    codes.
*/
int fsl__tag_insert( fsl_cx * f,
                     fsl_tagtype_e tagtype,
                     char const * zTag,
                     char const * zValue,
                     fsl_id_t srcId,
                     double mtime,
                     fsl_id_t rid,
                     fsl_id_t *outRid );
/** @internal

    Propagate all propagatable tags in artifact pid to the children of
    pid. Returns 0 on... non-error. Returns FSL_RC_RANGE if pid<=0.
*/
int fsl__tag_propagate_all(fsl_cx * f, fsl_id_t pid);

/** @internal

    Propagates a tag through the various internal pipelines.

    pid is the artifact id to whose children the tag should be
    propagated.

    tagid is the id of the tag to propagate (the tag.tagid db value).

    tagType is the type of tag to propagate. Must be either
    FSL_TAGTYPE_CANCEL or FSL_TAGTYPE_PROPAGATING. Note that
    FSL_TAGTYPE_ADD is not permitted.  The tag-handling internals
    (other than this function) translate ADD to CANCEL for propagation
    purposes. A CANCEL tag is used to stop propagation. (That's a
    historical behaviour inherited from fossil(1).) A potential TODO
    is for this function to simply treat ADD as CANCEL, without
    requiring that the caller be sure to never pass an ADD tag.

    origId is the artifact id of the origin tag if tagType ==
    FSL_TAGTYPE_PROPAGATING, otherwise it is ignored.

    zValue is the optional value for the tag. May be NULL.

    mtime is the Julian timestamp for the tag. Must be a valid time
    (no defaults here).

    This function is unforgiving of invalid values/ranges, and may assert
    in debug mode if passed invalid ids (values<=0), a NULL f, or if f has
    no opened repo.
*/
int fsl__tag_propagate(fsl_cx * f,
                      fsl_tagtype_e tagType,
                      fsl_id_t pid,
                      fsl_id_t tagid,
                      fsl_id_t origId,
                      const char *zValue,
                      double mtime );

/** @internal

    Generates an fsl_appendf()-formatted message to stderr and
    fatally aborts the application by calling exit(). This is only
    (ONLY!) intended for us as a placeholder for certain test cases
    and is neither thread-safe nor reantrant.

    fmt may be empty or NULL, in which case only the code and its
    fsl_rc_cstr() representation are output.

    This function does not return.
*/
void fsl__fatal( int code, char const * fmt, ... )
#ifdef __GNUC__
  __attribute__ ((noreturn))
#endif
  ;

/** @internal

    Translate a normalized, repo-relative filename into a
    filename-id (fnid). Create a new fnid if none previously exists
    and createNew is true. On success returns 0 and sets *rv to the
    filename.fnid record value. If createNew is false and no match
    is found, 0 is returned but *rv will be set to 0. Returns non-0
    on error.  Results are undefined if any parameter is NULL.


    In debug builds, this function asserts that no pointer arguments
    are NULL and that f has an opened repository.
*/
int fsl__repo_filename_fnid2( fsl_cx * f, char const * filename,
                             fsl_id_t * rv, bool createNew );


/** @internal

    Clears and frees all (char*) members of db but leaves the rest
    intact. If alsoBuffers is true then the error state and prep
    buffer are also freed, else they are kept as well.
*/
void fsl__db_clear_strings(fsl_db * db, bool alsoBuffers );

/** @internal

    Returns 0 if db appears to have a current repository schema, 1
    if it appears to have an out of date schema, and -1 if it
    appears to not be a repository. Results are undefined if db is
    NULL or not opened.
*/
int fsl__db_repo_verify_schema(fsl_db * db);


/** @internal

    Flags for APIs which add phantom blobs to the repository.  The
    values in this enum derive from fossil(1) code and should not be
    changed without careful forethought and (afterwards) testing.  A
    phantom blob is a blob about whose existence we know but for which
    we have no content. This normally happens during sync or rebuild
    operations, but can also happen when artifacts are stored directly
    as files in a repo (like this project's repository does, storing
    artifacts from *other* projects for testing purposes).
*/
enum fsl__phantom_e {
/**
   Indicates to fsl__uuid_to_rid2() that no phantom artifact
   should be created.
*/
FSL_PHANTOM_NONE = 0,
/**
   Indicates to fsl__uuid_to_rid2() that a public phantom
   artifact should be created if no artifact is found.
*/
FSL_PHANTOM_PUBLIC = 1,
/**
   Indicates to fsl__uuid_to_rid2() that a private phantom
   artifact should be created if no artifact is found.
*/
FSL_PHANTOM_PRIVATE = 2
};
typedef enum fsl__phantom_e fsl__phantom_e;

/** @internal

    Works like fsl_uuid_to_rid(), with these differences:

    - uuid is required to be a complete UUID, not a prefix.

    - If it finds no entry and the mode argument specifies so then it
      will add either a public or private phantom entry and return its
      new rid. If mode is FSL_PHANTOM_NONE then this this behaves just
      like fsl_uuid_to_rid().

    Returns a positive value on success, 0 if it finds no entry and
    mode==FSL_PHANTOM_NONE, and1 a negative value on error (e.g. if
    fsl_is_uuid(uuid) returns false). Errors which happen after
    argument validation will "most likely" update f's error state
    with details.
*/
fsl_id_t fsl__uuid_to_rid2( fsl_cx * f, fsl_uuid_cstr uuid,
                           fsl__phantom_e mode );

/** @internal

    Schedules the given rid to be verified before it is commited, but
    whether the validation will be done at the commit of the current
    transaction level or a higher-up one is as-yet unspecified . This
    is used by routines which add artifact records to the blob table.

    The only error case, assuming the arguments are valid, is an
    allocation error while appending rid to the internal to-verify
    queue.

    @see fsl__repo_verify_at_commit()
    @see fsl_repo_verify_cancel()
*/
int fsl__repo_verify_before_commit( fsl_cx * f, fsl_id_t rid );

/** @internal

    Clears the current transaction level's verify-at-commit list of
    RIDs.

    @see fsl__repo_verify_at_commit()
    @see fsl__repo_verify_before_commit()
*/
void fsl__repo_verify_cancel( fsl_cx * f );

/** @internal

    Processes all pending verify-at-commit entries for the current
    transaction level and clears the to-verify list. Returns 0 on
    success. On error f's error state will likely be updated.

    ONLY call this from fsl_db_txn_end() or its delegate (if
    refactored).

    Verification calls fsl_content_get() to "unpack" content added in
    the current transaction. If fetching the content (which applies
    any deltas it may need to) fails or its hash does not match its
    blob.uuid value then this routine fails and returns non-0.

    @see fsl_repo_verify_cancel()
    @see fsl__repo_verify_before_commit()
*/
int fsl__repo_verify_at_commit( fsl_cx * f );

/** @internal

    Removes all entries from the repo's blob table which are listed
    in the shun table. Returns 0 on success. This operation is
    wrapped in a transaction. Delta content which depend on
    to-be-shunned content are replaced with their undeltad forms.

    Returns 0 on success. On error, its transaction is aborted or a
    pseudo-nested transaction is set into a rollback state.
*/
int fsl__repo_shun_artifacts(fsl_cx * f);

/** @internal.

    Return a pointer to a string that contains the RHS of an SQL IN
    operator which will select config.name values that are part of
    the configuration that matches iMatch (a bitmask of
    fsl_configset_e values). Ownership of the returned string is
    passed to the caller, who must eventually pass it to
    fsl_free(). Returns NULL on allocation error.

    Reminder to self: this is part of the infrastructure for copying
    config state from an existing repo when creating new repo.
*/
char *fsl__config_inop_rhs(int iMask);

/** @internal

    Return a pointer to a string that contains the RHS of an IN
    operator that will select config.name values that are in the
    list of control settings. Ownership of the returned string is
    passed to the caller, who must eventually pass it to
    fsl_free(). Returns NULL on allocation error.

    Reminder to self: this is part of the infrastructure for copying
    config state from an existing repo when creating new repo.
*/
char *fsl__db_setting_inop_rhs(void);

/** @internal

    Creates the ticket and ticketchng tables in f's repository db,
    DROPPING them if they already exist. The schema comes from
    fsl_schema_ticket().

    TODO? Add a flag specifying whether to drop or keep existing
    copies.

    Returns 0 on success.
*/
int fsl__cx_ticket_create_table(fsl_cx * f);

/** @internal

    Frees all J-card entries in the given list.

    li is assumed to be empty or contain (fsl_card_J*)
    instances. If alsoListMem is true then any memory owned
    by li is also freed. li itself is not freed.

    Results are undefined if li is NULL.
*/
void fsl__card_J_list_free( fsl_list * li, bool alsoListMem );

/** @internal

    Values for fsl_card_J::flags.
*/
enum fsl_card_J_flags {
/**
   Sentinel value.
*/
FSL_CARD_J_INVALID = 0,
/**
   Indicates that the field is used by the ticket table.
*/
FSL_CARD_J_TICKET = 0x01,
/**
   Indicates that the field is used by the ticketchng table.
*/
FSL_CARD_J_CHNG = 0x02,
/**
   Indicates that the field is used by both the ticket and
   ticketchng tables.
*/
FSL_CARD_J_BOTH = FSL_CARD_J_TICKET | FSL_CARD_J_CHNG
};

/** @internal

    Loads all custom/customizable ticket fields from f's repo's
    ticket table info f. If f has already loaded the list and
    forceReload is false, this is a no-op.

    Returns 0 on success.

    @see fsl_cx::ticket::customFields
*/
int fsl__cx_ticket_load_fields(fsl_cx * f, bool forceReload);

/** @internal

    A comparison routine for qsort(3) which compares fsl_card_J
    instances in a lexical manner based on their names. The order is
    important for card ordering in generated manifests.

    This routine expects to get passed (fsl_card_J**) (namely from
    fsl_list entries), and will not work on an array of J-cards.
*/
int fsl__qsort_cmp_J_cards( void const * lhs, void const * rhs );

/** @internal

    This function updates the repo and/or global config databases
    with links between the dbs intended for various fossil-level
    bookkeeping and housecleaning. These links are not essential to
    fossil's functionality but assist in certain "global"
    operations.

    If no checkout is opened but a repo is, the global config (if
    opened) is updated to know about the opened repo db.

    If a checkout is opened, global config (if opened) and the
    repo are updated to point to the checked-out db.
*/
int fsl__repo_record_filename(fsl_cx * f);

/** @internal

    Updates f->ckout.uuid and f->ckout.rid to reflect the current
    checkout state. If no checkout is opened, the uuid is freed/NULLed
    and the rid is set to 0. Returns 0 on success. If it returns an
    error (OOM or db-related), the f->ckout state is left in a
    potentially inconsistent state, and it should not be relied upon
    until/unless the error is resolved.

    This is done when a checkout db is opened, when performing a
    checkin, and otherwise as needed, and so calling it from other
    code is normally not necessary.

    @see fsl__ckout_version_write()
*/
int fsl__ckout_version_fetch( fsl_cx * f );

/** @internal

    Updates f->ckout's state to reflect the given version info and
    writes the 'checkout' and 'checkout-hash' properties to the
    currently-opened checkout db. Returns 0 on success,
    FSL_RC_NOT_A_CKOUT if no checkout is opened (may assert() in that
    case!), or some other code if writing to the db fails.

    If vid is 0 then the version info is null'd out. Else if uuid is
    NULL then fsl_rid_to_uuid() is used to fetch the UUID for vid.

    If the RID differs from f->ckout.rid then f->ckout's version state
    is updated to the new values.

    This routine also updates or removes the checkout's manifest
    files, as per fsl_ckout_manifest_write(). If vid is 0 then it
    removes any such files which themselves are not part of the
    current checkout.

    @see fsl__ckout_version_fetch()
    @see fsl_cx_ckout_version_set()
*/
int fsl__ckout_version_write( fsl_cx * f, fsl_id_t vid,
                             fsl_uuid_cstr uuid );

/**
   @internal

   Exports the file with the given [vfile].[id] to the checkout,
   overwriting (if possible) anything which gets in its way. If
   the file is determined to have not been modified, it is
   unchanged.

   If the final argument is not NULL then it is set to 0 if the file
   was not modified, 1 if only its permissions were modified, and 2 if
   its contents were updated (which also requires resetting its
   permissions to match their repo-side state).

   Returns 0 on success, any number of potential non-0 codes on
   error, including, but not limited to:

   - FSL_RC_NOT_A_CKOUT - no opened checkout.
   - FSL_RC_NOT_FOUND - no matching vfile entry.
   - FSL_RC_OOM - we cannot escape this eventuality.

   Trivia:

   - fossil(1)'s vfile_to_disk() is how it exports a whole vfile, or a
   single vfile entry, to disk. e.g. it performs a checkout that way,
   whereas we currently perform a checkout using the "repo extraction"
   API. The checkout mechanism was probably the first major core
   fossil feature which was structured radically differently in
   libfossil, compared to the feature's fossil counterpart, when it
   was ported over.

   - This routine always writes to the vfile.pathname entry, as
   opposed to vfile.origname.

   Maintenance reminders: internally this code supports handling
   multiple files at once, but (A) that's not part of the interface
   and may change and (B) the 3rd parameter makes little sense in that
   case unless maybe we change it to a callback, which seems like
   overkill for our use cases.

   BUGS:

   - When doing a fsl_ckout_revert(), this function does not recognize
     a file which has been modified (only) via a merge as modified, so
     does not set `*wasWritten`. Whether that bug is in _this_
     function or fsl__is_locally_modified() is not yet clear.
     fsl_ckout_revert() works around that so that the client can be
     notified of reverted files, but that's only hiding the underlying
     discrepancy.
*/
int fsl__vfile_to_ckout(fsl_cx * f, fsl_id_t vfileId,
                       int * wasWritten);

/** @internal

    On Windows platforms (only), if fsl_isalpha(*zFile)
    and ':' == zFile[1] then this returns zFile+2,
    otherwise it returns zFile.
*/
char * fsl__file_without_drive_letter(char * zFile);

/** @internal

    This is identical to the public-API member fsl_deck_F_search(),
    except that it returns a non-const F-card.

    Locate a file named zName in d->F.list.  Return a pointer to the
    appropriate fsl_card_F object. Return NULL if not found.

    If d->f is set (as it is when loading decks via
    fsl_deck_load_rid() and friends), this routine works even if p is
    a delta-manifest. The pointer returned might be to the baseline
    and d->B.baseline is loaded on demand if needed.

    If the returned card's uuid member is NULL, it means that the file
    was removed in the checkin represented by d.

    If !d, zName is NULL or empty, or FSL_SATYPE_CHECKIN!=d->type, it
    asserts in debug builds and returns NULL in non-debug builds.

    We assume that filenames are in sorted order and use a binary
    search. As an optimization, to support the most common use case,
    searches through a deck update d->F.cursor to the last position a
    search was found. Because searches are normally done in lexical
    order (because of architectural reasons), this is normally an O(1)
    operation. It degrades to O(N) if out-of-lexical-order searches
    are performed.
*/
fsl_card_F * fsl__deck_F_seek(fsl_deck * d, const char *zName);

/** @internal

    Ensures that f's single file content buffer is available for use
    and returns it to the caller. If it appears to already be in use,
    this function fails fatally via fsl__fatal(), indicating a serious
    misuse of the internal API.

    Calling this obligates the caller to call
    fsl__cx_content_buffer_yield() as soon as they are done with the
    buffer.
*/
fsl_buffer * fsl__cx_content_buffer(fsl_cx * f);

/** @internal

    Part of the fsl_cx::cache::fileContent optimization. Passes
    f->cache.fileContent to fsl_buffer_reuse() and if its capacity is
    over a certain (unspecified, unconfigurable) size then it is
    trimmed to that size. It also updates some of f's metrics.
*/
void fsl__cx_content_buffer_yield(fsl_cx * f);

/**
   Inserts the given rid into the repo.private table.
*/
int fsl__private_rid_add(fsl_cx * f, fsl_id_t rid);

/**
   Deletes the given rid from the repo.private table.
*/
int fsl__private_rid_delete(fsl_cx * f, fsl_id_t rid);

/**
   Inserts the given rid into the repo.unclustered table.
*/
int fsl__unclustered_rid_add(fsl_cx * f, fsl_id_t rid);
/**
   Inserts the given rid into the repo.phantom table.
*/
int fsl__phantom_rid_add(fsl_cx * f, fsl_id_t rid);
/**
   Inserts the given rid into the repo.unsent table.
*/
int fsl__unsent_rid_add(fsl_cx * f, fsl_id_t rid);
/**
   Replaces the repo.delta entry with the given rid. This does not
   manipulate the related blobs in any way.
*/
int fsl__delta_replace(fsl_cx * f, fsl_id_t rid, fsl_id_t srcid);
/**
   Replaces the repo.delta entry with the given rid. This does not
   manipulate the related blobs in any way.
*/
int fsl__delta_delete(fsl_cx * f, fsl_id_t rid);

/**
   Expects zTable to name a table with an rid field. That record
   is deleted.
*/
int fsl__rid_delete(fsl_cx * f, fsl_dbrole_e dbRole,
                    char const *zTable, fsl_id_t rid);

/** @internal

   Currently disabled (always returns 0) pending resolution of a
   "wha???" result from one of the underlying queries.

   Queues up the given artifact for a search index update. This is
   only intended to be called from crosslinking steps and similar
   content updates. Returns 0 on success.

   The final argument is intended only for wiki titles (the L-card of
   a wiki post).

   If the repository database has no search index or the given content
   is marked as private, this function returns 0 and makes no changes
   to the db.
*/
int fsl__search_doc_touch(fsl_cx * f, fsl_satype_e saType,
                         fsl_id_t rid, const char * docName);

/** @internal

   Returns true if the given file name is a reserved filename
   (case-insensitive) on Windows platforms, else returns false.

   zPath must be a canonical path with forward-slash directory
   separators. nameLen is the length of zPath. If negative, fsl_strlen()
   is used to determine its length.
*/
bool fsl__is_reserved_fn_windows(const char *zPath, fsl_int_t nameLen);

/** @internal

   Clears any pending merge state from the f's checkout db's vmerge
   table. Returns 0 on success, non-0 on db error.

   If fullWipe is true, it clears all vfile contents uncondtionally,
   else it clears only entries for which the corresponding vfile
   entries are marked as unchanged and then cleans up remaining merge
   state if no file-level merge changes are pending.
*/
int fsl__ckout_clear_merge_state( fsl_cx * f, bool fullWipe );

/** @internal

   Installs or reinstalls the checkout database schema into f's open
   checkout db. Returns 0 on success, FSL_RC_NOT_A_CKOUT if f has
   no opened checkout, or an code if a lower-level operation fails.

   If dropIfExists is true then all affected tables are dropped
   beforehand if they exist. "It's the only way to be sure."

   If dropIfExists is false and the schema appears to already exists
   (without actually validating its validity), 0 is returned.
*/
int fsl__ckout_install_schema(fsl_cx * f, bool dropIfExists);

/** @internal

   Attempts to remove empty directories from under a checkout,
   starting with tgtDir and working upwards until it either cannot
   remove one or it reaches the top of the checkout dir.

   The second argument must be the canonicalized absolute path to some
   directory under the checkout root. The contents of the buffer may,
   for efficiency's sake, be modified by this routine as it traverses
   the directory tree. It will never grow the buffer but may mutate
   its memory's contents.

   Returns the number of directories it is able to remove.

   Results are undefined if tgtDir is not an absolute path rooted in
   f's current checkout.

   There are any number of valid reasons removal of a directory might
   fail, and this routine stops at the first one which does.
*/
unsigned int fsl__ckout_rm_empty_dirs(fsl_cx * f,
                                      fsl_buffer const * tgtDir);

/** @internal

   This is intended to be passed the name of a file which was just
   deleted and "might" have left behind an empty directory. The name
   _must_ an absolute path based in f's current checkout. This routine
   uses fsl_file_dirpart() to strip path components from the string
   and remove directories until either removing one fails or the top
   of the checkout is reached. Since removal of a directory can fail for
   any given reason, this routine ignores such errors. It returns 0 on
   success, FSL_RC_OOM if allocation of the working buffer for the
   filename hackery fails, and FSL_RC_MISUSE if zFilename is not
   rooted in the checkout (in which case it may assert(), so don't do
   that).

   @see fsl_is_rooted_in_ckout()
   @see fsl_rm_empty_dirs()
*/
int fsl__ckout_rm_empty_dirs_for_file(fsl_cx * f, char const *zAbsPath);

/** @internal

    If f->cache.seenDeltaManifest<=0 then this routine sets it to 1
    and sets the 'seen-delta-manifest' repository config setting to 1,
    else this has no side effects. Returns 0 on success, non-0 if
    there is an error while writing to the repository config.
*/
int fsl__cx_update_seen_delta_deck(fsl_cx * f);

/** @internal

   Very, VERY internal.

   Returns the next available buffer from f->scratchpads. Fatally
   aborts if there are no free buffers because "that should not
   happen."  Calling this obligates the caller to eventually pass
   its result to fsl__cx_scratchpad_yield().

   This function guarantees the returned buffer's 'used' member will be
   set to 0.

   Maintenance note: the number of buffers is hard-coded in the
   fsl_cx::scratchpads anonymous struct.
*/
fsl_buffer * fsl__cx_scratchpad(fsl_cx * f);

/** @internal

   Very, VERY internal.

   "Yields" a buffer which was returned from fsl__cx_scratchpad(),
   making it available for re-use. The caller must treat the buffer as
   if this routine frees it: using the buffer after having passed it
   to this function will internally be flagged as explicit misuse and
   will lead to a fatal crash the next time that buffer is fetched via
   fsl__cx_scratchpad(). So don't do that.
*/
void fsl__cx_scratchpad_yield(fsl_cx * f, fsl_buffer * b);

/** @internal

   Run automatically by fsl_deck_save(), so it needn't normally be run
   aside from that, at least not from average client code.

   Runs postprocessing on the Structural Artifact represented by
   d. d->f must be set, d->rid must be set and valid and d's contents
   must accurately represent the stored manifest for the given
   rid. This is normally run just after the insertion of a new
   manifest, but is sometimes also run after reading a deck from the
   database (in order to rebuild all db relations and add/update the
   timeline entry).

   Returns 0 on succes, FSL_RC_MISUSE !d->f, FSL_RC_RANGE if
   d->rid<=0, FSL_RC_MISUSE (with more error info in f) if d does not
   contain all required cards for its d->type value. It may return
   various other codes from the many routines it delegates work to.

   Crosslinking of ticket artifacts is currently (2021-11) missing.

   Design note: d "really should" be const here but some internals
   (d->F.cursor and delayed baseline loading) prohibit it.

   @see fsl__deck_crosslink_one()
*/
int fsl__deck_crosslink( fsl_deck /* const */ * d );

/** @internal

   Run automatically by fsl_deck_save(), so it needn't normally be run
   aside from that, at least not from average client code.

   This is a convience form of crosslinking which must only be used
   when a single deck (and only a single deck) is to be crosslinked.
   This function wraps the crosslinking in fsl_crosslink_begin()
   and fsl__crosslink_end(), but otherwise behaves the same as
   fsl__deck_crosslink(). If crosslinking fails, any in-progress
   transaction will be flagged as failed.

   Returns 0 on success.
*/
int fsl__deck_crosslink_one( fsl_deck * d );

/** @internal

   Checks whether the given filename is "safe" for writing to within
   f's current checkout.

   zFilename must be in canonical form: only '/' directory separators.
   If zFilename is not absolute, it is assumed to be relative to the top
   of the current checkout, else it must point to a file under the current
   checkout.

   Checks made on the filename include:

   - It must refer to a file under the current checkout.

   - Ensure that each directory listed in the file's path is actually
   a directory, and fail if any part other than the final one is a
   non-directory.

   If the name refers to something not (yet) in the filesystem, that
   is not considered an error.

   Returns 0 on success. On error f's error state is updated with
   information about the problem.
*/
int fsl__ckout_safe_file_check(fsl_cx * f, char const * zFilename);

/** @internal
   UNTESTED!

   Creates a file named zLinkFile and populates its contents with a
   single line: zTgtFile. This behaviour corresponds to how fossil
   manages SCM'd symlink entries on Windows and on other platforms
   when the 'allow-symlinks' repo-level config setting is disabled.
   (In late 2020 fossil defaulted that setting to disabled and made it
   non-versionable.)

   zLinkFile may be an absolute path rooted at f's current checkout or
   may be a checkout-relative path.

   Returns 0 on success, non-0 on error:

   - FSL_RC_NOT_A_CKOUT if f has no opened checkout.

   - FSL_RC_MISUSE if zLinkFile refers to a path outside of the
   current checkout.

   Potential TODO (maybe!): handle symlinks as described above or
   "properly" on systems which natively support them iff f's
   'allow-symlinks' repo-level config setting is true. That said: the
   addition of symlinks support into fossil was, IMHO, a poor decision
   for $REASONS. That might (might) be reflected long-term in this API
   by only supporting them in the way fossil does for platforms which
   do not support symlinks.
*/
int fsl__ckout_symlink_create(fsl_cx * f, char const *zTgtFile,
                              char const * zLinkFile);


/** @internal
   Compute all file name changes that occur going from check-in iFrom
   to check-in iTo. Requires an opened repository.

   If revOK is true, the algorithm is free to move backwards in the
   chain. This is the opposite of the oneWayOnly parameter for
   fsl_vpath_shortest().

   On success, the number of name changes is written into *pnChng.
   For each name change, two integers are allocated for *piChng. The
   first is the filename.fnid for the original name as seen in
   check-in iFrom and the second is for new name as it is used in
   check-in iTo. If *pnChng is 0 then *aiChng will be NULL.

   On error returns non-0, pnChng and aiChng are not modified, and
   f's error state might (depending on the error) contain a description
   of the problem.

   Space to hold *aiChng is obtained from fsl_malloc() and must
   be released by the caller.
*/
int fsl__find_filename_changes(fsl_cx * f,
                               fsl_id_t iFrom,
                               fsl_id_t iTo,
                               bool revOK,
                               uint32_t *pnChng,
                               fsl_id_t **aiChng);

/**  @internal
   Bitmask of file change types for use with
   fsl__is_locally_modified().
*/
enum fsl__localmod_e {
/** Sentinel value. */
FSL__LOCALMOD_NONE = 0,
/**
   Permissions changed.
*/
FSL__LOCALMOD_PERM = 0x01,
/**
   File size or hash (i.e. content) differ.
*/
FSL__LOCALMOD_CONTENT = 0x02,
/**
   The file type was switched between symlink and normal file.  In
   this case, no check for content change, beyond the file size
   change, is performed.
*/
FSL__LOCALMOD_LINK = 0x04,
/**
   File was not found in the local checkout.
 */
FSL__LOCALMOD_NOTFOUND = 0x10
};
typedef enum fsl__localmod_e fsl__localmod_e;
/** @internal

   Checks whether the given file has been locally modified compared to
   a known size, hash value, and permissions. Requires that f has an
   opened checkout.

   If zFilename is not an absolute path, it is assumed to be relative
   to the checkout root (as opposed to the current directory) and is
   canonicalized into an absolute path for purposes of this function.

   fileSize is the "original" version's file size.  zOrigHash is the
   initial hash of the file to use as a basis for comparison.
   zOrigHashLen is the length of zOrigHash, or a negative value if
   this function should use fsl_is_uuid() to determine the length. If
   the hash length is not that of one of the supported hash types,
   FSL_RC_RANGE is returned and f's error state is updated. This
   length is used to determine which hash to use for the comparison.

   If the file's current size differs from the given size, it is
   quickly considered modified, otherwise the file's contents get
   hashed and compared to zOrigHash.

   Because this is used for comparing local files to their state from
   the fossil database, where files have no timestamps, the local
   file's timestamp is never considered for purposes of modification
   checking.

   If isModified is not NULL then on success it is set to a bitmask of
   values from the fsl__localmod_e enum specifying the type(s) of
   change(s) detected:

   - FSL__LOCALMOD_PERM = permissions changed.

   - FSL__LOCALMOD_CONTENT = file size or hash (i.e. content) differ.

   - FSL__LOCALMOD_LINK = the file type was switched between symlink
     and normal file. In this case, no check for content change,
     beyond the file size change, is performed.

   - FSL__LOCALMOD_NOFOUND = file was not found in the local checkout.

   Noting that:

   - Combined values of (FSL__LOCALMOD_PERM | FSL__LOCALMOD_CONTENT) are
   possible, but FSL__LOCALMOD_NOTFOUND will never be combined with one
   of the other values.

   If stat() fails for any reason other than file-not-found
   (e.g. permissions), an error is triggered.

   Returns 0 on success. On error, returns non-0 and f's error state
   will be updated and isModified...  isNotModified. Errors include,
   but are not limited to:

   - Invalid hash length: FSL_RC_RANGE
   - f has no opened checkout: FSL_RC_NOT_A_CKOUT
   - Cannot find the file: FSL_RC_NOT_FOUND
   - Error accessing the file: FSL_RC_ACCESS
   - Allocation error: FSL_RC_OOM
   - I/O error during hashing: FSL_RC_IO

   And potentially other errors, roughly translated from errno values,
   for corner cases such as passing a directory name instead of a
   file.

   Results are undefined if any pointer argument is NULL or invalid.

   This function currently does NOT follow symlinks for purposes of
   resolving zFilename, but that behavior may change in the future or
   may become dependent on the repository's 'allow-symlinks' setting.

   Internal detail, not relevant for clients: this updates f's
   cache stat entry.
*/
int fsl__is_locally_modified(fsl_cx * f,
                            const char * zFilename,
                            fsl_size_t fileSize,
                            const char * zOrigHash,
                            fsl_int_t zOrigHashLen,
                            fsl_fileperm_e origPerm,
                            int * isModified);

/** @internal

   This routine cleans up the state of selected cards in the given
   deck. The 2nd argument is an list of upper-case letters
   representing the cards which should be cleaned up, e.g. "ADG". If
   it is NULL, all cards are cleaned up but d has non-card state
   which is not cleaned up by this routine. Unknown letters are simply
   ignored.
*/
void fsl__deck_clean_cards(fsl_deck * d, char const * letters);

/** @internal

    This starts a transaction (possibly nested) on the repository db
    and initializes some temporary db state needed for the
    crosslinking certain artifact types. It "should" (see below) be
    called at the start of the crosslinking process. Crosslinking
    *can* work without this but certain steps for certain (subject to
    change) artifact types will be skipped, possibly leading to
    unexpected timeline data or similar funkiness. No permanent
    SCM-relevant state will be missing, but the timeline might not be
    updated and tickets might not be fully processed. This should be
    used before crosslinking any artifact types, but will only have
    significant side effects for certain (subject to change) types.

    Returns 0 on success.

    If it returns 0 then the caller is OBLIGATED to either 1) call
    fsl__crosslink_end() or 2) call fsl_db_txn_rollback() and
    set f->cache.isCrosslinking to false. This process may install
    temporary tables and/or triggers, so failing to call one or the
    other of those will result in misbehavior.

    @see fsl__deck_crosslink()
*/
int fsl__crosslink_begin(fsl_cx * f);

/** @internal

    Must not be called unless fsl__crosslink_begin() has
    succeeded. This performs crosslink post-processing on certain
    artifact types and cleans up any temporary db state initialized by
    fsl__crosslink_begin().

    If the 2nd argument is not 0 then this routine triggers a rollback
    of the transaction started by fsl__crosslink_begin() and
    propagates any pending error code from f or (if f has no error
    code) from f's db handle.

    The second argument is intended to be the value of any pending
    result code (0 or otherwise) from any work done _after_
    fsl__crosslink_begin() succeeded. If passed 0, it assumes that
    there is no propagating error state and will attempt to complete
    the crosslinking process. If passed non-0, it triggers a rollback
    and unsets the f->cache.isCrosslinking flag, but does no
    additional work, then returns resultCode.

    Returns 0 on success. On error it initiates (or propagates) a
    rollback for the current transaction. If called when a rollback is
    pending, it unsets the crosslink-is-running flag and returns the
    propagating result code.
*/
int fsl__crosslink_end(fsl_cx * f, int resultCode);

/** @internal

   Searches the current repository database for a fingerprint and
   returns it as a string in *zOut.

   If rcvid<=0 then the fingerprint matches the last entry in the
   [rcvfrom] table, where "last" means highest-numbered rcvid (as
   opposed to most recent mtime, for whatever reason). If rcvid>0 then
   it searches for an exact match.

   Returns 0 on non-error. Finding no matching rcvid results in
   FSL_RC_NOT_FOUND. If 0 is returned then *zOut will be non-NULL and
   ownership of that value is transferred to the caller, who must
   eventually pass it to fsl_free(). On error, *zOut is not modified.

   Returns FSL_RC_NOT_A_REPO if f has no opened repository, FSL_RC_OOM
   on allocation error, or any number of potential db-related codes if
   something goes wrong at the db level.

   This API internally first checks for "version 1" fossil
   fingerprints and falls back to "version 0" fingerprint if a v1
   fingerprint is not found. Version 0 was very short-lived and is not
   expected to be in many repositories which are accessed via this
   library. Practice has, however, revealed some.

   @see fsl_ckout_fingerprint_check()
*/
int fsl__repo_fingerprint_search(fsl_cx * f, fsl_id_t rcvid,
                                 char ** zOut);

/** @internal

   State for running a raw diff.

   @see fsl__diff_all()
*/
struct fsl__diff_cx {
  /**
     aEdit describes the raw diff. Each triple of integers in aEdit[]
     means:

     (1) COPY:   Number of lines aFrom and aTo have in common
     (2) DELETE: Number of lines found only in aFrom
     (3) INSERT: Number of lines found only in aTo

     The triples repeat until all lines of both aFrom and aTo are
     accounted for. The array is terminated with a triple of (0,0,0).
  */
  int *aEdit /*TODO unsigned*/;
  /** Number of integers (3x num of triples) in aEdit[]. */
  int nEdit /*TODO unsigned*/;
  /** Number of elements allocated for aEdit[]. */
  int nEditAlloc /*TODO unsigned*/;
  /** File content for the left side of the diff. */
  fsl_dline *aFrom;
  /** Number of lines in aFrom[]. */
  int nFrom /*TODO unsigned*/;
  /** File content for the right side of the diff. */
  fsl_dline *aTo;
  /** Number of lines in aTo[]. */
  int nTo /*TODO unsigned*/;
  /** Predicate for comparing LHS/RHS lines for equivalence. */
  int (*cmpLine)(const fsl_dline * const, const fsl_dline *const);
};
/** @internal
   Convenience typeef.
*/
typedef struct fsl__diff_cx fsl__diff_cx;
/** @internal
    Initialized-with-defaults fsl__diff_cx structure, intended for
    const-copy initialization. */
#define fsl__diff_cx_empty_m {\
  NULL,0,0,NULL,0,NULL,0,fsl_dline_cmp \
}
/** @internal
   Initialized-with-defaults fsl__diff_cx structure, intended for
   non-const copy initialization. */
extern const fsl__diff_cx fsl__diff_cx_empty;

/** @internal

    Compute the differences between two files already loaded into
    the fsl__diff_cx structure.

    A divide and conquer technique is used.  We look for a large
    block of common text that is in the middle of both files.  Then
    compute the difference on those parts of the file before and
    after the common block.  This technique is fast, but it does
    not necessarily generate the minimum difference set.  On the
    other hand, we do not need a minimum difference set, only one
    that makes sense to human readers, which this algorithm does.

    Any common text at the beginning and end of the two files is
    removed before starting the divide-and-conquer algorithm.

    Returns 0 on succes, FSL_RC_OOM on an allocation error.
*/
int fsl__diff_all(fsl__diff_cx * p);

/** @internal */
void fsl__diff_cx_clean(fsl__diff_cx * cx);



/** @internal

    Undocumented. For internal debugging only. The 2nd argument is
    intended to be __FILE__ and the 3rd is intended to be __LINE__.
*/
void fsl__dump_triples(fsl__diff_cx const * p,
                       char const * zFile, int ln );

/** @internal

    Removes from the BLOB table all artifacts that are in the SHUN
    table. Returns 0 on success. Requires (asserts) that a repo is
    opened. Note that this is not a simple DELETE operation, as it
    requires ensuring that all removed blobs have been undeltified
    first so that no stale delta records are left behind.
*/
int fsl__shunned_remove(fsl_cx * f);

/** @internal

   This function is, as of this writing, only exposed via a header file
   for the sake of fnc, which still relies on it after we moved the
   "v1" diff code out of this library and into fnc.

   Attempt to shift insertion or deletion blocks so that they begin and
   end on lines that are pure whitespace.  In other words, try to transform
   this:

   ```
        int func1(int x){
           return x*10;
       +}
       +
       +int func2(int x){
       +   return x*20;
        }

        int func3(int x){
           return x/5;
        }
   ```

   Into one of these:

   ```
        int func1(int x){              int func1(int x){
           return x*10;                   return x*10;
        }                              }
       +
       +int func2(int x){             +int func2(int x){
       +   return x*20;               +   return x*20;
       +}                             +}
                                      +
        int func3(int x){              int func3(int x){
           return x/5;                    return x/5;
        }                              }
   ```
*/
void fsl__diff_optimize(fsl__diff_cx * p);

/** @internal

   This is a fossil-specific internal detail not needed by the more
   generic parts of the fsl_db API. It loops through all "cached"
   prepared statements for which stmt->role has been assigned a value
   which bitmasks as true against the given role and finalizes
   them. If such a statement is currently held by a call to/via
   fsl_db_prepare_cachedv() then this will NOT finalize that
   statement, will update db's error state, and return
   FSL_RC_MISUSE.

   Returns 0 on success.

   As a special case, if role==0 then ALL cached statements are
   closed, with the caveat that the process will still fail if any
   statement is currently flagged as active.
*/
int fsl__db_cached_clear_role(fsl_db * db, fsl_flag32_t role);

/** @internal

    Part of the crosslinking bits: rebuilds the entry for the ticket
    with the given K-card value.
*/
int fsl__ticket_rebuild(fsl_cx * f, char const * zTktId);

/** @internal

   Calls all registered crosslink link listeners, passing each the
   given deck. Returns 0 if there are no listeners or if all return 0,
   else it propagates an error from a failed listener.

   This must only be called while crosslinking is underway.

   @see fsl_xlink_listener()
*/
int fsl__call_xlink_listeners(fsl_deck * d);

/** @internal

   Copies symlink zFrom to a new symlink or pseudo-symlink named
   zTo.

   If realLink is true and this is a non-Windows platform,
   symlink zFrom is copied to zTo.

   If realLink is false or this is a Windows platform them...

   - On Windows this has currently undesired, or at least, highly
     arguable, behavior (historical, inherited from fossil(1)), in
     that an empty file named zTo will be created. In fossil(1) this
     function's counterpart is (apparently) never called on Windows,
     so that behavior seems to be moot. It is, however, important that
     this library never call it on Windows.

   - On non-Windows, a pseudo-symlink will be created: the string
     zFrom will be written to a regular file named zTo. That is, the
     file zTo will hold, as its contents, what it would point to if
     it were a symlink.
*/
int fsl__symlink_copy(char const *zFrom, char const *zTo, bool realLink);

/** @internal

   Translates sqliteCode (or, if it's 0, sqlite3_errcode()) to an
   approximate FSL_RC_xxx match but treats SQLITE_ROW and SQLITE_DONE
   as non-errors (result code 0). If non-0 is returned db's error
   state is updated with the current sqlite3_errmsg() string.
*/
int fsl__db_errcode(fsl_db * db, int sqliteCode);

/** @internal

   Plug in fsl_cx-specific db functionality into the given db handle.
   This must only be passed the MAIN db handle for the context,
   immediately after opening that handle, before f->dbMain is
   assigned.

   This function has very limited applicability and various
   preconditions which are assert()ed.
*/
int fsl__cx_init_db(fsl_cx * f, fsl_db * db);

/** @internal

    Attaches the given db file to f with the given role. This function "should"
    be static but we need it in repo.c when creating a new repository.

    This function has tightly-controlled preconditions which will assert
    if not met. r must be one of FSL_DBROLE_CKOUT or FSL_DBROLE_REPO.

    If createIfNotExists is true and zDbName does not exist in the
    filesystem, it is created before/as part of the OPEN or ATTACH. This is
    almost never desired, but is required for operations which create a
    repo (e.g. the aptly-named fsl_repo_create()) or a checkout db
    (e.g. fsl_repo_open_ckout()).
*/
int fsl__cx_attach_role(fsl_cx * f, const char *zDbName,
                        fsl_dbrole_e r, bool createIfNotExists);

/** @internal

    Returns one of f->{repo,ckout}.db or NULL.

    ACHTUNG and REMINDER TO SELF: the current (2021-03) design means
    that none of these handles except for FSL_DBROLE_MAIN actually has
    an sqlite3 db handle assigned to it. This returns a handle to the
    "level of abstraction" we need to keep track of each db's name and
    db-specific other state.

    e.g. passing a role of FSL_DBROLE_CKOUT this does NOT return
    the same thing as fsl_cx_db_ckout().
*/
fsl_db * fsl__cx_db_for_role(fsl_cx * f, fsl_dbrole_e r);

/** @internal

    Frees/clears the non-db state of f->ckout.
*/
void fsl__cx_ckout_clear(fsl_cx * f);


/** @internal
   Register the "files of checkin" (fsl_foci) SQLite3 virtual table.
*/
int fsl__foci_register(fsl_cx * f, fsl_db * db);

/** @internal

   If f has an SEE key-fetching function installed, it is called, its
   result is returned (but see below), and it is responsible for
   populating the final two arguments. If no such function is
   installed, or the function returns FSL_RC_UNSUPPORTED, the 3rd
   argument is unmodified, *keyType is set to 0, and 0 is
   returned. See fsl_cx_config::see and fsl_see_key_f() for details.

   If a non-0 value other than FSL_RC_UNSUPPORTED or FSL_RC_OOM are
   returned, f's error state will be updated with a generic message
   about SEE key init failure.
*/
int fsl__cx_see_key(fsl_cx * f, const char *zDbFile,
                    fsl_buffer *pOut, int *keyType);

/** @internal

    Maximum length of a line in a text file, in bytes. (2**15 = 32k)
*/
#define FSL__LINE_LENGTH_MASK_SZ  15

/** @internal

    Bitmask which, when AND-ed with a number, will result in the
    bottom FSL__LINE_LENGTH_MASK_SZ bits of that number.
*/
#define FSL__LINE_LENGTH_MASK     ((1<<FSL__LINE_LENGTH_MASK_SZ)-1)

/** @internal

    Internal impl of fsl_buffer_err(), implemented as a macro for
    efficiency's sake.
*/
#define fsl__buffer_err(B) (B)->errCode

//#if FSL_ENABLE_MUTEX
/** @internal

    If the lib is built with FSL_ENABLE_MUTEX, locks the library's
    global non-recursive mutex, which only guards a small amount of
    state. If the lib is not built with FSL_ENABLE_MUTEX, this is a
    no-op.
*/
void fsl__mutex_enter(void);

/** @internal

    If the lib is built with FSL_ENABLE_MUTEX, unlocks the library's
    global non-recursive mutex, else it's a no-op.

    May assert() that fsl__mutex_enter() has been called.
*/
void fsl__mutex_leave(void);

/** @internal

    Returns true if fsl__mutex_enter() is active, else false. Always
    returns true(!) in-single-threaded builds because that's how
    SQLite does it. This is only intended for use in assert() and the
    like - it must not be used for flow control.

    May assert() that fsl__mutex_enter() has been called.
*/
bool fsl__mutex_held(void);
//#else /* !FSL_ENABLE_MUTEX */
//#define fsl__mutex_held(X) ((void)(X),1)
//#define fsl__mutex_enter() (void)0
//#define fsl__mutex_leave() (void)0
//#endif
/** @internal

    Internal helper for parsing manifests and sync protocol lines.
    Holds the (mutable) content of a source file or input line and
    gets parsed and updated by fsl__tokenizer_next().
*/
struct fsl__tokenizer {
  /**
      First char of the next token.
  */
  unsigned char * z;
  /**
      One-past-the-end of the input.
  */
  unsigned char * zEnd;
  /**
      True if z points to the start of a new line.
  */
  bool atEol;
};
typedef struct fsl__tokenizer fsl__tokenizer;
#define fsl__tokenizer_empty_m {NULL,NULL,false}
extern const fsl__tokenizer fsl__tokenizer_empty;

/** @internal

   Returns a pointer to the next space-separated token in pIn. The
   token is zero-terminated.  Return NULL if there are no more tokens
   on the current line, but the call after that will return non-NULL
   unless we have reached the end of the input. If this function
   returns non-NULL then *pLen (which must not be NULL) is set to the
   byte length of the new token. Once the end of the input is reached,
   this function always returns NULL.

   A token is defined as a sequence of a non-whitespace (0x20),
   non-newline (0x0A) bytes. The input is modified, replacing
   spaces and newlines with a NUL.
*/
unsigned char *fsl__tokenizer_next(fsl__tokenizer * pIn,
                                   fsl_size_t * pLen);

enum {
  /**
     Max number of expected tokens in fsl__xfer::line.  This number is
     inherited from fossil(1).
  */
  fsl__xfer_max_tokens = 6
};

/** @internal

   Internal state for an ongoing fsl_sync(). Modelled largely after
   fossil(1)'s xfer.c:Xfer class.
*/
struct fsl__xfer {
  /**
     This object's fossil context. Errors encountered by the channel
     should be sent here using fsl_xfer_error() and friends. A channel's
     impl may use the context for other purposes, e.g. fetching or
     storing channel-specific configuration info like credentials or
     using a db-side cache.

     f must outlive this object.
  */
  fsl_cx * f;

  /**
     The underlying communication channel.
  */
  fsl_sc * ch;

  /**
     The configuration object for a given xfer trip. Its initial state
     represents the initial request parameters and it may get updated
     as the sync progresses.
  */
  fsl_xfer_config * config;

  /**
     Almost all APIs which deal with this class (A) become no-ops if
     this is non-0, returning this value instead, and (B) set their
     result code in this member. This simplifies some internals by
     allowing us to forego explicit error checks when making multiple
     xfer-related calls in a row.
  */
  int rc;

  /** One line of sync protocol input, which we tokenize via
      fsl__xfer_readln() for ease of processing. */
  struct {
    /**
       Holds the current input line. Populated and tokenized by
       fsl__xfer_readln().
    */
    fsl_buffer b;
    /**
       A raw, untokenized copy of b.
    */
    fsl_buffer bRaw;
    /** Holds pointers to the individual tokens of b.
        The memory lives in this->b. These are non-const
        only so that we can use fsl_bytes_defossilize()
        on them. */
    unsigned char * aToken[fsl__xfer_max_tokens];
    /**
       Holds the strlen() of each token in aTokens.
    */
    fsl_size_t aTokLen[fsl__xfer_max_tokens];
    /** The number of tokens in use in aToken. */
    uint8_t nToken;
  } line;
  /** Memory buffers */
  struct {
    /**
       When using compressed responses, this is where it will store
       the uncompressed response. We decompress it on behalf of the
       fsl_sc so that those impls don't have to all do this on their
       own.
    */
    fsl_buffer uresp;
    /** Hashing and UUID-fetching buffer */
    fsl_buffer hash;
    /** Generic scratchpad buffer */
    fsl_buffer scratch;
    /** Card payloads */
    fsl_buffer cardPayload;
    /** fsl_xfer_emit() and friends */
    fsl_buffer emit;
  } buf;
  /** Prepared statements */
  struct {
    fsl_stmt igotInsert;
    fsl_stmt igotHasRid;
    fsl_stmt gimmeInsert;
    //fsl_stmt gimmeDelete;
    fsl_stmt remoteUnkUuid;
  } q;
  /** Various flags. */
  /* TODO: move some of this into fsl_xfer_config. */
  struct {
    unsigned configRcvMask;
    unsigned configSendMask;
    char const *zAltPCode;
    /** True to enable syncing private content */
    bool syncPrivate;
    /** If true, next "file" received is a private artifact. */
    bool nextIsPrivate;
  } flag;
  /** Info about the remote. */
  struct {
    int releaseVersion;
    int manifestDate;
    int manifestTime;
    /** Seconds of offset from localhost. */
    int clockSkewSeen;
    /* Reminder to self: calculating the clock skew currently requires
       parsing a comment line from the sync protocol. TODO is add a
       pragma to simplify that on the remote end. */
  } remote;
  struct {
    int number;
    int date;
    int time;
    /**
       Tells us whether the remote project-code matches this one.
       If not, some operations won't work (like logging in - that
       only works when project codes match because the project code
       is part of the password hash).

       Values: <0=unknown, 0=mismatch, >0=match.
    */
    short projCodeMatch;
  } clientVersion;
  struct {
    int protocolVersion;
    fsl_size_t seqNo;
  } clone;
  /**
     Info about the user which is relevant for sync.
  */
  struct {
    /**
       User name memory owned by fsl__xfer::url.
    */
    char const * name;
    /**
       Password memory owned by fsl__xfer::url.
    */
    char const * password;
    /**
       Password memory owned by this object. Gets extracted from the
       repo.user table if no password is otherwise provided.
    */
    char * passwordToFree;

    /**
       Permissions calculated for the current user from sync login
       info.
    */
    fsl_uperm perm;
  } user;
  /**
     When using compressed responses, we need a layer of buffer to do
     the decompression. This object, if populated, is a _partial_
     fsl_sc implementation which buffers the whole uncompressed
     response and only supports reading.
  */
  fsl_sc chz;
  /** Various counters. */
  fsl_xfer_metrics n;

  struct {
    /**
       Hash of all request payload content, for generating the sync
       login card.
    */
    fsl_sha1_cx sha1;
    fsl_buffer loginCard;
    /**
       Number of bytes submitted for the request body.
    */
    fsl_size_t nBytes;
  } request;

  struct {
    int httpVersion;
    int httpCode;
  } response;
#if 0
  /* From fossil(1) xfer.c:Xfer struct: */
  fsl_size_t mxSend;         /* Stop sending "file" when pOut reaches this size */
  int resync;         /* Send igot cards for all holdings */
  uint32_t remoteVersion;  /* Version of fossil running on the other side */
  uint32_t remoteDate;     /* Date for specific client software edition */
  uint32_t remoteTime;     /* Time of date correspoding on remoteDate */
  fsl_time_t maxTime;     /* Time when this transfer should be finished */
#endif
};
typedef struct fsl__xfer fsl__xfer;
/** Empty-initialized fsl__xfer instance, intended for copy
    initialization. */
extern const fsl__xfer fsl__xfer_empty;


/** @internal

   Create a child process running shell command "zCmd". *pfdIn gets
   assigned to the stdout from the child process.  (The caller reads
   from *pfdIn in order to receive input from the child.) *ppOut gets
   assigned to a FILE that becomes the standard input of the child
   process.  (The caller writes to *ppOut in order to send text to the
   child.) Note that *pfdIn is an unbuffered file descriptor, not a
   FILE. The process ID of the child is written into *pChildPid.

   On success the values returned via *pfdIn, *ppOut, and *pChildPid
   must be passed to fsl_pclose2() to properly clean up.

   Return 0 on success, non-0 on error.

   @internal
*/
int fsl__popen2(const char *zCmd, int *pfdIn, FILE **ppOut,
                           int *pChildPid);

/** @internal

   Close the connection to a child process previously created using
   fsl__popen2(). All 3 arguments are assumed to values returned via
   fsl__popen2()'s output parameters: the input file descriptor,
   output FILE handle, and child process PID.

   Return the waitpid()-derived exit code of the child process.

   On Windows platforms, killing of the child process is not
   implemented. (Patches are welcomed.)
*/
int fsl__pclose2(int fdIn, FILE *pOut, int childPid);


/** @internal

   Hash value for fsl_xfcard card name hashing.

   This algo historically uses 32-bit hashes because the values
   (generated at build-time) were used in switch/case contexts, where
   64-bit values are not portable.

   @internal
*/
typedef uint32_t fsl_xfcard_hash_t;

/**
   Lowest-valued legal identifier character in a sync card name.  This
   plays a role in the hash.
*/
#define fsl_xfcard_LowestIdChar '$'
#define fsl_xfcard_MaxCardNameLen 11 /*==strlen("clone_seqno")*/

/** @internal

   Internal impl for one hash step of fsl_xfcard_hash() and the HASH#()
   family of macros (for compile-time hashes). Arguments:

   - H is the current hash value (starting at 0)
   - I is the char index into the word we're hashing.
   - CHAR is the character at index I of the word.

   Reminder to self:

   collision with "else" and "eval":
   h = (h << 1) + (++i) * (CHAR - 35)
*/
#if 1
#define fsl_xfcard_hash_step_ch(H,I,CHAR)  \
  (((((fsl_xfcard_hash_t)H << 1) + (I+1)) * (CHAR - fsl_xfcard_LowestIdChar-1)) ^ CHAR)
#elif 0
#define fsl_xfcard_hash_step_ch(H,I,CHAR)  \
  ((((H << 1) + (45*I) + CHAR - fsl_xfcard_LowestIdChar-1)) ^ CHAR)
#else
#define fsl_xfcard_hash_step_ch(H,I,CHAR)  \
  ((H << 1) + 113*I + (CHAR - fsl_xfcard_LowestIdChar-1))
#endif

/** @internal

   Slightly-convenience form of fsl_xfcard_hash_step_ch() which takes
   its 3rd argument as an unquoted word.

   The first invocation for a given WORD must pass 0 for H and I. Each
   subsequent call must pass the result the previous call as its H, an
   incremented-by-1 I value, and the same WORD.
*/
#define fsl_xfcard_hash_step(H,I,WORD) \
  fsl_xfcard_hash_step_ch(H,I,(#WORD[I]))

/** @internal

   Very simple hash which has proven to be collision-free for
   several dozen keywords in a live scripting engine.

   This hash is used to generate ids for limited keyword lists such
   that we can verify, via assert(), that the hash iscollision-free
   for the full input set.

   Aside from the current implementation, these variations of
   fsl_xfcard_hash_step have worked out in prior testing:

   - ((H << 1) + 100*I + I*CHAR - I* (fsl_xfcard_LowestIdChar - 1))
   - ((H << 1) + 45*I + CHAR - fsl_xfcard_LowestIdChar - 1)

   Design note: this takes an unsigned string because its argument is
   invariably from fsl__xfer::line::aToken.
*/
fsl_xfcard_hash_t fsl_xfcard_hash( unsigned const char * word );

/** Non-unsigned-qualified C-string convenience form of fsl_xfcard_hash(). */
#define fsl_xfcard_hashS(WORD) fsl_xfcard_hash((unsigned const char *)WORD)

/** @internal

   Compile-time hashers for words of length 1..11.
*/
#define fsl_xfcard_hash_1(W) fsl_xfcard_hash_step(0,0,W)
#define fsl_xfcard_hash_2(W) fsl_xfcard_hash_step(fsl_xfcard_hash_1(W),1,W)
#define fsl_xfcard_hash_3(W) fsl_xfcard_hash_step(fsl_xfcard_hash_2(W),2,W)
#define fsl_xfcard_hash_4(W) fsl_xfcard_hash_step(fsl_xfcard_hash_3(W),3,W)
#define fsl_xfcard_hash_5(W) fsl_xfcard_hash_step(fsl_xfcard_hash_4(W),4,W)
#define fsl_xfcard_hash_6(W) fsl_xfcard_hash_step(fsl_xfcard_hash_5(W),5,W)
#define fsl_xfcard_hash_7(W) fsl_xfcard_hash_step(fsl_xfcard_hash_6(W),6,W)
#define fsl_xfcard_hash_8(W) fsl_xfcard_hash_step(fsl_xfcard_hash_7(W),7,W)
#define fsl_xfcard_hash_9(W) fsl_xfcard_hash_step(fsl_xfcard_hash_8(W),8,W)
#define fsl_xfcard_hash_10(W) fsl_xfcard_hash_step(fsl_xfcard_hash_9(W),9,W)
#define fsl_xfcard_hash_11(W) fsl_xfcard_hash_step(fsl_xfcard_hash_10(W),10,W)
#define fsl_xfcard_hash_N(N,W) fsl_xfcard_hash_ ## N(W)

/** @internal

   Objects for mapping known sync cards to their fsl_xfcard_e and
   hash of their name.
*/
struct fsl_xfcard {
  /** Card type ID. */
  const fsl_xfcard_e type;
  /** The protocol-level name of this card, e.g. "file" or "cfile". */
  char const * const name;
  /** A hash unique (in the context of fsl_xfcard) to this->name. */
  const fsl_xfcard_hash_t hash;
};
typedef struct fsl_xfcard fsl_xfcard;

/** @internal

   Searches for a sync card with the given name. Returns a
   static/const instance of one on success, else NULL. This is a
   binary search ordered on the card names' hashes, so is fairly fast:
   hash z, binary search on that hash, and one fsl_strcmp() on a
   match.  The hash algo bails early if z is not shaped like a card
   name.

   Design note: this takes an unsigned string because its argument is
   invariably from fsl__xfer::line::aToken.
*/
fsl_xfcard const *fsl__xfcard_search(unsigned char const *z);
/** Non-unsigned-qualified C-string convenience form of fsl__xfcard_search(). */
#define fsl__xfcard_searchNU(WORD) fsl__xfcard_search((unsigned const char *)(WORD))

/** @internal

   An fgets() workalike which uses an unbuffered file descriptor for
   reading.

   Scans the input 1 byte at a time for a '\n', appending each read
   character to pOut, including the '\n' and a trailing NUL, up to a
   maximum of nOut-1 (to account for the NUL). If it hits EOF before
   finding a \n, that is not treated as an error, and downstream code
   may need to special-case that.

   On success it returns 0, sets *ppOut to pOut, and *pnOut to the
   number of bytes filled out in pOut. On error, returns FSL_RC_IO on
   read error or FSL_RC_RANGE if no newline is found before nOut bytes
   are read.

   If called when fdIn is at EOF, it will return 0 but set *pnOut to
   0.

   On error it will return either FSL_RC_RANGE (if it finds now
   newline before nOut bytes) or propagate an error from the read()
   call, in the form of another non-0 fsl_rc_e value.
*/
int fsl__fdgets(int fdIn, unsigned char * pOut, fsl_size_t nOut,
                char **ppOut, fsl_size_t * pnOut);

/** @internal

   fsl_file_tempname() proxy which uses fsl_temp_dirs_get() to get the
   target dir. zBaseName may be NULL but library routines are strongly
   enouraged to use a of "libfossil-PURPOSE", where PURPOSE is a
   descrtive identifier, e.g. "PopenRequest" or "popen-request".
*/
int fsl__tmpfile( fsl_buffer * tgt, char const * zBaseName );

/**
   If FSL_PLATFORM_IS_UNIX then this returns the result of chmod(2)
   for the given file name, converted from errno using
   fsl_errno_to_rc(). It uses permissions 0600.

   On other platforms this is a no-op and returns 0.
*/
int fsl__tmpchmod( char const * z );

// Potential TODO
//int fsl__tmpfile_open( char const * zBaseName, FILE **fOut );

/** @internal

   Searches the OS path zPath for a binary named zBinName. If found,
   it puts a library-allocated copy of the resolved name to *zOut
   (which the caller must eventually pass to fsl_free()) then returns
   0.

   If zPath is NULL, getenv("PATH") is used.

   On error, *zOut is not modified and non-0 is returned:

   - FSL_RC_NOT_FOUND if no match is found. This will also be returned
     if both zPath and $PATH are NULL.

   - FSL_RC_OOM on allocation error.

   On Windows platforms it will extend the search to include a ".exe"
   extension.
*/
int fsl__find_bin( char const *zPath, char const *zBinName, char **zOut );

/**
   If d->P.used then this makes d->P.list[0] a delta from d->rid, else
   this is a no-op. Returns non-0 if fetching the parent RID or
   deltification fail. Returns 0 if there's nothing to do or if what
   needs doing gets done.
*/
int fsl__deck_deltify_parent(fsl_deck * d);

/**
   Imports content to f's opened repository's BLOB table using a
   client-provided input source. f must have an opened repository
   db. inFunc is the source of the data and inState is the first
   argument passed to inFunc(). If inFunc() succeeds in fetching all
   data (i.e. if it always returns 0 when called by this function)
   then that data is inserted into the blob table _if_ no existing
   record with the same hash is already in the table. If such a record
   exists, it is assumed that the content is identical and this
   function has no side-effects vis-a-vis the db in that case.

   If rid is not NULL then the BLOB.RID record value (possibly of an
   older record!) is stored in *rid.  If uuid is not NULL then the
   BLOB.UUID record value is stored in *uuid and the caller takes
   ownership of those bytes, which must eventually be passed to
   fsl_free() to release them.

   rid and uuid are only modified on success and only if they are
   not NULL.

   Returns 0 on success, non-0 on error. For errors other than basic
   argument validation and OOM conditions, f's error state is
   updated with a description of the problem. Returns FSL_RC_MISUSE
   if either f or inFunc are NULL. Whether or not inState may be
   NULL depends on inFunc's concrete implementation.

   Be aware that BLOB.RID values can (but do not necessarily) change
   in the life of a repod db (via a reconstruct, a full re-clone, or
   similar, or simply when referring to different clones of the same
   repo). Thus clients should always store the full UUID, as opposed
   to the RID, for later reference. RIDs should, in general, be
   treated as session-transient values. That said, for purposes of
   linking tables in the db, the RID is used exclusively (clients are
   free to link their own extension tables using UUIDs, but doing so
   has a performance penalty comared to RIDs). For long-term storage
   of external links, and to guaranty that the data be usable with
   other copies of the same repo, the UUID is required.

   Note that Fossil may deltify, compress, or otherwise modify
   content on its way into the blob table, and it may even modify
   content long after its insertion (e.g. to make it a delta against
   a newer version). Thus clients should normally never try
   to read back the blob directly from the database, but should
   instead read it using fsl_content_get().

   That said: this routine has no way of associating and older version
   (if any) of the same content with this newly-imported version, and
   therefore cannot delta-compress the older version.

   Maintenance reminder: this is basically just a glorified form of
   the internal fsl__content_put(). Interestingly, fsl__content_put()
   always sets content to public (by default - the f object may
   override that later). It is not yet clear whether this routine
   needs to have a flag to set the blob private or not. Generally
   speaking, privacy is applied to fossil artifacts, as opposed to
   content blobs.

   @see fsl__repo_import_buffer()
*/
int fsl__repo_import_blob( fsl_cx * f, fsl_input_f inFunc,
                          void * inState, fsl_id_t * rid,
                          fsl_uuid_str * uuid );

/**
   A convenience form of fsl__repo_import_blob(), equivalent to:

   ```
   fsl__repo_import_blob(f, fsl_input_f_buffer, bIn, rid, uuid )
   ```

   except that (A) bIn is const in this call and non-const in the
   other form (due to cursor traversal requirements) and (B) it
   returns FSL_RC_MISUSE if bIn is NULL.
*/
int fsl__repo_import_buffer( fsl_cx * f, fsl_buffer const * bIn,
                            fsl_id_t * rid, fsl_uuid_str * uuid );

enum fsl__cx_stmt_e {
#define E(NAME,IGNORED) fsl__cx_stmt_e_ ## NAME,
  fsl__cx_cache_stmt_map(E)
#undef E
};

/**
   Returns the given cached statement object, preparing it if needed.
   If preparation fails then NULL is returned and f->error will
   hold the details.

   If this returns non-NULL then the caller is obligated to pass the
   same q object to fsl__cx_stmt_yield().
*/
fsl_stmt * fsl__cx_stmt(fsl_cx * f, enum fsl__cx_stmt_e which);

/**
   Must be passed all statement pointers obtained via fsl__cx_stmt().
   It MUST NOT be passed any statement from elsewhere except that
   q==NULL is permitted to simplify its usage. This resets the
   statement and clears any bindings.

   Callers must treat q as if this function finalizes it, i.e. they
   must not derefence any members or pass it to another API after
   this. It can only be legally used again after fetching it from
   fsl__cx_stmt().
*/
void fsl__cx_stmt_yield(fsl_cx * f, fsl_stmt * q);

/**
    Clears various internal caches and resets various
    internally-cached values related to repository and checkout
    dbs. Its argument must be true if we're clearing caches as a
    result of a transaction commit, else false.
*/
void fsl__cx_caches_reset(fsl_cx * f, bool isTxnCommit);

/**
   May clear or reset cached data. This is intended to be called as
   part of a transaction commit/rollback hook and be passed true for a
   commit and false for a rollback.
*/
void fsl__cx_reset_for_txn(fsl_cx * f, bool isCommit);

/**
   Clears only caches which are relevant to content lookup.
*/
void fsl__cx_content_caches_clear(fsl_cx * f);

/**
   Clears the contents of f->cache.mcache.
*/
void fsl__cx_mcache_clear(fsl_cx * f);

/**
   An experiment.

   Runs BODY ( a {...} block) in the context of a fsl_cx savepoint.
   The body is expected to assign int-type RcVar to 0 on success and
   false on error. If RcVar is 0 when this is invoked, BODY is not
   run.  If BODY succeeds, the savepoint is saved if KeepSPOnSuccess
   is true, else it is silently discarded. If RollbackTopOnError is
   true then any outer savepoint is put into a persistent rollback
   state, else any outer savepoint is unaffected. If BODY succeeds but
   the commit/rollback fails, RcVar is set to that failure code.
*/
#define fsl__cx_txn(FslCx, RcVar, RollbackTopOnError, KeepSPOnSuccess, BODY) { \
    RcVar = fsl_cx_txn_begin(FslCx);                                \
    if( 0==RcVar ){                                                 \
      BODY                                                          \
    }                                                               \
    int const RcVar ## __ =                                         \
      fsl_cx_txn_end_v2((FslCx), (0==RcVar) && (KeepSPOnSuccess),   \
                        ((RollbackTopOnError) && (0!=RcVar)));      \
    if( !RcVar ) RcVar = RcVar ## __;                               \
  }

/**
   fsl__cx_txn() proxy for use with local savepoints which should not
   trigger a global savepoint rollback on error.
*/
#define fsl__cx_txn_local(FslCx, RcVar, KeepSPOnSuccess, BODY) \
    fsl__cx_txn(FslCx, RcVar, 0, KeepSPOnSuccess, BODY)

/**
   fsl__cx_txn() proxy for use with local savepoints which should
   trigger a global savepoint rollback on error.
*/
#define fsl__cx_txn_global(FslCx, RcVar, KeepSPOnSuccess, BODY) \
    fsl__cx_txn(FslCx, RcVar, 1, KeepSPOnSuccess, BODY)

void fsl__id_bag_dump(fsl_id_bag const * src, FILE *to/*NULL==stdout*/,
                      char const *zHeader);

/* fsl__ptl details are in cache.c */
int fsl__ptl_init(fsl__ptl * c, uint16_t prealloc);
void fsl__ptl_clear(fsl__ptl * c);
bool fsl__ptl_has(fsl__ptl const * c, fsl__ptl_e where, fsl_id_t rid);
int fsl__ptl_insert(fsl__ptl * c, fsl__ptl_e where, fsl_id_t rid);
bool fsl__ptl_remove(fsl__ptl const * c, fsl__ptl_e where, fsl_id_t rid);

/**
   Can fail with OOM if isCommit and f->cache.ptl.level>0.
*/
int fsl__cx_ptl_pop(fsl_cx * f, bool isCommit);
int fsl__cx_ptl_push(fsl_cx * f);
/**
   Returns the given bag for the current transaction level.  Never
   returns NULL unless "which" is invalid, in which case it prefers to
   fail fatally.
*/
fsl_id_bag * fsl__cx_ptl_bag(fsl_cx *f, fsl__ptl_e which);
/**
   Removes rid from the given PTL list.
*/
bool fsl__cx_ptl_remove(fsl_cx * f, fsl__ptl_e where, fsl_id_t rid);
#define fsl__cx_ptl_has(F,W,R) fsl__ptl_has(&(F)->cache.ptl, W, R)
#define fsl__cx_ptl_insert(F,W,R) fsl__ptl_insert(&(F)->cache.ptl, W, R)

/**
   Sets *rid to f->db.ckout.rid, fetching the latter if
   needed. Returns non-0 if fetching fails.
*/
int fsl__ckout_rid(fsl_cx * f, fsl_id_t *rid);

#if !defined(NDEBUG)
/**
   A place to put a breakpoint to help trace down errors. Pass it the
   rc of a just-failed operation and set a breakpoint in the if(rc)
   block of this function in fsl.c.
*/
void fsl__bprc(int rc);
#else
#define fsl__bprc(X) (void)0
#endif

#if defined(__cplusplus)
} /*extern "C"*/
#endif
#endif
/* ORG_FOSSIL_SCM_FSL_INTERNAL_H_INCLUDED */
