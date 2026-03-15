/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
#if !defined(ORG_FOSSIL_SCM_FSL_SYNC_H_INCLUDED)
#define ORG_FOSSIL_SCM_FSL_SYNC_H_INCLUDED
/*
  Copyright 2025 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2025 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).
*/

/** @file sync.h

    sync.h declares APIs dealing with fossil repository
    synchronization.
*/

#include "core.h" /* MUST come first b/c of config macros */
#include "repo.h"

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct fsl_sc fsl_sc;

/**
   Callback for xfer-layer debugging messages. TODO consolidate this
   with the fsl_msg system.
 */
typedef void (*fsl_xfer_dbg_f)(fsl_xfer *xf, char const * zMsg);

/**
   Internal part of fsl_sc, uplifted into its own struct to simplify
   proper intialization of them in downstream fsl_sc implementations.
*/
struct fsl_xfer_dbg {
  /**
     This optional method is intended to be set by client-level code,
     not the sync channel implementor.

     If not NULL, the library or fsl_sc implementations may emit
     debugging output via this function.

     For output channel legibility, zMsg should always end with a
     newline (fsl_xfer_debug() and friends will automatically add one if
     needed).

     Implementations are free to mangle or emit the message in any
     form they like.

     Design note: its return type is void, but experience with binding
     C to Java and C++ shows that such bindings may need to propagate
     unexpected exceptions (e.g. resource acquisition failure, logging
     failure, etc.) somehow through the C API, and the conventional
     generic way to do that is to have a result code.  In normal use,
     it is never expected that a debugging function fails in a way
     which impacts the non-debugging functionality, but if an
     implementation absolutely must report an error, they should call
     fsl_xfer_error(), and that will be propagated back from this
     call. Channels, however, are _not_ required to check that after
     every debug call - they may assume success when the debug
     function is called, as the debug channel should not interfere
     with the sync channel's operation.

     @see fsl_xfer_debug_f_FILE()
     @see fsl_xfer_debug_f_buffer()
  */
  fsl_xfer_dbg_f callback;

  /**
     Available in callback() implementations via ch->debug.state.

     This memory is not owned by this object. It's owned by whoever
     set this member to non-NULL.
  */
  void * state;

  /**
     This is a scratch buffer solely for internal use by:

     1) library APIs like fsl_xfer_debugf()

     2) fsl_sc method implementations which do not themselves call
        into (1) while using this buffer.

     Without this, fsl_xfer_debug() becomes unduly memory-costly. This
     memory is owned by the library and gets cleared by
     fsl_sc_cleanup().  fsl_sc::cleanup() implementations may clean it
     up but are not required to.
  */
  fsl_buffer b;
};

typedef struct fsl_xfer_dbg fsl_xfer_dbg;

/** Empty-initialized fsl_xfer_dbg instance for use in cleanly
    const-copy initializing objects. */
#define fsl_xfer_dbg_empty_m {.callback=0, .state=0, .b=fsl_buffer_empty_m}

/** Empty-initialized instance for use in cleanly initializing
    objects. */
extern const fsl_xfer_dbg fsl_xfer_dbg_empty;

/**
   Frees any memory which may be owned by p but does not
   free p.
*/
FSL_EXPORT void fsl_xfer_dbg_cleanup( fsl_xfer_dbg * p );

/**
   Flags for use with fsl_sc::flags. The top-most 8 bits are reserved
   for use by common library-level flags which a fsl_sc implementation
   may choose to support or may be required to support.
*/
enum fsl_sc_F_lib_e {
  /**
     Tells the fsl_sc impl that it should post data using the HTTP
     Content-Type value of "application/x-fossil" instead of
     "application/x-fossil-uncompressed". The former will have a
     fossil-compressed response payload[^1] whereas the latter will
     have an uncompressed response. Compressed payloads require far
     more memory to process but can save considerable network
     bandwidth.

     fsl_sc implementations are required to honor this flag, in that
     they must set the appropriate Content-Type request header, so
     that the library can Do The Right Thing without requiring that
     each fsl_sc implementation know to stream or buffer the
     compressed content.

     This is a _hint_ for the remote, but the remote may override
     that, sending uncompressed responses instead of compressed.
     The remote will never do the opposite, convering a request for
     an uncompressed response to a compressed one.

     Compression may be handled differently for certain sync response
     types. e.g., a protocol-version-3 clone request will _not_ emit a
     fully-compressed response because most of the response body is
     already compressed in that case. The fsl_sc implementations need
     not concern themselves with that, however.

     Similarly, then fossil is operated via its "test-http" facility,
     it does not compress responses.

     When this flag is set, the sync process will read all of the
     response content from the fsl_sc instance and then take over
     reading of the response from there so that fsl_sc implementations
     do not have to deal with those details.

     Because compression support is so memory-hungry, it is opt-in
     flag instead of an opt-out. Compression will save network
     bandwidth but costs the client application and the remote fossil
     instance boatloads of RAM.

     [^1]: zlib compress() with a 4-byte big-endian header encoding
           the decompressed size.
  */
  FSL_SC_F_COMPRESSED = 0x40000000,

  /**
     For impls with this flag set, the library will populate their
     fsl_sc::requestHeaders immediately before calling the impl's
     submit() method. Some impls generate their own headers or feed
     them into a proxy application in a different way, so don't need
     this. e.g. the curl-binary-based impl passes headers to curl
     in the form of CLI flags.
  */
  FSL_SC_F_REQUEST_HEADERS = 0x20000000,

  /**
     Impls which use temp files must honor this flag by _not_ deleting
     those files, so that they can be examined for testing and
     debugging.

     Implementations are recommended to send a fsl_xfer_emit_msg()
     message with the type FSL_MSG_TMPFILE and the name of the
     file in the message at the time when they would have otherwise
     been deleted, to facilitate matching temp files to their purpose.
  */
  FSL_SC_F_LEAVE_TEMP_FILES = 0x10000000,

  /**
     Impls which might, due to shelling out, emit info to stderr are
     expected to honor this flag and redirect stderr to /dev/null (or
     equivalent) if at all possible.
  */
  FSL_SC_F_SUPPRESS_STDERR = 0x08000000,

  /**
     The mask of FSL_SC_F_... bits which are reserved for use by
     library-level options.
  */
  FSL_SC_F_lib_mask = 0x7FFF0000,
  /**
     The mask of FSL_SC_F_... bits which are reserved for use by
     client-level options.
  */
  FSL_SC_F_client_mask = ~FSL_SC_F_lib_mask
};

/**
   Indicates that fsl_sc::read() must read exactly 1 line of
   input. Its precise value is not well-defined but it is guaranteed
   to be of the same type as fsl_sc::read()'s final parameter. It is
   not an enum entry because we cannot (in C99) portably make an enum
   be precisely of a specific integer type.
*/
#define FSL_SC_READ_LINE ((fsl_int_t)-1)

/**
   Indicates that fsl_sc::read() must transfer or copy all of its
   input into the result buffer. See FSL_SC_READ_LINE for more
   details.
*/
#define FSL_SC_READ_ALL ((fsl_int_t)-2147483648)

/**
   Initialization modes for use with fsl_sc::init() implementations.
*/
enum fsl_sc_init_e {
  /**
     Indicates that the fsl_sc instance should initialize any core
     resources but not any request/response resources.

     See fsl_sc::init() for more details.
  */
  FSL_SC_INIT_INITIAL = 0x01,

  /**
     Indicates that the object must (re)initialize any state needed
     for constructing a request. e.g. initializing a buffer or temp
     file. The xfer layer may use this to compose a follow-up response
     while it is still reading a remote's response.

     See fsl_sc::init() for more details.

     Design note: there is no FSL_SC_INIT_RESPONSE counterpart because
     that role is effectively filled by fsl_sc::submit(), which
     initiates the processing of any response.
  */
  FSL_SC_INIT_REQUEST = 0x02
};

/**
   The fsl_sc class encapsulates a streamable (as opposed to
   random-access) I/O channel intended solely for use in implementing
   fossil(1)'s sync protocol in libfossil in an I/O-channel-agnostic
   manner.  (Thus the name "sc" - an abbreviation of "sync channel".)
   The idea is that we can hook up arbitrary streamable I/O channels
   via this interface, e.g. HTTP-, SSH-, and file-driven channels.

   Each instance must be fully populated to set up its fsl_cx, I/O
   methods, and non-NULL values for all requires methods (init(),
   cleanup(), etc.). Each instance may have arbitrary state associated
   with it, but whether such state must be provided by the user, or is
   created/initialized by the open() implementation, is
   implementation-defined.

   A channel's lifecycle, in terms of its API, goes something like:

   - User populates a state object of the
     type required by the channel.

   - User cleanly initializes a fsl_sc instance by copy-initializing
     it from a known-good starting point, e.g. fsl_sc_empty or
     fsl_sc_popen. We'll call this object "sc".

   - User sets sc.state.p if necessary for the implementation.
     e.g. fsl_sc_popen requires that its state pointer refer to a
     properly-populated fsl_sc_popen_state object, e.g. a copy of
     fsl_sc_popen_state_curl.

   - User passes it off to the library for handling. That API is still
     TBD.

   - Its init() method is called.

   - Its append() method will be called an arbitrary number of times
     to populate it with a fossil sync request payload.

   - Its submit() method is called to finalize the request payload
     and send it wherever it needs to go.

   - Its read() method is used to read the response payload. Sync
     protocol "cards" are read line-by-line and card-level payloads
     are read in their-payload-sized chunks.

   - For a multi-round-trip sync, its init() method may be called once
     per round trip, starting the process over.

   - Its cleanup() method is called to clean it up.


   Open design issues:

   - For some channels, the model does not support that posted content
     can be passed through without an extra copy, as we can't generate
     the channel-specific headers until we have the content, and need
     a second buffer to concatenate the headers and content.
     Accounting for HTTP's Content-Size header and the sync login card
     (also sent via HTTP header) are the main culprits here.

   - The status message interface may not be sufficient. We need to
     empower apps to do things like display progress dialogs, even if
     they're not as granular as real-time counts of network traffic
     and related channel-specific state.
*/
struct fsl_sc {
  /**
     The "transfer context" for this channel. This is set by the
     library and is needed for calling functions like fsl_xfer_emit()
     from within fsl_sc member function code.
  */
  fsl_xfer * xfer;

  /**
     A debugging-purposes name for this channel,
     e.g. "http-socket". The bytes are considered to be static, and
     must outlive any instance of this class which uses them.
  */
  char const * name;

  /**
     The remote fossil URL.

     It is up to the client to either set up the URL before passing
     this object on to the library or to ensure that the last-sync-url
     is of a schema compatible with this instance.

     It is also up to the client to ensure that the URL scheme matches
     the fsl_sc implementation. How best to do that is still TBD.

     This memory is owned by this object.
  */
  fsl_url url;

  /**
     Flags for customizing certain behaviors.
  */
  fsl_flag32_t flags;

  /**
     Implementation-dependent state.
   */
  struct {
    /**
       Arbitrary state for implementations. Its ownership is
       implementation-defined, as is whether this object's init()
       method will populate this or whether the client must provide
       it. If this object owns it, its cleanup() method must free the
       memory and clear this member.
    */
    void * p;
    /**
       An optional value which uniquely identifies with a specific
       type. Intended as a semi-type-safety measure for this->state.p.
       i.e. don't assume p is of the desired type unless this matches
       some well-defined value (which need not be public).
    */
    void const * type;
  } impl;

  /**
     This callback's responsibility is to initialize and validate any
     resources which may be implied via this->impl.

     The second argument tells the implementation what type of
     initialization is required, as per the fsl_sc_init_e docs.

     FSL_SC_INIT_INITIAL is only ever passed one time per object and
     is guaranteed to be called before FSL_SC_INIT_REQUEST is. If the
     channel needs to perform any validation, that is the time to do
     is.

     FSL_SC_INIT_REQUEST is called each time a new request body needs
     to be generated. Implementations must discard any prior
     request-specific state when initialized with this value.

     ch->flags must be set before calling this and must not be
     modified afterwards. The library will set the ch->xfer member.

     It is up to the direct users of this API to ensure that their
     fsl_sc instances contain no randomly-initialized state before the
     initial call to this function.

     This call may, depending on the impl, initialize ch->impl,
     whereas some channel implementations may require that the caller
     populate ch->impl with an implementation-defined "something".
     Those details are to be documented for each concrete
     implementation.

     Implementations must return 0 on success. On error they must
     return a fsl_rc_e value and should ideally use fsl_xfer_error()
     on ch->xfer to report an error message.

     The library guarantees that a db transaction will be in effect
     when it calls this function, and that failure from any fsl_sc API
     will be propagated back and cause the transaction to roll back.

     Regardless of success or failure, clients are required to
     eventually call the cleanup() method if they call init(). On a
     failed init() call, it is not generically knowable whether it's
     okay to call init() again, and clients should not do so.

     Whether or not this step will initialize any network and/or FILE
     and/or RPC connections is left to the implementation. It may do
     so here, if needed, or it may delay such things until submit().
  */
  int (*init)(fsl_sc *ch, enum fsl_sc_init_e mode);

  /**
     Must append the first n bytes of pIn to the outbound sync request
     payload. The library will call this an arbitrary number of times
     to generate the sync body payload.

     Implementations are free to buffer, or not, the payload,
     depending on their requirements, but practical considerations may
     make buffering unavoidable. e.g. an HTTP implementation will
     require the full payload before sending, so that the Content-Size
     header can be set and the headers pushed before the payload is. A
     streaming-to-(FILE*) implementation, however, has no such
     requirements, so could avoid an extra buffering step (unless,
     perhaps, that file's contents are intended for further processing
     by, e.g., netcat(1), in which case HTTP headers would need to be
     prepended).

     Achtung: calling this API directly from client code will
     effectively corrupt the output. The library internals funnel all
     fsl_sc output traffic through a single point so that the size of
     the request and its SHA1 hash can be kept track of (both are
     needed for sending authenticated requests). Calling this API
     directly will bypass that and result in request bodies whose
     hashes do not match fossil's authentication expectations.
  */
  int (*append)(fsl_sc *ch, void const * pIn, fsl_size_t n);

  /**
     Tells the channel that the library is done building the request
     payload and it should be submitted.

     If this method buffers its response, it may close (if still
     opened) any channel opened by init() but should retain any state
     which might be needed for reestablishing that connection. If it
     does not buffer the response, it may keep the channel open for
     downstream read() calls.

     This is never called before init() is passed FSL_SC_INIT_REQUEST
     and is never called when any call to init() fails.

     If loginCard is not NULL, it contains the full content of a login
     card for the request. Implementations must emit these bytes
     as an HTTP header in this form:

     ```
     Cookie: x-f-l-c=UrlEncodedValueOfTheCard
     ```

     Which, in C, might look something like:

     ```
     char * z = fsl_mprtinf("Cookie: x-f-l-c=%t\r\n",
                            fsl_buffer_cstr(loginCard));
     fsl_free(z);
     ```

     (Sidebar: that approach to authentication only works with fossil
     versions newer than 2025-07-27, so the library cannot log in
     to older fossil versions but can otherwise speak to them.)

     The library guarantees that the same db transaction as was in
     effect when ch->init() was called will be in place when this is
     called.

     Implementations must return 0 on success. On error,
     implementations must return a non-FSL_RC_OK value from the
     fsl_rc_e enum, and are strongly encouraged to set an error
     message using the fsl_xfer_error() family of functions, passing
     ch->xfer to it.
  */
  int (*submit)(fsl_sc *ch, fsl_buffer const *loginCard);

  /**
     Must read, or consume from its buffer, the requested amount of
     data and append it to tgt.  It must only append to the target
     buffer, not reset or otherwise manipulate it, as the buffer may
     already contain state when this is called.

     If howMuch is greater than zero, it's a number of bytes which are
     expected to be appended to tgt. A non-0 result code must be
     returned if exactly that many bytes cannot be consumed.

     If howMuch is FSL_SC_READ_LINE then:

     1) One newline-separated (0x0A) line must read from the input
        source and be append to tgt, including the newline character.
        Aside from interpretation of that one character,
        implementations must treat the preceeding data (if any) as
        opaque. i.e. it must not be otherwise parsed and empty lines
        must not be skipped.

     2) If no newline is found, return FSL_RC_BREAK if at EOF and no
        data precedes the EOF, else some other non-0 error code
        (e.g. FSL_RC_IO). The library will interpret FSL_RC_BREAK as a
        non-error EOF condition.

     If howMuch is FSL_SC_READ_ALL then the library must read the
     whole response into the target buffer. (This is used by the
     library to handle fossil-compressed response bodies on behalf of
     fsl_sc implementations.) If the implementation internally stores
     the response in fsl_buffer, it should use fsl_buffer_swap() to
     transfer it to tgt, rather than copy it.  If it uses a file,
     fsl_stream() can be used to transfer it, e.g.:

     ```
     fsl_stream(fsl_input_f_FILE, responseFile,
               fsl_output_f_buffer, tgt);
     // or:
     fsl_stream(fsl_input_f_fd, &myFileDescriptor,
                fsl_output_f_buffer, tgt);
     ```

     When howMuch is FSL_SC_READ_ALL, the library will not read any
     more from the channel until/unless ch->submit() is called again,
     so the implementation may free up any related resources at that
     point if it likes.

     The library will never pass a howMuch value of 0 to this
     function, but it reserves the option of eventually using 0 as a
     way of communicating that we are done reading, if such a
     capability ever becomes useful. For future compatibility's sake,
     it's legal to simply return 0 if howMuch is 0, but it's also
     legal to assert(howMuch!=0) until that happens.

     Must return 0 on success, non-0 on error, and should use one of
     the fsl_xfer_error() family of functions to to provide any error
     message and propagate the error through the library.
  */
  int (*read)(fsl_sc *ch, fsl_buffer * tgt, fsl_int_t howMuch);

  /**
     Must free up all dynamically-allocated resources owned by ch and
     ch->impl.

     Finalizing must, if appropriate, free up any resources it uses,
     but must not free the object passed to it (which might be
     stack-allocated or part of a larger struct). If this connection
     is still open, it must close it.

     It is up to the implementation to set ch->impl.p to 0 if that's
     appropriate for them - the library does not do so. Some
     implementations may use static state and not need to reset
     ch->impl.

     In the wisdom of "destructors must not throw," the library
     provides no way to report a failure to close a channel (nor does
     it really care, at that point - by then the channel will have
     either already failed or served its purpose). If, after this
     call, ch->f has error state, the library has the option, at its
     developers' discretion, of emitting a warning to the client (this
     is unspecified and currently unimplemented, but warning and/or
     debug channels may be added at some point).

     The library will never pass NULL to this function.

     The library does not guaranty that a db transaction will be in
     place when this is called.

     This method must never be called directly from client
     code. Instead, pass the object to fsl_sc_cleanup(), as that will
     free up the common fsl_sc members, leaving clean() impls to only
     concern themselves with their own private state.
  */
  void (*cleanup)(fsl_sc *ch);

  /**
     This is internal-use tooling to support adding "shim" layers of
     fsl_sc between the library and the "real" fsl_sc. The default
     impl (fsl_sc_self()) is suitable for all non-proxying channels
     and simply returns its argument. The impl for the tracer channel
     returns the object it's tracing. This distinction is relevant for
     handing of fsl_sc internals such as ch->url and ch->flags, in
     that (A) the internals always want those values to come from the
     "real" implementation, not the shim, and (B) they don't want code
     which is using a shim to have to know it's using a shim.  Calling
     ch->self(ch)->url will return the "correct" URL object.

     Implementation code of a fsl_sc, unless that channel is itself a
     proxy for another, should not use this - it's simply not needed
     there.  It's used by library code which wants to avoid having to
     know there's a shim/proxy channel in place. Implementors of proxy
     channels must provide a self() implementation which looks
     something like the following (taken from the tracer channel):

     ```
     // Custom channel state, set in fsl_sc::impl.p of channels
     // of this subtype.
     struct my_channel_state {
       fsl_sc * proxied; // proxied channel
       ... any other necessary state ...
     };
     typedef struct my_channel_state my_channel_state;

     fsl_sc * my_self(fsl_sc *ch){
       my_channel_state * const st = ch->impl.p;
       fsl_sc * const orig = st->proxied;
       return orig->self(orig); // call self() in case orig is a proxy!
     }
     ```
  */
  fsl_sc * (*self)(fsl_sc *ch);

  /**
     If this->flags contains FSL_SC_F_REQUEST_HEADERS, the library
     will populate this with raw HTTP headers for the request,
     including a trailing `\r\n` line, immediately before calling
     submit().  Implementations' submit() methods must, unless they do
     their own header generation, emit this before the body.  See
     FSL_SC_F_REQUEST_HEADERS.
  */
  fsl_buffer requestHeaders;
  /**
     A general-purpose scratchpad buffer for use by the channel object
     and the library internals.
  */
  fsl_buffer scratch;
}/*fsl_sc*/;

/**
   An implementation for fsl_xfer::debug::callback which requires
   that the associated state be a (FILE*) handle opened for output.
   Each call sends zMsg to that FILE handle via fprintf(), without
   additional decoration.
*/
FSL_EXPORT void fsl_xfer_debug_f_FILE(fsl_xfer *xf, char const * zMsg);

/**
   An implementation for fsl_xfer::debug::callback which requires
   that the associated state be a (fsl_buffer*). On each call zMsg is
   appended as-is to that buffer, without additional decoration.
*/
FSL_EXPORT void fsl_xfer_debug_f_buffer(fsl_xfer *xf, char const * zMsg);

/**
   The default implementation of fsl_sc::self(). Simply returns
   ch.
*/
FSL_EXPORT fsl_sc * fsl_sc_self(fsl_sc *ch);

/** Empty-initialized fsl_sc instance, intended for const-copy
    initialization. */
#define fsl_sc_empty_m {        \
    .xfer=0, .name="unnamed", \
    .url=fsl_url_empty_m,       \
    .flags = 0,                 \
    .impl = {.p = 0,.type = 0}, \
    .init=0, .append = 0,       \
    .submit=0,                  \
    .read=0, .cleanup=0,        \
    .self=fsl_sc_self,           \
    .requestHeaders = fsl_buffer_empty_m,     \
    .scratch = fsl_buffer_empty_m     \
}

/** Empty-initialized fsl_sc instance, intended for copy
    initialization. */
extern const fsl_sc fsl_sc_empty;


#if 0
enum fsl_sync_e {
  /* Adapted from fossil(1)'s xfer.c */
  FSL_SYNC_PUSH        = 1<<0, /* push content client to server */
  FSL_SYNC_PULL        = 1<<1, /* pull content server to client */
  FSL_SYNC_BIDI = FSL_SYNC_PUSH | FSL_SYNC_PULL,
  FSL_SYNC_CLONE       = 1<<2, /* clone the repository */
  FSL_SYNC_PRIVATE     = 1<<3, /* Also transfer private content */
  FSL_SYNC_VERBOSE     = 1<<4, /* Extra diagnostics */
  FSL_SYNC_RESYNC      = 1<<5, /* --verily */
  FSL_SYNC_DEBUG       = 1<<6, /* Install a testing-only debug func in the channel */
  FSL_SYNC_DRYRUN      = 1<<7, /* roll back transaction when done */
  FSL_SYNC_AUTO        = 1<<8, /* use autosync setting */
  //FSL_SYNC_FROMPARENT = 0x00040, /* Pull from the parent project */
  //FSL_SYNC_UNVERSIONED = 0x00100, /* Sync unversioned content */
  //FSL_SYNC_UV_REVERT = 0x00200, /* Copy server unversioned to client */
  //FSL_SYNC_UV_TRACE = 0x00400, /* Describe UV activities */
  //FSL_SYNC_UV_DRYRUN = 0x00800, /* Do not actually exchange files */
  //FSL_SYNC_IFABLE = 0x01000, /* Inability to sync is not fatal */
  //FSL_SYNC_CKIN_LOCK = 0x02000, /* Lock the current check-in */
  //FSL_SYNC_NOHTTPCOMPRESS = 0x04000, /* Do not compression HTTP messages */
  //FSL_SYNC_ALLURL = 0x08000, /* The --all flag - sync to all URLs */
  //FSL_SYNC_SHARE_LINKS = 0x10000, /* Request alternate repo links */
  //FSL_SYNC_XVERBOSE = 0x20000, /* Extra verbose.  Network traffic */
};
#endif

/**
   Flags regarding which configuration options to sync.

   This should probably be strings so that we don't have to adapt this
   for every protocol addition.
*/
enum fsl_config_sync_e {
  FSL_CFG_SYNC_NONE         = 0,
  FSL_CFG_SYNC_CSS          = 1<<0, /* Style sheet only */
  FSL_CFG_SYNC_SKIN         = 1<<1, /* WWW interface appearance */
  FSL_CFG_SYNC_TKT          = 1<<2, /* Ticket configuration */
  FSL_CFG_SYNC_PROJ         = 1<<3, /* Project name */
  FSL_CFG_SYNC_SHUN         = 1<<4, /* Shun settings */
  FSL_CFG_SYNC_USER         = 1<<5, /* The USER table */
  FSL_CFG_SYNC_ADDR         = 1<<6, /* The CONCEALED table */
  FSL_CFG_SYNC_XFER         = 1<<7, /* Transfer configuration */
  FSL_CFG_SYNC_ALIAS        = 1<<8, /* URL Aliases */
  FSL_CFG_SYNC_SUBSCRIBER   = 1<<9, /* Email subscribers */
  FSL_CFG_SYNC_IWIKI        = 1<<10, /* Interwiki codes */
  FSL_CFG_SYNC_ALL          = 0x0000ffff, /* Everything */
  FSL_CFG_SYNC_OVERWRITE    = 0x10000000 /* Causes overwrite instead of merge */
};

/**
   NOT YET IMPLEMENTED. This interface is experimental and subject to
   change. The plan, however, is...

   Fluid. The plan is fluid.

   On success, returns 0, else it returns one of any number of
   fsl_rc_e codes and may be propagating error information accessible
   via fsl_cx_err_get().
*/
FSL_EXPORT int fsl_sync_client(fsl_cx * f, ...);

#if 0
/**
   Resets ch's buffers for re-use and then calls ch->submit().

   This is prefered to calling ch->submit() directly because this
   ensures that ch's buffers are accounted for.
*/
FSL_EXPORT void fsl_sc_close(fsl_sc * const ch);
#endif

/**
   Calls fsl_sc_close() then, if ch->cleanup is not NULL, it is
   called. Then it frees all of ch's buffers. Does not free ch.

   This is prefered to calling ch->cleanup() directly because this
   ensures that ch's buffers are accounted for.
*/
FSL_EXPORT void fsl_sc_cleanup(fsl_sc * ch);

/**
   If xf has a debug channel installed, the given
   fsl_appendf()-compatible arguments are posted to it in string
   form. The output may be prefixed with other information.

   If xf has no debugging channel then this function has no
   side-effects.

   This function adds a newline to the result if it does not end in
   one.

   Returns 0 on success and the only error case is a formatting buffer
   allocation error, which causes FSL_RC_OOM to be returned. Such
   results should not be propagated by client code - an OOM is
   innocuous in the context of debugging output.

   This function requires allocating memory, and re-uses a buffer
   dedicated to that purpose. In ideal cases, it won't have to
   allocate more than once during the lifetime of a given fsl_xf
   instance.

   WARNING: fsl_xfer_config::debug-bound logging functions must not
   call this, else there will be an endless loop and eventual stack
   overflow.
*/
FSL_EXPORT int fsl_xfer_debugf(fsl_xfer * xf, char const * zFmt, ...);

/** va_list variant of fsl_xfer_debugf(). */
FSL_EXPORT int fsl_xfer_debugfv(fsl_xfer * xf, char const * zFmt, va_list args);

/**
   Equivalent to fsl_xfer_debugf(ch,"%s",zMsg) but is more efficient
   than that.
*/
FSL_EXPORT void fsl_xfer_debug(fsl_xfer * xf, char const * zMsg);

/**
   Returns xf's fsl_cx context. This is used in fsl_sc callback
   implementations.
*/
FSL_EXPORT fsl_cx * fsl_xfer_cx(fsl_xfer * xf);

/**
   Sets xf's (persistent) error state to the given information
   and propagates it through to xf's fsl_cx. After this,
   further calls to fsl_xf-taking APIs will fail.

   This is intended to only be used from fsl_sc implementations
   and this library's own internals.

   This function prefixes the message with some form of ch->name and
   the fsl_rc_cstr() value of the given result code.

   Returns the given code, or FSL_RC_ERROR if code is 0. i.e.
   it never returns 0. A code value of 0 is semantically illegal in
   this context.
*/
FSL_EXPORT int fsl_xfer_errorf(fsl_xfer *xf, int code, char const * zFmt,
                               ...);

/** va_list variant of fsl_xfer_errorf(). */
FSL_EXPORT int fsl_xfer_errorfv(fsl_xfer *xf, int code, char const * zFmt,
                              va_list args);

/**
   Equivalent to fsl_xfer_errorf(ch,code,"%s",zMsg) but more
   efficient.
*/
FSL_EXPORT int fsl_xfer_error(fsl_xfer *xf, int code, char const * zMsg);


/**
   Policies for use with fsl_sc_popen indicating how to fetch the
   response from invoking an external command to transport fossil sync
   requests. This is all internal experimentation, not part of the
   public API.
*/
enum fsl_sc_popen_e {
  /**
     Directs the binary's stdout to a temp file bound to
     fsl_sc_popen_state::response::fp using fopen().

     This "should" work the same as fsl_sc_popen_e_direct but this one
     works where that one doesn't, for reasons unknown.

     Measurements show, somewhat surprisingly, this to be
     significantly faster than fsl_sc_popen_e_membuf (anywhere from
     30% to twice as fast, depending on the test run), as well as far
     memory-lighter: 410kb peak RAM vs 9.4MB peak ram for the same
     clone of fossil's canonical repository.
  */
  fsl_sc_popen_e_filebuf = 1,

  /**
     Don't use this except for experimentation.

     Slurp whole response into a fsl_buffer at
     fsl_sc_popen_state::response. This works but can use a huge
     amount of memory.

     This is slower than fsl_sc_popen_e_filebuf and much more
     memory-hungry.  The main speed culprit seems to be
     fsl_buffer_stream_lines(), which takes up some 25% of the test
     run's CPU instructions (as reported by callgrind).
  */

  fsl_sc_popen_e_membuf = 2,

  /**
     Does not work!

     Don't buffer: read response from the binary's stdout, bound to
     fsl_sc_popen_state::process::fdStdOut.

     This mode is currently failing with curious read failures: for
     _some_ cards, reading the payload leaves (apparently) pieces
     which are then read by the next read-line op and of course don't
     parse as a card. The cause is as-yet unknown but it's got to be a
     bug somewhere in this code.
  */
  fsl_sc_popen_e_direct = 3,

  /**
     Does not work!

     Read the binary's stdout using an unbuffered file descriptor
     open()ed on fsl_sc_popen_state::response::fd.

     This is is known to fail with "not found" for the output file
     after fsl_popen2()'ing the command (at which point we expect the
     output to be there). This "should" fail in the same way as
     fsl_sc_popen_e_direct, as we're doing the same thing just with
     another file of indirection.
  */
  fsl_sc_popen_e_fd = 4,

  /**
     Mode used by fsl_sc_popen_state_empty.
  */
  fsl_sc_popen_e_default = fsl_sc_popen_e_filebuf
};

typedef struct fsl_sc_popen_state fsl_sc_popen_state;

/**
   State for use with the fsl_sc_popen fsl_sc impl. fsl_sc_popen
   requires that its state member point to an instance of this class.

   This type acts as a base implemtation for channels which
   "shell out" to an external binary to provide the transport
   layer. e.g. curl can be used for HTTP(S) and fossil(1)'s own
   "test-http" mechanism can be used this way.
*/
struct fsl_sc_popen_state {
  /**
     State for "subclasses". Not really subclasses, but "concrete
     implementations" gets unwieldy after a while.
  */
  struct {
    /**
       This optional method gets called as part of the
       FSL_SC_INIT_INITIAL phase of fsl_sc::init() to give subclasses
       a way to perform any validation they may need. It may also
       perform any one-time initialization it may need to do.

       If it returns non-0, channel initialization fails.
    */
    int (*init)(fsl_sc *ch);

    /**
       Command arguments initializer. Must be set by concrete
       implementations and it gets invoked via
       fsl_sc::submit(). Implementations must populate b using
       ((fsl_sc_popen_state*)ch->impl.p)'s state, namely the
       request.filename and response.filename members, which are names
       of temporary files which will eventually contain the request
       payload and response payloads.

       Implementations must use fsl_buffer_escape_arg() and friends to
       append a complete command line to b. They must return 0 on
       success.

       When this is called (early on in the fsl_sc::submit() process):

       - this->bin.location will be set to the command to invoke. It
         must be prepended to the command line (or may it may be
         proxied via a separate command known only to the concrete
         implementation).

       - this->url will be set to the remote fossil URL.

       - this->request.filename is a file which the
         command is expected to read the fossil sync request body
         payload from.

       - If this->response.filename is not NULL then it is a file
         which the command is expected to write the fossil sync
         response to, minus any transport-layer metadata like HTTP
         headers. If it is NULL, the process is expected to write to
         stdout.

       - None of the other this->request and this->response members
         will be set up.

       - If bLoginCard is not NULL then it will hold the content for a
         fossil sync login card. Implementations must emit an HTTP
         header in the form "Cookie: x-f-x-l=XYZ" where XYZ is the
         URL-encoded content of bLoginCard.  For fsl_sc impls which
         use the FSL_SC_F_REQUEST_HEADERS flag, the library will have
         populated ch->requestHeaders before this is called, including
         the login card.
    */
    int (*buildCommandLine)(fsl_sc * ch, fsl_buffer * b,
                            fsl_buffer const * bLoginCard);

    /**
       May be set for subclasses in order to clean up any dynamic
       state owned by this object in this->impl.state. It will be
       called early in the fsl_sc::cleanup() step, before st's base
       class state are cleaned.

       Whether or not it must free st is implementation-defined.
    */
    void (*cleanup)( fsl_sc_popen_state * st );

    /**
       Subclass-specific state. Owned by this object's cleanup()
       method.
    */
    void * state;
  } impl;

  /** Info about the binary. */
  struct {
    /**
       The name of the program binary, e.g. "curl". It is expected to
       be findable via the $PATH.
    */
    char const * name;
    /**
       Location where the binary was found. If NULL, it defaults to a
       $PATH search of this->bin.name. Clients may set this before
       use, but (A) it must be allocated using a libfossil API,
       e.g. fsl_mprintf(), and (B) this memory is owned by this
       object.

       If the client does not set this, the library will search in the
       $PATH for this->name and assign that value here.
    */
    char * location;
  } bin;
  /** State for/around fsl__popen2(). */
  struct {
    /** The external process's stdin, i.e. a write-only stream. */
    FILE *fStdIn;
    /** The external process's stdout, i.e. a read-only stream. */
    int fdStdOut;
    /** The external process's PID. */
    int pid;
  } process;

  /**
     State for the sync request payload.
  */
  struct {
    /** Filename of this->fp. */
    char *filename;
    /** Request payload temp file. */
    FILE *fp;
    /** Number of initiated requests. */
    unsigned count;
  } request;

  /**
     State for the sync response payload.
  */
  struct {
    /**
       How to read the response. This should never be set by clients.
       It's for library-internal experimentation. If it must be set as
       part of a struct initialization, set it to
       fsl_sc_popen_e_default.
    */
    enum fsl_sc_popen_e mode;
    /**
       Response temp file when this->response.mode is
       fsl_sc_popen_e_filebuf or fsl_sc_popen_e_fd.
    */
    char *filename;
    /**
       Response source file when this->mode is fsl_sc_popen_e_filebuf.
       This file is opened in "w+b" mode.
    */
    FILE * fp;
    /**
       Response input source when this->mode is fsl_sc_popen_e_fd.
    */
    int fd;
    /** Number of initiated response-reads. */
    unsigned count;
    /**
       Response body when this->mode is fsl_sc_popen_e_membuf.
    */
    fsl_buffer b;
  } response;

  /**
     Flag(s).
  */
  struct {
    /**
       Internal implementation detail. Tells fsl_sc_popen's
       cleanup() impl whether or not to free() this object.
    */
    void * allocStamp;

    /**
       A random prefix for temp file names. Memory is owned by this
       object. All request and response temp files are prefixed with
       this and get a predictable suffix. We generate a random prefix,
       rather than a new random name on each call, to simplify
       grouping of the files for development and debugging purposes.
    */
    char * tmpfilePrefix;
  } misc;

  /**
     The command line to invoke. This is intended to be set up by
     this->impl.buildCommandLine.
  */
  fsl_buffer cmd;
};

/**
   Initializer for a fsl_sc_popen_state object which sets
   obj.bin.name=BinName, obj.impl.buildCommandLine=XCmdInit, and
   obj.impl.cleanup=XCleanup.

   The object is intended only for use as the fsl_sc::state pointer of
   a copy of fsl_sc_popen.
*/
#define fsl_sc_popen_state_init(BinName,xInit,XCmdInit,XCleanup) {  \
  .impl = {                                                  \
    .init=xInit,                                             \
    .buildCommandLine=XCmdInit,                              \
    .cleanup=XCleanup, .state = 0                            \
  },                                                         \
  .bin = { .name = BinName, .location = 0 },                 \
  .process = {                                               \
    .fStdIn = 0, .fdStdOut = -1, .pid = 0                    \
  },                                                         \
  .request = {                                               \
    .filename = 0, .fp = 0, .count=0                         \
  },                                                         \
  .response = {                                              \
    .mode = fsl_sc_popen_e_default,                          \
    .filename = 0, .fp = 0, .fd = -1,                        \
    .count=0, .b = fsl_buffer_empty_m                        \
  },                                                         \
  .misc = {                                                  \
    .allocStamp = 0,                                         \
    .tmpfilePrefix = 0                                       \
  },                                                         \
  .cmd = fsl_buffer_empty_m                                  \
}

//#define fsl_sc_popen_state_empty_m fsl_sc_popen_state_init("unnamed",0,0)

/**
   An empty base fsl_sc_popen_state instance intended to be
   copied to intialize fsl_sc_popen_state implementations.
   Implementations must set the bin.name and
   impl.buildCommandLine members to whatever is appropriate for the
   object's external binary.
*/
extern const fsl_sc_popen_state fsl_sc_popen_state_empty;

/**
   An empty base fsl_sc instance for use with fsl_sc_popen_state
   implementations.

   To use it:

   1. Bitwise-copy it over a fsl_sc instance.

   2. Set its state.p member to point to a valid fsl_sc_popen_state
      instance.

   3. Do not modify its state.type member.

   Nuances of this implementation:

   - Its state.p _must_ be a (fsl_sc_popen_state*) and state.type must
     be &fsl_sc_popen. This initializer does that latter part.

   - If state->flag.allocStamp is not 0, it must point to state->p.
     This indicates that this object owns its state pointer and will
     free it via this object's cleanup() method.
*/
extern const fsl_sc fsl_sc_popen;

/**
   An empty state object of the type expected by fsl_sc_popen,
   configured to use the system's "curl" binary as the fossil sync
   communication channel.
*/
extern const fsl_sc_popen_state fsl_sc_popen_state_curl;

/**
   Like fsl_sc_popen but may include different fsl_sc::flags defaults.
   The state.p member _must_ point to a fsl_sc_popen_state instance
   initialized by bitwise copying from fsl_sc_popen_state_curl.

   Example usage:

   ```
   fsl_sc_popen_state pst = fsl_sc_popen_state_curl;
   fsl_sc st = fsl_sc_popen_curl;
   st.state.p = &pst;
   ```
*/
extern const fsl_sc fsl_sc_popen_curl;

/**
   Works like fsl_sc_popen_curl but uses "fossil test-http" for the
   transport layer. The fsl_sc::state::p pointer of these objects MUST be a
   pointer to an fsl_sc_popen_state object which was initialized by
   copying it from fsl_sc_popen_state_fth. Example:

   ```
   fsl_sc sc = fsl_sc_popen_fth;
   fsl_sc_popen_state st = fsl_sc_popen_state_fth;
   sc.state.p = &sc;
   ```
*/
extern const fsl_sc fsl_sc_popen_fth;
/**
   State type for use with fsl_sc_popen_fth.
*/
extern const fsl_sc_popen_state fsl_sc_popen_state_fth;

/**
   Works like fsl_sc_popen_curl but uses "ssh ... fossil test-http" for the
   transport layer. The fsl_sc::state::p pointer of these objects MUST be a
   pointer to an fsl_sc_popen_state object which was initialized by
   copying it from fsl_sc_popen_state_ssh. Example:

   ```
   fsl_sc sc = fsl_sc_popen_ssh;
   fsl_sc_popen_state st = fsl_sc_popen_state_ssh;
   sc.state.p = &sc;
   ```

   By default it assumes that fossil(1) is in the remote's $PATH, but
   it can be explicitly told the path by providing a query argument to
   the URL: fossil=/path/to/remote/fossil (URL-encoded if it contains
   anything which needs to be).

   Peculiarities of this implementation:

   - Any password part of the URL is used as the remote fossil
     password, not the SSH password. The URL part is stripped before
     passing the URL on to ssh. However, fossil does not honor logins
     over ssh (the user is given full admin access), so applying the
     fossil login card here is a matter of conscience and
     library-level consistency, not one of necessity.
*/
extern const fsl_sc fsl_sc_popen_ssh;

/**
   State type for use with fsl_sc_popen_ssh.
*/
extern const fsl_sc_popen_state fsl_sc_popen_state_ssh;

/**
   Expands to a mapping of all fsl_sc_tracer_e types by invoking
   E(NAME,VALUE) for each one. NAME is the suffix part of the name,
   e.g. init instead of FSL_SC_TRACER_init. VALUE is its integer
   value.
*/
#define fsl_sc_tracer_map(E)              \
  E(none,0)                               \
  E(init,1) E(read, 1<<1) E(append, 1<<2) \
  E(cleanup, 1<<3) E(submit, 1<<4) E(self, 1<<5) \
  E(debug, 1<<6)

enum fsl_sc_tracer_e {
#define E(NAME,FLAG) FSL_SC_TRACER_ ## NAME = FLAG,
  fsl_sc_tracer_map(E)
#undef E

  FSL_SC_TRACER_all = 0
#define E(NAME,FLAG) | FSL_SC_TRACER_ ## NAME
  fsl_sc_tracer_map(E),
#undef E

  FSL_SC_TRACER_default = FSL_SC_TRACER_init
  | FSL_SC_TRACER_cleanup
  | FSL_SC_TRACER_submit
  | FSL_SC_TRACER_self
};

/**
   An fsl_sc implementation which proxies another, which must be
   set as its state member.

   This proxy calls fsl_sc_debug() with the name of each called fsl_sc
   method before passing the method on to the proxied changed.

   The exception is: the fsl_sc::debug.callback method is passed that
   through directly because calling fsl_sc_debug() on itself leads to
   an endless loop.

   Example:

   ```
   fsl_sc orig = ...some other sc...;
   fsl_sc tracer = fsl_sc_tracer_empty;
   fsl_sc_tracer_init(&tracer, &orig);
   ```

   Then use tracer in place of orig with the downstream APIs.  Calling
   tracer.cleanup(&tracer) will also cleanup orig.

   To customize where the debug messages go, replace the object's
   debug.callback method with one which expects an object of this type
   and emit them from there wherever you like. Just don't call any of
   the fsl_sc_debug() family of functions on that object, else an
   endless debig call loop will trigger.
*/
extern const fsl_sc fsl_sc_tracer_empty;

/**
   Initializes chTrace as a copy of fsl_sc_tracer_empty then directs
   chTrace's state to point to ch. Returns chTrace on success.  On
   error it returns NULL.
*/
FSL_EXPORT fsl_sc * fsl_sc_tracer_init(fsl_sc *chTrace, fsl_sc *ch,
                                       fsl_flag32_t traceFlags,
                                       fsl_outputer * out);

/** @internal

   Pass the name of a macro which takes 2 arguments to this
   macro. Macro E will be called with (NAME,LENGTH) of each sync card
   type. NAME is the unquoted name. LENGTH is strlen(#NAME) (which we
   need in a compile-time form).

   !!! Keep these sorted by name and edit code which uses these to
   !!! match. Internals require them to be sorted by name. Actually...
   !!! maybe not anymore.
*/
#define fsl_xfcard_map(E) \
  /* KEEP */   E(cfile,5) E(clone,5) E(clone_seqno,11) E(config,6)   \
  /* THESE */  E(cookie,6) E(error,5) E(file,4) E(gimme,5) E(igot,4) \
  /* SORTED */ E(login,5) E(uvfile,6) E(message,7) E(pragma,6)       \
  /* BY */     E(private,7) E(pull,4) E(push,4) E(reqconfig,9)       \
  /* NAME */   E(uvgimme,7) E(uvigot,6)

/** @internal

   Ids for fossil sync protocol card types.
*/
enum fsl_xfcard_e {
  /* MUST BE FIRST. Reminder to self: this has to be outside of
     fsl_xfcard_map() because of how fsl__xfcard_search() is set up. */
  fsl_xfcard_e_unknown,

#define E(T,LEN) fsl_xfcard_e_ ## T,
  fsl_xfcard_map(E)
#undef E

  /* Sentinel entry */
  fsl_xfcard_e_COUNT
};
typedef enum fsl_xfcard_e fsl_xfcard_e;
/**
   A comma-separated list of zeroes: one for each entry in
   fsl_xfcard_e.
*/
#define fsl_xfcard_zeroes \
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0

/**
   Metrics for use with the library's "xfer" (sync) subsystem.
*/
struct fsl_xfer_metrics {
  /** Number of connections made to the remote. */
  fsl_size_t trips;
  /** Number of "igot" cards sent */
  fsl_size_t iGotSent;
  /** Number of private "igot" cards */
  fsl_size_t privIGot;
  /** Number of gimme cards sent */
  fsl_size_t gimmeSent;
  /** Number of files sent */
  fsl_size_t fileSent;
  /** Number of deltas sent */
  fsl_size_t deltaSent;
  /** Number of files received */
  fsl_size_t fileRcvd;
  /** Number of deltas received */
  fsl_size_t deltaRcvd;
  /** Number of dangling deltas received */
  fsl_size_t danglingFile;
  /** Total received artifacts */
  fsl_size_t rcvdArtifacts;
  /** Total received cards */
  fsl_size_t rcvdCards;
  /** Total sent cards */
  fsl_size_t sentCards;
  /** Largest card payload size */
  fsl_size_t largestCardPayload;
  /**
     Buffer capacity allocated for the latest single
     fossil-compressed response.
  */
  fsl_size_t largestDecompressedResponse;
  /**
     Number of compressed response bytes via fsl__xfer_submit().
     This counts only compressed response bodies, not the compressed
     content of cfile cards.
  */
  fsl_size_t bytesReadCompressed;
  /** Number of uncompressed response bytes via fsl__xfer_read() and
      fsl_xfer_readln(). */
  fsl_size_t bytesReadUncompressed;
  /** Number of bytes via fsl__xfer_append() and friends. */
  fsl_size_t bytesWritten;
  /** Counts of each card type seen in the response(s). */
  fsl_size_t cardsRx[fsl_xfcard_e_COUNT];
  /**
     Counts of each card type sent in request(s).

     TODO: replace most of the card-specific members with this.  We
     first need to change how fsl__xfer_appendln() works.
  */
  fsl_size_t cardsTx[fsl_xfcard_e_COUNT];

  /** Various timing metrics. */
  struct {
    /** Time spent waiting on fsl_sc::read(), but only via
        fsl__xfer_read(), fsl__xfer_readln(), and (when reading
        compressed responses) fsl__xfer_submit(). */
    fsl_timer read;
    /** Time spent waiting on fsl_sc::submit(). */
    fsl_timer submit;
    /**
       Time taken to decompress cfile card payloads.
    */
    fsl_timer uncompress;
    /**
       Time taken to decompress cfile card payloads.
    */
    fsl_timer process;
  } timer;
};

typedef struct fsl_xfer_metrics fsl_xfer_metrics;

/**
   Empty-initialized fsl_xfer_metrics instance intended for
   const-copy initialization.
*/
#define fsl_xfer_metrics_empty_m { \
    .trips = 0, \
    .iGotSent = 0, \
    .privIGot = 0, \
    .gimmeSent = 0, \
    .fileSent = 0, \
    .deltaSent = 0, \
    .fileRcvd = 0, \
    .deltaRcvd = 0, \
    .danglingFile = 0, \
    .rcvdArtifacts = 0, \
    .rcvdCards = 0, \
    .sentCards = 0, \
    .largestCardPayload = 0, \
    .largestDecompressedResponse = 0, \
    .bytesReadCompressed = 0, \
    .bytesReadUncompressed = 0, \
    .bytesWritten = 0, \
    .cardsRx = {fsl_xfcard_zeroes},  \
    .cardsTx = {fsl_xfcard_zeroes},  \
    .timer = {                       \
      .read = fsl_timer_empty_m, \
      .submit = fsl_timer_empty_m, \
      .uncompress = fsl_timer_empty_m,    \
      .process = fsl_timer_empty_m \
    } \
  }

/**
   Empty-initialized fsl_xfer_metrics instance intended for
   non-const copy initialization.
*/
extern const fsl_xfer_metrics fsl_xfer_metrics_empty;

/**
   A callback for use with the libfossil sync transfer subsystem.
   Implementations are passed the sync-related metrics, their state
   object, and the result code of the sync (so that they can choose
   whether or not to skip any display on error).
*/
typedef void (*fsl_xfer_metrics_f)(fsl_xfer_metrics const *, void*,
                                   int theRc);

/**
   A fsl_xfer_metrics_f implementation which requires that its second
   argument be a (fsl_xfer_metrics*). It copies the first argument
   bitwise over the second. The final argument is ignored.
*/
void fsl_xfer_metrics_f_dup(fsl_xfer_metrics const *, void*, int theRc);

/**
   A fsl_xfer_metrics_f implementation which requires its second
   argument to be a (fsl_outputer*). It sends all output to that
   channel. If theRc is non-0 then this is a no-op.
*/
void fsl_xfer_metrics_f_outputer(fsl_xfer_metrics const * m,
                                 void *state, int theRc);

/**
   This typedef describes a callback used by the library for fetching
   passwords when needed for remote synchronization.

   Its first two arguments are current fsl_cx and the URL the password
   is being requested for. The final argument is
   implementation-dependent state.

   Implementations must somehow collect a password for the given URL,
   append a copy of it to bPout, and return 0 on success. On error
   they must return non-0, preferably the result of passing f to
   fsl_cx_err_set(). If appending the result to the buffer fails, it's
   caused (barring invalid arguments) by an OOM, in which case that
   code can be returned directly.

   It is legal for implementations to use bOut for construct their
   input prompt, so long as they pass it to fsl_buffer_reuse() before
   appending the password to it.

   To indicate a non-error when no password is available, return 0 and
   do not modify bOut (or pass it to fsl_buffer_reuse() before
   returning). In such cases the library will continue as if no
   credentials are available, which might trigger an error later on if
   credentials are required.

   bOut is owned by the library. The state argument is owned by
   whoever put it there.

   Design notes:

   Sync passwords are stored, if at all, in f's global config
   database.  Each has a key of "libfossil-sync-pw:" + the URL shorn
   of any password part. Thus saving a password for
   "http://foo:bar@baz/bar" will leave a config entry named
   "libfossil-sync-pw:http://foo@baz/bar". The passwords are stored as
   conventionally-fossil-hashed passwords.

   Passwords are specific to a given repository "project-code", so
   attempting to sync to the same URL for multiple project codes will
   leave the sync unable to log in to any but the most recent one for
   which the password was saved.

   This password storage is different from fossil(1)'s. The thinking
   is "separate apps, separate passwords", though the passwords all go
   in the same database. That's subject to change, though.
*/
typedef int (*fsl_pw_f)(fsl_cx * f, char const * zUrl,
                        fsl_buffer * bOut, void * state);

#if 0
/**
   A fsl_pw_f() implementation which uses unistd.h's
   obsolete/deprecated getpass(2). This implementation exists
   primarily for testing.

   Warning: getpass(2) uses static state, so this function must not be
   used if multiple threads might need to use getpass(2).

   This function works only for terminal applications, not GUI apps or
   those which cannot read from stdin.

   This implementation ignores the final argument.


   TODO: implement a getpass()-free alternative like the one
   demonstrated at:

   https://www.gnu.org/software/libc/manual/html_node/getpass.html

   Ah, fossil(1) has one in its user.c as well.
*/
int fsl_pw_f_getpass(fsl_cx * f, char const * zUrl,
                     fsl_buffer * bOut, void * state);
#endif

/**
   Various configuration pieces for libfossil sync transfer subsystem.
*/
struct fsl_xfer_config {
  /**
     The URL or filename to sync with. If this is NULL then the
     library will try to determine it from the currently-opened
     repository.
  */
  char const * url;

  /**
     NYI! Max amount of time, in seconds, to spend generating a single
     response.
  */
  int8_t maxResponseSeconds;

  /**
     NYI! Approximate max size, in bytes, for outbound response
     payloads.  It may overstep this amount by a single artifact
     (which may be arbitrarily large).
  */
  int32_t maxResponseBytes;

  /**
     Verbosity level (higher is more verbose).
  */
  short verbosity;

  /**
     A hint for fsl_sc channels which support it. See
     FSL_SC_F_LEAVE_TEMP_FILES.
  */
  bool leaveTempFiles;

  /**
     If true, use the application/x-fossil-uncompressed Content-Type
     for communication, else use application/x-fossil (which fossil
     does not compress). This is a _hint_ to the server, which may
     response how it likes.

     NYI is compression of outbound traffic.
  */
  bool compressTraffic;

  /**
     When a sync operation happens on a transient fs_cx instance,
     e.g. during a clone operation into a new fsl_cx, this listener
     gets installed in the transient fsl_cx. Sync operations which
     happen in the context of a specific fsl_cx currently use that
     instance's message listener. "The plan" is to eventually allow an
     override of the listener for sync-specific messages, but that is
     not yet in place.
  */
  fsl_msg_listener listener;

  /** Optional debug message callback. */
  fsl_xfer_dbg debug;

  /**
     fsl_sc API tracing hook.
  */
  struct {
    /**
       If non-zero, a tracing channel is installed with the given mask
       of fsl_sc_tracer_e flags. If it is zero, no tracer is
       installed.
    */
    fsl_flag32_t mask;
    /**
       The output sink.
    */
    fsl_outputer outputer;
  } trace;

  /**
     Members which are set to true will activate
     specific aspects of sync.
  */
  struct {
    /* Clone with protocol version 1, 2, or 3. */
    short clone;
    /* Push content. */
    bool push;
    /* Pull content. */
    bool pull;
    /* Transfer private content. */
    bool privateContent;
    /* Ignore push/pull/clone and use the autosync setting. */
    bool autosync;
    /* Request all igot cards from the server. */
    bool verily;
  } op;

  struct {
    /**
       If not NULL, fsl__xfer's cleanup routine will call this,
       passing it the metrics, this->state, and the final result code
       of the sync process.
    */
    fsl_xfer_metrics_f callback;
    /**
       Optional state for the 2nd argument to this->callback(). It is
       owned by whoever assigns it to non-NULL.
    */
    void * state;
  } metrics;

  struct {
    /**
       Callback to use to fetch passwords.
    */
    fsl_pw_f callback;
    /**
       State for the password callback. It must match that
       implementation's expectations.
    */
    void * state;
    /**
       If true:

       - If the URL contains a password, then on successful sync
         the hashed password is saved for future use with that same
         URL (excluding the password part).

       - If the URL has no password but a saved one is found, it is
         used.
    */
    bool save;
  } password;
};

typedef struct fsl_xfer_config fsl_xfer_config;

/**
   Empty-initialized fsl_xfer_config instance intended for const-copy
   initialization.
*/
#define fsl_xfer_config_empty_m {    \
  .url = 0,                          \
  .maxResponseSeconds = -1,          \
  .maxResponseBytes = 2500000,       \
  .verbosity = 0,                    \
  .compressTraffic=false,            \
  .listener = fsl_msg_listener_empty_m, \
  .debug = fsl_xfer_dbg_empty_m,     \
  .trace = {                         \
    .mask = 0,                       \
    .outputer = fsl_outputer_FILE_m  \
  },                                 \
  .op = {                            \
    .clone = 0,                      \
    .push = false,                   \
    .pull = false,                   \
    .privateContent  = false,        \
    .autosync = false                \
  },                                 \
  .metrics = {                       \
    .callback = NULL, .state = NULL  \
  },                                 \
  .password = {                      \
    .callback = NULL,                \
    .state = NULL,                   \
    .save = false                    \
  }                                  \
}

/**
   Empty-initialized fsl_xfer_config instance intended for
   non-const-copy initialization.
*/
extern const fsl_xfer_config fsl_xfer_config_empty;

/**
   Frees any memory which may be owned by p but does not
   free p.
*/
FSL_EXPORT void fsl_xfer_config_cleanup( fsl_xfer_config * p );

/**
   Options for use with fsl_clone().
*/
struct fsl_clone_config {
  /** Options for fsl_repo_create(). */
  fsl_repo_create_opt repo;
  /**
     Options related to the transfer subsystem.
  */
  /** Core sync subsystem options. */
  fsl_xfer_config xfer;
};

typedef struct fsl_clone_config fsl_clone_config;

/**
   Empty-initialized fsl_clone_config instance intended for const-copy
   initialization.
*/
#define fsl_clone_config_empty_m {      \
  .repo = fsl_repo_create_opt_empty_m,  \
  .xfer = fsl_xfer_config_empty_m       \
}

/**
   Empty-initialized fsl_clone_config instance intended for
   non-const-copy initialization.
*/
extern const fsl_clone_config fsl_clone_config_empty;

/**
   Under construction.
*/
int fsl_clone( fsl_clone_config const * cc, fsl_error * err );

#if 0
/**
   Untested, incomplete, and it open design question remain regarding
   handling of the channel's state object's memory.

   Attempts to find a fsl_sc class capable of handling the given
   fossil URL.

   Currently handled:

   - http and https: uses fsl_sc_popen

   On success, returns a new instance, which the caller must
   eventually pass to fsl_sc_cleanup() and then to fsl_free().
*/
fsl_sc * fsl_sc_for_url( char const * zUrl );
#endif

#if defined(__cplusplus)
} /*extern "C"*/
#endif
#endif
/* ORG_FOSSIL_SCM_FSL_SYNC_H_INCLUDED */
