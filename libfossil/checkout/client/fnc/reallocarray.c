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

#include "fnc_compat.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef HAVE_REALLOCARRAY

#ifndef LONG_BIT
/*
 * LONG_BIT should be defined in limits.h; if not, compute with the
 * following macro courtesy of Hallvard B Furuseth via comp.lang.c:
 * http://groups.google.com/group/comp.lang.c/msg/e998153ef07ff04b
 */
#define IMAX_BITS(_m)	((_m) / ((_m) % 255 + 1) / 255 % 255 * 8 + 7 - 86 / \
			    ((_m) % 255 + 12))

#define LONG_BIT	IMAX_BITS(ULONG_MAX)
#endif  /* LONG_BIT */

#if LONG_BIT > 32
typedef __uint128_t	big_uint_t;
# else
typedef uint64_t	big_uint_t;
#endif  /* LONG_BIT > 32 */

#ifdef __has_builtin
#if __has_builtin(__builtin_mul_overflow)
#define mul_overflow(_n, _m, _ret)	__builtin_mul_overflow(_n, _m, _ret)
#endif
#endif

#ifndef mul_overflow
static inline int
mul_overflow(unsigned long n, unsigned long m, unsigned long *ret)
{
	big_uint_t product = (big_uint_t)n * (big_uint_t)m;

	*ret = (unsigned long)product;

	return (product >> LONG_BIT) != 0;
}
#endif  /* mul_overflow */

void *
reallocarray(void *ptr, size_t n, size_t sz)
{
	unsigned long product;

	if (__predict_false(mul_overflow(n, sz, &product)) != 0) {
		errno = ENOMEM;
		return NULL;
	}

	return realloc(ptr, product);
}

#endif  /* HAVE_REALLOCARRAY */
