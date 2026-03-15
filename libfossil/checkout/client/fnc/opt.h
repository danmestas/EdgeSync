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

#define GLOBAL_HELP(_)								\
	_( 'h', "help", NULL, "Display program help and usage then exit.",	\
	    no_argument ),							\
	_( 'v', "version", NULL, "Display program version number then exit.",	\
	    no_argument ),

#define BLAME_HELP(_)								\
	_( 'C', "no-colour", NULL,						\
	    "Disable colourised blame. Toggle at runtime with 'C' if this\n\t"	\
	    "option is not used.",						\
	    no_argument ),							\
	_( 'c', "commit", "commit",						\
	    "Blame file as at <commit>. Common symbols are:"			\
	    "\n\t    SHA{1,3} hash"						\
	    "\n\t    SHA{1,3} unique prefix"					\
	    "\n\t    branch"							\
	    "\n\t    tag:TAG"							\
	    "\n\t    root:BRANCH"						\
	    "\n\t    ISO8601 date"						\
	    "\n\t    ISO8601 timestamp"						\
	    "\n\t    {tip,current,prev,next}\n\t"				\
	    "For a complete list of symbols see Fossil's Check-in Names:\n\t"	\
	    "https://fossil-scm.org/home/doc/trunk/www/checkin_names.wiki",	\
	    required_argument ),						\
	_( 'h', "help", NULL, "Display blame command help and usage.",		\
	    no_argument ),							\
	_( 'l', "line", "lineno", "Open annotated file at <lineno>.",		\
	    required_argument ),						\
	_( 'n', "limit", "n",							\
	    "Limit blame history to <n> commits or seconds denoted with a\n\t"	\
	    "postfixed 's' (e.g., 30s).",					\
	    required_argument ),						\
	_( 'R', "reverse", NULL,						\
	    "Reverse annotate with the first time each line was changed after "	\
	    "\n\tthe specified commit. Requires --commit.",			\
	    no_argument ),							\
	_( 'r', "repo", "path", "Use the Fossil repository at <path>.",		\
	    required_argument ),						\
	_( 'z', "utc", NULL, "Use UTC (instead of local) time.", no_argument ),

#define BRANCH_HELP(_) \
	_( 'C', "no-colour", NULL,						\
	    "Disable colourised branch view. Toggle at runtime with 'C' if "	\
	    "\n\tthis option is not used.",					\
	    no_argument ),							\
	_( 'a', "after", "date",						\
	    "Show branches active after <date>, which must be either an\n\t"	\
	    "ISO 8601 extended format complete date (e.g., 2020-10-10) or\n\t"	\
	    "an unambiguous DD/MM/YYYY or MM/DD/YYYY formatted date.",		\
	    required_argument ),						\
	_( 'b', "before", "date", "Like -a|--after but active before <date>.",	\
	    required_argument ),						\
	_( 'c', "closed", NULL, "Show closed branches only.", no_argument ),	\
	_( 'h', "help", NULL, "Display branch command help and usage.",		\
	    no_argument ),							\
	_( 'o', "open", NULL, "Show open branches only.", no_argument ),	\
	_( 'p', "no-private", NULL, "Do not show private branches.",		\
	    no_argument ),							\
	_( 'R', "reverse", NULL, "Reverse the display order of branches.",	\
	    no_argument ),							\
	_( 'r', "repo", "path", "Use the Fossil repository at <path>.",		\
	    required_argument ),						\
	_( 's', "sort", "order", "Sort branches in <order>:"			\
	    "\n\t    mru   - most recently used"				\
	    "\n\t    state - open/closed state"					\
	    "\n\tBranches are lexicographically sorted by default.",		\
	    required_argument ),						\
	_( 'z', "utc", NULL, "Use UTC (instead of local) time.", no_argument ),

#define CONFIG_HELP(_)								\
	_( 'h', "help", NULL, "Display config command help and usage.",		\
	    no_argument ),							\
	_( 'l', "ls", NULL, "Display all available options.", no_argument ),	\
	_( 'r', "repo", "path", "Use the Fossil repository at <path>.",		\
	    required_argument ),						\
	_( 'u', "unset", "option", "Unset the named repository option.",	\
	    required_argument ),

#define DIFF_HELP(_)								\
	_( 'b', "brief", NULL,							\
	    "Display index and hash lines only. Toggle at runtime with 'B'.",	\
	    no_argument ),							\
	_( 'C', "no-colour", NULL,						\
	    "Disable colourised diff. Toggle at runtime with 'C' if this\n\t"	\
	    "option is not used.",						\
	    no_argument ),							\
	_( 'D', "min-diffstat", NULL,						\
	    "Show minimal vice histogram diffstat. Toggle at runtime with 'D'.",\
	    no_argument ),							\
	_( 'h', "help", NULL, "Display diff command help and usage.",		\
	    no_argument ),							\
	_( 'i', "invert", NULL,							\
	    "Invert difference between artifacts. Toggle at runtime with 'i'.",	\
	    no_argument ),							\
	_( 'l', "line-numbers", NULL,						\
	    "Show file line numbers. Can be toggled at runtime with 'L'.",	\
	    no_argument ),							\
	_( 'o', "no-curses", NULL,						\
	    "Write diff directly to the standard output.",			\
	    no_argument ),							\
	_( 'P', "no-prototype", NULL,						\
	    "Do not display function prototypes in hunk header.",		\
	    no_argument ),							\
	_( 'q', "quiet", NULL,							\
	    "Do not show added or removed file content. Toggle at runtime\n\t"	\
	    "with 'v'.",							\
	    no_argument ),							\
	_( 'r', "repo", "path", "Use the Fossil repository at <path>.",		\
	    required_argument ),						\
	_( 's', "sbs", NULL,							\
	    "Show side-by-side formatted diff. Toggle at runtime with 'S'.",	\
	    no_argument ),							\
	_( 'W', "wrap", NULL,							\
	    "Wrap lines longer than view width. Toggle at runtime with 'W'.",	\
	    no_argument ),							\
	_( 'w', "whitespace", NULL,						\
	    "Ignore whitespace-only changes. Toggle at runtime with 'w'.",	\
	    no_argument ),							\
	_( 'x', "context", "n", "Show <n> context lines (0 <= n <= 64).",	\
	    required_argument ),						\
	_( 'z', "utc", NULL, "Use UTC (instead of local) time.", no_argument ),

#define STASH_HELP(_)								\
	_( 'C', "no-colour", NULL,						\
	    "Disable colourised stash view. Toggle at runtime with 'C' if\n\t"	\
	    "this option is not used.",						\
	    no_argument ),							\
	_( 'h', "help", NULL, "Display stash command help and usage.",		\
	    no_argument ),							\
	_( 'P', "no-prototype", NULL,						\
	    "Do not display function prototypes in hunk header.",		\
	    no_argument ),							\
	_( 'x', "context", "n", "Show <n> context lines (0 <= n <= 64).",	\
	    required_argument ),

#define TIMELINE_HELP(_)							\
	_( 'b', "branch", "branch",						\
	    "Only display commits that reside on the given <branch>.",		\
	    required_argument ),						\
	_( 'C', "no-colour", NULL,						\
	    "Disable colourised timeline. Toggle at runtime with 'C'\n\t"	\
	    "if this option is not used.",					\
	    no_argument ),							\
	_( 'c', "commit", "commit",						\
	    "Open the timeline from <commit>. Common symbols are:"		\
	    "\n\t    SHA{1,3} hash"						\
	    "\n\t    SHA{1,3} unique prefix"					\
	    "\n\t    branch"							\
	    "\n\t    tag:TAG"							\
	    "\n\t    root:BRANCH"						\
	    "\n\t    ISO8601 date"						\
	    "\n\t    ISO8601 timestamp"						\
	    "\n\t    {tip,current,prev,next}\n\t"				\
	    "For a complete list of symbols see Fossil's Check-in Names:\n\t"	\
	    "https://fossil-scm.org/home/doc/trunk/www/checkin_names.wiki",	\
	    required_argument ),						\
	_( 'f', "filter", "glob",						\
	    "Load commits with <glob> in the comment, user, or branch field.",	\
	    required_argument ),						\
	_( 'h', "help", NULL, "Display timeline command help and usage.",	\
	    no_argument ),							\
	_( 'n', "limit", "n", "Load timeline with <n> commits.",		\
	    required_argument ),						\
	_( 'r', "repo", "path", "Use the Fossil repository at <path>.",		\
	    required_argument ),						\
	_( 'T', "tag", "tag", "Only load commits with <tag> T cards.",		\
	    required_argument ),						\
	_( 't', "type", "type", "Only display <type> commits. Valid types are:"	\
	    "\n\t    c - check-in"						\
	    "\n\t    w - wiki"							\
	    "\n\t    t - ticket"						\
	    "\n\t    e - technote"						\
	    "\n\t    f - forum post"						\
	    "\n\t    g - tag artifact"						\
	    "\n\tn.b. This is a repeatable flag (e.g., -t c -t w).",		\
	    required_argument ),						\
	_( 'u', "username", "user", "Display commits authored by <username>.",	\
	    required_argument ),						\
	_( 'z', "utc", NULL, "Use UTC (instead of local) time.", no_argument ),

#define TREE_HELP(_)								\
	_( 'C', "no-colour", NULL,						\
	    "Disable colourised tree. Toggle at runtime with 'C' if this\n\t"	\
	    "option is not used.",						\
	    no_argument ),							\
	_( 'c', "commit", "commit",						\
	    "Display repository tree as at <commit>. Common symbols are:"	\
	    "\n\t    SHA{1,3} hash"						\
	    "\n\t    SHA{1,3} unique prefix"					\
	    "\n\t    branch"							\
	    "\n\t    tag:TAG"							\
	    "\n\t    root:BRANCH"						\
	    "\n\t    ISO8601 date"						\
	    "\n\t    ISO8601 timestamp"						\
	    "\n\t    {tip,current,prev,next}\n\t"				\
	    "For a complete list of symbols see Fossil's Check-in Names:\n\t"	\
	    "https://fossil-scm.org/home/doc/trunk/www/checkin_names.wiki",	\
	    required_argument ),						\
	_( 'h', "help", NULL, "Display tree command help and usage.",		\
	    no_argument ),							\
	_( 'r', "repo", "path", "Use the Fossil repository at <path>.",		\
	    required_argument ),						\
	_( 'z', "utc", NULL, "Use UTC (instead of local) time.", no_argument ),

#define GEN_GETOPT_HELP(_)					\
	_(global, -1, GLOBAL_HELP)				\
	_(blame, FNC_VIEW_BLAME, BLAME_HELP)			\
	_(branch, FNC_VIEW_BRANCH, BRANCH_HELP)			\
	_(config, FNC_VIEW_CONFIG, CONFIG_HELP)			\
	_(diff, FNC_VIEW_DIFF, DIFF_HELP)			\
	_(stash, FNC_VIEW_STASH, STASH_HELP)			\
	_(timeline, FNC_VIEW_TIMELINE, TIMELINE_HELP)		\
	_(tree, FNC_VIEW_TREE, TREE_HELP)

#define CMD_HELP_STRUCT_VALUE(s, l, a, t, h) { (s), (l), (a), (t) }

#define GETOPT_OPTION_STRUCT_VALUE(s, l, a, t, h) { (l), (h), NULL, s }

#define GEN_CMD_HELP_STRUCT(name, id, options)			\
	struct cmd_help help_##name[] = {			\
		options(CMD_HELP_STRUCT_VALUE)			\
		{ 0, NULL, NULL, NULL }				\
	};							\
	static const struct option name##_opt[] = {		\
		options(GETOPT_OPTION_STRUCT_VALUE)		\
		{ NULL, 0, NULL, 0 }				\
	};
