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

#if !defined(_POSIX_C_SOURCE) || (_POSIX_C_SOURCE - 0) < 200112L
#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE	200112L  /* strerror_r(3) on linux */
#endif

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "error.h"
#include "libfossil.h"

#ifndef nitems
#define nitems(_a) (sizeof(_a) / sizeof((_a)[0]))
#endif

static struct fnc_error fnc_errors[] = {
#define ERR_(_rc, _msg)	{ _rc, _msg, "" }
	GENERATE_ERROR_MAP
#undef ERR_
};

const char *
fnc_strerror(int rc)
{
	struct fnc_error	*e = NULL;
	size_t			 i;

	if (rc >= FSL_RC_ERROR)
		rc = FNC_RC_LIBFOSSIL;

	for (i = 0; i < nitems(fnc_errors); ++i) {
		if (rc == fnc_errors[i].rc) {
			e = &fnc_errors[i];
			if (*e->buf != '\0')
				return e->buf;
			if (e->msg != NULL)
				return e->msg;
			break;
		}
	}

	return fnc_strerror(FNC_RC_FATAL);
}

const char *
fnc_errorf(int rc, const char *fmt, ...)
{
	const struct fnc_error	*err = NULL;
	static char		 msg[FNC_ERR_MSG_BUFSZ + sizeof(": ")];
	char			 pfx[FNC_ERR_PREFIX_SZ];
	va_list			 args;
	size_t			 i;
	int			 n = 0;

	memset(&pfx, 0, sizeof(pfx));

	va_start(args, fmt);
	vsnprintf(pfx, sizeof(pfx), fmt, args);
	va_end(args);

	for (i = 0; i < nitems(fnc_errors); ++i) {
		if (rc == fnc_errors[i].rc) {
			err = &fnc_errors[i];
			if (err->msg != NULL) {
				/*
				 * We know strlen(err->msg) is < sizeof(msg)
				 * but want n for the next snprintf(3) call.
				 */
				n = snprintf(msg, sizeof(msg), "%s", err->msg);
				if (n < 0)
					n = 0;
			}
			break;
		}
	}

	if (*pfx != '\0') {
		/* ignore truncation */
		snprintf(&msg[n], sizeof(msg) - n, "%s%s",
		    err->msg != NULL ? ": " : "", pfx);
	}
	return msg;
}

const char *
fnc_error_from_errno(const char *fmt, ...)
{
	static char	msg[FNC_ERR_MSG_BUFSZ + sizeof(": ")];
	char		pfx[FNC_ERR_PREFIX_SZ], errstr[FNC_ERRNO_STR_SZ];
	va_list		args;

	memset(&pfx, 0, sizeof(pfx));

	va_start(args, fmt);
	vsnprintf(pfx, sizeof(pfx), fmt, args);
	va_end(args);

	strerror_r(errno, errstr, sizeof(errstr));
	snprintf(msg, sizeof(msg), "%s%s%s",
	    pfx, *pfx != '\0' ? ": " : "", errstr);
	return msg;
}

const char *
fnc_error_from_libf(int rc, const char *fmt, ...)
{
	static char	 msg[FNC_ERR_MSG_BUFSZ + sizeof(": ")];
	char		 pfx[FNC_ERR_PREFIX_SZ];
	const char	*e;
	va_list		 args;

	memset(&pfx, 0, sizeof(pfx));

	va_start(args, fmt);
	vsnprintf(pfx, sizeof(pfx), fmt, args);
	va_end(args);

	fsl_error_get(fcli_error(), &e, NULL);
	if (e == NULL)
		e = fsl_rc_cstr(rc);

	snprintf(msg, sizeof(msg), "%s%s%s", pfx, *pfx != '\0' ? ": " : "", e);
	return msg;
}

int
fnc_error_set(int rc, const char *fmt, ...)
{
	va_list		ap;
	size_t		i;
	int		ret = rc;

	if (rc >= FSL_RC_ERROR)
		rc = FNC_RC_LIBFOSSIL;

	for (i = 0; i < nitems(fnc_errors); ++i) {
		if (fnc_errors[i].rc == rc) {
			struct fnc_error *e = &fnc_errors[i];

			va_start(ap, fmt);
			vsnprintf(e->buf, sizeof(e->buf), fmt, ap);
			va_end(ap);

			return ret;
		}
	}

	abort();
}

int
fnc_error_reset(void)
{
	struct fsl_cx	*f;
	size_t		 i;

	f = fcli_cx();
	if (f != NULL)
		fsl_cx_err_reset(f);
	fcli_err_reset();

	for (i = 0; i < nitems(fnc_errors); ++i)
		memset(fnc_errors[i].buf, 0, sizeof(fnc_errors[i].buf));

	return FNC_RC_OK;
}
