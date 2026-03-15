/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
#if !defined(ORG_FOSSIL_SCM_FSL_REPO_H_INCLUDED)
#define ORG_FOSSIL_SCM_FSL_REPO_H_INCLUDED
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/

/** @file repo.h

    repo.h declares APIs specifically dealing with repository-db-side
    state, as opposed to specifically checkout-side state or
    non-content-related APIs.
*/

#include "db.h" /* MUST come first b/c of config macros */
#include "hash.h"

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct fsl_card_F fsl_card_F;
typedef struct fsl_card_J fsl_card_J;
typedef struct fsl_card_Q fsl_card_Q;
typedef struct fsl_card_T fsl_card_T;
typedef struct fsl_checkin_opt fsl_checkin_opt;
typedef struct fsl_deck fsl_deck;
typedef struct fsl_uperm fsl_uperm;

/**
   Opens the given db file name as f's repository. Returns 0 on
   success. On error it sets f's error state and returns that code
   unless the error was FSL_RC_MISUSE (which indicates invalid
   arguments and it does not set the error state).

   Returns FSL_RC_ACCESS if f already has an opened repo db (use
   fsl_repo_close() or fsl_ckout_close() to close it).

   Returns FSL_RC_NOT_FOUND if repoDbFile is not found, as this
   routine cannot create a new repository db.

   Results are undefined if any argument is NULL.

   When a repository is opened, the fossil-level user name
   associated with f (if any) is overwritten with the default user
   from the repo's login table (the one with uid=1). Thus
   fsl_cx_user_get() may return a value even if the client has not
   called fsl_cx_user_set().

   It would be nice to have a parameter specifying that the repo
   should be opened read-only. That's not as straightforward as it
   sounds because of how the various dbs are internally managed
   (via one db handle). Until then, the permissions of the
   underlying repo file will determine how it is opened. i.e. a
   read-only repo will be opened read-only.


   Potentially interesting side-effects:

   - On success this re-sets several bits of f's configuration to
   match the repository-side settings.

   @see fsl_repo_create()
   @see fsl_repo_close()
   @see fsl_close_scm_dbs()
*/
FSL_EXPORT int fsl_repo_open( fsl_cx * f, char const * repoDbFile/*, char readOnlyCurrentlyIgnored*/ );

/**
   This function is a programmatic interpretation of
   this table:

   https://fossil-scm.org/index.html/doc/trunk/www/fileformat.wiki#summary

   For a given control artifact type and a card name in the form of
   the card name's letter (e.g. 'A', 'W', ...), this function
   returns 0 (false) if that card type is not permitted in this
   control artifact type, a negative value if the card is optional
   for this artifact type, and a positive value if the card type is
   required for this artifact type.

   As a special case, if t==FSL_SATYPE_ANY then this function
   always returns a negative value as long as card is a valid card
   letter.

   Another special case: when t==FSL_SATYPE_CHECKIN and card=='F',
   this returns a negative value because the table linked to above
   says F-cards are optional. In practice we have yet to find a use
   for checkins with no F-cards, so this library currently requires
   F-cards at checkin-time even though this function reports that
   they are optional.
*/
FSL_EXPORT int fsl_card_is_legal( fsl_satype_e t, char card );

/**
   Artifact tag types used by the Fossil framework. Their values
   are a hard-coded part of the Fossil format, and not subject to
   change (only extension, possibly).
*/
enum fsl_tagtype_e {
/**
   Sentinel value for use with constructors/initializers.
*/
FSL_TAGTYPE_INVALID = -1,
/**
   The "cancel tag" indicator, a.k.a. an anti-tag.
*/
FSL_TAGTYPE_CANCEL = 0,
/**
   The "add tag" indicator, a.k.a. a singleton tag.
*/
FSL_TAGTYPE_ADD = 1,
/**
   The "propagating tag" indicator.
*/
FSL_TAGTYPE_PROPAGATING = 2
};
typedef enum fsl_tagtype_e fsl_tagtype_e;

/**
   Hard-coded IDs used by the 'tag' table of repository DBs. These
   values get installed as part of the base Fossil db schema in new
   repos, and they must never change.
*/
enum fsl_tagid_e {
/**
   DB string tagname='bgcolor'.
*/
FSL_TAGID_BGCOLOR = 1,
/**
   DB: tag.tagname='comment'.
*/
FSL_TAGID_COMMENT = 2,
/**
   DB: tag.tagname='user'.
*/
FSL_TAGID_USER = 3,
/**
   DB: tag.tagname='date'.
*/
FSL_TAGID_DATE = 4,
/**
   DB: tag.tagname='hidden'.
*/
FSL_TAGID_HIDDEN = 5,
/**
   DB: tag.tagname='private'.
*/
FSL_TAGID_PRIVATE = 6,
/**
   DB: tag.tagname='cluster'.
*/
FSL_TAGID_CLUSTER = 7,
/**
   DB: tag.tagname='branch'.
*/
FSL_TAGID_BRANCH = 8,
/**
   DB: tag.tagname='closed'.
*/
FSL_TAGID_CLOSED = 9,
/**
   DB: tag.tagname='parent'.
*/
FSL_TAGID_PARENT = 10,
/**
   DB: tag.tagname='note'

   Extra text appended to a check-in comment.
*/
FSL_TAGID_NOTE = 11,

/**
   Largest tag ID reserved for internal use.
*/
FSL_TAGID_MAX_INTERNAL = 99
};


/**
   Returns one of '-', '+', or '*' for a valid input parameter, 0
   for any other value.
*/
FSL_EXPORT char fsl_tag_prefix_char( fsl_tagtype_e t );


/**
   A list of fsl_card_F objects. F-cards used a custom list type,
   instead of the framework's generic fsl_list, because experience has
   shown that the number of (de)allocations we need for F-card lists
   has a large negative impact when parsing and creating artifacts en
   masse. This list type, unlike fsl_list, uses a conventional object
   array approach to storage, as opposed to an array of pointers (each
   entry of which has to be separately allocated).

   These lists, and F-cards in generally, are typically maintained
   internally in the library. There's probably "no good reason" for
   clients to manipulate them.
*/
struct fsl_card_F_list {
  /**
     The list of F-cards. The first this->used elements are in-use.
     This pointer may change any time the list is reallocated.
  */
  fsl_card_F * list;
  /**
     The number of entries in this->list which are in use.
  */
  uint32_t used;
  /**
     The number of entries currently allocated in this->list.
  */
  uint32_t capacity;
  /**
     An internal cursor into this->list, used primarily for
     properly traversing the file list in delta manifests.

     Maintenance notes: internal updates to this member are the only
     reason some of the deck APIs require a non-const deck. This type
     needs to be signed for compatibility with some of the older
     algos, e.g. fsl__deck_F_seek_base().
  */
  int32_t cursor;
  /**
     Internal flags. Never, ever modify these from client code.
  */
  uint32_t flags;
};
typedef struct fsl_card_F_list fsl_card_F_list;
/** Empty-initialized fsl_card_F instance for const copy
    initialization */
#define fsl_card_F_list_empty_m {NULL, 0, 0, 0, 0}
/** Empty-initialized fsl_card_F instance for non-const copy
    initialization */
FSL_EXPORT const fsl_card_F_list fsl_card_F_list_empty;

/**
   A "deck" stores (predictably enough) a collection of "cards."
   Cards are constructs embedded within Fossil's Structural Artifacts
   to denote various sorts of changes in a Fossil repository, and a
   Deck encapsulates the cards for a single Structural Artifact of an
   arbitrary type, e.g. Manifest (a.k.a. "checkin") or Cluster. A card
   is basically a command with a single-letter name and a well-defined
   signature for its arguments. Each card is represented by a member
   of this struct whose name is the same as the card type
   (e.g. fsl_card::C holds a C-card and fsl_card::F holds a list of
   F-card). Each type of artifact only allows certain types of
   card. The complete list of valid card/construct combinations can be
   found here:

   https://fossil-scm.org/home/doc/trunk/www/fileformat.wiki#summary

   fsl_card_is_legal() can be used determine if a given card type
   is legal (per the above chart) with a given Control Artifiact
   type (as stored in the fsl_deck::type member).

   The type member is used by some algorithms to determine which
   operations are legal on a given artifact type, so that they can
   fail long before the user gets a chance to add a malformed artifact
   to the database. Clients who bypass the fsl_deck APIs and
   manipulate the deck's members "by hand" (so to say) effectively
   invoke undefined behaviour.

   The various routines to add/set cards in the deck are named
   fsl_deck_CARDNAME_add() resp. fsl_deck_CARDNAME_set(). The "add"
   functions represent cards which may appear multiple times
   (e.g. the 'F' card) or have multiple values (the 'P' card), and
   those named "set" represent unique or optional cards. The R-card
   is the outlier, with fsl_deck_R_calc(). NEVER EVER EVER directly
   modify a member of this struct - always use the APIs. The
   library performs some optimizations which can lead to corrupt
   memory and invalid free()s if certain members' values are
   directly replaced by the client (as opposed to via the APIs).

   Note that the 'Z' card is not in this structure because it is a
   hash of the other inputs and is calculated incrementally and
   appended automatically by fsl_deck_output().

   All non-const pointer members of this structure are owned by the
   structure instance unless noted otherwise (the fsl_deck::f member
   being the notable exception).

   Maintenance reminder: please keep the card members alpha sorted to
   simplify eyeball-searching through their docs.

   @see fsl_deck_malloc()
   @see fsl_deck_init()
   @see fsl_deck_parse()
   @see fsl_deck_load_rid()
   @see fsl_deck_finalize()
   @see fsl_deck_clean()
   @see fsl_deck_save()
   @see fsl_deck_A_set()
   @see fsl_deck_B_set()
   @see fsl_deck_C_set()
   @see fsl_deck_D_set()
   @see fsl_deck_E_set()
   @see fsl_deck_F_add()
   @see fsl_deck_J_add()
   @see fsl_deck_K_set()
   @see fsl_deck_L_set()
   @see fsl_deck_M_add()
   @see fsl_deck_N_set()
   @see fsl_deck_P_add()
   @see fsl_deck_Q_add()
   @see fsl_deck_R_set()
   @see fsl_deck_T_add()
   @see fsl_deck_branch_set()
   @see fsl_deck_U_set()
   @see fsl_deck_W_set()
*/
struct fsl_deck {
  /**
     Specifies the the type (or eventual type) of this
     artifact. The function fsl_card_is_legal() can be used to
     determined if a given card type is legal for a given value of
     this member. APIs which add/set cards use that to determine if
     the operation requested by the client is semantically legal.
  */
  fsl_satype_e type;

  /**
     DB repo.blob.rid value. Normally set by fsl_deck_load_rid().
  */
  fsl_id_t rid;

  /**
     The Fossil context responsible for this deck. Though this data
     type is normally, at least conceptually, free of any given fossil
     context, many related algorithms need a context in order to
     perform db- or caching-related work, as well as to simplify error
     message propagation. We store this as a struct member to keep all
     such algorithms from redundantly requiring both pieces of
     information as arguments.

     This object does not own the context and the context object must
     outlive this deck instance.
  */
  fsl_cx * f;

  /**
     The 'A' (attachment) card. Only used by FSL_SATYPE_ATTACHMENT
     decks. The spec currently specifies only 1 A-card per
     manifest, but conceptually this could/should be a list.
  */
  struct {
    /**
       Filename of the A-card.
    */
    char * name;

    /**
       Name of wiki page, or UUID of ticket or event (technote), to
       which the attachment applies.
    */
    char * tgt;

    /**
       UUID of the file being attached via the A-card.
    */
    fsl_uuid_str src;
  } A;

  struct {
    /**
       The 'B' (baseline) card holds the UUID of baseline manifest.
       This is empty for baseline manifests and holds the UUID of
       the parent for delta manifests.
    */
    fsl_uuid_str uuid;

    /**
       Baseline manifest corresponding to this->B. It is loaded on
       demand by routines which need it, typically by calling
       fsl_deck_F_rewind() (unintuitively enough!). The parent/child
       relationship in Fossil is the reverse of conventional -
       children own their parents, not the other way
       around. i.e. this->baseline will get cleaned up when the child
       part of that relationship is cleaned up.
    */
    fsl_deck * baseline;
  } B;
  /**
     The 'C' (comment) card.
  */
  char * C;

  /**
     The 'D' (date) card, in Julian format.
  */
  double D;

  /**
     The 'E' (event) card.
  */
  struct {
    /**
       The 'E' card's date in Julian Day format.
    */
    double julian;

    /**
       The 'E' card's UUID.
    */
    fsl_uuid_str uuid;
  } E;

  /**
     The 'F' (file) card container.
  */
  fsl_card_F_list F;

  /**
     UUID for the 'G' (forum thread-root) card.
  */
  fsl_uuid_str G;

  /**
     The H (forum title) card.
  */
  char * H;

  /**
     UUID for the 'I' (forum in-response-to) card.
  */
  fsl_uuid_str I;

  /**
     The 'J' card specifies changes to "value" of "fields" in
     tickets (FSL_SATYPE_TICKET).

     Holds (fsl_card_J*) entries.
  */
  fsl_list J;

  /**
     UUID for the 'K' (ticket) card.
  */
  fsl_uuid_str K;

  /**
     The 'L' (wiki name/title) card.
  */
  char * L;

  /**
     List of UUIDs (fsl_uuid_str) in a cluster ('M' cards).
  */
  fsl_list M;

  /**
     The 'N' (comment mime type) card. Note that this is only
     ostensibly supported by fossil, but fossil does not (as of
     2021-04-13) honor this value and always assumes that its value is
     "text/x-fossil-wiki".
  */
  char * N;

  /**
     List of UUIDs of parents ('P' cards). Entries are of type
     (fsl_uuid_str).
  */
  fsl_list P;

  /**
     'Q' (cherry pick) cards. Holds (fsl_card_Q*) entries.
  */
  fsl_list Q;

  /**
     The R-card holds an MD5 hash which is calculated based on the
     names, sizes, and contents of the files included in a
     manifest. See the class-level docs for a link to a page which
     describes how this is calculated.
  */
  char * R;

  /**
     List of 'T' (tag) cards. Holds (fsl_card_T*) instances.
  */
  fsl_list T;

  /**
     The U (user) card.
  */
  char * U;

  /**
     The W (wiki content) card.
  */
  fsl_buffer W;

  /**
     This is part of an optimization used when parsing fsl_deck
     instances from source text. For most types of card we re-use
     string values in the raw source text rather than duplicate them,
     and that requires storing the original text (as passed to
     fsl_deck_parse()). This requires that clients never tinker
     directly with values in a fsl_deck, in particular never assign
     over them or assume they know who allocated the memory for that
     bit.
  */
  fsl_buffer content;

  /**
     To potentially be used for a manifest cache.
  */
  fsl_deck * next;

  /**
     A marker which tells fsl_deck_finalize() whether or not
     fsl_deck_malloc() allocated this instance (in which case
     fsl_deck_finalize() will fsl_free() it) or not (in which case
     it does not fsl_free() it).
  */
  void const * allocStamp;
};

/**
   Initialized-with-defaults fsl_deck structure, intended for copy
   initialization.
*/
FSL_EXPORT const fsl_deck fsl_deck_empty;

/**
   Initialized-with-defaults fsl_deck structure, intended for
   in-struct and const copy initialization.
*/
#define fsl_deck_empty_m {                                  \
    FSL_SATYPE_ANY /*type*/,                                \
    0/*rid*/,                                            \
    NULL/*f*/,                                            \
    {/*A*/ NULL /* name */,                               \
           NULL /* tgt */,                                   \
           NULL /* src */},                                  \
    {/*B*/ NULL /*uuid*/,                                 \
           NULL /*baseline*/},                               \
    NULL /* C */,                                       \
    0.0 /*D*/,                                        \
    {/*E*/ 0.0 /* julian */,                          \
           NULL /* uuid */},                             \
    /*F*/ fsl_card_F_list_empty_m,  \
    0/*G*/,0/*H*/,0/*I*/,                           \
    fsl_list_empty_m /* J */,                       \
    NULL /* K */,                                 \
    NULL /* L */,                                 \
    fsl_list_empty_m /* M */,                     \
    NULL /* N */,                                 \
    fsl_list_empty_m /* P */,                     \
    fsl_list_empty_m /* Q */,                     \
    NULL /* R */,                                 \
    fsl_list_empty_m /* T */,                     \
    NULL /* U */,                                 \
    fsl_buffer_empty_m /* W */,                   \
    fsl_buffer_empty_m/*content*/,                \
    NULL/*next*/,                                 \
    NULL/*allocStamp*/                            \
  }


/**
   Allocates a new fsl_deck instance. Returns NULL on allocation
   error. The returned value must eventually be passed to
   fsl_deck_finalize() to free its resources.

   @see fsl_deck_finalize()
   @see fsl_deck_clean()
*/
FSL_EXPORT fsl_deck * fsl_deck_malloc(void);

/**
   Frees all resources belonging to the given deck's members
   (including its parents, recursively), and wipes deck clean of most
   state, but does not free() deck. Is a no-op if deck is NULL. As a
   special case, the (allocStamp, f) members of deck are kept intact.

   @see fsl_deck_finalize()
   @see fsl_deck_malloc()
   @see fsl_deck_clean2()
*/
FSL_EXPORT void fsl_deck_clean(fsl_deck * deck);

/**
   A variant of fsl_deck_clean() which "returns" its content buffer
   for re-use by transferring, after ensuring proper cleanup of its
   internals, its own content buffer's bytes into the given target
   buffer. Note that decks created "manually" do not have any content
   buffer contents, but those loaded via fsl_deck_load_rid() do. This
   function will fsl_buffer_swap() the contents of the given buffer
   (if any) with its own buffer, clean up its newly-acquired memory
   (tgt's previous contents, if any), and fsl_buffer_reuse() the
   output buffer.

   If tgt is NULL, this behaves exactly like fsl_deck_clean().
*/
FSL_EXPORT void fsl_deck_clean2(fsl_deck * deck, fsl_buffer * tgt);

/**
   Frees all memory owned by deck (see fsl_deck_clean()).  If deck
   was allocated using fsl_deck_malloc() then this function
   fsl_free()'s it, otherwise it does not free it.

   @see fsl_deck_malloc()
   @see fsl_deck_clean()
*/
FSL_EXPORT void fsl_deck_finalize(fsl_deck *deck);

/**
   Sets the A-card for an Attachment (FSL_SATYPE_ATTACHMENT)
   deck. Returns 0 on success.

   Returns FSL_RC_MISUSE if any of (mf, filename, target) are NULL,
   FSL_RC_RANGE if !*filename or if uuidSrc is not NULL and
   fsl_is_uuid(uuidSrc) returns false.

   Returns FSL_RC_TYPE if mf is not (as determined by its mf->type
   member) of a deck type capable of holding 'A' cards. (Only decks
   of type FSL_SATYPE_ATTACHMENT may hold an 'A' card.) If uuidSrc
   is NULL or starts with a NUL byte then it is ignored, otherwise
   the same restrictions apply to it as to target.

   The target parameter represents the "name" of the
   wiki/ticket/event record to which the attachment applies. For
   wiki pages this is their normal name (e.g. "MyWikiPage"). For
   events and tickets it is their full 40-byte UUID.

   uuidSrc is the UUID of the attachment blob itself. If it is NULL
   or empty then this card indicates that the attachment will be
   "deleted" (insofar as anything is ever deleted in Fossil).
*/
FSL_EXPORT int fsl_deck_A_set( fsl_deck * mf, char const * filename,
                               char const * target,
                               fsl_uuid_cstr uuidSrc);

/**
   Sets or unsets (if uuidBaseline is NULL or empty) the B-card for
   the given manifest to a copy of the given UUID. Returns 0 on
   success, FSL_RC_MISUSE if !mf, FSL_RC_OOM on allocation
   error. Setting this will free any prior values in mf->B, including
   a previously loaded mf->B.baseline.

   If uuidBaseline is not NULL and fsl_is_uuid() returns false,
   FSL_RC_SYNTAX is returned. If it is NULL the current value is
   freed (semantically, though the deck may still own the memory), the
   B card is effectively removed, and 0 is returned.

   Returns FSL_RC_TYPE if mf is not syntactically allowed to have
   this card card (as determined by
   fsl_card_is_legal(mf->type,...)).

   Sidebar: the ability to unset this card is unusual within this API,
   and is a requirement the library-internal delta manifest creation
   process. Most of the card-setting APIs, even when they are
   described as working like this one, do not accept NULL hash values.
*/
FSL_EXPORT int fsl_deck_B_set( fsl_deck * mf, fsl_uuid_cstr uuidBaseline);

/**
   Semantically identical to fsl_deck_B_set() but sets the C-card and
   does not place a practical limit on the comment's length.  comment
   must be the comment text for the change being applied.  If the
   given length is negative, fsl_strlen() is used to determine its
   length.
*/
FSL_EXPORT int fsl_deck_C_set( fsl_deck * mf, char const * comment, fsl_int_t cardLen);

/**
   Sets mf's D-card as a Julian Date value. Returns FSL_RC_MISUSE if
   !mf, FSL_RC_RANGE if date is negative, FSL_RC_TYPE if a D-card is
   not valid for the given deck, else 0. Passing a value of 0
   effectively unsets the card.
*/
FSL_EXPORT int fsl_deck_D_set( fsl_deck * mf, double date);

/**
   Sets the E-card in the given deck. date may not be negative -
   use fsl_db_julian_now() or fsl_julian_now() to get a default
   time if needed.  Retursn FSL_RC_MISUSE if !mf or !uuid,
   FSL_RC_RANGE if date is not positive, FSL_RC_RANGE if uuid is
   not a valid UUID string.

   Note that the UUID for an event, unlike most other UUIDs, need
   not be calculated - it may be a random hex string, but it must
   pass the fsl_is_uuid() test. Use fsl_db_random_hex() to generate
   random UUIDs. When editing events, e.g. using the HTML UI, only
   the most recent event with the same UUID is shown. So when
   updating events, be sure to apply the same UUID to the edited
   copies before saving them.
*/
FSL_EXPORT int fsl_deck_E_set( fsl_deck * mf, double date, fsl_uuid_cstr uuid);

/**
   Adds a new F-card to the given deck. The uuid argument is required
   to be NULL or pass the fsl_is_uuid() test. The name must be a
   "simplified path name" (as determined by fsl_is_simple_pathname()),
   or FSL_RC_RANGE is returned. Note that a NULL uuid is only valid
   when constructing a delta manifest, and this routine will return
   FSL_RC_MISUSE and update d->f's error state if uuid is NULL and
   d->B.uuid is also NULL. Also returns FSL_RC_MISUSE if the name and
   priorNames are the same.

   perms should be one of the fsl_fileperm_e values (0 is the usual
   case).

   priorName must only be non-NULL when renaming a file, and it must
   follow the same naming rules as the name parameter.

   Returns 0 on success.

   @see fsl_deck_F_set()
*/
FSL_EXPORT int fsl_deck_F_add( fsl_deck * d, char const * name,
                               fsl_uuid_cstr uuid,
                               fsl_fileperm_e perm,
                               char const * priorName);

/**
   Works mostly like fsl_deck_F_add() except that:

   1) It enables replacing an existing F-card with a new one matching
   the same name.

   2) It enables removing an F-card by passing a NULL uuid.

   3) It refuses to work on a deck for which d->uuid is not NULL or
   d->rid!=0, returning FSL_RC_MISUSE if either of those apply.

   If d contains no F-card matching the given name (case-sensitivity
   depends on d->f's fsl_cx_is_case_sensitive() value) then:

   - If the 3rd argument is NULL, it returns FSL_RC_NOT_FOUND with
     (effectively) no side effects (aside, perhaps, from sorting d->F
     if needed to perform the search).

   - If the 3rd argument is not NULL then it behaves identically to
     fsl_deck_F_add().

   If a match is found, then:

   - If the 3rd argument is NULL, it removes that entry from the
     F-card list and returns 0.

   - If the 3rd argument is not NULL, the fields of the resulting
     F-card are modified to match the arguments passed to this
     function, copying the values of all C-string arguments. (Sidebar:
     we may need to copy the name, despite already knowing it, because
     of how fsl_deck instances manage F-card memory.)

   In all cases, if the 3rd argument is NULL then the 4th and 5th
   arguments are ignored.

   Returns 0 on success, FSL_RC_OOM if an allocation fails.  See
   fsl_deck_F_add() for other failure modes. On error, d's F-card list
   may be left in an inconsistent state and it must not be used
   further.

   @see fsl_deck_F_add()
   @see fsl_deck_F_set_content()
*/
FSL_EXPORT int fsl_deck_F_set( fsl_deck * d, char const * name,
                               fsl_uuid_cstr uuid,
                               fsl_fileperm_e perm,
                               char const * priorName);
/**
   UNDER CONSTRUCTION! EXPERIMENTAL!

   This variant of fsl_deck_F_set() accepts a buffer of content to
   store as the file's contents. Its hash is derived from that
   content, using fsl_repo_blob_lookup() to hash the given content.
   Thus this routine can replace existing F-cards and save their
   content at the same time. When doing so, it will try to make the
   parent version (if this is a replacement F-card) a delta of the new
   content version (it may refuse to do so for various resources, but
   any heuristics which forbid that will not trigger an error).

   The intended use of this routine is for adding or replacing content
   in a deck which has been prepared using fsl_deck_derive().

   Returns 0 on success, else an error code propagated by
   fsl_deck_F_set(), fsl_repo_blob_lookup(), or some other lower-level
   routine. This routine requires that a transaction is active and
   returns FSL_RC_MISUSE if none is active. For any non-trivial
   error's, d->f's error state will be updated with a description of
   the problem.

   Returns FSL_RC_MISUSE if `d->rid>0` (which indicates that the deck
   has already been saved in the repository). fsl_deck_derive() can be
   used to "extend" a saved deck into a new version before using this API.

   Returns FSL_RC_RANGE if zName is not a valid filename for use as a
   repository entry, as per fsl_is_simple_pathname().

   On error, d->f's error state will be updated with a description of
   the problem.

   TODO: add a fsl_cx-level or fsl_deck-level API for marking content
   saved this way as private. This type of content is intended for use
   cases which do not have a checkout, and thus cannot be processed
   with fsl_checkin_commit() (which includes a flag to mark its
   content as private).

   @see fsl_deck_F_set()
   @see fsl_deck_F_add()
   @see fsl_deck_derive()
*/
FSL_EXPORT int fsl_deck_F_set_content( fsl_deck * d, char const * name,
                                       fsl_buffer const * src,
                                       fsl_fileperm_e perm,
                                       char const * priorName);

/**
   UNDER CONSTRUCTION! EXPERIMENTAL!

   This routine rewires d such that it becomes the basis for a derived
   version of itself. Requires that d be a loaded from a repository,
   complete with an RID, else FSL_RC_MISUSE is returned.

   In short, this function performs the following:

   - Clears d->P
   - Assigns d->P[0] to the UUID of d->rid
   - Clears d->rid
   - Clears any other members which need to be (re)set by the new
     child/derived version. That includes the following card
     letters: `ACDEGHIJKLMNQRTUW`.
   - If d is a delta manifest it restructures it as a new baseline
     (see below).
   - It specifically keeps d->F intact OR creates a new one (see below).

   Returns 0 on success, FSL_RC_OOM on an allocation error,
   FSL_RC_MISUSE if d->rid<=0 (i.e. the deck has never been saved or
   was not loaded from the db). If d->type is not FSL_SATYPE_CHECKIN,
   FSL_RC_TYPE is returned (fixing that for other derivable types is
   TODO). On error, d may be left in an inconsistent state and must
   not be used further except to pass it to fsl_deck_finalize().

   The intention of this function is to simplify creation of decks
   which are to be used for creating checkins without requiring a
   checkout.

   To avoid certain corner cases, this function does not allow
   creation of delta manifests. If d has a B-card then it is a delta.
   This function clears its B-card and recreates the F-card list using
   the B-card's F-card list and any F-cards from the current delta. In
   other words, it creates a new baseline manifest.

   The expected workflow for this API is something like:

   - Use fsl_deck_load_rid() to load a deck.
   - Pass that deck to fsl_deck_derive().
   - Update the deck's cards to suit.
   - fsl_deck_save() the deck.

   @todo Extend this to support other inheritable deck types, e.g.
   wiki, forum posts, and technotes.

   @see fsl_deck_F_set_content()
*/
FSL_EXPORT int fsl_deck_derive(fsl_deck * d);

/**
   Callback type for use with fsl_deck_F_foreach() and
   friends. Implementations must return 0 on success, FSL_RC_BREAK
   to abort looping without an error, and any other value on error.
*/
typedef int (*fsl_card_F_visitor_f)(fsl_card_F const * fc,
                                     void * state);

/**
   For each F-card in d, cb(card,visitorState) is called. Returns
   the result of that loop. If cb returns FSL_RC_BREAK, the
   visitation loop stops immediately and this function returns
   0. If cb returns any other non-0 code, looping stops and that
   code is returned immediately.

   This routine calls fsl_deck_F_rewind() to reset the F-card cursor
   and/or load d's baseline manifest (if any). If loading the baseline
   fails, an error code from fsl_deck_baseline_fetch() is returned.

   The F-cards will be visited in the order they are declared in
   d. For loaded-from-a-repo manifests this is always lexical order
   (for delta manifests, consistent across the delta and
   baseline). For hand-created decks which have not yet been
   fsl_deck_unshuffle()'d, the order is unspecified.
*/
FSL_EXPORT int fsl_deck_F_foreach( fsl_deck * d,
                                   fsl_card_F_visitor_f cb,
                                   void * visitorState );

/**
   Fetches the next F-card entry from d. fsl_deck_F_rewind() must
   have be successfully executed one time before calling this, as
   that routine ensures that the baseline is loaded (if needed),
   which is needed for proper iteration over delta manifests.

   This routine always assigns *f to NULL before starting its work, so
   the client can be assured that it will never contain the same value
   as before calling this (unless that value was NULL).

   On success 0 is returned and *f is assigned to the next F-card.
   If *f is NULL when returning 0 then the end of the list has been
   reached (fsl_deck_F_rewind() can be used to re-set it).

   Example usage:

   ```
   int rc;
   fsl_card_F const * fc = NULL;
   rc = fsl_deck_F_rewind(d);
   if(!rc) while( !(rc=fsl_deck_F_next(d, &fc)) && fc) {...}
   ```

   Note that files which were deleted in a given version are not
   recorded in baseline manifests but are in deltas. To avoid
   inconsistencies, this routine does NOT include deleted files in its
   results, regardless of whether d is a baseline or delta. (It used
   to, but that turned out to be a design flaw.)

   Implementation notes: for baseline manifests this is a very
   fast and simple operation. For delta manifests it gets
   rather complicated.
*/
FSL_EXPORT int fsl_deck_F_next( fsl_deck * d, fsl_card_F const **f );

/**
   Rewinds d's F-card traversal iterator and loads d's baseline
   manifest, if it has one (i.e. if d->B.uuid is not NULL) and it is
   not loaded already (i.e. if d->B.baseline is NULL). Returns 0 on
   success. The only error condition is if loading of the a baseline
   manifest fails, noting that only delta manifests have baselines.

   Results are undefined if d->f is NULL, and that may trigger an
   assert() in debug builds.
*/
FSL_EXPORT int fsl_deck_F_rewind( fsl_deck * d );

/**
   Looks for a file in a manifest or (for a delta manifest) its
   baseline. No normalization of the given filename is performed -
   it is assumed to be relative to the root of the checkout.

   It requires that d->type be FSL_SATYPE_CHECKIN and that d be
   loaded from a stored manifest or have been fsl_deck_unshuffle()'d
   (if called on an under-construction deck). Specifically, this
   routine requires that d->F be sorted properly or results are
   undefined.

   d->f is assumed to be the fsl_cx instance which deck was loaded
   from, which impacts the search process as follows:

   - The search take's d->f's underlying case-insensitive option into
   account. i.e. if case-insensitivy is on then files in any case
   will match.

   - If no match is found in d and is a delta manifest (d->B.uuid
   is set) then d's baseline is lazily loaded (if needed) and
   the search continues there. (Delta manifests are only one level
   deep, so this is not recursive.)

   Returns NULL if !d, !d->f, or d->type!=FSL_SATYPE_CHECKIN, if no
   entry is found, or if delayed loading of the parent manifest (if
   needed) of a delta manifest fails (in which case d->f's error
   state should hold more information about the problem).

   In debug builds this function asserts that d is not NULL.

   Design note: d "should" be const, but search optimizations for
   the typical use case require potentially lazy-loading
   d->B.baseline and updating d->F.
*/
FSL_EXPORT fsl_card_F const * fsl_deck_F_search(fsl_deck *d, const char *zName);

/**
   Given two F-card instances, this function compares their names
   (case-sensitively). Returns a negative value if lhs is
   lexically less than rhs, a positive value if lhs is lexically
   greater than rhs, and 0 if they are lexically equivalent (or are
   the same pointer).


   Though fossil repositories may be case-insensitive, the F-cards use
   a stable casing unless a file is removed and re-added with a
   different case, so this comparison is case-sensitive..

   Results are undefined if either argument is NULL.
*/
FSL_EXPORT int fsl_card_F_compare_name( fsl_card_F const * lhs,
                                        fsl_card_F const * rhs);

/**
   If fc->uuid refers to a blob in f's repository database then that
   content is placed into dest (as per fsl_content_get()) and 0 is
   returned. Returns FSL_RC_NOT_FOUND if fc->uuid is not
   found. Returns FSL_RC_MISUSE if any argument is NULL.  If
   fc->uuid is NULL (indicating that it refers to a file deleted in
   a delta manifest) then FSL_RC_RANGE is returned. Returns
   FSL_RC_NOT_A_REPO if f has no repository opened.

   On any error but FSL_RC_MISUSE (basic argument validation) f's
   error state is updated to describe the error.

   @see fsl_content_get()
*/
FSL_EXPORT int fsl_card_F_content( fsl_cx * f, fsl_card_F const * fc,
                                   fsl_buffer * dest );

/**
   Sets the 'G' card on a forum-post deck to a copy of the given
   UUID.
*/
FSL_EXPORT int fsl_deck_G_set( fsl_deck * mf, fsl_uuid_cstr uuid);
/**
   Sets the 'H' card on a forum-post deck to a copy of the given
   comment. If cardLen is negative then fsl_strlen() is used to
   calculate its length.
 */
FSL_EXPORT int fsl_deck_H_set( fsl_deck * mf, char const * comment, fsl_int_t cardLen);
/**
   Sets the 'I' card on a forum-post deck to a copy of the given
   UUID.
*/
FSL_EXPORT int fsl_deck_I_set( fsl_deck * mf, fsl_uuid_cstr uuid);

/**
   Adds a J-card to the given deck, setting/updating the given ticket
   property key to the given value. The key is required but the value
   is optional (may be NULL). If isAppend then the value is appended
   to any existing value, otherwise it replaces any existing value.

   It is currently unclear whether it is legal to include multiple
   J cards for the same key in the same control artifact, in
   particular if their isAppend values differ.

   Returns 0 on success, FSL_RC_MISUSE if !mf or !key, FSL_RC_RANGE
   if !*field, FSL_RC_TYPE if mf is of a type for which J cards are
   not legal (see fsl_card_is_legal()), FSL_RC_OOM on allocation
   error.
*/
FSL_EXPORT int fsl_deck_J_add( fsl_deck * mf, bool isAppend,
                               char const * key, char const * value );

/**
   Sets the K-card (ticket ID) on the given deck. If passed NULL, it
   creates a new ticket ID (a 40-digit string of random hex bytes) and
   returns FSL_RC_OOM if allocation of those bytes fails. If uuid is
   not NULL then it must be a 40-byte lower-case hex string, the K-card
   value of the ticket this change is being applied to.
*/
FSL_EXPORT int fsl_deck_K_set( fsl_deck * mf, fsl_uuid_cstr uuid);

/**
   Semantically identical fsl_deck_B_set() but sets the L-card.
   title must be the wiki page title text of the wiki page this
   change is being applied to.
*/
FSL_EXPORT int fsl_deck_L_set( fsl_deck * mf, char const *title, fsl_int_t len);

/**
   Adds the given UUID as an M-card entry. Returns 0 on success, or:

   FSL_RC_MISUSE if !mf or !uuid

   FSL_RC_TYPE if fsl_deck_check_type(mf,'M') returns false.

   FSL_RC_RANGE if !fsl_is_uuid(uuid).

   FSL_RC_OOM if memory allocation fails while adding the entry.
*/
FSL_EXPORT int fsl_deck_M_add( fsl_deck * mf, fsl_uuid_cstr uuid );

/**
   Semantically identical to fsl_deck_B_set() but sets the N card.
   mimeType must be the content mime type for comment text of the
   change being applied.
*/
FSL_EXPORT int fsl_deck_N_set( fsl_deck * mf, char const *mimeType, fsl_int_t len);

/**
   Adds the given UUID as a parent of the given change record. If len
   is less than 0 then fsl_strlen(parentUuid) is used to determine its
   length. Returns FSL_RC_MISUE if !*parentUuid. Returns FSL_RC_RANGE
   if parentUuid does not pass fsl_is_uuid().

   Results are undefined if parentUuid is NULL.

   The first P-card added to a deck MUST be the UUID of its primary
   parent (one which was not involved in a merge operation). All
   others (from merges) are considered "non-primary."

*/
FSL_EXPORT int fsl_deck_P_add( fsl_deck * mf, fsl_uuid_cstr parentUuid);
/**
   A convenience wrapper around fsl_deck_P_add() which resolves the given
   RID to its UUID and passes it on to fsl_deck_P_add(). Returns non-0
   on error.
*/
FSL_EXPORT int fsl_deck_P_add_rid( fsl_deck * mf, fsl_id_t parentRid );

/**
   If d contains a P card with the given index, this returns the RID
   corresponding to the UUID at that index. Returns a negative value
   on error, 0 if there is no entry for that index (the index is out
   of bounds).
*/
FSL_EXPORT fsl_id_t fsl_deck_P_get_id(fsl_deck * d, int index);

/**
   Adds a Q-card record to the given deck. The type argument must
   be negative for a backed-out change, positive for a cherrypicked
   change.  target must be a valid UUID string. If baseline is not
   NULL then it also must be a valid UUID.

   Returns 0 on success, non-0 on error. FSL_RC_MISUSE if !mf or
   !target, FSL_RC_SYNTAX if type is 0 or target/baseline are not
   valid UUID strings (baseline may be NULL).
*/
FSL_EXPORT int fsl_deck_Q_add( fsl_deck * mf, int type,
                               fsl_uuid_cstr target,
                               fsl_uuid_cstr baseline );

/**
   Functionally identical to fsl_deck_B_set() except that it sets
   the R-card. Returns 0 on succes, FSL_RC_RANGE if md5 is not NULL
   or exactly FSL_STRLEN_MD5 bytes long (not including trailing
   NUL). If md5==NULL the current R value is cleared.

   It would be highly unusual to have to set the R-card manually,
   as its calculation is quite intricate/intensive. See
   fsl_deck_R_calc() and fsl_deck_unshuffle() for details
*/
FSL_EXPORT int fsl_deck_R_set( fsl_deck * mf, char const *md5);

/**
   Adds a new T-card (tag) entry to the given deck.

   If uuid is not NULL and fsl_is_uuid(uuid) returns false then
   this function returns FSL_RC_RANGE. If uuid is NULL then it is
   assumed to be the UUID of the currently-being-constructed
   artifact in which the tag is contained (which appears as the '*'
   character in generated artifacts).

   Returns 0 on success. Returns FSL_RC_MISUSE if !d or
   !name. Returns FSL_RC_TYPE (and update's d's error state with a
   message) if the T card is not legal for d (see
   fsl_card_is_legal()).  Returns FSL_RC_RANGE if !*name, tagType
   is invalid, or if uuid is not NULL and fsl_is_uuid(uuid)
   return false. Returns FSL_RC_OOM if an allocation fails.
*/
FSL_EXPORT int fsl_deck_T_add( fsl_deck * d, fsl_tagtype_e tagType,
                               fsl_uuid_cstr uuid, char const * name,
                               char const * value);

/**
   Adds the given tag instance to the given manifest.
   Returns 0 on success, FSL_RC_MISUSE if either argument
   is NULL, FSL_RC_OOM if appending the tag to the list
   fails.

   On success ownership of t is passed to mf. On error ownership is
   not modified.
*/
FSL_EXPORT int fsl_deck_T_add2( fsl_deck * mf, fsl_card_T * t);

/**
   A convenience form of fsl_deck_T_add() which adds two propagating
   tags to the given deck: "branch" with a value of branchName and
   "sym-branchName" with no value.

   Returns 0 on success. Returns FSL_RC_OOM on allocation error and
   FSL_RC_RANGE if branchName is empty or contains any characters with
   ASCII values <=32d. It natively assumes that any characters >=128
   are part of multibyte UTF8 characters.

   ACHTUNG: this does not arrange for canceling the previous branch
   because it doesn't know that branch at this point. To cancel the
   previous branch a cancelation T-card needs to be added to the deck
   named "sym-BRANCHNAME". Historically such tags have had the value
   "Cancelled by branch", but that's not a requirement.
*/
FSL_EXPORT int fsl_deck_branch_set( fsl_deck * d, char const * branchName );

/**
   Calculates the value of d's R-card based on its F-cards and updates
   d->R. It may also, as a side-effect, sort d->F.list lexically (a
   requirement of a R-card calculation).

   Returns 0 on success. Requires that d->f have an opened
   repository db, else FSL_RC_NOT_A_REPO is returned. If d's type is
   not legal for an R-card then FSL_RC_TYPE is returned and d->f's
   error state is updated with a description of the error. If d is of
   type FSL_SATYPE_CHECKIN and has no F-cards then the R-card's value
   is that of the initial MD5 hash state. Various other codes can be
   returned if fetching file content from the db fails.

   Note that this calculation is exceedingly memory-hungry. While
   Fossil originally required R-cards, the cost of calculation
   eventually caused the R-card to be made optional. This API
   allows the client to decide on whether to use them (for more
   (admittedly redundant!) integrity checking) or not (much faster
   but "not strictly historically correct"), but defaults to having
   them enabled for symmetry with fossil(1).

   @see fsl_deck_R_calc2()
*/
FSL_EXPORT int fsl_deck_R_calc(fsl_deck * d);

/**
   A variant of fsl_deck_R_calc() which calculates the given deck's
   R-card but does not assign it to the deck, instead returning it
   via the 2nd argument:

   If *tgt is not NULL when this function is called, it is required to
   point to at least FSL_STRLEN_MD5+1 bytes of memory to which the
   NUL-terminated R-card hash will be written. If *tgt is NULL then
   this function assigns (on success) *tgt to a dynamically-allocated
   R-card hash and transfers ownership of it to the caller (who must
   eventually fsl_free() it). On error, *tgt is not modified.

   Results are undefined if either argument is NULL.

   Returns 0 on success. See fsl_deck_R_calc() for information about
   possible errors, with the addition that FSL_RC_OOM is returned
   if *tgt is NULL and allocating a new *tgt value fails.

   Calculating the R-card necessarily requires that d's F-card list be
   sorted, which this routine does if it seems necessary. The
   calculation also necessarily mutates the deck's F-card-traversal
   cursor, which requires loading the deck's B-card, if it has
   one. Aside from the F-card sorting, and potentially B-card, and the
   cursor resets, this routine does not modify the deck. On success,
   the deck's F-card iteration cursor (and that of d->B, if it's
   loaded) is rewound.
*/
FSL_EXPORT int fsl_deck_R_calc2(fsl_deck *d, char ** tgt);

/**
   Semantically identical fsl_deck_B_set() but sets the U-card.
   userName must be the user who's name should be recorded for
   this change.
*/
FSL_EXPORT int fsl_deck_U_set( fsl_deck * mf, char const *userName);

/**
   Semantically identical fsl_deck_B_set() but sets the W-card.
   content must be the page content of the Wiki page or Event this
   change is being applied to.
*/
FSL_EXPORT int fsl_deck_W_set( fsl_deck * mf, char const *content, fsl_int_t len);

/**
   Must be called to initialize a newly-created/allocated deck
   instance. This function clears out all contents of the d
   parameter except for its (f, type, allocStamp) members, sets its
   (f, type) members, and leaves d->allocStamp intact.

   Note that, prior to calling this, the deck _must_ have been cleanly
   initialized via copying from fsl_deck_empty or (depending on the
   context) fsl_deck_empty_m or results are undefined.
*/
FSL_EXPORT void fsl_deck_init( fsl_cx * cx, fsl_deck * d,
                               fsl_satype_e type );

/**
   Returns true if d contains data for all _required_ cards, as
   determined by the value of d->type, else returns false. It returns
   false if d->type==FSL_SATYPE_ANY, as that is a placeholder value
   intended to be re-set by the deck's user.

   If it returns false, d->f's error state will help a description of
   the problem.

   The library calls this as needed, but clients may, if they want
   to. Note, however, that for FSL_SATYPE_CHECKIN decks it may fail
   if the deck has not been fsl_deck_unshuffle()d yet because the
   R-card gets calculated there (if needed).

   As a special case, d->f is configured to calculate R-cards,
   d->type==FSL_SATYPE_CHECKIN, AND d->R is not set, this will fail
   (with a descriptive error message).

   Another special case: for FSL_SATYPE_CHECKIN decks, if no
   F-cards are in th deck then an R-card is required to avoid a
   potental (admittedly harmless) syntactic ambiguity with
   FSL_SATYPE_CONTROL artifacts. The only legal R-card for a
   checkin with no F-cards has the initial MD5 hash state value
   (defined in the constant FSL_MD5_INITIAL_HASH), and that
   precondition is checked in this routine. fsl_deck_unshuffle()
   recognizes this case and adds the initial-state R-card, so
   clients normally need not concern themselves with this. If d has
   F-cards, whether or not an R-card is required depends on
   whether d->f is configured to require them or not.

   Enough about the R-card. In all other cases not described above,
   R-cards are not required (and they are only ever required on
   FSL_SATYPE_CHECKIN manifests).

   Though fossil(1) does not technically require F-cards in
   FSL_SATYPE_CHECKIN decks, so far none of the Fossil developers
   have found a use for a checkin without F-cards except the
   initial empty checkin. Additionally, a checkin without F-cards
   is potentially syntactically ambiguous (it could be an EVENT or
   ATTACHMENT artifact if it has no F- or R-card). So... this
   library _normally_ requires that CHECKIN decks have at least one
   F-card. This function, however, does not consider F-cards to be
   strictly required.
*/
FSL_EXPORT bool fsl_deck_has_required_cards( fsl_deck const * d );

/**
   Prepares the given deck for output by ensuring that cards
   which need to be sorted are sorted, and it may run some
   last-minute validation checks.

   The cards which get sorted are: F, J, M, Q, T. The P-card list is
   _not_ sorted - the client is responsible for ensuring that the
   primary parent is added to that list first, and after that the
   ordering is largely irrelevant. It is not possible for the library
   to determine a proper order for P-cards, nor to validate that order
   at input-time.

   If calculateRCard is true and fsl_card_is_legal(d,'R') then this
   function calculates the R-card for the deck. The R-card
   calculation is _extremely_ memory-hungry but adds another level
   of integrity checking to Fossil. If d->type is not
   FSL_SATYPE_MANIFEST then calculateRCard is ignored.

   If calculateRCard is true but no F-cards are present AND d->type is
   FSL_SATYPE_CHECKIN then the R-card is set to the initial MD5 hash
   state (the only legal R-card value for an empty F-card list). (This
   is necessary in order to prevent a deck-type ambiguity in one
   corner case.)

   The R-card, if used, must be calculated before
   fsl_deck_output()ing a deck containing F-cards. Clients may
   alternately call fsl_deck_R_calc() to calculate the R card
   separately, but there is little reason to do so. There are rare
   cases where the client can call fsl_deck_R_set()
   legally. Historically speaking the R-card was required when
   F-cards were used, but it was eventually made optional because
   (A) the memory cost and (B) it's part of a 3rd or 4th level of
   integrity-related checks, and is somewhat superfluous.

   @see fsl_deck_output()
   @see fsl_deck_save()
*/
FSL_EXPORT int fsl_deck_unshuffle( fsl_deck * d, bool calculateRCard );

/**
   Renders the given control artifact's contents to the given output
   function and calculates any cards which cannot be calculated until
   the contents are complete (namely the R-card and Z-card).

   If both (output, outputState) are NULL then d->f's outputer is
   used.

   The given deck is "logically const" but traversal over F-cards and
   baselines requires non-const operations. To keep this routine from
   requiring an undue amount of pre-call effort on the client's part,
   it also takes care of calling fsl_deck_unshuffle() to ensure that
   all of the deck's cards are in order. (If the deck has no R card,
   but has F-cards, and d->f is configured to generate R-cards, then
   unshuffling will also calculate the R-card.)

   Returns 0 on success, FSL_RC_MISUSE if !d or !d->f or !out. If
   out() returns non-0, output stops and that code is
   returned. outputState is passed as the first argument to out() and
   out() may be called an arbitrary number of times by this routine.

   Returns FSL_RC_SYNTAX if fsl_deck_has_required_cards()
   returns false.

   On errors more serious than argument validation, the deck's
   context's (d->f) error state is updated.

   The exact structure of the ouput depends on the value of
   d->type, and FSL_RC_TYPE is returned if this function cannot
   figure out what to do with the given deck's type.

   @see fsl_deck_unshuffle()
   @see fsl_deck_save()
*/
FSL_EXPORT int fsl_deck_output(fsl_deck * d, fsl_output_f out,
                               void * outputState);

/**
   Emits a JSON representation of the given deck to the given output
   routine. Returns 0 on success, non-0 on error. Any error codes,
   aside from FSL_RC_OOM, will have come from the given callback.

   The output is intended to be compatible with fossil(1)'
   artifact_to_json() (any deviation is a bug). The output is compact,
   not nicely-formatted. It can be sent to sqlite's json_pretty() to
   format it.

   If d->rid<=0, e.g. because it was just processed by
   fsl_deck_parse(), the resulting JSON object will have a "uuid" of
   null. If that property is important, it is up to the caller to set
   it before calling this.
*/
FSL_EXPORT int fsl_deck_to_json(fsl_deck const * d, fsl_output_f fn, void * fState);

/**
   Saves the given deck into f's repository database as new control
   artifact content. If isPrivate is true then the content is
   marked as private, otherwise it is not. Note that isPrivate is a
   suggestion and might be trumped by existing state within f or
   its repository, and such a trumping is not treated as an
   error. e.g. tags are automatically private when they tag private
   content.

   Before saving, the deck is passed through fsl_deck_unshuffle()
   and fsl_deck_output(), which will fail for a variety of
   easy-to-make errors such as the deck missing required cards.
   For unshuffle purposes, the R-card gets calculated if the deck
   has any F-cards AND if the caller has not already set/calculated
   it AND if f's FSL_CX_F_CALC_R_CARD flag is set (it is on by
   default for historical reasons, but this may change at some
   point).

   Returns 0 on success, the usual non-0 suspects on error.

   - If d->rid is positive, it is assumed to refer to a valid
   `blob.rid` and this function returns 0 without side-effects. It
   does _not_ attempt to re-save such a record, as that could very
   possible result in a new hash (and would definitely do so if the
   deck's state had been modified in the slightest bit (literally
   "bit")). It should arguably return FSL_RC_ALREADY_EXISTS for this
   case, but does not do so for historical reasons.

   - PREVIOUSLY (prior to 2021-10-20), we assumed such a deck referred
   to a phantom which we would then save, but that behaviour seems to
   be misguided, as we only ever want to save newly-created decks, not
   decks created from existing content.

   On success, d->rid be set to the new record's RID. It will only be
   set on success because it would otherwise refer to a db record
   which get destroyed when the transaction rolls back.

   After saving, the deck will be cross-linked to update any
   relationships described by the deck.

   The save operation happens within a transaction, of course, and
   on any sort of error, db-side changes are rolled back. Note that
   it _is_ legal to start the transaction before calling this,
   which effectively makes this operation part of that transaction.

   This function will fail with FSL_RC_ACCESS if d is a delta manifest
   (has a B-card) and d->f's forbid-delta-manifests configuration
   option is set to a truthy value. See
   fsl_repo_forbids_delta_manifests().

   Maintenance reminder: this function also does a very small bit of
   artifact-type-specific processing.

   @see fsl_deck_output()
*/
FSL_EXPORT int fsl_deck_save( fsl_deck * d, bool isPrivate );

/**
   Parses src as Control Artifact content and populates d with it.

   d will be cleaned up before parsing if it has any contents,
   retaining its d->f member (which must be set to non-NULL by the
   caller for error-reporting purposes).

   This function _might_ take over the contents of the source
   buffer. If it does, after returning src->mem will be NULL, and all
   other members will be reset to their default state. This function
   only takes over the contents if it decides to implement certain
   memory optimizations.

   Ownership of src itself is never changed by this function, only
   the ownership of its contents. On success, this function always
   clears the buffer's contents, possibly (but not necessarily)
   transfering ownership of them to the deck.

   In any case, the content of the source buffer is (normally)
   modified by this function because (A) that simplifies tokenization
   greatly, (B) saves us having to make another copy to work on, (C)
   the original implementation did it that way, (D) because in
   historical use the source is normally thrown away after parsing,
   anyway, and (E) in combination with taking ownership of src's
   contents it allows us to optimize away some memory allocations by
   re-using the internal memory of the buffer. This function never
   changes src's size, but it mutilates its contents (injecting NUL
   bytes as token delimiters).

   If d->type is _not_ FSL_SATYPE_ANY when this is called, then
   this function requires that the input to be of that type. We can
   fail relatively quickly in that case, and this can be used to
   save some downstream code some work. Note that the initial type
   for decks created using fsl_deck_malloc() or copy-initialized
   from ::fsl_deck_empty is FSL_SATYPE_ANY, so normally clients do
   not need to set this (unless they want to, as a small
   optimization).

   On success it returns 0 and d will be updated with the state from
   the input artifact and the contents of the source buffer will
   either be cleared or taken over by d. (Ideally, outputing d via
   fsl_deck_output() will produce a lossless copy of the original, but
   timestamp granularity might prevent that.)

   On error, if there is error information to propagate beyond the
   result code then it is stored in d->f. Whether or not such error
   info is propagated depends on the type of error, but anything more
   trivial than invalid arguments will be noted there. On error, the
   source buffer's contents _might_, depending on which phase the
   error happened, be left in place but may have been modified by the
   parsing process.  If the error happens after _certain_ parsing
   steps then the deck will be required to take over the buffer's
   memory in order to keep memory management sane. On error, clients
   should always eventually pass the source buffer to
   fsl_buffer_clear(). On success its contents are guaranteed to have
   been cleared or or transfered to the destination deck when this
   function returns.

   d might be partially populated on error, so regardless of success
   or failure, the client must eventually pass d to
   fsl_deck_finalize() to free its memory.

   Error result codes include:

   - FSL_RC_SYNTAX on syntax errors. Fossil's tried-and-true approach
     to determine "is this an artifact?" is "can it pass through
     fsl_deck_parse()?" This result code simply means that the input
     is, strictly speaking, not a fossil artifact. In some contexts
     this condition must be caught and treated as not-an-error (but
     also not an artifact).

   - FSL_RC_MISUSE if any pointer argument is NULL or d->f is NULL.

   - FSL_RC_CONSISTENCY if validation of a Z-card fails. This is a
     more specialized form of FSL_RC_SYNTAX but indicates that the
     artifact is (or may be) well-formed but has an incorrect hash.
     This check happens relatively early in the parsing process, but
     after this function has uses fsl_might_be_artifact() to do a
     basic sniff-test. In practice, this error "cannot happen" unless
     the source buffer has been manually manipulated. Whether or not
     client-side code wants to treat this as FSL_RC_SYNTAX (i.e. "not
     an artifact but not an error") is up to the client

   - Any number of errors coming from the allocator, database, or
     fsl_deck APIs used here.

   ACHTUNG API CHANGE: prior to 2021-10-20, this routine set d->rid
   (and the now-removed d->uuid) based on the hash of the input buffer
   if a matching record could be found in the db. That proved to be
   a huge performance hit and was removed.

   Maintenance reminder: in keeping with fossil's "if it quacks like
   an artifact, it is an artifact, else it's not" approach to
   determining whether opaque blobs are artifacts, this function
   _must_ continue to return FSL_RC_SYNTAX to indicate that "it
   doesn't quack like an artifact but there's otherwise nothing
   wrong," which downstream code must be able to rely upon as the
   input being a non-artifact. More serious errors, e.g. FSL_RC_OOM,
   are (of course) to be propagated back.

   @see fsl_deck_parse2()
*/
FSL_EXPORT int fsl_deck_parse(fsl_deck * d, fsl_buffer * src);

/**
    This variant of fsl_deck_parse() works identically to that
    function except for the 3rd argument.

    If you happen to know the _correct_ RID for the deck being parsed,
    pass it as the rid argument, else pass 0. A negative value will
    result in a FSL_RC_RANGE error. This value is (or may be) only
    used as an optimization in this function and/or downstream
    functions. Passing a positive value will cause d->f to do a cache
    lookup which may avoid it having to parse the deck at all.
*/
FSL_EXPORT int fsl_deck_parse2(fsl_deck * d, fsl_buffer * src, fsl_id_t rid);

/**
   Quickly determines whether the content held by the given buffer
   "might" be a structural artifact. It performs a fast sanity check
   for prominent features which can be checked either in O(1) or very
   short O(N) time (with a fixed N). If it returns false then the
   given buffer's contents are, with 100% certainty, _not_ a
   structural artifact. If it returns true then they _might_ be, but
   being 100% certain requires passing the contents to
   fsl_deck_parse() to fully parse them.
*/
FSL_EXPORT bool fsl_might_be_artifact(fsl_buffer const * src);

/**
   Loads the content from given rid and tries to parse it as a
   Fossil artifact. If rid==0 the current checkout (if opened) is
   used. (Trivia: there can never be a checkout with rid==0 but
   rid==0 is sometimes valid for an new/empty repo devoid of
   commits.) If type==FSL_SATYPE_ANY then it will allow any type of
   control artifact, else it returns FSL_RC_TYPE if the loaded
   artifact is of the wrong type.

   Returns 0 on success. Results are undefined if f or d are NULL. The
   potential error result codes include, but are not limited to:

   - FSL_RC_OOM

   - FSL_RC_RANGE if rid is negative or is 0 and no checkout is
     opened.

   - FSL_RC_TYPE if `type` is not `FSL_SATYPE_ANY` and the loaded
     result is of any artifact type other than `type`.

   d must be properly initialized, either copy-initialized from
   fsl_deck_empty or populated by the library's API. This function
   will (re)initialize it and may leave it partially populated on
   error. The caller must eventually pass it to fsl_deck_finalize()
   resp. fsl_deck_clean() regardless of success or error. This
   function "could" clean it up on error, but leaving it partially
   populated makes debugging easier.

   f's error state may be updated on error (for anything more
   serious than basic argument validation errors).

   On success this function sets d->rid to rid.

   @see fsl_deck_load_sym()
*/
FSL_EXPORT int fsl_deck_load_rid( fsl_cx * f, fsl_deck * d,
                                  fsl_id_t rid, fsl_satype_e type );

/**
   A convenience form of fsl_deck_load_rid() which uses
   fsl_sym_to_rid() to convert symbolicName into an artifact RID.  See
   fsl_deck_load_rid() for the symantics of the first, second, and
   fourth arguments, as well as the return value. See fsl_sym_to_rid()
   for the allowable values of symbolicName.

   @see fsl_deck_load_rid()
*/
FSL_EXPORT int fsl_deck_load_sym( fsl_cx * f, fsl_deck * d,
                                  char const * symbolicName,
                                  fsl_satype_e type );

/**
   Loads the baseline manifest specified in d->B.uuid, if any and if
   necessary. Returns 0 on success. If d->B.baseline is already loaded
   or d->B.uuid is NULL (in which case there is no baseline), it
   returns 0 and has no side effects.

   Neither argument may be NULL and d must be a fully-populated
   object, complete with a proper d->rid, before calling this.

   On success 0 is returned. If d->B.baseline is NULL then
   it means that d has no baseline manifest (and d->B.uuid will be NULL
   in that case). If d->B.baseline is not NULL then it is owned by
   d and will be cleaned up when d is cleaned/finalized.

   Error codes include, but are not limited to:

   - FSL_RC_MISUSE if !d->f.

   - FSL_RC_NOT_A_REPO if d->f has no opened repo db.

   - FSL_RC_RANGE if d->rid<=0, but that code might propagate up from
   a lower-level call as well.

   On non-trivial errors d->f's error state will be updated to hold
   a description of the problem.

   Some misuses trigger assertions in debug builds.
*/
FSL_EXPORT int fsl_deck_baseline_fetch( fsl_deck * d );

/**
   A callback interface for manifest crosslinking, so that we can farm
   out the updating of the event table (and similar bookkeeping) as
   manifests are crosslinked. Each callback registered via
   fsl_xlink_listener() will be called at the end of the so-called
   crosslinking process, which is run every time an artifact is
   processed for d->f's repository database, passed the deck being
   crosslinked and the client-provided state which was registered with
   fsl_xlink_listener(). Note that the deck object itself holds other
   state useful for crosslinking, like the blob.rid value of the deck
   and its fsl_cx instance.

   If an implementation is only interested in a specific type of
   artifact, it must check d->type and return 0 if it's an
   "uninteresting" type.

   Implementations must return 0 on success or some other fsl_rc_e
   value on error. Returning non-0 causes the database transaction
   for the crosslinking operation to roll back, effectively
   cancelling whatever pending operation triggered the
   crosslink. If any callback fails, processing stops immediately -
   no other callbacks are executed.

   Implementations which want to report more info than an integer
   should call fsl_cx_err_set() to set d->f's error state, as that
   will be propagated up to the code which initiated the failed
   crosslink.

   ACHTUNG and WARNING: the fsl_deck parameter "really should" be
   const, but certain operations on a deck are necessarily non-const
   operations. That includes, but may not be limited to:

   - Iterating over F-cards, which requires calling
     fsl_deck_F_rewind() before doing so. Traversal of F-cards
     internally uses a necessarily-mutable cursor.

   - Loading a checkin's baseline (required for F-card iteration and
     performed automatically by fsl_deck_F_rewind()).

   Aside from such iteration-related mutable state, it is STRICTLY
   ILLEGAL to modify a deck's artifact-related state while it is
   undergoing crosslinking. Violating that will lead to undefined
   results.

   Potential TODO: add some client-opaque state to decks so that they
   can be flagged as "being crosslinked" and fail mutation operations
   such as card adders/setters.

   @see fsl_xlink_listener()
*/
typedef int (*fsl_deck_xlink_f)(fsl_deck * d, void * state);

/**
    A type for holding state for artifact crosslinking callbacks.
*/
struct fsl_xlinker {
  /** Human-readable name of the crosslinker, noting that each
      registered crosslinker must have a unique name. Registering a
      crosslinker with the same name as an existing one replaces that
      one.
  */
  char const * name;
  /** Callback function. */
  fsl_deck_xlink_f f;
  /** State for this->f's last argument. */
  void * state;
};
typedef struct fsl_xlinker fsl_xlinker;

/** Empty-initialized fsl_xlinker struct, intended for const-copy
    intialization. */
#define fsl_xlinker_empty_m {NULL,NULL,NULL}

/** Empty-initialized fsl_xlinker struct, intended for copy intialization. */
extern const fsl_xlinker fsl_xlinker_empty;

/**
    A list of fsl_xlinker instances.
*/
struct fsl_xlinker_list {
  /** Number of used items in this->list. */
  fsl_size_t used;
  /** Number of slots allocated in this->list. */
  fsl_size_t capacity;
  /** Array of this->used elements. */
  fsl_xlinker * list;
};
typedef struct fsl_xlinker_list fsl_xlinker_list;

/** Empty-initializes fsl_xlinker_list struct, intended for
    const-copy intialization. */
#define fsl_xlinker_list_empty_m {0,0,NULL}

/** Empty-initializes fsl_xlinker_list struct, intended for copy intialization. */
extern const fsl_xlinker_list fsl_xlinker_list_empty;

/**
    Searches f's crosslink callbacks for an entry with the given
    name and returns that entry, or NULL if no match is found.  The
    returned object is owned by f.
*/
fsl_xlinker * fsl_xlinker_by_name( fsl_cx * f, char const * name );

/**
   Adds the given function as a "crosslink callback" for the given
   Fossil context. The callback is called at the end of a successful
   "artifact crosslink" operation and provides a way for the client to
   perform their own work based on the app having crosslinked an
   artifact. Crosslinking happens when artifacts are saved or upon a
   rebuild operation.

   This function returns 0 on success, non-0 on error. Behaviour is
   undefined if any of the first 3 arguments are NULL.

   Crosslink callbacks are called at the end of the core crosslink
   steps, in the order they are registered, with the caveat that if a
   listener is overwritten by another with the same name, the new
   entry retains the older one's position in the list. The library may
   register its own before the client gets a chance to.

   If _any_ crosslinking callback fails (returns non-0) then the
   _whole_ crosslinking fails and is rolled back (which may very
   well include pending tags/commits/whatever getting rolled back).

   The state parameter has no meaning for this function, but is
   passed on as the final argument to cb(). If not NULL, cbState
   "may" be required to outlive f, depending on cbState's exact
   client-side internal semantics/use, as there is currently no API
   to remove registered crosslink listeners.

   The name must be non-NULL/empty. If a listener is registered with a
   duplicate name then the first one is replaced. This function does
   not copy the name bytes - they are assumed to be static or
   otherwise to live at least as long as f. The name may be
   arbitrarily long, but must have a terminating NUL byte. It is
   recommended that clients choose a namespace/prefix to apply to the
   names they register. The library reserves the prefix "fsl/" for
   its own use, and will happily overwrite client-registered entries
   with the same names. The name string need not be stable across
   application sessions and maybe be a randomly-generated string.

   Caveat: some obscure artifact crosslinking steps do not happen
   unless crosslinking takes place in the context of a
   fsl__crosslink_begin() and fsl__crosslink_end()
   session. Thus, at the time client-side crosslinker callbacks are
   called, certain crosslinking state in the database may still be
   pending. It is as yet unclear how best to resolve that minor
   discrepancy, or whether it even needs resolving.

   As a rule, it is important that crosslink handler checks the
   deck->type field of the deck they are passed, and return 0, without
   side effects, if the type is not specifically handled by that
   handler. Every crosslink handler is passed every crosslinked
   artifact, but it's rare for crosslink handlers to handle more than
   one type of artifact, except perhaps for purposes of notifying a
   user that some progress is being made.

   Default (overrideable) crosslink handlers:

   The library internally splits crosslinking of artifacts into two
   parts: the main one (which clients cannot modify) handles the
   database-level linking of relational state implied by a given
   artifact. The secondary one adds an entry to the "event" table,
   which is where Fossil's timeline lives. The crosslinkers for the
   timeline updates may be overridden by clients by registering
   a crosslink listener with the following names:

   - Attachment artifacts: "fsl/attachment/timeline"

   - Checkin artifacts: "fsl/checkin/timeline"

   - Control artifacts: "fsl/control/timeline"

   - Forum post artifacts: "fsl/forumpost/timeline"

   - Technote artifacts: "fsl/technote/timeline"

   - Wiki artifacts: "fsl/wiki/timeline"

   A context registers listeners under those names when it
   initializes, and clients may override them at any point after that.

   Sidebar: due to how tickets are crosslinked (_after_ the general
   crosslinking phase is actually finished and requiring state which
   other crosslinkers do not), it is not currently possible to
   override the ticket crosslink handler. Thus the core ticket
   crosslinker will always run, and update the [event] table, but a
   custom crosslinker may overwrite the resulting [event] table
   entries (in particular, the comment). Determining whether/how
   ticket crosslinking can be restructured to be consistent with the
   other types is on the TODO list.

   Caveat: updating the timeline requires a bit of knowledge about the
   Fossil DB schema and/or conventions. Updates for certain types,
   e.g. attachment/control/forum post, is somewhat more involved and
   updating the timeline for wiki comments requires observing a "quirk
   of conventions" for labeling such comments, such that they will
   appear properly when the main fossil app renders them. That said,
   the only tricky parts of those updates involve generating the
   "correct" comment text. So long as the non-comment parts are
   updated properly (that part is easy to do), fossil can function
   with it.  The timeline comment text/links are soley for human
   consumption. Fossil makes much use of the "event" table internally,
   however, so the rest of that table must be properly populated.

   Because of that caveat, clients may, rather than overriding the
   defaults, install their own crosslink listners which ammend the
   state applied by the default ones. e.g. add a listener which
   watches for checkin updates and replace the default-installed
   comment with one suitable for your application, leaving the rest of
   the db state in place. At its simplest, that looks more or less
   like the following code (inside a fsl_deck_xlink_f() callback):

   ```
   int rc = fsl_db_exec(fsl_cx_db_repo(deck->f),
                        "UPDATE event SET comment=%Q "
                        "WHERE objid=%"FSL_ID_T_PFMT,
                        "the new comment.", deck->rid);
   ```
*/
FSL_EXPORT int fsl_xlink_listener( fsl_cx * f, char const * name,
                                   fsl_deck_xlink_f cb, void * cbState );


/**
   For the given blob.rid value, returns the blob.size value of
   that record via *rv. Returns 0 or higher on success, -1 if a
   phantom record is found, -2 if no entry is found, or a smaller
   negative value on error (dig around the sources to decode them -
   this is not expected to fail unless the system is undergoing a
   catastrophe).

   @see fsl_content_raw()
   @see fsl_content_get()
   @see fsl_content_size_v2()
*/
FSL_EXPORT fsl_int_t fsl_content_size( fsl_cx * f, fsl_id_t blobRid );

/**
   Given a blob RID, this function extracts its size from the
   `blob.rid` db field. On success, returns 0 and sets *pOut to
   the new size. Negative sizes mean the blob is a "phantom."

   On error it returns non-0 and updates f's error state.

   @see fsl_content_size()
*/
FSL_EXPORT int fsl_content_size_v2(fsl_cx * f, fsl_id_t rid,
                                   fsl_int_t *pOut);

/**
   For the given blob.rid value, fetches the content field of that
   record and overwrites tgt's contents with it (reusing tgt's
   memory if it has any and if it can). The blob's contents are
   uncompressed if they were stored in compressed form. This
   extracts a raw blob and does not apply any deltas - use
   fsl_content_get() to fully expand a delta-stored blob.

   By and large, client-side code will want fsl_content_get() over
   this functino.

   Returns 0 on success. On error tgt might be partially updated,
   e.g. it might be populated with compressed data instead of
   uncompressed. On error tgt's contents should be recycled
   (e.g. fsl_buffer_reuse()) or discarded (e.g. fsl_buffer_clear()) by
   the client. Returns FSL_RC_RANGE if blobRid<=0, FSL_RC_NOT_A_REPO
   if f has no repo opened, FSL_RC_OOM on allocation error, or
   potentially any number of other codes via the db layer.

   Results are undefined if any pointer argument is NULL.

   @see fsl_content_get()
   @see fsl_content_size()
*/
FSL_EXPORT int fsl_content_raw( fsl_cx * f, fsl_id_t blobRid,
                                 fsl_buffer * tgt );

/** @deprecated

    Older (deprecated) name of fsl_content_raw().
*/
#define fsl_content_blob fsl_content_raw

/**
   Functionally similar to fsl_content_raw() but does a lot of
   work to ensure that the returned blob is expanded from its
   deltas, if any. The tgt buffer's memory, if any, will be
   replaced/reused if it has any.

   Returns 0 on success. There are no less than 50 potental
   different errors, so we won't bother to list them all. On error
   tgt might be partially populated. The basic error cases are:

   - FSL_RC_MISUSE if !tgt or !f.

   - FSL_RC_RANGE if rid<=0 or if an infinite loop is discovered in
   the repo delta table links (that is a consistency check to avoid
   an infinite loop - that condition "cannot happen" because the
   verify-before-commit logic catches that error case).

   - FSL_RC_NOT_A_REPO if f has no repo db opened.

   - FSL_RC_NOT_FOUND if the given rid is not in the repo db.

   - FSL_RC_OOM if an allocation fails.


   @see fsl_content_raw()
   @see fsl_content_size()
*/
FSL_EXPORT int fsl_content_get( fsl_cx * f, fsl_id_t blobRid,
                                fsl_buffer * tgt );

/**
   Uses fsl_sym_to_rid() to convert sym to a record ID, then
   passes that to fsl_content_get(). Returns 0 on success.
*/
FSL_EXPORT int fsl_content_get_sym( fsl_cx * f, char const * sym,
                                    fsl_buffer * tgt );

/**
   Returns true if the given rid is marked as PRIVATE in f's current
   repository. Returns false (0) on error or if the content is not
   marked as private.
*/
FSL_EXPORT bool fsl_content_is_private(fsl_cx * f, fsl_id_t rid);

/**
   Marks the given rid public, if it was previously marked as
   private. Returns 0 on success, non-0 on error.

   Note that it is not possible to make public content private.
*/
FSL_EXPORT int fsl_content_make_public(fsl_cx * f, fsl_id_t rid);

/**
   Generic callback interface for visiting decks. The interface
   does not generically require that d survive after this call
   returns.

   Implementations must return 0 on success, non-0 on error. Some
   APIs using this interface may specify that FSL_RC_BREAK can be
   used to stop iteration over a loop without signaling an error.
   In such cases the APIs will translate FSL_RC_BREAK to 0 for
   result purposes, but will stop looping over whatever it is they
   are looping over.

   Note that the passed-in deck "should" be const but is not because
   iterating over a deck's F-cards requires non-const state.
*/
typedef int (*fsl_deck_visitor_f)( fsl_cx * f, fsl_deck * d,
                                   void * state );

/**
   For each unique wiki page name in f's repostory, this calls
   cb(), passing it the manifest of the most recent version of that
   page. The callback should return 0 on success, FSL_RC_BREAK to
   stop looping without an error, or any other non-0 code
   (preferably a value from fsl_rc_e) on error.

   The 3rd parameter has no meaning for this function but it is
   passed on as-is to the callback.

   ACHTUNG: the deck passed to the callback is transient and will
   be cleaned up after the callback has returned, so the callback
   must not hold a pointer to it or its contents.

   @see fsl_wiki_load_latest()
   @see fsl_wiki_latest_rid()
   @see fsl_wiki_names_get()
   @see fsl_wiki_page_exists()
*/
FSL_EXPORT int fsl_wiki_foreach_page( fsl_cx * f, fsl_deck_visitor_f cb, void * state );

/**
   Fetches the most recent RID for the given wiki page name and
   assigns *newId (if it is not NULL) to that value. Returns 0 on
   success, FSL_RC_MISUSE if !f or !pageName, FSL_RC_RANGE if
   !*pageName, and a host of other potential db-side errors
   indicating more serious problems. If no such page is found,
   newRid is not modified and this function returns 0 (as opposed
   to FSL_RC_NOT_FOUND) because that simplifies usage (so far).

   On error *newRid is not modified.

   @see fsl_wiki_load_latest()
   @see fsl_wiki_foreach_page()
   @see fsl_wiki_names_get()
   @see fsl_wiki_page_exists()
*/
FSL_EXPORT int fsl_wiki_latest_rid( fsl_cx * f, char const * pageName, fsl_id_t * newRid );

/**
   Loads the artifact for the most recent version of the given wiki page,
   populating d with its contents.

   Returns 0 on success. On error d might be partially populated,
   so it needs to be passed to fsl_deck_finalize() regardless of
   whether this function succeeds or fails.

   Returns FSL_RC_NOT_FOUND if no page with that name is found.

   @see fsl_wiki_latest_rid()
   @see fsl_wiki_names_get()
   @see fsl_wiki_page_exists()
*/
FSL_EXPORT int fsl_wiki_load_latest( fsl_cx * f, char const * pageName, fsl_deck * d );

/**
   Returns true (non-0) if f's repo database contains a page with the
   given name, else false.

   @see fsl_wiki_load_latest()
   @see fsl_wiki_latest_rid()
   @see fsl_wiki_names_get()
   @see fsl_wiki_names_get()
*/
FSL_EXPORT bool fsl_wiki_page_exists(fsl_cx * f, char const * pageName);

/**
   A helper type for use with fsl_wiki_save(), intended primarily
   to help client-side code readability somewhat.
*/
enum fsl_wiki_save_mode_t {
/**
   Indicates that fsl_wiki_save() must only allow the creation of
   a new page, and must fail if such an entry already exists.
*/
FSL_WIKI_SAVE_MODE_CREATE = -1,
/**
   Indicates that fsl_wiki_save() must only allow the update of an
   existing page, and will not create a branch new page.
*/
FSL_WIKI_SAVE_MODE_UPDATE = 0,
/**
   Indicates that fsl_wiki_save() must allow both the update and
   creation of pages. Trivia: "upsert" is a common SQL slang
   abbreviation for "update or insert."
*/
FSL_WIKI_SAVE_MODE_UPSERT = 1
};

typedef enum fsl_wiki_save_mode_t fsl_wiki_save_mode_t;

/**
   Saves wiki content to f's repository db.

   pageName is the name of the page to update or create.

   b contains the content for the page.

   userName specifies the user name to apply to the change. If NULL
   or empty then fsl_cx_user_get() or fsl_user_name_guess() are
   used (in that order) to determine the name.

   mimeType specifies the mime type for the content (may be NULL).
   Mime type names supported directly by fossil(1) include (as of
   this writing): text/x-fossil-wiki, text/x-markdown,
   text/plain

   Whether or not this function is allowed to create a new page is
   determined by creationPolicy. If it is
   FSL_WIKI_SAVE_MODE_UPDATE, this function will fail with
   FSL_RC_NOT_FOUND if no page with the given name already exists.
   If it is FSL_WIKI_SAVE_MODE_CREATE and a previous version _does_
   exist, it fails with FSL_RC_ALREADY_EXISTS. If it is
   FSL_WIKI_SAVE_MODE_UPSERT then both the save-exiting and
   create-new cases are allowed. In summary:

   - use FSL_WIKI_SAVE_MODE_UPDATE to allow updates to existing pages
   but disallow creation of new pages,

   - use FSL_WIKI_SAVE_MODE_CREATE to allow creating of new pages
   but not of updating an existing page.

   - FSL_WIKI_SAVE_MODE_UPSERT allows both updating and creating
   a new page on demand.

   Returns 0 on success, or any number fsl_rc_e codes on error. On
   error no content changes are saved, and any transaction is
   rolled back or a rollback is scheduled if this function is
   called while a transaction is active.


   Potential TODO: add an optional (fsl_id_t*) output parameter
   which gets set to the new record's RID.

   @see fsl_wiki_page_exists()
   @see fsl_wiki_names_get()
*/
FSL_EXPORT int fsl_wiki_save(fsl_cx * f, char const * pageName,
                  fsl_buffer const * b, char const * userName,
                  char const * mimeType, fsl_wiki_save_mode_t creationPolicy );

/**
   Fetches the list of all wiki page names in f's current repo db
   and appends them as new (char *) strings to tgt. On error tgt
   might be partially populated (but this will only happen on an
   OOM or serious system-level error).

   It is up to the caller free the entries added to the list. Some
   of the possibilities include:

   ```
   fsl_list_visit( list, 0, fsl_list_v_fsl_free, NULL );
   fsl_list_reserve(list,0);
   // Or:
   fsl_list_clear(list, fsl_list_v_fsl_free, NULL);
   // Or simply:
   fsl_list_visit_free( list, 1 );
   ```

*/
FSL_EXPORT int fsl_wiki_names_get( fsl_cx * f, fsl_list * tgt );

/**
   F-cards each represent one file entry in a Manifest Artifact (i.e.,
   a checkin version).

   All of the non-const pointers in this class are owned by the
   respective instance of the class OR by the fsl_deck which created
   it, and must neither be modified nor freed except via the
   appropriate APIs.
*/
struct fsl_card_F {
  /**
     UUID of the underlying blob record for the file. NULL for
     removed entries.
  */
  fsl_uuid_str uuid;
  /**
     Name of the file.
  */
  char * name;
  /**
     Previous name if the file was renamed, else NULL.
  */
  char * priorName;
  /**
     File permissions. Fossil only supports one "permission" per
     file, and it does not necessarily map to a real
     filesystem-level permission.

     @see fsl_fileperm_e
  */
  fsl_fileperm_e perm;

  /**
     An internal optimization. Do not mess with this.  When this is
     true, the various string members of this struct are not owned
     by this struct, but by the deck which created this struct. This
     is used when loading decks from storage - the strings are
     pointed to the original content data, rather than strdup()'d
     copies of it. fsl_card_F_clean() will DTRT and delete the
     strings (or not).
  */
  bool deckOwnsStrings;
};
/**
   Empty-initialized fsl_card_F structure, intended for use in
   initialization when embedding fsl_card_F in another struct or
   copy-initializing a const struct.
*/
#define fsl_card_F_empty_m {   \
  NULL/*uuid*/,                \
  NULL/*name*/,                \
  NULL/*priorName*/,           \
  0/*perm*/,                   \
  false/*deckOwnsStrings*/     \
}
FSL_EXPORT const fsl_card_F fsl_card_F_empty;

/**
   Represents a J card in a Ticket Control Artifact.
*/
struct fsl_card_J {
  /**
     If true, the new value should be appended to any existing one
     with the same key, else it will replace any old one.
  */
  bool append;
  /**
     For internal use only.
  */
  unsigned char flags;
  /**
     The ticket field to update. The bytes are owned by this object.
  */
  char * field;
  /**
     The value for the field. The bytes are owned by this object.
  */
  char * value;
};
/** Empty-initialized fsl_card_J struct. */
#define fsl_card_J_empty_m {false,0,NULL, NULL}
/** Empty-initialized fsl_card_J struct. */
FSL_EXPORT const fsl_card_J fsl_card_J_empty;

/**
   Represents a tag in a Manifest or Control Artifact.
*/
struct fsl_card_T {
  /**
     The type of tag.
  */
  fsl_tagtype_e type;
  /**
     UUID of the artifact this tag is tagging. When applying a tag to
     a new checkin, this value is left empty (=NULL) and gets replaced
     by a '*' in the resulting control artifact.
  */
  fsl_uuid_str uuid;
  /**
     The tag's name. The bytes are owned by this object.
  */
  char * name;
  /**
     The tag's value. May be NULL/empty. The bytes are owned by
     this object.
  */
  char * value;
};
/** Defaults-initialized fsl_card_T instance. */
#define fsl_card_T_empty_m {FSL_TAGTYPE_INVALID, NULL, NULL,NULL}
/** Defaults-initialized fsl_card_T instance. */
FSL_EXPORT const fsl_card_T fsl_card_T_empty;

/**
   Types of cherrypick merges.
*/
enum fsl_cherrypick_type_e {
/** Sentinel value. */
FSL_CHERRYPICK_INVALID = 0,
/** Indicates a cherrypick merge. */
FSL_CHERRYPICK_ADD = 1,
/** Indicates a cherrypick backout. */
FSL_CHERRYPICK_BACKOUT = -1
};
typedef enum fsl_cherrypick_type_e fsl_cherrypick_type_e;

/**
   Represents a Q card in a Manifest or Control Artifact.
*/
struct fsl_card_Q {
  /** 0==invalid, negative==backed out, positive=cherrypicked. */
  fsl_cherrypick_type_e type;
  /**
     UUID of the target of the cherrypick. The bytes are owned by
     this object.
  */
  fsl_uuid_str target;
  /**
     UUID of the baseline for the cherrypick. The bytes are owned by
     this object.
  */
  fsl_uuid_str baseline;
};
/** Empty-initialized fsl_card_Q struct. */
#define fsl_card_Q_empty_m {FSL_CHERRYPICK_INVALID, NULL, NULL}
/** Empty-initialized fsl_card_Q struct. */
FSL_EXPORT const fsl_card_Q fsl_card_Q_empty;

/**
   Allocates a new J-card record instance

   On success it returns a new record which must eventually be
   passed to fsl_card_J_free() to free its resources. On
   error (invalid arguments or allocation error) it returns NULL.
   field may not be NULL or empty but value may be either.

   These records are immutable - the API provides no way to change
   them once they are instantiated.
*/
FSL_EXPORT fsl_card_J * fsl_card_J_malloc(bool isAppend,
                                          char const * field,
                                          char const * value);
/**
   Frees a J-card record created by fsl_card_J_malloc().
   Is a no-op if cp is NULL.
*/
FSL_EXPORT void fsl_card_J_free( fsl_card_J * cp );

/**
   Allocates a new fsl_card_T instance. If any of the pointer
   parameters are non-NULL, their values are assumed to be
   NUL-terminated strings, which this function copies.  Returns NULL
   on allocation error.  The returned value must eventually be passed
   to fsl_card_T_clean() or fsl_card_T_free() to free its resources.

   If uuid is not NULL and fsl_is_uuid(uuid) returns false then
   this function returns NULL. If it is NULL and gets assigned
   later, it must conform to fsl_is_uuid()'s rules or downstream
   results are undefined.

   @see fsl_card_T_free()
   @see fsl_card_T_clean()
   @see fsl_deck_T_add()
*/
FSL_EXPORT fsl_card_T * fsl_card_T_malloc(fsl_tagtype_e tagType,
                                          fsl_uuid_cstr uuid,
                                          char const * name,
                                          char const * value);
/**
   If t is not NULL, calls fsl_card_T_clean(t) and then passes t to
   fsl_free().

   @see fsl_card_T_clean()
*/
FSL_EXPORT void fsl_card_T_free(fsl_card_T *t);

/**
   Frees up any memory owned by t and clears out t's state,
   but does not free t.

   @see fsl_card_T_free()
*/
FSL_EXPORT void fsl_card_T_clean(fsl_card_T *t);

/**
   Allocates a new cherrypick record instance. The type argument must
   be one of FSL_CHERRYPICK_ADD or FSL_CHERRYPICK_BACKOUT.  target
   must be a valid UUID string. If baseline is not NULL then it also
   must be a valid UUID.

   On success it returns a new record which must eventually be
   passed to fsl_card_Q_free() to free its resources. On
   error (invalid arguments or allocation error) it returns NULL.

   These records are immutable - the API provides no way to change
   them once they are instantiated.
*/
FSL_EXPORT fsl_card_Q * fsl_card_Q_malloc(fsl_cherrypick_type_e type,
                                          fsl_uuid_cstr target,
                                          fsl_uuid_cstr baseline);
/**
   Frees a cherrypick record created by fsl_card_Q_malloc().
   Is a no-op if cp is NULL.
*/
FSL_EXPORT void fsl_card_Q_free( fsl_card_Q * cp );

/**
   Returns true (non-0) if f is not NULL and f has an opened repo
   which contains a checkin with the given rid, else it returns
   false.

   As a special case, if rid==0 then this only returns true
   if the repository currently has no content in the blob
   table.
*/
FSL_EXPORT char fsl_rid_is_a_checkin(fsl_cx * f, fsl_id_t rid);

/**
   Fetches the list of all directory names for a given checkin record
   id or (if rid is negative) the whole repo over all of its combined
   history. Each name entry in the list is appended to tgt. The
   results are reduced to unique names only and are sorted
   lexically. If addSlash is true then each entry will include a
   trailing slash character, else it will not. The list does not
   include an entry for the top-most directory.

   If rid is less than 0 then the directory list across _all_
   versions is returned. If it is 0 then the current checkout's RID
   is used (if a checkout is opened, otherwise a usage error is
   triggered). If it is positive then only directories for the
   given checkin RID are returned. If rid is specified, it is
   assumed to be the record ID of a commit (manifest) record, and
   it is impossible to distinguish between the results "invalid
   rid" and "empty directory list" (which is a legal result).

   On success it returns 0 and tgt will have a number of (char *)
   entries appended to it equal to the number of subdirectories in
   the repo (possibly 0).

   Returns non-0 on error, FSL_RC_MISUSE if !tgt, FSL_RC_NOT_A_REPO if
   f has no opened repository. On other errors error tgt might have
   been partially populated and the list contents should not be
   considered valid/complete. Results are undefined if f is NULL.

   Ownership of the returned strings is transfered to the caller,
   who must eventually free each one using
   fsl_free(). fsl_list_visit_free() is the simplest way to free
   them all at once.
*/
FSL_EXPORT int fsl_repo_dir_names( fsl_cx * f, fsl_id_t rid,
                                   fsl_list * tgt, bool addSlash );


/**
   ZIPs up a copy of the contents of a specific version from f's
   opened repository db. sym is the symbolic name for the checkin
   to ZIP. filename is the name of the ZIP file to output the
   result to. See fsl_zip_writer for details and caveats of this
   library's ZIP creation. If vRootDir is not NULL and not empty
   then each file injected into the ZIP gets that directory
   prepended to its name.

   If progressVisitor is not NULL then it is called once just before
   each file is processed, passed the F-card for the file about to be
   zipped and the progressState parameter. If it returns non-0,
   ZIPping is cancelled and that result code is returned. This is
   intended primarily for providing feedback on the zip progress, but
   could also be used to cancel the operation between files.

   As of 2021-09-05 this routine automatically adds the files
   (manifest, manifest.uuid, manifest.tags) to the zip file,
   regardless of repository-level settings regarding those
   pseudo-files (see fsl_ckout_manifest_write()). As there are no
   F-cards associated with those non-files, the progressVisitor is not
   called for those.

   BUG/FIXME: this function does not honor symlink content in a
   fossil-compatible fashion. If it encounters a symlink entry during
   ZIP generation, it will fail and f's error state will be updated
   with an explanation of this shortcoming.

   @see fsl_zip_writer
   @see fsl_card_F_visitor_f()
*/
FSL_EXPORT int fsl_repo_zip_sym_to_filename( fsl_cx * f, char const * sym,
                                             char const * vRootDir,
                                             char const * fileName,
                                             fsl_card_F_visitor_f progressVisitor,
                                             void * progressState);


/**
   Callback state for use with fsl_repo_extract_f() implementations
   to stream a given version of a repository's file's, one file at a
   time, to a client. Instances are never created by client code,
   only by fsl_repo_extract() and its delegates, which pass them to
   client-provided fsl_repo_extract_f() functions.
*/
struct fsl_repo_extract_state {
  /**
     The associated Fossil context.
  */
  fsl_cx * f;
  /**
     RID of the checkin version for this file. For a given call to
     fsl_repo_extract(), this number will be the same across all
     calls to the callback function.
  */
  fsl_id_t checkinRid;
  /**
     File-level blob.rid for fc. Can be used with, e.g.,
     fsl_mtime_of_manifest_file().
  */
  fsl_id_t fileRid;
  /**
     Client state passed to fsl_repo_extract(). Its interpretation
     is callback-implementation-dependent.
  */
  void * callbackState;
  /**
     The F-card being iterated over. This holds the repo-level
     metadata associated with the file, other than its RID, which is
     available via this->fileRid.

     Deleted files are NOT reported via the extraction process
     because reporting them accurately is trickier and more
     expensive than it could be. Thus this member's uuid field
     will always be non-NULL.

     Certain operations which use this class, e.g. fsl_repo_ckout()
     and fsl_ckout_update(), will temporarily synthesize an F-card to
     represent the state of a file update, in which case this object's
     contents might not 100% reflect any given db-side state. e.g.
     fsl_ckout_update() synthesizes an F-card which reflects the
     current state of a file after applying an update operation to it.
     In such cases, the fCard->uuid may refer to a repository-side
     file even though the hash of the on-disk file contents may differ
     because of, e.g., a merge.
  */
  fsl_card_F const * fCard;

  /**
     If the fsl_repo_extract_opt object which was used to initiate the
     current extraction has the extractContent member set to false,
     this will be a NULL pointer. If it's true, this member points to
     a transient buffer which holds the full, undelta'd/uncompressed
     content of fc's file record. The content bytes are owned by
     fsl_repo_extract() and are invalidated as soon as this callback
     returns, so the callback must copy/consume them immediately if
     needed.
  */
  fsl_buffer const * content;

  /**
     These counters can be used by an extraction callback to calculate
     a progress percentage.
  */
  struct {
    /** The current file number, starting at 1. */
    uint32_t fileNumber;
    /** Total number of files to extract. */
    uint32_t fileCount;
  } count;
};
typedef struct fsl_repo_extract_state fsl_repo_extract_state;

/**
   Initialized-with-defaults fsl_repo_extract_state instance, intended
   for const-copy initialization.
*/
#define fsl_repo_extract_state_empty_m {\
  NULL/*f*/, 0/*checkinRid*/, 0/*fileRid*/, \
  NULL/*state*/, NULL/*fCard*/, NULL/*content*/,    \
  {/*count*/0,0} \
}
/**
   Initialized-with-defaults fsl_repo_extract_state instance,
   intended for non-const copy initialization.
*/
FSL_EXPORT const fsl_repo_extract_state fsl_repo_extract_state_empty;

/**
   A callback type for use with fsl_repo_extract(). See
   fsl_repo_extract_state for the meanings of xstate's various
   members.  The xstate memory must be considered invalidated
   immediately after this function returns, thus implementations
   must copy or consume anything they need from xstate before
   returning.

   Implementations must return 0 on success. As a special case, if
   FSL_RC_BREAK is returned then fsl_repo_extract() will stop
   looping over files but will report it as success (by returning
   0). Any other code causes extraction looping to stop and is
   returned as-is to the caller of fsl_repo_extract().

   When returning an error, the client may use fsl_cx_err_set() to
   populate state->f with a useful error message which will
   propagate back up through the call stack.

   @see fsl_repo_extract()
*/
typedef int (*fsl_repo_extract_f)( fsl_repo_extract_state const * xstate );

/**
   Options for use with fsl_repo_extract().
*/
struct fsl_repo_extract_opt {
  /**
     The version of the repostitory to check out. This must be
     the blob.rid of a checkin artifact.
  */
  fsl_id_t checkinRid;
  /**
     The callback to call for each extracted file in the checkin.
     May not be NULL.
  */
  fsl_repo_extract_f callback;
  /**
     Optional state pointer to pass to the callback when extracting.
     Its interpretation is client-dependent.
  */
  void * callbackState;
  /**
     If true, the fsl_repo_extract_state::content pointer passed to
     the callback will be non-NULL and will contain the content of the
     file. If false, that pointer will be NULL. Such extraction is a
     relatively costly operation, so should only be enabled when
     necessary. Some uses cases can delay this decision until the
     callback and only fetch the content for cases which need it.
  */
  bool extractContent;
};

typedef struct fsl_repo_extract_opt fsl_repo_extract_opt;
/**
   Initialized-with-defaults fsl_repo_extract_opt instance, intended
   for intializing via const-copy initialization.
*/
#define fsl_repo_extract_opt_empty_m \
  {0/*checkinRid*/,NULL/*callback*/, \
   NULL/*callbackState*/,false/*extractContent*/}
/**
   Initialized-with-defaults fsl_repo_extract_opt instance,
   intended for intializing new non-const instances.
*/
FSL_EXPORT const fsl_repo_extract_opt fsl_repo_extract_opt_empty;

/**
   Iterates over the file content of a single checkin in a repository,
   sending the appropriate version of each file's contents to a
   client-specified callback.

   For each file in the given checkin, opt->callback() is passed a
   fsl_repo_extract_state instance containing enough information to,
   e.g., unpack the contents to a working directory, add it to a
   compressed archive, or send it to some other destination.

   Returns 0 on success, non-0 on error. It will fail if f has no
   opened repository db.

   If the callback returns any code other than 0 or FSL_RC_BREAK,
   looping over the list of files ends and this function returns
   that value. FSL_RC_BREAK causes looping to stop but 0 is
   returned.

   See fsl_repo_extract_f() for more details about the semantics of
   the callback. See fsl_repo_extract_opt for the documentation of the
   various options.

   Fossil's internal metadata format guarantees that files will be
   passed to the callback in "lexical order" (as defined by fossil's
   manifest format definition). i.e. the files will be passed in
   case-sensitive, alphabetical order. Note that upper-case letters
   sort before lower-case ones.

   Sidebar: this function makes a bitwise copy of the 2nd argument
   before starting its work, just in case the caller gets the crazy
   idea to modify it from the extraction callback. Whether or not
   there are valid/interesting uses for such modification remains to
   be seen. If any are found, this copy behavior may change.
*/
FSL_EXPORT int fsl_repo_extract( fsl_cx * f,
                                 fsl_repo_extract_opt const * opt );

/**
   Equivalent to fsl_tag_an_rid() except that it takes a symbolic
   artifact name in place of an artifact ID as the third
   argumemnt.

   This function passes symToTag to fsl_sym_to_rid(), and on
   success passes the rest of the parameters as-is to
   fsl_tag_an_rid(). See that function the semantics of the other
   arguments and the return value, as well as a description of the
   side effects.
*/
FSL_EXPORT int fsl_tag_sym( fsl_cx * f, fsl_tagtype_e tagType,
                 char const * symToTag, char const * tagName,
                 char const * tagValue, char const * userName,
                 double mtime, fsl_id_t * newId );

/**
   Adds a control record to f's repositoriy that either creates or
   cancels a tag.

   artifactRidToTag is the RID of the record to be tagged.

   tagType is the type (add, cancel, or propagate) of tag.

   tagName is the name of the tag. Must not be NULL/empty.

   tagValue is the optional value for the tag. May be NULL.

   userName is the user's name to apply to the artifact. May not be
   empty/NULL. Use fsl_user_name_guess() to try to figure out a
   proper user name based on the environment. See also:
   fsl_cx_user_get(), but note that the application must first
   use fsl_cx_user_set() to set a context's user name.

   mtime is the Julian Day timestamp for the new artifact. Pass a
   value <=0 to use the current time.

   If newId is not NULL then on success the rid of the new tag control
   artifact is assigned to *newId.

   Returns 0 on success and has about a million and thirteen
   possible error conditions. On success a new artifact record is
   written to the db, its RID being written into newId as described
   above.

   If the artifact being tagged is private, the new tag is also
   marked as private.

*/
FSL_EXPORT int fsl_tag_an_rid( fsl_cx * f, fsl_tagtype_e tagType,
                 fsl_id_t artifactRidToTag, char const * tagName,
                 char const * tagValue, char const * userName,
                 double mtime, fsl_id_t * newId );

/**
    Searches for a repo.tag entry given name in the given context's
    repository db. If found, it returns the record's id. If no
    record is found and create is true (non-0) then a tag is created
    and its entry id is returned. Returns 0 if it finds no entry, a
    negative value on error. On db-level error, f's error state is
    updated.
*/
FSL_EXPORT fsl_id_t fsl_tag_id( fsl_cx * f, char const * tag, bool create );


/**
   Returns true if the checkin with the given rid is a leaf, false if
   not. Returns false if f has no repo db opened, the query fails
   (likely indicating that it is not a repository db), or just about
   any other conceivable non-success case.

   A leaf, by the way, is a commit which has no children in the same
   branch.

   Sidebar: this function calculates whether the RID is a leaf, as
   opposed to checking the "static" (pre-calculated) list of leaves in
   the [leaf] table.
*/
FSL_EXPORT bool fsl_rid_is_leaf(fsl_cx * f, fsl_id_t rid);
/**
   Returns true if, according to f's current repo's [event] table,
   rid refers to a checkin, else false.
*/
FSL_EXPORT bool fsl_rid_is_version(fsl_cx * f, fsl_id_t rid);

/**
   Counts the number of primary non-branch children for the given
   check-in.

   A primary child is one where the parent is the primary parent, not
   a merge parent.  A "leaf" is a node that has zero children of any
   kind. This routine counts only primary children.

   A non-branch child is one which is on the same branch as the parent.

   Returns a negative value on error.
*/
FSL_EXPORT fsl_int_t fsl_count_nonbranch_children(fsl_cx * f,
                                                  fsl_id_t rid);

/**
   Looks for the delta table record where rid==deltaRid, and
   returns that record's srcid via *rv. Returns 0 on success, non-0
   on error. If no record is found, *rv is set to 0 and 0 is
   returned (as opposed to FSL_RC_NOT_FOUND) because that generally
   simplifies the error checking.

   Results are undefined if any pointer argument is NULL.
*/
FSL_EXPORT int fsl_delta_r2s( fsl_cx * f, fsl_id_t deltaRid,
                              fsl_id_t * rv );
/* Deprecated name for fsl_delta_r2s. */
#define fsl_delta_src_id fsl_delta_r2s

/**
   The counterpart of fsl_delta_r2s(): for a given delta.srcid, return
   the associated delta.rid via *rv.  See that function for more
   details.
*/
FSL_EXPORT int fsl_delta_s2r( fsl_cx * f, fsl_id_t deltaRid,
                              fsl_id_t * rv );


/**
   Return true if the given artifact ID should is listed in f's
   shun table or if zUuid is an SHA1 has and f's hash policy
   is set to FSL_HPOLICY_SHUN_SHA1.

   This function has no way to report errors except via the f
   object. If there is an error in this call, f's error state will be
   updated.

   Reminder to self: there is no fsl_is_shunned_rid() because rids
   refer to blob table records and the blob table is not supposed to
   contain shunned artifacts.
*/
FSL_EXPORT bool fsl_is_shunned_uuid(fsl_cx * f, fsl_uuid_cstr zUuid);
/* Old name for fsl_is_shunned_uuid(). */
#define fsl_uuid_is_shunned fsl_is_shunned_uuid


/**
   Compute the "mtime" of the file given whose blob.rid is "fid"
   that is part of check-in "vid".  The mtime will be the mtime on
   vid or some ancestor of vid where fid first appears. Note that
   fossil does not track the "real" mtimes of files, it only
   computes reasonable estimates for those files based on the
   timestamps of their most recent checkin in the ancestry of vid.

   If fid is 0 or less then the D-card time of the artifact with the
   blob.rid == vid is written to pMTime (this is a much less expensive
   operation, by the way).  In this particular case (A) vid may refer
   to any artifact type which has a D-card and (B) FSL_RC_NOT_FOUND is
   returned if vid is not a valid artifact RID.

   On success, if pMTime is not null then the result is written to
   *pMTime.

   Returns 0 on success, non-0 on error. Returns FSL_RC_NOT_FOUND
   if fid is not found in vid.

   This routine is much more efficient if used to answer several
   queries in a row for the same manifest (the vid parameter). It
   is least efficient when it is passed intermixed manifest IDs,
   e.g. (1, 3, 1, 4, 1,...). This is a side-effect of the caching
   used in the computation of ancestors for a given vid.
*/
FSL_EXPORT int fsl_mtime_of_manifest_file(fsl_cx * f, fsl_id_t vid, fsl_id_t fid,
                                          fsl_time_t * pMTime);

/**
   A convenience form of fsl_mtime_of_manifest_file() which looks up
   fc's RID based on its UUID. vid must be the RID of the checkin
   version fc originates from. See fsl_mtime_of_manifest_file() for
   full details - this function simply calculates the 3rd argument
   for that one.
*/
FSL_EXPORT int fsl_mtime_of_F_card(fsl_cx * f, fsl_id_t vid, fsl_card_F const * fc,
                                   fsl_time_t * pMTime);

/**
   Ensures that the given list has capacity for at least n entries. If
   the capacity is currently equal to or less than n, this is a no-op
   unless n is 0, in which case li->list is freed and the list is
   zeroed out. Else li->list is expanded to hold at least n
   elements. Returns 0 on success, FSL_RC_OOM on allocation error.
 */
FSL_EXPORT int fsl_card_F_list_reserve( fsl_card_F_list * li, uint32_t n );

/**
   Frees all memory owned by li and the F-cards it contains. Does not
   free the li pointer.
*/
FSL_EXPORT void fsl_card_F_list_finalize( fsl_card_F_list * li );

/**
   Iterates over all artifacts in f's current repository of the given
   type, passing each one to the given callback. If the callback
   returns non-0, iteration stops and that code is propagated back to
   the caller _unless_ the code is FSL_RC_BREAK, which means to stop
   iteration and return 0.

   The following artifact types are currently supported by this
   operation:

   FSL_SATYPE_CHECKIN, FSL_SATYPE_CONTROL, FSL_SATYPE_WIKI,
   FSL_SATYPE_TICKET, FSL_SATYPE_TECHNOTE, FSL_SATYPE_FORUMPOST

   (CLUSTER and ATTACHMENT are missing only because they're not(?)
   readily findable without parsing every blob in the blob
   table.)

   The decks are iterated over an an unspecified order except for:

   - WIKI and TECHNOTE entries are visited from newest to oldest.

   - TICKET entries are visited from oldest to newest to accommodate
     reconstructing their aggregate state in the callback.

   Each deck is finalized immediately after the callback returns, so
   the callback must not hold any pointers to the deck or its
   contents.

   Returns 0 on success or any number of error codes from lower levels,
   including, but not limited to:

   - FSL_RC_OOM on allocation error
   - FSL_RC_NOT_A_REPO if f has no repository opened.
   - FSL_RC_TYPE if the 2nd argument is invalid or if its type is
     not supported (e.g. FSL_SATYPE_CLUSTER).
   - FSL_RC_MISUSE if the 3rd argument is NULL.

*/
FSL_EXPORT int fsl_deck_foreach(fsl_cx * f, fsl_satype_e type,
                                fsl_deck_visitor_f visitor,
                                void * visitorState);

/**
   Holds options for use with fsl_branch_create().
*/
struct fsl_branch_opt {
  /**
     The checkin RID from which the branch should originate.
  */
  fsl_id_t basisRid;
  /**
     The name of the branch. May not be NULL or empty.
  */
  char const * name;
  /**
     User name for the branch. If NULL, fsl_cx_user_get() will
     be used.
  */
  char const * user;
  /**
     Optional comment (may be NULL). If NULL or empty, a default
     comment is generated (because fossil requires a non-empty
     comment string).
  */
  char const * comment;
  /**
     Optional background color for the fossil(1) HTML timeline
     view.  Must be in \#RRGGBB format, but this API does not
     validate it as such.
  */
  char const * bgColor;
  /**
     The julian time of the branch. If 0 or less, default is the
     current time.
  */
  double mtime;
  /**
     If true, the branch will be marked as private.
  */
  bool isPrivate;
};
typedef struct fsl_branch_opt fsl_branch_opt;
#define fsl_branch_opt_empty_m {                \
    0/*basisRid*/, NULL/*name*/,                \
      NULL/*user*/, NULL/*comment*/,            \
      NULL/*bgColor*/,                          \
      0.0/*mtime*/, 0/*isPrivate*/              \
      }
FSL_EXPORT const fsl_branch_opt fsl_branch_opt_empty;

/**
   Creates a new branch in f's repository. The 2nd paramter holds
   the options describing the branch. The 3rd parameter may be
   NULL, but if it is not then on success the RID of the new
   manifest is assigned to *newRid.

   In Fossil branches are implemented as tags. The branch name
   provided by the client will cause the creation of a tag with
   name name plus a "sym-" prefix to be created (if needed).
   "sym-" denotes that it is a "symbolic tag" (fossil's term for
   "symbolic name applying to one or more checkins,"
   i.e. branches).

   Creating a branch cancels all other branch tags which the new
   branch would normally inherit.

   Returns 0 on success, non-0 on error.
*/
FSL_EXPORT int fsl_branch_create(fsl_cx * f, fsl_branch_opt const * opt, fsl_id_t * newRid );

/**
   Checks f's repo db for a setting named 'main-branch'. If
   found. *zOut is set to that string. The mememory is owned by f and
   is retained for later calls to this function, but it may be
   invalidated by certain fsl APIs (namely this very function, if
   forceReload is true, and fsl_cx_finalize()). If forceReload is true
   then any cached value is freed and re-loaded.

   Returns 0 on success. On error, *zOut is not modified. If no such
   setting is found, "trunk" is used.
 */
FSL_EXPORT int fsl_branch_main(fsl_cx *f, char const **zOut, bool forceReload);

/**
   Tries to determine the [filename.fnid] value for the given
   filename.  Returns a positive value if it finds one, 0 if it
   finds none, and some unspecified negative value(s) for any sort
   of error. filename must be a normalized, relative filename (as it
   is recorded by a repo).
*/
FSL_EXPORT fsl_id_t fsl_repo_filename_fnid( fsl_cx * f, char const * filename );

/**
   Resolves client-provided symbol as an artifact's db record ID.
   f must have an opened repository db, and some symbols can only
   be looked up if it has an opened checkout (see the list below).

   Returns 0 and sets *rv to the id if it finds an unambiguous
   match.

   Returns FSL_RC_MISUSE if !sym, !*sym, or !rv.

   Returns FSL_RC_NOT_A_REPO if f has no opened repository.

   Returns FSL_RC_AMBIGUOUS if sym is a partial UUID which matches
   multiple full UUIDs.

   Returns FSL_RC_NOT_FOUND if it cannot find anything.

   Symbols supported by this function:

   - SHA1/3 hash
   - SHA1/3 hash prefix of at least 4 characters
   - Symbolic Name, e.g. branch name
   - "tag:" + symbolic name
   - Date or date-time
   - "date:" + Date or date-time
   - symbolic-name ":" date-time
   - "tip" means the most recent checkin, regardless of its branch
   - "rid:###" resolves to the hash of blob.rid ### if that RID is in
     the database

   The following additional forms are available in local checkouts:

   - "current"
   - "prev" or "previous"
   - "next"

   The following prefix may be applied to the above to modify how
   they are resolved:

   - "root:" prefix resolves to the checkin of the parent branch from
   which the record's branch divered. i.e. the version from which it
   was branched. In the trunk this will always resolve to the first
   checkin.

   - "start:" prefix resolves to the first checkin of the branch to
   which the given checkin belongs. This differs from "root:" by a
   single checkin: the "root:" point is the parent checkin of the
   "start:" point.

   - "merge-in:" TODO - document this once its implications are
   understood.

   If type is not FSL_SATYPE_ANY then it will only match artifacts of
   the specified type. In order to resolve arbitrary UUIDs, e.g.
   those of arbitrary blob content, type needs to be FSL_SATYPE_ANY.
*/
FSL_EXPORT int fsl_sym_to_rid( fsl_cx * f, char const * sym,
                               fsl_satype_e type, fsl_id_t * rv );

/**
   Similar to fsl_sym_to_rid() but on success it returns a UUID string
   by assigning it to *rv (if rv is not NULL). If rid is not NULL then
   on success the db record ID corresponding to the returned UUID is
   assigned to *rid. The caller must eventually free the returned
   string memory by passing it to fsl_free(). Returns 0 if it finds a
   match and one of any number of possible result codes on error, most
   notably FSL_RC_NOT_FOUND if no match is found.
*/
FSL_EXPORT int fsl_sym_to_uuid( fsl_cx * f, char const * sym,
                                fsl_satype_e type, fsl_uuid_str * rv,
                                fsl_id_t * rid );


/**
   Searches f's repo database for the a blob with the given uuid
   (any unique UUID prefix). On success a positive record ID is
   returned. On error one of several unspecified negative values is
   returned. If no uuid match is found 0 is returned.

   Error cases include: either argument is NULL, uuid does not appear
   to be a full or partial UUID (or is too long), uuid is ambiguous
   (try providing a longer one). For error cases other than an invalid
   2nd argument, f's error state is updated.

   This implementation is more efficient when given a full, valid UUID
   (one for which fsl_is_uuid() returns true).
*/
FSL_EXPORT fsl_id_t fsl_uuid_to_rid( fsl_cx * f, char const * uuid );

/**
   The opposite of fsl_uuid_to_rid(), this returns the UUID string
   of the given blob record ID. Ownership of the string is passed
   to the caller and it must eventually be freed using
   fsl_free(). Returns NULL on error (invalid arguments or f has no
   repo opened) or if no blob record is found. If no record is
   found, f's error state is updated with an explanation of the
   problem.
*/
FSL_EXPORT fsl_uuid_str fsl_rid_to_uuid(fsl_cx * f, fsl_id_t rid);

/**
   Works like fsl_rid_to_uuid() but assigns the UUID to the given
   buffer, re-using its memory, if any. Returns 0 on success,
   FSL_RC_MISUSE if rid is not positive, FSL_RC_OOM on allocation
   error, and FSL_RC_NOT_FOUND if no blob entry matching the given rid
   is found.
*/
FSL_EXPORT int fsl_rid_to_uuid2(fsl_cx * f, fsl_id_t rid, fsl_buffer *uuid);

/**
   This works identically to fsl_rid_to_uuid() except that it will
   only resolve to a UUID if an artifact matching the given type has
   that UUID. If no entry is found, f's error state gets updated
   with a description of the problem.

   This can be used to distinguish artifact UUIDs from file blob
   content UUIDs by passing the type FSL_SATYPE_ANY. A non-artifact
   blob will return NULL in that case, but any artifact type will
   match (assuming rid is valid).
*/
FSL_EXPORT fsl_uuid_str fsl_rid_to_artifact_uuid(fsl_cx * f, fsl_id_t rid,
                                                 fsl_satype_e type);
/**
   Returns the raw SQL code for a Fossil global config database.

   TODO: add optional (fsl_size_t*) to return the length.
*/
FSL_EXPORT char const * fsl_schema_config(void);

/**
   Returns the raw SQL code for the "static" parts of a Fossil
   repository database. These are the parts which are immutable
   (for the most part) between Fossil versions. They change _very_
   rarely.

   TODO: add optional (fsl_size_t*) to return the length.
*/
FSL_EXPORT char const * fsl_schema_repo1(void);

/**
   Returns the raw SQL code for the "transient" parts of a Fossil
   repository database - any parts which can be calculated via data
   held in the primary "static" schemas. These parts are
   occassionally recreated, e.g. via a 'rebuild' of a repository.

   TODO: add optional (fsl_size_t*) to return the length.
*/
FSL_EXPORT char const * fsl_schema_repo2(void);

/**
   Returns the raw SQL code for a Fossil checkout database.

   TODO: add optional (fsl_size_t*) to return the length.
*/
FSL_EXPORT char const * fsl_schema_ckout(void);

/**
   Returns the raw SQL code for a Fossil checkout db's
   _default_ core ticket-related tables.

   TODO: add optional (fsl_size_t*) to return the length.

   @see fsl_cx_schema_ticket()
*/
FSL_EXPORT char const * fsl_schema_ticket(void);

/**
   Returns the raw SQL code for the "forum" parts of a Fossil
   repository database.

   TODO: add optional (fsl_size_t*) to return the length.
*/
FSL_EXPORT char const * fsl_schema_forum(void);

/**
   If f's opened repository has a non-empty config entry named
   'ticket-table', this returns its text via appending it to
   pOut. If no entry is found, fsl_schema_ticket() is appended to
   pOut.

   Returns 0 on success. On error the contents of pOut must not be
   considered valid but pOut might be partially populated.
*/
FSL_EXPORT int fsl_cx_schema_ticket(fsl_cx * f, fsl_buffer * pOut);

/**
   Returns the raw SQL code for Fossil ticket reports schemas.
   This gets installed as needed into repository databases.

   TODO: add optional (fsl_size_t*) to return the length.
*/
FSL_EXPORT char const * fsl_schema_ticket_reports(void);

/**
   This is a wrapper around fsl_cx_hash_buffer() which looks for a
   matching artifact for the given input blob. It first hashes src
   using f's "alternate" hash and then, if no match is found, tries
   again with f's preferred hash.

   On success (a match is found):

   - Returns 0.

   - If ridOut is not NULL, *ridOut is set to the RID of the matching
     blob.

   - If hashOut is not NULL, *hashOut is set to the hash of the
     blob. Its ownership is transferred to the caller, who must
     eventually pass it to fsl_free().

   If no matching blob is found in the repository, FSL_RC_NOT_FOUND is
   returned (but f's error state is not annotated with more
   information). Returns FSL_RC_NOT_A_REPO if f has no repository
   opened. For more serious errors, e.g. allocation error or db
   problems, another (more serious) result code is returned,
   e.g. FSL_RC_OOM or FSL_RC_DB.

   If FSL_RC_NOT_FOUND is returned and hashOut is not NULL, *hashOut
   is set to the value of f's preferred hash and *hashOut's ownership
   is transferred to the caller. *ridOut is only modified if 0 is
   returned, in which case *ridOut will have a positive value.
*/
FSL_EXPORT int fsl_repo_blob_lookup( fsl_cx * f, fsl_buffer const * src,
                                     fsl_id_t * ridOut,
                                     fsl_uuid_str * hashOut );

/**
   Returns true if the specified file name ends with any reserved
   name, e.g.: _FOSSIL_ or .fslckout.

   For the sake of efficiency, zFilename must be a canonical name,
   e.g. an absolute or checkout-relative path using only forward slash
   ('/') as a directory separator.

   On Windows builds, this also checks for reserved Windows filenames,
   e.g. "CON" and "PRN".

   nameLen must be the length of zFilename. If it is negative,
   fsl_strlen() is used to calculate it.
*/
FSL_EXPORT bool fsl_is_reserved_fn(const char *zFilename,
                                   fsl_int_t nameLen );

/**
   Uses fsl_is_reserved_fn() to determine whether the filename part of
   zPath is legal for use as an in-repository filename. If it is, 0 is
   returned, else FSL_RC_RANGE (or FSL_RC_OOM) is returned and f's
   error state is updated to indicate the nature of the problem. nFile
   is the length of zPath. If negative, fsl_strlen() is used to
   determine its length.

   If relativeToCwd is true then zPath, if not absolute, is
   canonicalized as if were relative to the current working directory
   (see fsl_getcwd()), else it is assumed to be relative to the
   current checkout (if any - falling back to the current working
   directory). This flag is only relevant if zPath is not absolute and
   if f has a checkout opened. An absolute zPath is used as-is and if
   no checkout is opened then relativeToCwd is always treated as if it
   were true.

   This routine does not validate that zPath lives inside a checkout
   nor that the file actually exists. It does only name comparison and
   only uses the filesystem for purposes of canonicalizing (if needed)
   zPath.

   This routine does not require that f have an opened repo, but if it
   does then this routine compares the canonicalized forms of both the
   repository db and the given path and fails if zPath refers to the
   repository db. Be aware that the relativeToCwd flag may influence
   that test.

   This routine also checks fsl_ckout_manifest_setting() and reports
   any of the files represented by that function's results as being
   reserved. It only treats such names as reserved if they are at the
   top level of the repository - those same names in subdirectories are
   not reserved. If f has no checkout opened and relativeToCwd is true
   then those names are considered to be at the "top" if they are in
   the current directory.
*/
FSL_EXPORT int fsl_reserved_fn_check(fsl_cx * f, const char *zPath,
                                     fsl_int_t nFile, bool relativeToCwd);

/**
   Recompute/rebuild the entire repo.leaf table. This is not normally
   needed, as leaf tracking is part of the crosslinking process, but
   "just in case," here it is.

   This can supposedly be expensive (in time) for a really large
   repository. Testing implies otherwise.

   Returns 0 on success. On error f's error state may be updated.
   Results are undefined if f is invalid or has no opened repository.
*/
FSL_EXPORT int fsl_repo_leaves_rebuild(fsl_cx * f);

/**
   Flags for use with fsl_leaves_compute().
*/
enum fsl_leaves_compute_e {
/**
   Compute all leaves regardless of the "closed" tag.
*/
FSL_LEAVES_COMPUTE_ALL = 0,
/**
   Compute only leaves without the "closed" tag.
*/
FSL_LEAVES_COMPUTE_OPEN = 1,
/**
   Compute only leaves with the "closed" tag.
*/
FSL_LEAVES_COMPUTE_CLOSED = 2
};
typedef enum fsl_leaves_compute_e fsl_leaves_compute_e;

/**
   Creates a temporary table named "leaves" if it does not already
   exist, else empties it. Populates that table with the RID of all
   check-ins that are leaves which are descended from the checkin
   referred to by vid.

   A "leaf" is a check-in that has no children in the same branch.
   There is a separate permanent table named [leaf] that contains all
   leaves in the tree. This routine is used to compute a subset of
   that table consisting of leaves that are descended from a single
   check-in.

   The leafMode flag determines behavior associated with the "closed"
   tag, as documented for the fsl_leaves_compute_e enum.

   If vid is <=0 then this function, after setting up or cleaning out
   the [leaves] table, simply copies the list of leaves from the
   repository's pre-computed [leaf] table (see
   fsl_repo_leaves_rebuild()).

   @see fsl_leaves_computed_has()
   @see fsl_leaves_computed_count()
   @see fsl_leaves_computed_latest()
   @see fsl_leaves_computed_cleanup()
*/
FSL_EXPORT int fsl_leaves_compute(fsl_cx * f, fsl_id_t vid,
                                  fsl_leaves_compute_e leafMode);

/**
   Requires that a prior call to fsl_leaves_compute() has succeeded,
   else results are undefined.

   Returns true if the leaves list computed by fsl_leaves_compute() is
   not empty, else false. This is more efficient than checking
   against fsl_leaves_computed_count()>0.
*/
FSL_EXPORT bool fsl_leaves_computed_has(fsl_cx * f);

/**
   Requires that a prior call to fsl_leaves_compute() has succeeded,
   else results are undefined.

   Returns a count of the leaves list computed by
   fsl_leaves_compute(), or a negative value if a db-level error is
   encountered. On errors other than FSL_RC_OOM, f's error state will
   be updated with information about the error.
*/
FSL_EXPORT fsl_int_t fsl_leaves_computed_count(fsl_cx * f);

/**
   Requires that a prior call to fsl_leaves_compute() has succeeded,
   else results are undefined.

   Returns the RID of the most recent checkin from those computed by
   fsl_leaves_compute(), 0 if no entries are found, or a negative
   value if a db-level error is encountered. On errors other than
   FSL_RC_OOM, f's error state will be updated with information about
   the error.
*/
FSL_EXPORT fsl_id_t fsl_leaves_computed_latest(fsl_cx * f);

/**
   Cleans up any db-side resources created by fsl_leaves_compute().
   e.g. drops the temporary table created by that routine. Any errors
   are silenty ignored.
*/
FSL_EXPORT void fsl_leaves_computed_cleanup(fsl_cx * f);

/**
   Returns true if f's current repository has the
   forbid-delta-manifests setting set to a truthy value. Results are
   undefined if f has no opened repository. Some routines behave
   differently if this setting is enabled. e.g. fsl_checkin_commit()
   will never generate a delta manifest and fsl_deck_save() will
   refuse to save a delta. This does not affect parsing or deltas or
   those which are injected into the db via lower-level means (e.g. a
   direct blob import or from a remote sync).

   Results are undefined if f has no opened repository.
*/
FSL_EXPORT bool fsl_repo_forbids_delta_manifests(fsl_cx * f);

/**
   This is a variant of fsl_ckout_manifest_write() which writes data
   regarding the given manifest RID to the given blobs. If manifestRid
   is 0 or less then the current checkout is assumed and
   FSL_RC_NOT_A_CKOUT is returned if no checkout is opened (or
   FSL_RC_RANGE if an empty checkout is opened - a freshly-created
   repository with no checkins).

   For each buffer argument which is not NULL, the corresponding
   checkin-related data are appended to it. All such blobs will end
   in a terminating newline character.

   Returns 0 on success, any of numerious non-0 fsl_rc_e codes on
   error.
*/
FSL_EXPORT int fsl_repo_manifest_write(fsl_cx * f,
                                       fsl_id_t manifestRid,
                                       fsl_buffer * manifest,
                                       fsl_buffer * manifestUuid,
                                       fsl_buffer * manifestTags );


/**
   "Stage" flag for use with fsl_annotate_step and
   fsl_annotate_step_f().
*/
enum fsl_annotate_step_e {
/**
   Indicates that the current fsl_annotate_step_f() call is
   part of the "version dump" stage of the annotation.
*/
FSL_ANNOTATE_STEP_VERSION,
/**
   Indicates that the current fsl_annotate_step_f() call has
   complete version information for the line it is reporting
   about.
*/
FSL_ANNOTATE_STEP_FULL,
/**
   Indicates that the current fsl_annotate_step_f() call has
   only partial version information for the line it is reporting
   about, as a result of a limited-run annotation.
*/
FSL_ANNOTATE_STEP_LIMITED
};
typedef enum fsl_annotate_step_e fsl_annotate_step_e;

/**
   Callback state for use by fsl_annotate_step_f().

   ACHTUNG: this state gets repopulated for each call to the
   fsl_annote_step_f() callback and any pointers it holds must be
   treated as if they are invalidated as soon as the callback returns
   (whether or not that is the case is undefined, though).

   Not all state is set on each call involving this object. See the
   stepType member for details.
*/
struct fsl_annotate_step {
  /**
     Tells the caller the "type" of annotation step this call
     represents. The type of step will determine which fields
     of this object are populated when it is passed to a
     fsl_annotate_step_f() callback:

     Always set:

     stepType, stepNumber (but interpretation varies - see that
     member's docs for details).

     FSL_ANNOTATE_STEP_VERSION: fileHash, versionHash, mtime

     FSL_ANNOTATE_STEP_LIMITED: stepNumber (always 0 for the limited
     case), lineNumber, line, lineNength.

     FSL_ANNOTATE_STEP_FULL: as for FSL_ANNOTATE_STEP_LIMITED plus:
     fileHash, versionHash, mtime, username.
  */
  fsl_annotate_step_e stepType;
  /**
     Step number in this annotation run. When this->stepType is
     FSL_ANNOTATE_STEP_VERSION, this value is the relative number of
     the version, starting at 0 and incremented by 1 on each call. In
     the other modes it is 0-based relative version from which the
     current line is from, or negative if that information is
     incomplete due to a limited annotation run. e.g. a value of 3
     indicates that this line is from 3 version away from the starting
     version.
  */
  int stepNumber;
  /**
     Line number for the current file.
  */
  uint32_t lineNumber;
  /**
     NUL-terminated current line of the input file, minus any
     newline and/or carriage return.
  */
  char const * line;
  /**
     The number of bytes in this->line.
  */
  uint32_t lineLength;
  /**
     The hash of the file version from which this->line was
     pulled.
  */
  fsl_uuid_cstr fileHash;
  /**
     The hash of the checkin version from which this->line was
     pulled.
  */
  fsl_uuid_cstr versionHash;

  /**
     The mtime field from the [event] table (timeline) entry
     associated with this version. This is a Julian Date and, because
     it is updated by annotations which modify timestamps in the
     [event] table, it reflects any "edited" time (if any).

     @see fsl_julian_to_iso8601()
  */
  double mtime;
  /**
     The user name this change was attributed to, noting that merges
     are attributed to the one who did the merge.
  */
  char const * username;
};

/** Convenience typedef. */
typedef struct fsl_annotate_step fsl_annotate_step;
/** Forward decl and convenience typedef. */
typedef struct fsl_annotate_opt fsl_annotate_opt;

/**
   Callback for use with fsl_annotate(). Implementations receive
   state about each step of an annotation process. They must return 0
   on success. On error, their non-0 result code is propagated back
   to the fsl_annotate() caller.
*/
typedef int (*fsl_annotate_step_f)(void * state,
                                   fsl_annotate_opt const * opt,
                                   fsl_annotate_step const * step);

/**
   A fsl_annotate_step_f() impl. which requires that its first argument
   be a fsl_outputer. It formats each step of the annotation
   in a manner similar to fossil(1) and forwards the result to
   state->out(state->state, ...), returning that function's result code.
*/
int fsl_annotate_step_f_fossilesque(void * state,
                                    fsl_annotate_opt const * opt,
                                    fsl_annotate_step const * step);

/**
   Configuration for use with fsl_annotate().

   This structure holds options for the "annotate" operation and its
   close cousin, "blame" a.k.a. "praise." Annotation takes a given
   file version and builds a line-by-line history, showing when each
   line was last modified. The "blame" a.k.a. "praise" option includes
   *who* modified that line.
*/
struct fsl_annotate_opt {
  /**
     The repository-root-relative NUL-terminated filename to annotate.
  */
  char const * filename;
  /**
     The checkin from which the file's version should be selected. A
     value of 0 or less means the current checkout, if in a checkout,
     and is otherwise an error.
  */
  fsl_id_t versionRid;
  /**
     The origin checkin version. A value of 0 or less means the "root of the
     tree."

     TODO: figure out and explain the difference between versionRid
     and originRid.
  */
  fsl_id_t originRid;
  /**
     The maximum number of versions to search through.
  */
  uint32_t limitVersions;

  /**
     An approximate number of milliseconds of processing time to limit
     the annotation to. Note that this is measured in CPU time, not
     "wall clock" time. This value is rough minimum approximation,
     and the annotation will stop at the first processing step after which
     this limit has been hit or surpassed.

     Even with this limit in place, the annotation engine may impose a
     minimum number of versions to step through before it enforces
     this limit.

     If both this and limitVersions are set to positive values, the
     first limit which is exceeded is applied.
  */
  uint32_t limitMs;
  /**
     - 0 = do not ignore any spaces.
     - <0 = ignore trailing end-of-line spaces.
     - >1 = ignore all spaces
  */
  int16_t spacePolicy;
  /**
     If true, include the name of the user for which each change is
     attributed (noting that merges show whoever merged the change,
     which may differ from the original committer, and amended user
     names will be used over those in the initial commit). If false,
     show only version information.

     This option is alternately known as "blame".

     For reasons lost to history, blame/praise mode does not include
     line numbers. That may change in the future.
  */
  bool praise;
  /**
     Output file blob versions, instead of checkin versions.
  */
  bool fileVersions;

  /**
     If true, annotation output will start with a list of all
     versions analyzed by the annotation process.
  */
  bool dumpVersions;
  /**
     The output channel for the resulting annotation.
  */
  fsl_annotate_step_f out;
  /**
     State for passing as the first argument to this->out().
  */
  void * outState;
};

/** Initialized-with-defaults fsl_annotate_opt structure, intended for
    const-copy initialization. */
#define fsl_annotate_opt_empty_m {\
  NULL/*filename*/, \
  0/*versionRid*/,0/*originRid*/,    \
  0U/*limitVersions*/, 0U/*limitMs*/,\
  0/*spacePolicy*/,                         \
  false/*praise*/, false/*fileVersions*/,     \
  false/*dumpVersions*/,                  \
  NULL/*out*/, NULL/*outState*/               \
}

/** Initialized-with-defaults fsl_annotate_opt structure, intended for
    non-const copy initialization. */
extern const fsl_annotate_opt fsl_annotate_opt_empty;

/**
   UNDER CONSTRUCTION. Not yet known to be fully functional or
   bug-free.

   Runs an "annotation" of an SCM-controled file and sends the results
   to opt->out().

   Returns 0 on success. On error, returns one of:

   - FSL_RC_OOM on OOM

   - FSL_RC_NOT_A_CKOUT if opt->versionRid<=0 and f has no opened checkout.

   - FSL_RC_NOT_FOUND if the given filename cannot be found in the
     repository OR a given version ID does not resolve to a blob. (Sorry
     about this ambiguity!)

   - FSL_RC_PHANTOM if a phantom blob is encountered while trying to
     annotate.

   opt->out() may return arbitrary non-0 result codes, in which case
   the returned code is propagated to the caller of this function.

   Results are undefined if either argument is invalid or opt->out is
   NULL.
*/
FSL_EXPORT int fsl_annotate( fsl_cx * f,
                             fsl_annotate_opt const * opt );


/** Convenience typedef and forward decl. */
typedef struct fsl_rebuild_opt fsl_rebuild_opt;

struct fsl_rebuild_metrics {
  /**
     The number of phantom artifacts seen during rebuild.
   */
  fsl_size_t phantomCount;
  /**
     Counts of each blob type seen during rebuild.  The FSL_SATYPE_ANY
     slot (0) counts non-artifact blobs.
  */
  fsl_size_t counts[FSL_SATYPE_count];
  /**
     The accumulated sizes of each blob type seen during rebuild. The
     FSL_SATYPE_ANY slot (0) counts non-artifact blobs.
  */
  fsl_size_t sizes[FSL_SATYPE_count];
};
typedef struct fsl_rebuild_metrics fsl_rebuild_metrics;

#define fsl_rebuild_metrics_empty_m { \
    .phantomCount = 0,                \
    .counts = {0,0,0,0,0,0,0,0,0},    \
    .sizes = {0,0,0,0,0,0,0,0,0}      \
  }

/**
   State for use with the fsl_rebuild_f() callback type.
*/
struct fsl_rebuild_step {
  /**
     Fossil context for which this rebuild is being performed.
  */
  fsl_cx * f;
  /**
     The options object used for this invocation of fsl_repo_rebuild().
  */
  fsl_rebuild_opt const * opt;
  /**
     An _approximate_ upper bound on the number of files
     fsl_repo_rebuild() will process. This number is very likely
     somewhat larger than the number of times which opt->callback()
     will be called, but it is close enough to give some indication of
     how far along a rebuild is.
  */
  fsl_size_t artifactCount;
  /**
     One-based counter of total artifacts processed so far. After
     rebuilding is finished, opt->callback will be called one final
     time with this value set to 0. The callback may use the value 0
     to recognize that this is post-rebuild step and finalize any
     output or whatever it wants to do.
  */
  fsl_size_t stepNumber;

  /**
     The `blob.rid` value of the just-processed blob. If stepNumber is 0,
     this will be 0.
  */
  fsl_id_t rid;

  /**
     The size of the just-processed blob, -1 if it is a phantom
     blob, or 0 when this object is part of a FSL_MSG_REBUILD_DONE
     message.
  */
  fsl_int_t blobSize;

  /**
     Set to FSL_SATYPE_INVALID if the just-processed blob
     is _not_ a fossil artifact, else false. This will never be
     set to FSL_SATYPE_ANY.
  */
  fsl_satype_e artifactType;

  /**
     This is only non-0 for FSL_MSG_REBUILD_DONE messages, in which
     case it holds the result code of the overall rebuild process.
  */
  int errCode;

  /**
     Metrics from the rebuild. On each rebuild step callback call,
     these will contain the updated metrics.
  */
  fsl_rebuild_metrics metrics;
};
/** Convenience typedef. */
typedef struct fsl_rebuild_step fsl_rebuild_step;

/** Initialized-with-defaults fsl_rebuild_step structure, intended for
    const-copy initialization. */
#define fsl_rebuild_step_empty_m { \
    .f = NULL, .opt = NULL, .artifactCount = 0, \
    .stepNumber = 0, .rid = 0, .blobSize = -1, \
    .artifactType = FSL_SATYPE_INVALID, \
    .errCode = 0, \
    .metrics = fsl_rebuild_metrics_empty_m \
  }

/** Initialized-with-defaults fsl_rebuild_step structure, intended for
    non-const copy initialization. */
FSL_EXPORT const fsl_rebuild_step fsl_rebuild_step_empty;

/**
   Callback for use with fsl_repo_rebuild() in order to report
   progress. It must return 0 on success, and any non-0 results will
   abort the rebuild and be propagated back to the caller.

   Each time this is called, state->stepNumber will be incremented, starting
   at 1. After rebuilding is complete, it is called with a stepNumber
   of 0 to give the callback a chance to do any final bookkeeping or output
   cleanup or whatever.

   TODO: replace this with the fsl_msg interface and use
   fsl_msg_listener configured for the being-rebuilt fsl_cx.
*/
typedef int (*fsl_rebuild_f)(fsl_rebuild_step const * state);

/**
   Options for the rebuild process.
*/
struct fsl_rebuild_opt {
  /**
     Scan artifacts in a random order (generally only of use in testing
     the library's code). This is primarily for testing that the library
     can handle its inputs in an arbitrary order.
  */
  bool randomize;
  /**
     True if clusters should be created.

     NOT YET IMPLEMENTED.
  */
  bool createClusters;
  /**
     If true, the transaction started by the rebuild process will end
     in a rollback even on success. In that case, if a transaction is
     started before the rebuild is initiated, it will be left in the
     rolling-back state after rebuild completes.
  */
  bool dryRun;
};

/** Initialized-with-defaults fsl_rebuild_opt structure, intended for
    const-copy initialization. */
#define fsl_rebuild_opt_empty_m {\
  .randomize=false, .createClusters=false, .dryRun=false \
}

/** Initialized-with-defaults fsl_rebuild_opt structure, intended for
    non-const copy initialization. */
FSL_EXPORT const fsl_rebuild_opt fsl_rebuild_opt_empty;

/**
   "Rebuilds" the current repository database. This involves _at least_
   the following:

   - DROPPING all transient repository tables. ALL tables in the db
     which do not specifically belong to fossil and do not start with
     the name `fx_` _will be dropped_.

   - Recreating all transient tables from immutable state in the
     database.

   - Updating the schema, if needed, from "older" versions of the
     fossil schema to the "current" one. This library does not
     currently check for updates which need to be made to a decade-old
     schema, however! That is, schema changes which were introduced
     10+ years ago are not currently addressed by this library
     because, frankly, it's not expected that anyone using this
     library will be using it with such ancient repositories (and
     those who do can rebuild it once with fossil(1) to update it).

   - Crosslinking all blobs which appear to be artifacts (meaning they
     can be parsed as such). Any crosslink handlers registered via
     fsl_xlink_listener() will be called.

   If opt is NULL then fsl_rebuild_opt_empty is used.

   Returns 0 on success, non-0 on error (with any number of potential
   result codes from the db or crosslinking layers). It pushes its own
   transaction and will roll that back on non-0, so it "shouldn't"
   leave a mess on error. On error it does _not_ put any higher-level
   transaction into rollback mode.

   If opt->dryRun is true then it also rolls back the transaction but
   it is not treated as an error and the rollback does not affect
   higher levels of the transaction stack

   During the rebuild process, fsl_msg messages of types
   FSL_MSG_REBUILD_STEP and FSL_MSG_REBUILD_DONE may be fired.  The
   latter is only fired if the former is.

   This operation honors interruption via fsl_cx_interrupt() but, due
   to intricacies of timing, it's possible that a triggered interrupt
   gets trumped by another error which happens while the check for the
   interrupt flag is pending. In both cases this function could return
   a non-0 code, though.
*/
FSL_EXPORT int fsl_repo_rebuild(fsl_cx * f, fsl_rebuild_opt const * opt);

/**
   Tries to determine the branch name of the given rid, which is assumed to
   refer to a checkin artifact. If it cannot find one and doFallback
   is true then it looks for the `main-branch` repository-level config
   setting and uses that (falling back to "trunk" is that setting is not set).

   On success it returns 0 and sets `*zOut` to the branch name,
   transfering ownership of those bytes to the caller (who must
   eventually pass them to fsl_free()). If doFallback is false and no
   direct branch name is found then it sets `*zOut` to NULL. If
   doFallback is true then, on success, `*zOut` will always be set to
   some non-NULL value. On error `*zOut` is not modified.

   On error it may return FSL_RC_NOT_A_REPO, FSL_RC_OOM, or any number
   of db-side error codes.
*/
FSL_EXPORT int fsl_branch_of_rid(fsl_cx * f, fsl_id_t rid,
                                 bool doFallback, char ** zOut );

/** Convenience typedef and obligatory forward declaration. */
typedef struct fsl_cidiff_state fsl_cidiff_state;

/**
   Callback type for use with fsl_cidiff(). It must return 0 on
   success or a value from the fsl_rc_e enum on error.  On error it
   "should" update the error state in the corresponding fsl_cx object
   by passing state->f to fsl_cx_err_set() (or equivalent).
*/
typedef int (*fsl_cidiff_f)(fsl_cidiff_state const *state);

/**
   Options for use with fsl_cidiff()
*/
struct fsl_cidiff_opt {
  /**
     Checkin version (RID) for "version 1".
  */
  fsl_id_t v1;
  /**
     Checkin version (RID) for "version 2". This checkin need not have
     any relationship with version 1, in terms of SCM-side lineage.
  */
  fsl_id_t v2;
  /**
     Callback to call on each iteration step.
  */
  fsl_cidiff_f callback;
  /**
     Opaque state for the callback. It can be accessed
     from the callback via arg->opt->callbackState.
  */
  void * callbackState;
};

/** Convenience typedef. */
typedef struct fsl_cidiff_opt fsl_cidiff_opt;

/** Initialized-with-defaults fsl_cidiff_opt structure, intended for
    const-copy initialization. */
#define fsl_cidiff_opt_empty_m {0,0,0,0}

/** Initialized-with-defaults fsl_cidiff_opt structure, intended for
    non-const copy initialization. */
FSL_EXPORT const fsl_cidiff_opt fsl_cidiff_opt_empty;

/**
   Descriptors for the type of information being reported for each
   step of fsl_cidiff() iteration, as reported via the
   fsl_cidiff_state::changes attribute.
*/
enum fsl_cidiff_e {
/**
   Indicates that the v1 and v2 files are the same. Specifically,
   this means:

   - Same current names, compared case-sensitively.
     fsl_card_F::priorName values are ignored for this
   - Same permissions.
   - Same hash.
*/
FSL_CIDIFF_NONE = 0,
/**
   Indicates that the hashes of the v1 and v2 files
   differ.
*/
FSL_CIDIFF_FILE_MODIFIED = 0x0001,
/**
   Indicates that the v2 file was renamed.

   Caveats:

   - If v1 and v2 are not immediate relatives in the SCM DAG
     sense. e.g. when comparing a version X and unrelated version Y,
     or version X and X+30, it is possible that a rename goes
     unreported via fsl_cidiff or that it gets misdiagnosed. e.g. if
     version X renames file A to B and version X+100 renames file C to
     A, it will be reported as a rename. Detecting the true reality of
     such cases is remarkably challenging, requiring going through the
     entire history which links v1 and v2.

   - Pedantically speaking, if v1 and v2 are unrelated in the SCM
     DAG, "renames" are not possible but will be reported as such
     if their names happen to match up.
*/
FSL_CIDIFF_FILE_RENAMED  = 0x0002,
/**
   Indicates that the permissions of the current file
   differ between v1 and v2. This is only set if
   both versions have a given file (by name, as opposed
   to hash).
*/
FSL_CIDIFF_FILE_PERMS    = 0x0004,
/**
   Indicates that the file is in v2 but not v1. If v1 and v2
   are unrelated versions or are not immediate DAG neighbors,
   this might also indicate that a file from v1 was renamed
   in v2.
*/
FSL_CIDIFF_FILE_ADDED    = 0x0010,
/**
   Indicates that the file is in v1 but not v2. If v1 and v2
   are unrelated versions or are not immediate DAG neighbors,
   this might also indicate that a file from v1 was renamed
   in v2.
*/
FSL_CIDIFF_FILE_REMOVED  = 0x0020
};
/** Convenience typedef. */
typedef enum fsl_cidiff_e fsl_cidiff_e;
/**
   Holds the state for a single iteration step of fsl_cidiff.
*/
struct fsl_cidiff_state {
  /**
     The associated fsl_cx instance.
  */
  fsl_cx * f;
  /**
     The options object passed to fsl_cidiff().
  */
  fsl_cidiff_opt const *opt;
  /**
     Denotes the type of this step in the iteration.

     Before fsl_cidiff() starts its loop over F-card changes,
     it calls the callback once with this value set to 0. The intent is to
     give the caller an opporuntity to do any required setup, e.g.
     outputing a report header. The
     fc1 and fc2 members will both be NULL, but the d1 and d2
     members will be set.

     For each step of the F-card iteration, stepType will be set to
     FSL_RC_STEP_ROW.

     After successful difference iteration, fsl_cidiff() calls its
     callback with stepType set to FSL_RC_STEP_DONE. The intent is to
     give the caller an opporuntity to do any required cleanup, e.g.
     outputing a report footer. The fc1 and fc2 members will both be
     NULL, but the d1 and d2 members will be set.
  */
  int stepType;
  /**
     Describes the change(s) being reported by the current
     iteration step, as a bitmask of fsl_cidiff_e values.
  */
  int changes;
  /**
     The file, if any, corresponding to this->v1. This will be NULL
     this->changeType is FSL_CIDIFF_FILE_ADDED.
  */
  fsl_card_F const * fc1;
  /**
     The file, if any, corresponding to this->v2. This will be NULL
     this->changeType is FSL_CIDIFF_FILE_REMOVED.
  */
  fsl_card_F const * fc2;
  /**
     The deck corresponding to this->opt->v1. It is strictly forbidden
     for this->callback to manipulate this object. Specifically, any
     traversal of its F-card list will invalidate the iteration being
     done by fsl_cidiff(). Read-only operations on other state of the
     deck are legal.
  */
  fsl_deck const * d1;
  /**
     The deck corresponding to this->opt->v2. See the docs for
     this->d1 for important details.
  */
  fsl_deck const * d2;
};

/** Initialized-with-defaults fsl_cidiff_state structure, intended for
    const-copy initialization. */
#define fsl_cidiff_state_empty_m {\
    NULL/*f*/,NULL/*opt*/,             \
    0/*stepType*/,FSL_CIDIFF_NONE/*changes*/, \
    NULL/*fc1*/,NULL/*fc2*/,    \
    NULL/*d1*/,NULL/*d2*/     \
  }

/** Initialized-with-defaults fsl_cidiff_state structure, intended for
    non-const copy initialization. */
FSL_EXPORT const fsl_cidiff_state fsl_cidiff_state_empty;

/**
   A utility for reporting the differences between the manifests of
   two checkins.

   This loads the fsl_deck instances for each version opt->v1 and
   opt->v2, then calls opt->callback for each step of the
   difference-checking process. It only inspects and reports the
   difference between _exactly_ v1 and _exactly_ v2, not the
   difference between the chain of SCM DAG relatives (if any) between
   the two.

   It is not required that v1 and v2 be related in the SCM DAG but its
   report of differences may be misleading if v1 and v2 are either
   unrelated or separated by more than 1 step in the DAG. See the docs
   for fsl_cidiff_e for cases known to be potentially confusing.

   Returns 0 on success. If the callback returns non-0, that result is
   propagated back to the caller. It may otherwise return any number of
   othe result codes including, but not limited to:

   - FSL_RC_OOM on allocation error

   - FSL_RC_NOT_A_REPO if f has no opened repository.

   - FSL_RC_TYPE if either of the given versions is not a checkin.

   - FSL_RC_NOT_FOUND if either of given versions is not found in the
   repository.


   Potential TODO: add fsl_deck_diff() which takes two decks, instead
   of RIDs, to compare. This function would just be a proxy for that
   one, loading the given RIDs and passing on the decks. That would
   require a significantly larger set of possible change-type values
   and would require much type-specific handling (e.g. maybe reporting
   J-card differences for a pair of ticket decks).
*/
FSL_EXPORT int fsl_cidiff(fsl_cx * f, fsl_cidiff_opt const * opt);


/**
   Configuration parameters for fsl_repo_create().  Always
   copy-construct these from fsl_repo_create_opt_empty
   resp. fsl_repo_create_opt_empty_m in order to ensure proper
   behaviour vis-a-vis default values.

   TODOs:

   - Add project name/description, and possibly other
   configuration bits.

   - Allow client to set password for default user (currently set
   randomly, as fossil(1) does).
*/
struct fsl_repo_create_opt {
  /**
     The file name for the new repository.
  */
  char const * filename;
  /**
     Fossil user name for the admin user in the new repo.  If NULL,
     defaults to the Fossil context's user (see
     fsl_cx_user_get()). If that is NULL, it defaults to
     "root" for historical reasons.
  */
  char const * username;

  /**
     The comment text used for the initial commit. If NULL or empty
     (starts with a NUL byte) then no initial check is
     created. fossil(1) is largely untested with that scenario (but
     it seems to work), so for compatibility it is not recommended
     that this be set to NULL.

     The default value (when copy-initialized) is "egg". There's a
     story behind the use of "egg" as the initial checkin comment,
     and it all started with a typo: "initial chicken"
  */
  char const * commitMessage;

  /**
     Mime type for the commit message (manifest N-card). Manifests
     support this but fossil(1) has never (as of 2025-07) made use of
     it. It is provided for completeness but should, for
     compatibility's sake, probably not be set, as the fossil UI may
     not honor it. The implied default is text/x-fossil-wiki. Other
     ostensibly legal values include text/plain and text/x-markdown.
     This API will accept any value, but results are technically
     undefined with any values other than those listed above.
  */
  char const * commitMessageMimetype;

  /**
     If not NULL and not empty, fsl_repo_create() will use this
     repository database to copy the configuration, copying over
     the following settings:

     - The reportfmt table, overwriting any existing entries.

     - The user table fields (cap, info, mtime, photo) are copied for
       the "system users": anonymous, nobody, developer, reader.

     - The vast majority of the config table is copied, arguably more
       than it should (e.g. the 'manifest' setting).
  */
  char const * configRepo;

  /**
     If false, fsl_repo_create() will fail if this->filename
     already exists.
  */
  bool allowOverwrite;

  /**
     If true, the new repo will not have a "project-code" config
     entry. Only set this to true when the next operation on this repo
     will be an inbound clone.
   */
  bool elideProjectCode;
};
typedef struct fsl_repo_create_opt fsl_repo_create_opt;

/** Initialized-with-defaults fsl_repo_create_opt struct, intended
    for in-struct initialization. */
#define fsl_repo_create_opt_empty_m { \
    .filename=NULL,                   \
    .username=NULL,                   \
    .commitMessage="egg",             \
    .commitMessageMimetype=NULL,      \
    .configRepo=NULL,                 \
    .allowOverwrite=false,            \
    .elideProjectCode=false           \
  }

/** Initialized-with-defaults fsl_repo_create_opt struct, intended
    for copy-initialization. */
FSL_EXPORT const fsl_repo_create_opt fsl_repo_create_opt_empty;

/**
   Creates a new repository database using the options provided in the
   second argument. If f is not NULL, it must be a valid context
   instance, though it need not have an opened checkout/repository. If
   f has an opened repo or checkout, this routine closes them but that
   closing _will fail_ if a transaction is currently active.

   If f is NULL, a temporary context is used for creating the
   repository, in which case the caller will not have access to
   detailed error information (only the result code) if this operation
   fails. In that case, the resulting repository file will, on
   success, be found at the location referred to by opt.filename.

   The opt argument may not be NULL.

   If opt->allowOverwrite is false (0) and the file exists, it fails
   with FSL_RC_ALREADY_EXISTS, otherwise is creates/overwrites the
   file. This is a destructive operation if opt->allowOverwrite is
   true, so be careful: the existing database will be truncated and
   re-created.

   This operation installs the various "static" repository schemas
   into the db, sets up some default settings, and installs a
   default user.

   This operation always closes any repository/checkout opened by f
   because setting up the new db requires wiring it to f to set up
   some of the db-side infrastructure. The one exception is if
   argument validation fails, in which case f's repo/checkout-related
   state are not modified. Note that closing will fail if a
   transaction is currently active and that, in turn, will cause this
   operation to fail.

   See the fsl_repo_create_opt docs for more details regarding the
   creation options.

   On success, 0 is returned and f (if not NULL) is left with the
   new repository opened and ready for use. On error, f's error
   state is updated and any number of the FSL_RC_xxx codes may be
   returned - there are no less than 30 different _potential_ error
   conditions on the way to creating a new repository.

   If initialization of the repository fails, this routine will
   attempt to remove its partially-initialize corpse from the
   filesystem but will ignore any errors encountered while doing so.

   Example usage:

   ```
   fsl_repo_create_opt opt = fsl_repo_create_opt_empty;
   int rc;
   opt.filename = "my.fossil";
   // ... any other opt.xxx you want to set, e.g.:
   // opt.user = "fred";
   // Assume fsl is a valid fsl_cx instance:
   rc = fsl_repo_create(fsl, &opt);
   if(rc) { ...error... }
   else {
     fsl_db * db = fsl_cx_db_repo(f);
     assert(db); // == the new repo db
   ...
   }
   ```

   @see fsl_repo_open()
   @see fsl_repo_close()
*/
FSL_EXPORT int fsl_repo_create(fsl_cx * f, fsl_repo_create_opt const * opt );

/**
   UNTESTED.

   Returns true if f has an opened repository database which is
   opened in read-only mode, else returns false.
*/
FSL_EXPORT bool fsl_repo_is_readonly(fsl_cx const * f);

/**
   Searches for matching blob records for a fossil ticket ID. (A
   ticket's ID is the value of its K-card.) The given ticket ID must
   be a full-length ticket ID (40 characters of lower-case hex) or an
   unambiguous prefix.

   If at least one match is found, it returns 0 and updates the 3rd
   argument: `*ridList` is assigned to an array of fsl_id_t
   (`blob.rid` values) terminated by an entry with the value
   0. Ownership of the array is passed to the caller, who must
   eventually pass it to fsl_free(). The entries are ordered from
   oldest to newest (based on their `tagxref.mtime` value).

   On error it may return:

   - FSL_RC_OOM on allocation error.
   - FSL_RC_RANGE if the given ticket ID is longer than FSL_STRLEN_SHA1.
   - FSL_RC_AMBIGUOUS if the ticket ID prefix would match multiple distinct
     ticket IDs. This result is not possible if passed a full-length ticket
     ID.
   - FSL_RC_NOT_FOUND if no match is found.
   - FSL_RC_NOT_A_REPO if f has no repository opened.
   - Any number of db-related errors if something goes horribly wrong
     in the db layer.

   On error, the 3rd argument is not modified.
*/
FSL_EXPORT int fsl_tkt_id_to_rids(fsl_cx * f, char const * tktId,
                                  fsl_id_t ** ridList);

/**
   Models user permissions precisely as fossil(1) does. Most of these
   do not apply to the library - they are "app-level" permissions.
   Some very few are honored by the library, namely those relevant for
   sync support.  The app-level permissions are exposed here so that
   app-level code can handle those in ways compatible with fossil(1),
   while leaving the library to handle the permission storage (via the
   `user.cap` repository db field).
*/
struct fsl_uperm {
 bool setup;            /* s: use Setup screens on web interface */
 bool admin;            /* a: administrative permission */
 bool password;         /* p: change password */
 bool write;            /* i: xfer inbound. check-in */
 bool read;             /* o: xfer outbound. check-out */
 bool hyperlink;        /* h: enable the display of hyperlinks */
 bool clone;            /* g: clone */
 bool readWiki;         /* j: view wiki via web */
 bool newWiki;          /* f: create new wiki via web */
 bool appendWiki;       /* m: append to wiki via web */
 bool writeWiki;        /* k: edit wiki via web */
 bool moderateWiki;     /* l: approve and publish wiki content (Moderator) */
 bool readTicket;       /* r: view tickets via web */
 bool newTicket;        /* n: create new tickets */
 bool appendTicket;     /* c: append to tickets via the web */
 bool writeTicket;      /* w: make changes to tickets via web */
 bool moderateTicket;   /* q: approve and publish ticket changes (Moderator) */
 bool attach;           /* b: add attachments */
 bool ticketReport;     /* t: create new ticket report formats */
 bool readAddr;         /* e: read email addresses or other private data */
 bool zip;              /* z: download zipped artifact via /zip URL */
 bool privateContent;   /* x: can send and receive private content */
 bool writeUnversioned; /* y: can push unversioned content */
 bool readForum;        /* 2: Read forum posts */
 bool writeForum;       /* 3: Create new forum posts */
 bool trustedForum;     /* 4: Post to forums not subject to moderation */
 bool moderateForum;    /* 5: Moderate (approve or reject) forum posts */
 bool adminForum;       /* 6: Grant capability 4 to other users */
 bool emailAlert;       /* 7: Sign up for email notifications */
 bool announce;         /* A: Send announcements */
 bool chat;             /* C: read or write the chatroom */
 bool debug;            /* D: show extra Fossil debugging features */
  /* These last two represent capabilities inherited from predefined
     users. */
 bool xReader;          /* u: Inherit all privileges of "reader" */
 bool xDeveloper;       /* v: Inherit all privileges of "developer" */
};

/**
   Helper for mass-handling of fsl_uperm permissions (e.g. generating
   switch/case blocks).  Usage:

   1) #define a macro which takes two argument: the fsl_uperm member
     name (identifier) and its corresponding capabilities character
     (as a char literal).

   2) Invoke this macro, passing it the name of (1).

   The (1) macro will be invoked one time per fsl_uperm member,
   passing it (memberName,aChar). No commas or semicolons are emited
   between calls, so they need to be included in the definition of (1)
   if they're relevant.

   This macro does not include the "inehrits from" permissions
   (xReader, xDeveloper). Use fsl_uperm_map_inherited(M) or
   fsl_uperm_map_all(M) to include those.
*/
#define fsl_uperm_map_base(M)        \
  M(setup,'s') M(admin,'a') M(password,'p')              \
  M(write,'i') M(read,'o') M(hyperlink,'h') M(clone,'g') \
  M(readWiki,'j') M(newWiki,'f') M(appendWiki,'m') M(writeWiki,'k') \
  M(moderateWiki,'l') M(readTicket,'r') M(newTicket,'n') M(appendTicket,'c') \
  M(writeTicket,'w') M(moderateTicket,'q') M(attach,'b') M(ticketReport,'t') \
  M(readAddr,'e') M(zip,'z') M(privateContent,'x') M(writeUnversioned,'y') \
  M(readForum,'2') M(writeForum,'3') M(trustedForum,'4') M(moderateForum,'5') \
  M(adminForum,'6') M(emailAlert,'7') M(announce,'A') M(chat,'C') \
  M(debug,'D')

/**
   Like fsl_uperm_map_base() but only includes the "inherits from"
   mebmers.
*/
#define fsl_uperm_map_inherited(M) \
  M(xReader,'u') M(xDeveloper,'v')

#define fsl_uperm_map_all(M) \
  fsl_uperm_map_base(M) fsl_uperm_map_inherited(M)

/** @internal */
#define fsl__uperm_map_init(M,CH) .M=false,

/**
   Const-copy initializer for a fsl_uperm.
*/
#define fsl_uperm_empty_m { fsl_uperm_map_all(fsl__uperm_map_init) }

/**
   A fsl_uperm instance with all members set to false.
*/
extern const fsl_uperm fsl_uperm_empty;
/**
   A fsl_uperm instance with all members set to true except for the
   "inherits from" properties (xReader and xDeveloper), as those
   permissions are already implied in the setting of all other
   permissions to true.
*/
extern const fsl_uperm fsl_uperm_all;

/**
   Adds any permissions specified in zCap to pTgt. It does not
   modify permissions which are not in zCap.

   Returns 0 on success, non-0 on error. If zCap contains any
   unhandled characters it returns FSL_RC_RANGE, with the exception of
   spaces and tabs, which it ignores. It is not an error if zCap is
   empty or contains only spaces/tabs.

   If it returns non-0, pTgt's state is not well-defined. It may have
   been partially updated before the failure.

   Any given permission letter may imply other permissions. e.g.  the
   'a' and 's' permissions imply most others.

   f is optional (may be NULL) unless zCap contains 'u' or 'v', in
   which case f is required in order to collect the permissions for
   the the "reader" resp. "developer" user. If f is NULL in those
   cases, FSL_RC_MISUSE is returned:
*/
FSL_EXPORT int fsl_uperm_add(fsl_cx * f, fsl_uperm * pTgt, char const *zCap);


/**
   Fetches f's current project code. On success, assigns it to *pOut
   and returns 0. Caches the result for future calls. Each repository
   has a project code which is shared across all of its clones.
*/
FSL_EXPORT int fsl_repo_project_code( fsl_cx * f, char const **pOut );


#if defined(__cplusplus)
} /*extern "C"*/
#endif
#endif
/* ORG_FOSSIL_SCM_FSL_REPO_H_INCLUDED */
