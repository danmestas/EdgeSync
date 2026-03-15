/*
 * Copyright (c) 2022, 2023 Mark Jamsek <mark@bsdbox.org>
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
 * Flags set by callers of the below diff APIs to determine diff output.
 */
enum fnc_diff_flag {
	FNC_DIFF_IGNORE_EOLWS	= 0x01,
	FNC_DIFF_IGNORE_ALLWS	= 0x03,
	FNC_DIFF_SIDEBYSIDE	= (1 << 2),
	FNC_DIFF_VERBOSE	= (1 << 3),  /* show added/rm'd file content */
	FNC_DIFF_BRIEF		= (1 << 4),  /* no content, just index lines */
	FNC_DIFF_HTML		= (1 << 5),
	FNC_DIFF_LINENO		= (1 << 6),  /* show file line numbers */
	FNC_DIFF_STATMIN	= (1 << 7),  /* show minimal diffstat */
	FNC_DIFF_NOOPT		= (1 << 8),  /* no optimisations (debug) */
	FNC_DIFF_INVERT		= (1 << 9),
	FNC_DIFF_PROTOTYPE	= (1 << 10), /* show scope in hunk header */
	FNC_DIFF_NOTTOOBIG	= (1 << 11), /* don't compute "large" diffs */
	FNC_DIFF_STRIP_EOLCR	= (1 << 12), /* strip trailing '\r' */
	FNC_DIFF_ANSI_COLOR	= (1 << 13)
#define FNC_DIFF_CONTEXT_EX	(((uint64_t)0x04) << 32) /* allow 0 context */
#define FNC_DIFF_CONTEXT_MASK	((uint64_t)0x0000ffff)	 /* default context */
#define FNC_DIFF_WIDTH_MASK	((uint64_t)0xffff0000)	 /* SBS column width */
};

enum line_type {
	LINE_BLANK,
	LINE_TIMELINE_HEADER,
	LINE_TIMELINE_COMMIT,
	LINE_DIFF_ARTIFACT,
	LINE_DIFF_USER,
	LINE_DIFF_TAGS,
	LINE_DIFF_DATE,
	LINE_DIFF_COMMENT,
	LINE_DIFF_CHANGESET,
	LINE_DIFF_INDEX,
	LINE_DIFF_HEADER,
	LINE_DIFF_META,
	LINE_DIFF_MINUS,
	LINE_DIFF_PLUS,
	LINE_DIFF_EDIT,
	LINE_DIFF_CONTEXT,
	LINE_DIFF_HUNK,
	LINE_DIFF_SEPARATOR
};

/*
 * Compute the diff of changes to convert the file in fsl_buffer parameter 2
 * to the file in fsl_buffer parameter 3 and save the result to the provided
 * output fsl_buffer in parameter 1.
 *
 * A unified diff is output by default. Alternatively, a side-by-side diff
 * along with other changes (documented in the fnc_diff_flag enum) can be
 * produced by setting the corresponding flags passed in int parameter 9.

 * If not NULL, the enum array pointer in paramater 4 and size_t pointer in
 * parameter 5 will be populated with each line_type and the total number of
 * lines written out to the diff, respectively. The enum array and output
 * buffer can be prepopulated with the size_t value inidicating the prefilled
 * line count; the former two must be disposed of by the caller. The uint64_t
 * pointer in parameter 6 will be encoded with the total number of added and
 * removed lines in the low and high 32 bits, respectively.
 *
 * If a unified diff, parameter 8 is ignored, and the number of context lines
 * is specified in short parameter 7; negative values fallback to default. If
 * a side-by-side diff, parameter 7 is ignored, and the column width of each
 * side is specified in short parameter 8; only values larger than the longest
 * line are honoured, otherwise the column width of each side will be sized
 * large enough to accommodate the longest line in the diff.
 */
int fnc_diff_text_to_buffer(fsl_buffer *, const fsl_buffer *,
    const fsl_buffer *, enum line_type **, size_t *, uint64_t *,
    short, short, int);

/*
 * Compute the diff of changes to convert the file in fsl_buffer parameter 1
 * to the file in fsl_buffer parameter 2 and invoke the fsl_output_f callback
 * in parameter 3 for each computed line.  The callback receives the provided
 * void parameter 4 as its output state and a char pointer of the diffed line
 * or diff metadata (e.g., hunk header, index).  Remaining parameters are the
 * same as the above fnc_diff_text_to_buffer() routine.
 */
int fnc_diff_text(const fsl_buffer *, const fsl_buffer *, fsl_output_f, void *,
    enum line_type **, size_t *, uint64_t *, short, short, int);

/*
 * Assign to the double pointer to int out parameter 1 the copy/delete/insert
 * triples array describing the sequence of changes to convert the contents of
 * fsl_buffer parameter 2 to those of fsl_buffer parameter 3. Callers assume
 * ownership of the dynamically allocated triples, which is terminated with
 * three contiguous zero sentinel values. Formatting options are set via the
 * bitwise OR of enum fnc_diff_flag values passed in int parameter 4.
 * Each triple of integers means:
 *   T[0,3,6,..,n-3] (copy):   number of lines both buffers have in common
 *   T[1,4,7,..,n-2] (delete): number of lines found only in buffer 1
 *   T[2,5,8,..,n-1] (insert): number of lines found only in buffer 2
 * Triples repeat until all lines from both buffers are consumed.
 * The array is terminated with a sentinel triple of three zeros: { 0,0,0 }
 */
int fnc_diff_text_raw(int **, const fsl_buffer *, const fsl_buffer *, int);

/*
 * Save the line_type specified in parameter 3 to the nth index denoted by
 * the size_t pointer in parameter 2 of the line_type array in parameter 1.
 * The size_t index pointed to by the 2nd parameter will be incremented.
 */
int add_line_type(enum line_type **, size_t *, enum line_type);
