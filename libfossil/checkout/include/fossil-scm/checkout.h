/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
#if !defined(ORG_FOSSIL_SCM_FSL_CHECKOUT_H_INCLUDED)
#define ORG_FOSSIL_SCM_FSL_CHECKOUT_H_INCLUDED
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/

/** @file checkout.h

    fossil-checkout.h declares APIs specifically dealing with
    checkout-side state, as opposed to purely repository-db-side state
    or non-content-related APIs.
*/

#include "db.h" /* MUST come first b/c of config macros */
#include "repo.h"

#if defined(__cplusplus)
extern "C" {
#endif


/**
   Tries to open a checked-out fossil repository db in the given
   directory (or "." if dirName is NULL). This routine canonicalizes
   its dirName argument using fsl_file_canonical_name(), then passes
   that and checkParentDirs on to fsl_ckout_db_search() to find a
   checkout db, so see that routine for how it searches.

   If this routine finds/opens a checkout, it also tries to open
   the repository database from which the checkout derives, and
   fails if it cannot. The library never allows a checkout to be
   opened without its corresponding repository partner because
   a checkout has hard dependencies on the repo's state.

   Returns 0 on success. If there is an error opening or validating
   the checkout or its repository db, f's error state will be
   updated. Error codes/conditions include:

   - FSL_RC_MISUSE if f is NULL.

   - FSL_RC_ACCESS if f already has and opened checkout.

   - FSL_RC_OOM if an allocation fails.

   - FSL_RC_NOT_FOUND if no checkout is foud or if a checkout's
   repository is not found.

   - FSL_RC_RANGE if dirname is not NULL but has a length of 0.

   - Various codes from fsl_getcwd() (if dirName is NULL).

   - Various codes if opening the associated repository DB fails.

   TODO: there's really nothing in the architecture which restricts a
   checkout db to being in the same directory as the checkout, except
   for some historical bits which "could" be refactored. It "might be
   interesting" to eventually provide a variant which opens a checkout
   db file directly. We have the infrastructure, just need some
   refactoring. It "shouldn't" require any trickery or
   incompatibilities with fossil(1).
*/
FSL_EXPORT int fsl_ckout_open_dir( fsl_cx * const f, char const * dirName,
                                   bool checkParentDirs );


/**
   Searches the given directory (or the current directory if dirName
   is 0) for a fossil checkout database file named one of (_FOSSIL_,
   .fslckout).  If it finds one, it returns 0 and appends the file's
   path to pOut if pOut is not 0.  If neither is found AND if
   checkParentDirs is true an then it moves up the path one directory
   and tries again, until it hits the root of the dirPath (see below
   for a note/caveat).

   If dirName is NULL then it behaves as if it had been passed the
   absolute path of the current directory (as determined by
   fsl_getcwd()).

   This function does no normalization of dirName. Because of that...

   Achtung: if dirName is relative, this routine might not find a
   checkout where it would find one if given an absolute path (because
   it traverses the path string given it instead of its canonical
   form). Whether this is a bug or a feature is not yet clear. When in
   doubt, use fsl_file_canonical_name() to normalize the directory
   name before passing it in here. If it turns out that we always want
   that behaviour, this routine will be modified to canonicalize the
   name.

   This routine can return at least the following error codes:

   - FSL_RC_NOT_FOUND: either no checkout db was found or the given
   directory was not found.

   - FSL_RC_RANGE if dirName is an empty string. (We could arguably
   interpret this as a NULL string, i.e. the current directory.)

   - FSL_RC_OOM if allocation of a filename buffer fails.

   TODO?

   - Why was the decision made not to canonicalize dirName from here?
   We might want to change that.

*/
FSL_EXPORT int fsl_ckout_db_search( char const * dirName,
                                    bool checkParentDirs,
                                    fsl_buffer * const pOut );

/**
   Returns version information for the current checkout.

   If f has an opened checkout then...

   If uuid is not NULL then *uuid is set to the UUID of the opened
   checkout, or NULL if there is no checkout. If rid is not NULL, *rid
   is set to the record ID of that checkout, or 0 if there is no
   checkout (or the current checkout is from an empty repository). The
   returned uuid bytes and rid are owned by f and valid until the
   library updates its checkout state to a newer checkout version
   (essentially unpredictably). When in doubt about lifetime issues,
   copy the UUID immediately after calling this if they will be needed
   later.

   Corner case: a new repo with no checkins has an RID of 0 and a UUID
   of NULL. That does not happen with fossil-generated repositories,
   as those always "seed" the database with an initial commit artifact
   containing no files.
*/
FSL_EXPORT void fsl_ckout_version_info(fsl_cx * const f, fsl_id_t * const rid,
                                       fsl_uuid_cstr * const uuid );

/**
   Given a fsl_cx with an opened checkout, and a file/directory name,
   this function canonicalizes zOrigName to a form suitable for use as
   an in-repo filename, _appending_ the results to pOut. If pOut is
   NULL, it performs its normal checking but does not write a result,
   other than to return 0 for success.

   As a special case, if zOrigName refers to the top-level checkout
   directory, it resolves to either "." or "./", depending on whether
   zOrigName contains a trailing slash.

   If relativeToCwd is true then the filename is canonicalized
   based on the current working directory (see fsl_getcwd()),
   otherwise f's current checkout directory is used as the virtual
   root.

   If the input name contains a trailing slash, it is retained in
   the output sent to pOut except in the top-dir case mentioned
   above.

   Returns 0 on success, meaning that the value appended to pOut
   (if not NULL) is a syntactically valid checkout-relative path.

   Returns FSL_RC_RANGE if zOrigName points to a path outside
   of f's current checkout root.

   Returns FSL_RC_NOT_A_CKOUT if f has no checkout opened.

   Returns FSL_RC_MISUSE if !zOrigName, FSL_RC_OOM on an allocation
   error.

   This function does not validate whether or not the file actually
   exists, only that its name is potentially valid as a filename
   for use in a checkout (though other, downstream rules might prohibit that, e.g.
   the filename "..../...." is not valid but is not seen as invalid by
   this function). (Reminder to self: we could run the end result through
   fsl_is_simple_pathname() to catch that?)
*/
FSL_EXPORT int fsl_ckout_filename_check( fsl_cx * const f, bool relativeToCwd,
                                         char const * zOrigName, fsl_buffer * const pOut );

/**
   Convenience typedef and required forward decl.
 */
typedef struct fsl_ckout_manage_opt fsl_ckout_manage_opt;

/**
   State passed to fsl_ckout_manage_f() callback.
*/
struct fsl_ckout_manage_state {
  /**
     The fsl_cx instance for the correspnding fsl_ckout_manage()
     call.
  */
  fsl_cx * f;
  /**
     The options object passed to the corresponding call to
     fsl_ckout_manage(). This is provided primarily for access to
     its callbackState member. The client must never modify any state
     in this opt object other than the callbackState or to use other
     APIs of this->f.
  */
  fsl_ckout_manage_opt const * opt;
  /**
     The name of the file being managed, relative to the root of the
     checkout. fsl_cx_stat2() can be used to transform it to an
     absolute path, if needed. Given a fsl_ckout_manage_state
     named `ms`, that call would look like:

     ```
     fsl_buffer_reuse(&yourBuffer);
     // ^^^ noting that fsl_cx_stat2() APPENDS to the buffer.
     int rc = fsl_cx_stat2(ms->f, false, ms->filename,
                           NULL, &yourBuffer, true);
     // ^^^ noting that the stat "should not" fail in this case
     // because fsl_ckout_manage() has validated that the file
     // exists.
     ```
  */
  char const * filename;
};

/**
   Convenience typedef.
*/
typedef struct fsl_ckout_manage_state fsl_ckout_manage_state;

/**
   Callback type for use with fsl_ckout_manage_opt(). It should
   inspect the given (`mst->filename`) using whatever criteria it likes, set
   *include to true or false to indicate whether the filename is okay
   to include the current add-file-to-repo operation, and return 0.

   If it returns non-0 the add-file-to-repo process will end and that
   error code will be reported to its caller. Such result codes must
   come from the FSL_RC_xxx family. They may report details of
   the error via fsl_cx_err_set() with the `mst->f` object.

   It will be passed a name which is relative to the top-most checkout
   directory.

   The final argument is not used by the library, but is passed on
   as-is from the fsl_ckout_manage_opt::callbackState pointer which
   is passed to fsl_ckout_manage().
*/
typedef int (*fsl_ckout_manage_f)(fsl_ckout_manage_state const *mst,
                                  bool *include);
/**
   Options for use with fsl_ckout_manage().
*/
struct fsl_ckout_manage_opt {
  /**
     The file or directory name to add. If it is a directory, the add
     process will recurse into it.

     Note that filenames need to be passed in their "proper" case for
     the current platform. fsl_filename_preferred_case() case be used
     to do that, but that functionality is not currently part of the
     fsl_ckout_manage() inner workings (that may change at some
     point). fsl_cx_is_case_sensitive() can be used to determine
     whether or not case-sensitivity is preferred for the current
     repository.
  */
  char const * filename;
  /**
     Whether to evaluate the given name as relative to the current working
     directory or to the current checkout root.

     This makes a subtle yet important difference in how the name is
     resolved. CLI apps which take file names from the user from
     within a checkout directory will generally want to set
     relativeToCwd to true. GUI apps, OTOH, will possibly need it to
     be false, depending on how they resolve and pass on the
     filenames.
  */
  bool relativeToCwd;
  /**
     Whether or not to check the name(s) against the 'ignore-globs'
     config setting (if set).
  */
  bool checkIgnoreGlobs;
  /**
     Optional predicate function which may be called for each
     to-be-added filename. It is only called if:

     - It is not NULL (obviously) and...

     - The file is not already in the checkout database and...

     - The is-internal-name check passes (see
     fsl_reserved_fn_check()) and...

     - If checkIgnoreGlobs is false or the name does not match one of
     the ignore-globs values.

     The name it is passed is relative to the checkout root.

     Because the callback is called only if other options have not
     already excluded the file, the client may use the callback to
     report to the user (or otherwise record) exactly which files
     get added.
  */
  fsl_ckout_manage_f callback;

  /**
     State to be passed to this->callback.
  */
  void * callbackState;

  /**
     These counts are updated by fsl_ckout_manage() to report
     what it did.
  */
  struct {
    /**
       Number of files actually added by fsl_ckout_manage().
    */
    uint32_t added;
    /**
       Number of files which were requested to be added but were only
       updated because they had previously been added. Updates set the
       vfile table entry's current mtime, executable-bit state, and
       is-it-a-symlink state. (That said, this code currently ignores
       symlinks altogether!)
    */
    uint32_t updated;
    /**
       The number of files skipped over for addition. This includes
       files which meet any of these criteria:

       - fsl_reserved_fn_check() fails.

       - If the checkIgnoreGlobs option is true and a filename matches
         any of those globs.

       - The client-provided callback says not to include the file.
    */
    uint32_t skipped;
  } counts;
};
/**
   Initialized-with-defaults fsl_ckout_manage_opt instance,
   intended for use in const-copy initialization.
*/
#define fsl_ckout_manage_opt_empty_m {\
    NULL/*filename*/, true/*relativeToCwd*/, true/*checkIgnoreGlobs*/, \
    NULL/*callback*/, NULL/*callbackState*/,                         \
    {/*counts*/ 0/*added*/, 0/*updated*/, 0/*skipped*/}               \
  }
/**
   Initialized-with-defaults fsl_ckout_manage_opt instance,
   intended for use in non-const copy initialization.
*/
FSL_EXPORT const fsl_ckout_manage_opt fsl_ckout_manage_opt_empty;

/**
   Adds the given filename or directory (recursively) to the current
   checkout vfile list of files as a to-be-added file, or updates an
   existing record if one exists.

   This function ensures that opt->filename gets canonicalized and can
   be found under the checkout directory, and fails if no such file
   exists (checking against the canonicalized name). Filenames are all
   filtered through fsl_reserved_fn_check() and may have other filters
   applied to them, as determined by the options object.

   Each filename which passes through the filters is passed to
   the opt->callback (if not NULL), which may perform a final
   filtering check and/or alert the client about the file being
   queued.

   The options object is non-const because this routine updates
   opt->counts when it adds, updates, or skips a file. On each call,
   it updated opt->counts without resetting it (as this function is
   typically called in a loop). This function does not modify any
   other entries of that object and it requires that the object not be
   modified (e.g. via opt->callback()) while it is recursively
   processing. To reset the counts between calls, if needed:

   ```
   opt->counts = fsl_ckout_manage_opt_empty.counts;
   ```

   Returns 0 on success, non-0 on error.

   Files queued for addition this way can be unqueued before they are
   committed using fsl_ckout_unmanage().

   To avoid Significant Grief, this routine automatically skips any
   directories under opt->filename which appear to be fossil checkout
   roots unless that directory is f's current checkout directory, into
   which it will recurse normally.

   @see fsl_ckout_unmanage()
   @see fsl_reserved_fn_check()
*/
FSL_EXPORT int fsl_ckout_manage( fsl_cx * const f,
                                 fsl_ckout_manage_opt * const opt );

/**
   Convenience typedef and required forward decl.
 */
typedef struct fsl_ckout_unmanage_opt fsl_ckout_unmanage_opt;

/**
   State passed to fsl_ckout_unmanage_f() callback.
*/
struct fsl_ckout_unmanage_state {
  /**
     The fsl_cx instance for the correspnding fsl_ckout_unmanage()
     call.
  */
  fsl_cx * f;
  /**
     The options object passed to the corresponding call to
     fsl_ckout_unmanage(). This is provided primarily for access to
     its callbackState member. The client must never modify any state
     in this opt object other than the callbackState or to use other
     APIs of this->f.
  */
  fsl_ckout_unmanage_opt const * opt;
  /**
     The name of the file being unmanaged, relative to the root of the
     checkout. fsl_cx_stat2() can be used to transform it to an
     absolute path, if needed. Given a fsl_ckout_unmanage_state
     named `ums`, that call would look like:

     ```
     fsl_buffer_reuse(&yourBuffer);
     // ^^^ noting that fsl_cx_stat2() APPENDS to the buffer.
     int rc = fsl_cx_stat2(ums->f, false, ums->filename,
                           NULL, &yourBuffer, true);
     // ^^^ noting that the stat will legitimately and harmlessly
     // fail if the file does not exist, so this error can
     // normally be ignored.
     ```
  */
  char const * filename;
};

/** Convenience typedef. */
typedef struct fsl_ckout_unmanage_state fsl_ckout_unmanage_state;


/**
   Callback type for use with fsl_ckout_unmanage(). It is called
   by the removal process, immediately after a file is "removed"
   from SCM management (a.k.a. when the file becomes "unmanaged").

   If it returns non-0 the unmanage process will end and that error
   code will be reported to its caller. Such result codes must come
   from the FSL_RC_xxx family. They may report details of the error
   via fsl_cx_err_set() with the `ums->f` object.

   The object passed to it will hold the name of the file being
   unmanaged, relative to the top-most checkout directory. The client
   is free to unlink the file from the filesystem if they like - the
   library does not do so automatically
*/
typedef int (*fsl_ckout_unmanage_f)(fsl_ckout_unmanage_state const * ums);

/**
   Options for use with fsl_ckout_unmanage().
*/
struct fsl_ckout_unmanage_opt {
  /**
     The file or directory name to add. If it is a directory, the add
     process will recurse into it. See also this->vfileIds.
  */
  char const * filename;
  /**
     An alternative to assigning this->filename is to point
     this->vfileIds to a bag of vfile.id values. If this member is not
     NULL, fsl_ckout_unmanage() will ignore this->filename.

     @see fsl_filename_to_vfile_ids()
  */
  fsl_id_bag const * vfileIds;
  /**
     Whether to evaluate this->filename as relative to the current
     working directory (true) or to the current checkout root
     (false). This is ignored when this->vfileIds is not NULL.

     This makes a subtle yet important difference in how the name is
     resolved. CLI apps which take file names from the user from
     within a checkout directory will generally want to set
     relativeToCwd to true. GUI apps, OTOH, will possibly need it to
     be false, depending on how they resolve and pass on the
     filenames.
  */
  bool relativeToCwd;
  /**
     If true, fsl_vfile_changes_scan() is called to ensure that
     the filesystem and vfile tables agree. If the client code has
     called that function, or its equivalent, since any changes were
     made to the checkout then this may be set to false to speed up
     the rm process.
  */
  bool scanForChanges;
  /**
     Optional predicate function which will be called after each
     file is made unmanaged.

     The name it is passed is relative to the checkout root.
  */
  fsl_ckout_unmanage_f callback;
  /**
     State to be passed to this->callback.
  */
  void * callbackState;
};
/**
   Initialized-with-defaults fsl_ckout_unmanage_opt instance,
   intended for use in const-copy initialization.
*/
#define fsl_ckout_unmanage_opt_empty_m {\
    NULL/*filename*/, NULL/*vfileIds*/,\
    true/*relativeToCwd*/,true/*scanForChanges*/, \
    NULL/*callback*/, NULL/*callbackState*/ \
}
/**
   Initialized-with-defaults fsl_ckout_unmanage_opt instance,
   intended for use in non-const copy initialization.
*/
FSL_EXPORT const fsl_ckout_unmanage_opt fsl_ckout_unmanage_opt_empty;

/**
   The converse of fsl_ckout_manage(), this queues a file for removal
   from the current checkout. Unlike fsl_ckout_manage(), this routine
   does not ensure that opt->filename actually exists - it only
   normalizes zFilename into its repository-friendly form and passes
   it through the vfile table.

   If opt->filename refers to a directory then this operation queues
   all files under that directory (recursively) for removal. In this
   case, it is irrelevant whether or not opt->filename ends in a
   trailing slash.

   Returns 0 on success, any of a number of non-0 codes on error.
   Returns FSL_RC_MISUSE if !opt->filename or !*opt->filename.
   Returns FSL_RC_NOT_A_CKOUT if f has no opened checkout.

   If opt->callback is not NULL, it is called for each
   newly-unamanaged entry. The intention is to provide it the
   opportunity to notify the user, record the filename for later use,
   remove the file from the filesystem, etc. If it returns non-0, the
   unmanaging process will fail with that code and any pending
   transaction will be placed into a rollback state.

   This routine does not actually remove any files from the
   filesystem, it only modifies the vfile table entry so that the
   file(s) will be removed from the SCM by the commit process. If
   opt->filename is an entry which was previously
   fsl_ckout_manage()'d, but not yet committed, or any such entries
   are found under directory opt->filename, they are removed from the
   vfile table. i.e. this effective undoes the add operation.

   @see fsl_ckout_manage()
*/
FSL_EXPORT int fsl_ckout_unmanage( fsl_cx * f,
                                  fsl_ckout_unmanage_opt const * opt );

/**
   Hard-coded range of values of the vfile.chnged db field.
   These values are part of the fossil schema and must not
   be modified.
*/
enum fsl_vfile_change_e {
  /** File is unchanged. */
  FSL_VFILE_CHANGE_NONE = 0,
  /** File edit. */
  FSL_VFILE_CHANGE_MOD = 1,
  /** File changed due to a merge. */
  FSL_VFILE_CHANGE_MERGE_MOD = 2,
  /** File added by a merge. */
  FSL_VFILE_CHANGE_MERGE_ADD = 3,
  /** File changed due to an integrate merge. */
  FSL_VFILE_CHANGE_INTEGRATE_MOD = 4,
  /** File added by an integrate merge. */
  FSL_VFILE_CHANGE_INTEGRATE_ADD = 5,
  /** File became executable but has unmodified contents. */
  FSL_VFILE_CHANGE_IS_EXEC = 6,
  /** File became a symlink whose target equals its old contents. */
  FSL_VFILE_CHANGE_BECAME_SYMLINK = 7,
  /** File lost executable status but has unmodified contents. */
  FSL_VFILE_CHANGE_NOT_EXEC = 8,
  /** File lost symlink status and has contents equal to its old target. */
  FSL_VFILE_CHANGE_NOT_SYMLINK = 9
};
typedef enum fsl_vfile_change_e fsl_vfile_change_e;

/**
   Change-type flags for use with fsl_ckout_changes_visit() and
   friends.

   TODO: consolidate this with fsl_vfile_change_e insofar as possible.
   There are a few checkout change statuses not reflected in
   fsl_vfile_change_e.
*/
enum fsl_ckout_change_e {
/**
   Sentinel placeholder value.
*/
FSL_CKOUT_CHANGE_NONE = 0,
/**
   Indicates that a file was modified in some unspecified way.
*/
FSL_CKOUT_CHANGE_MOD = FSL_VFILE_CHANGE_MOD,
/**
   Indicates that a file was modified as the result of a merge.
*/
FSL_CKOUT_CHANGE_MERGE_MOD = FSL_VFILE_CHANGE_MERGE_MOD,
/**
   Indicates that a file was added as the result of a merge.
*/
FSL_CKOUT_CHANGE_MERGE_ADD = FSL_VFILE_CHANGE_MERGE_ADD,
/**
   Indicates that a file was modified as the result of an
   integrate-merge.
*/
FSL_CKOUT_CHANGE_INTEGRATE_MOD = FSL_VFILE_CHANGE_INTEGRATE_MOD,
/**
   Indicates that a file was added as the result of an
   integrate-merge.
*/
FSL_CKOUT_CHANGE_INTEGRATE_ADD = FSL_VFILE_CHANGE_INTEGRATE_ADD,
/**
   Indicates that the file gained the is-executable trait
   but is otherwise unmodified.
*/
FSL_CKOUT_CHANGE_IS_EXEC = FSL_VFILE_CHANGE_IS_EXEC,
/**
   Indicates that the file has changed to a symlink.
*/
FSL_CKOUT_CHANGE_BECAME_SYMLINK = FSL_VFILE_CHANGE_BECAME_SYMLINK,
/**
   Indicates that the file lost the is-executable trait
   but is otherwise unmodified.
*/
FSL_CKOUT_CHANGE_NOT_EXEC = FSL_VFILE_CHANGE_NOT_EXEC,
/**
   Indicates that the file was previously a symlink but is
   now a plain file.
*/
FSL_CKOUT_CHANGE_NOT_SYMLINK = FSL_VFILE_CHANGE_NOT_SYMLINK,
/**
   Indicates that a file was added.
*/
FSL_CKOUT_CHANGE_ADDED = FSL_CKOUT_CHANGE_NOT_SYMLINK + 1000,
/**
   Indicates that a file was removed from SCM management.
*/
FSL_CKOUT_CHANGE_REMOVED,
/**
   Indicates that a file is missing from the local checkout.
*/
FSL_CKOUT_CHANGE_MISSING,
/**
   Indicates that a file was renamed.
*/
FSL_CKOUT_CHANGE_RENAMED
};

typedef enum fsl_ckout_change_e fsl_ckout_change_e;

/**
   This is equivalent to calling fsl_vfile_changes_scan() with the
   arguments (f, -1, 0).

   @see fsl_ckout_changes_visit()
   @see fsl_vfile_changes_scan()
*/
FSL_EXPORT int fsl_ckout_changes_scan(fsl_cx * f);

/**
   A typedef for visitors of checkout status information via
   fsl_ckout_changes_visit(). Implementions will receive the
   last argument passed to fsl_ckout_changes_visit() as their
   first argument. The second argument indicates the type of change
   and the third holds the repository-relative name of the file.

   If changes is FSL_CKOUT_CHANGE_RENAMED then origName will hold
   the original name, else it will be NULL.

   Implementations must return 0 on success, non-zero on error. On
   error any looping performed by fsl_ckout_changes_visit() will
   stop and this function's result code will be returned.

   @see fsl_ckout_changes_visit()
*/
typedef int (*fsl_ckout_changes_f)(void * state, fsl_ckout_change_e change,
                                      char const * filename,
                                      char const * origName);

/**
   Compares the changes of f's local checkout against repository
   version vid (checkout version if vid is negative). For each
   change detected it calls visitor(state,...) to report the
   change.  If visitor() returns non-0, that code is returned from
   this function. If doChangeScan is true then
   fsl_ckout_changes_scan() is called by this function before
   iterating, otherwise it is assumed that the caller has called
   that or has otherwise ensured that the checkout db's vfile table
   has been populated.

   If the callback returns FSL_RC_BREAK, this function stops iteration
   and returns 0.

   Returns 0 on success.

   @see fsl_ckout_changes_scan()
*/
FSL_EXPORT int fsl_ckout_changes_visit( fsl_cx * f, fsl_id_t vid,
                                           bool doChangeScan,
                                           fsl_ckout_changes_f visitor,
                                           void * state );
/**
   A bitmask of flags for fsl_vfile_changes_scan().
*/
enum fsl_ckout_sig_e {
/**
   The empty flags set.
*/
FSL_VFILE_CKSIG_NONE = 0,

/**
   Non-file/non-link FS objects trigger an error.
*/
FSL_VFILE_CKSIG_ENOTFILE = 0x001,
/**
   Verify file content using hashing, regardless of whether or not
   file timestamps differ.
*/
FSL_VFILE_CKSIG_HASH = 0x002,
/**
   For unchanged or changed-by-merge files, set the mtime to last
   check-out time, as determined by fsl_mtime_of_manifest_file().
*/
FSL_VFILE_CKSIG_SETMTIME = 0x004,
/**
   Indicates that when populating the vfile table, it should be not be
   cleared of entries for other checkins. Normally we want to clear
   all versions except for the one we're working with, but at least
   a couple of use cases call for having multiple versions in vfile at
   once. Many algorithms generally assume only a single checkin's
   worth of state is in vfile and can get confused if that is not the
   case.
*/
FSL_VFILE_CKSIG_KEEP_OTHERS = 0x008,
/**
   If set and fsl_vfile_changes_scan() is passed a version other than
   the pre-call checkout version, it will, when finished, write the
   given version in the "checkout" setting of the ckout.vvar table,
   effectively switching the checkout to that version. It does not do
   this by default because it is sometimes necessary to have two
   versions in the vfile table at once and the operation doing so
   needs to control which version number is the current checkout.
*/
FSL_VFILE_CKSIG_WRITE_CKOUT_VERSION = 0x010
};

/**
    This function populates (if needed) the vfile table of f's
    checkout db for the given checkin version ID then compares files
    listed in it against files in the checkout directory, updating
    vfile's status for the current checkout version id as its goes. If
    vid is<=0 then the current checkout's RID is used in its place
    (note that 0 is the RID of an initial empty repository!).

    cksigFlags must be 0 or a bitmask of fsl_ckout_sig_e values.

    This is a relatively memory- and filesystem-intensive operation,
    and should not be performed more often than necessary. Many SCM
    algorithms rely on its state being correct, however, so it's
    generally better to err on the side of running it once too often
    rather than once too few times.

    Returns 0 on success, non-0 on error.

    BUG: this does not properly catch one particular corner-case
    change, where a file has been replaced by a same-named non-file
    (symlink or directory).
*/
FSL_EXPORT int fsl_vfile_changes_scan(fsl_cx * const f, fsl_id_t vid,
                                      unsigned cksigFlags);

/**
   If f has an opened checkout which has local changes noted in its
   checkout db state (the vfile table), returns true, else returns
   false. Note that this function does not do the filesystem scan to
   check for changes, but checks only the db state. Use
   fsl_vfile_changes_scan() to perform the actual scan (noting that
   library-side APIs which update that state may also record
   individual changes or automatically run a scan).
*/
FSL_EXPORT bool fsl_ckout_has_changes(fsl_cx * const f);

/**
   Callback type for use with fsl_checkin_queue_opt for alerting a
   client about exactly which files get enqueued/dequeued via
   fsl_checkin_enqueue() and fsl_checkin_dequeue().

   This function gets passed the checkout-relative name of the file
   being enqueued/dequeued and the client-provided state pointer which
   was passed to the relevant API. It must return 0 on success. If it
   returns non-0, the API on whose behalf this callback is invoked
   will propagate that error code back to the caller.

   The intent of this callback is simply to report changes to the
   client, not to perform validation. Thus such callbacks "really
   should not fail" unless, e.g., they encounter an OOM condition or
   some such. Any validation required by the client should be
   performed before calling fsl_checkin_enqueue()
   resp. fsl_checkin_dequeue().
*/
typedef int (*fsl_checkin_queue_f)(const char * filename, void * state);

/**
   Options object type used by fsl_checkin_enqueue() and
   fsl_checkin_dequeue().
*/
struct fsl_checkin_queue_opt {
  /**
     File or directory name to enqueue/dequeue to/from a pending
     checkin.
  */
  char const * filename;
  /**
     If true, filename (if not absolute) is interpreted as relative to
     the current working directory, else it is assumed to be relative
     to the top of the current checkout directory.
  */
  bool relativeToCwd;

  /**
     If not NULL then this->filename and this->relativeToCwd are
     IGNORED and any to-queue filename(s) is/are added from this
     container. It is an error (FSL_RC_MISUSE) to pass an empty bag.
     (Should that be FSL_RC_RANGE instead?)

     The bag is assumed to contain values from the vfile.id checkout
     db field, refering to one or more files which should be queued
     for the pending checkin. It is okay to pass IDs for unmodified
     files or to queue the same files multiple times. Unmodified files
     may be enqueued but will be ignored by the checkin process if, at
     the time the checkin is processed, they are still unmodified.
     Duplicated entries are simply ignored for the 2nd and subsequent
     inclusion.

     @see fsl_ckout_vfile_ids()
  */
  fsl_id_bag const * vfileIds;

  /**
     If true, fsl_vfile_changes_scan() is called to ensure that the
     filesystem and vfile tables agree. If the client code has called
     that function, or its equivalent, since any changes were made to
     the checkout then this may be set to false to speed up the
     enqueue process. This is only used by fsl_checkin_enqueue(), not
     fsl_checkin_dequeue().
  */
  bool scanForChanges;

  /**
     If true, only flagged-as-modified files will be enqueued by
     fsl_checkin_enqueue(). By and large, this should be set to
     true. Setting this to false is generally only intended/useful for
     testing.
  */
  bool onlyModifiedFiles;

  /**
     It not NULL, is pass passed the checkout-relative filename of
     each enqueued/dequeued file and this->callbackState. See the
     callback type's docs for more details.
  */
  fsl_checkin_queue_f callback;

  /**
     Opaque client-side state for use as the 2nd argument to
     this->callback.
  */
  void * callbackState;

  /* TODO (2023-02-02): any state necessary for indicating that this
     checking should be flagged as "private".
   */
};

/** Convenience typedef. */
typedef struct fsl_checkin_queue_opt fsl_checkin_queue_opt;

/** Initialized-with-defaults fsl_checkin_queue_opt structure, intended for
    const-copy initialization. */
#define fsl_checkin_queue_opt_empty_m { \
  NULL/*filename*/,true/*relativeToCwd*/,    \
  NULL/*vfileIds*/,                                 \
  true/*scanForChanges*/,true/*onlyModifiedFiles*/,   \
  NULL/*callback*/,NULL/*callbackState*/      \
}

/** Initialized-with-defaults fsl_checkin_queue_opt structure, intended for
    non-const copy initialization. */
extern const fsl_checkin_queue_opt fsl_checkin_queue_opt_empty;

/**
   Adds one or more files to f's list of "selected" files - those
   which should be included in the next commit (see
   fsl_checkin_commit()).

   Warning: if this function is not called before
   fsl_checkin_commit(), then fsl_checkin_commit() will select all
   modified, fsl_ckout_manage()'d, fsl_ckout_unmanage()'d, or renamed
   files by default.

   opt->filename must be a non-empty NUL-terminated string. The
   filename is canonicalized via fsl_ckout_filename_check() - see that
   function for the meaning of the opt->relativeToCwd parameter. To
   queue all modified files in a checkout, set opt->filename to ".",
   opt->relativeToCwd to false, and opt->onlyModifiedFiles to true.
   "Modified" includes any which are pending deletion, are
   newly-added, or for which a rename is pending.

   The resolved name must refer to either a single vfile.pathname
   value in the current vfile table or to a checkout-root-relative
   directory. All matching filenames which refer to modified files (as
   recorded in the vfile table) are queued up for the next commit.
   If opt->filename is NULL, empty, or ("." and opt->relativeToCwd is false)
   then all files in the vfile table are checked for changes.

   If opt->scanForChanges is true then fsl_vfile_changes_scan() is
   called before starting to ensure that the vfile entries are up to
   date. If the client app has "recently" run that (or its
   equivalent), that (slow) step can be skipped by setting
   opt->scanForChanges to false before calling this

   Note that after this returns, any given file may still be modified
   by the client before the commit takes place, and the changes on
   disk at the point of the fsl_checkin_commit() are the ones which
   get saved (or not).

   For each resolved entry which actually gets enqueued (i.e. was not
   already enqueued and which is marked as modified), opt->callback
   (if it is not NULL) is passed the checkout-relative file name and
   the opt->callbackState pointer.

   Returns 0 on success, FSL_RC_MISUSE if either pointer is NULL, or
   *zName is NUL. Returns FSL_RC_OOM on allocation error. It is not
   inherently an error for opt->filename to resolve to no queue-able
   entries. A client can check for that case, if needed, by assigning
   opt->callback and incrementing a counter in that callback. If the
   callback is never called, no queue-able entries were found.

   On error f's error state might (depending on the nature of the
   problem) contain more details.

   @see fsl_checkin_is_enqueued()
   @see fsl_checkin_dequeue()
   @see fsl_checkin_discard()
   @see fsl_checkin_commit()
*/
FSL_EXPORT int fsl_checkin_enqueue(fsl_cx * f,
                                   fsl_checkin_queue_opt const * opt);

/**
   The opposite of fsl_checkin_enqueue(), this opt->filename,
   which may resolve to a single name or a directory, from the checkin
   queue. Returns 0 on succes. This function does no validation on
   whether a given file(s) actually exist(s), it simply asks the
   internals to clean up matching strings from the checkout's vfile
   table. Specifically, it does not return an error if this operation
   finds no entries to dequeue.

   If opt->filename is empty or NULL then ALL files are unqueued from
   the pending checkin.

   If opt->relativeToCwd is true (non-0) then opt->filename is
   resolved based on the current directory, otherwise it is resolved
   based on the checkout's root directory.

   If opt->filename is not NULL or empty, this functions runs the
   given path through fsl_ckout_filename_check() and will fail if that
   function fails, propagating any error from that function. Ergo,
   opt->filename must refer to a path within the current checkout.

   @see fsl_checkin_enqueue()
   @see fsl_checkin_is_enqueued()
   @see fsl_checkin_discard()
   @see fsl_checkin_commit()
*/
FSL_EXPORT int fsl_checkin_dequeue(fsl_cx * const f,
                                   fsl_checkin_queue_opt const * opt);

/**
   Returns true if the file named by zName is in f's current file
   checkin queue. If NO files are in the current selection queue then
   this routine assumes that ALL files are implicitely selected. As
   long as at least one file is enqueued (via fsl_checkin_enqueue())
   then this function only returns true for files which have been
   explicitly enqueued.

   If relativeToCwd then zName is resolved based on the current
   directory, otherwise it assumed to be related to the checkout's
   root directory.

   This function returning true does not necessarily indicate that
   the file _will_ be checked in at the next commit. If the file has
   not been modified at commit-time then it will not be part of the
   commit.

   This function honors the fsl_cx_is_case_sensitive() setting
   when comparing names.

   Achtung: this does not resolve directory names like
   fsl_checkin_enqueue() and fsl_checkin_dequeue() do. It
   only works with file names.

   Results are undefined if f is NULL.

   @see fsl_checkin_enqueue()
   @see fsl_checkin_dequeue()
   @see fsl_checkin_discard()
   @see fsl_checkin_commit()
*/
FSL_EXPORT bool fsl_checkin_is_enqueued(fsl_cx * const f,
                                        char const * zName,
                                        bool relativeToCwd);

/**
   Discards any state accumulated for a pending checking,
   including any files queued via fsl_checkin_enqueue()
   and tags added via fsl_checkin_T_add().

   @see fsl_checkin_enqueue()
   @see fsl_checkin_dequeue()
   @see fsl_checkin_is_enqueued()
   @see fsl_checkin_commit()
   @see fsl_checkin_T_add()
*/
FSL_EXPORT void fsl_checkin_discard(fsl_cx * const f);

/**
   Parameters for fsl_checkin_commit().

   Checkins are created in a multi-step process:

   - fsl_checkin_enqueue() queues up a file or directory for
   commit at the next commit.

   - fsl_checkin_dequeue() removes an entry, allowing
   UIs to toggle files in and out of a checkin before
   committing it.

   - fsl_checkin_is_enqueued() can be used to determine whether
   a given name is already enqueued or not.

   - fsl_checkin_T_add() can be used to T-cards (tags) to a
   deck. Branch tags are intended to be applied via the
   fsl_checkin_opt::branch member.

   - fsl_checkin_discard() can be used to cancel any pending file
   enqueuings, effectively cancelling a commit (which can be
   re-started by enqueuing another file).

   - fsl_checkin_commit() creates a checkin for the list of enqueued
   files (defaulting to all modified files in the checkout!). It
   takes an object of this type to specify a variety of parameters
   for the check.

   Note that this API uses the terms "enqueue" and "unqueue" rather
   than "add" and "remove" because those both have very specific
   (and much different) meanings in the overall SCM scheme.
*/
struct fsl_checkin_opt {
  /**
     The commit message. May not be empty - the library
     forbids empty checkin messages.
  */
  char const * message;

  /**
     The optional mime type for the message. Only set
     this if you know what you're doing.
  */
  char const * messageMimeType;

  /**
     The user name for the checkin. If NULL or empty, it defaults to
     fsl_cx_user_get(). If that is NULL, a FSL_RC_RANGE error is
     triggered.
  */
  char const * user;

  /**
     If not NULL, makes the checkin the start of a new branch with
     this name.
  */
  char const * branch;

  /**
     If this->branch is not NULL, this is applied as its "bgcolor"
     propagating property. If this->branch is NULL then this is
     applied as a one-time color tag to the checkin.

     It must be NULL, empty, or in a form usable by HTML/CSS,
     preferably \#RRGGBB form. Length-0 values are ignored (as if
     they were NULL).
  */
  char const * bgColor;

  /**
     If true, the checkin will be marked as private, otherwise it
     will be marked as private or public, depending on whether or
     not it inherits private content.
  */
  bool isPrivate;

  /**
     Whether or not to calculate an R-card. Doing so is very
     expensive (memory and I/O) but it adds another layer of
     consistency checking to manifest files. In practice, the R-card
     is somewhat superfluous and the cost of calculating it has
     proven painful on very large repositories. fossil(1) creates an
     R-card for all checkins but does not require that one be set
     when it reads a manifest.
  */
  bool calcRCard;

  /**
     Tells the checkin to close merged-in branches (merge type of
     0). INTEGRATE merges (type=-4) are always closed by a
     checkin. This does not apply to CHERRYPICK (type=-1) and
     BACKOUT (type=-2) merges.
  */
  bool integrate;

  /**
     If true, allow a file to be checked in if it contains
     fossil-style merge conflict markers, else fail if an attempt is
     made to commit any files with such markers.
  */
  bool allowMergeConflict;

  /**
     A hint to fsl_checkin_commit() about whether it needs to scan the
     checkout for changes. Set this to false ONLY if the calling code
     calls fsl_ckout_changes_scan() (or equivalent,
     e.g. fsl_vfile_changes_scan()) immediately before calling
     fsl_checkin_commit(). fsl_checkin_commit() requires a non-stale
     changes scan in order to function properly, but it's a
     computationally slow operation so the checkin process does not
     want to duplicate it if the application has recently done so.
  */
  bool scanForChanges;

  /**
     NOT YET IMPLEMENTED! TODO!

     If true, files which are removed from the SCM by this checkin
     should be removed from the filesystem.

     Reminder to self: when we do this, incorporate
     fsl_rm_empty_dirs().
  */
  bool rmRemovedFiles;

  /**
     If true, the checkin's db savepoint is rolled back after it's
     complete, effectively meaning that the checkin does not take
     place (but all of the work involved happens). This unfortunately
     has to put any pending transactions into pending-rollback mode,
     too, because of $REASONS.
  */
  bool dryRun;

  /**
     Whether to allow (or try to force) a delta manifest or not. 0
     means no deltas allowed - it will generate a baseline
     manifest. Greater than 0 forces generation of a delta if
     possible (if one can be readily found) even if doing so would not
     save a notable amount of space. Less than 0 means to
     decide via some heuristics.

     A "readily available" baseline means either the current checkout
     is a baseline or has a baseline. In either case, we can use that
     as a baseline for a delta. i.e. a baseline "should" generally be
     available except on the initial checkin, which has neither a
     parent checkin nor a baseline.

     The current behaviour for "auto-detect" mode is: it will generate
     a delta if a baseline is "readily available" _and_ the repository
     has at least one delta already. Once it calculates a delta form,
     it calculates whether that form saves any appreciable
     space/overhead compared to whether a baseline manifest was
     generated. If so, it discards the delta and re-generates the
     manifest as a baseline. The "force" behaviour (deltaPolicy>0)
     bypasses the "is it too big?" test, and is only intended for
     testing, not real-life use.

     Caveat: if the repository has the "forbid-delta-manifests" set to
     a true value, this option is ignored: that setting takes
     priority. Similarly, it will not create a delta in a repository
     unless a delta has been "seen" in that repository before or this
     policy is set to >0. When a checkin is created with a delta
     manifest, that fact gets recorded in the repository's config
     table.

     Note that delta manifests have some advantages and may not
     actually save much (if any) repository space because the
     lower-level delta framework already compresses parent versions of
     artifacts tightly. For more information see:

     https://fossil-scm.org/home/doc/tip/www/delta-manifests.md
  */
  int deltaPolicy;

  /**
     Time of the checkin. If 0 or less, the time of the
     fsl_checkin_commit() call is used.
  */
  double julianTime;

  /**
     If this is not NULL then the committed manifest will include a
     tag which closes the branch. The value of this string will be
     the value of the "closed" tag, and the value may be an empty
     string. The intention is that this gets set to a comment about
     why the branch is closed, but it is in no way mandatory.
  */
  char const * closeBranch;

  /**
     Tells fsl_checkin_commit() to dump the generated manifest to
     this file. Intended only for debugging and testing. Checking in
     will fail if this file cannot be opened for writing.
  */
  char const * dumpManifestFile;
  /*
    fossil(1) has many more options. We might want to wrap some of
    it up in the "incremental" state (f->ckin.mf).

    TODOs:

    A callback mechanism which supports the user cancelling
    the checkin. It is (potentially) needed for ops like
    confirming the commit of CRNL-only changes.

    2021-03-09: we now have fsl_confirmer for this but currently no
    part of the checkin code needs a prompt.
  */
};

/**
   Empty-initialized fsl_checkin_opt instance, intended for use in
   const-copy constructing.
*/
#define fsl_checkin_opt_empty_m {               \
  .message = NULL,                            \
  .messageMimeType = NULL,                  \
  .user = NULL,                             \
  .branch = NULL,                           \
  .bgColor = NULL,                          \
  .isPrivate = false,                           \
  .calcRCard = true,                           \
  .integrate = false,                           \
  .allowMergeConflict = false,\
  .scanForChanges = true,\
  .rmRemovedFiles = false,\
  .dryRun = false, \
  .deltaPolicy = 0,                        \
  .julianTime = 0.0,                        \
  .closeBranch = NULL,                      \
  .dumpManifestFile = NULL               \
}

/**
   Empty-initialized fsl_checkin_opt instance, intended for use in
   copy-constructing. It is important that clients copy this value
   (or fsl_checkin_opt_empty_m) to cleanly initialize their
   fsl_checkin_opt instances, as this may set default values which
   (e.g.) a memset() would not.
*/
FSL_EXPORT const fsl_checkin_opt fsl_checkin_opt_empty;

/**
   This creates and saves a "checkin manifest" for the current
   checkout.

   Its primary inputs is a list of files to commit. This list is
   provided by the client by calling fsl_checkin_enqueue() one or
   more times.  If no files are explicitely selected (enqueued) then
   it calculates which local files have changed vs the current
   checkout and selects all of those.

   Non-file inputs are provided via the opt parameter.

   On success, it returns 0 and...

   - If newRid is not NULL, it is assigned the new checkin's RID
   value.

   - If newUuid is not NULL, it is assigned the new checkin's UUID
   value. Ownership of the bytes is passed to the caller, who must
   eventually pass them to fsl_free() to free them.

   Note that the new RID and UUID can also be fetched afterwards by
   calling fsl_ckout_version_info().

   On error non-0 is returned and f's error state may (depending on
   the nature of the problem) contain details about the problem.
   Note, however, that any error codes returned here may have arrived
   from several layers down in the internals, and may not have a
   single specific interpretation here. When possible/practical, f's
   error state gets updated with a human-readable description of the
   problem.

   opt->dryRun: does not work properly, at least partially due to
   stale internal caches.

   ACHTUNG: all pending checkin state is cleared by this function
   unless it fails basic argument validation. This means any queued
   files or tags need to be re-applied if the client wants to try
   again. That is somewhat of a bummer, but this behaviour is the only
   way we can ensure that then the pending checkin state does not get
   garbled on a second use. When in doubt about the state, the client
   should call fsl_checkin_discard() to clear it before try to
   re-commit. (Potential TODO: add a success/fail state flag to the
   checkin state and only clean up on success? OTOH, since something
   in the state likely caused the problem, we might not want to do
   that.)

   Some of the more notable, potentially not obvious, error
   conditions:

   - Trying to commit against a closed leaf: FSL_RC_ACCESS. Doing so
   is not permitted by fossil(1), so we disallow it here.

   - An empty/NULL user name or commit message, or no files were
   selected which actually changed: FSL_RC_MISSING_INFO. In these
   cases f's error state describes the problem.

   - Some resource is not found (e.g. an expected RID/UUID could not
   be resolved): FSL_RC_NOT_FOUND. This would generally indicate
   some sort of data consistency problem. i.e. it's quite possibly
   very bad if this is returned.

   - If the checkin would result in no file-level changes vis-a-vis
   the current checkout, FSL_RC_NOOP is returned.

   BUGS:

   - It cannot currently properly distinguish a "no-op" commit, one in
   which no files were modified or only their permissions were
   modifed.

   TODO:

   - Honor the opt->dryRun flag. Getting this working has proven tricky
     because of stale cache issues.

   @see fsl_checkin_enqueue()
   @see fsl_checkin_dequeue()
   @see fsl_checkin_discard()
   @see fsl_checkin_T_add()
*/
FSL_EXPORT int fsl_checkin_commit(fsl_cx * f, fsl_checkin_opt const * opt,
                                  fsl_id_t * newRid, fsl_uuid_str * newUuid);

/**
   Works like fsl_deck_T_add(), adding the given tag information to
   the pending checkin state. Returns 0 on success, non-0 on error. A
   checkin may, in principal, have any number of tags, and this may be
   called any number of times to add new tags to the pending
   commit. This list of tags gets cleared by a successful
   fsl_checkin_commit() or by fsl_checkin_discard(). Decks require
   that each tag be distinct from each other (none may compare
   equivalent), but that check is delayed until the deck is output
   into its final artifact form.

   @see fsl_checkin_enqueue()
   @see fsl_checkin_dequeue()
   @see fsl_checkin_commit()
   @see fsl_checkin_discard()
   @see fsl_checkin_T_add2()
*/
FSL_EXPORT int fsl_checkin_T_add( fsl_cx * f, fsl_tagtype_e tagType,
                                   fsl_uuid_cstr uuid, char const * name,
                                   char const * value);

/**
   Works identically to fsl_checkin_T_add() except that it takes its
   argument in the form of a T-card object.

   On success ownership of t is passed to mf. On error (see
   fsl_deck_T_add()) ownership is not modified.

   Results are undefined if either argument is NULL or improperly
   initialized.
*/
FSL_EXPORT int fsl_checkin_T_add2( fsl_cx * f, fsl_card_T * t );

/**
   Clears all contents from f's checkout database, including the vfile
   table, vmerge table, and some of the vvar table. The tables are
   left intact. Returns 0 on success, non-0 if f has no checkout or for
   a database error.
*/
FSL_EXPORT int fsl_ckout_clear_db(fsl_cx *f);

/**
   Returns the base name of the current platform's checkout database
   file. That is "_FOSSIL_" on Windows and ".fslckout" everywhere
   else. The returned bytes are static.

   TODO: an API which takes a dir name and looks for either name
*/
FSL_EXPORT char const *fsl_preferred_ckout_db_name(void);

/**
   File-overwrite policy values for use with fsl_ckup_opt and friends.
*/
enum fsl_file_overwrite_policy_e {
/** Indicates that an error should be triggered if a file would be
    overwritten. */
FSL_OVERWRITE_ERROR = 0,
/**
   Indicates that files should always be overwritten.
*/
FSL_OVERWRITE_ALWAYS,
/**
   Indicates that files should never be overwritten, and silently
   skipped over. This is almost never what one wants to do.
*/
FSL_OVERWRITE_NEVER
};
typedef enum fsl_file_overwrite_policy_e fsl_file_overwrite_policy_e;

/**
   State values for use with fsl_ckup_state::fileRmInfo.
*/
enum fsl_ckup_rm_state_e {
/**
   Indicates that the file was not removed in a given checkout.
   Guaranteed to have the value 0 so that it is treated as boolean
   false. No other entries in this enum have well-defined values.
*/
FSL_CKUP_RM_NOT = 0,
/**
   Indicates that a file was removed from a checkout but kept
   in the filesystem because it was locally modified.
*/
FSL_CKUP_RM_KEPT,
/**
   Indicates that a file was removed from a checkout and the
   filesystem, with the caveat that failed attempts to remove from the
   filesystem are ignored for Reasons but will be reported as if the
   unlink worked.
*/
FSL_CKUP_RM
};
typedef enum fsl_ckup_rm_state_e fsl_ckup_rm_state_e;

/**
  Under construction. Work in progress...

  Options for "opening" a fossil repository database. That is,
  creating a new fossil checkout database and populating its schema,
  _without_ checking out any files. (That latter part is up for
  reconsideration and this API might change in the future to check
  out files after creating/opening the db.)
*/
struct fsl_repo_open_ckout_opt {
  /**
     Name of the target directory, which must already exist. May be
     relative, e.g. ".". The repo-open operation will chdir to this
     directory for the duration of the operation. May be NULL, in
     which case the current directory is assumed and no chdir is
     performed.
  */
  char const * targetDir;

  /**
     The filename, with no directory components, of the desired
     checkout db name. For the time being, always leave this NULL and
     let the library decide. It "might" (but probably won't) be
     interesting at some point to allow the client to specify a
     different name (noting that that would be directly incompatible
     with fossil(1)).
  */
  char const * ckoutDbFile;

  /**
     Policy for how to handle overwrites of files extracted from a
     newly-opened checkout.

     Potential TODO: replace this with a fsl_confirmer, though that
     currently seems like overkill for this particular case.
  */
  fsl_file_overwrite_policy_e fileOverwritePolicy;

  /**
     fsl_repo_open_ckout() installs the fossil checkout schema. If
     this is true it will forcibly replace any existing relevant
     schema components in the checkout db, otherwise it will fail when
     it tries to overwrite an existing schema and cannot.
  */
  bool dbOverwritePolicy;

  /**
     If true, the checkout-open process will look for an opened
     checkout in the target directory and its parents (recursively)
     and fail with FSL_RC_ALREADY_EXISTS if one is found.
  */
  bool checkForOpenedCkout;
};
typedef struct fsl_repo_open_ckout_opt fsl_repo_open_ckout_opt;

/**
  Empty-initialized fsl_repo_open_ckout_opt const-copy constructer.
*/
#define fsl_repo_open_ckout_opt_m { \
  NULL/*targetDir*/, NULL/*ckoutDbFile*/, \
  FSL_OVERWRITE_ERROR/*fileOverwritePolicy*/, \
  false/*dbOverwritePolicy*/,               \
  -1/*checkForOpenedCkout*/              \
}

/**
  Empty-initialised fsl_repo_open_ckout_opt instance. Clients should copy
  this value (or fsl_repo_open_ckout_opt_empty_m) to initialise
  fsl_repo_open_ckout_opt instances for sane default values.
*/
FSL_EXPORT const fsl_repo_open_ckout_opt fsl_repo_open_ckout_opt_empty;

/**
   Opens a checkout db for use with the currently-connected repository
   or creates a new checkout db. If opening an existing one, it gets
   "stolen" from any repository it might have been previously mapped
   to.

   - Requires that f have an opened repository db and no opened
     checkout. Returns FSL_RC_NOT_A_REPO if no repo is opened and
     FSL_RC_MISUSE if a checkout *is* opened.

   - Creates/re-uses a .fslckout DB in the dir opt->targetDir. The
     directory must be NULL or already exist, else FSL_RC_NOT_FOUND is
     returned. If opt->dbOverwritePolicy is false then it fails with
     FSL_RC_ALREADY_EXISTS if that directory already contains a
     checkout db.

   Note that this does not extract any SCM'd files from the
   repository, it only opens (and possibly creates) the checkout
   database.

   Pending:

   - If opening an existing checkout db for a different repo then
   delete the STASH and UNDO entries, as they're not valid for a
   different repo.
*/
FSL_EXPORT int fsl_repo_open_ckout( fsl_cx * const f,
                                    fsl_repo_open_ckout_opt const * opt );

typedef struct fsl_ckup_state fsl_ckup_state;
/**
   A callback type for use with fsl_ckup_state.  It gets called via
   fsl_repo_ckout() and fsl_ckout_update() to report progress of the
   extraction process. It gets called after one of those functions has
   successfully extracted a file or skipped over it because the file
   existed and the checkout options specified to leave existing files
   in place. It must return 0 on success, and non-0 will end the
   extraction process, propagating that result code back to the
   caller. If this callback fails, the checkout's contents may be left
   in an undefined state, with some files updated and others not.  All
   database-side data will be consistent (the transaction is rolled
   back) but filesystem-side changes may not be.
*/
typedef int (*fsl_ckup_f)( fsl_ckup_state const * cState );

/**
   This enum lists the various types of individual file change states
   which can happen during a checkout, update, or merge.
*/
enum fsl_ckup_fchange_e {
/** Sentinel value. */
FSL_CKUP_FCHANGE_INVALID = -1,
/** Was unchanged between the previous and updated-to version,
    so no change was made to the on-disk file. This is the
    only entry in the enum which is guaranteed to have a specific
    value: 0, so that it can be used as a boolean false. */
FSL_CKUP_FCHANGE_NONE = 0,
/**
   Added to SCM in the updated-to version.
*/
FSL_CKUP_FCHANGE_ADDED,
/**
   Added to SCM in the current checkout version and carried over into
   the updated-to version.
*/
FSL_CKUP_FCHANGE_ADD_PROPAGATED,
/**
   Removed from SCM in the updated-to to version OR in the checked-out
   version but not yet committed. a.k.a. it became "unmanaged."

   Do we need to differentiate between those cases?
*/
FSL_CKUP_FCHANGE_RM,
/**
   Removed from the checked-out version but not yet commited,
   so was carried over to the updated-to version.
*/
FSL_CKUP_FCHANGE_RM_PROPAGATED,
/** Updated or replaced without a merge by the checkout/update
    process. */
FSL_CKUP_FCHANGE_UPDATED,
/** Merge was not performed because at least one of the inputs appears
    to be binary. The updated-to version overwrites the previous
    version in this case.
*/
FSL_CKUP_FCHANGE_UPDATED_BINARY,
/** Updated with a merge by the update process. */
FSL_CKUP_FCHANGE_MERGED,
/** Special case of FSL_CKUP_FCHANGE_MERGED. Merge was performed
    and conflicts were detected. The newly-updated file will contain
    conflict markers.

    @see fsl_buffer_contains_merge_marker()
*/
FSL_CKUP_FCHANGE_CONFLICT_MERGED,
/** Added in the current checkout but also contained in the
    updated-to version. The local copy takes precedence.
*/
FSL_CKUP_FCHANGE_CONFLICT_ADDED,
/**
   Added by the updated-to version but a local unmanaged copy exists.
   The local copy is overwritten, per historical fossil(1) convention
   (noting that fossil has undo support to allow one to avoid loss of
   such a file's contents).

   TODO: use confirmer here to ask user whether to overwrite.
*/
FSL_CKUP_FCHANGE_CONFLICT_ADDED_UNMANAGED,
/** Edited locally but removed from updated-to version. Local
    edits will be left in the checkout tree. */
FSL_CKUP_FCHANGE_CONFLICT_RM,
/** Cannot merge if one or both of the update/updating verions of a
    file is a symlink. The updated-to version overwrites the previous
    version in this case.

    We probably need a better name for this.
*/
FSL_CKUP_FCHANGE_CONFLICT_SYMLINK,
/**
   Indicates that a merge of binary content was requested.

   TODO: figure out why UPDATE uses the target version, instead of
   triggering an error here, and why MERGE does not do the same.
   fossil(1) simply skips over, with a warning, binaries during a
   merge.
*/
FSL_CKUP_FCHANGE_CONFLICT_BINARY,
/** File was renamed in the updated-to version. If a file is both
    modified and renamed, it is flagged as renamed instead
    of modified. */
FSL_CKUP_FCHANGE_RENAMED,
/** Locally modified. This state appears only when
    "updating" a checkout to the same version. */
FSL_CKUP_FCHANGE_EDITED
};
typedef enum fsl_ckup_fchange_e fsl_ckup_fchange_e;

/**
   State to be passed to fsl_ckup_f() implementations via
   calls to fsl_repo_ckout() and fsl_ckout_update().
*/
struct fsl_ckup_state {
  /**
     The core SCM state for the just-extracted file. Note that its
     content member will be NULL: the content is not passed on via
     this interface because it is only loaded for files which require
     overwriting.

     An update process may synthesize content for extractState->fCard
     which do not 100% reflect the file on disk. Of primary note here:

     1) fCard->uuid will refer to the hash of the updated-to
     version, as opposed to the hash of the on-disk file (which may
     differ due to having local edits merged in). 

     2) For the update process, fCard->priorName will be NULL unless
     the file was renamed between the original and updated-to
     versions, in which case priorName will refer to the original
     version's name.
  */
  fsl_repo_extract_state const * extractState;
  /**
     Optional client-dependent state for use in the fsl_ckup_f()
     callback. This is copies from the corresponding
     fsl_ckup_opt::callbackState member.
  */
  void * callbackState;

  /**
     Vaguely describes the type of change the current call into
     the fsl_ckup_f() represents. The full range of values is
     not valid for all operations. Specifically:

     Checkout only uses:

     FSL_CKUP_FCHANGE_NONE
     FSL_CKUP_FCHANGE_UPDATED
     FSL_CKUP_FCHANGE_RM

     For update operations all (or most) values are potentially
     possible.

     If this has a value of FSL_CKUP_FCHANGE_RM,
     this->fileRmInfo will provide a bit more detail.
  */
  fsl_ckup_fchange_e fileChangeType;

  /**
     Indicates whether the file was removed by the process:

     - FSL_CKUP_RM_NOT = Was not removed.

     - FSL_CKUP_RM_KEPT = Was removed from the checked-out version but
     left in the filesystem because the confirmer said to.

     - FSL_CKUP_RM = Was removed from the checkout and the filesystem.

     When this->dryRun is true, this specifies whether the file would
     have been removed.
  */
  fsl_ckup_rm_state_e fileRmInfo;

  /**
     If fsl_repo_ckout()'s or fsl_ckout_update()'s options specified
     that the mtime should be set on each updated file, this holds
     that time. If the file existed and was not overwritten, it is set
     to that file's time. Else it is set to the current time (which
     may differ by a small fraction of a second from the file-write
     time because we avoid stat()'ing it again after writing). If
     this->fileRmInfo indicates that a file was removed, this might
     (depending on availability of the file in the filesystem at the
     time) be set to 0.

     When running in dry-run mode, this value may be 0, as we may not
     have a file in place which we can stat() to get it, nor a db
     entry from which to fetch it.

     This option is ignored for merge operations.
  */
  fsl_time_t mtime;

  /**
     The size of the extracted file, in bytes. If the file was removed
     from the filesystem (or removal was at least attempted) then this
     is set to -1.
  */
  fsl_int_t size;

  /**
     True if the current checkout/update is running in dry-run mode,
     else false. See fsl_ckup_opt::dryRun for details.
  */
  bool dryRun;
};

/**
   Options for use with fsl_repo_ckout() and
   fsl_ckout_update(). i.e. for checkout and update.
*/
struct fsl_ckup_opt {
  /**
     The version of the repostitory to check out or update. This must
     be the blob.rid of a checkin artifact.
  */
  fsl_id_t checkinRid;

  /**
     Gets called once per checked-out or updated file, passed a
     fsl_ckup_state instance with information about the
     checked-out file and related metadata. May be NULL.
  */
  fsl_ckup_f callback;

  /**
     State to be passed to this->callback via the
     fsl_ckup_state::callbackState member.
   */
  void * callbackState;

  /**
     An optional "confirmer" for answering questions about file
     overwrites and deletions posed by the checkout process.
     By default this confirmer of the associated fsl_cx instance
     is used.

     Caveats:

     - This is not currently used by the update process, only
     checkout.

     - If this->setMTime is true, the mtime is NOT set for any files
     which already exist and are skipped due to the confirmer saying
     to leave them in place.

     - Similarly, if the confirmer says to never overwrite files,
     permissions on existing files are not modified. fsl_repo_ckout()
     does not (re)write unmodified files, and thus may leave such
     files with different permissions. That's on the to-fix list.
  */
  fsl_confirmer confirmer;

  /**
     If true, the checkout/update processes will calculate the
     (synthetic) mtime of each extracted file and set its mtime. This
     is a relatively expensive operation which calculates the
     "effective mtime" of each file by calculating it: Fossil does not
     record file timestamps, instead treating files as if they had the
     timestamp of the most recent checkin in which they were added or
     modified.

     It's generally a good idea to let the update process stamp the
     _current_ time on modified files, in order to avoid any hiccups
     with build processes which rely on accurate times
     (e.g. Makefiles). When doing a clean checkout, it's often
     interesting to see the "original" times, though.
  */
  bool setMtime;

  /**
     A hint to fsl_repo_ckout() and fsl_ckout_update() about whether
     it needs to scan the checkout for changes. Set this to false ONLY
     if the calling code calls fsl_ckout_changes_scan() (or
     equivalent, e.g. fsl_vfile_changes_scan()) immediately before
     calling fsl_repo_ckout() or fsl_ckout_update(), as those require
     a non-stale changes scan in order to function properly.
  */
  bool scanForChanges;

  /**
     If true, the extraction process will "go through the motions" but
     will not write any files to disk. It will perform I/O such as
     stat()'ing to see, e.g., if it would have needed to overwrite a
     file.
  */
  bool dryRun;
};
typedef struct fsl_ckup_opt fsl_ckup_opt;

/**
  Empty-initialized fsl_ckup_opt const-copy constructor.
*/
#define fsl_ckup_opt_m {\
  -1/*checkinRid*/, NULL/*callback*/, NULL/*callbackState*/,  \
  fsl_confirmer_empty_m/*confirmer*/,\
  false/*setMtime*/, true/*scanForChanges*/,false/*dryRun*/ \
}

/**
  Empty-initialised fsl_ckup_opt instance. Clients should copy
  this value (or fsl_ckup_opt_empty_m) to initialise
  fsl_ckup_opt instances for sane default values.
*/
FSL_EXPORT const fsl_ckup_opt fsl_ckup_opt_empty;

/**
   A fsl_repo_extract() proxy which extracts the contents of the
   repository version specified by opt->checkinRid to the root
   directory of f's currently-opened checkout. i.e. it performs a
   "checkout" operation.

   For each extracted entry, cOpt->callback (if not NULL) will be
   passed a (fsl_ckup_state const*) which contains a pointer
   to the fsl_repo_extract_state and some additional metadata
   regarding the extraction. The value of cOpt->callbackState will be
   set as the callbackState member of that fsl_ckup_state
   struct, so that the client has a way of passing around app-specific
   state to that callback.

   After successful completion, the process will report (see below)
   any files which were part of the previous checkout version but are
   not part of the current version, optionally removing them from the
   filesystem (depending on the value of opt->rmMissingPolicy). It
   will IGNORE ANY DELETION FAILURE of files it attempts to
   delete. The reason it does not fail on removal error is because
   doing so would require rolling back the transaction, effectively
   undoing the checkout, but it cannot roll back any prior deletions
   which succeeded. Similarly, after all file removal is complete, it
   attempts to remove any now-empty directories left over by that
   process, also silently ignoring any errors. If the cOpt->dryRun
   option is specified, it will "go through the motions" of removing
   files but will not actually attempt filesystem removal. For
   purposes of the callback, however, it will report deletions as
   having happened (but will also set the dryRun flag on the object
   passed to the callback).

   After unpacking the SCM-side files, it may write out one or more
   manifest files, as described for fsl_ckout_manifest_write(), if the
   'manifest' config setting says to do so.

   As part of the file-removal process, AFTER all "existing" files are
   processed, it calls cOpt->callback() (if not NULL) for each removed
   file, noting the following peculiarities in the
   fsl_ckup_state object which is passed to it for those
   calls:

   - It is called after the processing of "existing" files. Thus the
   file names passed during this step may appear "out of order" with
   regards to the others (which are guaranteed to be passed in lexical
   order, though whether it is case-sensitive or not depends on the
   repository's case-sensitivity setting).

   - fileRmInfo will indicate that the file was removed from the
   checkout, and whether it was actually removed or retained in the
   filesystem. This will indicate filesystem-level removal even when
   in dry-run mode, though in that case no filesystem-level removal is
   actually attempted.

   - extractState->fileRid will refer to the file's blob's RID for the
   previous checkout version.

   - extractState->content will be NULL.

   - extractState->callbackState will be NULL. 

   - extractState->fCard will refer to the pre-removal state of the
   file. i.e. the state as it was in the checkout prior to this
   function being called.

   Returns 0 on success. Returns FSL_RC_NOT_A_REPO if f has no opened
   repo, FSL_RC_NOT_A_CKOUT if no checkout is opened. If
   cOpt->callback is not NULL and returns a non-0 result code,
   extraction ends and that result is returned. If it returns non-0 at
   any point after basic argument validation, it rolls back all
   changes or sets the current transaction stack into a rollback
   state.

   @see fsl_repo_open_ckout()
*/
FSL_EXPORT int fsl_repo_ckout(fsl_cx * f, fsl_ckup_opt const * cOpt);


/**
   UNDER CONSTRUCTION.

   Performs an "update" operation on f's currenly-opened
   checkout. Performing an update is similar to performing a checkout,
   the primary difference being that an update will merge local file
   modifications into any newly-updated files, whereas a checkout will
   overwrite them.

   TODO?: fossil(1)'s update permits a list of files, in which case it
   behaves differently: it updates the given files to the version
   requested but leaves the checkout at its current version. To be
   able to implement that we either need clients to call this in a
   loop, changing opt->filename on each call (like how we do
   fsl_ckout_manage()) or we need a way for them to pass on the list
   of files/dir in the opt object.

   @see fsl_repo_ckout().
*/
FSL_EXPORT int fsl_ckout_update(fsl_cx * f, fsl_ckup_opt const *opt);


/**
   Tries to calculate a version to update the current checkout version
   to, preferring the tip of the current checkout's branch.

   On success, 0 is returned and *outRid is set to the calculated RID,
   which may be 0, indicating that no errors were encountered but no
   version could be calculated.

   On error, non-0 is returned, outRid is not modified, and f's error
   state is updated.

   Returns FSL_RC_NOT_A_CKOUT if f has no checkout opened and
   FSL_RC_NOT_A_REPO if no repo is opened.

   If it calculates that there are multiple viable descendants it
   returns FSL_RC_AMBIGUOUS and f's error state will contain a list of
   the UUIDs (or UUID prefixes) of those descendants.

   Sidebar: to get the absolute latest version, irrespective of the
   branch, use fsl_sym_to_rid() to resolve the symbolic name "tip".
*/
FSL_EXPORT int fsl_ckout_calc_update_version(fsl_cx * f, fsl_id_t * outRid);

/**
   Bitmask used by fsl_ckout_manifest_setting() and
   fsl_ckout_manifest_write().
*/
enum fsl_cx_manifest_mask_e {
/** Coresponds to the file "manifest". */
FSL_MANIFEST_MAIN = 0x001,
/** Coresponds to the file "manifest.uuid". */
FSL_MANIFEST_UUID = 0x010,
/** Coresponds to the file "manifest.tags". */
FSL_MANIFEST_TAGS = 0x100
};
typedef enum fsl_cx_manifest_mask_e fsl_cx_manifest_mask_e;

/**
   Returns a bitmask representing which manifest files, if any, will
   be written when opening or updating a checkout directory, as
   specified by the repository's 'manifest' configuration setting, and
   sets *m to a bitmask indicating which of those are enabled. It
   first checks for a versioned setting then, if no versioned setting
   is found, a repository-level setting.

   A truthy setting value (1, "on", "true") means to write the
   manifest and manifest.uuid files. A string with any of the letters
   'r', 'u', or 't' means to write the [r]aw, [u]uid, and/or [t]ags
   file(s), respectively.

   If the manifest setting is falsy or not set, *m is set to 0, else
   *m is set to a bitmask representing which file(s) are considered to
   be auto-generated for this repository:

   - FSL_MANIFEST_MAIN = manifest
   - FSL_MANIFEST_UUID = manifest.uuid
   - FSL_MANIFEST_TAGS = manifest.tags

   Any db-related or allocation errors while trying to fetch the
   setting are silently ignored.

   For performance's sake, since this is potentially called often from
   fsl_reserved_fn_check(), this setting is currently cached by this
   routine (in the fsl_cx object), but that ignores the fact that the
   manifest setting can be modified at any time, either in a versioned
   setting file or the repository db, and may be modified from outside
   the library. There's a tiny back-door for working around that: if m
   is NULL, the cache will be flushed and no other work will be
   performed. Thus the following approach can be used to force a fresh
   check for that setting:

   ```
   fsl_ckout_manifest_setting(f, NULL); // clears caches, does nothing else
   fsl_ckout_manifest_setting(f, &myInt); // loads/caches the setting
   ```
*/
FSL_EXPORT void fsl_ckout_manifest_setting(fsl_cx *f, int *m);

/**
   Might write out the files manifest, manifest.uuid, and/or
   manifest.tags for the current checkout to the the checkout's root
   directory. The 2nd-4th arguments are interpreted as follows:

   0: Do not write that file.

   >0: Always write that file.

   <0: Use the value of the "manifest" config setting (see
   fsl_ckout_manifest_setting()) to determine whether or not to write
   that file.

   As each file is written, its mtime is set to that of the checkout
   version. (Forewarning: that behaviour may change if it proves to be
   problematic vis a vis build processes.)

   Returns 0 on success, non-0 on error:

   - FSL_RC_NOT_A_CKOUT if no checkout is opened.

   - FSL_RC_RANGE if the current checkout RID is 0 (indicating a fresh,
   empty repository).

   - Various potential DB/IO-related error codes.

   If the final argument is not NULL then it will be updated to
   contain a bitmask representing which files, if any, were written:
   see fsl_ckout_manifest_setting() for the values. It is updated
   regardless of success or failure and will indicate which file(s)
   was/were written before the error was triggered.

   Each file implied by the various manifest settings which is NOT
   written by this routine and is also not part of the current
   checkout (i.e. not listed in the vfile table) will be removed from
   disk, but a failure while attempting to do so will be silently
   ignored.

   @see fsl_repo_manifest_write()
*/
FSL_EXPORT int fsl_ckout_manifest_write(fsl_cx * const f,
                                        int manifest,
                                        int manifestUuid,
                                        int manifestTags,
                                        int * const wroteWhat );

/**
   Returns true if f has an opened checkout and the given absolute
   path is rooted in that checkout, else false. As a special case, it
   returns false if the path _is_ the checkout root unless zAbsPath
   has a trailing slash. (The checkout root is always stored with a
   trailing slash because that simplifies its internal usage.)

   Note that this is strictly a string comparison, not a
   filesystem-level operation.
*/
FSL_EXPORT bool fsl_is_rooted_in_ckout(fsl_cx * const f, char const * const zAbsPath);

/**
   Works like fsl_is_rooted_in_ckout() except that it returns 0 on
   success, and on error updates f with a description of the problem
   and returns non-0: FSL_RC_RANGE or (if updating the error state
   fails) FSL_RC_OOM.
 */
FSL_EXPORT int fsl_is_rooted_in_ckout2(fsl_cx * const f, char const * const zAbsPath);

/**
   Change-type values for use with fsl_ckout_revert_f() callbacks.
*/
enum fsl_ckout_revert_e {
/** Sentinel value. */
FSL_REVERT_NONE = 0,
/**
   File was previously queued for addition but unqueued
   by the revert process.
*/
FSL_REVERT_UNMANAGE,
/**
   File was previously queued for removal but unqueued by the revert
   process. If the file's contents or permissions were also reverted
   then the file is reported as FSL_REVERT_PERMISSIONS or
   FSL_REVERT_CONTENTS instead.
*/
FSL_REVERT_REMOVE,
/**
   File was previously scheduled to be renamed, but the rename was
   reverted. The name reported to the callback is the original one.
   If a file was both modified and renamed, it will be flagged as
   renamed instead of modified, for consistency with the usage of
   fsl_ckup_fchange_e's FSL_CKUP_FCHANGE_RENAMED.

   FIXME: this does not mean that the file on disk was actually
   renamed (if needed). That is TODO, pending addition of code to
   perform renames.
*/
FSL_REVERT_RENAME,
/** File's permissions (only) were reverted. */
FSL_REVERT_PERMISSIONS,
/**
   File's contents reverted. This value trumps any others in this
   enum. Thus if a file's permissions and contents were reverted,
   or it was un-renamed and its contents reverted, it will be
   reported using this enum entry.
*/
FSL_REVERT_CONTENTS
};
typedef enum fsl_ckout_revert_e fsl_ckout_revert_e;
/**
   Callback type for use with fsl_ckout_revert(). For each reverted
   file it gets passed the checkout-relative filename, type of change,
   and the callback state pointer which was passed to
   fsl_ckout_revert(). If it returns non-0, the revert process will
   end in an error and that code will be propagated back to the
   caller.  In such cases, any files reverted up until that point will
   still be reverted on disk but the reversion in the database will be
   rolled back. A change scan (e.g. fsl_ckout_changes_scan()) will
   restore balance to that equation, but these callbacks should only
   return non-0 in for catastrophic failure.
*/
typedef int (*fsl_ckout_revert_f)( char const *zFilename,
                                   fsl_ckout_revert_e changeType,
                                   void * callbackState );

/**
   Options for passing to fsl_ckout_revert().
*/
struct fsl_ckout_revert_opt {
  /**
     File or directory name to revert. See also this->vfileIds.
  */
  char const * filename;
  /**
     An alternative to assigning this->filename is to point
     this->vfileIds to a bag of vfile.id values. If this member is not
     NULL, fsl_ckout_revert() will ignore this->filename.

     @see fsl_filename_to_vfile_ids()
  */
  fsl_id_bag const * vfileIds;
  /**
     Interpret filename as relative to cwd if true, else relative to
     the current checkout root. This is ignored when this->vfileIds is
     not NULL.
  */
  bool relativeToCwd;
  /**
     If true, fsl_vfile_changes_scan() is called to ensure that
     the filesystem and vfile tables agree. If the client code has
     called that function, or its equivalent, since any changes were
     made to the checkout then this may be set to false to speed up
     the revert process.
  */
  bool scanForChanges;

  /**
     Optional callback to notify the client of what gets reverted.
  */
  fsl_ckout_revert_f callback;
  /**
     State for this->callback.
  */
  void * callbackState;
};
typedef struct fsl_ckout_revert_opt fsl_ckout_revert_opt;
/**
   Initialized-with-defaults fsl_ckout_revert_opt instance,
   intended for use in const-copy initialization.
*/
#define fsl_ckout_revert_opt_empty_m { \
  NULL/*filename*/,NULL/*vfileIds*/,true/*relativeToCwd*/,true/*scanForChanges*/, \
  NULL/*callback*/,NULL/*callbackState*/      \
}
/**
   Initialized-with-defaults fsl_ckout_revert_opt instance,
   intended for use in non-const copy initialization.
*/
FSL_EXPORT const fsl_ckout_revert_opt fsl_ckout_revert_opt_empty;

/**
   Reverts changes to checked-out files, replacing their
   on-disk versions with the current checkout's version.

   If zFilename refers to a directory, all managed files under that
   directory are reverted (if modified). If zFilename is NULL or
   empty, all modifications in the current checkout are reverted.

   If a file has been added but not yet committed, the add
   is un-queued but the file is otherwise untouched. If the
   file has been queued for removal, this removes it from
   that queue as well as restores its contents.

   If a rename is pending for the given filename, the name may match
   either its original name or new name. Whether or not that will
   actually work when file A is renamed to B and file C is renamed to
   A is anyone's guess. (Noting that (fossil mv) won't allow that
   situation to exist in the vfile table for a single checkout
   version, so it seems safe enough.)

   If the 4th argument is not NULL then it is called for each revert
   (_after_ the revert happens) to report what was done with that
   file. It gets passed the checkout-relative name of each reverted
   file. The 5th argument is not interpreted by this function but is
   passed on as-is as the final argument to the callback. If the
   callback returns non-0, the revert process is cancelled, any
   pending transaction is set to roll back, and that error code is
   returned. Note that cancelling a revert mid-process will leave file
   changes made by the revert so far in place, and thus the checkout
   db and filesystem will be in an inconsistent state until
   fsl_vfile_changes_scan() (or equivalent) is called to restore
   balance to the world.

   Files which are not actually reverted because their contents or
   permissions were not modified on disk are not reported to the
   callback unless the reversion was the un-queuing of an ADD or
   REMOVE operation.

   Returns 0 on success, any number of non-0 results on error.

   [tag:bug:revert-merge-hiccup] (2024-09-13): it is possible, via
   merging and copying modified files between multiple checkouts of
   the same repo, to get the tree into a state where this function has
   no files to revert and therefore does not clear the merge
   state. This condition is very tricky to trigger, and has only been
   seen once in the wild. "The problem" with fixing that is that
   whether or not we need to do a _full_ wipe of the merge state (as
   opposed to just wiping merge state of individual files) depends
   largely on information which (A) this function does have and (B) is
   onerous for the caller to figure out and provide (namely, whether
   it's a full revert or not). As this bug is so rare, we'll leave it
   for now and ponder potential solutions. As of 2024-09-13, this bug
   is believed to have been adequately resolved, but exactly how it
   was initially triggered is unclear, so we don't have a test which
   proves it is resolved.
*/
FSL_EXPORT int fsl_ckout_revert( fsl_cx * const f,
                                 fsl_ckout_revert_opt const * opt );

/**
   Expects f to have an opened checkout and zName to be the name of
   an entry in the vfile table where vfile.vid == vid. If vid<=0 then
   the current checkout RID is used. This function does not do any
   path resolution or normalization on zName and checks only for an
   exact match (honoring f's case-sensitivity setting - see
   fsl_cx_case_sensitive_set()).

   On success it returns 0 and assigns *vfid to the vfile.id value of
   the matching file.  If no match is found, 0 is returned and *vfile
   is set to 0. Returns FSL_RC_NOT_A_CKOUT if no checkout is opened,
   FSL_RC_RANGE if zName is not a simple path (see
   fsl_is_simple_pathname()), and any number of codes for db-related
   errors.

   This function matches only vfile.pathname, not vfile.origname,
   because it is possible for a given name to be in both fields (in
   different records) at the same time.
*/
FSL_EXPORT int fsl_filename_to_vfile_id( fsl_cx * const f, fsl_id_t vid,
                                         char const * zName,
                                         fsl_id_t * const vfid );

/**
   Searches the `vfile` table where `vfile.vid=vid` for a name which
   matches `zName` or all `vfile` entries found under a subdirectory
   named `zName` (with no trailing slash). `zName` must be relative to
   the checkout root. As a special case, if `zName` is `NULL`, empty,
   or `"."` then all files in `vfile` with the given `vid` are
   selected. For each entry it finds, it adds the `vfile.id` to
   `dest`. If `vid<=0` then the current checkout RID is used.

   If `changedOnly` is `true` then only entries which have been marked
   in the `vfile` table as having some sort of change are included, so
   if `true` then fsl_ckout_changes_scan() (or equivalent) must have
   been "recently" called to ensure that state is up to do. This routine
   only checks the `vfile` table for "is changed" state, it does not
   do filesystem-level checks on the files.

   This search honors the context-level case-sensitivity setting (see
   fsl_cx_case_sensitive_set()).

   Returns 0 on success. Not finding anything is not treated as an
   error, though we could arguably return `FSL_RC_NOT_FOUND` for the
   cases which use this function. In order to determine whether or not
   any results were actually found, compare `dest->entryCount` before
   and after calling this.

   This function matches only `vfile.pathname`, not `vfile.origname`,
   because it is possible for a given name to be in both fields (in
   different records) at the same time.

   @see fsl_ckout_vfile_ids()
*/
FSL_EXPORT int fsl_filename_to_vfile_ids( fsl_cx * const f, fsl_id_t vid,
                                          fsl_id_bag * const dest,
                                          char const * zName,
                                          bool changedOnly);

/**
   This is a variant of fsl_filename_to_vfile_ids() which accepts
   filenames in a more flexible form than that routine. This routine
   works exactly like that one except for the following differences:

   1) The given filename and the relativeToCwd arguments are passed to
   by fsl_ckout_filename_check() to canonicalize the name and ensure
   that it points to someplace within f's current checkout.

   2) Directory names passed to it may optionally end in an trailing
   slash.

   3) Because of (1), zName may not be NULL or empty. To fetch all of
   the vfile IDs for the current checkout, pass a zName of "."  and
   relativeToCwd=false.

   Returns 0 on success, else:

   - FSL_RC_MISUSE if zName is NULL or empty.
   - FSL_RC_OOM on allocation error.
   - FSL_RC_NOT_A_CKOUT if f has no opened checkout.
   - FSL_RC_RANGE if the given name points outside of the checkout.
*/
FSL_EXPORT int fsl_ckout_vfile_ids( fsl_cx * const f, fsl_id_t vid,
                                    fsl_id_bag * const dest, char const * zName,
                                    bool relativeToCwd, bool changedOnly );


/**
   This "mostly internal" routine (re)populates f's checkout vfile
   table with all files from the given checkin manifest. If
   manifestRid is 0 or less then the current checkout's RID is
   used. If vfile already contains any content for the given checkin,
   it is left intact (and several processes rely on that behavior to
   keep it from nuking, e.g., as-yet-uncommitted queued add/rm
   entries).

   Returns 0 on success, any number of codes on any of many potential
   errors.

   f must not be NULL and must have opened checkout and repository
   databases. In debug builds it will assert that that is so.

   If the 3rd argument is true, any entries in vfile for checkin
   versions other than the one specified in the 2nd argument are
   cleared from the vfile table. That is _almost_ always the desired
   behavior, but there are rare cases where vfile needs to temporarily
   (for the duration of a single transaction) hold state for multiple
   versions.

   If the 4th argument is not NULL, it gets assigned the number of
   blobs from the given version which are currently missing from the
   repository due to being phantoms (as opposed to being shunned).

   Returns 0 on success, FSL_RC_NOT_A_CKOUT if no checkout is opened,
   FSL_RC_OOM on allocation error, FSL_RC_DB for db-related problems,
   et.al.

   Misc. notes:

   - This does NOT update the "checkout" vvar table entry because this
   routine is sometimes used in contexts where we need to briefly
   maintain two vfile versions and keep the previous checkout version.

   - Apps must take care to not leave more than one version in the
   vfile table for longer than absolutely needed. They "really should"
   use fsl_vfile_unload() to clear out any version they load with this
   routine.

   @see fsl_vfile_unload()
   @see fsl_vfile_unload_except()
*/
FSL_EXPORT int fsl_vfile_load(fsl_cx * const f, fsl_id_t manifestRid,
                              bool clearOtherVersions,
                              uint32_t * missingCount);

/**
   Clears out all entries in the current checkout's vfile table with
   the given vfile.vid value. If vid<=0 then the current checkout RID
   is used (which is never a good idea from client-side code!).

   ACHTUNG: never do this without understanding the consequences. It
   can ruin the current checkout state.

   Returns 0 on success, FSL_RC_NOT_A_CKOUT if f has no checkout
   opened, FSL_RC_DB on any sort of db-related error (in which case
   f's error state is updated with a description of the problem).

   @see fsl_vfile_load()
   @see fsl_vfile_unload_except()
*/
FSL_EXPORT int fsl_vfile_unload(fsl_cx * const f, fsl_id_t vid);

/**
   A counterpart of fsl_vfile_unload() which removes all vfile
   entries where vfile.vid is not the given vid. If vid is <=0 then
   the current checkout RID is used.

   Returns 0 on success, FSL_RC_NOT_A_CKOUT if f has no checkout
   opened, FSL_RC_DB on any sort of db-related error (in which case
   f's error state is updated with a description of the problem).

   @see fsl_vfile_load()
   @see fsl_vfile_unload()
*/
FSL_EXPORT int fsl_vfile_unload_except(fsl_cx * const f, fsl_id_t vid);


/**
   Performs a "fingerprint check" between f's current checkout and
   repository databases. Returns 0 if either there is no checkout, no
   mismatch, or it is impossible to determine because the checkout is
   missing a fingerprint (which is legal for "older" checkout
   databases).

   If a mismatch is found, FSL_RC_REPO_MISMATCH is returned. Returns
   some other non-0 code on a lower-level error (db access, OOM,
   etc.).

   As a special case, this is a no-op for a newly-created repo, as
   those cannot yet have a fingerprint.

   A mismatch can happen when the repository to which a checkout
   belongs is replaced, either with a completely different repository
   or a copy/clone of that same repository. Each repository copy may
   have differing blob.rid values, and those are what the checkout
   database uses to refer to repository-side data. If those RIDs
   change, then the checkout is left pointing to data other than what
   it should be.

   TODO: currently the library offers no automated recovery mechanism
   from a mismatch, the only remedy being to close the checkout
   database, destroy it, and re-create it. fossil(1) is able, in some cases,
   to automatically recover from this situation.
*/
FSL_EXPORT int fsl_ckout_fingerprint_check(fsl_cx * const f);

/**
   Looks for the given file in f's current checkout. If relativeToCwd
   then the name is resolved from the current directory, otherwise it is
   assumed to be relative to the checkout root or an absolute path
   with the checkout dir as a prefix of that path.

   On success, 0 is returned and dest's gets populated with the
   content of the file.

   On error, non-0 is returned and, depending on the error type, dest
   might be partially populated. f's error state will be updated to
   describe the error.

   Results are undefined if any pointer argument is NULL.

   This function currently resolves symlinks on its way to the
   content, but that behaviour may change in the future to reflect f's
   symlink preferences.
*/
FSL_EXPORT int fsl_ckout_file_content(fsl_cx * const f, bool relativeToCwd,
                                      char const * zName,
                                      fsl_buffer * const dest);

/**
   Fetches the timestamp of the given F-card's name against the
   filesystem and/or the most recent checkin in which it was modified
   (as reported by fsl_mtime_of_manifest()). vid is the checkin
   version to look at. If it's <=0, the current checkout will be used.

   On success, returns 0 and:

   - If repoMtime is not NULL then (*repoMtime) is assigned to the
   result of fsl_mtime_of_manifest_file() for the given file.

   - If localMtime is not NULL then (*localMtime) is assigned to
   the checkout-local timestamp of the file.

   Returns non-0 on error. Some of the potential results include:

   - FSL_RC_NOT_A_CKOUT.

   - FSL_RC_NOT_FOUND if the filename cannot be resolved in the
     requested version or cannot be stat()'d. This can happen if,
     e.g. fc's file has been renamed, but not yet checked in, in the
     current checkout. A _potential_ TODO is to check for a
     (`vfile.origname=fc->name`) record and use `vfile.pathname` for
     the stat() call.

   - FSL_RC_OOM.
*/
FSL_EXPORT int fsl_card_F_ckout_mtime(fsl_cx * const f, fsl_id_t vid,
                                      fsl_card_F const * const fc,
                                      fsl_time_t * const repoMtime,
                                      fsl_time_t * const localMtime);

/**
   File change types for use with fsl_merge_state::fileChangeType.

   Terminology used in some of the descriptions:

   - (P) is the "pivot" - the common ancestor for the merge.
   - (M) is the version being merged in to...
   - (V) is current checkout version into which (M) is being merged.

   Maintenance reminder: this enum's values must start at 0 and
   increment sequentially, and FSL_MERGE_FCHANGE_count must be the
   final entry. This is to enable the creation of arrays, e.g. for
   keeping track (client-side) of how many times a given change type
   has been seen during a given merge run.
*/
enum fsl_merge_fchange_e {
/**
    Not currently used. Merge does not (and cannot without some
    surgery) report state of files unaffected by a merge. This entry
    exists for the case that that changes. This is the only entry in
    the enum which is guaranteed to have a specific value: 0, so that
    it can be used as a boolean false. */
FSL_MERGE_FCHANGE_NONE = 0,
/**
   File was added to (V) from (M).
*/
FSL_MERGE_FCHANGE_ADDED,
/**
   File content was copied as-is from (M) to (V).
*/
FSL_MERGE_FCHANGE_COPIED,
/**
   File was removed from (V) via (M). a.k.a. it became "unmanaged."
*/
FSL_MERGE_FCHANGE_RM,
/**
   Content from (M) was merged into (V).
*/
FSL_MERGE_FCHANGE_MERGED,
/**
   Special case of FSL_MERGE_FCHANGE_MERGED. Merge was performed from
   (M) to (V) and conflicts were detected. The newly-updated file will
   contain conflict markers.

    @see fsl_buffer_contains_merge_marker()
*/
FSL_MERGE_FCHANGE_CONFLICT_MERGED,
#if 0
/** Added in the current checkout but also contained in the
    updated-to version. The local copy takes precedence.
*/
FSL_MERGE_FCHANGE_CONFLICT_ADDED,
#endif
/**
   Added to (V) by (M) but a local unmanaged copy exists.
   The local copy is overwritten, per historical fossil(1) convention
   (noting that fossil has undo support to allow one to avoid loss of
   such a file's contents).
*/
FSL_MERGE_FCHANGE_CONFLICT_ADDED_UNMANAGED,
#if 0
/*
  Fossil deletes merged-over removed files, regardless of whether
  they're locally edited, so we'll do the same for now (noting that
  fossil has undo support which can hypothetically save that case
  from data loss). fsl_ckout_update() does not do so, but has extra
  infastructure to deal with this. */
/** Edited locally but removed from merged-in version. Local
    edits will be left in the checkout tree. */
FSL_MERGE_FCHANGE_CONFLICT_RM,
#endif
/**
   Cannot merge if one or both of the update/updating versions of a
   file is a symlink.

   This case needs re-thinking. fossil(1) simply skips such merges
   with a warning but no error. This library has no warning mechanism
   other than to pass this code on to the fsl_merge_f() callback,
   so that's what we do.

   For UPDATE ops, the updated-to version overwrites the previous
   version in this case. Why merge doesn't do that isn't clear, but
   it's probably because we can have any number of merge parents and
   choosing which one to use in the merge/replace case would be
   impossible.
*/
FSL_MERGE_FCHANGE_CONFLICT_SYMLINK,
/**
   Indicates that a merge of binary content was requested. We
   cannot merge binaries, so this indicates that the file in question
   was skipped over for merge purposes.

   fossil(1) simply skips over, with a warning, binaries during a
   merge, so we do the same (for lack of a better option).
*/
FSL_MERGE_FCHANGE_CONFLICT_BINARY,
/**
   File was renamed in the updated-to version. If a file is both
   modified and renamed, it will be reported twice: once for each type
   of change. The new name is reported via fsl_merge_state::filename
   and the previous name via fsl_merge_state::priorName.
*/
FSL_MERGE_FCHANGE_RENAMED,
/**
   This is the number of entries in this enum, for purposes of creating,
   e.g. arrays of counters. This entry is never reported via a
   fsl_merge_state::fileChangeType.
*/
FSL_MERGE_FCHANGE_count
};
typedef enum fsl_merge_fchange_e fsl_merge_fchange_e;

/** Reqired forward decl. */
typedef struct fsl_merge_opt fsl_merge_opt;

/**
   UNDER CONSTRUCTION! INCOMPLETE!

   A type for passing state to fsl_merge_f callbacks during
   fsl_ckout_merge() processing.

   For each step of a merge operation which affects a file,
   state reflecting that change is set in one of these objects
   and it is passed to the fsl_merge_f implementation supplied by
   the caller.

   Unlike fsl_ckout_update(), fsl_ckout_merge() reports only files
   which are affected by a merge, not unmodified files.  Whether
   that's a bug or a feature is not yet clear, but the merge algorithm
   does not, as is, support reporting unaffected files.

   Due to the complexity and intricacy of the merge operation, it is
   possible that any given file will get passed to the fsl_merge_f()
   callback more than once with a different
   fsl_merge_state::fileChangeType value. Most notably, a file which
   has been modified and renamed may be passed on one with
   FSL_MERGE_FCHANGE_COPIED and once with FSL_MERGE_FCHANGE_RENAMED.
   Whether that's a bug or a feature is as-yet undecided, but
   (A) fossil(1) does it that way and (B) _not_ doing that would
   require some rearchitecting.
*/
struct fsl_merge_state {
  /**
     The fsl_cx object for which the current merge is running.
   */
  fsl_cx * f;
  /**
     The options object which drives the current fsl_ckout_merge()
     run.
  */
  fsl_merge_opt const * opt;

  /**
     The checkout-relative name of the file affected by the merge.
     These bytes are invalidated after the fsl_merge_f() callback
     returns, so must be copied if the client requires them for later.
  */
  char const * filename;

  /**
     If this->fileChangeType is FSL_MERGE_FCHANGE_RENAMED then
     this is the previous name of the file, else it is NULL.
  */
  char const * priorName;
  
  /**
     Indicates the state of the file currently being merged.  Merge
     does not support the full range fo fsl_ckup_fchange_e
     values. TODO: list which it does or create a new enum which
     enumerates only merge-specific change types.
  */
  fsl_merge_fchange_e fileChangeType;

  /**
     Indicates whether the current file was removed. A state of
     FSL_CKUP_RM_KEPT and fileChangeType of FSL_MERGE_FCHANGE_RM
     indicates that the file has become unmanaged but the local
     copy was retained because it was flagged as locally modified.
  */
  fsl_ckup_rm_state_e fileRmInfo;
};
typedef struct fsl_merge_state fsl_merge_state;

/**
   Callback type for use with fsl_merge_opt and fsl_ckout_merge().
*/
typedef int (*fsl_merge_f)(fsl_merge_state const * const);

/**
   Merge type enum for use with fsl_merge_opt::mergeType.

   The values of these entries are fossil-magic values for the
   `vmerge.id` db field and must stay in sync with fossil's
   definition.
*/
enum fsl_merge_type_e {
/**
   Indicates a normal merge.
*/
FSL_MERGE_TYPE_NORMAL = 0,
/**
   Indicates an "integrate" merge, which tells the next checkin
   operation to apply a "closed" tag to the checkin from which this
   merge is performed (effectively closing its branch).

   Certain merge-time state will force this merge type to silently
   behave like FSL_MERGE_TYPE_NORMAL:

   - If the being-merged-in content is marked as private.
   - If the being-merged-in content is not a leaf.
*/
FSL_MERGE_TYPE_INTEGRATE = -4,
/**
   Indicates a cherrypick merge, pulling in only the changes made to a
   specific checkin without otherwise inheriting its lineage.
*/
FSL_MERGE_TYPE_CHERRYPICK = -1,
/**
   Indicates a backout merge, a reverse cherrypick, backing out any
   changes which were added by the corresponding
   fsl_merge_opt::mergeRid.
*/
FSL_MERGE_TYPE_BACKOUT = -2
};
typedef enum fsl_merge_type_e fsl_merge_type_e;

/**
   UNDER CONSTRUCTION.

   Options for use with fsl_ckout_merge().
*/
struct fsl_merge_opt {
  /**
     The version of the repostitory to merge into the current
     checkout. This must be the `blob.rid` of a checkin artifact.
  */
  fsl_id_t mergeRid;
  /**
     The version of the most recent common ancestor. Must normally be
     0. The default is calculated automatically based on
     this->mergeRid and this->mergeType.

     This corresponds to fossil(1)'s `--baseline` merge flag.

     fsl_ckout_merge() will fail if this is >0 and it does not refer
     to a checkin version or if this->mergeType is
     FSL_MERGE_TYPE_CHERRYPICK.
  */
  fsl_id_t baselineRid;
  /**
     Specifies the merge type to perform. Certain merge-internal logic
     may override this. Specifically, integrate-merges may be treated
     as regular merges, as documented for FSL_MERGE_TYPE_INTEGRATE.
  */
  fsl_merge_type_e mergeType;
  /**
     Gets called once per merge-updated file, passed a fsl_ckup_state
     instance with information about the merged file and related
     metadata. May be NULL, in which case the merge process will do as
     much work as it can, even if that means doing certain
     questionable things (such as skipping updates of binary files or
     symlinks because it refuses to merge them). As a rule of thumb,
     if fossil(1) performs a given "questional" merge features without
     generating an error, fsl_ckout_merge() does as well. This
     callback gives clients a chance to decide that certain states
     _Simply Will Not Stand_ and cancel the merge by returning non-0
     (preferably after calling fsl_cx_err_set() on the passed-in
     fsl_merge_state::f object).

     The callback is called after any on-disk changes are made to
     the file, e.g. merging, file permissions, renaming, etc. However,
     when this->dryRun is true, filesystem-level changes are skipped.
  */
  fsl_merge_f callback;
  /**
     Client-defined state for use with this->callback.
  */
  void * callbackState;
  /**
     A hint to fsl_ckout_merge() about whether it needs to scan the
     checkout for changes. Set this to false ONLY if the calling code
     calls fsl_ckout_changes_scan() (or equivalent,
     e.g. fsl_vfile_changes_scan()) immediately before calling
     fsl_ckout_merge(), as that function requires a non-stale changes
     scan in order to function properly.
  */
  bool scanForChanges;
  /**
     If true, the extraction process will "go through the motions" but
     will not write any files to disk. It may still perform I/O such
     as stat()'ing to see, e.g., if it would have needed to overwrite
     a file. When in dry-run, this->callback is still called as if
     dry-run mode were not in effect. Thus the on-disk state may not
     actually reflect what the callback sees when dry-run mode is
     active.
  */
  bool dryRun;
  /**
     This flag is not part of the public API and will be removed
     once the merge operation's development has settled down.
  */
  unsigned short debug;
  /**
     TODO:

     - How to handle fossil's --binary GLOBPATTERN flag. Plain string
     or a glob list object or a stateful predicate function or... ?

     We currently rely entirely on the global `binary-glob` setting.
  */
};
/** Initialized-with-defaults fsl_merge_opt structure, intended for
    const-copy initialization. */
#define fsl_merge_opt_empty_m \
  {-1/*mergeRid*/,0/*baselineRid*/, \
   FSL_MERGE_TYPE_NORMAL/*mergeType*/, \
   NULL/*callback*/, NULL/*callbackState*/, \
   true/*scanForChanges*/, \
   false/*dryRun*/,0/*debug*/}
/** Initialized-with-defaults fsl_merge_opt structure, intended for
    non-const copy initialization. */
extern const fsl_merge_opt fsl_merge_opt_empty;

/**
   UNDER CONSTRUCTION and not yet well-tested.

   Performs a "merge" operation on the current checkout, merging in
   version opt->mergeRid. If that version has already been merged,
   this call has no SCM-related side effects.

   Returns 0 on success, any number of non-0 codes on error,
   including, _but not limited to_:

   - FSL_RC_NOT_A_CKOUT if f has no opened checkout.

   - FSL_RC_OOM on allocation error.

   - FSL_RC_TYPE if opt->mergeRid or opt->baselineRid do to refer to
     a checkin.

   - FSL_RC_PHANTOM if a file participating in the merge is
     a phantom.

   - FSL_RC_RANGE if the to-be-merged-in RID is the same as the
     current checkout RID or the same as the pivot/baseline of
     the merge.

   - FSL_RC_NOT_FOUND if no common ancestor can be found for use as a
     basis for the merge.

   - FSL_RC_MISUSE if the current checkout is empty (has an RID of 0).

   - Any number of DB- or I/O-related codes, as well as codes from
     underlying APIs such as fsl_vfile_changes_scan().

   For all but the most trivial argument validation errors or
   allocation errors, f's error state will be updated with a
   description of the problem.

   TODOs and potential TODOs:

   - There are certain illegal combinations of merge state which may
     require adding new result codes for. e.g. a no-op merge.

   - Empty directories may be left behind when a merge removes all
     files in a directory. fsl_ckout_update() handles that case but
     fsl_ckout_merge() currently does not.

*/
FSL_EXPORT int fsl_ckout_merge(fsl_cx * const f, fsl_merge_opt const * const opt);

/**
   If zDirName is a directory name which contains what appears to
   be a fossil checkout database (noting that only a cursory check is done - the
   db is not opened and validated), the name of that database file (minus
   the directory part) is returned, else NULL is returned. The returned bytes
   are static.

   @see fsl_ckout_dbnames()
*/
FSL_EXPORT char const * fsl_is_top_of_ckout(char const *zDirName);

/**
   Returns an array of strings with the base names of valid fossil checkout
   databases. The array is terminated by a NULL element.

   As of this writing, and for the foreseeable future, the list is
   comprised of only 3 elements, {".fslckout", "_FOSSIL_", NULL}, but
   the order of the non-NULL elements is unspecified by the interface.
*/
FSL_EXPORT char const ** fsl_ckout_dbnames(void);

/** Convenience typedef. */
typedef struct fsl_ckout_rename_opt fsl_ckout_rename_opt;
/**
   Callback type for use with fsl_ckout_rename(). It gets called once per
   iteration of a rename operation, along with the original options object
   for the rename and:

   - zSrcName is the checkout-relative original name of the file.
   - zDestname is the checkout-relative new name of the file.

   Both strings are owned by the calling operation and will be freed
   soon after this call returns. If the client needs them, they must
   make copies.

   It must returns 0 on success. If it returns non-0 then the current
   renaming op is cancelled (rolled back) and the result code is
   propagated back to the caller of fsl_ckout_rename().

   This gets called immediately after the associated db record is
   modified and before the filesystem move (if any) is performed.

   Design note: ideally the callback would be called after the
   filesystem move, but the way the moves are currently processed (as
   a batch after the db updates, noting that those updates may trigger
   variou error conditions) precludes that. Alternately, we could
   delay the callback until the file-move phase (noting that the
   file-move step is optional), and that might be a sensible change to
   make.
*/
typedef int (*fsl_ckout_rename_f)(fsl_cx *, fsl_ckout_rename_opt const *,
                                  char const * zSrcName, char const *zDestName);

/**
   Options object for use with fsl_ckout_rename().
*/
struct fsl_ckout_rename_opt {
  /**
     The source filename(s) or directory name(s) to rename. The
     contents of this list _must not_ be modified while
     fsl_ckout_rename() is running (e.g. via a callback function).
  */
  fsl_list const * src;
  /**
     The target filename or directory name to rename to.
  */
  char const * dest;
  /**
     If true, src and dest are resolved/normalized based on the current
     working directory, else they must be relative to the top of the
     checkout.
  */
  bool relativeToCwd;
  /**
     If true fsl_ckout_rename() will attempt to move files within the
     filesystem. If false, it will only do the db-side renaming.
  */
  bool doFsMv;
  /**
     If true, fsl_ckout_rename() will not perform any lasting
     operations but will "go through the motions" with the exception
     of actually attempting to move files on disk (if doFsMv is
     true). It will still report errors in this mode, with the
     exception that filesystem-level moving is not attempted so
     potential failures there cannot be detected.

     Achtung: in dry-run mode it will trigger a rollback of any
     pending transaction which is opened before fsl_ckout_rename() is
     called.
  */
  bool dryRun;
  /** Optional callback. May be NULL. */
  fsl_ckout_rename_f callback;
  /** Optional state for the callback. */
  void * callbackState;
};

/** Initialized-with-defaults fsl_ckout_rename_opt structure, intended for
    const-copy initialization. */
#define fsl_ckout_rename_opt_empty_m {NULL,NULL,true,false,false,NULL,NULL}

/** Initialized-with-defaults fsl_ckout_rename_opt structure, intended for
    non-const copy initialization. */
extern const fsl_ckout_rename_opt fsl_ckout_rename_opt_empty;

/**
   This routine renames one or more SCM-managed files matching all
   entries in opt->src to opt->dest within the current checkout. This
   updates the current checkout's vfile table with the changes, and
   optionally attempts to move the files within the filesystem, but
   does not commit anything, so the change may still be reverted later
   on.

   If an entry in opt->src matches an existing directory name within
   the SCM-managed file list, this function assumes that all files
   under that directory are to be moved.

   If opt->relativeToCwd is true then the destination name and all
   opt->src names are canonicalized based on the current working
   directory (see fsl_getcwd()), otherwise they are assumed to be
   relative to f's current checkout directory.

   If opt->dest refers to an _existing_ directory, when an opt->src
   entry refers to a directory then that whole directory, including the
   directory name part, is renamed. If opt->dest does not exist in the
   filesystem and if opt->src resolves to multiple inputs, opt->dest is
   assumed to be a new target directory but the renaming behavior of
   the opt->src files differs. Examples:

   - Case 1: opt->dest refers to an existing directory: moving
     directory foo/bar to baz will result in baz/bar.

   - Case 2: opt->dest does not refer to an existing directory: moving
   directory foo/bar to baz results in baz with the previous contents
   of foo/bar.

   Why? Because that's how fossil(1) does it.

   This function matches opt->src entries only against
   `vfile.pathname`, not `vfile.origname`.

   If opt->doFsMv is true then the final operation this function
   attempts is to rename source files to their new destination. It
   will only attempt this in cases where (A) the original file exists
   and (B) the destination file does _not_ exist. Any errors generated
   during this filesystem-level rename/move step _are ignored_ for two
   reasons: 1) fossil(1) does it that way and (2) after moving any
   files, trying to roll back and undo the filesystem-level operations
   would add a significant amount of complexity and new errror cases
   (what happens when a try-to-undo-rename fails?).

   Returns 0 on succcess. Potential errors include:

   - FSL_RC_OOM on allocation error.

   - FSL_RC_NOT_A_CKOUT if f has no opened checkout.

   - FSL_RC_MISUSE: opt->src contains multiple entries but the
   destination is not an _existing_ directory.

   - FSL_RC_ALREADY_EXISTS: cannot rename because the destination name
   is already in use by another record.

   - FSL_RC_TYPE: opt->dest refers to a non-directory and an opt->src
   entry refers to a directory.

   - Any potential DB-related errors.

   BUGS/Shortcomings:

   - If it fails moving files in the filesystem, it might have
   successfully already moved one or more source files, and those
   moves are not undone.

   - Depending on the case-sensitivity of the repo and the filesystem,
   it might (UNTESTED) fail to rename a file if the new name differs
   only in its case (e.g. `README.TXT` to `README.txt`). Those
   relavant code renames such vfile entries but leaves the filesystem
   entries intact because fossil(1) does it that way.

   - If it moves a whole directory full of files it leaves any
   original directories intact. Improving this is on the TODO list.
   Often, such directories have contents (e.g. compiled/generated/temp
   files) and cannot be removed.
*/
FSL_EXPORT int fsl_ckout_rename(fsl_cx * const f,
                                fsl_ckout_rename_opt const * opt);

/**
   Undoes a rename scheduled by fsl_ckout_rename(). zNewName is the
   current (renamed) name of the file.

   If relativeToCwd is true then zNewName is canonicalized based on
   the current working directory (see fsl_getcwd()), otherwise it is
   assumed to be relative to f's current checkout directory.

   On success, or if no such pending rename is found, it returns 0.
   If it actually performs a db-level rename then it sets
   `*didSomething` to true if didSomething is not NULL. It should
   arguably return FSL_RC_NOT_FOUND (or some such) if no matching
   rename is pending, but that currently feels overly-pedantic.

   If doFsMv is true and a db entry is renamed and a file with the
   new name exists in the filesystem then this function will
   _delete_ any file which has the original name and rename the zNewName
   file to its reverted name. If that fails, the operation as a
   whole will fail and the db changes will be rolled back. Note,
   however, that a file deleted by this operation cannot be recovered
   and that the deletion must be done before the rename is attempted.

   On error, returns any of a wide array of non-0 result codes, only
   two of which can come directly from this function: FSL_RC_OOM or
   FSL_RC_NOT_A_CKOUT. Any other non-zero result codes are propagated
   errors from lower-level code.
*/
FSL_EXPORT int fsl_ckout_rename_revert(fsl_cx * const f,
                                       char const * zNewName,
                                       bool relativeToCwd, bool doFsMv,
                                       bool *didSomething);

/**
    Fetches the vfile.pathname value for the given vfile.id
    entry. Results are undefined if f does not have an opened
    checkout. On success returns 0 and sets `*zOut` to the name,
    transfering ownership to the caller (who must eventually pass it
    to fsl_free()). If absolute is true then the returned name has the
    checkout dir prepended to it, else it is relative to the top of
    the repo.

    Potential TODO: this routine is in no way optimized for heavy use
    within an app, expecting to be only rarely used, e.g. in
    conjunction with fsl_ckout_rename()-like ops. If it sees any
    significant use it should be refactored to use a cached statement.
*/
FSL_EXPORT int fsl_vfile_pathname(fsl_cx * const f, fsl_id_t vfid,
                                  bool absolute, char **zOut);


#if defined(__cplusplus)
} /*extern "C"*/
#endif
#endif
/* ORG_FOSSIL_SCM_FSL_CHECKOUT_H_INCLUDED */
