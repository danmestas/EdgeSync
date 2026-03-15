/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
#if !defined(ORG_FOSSIL_SCM_FSL_UTIL_H_INCLUDED)
#define ORG_FOSSIL_SCM_FSL_UTIL_H_INCLUDED
/*
  Copyright 2013-2021 The Libfossil Authors, see LICENSES/BSD-2-Clause.txt

  SPDX-License-Identifier: BSD-2-Clause-FreeBSD
  SPDX-FileCopyrightText: 2021 The Libfossil Authors
  SPDX-FileType: Code

  Heavily indebted to the Fossil SCM project (https://fossil-scm.org).

*/

/** @file util.h

    This file declares a number of utility classes and routines used by
    libfossil. All of them considered "public", suitable for direct use
    by client code.
*/

#include "config.h" /* MUST come first b/c of config macros */
#include <stdio.h> /* FILE type */
#include <stdarg.h> /* va_list */
#include <time.h> /* tm struct */
#include <stdbool.h>
#if defined(__cplusplus)
extern "C" {
#endif
typedef struct fsl_allocator fsl_allocator;
typedef struct fsl_buffer fsl_buffer;
typedef struct fsl_error fsl_error;
typedef struct fsl_finalizer fsl_finalizer;
typedef struct fsl_fstat fsl_fstat;
typedef struct fsl_list fsl_list;
typedef struct fsl_outputer fsl_outputer;
typedef struct fsl_state fsl_state;
typedef struct fsl_id_bag fsl_id_bag;

/**
   fsl_uuid_str and fsl_uuid_cstr are "for documentation and
   readability purposes" typedefs used to denote strings which the API
   requires to be in the form of Fossil UUID strings. Such strings are
   exactly FSL_STRLEN_SHA1 or FSL_STRLEN_K256 bytes long plus a
   terminating NUL byte and contain only lower-case hexadecimal
   bytes. Where this typedef is used, the library requires, enforces,
   and/or assumes (at different times) that fsl_is_uuid() returns true
   for such strings (if they are not NULL, though not all contexts
   allow a NULL UUID). These typedef are _not_ used to denote
   arguments which may refer to partial UUIDs or symbolic names, only
   100% bonafide Fossil UUIDs (which are different from RFC4122
   UUIDs).

   The API guarantees that this typedef will always be (char *) and
   that fsl_uuid_cstr will always ben (char const *), and thus it
   is safe/portable to use those type instead of these. These
   typedefs serve only to improve the readability of certain APIs
   by implying (through the use of this typedef) the preconditions
   defined for UUID strings.

   Sidebar: fossil historically used the term UUID for blob IDs, and
   still uses that term in the DB schema, but it has fallen out of
   favor in documentation and discussions, with "hash" being the
   preferred term. Much of the libfossil code was developed before
   that happened, though, so "UUID" is still prevalent in its API and
   documentation.

   @see fsl_is_uuid()
   @see fsl_uuid_cstr
*/
typedef char * fsl_uuid_str;

/**
   The const counterpart of fsl_uuid_str.

   @see fsl_is_uuid()
   @see fsl_uuid_str
*/
typedef char const * fsl_uuid_cstr;

/**
   A typedef for comparison function used by standard C routines such
   as qsort(). It is provided here primarily to simplify documentation
   of other APIs. Concrete implementations must compare lhs and rhs,
   returning negative, 0, or positive depending on whether lhs is less
   than, equal to, or greater than rhs.

   Implementations might need to be able to deal with NULL
   arguments. That depends on the routine which uses the comparison
   function.
*/
typedef int (*fsl_generic_cmp_f)( void const * lhs, void const * rhs );

/**
   If the NUL-terminated input str is exactly FSL_STRLEN_SHA1 or
   FSL_STRLEN_K256 bytes long and contains only lower-case
   hexadecimal characters, returns the length of the string, else
   returns 0.

   Note that Fossil UUIDs are not RFC4122 UUIDs, but are SHA1 or
   SHA3-256 hash strings. Don't let that disturb you. As Tim
   Berners-Lee writes:

   'The assertion that the space of URIs is a universal space
   sometimes encounters opposition from those who feel there should
   not be one universal space. These people need not oppose the
   concept because it is not of a single universal space: Indeed,
   the fact that URIs form universal space does not prevent anyone
   else from forming their own universal space, which of course by
   definition would be able to envelop within it as a subset the
   universal URI space. Therefore the web meets the "independent
   design" test, that if a similar system had been concurrently and
   independently invented elsewhere, in such a way that the
   arbitrary design decisions were made differently, when they met
   later, the two systems could be made to interoperate.'

   Source: https://www.w3.org/DesignIssues/Axioms.html

   (Just mentally translate URI as UUID.)
*/
FSL_EXPORT int fsl_is_uuid(char const * str);

/**
   If x is a valid fossil UUID length, it is returned, else 0 is returned.
*/
FSL_EXPORT int fsl_is_uuid_len(int x);

/**
   Expects str to be a string containing an unsigned decimal
   value. Returns its decoded value, or -1 on error.
*/
FSL_EXPORT fsl_size_t fsl_str_to_size(char const * str);

/**
   Expects str to be a string containing a decimal value,
   optionally with a leading sign. Returns its decoded value, or
   dflt if !str or on error.
*/
FSL_EXPORT fsl_int_t fsl_str_to_int(char const * str, fsl_int_t dflt);


/**
   Generic list container type. This is used heavily by the Fossil
   API for storing arrays of dynamically-allocated objects. It is
   not useful as a non-pointer-array replacement.

   It is up to the APIs using this type to manage the entry count
   member and use fsl_list_reserve() to manage the "capacity"
   member.

   @see fsl_list_reserve()
   @see fsl_list_append()
   @see fsl_list_visit()
*/
struct fsl_list {
  /**
     Array of entries. It contains this->capacity entries,
     this->count of which are "valid" (in use).
  */
  void ** list;
  /**
     Number of "used" entries in the list.
  */
  fsl_size_t used;
  /**
     Number of slots allocated in this->list. Use fsl_list_reserve()
     to modify this. Doing so might move the this->list pointer but
     the values it points to will stay stable.
  */
  fsl_size_t capacity;
};

/**
   Empty-initialized fsl_list structure, intended for const-copy
   initialization.
*/
#define fsl_list_empty_m { .list=NULL, .used=0, .capacity=0 }

/**
   Empty-initialized fsl_list structure, intended for copy
   initialization.
*/
FSL_EXPORT const fsl_list fsl_list_empty;

/**
   Generic interface for finalizing/freeing memory. Intended
   primarily for use as a destructor/finalizer for high-level
   structs. Implementations must semantically behave like free(mem),
   regardless of whether or not they actually free the memory. At
   the very least, they generally should clean up any memory owned by
   mem (e.g. db resources or buffers), even if they do not free() mem.
   some implementations assume that mem is stack-allocated
   and they only clean up resources owned by mem.

   The state parameter is any state needed by the finalizer
   (e.g. a memory allocation context) and mem is the memory which is
   being finalized.

   The exact interpretaion of the state and mem are of course
   implementation-specific.
*/
typedef void (*fsl_finalizer_f)( void * state, void * mem );

/**
   Generic interface for memory finalizers.
*/
struct fsl_finalizer {
  /**
     State to be passed as the first argument to f().
  */
  void * state;
  /**
     Finalizer function. Should be called like this->f( this->state, ... ).
  */
  fsl_finalizer_f f;
};

/** Empty-initialized fsl_finalizer struct. */
#define fsl_finalizer_empty_m {NULL,NULL}

/**
   fsl_finalizer_f() impl which requires that mem be-a
   (fsl_buffer*).  This function frees all memory associated with
   that buffer and zeroes out the structure, but does not free mem
   (because it is rare that fsl_buffers are created on the
   heap). The state parameter is ignored.
*/
FSL_EXPORT int fsl_finalizer_f_buffer( void * state, void * mem );


/**
   Generic state-with-finalizer holder. Used for binding
   client-specified state to another object, such that a
   client-specified finalizer is called with the other object is
   cleaned up.
*/
struct fsl_state {
  /**
     Arbitrary context-dependent state.
  */
  void * state;
  /**
     Finalizer for this->state. If used, it should be called like:

     ```
     this->finalize.f( this->finalize.state, this->state );
     ```

     After which this->state must be treated as if it has been
     free(3)'d. See fsl_state_finalize().
  */
  fsl_finalizer finalize;
};

/** Empty-initialized fsl_state struct. */
#define fsl_state_empty_m {NULL,fsl_finalizer_empty_m}

/**
   Empty-initialized fsl_state struct, intended for
   copy-initializing.
*/
FSL_EXPORT const fsl_state fsl_state_empty;

/**
   If fst is not NULL then:

   - If fst->finalize.f is not null, then it is called and
     passed (fst->finalize.f, fst->state).

   - *fst is cleared out. fst is not freed.

   This is a no-op if fst is NULL.
*/
void fsl_state_finalize(fsl_state * fst);

/**
   Generic interface for streaming out data. Implementations must
   write n bytes from src to their destination channel and return 0
   on success, non-0 on error (assumed to be a value from the fsl_rc_e
   enum). The state parameter is the implementation-specified output
   channel.

   Potential TODO: change the final argument to a pointer, with
   semantics similar to fsl_input_f(): at call-time n is the number
   of bytes to output, and on returning n is the number of bytes
   actually written. This would allow, e.g. the fsl_zip_writer APIs
   to be able to stream a ZIP file (they have to know the real size
   of the output, and this interface doesn't support that
   operation).
*/
typedef int (*fsl_output_f)( void * state, void const * src, fsl_size_t n );


/**
   Generic interface for flushing arbitrary output streams.  Must
   return 0 on success, non-0 on error, but the result code
   "should" (to avoid downstream confusion) be one of the fsl_rc_e
   values. When in doubt, return FSL_RC_IO on error. The
   interpretation of the state parameter is
   implementation-specific.
*/
typedef int (*fsl_flush_f)(void * state);

/**
   Generic interface for streaming in data. Implementations must
   read (at most) *n bytes from their input, copy it to dest, assign
   *n to the number of bytes actually read, return 0 on success, and
   return non-0 on error (assumed to be a value from the fsl_rc_e
   enum). When called, *n is the max length to read. On return, *n
   is the actual amount read. The state parameter is the
   implementation-specified input file/buffer/whatever channel.

   Implementations may need to distinguish a short read due to EOF
   from a short read due to an I/O error, e.g. using feof() and/or
   ferror().
*/
typedef int (*fsl_input_f)( void * state, void * dest, fsl_size_t * n );

/**
   fsl_output_f() implementation which requires state to be a
   (fsl_cx*) to which this routine simply redirects the output via
   fsl_output().  Is a no-op (returning 0) if !n. Returns
   FSL_RC_MISUSE if !state or !src.
*/
FSL_EXPORT int fsl_output_f_fsl_cx(void * state, void const * src, fsl_size_t n );

/**
   An interface which encapsulates data for managing an output
   destination, primarily intended for use with fsl_output(). Why
   abstract it to this level? So that we can do interesting things
   like output to buffers, files, sockets, etc., using the core
   output mechanism. e.g. so script bindings can send their output
   to the same channel used by the library and other library
   clients.
*/
struct fsl_outputer {
  /**
     Output channel.
  */
  fsl_output_f out;
  /**
     flush() implementation. This may be NULL for most uses of this
     class. Cases which specifically require it must document that
     requirement so.
  */
  fsl_flush_f flush;
  /**
     State to be used when calling this->out(), namely:
     this->out( this->state, ... ) and this->flush(this->state).
  */
  void * state;
};
/** Empty-initialized fsl_outputer instance. */
#define fsl_outputer_empty_m {NULL,NULL,NULL}
/**
   Empty-initialized fsl_outputer instance, intended for
   copy-initializing.
*/
FSL_EXPORT const fsl_outputer fsl_outputer_empty;

/**
   A fsl_outputer instance which is initialized to output to a
   (FILE*). To use it, this value then set the copy's state
   member to an opened-for-write (FILE*) handle. By default it will
   use stdout.
*/
FSL_EXPORT const fsl_outputer fsl_outputer_FILE;

/**
   fsl_outputer initializer which uses fsl_flush_f_FILE() and
   fsl_output_f_FILE(). After copying, the state member must be
   pointed to an opened-for-writing (FILE*).
*/
#define fsl_outputer_FILE_m { \
  .out = fsl_output_f_FILE,   \
  .flush = fsl_flush_f_FILE,  \
  .state = NULL                 \
}

/**
   Generic stateful alloc/free/realloc() interface.

   Implementations must behave as follows:

   - If 0==n then semantically behave like free(3) and return
   NULL.

   - If 0!=n and !mem then semantically behave like malloc(3), returning
   newly-allocated memory on success and NULL on error.

   - If 0!=n and NULL!=mem then semantically behave like
   realloc(3). Note that realloc specifies: "If n was equal to 0,
   either NULL or a pointer suitable to be passed to free() is
   returned." Which is kind of useless, and thus implementations
   MUST return NULL when n==0.
*/
typedef void *(*fsl_realloc_f)(void * state, void * mem, fsl_size_t n);

/**
   Holds an allocator function and its related state.
*/
struct fsl_allocator {
  /**
     Base allocator function. It must be passed this->state as its
     first argument.
  */
  fsl_realloc_f f;
  /**
     State intended to be passed as the first argument to this->f().
  */
  void * state;
};

/** Empty-initialized fsl_allocator instance. */
#define fsl_allocator_empty_m {NULL,NULL}


/**
   A fsl_realloc_f() implementation which uses the standard
   malloc()/free()/realloc(). The state parameter is ignored.
*/
FSL_EXPORT void * fsl_realloc_f_stdalloc(void * state, void * mem, fsl_size_t n);


/**
   Semantically behaves like malloc(3), but may introduce instrumentation,
   error checking, or similar.
*/
FSL_EXPORT void * fsl_malloc( fsl_size_t n )
#ifdef __GNUC__
  __attribute__ ((malloc))
#endif
  ;

/**
   Semantically behaves like free(3), but may introduce instrumentation,
   error checking, or similar.
*/
FSL_EXPORT void fsl_free( void * mem );

/**
   Behaves like realloc(3). Clarifications on the behaviour (because
   the standard has one case of unfortunate wording involving what
   it returns when n==0):

   - If passed (NULL, n>0) then it semantically behaves like
   fsl_malloc(f, n).

   - If 0==n then it semantically behaves like free(2) and returns
   NULL (clarifying the aforementioned wording problem).

   - If passed (non-NULL, n) then it semantically behaves like
   realloc(mem,n).

*/
FSL_EXPORT void * fsl_realloc( void * mem, fsl_size_t n );

/**
   A fsl_flush_f() impl which expects _FILE to be-a (FILE*) opened
   for writing, which this function passes the call on to
   fflush(). If fflush() returns 0, so does this function, else it
   returns non-0.
*/
FSL_EXPORT int fsl_flush_f_FILE(void * _FILE);

/**
   A fsl_finalizer_f() impl which requires that mem be-a (FILE*).
   This function passes that FILE to fsl_fclose(). The state
   parameter is ignored.
*/
FSL_EXPORT void fsl_finalizer_f_FILE( void * state, void * mem );

/**
   A fsl_output_f() impl which requires state to be-a (FILE*), which
   this function passes the call on to fwrite(). Returns 0 on
   success, FSL_RC_IO on error.
*/
FSL_EXPORT int fsl_output_f_FILE( void * state, void const * src, fsl_size_t n );

/**
   A fsl_output_f() impl which requires state to be-a (fsl_buffer*),
   which this function passes to fsl_buffer_append(). Returns 0 on
   success, FSL_RC_OOM (probably) on error.
*/
FSL_EXPORT int fsl_output_f_buffer( void * state, void const * src, fsl_size_t n );


/**
   A fsl_output_f() impl which requires state to be-a (int*) referring
   to a writable file descriptor, which this function passes to
   write(2). Returns 0 on success.
*/
FSL_EXPORT int fsl_output_f_fd( void * state, void const * src, fsl_size_t n );

/**
   A fsl_input_f() implementation which requires that state be
   a readable (FILE*) handle.
*/
FSL_EXPORT int fsl_input_f_FILE( void * state, void * dest, fsl_size_t * n );

/**
   A fsl_input_f() implementation which requires that state be a
   readable file descriptor, in the form of an ([const] int*).  This
   function will not modify that value.
*/
FSL_EXPORT int fsl_input_f_fd( void * state, void * dest, fsl_size_t * n );

/**
   A fsl_input_f() implementation which requires that state be a
   readable (fsl_buffer*) handle. The buffer's cursor member is
   updated to track input postion, but that is the only modification
   made by this routine. Thus the user may need to reset the cursor to
   0 if he wishes to start consuming the buffer at its starting point
   (e.g. call fsl_buffer_rewind()). Subsequent calls to this function
   will increment the cursor by the number of bytes returned via *n.
   The buffer's "used" member is used to determine the logical end of
   input.

   If fsl_buffer_err() is non-zero for the state argument, that code
   is returned without other side effects.

   Returns 0 on success and has no error conditions except for invalid
   arguments, which result in undefined beavhiour, or fsl_buffer_err()
   being true for the state argument. Results are undefined if any
   argument is NULL.

   Tip (and warning): sometimes a routine might have a const buffer
   handle which it would like to use in conjunction with this
   routine but cannot without violating constness. Here's a crude
   workaround:

   ```
   fsl_buffer kludge = fsl_buffer_empty;
   fsl_buffer_external(&kludge, theConstBuffer->mem, theConstBuffer->used);
   rc = some_func( fsl_input_f_buffer, &kludge, ... );
   assert( kludge.mem == theConstBuffer->mem );// not reallocated
   assert( 0==kludge.capacity );// b/c it's an external buffer
   ```

   That is legal because this routine modifies only
   fsl_buffer::cursor. It is not useful as a constness workaround if
   there is ANY CHANCE WHATSOEVER that the buffer's memory will be
   modified. If such tricks are used for APIs which may modify their
   buffer, the "kludge" buffer (above), because it's an "external"
   buffer, will create a new copy of theConstBuffer's memory before
   making any changes, which will lead a leak if that object is not
   cleaned up.

   [Much later:] a cleaner approach is to use fsl_buffer_external().
*/
FSL_EXPORT int fsl_input_f_buffer( void * state, void * dest, fsl_size_t * n );


/**
   A generic streaming routine which copies data from an
   fsl_input_f() to an fsl_outpuf_f().

   Reads all data from inF() in chunks of an unspecified size and
   passes them on to outF(). It reads until inF() returns fewer bytes
   than requested. Returns the result of the last call to outF() or
   (only if reading fails) inF(). Results are undefined if either of
   the callback function arguments are NULL.  (This function cannot
   know whether a NULL state argument is legal.)

   Here is an example which basically does the same thing as the
   cat(1) command on Unix systems:

   ```
   fsl_stream( fsl_input_f_FILE, stdin, fsl_output_f_FILE, stdout );
   ```

   Or copy a FILE to a buffer:

   ```
   FILE * f = fopen(...);
   fsl_buffer myBuf = fsl_buffer_empty;
   rc = fsl_stream( fsl_input_f_FILE, f, fsl_output_f_buffer, &myBuf );
   // Note that on error myBuf might be partially populated.
   // Eventually clean up the buffer:
   fsl_buffer_clear(&myBuf);
   ```
*/
FSL_EXPORT int fsl_stream( fsl_input_f inF, void * inState,
                           fsl_output_f outF, void * outState );

/**
   Consumes two input streams looking for differences.  It stops
   reading as soon as either or both streams run out of input or a
   byte-level difference is found.  It consumes input in chunks of
   an unspecified size, and after this returns the input cursor of
   the streams is not well-defined.  i.e. the cursor probably does
   not point to the exact position of the difference because this
   level of abstraction does not allow that unless we read byte by
   byte.

   Returns 0 if both streams emit the same amount of output and
   that ouput is bitwise identical, otherwise it returns non-0.
*/
FSL_EXPORT int fsl_stream_compare( fsl_input_f in1, void * in1State,
                                   fsl_input_f in2, void * in2State );


/**
   A general-purpose buffer class, analog to Fossil's Blob
   class, but it is not called fsl_blob to avoid confusion with
   DB-side blobs. Buffers are used extensively in fossil to do
   everything from reading files to compressing artifacts to
   creating dynamically-formatted strings. Because they are such a
   pervasive low-level type, and have such a simple structure,
   their members (unlike most other structs in this API) may be
   considered public and used directly by client code (as long as
   they do not mangle their state, e.g. by setting this->capacity
   smaller than this->used!).

   General conventions of this class:

   - ALWAYS initialize them by copying fsl_buffer_empty or
   (depending on the context) fsl_buffer_empty_m. Failing to
   initialize them properly leads to undefined behaviour.

   - ALWAYS fsl_buffer_clear() buffers when done with
   them. Remember that failed routines which output to buffers
   might partially populate the buffer, so be sure to clean up in
   error cases.

   - The `capacity` member specifies how much memory the buffer
   currently holds in its `mem` member. If `capacity` is 0 and `mem`
   is not then the memory is expected to refer to `used` bytes of
   memory which are owned elsewhere. See fsl_buffer_external() and
   fsl_buffer_materialize() for details.

   - The `used` member specifies how much of the memory is actually
   "in use" by the buffer. The exact meaning of "used" is
   context-dependent but it's typically equivalent to the strlen() of
   the buffer's contents.

   - As a rule, the public APIs keep (`used`<`capacity`) and always
   (unless documented otherwise) tries to keep the memory buffer
   NUL-terminated (if it has any memory at all). The notable
   potential exception to that is that "external" buffers
   may not be NUL-terminated (see fsl_buffer_external()).

   - Use fsl_buffer_reuse() to keep memory around and reset the `used`
   amount to 0. Most library-wide routines which write to buffers will
   re-use that memory if they can, rather than re-allocating.

   This example demonstrates the difference between `used` and
   `capacity` (error checking reduced to assert()ions for clarity):

   ```
   fsl_buffer b = fsl_buffer_empty;
   // ALWAYS init via copying fsl_buffer_empty or (depending on
   // the context) fsl_buffer_empty_m. The latter is used for
   // in-struct initialization of struct members.
   int rc = fsl_buffer_reserve(&b, 20);
   assert(0==rc);
   assert(b.capacity>=20); // it may reserve more!
   assert(0==b.used);
   rc = fsl_buffer_append(&b, "abc", 3);
   assert(0==rc);
   assert(3==b.used);
   assert(0==b.mem[b.used]); // API always NUL-terminates
   fsl_buffer_reuse(&b);
   assert(0==b.used); // Resets the buffer but...
   assert(b.capacity>=20);  // ... keeps the memory.
   fsl_buffer_clear(&b); // ALWAYS clean up
   ```

   @see fsl_buffer_reserve()
   @see fsl_buffer_resize()
   @see fsl_buffer_external()
   @see fsl_buffer_materialize()
   @see fsl_buffer_append()
   @see fsl_buffer_appendf()
   @see fsl_buffer_cstr()
   @see fsl_buffer_size()
   @see fsl_buffer_capacity()
   @see fsl_buffer_clear()
   @see fsl_buffer_reuse()
   @see fsl_buffer_err()
   @see fsl_buffer_err_clear()
*/
struct fsl_buffer {
  /**
     The raw memory pointed to by this buffer. There are two ways of
     using this member:

     - If `this->capacity` is non-0 then the first `this->capacity`
     bytes of `this->mem` are owned by this buffer instance. The API
     docs call this state "managed" buffers.

     - `If this->capacity` is 0 and this->mem is not NULL then the
     memory is owned by "somewhere else" and this API will treat it as
     _immutable_ (so it may safely point to const data). Its lifetime
     must exceed this object's and any attempt made via this API to
     write to it will cause the memory to be copied (effectively a
     copy-on-write op). The API calls this state "external" buffers
     and refers to the copy-on-write of such buffers as
     "materializing" them. See fsl_buffer_external() and
     fsl_buffer_materialize() for details.

     `this->used` bytes are treated as the "used" part of the buffer
     (as opposed to its capacity). When `this->capacity>0` the
     difference beween (`this->capacity - this->used`) represents
     space the buffer has available for use before it will require
     another expansion/reallocation.
  */
  unsigned char * mem;
  /**
     Number of bytes allocated for this buffer. If capacity is 0
     and `this->mem` is not NULL then this buffer's memory is assumed
     to be owned "elsewhere" and will be considered immutable by the
     API. Any attempt to modify it will result in a copy-on-write
     operation
  */
  fsl_size_t capacity;

  /**
     Number of "used" bytes in the buffer. This is generally
     interpreted as the string length of this->mem, and the buffer
     APIs which add data to a buffer always ensure that
     this->capacity is large enough to account for a trailing NUL
     byte in this->mem.

     Library routines which manipulate buffers must ensure that
     (this->used<this->capacity) is always true, expanding the buffer
     if necessary. Much of the API assumes that precondition is always
     met, and any violation of it opens the code to undefined
     behaviour (which is okay, just don't ever break that
     precondition). Most APIs ensure that (used<capacity) is always
     true (as opposed to used<=capacity) because they add a trailing
     NUL byte which is not counted in the "used" length.
  */
  fsl_size_t used;
  /**
     Used by some routines to keep a cursor into this->mem. Note that
     a pointer-based cursor is not valid for some uses because mem can
     be reallocated (and thus moved).
  */
  fsl_size_t cursor;

  /**
     When any buffer-related routines encounter an error, they set
     this flag so that the error can be propagated. Many APIs also
     become no-ops when this is set, the intention being to simplify
     many common uses of this class, e.g.:

     ```
     fsl_buffer_append(b, ... );
     fsl_buffer_append(b, ... );
     fsl_buffer_append(b, ... );
     if(fsl_buffer_err(b)) ...
     ```

     The alternative (without this member) being to check for an OOM
     error after every append operation.
  */
  int errCode;
};

/** Empty-initialized fsl_buffer instance, intended for const-copy
    initialization. */
#define fsl_buffer_empty_m \
  {.mem=NULL,.capacity=0U,.used=0U,.cursor=0U,.errCode=0}

/** Empty-initialized fsl_buffer instance, intended for copy
    initialization. */
FSL_EXPORT const fsl_buffer fsl_buffer_empty;

/**
   A container for storing generic error state. It is used to
   propagate error state between layers of the API back to the
   client. i.e. they act as basic exception containers.

   @see fsl_error_set()
   @see fsl_error_get()
   @see fsl_error_propagate()
   @see fsl_error_clear()
*/
struct fsl_error {
  /**
     Error message text is stored in this->msg.mem. The usable text
     part is this->msg.used bytes long.
  */
  fsl_buffer msg;
  /**
     Error code, generally assumed to be a fsl_rc_e value. The
     "non-error" code is 0.
  */
  int code;
};

/** Empty-initialized fsl_error instance, intended for const-copy
    initialization. */
#define fsl_error_empty_m {.msg = fsl_buffer_empty_m, .code = 0}

/** Empty-initialized fsl_error instance, intended for copy
    initialization. */
FSL_EXPORT const fsl_error fsl_error_empty;

/**
   If err is not NULL, populates err with the given code and formatted
   string, replacing any existing state. If fmt==NULL then
   fsl_rc_cstr(rc) is used to get the error string.

   If err is NULL this is a no-op and returns code.

   Returns code on success, some other non-0 code on error.

   As a special case, if 0==code then fmt is ignored and the error
   state is cleared. This will not free any memory held by err but
   will re-set its string to start with a NUL byte, ready for
   re-use later on.

   As a special case, if code==FSL_RC_OOM then fmt is ignored
   to avoid a memory allocation (which would presumably fail).

   @see fsl_error_get()
   @see fsl_error_clear()
   @see fsl_error_propagate()
*/
FSL_EXPORT int fsl_error_set( fsl_error *  err, int code,
                              char const * fmt, ... );

/**
   va_list counterpart to fsl_error_set().
*/
FSL_EXPORT int fsl_error_setv( fsl_error *  err, int code,
                               char const * fmt, va_list args );

/**
   Fetches the error state from err. Returns err's current error code.

   If str is not NULL then *str will be assigned to the raw
   (NUL-terminated) error string (which might be empty or even
   NULL). The memory for the string is owned by err and may be
   invalidated by any calls which take err as a non-const parameter OR
   which might modify it indirectly through a container object, so the
   client is required to copy it if it is needed for later
   reference. As a special case, if the error object has no message
   then the returned string is set to NULL, as opposed to an empty
   string.

   If len is not NULL then *len will be assigned to the length of
   the (*str) string (in bytes).

   @see fsl_error_set()
   @see fsl_error_clear()
   @see fsl_error_propagate()
*/
FSL_EXPORT int fsl_error_get( fsl_error const * err,
                              char const ** str, fsl_size_t *  len );

/**
   If err is not NULL, frees up any resources owned by err and sets
   its error code to 0, but does not free err. This is harmless no-op
   if err is NULL or holds no dynamically allocated no memory.

   @see fsl_error_set()
   @see fsl_error_get()
   @see fsl_error_propagate()
   @see fsl_error_reset()
*/
FSL_EXPORT void fsl_error_clear( fsl_error *  err );

/**
   Sets err->code to 0 and resets its buffer, but keeps any err->msg
   memory around for later re-use. This is a no-op if err is NULL.

   @see fsl_error_clear()
*/
FSL_EXPORT void fsl_error_reset( fsl_error *  err );

/**
   Copies the error state from src to dest. If dest contains state, it is
   cleared/recycled by this operation.

   Returns 0 on success, FSL_RC_MISUSE if either argument is NULL
   or if (src==dest), and FSL_RC_OOM if allocation of the message
   string fails.

   As a special case, if src->code==FSL_RC_OOM, then the code is
   copied but the message bytes (if any) are not (under the
   assumption that we have no more memory).
*/
FSL_EXPORT int fsl_error_copy( fsl_error const * src,
                               fsl_error *  dest );

/**
   Swaps the state between two fsl_error objects and then resets the
   error state on the `from` object. Intended as an allocation
   optimization when propagating error state up the API.

   This is a no-op if from==to.

   Because this modifies one argument differently from the other, it's
   not exactly like a swap, but it has the effect of moving an error
   from the `from` object to the `to` object, while keeping `to`'s
   former memory intact for re-use (now inside the `from` object).

   Results are undefined if either parameter is NULL or either is not
   properly initialized. i.e. neither may refer to uninitialized
   memory. Copying fsl_error_empty at declaration-time is a simple way
   to ensure that instances are cleanly initialized.
*/
FSL_EXPORT void fsl_error_propagate( fsl_error *  from, fsl_error *  to );

/** Old name for fsl_error_propagate() */
#define fsl_error_move fsl_error_propagate
///** Old, misleading name for fsl_error_propagate() */
//#define fsl_error_swap fsl_error_propagate

/**
   Returns the given Unix Epoch timestamp value as its approximate
   Julian Day value. Note that the calculation does not account for
   leap seconds.
*/
FSL_EXPORT double fsl_unix_to_julian( fsl_time_t unixEpoch );

/**
   Returns the current Unix Epoch time converted to its approximate
   Julian form. Equivalent to fsl_unix_to_julian(time(0)). See
   fsl_unix_to_julian() for details. Note that the returned time
   has seconds, not milliseconds, precision.
*/
FSL_EXPORT double fsl_julian_now(void);

#if 0
/** UNTESTED, possibly broken vis-a-vis timezone conversion.

    Returns the given Unix Epoch time value formatted as an ISO8601
    string.  Returns NULL on allocation error, else a string 19
    bytes long plus a terminating NUL
    (e.g. "2013-08-19T20:35:49"). The returned memory must
    eventually be freed using fsl_free().
*/
FSL_EXPORT char * fsl_unix_to_iso8601( fsl_time_t j );
#endif

/**
   Returns true if the first 10 digits of z _appear_ to form the start
   of an ISO date string (YYYY-MM-DD). Whether or not the string is
   really a valid date is left for downstream code to
   determine. Returns false in all other cases, including if z is
   NULL.
*/
FSL_EXPORT bool fsl_str_is_date(const char *z);


/**
   Checks if z is syntactically a time-format string in the format:

   [Y]YYYY-MM-DD

   (Yes, the year may be five-digits, left-padded with a zero for
   years less than 9999.)

   Returns a positive value if the YYYYY part has five digits, a
   negative value if it has four. It returns 0 (false) if z does not
   match that pattern.

   If it returns a negative value, the MM part of z starts at byte offset
   (z+5), and a positive value means the MM part starts at (z+6).

   z need not be NUL terminated - this function does not read past
   the first invalid byte. Thus is can be used on, e.g., full
   ISO8601-format strings. If z is NULL, 0 is returned.
*/
FSL_EXPORT int fsl_str_is_date2(const char *z);


/**
   Returns the current error code of the given buffer. Many buffer
   APIs become noops when this function returns non-0.
*/
FSL_EXPORT int fsl_buffer_err(fsl_buffer const * b);

/**
   Resets the error code of the given buffer to 0.
*/
FSL_EXPORT void fsl_buffer_err_clear(fsl_buffer *  b);

/**
   Reserves at least n bytes of capacity in buf. Returns 0 on
   success, FSL_RC_OOM if allocation fails. Results are undefined
   if !buf.

   If fsl_buffer_err() is true and n is not 0 then this function
   returns its value without other side effects. If n is 0 then the
   error state is cleared.

   If b is an external buffer then:

   - If n is 0, this disassociates b->mem from b, effectively clearing
     the buffer's state. Else...

   - The buffer is materialized, transformed into a managed buffer.
     This happens even if n is less than b->used because this routine
     is always used in preparation for writing to the buffer.

   - If n>0 then the greater of (n, b->used) bytes of memory are
     allocated, b->used bytes are copied from b->mem (its external
     memory) to the new block, and b->mem is replaced with the new
     block. Afterwards, b->capacity will be non-0.

   This does not change b->used, nor will it shrink the buffer
   (reduce buf->capacity) unless n is 0, in which case it immediately:

   - frees b->mem (if b is a managed buffer)
   - sets b->capacity, buf->used, and b->cursor to 0
   - sets b->errCode to 0

   @see fsl_buffer_resize()
   @see fsl_buffer_materialize()
   @see fsl_buffer_clear()
*/
FSL_EXPORT int fsl_buffer_reserve( fsl_buffer *  b, fsl_size_t n );

/**
   If b is a "managed" buffer, this is a no-op and returns 0, else b
   is an "external" buffer and it...

   - Allocates enough memory to store b->used bytes plus a NUL
     terminator.

   - Copies b->mem to the new block.

   - NUL-terminates the new block.

   - Assigns b->mem to the new block.

   b is thereby transformed to a managed buffer, i.e. b owns b->mem.

   If fsl_buffer_err() is non-0 before this call, this call is a no-op
   and that value is returned.

   Returns 0 on success, FSL_RC_OOM on allocation error.

   Note that materialization happens automatically on demand by
   fsl_buffer APIs which write to the buffer but clients can use this
   to ensure that it is managed memory before they manipulate b->mem
   directly.

   @see fsl_buffer_external()
*/
FSL_EXPORT int fsl_buffer_materialize( fsl_buffer *  b );

/**
   Initializes b to be an "external" buffer pointing to n bytes of the
   given memory. If n is negative, the equivalent of fsl_strlen() is
   used to count its length. The buffer API treats external buffers as
   immutable. If asked to write to one, the API will first
   "materialize" the buffer, as documented for
   fsl_buffer_materialize().

   Either mem must be guaranteed to outlive b or b must be
   materialized before mem goes out of scope.

   ACHTUNG: it is NEVER legal to pass a pointer which may get
   reallocated, as doing so may change its address, invaliding the
   resulting `b->mem` pointer. Similarly, it is never legal to pass it
   scope-local memory unless b's lifetime is limited to that scope.

   If b->mem is not NULL, this function first passes the buffer to
   fsl_buffer_clear() to ensure that this routine does not leak any
   dynamic memory it may already own.

   Results are undefined if mem is NULL, but n may be 0.

   Results are undefined if passed a completely uninitialized buffer
   object. _Always_ initialize new buffer objects by copying
   fsl_buffer_empty or (when appropriate) fsl_buffer_empty_m.

   This function resets b->errCode.

   @see fsl_buffer_materialize()
*/
FSL_EXPORT void fsl_buffer_external( fsl_buffer *  b, void const * mem, fsl_int_t n );

/**
   Convenience equivalent of fsl_buffer_reserve(b,0).
*/
FSL_EXPORT void fsl_buffer_clear( fsl_buffer *  b );

/**
   If b is a managed buffer, this resets b->used, b->cursor, and
   b->mem[0] (if b->mem is not NULL) to 0. If b is an external buffer,
   this clears all state from the buffer, behaving like
   fsl_buffer_clear() (making it available for reuse as a managed or
   external buffer).

   This does not (de)allocate memory, only changes the logical "used"
   size of the buffer. Returns its argument.

   This function resets b->errCode.

   Returns b.

   Achtung for fossil(1) porters: this function's semantics are much
   different from the fossil's blob_reset(). To get those semantics,
   use fsl_buffer_reserve(buf, 0) or its convenience form
   fsl_buffer_clear(). (This function _used_ to be called
   fsl_buffer_reset(), but it was renamed in the hope of avoiding
   related confusion.)
*/
FSL_EXPORT fsl_buffer * fsl_buffer_reuse( fsl_buffer *  b );

/**
   Similar to fsl_buffer_reserve() except that...

   - If fsl_buffer_err() is true, that result is returned with no other
     side effects.

   For managed buffers:

   - It does not free all memory when n==0. Instead it essentially
     makes the memory a length-0, NUL-terminated string.

   - It will try to shrink (realloc) buf's memory if (n<buf->capacity).

   - It sets buf->capacity to (n+1) and buf->used to n. This routine
     allocates one extra byte to ensure that buf is always
     NUL-terminated.

   - On success it always NUL-terminates the buffer at
   offset buf->used.

   For external buffers it behaves slightly differently:

   - If n==buf->used, this is a no-op and returns 0.

   - If n==0 then it behaves like fsl_buffer_external(buf,"",0)
     and returns 0.

   - Else it materializes the buffer, as per fsl_buffer_materialize(),
     copies the lesser of (n, buf->used) bytes from buf->mem to that
     memory, NUL-terminates the new block, replaces buf->mem with the
     new block, sets buf->used to n and buf->capacity to n+1.

   Returns 0 on success or FSL_RC_OOM if a (re)allocation fails. On
   allocation error, the buffer's memory state is unchanged.

   @see fsl_buffer_reserve()
   @see fsl_buffer_materialize()
   @see fsl_buffer_clear()
*/
FSL_EXPORT int fsl_buffer_resize( fsl_buffer *  buf, fsl_size_t n );

/**
   Swaps the entire state of the left and right arguments. Results are
   undefined if either argument is NULL or points to uninitialized
   memory.
*/
FSL_EXPORT void fsl_buffer_swap( fsl_buffer *  left, fsl_buffer *  right );

/**
   Similar fsl_buffer_swap() but it also optionally frees one of
   the buffer's memories after swapping them. If clearWhich is
   negative then the left buffer (1st arg) is cleared _after_
   swapping (i.e., the NEW left hand side gets cleared). If
   clearWhich is greater than 0 then the right buffer (2nd arg) is
   cleared _after_ swapping (i.e. the NEW right hand side gets
   cleared). If clearWhich is 0, this function behaves identically
   to fsl_buffer_swap().

   A couple examples should clear this up:

   ```
   fsl_buffer_swap_free( &b1, &b2, -1 );
   ```

   Swaps the contents of b1 and b2, then frees the contents
   of the left-side buffer (b1).

   ```
   fsl_buffer_swap_free( &b1, &b2, 1 );
   ```

   Swaps the contents of b1 and b2, then frees the contents
   of the right-side buffer (b2).
*/
FSL_EXPORT void fsl_buffer_swap_free( fsl_buffer *  left,
                                      fsl_buffer *  right,
                                      int clearWhich );

/**
   Appends the first n bytes of src, plus a NUL byte, to b,
   expanding b as necessary and incrementing b->used by n. If n is
   less than 0 then the equivalent of fsl_strlen((char const*)src)
   is used to calculate the length.

   If fsl_buffer_err() is true, its result is returned without further
   side effects.

   If b is an external buffer, it is first transformed into a
   managed buffer.

   Results are undefined if b or src are NULL.

   If n is 0 (or negative and !*src), this function ensures that
   b->mem is not NULL and is NUL-terminated, so it may allocate
   to have space for that NUL byte.

   Returns 0 on success, FSL_RC_OOM if allocation of memory fails.

   If this function succeeds, it guarantees that it NUL-terminates the
   buffer (but that the NUL terminator is not counted in b->used).

   This function does not modify b's cursor.

   @see fsl_buffer_appendf()
   @see fsl_buffer_reserve()
*/
FSL_EXPORT int fsl_buffer_append( fsl_buffer *  b,
                                  void const * src, fsl_int_t n );

/**
   Uses fsl_appendf() to append formatted output to the given buffer.
   Returns 0 on success and FSL_RC_OOM if an allocation fails while
   expanding dest. Results are undefined if either of the first two
   arguments are NULL.

   If fsl_buffer_err() is true, its result is returned without further
   side effects.

   @see fsl_buffer_append()
   @see fsl_buffer_reserve()
*/
FSL_EXPORT int fsl_buffer_appendf( fsl_buffer *  dest,
                                   char const * fmt, ... );

/** va_list counterpart to fsl_buffer_appendf(). */
FSL_EXPORT int fsl_buffer_appendfv( fsl_buffer *  dest,
                                    char const * fmt, va_list args );

/**
   Appends the given byte to b. This is slightly more efficient than
   fsl_buffer_append() for the common case where b has slack memory to
   fill.

   If b->errCode is non-0, this is a no-op and returns that code.

   On success this routine always NUL-terminates b. On error,
   b->errCode is returned.
*/
FSL_EXPORT int fsl_buffer_appendch(fsl_buffer *  b, char c);

/**
   Add an EOL to b if it has content (i.e. b->used is not 0) but does
   not end on one, else do nothing. It does not modify empty buffers.
   b is materialized if needed.

   Returns b->errCode and is a no-op if b->errCode is non-0 when this
   is called.
*/
FSL_EXPORT int fsl_buffer_ensure_eol(fsl_buffer *  b);

/**
   If b ends with a `\n` character or a pair of `\r\n`, the `\n` or
   `\r\n` pair are "chomped" (to use Perl's term for it), reducing
   b->used by 1 or 2, else this function has no side-effects.

   For external buffers, the b->used is adjusted but the memory is
   otherwise not modified (as external buffers are never guaranteed to
   be NUL-terminated). For non-external buffers b is NUL-terminated at
   its new length. If you really need to chomp an external buffer,
   pass it to fsl_buffer_materialize() first.

   This is a no-op if b->errCode is non-0. (We don't strictly need to
   do so, but we do for consistency - b's state should not change when
   errCode is non-0.)
*/
FSL_EXPORT void fsl_buffer_chomp(fsl_buffer *  b);

/**
   Expects z to be a single argument intended for future call to
   system()-like function. It is quoted/escaped appropriately and
   appended to b, adding a space between any previous contents of
   b. It will fail, returning FSL_RC_SYNTAX, if the input contains
   what it believes to be malicious characters or if z contains any
   non-UTF-8 characters. It does not permit control characters of any
   kind, for example.

   If isFilename is true and z starts with a '-' character then the
   result gets prefixed with `./` (or `.\` on Windows) to (A)
   differentiate it from a flag argument and (B) avoid passing an
   unintended, but valid, flag to a command. isFilename has no further
   effect.

   The intent is that this function be called once per argument for a
   pending system()-style call, and b's contents eventually be used as
   the argument for that system()-style call.

   If b->errCode is non-0 when this is called, this function is a no-op
   and that value is returned.

   If err is not NULL then any validation-time error state are set in
   err and err->code is returned, It may fail for a number of reasons,
   as it performs checks on the input to avoid certain types of
   malicious uses. If initial validation of b->errCode and
   materialization of b (if needed) both pass, err is passed to
   fsl_error_reset() before this function starts its work.

   If err is NULL then this function still returns non-0 on error but
   the caller will not know exactl which validation failed.

   If this function returns FSL_RC_SYNTAX and err is not NULL, err
   will contain more info about the problem. If it returns any other
   error code, it's an allocation error and err will not have been
   updated.

   On error it always updates b->errCode so that subsequent calls
   will not mask the error.
*/
FSL_EXPORT int fsl_buffer_esc_arg_v2(fsl_buffer *  b,
                                     fsl_error *  err,
                                     char const *z,
                                     bool isFilename);
/**
   Equivalent to fsl_buffer_esc_arg_v2() with NULL as its second
   argument.
*/

FSL_EXPORT int fsl_buffer_esc_arg(fsl_buffer *  b,
                                  char const *z,
                                  bool isFilename);

/**
   Hex-encodes n bytes of src into n*2 hex-digit bytes appended to
   b. On error, b->errCode will be set to non-0 and that value is
   returned. If the final argument is true, upper-case hex digits are
   used, else lower-case are used.

   If fsl_buffer_err() is non-0 when this is called, this function has
   no side effects and that value is returned.
*/
FSL_EXPORT int fsl_buffer_append_hex( fsl_buffer *  b,
                                      void const * src, fsl_uint_t n,
                                      bool upperCase);


/**
   Compresses the first pIn->used bytes of pIn to pOut. It is ok for
   pIn and pOut to be the same blob.

   If fsl_buffer_err() is true for either buffer, its result is
   returned without further side effects. The buffers are checked in
   the order of their parameter declaration.

   pOut must either be the same as pIn or else a properly
   initialized buffer. Any prior contents will be freed or their
   memory reused.

   Results are undefined if any argument is NULL.

   Returns 0 on success, FSL_RC_OOM on allocation error, and
   FSL_RC_ERROR if the lower-level compression routines fail. If this
   function returns non-0, it does not update pOut's error state
   because the error will have happened on a temporary buffer.

   Use fsl_buffer_uncompress() to uncompress the data. The data is
   encoded with a big-endian, unsigned 32-bit length as the first four
   bytes (holding its uncomressed size), and then the data as
   compressed by zlib.

   TODO: if pOut!=pIn1 then re-use pOut's memory, if it has any.

   @see fsl_buffer_compress2()
   @see fsl_buffer_uncompress()
   @see fsl_buffer_is_compressed()
*/
FSL_EXPORT int fsl_buffer_compress(fsl_buffer const *pIn, fsl_buffer *  pOut);

/**
   Compress the concatenation of a blobs pIn1 and pIn2 into pOut.

   pOut must be either empty (cleanly initialized or newly
   recycled) or must be the same as either pIn1 or pIn2.

   Results are undefined if any argument is NULL.

   If fsl_buffer_err() is true for any buffer, its result is
   returned without further side effects. The buffers are checked in
   the order of their parameter declaration.

   Returns 0 on success, FSL_RC_OOM on allocation error, and FSL_RC_ERROR
   if the lower-level compression routines fail.

   TODO: if pOut!=(pIn1 or pIn2) then re-use its memory, if it has any.

   @see fsl_buffer_compress()
   @see fsl_buffer_uncompress()
   @see fsl_buffer_is_compressed()
*/
FSL_EXPORT int fsl_buffer_compress2(fsl_buffer const *pIn1,
                                    fsl_buffer const *pIn2,
                                    fsl_buffer *  pOut);

/**
   Uncompress buffer pIn and store the result in pOut. It is ok for
   pIn and pOut to be the same buffer. Returns 0 on success. If
   pIn!=pOut then on error, depending on the type of error, pOut may
   have been partially written so the state of its contents are
   unspecified (but its state as a buffer object is still valid).

   If fsl_buffer_err() is true for either buffer, its result is
   returned without further side effects. The buffers are checked in
   the order of their parameter declaration.

   pOut must be either cleanly initialized/empty or the same object as
   pIn. If it has any current memory, it will be reused if it's
   large enough and it is not the same pointer as pIn.

   Results are undefined if any argument is NULL.

   Returns 0 on success, FSL_RC_OOM on allocation error, and some
   other code if the lower-level decompression routines fail. On
   error, pOut->errCode is updated.

   Note that the decompression process, though computationally costly,
   is a no-op if pIn is not actually compressed.

   As a special case, if pIn==pOut and fsl_buffer_is_compressed() returns
   false for pIn then this is a no-op.

   @see fsl_buffer_compress()
   @see fsl_buffer_compress2()
   @see fsl_buffer_is_compressed()
*/
FSL_EXPORT int fsl_buffer_uncompress(fsl_buffer const * pIn,
                                     fsl_buffer *  pOut);

/**
   Returns true if this function believes that mem (which must be
   at least len bytes of valid memory long) appears to have been
   compressed by fsl_buffer_compress() or equivalent. This is not a
   100% reliable check - it could potentially have false positives
   on certain inputs, but that is thought to be unlikely (at least
   for text data).

   Returns 0 if mem is NULL.
*/
FSL_EXPORT bool fsl_data_is_compressed(unsigned char const * mem, fsl_size_t len);

/**
   Equivalent to fsl_data_is_compressed(buf->mem, buf->used).
*/
FSL_EXPORT bool fsl_buffer_is_compressed(fsl_buffer const * buf);

/**
   If fsl_data_is_compressed(mem,len) returns true then this function
   returns the uncompressed size of the data, else it returns a negative
   value.
*/
FSL_EXPORT fsl_int_t fsl_data_uncompressed_size(unsigned char const *mem, fsl_size_t len);

/**
   The fsl_buffer counterpart of fsl_data_uncompressed_size().
*/
FSL_EXPORT fsl_int_t fsl_buffer_uncompressed_size(fsl_buffer const * b);

/**
   Equivalent to ((char const *)b->mem) except that if b->errCode is
   non-0, this returns NULL. The returned string is effectively
   b->used bytes long unless the user decides to apply his own
   conventions. Note that the buffer APIs generally assure that
   buffers are NUL-terminated, meaning that strings returned from this
   function can (for the vast majority of cases) assume that the
   returned string is NUL-terminated (with a string length of b->used
   _bytes_). It is, however, possible for client code to violate that
   convention via direct manipulation of the buffer or using
   non-NUL-terminated extranal buffers.

   Results are undefined if b is NULL.

   @see fsl_buffer_str()
   @see fsl_buffer_cstr2()
*/
FSL_EXPORT char const * fsl_buffer_cstr(fsl_buffer const * b);

/**
   If b has any memory allocated to it and b->errCode is non-0, that
   memory is returned. If len is not NULL then *len is set to
   b->used. If b has no memory or b->errCode is non-0 then NULL is
   returned and *len (if len is not NULL) is set to 0.

   Results are undefined if b is NULL.

   @see fsl_buffer_str()
   @see fsl_buffer_cstr()
*/
FSL_EXPORT char const * fsl_buffer_cstr2(fsl_buffer const * b, fsl_size_t *  len);

/**
   Equivalent to ((char *)b->mem) except that if b->errCode is non-0
   then NULL is returned. The returned memory is effectively b->used
   bytes long unless the user decides to apply their own conventions.

   Care must be taken to only write to the returned pointer for memory
   owned or write-proxied by this buffer. More specifically, results
   are undefined if b is an external buffer proxying const bytes. When
   in doubt about whether b is external, use fsl_buffer_materialize()
   to transform it to a managed buffer before using this routine,
   noting that any of the public fsl_buffer APIs which write to a
   buffer will materialize it on demand if needed.

   @see fsl_buffer_take()
*/
FSL_EXPORT char * fsl_buffer_str(fsl_buffer const * b);

/**
   "Takes" the memory refered to by the given buffer, transfering
   ownership to the caller. After calling this, b's state will be
   empty. If b is an external buffer, this will materialize it
   first and return NULL if that fails.

   @see fsl_buffer_materialize()
   @see fsl_buffer_str()
   @see fsl_buffer_cstr()
   @see fsl_buffer_cstr2()
*/
FSL_EXPORT char * fsl_buffer_take(fsl_buffer *  b);

/**
   Returns the "used" size of b, or 0 if !b.
*/
#define fsl_buffer_size(b) (b)->used
#if 0
FSL_EXPORT fsl_size_t fsl_buffer_size(fsl_buffer const * b);
#endif

/**
   Returns the current capacity of b, or 0 if !b.
*/
#define fsl_buffer_capacity(b) (b)->capacity
#if 0
FSL_EXPORT fsl_size_t fsl_buffer_capacity(fsl_buffer const * b);
#endif
/**
   Compares the contents of buffers lhs and rhs using memcmp(3)
   semantics. Returns negative, zero, or positive if the first buffer
   is less then, equal to, or greater than the second.  Results are
   undefined if either argument is NULL.

   When buffers of different length match on the first N bytes,
   where N is the shorter of the two buffers' lengths, it treats the
   shorter buffer as being "less than" the longer one.
*/
FSL_EXPORT int fsl_buffer_compare(fsl_buffer const * lhs,
                                  fsl_buffer const * rhs);

/**
   Compares b and the first nStr bytes of the given string. If nStr is
   negative, fsl_strlen() is used to calculate it. Returns true if the
   buffer and the string match, as per the rules of
   fsl_buffer_compare(), else returns false.
*/
FSL_EXPORT bool fsl_buffer_eq(fsl_buffer const * b, char const * str,
                              fsl_int_t nStr);

/**
   Bitwise-compares the contents of b against the file named by
   zFile.  Returns 0 if they have the same size and contents, else
   non-zero.  This function has no way to report if zFile cannot be
   opened, and any error results in a non-0 return value. No
   interpretation/canonicalization of zFile is performed - it is
   used as-is.

   This resolves symlinks and returns non-0 if zFile refers (after
   symlink resolution) to a non-file.

   If zFile does not exist, is not readable, or has a different
   size than b->used, non-0 is returned without opening/reading the
   file contents. If a content comparison is performed, it is
   streamed in chunks of an unspecified (but relatively small)
   size, so it does not need to read the whole file into memory
   (unless it is smaller than the chunk size).
*/
FSL_EXPORT int fsl_buffer_compare_file( fsl_buffer const * b, char const * zFile );

/**
   Compare two buffers in constant (a.k.a. O(1)) time and return
   zero if they are equal.  Constant time comparison only applies
   for buffers of the same length.  If lengths are different,
   immediately returns 1. This operation is provided for cases
   where the timing/duration of fsl_buffer_compare() (or an
   equivalent memcmp()) might inadvertently leak security-relevant
   information.  Specifically, it address the concern that
   attackers can use timing differences to check for password
   misses, to narrow down an attack to passwords of a specific
   length or content properties.
*/
FSL_EXPORT int fsl_buffer_compare_O1(fsl_buffer const * lhs, fsl_buffer const * rhs);

/**
   Overwrites dest's contents with a copy of those from src
   (reusing dest's memory if it has any). Results are undefined if
   either pointer is NULL or invalid. Returns 0 on success,
   FSL_RC_OOM on allocation error.
*/
FSL_EXPORT int fsl_buffer_copy( fsl_buffer *  dest,
                                fsl_buffer const * src );

/**
   Apply the delta in pDelta to the original content pOriginal to
   generate the target content pTarget. All three pointers must point
   to properly initialized memory.

   If pTarget==pOriginal then this is a destructive operation,
   replacing the original's content with its new form.

   If fsl_buffer_err() is true for any buffer, its result is
   returned without further side effects. The buffers are checked in
   the order of their parameter declaration.

   Return 0 on success.

   @see fsl_buffer_delta_apply()
   @see fsl_delta_apply()
   @see fsl_delta_apply2()
*/
FSL_EXPORT int fsl_buffer_delta_apply( fsl_buffer const * pOriginal,
                                       fsl_buffer const * pDelta,
                                       fsl_buffer *  pTarget);

/**
   Identical to fsl_buffer_delta_apply() except that if delta
   application fails then any error messages/codes are written to
   pErr if it is not NULL. It is rare that delta application fails
   (only if the inputs are invalid, e.g. do not belong together or
   are corrupt), but when it does, having error information can be
   useful.

   If fsl_buffer_err() is true for any buffer, its result is
   returned without further side effects. The buffers are checked in
   the order of their parameter declaration.

   @see fsl_buffer_delta_apply()
   @see fsl_delta_apply()
   @see fsl_delta_apply2()
*/
FSL_EXPORT int fsl_buffer_delta_apply2( fsl_buffer const * pOriginal,
                                        fsl_buffer const * pDelta,
                                        fsl_buffer *  pTarget,
                                        fsl_error *  pErr);


/**
   Uses a fsl_input_f() function to buffer input into a fsl_buffer.

   dest must be a non-NULL, initialized (though possibly empty)
   fsl_buffer object. Its contents, if any, will be overwritten by
   this function, and any memory it holds might be re-used.

   If fsl_buffer_err() is true then this function returns its value
   without other side effects.

   The src function is called, and passed the state parameter, to
   fetch the input. If it returns non-0, this function returns that
   error code. src() is called, possibly repeatedly, until it
   reports that there is no more data.

   Whether or not this function succeeds, dest still owns any memory
   pointed to by dest->mem, and the client must eventually free it
   by calling fsl_buffer_reserve(dest,0).

   dest->mem might (and possibly will) be (re)allocated by this
   function, so any pointers to it held from before this call might
   be invalidated by this call.

   On error non-0 is returned and dest may be partially populated.

   Errors include:

   - Allocation error (FSL_RC_OOM), in which case dest->errCode is
     updated.

   - src() returns an error code, in which case dest->errCode is _not_
     modified.

   Whether or not the state parameter may be NULL depends on the src
   implementation requirements.

   On success dest will contain the contents read from the input
   source. dest->used will be the length of the read-in data, and
   dest->mem will point to the memory. dest->mem is automatically
   NUL-terminated if this function succeeds, but dest->used does not
   count that terminator. On error the state of dest->mem must be
   considered incomplete, and is not guaranteed to be NUL-terminated.

   Example usage:

   ```
   fsl_buffer buf = fsl_buffer_empty;
   int rc = fsl_buffer_fill_from( &buf,
     fsl_input_f_FILE,
      stdin );
   if( rc ){
   fprintf(stderr,"Error %d (%s) while filling buffer.\n",
   rc, fsl_rc_cstr(rc));
   fsl_buffer_reserve( &buf, 0 );
     return ...;
   }
   ... use the buf->mem ...
   ... clean up the buffer ...
   fsl_buffer_reserve( &buf, 0 );
   ```

   To take over ownership of the buffer's memory, do:

   ```
   void * mem = buf.mem;
   buf = fsl_buffer_empty;
   ```

   In which case the memory must eventually be passed to fsl_free()
   to free it.
*/
FSL_EXPORT int fsl_buffer_fill_from( fsl_buffer *  dest,
                                     fsl_input_f src, void *  state );

/**
   A fsl_buffer_fill_from() proxy which overwrite's dest->mem with
   the contents of the given FILE handler (which must be opened for
   read access).  Returns 0 on success, after which dest->mem
   contains dest->used bytes of content from the input source. On
   error dest may be partially filled.
*/
FSL_EXPORT int fsl_buffer_fill_from_FILE( fsl_buffer *  dest,
                                          FILE *  src );

/**
   A wrapper for fsl_buffer_fill_from_FILE() which gets its input
   from the given file name.

   If fsl_buffer_err() is true then this function returns its value
   without other side effects.

   It uses fsl_fopen() to open the file, so it supports the name '-'
   as an alias for stdin.
*/
FSL_EXPORT int fsl_buffer_fill_from_filename( fsl_buffer *  dest,
                                              char const * filename );

/**
   Writes the given buffer to the given filename. Returns 0 on success,
   FSL_RC_MISUSE if !b or !fname, FSL_RC_IO if opening or writing fails.

   If fsl_buffer_err() is true, that result is returned with no other
   side effects.

   Uses fsl_fopen() to open the file, so it supports the name '-'
   as an alias for stdout.
*/
FSL_EXPORT int fsl_buffer_to_filename( fsl_buffer const * b,
                                       char const * fname );

/**
   Copy N lines of text from pFrom into pTo. The copy begins at the
   current pFrom->cursor position. On success, pFrom->cursor is left
   pointing at the first character past the last `\n` copied.
   (Modification of the cursor is why pFrom is not const.) The copy is
   made in a single go, not line-by-line.

   If fsl_buffer_err() is true for either buffer (checked in parameter
   order), that result is returned with no other side effects.

   If pTo==NULL then this routine simply skips over N lines.

   Returns 0 if it copies lines or does nothing (because N is 0 or
   pFrom's contents have been exhausted). Copying fewer lines than
   requested (because of EOF) is not an error. Returns non-0 only on
   allocation error. Results are undefined if pFrom is NULL or not
   properly initialized. Whether or not it copies anything can be
   checked by copying pTo->used before calling this and comparing
   pTo->used to that value afterwards. The difference is the number of
   bytes added.

   @see fsl_buffer_stream_lines()
*/
FSL_EXPORT int fsl_buffer_copy_lines(fsl_buffer *  pTo,
                                     fsl_buffer *  pFrom,
                                     fsl_size_t N);

/**
   Works identically to fsl_buffer_copy_lines() except that it sends
   its output to the fTo output function. If fTo is NULL then it
   simply skips over N lines. The callback is called a single time,
   not once per line.

   If fsl_buffer_err() is true, that result is returned with no other
   side effects.

   @see fsl_buffer_copy_lines()
*/
FSL_EXPORT int fsl_buffer_stream_lines(fsl_output_f fTo, void *  toState,
                                       fsl_buffer *  pFrom,
                                       fsl_size_t N);


/**
   Works like fsl_appendfv(), but appends all output to a
   dynamically-allocated string, expanding the string as necessary
   to collect all formatted data. The returned NUL-terminated string
   is owned by the caller and it must be cleaned up using
   fsl_free(...). If !fmt, NULL is returned. It is conceivable that
   it returns NULL on a zero-length formatted string, e.g.  (%.*s)
   with (0,"...") as arguments, but it will only do that if the
   whole format string resolves to empty.
*/
FSL_EXPORT char * fsl_mprintf( char const * fmt, ... );

/**
   va_list counterpart to fsl_mprintf().
*/
FSL_EXPORT char * fsl_mprintfv(char const * fmt, va_list vargs );

/**
   An sprintf(3) clone which uses fsl_appendf() for the formatting.
   Outputs at most n bytes to dest. Returns 0 on success, non-0
   on error. Returns 0 without side-effects if !n or !*fmt.

   If the destination buffer is long enough, this function
   NUL-terminates it.
*/
FSL_EXPORT int fsl_snprintf( char * dest, fsl_size_t n, char const * fmt, ... );

/**
   va_list counterpart to fsl_snprintf()
*/
FSL_EXPORT int fsl_snprintfv( char * dest, fsl_size_t n, char const * fmt, va_list args );

/**
   Equivalent to fsl_strndup(src,-1).
*/
FSL_EXPORT char * fsl_strdup( char const * src );

/**
   Similar to strndup(3) but returns NULL if !src.  The returned
   memory must eventually be passed to fsl_free(). Returns NULL on
   allocation error. If len is less than 0 and src is not NULL then
   fsl_strlen() is used to calculate its length.

   If src is not NULL but len is 0 then it will return an empty
   (length-0) string, as opposed to NULL.
*/
FSL_EXPORT char * fsl_strndup( char const * src, fsl_int_t len );

/**
   Equivalent to strlen(3) but returns 0 if src is NULL.
   Note that it counts bytes, not UTF characters.
*/
FSL_EXPORT fsl_size_t fsl_strlen( char const * src );

/**
   Returns the number of UTF8 characters which begin within the first
   n bytes of str (noting that it's possible that a multi-byte
   character starts 1-3 bytes away from the end and overlaps past the
   end of (str+len)). If len is negative then fsl_strlen() is used to
   calculate it.

   Results are undefined if str is not UTF8 input or if str contains a
   BOM marker.
*/
FSL_EXPORT fsl_size_t fsl_strlen_utf8( char const * str, fsl_int_t n );

/**
   Like strcmp(3) except that it accepts NULL pointers.  NULL sorts
   before all non-NULL string pointers.  Also, this routine
   performs a binary comparison that does not consider locale.
*/
FSL_EXPORT int fsl_strcmp( char const * lhs, char const * rhs );

/**
   Equivalent to fsl_strcmp(), but with a signature suitable
   for use as a generic comparison function (e.g. for use with
   qsort() and search algorithms).
*/
FSL_EXPORT int fsl_strcmp_cmp( void const * lhs, void const * rhs );

/**
   Case-insensitive form of fsl_strcmp().

   @implements fsl_generic_cmp_f()
*/
FSL_EXPORT int fsl_stricmp(const char *zA, const char *zB);

/**
   Equivalent to fsl_stricmp(), but with a signature suitable
   for use as a generic comparison function (e.g. for use with
   qsort() and search algorithms).

   @implements fsl_generic_cmp_f()
*/
FSL_EXPORT int fsl_stricmp_cmp( void const * lhs, void const * rhs );

/**
   fsl_strcmp() variant which compares at most nByte bytes of the
   given strings, case-insensitively.  If nByte is less than 0 then
   fsl_strlen(zB) is used to obtain the length for comparision
   purposes.
*/
FSL_EXPORT int fsl_strnicmp(const char *zA, const char *zB, fsl_int_t nByte);

/**
   fsl_strcmp() variant which compares at most nByte bytes of the
   given strings, case-sensitively. Returns 0 if nByte is 0.
*/
FSL_EXPORT int fsl_strncmp(const char *zA, const char *zB, fsl_size_t nByte);

/**
   BSD strlcpy() variant which is less error prone than strncpy. Copy up to
   dstsz - 1 characters from src to dst and NUL-terminate the resulting string
   if dstsz is not 0.

   Returns the length of the string it writes to dst. If it returns a value
   equal to or greater than its 3rd argument then the output was truncated.
*/
FSL_EXPORT fsl_size_t fsl_strlcpy(char *dst, const char *src, fsl_size_t dstsz);

/**
   BSD strlcat() variant which is less error prone than strncat. Append src to
   the end of dst. Append at most dstsz - strlen(dst - 1) characters, and
   NUL-terminate unless dstsize is 0 or the passed in dst string was longer
   than dstsz to begin with.

   Returns the initial string-length of dest plus the length src. If
   it returns a value equal to or greater than its 3rd argument then
   the output was truncated.
*/
FSL_EXPORT fsl_size_t fsl_strlcat(char *dst, const char *src, fsl_size_t dstsz);

/**
   Equivalent to fsl_strncmp(lhs, rhs, X), where X is either
   FSL_STRLEN_SHA1 or FSL_STRLEN_K256: if both lhs and rhs are
   longer than FSL_STRLEN_SHA1 then they are assumed to be
   FSL_STRLEN_K256 bytes long and are compared as such, else they
   are assumed to be FSL_STRLEN_SHA1 bytes long and compared as
   such.

   Potential FIXME/TODO: if their lengths differ, i.e. one is v1 and
   one is v2, compare them up to their common length then, if they
   still compare equivalent, treat the shorter one as less-than the
   longer.
*/
FSL_EXPORT int fsl_uuidcmp( fsl_uuid_cstr lhs, fsl_uuid_cstr rhs );

/**
   Returns false if s is NULL or starts with any of (0 (NUL), '0'
   (ASCII character zero), 'f', 'n', "off"), case-insensitively,
   else it returns true.
*/
FSL_EXPORT bool fsl_str_bool( char const * s );

/**
   _Almost_ equivalent to fopen(3) but:

   - expects name to be UTF8-encoded.

   - If name=="-", it returns one of stdin or stdout, depending on
   the mode string: stdout is returned if 'w' or '+' appear,
   otherwise stdin.

   If it returns NULL, the global errno "should" contain a description
   of the problem unless the problem was argument validation. Pass it
   to fsl_errno_to_rc() to convert that into an API-conventional error
   code.

   If at all possible, use fsl_fclose() (as opposed to fclose()) to
   close these handles, as it has logic to skip closing the
   standard streams.

   Potential TODOs:

   - extend mode string to support 'x', meaning "exclusive", analog
   to open(2)'s O_EXCL flag. Barring race conditions, we have
   enough infrastructure to implement that. (It turns out that
   glibc's fopen() supports an 'x' with exactly this meaning.)

   - extend mode to support a 't', meaning "temporary". The idea
   would be that we delete the file from the FS right after
   opening, except that Windows can't do that.
*/
FSL_EXPORT FILE * fsl_fopen(char const * name, char const *mode);

/**
   Passes f to fclose(3) unless f is NULL or one of the C-standard
   (stdin, stdout, stderr) handles, in which cases it does nothing
   at all.
*/
FSL_EXPORT void fsl_fclose(FILE * f);

/**
   This function works similarly to classical printf
   implementations, but instead of outputing somewhere specific, it
   uses a callback function to push its output somewhere. This
   allows it to be used for arbitrary external representations. It
   can be used, for example, to output to an external string, a UI
   widget, or file handle (it can also emulate printf by outputing
   to stdout this way).

   INPUTS:

   pfAppend: The is a fsl_output_f function which is responsible
   for accumulating the output. If pfAppend returns a negative
   value then processing stops immediately.

   pfAppendArg: is ignored by this function but passed as the first
   argument to pfAppend. pfAppend will presumably use it as a data
   store for accumulating its string.

   fmt: This is the format string, as in the usual printf(3), except
   that it supports more options (detailed below).

   ap: This is a pointer to a list of arguments.  Same as in
   vprintf() and friends.


   OUTPUTS:

   ACHTUNG: this interface changed significantly in 2021-09:

   - The output function was changed from a type specific to this
   interface to fsl_output_f().

   - The return semantics where changed from printf()-like to
   0 on success, non-0 on error.

   Most printf-style specifiers work as they do in standard printf()
   implementations. There might be some very minor differences, but
   the more common format specifiers work as most developers expect
   them to. In addition...

   Current (documented) printf extensions:

   `%s` works like conventional printf `%s` except that any precision
   value can be modified via the '#' flag to count in UTF8 characters
   instead of bytes. That is, if an `%#.10s` argument has a byte
   length of 20, a precision of 10, and contains only 8 UTF8, its
   precision will allow it to output all 8 characters, even though
   they total 20 bytes. The '#' flag works this way for both width and
   precision.

   `%z` works exactly like `%s`, but takes a non-const (char *) and
   deletes the string (using fsl_free()) after appending it to the
   output.

   `%h` (HTML) works like `%s` but (A) does not support the '#' flag and
   (B) converts certain characters (namely '<' and '&') to their HTML
   escaped equivalents.

   `%t` (URL encode) works like `%h` but converts certain characters
   into a representation suitable for use in an HTTP URL. (e.g. ' '
   gets converted to `%20`)

   `%T` (URL decode) does the opposite of `%t` - it decodes
   URL-encoded strings and outputs their decoded form. ACHTUNG:
   fossil(1) interprets this the same as `%t` except that it leaves
   '/' characters unescaped (did that change at some point? This code
   originally derived from that one some years ago!). It is still to
   be determined whether we "really need" that behaviour (we don't
   really need either one, seeing as the library is not CGI-centric
   like fossil(1) is).

   `%r` requires an int and renders it in "ordinal form". That is,
   the number 1 converts to "1st" and 398 converts to "398th".

   `%q` quotes a string as required for SQL. That is, '\''
   characters get doubled. It does NOT included the outer quotes
   and NULL values get replaced by the string "(NULL)" (without
   quotes). See `%Q`...

   `%Q` works like `%q`, but includes the outer '\'' characters and
   NULL pointers get output as the string literal "NULL" (without
   quotes), i.e. an SQL NULL. If modified with `%!Q` then it instead
   uses double quotes, the intent being for use with identifiers.
   In that form it still emits `NULL` without quotes, but it is not
   intended to be used with `NULL` values.

   `%S` works like `%.16s`. It is intended for fossil hashes. The '!'
   modifier removes the length limit, resulting in the whole hash
   (making this formatting option equivalent to `%s`).  (Sidebar: in
   fossil(1) this length is runtime configurable but that requires
   storing that option in global state, which is not an option for
   this implementation.)

   `%/`: works mostly like `%s` but normalizes path-like strings by
   replacing backslashes with the One True Slash.

   `%b`: works like `%s` but takes its input from a (fsl_buffer
   const*) argument. It does not support the '#' flag.

   `%B`: works like `%Q` but takes its input from a (fsl_buffer
   const*) argument.

   `%F`: works like `%s` but runs the output through
   fsl_bytes_fossilize().  This requires dynamic memory allocation, so
   is less efficient than re-using a client-provided buffer with
   fsl_bytes_fossilize() if the client needs to fossilize more than
   one element. Does not support the '#' flag.

   `%j`: works like `%s` but JSON-encodes the string. It does not
   include the outer quotation marks by default, but using the '!'
   flag, i.e. `%!j`, causes those to be added. The length and precision
   flags are NOT supported for this format. Results are undefined if
   given input which is not legal UTF8. By default non-ASCII
   characters with values less than 0xffff are emitted as as literal
   characters (no escaping), but the '#' modifier flag will cause it
   to emit such characters in the `\u####` form. It always encodes
   characters above 0xFFFF as UTF16 surrogate pairs (as JSON
   requires). Invalid UTF8 characters may get converted to '?' or may
   produce invalid JSON output. As a special case, if the value is NULL
   pointer, it resolves to "null" without quotes (regardless of the '!'
   modifier).

   `%R`: requires an integer argument and renders it like fsl_rc_cstr()
   would. If that function result is NULL then "#n" is used, where n
   is the integer argument.

   Some of these extensions may be disabled by setting certain macros
   when compiling appendf.c (see that file for details).

   Potential TODO: add fsl_bytes_fossilize_out() which works like
   fsl_bytes_fossilize() but sends its output to an fsl_output_f(), so
   that this routine doesn't need to alloc for that case.
*/
FSL_EXPORT int fsl_appendfv(fsl_output_f pfAppend, void * pfAppendArg,
                            const char *fmt, va_list ap );

/**
   Identical to fsl_appendfv() but takes an ellipses list (...)
   instead of a va_list.
*/
FSL_EXPORT int fsl_appendf(fsl_output_f pfAppend,
                           void * pfAppendArg,
                           const char *fmt,
                           ... )
#if 0
/* Would be nice, but complains about our custom format options: */
  __attribute__ ((__format__ (__printf__, 3, 4)))
#endif
  ;

/**
   Works like fsl_appendf() except that it sends the first n bytes of
   str as-is to pfAppend(). If n is negative, fsl_strlen() is used to
   calculate its lenght.
*/
FSL_EXPORT int fsl_appendn(fsl_output_f pfAppend, void * pfAppendArg,
                           const char *str, fsl_int_t n);


/**
   A fsl_output_f() impl which requires that state be an opened,
   writable (FILE*) handle.
*/
FSL_EXPORT int fsl_output_f_FILE( void * state, void const * s,
                                  fsl_size_t n );


/**
   Emulates fprintf() using fsl_appendf(). Returns the result of
   passing the data through fsl_appendf() to the given file handle.
*/
FSL_EXPORT int fsl_fprintf( FILE * fp, char const * fmt, ... );

/**
   The va_list counterpart of fsl_fprintf().
*/
FSL_EXPORT int fsl_fprintfv( FILE * fp, char const * fmt, va_list args );


/**
   Possibly reallocates self->list, changing its size. This function
   ensures that self->list has at least n entries. If n is 0 then
   the list is deallocated (but the self object is not), BUT THIS
   DOES NOT DO ANY TYPE-SPECIFIC CLEANUP of the items. If n is less
   than or equal to self->capacity then there are no side effects. If
   n is greater than self->capacity, self->list is reallocated and
   self->capacity is adjusted to be at least n (it might be bigger -
   this function may pre-allocate a larger value).

   Passing an n of 0 when self->capacity is 0 is a no-op.

   Newly-allocated slots will be initialized with NULL pointers.

   Returns 0 on success, FSL_RC_MISUSE if !self, FSL_RC_OOM if
   reservation of new elements fails.

   The return value should be used like this:

   ```
   fsl_size_t const n = number of bytes to allocate;
   int const rc = fsl_list_reserve( myList, n );
   if( rc ) { ... error ... }
   ```

   @see fsl_list_clear()
   @see fsl_list_visit_free()
*/
FSL_EXPORT int fsl_list_reserve( fsl_list *  self, fsl_size_t n );

/**
   Appends a bitwise copy of cp to self->list, expanding the list as
   necessary and adjusting self->used.

   Returns 0 on success, FSL_RC_OOM on allocation error.

   Ownership of cp is unchanged by this call. cp may not be NULL.
*/
FSL_EXPORT int fsl_list_append( fsl_list *  self, void * cp );

/**
   Swaps all contents of both lhs and rhs. Results are undefined if
   lhs or rhs are NULL or not properly initialized (via initial copy
   initialization from fsl_list_empty resp. fsl_list_empty_m).
*/
FSL_EXPORT void fsl_list_swap( fsl_list *  lhs, fsl_list *  rhs );

/** @typedef typedef int (*fsl_list_visitor_f)(void * p, void * visitorState )

    Generic visitor interface for fsl_list lists.  Used by
    fsl_list_visit(). p is the pointer held by that list entry and
    visitorState is the 4th argument passed to fsl_list_visit().

    Implementations must return 0 on success. Any other value causes
    looping to stop and that value to be returned, but interpretation
    of the value is up to the caller (it might or might not be an
    error, depending on the context). Note that client code may use
    custom values, and is not strictly required to use FSL_RC_xxx
    values. HOWEVER...  all of the libfossil APIs which take these
    as arguments may respond differently to some codes (most notable
    FSL_RC_BREAK, which they tend to treat as a
    stop-iteration-without-error result), so clients are strongly
    encourage to return an FSL_RC_xxx value on error.
*/
typedef int (*fsl_list_visitor_f)(void * obj, void * visitorState );

/**
   A fsl_list_visitor_f() implementation which requires that obj be
   arbitrary memory which can legally be passed to fsl_free()
   (which this function does). The visitorState parameter is
   ignored.
*/
FSL_EXPORT int fsl_list_v_fsl_free(void * obj, void * visitorState );


/**
   For each item in self->list, visitor(item,visitorState) is
   called.  The item is owned by self. The visitor function MUST
   NOT free the item (unless the visitor is a finalizer!), but may
   manipulate its contents if application rules do not specify
   otherwise.

   If order is 0 or greater then the list is traversed from start
   to finish, else it is traverse from end to begin.

   Returns 0 on success, non-0 on error.

   If visitor() returns non-0 then looping stops and that code is
   returned.
*/
FSL_EXPORT int fsl_list_visit( fsl_list const * self, int order,
                               fsl_list_visitor_f visitor, void * visitorState );

/**
   A list clean-up routine which takes a callback to clean up its
   contents.

   Passes each element in the given list to
   childFinalizer(item,finalizerState). If that returns non-0,
   processing stops and that value is returned, otherwise
   fsl_list_reserve(list,0) is called and 0 is returned.

   WARNING: if cleanup fails because childFinalizer() returns non-0,
   the returned object is effectively left in an undefined state and
   the client has no way (unless the finalizer somehow accounts for it)
   to know which entries in the list were cleaned up. Thus it is highly
   recommended that finalizer functions follow the conventional wisdom
   of "destructors do not throw."

   @see fsl_list_visit_free()
*/
FSL_EXPORT int fsl_list_clear( fsl_list *  list,
                               fsl_list_visitor_f childFinalizer,
                               void * finalizerState );
/**
   Similar to fsl_list_clear(list, fsl_list_v_fsl_free, NULL), but
   only frees list->list if the second argument is true, otherwise
   it sets the list's length to 0 but keep the list->list memory
   intact for later use.

   Be sure only to use this on lists of types for which fsl_free()
   is legal. i.e. don't use it on a list of fsl_deck objects or
   other types which have their own finalizers.

   Results are undefined if list is NULL.

   @see fsl_list_clear()
*/
FSL_EXPORT void fsl_list_visit_free( fsl_list *  list, bool freeListMem );

/**
   Works similarly to the visit operation without the _p suffix
   except that the pointer the visitor function gets is a (**)
   pointing back to the entry within this list. That means that
   callers can assign the entry in the list to another value during
   the traversal process (e.g. set it to 0). If shiftIfNulled is
   true then if the callback sets the list's value to 0 then it is
   removed from the list and self->used is adjusted (self->capacity
   is not changed).
*/
FSL_EXPORT int fsl_list_visit_p( fsl_list *  self, int order,
                                 bool shiftIfNulled,
                                 fsl_list_visitor_f visitor, void * visitorState );


/**
   Sorts the given list using the given comparison function. Neither
   argument may be NULL. The arugments passed to the comparison function
   will be pointers to pointers to the original entries, and may (depending
   on how the list is used) point to NULL.
*/
FSL_EXPORT void fsl_list_sort( fsl_list *  li, fsl_generic_cmp_f cmp);

/**
   Searches for a value in the given list, using the given
   comparison function to determine equivalence. The comparison
   function will never be passed a NULL value by this function - if
   value is NULL then only a NULL entry will compare equal to it.
   Results are undefined if li or cmpf are NULL.

   Returns the index in li of the entry, or a negative value if no
   match is found.
*/
FSL_EXPORT fsl_int_t fsl_list_index_of( fsl_list const * li,
                                        void const * value,
                                        fsl_generic_cmp_f cmpf);

/**
   Equivalent to fsl_list_index_of(li, key, fsl_strcmp_cmp).
*/
FSL_EXPORT fsl_int_t fsl_list_index_of_cstr( fsl_list const * li,
                                             char const * key );


/**
   Returns 0 if the given file is readable. Flags may be any values
   accepted by the access(2) resp. _waccess() system calls.
*/
FSL_EXPORT int fsl_file_access(const char *zFilename, int flags);

/**
   Computes a canonical pathname for a file or directory. Makes the
   name absolute if it is relative. Removes redundant / characters.
   Removes all /./ path elements. Converts /A/../ to just /. If the
   slash parameter is non-zero, the trailing slash, if any, is
   retained.

   If zRoot is not NULL then it is used for transforming a relative
   zOrigName into an absolute path. If zRoot is NULL fsl_getcwd()
   is used to determine the virtual root directory. If zRoot is
   empty (starts with a NUL byte) then this function effectively
   just sends zOrigName through fsl_file_simplify_name().

   Returns 0 on success, FSL_RC_MISUSE if !zOrigName or !pOut,
   FSL_RC_OOM if an allocation fails.

   pOut, if not NULL, is _appended_ to, so be sure to set pOut->used=0
   (or pass it to fsl_buffer_reuse()) before calling this to start
   writing at the beginning of a re-used buffer. On error pOut might
   conceivably be partially populated, but that is highly
   unlikely. Nonetheless, be sure to fsl_buffer_clear() it at some
   point regardless of success or failure.

   This function does no actual filesystem-level processing unless
   zRoot is NULL or empty (and then only to get the current
   directory). This does not confirm whether the resulting file
   exists, nor that it is strictly a valid filename for the current
   filesystem. It simply transforms a potentially relative path
   into an absolute one.

   Example:

   ```
   int rc;
   char const * zRoot = "/a/b/c";
   char const * zName = "../foo.bar";
   fsl_buffer buf = fsl_buffer_empty;
   rc = fsl_file_canonical_name2(zRoot, zName, &buf, 0);
   if(rc){
     fsl_buffer_clear(&buf);
     return rc;
   }
   assert(0 == fsl_strcmp( "/a/b/foo.bar, fsl_buffer_cstr(&buf)));
   fsl_buffer_clear(&buf);
   ```
*/
FSL_EXPORT int fsl_file_canonical_name2(const char *zRoot,
                                        const char *zOrigName,
                                        fsl_buffer *  pOut,
                                        bool slash);

/**
   Equivalent to fsl_file_canonical_name2(NULL, zOrigName, pOut, slash).

   @see fsl_file_canonical_name2()
*/

FSL_EXPORT int fsl_file_canonical_name(const char *zOrigName,
                                       fsl_buffer *  pOut, bool slash);

/**
   Calculates the "directory part" of zFilename and _appends_ it to
   pOut. The directory part is all parts up to the final path
   separator ('\\' or '/'). If leaveSlash is true then the separator
   part is appended to pOut, otherwise it is not. This function only
   examines the first nLen bytes of zFilename.  If nLen is negative
   then fsl_strlen() is used to determine the number of bytes to
   examine.

   If zFilename ends with a slash then it is considered to be its
   own directory part. i.e.  the dirpart of "foo/" evaluates to
   "foo" (or "foo/" if leaveSlash is true), whereas the dirpart of
   "foo" resolves to nothing (empty - no output except a NUL
   terminator sent to pOut).

   Returns 0 on success, FSL_RC_MISUSE if !zFilename or !pOut,
   FSL_RC_RANGE if 0==nLen or !*zFilename, and FSL_RC_OOM if
   appending to pOut fails. If zFilename contains only a path
   separator and leaveSlash is false then only a NUL terminator is
   appended to pOut if it is not already NUL-terminated.

   This function does no filesystem-level validation of the the
   given path - only string evaluation.
*/
FSL_EXPORT int fsl_file_dirpart(char const * zFilename, fsl_int_t nLen,
                                fsl_buffer * pOut, bool leaveSlash);

/**
   Return the tail of a NUL-terminated file pathname. The tail is the
   last component of the path.  For example, the tail of "/a/b/c.d" is
   "c.d". If the name ends in a slash, a pointer to its NUL terminator
   is returned.
*/
FSL_EXPORT const char *fsl_file_tail(const char *z);

/**
   Writes the absolute path name of the current directory to zBuf,
   which must be at least nBuf bytes long (nBuf includes the space
   for a trailing NUL terminator).

   Returns FSL_RC_RANGE if the name would be too long for nBuf,
   FSL_RC_IO if it cannot determine the current directory (e.g. a
   side effect of having removed the directory at runtime or similar
   things), and 0 on success.

   On success, if outLen is not NULL then the length of the string
   written to zBuf is assigned to *outLen. The output string is
   always NUL-terminated.

   On Windows, the name is converted from unicode to UTF8 and all '\\'
   characters are converted to '/'.  No conversions are needed on
   Unix.
*/
FSL_EXPORT int fsl_getcwd(char *zBuf, fsl_size_t nBuf, fsl_size_t * outLen);


/**
   Return true if the filename given is a valid filename
   for a file in a repository. Valid filenames follow all of the
   following rules:

   -  Does not begin with "/"
   -  Does not contain any path element named "." or ".."
   -  Does not contain "/..." (special case)
   -  Does not contain any of these characters in the path: "\"
   -  Does not end with "/".
   -  Does not contain two or more "/" characters in a row.
   -  Contains at least one character

   Invalid UTF8 characters result in a false return if bStrictUtf8 is
   true.  If bStrictUtf8 is false, invalid UTF8 characters are
   silently ignored. See
   https://en.wikipedia.org/wiki/UTF-8#Invalid_byte_sequences and
   https://en.wikipedia.org/wiki/Unicode (for the non-characters).

   Fossil compatibility note: the bStrictUtf8 flag must be true
   when parsing new manifests but is false when parsing legacy
   manifests, for backwards compatibility.

   z must be NUL terminated. Results are undefined if !z.

   Note that periods in and of themselves are valid filename
   components, with the special exceptions of "." and "..", one
   implication being that "...." is, for purposes of this function,
   a valid simple filename.
*/
FSL_EXPORT bool fsl_is_simple_pathname(const char *z, bool bStrictUtf8);

/**
   Return the size of a file in bytes. Returns -1 if the file does
   not exist or is not stat(2)able.
*/
FSL_EXPORT fsl_int_t fsl_file_size(const char *zFilename);

/**
   Return the modification time for a file.  Return -1 if the file
   does not exist or is not stat(2)able.
*/
FSL_EXPORT fsl_time_t fsl_file_mtime(const char *zFilename);

#if 0
/**
   Don't use this. The wd (working directory) family of functions
   might or might-not be necessary and in any case they require
   a fsl_cx context argument because they require repo-specific
   "allow-symlinks" setting.

   Return TRUE if the named file is an ordinary file or symlink
   and symlinks are allowed.

   Return false for directories, devices, fifos, etc.
*/
FSL_EXPORT bool fsl_wd_isfile_or_link(const char *zFilename);
#endif

/**
   Returns true if the named file is an ordinary file. Returns false
   for directories, devices, fifos, symlinks, etc. The name
   may be absolute or relative to the current working dir.
*/
FSL_EXPORT bool fsl_is_file(const char *zFilename);

/**
   Returns true if the given file is a symlink, else false. The name
   may be absolute or relative to the current working dir. On Windows
   platforms this always returns false.
*/
FSL_EXPORT bool fsl_is_symlink(const char *zFilename);

/**
   Returns true if the given filename refers to a plain file or
   symlink, else returns false. The name may be absolute or relative
   to the current working dir.
*/
FSL_EXPORT bool fsl_is_file_or_link(const char *zFilename);

/**
   Returns true if the given path appears to be absolute, else
   false. On Unix a path is absolute if it starts with a '/'.  On
   Windows a path is also absolute if it starts with a letter, a
   colon, and either a backslash or forward slash.
*/
FSL_EXPORT bool fsl_is_absolute_path(const char *zPath);

/**
   Simplify a filename by:

   * converting all \ into / on windows and cygwin
   * removing any trailing and duplicate /
   * removing /./
   * removing /A/../

   Changes are made in-place.  Return the new name length.

   If n is <0 then fsl_strlen() is used to calculate the length.

   If the slash parameter is true, the trailing slash, if any, is
   retained, else any trailing slash is removed.

   As a special case, if the input string (simplified) is "/" then the
   output string will be "/", regardless of the value of the final
   argument. That behavior is debatable but this API is really
   intended to be used for paths deeper than the root directory.
*/
FSL_EXPORT fsl_size_t fsl_file_simplify_name(char *z, fsl_int_t n_, bool slash);

/**
   Return true if string z matches glob pattern zGlob and false if the
   pattern does not match. Always returns false if either argument is
   NULL. Supports all globbing rules supported by sqlite3_strglob().
*/
FSL_EXPORT bool fsl_str_glob(const char *zGlob, const char *z);

/**
   Parses zPatternList as a comma-and/or-fsl_isspace()-delimited
   list of glob patterns (as supported by fsl_str_glob()). Each
   pattern in that list is copied and appended to tgt in the form
   of a new (char *) owned by that list.

   Returns 0 on success, FSL_RC_OOM if copying a pattern to tgt
   fails, FSL_RC_MISUSE if !tgt or !zPatternList. An empty
   zPatternList is not considered an error (to simplify usage) and
   has no side-effects. On allocation error, tgt might be partially
   populated.

   Elements of the glob list may be optionally enclosed in single
   or double-quotes.  This allows a comma to be part of a glob
   pattern.

   Leading and trailing spaces on unquoted glob patterns are
   ignored.

   Note that there is no separate "glob list" class. A "glob list"
   is simply a fsl_list whose list entries are glob-pattern strings
   owned by that list.

   Calling this does not reset tgt's list, so it may be called
   multiple times to prepare a given list.

   Examples of a legal value for zPatternList:

   ```
   "*.c *.h, *.sh, '*.in'"
   ```

   @see fsl_glob_list_append()
   @see fsl_glob_list_matches()
   @see fsl_glob_list_clear()
*/
FSL_EXPORT int fsl_glob_list_parse( fsl_list *  tgt, char const * zPatternList );

/**
   Appends a single blob pattern to tgt, in the form of a new (char *)
   owned by tgt. This function copies zGlob and appends that copy
   to tgt.

   Returns 0 on success, FSL_RC_MISUSE if !tgt or !zGlob or
   !*zGlob, FSL_RC_OOM if appending to the list fails.

   @see fsl_glob_list_parse()
   @see fsl_glob_list_matches()
   @see fsl_glob_list_clear()
*/
FSL_EXPORT int fsl_glob_list_append( fsl_list *  tgt, char const * zGlob );

/**
   Assumes globList is a list of (char [const] *) glob values and
   tries to match each one against zNeedle using
   fsl_str_glob(). If any glob matches, it returns a pointer to the
   matched globList->list entry. If no matches are found, or if any
   argument is invalid, NULL is returned.

   The returned bytes are owned by globList and may be invalidated at
   its leisure. It is primarily intended to be used as a boolean,
   for example:

   ```
   if( fsl_glob_list_matches(myGlobs, someFilename) ) { ... }
   ```

   @see fsl_glob_list_parse()
   @see fsl_glob_list_append()
   @see fsl_glob_list_clear()
*/
FSL_EXPORT char const * fsl_glob_list_matches( fsl_list const * globList,
                                               char const * zNeedle );

/**
   If globList is not NULL this is equivalent to
   fsl_list_visit_free(globList, 1), otherwise it is a no-op.

   Note that this does not free the globList object itself, just
   its underlying list entries and list memory. (In practice, lists
   are either allocated on the stack or as part of a higher-level
   structure, and not on the heap.)

   @see fsl_glob_list_parse()
   @see fsl_glob_list_append()
   @see fsl_glob_list_matches()
*/
FSL_EXPORT void fsl_glob_list_clear( fsl_list *  globList );


/**
   Returns true if the given letter is an ASCII alphabet character.
*/
FSL_EXPORT bool fsl_isalpha(int c);

/**
   Returns true if c is a lower-case ASCII alphabet character.
*/
FSL_EXPORT bool fsl_islower(int c);

/**
   Returns true if c is an upper-case ASCII alphabet character.
*/
FSL_EXPORT bool fsl_isupper(int c);

/**
   Returns true if c is ' ', '\\r' (ASCII 13dec), or '\\t' (ASCII 9
   dec).
*/
FSL_EXPORT bool fsl_isspace(int c);

/**
   Returns true if c is an ASCII digit in the range '0' to '9'.
*/
FSL_EXPORT bool fsl_isdigit(int c);

/**
   Equivalent to fsl_isdigit(c) || fsl_isalpha().
*/
FSL_EXPORT bool fsl_isalnum(int c);

/**
   Returns the upper-case form of c if c is an ASCII alphabet
   letter, else returns c.
*/
FSL_EXPORT int fsl_tolower(int c);

/**
   Returns the lower-case form of c if c is an ASCII alphabet
   letter, else returns c.
*/
FSL_EXPORT int fsl_toupper(int c);

#ifdef _WIN32
/**
   Translate MBCS to UTF-8.  Return a pointer to the translated
   text. ACHTUNG: Call fsl_mbcs_free() (not fsl_free()) to
   deallocate any memory used to store the returned pointer when
   done.
*/
FSL_EXPORT char * fsl_mbcs_to_utf8(char const * mbcs);

/**
   Frees a string allocated from fsl_mbcs_to_utf8(). Results are undefined
   if mbcs was allocated using any other mechanism.
*/
FSL_EXPORT void fsl_mbcs_free(char * mbcs);
#endif
/* _WIN32 */

/**
   Deallocates the given memory, which must have been allocated
   from fsl_unicode_to_utf8(), fsl_utf8_to_unicode(), or any
   function which explicitly documents this function as being the
   proper finalizer for its returned memory.
*/
FSL_EXPORT void fsl_unicode_free(void *);

/**
   Translate UTF-8 to Unicode for use in system calls. Returns a
   pointer to the translated text. The returned value must
   eventually be passed to fsl_unicode_free() to deallocate any
   memory used to store the returned pointer when done.

   This function exists only for Windows. On other platforms
   it behaves like fsl_strdup().

   The returned type is (wchar_t*) on Windows and (char*)
   everywhere else.
*/
FSL_EXPORT void *fsl_utf8_to_unicode(const char *zUtf8);

/**
   Translates Unicode text into UTF-8.  Return a pointer to the
   translated text. Call fsl_unicode_free() to deallocate any
   memory used to store the returned pointer when done.

   This function exists only for Windows. On other platforms it
   behaves like fsl_strdup().
*/
FSL_EXPORT char *fsl_unicode_to_utf8(const void *zUnicode);

/**
   Translate text from the OS's character set into UTF-8. Return a
   pointer to the translated text. Call fsl_os_str_free() to
   deallocate any memory used to store the returned pointer when
   done.

   This function must not convert '\' to '/' on Windows/Cygwin, as
   it is used in places where we are not sure it's really filenames
   we are handling, e.g. fsl_getenv() or handling the argv
   arguments from main().

   On Windows, translate some characters in the in the range
   U+F001 - U+F07F (private use area) to ASCII. Cygwin sometimes
   generates such filenames. See:
   <https://cygwin.com/cygwin-ug-net/using-specialnames.html>
*/
FSL_EXPORT char *fsl_filename_to_utf8(const void *zFilename);

/**
   Attempts to ensure that the name of a file in the filesystem matches
   the same case as one provided by the user. This distinction can be
   significant for users on case-insensitive filesystems.

   `zDir` must be the name of the target directory and `zPath` must be
   a relative path rooted at `zDir`.  If `zPath` does not exist, it is
   assumed to be a _potential_ name and `*zOut` will be set to an
   as-is copy of `zPath`.

   If `isCaseSensitive` is true then this function is essentially a
   dummy stub which returns a copy of `zPath`, made using
   fsl_strdup(), via `*zOut`.

   If isCaseSensitive is false, then it behaves rather differently:

   Uses opendir() on the given directory and scans it for a name
   matching `zPath`, case-insensitvely and recursively. On success,
   the result is a case-correct copy of `zPath`.

   On error, non-zero is returned and `*zOut` is not modified. On
   success, `*zOut` is modified as described above and ownership of it
   is transfered to the caller, who must eventually pass it to
   fsl_free().

   Non-0 error codes include:

   - FSL_RC_OOM on allocation error.
   - FSL_RC_IO if opendir() fails.

   Design note: `isCaseSensitive` is provided as a bool, rather than a
   fsl_cx instance (which has a setting for that flag), so that this
   routine can be used in cases where a fsl_cx is not readily
   available and in cases where this validation is desired on
   case-sensitive filesystems. When a fsl_cx is available,
   fsl_cx_is_case_sensitive() may optionally be used to as the value
   for the first argument. Depending on _how_ filenames are collected,
   however, this case-check may or may not be entirely superfluous.
*/
FSL_EXPORT int fsl_filename_preferred_case(bool isCaseSensitive, const char *zDir,
                                           const char *zPath, char **zOut);

/**
   Translate text from UTF-8 to the OS's filename character set.
   Return a pointer to the translated text. Call
   fsl_os_str_free() to deallocate any memory used to store the
   returned pointer when done.

   On Windows, characters in the range U+0001 to U+0031 and the
   characters '"', '*', ':', '<', '>', '?' and '|' are invalid in
   filenames. Therefore, translate those to characters in the in the
   range U+F001 - U+F07F (private use area), so those characters
   never arrive in any Windows API. The filenames might look
   strange in Windows explorer, but in the cygwin shell everything
   looks as expected.

   See: <https://cygwin.com/cygwin-ug-net/using-specialnames.html>

   The returned type is (wchar_t*) on Windows and (char*)
   everywhere else.
*/
FSL_EXPORT void *fsl_utf8_to_filename(const char *zUtf8);


/**
   Deallocate pOld, which must be NULL or must have been allocated by
   fsl_filename_to_utf8(), fsl_utf8_to_filename(), fsl_getenv(), or
   another routine which explicitly documents this function as being
   the proper finalizer for its returned memory.
*/
FSL_EXPORT void fsl_os_str_free(void *pOld);

/**
   Returns a (possible) copy of the environment variable with the
   given key, or NULL if no entry is found. The returned value must
   be passed to fsl_os_str_free() to free it. ACHTUNG: DO NOT
   MODIFY the returned value - on Unix systems it is _not_ a
   copy. That interal API inconsistency "should" be resolved
   (==return a copy from here, but that means doing it everywhere)
   to avoid memory ownership problems later on.

   Why return a copy? Because native strings from at least one of
   the more widespread OSes often have to be converted to something
   portable and this requires allocation on such platforms, but
   not on Unix. For API transparency, that means all platforms get
   the copy(-like) behaviour.
*/
FSL_EXPORT char *fsl_getenv(const char *zName);

/**
   Collects a list of directories intended to use as temp dirs in the
   current environment. The returned array ends with a NULL element to
   mark its end.  The memory's ownership is a bit awkward and
   therefore it must be eventually freed by passing it to
   fsl_temp_dirs_free().

   No entries in the returned list contain a trailing slash unless the
   entry is itself a reference to a Unix-style root directory (which
   would be highly unusual).

   The list of directories varies by platform:

   Windows:

   - `GetTempPath()`, `$TEMP`, `$TMP`, "."

   Non-Windows:

   - `$TMPDIR`, "/var/tmp", "/usr/tmp", "/tmp", "/temp",
   "."

   (`$X` refers to the value of environment variable `X`.)

   Noting that only directories in those lists which actually exist
   (at the time this is called) are added to the list (also noting
   that "." always exists unless CWD is deleted while the app is
   active). If no suitable directories are found, an empty array (with
   a leading `NULL` element) is returned, but this would indicate
   that, e.g., even CWD does not exist (so the app has bigger
   problems).

   `NULL` is only returned if allocation of the containing array
   fails. Failure to allocate memory for a given directory name (as
   those require conversion on some platforms) is ignored and such
   entries are simply skipped over.

   @see fsl_temp_dirs_free()
   @see fsl_file_tempname()
*/
FSL_EXPORT char ** fsl_temp_dirs_get(void);

/**
   If aDirs is not NULL then it is expected to be an array created by
   fsl_temp_dirs_get() and it frees the list's entries and the list
   itself. After calling this, the argument's memory is invalidated
   and any use of it leads to undefined results.

   This is a harmless no-op if the argument is NULL.

   @see fsl_temp_dirs_get()
   @see fsl_file_tempname()
*/
FSL_EXPORT void fsl_temp_dirs_free(char **aDirs);

/**
   Creates a semi-random temp filename and appends it to the given
   buffer. The second argument is an optional string to prefix
   each filename with. If it is NULL then a library-wide default is
   used. If it is empty then no prefix is used. The final argument is
   expected to be NULL or a list of directories in the format
   constructed by fsl_temp_dirs_get(). If it is not NULL, this
   function uses the first entry in dirs which refers to an existing
   directory. If it is NULL then the buffer is filled with the new
   name with no directory prefix.

   Returns...

   - 0 on success, in which case tgt is populated with the new name.

   - If tgt->errCode is not 0 when this is called, that is returned
     and this function has no further side effects.

   - FSL_RC_NOT_FOUND if dirs is empty or refers only to non-existent
     directories.

   - FSL_RC_OOM if allocating memory for the target buffer fails.

   - FSL_RC_RANGE if, after some hard-coded number of attempts, it is
     unable to construct a unique filename due to name collisions in
     the target directory. That "shouldn't ever happen."

   Sidebars:

   - This function does no translation or validation of the 2nd
     argument other than to check if it is NULL. It "should" be either
     a file base name, with no path component, or a relative path
     component for which the caller has already created the directory.
     e.g. use the base name of the application, e.g. "my-app".

   - This function does not actually write to the filesystem, it just
     constructs a name. There is hypothetically a window of
     opportunity for another file with the same name to be created
     before the caller has a chance to create the file, but the
     chances of that actually happening are close enough to zero to
     rule them out for all practical purposes.

   - The RNG is officially unspecified. (That said:
     sqlite3_randomness().)

   @see fsl_temp_dirs_get()
   @see fsl_temp_dirs_free()
*/
FSL_EXPORT int fsl_file_tempname(fsl_buffer *  tgt, char const *zPrefix,
                                 char * const * dirs);

/**
   Returns a positive value if zFilename is a directory, 0 if
   zFilename does not exist, or a negative value if zFilename
   exists but is something other than a directory. Results are
   undefined if zFilename is NULL.

   This function expects zFilename to be a well-formed path -
   it performs no normalization on it.
*/
FSL_EXPORT int fsl_dir_check(const char *zFilename);

/**
   Check the given path to determine whether it is an empty directory.
   Returns 0 on success (i.e., directory exists and is empty), <0 if
   the provided path is not a directory or cannot be opened, and >0 if
   the directory is not empty.
*/
FSL_EXPORT int fsl_dir_is_empty(const char *path);

/**
   Deletes the given file from the filesystem. Returns 0 on
   success. If the component is a directory, this operation will
   fail. If zFilename refers to a symlink, the link (not its target)
   is removed.

   Results are undefined if zFilename is NULL.

   Potential TODO: if it refers to a dir, forward the call to
   fsl_rmdir().
*/
FSL_EXPORT int fsl_file_unlink(const char *zFilename);

/**
   Renames file zFrom to zTo using the OS's equivalent of
   `rename(2)`. Both files must be in the same filesystem and any
   directory parts of zTo must already exist. Returns 0 on success,
   FSL_RC_OOM if a filename conversion allocation fails (on platforms
   which do that), or a FSL_RC_xxx counterpart of an `errno` value if
   the `rename(2)` call fails (as per fsl_errno_to_rc()).
*/
FSL_EXPORT int fsl_file_rename(const char *zFrom, const char *zTo);

/**
   Deletes an empty directory from the filesystem. Returns 0
   on success. There are any number of reasons why deletion
   of a directory can fail, some of which include:

   - It is not empty or permission denied (FSL_RC_ACCESS).

   - Not found (FSL_RC_NOT_FOUND).

   - Is not a directory or (on Windows) is a weird pseudo-dir type for
     which rmdir() does not work (FSL_RC_TYPE).

   - I/O error (FSL_RC_IO).

   @see fsl_dir_is_empty()
*/
FSL_EXPORT int fsl_rmdir(const char *zFilename);

/**
   Sets the mtime (Unix epoch) for a file. Returns 0 on success,
   non-0 on error. If newMTime is less than 0 then the current
   time(2) is used. This routine does not create non-existent files
   (e.g. like a Unix "touch" command).
*/
FSL_EXPORT int fsl_file_mtime_set(const char *zFilename, fsl_time_t newMTime);

/**
   On non-Windows platforms, this function sets or unsets the
   executable bits on the given filename. All other permissions are
   retained as-is. Returns 0 on success. On Windows this is a no-op,
   returning 0.

   If the target is a directory or a symlink, this is a no-op and
   returns 0.
*/
FSL_EXPORT int fsl_file_exec_set(const char *zFilename, bool isExec);

/**
   Returns true if the argument is fsl_stat()'able and has the
   executable bit set, else false.
*/
FSL_EXPORT bool fsl_file_isexec(const char *zFilename);

/**
   Makes a bitwise copy of the file zFrom to file zTo, creating any
   directories needed for the target file's if they do not already
   exist. If the source file is executable then the target file will
   also be made so, but any failure to do so (e.g. because the target
   filesystem does not support it) is silently ignored.

   Returns 0 on success. On error it may return any number of result
   codes if opening a file fails, if a mkdir fails (see
   fsl_mkdir_for_file()), or if any I/O fails.

   Results are undefined if the source and target refer to the
   same file.
*/
FSL_EXPORT int fsl_file_copy(char const *zFrom, char const *zTo);

/**
   Create the directory with the given name if it does not already
   exist. If forceFlag is true, delete any prior non-directory
   object with the same name.

   Return 0 on success, non-0 on error.

   If the directory already exists then 0 is returned, not an error
   (FSL_RC_ALREADY_EXISTS), because that simplifies usage. If
   another filesystem entry with this name exists and forceFlag is
   true then that entry is deleted before creating the directory,
   and this operation fails if deletion fails. If forceFlag is
   false and a non-directory entry already exists, FSL_RC_TYPE is
   returned.

   For recursively creating directories, use fsl_mkdir_for_file().

   Bug/corner case: if zFilename refers to a symlink to a
   non-existent directory, this function gets slightly confused,
   tries to make a dir with the symlink's name, and returns
   FSL_RC_ALREADY_EXISTS. How best to resolve that is not yet
   clear. The problem is that stat(2)ing the symlink says "nothing
   is there" (because the link points to a non-existing thing), so
   we move on to the underlying mkdir(), which then fails because
   the link exists with that name.
*/
FSL_EXPORT int fsl_mkdir(const char *zName, bool forceFlag);

/**
   A convenience form of fsl_mkdir() which can recursively create
   directories. If zName has a trailing slash then the last
   component is assumed to be a directory part, otherwise it is
   assumed to be a file part (and no directory is created for that
   part). zName may be either an absolute or relative path.

   Returns 0 on success (including if all directories already exist).
   Returns FSL_RC_OOM if there is an allocation error. Returns
   FSL_RC_TYPE if one of the path components already exists and is not
   a directory. Returns FSL_RC_RANGE if zName is NULL or empty. If
   zName is only 1 byte long, this is a no-op.

   On systems which support symlinks, a link to a directory is
   considered to be a directory for purposes of this function.

   If forceFlag is true and a non-directory component is found in
   the filesystem where zName calls for a directory, that component
   is removed (and this function fails if removal fails).

   Examples:

   "/foo/bar" creates (if needed) /foo, but assumes "bar" is a file
   component. "/foo/bar/" creates /foo/bar. However "foo" will not
   create a directory - because the string has no path component,
   it is assumed to be a filename.

   Both "/foo/bar/my.doc" and "/foo/bar/" result in the directories
   "/foo/bar".
*/
FSL_EXPORT int fsl_mkdir_for_file(char const *zName, bool forceFlag);

/**
   This function expects its second argument to refer to a symlink
   (and that the caller has already validated that it is one). The
   `readlink(2)` system call is used to fetch the contents of that
   link and the target buffer is populated with those contents
   (reusing any memory the buffer may already own).

   Returns 0 on success, FSL_RC_OOM if copying to the target buffer
   fails due to an allocation error, or an error propagated from
   `readlink()` in the form of a fsl_errno_to_rc() result.

   Caveat: on Windows platforms this function simply passes the buffer
   to fsl_buffer_reuse() and returns 0. It should arguably read the
   contents of zFilename into the buffer in that case, on the
   assumption that the file is a pseudo-symlink. That decision may be
   re-evaluated if/when the library claims to have fossil-compatible
   symlink support.

   Bugs:

   - This function has a limited internal buffer as a target for
     `readlink()`. It "should be" long enough for any "reasonable"
     uses in the scope of this library, but there's bound to be some
     user out there who wants to use it for other contexts.
*/
FSL_EXPORT int fsl_symlink_read(fsl_buffer *  tgt, char const * zFilename);

/**
   Creates a symlink or pseudo-symlink named zLinkFile linking to
   zTargetFile. If realLink is true and this is a non-Windows platform
   then:

   - fsl_file_simplify_name() is used to normalize zLinkFile.

   - `symlink(2)` is used to link zTargetFile to a new link named the
      simplified form of zLinkFile. If zLinkFile already exists, this
      will fail.

   If realLink is false or this is a Windows platform, a file is
   created named zLinkFile containing the string zTargetFile as its
   contents. If a file or symlink named zLinkFile already exists, it
   is removed before writing the new contents.

   In both cases the parent directories for zLinkFile are created, if
   needed, but that process will fail with FSL_RC_TYPE if any
   non-directory components with conflicting names are found in the
   to-be-mkdir'd path.

   Returns 0 on success or some lower-level result code if
   creation/writing of a directory, a symlink, or pseudo-symlink
   fails.
*/
FSL_EXPORT int fsl_symlink_create(char const *zTargetFile, char const * zLinkFile,
                                  bool realLink);

/**
   Reads symlink zFrom, as per fsl_symlink_read(), then creates a copy
   named zTo, as per fsl_symlink_create(). The first argument for the
   latter call is the contents of the result of fsl_symlink_read().
   The 2nd and 3rd arguments to fsl_symlink_create() are the 2nd and
   3rd arguments to this function.

   Returns 0 on success, FSL_RC_OOM on OOM, or any of various
   filesystem-related non-0 codes if reading or saving the link fails.
*/
FSL_EXPORT int fsl_symlink_copy(char const *zFrom, char const *zTo, bool realLink);

/**
   Uses fsl_getenv() to look for the environment variables
   (FOSSIL_USER, (Windows: USERNAME), (Unix: USER, LOGNAME)). If
   it finds one it returns a copy of that value, which must
   eventually be passed to fsl_free() to free it (NOT
   fsl_os_str_free(), though fsl_getenv() requires that one). If
   it finds no match, or if copying the entry fails, it returns
   NULL.

   @see fsl_cx_user_set()
   @see fsl_cx_user_get()
*/
FSL_EXPORT char * fsl_user_name_guess(void);

/**
   Tries to find the user's home directory. If found, 0 is
   returned, tgt's memory is _overwritten_ (not appended) with the
   path, and tgt->used is set to the path's string length.  (Design
   note: the overwrite behaviour is inconsistent with most of the
   API, but the implementation currently requires this.)

   If requireWriteAccess is true then the directory is checked for
   write access, and FSL_RC_ACCESS is returned if that check
   fails. For historical (possibly techinical?) reasons, this check
   is only performed on Unix platforms. On others this argument is
   ignored. When writing code on Windows, it may be necessary to
   assume that write access is necessary on non-Windows platform,
   and to pass 1 for the second argument even though it is ignored
   on Windows.

   On error non-0 is returned and tgt is updated with an error
   string OR (if the error was an allocation error while appending
   to the path or allocating MBCS strings for Windows), it returns
   FSL_RC_OOM and tgt "might" be updated with a partial path (up to
   the allocation error), and "might" be empty (if the allocation
   error happens early on).

   This routine does not canonicalize/transform the home directory
   path provided by the environment, other than to convert the
   string byte encoding on some platforms. i.e. if the environment
   says that the home directory is "../" then this function will
   return that value, possibly to the eventual disappointment of
   the caller.

   Result codes include:

   - FSL_RC_OK (0) means a home directory was found and tgt is
   populated with its path.

   - FSL_RC_NOT_FOUND means the home directory (platform-specific)
   could not be found.

   - FSL_RC_ACCESS if the home directory is not writable and
   requireWriteAccess is true. Unix platforms only -
   requireWriteAccess is ignored on others.

   - FSL_RC_TYPE if the home (as determined via inspection of the
   environment) is not a directory.

   - FSL_RC_OOM if a memory (re)allocation fails.
*/
FSL_EXPORT int fsl_find_home_dir( fsl_buffer *  tgt, bool requireWriteAccess );

/**
   Values for use with the fsl_fstat::type field.
*/
enum fsl_fstat_type_e {
/** Sentinel value for unknown/invalid filesystem entry types. */
FSL_FSTAT_TYPE_UNKNOWN = 0,
/** Indicates a directory filesystem entry. */
FSL_FSTAT_TYPE_DIR,
/** Indicates a non-directory, non-symlink filesystem entry.
    Because fossil's scope is limited to SCM work, it assumes that
    "special files" (sockets, etc.) are just files, and makes no
    special effort to handle them.
*/
FSL_FSTAT_TYPE_FILE,
/** Indicates a symlink filesystem entry. */
FSL_FSTAT_TYPE_LINK
};
typedef enum fsl_fstat_type_e fsl_fstat_type_e;

/**
   Bitmask values for use with the fsl_fstat::perms field.

   Only permissions which are relevant for fossil are listed here.
   e.g. read-vs-write modes are irrelevant for fossil as it does not
   track them. It manages only the is-executable bit. In in the
   contexts of fossil manifests, it also treats "is a symlink" as a
   permission flag.
*/
enum fsl_fstat_perm_e {
/**
   Sentinel value.
*/
FSL_FSTAT_PERM_UNKNOWN = 0,
/**
   The executable bit, as understood by Fossil. Fossil does not
   differentiate between different +x values for user/group/other.
*/
FSL_FSTAT_PERM_EXE = 0x01
};
typedef enum fsl_fstat_perm_e fsl_fstat_perm_e;

/**
   A simple wrapper around the stat(2) structure resp. _stat/_wstat
   (on Windows). It exposes only the aspects of stat(2) info which
   Fossil works with, and not any platform-/filesystem-specific
   details except the executable bit for the permissions mode and some
   handling of symlinks.
*/
struct fsl_fstat {
  /**
     Indicates the type of filesystem object.
  */
  fsl_fstat_type_e type;
  /**
     The time of the last file metadata change (owner, permissions,
     etc.). The man pages (neither for Linux nor Windows) do not
     specify exactly what unit this is. Let's assume seconds since the
     start of the Unix Epoch.
  */
  fsl_time_t ctime;
  /**
     Last modification time.
  */
  fsl_time_t mtime;
  /**
     The size of the stat'd file, in bytes.
  */
  fsl_size_t size;
  /**
     Contains the filesystem entry's permissions as a bitmask of
     fsl_fstat_perm_e values. Note that only the executable bit for
     _files_ (not directories) is exposed here.
  */
  int perm;
};

/** Empty-initialized fsl_fstat structure, intended for const-copy
    construction. */
#define fsl_fstat_empty_m {FSL_FSTAT_TYPE_UNKNOWN,0,0,-1,0}

/** Empty-initialized fsl_fstat instance, intended for non-const copy
    construction. */
FSL_EXPORT const fsl_fstat fsl_fstat_empty;

/**
   Runs the OS's stat(2) equivalent to populate fst (if not NULL) with
   information about the given file.

   The second argument may be NULL.

   Returns 0 on success. Results are undefined if zFilename is NULL or
   invalid. Returns FSL_RC_NOT_FOUND if no filesystem entry is found
   for the given name. Returns FSL_RC_IO if the underlying stat() (or
   equivalent) fails for undetermined reasons inside the underlying
   stat()/_wstati64() call. Note that the fst parameter may be NULL,
   in which case the return value will be 0 if the name is stat-able,
   but will return no other information about it.

   The derefSymlinks argument is ignored on non-Unix platforms.  On
   Unix platforms, if derefSymlinks is true then stat(2) is used, else
   lstat(2) (if available on the platform) is used. For most cases
   clients should pass true. They should only pass false if they need
   to differentiate between symlinks and files.

   The fsl_fstat_type_e family of flags can be used to determine the
   type of the filesystem object being stat()'d (file, directory, or
   symlink). It does not apply any special logic for platform-specific
   oddities other than symlinks (e.g. character devices and such).
*/
FSL_EXPORT int fsl_stat(const char *zFilename, fsl_fstat *  fst,
                        bool derefSymlinks);

/**
   Create a new fossil-format delta between the memory zIn and zOut.

   The delta is written into a preallocated buffer, zDelta, which
   must be at least 60 bytes longer than the target memory, zOut.
   The delta string will be NUL-terminated, but it might also
   contain embedded NUL characters if either the zSrc or zOut files
   are binary.

   On success this function returns 0 and the length of the delta
   string, in bytes, excluding the final NUL terminator character,
   is written to *deltaSize.

   Returns FSL_RC_OOM if memory allocation fails during generation of
   the delta. Returns FSL_RC_RANGE if lenSrc or lenOut are "too
   big" (if they cause an overflow in the math).

   Results are undefined if any pointer is NULL.

   Output Format:

   The delta begins with a base64 number followed by a newline.
   This number is the number of bytes in the TARGET file.  Thus,
   given a delta file z, a program can compute the size of the
   output file simply by reading the first line and decoding the
   base-64 number found there.  The fsl_delta_applied_size()
   routine does exactly this.

   After the initial size number, the delta consists of a series of
   literal text segments and commands to copy from the SOURCE file.
   A copy command looks like this:

   (Achtung: extra backslashes are for Doxygen's benefit - not
   visible in the processsed docs.)

   NNN\@MMM,

   where NNN is the number of bytes to be copied and MMM is the
   offset into the source file of the first byte (both base-64).
   If NNN is 0 it means copy the rest of the input file.  Literal
   text is like this:

   NNN:TTTTT

   where NNN is the number of bytes of text (base-64) and TTTTT is
   the text.

   The last term is of the form

   NNN;

   In this case, NNN is a 32-bit bigendian checksum of the output
   file that can be used to verify that the delta applied
   correctly.  All numbers are in base-64.

   Pure text files generate a pure text delta.  Binary files
   generate a delta that may contain some binary data.

   Algorithm:

   The encoder first builds a hash table to help it find matching
   patterns in the source file.  16-byte chunks of the source file
   sampled at evenly spaced intervals are used to populate the hash
   table.

   Next we begin scanning the target file using a sliding 16-byte
   window.  The hash of the 16-byte window in the target is used to
   search for a matching section in the source file.  When a match
   is found, a copy command is added to the delta.  An effort is
   made to extend the matching section to regions that come before
   and after the 16-byte hash window.  A copy command is only
   issued if the result would use less space that just quoting the
   text literally. Literal text is added to the delta for sections
   that do not match or which can not be encoded efficiently using
   copy commands.

   @see fsl_delta_applied_size()
   @see fsl_delta_apply()
*/
FSL_EXPORT int fsl_delta_create( unsigned char const *zSrc, fsl_size_t lenSrc,
                      unsigned char const *zOut, fsl_size_t lenOut,
                      unsigned char *zDelta, fsl_size_t * deltaSize);

/**
   Works identically to fsl_delta_create() but sends its output to
   the given output function. out(outState,...) may be called any
   number of times to emit delta output. Each time it is called it
   should append the new bytes to its output channel.

   The semantics of the return value and the first four arguments
   are identical to fsl_delta_create(), with these ammendments
   regarding the return value:

   - Returns FSL_RC_MISUSE if any of (zV1, zV2, out) are NULL.

   - If out() returns non-0 at any time, delta generation is
     aborted and that code is returned.

   Example usage:

   ```
   int rc = fsl_delta_create( v1, v1len, v2, v2len,
                              fsl_output_f_FILE, stdout);
   ```
*/
FSL_EXPORT int fsl_delta_create2( unsigned char const *zV1, fsl_size_t lenV1,
                                  unsigned char const *zV2, fsl_size_t lenV2,
                                  fsl_output_f out, void * outState);

/**
   A fsl_delta_create() wrapper which uses the first two arguments
   as the original and "new" content versions to delta, and outputs
   the delta to the 3rd argument (overwriting any existing contents
   and re-using any memory it had allocated).

   If any of the buffers have the same address, FSL_RC_MISUSE is
   returned. If fsl_buffer_err() is true for the 3rd argument, that
   value is returned without side effects. If allocation of memory for
   the delta fails, FSL_RC_OOM is returned and delta's errCode is
   updated with that value.

   Results are undefined if any argument is NULL.

   Returns 0 on success.
*/
FSL_EXPORT int fsl_buffer_delta_create( fsl_buffer const * src,
                                        fsl_buffer const * newVers,
                                        fsl_buffer *  delta);

/**
   Apply a delta created using fsl_delta_create().

   The output buffer must be big enough to hold the whole output
   file and a NUL terminator at the end. The
   fsl_delta_applied_size() routine can be used to determine that
   size.

   zSrc represents the original sources to apply the delta to.
   It must be at least lenSrc bytes of valid memory.

   zDelta holds the delta (created using fsl_delta_create()),
   and it must be lenDelta bytes long.

   On success this function returns 0 and writes the applied delta
   to zOut.

   Returns FSL_RC_MISUSE if any pointer argument is NULL. Returns
   FSL_RC_RANGE if lenSrc or lenDelta are "too big" (if they cause
   an overflow in the math). Invalid delta input can cause any of
   FSL_RC_RANGE, FSL_RC_DELTA_INVALID_TERMINATOR,
   FSL_RC_CHECKSUM_MISMATCH, FSL_RC_SIZE_MISMATCH, or
   FSL_RC_DELTA_INVALID_OPERATOR to be returned.

   Refer to the fsl_delta_create() documentation above for a
   description of the delta file format.

   @see fsl_delta_applied_size()
   @see fsl_delta_create()
   @see fsl_delta_apply2()
*/
FSL_EXPORT int fsl_delta_apply( unsigned char const *zSrc, fsl_size_t lenSrc,
                     unsigned char const *zDelta, fsl_size_t lenDelta,
                     unsigned char *zOut );

/**
   Functionally identical to fsl_delta_apply() but any errors generated
   during application of the delta are described in more detail
   in pErr. If pErr is NULL this behaves exactly as documented for
   fsl_delta_apply().
*/
FSL_EXPORT int fsl_delta_apply2( unsigned char const *zSrc,
                                 fsl_size_t lenSrc,
                                 unsigned char const *zDelta,
                                 fsl_size_t lenDelta,
                                 unsigned char *zOut,
                                 fsl_error * pErr);
/**
  Calculates the size (in bytes) of the output from applying a the
  given delta. On success 0 is returned and *appliedSize will be
  updated with the amount of memory required for applying the
  delta. zDelta must point to lenDelta bytes of memory in the
  format emitted by fsl_delta_create(). It is legal for appliedSize
  to point to the same memory as the 2nd argument.

  Returns FSL_RC_RANGE if lenDelta is too short to be a delta. Returns
  FSL_RC_DELTA_INVALID_TERMINATOR if the delta's encoded length is not
  properly terminated.

  Results are undefined if any pointer argument is NULL.

  This routine is provided so that an procedure that is able to call
  fsl_delta_apply() can learn how much space is required for the
  output and hence allocate no more space that is really needed.

  TODO?: consolidate 2nd and 3rd parameters into one i/o parameter?

  @see fsl_delta_apply()
  @see fsl_delta_create()
*/
FSL_EXPORT int fsl_delta_applied_size(unsigned char const *zDelta,
                                      fsl_size_t lenDelta,
                                      fsl_size_t * appliedSize);

/**
   "Fossilizes" the first len bytes of the given input string. If
   (len<0) then fsl_strlen(inp) is used to calculate its length.
   The output is appended to out, which is expanded as needed and
   out->used is updated accordingly.  Returns 0 on success,
   FSL_RC_MISUSE if !inp or !out. Returns 0 without side-effects if
   0==len or (!*inp && len<0). Returns FSL_RC_OOM if reservation of
   the output buffer fails (it is expanded, at most, one time by
   this function).

   Fossilization replaces the following bytes/sequences with the
   listed replacements:

   (Achtung: usage of doubled backslashes here it just to please
   doxygen - they will show up as single slashes in the processed
   output.)

   - Backslashes are doubled.

   - (\\n, \\r, \\v, \\t, \\f) are replaced with \\\\X, where X is the
   conventional encoding letter for that escape sequence.

   - Spaces are replaced with \\s.

   - Embedded NULs are replaced by \\0 (numeric 0, not character
   '0').
*/
FSL_EXPORT int fsl_bytes_fossilize( unsigned char const * inp, fsl_int_t len,
                         fsl_buffer * out );
/**
   "Defossilizes" bytes encoded by fsl_bytes_fossilize() in-place.
   inp must be a string encoded by fsl_bytes_fossilize(), and the
   decoding processes stops at the first unescaped NUL terminator.
   It has no error conditions except for !inp or if inp is not
   NUL-terminated, both of which invoke in undefined behaviour.

   If resultLen is not NULL then *resultLen is set to the resulting string
   length.

*/
FSL_EXPORT void fsl_bytes_defossilize( unsigned char * inp, fsl_size_t * resultLen );

/**
   Defossilizes the contents of b. Equivalent to:
   fsl_bytes_defossilize( b->mem, &b->used );
*/
FSL_EXPORT void fsl_buffer_defossilize( fsl_buffer *  b );

/**
   Returns true if the input string contains only valid lower-case
   base-16 digits. If any invalid characters appear in the string,
   false is returned.
*/
FSL_EXPORT bool fsl_validate16(const char *zIn, fsl_size_t nIn);

/**
   The input string is a base16 value.  Convert it into its canonical
   form.  This means that digits are all lower case and that conversions
   like "l"->"1" and "O"->"0" occur.
*/
FSL_EXPORT void fsl_canonical16(char *z, fsl_size_t n);

/**
   Decode a N-character base-16 number into base-256.  N must be a
   multiple of 2. The output buffer must be at least N/2 characters
   in length. Returns 0 on success.
*/
FSL_EXPORT int fsl_decode16(const unsigned char *zIn, unsigned char *pOut, fsl_size_t N);

/**
   Encode a N-digit base-256 in base-16. N is the byte length of pIn
   and zOut must be at least (N*2+1) bytes long (the extra is for a
   terminating NUL). Returns zero on success, FSL_RC_MISUSE if !pIn
   or !zOut.
*/
FSL_EXPORT int fsl_encode16(const unsigned char *pIn, unsigned char *zOut, fsl_size_t N);


/**
   Tries to convert the value of errNo, which is assumed to come
   from the global errno, to a fsl_rc_e code. If it can, it returns
   something approximating the errno value, else it returns dflt.

   Example usage:

   ```
   FILE * f = fsl_fopen("...", "...");
   int rc = f ? 0 : fsl_errno_to_rc(errno, FSL_RC_IO);
   ...
   ```

   Why require the caller to pass in errno, instead of accessing it
   directly from this function? To avoid the the off-chance that
   something changes errno between the call and the conversion
   (whether or not that's possible is as yet undetermined). It can
   also be used by clients to map to explicit errno values to
   fsl_rc_e values, e.g. fsl_errno_to_rc(EROFS,-1) returns
   FSL_RC_ACCESS.

   A list of the errno-to-fossil conversions:

   - 0: FSL_RC_OK (0)

   - EINVAL: FSL_RC_MISUSE (could arguably be FSL_RC_RANGE, though)

   - ENOMEM: FSL_RC_OOM

   - EACCES, EBUSY, EPERM, EROFS, EDQUOT, EAGAIN: FSL_RC_ACCESS

   - EISDIR, ENOTDIR: FSL_RC_TYPE

   - ENAMETOOLONG, ELOOP, ERANGE: FSL_RC_RANGE

   - ENOENT: FSL_RC_NOT_FOUND

   - EEXIST: FSL_RC_ALREADY_EXISTS

   - EIO: FSL_RC_IO

   Any other value for errNo causes dflt to be returned.
*/
FSL_EXPORT int fsl_errno_to_rc(int errNo, int dflt);

/**
   Make the given string safe for HTML by converting every "<" into
   "&lt;", every ">" into "&gt;", every "&" into "&amp;", and
   encode '"' as &quot; so that it can appear as an argument to
   markup.

   The escaped output is send to out(oState,...).

   Returns 0 on success or if there is nothing to do (input has a
   length of 0, in which case out() is not called). Returns
   FSL_RC_MISUSE if !out or !zIn. If out() returns a non-0 code
   then that value is returned to the caller.

   If n is negative, fsl_strlen() is used to calculate zIn's length.
*/
FSL_EXPORT int fsl_htmlize(fsl_output_f out, void * oState,
                const char *zIn, fsl_int_t n);

/**
   Functionally equivalent to fsl_htmlize() but optimized to perform
   only a single allocation.

   Returns 0 on success or if there is nothing to do (input has a
   length of 0). Returns FSL_RC_MISUSE if !p or !zIn, and
   FSL_RC_OOM on allocation error.

   If n is negative, fsl_strlen() is used to calculate zIn's length.
*/
FSL_EXPORT int fsl_htmlize_to_buffer(fsl_buffer *p, const char *zIn, fsl_int_t n);

/**
   Equivalent to fsl_htmlize_to_buffer() but returns the result as a
   new string which must eventually be fsl_free()d by the caller.

   Returns NULL for invalid arguments or allocation error.
*/
FSL_EXPORT char *fsl_htmlize_str(const char *zIn, fsl_int_t n);

/**
   If c is a character Fossil likes to HTML-escape, assigns *xlate
   to its transformed form, else set it to NULL. Returns 1 for
   untransformed characters and the strlen of *xlate for others.
   Bytes returned via xlate are static and immutable.

   Results are undefined if xlate is NULL.
*/
FSL_EXPORT fsl_size_t fsl_htmlize_xlate(int c, char const ** xlate);


/** @enum fsl_diff2_flag_e

   Flags for use with the 2021-era text-diff generation APIs
   (fsl_dibu and friends). This set of flags may still change
   considerably.

   Maintenance reminders:

   - Some of these values are holy and must not be changed without
     also changing the corresponding code in diff2.c.

   - Where these entries semantically overlap with their
     older/deprecated fsl_diff_flag_e counterparts, they MUST (for the
     sake of avoiding client-side Grief) have the same values because
     some internal APIs are used by both of the diff APIs.

  @see fsl_dibu_impl_flags_e

   TODO?:

   - Move the diff-builder-specific flags from here to, possibly, the
     fsl_dibu::implFlags member or a new flag member dedicated to this
     type of flag.
*/
enum fsl_diff2_flag_e {
/** Ignore end-of-line whitespace. Applies to all diff builders. */
FSL_DIFF2_IGNORE_EOLWS = 0x01,
/** Ignore all whitespace. Applies to all diff builders.  */
FSL_DIFF2_IGNORE_ALLWS = 0x03,
/** Suppress optimizations (debug). Applies to
    all diff builders. */
FSL_DIFF2_NOOPT =        0x0100,
/** Invert the diff. Applies to all diff builders. */
FSL_DIFF2_INVERT =       0x0200,
/** Use context line count even if it's zero. Applies to all diff
    builders. Normally a value of 0 is treated as the built-in
    default. */
FSL_DIFF2_CONTEXT_ZERO =  0x0400,
/**
   Only calculate diff if it's not "too big." Applies to all diff
   builders and will cause the public APIs which hit this to return
   FSL_RC_RANGE.
*/
FSL_DIFF2_NOTTOOBIG =    0x0800,
/**
   Strip trailing CR before diffing. Applies to all diff builders.
*/
FSL_DIFF2_STRIP_EOLCR =    0x1000,
/*
  ACHTUNG: do not use 0x2000 because it would semantically collide
  with FSL_DIFF_ANSI_COLOR.
*/
/**
   More precise but slower side-by-side diff algorithm, for diffs
   which use that.
*/
FSL_DIFF2_SLOW_SBS =       0x4000,
/**
   Tells diff builders which support it to include line numbers in
   their output.
*/
FSL_DIFF2_LINE_NUMBERS = 0x10000,
/**
   Tells diff builders which optionally support an "index" line
   to NOT include it in their output.
*/
FSL_DIFF2_NOINDEX = 0x20000,

/**
   Reserved for client-defined diff builder use.
*/
FSL_DIFF2_CLIENT1 = 0x01000000,
/**
   Reserved for client-defined diff builder use.
*/
FSL_DIFF2_CLIENT2 = 0x02000000,
/**
   Reserved for client-defined diff builder use.
*/
FSL_DIFF2_CLIENT3 = 0x04000000,
/**
   Reserved for client-defined diff builder use.
*/
FSL_DIFF2_CLIENT4 = 0x08000000
};

/**
   Flags for use with various concrete fsl_dibu implementations.
   Specifically, these are flags for fsl_dibu::implFlags (as opposed
   to fsl_dibu::pimplFlags, which must not be modified by clients).

   Note that it is perfectly legitimate for different entries to have
   the same values, but different semantics, so long as the different
   entries are specific to different fsl_dibu types.
*/
enum fsl_dibu_impl_flags_e {
/**
   Tells the TCL diff builder that the complete output and each line
   should be wrapped in {...}.
*/
FSL_DIBU_TCL_BRACES = 0x01,
/**
   Tells the TCL diff builder that the squiggly braces embedded in any
   output (as opposed to the braces added by FSL_DIBU_TCL_BRACES)
   need to be backslash-escaped. Whether this is required depends on
   how the output will be used.
*/
FSL_DIBU_TCL_BRACES_ESC = 0x02,
/**
   Tells the TCL diff builder to generate a complete TCL/TK app as output.
   The resulting script can (if tcl and tk are installed) be run with
   tclsh to get a graphical diff.

   Note that this flag includes both FSL_DIBU_TCL_BRACES and
   FSL_DIBU_TCL_BRACES_ESC.
*/
FSL_DIBU_TCL_TK = 0x07

};

/**
   An instance of this class is used to convey certain state to
   fsl_dibu objects. Some of this state is configuration
   provided by the client and some is volatile, used for communicating
   common state to diff builder instances during the diff rendering
   process.

   Certain fsl_dibu implementations may require that some
   ostensibly optional fields be filled out. Documenting that is TODO,
   as the builders get developed.
*/
struct fsl_dibu_opt {
  /**
     Flags from the fsl_diff2_flag_e enum.
  */
  uint32_t diffFlags;
  /**
     Number of lines of diff context (number of lines common to the
     LHS and RHS of the diff). Library-wide default is 5.
  */
  unsigned short contextLines;
  /**
     Maximum column width hint for side-by-side, a.k.a. split, diffs.

     FSL_DIBU_SPLIT_TEXT truncates its content columns (as
     opposed to line numbers and its modification marker) to, at most,
     this width. By default it uses as much width as is necessary to
     render whole lines. It treats this limit as UTF8 characters, not
     bytes.

     This is a hint, not a rule. A given diff builder is free to
     ignore it or to ignore values which are arbitrarily deemed "too
     small" or "too large."
  */
  unsigned short columnWidth;
  /**
     The filename of the object represented by the LHS of the
     diff. This is intentended for, e.g., generating header-style
     output. This may be NULL.
  */
  const char * nameLHS;
  /**
     The hash of the object represented by the LHS of the diff. This
     is intentended for, e.g., generating header-style output. This
     may be NULL.
  */
  const char * hashLHS;
  /**
     The filename of the object represented by the LHS of the
     diff. This is intentended for, e.g., generating header-style
     output. If this is NULL but nameLHS is not then they are assumed
     to have the same name.
  */
  const char * nameRHS;
  /**
     The hash of the object represented by the RHS of the diff. This
     is intentended for, e.g., generating header-style output. This
     may be NULL.
  */
  const char * hashRHS;
  /**
     Output destination. Any given builder might, depending on how it
     actually constructs the diff, buffer output and delay calling
     this until its finish() method is called.

     Note that not all diff builders use this. e.g. a diff builder
     which outputs to an ncurses widget cannot do so via this method
     and instead has to use the drawing methods of that API. Thus is
     is legal for this method to be NULL for some builders.
  */
  fsl_output_f out;
  /**
     State for this->out(). Ownership is unspecified by this
     interface: it is for use by this->out() but what is supposed to
     happen to it after this object is done with it depends on
     higher-level code.
  */
  void * outState;
  /**
     EXPERIMENTAL AND SUBJECT TO CHANGE.

     Optional ANSI color control sequences to be injected into
     text-mode diff output by diff builders which support them. All
     members of this struct must either be NULL, an empty string, or a
     valid ANSI escape sequence. The reset option must be the escape
     sequence to reset either just the color or to reset all ANSI
     control attributes, depending on how the other members are
     set. If any member other than reset is set, all must be set.

     The diff driver will treat any members of this which are NULL as
     empty strings to simplify diff builder color integration. The
     exception is the reset member - see its docs for details.
  */
  struct fsl_dibu_opt_ansi {
    /**
       Color escape sequence for inserted lines.
    */
    char const * insertion;
    /**
       Color escape sequence for edited or replaced lines. This option
       might be ignored, depending on how the renderer works. Some
       will render edits as a deletion/insertion pair.
    */
    char const * edit;
    /**
       Color escape sequence for inserted lines.
    */
    char const * deletion;
    /**
       Escape sequence to reset colors. If reset is empty or NULL and
       any others are not then reset is automatically treated as if it
       were the ANSI code to reset all color attributes.
    */
    char const * reset;
  } ansiColor;
};

/** Convenience typedef. */
typedef struct fsl_dibu_opt fsl_dibu_opt;

/** Initialized-with-defaults fsl_dibu_opt structure, intended for
    const-copy initialization. */
#define fsl_dibu_opt_empty_m {\
  0/*diffFlags*/, 5/*contextLines*/, 0/*columnWidth*/,\
  NULL/*nameLHS*/,NULL/*hashLHS*/,                       \
  NULL/*nameRHS*/, NULL/*hashRHS*/,                      \
  NULL/*out*/, NULL/*outState*/,                          \
  {/*ansiColor*/ ""/*insertion*/,""/*edit*/,""/*deletion*/,\
    ""/*reset*/}                                                \
}

/** Initialized-with-defaults fsl_dibu_opt structure, intended for
    non-const copy initialization. */
extern const fsl_dibu_opt fsl_dibu_opt_empty;

/**
   Information about each line of a file being diffed.

   This type is only in the public API for use by the fsl_dibu
   interface, specifically for use with fsl_dline_change_spans(). It
   is not otherwise intended for public use. None of its members are
   considered even remotely public except for this->z and this->n.
*/
struct fsl_dline {
  /**
      The text of the line. Owned by higher-level code.  Not
      necessarily NUL-terminated: this->n holds its length.
  */
  const char *z;
  /** Number of bytes of z which belong to this line. */
  unsigned short n;
  // All members after this point are strictly for internal use only.
  /** Index of first non-space char. */
  unsigned short indent;
  /** Number of bytes without leading/trailing space. */
  unsigned short nw;
  /** Hash of the line. Lower X bits are the length. */
  uint64_t h;
  /** 1+(Index of next line with same the same hash) */
  unsigned int iNext;
  /**
     An array of fsl_dline elements serves two purposes.  The fields
     above are one per line of input text.  But each entry is also
     a bucket in a hash table, as follows:
  */
  unsigned int iHash;   /* 1+(first entry in the hash chain) */
};

/**
   Convenience typedef.
*/
typedef struct fsl_dline fsl_dline;
/** Initialized-with-defaults fsl_dline structure, intended for
    const-copy initialization. */
#define fsl_dline_empty_m {NULL,0U,0U,0U,0U,0U,0U}
/** Initialized-with-defaults fsl_dline structure, intended for
    non-const copy initialization. */
extern const fsl_dline fsl_dline_empty;

/**
   Maximum number of change spans for fsl_dline_change.
*/
#define fsl_dline_change_max_spans 8

/**
   This "mostly-internal" type describes zero or more (up to
   fsl_dline_change_max_spans) areas of difference between two lines
   of text. This type is only in the public API for use with concrete
   fsl_dibu implementations.
*/
struct fsl_dline_change {
  /** Number of change spans (number of used elements in this->a). */
  unsigned char n;
  /** Array of change spans, in left-to-right order */
  struct fsl_dline_change_span {
    /* Reminder: int instead of uint b/c some ported-in algos use
       negatives. */
    /** Byte offset to start of a change on the left */
    int iStart1;
    /** Length of the left change in bytes */
    int iLen1;
    /** Byte offset to start of a change on the right */
    int iStart2;
    /** Length of the change on the right in bytes */
    int iLen2;
    /** True if this change is known to have no useful subdivs */
    int isMin;
  } a[fsl_dline_change_max_spans];
};

/**
   Convenience typedef.
*/
typedef struct fsl_dline_change fsl_dline_change;

/** Initialized-with-defaults fsl_dline_change structure, intended for
    const-copy initialization. */
#define fsl_dline_change_empty_m { \
  0, {                                                \
    {0,0,0,0,0}, {0,0,0,0,0}, {0,0,0,0,0}, {0,0,0,0,0}, \
    {0,0,0,0,0}, {0,0,0,0,0}, {0,0,0,0,0}, {0,0,0,0,0} \
  } \
}

/** Initialized-with-defaults fsl_dline_change structure, intended for
    non-const copy initialization. */
extern const fsl_dline_change fsl_dline_change_empty;

/**
   Given two lines of a diff, this routine computes a set of changes
   between those lines for display purposes and writes a description
   of those changes into the 3rd argument. After returning, p->n
   contains the number of elements in p->a which were populated by
   this routine.

   This function is only in the public API for use with
   fsl_dibu objects. It is not a requirement for such objects
   but can be used to provide more detailed diff changes than marking
   whole lines as simply changed or not.
*/
FSL_EXPORT void fsl_dline_change_spans(const fsl_dline *pLeft,
                                       const fsl_dline *pRight,
                                       fsl_dline_change *  p);

/**
   Compares two fsl_dline instances using memcmp() semantics,
   returning 0 if they are equivalent.

   @see fsl_dline_cmp_ignore_ws()
*/
FSL_EXPORT int fsl_dline_cmp(const fsl_dline *  pA,
                             const fsl_dline *  pB);

/**
   Counterpart of fsl_dline_cmp() but ignores all whitespace
   when comparing for equivalence.
*/
FSL_EXPORT int fsl_dline_cmp_ignore_ws(const fsl_dline *  pA,
                                       const fsl_dline *  pB);

/**
   Breaks the first n bytes of z into an array of fsl_dline records,
   each of which refers back to z (so it must remain valid for their
   lifetime). If n is negative, fsl_strlen() is used to calculate
   z's length.

   The final argument may be any flags from the fsl_diff2_flag_e
   enum, but only the following flags are honored:

   - FSL_DIFF2_STRIP_EOLCR
   - FSL_DIFF2_IGNORE_EOLWS
   - FSL_DIFF2_IGNORE_ALLWS

   On success, returns 0, assigns *pnLine to the number of lines, and
   sets *pOut to the array of fsl_dline objects, transfering ownership
   to the caller, who must eventually pass it to fsl_free() to free
   it.

   If z is NULL or n is 0, the input is assumed to be empty, 0 is returned,
   (*pOut) will be set to NULL, and (*pnLine) will be set to 0.

   On error, neither (*pnLine) nor (*pOut) are modified and returns one
   of:

   - FSL_RC_DIFF_BINARY if the input appears to be non-text.
   - FSL_RC_OOM on allocation error.
*/
FSL_EXPORT int fsl_break_into_dlines(const char *z, fsl_int_t n,
                                     uint32_t *pnLine,
                                     fsl_dline **pOut, uint64_t diff2Flags);

/** Convenience typedef. */
typedef struct fsl_dibu fsl_dibu;

/**
   This class is the basis of libfossil's port of the diff engine
   added to fossil(1) in 2021-09, colloquially known as the "diff
   builder."

   A diff builder is an object responsible for formatting low-level
   diff info another form, typically for human readability but also
   for machine readability (patches). The library generates a
   low-level diff then feeds that through an algorithm which
   determines which methods of this class to call, delegating all
   rendering of the diff to an instance of this class.

   The internal APIs which drive each instance of this class guaranty
   that if any method of this class returns non-0 (an error code) then
   no futher methods will be called except for finalize().
*/
struct fsl_dibu {
  /**
     Config info, owned by higher-level routines. Every diff builder
     requires one of these but not all options are relevant for all
     builders.

     Note that the diff driver may make a bitwise copy of this object
     and use _that_ one for the actual diff generation. That is,
     methods of this class must never assume that this member's
     pointer refers to a specific object. (This leeway is necessary in
     order to implement diff inversion (swapping the LHS/RHS of a
     diff).)
  */
  fsl_dibu_opt const * opt;
  /**
     Can optionally be set by factory functions to some internal
     opaque value, so that non-member routines specific to that type can determine
     whether any given builder is of the proper type.
  */
  void const * typeID;
  /**
     If not NULL, this is called once per pass per diff to give the
     builder a chance to perform any bootstrapping initialization or
     header output. At the point this is called, this->cfg is assumed
     to have been filled out properly. Diff builder implementations
     which require dynamic resource allocation may perform it here or
     in their factory routine(s).

     b->lnLHS and b->lnRHS will be set to 0 before each call.

     This method should also reset any dynamic state of a builder so
     that it may be reused for subsequent diffs. This enables the API
     to use a single builder for a collection of logically grouped
     files without having to destroy and reallocate the builder.

     Must return 0 on success, non-0 on error. If it returns non-0,
     the only other method of the instance which may be legally called
     is finalize().

     The diff driver sets this->lnLHS and this->lnRHS to 0 before
     calling this.
  */
  int (*start)(fsl_dibu *  b);

  /**
     If this is not NULL, it is called one time at the start of each
     chunk of diff for a given file and is passed the line number of
     each half of the diff and the number of lines in that chunk for
     that half (including insertions and deletions). This is primarily
     intended for generating conventional unified diff chunk headers
     in the form:

     ```
     @@ -A,B +C,D @@
     ```

     The inclusion of this method in an object might preclude certain
     other diff formatting changes which might otherwise apply.
     Notably, if the span between two diff chunks is smaller than the
     context lines count, the diff builder driver prefers to merge
     those two chunks together. That "readability optimization" is
     skipped when this method is set because this method may otherwise
     report that lines are being skipped which then subsequently get
     output by the driver.

     Must return 0 on success, non-0 on error.
  */
  int (*chunkHeader)(fsl_dibu* ,
                     uint32_t A, uint32_t B,
                     uint32_t C, uint32_t D);

  /**
     Tells the builder that n lines of common output are to be
     skipped. How it represents this is up to the impl. Must return 0
     on success, non-0 on error.

     Typical common implementation details:

     - Increment both this->lnLHS and this->lnRHS by n.
  */
  int (*skip)(fsl_dibu* , uint32_t n);
  /**
     Tells the builder that the given line represents one line of
     common output. Must return 0 on success, non-0 on error.

     Typical common implementation details:

     - Increment both this->lnLHS and this->lnRHS by 1.
  */
  int (*common)(fsl_dibu* , fsl_dline const * line);
  /**
     Tells the builder that the given line represents an "insert" into
     the RHS. Must return 0 on success, non-0 on error.

     Typical common implementation details:

     - Increment this->lnRHS by 1.
  */
  int (*insertion)(fsl_dibu* , fsl_dline const * line);
  /**
     Tells the builder that the given line represents a "deletion" - a
     line removed from the LHS. Must return 0 on success, non-0 on
     error.

     Typical common implementation details:

     - Increment this->lnLHS by 1.
  */
  int (*deletion)(fsl_dibu* , fsl_dline const * line);
  /**
     Tells the builder that the given line represents a replacement
     from the LHS to the RHS. Must return 0 on success, non-0 on
     error. This differs from an "edit" in that the line being
     replaced seems to have on relationship to the replacement. Even
     so, builders are free to represent replacements and edits
     identically, and are free to represent either or both as a pair
     of deletion/insertion operations.

     Typical common implementation details:

     - Increment both this->lnLHS and this->lnRHS by 1.
  */
  int (*replacement)(fsl_dibu* , fsl_dline const * lineLhs,
                     fsl_dline const * lineRhs);
  /**
     Tells the builder that the given line represents an "edit" from
     the LHS to the RHS. Must return 0 on success, non-0 on
     error. Builders are free, syntax permitting, to use the
     fsl_dline_change_spans() API to elaborate on edits for display
     purposes, to treat it identically to this->replacement(), or to
     treat this as a single pair of calls to this->deletion() and
     this->insertion(). In the latter case they simply need to pass
     lineLhs to this->deletion() and lineRhs to this->insertion().

     Typical common implementation details:

     - Increment both this->lnLHS and this->lnRHS by 1.
  */
  int (*edit)(fsl_dibu* , fsl_dline const * lineLhs,
              fsl_dline const * lineRhs);
  /**
     Must "finish" the diff process. Depending on the diff impl, this
     might flush any pending output or may be a no-op. This is only
     called if the rest of the diff was generated without producing an
     error result.

     This member may be NULL.

     Implementations are free to collect all of their output in an
     internal representation and delay flushing it until this routine
     is called.

     Must return 0 on success, non-0 on error (e.g. output flushing
     fails).

     Minor achtung: for a multi-file diff run, this gets called after
     each file. The library does not have enough information to know
     when a builder is "done for good" and if a custom builder
     requires, e.g., extra post-diff-loop processing, the client will
     have to take care of that themselves.
  */
  int (*finish)(fsl_dibu*  b);
  /**
     This optional method is similar to this->finish() but it is not
     called by the library. It is intended to be called, if it's not
     NULL, by the client after they are done, e.g., looping over a
     series of diffs with the same builder. Some builders can use this
     to flush any final state, e.g. dumping out change count totals or
     some such.

     As an example, the TCL/TK-based builder, when the FSL_DIBU_TCL_TK
     flag is set on this->implFlag, embeds a TK script in the output
     from this method.
  */
  int (*finally)(fsl_dibu*  b);
  /**
     Must free any state owned by this builder, including the builder
     object. It must not generate any output.
  */
  void (*finalize)(fsl_dibu* );
  /**
     If true, this builder gets passed through the diff generation
     process twice. See this->passNumber for details.
  */
  bool twoPass;
  /**
     Gets set to the "pass number" immediately before this->start() is
     called, starting with pass number 1. This value is only relevant
     for two-pass builders, which can use this to control their mode
     of operation, e.g. data collection in pass 1 and actual work in
     pass 2. Note that all of the diff-building API methods are called
     for both passes, including start() and finish().  Only finalize()
     and finally() are not affected by this.
  */
  unsigned short passNumber;
  /**
     Impl-specific diff-generation state. If it is owned by this
     instance then this->finalize() must clean it up.
  */
  void * pimpl;
  /**
     Space for private, implementation-specific flags, e.g. for
     tracking basic output state, e.g. of opening/closing tags. This
     must not be modified by clients.
  */
  unsigned int pimplFlags;
  /**
     Space for implementation-specific flags which clients may set to
     modify the dibu's behaviour. This is different from
     fsl_dibu_opt::diffFlags in that these flags are specific to a
     given dibu type. See the fsl_dibu_impl_flags_e enum for the list
     of flags for the library-provided diff builders.
  */
  unsigned int implFlags;
  /**
     A place to store the number of files seen by this builder so far,
     for builders which need to distinguish that somehow (e.g. adding
     a separator before each file after the first). Implementations
     which use this should increment it in their start() method.

     Maintenance reminder/TODO: we can't increment this from the main
     diff driver because... we possibly could, but we'd need to patch
     the various diff builders which currently do this themselves.
     The main diff driver doesn't have enough info to know when to set
     it, though. That "could" be done in this->finally() but that
     method is optional.
  */
  uint32_t fileCount;
  /**
     Number of lines seen of the LHS content. It is up to the concrete
     builder impl to update this if it's needed. The core diff driver
     sets this to 0 before calling this->start().
  */
  uint32_t lnLHS;
  /**
     Number of lines seen of the RHS content. It is up to the concrete
     builder impl to update this if it's needed. The core diff driver
     sets this to 0 before calling this->start().
  */
  uint32_t lnRHS;

  /**
     Metrics maintained by the core diff algorithm. These are updated
     during the first pass through a given fsl_diff_v2() run. They are
     NEVER reset by the builder algo because it cannot know if the
     user wants a running total or a per-file total. Clients running
     diffs in a loop may want to reset them on each loop. The simplest
     way is to cop fsl_dibu_empty.metrics over them. It is legal for a
     given builder to reset these in their start(), finish(), or
     finally() methods, depending on how the builder is used.

     Note that all metrics apply to the RHS version, not the LHS.
  */
  struct {
    /** Number of lines inserted performed so far. */
    uint32_t insertions;
    /** Number of lines deleted so far. */
    uint32_t deletions;
    /** Number of line-level edits performed so far. */
    uint32_t edits;
    /** Number of line replacements performed so far.
        These are different from edits in that the whole line
        differs in the LHS/RHS. */
    uint32_t replacements;
  } metrics;
};

/** Initialized-with-defaults fsl_dibu structure, intended for
    const-copy initialization. */
#define fsl_dibu_empty_m { \
  NULL/*opt*/,NULL/*typeID*/,                                         \
  NULL/*start()*/,NULL/*chunkHeader()*/,NULL/*skip()*/, NULL/*common()*/, \
  NULL/*insertion()*/,NULL/*deletion()*/, NULL/*replacement()*/, \
  NULL/*edit()*/, NULL/*finish()*/, NULL/*finally()*/,NULL/*finalize()*/, \
  false/*twoPass*/,0U/*passNumber*/, \
  NULL/*pimpl*/, 0U/*pimplFlags*/,0U/*implFlags*/,0U/*fileCount*/,         \
  0/*lnLHS*/,0/*lnRHS*/,                                              \
  {/*metrics*/0,0,0,0} \
}

/** Initialized-with-defaults fsl_dibu structure, intended for
    non-const copy initialization. */
extern const fsl_dibu fsl_dibu_empty;

/**
   Type IDs for use with fsl_dibu_factory().
*/
enum fsl_dibu_e {
/**
   Sentinel entry.
*/
FSL_DIBU_INVALID = 0,
/**
   A "dummy" diff builder intended only for testing the
   fsl_dibu interface and related APIs. It does not produce
   output which is generically useful.
*/
FSL_DIBU_DEBUG = 1,
/**
   Generates diffs in a compact low(ist)-level form originally
   designed for use by diff renderers implemented in JavaScript.

   This diff builder outputs a JSON object with the following
   properties:

   - hashLHS, hashRHS: the hashes of the LHS/RHS content.

   - nameLHS, nameRHS: the filenames of the LHS/RHS. By convention, if
     the RHS is NULL but the LHS is not, both sides have the same
     name.

   - diff: raw diff content, an array with the structure described
     below.

   Note that it is legal for the names and hashes to be "falsy" (null,
   not set, or empty strings).

   The JSON array consists of integer opcodes with each opcode
   followed by zero or more arguments:

   ```
   Syntax        Mnemonic    Description

   -----------   --------    --------------------------
   0             END         This is the end of the diff.
   1  INTEGER    SKIP        Skip N lines from both files.
   2  STRING     COMMON      The line STRING is in both files.
   3  STRING     INSERT      The line STRING is in only the right file.
   4  STRING     DELETE      The line STRING is in only the left file.
   5  SUBARRAY   EDIT        One line is different on left and right.
   ```

   The SUBARRAY is an array of 3*N+1 strings with N>=0.  The triples
   represent common-text, left-text, and right-text.  The last string
   in SUBARRAY is the common-suffix.  Any string can be empty if it
   does not apply.
*/
FSL_DIBU_JSON1,

/**
   A diff builder which produces output compatible with the patch(1)
   command. Its output is functionally identical to fossil(1)'s
   default diff output except that by default includes an Index line
   at the top of each file (use the FSL_DIFF2_NOINDEX flag in its
   fsl_dibu_opt::diffFlags to disable that).

   Supported flags:

   - FSL_DIFF2_LINE_NUMBERS (makes it incompatible with patch(1))
   - FSL_DIFF2_NOINDEX
*/
FSL_DIBU_UNIFIED_TEXT,

/**
   A diff builder which outputs a description of the diff in a
   TCL-readable form. It requires external TCL code in order to
   function.
*/
FSL_DIBU_TCL,
/**
   A pain-text side-by-side (a.k.a. split) diff view. This diff always
   behaves as if the FSL_DIFF2_LINE_NUMBERS flag were set because its
   output is fairly useless without line numbers. It optionally
   supports ANSI coloring.
*/
FSL_DIBU_SPLIT_TEXT
};
typedef enum fsl_dibu_e fsl_dibu_e;

/**
   A factory for creating fsl_dibu instances of types which
   are built in to the library. This does not preclude the creation of
   client-side diff builders (e.g. ones which write to ncurses widgets
   or similar special-case output).

   On success, returns 0 and assigns *pOut to a new builder instance
   which must eventually be freed by calling its pOut->finalize()
   method. On error, returns non-0 and *pOut is not modified. Error
   codes include FSL_RC_OOM (alloc failed) and FSL_RC_TYPE (unknown
   type ID), FSL_RC_TYPE (type is not (or not yet) implemented).
*/
FSL_EXPORT int fsl_dibu_factory( fsl_dibu_e type,
                                 fsl_dibu **pOut );

/**
   Base allocator for fsl_dibu instances. If extra is >0 then
   that much extra space is allocated as part of the same memory block
   and the pimpl member of the returned object is pointed to that
   space. Example (OOM handling elided for legibility):

   ```
   struct MyState { int x; int y; };
   typedef struct MyState MyState;
   fsl_dibu * b = fsl_dibu_alloc(sizeof(MyState));
   MyState * my = (MyState*)b->pimpl;
   my->x = 1;
   my->y = 2;
   ... populate b's members ...
   ... use b, then clean it up ...
   b->finalize(b);
   ```

   From within b's methods, the custom state can be accessed via its
   `pimpl` member.

   @see fsl_dibu_finalizer()
*/
FSL_EXPORT fsl_dibu * fsl_dibu_alloc(fsl_size_t extra);

/**
   This is a generic finalizer function for use as a
   fsl_dibu::finalize() method. It simply zeroes out b and
   passes it fsl_free(). This is suitable for builders created using
   fsl_dibu_alloc() _only_ if their custom state manages no
   extra memory. If they manage any custom memory then the require a
   custom, type-specific finalizer method.
*/
FSL_EXPORT void fsl_dibu_finalizer(fsl_dibu *  b);

/**
   This counterpart of fsl_diff_text() defines its output format in
   terms of a fsl_dibu instance which the caller must provide.
   The caller is responsible for pointing pBuilder->cfg to a
   configuration object suitable for the desired diff. In particular,
   pBuilder->cfg->out and (if necessary) pBuilder->cfg->outState must
   be set to non-NULL values.

   This function generates a low-level diff of two versions of content,
   contained in the given buffers, and passes that diff through the
   given diff builder to format it.

   Returns 0 on success. On error, it is not generally knowable whether
   or not any diff output was generated.

   The builder may produce any error codes it wishes, in which case
   they are propagated back to the caller. Common error codes include:

   - FSL_RC_OOM if an allocation fails.

   - FSL_RC_RANGE if the diff is "too big" and
   pBuilder->config->diffFlags contains the FSL_DIFF2_NOTTOOBIG flag.

   - FSL_RC_DIFF_BINARY if the to-diff content appears to be binary,
   noting that "appears to be" is heuristric-driven and subject to
   false positives. Specifically, files with extremely long lines will
   be recognized as binary (and are, in any case, generally less than
   useful for most graphical diff purposes).

   @see fsl_dibu_factory()
   @see fsl_diff_text()
   @see fsl_diff_v2_raw()
*/
FSL_EXPORT int fsl_diff_v2(fsl_buffer const * pv1,
                           fsl_buffer const * pv2,
                           fsl_dibu *  pBuilder);

/**
   Performs a diff, as for fsl_diff_v2(), but returns the results in
   the form of an array of COPY, DELETE, INSERT triples terminated by
   3 entries with the value 0.

   Each triple in the list specifies how many *lines* of each half of
   the diff (the first 2 arguments to this function) to COPY as-is
   (common code), DELETE (exists in the LHS but not in the RHS), and
   INSERT (exists in the RHS but not in the LHS). By breaking the
   input into lines and following these values, a line-level text-mode
   diff of the two blobs can be generated.

   See fsl_diff_v2() for the details, all of which apply except for
   the output:

   - cfg may be NULL, in which case fsl_dibu_opt_empty is used.

   - cfg->out is ignored.

   - On success, *outRaw is assigned to the output array and ownership
     of it is transfered to the caller, who must eventually pass it to
     fsl_free() to free it.

*/
FSL_EXPORT int fsl_diff_v2_raw(fsl_buffer const * pv1,
                               fsl_buffer const * pv2,
                               fsl_dibu_opt const * cfg,
                               int **outRaw );

/**
   If zDate is an ISO8601-format string, optionally with a .NNN
   fractional suffix, then this function returns true and sets
   *pOut (if pOut is not NULL) to the corresponding Julian
   value. If zDate is not an ISO8601-format string then this
   returns false and pOut is not modified.

   This function does NOT confirm that zDate ends with a NUL
   byte. i.e.  if passed a valid date string which has trailing
   bytes after it then those are simply ignored. This is so that it
   can be used to read subsets of larger strings.

   Achtung: this calculation may, due to voodoo-level
   floating-point behaviours, differ by a small fraction of a point
   (at the millisecond level) for a given input compared to other
   implementations (e.g. sqlite's strftime() _might_ differ by a
   millisecond or two or _might_ not). Thus this routine should not
   be used when 100% round-trip fidelity is required, but is close
   enough for routines which do not require 100% millisecond-level
   fidelity in time conversions.

   @see fsl_julian_to_iso8601()
*/
FSL_EXPORT bool fsl_iso8601_to_julian( char const * zDate, double * pOut );

/**
    Converts the Julian Day J to an ISO8601 time string. If addMs is
    true then the string includes the '.NNN' fractional part, else
    it will not. This function writes (on success) either 20 or 24
    bytes (including the terminating NUL byte) to pOut, depending on
    the value of addMs, and it is up to the caller to ensure that
    pOut is at least that long.

    Returns true (non-0) on success and the only error conditions
    [it can catch] are if pOut is NULL, J is less than 0, or
    evaluates to a time value which does not fit in ISO8601
    (e.g. only years 0-9999 are supported).

    @see fsl_iso8601_to_julian()
*/
FSL_EXPORT bool fsl_julian_to_iso8601( double J, char * pOut, bool addMs );

/**
   Returns the Julian Day time J value converted to a Unix Epoch
   timestamp. It assumes 86400 seconds per day and does not account
   for leap seconds, leap years, leap frogs, or any other kind of
   leap, up to and including leaps of faith.
*/
FSL_EXPORT fsl_time_t fsl_julian_to_unix( double J );

/**
   Performs a chdir() to the directory named by zChDir.

   Returns 0 on success. On error it tries to convert the
   underlying errno to one of the FSL_RC_xxx values, falling
   back to FSL_RC_IO if it cannot figure out anything more
   specific.
*/
FSL_EXPORT int fsl_chdir(const char *zChDir);

/**
   Works like fsl_chdir(zTo) but passes (zCurrent,nCurrent,NULL) to
   fsl_getcwd() to save the current dir name. Returns non-0 if either
   fsl_getcwd() or fsl_chdir() fails. To "pop" the dir just
   fsl_chdir() to zCurrent.
*/
FSL_EXPORT int fsl_chdir2(const char *zTo, char * zCurrent, fsl_size_t nCurrent);

/**
   A custom strftime() implementation.

   dest must be valid memory at least destLen bytes long. The result
   will be written there.

   fmt must contain the format string. See the source file strftime.c
   for the complete list of format specifiers and their descriptions.

   timeptr must be the time the caller wants to format.

   Returns 0 if any arguments are NULL.

   On success it returns the number of bytes written to dest, not
   counting the terminating NUL byte (which it also writes). It
   returns 0 on any error, and the client may need to distinguish
   between real errors and (destLen==0 or !*fmt), both of which could
   also look like errors.

   TODOs:

   - Refactor this to take a callback or a fsl_buffer, so that we can
   format arbitrarily long output.

   - Refactor it to return an integer error code.

   (This implementation is derived from public domain sources
   dating back to the early 1990's.)
*/
FSL_EXPORT fsl_size_t fsl_strftime(char *dest, fsl_size_t destLen,
                                   const char *format,
                                   const struct tm *timeptr);

/**
   A convenience form of fsl_strftime() which takes its timestamp in
   the form of a Unix Epoch time. See fsl_strftime() for the
   semantics of the first 3 arguments and the return value. If
   convertToLocal is true then epochTime gets converted to local
   time (via, oddly enough, localtime(3)), otherwise gmtime(3) is
   used for the conversion.
*/
FSL_EXPORT fsl_size_t fsl_strftime_unix(char * dest, fsl_size_t destLen,
                                        char const * format,
                                        fsl_time_t epochTime,
                                        bool convertToLocal);


/**
   A convenience form of fsl_strftime() which assumes that the
   formatted string is of "some reasonable size" and appends its
   formatted representation to b. Returns 0 on success, non-0 on
   error. If any argument is NULL or !*format then FSL_RC_MISUSE is
   returned. FSL_RC_RANGE is returned if the underlying call to
   fsl_strftime() fails (which it will if the format string
   resolves to something "unususually long"). It returns FSL_RC_OOM
   if appending to b fails due to an allocation error.
*/
FSL_EXPORT int fsl_buffer_strftime(fsl_buffer *  b,
                                   char const * format,
                                   const struct tm *timeptr);

/**
   "whence" values for use with fsl_buffer_seek(). They are analog to
   the "whence" argument of fseek(2) but are not guaranteed to have
   the same values.
*/
enum fsl_buffer_seek_e {
FSL_BUFFER_SEEK_SET = 1,
FSL_BUFFER_SEEK_CUR = 2,
FSL_BUFFER_SEEK_END = 3
};
typedef enum fsl_buffer_seek_e fsl_buffer_seek_e;

/**
   "Seeks" b's internal cursor to a position specified by the given offset
   from either the current cursor position (FSL_BUFFER_SEEK_CUR), the start
   of the buffer (FSL_BUFFER_SEEK_SET), or the end (FSL_BUFFER_SEEK_END).
   If the cursor would be placed out of bounds, it will be placed at the start
   resp. end of the buffer.

   The "end" of a buffer is the value of its fsl_buffer::used member,
   i.e. its one-after-the-end marker, as opposed to the at-the-end
   marker. That is: a seek to FSL_BUFFER_SEEK_END with an offset of -1
   would leave the cursor at the last available byte, not
   one-past-the-end.

   Returns the new position.

   Note that most buffer algorithms, e.g. fsl_buffer_append(), do not
   modify the cursor. Only certain special-case algorithms use it.

   @see fsl_buffer_tell()
   @see fsl_buffer_rewind()
*/
FSL_EXPORT fsl_size_t fsl_buffer_seek(fsl_buffer *  b, fsl_int_t offset,
                                      fsl_buffer_seek_e  whence);

/**
   Returns the buffer's current cursor position.

   @see fsl_buffer_rewind()
   @see fsl_buffer_seek()
*/
FSL_EXPORT fsl_size_t fsl_buffer_tell(fsl_buffer const * b);

/**
   Resets b's cursor to the beginning of the buffer.

   @see fsl_buffer_tell()
   @see fsl_buffer_seek()
*/
FSL_EXPORT void fsl_buffer_rewind(fsl_buffer *  b);

/**
   Read from buf until consuming a character matching delim, which must
   be representable as an unsigned char. The consumed bytes, including any
   matching delim character, are stored in *ret, and a terminating NUL
   added when the delimiter or end-of-buffer is encountered.

   If *retsz is non-zero, *ret must either be pre-allocated to at least
   *retsz bytes, or be a NULL pointer. *ret must be passable to fsl_free();
   it is grown as needed as if by fsl_realloc() and *retsz updated with
   its new size. It is the caller's responsiblity to fsl_free() *ret when
   it is no longer needed. Even on failure, *ret may be updated.

   Return the number of bytes read, including any matched delim character.
   If no bytes were read and the stream is at end-of-buffer, return -1.
   If an error occurs, return -1 and set buf->errCode to indicate the error.

   n.b. This routine matches getdelim(3) semantics. Callers must check
   buf->errCode to distinguish between end-of-buffer and error.
   https://pubs.opengroup.org/onlinepubs/9799919799/functions/getdelim.html

   Example:

   ```
   char *line = NULL;
   size_t sz = 0;
   ssize_t len;

   while ((len = fsl_buffer_getdelim(&line, &sz, '\n', &buf)) != -1)
     fprintf(stdout, "line (%ld bytes): %s\n", len, line);

   fsl_free(line);
   if (fsl_buffer_err(&buf))
     errx(buf.errCode, "fsl_buffer_getdelim: %s", fsl_rc_cstr(buf.errCode));
   ```

   @see fsl_buffer_getline()
*/
FSL_EXPORT fsl_int_t fsl_buffer_getdelim(char **ret, size_t *retsz,
                                         int delim, fsl_buffer *buf);

/**
   Equivalent to fsl_buffer_getdelim() with `delim` equal to '\n'.

   @see fsl_buffer_getdelim()
*/
FSL_EXPORT fsl_int_t fsl_buffer_getline(char **ret, size_t *retsz,
                                        fsl_buffer *buf);

/**
   The "Path Finder" class is a utility class for searching the
   filesystem for files matching a set of common prefixes and/or
   suffixes (i.e. directories and file extensions).

   Example usage:

   ```
   fsl_pathfinder pf = fsl_pathfinder_empty;
   int rc;
   char const * found = NULL;
   rc = fsl_pathfinder_ext_add( &pf, ".doc" );
   if(rc) { ...error... }
   // The following error checks are elided for readability:
   rc = fsl_pathfinder_ext_add( &pf, ".txt" );
   rc = fsl_pathfinder_ext_add( &pf, ".wri" );
   rc = fsl_pathfinder_dir_add( &pf, "." );
   rc = fsl_pathfinder_dir_add( &pf, "/my/doc/dir" );
   rc = fsl_pathfinder_dir_add( &pf, "/other/doc/dir" );

   rc = fsl_pathfinder_search( &pf, "MyDoc", &found, NULL);
   if(0==rc){ assert(NULL!=found); }

   // Eventually clean up:
   fsl_pathfinder_clear(&pf);
   ```

   @see fsl_pathfinder_dir_add()
   @see fsl_pathfinder_ext_add()
   @see fsl_pathfinder_clear()
   @see fsl_pathfinder_search()
*/
struct fsl_pathfinder {
  /**
     Holds the list of search extensions. Each entry
     is a (char *) owned by this object.
  */
  fsl_list ext;
  /**
     Holds the list of search directories. Each entry is a (char *)
     owned by this object.
  */
  fsl_list dirs;
  /**
     Used to build up a path string during fsl_pathfinder_search(),
     and holds the result of a successful search. We use a buffer,
     as opposed to a simple string, because (A) it simplifies the
     search implementation and (B) reduces allocations (it gets
     reused for each search).
  */
  fsl_buffer buf;
};

typedef struct fsl_pathfinder fsl_pathfinder;
/**
   Initialized-with-defaults fsl_pathfinder instance, intended for
   const copy initialization.
*/
#define fsl_pathfinder_empty_m {\
  .ext = fsl_list_empty_m,      \
  .dirs = fsl_list_empty_m,     \
  .buf = fsl_buffer_empty_m     \
}

/**
   Initialized-with-defaults fsl_pathfinder instance, intended for
   copy initialization.
*/
FSL_EXPORT const fsl_pathfinder fsl_pathfinder_empty;

/**
   Frees all memory associated with pf, but does not free pf.
   Is a no-op if pf is NULL.
*/
FSL_EXPORT void fsl_pathfinder_clear(fsl_pathfinder *  pf);

/**
   Equivalent to fsl_pathfinder_dir_add2() with -1 as a final
   argument.

   @see fsl_pathfinder_ext_add()
   @see fsl_pathfinder_search()
*/
FSL_EXPORT int fsl_pathfinder_dir_add(fsl_pathfinder *  pf,
                                      char const * dir);

/**
   Adds the given directory to pf's search path. Returns 0 on
   success, FSL_RC_MISUSE if !pf or !dir (dir _may_ be an empty
   string), FSL_RC_OOM if copying the string or adding it to the
   list fails.

   If strLen is negative, fsl_strlen() is used to calculate the length
   of the string.

   @see fsl_pathfinder_ext_add()
   @see fsl_pathfinder_search()
*/
FSL_EXPORT int fsl_pathfinder_dir_add2(fsl_pathfinder *  pf, char const * dir,
                                       fsl_int_t strLen);

/**
   Adds the given directory to pf's search extensions. Returns 0 on
   success, FSL_RC_MISUSE if !pf or !dir (dir _may_ be an empty
   string), FSL_RC_OOM if copying the string or adding it to the
   list fails.

   Note that the client is responsible for adding a "." to the
   extension, if needed, as this API does not apply any special
   meaning to any characters in a search extension. e.g. "-journal"
   and "~" are both perfectly valid extensions for this purpose.

   @see fsl_pathfinder_dir_add()
   @see fsl_pathfinder_search()
*/
FSL_EXPORT int fsl_pathfinder_ext_add2(fsl_pathfinder *  pf, char const * ext,
                                       fsl_int_t strLen);

/**
   Equivalent to fsl_pathfinder_ext_add2() with -1 as a final
   argument.

   @see fsl_pathfinder_dir_add()
   @see fsl_pathfinder_search()
*/
FSL_EXPORT int fsl_pathfinder_ext_add(fsl_pathfinder *  pf,
                                      char const * ext);

/**
   Splits a conventional path-separator-delimited string into tokens
   and adds each as either a directory (if isDirs is true) or an
   extension in the given fsl_pathfinder object. Returns 0 on success,
   FSL_RC_OOM on allocation error. pathLen is the length of the path
   string. If it's negative, fsl_strlen() is used to calculate it.

   See fsl_path_splitter for the semantics of the splitting.
*/
FSL_EXPORT int fsl_pathfinder_split( fsl_pathfinder *  tgt,
                                     bool isDirs,
                                     char const * path,
                                     fsl_int_t pathLen );

/**
   Searches for a file whose name can be constructed by some
   combination of pf's directory/extension lists and the given base
   name.

   It searches for files in the following manner:

   If the 2nd parameter exists as-is in the filesystem, it is
   treated as a match, otherwise... Loop over all directories
   in pf->dirs. Create a path with DIR/base, or just base if
   the dir entry is empty (length of 0). Check for a match.
   If none is found, then... Loop over each extension in
   pf->ext, creating a path named DIR/baseEXT (note that it
   does not add any sort of separator between the base and the
   extensions, so "~" and "-foo" are legal extensions). Check
   for a match.

   On success (a readable filesystem entry is found):

   - It returns 0.

   - If pOut is not NULL then `*pOut` is set to the NUL-terminated
     path it found. The bytes of the returned string are only valid
     until the next search operation on pf, so copy them if you need
     them.  Note that the returned path is _not_ normalized via
     fsl_file_canonical_name() or similar, and it may very well return
     a relative path (if base or one of pf->dirs contains a relative
     path part). As a special case, if `base` is found as-is, without
     a lookup, `*pOut` is set to `base`, so has its lifetime.

   - If outLen is not NULL, *outLen will be set to the length of the
     returned string.

   On error:

   - Returns FSL_RC_MISUSE if !pf, !base, !*base.

   - Returns FSL_RC_OOM on allocation error (it uses a buffer to hold
     its path combinations and return value).

   - Returns FSL_RC_NOT_FOUND if it finds no entry.

   The host platform's customary path separator is used to separate
   directory/file parts ('\\' on Windows and '/' everywhere else).

   Note that it _is_ legal for pOut and outLen to both be NULL, in
   which case a return of 0 signals that an entry was found, but the
   client has no way of knowing what path it might be (unless, of
   course, they rely on internal details of the fsl_pathfinder API,
   which they most certainly should not do).

   Tip: if the client wants to be certain that this function will not
   allocate memory, simply use fsl_buffer_reserve() on pf->buf to
   reserve the desired amount of space in advance. As long as the
   search paths never surpass that length, this function will not need
   to allocate. (Until/unless the following TODO is implemented...)

   Potential TODO: use fsl_file_canonical_name() so that the search
   dirs themselves do not need to be entered using
   platform-specific separators. The main reason it's not done now
   is that it requires another allocation. The secondary reason is
   because it's sometimes useful to use relative paths in this
   context (based on usage in previous trees from which this code
   derives).

   @see fsl_pathfinder_dir_add()
   @see fsl_pathfinder_ext_add()
   @see fsl_pathfinder_clear()
*/
FSL_EXPORT int fsl_pathfinder_search(fsl_pathfinder *  pf,
                                     char const * base,
                                     char const ** pOut,
                                     fsl_size_t *  outLen );


/**
   A utility class for creating ZIP-format archives. All members
   are internal details and must not be mucked about with by the
   client. See fsl_zip_file_add() for an example of how to use it.

   Note that it creates ZIP content in memory, as opposed to
   streaming it (it is not yet certain if abstractly streaming a
   ZIP is possible), so creating a ZIP file this way is exceedingly
   memory-hungry.

   @see fsl_zip_file_add()
   @see fsl_zip_timestamp_set_julian()
   @see fsl_zip_timestamp_set_unix()
   @see fsl_zip_end()
   @see fsl_zip_body()
   @see fsl_zip_finalize()
*/
struct fsl_zip_writer {
  /**
     Number of entries (files + dirs) added to the zip file so far.
  */
  fsl_size_t entryCount;
  /**
     Current DOS-format time of the ZIP.
  */
  int32_t dosTime;
  /**
     Current DOS-format date of the ZIP.
  */
  int32_t dosDate;
  /**
     Current Unix Epoch time of the ZIP.
  */
  fsl_time_t unixTime;
  /**
     An artificial root directory which gets prefixed
     to all inserted filenames.
  */
  char * rootDir;
  /**
     The buffer for the table of contents.
  */
  fsl_buffer toc;
  /**
     The buffer for the ZIP file body.
  */
  fsl_buffer body;
  /**
     Internal scratchpad for ops which often allocate
     small buffers.
  */
  fsl_buffer scratch;
  /**
     The current list of directory entries (as (char *)).
  */
  fsl_list dirs;
};
typedef struct fsl_zip_writer fsl_zip_writer;

/**
   An initialized-with-defaults fsl_zip_writer instance, intended
   for in-struct or const-copy initialization.
*/
#define fsl_zip_writer_empty_m {                \
  0/*entryCount*/,                            \
  0/*dosTime*/,                             \
  0/*dosDate*/,                             \
  0/*unixTime*/,                            \
  NULL/*rootDir*/,                          \
  fsl_buffer_empty_m/*toc*/,                \
  fsl_buffer_empty_m/*body*/,               \
  fsl_buffer_empty_m/*scratch*/,            \
  fsl_list_empty_m/*dirs*/                  \
}

/**
   An initialized-with-defaults fsl_zip_writer instance,
   intended for copy-initialization.
*/
FSL_EXPORT const fsl_zip_writer fsl_zip_writer_empty;

/**
   Sets a virtual root directory in z, such that all files added
   with fsl_zip_file_add() will get this directory prefixed to
   it.

   If zRoot is NULL or empty then this clears the virtual root,
   otherwise is injects any directory levels it needs to into the
   being-generated ZIP. Note that zRoot may contain multiple levels of
   directories, e.g. "foo/bar/baz", but it must be legal for use in a
   ZIP file.

   This routine copies zRoot's bytes, so they may be transient.

   Returns 0 on success, FSL_RC_MISUSE if !z, FSL_RC_OOM on
   allocation error. Returns FSL_RC_RANGE if zRoot is an absolute
   path or if zRoot cannot be normalized to a "simplified name" (as
   per fsl_is_simple_pathname(), with the note that this routine
   will pass a copy of zRoot through fsl_file_simplify_name()
   first).

   @see fsl_zip_finalize()
*/
FSL_EXPORT int fsl_zip_root_set(fsl_zip_writer *  z,
                                char const * zRoot );

/**
   Adds a file or directory to the ZIP writer z. zFilename is the
   virtual name of the file or directory. If pContent is NULL then
   it is assumed that we are creating one or more directories,
   otherwise the ZIP's entry is populated from pContent. The
   permsFlag argument specifies the fossil-specific permission
   flags from the fsl_fileperm_e enum, but currently ignores the
   permsFlag argument for directories. Not that this function
   creates directory entries for any files automatically, so there
   is rarely a need for client code to create them (unless they
   specifically want to ZIP an empty directory entry).

   Notes of potential interest:

   - The ZIP is created in memory, and thus creating ZIPs with this
   API is exceedingly memory-hungry.

   - The timestamp of any given file must be set separately from
   this call using fsl_zip_timestamp_set_unix() or
   fsl_zip_timestamp_set_julian(). That value is then used for
   subsequent file-adds until a new time is set.

   - If a root directory has been set using fsl_zip_root_set() then
   that name, plus '/' (if the root does not end with one) gets
   prepended to all files added via this routine.

   An example of the ZIP-generation process:

   ```
   int rc;
   fsl_zip_writer z = fsl_zip_writer_empty;
   fsl_buffer buf = fsl_buffer_empty;
   fsl_buffer const * zipBody;

   // ...fill the buf buffer (not shown here)...

   // Optionally set a virtual root dir for new files:
   rc = fsl_zip_root_set( &z, "myRootDir" ); // trailing slash is optional
   if(rc) { ... error ...; goto end; }

   // We must set a timestamp which will be used until we set another:
   fsl_zip_timestamp_set_unix( &z, time(NULL) );

   // Add a file:
   rc = fsl_zip_file_add( &z, "foo/bar.txt", &buf, FSL_FILE_PERM_REGULAR );
   // Clean up our content:
   fsl_buffer_reuse(&buf); // only needed if we want to re-use the buffer's memory
   if(rc) goto end;

   // ... add more files the same way (not shown) ...

   // Now "seal" the ZIP file:
   rc = fsl_zip_end( &z );
   if(rc) goto end;

   // Fetch the ZIP content:
   zipBody = fsl_zip_body( &z );
   // zipBody now points to zipBody->used bytes of ZIP file content
   // which can be sent to an arbitrary destination, e.g.:
   rc = fsl_buffer_to_filename( zipBody, "my.zip" );

   end:
   fsl_buffer_clear(&buf);
   // VERY important, once we're done with z:
   fsl_zip_finalize( &z );
   if(rc){...we had an error...}
   ```

   @see fsl_zip_timestamp_set_julian()
   @see fsl_zip_timestamp_set_unix()
   @see fsl_zip_end()
   @see fsl_zip_body()
   @see fsl_zip_finalize()
*/
FSL_EXPORT int fsl_zip_file_add( fsl_zip_writer *  z,
                                 char const * zFilename,
                                 fsl_buffer const * pContent,
                                 int permsFlag );

/**
   Ends the ZIP-creation process, padding all buffers, writing all
   final required values, and freeing up most of the memory owned
   by z. After calling this, z->body contains the full generated
   ZIP file.

   Returns 0 on success. On error z's contents may still be
   partially intact (for debugging purposes) and z->body will not
   hold complete/valid ZIP file contents. Results are undefined if
   !z or z has not been properly initialized.

   The caller must eventually pass z to fsl_zip_finalize() to free
   up any remaining resources.

   @see fsl_zip_timestamp_set_julian()
   @see fsl_zip_timestamp_set_unix()
   @see fsl_zip_file_add()
   @see fsl_zip_body()
   @see fsl_zip_finalize()
   @see fsl_zip_end_take()
*/
FSL_EXPORT int fsl_zip_end( fsl_zip_writer *  z );

/**
   This variant of fsl_zip_end() transfers the current contents
   of the zip's body to dest, replacing (freeing) any contents it may
   hold when this is called, then passes z to fsl_zip_finalize()
   to free any other resources (which are invalidated by the removal
   of the body).

   Returns 0 on success, FSL_RC_MISUSE if either pointer is NULL,
   some non-0 code if the proxied fsl_zip_end() call fails. On
   error, the transfer of contents to dest does NOT take place, but
   z is finalized (if it is not NULL) regardless of success or
   failure (even if dest is NULL). i.e. on error z is still cleaned
   up.
*/
FSL_EXPORT int fsl_zip_end_take( fsl_zip_writer *  z,
                                 fsl_buffer *  dest );

/**
   This variant of fsl_zip_end_take() passes z to fsl_zip_end(),
   write's the ZIP body to the given filename, passes
   z to fsl_zip_finalize(), and returns the result of
   either end/save combination. Saving is not attempted
   if ending the ZIP fails.

   On success 0 is returned and the contents of the ZIP are in the
   given file. On error z is STILL cleaned up, and the file might
   have been partially populated (only on I/O error after writing
   started). In either case, z is cleaned up and ready for re-use or
   (in the case of a heap-allocated instance) freed.
*/
FSL_EXPORT int fsl_zip_end_to_filename( fsl_zip_writer *  z,
                                        char const * filename );


/**
   Returns a pointer to z's ZIP content buffer. The contents are ONLY
   valid after fsl_zip_end() returns 0.

   @see fsl_zip_timestamp_set_julian()
   @see fsl_zip_timestamp_set_unix()
   @see fsl_zip_file_add()
   @see fsl_zip_end()
   @see fsl_zip_end_take()
   @see fsl_zip_finalize()
*/
FSL_EXPORT fsl_buffer const * fsl_zip_body( fsl_zip_writer const * z );

/**
   Frees all memory owned by z and resets it to a clean state, but
   does not free z. Any fsl_zip_writer instance which has been
   modified via the fsl_zip_xxx() family of functions MUST
   eventually be passed to this function to clean up any contents
   it might have accumulated during its life. After this returns,
   z is legal for re-use in creating a new ZIP archive.

   @see fsl_zip_timestamp_set_julian()
   @see fsl_zip_timestamp_set_unix()
   @see fsl_zip_file_add()
   @see fsl_zip_end()
   @see fsl_zip_body()
*/
FSL_EXPORT void fsl_zip_finalize(fsl_zip_writer *  z);

/**
   Set z's date and time from a Julian Day number. Results are
   undefined if !z. Results will be invalid if rDate is negative. The
   timestamp is applied to all fsl_zip_file_add() operations until it
   is re-set.

   @see fsl_zip_timestamp_set_unix()
   @see fsl_zip_file_add()
   @see fsl_zip_end()
   @see fsl_zip_body()
*/
FSL_EXPORT void fsl_zip_timestamp_set_julian(fsl_zip_writer *  z,
                                             double rDate);

/**
   Set z's date and time from a Unix Epoch time. Results are
   undefined if !z. Results will be invalid if rDate is negative. The
   timestamp is applied to all fsl_zip_file_add() operations until it
   is re-set.
*/
FSL_EXPORT void fsl_zip_timestamp_set_unix(fsl_zip_writer *  z,
                                           fsl_time_t epochTime);

/**
   State for the fsl_timer_xxx() family of functions.

   Achtung: timer support is only enabled if the library is built with
   the proper clock support. Windows builds mostly support it
   automatically (all except wall clock time) and non-Windows builds
   do if the library is built with HAVE_CLOCK_GETTIME (for
   clock_gettime(2)) and HAVE_GETRUSAGE (for getrusage(2)) both set to
   true values. That said: the library currently _assumes_ that
   non-Windows builds have both of those POSIX-2008 functions. On
   Windows, building with HAVE_CLOCK_GETTIME is necessary only for
   fsl_timer::wall.

   Example usage:

   ```
   fsl_timer tTotal = fsl_timer_empty;
   fsl_timer tRun;
   fsl_timer_start(&tRun);
   ... do some work ...
   fsl_timer_add(&tRun, &tTotal);
   ```

   After that, `tTotal` contains the microseconds of time which
   elapsed between the time `tRun` was started and the time
   fsl_timer_add() was called.

   @see fsl_timer_start()
   @see fsl_timer_fetch()
   @see fsl_timer_add()
   @see fsl_timer_stop()
*/
struct fsl_timer {
  /**
     The amount of time (microseconds) spent in "user space".
  */
  uint64_t user;

  /**
     The amount of time (microseconds) spent in "kernel space".
  */
  uint64_t system;

  /**
     The microseconds of "wall clock" time. Only available if built
     with HAVE_CLOCK_GETTIME. (Patches for a Windows alternative would
     be welcomed.)
  */
  uint64_t wall;
};
typedef struct fsl_timer fsl_timer;

/**
   Initialized-with-defaults fsl_timer instance,
   intended for const copy initialization.
*/
#define fsl_timer_empty_m {.user=0,.system=0,.wall=0}

/**
   Initialized-with-defaults fsl_timer instance,
   intended for non-const copy initialization.
*/
FSL_EXPORT const fsl_timer fsl_timer_empty;

/**
   Sets t's counter state to the current CPU timer usage, as
   determined by the OS. It is legal to pass an uninitialized instance
   to this function, as this function will fully populate it.

   @see fsl_timer_fetch()
   @see fsl_timer_add()
*/
FSL_EXPORT void fsl_timer_start(fsl_timer *  t);

/**
   Sets each of pUser, pSys, and pWall to the number of
   microseconds which have elapsed since t was passed
   to fsl_timer_start().
*/
FSL_EXPORT void fsl_timer_fetch(fsl_timer const * t,
                                uint64_t * pUser, uint64_t * pSys,
                                uint64_t * pWall);

/**
   Captures the current time and adds the difference of tStart and
   the current time to tgt's members. Intended for
   keeping track of interruptable running times.

   Example usage:

   ```
   fsl_timer timerTotal = fsl_timer_empty; // copy init is important
   fsl_timer timerWork;                    // here it's not

   fsl_timer_start(&timerWork);
   ... do some timed work...
   fsl_timer_add(&timerWork, &timerTotal);

   ... do some non-timed work ...

   fsl_timer_start(&timerWork);
   ... do some timed work...
   fsl_timer_add(&timerWork, &timerTotal);
   ```

   The end result will be that timerTotal holds the accumulated times
   between each start/add invocation.
*/
FSL_EXPORT void fsl_timer_add(fsl_timer const * tStart,
                                fsl_timer *  tgt);

/**
   Returns the difference in _CPU_ times in microseconds since t was
   last passed to fsl_timer_start() or fsl_timer_reset().  It might
   return 0 due to system-level precision restrictions. Note that this
   is not useful for measuring wall times.
*/
FSL_EXPORT uint64_t fsl_timer_cpu(fsl_timer const * t);

/**
   Resets t's state to contain relative times by subtracting its
   starting time from the current time. Returns the post-adjustment
   (t->system+t->user).

   @see fsl_timer_start()
   @see fsl_timer_reset()
*/
FSL_EXPORT uint64_t fsl_timer_stop(fsl_timer *  t);

/**
   A convenience macro for timing a block of code (BODY) with the
   fsl_timer API. TGT is a (fsl_timer*) object which must initially
   start out zeroed out (e.g. initialied by copying from
   fsl_timer_empty or fsl_timer_empty_m). SUFFIX is a unique-per-scope
   name suffix for a scope-local temporary fsl_timer object, so must
   be suitable as the suffix of a C identifier. The run-time of BODY
   is fsl_timer_add()'d to (fsl_timer*) TGT.

   Example usage:

   ```
   fsl_timer_scope3(__COUNTER__,&myTimer,{
     ... your code to time ...
   });
   ```

   For compilers which do not like `__COUNTER__`, just use any
   identifier suffix which is unique to the call scope, e.g. an
   integer.
*/
#define fsl_timer_scope3(SUFFIX,TGT,BODY)  \
  fsl_timer _tm ## SUFFIX;                 \
  fsl_timer_start(&_tm ## SUFFIX);         \
  BODY;                                    \
  fsl_timer_add(&_tm ## SUFFIX, TGT)

/**
   Convenience form of fsl_timer_scope3() which uses __COUNTER__ as
   its 2nd argument. Note that __COUNTER__ is a C extension but is
   believed to be supported on all modern (as of 2025) C compilers.
*/
#define fsl_timer_scope(TGT,BODY) \
  fsl_timer_scope3(__COUNTER__,TGT,BODY)


/**
   For the given red/green/blue values (all in the range of 0 to
   255, or truncated to be so!) this function returns the RGB
   encoded in the lower 24 bits of a single number. See
   fsl_gradient_color() for an explanation and example.

   For those asking themselves, "why does an SCM API have a function
   for encoding RGB colors?" the answer is: fossil(1) has a long
   history of using HTML color codes to set the color of branches,
   and this is provided in support of such features.

   @see fsl_rgb_decode()
   @see fsl_gradient_color()
*/
FSL_EXPORT unsigned int fsl_rgb_encode( int r, int g, int b );

/**
   Given an RGB-encoded source value, this function decodes
   the lower 24 bits into r, g, and b. Any of r, g, and b may
   be NULL to skip over decoding of that part.

   @see fsl_rgb_encode()
   @see fsl_gradient_color()
*/
FSL_EXPORT void fsl_rgb_decode( unsigned int src, int *r, int *g, int *b );

/**
   For two color values encoded as RRGGBB values (see below for the
   structure), this function computes a gradient somewhere between
   those colors. c1 and c2 are the edges of the gradient.
   numberOfSteps is the number of steps in the gradient. stepNumber
   is a number less than numberOfSteps which specifies the "degree"
   of the gradients. If either numberOfSteps or stepNumber is 0, c1
   is returned. stepNumber of equal to or greater than c2 returns
   c2.

   The return value is an RGB-encoded value in the lower 24 bits,
   ordered in big-endian. In other words, assuming rc is the return
   value:

   - red   = (rc&0xFF0000)>>16
   - green = (rc&0xFF00)>>8
   - blue  = (rc&0xFF)

   Or use fsl_rgb_decode() to break it into its component parts.

   It can be passed directly to a printf-like function, using the
   hex-integer format specifier, e.g.:

   ```
   fsl_buffer_appendf(&myBuf, "#%06x", rc);
   ```

   Tip: for a given HTML RRGGBB value, its C representation is
   identical: HTML \#F0D0A0 is 0xF0D0A0 in C.

   @see fsl_rgb_encode()
   @see fsl_rgb_decode()
*/
FSL_EXPORT unsigned int fsl_gradient_color(unsigned int c1, unsigned int c2,
                                           unsigned int numberOfSteps,
                                           unsigned int stepNumber);

/**
   "Simplifies" an SQL string by making the following modifications
   inline:

   - Consecutive non-newline spaces outside of an SQL string are
   collapsed into one space.

   - Consecutive newlines outside of an SQL string are collapsed into
   one space.

   Contents of SQL strings are not transformed in any way.

   len must be the length of the sql string. If it is negative,
   fsl_strlen(sql) is used to calculate the length.

   Returns the number of bytes in the modified string (its strlen) and
   NUL-terminates it at the new length. Thus the input string must be
   at least one byte longer than its virtual length (its NUL
   terminator byte suffices, provided it is NUL-terminated, as we can
   safely overwrite that byte).

   If !sql or its length resolves to 0, this function returns 0
   without side effects.
*/
FSL_EXPORT fsl_size_t fsl_simplify_sql( char * sql, fsl_int_t len );

/**
   Convenience form of fsl_simplify_sql() which assumes b holds an SQL
   string. It gets processed by fsl_simplify_sql() and its 'used'
   length potentially gets adjusted to match the adjusted SQL string.
*/
FSL_EXPORT fsl_size_t fsl_simplify_sql_buffer( fsl_buffer *  b );

/**
   Returns the result of calling the platform's equivalent of
   isatty(fd). e.g. on Windows this is _isatty() and on Unix
   isatty(). i.e. it returns true if it thinks that the given file
   descriptor value is attached to an interactive terminal, else it
   returns false.

   The standard file descriptors are: 0=stdin, 1=stdout, and 2=stderr.
*/
FSL_EXPORT bool fsl_isatty(int fd);

/**

   A container type for lists of db record IDs. This is used in
   several places as a cache for record IDs, to keep track of ones
   we know about, ones we know that we don't know about, and to
   avoid duplicate processing in some contexts.
*/
struct fsl_id_bag {
  /**
     Number of entries of this->list which are in use (have a
     positive value). They need not be contiguous!  Must be <=
     capacity.
  */
  fsl_size_t entryCount;
  /**
     The number of elements allocated for this->list.
  */
  fsl_size_t capacity;
  /**
     The number of elements in this->list which have a zero or
     positive value. Must be <= capacity.
  */
  fsl_size_t used;
  /**
     Array of IDs this->capacity elements long. "Used" elements
     have a positive value. Unused ones are set to 0.
  */
  fsl_id_t * list;
};

/**
   Initialized-with-defaults fsl_id_bag structure,
   intended for copy initialization.
*/
FSL_EXPORT const fsl_id_bag fsl_id_bag_empty;

/**
   Initialized-with-defaults fsl_id_bag structure,
   intended for in-struct initialization.
*/
#define fsl_id_bag_empty_m {      \
  .entryCount = 0, .capacity = 0, \
  .used = 0, .list = NULL         \
}

/**

   Return the number of elements in the bag.
*/
FSL_EXPORT fsl_size_t fsl_id_bag_count(fsl_id_bag const * p);

/**

   Remove element e from the bag if it exists in the bag. If e is not
   in the bag, this is a no-op. Returns true if it removes an element,
   else false.

   e must be positive. Results are undefined if e<=0.
*/
FSL_EXPORT bool fsl_id_bag_remove(fsl_id_bag *  p, fsl_id_t e);

/**
   Returns true if e is in the given bag. Returns false if it is
   not. It is illegal to pass an e value of 0, and that will trigger
   an assertion in debug builds. In non-debug builds, behaviour if
   passed 0 is undefined.
*/
FSL_EXPORT bool fsl_id_bag_contains(fsl_id_bag const * p, fsl_id_t e);

/**
   Insert element e into the bag if it is not there already.  Returns
   0 if it actually inserts something or if it already contains such
   an entry, and some other value on error (namely FSL_RC_OOM on
   allocation error).

   e must be positive or an assertion is triggered in debug builds. In
   non-debug builds, behaviour is undefined if passed 0.
*/
FSL_EXPORT int fsl_id_bag_insert(fsl_id_bag *  p, fsl_id_t e);

/**
   Returns the ID of the first element in the bag.  Returns 0 if the
   bag is empty.

   ```
   fsl_id_t rid = fsl_id_bag_first(debag);
   int rc = 0;
   for( ; !rc && rid>0; rid = fsl_id_bag_next(debag, rid) ){
     rc = ...;
   }
   ```

*/
FSL_EXPORT fsl_id_t fsl_id_bag_first(fsl_id_bag const * p);

/**
   Returns the next element in the bag after e.  Return 0 if e is
   the last element in the bag.  Any insert or removal from the bag
   might reorder the bag. It is illegal to pass this 0 (and will
   trigger an assertion in debug builds). For the first call, pass
   it the non-0 return value from fsl_id_bag_first(). For
   subsequent calls, pass the previous return value from this
   function.

   @see fsl_id_bag_first()
*/
FSL_EXPORT fsl_id_t fsl_id_bag_next(fsl_id_bag const * p, fsl_id_t e);

/**
   Swaps the contents of the given bags.
*/
FSL_EXPORT void fsl_id_bag_swap(fsl_id_bag *  lhs, fsl_id_bag *  rhs);

/**
   Copies all entries from src into dest. Returns 0 on success,
   FSL_RC_OOM on allocation error.

   An alternative approach: if dest is empty then
   fsl_id_bag_swap(src,dest), perhaps followed by
   fsl_id_bag_clear(src) or fsl_id_bag_reuse(src), is more efficient
   (does not allocate).
*/
FSL_EXPORT int fsl_id_bag_copy(fsl_id_bag const * src, fsl_id_bag * dest);

/**
   Frees any memory owned by p, but does not free p.
*/
FSL_EXPORT void fsl_id_bag_clear(fsl_id_bag *  p);

/**
   Resets p's internal list, effectively emptying it for re-use, but
   does not free its memory. Immediately after calling this
   fsl_id_bag_count() will return 0. Returns p.
*/
FSL_EXPORT fsl_id_bag * fsl_id_bag_reuse(fsl_id_bag *  p);

/** @deprecated

   Older (deprecated) name of fsl_id_bag_reuse().
*/
#define fsl_id_bag_reset fsl_id_bag_reuse

/**
   Returns true if p contains a fossil-format merge conflict marker,
   else returns false.

   @see fsl_buffer_merge3()
*/
FSL_EXPORT bool fsl_buffer_contains_merge_marker(fsl_buffer const *p);

/**
   Performs a three-way merge.

   The merge is an edit against pV2. Both pV1 and pV2 have a common
   origin at pPivot. Apply the changes of pPivot ==> pV1 to pV2,
   appending them to pOut. (Pedantic side-note: the input buffers are
   not const because we need to manipulate their cursors, but their
   buffered memory is not modified.)

   If merge conflicts are encountered, it continues as best as it can
   and injects "indiscrete" markers in the output to denote the nature
   of each conflict. If conflictCount is not NULL then on success the
   number of merge conflicts is written to *conflictCount.

   Returns 0 on success, FSL_RC_OOM on OOM, FSL_RC_TYPE if any input
   appears to be binary.

   FIXME/TODO: return FSL_RC_DIFF_BINARY instead of FSL_RC_TYPE when
   encountering binary inputs.

   @see fsl_buffer_contains_merge_marker()
*/
FSL_EXPORT int fsl_buffer_merge3(fsl_buffer *  pPivot,
                                 fsl_buffer *  pV1,
                                 fsl_buffer *  pV2,
                                 fsl_buffer *  pOut,
                                 unsigned int *  conflictCount);

/**
   Appends the first n bytes of string z to buffer b in the form of
   TCL-format string literal. If n<0 then fsl_strlen() is used to
   determine the length. Returns 0 on success, FSL_RC_OOM on error.

   If fsl_buffer_err() is true for the given buffer, that code is
   returned without other side effects.

   If the 2nd argument is true, squiggly braces within the string are
   escaped, else they are not. Whether that's required or not depends
   on how the resulting TCL will be used. If it will be eval'd directly,
   it must be escaped. If it will be read as a file and tokenized, it
   needn't be.
 */
FSL_EXPORT int fsl_buffer_append_tcl_literal(fsl_buffer *  b,
                                             bool escapeSquigglies,
                                             char const * z, fsl_int_t n);


/**
   Event IDs for use with fsl_confirm_callback_f implementations.

   The idea here is to send, via callback, events from the library to
   the client when a potentially interactive response is necessary.
   We define a bare minimum of information needed for the client to
   prompt a user for a response. To that end, the interface passes on
   2 pieces of information to the client: the event ID and a filename.
   It is up to the application to translate that ID into a
   user-readable form, get a response (using a well-defined set of
   response IDs), and convey that back to the
   library via the callback's result pointer interface.

   This enum will be extended as the library develops new requirements
   for interactive use.

   @see fsl_confirm_response_e
*/
enum fsl_confirm_event_e {
/**
   Sentinal value.
*/
FSL_CEVENT_INVALID = 0,
/**
   An operation requests permission to overwrite a locally-modified
   file. e.g. when performing a checkout over a locally-edited
   version. Overwrites of files which are known to be in the previous
   (being-overwritten) checkout version are automatically overwritten.
*/
FSL_CEVENT_OVERWRITE_MOD_FILE = 1,
/**
   An operation requests permission to overwrite an SCM-unmanaged file
   with one which is managed by SCM. This can happen, e.g., when
   switching from a version which did not contain file X, but had file
   X on disk, to a version which contains file X.
*/
FSL_CEVENT_OVERWRITE_UNMGD_FILE = 2,
/**
   An operation requests permission to remove a LOCALLY-MODIFIED file
   which has been removed from SCM management. e.g. when performing a
   checkout over a locally-edited version and an edited file was
   removed from the SCM somewhere between those two versions.
   UMODIFIED files which are removed from the SCM between two
   checkouts are automatically removed on the grounds that it poses no
   data loss risk because the other version is "somewhere" in the SCM.
*/
FSL_CEVENT_RM_MOD_UNMGD_FILE = 3,

/**
   Indicates that the library cannot determine which of multiple
   potential versions to choose from and requires the user to
   select one.
*/
FSL_CEVENT_MULTIPLE_VERSIONS = 4

};
typedef enum fsl_confirm_event_e fsl_confirm_event_e;

/**
   Answers to questions posed to clients via the
   fsl_confirm_callback_f() interface.

   This enum will be extended as the library develops new requirements
   for interactive use.

   @see fsl_confirm_event_e
*/
enum fsl_confirm_response_e {
/**
   Sentinel/default value - not a valid answer. Guaranteed to have a
   value of 0. No other entries in this enum are guaranteed to have
   well-known/stable values: always use the enum symbols instead of
   integer values.
*/
FSL_CRESPONSE_INVALID = 0,
/**
   Accept the current event and continue processes.
*/
FSL_CRESPONSE_YES = 1,
/**
   Reject the current event and continue processes.
*/
FSL_CRESPONSE_NO = 2,
/**
   Reject the current event and stop processesing. Cancellation is
   generally considered to be a recoverable error.
*/
FSL_CRESPONSE_CANCEL = 3,
/**
   Accept the current event and all identical event types for the
   current invocation of this particular SCM operation.
*/
FSL_CRESPONSE_ALWAYS = 5,
/**
   Reject the current event and all identical event types for the
   current invocation of this particular SCM operation.
*/
FSL_CRESPONSE_NEVER = 6,
/**
   For events which are documented as being multiple-choice,
   this answer indicates that the client has set the index of
   their choice in the fsl_confirm_response::multipleChoice
   field:

   - FSL_CEVENT_MULTIPLE_VERSIONS
*/
FSL_CRESPONSE_MULTI = 7
};
typedef enum fsl_confirm_response_e fsl_confirm_response_e;

/**
   A response for use with the fsl_confirmer API. It is intended to
   encapsulate, with a great deal of abstraction, answers to typical
   questions which the library may need to interactively query a user
   for. e.g. confirmation about whether to overwrite a file or which
   one of 3 versions to select.

   This type will be extended as the library develops new requirements
   for interactive use.
*/
struct fsl_confirm_response {
  /**
     Client response to the current fsl_confirmer question.
  */
  fsl_confirm_response_e response;
  /**
     If this->response is FSL_CRESPONSE_MULTI then this must be set to
     the index of the client's multiple-choice answer.

     Events which except this in their response:

     - FSL_CEVENT_MULTIPLE_VERSIONS
  */
  uint16_t multipleChoice;
};
/**
   Convenience typedef.
*/
typedef struct fsl_confirm_response fsl_confirm_response;

/**
   Empty-initialized fsl_confirm_detail instance to be used for
   const copy initialization.
*/
#define fsl_confirm_response_empty_m {FSL_CRESPONSE_INVALID, -1}

/**
   Empty-initialized fsl_confirm_detail instance to be used for
   non-const copy initialization.
*/
FSL_EXPORT const fsl_confirm_response fsl_confirm_response_empty;

/**
   A struct for passing on interactive questions to
   fsl_confirmer_callback_f implementations.
*/
struct fsl_confirm_detail {
  /**
     The message ID of this confirmation request. This value
     determines how the rest of this struct's values are to
     be interpreted.
  */
  fsl_confirm_event_e eventId;
  /**
     Depending on the eventId, this might be NULL or might refer to a
     filename. This will be a filename for following confirmations
     events:

     - FSL_CEVENT_OVERWRITE_MOD_FILE
     - FSL_CEVENT_OVERWRITE_UNMGD_FILE
     - FSL_CEVENT_RM_MOD_UNMGD_FILE

     For all others it will be NULL.

     Whether this name refers to an absolute or relative path is
     context-dependent, and not specified by this API. In general,
     relative paths should be used if/when what they are relative to
     (e.g. a checkout root) is/should be clear to the user. The intent
     is that applications can display that name to the user in a UI
     control, so absolute paths "should" "generally" be avoided
     because they can be arbitrarily long.
  */
  const char * filename;
  /**
     Depending on the eventId, this might be NULL or might
     refer to a list of details of a type specified in the
     documentation for that eventId.

     Implementation of such an event is still TODO, but we have at
     least one use case lined up (asking a user which of several
     versions is intended when the checkout-update operation is given
     an ambiguous hash prefix).

     Events for which this list will be populated:

     - FSL_CEVENT_MULTIPLE_VERSIONS: each list entry will be a (char
     const*) with a version number, branch name, or similar, perhaps
     with relevant metadata such as a checkin timestamp. The client is
     expected to pick one answer, set its list index to the
     fsl_confirm_response::multipleChoice member, and to set
     fsl_confirm_response::response to FSL_CRESPONSE_MULTI.

     In all cases, a response of FSL_CRESPONSE_CANCEL will trigger a
     cancellation.

     In all cases, the memory for the items in this list is owned by
     (or temporarily operated on the behalf of) the routine which has
     launched this query. fsl_confirm_callback_f implements must never
     manipulate the list's or its content's state.
  */
  const fsl_list * multi;
};
typedef struct fsl_confirm_detail fsl_confirm_detail;

/**
   Empty-initialized fsl_confirm_detail instance to be used for
   const-copy initialization.
*/
#define fsl_confirm_detail_empty_m \
  {FSL_CEVENT_INVALID, NULL, NULL}

/**
   Empty-initialized fsl_confirm_detail instance to be used for
   non-const-copy initialization.
*/
FSL_EXPORT const fsl_confirm_detail fsl_confirm_detail_empty;

/**
   Should present the user (if appropriate) with an option of how to
   handle the given event write that answer to
   outAnswer->response. Return 0 on success, non-0 on error, in which
   case the current operation will fail with that result code.
   Answering with FSL_CRESPONSE_CANCEL is also considered failure but
   recoverably so, whereas a non-cancel failure is considered
   unrecoverable.
*/
typedef int (*fsl_confirm_callback_f)(fsl_confirm_detail const * detail,
                                      fsl_confirm_response *outAnswer,
                                      void * confirmerState);

/**
   A fsl_confirm_callback_f and its callback state, packaged into a
   neat little struct for easy copy/replace/restore of confirmers.
*/
struct fsl_confirmer {
  /**
     Callback which can be used for basic interactive confirmation
     purposes, within the very libfossil-centric limits of the
     interface.
  */
  fsl_confirm_callback_f callback;
  /**
     Opaque state pointer for this->callback. Its lifetime is not
     managed by this object and it is assumed, if not NULL, to live at
     least as long as this object.
  */
  void * callbackState;
};
typedef struct fsl_confirmer fsl_confirmer;
/** Empty-initialized fsl_confirmer instance for const-copy
    initialization. */
#define fsl_confirmer_empty_m {NULL,NULL}
/** Empty-initialized fsl_confirmer instance for non-const-copy
    initialization. */
FSL_EXPORT const fsl_confirmer fsl_confirmer_empty;

/**
   State for use with fsl_dircrawl_f() callbacks.

   @see fsl_dircrawl()
*/
struct fsl_dircrawl_state {
  /**
     Absolute directory name of the being-visited directory.
  */
  char const *absoluteDir;
  /**
     Name (no path part) of the entry being visited.
  */
  char const *entryName;
  /**
     Filesystem entry type.
  */
  fsl_fstat_type_e entryType;
  /**
     Opaque client-specified pointer which was passed to
     fsl_dircrawl().
  */
  void * callbackState;

  /**
     Directory depth of the crawl process, starting at 1 with
     the directory passed to fsl_dircrawl().
  */
  unsigned int depth;
};
typedef struct fsl_dircrawl_state fsl_dircrawl_state;

/**
   Callback type for use with fsl_dircrawl(). Its argument contains
   all crawling-relevant state for entry being visited.

   Implementations must return 0 on success or another FSL_RC_xxx
   value on error.

   Special return values:

   - FSL_RC_BREAK will cause directory-crawling to stop without an
     error (returning 0).

   - Returning FSL_RC_NOOP will cause recursion into a directory to
     be skipped, but traversal to otherwise continue.

   All pointers in the state argument are owned by fsl_dircrawl() and
   will be invalidated as soon as the callback returns, thus they must
   be copied if they are needed for later.
*/
typedef int (*fsl_dircrawl_f)(fsl_dircrawl_state const *);

/**
   Recurses into a directory and calls a callback for each filesystem
   entry.  It does not change working directories, but callbacks are
   free to do so as long as they restore the working directory before
   returning.

   The first argument is the name of the directory to crawl. In order
   to avoid any dependence on a specific working directory, if it is
   not an absolute path then this function will expand it to an
   absolute path before crawling begins. For each entry under the
   given directory, it calls the given callback, passing it a
   fsl_dircrawl_state object holding various state. All pointers in
   that object, except for the callbackState pointer, are owned by
   this function and may be invalidated as soon as the callback
   returns.

   For each directory entry, it recurses into that directory,
   depth-first _after_ passing it to the callback.

   It DOES NOT resolve/follow symlinks, instead passing them on to the
   callback for processing. Note that passing a symlink to this
   function will not work because this function does not resolve
   symlinks. Thus it provides no way to traverse symlinks, as its
   scope is only features suited for the SCM and symlinks have no
   business being in an SCM. (Fossil supports symlinks, more or less,
   but libfossil does not.)

   It silently skips any files for which stat() fails or is not of a
   "basic" file type (e.g. character devices and such).

   Returns 0 on success, FSL_RC_TYPE if the given name is not a
   directory, and FSL_RC_RANGE if it recurses "too deep," (some
   "reasonable" internally hard-coded limit), in order to help avoid a
   stack overflow.

   If the callback returns non-0, iteration stops and returns that
   result code unless the result is FSL_RC_BREAK or FSL_RC_NOOP, with
   those codes being treated specially, as documented for
   fsl_dircrawl_f() callbacks.
*/
FSL_EXPORT int fsl_dircrawl(char const * dirName, fsl_dircrawl_f callback,
                            void * callbackState);

/**
   Strips any trailing slashes ('/') from the given string by
   assigning those bytes to NUL and returns the number of slashes
   NUL'd out. nameLen must be the length of the string. If nameLen is
   negative, fsl_strlen() is used to calculate its length.
*/
FSL_EXPORT fsl_size_t fsl_strip_trailing_slashes(char * name, fsl_int_t nameLen);

/**
   A convenience from of fsl_strip_trailing_slashes() which strips
   trailing slashes from the given buffer and changes its b->used
   value to account for any stripping. Results are undefined if b is
   not properly initialized.
*/
FSL_EXPORT void fsl_buffer_strip_slashes(fsl_buffer *  b);

/**
   Appends each ID from the given bag to the given buffer using the given
   separator string. Returns FSL_RC_OOM on allocation error.

   If fsl_buffer_err() is true for the given buffer, that code is
   returned without other side effects.
*/
FSL_EXPORT int fsl_id_bag_to_buffer(fsl_id_bag const * bag,
                                    fsl_buffer *  b,
                                    char const * separator);

/**
   Flags for use with the fsl_looks family of functions.
*/
enum fsl_lookslike_e {
/* Nothing special was found. */
FSL_LOOKSLIKE_NONE    = 0x00000000,
/* One or more NUL chars were found. */
FSL_LOOKSLIKE_NUL     = 0x00000001,
/* One or more CR chars were found. */
FSL_LOOKSLIKE_CR      = 0x00000002,
/* An unpaired CR char was found. */
FSL_LOOKSLIKE_LONE_CR = 0x00000004,
/* One or more LF chars were found. */
FSL_LOOKSLIKE_LF      = 0x00000008,
/* An unpaired LF char was found. */
FSL_LOOKSLIKE_LONE_LF = 0x00000010,
/* One or more CR/LF pairs were found. */
FSL_LOOKSLIKE_CRLF    = 0x00000020,
/* An over length line was found. */
FSL_LOOKSLIKE_LONG    = 0x00000040,
/* An odd number of bytes was found. */
FSL_LOOKSLIKE_ODD     = 0x00000080,
/* Unable to perform full check. */
FSL_LOOKSLIKE_SHORT   = 0x00000100,
/* Invalid sequence was found. */
FSL_LOOKSLIKE_INVALID = 0x00000200,
/* Might be binary. */
FSL_LOOKSLIKE_BINARY  = FSL_LOOKSLIKE_NUL | FSL_LOOKSLIKE_LONG | FSL_LOOKSLIKE_SHORT,
 /* Line separators. */
FSL_LOOKSLIKE_EOL     = (FSL_LOOKSLIKE_LONE_CR | FSL_LOOKSLIKE_LONE_LF | FSL_LOOKSLIKE_CRLF)
};

/**
   Returns true if b appears to contain "binary" (non-UTF8/16) content,
   else returns false.
*/
FSL_EXPORT bool fsl_looks_like_binary(fsl_buffer const * b);

/**
   If b appears to contain any non-UTF8 content, returns a truthy
   value: one or more values from the fsl_lookslike_e enum indicating
   which sort of data was seen which triggered its conclusion:

   - FSL_LOOKSLIKE_BINARY means the content appears to be binary
     because it contains embedded NUL bytes or an "extremely long"
     line. This function may diagnose UTF-16 as binary.

   - !FSL_LOOKSLIKE_BINARY means the content is non-binary but may
     not necessarily be valid UTF-8.

   - 0 means the contents appear to be valid UTF-8.

   It b's content is empty, returns 0.

   The 2nd argument can be a mask of any values from fsl_lookslike_e
   and will cause this routine to stop inspecting the input once it
   encounters any content described by those flags.

   The contents are examined until the end is reached or a condition
   described by the 2nd parameter's flags is encountered.

   WARNINGS:

   - This function does not validate that the blob content is properly
     formed UTF-8.  It assumes that all code points are the same size.
     It does not validate any code points.  It makes no attempt to
     detect if any [invalid] switches between UTF-8 and other
     encodings occur.

   - The only code points that this function cares about are the NUL
     character, carriage-return, and line-feed.
*/
FSL_EXPORT int fsl_looks_like_utf8(fsl_buffer const * b, int stopFlags);

/**
   Returns true if b's contents appear to contain anything other than valid
   UTF8.

   It uses the approach described at:

   http://en.wikipedia.org/wiki/UTF-8#Invalid_byte_sequences

   except for the "overlong form" of \\u0000 which is not considered
   invalid here: Some languages, like Java and Tcl, use it. This function
   also considers valid the derivatives CESU-8 & WTF-8.
*/
FSL_EXPORT bool fsl_invalid_utf8(fsl_buffer const * b);

/**
   Returns a static pointer to bytes of a UTF8 BOM. If the argument is
   not NULL, it is set to the strlen of those bytes (always 3).
*/
FSL_EXPORT unsigned char const *fsl_utf8_bom(unsigned int *pnByte);

/**
   Returns true if b starts with a UTF8 BOM. If the 2nd argument is
   not NULL, *pBomSize is set to the number of bytes in the BOM
   (always 3), regardless of whether or not the function returns true
   or false.
*/
FSL_EXPORT bool fsl_starts_with_bom_utf8(fsl_buffer const * b, unsigned int *pBomSize);

/**
   Populates the first n bytes of tgt with random bytes. Note that n
   must be 31 bits or less (2GB). The exact source of randomness is
   not guaranteed by the API, but the implementation currently uses
   sqlite3_randomness().
*/
FSL_EXPORT void fsl_randomness(unsigned int n, void *tgt);


/**
   Given a filename and its length, this function returns a pointer to
   the last instance of a path separator character in that string,
   checking for both `/` and `\\`. If the length is negative,
   fsl_strlen() is used to calculate it. If no separator is found,
   NULL is returned.
*/
FSL_EXPORT char const * fsl_last_path_sep(char const * str, fsl_int_t slen );

/**
   A helper type for tokenizing conventional PATH-style strings.
   Initialize them with fsl_path_splitter_init() and iterate over them
   with fsl_path_splitter_next().
*/
struct fsl_path_splitter {
  /** Begining of the input range. */
  char const * begin;
  /** One-after-the-end of the input range. */
  char const * end;
  /** Position for the next token lookup. */
  char const * pos;
  /** List of token separator characters (ASCII only). */
  char const * separators;
};
typedef struct fsl_path_splitter fsl_path_splitter;
/** @def fsl_path_splitter_empty_m

   Default-initialized fsl_path_splitter instance, intended for const-copy
   initialization. On Windows builds its separators member is set to
   ";" and on other platforms it's set to ":;".
*/
#if FSL_PLATFORM_IS_WINDOWS
#  define fsl_path_splitter_empty_m {NULL,NULL,NULL,";"}
#else
#  define fsl_path_splitter_empty_m {NULL,NULL,NULL,":;"}
#endif

/**
   Default-initialized fsl_path_splitter instance, intended for
   copy initialization.

   @see fsl_path_splitter_empty_m
*/
FSL_EXPORT const fsl_path_splitter fsl_path_splitter_empty;

/**
   Wipes out pt's current state by copying fsl_path_splitter_empty
   over it and initializes pt to use the given path as its input. If
   len is 0 or more then it must be the length of the string, in
   bytes. If len is less than 0, fsl_strlen() is used to determine the
   path's length.  (When dealing with inputs which are not
   NUL-terminated, it's critical that the user pass the correct
   non-negative length.)

   If the client wants to modify pt->separators, it must be done
   *after* calling this.

   Use fsl_path_splitter_next() to iterate over the path entries.
*/
void fsl_path_splitter_init( fsl_path_splitter *  pt, char const * path,
                             fsl_int_t len );

/**
   Given a fsl_path_splitter which was formerly initialized using
   fsl_path_splitter_init(), this iterates over the next-available
   path component in the input, skipping over empty entries (leading,
   consecutive, or trailing separator characters).

   The separator characters are specified by pt->separators, which must
   be a NUL-terminated string of 1 or more characters.

   If a non-empty entry is found then:

   - *token is set to the first byte of the entry.

   - *len is assigned to the byte length of the entry.

   If no entry is found then:

   - *token, and *len are not modified.

   - FSL_RC_NOT_FOUND is returned if the end of the path was found
   while tokenizing.

   - FSL_RC_MISUSE is returned if pt->separators is NULL or empty or
   contains any non-ASCII characters.

   - FSL_RC_RANGE is returned if called after the previous case, or
   if the input object's path has a length of 0.

   In any non-0-return case, it's not a fatal error, it's simply
   information about why tokenization cannot continue, and can
   normally be ignored. After non-0 is returned, the tokenizer must be
   re-initialized if it is to be used again.

   Example:

   @code
   char const * t = 0;
   fsl_size_t tLen = 0;
   fsl_path_splitter pt = fsl_path_splitter_empty;
   fsl_path_splitter_init(&pt, path, pathLen);
   while(0==fsl_path_splitter_next(&pt, &t, &tLen)){
      // The next element is the tLen bytes of memory starting at t:
      printf("Path element: %.*s\n", (int)tLen, t);
   }
   @endcode
*/
int fsl_path_splitter_next( fsl_path_splitter *  pt, char const ** token,
                            fsl_size_t *  len );

/**
   Implements a cross-platform system(3) interface. It passes its
   argument, which must not be NULL or empty, to the equivalent of
   system(3). Returns 0 if that call returns 0, else it will return a
   FSL_RC code approximating the lower-level error code (noting that
   this system's man pages are a bit vague on the exact return
   semantics of system(3)). If it cannot convert an errno value to an
   approximate FSL_RC value, it will return FSL_RC_ERROR.
*/
int fsl_system(const char *zOrigCmd);

typedef struct fsl_url fsl_url;

/**
   A type for parsing URLs. Its memory is managed by fsl_url_parse()
   and fsl_url_cleanup().
*/
struct fsl_url {
  /** The raw, unparsed URL. */
  char const * raw;
  /** The URL's scheme. */
  char const * scheme;
  /** The URL's username or NULL. */
  char const * username;
  /** The URL's username's password or NULL. */
  char const * password;
  /** The remote host name. Is only NULL for file URLs. */
  char const * host;
  /** The :portNumber part or 0. */
  uint16_t port;
  /** The path. For file URLs, this is where the file's name is
      stored. */
  char const * path;
  /** The query string, including the '?' prefix. */
  char const * query;
  /** The fragment part, including the '#' prefix. */
  char const * fragment;

  /**
     Private implementation details. fsl_url_parse() allocates this
     buffer a single time and points the other members into the memory
     owned by impl.b. Its approximate allocated size is a handful of
     bytes more than twice the strlen() of this->raw.
  */
  struct { fsl_buffer b; } impl;
};

/** Initialized-with-defaults fsl_url structure, intended for
    const-copy initialization. */
#define fsl_url_empty_m {                        \
    .raw=0, .scheme=0, .username=0, .password=0, \
    .host=0, .port=0, .path=0, .query=0,         \
    .fragment=0, .impl = {.b=fsl_buffer_empty_m} \
}

/** Initialized-with-defaults fsl_url structure, intended for
    non-const copy initialization. */
extern const fsl_url fsl_url_empty;

/**
   Attempts to parse zUrl as a URL, filling out u with the
   results.

   It passes u to fsl_url_cleanup2(), with a false 2nd argument,
   before starting.

   Results are undefined if zUrl is NULL.

   On success, returns 0 and populates u. On error, returns FSL_RC_OOM
   or FSL_RC_SYNTAX and it passes u to fsl_url_cleanup() before
   returning.

   All memory referenced by u's members is invalidated by any of
   the fsl_url APIs. Do not hold copies of pointers to a fsl_url
   member for later use.

   For each missing component of the URL, the corresponding member of
   u will be NULL. A missing port is designated by 0, and a port value
   ouside of the range (1..65535) triggers a syntax error.

   Quirks:

   - It may mis-parse malformed URLs.

   - It does no unescaping of the string. It "could" provide the query
     string in an unescaped form but that would require some
     refactoring.

   - file URLs (in this API) do not support a host name, port, query
     string, or fragment. Everything after the scheme is available via
     u->path.

   - zUrl may be a filesystem path, starting with either '/' or '.',
     in which case it is treated as if it were prefixed with
     "file://".

   - file:/X and file:///X both resolve to /X. file://X, though
     technically illegal[^1] resolves to X.

   - It does not recognize Windows drive letters as such. A URL in the
     form "file:/c:/..." or "file:///c:/..." will be assigned a
     u->path of "/c:/...", and "file:c:/..." will have a u->path of
     "c:/...". In non-file URLs, a drive letter will very probably be
     treated like a hostname with an invalid port (so will fail to
     parse).

   [^1]: https://en.wikipedia.org/wiki/File_URI_scheme
*/
FSL_EXPORT int fsl_url_parse(fsl_url * u, char const * zUrl);

/**
   Flags for use with fsl_url_render().
*/
enum fsl_url_render_e {

  /** Elide the scheme:// prefix. */
  FSL_URL_RENDER_NO_SCHEME = 0x01,

  /** Elide the user name and password. */
  FSL_URL_RENDER_NO_USER = 1<<1,

  /** Elide any password part of the URL. */
  FSL_URL_RENDER_NO_PASSWORD = 1<<2,

  /**
     Render any password part of the URL as some unspecified number of
     "*" characters.
  */
  FSL_URL_RENDER_MASK_PASSWORD = 1<<3,

  /** Elide the query string and fragment. */
  FSL_URL_RENDER_NO_QUERY = 1<<4,

  /** Elide the fragment. */
  FSL_URL_RENDER_NO_FRAGMENT = 1<<5
};

/**
   Appends a rendering of the given parsed url object to the given
   buffer.  The final argument can be used to modify how the rendering
   is done.

   Returns pOut->errCode. If pOut->errCode is non-0 when this is
   called, this call is a no-op.
*/
FSL_EXPORT int fsl_url_render(fsl_url const * u, fsl_buffer * pOut,
                              int flags);

/**
   Equivalent to fsl_url_cleanup2(u, true).
*/
FSL_EXPORT void fsl_url_cleanup(fsl_url * u);

/**
   Frees all memory owned by u, and zeroes it out, but does not free
   u.  If freeBuffer is false then u's internal buffer is also freed
   otherwise it is retained, which will lead to a leak if u is not
   later either passed to fsl_url_cleanup() or to this function with a
   true freeBuffer value.
*/
FSL_EXPORT void fsl_url_cleanup2(fsl_url * u, bool freeBuffer);

/**
   Swaps the contents of the given objects.
*/
FSL_EXPORT void fsl_url_swap(fsl_url * lhs, fsl_url *rhs);

/**
   Parses key-value pairs from u->query. On the initial call, the
   second argument should be a pointer to a call-local NULL value.

   If this returns true:

   - It will have replaced bKey's contents with the
     next query string key, and bVal's contents to its value (if any).

   - It sets `*zCursor` to the next parse position. The intent is that
     the caller pass the same pointer to each call until this function
     returns false.

   It returns false if:

   - No more URL args are found.
   - An allocation error while appending to bKey or bVal.

   Regardless of success or failure, it calls fsl_buffer_reuse() on
   its output buffers before beginning work.

   The query string is assumed to be URL-encoded and the parsed-out
   keys and values get URL-decoded when adding them to the output
   buffers.

   Example:

   ```
   fsl_buffer key = fsl_buffer_empty;
   fsl_buffer val = fsl_buffer_empty;
   char const * zTail = 0;
   while( fsl_url_next_query_arg(myUrl, &zTail, &key, &val) ){
     // key holds the key and val might hold a value
     // (else it will be empty)
   }
   fsl_buffer_clear(&key);
   fsl_buffer_clear(&val);
   ```

   That example ignores alloc errors which happen during parsing but
   for completeness one should check key.errCode and val.errCode,
   either of which might be FSL_RC_OOM.
*/
FSL_EXPORT bool fsl_url_next_query_arg(fsl_url const * u,
                                       char const **zCursor,
                                       fsl_buffer * bKey,
                                       fsl_buffer * bVal);

/**
   Converts the first nBytes bytes from pBytes to hex form, writing
   the result to pOut. pOut must have at least 2*nBytes+1 bytes of
   space, as this function writes 2*nBytes of hex and a NUL
   terminator to it.
*/
FSL_EXPORT void fsl_to_hex(unsigned char const *pBytes,
                           fsl_size_t nBytes, char *pOut);


#if 0
/**
   The UTF16 counterpart of fsl_looks_like_utf8(), with the addition that the
   2nd argument, if true, specifies that the 2nd argument is true then
   the contents of the buffer are byte-swapped for checking purposes.

   This does not validate that the blob is valid UTF16. It assumes that all
   code points are the same size and does not validate any of them, nor does
   it attempt to detect (invalid) switches between big-endian and little-endian.
 */
FSL_EXPORT int fsl_looks_like_utf16(fsl_buffer const * pContent,
                                    bool reverse, int stopFlags);

/**
   Returns true if b'c contents begin with a UTF-16 BOM. If pBomSize is not NULL
   then it is set to the byte length of the UTF-16 BOM (2 bytes). If isReversed
   is not NULL, it gets assigned to true if the input appears to be in little-endian
   format and false if it appears to be in big-endian format.
*/
FSL_EXPORT bool fsl_starts_with_bom_utf16(fsl_buffer const * b, unsigned int *pBomSize,
                                          bool * isReversed);
/**
   Returns true if b's content _might_ be valid UTF-16. If the 2nd argument is not NULL,
   it gets set to true if the input appears to be little-endian and false if it appears
   to be big-endian.
*/
FSL_EXPORT bool fsl_might_be_utf16(fsl_buffer const * b, bool * isReversed);
#endif
/* ^^^^ UTF-16 interfaces */

#if defined(__cplusplus)
} /*extern "C"*/
#endif
#endif
/* ORG_FOSSIL_SCM_FSL_UTIL_H_INCLUDED */
