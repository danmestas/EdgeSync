/*
 * Copyright (c) 2023 Mark Jamsek <mark@jamsek.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Error strings shall not exceed FNC_ERR_PREFIX_SZ.
 * Result codes shall not exceed 100, which is where
 * libfossil codes begin (although this is not guaranteed).
 */
#define GENERATE_ERROR_MAP \
	ERR_(FNC_RC_OK,			NULL),					\
	ERR_(FNC_RC_BREAK,		NULL), /* non-error early return/exit */\
	ERR_(FNC_RC_SITREP,		NULL), /* non-error situation report */	\
	ERR_(FNC_RC_ERROR,		"undefined error"),			\
	ERR_(FNC_RC_ERRNO,		NULL),	/* see RC_ERRNO() */		\
	ERR_(FNC_RC_CANCELED,		"operation in progress canceled"),	\
	ERR_(FNC_RC_NO_CKOUT,		"no work tree found"),			\
	ERR_(FNC_RC_NO_REPO,		"no fossil repository found"),		\
	ERR_(FNC_RC_NO_MATCH,		"no matches found"),			\
	ERR_(FNC_RC_BAD_PATH,		"invalid pathname"),			\
	ERR_(FNC_RC_NO_PATH,		"no such pathname"),			\
	ERR_(FNC_RC_NO_TREE_ENTRY,	"no such entry found in tree"),		\
	ERR_(FNC_RC_NO_BRANCH,		"no such branch"),			\
	ERR_(FNC_RC_NO_USER,		"no such user"),			\
	ERR_(FNC_RC_NO_TAG,		"no such tag"),				\
	ERR_(FNC_RC_NO_RID,		"no such database rid"),		\
	ERR_(FNC_RC_NO_COMMIT,		"no such commit"),			\
	ERR_(FNC_RC_NO_REF,		"no such reference"),			\
	ERR_(FNC_RC_BAD_HASH,		"invalid SHA hash"),			\
	ERR_(FNC_RC_BAD_ARTIFACT,	"bad artifact type"),			\
	ERR_(FNC_RC_AMBIGUOUS_ID,	"ambiguous artifact id"),		\
	ERR_(FNC_RC_EMPTY_TREE,		"tree is empty"),			\
	ERR_(FNC_RC_IO,			"input/output or encoding error"),	\
	ERR_(FNC_RC_EOF,		"unexpected end of file"),		\
	ERR_(FNC_RC_NO_SPACE,		"buffer too small"),			\
	ERR_(FNC_RC_RANGE,		"value out of range"),			\
	ERR_(FNC_RC_BAD_NUMBER,		"invalid number"),			\
	ERR_(FNC_RC_BAD_OPTION,		"invalid option"),			\
	ERR_(FNC_RC_BAD_CMD,		"invalid command"),			\
	ERR_(FNC_RC_AMBIGUOUS_CMD,	"ambiguous command"),			\
	ERR_(FNC_RC_CKOUT_BUSY,		"checkout database is locked"),		\
	ERR_(FNC_RC_DIFF_BINARY,	"cannot diff binary file"),		\
	ERR_(FNC_RC_BLAME_BINARY,	"cannot blame binary file"),		\
	ERR_(FNC_RC_AMBIGUOUS_DATE,	"ambiguous date"),			\
	ERR_(FNC_RC_BAD_DATE,		"invalid date"),			\
	ERR_(FNC_RC_BAD_KEYWORD,	"invalid keyword"),			\
	ERR_(FNC_RC_REGEX,		"regular expression error"),		\
	ERR_(FNC_RC_PATCH_MALFORMED,	"malformed patch file"),		\
	ERR_(FNC_RC_PATCH_TRUNCATED,	"truncated patch file"),		\
	ERR_(FNC_RC_NO_PATCH,		"no diff found in patch file"),		\
	ERR_(FNC_RC_HUNK_FAILED,	"patch hunk failed to apply"),		\
	ERR_(FNC_RC_PATCH_FAILED,	"patch failed to apply"),		\
	ERR_(FNC_RC_CURSES,		"fatal curses error"),			\
	ERR_(FNC_RC_NYI,		"feature is not implemented"),		\
	ERR_(FNC_RC_NOSUPPORT,		"operation not supported"),		\
	ERR_(FNC_RC_FATAL,		"unexpected fatality"),			\
	ERR_(FNC_RC_LIBFOSSIL,		"fatal libfossil error")

enum fnc_err_code {
#define ERR_(_rc, _msg)	_rc
	GENERATE_ERROR_MAP
#undef ERR_
};

/*
 * Private implementation.
 */
#define XSTRINGIFY(_s)		#_s
#define STRINGIFY(_s)		XSTRINGIFY(_s)
#define FILE_POSITION		__FILE__ ":" STRINGIFY(__LINE__)

#define FNC_ERRNO_STR_SZ	512
#define FNC_ERR_PREFIX_SZ	512
#define FNC_ERR_MSG_BUFSZ	(FNC_ERR_PREFIX_SZ + FNC_ERRNO_STR_SZ)

#if DEBUG
#define RC_SET(_r, _fmt, ...) \
	fnc_error_set(_r, "%s::%s " _fmt, __func__, FILE_POSITION, __VA_ARGS__)
#else
#define RC_SET(_r, _fmt, ...)	fnc_error_set(_r, _fmt,	__VA_ARGS__)
#endif /* DEBUG */

#define RCX(_r, ...)							\
	_r == FNC_RC_ERRNO ? RC_ERRNO(__VA_ARGS__) :			\
	    _r == FNC_RC_BREAK ? RC_BREAK(__VA_ARGS__) :		\
	    _r >= (int)FSL_RC_ERROR ? RC_LIBF(_r, __VA_ARGS__) :	\
	    RC_SET(_r, "%s", fnc_errorf(_r, __VA_ARGS__))

#define RC_LIBFX(_r, ...) \
	_r != 0 ? RC_SET(_r, "%s", fnc_error_from_libf(_r, __VA_ARGS__)) : 0

/*
 * Public API.
 */
struct fnc_error {
	int		 rc;
	const char	*msg;
	char		 buf[FNC_ERR_MSG_BUFSZ];
};

/*
 * Set the error state for result code rc with a message built from the
 * optional provided format string and variable length argument list,
 * which will be prefixed with rc's corresponding default error message
 * from the below error map. In debug builds, this will include the filename
 * and line number of the caller. FNC_RC_ERRNO, FNC_RC_BREAK, and fsl_rc_e
 * FSL_RC_* enum result codes are special cases with slightly different
 * semantics per the below documented RC_ERRNO, RC_BREAK, and RC_LIBF API.
 *	RC(rc)
 *	RC(rc, fmt, ...)
 */
#define RC(...)		RCX(__VA_ARGS__, "", 0)

/*
 * Set the error state for FNC_RC_ERRNO, with a message built from the
 * provided format string and variable length argument list, which will
 * be suffixed with an errno error string obtained from strerror(3).
 */
#define RC_ERRNO(...) \
	RC_SET(FNC_RC_ERRNO, "%s", fnc_error_from_errno(__VA_ARGS__))

/*
 * Set errno to _e and the error state for FNC_RC_ERRNO, with a message built
 * from the provided format string and variable length argument list, which
 * will be suffixed with an errno error string obtained from strerror(3).
 */
#define RC_ERRNO_SET(_e, ...) \
	((errno=_e), \
	RC(_e != 0 ? FNC_RC_ERRNO : FNC_RC_ERROR, "%s", __VA_ARGS__))

/*
 * Set the error state based on the error indicator for file stream _f.
 * If ferror(3) returns non-zero, use RC_ERRNO() to create an error object.
 * Otherwise, use RC() as documented above with the provided _rc error code.
 */
#define RC_FERROR(_f, _rc, ...) \
	ferror(_f) ? RC_ERRNO(__VA_ARGS__) : RC(_rc, __VA_ARGS__)

/*
 * Set the error state for FNC_RC_BREAK, a special case used to report
 * that the requested operation cannot proceed due to unmet preconditions
 * (e.g., different artifacts selected from the timeline view to diff),
 * but will not fatally error. This is often used to report such cases
 * before resuming the view loop, and to return zero on exit with a message
 * built from the provided format string and variable length argument list.
 */
#define RC_BREAK(...)	RC_SET(FNC_RC_BREAK, __VA_ARGS__, "")

/*
 * Set the error state for FNC_RC_LIBFOSSIL with a message built from the
 * provided format string and variable length argument list, prefixed by
 * libfossil's error message for rc, where rc is a valid fsl_rc_e FSL_RC_*
 * enum error code, obtained from fsl_error_get() or fsl_rc_cstr().
 *	RC_LIBF(rc)
 *	RC_LIBF(rc, fmt, ...)
 */
#define RC_LIBF(...)	RC_LIBFX(__VA_ARGS__, "", 0)

/*
 * For libfossil APIs that fail without returning an error code such as
 * fsl_uuid_to_rid(). Create an error object using the error code returned
 * from libf's fcli_error() API if it is set, otherwise use the provided _rc.
 */
#define RC_FCLI(_rc, ...)						\
	fcli_error()->code != 0 ?					\
	RC_LIBF(fcli_error()->code, __VA_ARGS__) : RC(_rc, __VA_ARGS__)

/*
 * Create an error object using the error code attached to the provided
 * _db object if it is set, otherwise use the provided _rc error code.
 */
#define RC_DB(_db, _rc, ...)						\
	_db != NULL && _db->error.code != 0 ?				\
	RC_LIBF(_db->error.code, __VA_ARGS__) : RC(_rc, __VA_ARGS__)

/*
 * Retrieve the error corresponding to result code _e and return either
 * its custom error string if it is set otherwise its default error string.
 * If _e is not a valid FNC_RC_* result code or it does not have a custom
 * or default error string set, return the FNC_RC_FATAL error string.
 */
#define RCSTR(_e)	fnc_strerror(_e)

/* Clear and reset any custom error buffers and all libfossil error state. */
#define RC_RESET	fnc_error_reset()

const char		*fnc_errorf(int, const char *, ...);
const char		*fnc_error_from_errno(const char *, ...);
const char		*fnc_error_from_libf(int, const char *, ...);
const char		*fnc_strerror(int);
int			 fnc_error_set(int, const char *, ...);
int			 fnc_error_reset(void);
