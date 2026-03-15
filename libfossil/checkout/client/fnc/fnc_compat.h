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

#ifdef __linux__

#ifndef _BSD_SOURCE
#define _BSD_SOURCE		/* mkstemps(3) on glibc <= 2.19 */
#endif

/*
 * Linux siginfo_t needs _POSIX_C_SOURCE >= 199309L but feature_test_macros(7)
 * claims _POSIX_C_SOURCE is defined with 200809L when _XOPEN_SOURCE >= 700,
 * and curses(3) recommends defining _XOPEN_SOURCE to 700 for curses wide-char.
 */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700	/* sigaction(2) and sigemptyset(3) */
#endif

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE		/* strsep() on glibc >= 2.19. */
#endif

#ifdef __has_include

#if __has_include("bsd/string.h")
#include <bsd/string.h>

#define HAVE_BSD_STRING
#endif

#if __has_include("linux/landlock.h")
#define HAVE_LANDLOCK
#endif

#endif  /* __has_include */
#endif  /* __linux__ */

#ifdef __NetBSD__
#define _OPENBSD_SOURCE		/* strtonum(3) */
#endif

#if !defined(__linux__) && !defined(__APPLE__) && !defined(__HAIKU__)
#define HAVE_REALLOCARRAY
#define HAVE_BSD_STRING
#define HAVE_STRTONUM
#endif

#if defined(__APPLE__)
#include <Availability.h>

#define HAVE_BSD_STRING

/*
 * strtonum(3) was added to macOS 11.0 (cf. availability directives):
 *   https://developer.apple.com/library/archive/documentation/	\
 *     DeveloperTools/Conceptual/cross_development/Using/	\
 *     using.html#//apple_ref/doc/uid/20002000-1114741-CJADDEIB
 */
#ifdef __MAC_OS_X_VERSION_MAX_ALLOWED
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 110000
#define HAVE_STRTONUM
#endif
#endif  /* __MAC_OS_X_VERSION_MAX_ALLOWED */
#endif  /* __APPLE__ */

#if defined(__HAIKU__)
#define _DEFAULT_SOURCE
#define HAVE_BSD_STRING
#define HAVE_STRTONUM
#include <BeBuild.h>
#if B_HAIKU_VERSION > B_HAIKU_VERSION_1_BETA_5
#define HAVE_REALLOCARRAY
#endif
/* Missing macros*/
#define	NBBY	8
#define	setbit(a,i)	(((unsigned char *)(a))[(i)/NBBY] |= 1<<((i)%NBBY))
#define	clrbit(a,i)	(((unsigned char *)(a))[(i)/NBBY] &= ~(1<<((i)%NBBY)))
#define	isset(a,i)	(((const unsigned char *)(a))[(i)/NBBY] & (1<<((i)%NBBY)))
#endif  /* __HAIKU__ */

#ifndef __predict_true
#ifdef __has_builtin
#if __has_builtin(__builtin_expect)
#define __predict_true(_e)	__builtin_expect(((_e) != 0), 1)
#define __predict_false(_e)	__builtin_expect(((_e) != 0), 0)
#endif
#endif  /* __has_builtin */
#endif  /* __predict_true */

#ifndef __predict_true
#define __predict_true(_e)	((_e) != 0)
#define __predict_false(_e)	((_e) != 0)
#endif  /* __predict_true */

#if defined(__linux__)
#include <features.h>

#ifdef _DEFAULT_SOURCE
#if !defined(__GLIBC__) || __GLIBC__ > 2 ||	\
    (__GLIBC__ == 2 && __GLIBC_MINOR__ > 28)
/* glibc >= 2.29 and musl provide reallocarray(3) with _DEFAULT_SOURCE */
#define HAVE_REALLOCARRAY
#endif

#elif defined(_GNU_SOURCE)  /* _DEFAULT_SOURCE */
#if !defined(__GLIBC__) || __GLIBC__ < 2 ||	\
    (__GLIBC__ == 2 && __GLIBC_MINOR__ < 29)
/* glibc <= 2.28 and musl provide reallocarray(3) with _GNU_SOURCE */
#define HAVE_REALLOCARRAY
#endif
#endif  /* _DEFAULT_SOURCE */

#endif  /* __linux__ */

#ifndef HAVE_STRTONUM
long long strtonum(const char *, long long, long long, const char **);
#endif

#ifndef HAVE_REALLOCARRAY
#include <stddef.h>	/* size_t */

void *reallocarray(void *, size_t, size_t);
#endif
