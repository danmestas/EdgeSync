/*
 * Copyright (c) 2021, 2022, 2023 Mark Jamsek <mark@jamsek.com>
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
 * Set fnc__progname to getprogname(3) or program_invocation_short_name(3)
 * on BSD and linux, respectively. Ignore truncation if it inexplicably
 * exceeds PATH_MAX. Otherwise, take the basename of argv[0] unless it
 * is larger than PATH_MAX, in which case fallback to a hardcoded "fnc".
 */
#if defined(__OpenBSD__) || defined(__FreeBSD__) || defined(__APPLE__)
#define fnc_progname(_arg)	memccpy(fnc__progname, getprogname(),	\
				    '\0', sizeof(fnc__progname))
#elif defined(_GNU_SOURCE)
extern char *program_invocation_short_name;

#define fnc_progname(_arg)	memccpy(fnc__progname,			\
				    program_invocation_short_name,	\
				    '\0', sizeof(fnc__progname))
#else
#define fnc_progname(_arg) do {						\
	const char *_p = "fnc";						\
									\
	if (_arg != NULL && *_arg != '\0') {				\
		size_t		 _len;					\
		const char	*_s, *_t;				\
									\
		_s = _t = _arg + strlen(_arg) - 1;			\
									\
		while (_s > _arg && *(_s - 1) != '/')			\
			--_s;						\
		_len = _t - _s + 1;					\
		if (_len < sizeof(fnc__progname))			\
			_p = _s;					\
	}								\
	memccpy(fnc__progname, _p, '\0', sizeof(fnc__progname));	\
} while (0)
#endif  /* __OpenBSD__ || __FreeBSD__ || __APPLE__ */

#ifdef FCLI_USE_SIGACTION
#define FCLI_USE_SIGACTION 0	/* we want ^c to exit */
#endif

/* utility macros */
#ifndef MIN
#define MIN(_a, _b)		((_a) < (_b) ? (_a) : (_b))
#endif

#ifndef MAX
#define MAX(_a, _b)		((_a) > (_b) ? (_a) : (_b))
#endif

#define ABS(_n)			((_n) >= 0 ? (_n) : -(_n))

#ifndef CTRL
#define CTRL(key)		((key) & 037)	/* ^key input */
#endif

#ifndef nitems
#define nitems(_a)		(sizeof((_a)) / sizeof((_a)[0]))
#endif

#define ndigits(_d, _n)		do { _d++; } while (_n /= 10)

#define XCONCAT(_a, _b)		_a ## _b
#define CONCAT(_a, _b)		XCONCAT(_a, _b)

#ifndef STRINGIFY
#define XSTRINGIFY(_s)		#_s
#define STRINGIFY(_s)		XSTRINGIFY(_s)
#endif

#define FLAG_SET(_f, _b)	((_f) |= (_b))
#define FLAG_CHK(_f, _b)	((_f) & (_b))
#define FLAG_TOG(_f, _b)	((_f) ^= (_b))
#define FLAG_CLR(_f, _b)	((_f) &= ~(_b))

#define nbytes(_nbits)		(((_nbits) + 7) >> 3)

#define PRINTFV(fmt, args)	__attribute__((format (printf, fmt, args)))

/* application macros */
#define PRINT_VERSION	STRINGIFY(FNC_VERSION)
#define PRINT_HASH	STRINGIFY(FNC_HASH)
#define PRINT_DATE	STRINGIFY(FNC_DATE)

#define DIFF_TOO_MANY_CHANGES	"diff has too many changes"
#define DIFF_FILE_BINARY	"binary files cannot be diffed"
#define DIFF_FILE_MISSING	"file (artifact) missing from tree"

#define DEF_DIFF_CTX	5		/* default diff context lines */
#define MAX_DIFF_CTX	64		/* max diff context lines */

#define HSPLIT_SCALE	0.3f		/* default horizontal split scale */

#define SPINNER		"\\|/-\0"
#define SPIN_INTERVAL	200		/* status line progress indicator */

#define LINENO_WIDTH	6		/* view lineno max column width */
#define MAX_PCT_LEN	7		/* line position upto max len 99.99% */

#define KEY_ESCAPE	27

#ifndef __dead
#define __dead	__attribute__((noreturn))
#endif

#ifndef TAILQ_FOREACH_SAFE
/* rewrite of OpenBSD 6.9 sys/queue.h for linux builds */
#define TAILQ_FOREACH_SAFE(var, head, field, tmp)			\
	for ((var) = ((head)->tqh_first);				\
		(var) != (NULL) && ((tmp) = TAILQ_NEXT(var, field), 1);	\
		(var) = (tmp))
#endif  /* TAILQ_FOREACH_SAFE */

#ifndef STAILQ_FOREACH_SAFE
#define STAILQ_FOREACH_SAFE(var, head, field, tmp)			\
	for ((var) = ((head)->stqh_first);				\
		(var) && ((tmp) = STAILQ_NEXT(var, field), 1);		\
		(var) = (tmp))
#endif  /* STAILQ_FOREACH_SAFE */

/* XXX OpenBSD added STAILQ in 6.9; fallback to SIMPLEQ for prior versions. */
#if defined(__OpenBSD__) && !defined(STAILQ_HEAD)
#define SQ(_do)	CONCAT(SIMPLEQ ## _, _do)
#else
#define SQ(_do)	CONCAT(STAILQ ## _, _do)
#endif  /* __OpenBSD__ */

#ifndef HAVE_BSD_STRING
#define strlcat(_d, _s, _sz) fsl_strlcat(_d, _s, _sz)
#define strlcpy(_d, _s, _sz) fsl_strlcpy(_d, _s, _sz)
#endif /* HAVE_BSD_STRING */
