/*
 * Copyright (c) 2021, 2022, 2023 Mark Jamsek <mark@jamsek.com>
 * Copyright (c) 2020 Stefan Sperling <stsp@openbsd.org>
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

#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/stat.h>

#ifdef HAVE_LANDLOCK
#include <linux/landlock.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <libgen.h>
#endif

#include <fcntl.h>
#include <getopt.h>
#include <ctype.h>
#define _XOPEN_SOURCE_EXTENDED	/* curses wide-character functions */
#include <curses.h>
#include <panel.h>
#include <paths.h>
#include <locale.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <err.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <pthread.h>
#include <regex.h>
#include <signal.h>
#include <wchar.h>
#include <wctype.h>
#include <langinfo.h>

#define FSL_EXPOSE_INTERNAL_APIS /* backwards compat crutch */
#include "libfossil.h"
#include "diff.h"
#include "enum.h"
#include "error.h"
#include "fnc.h"
#include "opt.h"

static int		cmd_timeline(int argc, char **argv);
static int		cmd_diff(int argc, char **argv);
static int		cmd_tree(int argc, char **argv);
static int		cmd_blame(int argc, char **argv);
static int		cmd_branch(int argc, char **argv);
static int		cmd_config(int argc, char **argv);
static int		cmd_stash(int argc, char **argv);

struct cmd_help {
	const char  sopt;
	const char *lopt;
	const char *arg;
	const char *txt;
};

GEN_GETOPT_HELP(GEN_CMD_HELP_STRUCT)  /* opt.h */

struct fnc_cmd {
	const char	*name;
	const char	*aliases;
	int		 (*f)(int, char **);
	struct cmd_help	*help;
};

static const struct fnc_cmd fnc_commands[] = {
	{"blame",	"bl\0praise\0",	cmd_blame,	help_blame},
	{"branch",	"br\0ref\0",	cmd_branch,	help_branch},
	{"config",	"conf\0set\0",	cmd_config,	help_config},
	{"diff",	"di\0",		cmd_diff,	help_diff},
	{"stash",	"save\0sta\0",	cmd_stash,	help_stash},
	{"timeline",	"tl\0log\0",	cmd_timeline,	help_timeline},
	{"tree",	"tr\0dir\0",	cmd_tree,	help_tree}
};

__dead static void	usage(const struct fnc_cmd *, int);

#define usage_blame(_rc)	usage(&fnc_commands[FNC_VIEW_BLAME], _rc)
#define usage_branch(_rc)	usage(&fnc_commands[FNC_VIEW_BRANCH], _rc)
#define usage_config(_rc)	usage(&fnc_commands[FNC_VIEW_CONFIG], _rc)
#define usage_diff(_rc)		usage(&fnc_commands[FNC_VIEW_DIFF], _rc)
#define usage_stash(_rc)	usage(&fnc_commands[FNC_VIEW_STASH], _rc)
#define usage_timeline(_rc)	usage(&fnc_commands[FNC_VIEW_TIMELINE], _rc)
#define usage_tree(_rc)		usage(&fnc_commands[FNC_VIEW_TREE], _rc)

static chtype	fnc__highlight = A_BOLD | A_REVERSE;
static bool	fnc__utc;
static char	fnc__progname[PATH_MAX];

/* fossil(1) db and runtime related paths to unveil() */
#define CKOUTDIR	fsl_cx_ckout_dir_name(fcli_cx(), NULL)
#define REPODB		fsl_cx_db_file_repo(fcli_cx(), NULL)
#define REPODIR		getdirname(REPODB, -1, false)

#ifdef P_tmpdir
static const char *fnc__tmpdir = P_tmpdir;
#elif defined(_PATH_TMP)
static const char *fnc__tmpdir = _PATH_TMP;
#else
static const char *fnc__tmpdir = "/tmp/";
#endif

struct input {
	void		*data;
	char		*prompt;
	enum input_type	 type;
	int		 flags;
#define SR_CLREOL	(1 << 0)
#define SR_UPDATE	(1 << 1)
#define SR_SLEEP	(1 << 2)
#define SR_RESET	(1 << 3)
#define SR_ALL		(SR_CLREOL | SR_UPDATE | SR_SLEEP | SR_RESET)
	char		 buf[BUFSIZ];
	long		 ret;
};

enum date_string {
	ISO8601_DATE_ONLY = 10,  /* YYYY-MM-DD */
	ISO8601_DATE_HHMM = 16,  /* YYYY-MM-DD HH:MM */
	ISO8601_TIMESTAMP = 19   /* YYYY-MM-DD HH:MM:SS */
};

struct fnc_file_artifact {
	fsl_card_F		*fc;
	fsl_uuid_str		 puuid;
	uint64_t		 diffstat;
	fsl_ckout_change_e	 change;
};

struct fnc_commit_artifact {
	fsl_buffer		 wiki;
	fsl_buffer		 pwiki;
	fsl_list		 changeset;
	fsl_uuid_str		 uuid;
	fsl_uuid_str		 puuid;
	char			*user;
	char			*timestamp;
	char			*comment;
	char			*branch;
	char			*type;
	size_t			 maxpathlen;
	uint32_t		 dswidths;
	int32_t			 rid;
	int32_t			 prid;
	enum fnc_diff_type	 diff_type;
};

TAILQ_HEAD(commit_tailhead, commit_entry);
struct commit_entry {
	TAILQ_ENTRY(commit_entry)	 entries;
	struct fnc_commit_artifact	*commit;
	int				 idx;
};

struct commit_queue {
	struct commit_tailhead	head;
	int			ncommits;
};

/*
 * The following two structs are used to construct the tree of the entire
 * repository; that is, from the root through to all subdirectories and files.
 */
struct fnc_repository_tree {
	struct fnc_repo_tree_node	*head;     /* Head of repository tree */
	struct fnc_repo_tree_node	*tail;     /* Final node in the tree. */
	struct fnc_repo_tree_node	*rootail;  /* Final root level node. */
	size_t				 nentries;
};

struct fnc_repo_tree_node {
	struct fnc_repo_tree_node	*next;	     /* Next node in tree. */
	struct fnc_repo_tree_node	*prev;	     /* Prev node in tree. */
	struct fnc_repo_tree_node	*parent_dir; /* Dir containing node. */
	struct fnc_repo_tree_node	*sibling;    /* Next node in same dir */
	struct fnc_repo_tree_node	*children;   /* List of node children */
	struct fnc_repo_tree_node	*lastchild;  /* Last child in list. */
	char				*basename;   /* Final path component. */
	char				*path;	     /* Full pathname of node */
	char				*uuid;	     /* File artifact hash. */
	mode_t				 mode;	     /* File mode. */
	time_t				 mtime;	     /* Mod time of file. */
	uint_fast16_t			 pathlen;    /* Length of path. */
	uint_fast16_t			 nparents;   /* Path components sans- */
						     /* -basename. */
};

/*
 * The following two structs represent a given subtree within the repository;
 * for example, the top level tree and all its elements, or the elements of
 * the src/ directory (but not any members of src/ subdirectories).
 */
struct fnc_tree_object {
	struct fnc_tree_entry	*entries;  /* Array of tree entries. */
	int			 nentries; /* Number of tree entries. */
};

struct fnc_tree_entry {
	char	*basename;	/* final component of path */
	char	*path;		/* full pathname */
	char	*uuid;		/* file artifact hash */
	mode_t	 mode;		/* file mode */
	time_t	 mtime;		/* modification time */
	int	 idx;		/* index of this tree entry */
};

/*
 * Each fnc_tree_object that is _not_ the repository root
 * will have a (list of) fnc_parent_tree(s) to be tracked.
 */
struct fnc_parent_tree {
	TAILQ_ENTRY(fnc_parent_tree)	 entry;
	struct fnc_tree_object		*tree;
	struct fnc_tree_entry		*first_entry_onscreen;
	struct fnc_tree_entry		*selected_entry;
	int				 selected;
};

pthread_mutex_t fnc__mutex = PTHREAD_MUTEX_INITIALIZER;

struct ckout_state {
	int		idx;
	unsigned char	state;
};

#define FNC_CKOUT_STATE_UNKNOWN	0
#define FNC_CKOUT_STATE_CLEAN	'@'
#define FNC_CKOUT_STATE_CHANGED	'~'

struct fnc_tl_thread_cx {
	struct commit_queue	 *commits;
	struct commit_entry	**first_commit_onscreen;
	struct commit_entry	**selected_entry;
	fsl_db			 *db;
	fsl_stmt		 *q;
	regex_t			 *regex;
	struct ckout_state	  ckout;
	char			 *path;
	enum fnc_search_state	 *search_status;
	enum fnc_search_mvmt	 *searching;
	int			  spin_idx;
	int			  ncommits_needed;
	bool			  endjmp;
	bool			  eotl;
	bool			  reset;
	sig_atomic_t		 *quit;
	pthread_cond_t		  commit_consumer;
	pthread_cond_t		  commit_producer;
};

struct fnc_colour {
	STAILQ_ENTRY(fnc_colour) entries;
	regex_t	regex;
	uint8_t	scheme;
};
STAILQ_HEAD(fnc_colours, fnc_colour);

struct fnc_tl_view_state {
	struct fnc_tl_thread_cx	 thread_cx;
	struct commit_queue	 commits;
	struct commit_entry	*first_commit_onscreen;
	struct commit_entry	*last_commit_onscreen;
	struct commit_entry	*selected_entry;
	struct commit_entry	*matched_commit;
	struct commit_entry	*search_commit;
	struct fnc_colours	 colours;
	struct timeline_tag {
		struct fnc_commit_artifact	*one;	/* 1st tagged entry */
		struct fnc_commit_artifact	*two;	/* 2nd tagged entry */
		fsl_uuid_str			 ogid;	/* parent uuid of two */
		fsl_id_t			 ogrid;	/* parent rid of two */
	} tag;
	const char		*glob;  /* Match commits containing glob. */
	char			*path;	/* Match commits involving path. */
	int			 selected;
	sig_atomic_t		 quit;
	pthread_t		 thread_id;
	bool			 colour;
	bool			 showmeta;
};

/*
 * A stash context is comprised of two patch contexts: a (1) patch of all hunks
 * selected to stash; and a (2) patch of all hunks kept in the checkout. Each
 * patch has a queue of fnc_patch_file(s), one for each versioned file with
 * hunks to be stashed or kept. Each fnc_patch_file has a queue of hunks with
 * an array of all context, plus, and minus lines comprising the hunk. Each
 * patch context produces a patch(1) file that gets applied to the base ckout.
 */
struct fnc_patch_hunk {
	STAILQ_ENTRY(fnc_patch_hunk) entries;
	char		**lines;	/* plus, minus, context lines */
	long		  offset;	/* line offset into this hunk */
	size_t		  nlines;	/* number of *lines */
	size_t		  cap;		/* capacity of **lines */
	int_least32_t	  oldfrom;	/* start line in "from" file */
	int_least32_t	  oldlines;	/* number of lines from "oldfrom" */
	int_least32_t	  newfrom;	/* start line in "new" file */
	int_least32_t	  newlines;	/* number of lines from "newfrom" */
	bool		  nonl;		/* line continuation flag */
	int		  rc;
};

STAILQ_HEAD(fnc_patch_hunk_head, fnc_patch_hunk);
struct fnc_patch_file {
	STAILQ_ENTRY(fnc_patch_file)	entries;
	char				old[PATH_MAX];
	char				new[PATH_MAX];
	struct fnc_patch_hunk_head	head;
};

typedef int (*fnc_patch_report_cb)(struct fnc_patch_file *, const char *,
    const char *, unsigned char);
STAILQ_HEAD(fnc_patch_file_head, fnc_patch_file);
struct patch_cx {
	fnc_patch_report_cb		 report_cb;
	struct fnc_patch_file		*pf;	/* current fnc_patch_file */
	struct fnc_patch_file_head	 head;	/* queue of fnc_patch_file(s) */
	uint8_t				 context; /* MAX_DIFF_CTX lines = 64 */
	int				 rc;
	bool				 report;
};

struct stash_cx {
	struct patch_cx	 pcx;
	char		 patch[2][PATH_MAX];	/* stash/ckout diff filepaths */
	unsigned char	*stash;			/* bitarray of hunks to stash */
};

struct fnc_pathlist_entry {
	TAILQ_ENTRY(fnc_pathlist_entry) entry;
	const char	*path;
	size_t		 pathlen;
	void		*data;
};
TAILQ_HEAD(fnc_pathlist_head, fnc_pathlist_entry);

struct fnc_diff_view_state {
	struct fnc_view			*view;
	struct fnc_commit_artifact	*selected_entry;
	struct fnc_pathlist_head	*paths;
	struct stash_cx			 scx;
	struct fsl_buffer		 buf;
	struct fnc_colours		 colours;
	fsl_uuid_str			 id1;
	fsl_uuid_str			 id2;
	int				 first_line_onscreen;
	int				 last_line_onscreen;
	int				 diff_flags;
	int				 context;
	int				 sbs;
	int				 matched_line;
	int				 selected_line;
	int				 lineno;
	int				 gtl;
	int				*ncols;
	size_t				 nhunks;
	size_t				 nlines;
	size_t				 ndlines;
	enum line_type			*dlines;
	enum line_attr			 sline;
	enum fnc_diff_hunk		 stash;
	enum fnc_diff_mode		 diff_mode;
	off_t				*line_offsets;
	bool				 eof;
	bool				 showln;
	bool				 patch;
	bool				 wrap;
};

TAILQ_HEAD(fnc_parent_trees, fnc_parent_tree);
struct fnc_tree_view_state {			  /* Parent trees of the- */
	struct fnc_parent_trees		 parents; /* -current subtree. */
	struct fnc_repository_tree	*repo;    /* The repository tree. */
	struct fnc_tree_object		*root;    /* Top level repo tree. */
	struct fnc_tree_object		*tree;    /* Currently displayed tree */
	struct fnc_tree_entry		*first_entry_onscreen;
	struct fnc_tree_entry		*last_entry_onscreen;
	struct fnc_tree_entry		*selected_entry;
	struct fnc_tree_entry		*matched_entry;
	struct fnc_colours		 colours;
	char				*tree_label;  /* Headline string. */
	fsl_uuid_str			 commit_id;
	fsl_id_t			 rid;
	int				 ndisplayed;
	int				 selected;
	bool				 show_id;
	bool				 show_date;
};

struct fnc_blame_line {
	fsl_uuid_str	id;
	unsigned int	lineno;
	bool		annotated;
};

struct fnc_blame_cb_cx {
	struct fnc_view		*view;
	struct fnc_blame_line	*lines;
	fsl_uuid_cstr		 commit_id;
	int			 nlines;
	bool			*quit;
};

typedef int (*fnc_cancel_cb)(void *);

struct fnc_blame_thread_cx {
	struct fnc_blame_cb_cx	*cb_cx;
	fsl_annotate_opt	 blame_opt;
	fnc_cancel_cb		 cancel_cb;
	const char		*path;
	void			*cancel_cx;
	bool			*complete;
};

struct fnc_blame {
	struct fnc_blame_thread_cx	 thread_cx;
	struct fnc_blame_cb_cx		 cb_cx;
	struct fsl_buffer		 buf;	/* unadorned copy of file */
	struct fnc_blame_line		*lines;
	off_t				*line_offsets;
	fsl_id_t			 origin; /* Tip rid for reverse blame */
	int				 nlines;
	int				 nlimit;    /* Limit depth traversal. */
	pthread_t			 thread_id;
};

SQ(HEAD)(fnc_commit_id_queue, fnc_commit_qid);
struct fnc_commit_qid {
	SQ(ENTRY)(fnc_commit_qid)	entry;
	char				id[FSL_STRLEN_K256 + 1];
};

struct fnc_blame_view_state {
	struct fnc_blame		 blame;
	struct fnc_commit_id_queue	 blamed_commits;
	struct fnc_commit_qid		*blamed_commit;
	struct fnc_commit_artifact	*selected_entry;
	struct fnc_colours		 colours;
	const char			*lineno;
	char				*commit_id;
	char				*path;
	int				 line_rid;
	int				 first_line_onscreen;
	int				 last_line_onscreen;
	int				 selected_line;
	int				 matched_line;
	int				 spin_idx;
	int				 gtl;
	bool				 done;
	bool				 blame_complete;
	bool				 eof;
	bool				 showln;
};

struct fnc_branch {
	char	*name;
	char	*id;
	time_t	 mtime;
	int	 state;
#define BRANCH_STATE_OPEN	(1 << 0)
#define BRANCH_STATE_PRIV	(1 << 1)
#define BRANCH_STATE_CURR	(1 << 2)
};

struct fnc_branchlist_entry {
	TAILQ_ENTRY(fnc_branchlist_entry) entries;
	struct fnc_branch	*branch;
	int			 idx;
};
TAILQ_HEAD(fnc_branchlist_head, fnc_branchlist_entry);

struct fnc_branch_view_state {
	struct fnc_branchlist_head	 branches;
	struct fnc_branchlist_entry	*first_branch_onscreen;
	struct fnc_branchlist_entry	*last_branch_onscreen;
	struct fnc_branchlist_entry	*matched_branch;
	struct fnc_branchlist_entry	*selected_entry;
	struct fnc_colours		 colours;
	const char			*branch_glob;
	double				 dateline;
	int				 branch_flags;
#define BRANCH_LS_CLOSED_ONLY	0x001  /* Show closed branches only. */
#define BRANCH_LS_OPEN_ONLY	0x002  /* Show open branches only. */
#define BRANCH_LS_OPEN_CLOSED	0x003  /* Show open & closed branches (dflt). */
#define BRANCH_LS_BITMASK	0x003
#define BRANCH_LS_NO_PRIVATE	0x004  /* Show public branches only. */
#define BRANCH_SORT_MTIME	0x008  /* Sort by activity. (default: name) */
#define BRANCH_SORT_STATUS	0x010  /* Sort by open/closed. */
#define BRANCH_SORT_REVERSE	0x020  /* Reverse sort order. */
	int				 nbranches;
	int				 ndisplayed;
	int				 selected;
	int				 when;
	bool				 show_date;
	bool				 show_id;
};

struct position {
	int	x, y;
	int	maxx;
	int	offset;
};

TAILQ_HEAD(view_tailhead, fnc_view);
struct fnc_view {
	TAILQ_ENTRY(fnc_view)	 entries;
	WINDOW			*window;
	PANEL			*panel;
	struct fnc_view		*parent;
	struct fnc_view		*child;
	struct position		 pos;
	union {
		struct fnc_diff_view_state	diff;
		struct fnc_tl_view_state	timeline;
		struct fnc_tree_view_state	tree;
		struct fnc_blame_view_state	blame;
		struct fnc_branch_view_state	branch;
	} state;
	char			*status;
	enum fnc_view_id	 vid;
	enum view_mode		 mode;
	enum fnc_search_state	 search_status;
	enum fnc_search_mvmt	 searching;
	int			 nlines;	/* dependent on split height */
	int			 ncols;		/* dependent on split width */
	int			 begin_y;	/* top left line of window */
	int			 begin_x;	/* top left column of window */
	int			 lines;		/* always curses LINES macro */
	int			 cols;		/* always curses COLS macro */
	int			 nscrolled;	/* lines scrolled in view */
	int			 resized_y;	/* new begin_y after resize */
	int			 resized_x;	/* new begin_x after resize */
	bool			 resizing;	/* view resize in progress */
	bool			 focus_child;
	bool			 active;
	bool			 egress;
	bool			 started_search;
	bool			 nodelay;
	bool			 colour;
	regex_t			 regex;
	regmatch_t		 regmatch;

	int	(*show)(struct fnc_view *);
	int	(*input)(struct fnc_view **, struct fnc_view *, int);
	int	(*resize)(struct fnc_view *, int);
	int	(*close)(struct fnc_view *);
	void	(*grep_init)(struct fnc_view *);
	int	(*grep)(struct fnc_view *);
};

static volatile sig_atomic_t fnc__recv_sigwinch;
static volatile sig_atomic_t fnc__recv_sigpipe;
static volatile sig_atomic_t fnc__recv_sigcont;
static volatile sig_atomic_t fnc__recv_sigint;
static volatile sig_atomic_t fnc__recv_sigterm;

static const struct fnc_cmd	*fnc_cmd_lookup(char **);
static int		 fnc_cx_open(const char *);
static void		 fnc_cx_close(void);
static int		 fnc_show_version(void);
static int		 fnc_cmd_aliascmp(const struct fnc_cmd *, const char *);
static void		 fnc_cmd_aliases(const char *);
static void		 show_help(const struct cmd_help *, bool);
static int		 init_curses(bool);
static int		 fnc_set_signals(void);
static struct fnc_view	*view_open(int, int, int, int, enum fnc_view_id);
static int		 open_timeline_view(struct fnc_view *, fsl_id_t,
			    const char *, const char *, const char *,
			    const char *, const char *, long,
			    const char *, bool);
static int		 view_loop(struct fnc_view *);
static int		 show_timeline_view(struct fnc_view *);
static void		*tl_producer_thread(void *);
static int		 block_main_thread_signals(void);
static int		 build_commits(struct fnc_tl_thread_cx *);
static int		 commit_builder(struct fnc_commit_artifact **, fsl_id_t,
			    fsl_stmt *);
static int		 signal_tl_thread(struct fnc_view *, int);
static int		 draw_commits(struct fnc_view *);
static int		 formatuser(wchar_t **, int *, char *, size_t, int);
static int		 formatln(wchar_t **, int *, int *, const char *, int,
			    int, int, bool);
static int		 span_wline(int *, int, wchar_t *, int, int);
static int		 expand_tab(char **, size_t *, const char *);
static int		 mbs2ws(wchar_t **, size_t *, const char *);
static int		 replace_unicode(char **, const char *);
static int		 write_commit_line(struct fnc_view *,
			    struct commit_entry *, int);
static int		 view_input(struct fnc_view **, int *,
			    struct fnc_view *, struct view_tailhead *);
static int		 view_switch_split(struct fnc_view *);
static int		 view_resize_split(struct fnc_view *, int);
static void		 view_adjust_offset(struct fnc_view *, int);
static int		 cycle_view(struct fnc_view *);
static int		 toggle_fullscreen(struct fnc_view **,
			    struct fnc_view *);
static int		 stash_help(struct fnc_view *, enum stash_mvmt, int *);
static int		 help(struct fnc_view *, int *);
static int		 drawpad(struct fnc_view *, const char *[][2],
			    const char **, const char *, enum stash_mvmt,
			    int *);
static int		 centerprint(WINDOW *, size_t, size_t, size_t,
			    const char *, chtype);
static int		 tl_input_handler(struct fnc_view **, struct fnc_view *,
			    int);
static int		 move_tl_cursor_down(struct fnc_view *, uint16_t);
static void		 move_tl_cursor_up(struct fnc_view *, uint16_t, bool);
static int		 timeline_scroll_down(struct fnc_view *, int);
static void		 timeline_scroll_up(struct fnc_tl_view_state *, int);
static int		 tag_timeline_entry(struct fnc_tl_view_state *);
static void		 select_commit(struct fnc_tl_view_state *);
static int		 view_request_new(struct fnc_view **,
			    struct fnc_view *, enum fnc_view_id);
static int		 view_dispatch_request(struct fnc_view **,
			    struct fnc_view *, enum fnc_view_id, int, int);
static void		 view_split_getyx(struct fnc_view *, int *, int *);
static int		 view_split_horizontally(struct fnc_view *, int);
static void		 view_offset_scrollup(struct fnc_view *);
static int		 view_offset_scrolldown(struct fnc_view *);
static int		 view_split_getx(int);
static int		 view_split_gety(int);
static int		 make_splitscreen(struct fnc_view *);
static int		 make_fullscreen(struct fnc_view *);
static int		 view_search_start(struct fnc_view *);
static void		 tl_grep_init(struct fnc_view *);
static int		 tl_search_next(struct fnc_view *);
static bool		 find_commit_match(struct fnc_commit_artifact *,
			    regex_t *);
static int		 object_get_type(int *, fsl_id_t, int);
static int		 init_diff_view(struct fnc_view **, int, int,
			    struct fnc_commit_artifact *,
			    enum fnc_diff_mode, int, int, int, int);
static int		 open_diff_view(struct fnc_view *,
			    struct fnc_commit_artifact *,
			    struct fnc_pathlist_head *,
			    enum fnc_diff_mode, int, int, bool, int);
static int		 set_diff_opt(struct fnc_diff_view_state *, bool *);
static void		 show_diff_status(struct fnc_view *);
static int		 create_diff(struct fnc_diff_view_state *);
static int		 dispatch_diff_request(struct fnc_diff_view_state *);
static int		 alloc_commit_meta(struct fnc_diff_view_state *);
static int		 create_changeset(struct fnc_commit_artifact *,
			    enum fnc_diff_mode);
static int		 parse_manifest(struct fnc_commit_artifact *);
static int		 alloc_file_artifact(struct fnc_file_artifact **,
			    const char *path, const char *, size_t *,
			    const char *, const char *, size_t, int);
static int		 make_stash_diff(struct fnc_diff_view_state *,
			    struct fsl_buffer *);
static int		 write_commit_meta(struct fsl_buffer *,
			    struct fnc_commit_artifact *, size_t, int,
			    off_t **, size_t *, enum line_type **, size_t *);
static int		 fmtlogmsg(struct fsl_buffer *, char *, size_t,
			    off_t **, size_t *, enum line_type **,
			    size_t *);
static int		 logmsgln(struct fsl_buffer *, char *, size_t,
			    size_t, off_t **, size_t *, enum line_type **,
			    size_t *, off_t *, size_t *);
static int		 write_changeset(struct fsl_buffer *buf,
			    struct fnc_diff_view_state *, off_t **,
			    size_t *, enum line_type **, size_t *);
static int		 plot_histogram(struct fsl_buffer *,
			    struct fnc_diff_view_state *, off_t *, uint64_t);
static int		 plot_bar(long *, struct fsl_buffer *, long, int, int,
			    int, long *);
static int		 wrapline(struct fsl_buffer *, char *, size_t,
			    off_t **, size_t *, enum line_type **, size_t *,
			    off_t *, size_t *);
static int		 add_line_offset(off_t **, size_t *, off_t);
static int		 diff_commit(struct fnc_diff_view_state *);
static int		 diff_versions(struct fnc_diff_view_state *);
static int		 fnc_file_artifact_diffstat(const struct fsl_card_F *,
			    struct fnc_commit_artifact *, uint64_t);
static int		 fnc_file_artifact_cmp(const void *, const void *);
static void		 encode_diffstat_widths(uint32_t *, uint64_t);
static void		 encode_diffstat(uint64_t *, uint64_t);
static bool		 path_to_diff(const struct fnc_pathlist_head *,
			    const char *, const char *);
static int		 diff_checkout(struct fnc_diff_view_state *);
static int		 write_diff_meta(struct fnc_diff_view_state *,
			    const char *, fsl_uuid_cstr, const char *,
			    fsl_uuid_cstr, const fsl_ckout_change_e);
static int		 diff_file(struct fnc_diff_view_state *, fsl_buffer *,
			    const char *, const char *, fsl_uuid_cstr,
			    const char *, const fsl_ckout_change_e, int);
static int		 diff_wiki(struct fnc_diff_view_state *);
static int		 diff_file_artifact(struct fnc_diff_view_state *,
			    fsl_id_t, const fsl_card_F *, const fsl_card_F *,
			    const fsl_ckout_change_e);
static int		 diff_buffer_from_state(struct fnc_diff_view_state *,
			    struct fsl_buffer *, struct fsl_buffer *,
			    struct fnc_file_artifact *);
static int		 fnc_read_symlink(struct fsl_buffer *, const char *);
static int		 show_diff_view(struct fnc_view *);
static int		 write_diff_headln(struct fnc_view *,
			    struct fnc_diff_view_state *, FILE *);
static int		 match_line(const char *, regex_t *, size_t,
			    regmatch_t *);
static int		 draw_matched_line(int *, const char *, int, int, int,
			    WINDOW *, regmatch_t *, attr_t);
static void		 drawborder(struct fnc_view *);
static int		 diff_input_handler(struct fnc_view **,
			    struct fnc_view *, int);
static int		 difftofile(FILE *, struct fnc_diff_view_state *);
static void		 diff_prev_index(struct fnc_diff_view_state *,
			    enum line_type);
static void		 diff_next_index(struct fnc_diff_view_state *,
			    enum line_type);
static int		 request_tl_commits(struct fnc_view *);
static int		 reset_diff_view(struct fnc_view *, bool);
static int		 stash_get_rm_cb(fsl_ckout_unmanage_state const *);
static int		 stash_get_add_cb(fsl_ckout_manage_state const *,
			    bool *);
static int		 f__add_files_in_sfile(int *, int);
static int		 f__stash_get(uint32_t, bool);
static int		 stash_get_row(struct fsl_stmt *, const char **,
			    const char **, int *, int *, int *, int *);
static int		 fnc_stash_add_file(struct fsl_db *, struct fsl_stmt *,
			    const char *, const char *, int);
static int		 fnc_stash_rm_file(const char *);
static int		 fnc_stash_update_file(struct fsl_stmt *, fsl_id_t,
			    const char *, const char *, const char *,
			    const char *, int, int, uint32_t *);
static int		 fnc_stash(struct fnc_view *);
static int		 fnc_stash_get(const char *, bool);
static int		 select_hunks(struct fnc_view *);
static int		 stash_input_handler(struct fnc_view *, bool *);
static void		 set_choice(struct fnc_diff_view_state *, bool *,
			    char *, size_t, enum stash_opt *);
static int		 generate_prompt(char *, char *, size_t,
			    enum stash_mvmt);
static bool		 valid_input(const char, char *);
static int		 revert_ckout(bool, bool);
static int		 rm_vfile_renames_cb(fsl_stmt *, void *);
static int		 fnc_patch(struct patch_cx *, const char *);
static int		 scan_patch(struct patch_cx *, FILE *);
static int		 find_patch_file(struct fnc_patch_file **,
			    struct patch_cx *, FILE *);
static int		 parse_filename(char **, const char *, int);
static int		 set_patch_paths(struct fnc_patch_file *, const char *,
			    const char *);
static int		 parse_hunk(struct fnc_patch_hunk **, FILE *, uint8_t,
			    bool *);
static int		 parse_hdr(char *, bool *, struct fnc_patch_hunk *);
static int		 strtolnum(char **, int_least32_t *);
static int		 pushline(struct fnc_patch_hunk *, const char *);
static int		 alloc_hunk_line(struct fnc_patch_hunk *, const char *);
static int		 peek_special_line(struct fnc_patch_hunk *, FILE *, int);
static int		 apply_patch(struct patch_cx *, bool);
static int		 fnc_open_tmpfile(char **, FILE **, const char *,
			    const char *);
static int		 patch_file(struct fnc_patch_file *, const char *,
			    FILE *, int, mode_t *);
static int		 apply_hunk(FILE *, struct fnc_patch_hunk *, long *);
static int		 locate_hunk(FILE *, struct fnc_patch_hunk *, off_t *,
			    long *);
static int		 copyfile(FILE *, FILE *, off_t, off_t);
static int		 test_hunk(FILE *, struct fnc_patch_hunk *);
static int		 fnc_add_vfile(struct patch_cx *, const char *, bool);
static int		 fnc_addvfile_cb(const fsl_ckout_manage_state *, bool *);
static int		 fnc_rm_vfile(struct patch_cx *, const char *, bool);
static int		 fnc_rmvfile_cb(const fsl_ckout_unmanage_state *);
static int		 fnc_rename_vfile(const char *, const char *);
static int		 patch_reporter(struct fnc_patch_file *,
			    const char *, const char *, unsigned char);
static int		 patch_report(const char *, const char *,
			    unsigned char, long, long, long, long, long, int);
static void		 free_patch(struct fnc_patch_file *);
static void		 free_hunk(struct fnc_patch_hunk *);
static int		 f__stash_path(int, int, const char *);
static int		 f__check_stash_tables(void);
static int		 f__stash_create(const char *, int);
/* static int		 fnc_execp(const char *const *, const int); */
static int		 set_selected_commit(struct fnc_diff_view_state *,
			    struct commit_entry *);
static void		 diff_grep_init(struct fnc_view *);
static int		 find_next_match(struct fnc_view *);
static int		 grep_set_view(struct fnc_view *, struct fsl_buffer **,
			    off_t **, size_t *, int **, int **, int **, int **,
			    uint8_t *);
static int		 view_close(struct fnc_view *);
static int		 fnc_canonpath(char **, const char *, const char *);
static int		 map_version_path(const char *, fsl_id_t);
static int		 init_timeline_view(struct fnc_view **, int, int,
			    fsl_id_t, const char *, const char *, const char *,
			    const char *, const char *, long,
			    const char *, bool);
static bool		 path_is_child(const char *, const char *, size_t);
static int		 path_skip_common_ancestor(char **, const char *,
			    size_t, const char *, size_t);
static bool		 fnc_path_is_root_dir(const char *);
/* static bool		 fnc_path_is_cwd(const char *); */
static int		 fnc_pathlist_insert(struct fnc_pathlist_entry **,
			    struct fnc_pathlist_head *, const char *, void *);
static int		 fnc_path_cmp(const char *, const char *, size_t,
			    size_t);
static void		 fnc_pathlist_free(struct fnc_pathlist_head *);
static int		 browse_commit_tree(struct fnc_view **, int, int,
			    struct commit_entry *, const char *, int);
static int		 open_tree_view(struct fnc_view *, const char *,
			    fsl_id_t, bool);
static int		 walk_tree_path(struct fnc_tree_view_state *,
			    const char *, uint16_t);
static int		 create_repository_tree(struct fnc_repository_tree **,
			    fsl_id_t);
static int		 tree_builder(struct fnc_tree_object **,
			    struct fnc_repository_tree *, const char *);
/* static void		 delete_tree_node(struct fnc_tree_entry **, */
/*			    struct fnc_tree_entry *); */
static int		 insert_tree_node(struct fnc_repository_tree *,
			    const char *, const char *, enum fsl_fileperm_e,
			    time_t);
static int		 show_tree_view(struct fnc_view *);
static int		 tree_input_handler(struct fnc_view **,
			    struct fnc_view *, int);
static int		 blame_tree_entry(struct fnc_view **, int, int,
			    struct fnc_tree_entry *, struct fnc_parent_trees *,
			    fsl_id_t, int);
static void		 tree_grep_init(struct fnc_view *);
static int		 tree_search_next(struct fnc_view *);
static int		 tree_entry_path(char **, struct fnc_parent_trees *,
			    struct fnc_tree_entry *);
static int		 draw_tree(struct fnc_view *, const char *);
static int		 blame_selected_file(struct fnc_view **,
			    struct fnc_view *);
static int		 timeline_tree_entry(struct fnc_view **, int, int,
			    struct fnc_tree_view_state *, int);
static void		 tree_scroll_up(struct fnc_tree_view_state *, int);
static int		 tree_scroll_down(struct fnc_view *, int);
static int		 visit_subtree(struct fnc_tree_view_state *,
			    struct fnc_tree_object *);
static int		 tree_entry_get_symlink_target(char **, size_t *,
			    struct fnc_tree_entry *);
static int		 fnc_blob_get_content(char **, ssize_t *, fsl_id_t);
static int		 match_tree_entry(struct fnc_tree_entry *, regex_t *);
static void		 fnc_object_tree_close(struct fnc_tree_object *);
static void		 fnc_close_repo_tree(struct fnc_repository_tree *);
static int		 open_blame_view(struct fnc_view *, char *,
			    fsl_id_t, fsl_id_t, int, const char *, bool);
static int		 run_blame(struct fnc_view *);
static int		 show_blame_view(struct fnc_view *);
static void		*blame_thread(void *);
static int		 blame_cb(void *, fsl_annotate_opt const * const,
			    fsl_annotate_step const * const);
static int		 draw_blame(struct fnc_view *);
static int		 blame_input_handler(struct fnc_view **,
			    struct fnc_view *, int);
static void		 blame_grep_init(struct fnc_view *);
static const char	*get_selected_commit_id(struct fnc_blame_line *,
			    int, int, int);
static int		 fnc_commit_qid_alloc(struct fnc_commit_qid **,
			    fsl_uuid_cstr);
static int		 close_blame_view(struct fnc_view *);
static int		 stop_blame(struct fnc_blame *);
static int		 cancel_blame(void *);
static void		 fnc_commit_qid_free(struct fnc_commit_qid *);
static int		 fnc_load_branches(struct fnc_branch_view_state *);
static int		 create_tmp_branchlist_table(void);
static int		 alloc_branch(struct fnc_branch **, const char *,
			    double, bool, bool, bool);
static int		 fnc_branchlist_insert(struct fnc_branchlist_entry **,
			    struct fnc_branchlist_head *, struct fnc_branch *);
static int		 open_branch_view(struct fnc_view *, int, const char *,
			    double, int, bool);
static int		 show_branch_view(struct fnc_view *);
static int		 branch_input_handler(struct fnc_view **,
			    struct fnc_view *, int);
static int		 browse_branch_tree(struct fnc_view **, int, int,
			    struct fnc_branchlist_entry *, int);
static void		 branch_scroll_up(struct fnc_branch_view_state *, int);
static int		 branch_scroll_down(struct fnc_view *, int);
static int		 branch_search_next(struct fnc_view *);
static void		 branch_grep_init(struct fnc_view *);
static int		 match_branchlist_entry(struct fnc_branchlist_entry *,
			    regex_t *);
static int		 close_branch_view(struct fnc_view *);
static void		 fnc_free_branches(struct fnc_branchlist_head *);
static void		 fnc_branch_close(struct fnc_branch *);
static bool		 view_is_parent(struct fnc_view *);
static int		 view_set_child(struct fnc_view *, struct fnc_view *);
static void		 view_copy_size(struct fnc_view *, struct fnc_view *);
static int		 view_close_child(struct fnc_view *);
static int		 close_tree_view(struct fnc_view *);
static int		 close_timeline_view(struct fnc_view *);
static int		 close_diff_view(struct fnc_view *);
static void		 free_diff_state(struct fnc_diff_view_state *);
static int		 reset_tags(struct fnc_tl_view_state *);
static int		 view_resize(struct fnc_view *);
static int		 resize_timeline_view(struct fnc_view *, int);
static bool		 view_is_split(struct fnc_view *);
static bool		 view_is_top_split(struct fnc_view *);
static bool		 view_is_fullscreen(struct fnc_view *);
static bool		 view_is_shared(struct fnc_view *);
static int		*view_width(int);
static void		 updatescreen(WINDOW *, bool, bool);
static void		 fnc_resizeterm(void);
static int		 join_tl_thread(struct fnc_tl_view_state *);
static void		 fnc_free_commits(struct commit_queue *);
static void		 fnc_commit_artifact_close(struct fnc_commit_artifact*);
static int		 fnc_file_artifact_free(void *, void *);
static void		 sigwinch_handler(int);
static void		 sigpipe_handler(int);
static void		 sigcont_handler(int);
static void		 sigint_handler(int);
static void		 sigterm_handler(int);
static bool		 fatal_signal(void);
static int		 draw_lineno(struct fnc_view *, int, int, attr_t);
static int		 gotoline(struct fnc_view *, int *, int *);
static int		 validate_user(const char *);
static int		 resolve_path(char **, const char *, fsl_id_t);
static int		 map_worktree_path(const char *);
static int		 map_repo_path(char **, const char *, fsl_id_t,
			    const char *);
static int		 xstrtonum(long *, const char *, const long,
			    const long);
static int		 fnc_prompt_input(struct fnc_view *, struct input *);
static int		 fnc_date_to_mtime(double *, const char *, int);
static int		 fnc_strftime(char *, size_t, const char *, time_t);
static int		 cook_input(char *, int, WINDOW *);
static int PRINTFV(3, 4) sitrep(struct fnc_view *, int, const char *, ...);
static char		*fnc_strsep (char **, const char *, size_t *);
static bool		 fnc_str_has_upper(const char *);
static int		 fnc_make_sql_glob(char **, char **, const char *);
static const char	*tzfile(void);
#ifndef HAVE_LANDLOCK
static int		 init_unveil(const char **, const char **, int, bool);
#else
static int		 init_landlock(const char **, const char **, const int);
#define init_unveil(_p, _m, _n, _d)	init_landlock((_p), (_m), (_n))
#endif  /* HAVE_LANDLOCK */
static int		 buf_putc(struct fsl_buffer *, int);
static int		 buf_write(struct fsl_buffer *, const void *, ssize_t);
static int PRINTFV(2, 3) buf_printf(struct fsl_buffer *, const char *, ...);
static const char	*getdirname(const char *, int, bool);
static int		 set_colours(struct fnc_colours *, enum fnc_view_id);
static int		 set_colour_scheme(struct fnc_colours *,
			    const int (*)[2], const char **, int);
static int		 init_colour(int *, enum fnc_opt_id);
static int		 default_colour(enum fnc_opt_id);
static void		 free_colours(struct fnc_colours *);
static bool		 fnc_home(struct fnc_view *);
static int		 fnc_conf_getopt(char **, enum fnc_opt_id, bool);
static int		 fnc_conf_setopt(enum fnc_opt_id, const char *, bool);
static int		 fnc_conf_lsopt(bool);
static enum fnc_opt_id	 fnc_conf_str2enum(const char *);
static const char	*fnc_conf_enum2str(enum fnc_opt_id);
static struct fnc_colour	*get_colour(struct fnc_colours *, int);
static struct fnc_colour	*match_colour(struct fnc_colours *,
				    const char *);
static struct fnc_tree_entry	*get_tree_entry(struct fnc_tree_object *,
				    int);
static struct fnc_tree_entry	*find_tree_entry(struct fnc_tree_object *,
				    const char *, size_t);

int
main(int argc, char **argv)
{
	const struct fnc_cmd	*cmd;
	int			 ch, rc = FNC_RC_OK;
	bool			 hflag = false;

	if (isatty(fileno(stdin)) == 0)
		err(1, "stdin is not a tty");	/* guard against misuse */

	setlocale(LC_CTYPE, "");
	fnc_progname(argv[0]);

	while ((ch = getopt_long(argc, argv, "+hv", global_opt,
	    NULL)) != -1) {
		switch (ch) {
		case 'h':
			hflag = true;
			break;
		case 'v':
			return fnc_show_version();
		default:
			usage(NULL, 1);
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;
#if defined(__linux__) || defined(__HAIKU__)
	optind = 0;
#else
	optind = 1;
	optreset = 1;
#endif

	cmd = fnc_cmd_lookup(argv);
	if (hflag) {
		if (argc == 0) {
			usage(NULL, 0);
			/* NOTREACHED */
		}
		if (cmd != NULL) {
			usage(cmd, 0);
			/* NOTREACHED */
		}
	}

#ifdef __OpenBSD__
	/*
	 * This is the most restrictive set of promises suitable for all
	 * fnc commands except 'fnc stash' which needs further abilities
	 * so pledge() is called from cmd_stash() (cf. pledge(2)).
	 */
	if ((cmd == NULL || cmd != &fnc_commands[FNC_VIEW_STASH])
	    && pledge("stdio rpath wpath cpath flock tty unveil", NULL) == -1)
		err(1, "pledge");
#endif

	if (cmd == NULL) {
		if (argc != 1) {
			fprintf(stderr, "%s: %s\n", fnc__progname,
			    fnc_errorf(FNC_RC_BAD_CMD, "%s", *argv));
			usage(NULL, 1);
			/* NOTREACHED */
		}
		/* treat 'fnc arg' as 'fnc timeline arg' and 'arg' as a path */
		cmd = &fnc_commands[FNC_VIEW_TIMELINE];
		rc = cmd->f(2, (char *[]){ "timeline", *argv });
		if (rc == FNC_RC_NO_PATH) {
			fprintf(stderr, "invalid command or path: %s\n", *argv);
			usage(NULL, 1);
			/* NOTREACHED */
		}
	} else
		rc = cmd->f(argc, argv);

	endwin();
	if (rc)
		fprintf(stderr, "%s: %s\n", fnc__progname, RCSTR(rc));

	fnc_cx_close();
	return rc && rc != FNC_RC_BREAK ? EXIT_FAILURE : EXIT_SUCCESS;
}

static const struct fnc_cmd *
fnc_cmd_lookup(char **argv)
{
	const char	*arg;
	size_t		 len;
	int		 high, low, mid, ncmds;

	/* default to 'fnc timeline' when invoked with no arguments */
	if (argv == NULL || *argv == NULL)
		return &fnc_commands[FNC_VIEW_TIMELINE];

	arg = *argv;

	low = 0;
	len = strlen(arg);
	ncmds = nitems(fnc_commands);
	high = ncmds - 1;

	/* check if arg matches command alias verbatim */
	for (mid = 0; mid < ncmds; ++mid) {
		if (fnc_cmd_aliascmp(&fnc_commands[mid], arg) == 0)
			return &fnc_commands[mid];
	}

	/* check if arg matches command verbatim */
	while (low <= high) {
		int cmp;

		mid = (high + low) / 2;
		cmp = strcmp(arg, fnc_commands[mid].name);
		if (cmp == 0)
			return &fnc_commands[mid];
		else if (cmp < 0)
			high = mid - 1;
		else
			low = mid + 1;
	}

	/* check if arg partially matches _exactly one_ command */
	if (low < ncmds && strncmp(arg, fnc_commands[low].name, len) == 0) {
		/*
		 * Partial prefix match; confirm it's the only matching cmd.
		 */
		mid = low;
		do {
			if (fnc_commands[low].f != fnc_commands[mid].f) {
				RC(FNC_RC_AMBIGUOUS_CMD);
				return NULL;
			}
		} while (++low < ncmds &&
		    strncmp(arg, fnc_commands[low].name, len) == 0);

		if (mid >= 0)
			return &fnc_commands[mid];
	}

	RC(FNC_RC_BAD_CMD);
	return NULL;
}

static int
fnc_cx_open(const char *repo)
{
	struct fsl_cx	*f = NULL;
	int		 rc;

	if ((rc = fsl_cx_init(&f, NULL)) != 0)
		return RC_LIBF(rc, "fsl_cx_init");
	fcli.f = f;

	if (repo != NULL) {
		if ((rc = fsl_repo_open(f, repo)) != 0)
			return RC_LIBF(rc, "fsl_repo_open: %s", repo);
	} else if ((rc = fsl_ckout_open_dir(f, ".", true)) != 0)
		return RC_LIBF(rc, "fsl_ckout_open_dir");

	if (fsl_cx_db_repo(f) == NULL)
		return RC(FNC_RC_NO_REPO);

	if ((rc = fsl_ckout_fingerprint_check(f)) != 0)
		return RC_LIBF(rc, "fsl_ckout_fingerprint_check");

	if (!fnc__utc) {
		char *utc;

		if ((rc = fnc_conf_getopt(&utc, FNC_UTC, false)) != 0)
			return rc;
		if (utc != NULL && *utc != '\0')
			fnc__utc = true;
		free(utc);
	}

	return FNC_RC_OK;
}

static void
fnc_cx_close(void)
{
	struct fsl_cx *f;

	RC_RESET;
	fsl_error_clear(&fcli.err);
	fsl_pathfinder_clear(&fcli.paths.bins);

	if ((f = fcli_cx()) != NULL) {
		if (fsl_cx_txn_level(f)) {
			if (fsl_cx_txn_end(f, true))
				fprintf(stderr, "%s: rollback open db "
				    "transaction: failed\n", fnc__progname);
		}
		/*
		 * XXX Elide this block as fsl_close_scm_dbs() will be called
		 * via fsl_cx_finalize()::fsl__cx_reset()::fsl_cx_close_dbs().
		 */
		if (0 && fsl_cx_db_ckout(f) != NULL) {
			if (fsl_close_scm_dbs(f))
				fprintf(stderr,
				    "%s: close fossil databases: failed\n",
				    fnc__progname);
		}
		fsl_cx_finalize(f);
	}
	memset(&fcli, 0, sizeof(fcli));
}

static int
fnc_cmd_aliascmp(const struct fnc_cmd *cmd, const char *arg)
{
	const char *alias = cmd->aliases;

	if (arg == NULL || *arg == '\0')
		return 1;

	while (alias != NULL && *alias != '\0') {
		if (strcmp(alias, arg) == 0)
			return 0;

		alias = strchr(alias, '\0') + 1;
	}

	return 1;
}

static inline int
symtorid(fsl_id_t *rid, const char *commit, enum fsl_satype_e satype)
{
	struct fsl_cx	*f;
	int		 rc;

	*rid = 0;

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	rc = fsl_sym_to_rid(f, commit, satype, rid);
	switch (rc) {
	case FSL_RC_AMBIGUOUS:
		return RC(FNC_RC_AMBIGUOUS_ID, "%s", commit);
	case FSL_RC_NOT_A_REPO:
		return RC(FNC_RC_NO_REPO);	/* cannot happen */
	case FSL_RC_NOT_FOUND:
		switch (satype) {
		case FSL_SATYPE_CHECKIN:
			rc = FNC_RC_NO_COMMIT;
			break;
		case FSL_SATYPE_CONTROL:
			rc = FNC_RC_NO_TAG;
			break;
		case FSL_SATYPE_WIKI:
		case FSL_SATYPE_TICKET:
		case FSL_SATYPE_TECHNOTE:
		case FSL_SATYPE_FORUMPOST:
		default:
			rc = FNC_RC_NO_REF;
			break;
		}
		return RC(rc, "%s", commit);
	case FSL_RC_OK:
		if (*rid == 0)
			return RC(FNC_RC_NO_REF, "%s", commit);
		break;
	default:
		return RC_LIBF(rc, "fsl_sym_to_rid: %s", commit);
	}

	return FNC_RC_OK;
}

static int
cmd_timeline(int argc, char **argv)
{
	struct fnc_view		*v;
	struct fsl_cx		*f;
	const char		*branch = NULL, *commit = NULL, *glob = NULL;
	const char		*repo = NULL, *tag = NULL, *user = NULL;
	char			*path = NULL;
	char			 types[7];
	fsl_id_t		 rid = 0;
	long			 limit = 0;
	int			 ch, rc, ntypes = 0;
	bool			 colour = true;

	while ((ch = getopt_long(argc, argv, "+b:Cc:f:hn:r:T:t:u:z",
	    timeline_opt, NULL)) != -1) {
		switch (ch) {
		case 'b':
			branch = optarg;
			break;
		case 'C':
			colour = false;
			break;
		case 'c':
			commit = optarg;
			break;
		case 'f':
			glob = optarg;
			break;
		case 'h':
			usage_timeline(0);
			/* NOTREACHED */
		case 'n':
			rc = xstrtonum(&limit, optarg, INT_MIN, INT_MAX);
			if (rc != 0)
				return rc;
			break;
		case 'r':
			repo = optarg;
			break;
		case 'T':
			tag = optarg;
			break;
		case 't':
			if (ntypes == nitems(types) - 1)
				return RC(FNC_RC_RANGE,
				    "too many types: %s%s", types, optarg);
			if (optarg[1] != '\0' || strcspn(optarg, "cefgtw") > 0)
				return RC(FNC_RC_BAD_ARTIFACT, "%s", optarg);
			types[ntypes++] = *optarg;
			types[ntypes] = '\0';
			break;
		case 'u':
			user = optarg;
			break;
		case 'z':
			fnc__utc = true;
			break;
		default:
			usage_timeline(1);
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;

	if (argc > 1) {
		fprintf(stderr, "%s: too many arguments: %s\n",
		    fnc__progname, argv[1]);
		usage_timeline(1);
		/* NOTREACHED */
	}

	if ((rc = fnc_cx_open(repo)) != 0)
		return rc;
	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	if (commit != NULL) {
		rc = symtorid(&rid, commit, FSL_SATYPE_ANY);
		if (rc != 0)
			return rc;
	}

	if (user != NULL) {
		rc = validate_user(user);
		if (rc != 0)
			return rc;
	}

	if (argc == 1) {
		rc = resolve_path(&path, *argv, -1);
		if (rc != 0)
			return rc;
	}

	rc = init_curses(colour);
	if (rc)
		goto end;
	rc = init_unveil(
		((const char *[]){ REPODB, CKOUTDIR, fnc__tmpdir, tzfile() }),
		((const char *[]){ "rw", "rwc", "rwc", "r" }), 4, 1
	);
	if (rc != 0)
		goto end;

	rc = init_timeline_view(&v, 0, 0, rid, path, branch, glob, user, tag,
	    limit, ntypes == 0 ? NULL : types, colour);
	if (rc)
		goto end;

	rc = view_loop(v);

end:
	free(path);
	return rc;
}

static int
init_timeline_view(struct fnc_view **view, int y, int x, fsl_id_t rid,
    const char *path, const char *branch, const char *glob, const char *user,
    const char *tag, long limit, const char *types, bool colour)
{
	*view = view_open(0, 0, y, x, FNC_VIEW_TIMELINE);
	if (view == NULL)
		return RC(FNC_RC_CURSES, "view_open");

	return open_timeline_view(*view, rid, path, branch, glob, user, tag,
	    limit, types, colour);
}

/*
 * If in a work tree, canonicalise input_path to prefix if it is non-NULL,
 * else to the cwd, then strip the common prefix with the work tree root.
 * If not in a work tree, ignore prefix and only simplify input_path.
 * In either case, assign the result to *ret, which the caller must free.
 */
static int
fnc_canonpath(char **ret, const char *input_path, const char *prefix)
{
	struct fsl_cx		*f;
	struct fsl_buffer	 buf;
	char			*child = NULL;
	const char		*path, *rootpath;
	fsl_size_t		 rootlen;
	size_t			 len;
	int			 rc;

	*ret = NULL;
	memset(&buf, 0, sizeof(buf));

	if (input_path == NULL || *input_path == '\0')
		return FNC_RC_OK;
	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	if (!fsl_cx_has_ckout(f)) {
		/* no work tree, normalise path for map*path() to validate */
		*ret = strdup(input_path);
		if (*ret == NULL)
			return RC_ERRNO("strdup");
		fsl_file_simplify_name(*ret, -1, false);
		return FNC_RC_OK;
	}

	rootpath = fsl_cx_ckout_dir_name(f, &rootlen);
	rc = fsl_file_canonical_name2(prefix, input_path, &buf, 0);
	if (rc != FNC_RC_OK) {
		rc = RC_ERRNO("fsl_file_canonical_name2: %s", input_path);
		goto end;
	}

	if (fsl_buffer_cstr(&buf) == NULL || *fsl_buffer_cstr(&buf) == '\0')
		goto end;

	len = buf.used;
	path = fsl_buffer_cstr(&buf);
	if (len == rootlen - 1 && strncmp(path, rootpath, len) == 0) {
		/* path is the work tree root, which is represented by NULL */
		goto end;
	}

	if (len <= rootlen || !path_is_child(path, rootpath, rootlen)) {
		/* path is not a child of the work tree */
		rc = RC(FNC_RC_BAD_PATH, "'%s' is outside the work tree",
		    input_path);
		goto end;
	}

	rc = path_skip_common_ancestor(&child, rootpath, rootlen, path, len);

end:
	fsl_buffer_clear(&buf);
	if (rc != FNC_RC_OK)
		free(child);
	else
		*ret = child;
	return rc;
}

static bool
path_is_child(const char *child, const char *parent, size_t parentlen)
{
	if (parentlen == 0 || fnc_path_is_root_dir(parent))
		return true;

	if (parent[parentlen - 1] == '/')
		--parentlen;

	if (child == NULL || *child == '\0' || strlen(child) < parentlen)
		return false;
	if (strncmp(parent, child, parentlen) != 0)
		return false;
	if (child[parentlen] != '/')
		return false;

	return true;
}

/*
 * If path is the work tree or repository root, return 1 else return 0.
 * NULL, "/", and "." resolve to the work tree or repository root.
 * The dot "." is a special case due to fsl_ckout_filename_check()
 * using it to denote the work tree root. For this reason, "." can
 * only be used to denote the cwd if it is the work tree root.
 */
static bool
fnc_path_is_root_dir(const char *path)
{
	if (path == NULL || (*path == '.' && path[1] == '\0'))
		return true;
	if (*path == '\0')
		return false;

	while (*path == '/')
		++path;
	return (*path == '\0');
}

static int
path_skip_common_ancestor(char **child, const char *parent_abspath,
    size_t parentlen, const char *abspath, size_t len)
{
	size_t bufsz;

	*child = NULL;

	if (parentlen > 0 && parent_abspath[parentlen - 1] == '/')
		--parentlen;

	if (abspath == NULL || *abspath == '\0' || parentlen >= len)
		return RC(FNC_RC_BAD_PATH, "%s", abspath);
	if (parent_abspath != NULL &&
	    strncmp(parent_abspath, abspath, parentlen) != 0)
		return RC(FNC_RC_BAD_PATH, "%s", abspath);
	if (!fnc_path_is_root_dir(parent_abspath) && abspath[parentlen] != '/')
		return RC(FNC_RC_BAD_PATH, "%s", abspath);

	while (abspath[parentlen] == '/')
		++abspath;

	bufsz = len - parentlen;
	*child = malloc(bufsz);
	if (*child == NULL)
		return RC_ERRNO("malloc");

	if (memccpy(*child, abspath + parentlen, '\0', bufsz) == NULL) {
		free(*child);
		*child = NULL;
		return RC(FNC_RC_NO_SPACE, "memccpy");
	}

	return FNC_RC_OK;
}

static int
init_curses(bool colour)
{
	if (fnc_set_signals())
		return FNC_RC_FATAL;

	initscr();
	cbreak();
	noecho();
	nonl();
	intrflush(stdscr, FALSE);
	keypad(stdscr, TRUE);
	raw();			/* don't signal control characters */
	curs_set(0);
	set_escdelay(0);	/* make ESC return immediately */
#ifndef __linux__
	typeahead(-1);		/* don't disrupt screen update operations */
#endif

	if (colour && has_colors()) {
		start_color();
		use_default_colors();
	}
	/*
	 * XXX On some terminals, bold intersects the colour space making
	 * colours that use the intensity bit (e.g., yellow) unavailable.
	 * This breaks coloured output (e.g., xterm(1)) so use A_REVERSE by
	 * itself. I've not been able to find a more reliable solution with
	 * tiget{flag,num,str}(3) or term[_]?attrs(3) so use this ugly hack
	 * until the list of terminals known to be affected grows.
	 */
	if (strncasecmp(termname(), "xterm", 5) == 0)
		FLAG_CLR(fnc__highlight, A_BOLD);

	return FNC_RC_OK;
}

static int
fnc_set_signals(void)
{
	/*
	 * This verbose handling is a portability thing to avoid
	 * "uninitialized struct member X" warnings. sigaction's
	 * struct differs by platforms, so we can't know how to
	 * populate it.
	 */
	static struct sigaction sapipe;
	static struct sigaction sawinch;
	static struct sigaction sacont;
	static struct sigaction saint;
	static struct sigaction saterm;
	static int once = 0;
	if( !once ){
		once = 1;
		memset(&sapipe, 0,sizeof(struct sigaction));
		memset(&sawinch,0,sizeof(struct sigaction));
		memset(&sacont, 0,sizeof(struct sigaction));
		memset(&saint,  0,sizeof(struct sigaction));
		memset(&saterm, 0,sizeof(struct sigaction));
		sapipe.sa_handler = sigpipe_handler;
		sawinch.sa_handler = sigwinch_handler;
		sacont.sa_handler = sigcont_handler;
		saint.sa_handler = sigint_handler;
		saterm.sa_handler = sigterm_handler;
	}
	if (sigaction(SIGPIPE, &sapipe, NULL)
	    == -1)
		return RC_ERRNO("sigaction(SIGPIPE)");
	if (sigaction(SIGWINCH, &sawinch, NULL)
	    == -1)
		return RC_ERRNO("sigaction(SIGWINCH)");
	if (sigaction(SIGCONT, &sacont, NULL)
	    == -1)
		return RC_ERRNO("sigaction(SIGCONT)");
	if (sigaction(SIGINT, &saint, NULL)
	    == -1)
		return RC_ERRNO("sigaction(SIGINT)");
	if (sigaction(SIGTERM, &saterm, NULL)
	    == -1)
		return RC_ERRNO("sigaction(SIGTERM)");

	return FNC_RC_OK;
}

static struct fnc_view *
view_open(int nlines, int ncols, int begin_y, int begin_x,
    enum fnc_view_id vid)
{
	struct fnc_view *view;

	view = calloc(1, sizeof(*view));
	if (view == NULL)
		return NULL;

	view->vid = vid;
	view->lines = LINES;
	view->cols = COLS;
	view->nlines = nlines ? nlines : LINES - begin_y;
	view->ncols = ncols ? ncols : COLS - begin_x;
	view->begin_y = begin_y;
	view->begin_x = begin_x;
	view->window = newwin(nlines, ncols, begin_y, begin_x);
	if (view->window == NULL) {
		view_close(view);
		return NULL;
	}
	view->panel = new_panel(view->window);
	if (view->panel == NULL || set_panel_userptr(view->panel, view) != OK) {
		view_close(view);
		return NULL;
	}

	keypad(view->window, TRUE);
	return view;
}

static int
basecommit_isvisible(bool *visible, struct fsl_stmt *q, fsl_id_t rid, int n)
{
	struct fsl_cx	*f;
	fsl_time_t	 mtime;
	int		 i, rc;

	*visible = 0;

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");
	if (!fsl_cx_has_ckout(f))
		return RC(FNC_RC_NO_CKOUT);

	if (rid > 0) {
		rc = fsl_mtime_of_manifest_file(f, rid, -1, &mtime);
		if (rc != FNC_RC_OK)
			return RC_LIBF(rc, "fsl_mtime_of_manifest_file");
		if (mtime < fsl_julian_to_unix(f->db.ckout.mtime)) {
			/*
			 * The work tree base commit will not be loaded
			 * because log traversal starts from an older commit.
			 */
			return FNC_RC_OK;
		}
	}

	for (i = 0; i < n && (rc = fsl_stmt_step(q)) == FSL_RC_STEP_ROW; ++i) {
		rc = fsl_stmt_get_id(q, 3, &rid);
		if (rc != 0)
			return RC(FNC_RC_NO_RID, "fsl_stmt_get_id");

		if (rid == f->db.ckout.rid) {
			*visible = 1;
			break;	/* base commit is on the first page */
		}
	}

	if (rc != 0 && rc != FSL_RC_STEP_ROW && rc != FSL_RC_STEP_DONE)
		return RC_LIBF(rc, "fsl_stmt_step");
	if ((rc = fsl_stmt_reset(q)) != 0)
		return RC_LIBF(rc, "fsl_stmt_reset");

	return FNC_RC_OK;
}

static int
open_timeline_view(struct fnc_view *view, fsl_id_t rid, const char *path,
    const char *branch, const char *glob, const char *user, const char *tag,
    long limit, const char *types, bool colour)
{
	struct fnc_tl_view_state	*s = &view->state.timeline;
	struct fsl_cx			*f;
	struct fsl_db			*db;
	struct fsl_buffer		 sql = fsl_buffer_empty;
	char				*op = NULL, *str = NULL;
	fsl_id_t			 idtag = 0;
	int				 rc;

	TAILQ_INIT(&s->commits.head);

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");
	if ((db = fsl_needs_repo(f)) == NULL)
		return RC(FNC_RC_NO_REPO);

	if (path != s->path) {
		free(s->path);
		if (path) {
			s->path = strdup(path);
			if (s->path == NULL) {
				rc = RC_ERRNO("strdup");
				goto end;
			}
		}
	}

	rc = pthread_cond_init(&s->thread_cx.commit_consumer, NULL);
	if (rc) {
		RC_ERRNO_SET(rc, "pthread_cond_init");
		goto end;
	}
	rc = pthread_cond_init(&s->thread_cx.commit_producer, NULL);
	if (rc) {
		RC_ERRNO_SET(rc, "pthread_cond_init");
		goto end;
	}

	if ((rc = fsl_buffer_reserve(&sql, 1024)) != 0) {
		rc = RC_ERRNO("malloc");
		goto end;
	}

	if (buf_printf(&sql,
	    "SELECT"
	    "    uuid,"
	    "    datetime(event.mtime%s),"
	    "    coalesce(euser, user),"
	    "    rid AS rid,"
	    "    event.type AS eventtype,"
	    "    ("
	    "        SELECT group_concat(substr(tagname,5), ',')"
	    "        FROM tag, tagxref"
	    "        WHERE tagname GLOB 'sym-*'"
	    "        AND tag.tagid=tagxref.tagid"
	    "        AND tagxref.rid=blob.rid"
	    "        AND tagxref.tagtype > 0"
	    "    ) as tags,"
	    "    coalesce(ecomment, comment) AS comment "
	    "FROM event JOIN blob "
	    "WHERE blob.rid=event.objid",
	    fnc__utc ? "" : ", 'localtime'") == -1) {
		rc = RC_LIBF(sql.errCode, "buf_printf");
		goto end;
	}

	if (types != NULL) {
		if (buf_write(&sql, " AND (", 6) == -1) {
			rc = RC_LIBF(sql.errCode, "buf_write");
			goto end;
		}
		while (*types != '\0') {
			if (fsl_buffer_appendf(&sql, "eventtype='%c%s'%s",
			    *types, *types == 'c' ? "i" : "",
			    *(types + 1) != '\0' ? " OR " : ")") != 0) {
				rc = RC_LIBF(sql.errCode, "fsl_buffer_appendf");
				goto end;
			}
			++types;
		}
	}

	if (branch != NULL) {
		rc = fnc_make_sql_glob(&op, &str, branch);
		if (rc)
			goto end;

		idtag = fsl_db_g_id(db, 0,
		    "SELECT tagid FROM tag WHERE tagname %q 'sym-%q'"
		    " AND EXISTS(SELECT 1 FROM tagxref"
		    " WHERE tag.tagid = tagxref.tagid AND tagtype > 0)"
		    " ORDER BY tagid DESC", op, str);
		if (idtag == 0) {
			rc = RC(FNC_RC_NO_BRANCH, "%s", branch);
			goto end;
		}
		if (buf_printf(&sql,
		    " AND EXISTS("
		    "    SELECT 1 FROM tagxref"
		    "    WHERE tagid=%"FSL_ID_T_PFMT
		    "    AND tagtype > 0 AND rid=blob.rid"
		    " )", idtag) == -1) {
			rc = RC_LIBF(sql.errCode, "buf_printf");
			goto end;
		}
	}

	if (tag != NULL) {
		free(op);
		free(str);
		/* lookup non-branch tag first; if not found, lookup branch */
		rc = fnc_make_sql_glob(&op, &str, tag);
		if (rc)
			goto end;

		idtag = fsl_db_g_id(db, 0,
		    "SELECT tagid FROM tag WHERE tagname %q '%q'"
		    " ORDER BY tagid DESC", op, str);
		if (idtag == 0)
			idtag = fsl_db_g_id(db, 0,
			    "SELECT tagid FROM tag WHERE tagname %q 'sym-%q'"
			    " ORDER BY tagid DESC", op, str);
		if (idtag == 0) {
			rc = RC(FNC_RC_NO_TAG, "%s", tag);
			goto end;
		}
		if (buf_printf(&sql,
		    " AND EXISTS("
		    "    SELECT 1 FROM tagxref"
		    "    WHERE tagid=%"FSL_ID_T_PFMT
		    "    AND tagtype > 0 AND rid=blob.rid"
		    " )", idtag) == -1) {
			rc = RC_LIBF(sql.errCode, "buf_printf");
			goto end;
		}
	}

	if (user != NULL) {
		free(op);
		free(str);
		rc = fnc_make_sql_glob(&op, &str, user);
		if (rc)
			goto end;
		if (fsl_buffer_appendf(&sql,
		    " AND coalesce(euser, user) %q '%q'", op, str) != 0) {
			rc = RC_LIBF(sql.errCode, "fsl_buffer_appendf");
			goto end;
		}
	}

	if (glob != NULL) {  /* filter commits on comment, user, and branch */
		free(op);
		free(str);
		rc = fnc_make_sql_glob(&op, &str, glob);
		if (rc)
			goto end;

		idtag = fsl_db_g_id(db, 0,
		    "SELECT tagid FROM tag WHERE tagname %q 'sym-%q'"
		    " ORDER BY tagid DESC", op, str);
		if (fsl_buffer_appendf(&sql,
		    " AND (coalesce(ecomment, comment) %q %Q"
		    " OR coalesce(euser, user) %q %Q%c",
		    op, str, op, str, idtag ? ' ' : ')') != 0) {
			rc = RC_LIBF(sql.errCode, "fsl_buffer_appendf");
			goto end;
		}
		if (idtag > 0) {
			if (buf_printf(&sql,
			    " OR EXISTS("
			    "    SELECT 1 FROM tagxref"
			    "    WHERE tagid=%"FSL_ID_T_PFMT
			    "    AND tagtype > 0 AND rid=blob.rid"
			    " ))", idtag) == -1) {
				rc = RC_LIBF(sql.errCode, "buf_printf");
				goto end;
			}
		}
	}

	if (rid) {
		if (buf_printf(&sql,
		    " AND event.mtime <= ("
		    "    SELECT mtime FROM event WHERE objid=%"FSL_ID_T_PFMT
		    " )", rid) == -1) {
			rc = RC_LIBF(sql.errCode, "buf_printf");
			goto end;
		}
	}

	/*
	 * If path is not root (NULL, /, .), a tracked path in the repository
	 * has been requested, only retrieve commits involving path.
	 */
	if (path != NULL) {
		if (buf_write(&sql,
		    " AND EXISTS(SELECT 1 FROM mlink"
		    " WHERE mlink.mid = event.objid"
		    " AND mlink.fnid IN ", -1) == -1) {
			rc = RC_LIBF(sql.errCode, "buf_write");
			goto end;
		}
		if (fsl_cx_is_case_sensitive(f, false)) {
			if (fsl_buffer_appendf(&sql, "("
			    "    SELECT fnid FROM filename"
			    "    WHERE name = %Q OR name GLOB '%q/*'"
			    ")", path, path) != 0) {
				rc = RC_LIBF(sql.errCode, "fsl_buffer_appendf");
				goto end;
			}
		} else {
			if (fsl_buffer_appendf(&sql, "("
			    "    SELECT fnid FROM filename"
			    "    WHERE name = %Q COLLATE nocase"
			    "    OR lower(name) GLOB lower('%q/*')"
			    ")", path, path) != 0) {
				rc = RC_LIBF(sql.errCode, "fsl_buffer_appendf");
				goto end;
			}
		}
		if (buf_putc(&sql, ')') == -1) {
			rc = RC_LIBF(sql.errCode, "buf_putc");
			goto end;
		}
	}

	if (buf_write(&sql, " ORDER BY event.mtime DESC", 26) == -1) {
		rc = RC_LIBF(sql.errCode, "buf_write");
		goto end;
	}

	if (limit > 0) {
		if (buf_printf(&sql, " LIMIT %ld", limit) == -1) {
			rc = RC_LIBF(sql.errCode, "buf_printf");
			goto end;
		}
	}

	view->show = show_timeline_view;
	view->input = tl_input_handler;
	view->resize = resize_timeline_view;
	view->close = close_timeline_view;
	view->grep_init = tl_grep_init;
	view->grep = tl_search_next;

	s->thread_cx.q = fsl_stmt_malloc();
	if (s->thread_cx.q == NULL) {
		rc = RC_ERRNO("fsl_stmt_malloc");
		goto end;
	}

	rc = fsl_db_prepare(db, s->thread_cx.q, "%b", &sql);
	if (rc != 0) {
		rc = RC_DB(db, rc, "fsl_db_prepare");
		goto end;
	}

	if (fsl_cx_has_ckout(f)) {
		/*
		 * If the work tree base commit will be loaded
		 * and is on the first page, do not block in
		 * view_input() til its state marker is drawn.
		 */
		rc = basecommit_isvisible(&view->nodelay,
		    s->thread_cx.q, rid, view->nlines - 1);
		if (rc != FNC_RC_OK)
			goto end;
	}

	rc = fsl_stmt_step(s->thread_cx.q);
	switch (rc) {
	case FSL_RC_STEP_ROW:
		rc = FNC_RC_OK;
		break;
	case FSL_RC_STEP_ERROR:
		rc = RC(rc, "fsl_stmt_step");
		goto end;
	case FSL_RC_STEP_DONE:
		rc = RC(FNC_RC_NO_MATCH);
		goto end;
	default:
		if (db->error.code)
			rc = fsl_cx_uplift_db_error(f, db);
		goto end;
	}

	s->showmeta = true;
	s->thread_cx.db = db;
	s->thread_cx.spin_idx = 0;
	s->thread_cx.ncommits_needed = view->nlines - 1;
	s->thread_cx.commits = &s->commits;
	s->thread_cx.eotl = false;
	s->thread_cx.quit = &s->quit;
	s->thread_cx.first_commit_onscreen = &s->first_commit_onscreen;
	s->thread_cx.selected_entry = &s->selected_entry;
	s->thread_cx.searching = &view->searching;
	s->thread_cx.search_status = &view->search_status;
	s->thread_cx.regex = &view->regex;
	s->thread_cx.path = s->path;
	s->thread_cx.reset = true;
	s->thread_cx.ckout.idx = -1;

	if (has_colors() && COLORS) {
		STAILQ_INIT(&s->colours);
		rc = set_colours(&s->colours, FNC_VIEW_TIMELINE);
		if (rc)
			goto end;
		view->colour = colour;
	}

end:
	fsl_buffer_clear(&sql);
	free(str);
	free(op);
	if (rc) {
		if (view->close)
			view_close(view);
		else
			close_timeline_view(view);
	}
	return rc;
}

static int
validate_user(const char *usr)
{
	struct fsl_cx	*f;
	struct fsl_db	*db;
	char		*op, *str;
	int		 rc, n = 0;

	if (usr == NULL)
		return FNC_RC_OK;

	f = fcli_cx();
	if (f == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	db = fsl_needs_repo(f);
	if (db == NULL)
		return RC(FNC_RC_NO_REPO);

	rc = fnc_make_sql_glob(&op, &str, usr);
	if (rc)
		return rc;

	n = fsl_db_g_int32(db, 0, "SELECT count(*) FROM event"
	    " WHERE user %q %Q OR euser %q %Q", op, str, op, str);

	free(op);
	free(str);
	return n ? FNC_RC_OK : RC(FNC_RC_NO_USER, "%s", usr);
}

/*
 * Wrapper around map_repo_path(). Try to resolve path with the cwd or the
 * work tree root as its prefix. For example, if the cwd is "subdir" of the
 * work tree root and path is "src/code.c", lookup the following paths:
 *.	/subdir/src/code.c
 *.	/src/code.c
 */
static int
resolve_path(char **ret, const char *path, fsl_id_t rid)
{
	const char	*wt;
	char		 cwd[PATH_MAX];
	uint64_t	 len;
	int		 rc;

	/* use cwd as path prefix */
	rc = map_repo_path(ret, path, rid, NULL);
	if (rc == FNC_RC_OK || rc != FNC_RC_NO_PATH)
		return rc;

	/*
	 * Path may be relative to the root of the work tree.
	 * If the user's cwd is not the work tree root,
	 * retry with the work tree root as the path prefix.
	 */
	if (getcwd(cwd, sizeof(cwd))== NULL)
		return RC_ERRNO("getcwd");

	/* fsl_cx_ckout_dir_name() returns NULL if fcli_cx() is NULL */
	wt = fsl_cx_ckout_dir_name(fcli_cx(), &len);
	if (wt == NULL)
		return rc;

	if (strncmp(wt, cwd, MAX(len - 1, strlen(cwd))) == 0)
		return rc;

	return map_repo_path(ret, path, rid, CKOUTDIR);
}

/*
 * Map arg to a repository path and assign the result to *ret, which must
 * be freed by the caller. If rid is >0, the mapped path must exist in the
 * tree of the commit identified by rid. If rid is zero, the path must exist
 * in the current work tree. If prefix is non-NULL, prepend it to arg before
 * mapping, otherwise prepend the cwd. If arg is absolute, ignore prefix and
 * map arg verbatim. On failure, *ret is NULL and nonzero is returned.
 */
static int
map_repo_path(char **ret, const char *arg, fsl_id_t rid, const char *prefix)
{
	char	*path;
	int	 n, rc = RC_RESET;

	*ret = NULL;

	if (arg == NULL)
		return RC(FNC_RC_BAD_PATH);

	rc = fnc_canonpath(&path, arg, prefix);
	if (rc != FNC_RC_OK || path == NULL)
		return rc;

	if (rid > 0)
		rc = map_version_path(path, rid);
	else if (rid == 0)
		rc = map_worktree_path(path);
	else {
		struct fsl_cx	*f;
		struct fsl_db	*db;

		f = fcli_cx();
		if (f == NULL)
			return RC(FNC_RC_FATAL, "fcli_cx");

		db = fsl_needs_repo(f);
		if (db == NULL) {
			rc = RC(FNC_RC_NO_REPO);
			goto end;
		}

		if (fsl_cx_is_case_sensitive(f, false))
			n = fsl_db_g_int32(db, 0,
			    "SELECT count(*) FROM filename"
			    " WHERE name=%Q OR name GLOB '%q/*'", path, path);
		else
			n = fsl_db_g_int32(db, 0,
			    "SELECT count(*) FROM filename"
			    " WHERE name = %Q COLLATE nocase"
			    " OR lower(name) GLOB lower('%q/*')", path, path);
		if (n == 0)
			rc = RC(FNC_RC_NO_PATH, "%s", path);
	}

end:
	if (rc != FNC_RC_OK)
		free(path);
	else
		*ret = path;
	return rc;
}

/*
 * Verify that path resolves to a path on disk in the work tree.
 * If path is not absolute, prepend the work tree root before mapping.
 */
static int
map_worktree_path(const char *path)
{
	struct fsl_cx	*f;
	struct stat	 sb;
	char		*abspath, *resolved = NULL;
	int		 rc = RC_RESET;

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");
	if (!fsl_cx_has_ckout(f))
		return RC(FNC_RC_NO_CKOUT);

	if (*path != '/') {
		abspath = fsl_mprintf("%s%s", CKOUTDIR, path);
		if (abspath == NULL)
			return RC_ERRNO("fsl_mprintf");
	} else {
		abspath = strdup(path);
		if (abspath == NULL)
			return RC_ERRNO("stdrup");
	}

	if (lstat(abspath, &sb) == -1) {
		if (errno != ENOENT) {
			rc = RC_ERRNO("lstat: %s", path);
			goto end;
		}
		sb.st_mode = 0;
	}

	if (!S_ISLNK(sb.st_mode)) {
		/*
		 * Don't use realpath(3) on symlinks because
		 * we only use the symlink--not target--path.
		 */
		resolved = realpath(abspath, NULL);
		if (resolved == NULL) {
			if (errno != ENOENT)
				rc = RC_ERRNO("realpath: %s", path);
			else
				rc = RC(FNC_RC_NO_PATH, "%s", path);
		}
	}

end:
	free(resolved);
	free(abspath);
	return rc;
}

static int
view_loop(struct fnc_view *view)
{
	struct view_tailhead	 views;
	struct fnc_view		*new_view;
	char			*mode;
	int			 rcpt, rc = FNC_RC_OK, done = 0;

	rc = fnc_conf_getopt(&mode, FNC_VIEW_SPLIT_MODE, false);
	if (rc != FNC_RC_OK)
		return rc;
	if (mode == NULL || (*mode != 'h' && *mode != 'H'))
		view->mode = VIEW_SPLIT_VERT;
	else
		view->mode = VIEW_SPLIT_HRZN;
	free(mode);

	rc = pthread_mutex_lock(&fnc__mutex);
	if (rc)
		return RC_ERRNO_SET(rc, "pthread_mutex_lock");

	TAILQ_INIT(&views);
	TAILQ_INSERT_HEAD(&views, view, entries);

	view->active = true;
	rc = view->show(view);
	if (rc)
		return rc;

	while (!TAILQ_EMPTY(&views) && !done && !fatal_signal()) {
		rc = view_input(&new_view, &done, view, &views);
		if (rc)
			break;
		if (view->egress) {
			struct fnc_view *v, *prev = NULL;

			if (view_is_parent(view))
				prev = TAILQ_PREV(view, view_tailhead, entries);
			else if (view->parent)
				prev = view->parent;

			if (view->parent) {
				view->parent->child = NULL;
				view->parent->focus_child = false;
				/* restore fullscreen line height */
				view->parent->nlines = view->parent->lines;
				rc = view_resize(view->parent);
				if (rc)
					goto end;
				/* persist resized split dimensions */
				view_copy_size(view->parent, view);
			} else
				TAILQ_REMOVE(&views, view, entries);

			rc = view_close(view);
			if (rc)
				goto end;

			view = NULL;
			TAILQ_FOREACH(v, &views, entries) {
				if (v->active)
					break;
			}
			if (view == NULL && new_view == NULL) {
				/* No view is active; try to pick one. */
				if (prev)
					view = prev;
				else if (!TAILQ_EMPTY(&views))
					view = TAILQ_LAST(&views,
					    view_tailhead);
				if (view) {
					if (view->focus_child) {
						view->child->active = true;
						view = view->child;
					} else
						view->active = true;
				}
			}
		}
		if (new_view) {
			struct fnc_view *v, *t;

			/* allow only one parent view per type */
			TAILQ_FOREACH_SAFE(v, &views, entries, t) {
				if (v->vid != new_view->vid)
					continue;
				TAILQ_REMOVE(&views, v, entries);
				rc = view_close(v);
				if (rc)
					goto end;
				break;
			}
			TAILQ_INSERT_TAIL(&views, new_view, entries);
			view = new_view;
		}
		if (view) {
			if (view_is_parent(view)) {
				if (view->child && view->child->active)
					view = view->child;
			} else {
				if (view->parent && view->parent->active)
					view = view->parent;
			}
			show_panel(view->panel);
			if (view->child && view_is_split(view->child))
				show_panel(view->child->panel);
			if (view->parent && view_is_split(view)) {
				rc = view->parent->show(view->parent);
				if (rc)
					goto end;
			}
			rc = view->show(view);
			if (rc)
				goto end;
			if (view->child) {
				rc = view->child->show(view->child);
				if (rc)
					goto end;
				updatescreen(view->child->window, false, false);
			}
			updatescreen(view->window, true, true);
		}
	}

end:
	while (!TAILQ_EMPTY(&views)) {
		int rc2;

		view = TAILQ_FIRST(&views);
		TAILQ_REMOVE(&views, view, entries);
		rc2 = view_close(view);
		if (rc2 && rc == FNC_RC_OK)
			rc = rc2;
	}
	rcpt = pthread_mutex_unlock(&fnc__mutex);
	if (rcpt && rc == FNC_RC_OK)
		rc = RC_ERRNO_SET(rcpt, "pthread_mutex_unlock");
	return rc;
}

static int
show_timeline_view(struct fnc_view *view)
{
	struct fnc_tl_view_state	*s = &view->state.timeline;
	int				 rc = FNC_RC_OK;

	/*
	 * pthread_t is a pointer type to a struct pthread on OpenBSD but is
	 * an arithmetic type on linux so compare to 0 to work in both cases.
	 */
	if (s->thread_id == 0) {
		rc = pthread_create(&s->thread_id, NULL, tl_producer_thread,
		    &s->thread_cx);
		if (rc)
			return RC_ERRNO_SET(rc, "pthread_create");
		if (s->thread_cx.ncommits_needed > 0) {
			rc = signal_tl_thread(view, 1);
			if (rc)
				return rc;
		}
	}

	return draw_commits(view);
}

static void *
tl_producer_thread(void *state)
{
	struct fnc_tl_thread_cx	*cx = state;
	struct fsl_cx		*fx;
	int			 rc;
	bool			 done = false;

	if ((fx = fcli_cx()) == NULL)
		return (void *)(intptr_t)(RC(FNC_RC_FATAL, "fcli_cx"));

	rc = block_main_thread_signals();
	if (rc)
		return (void *)(intptr_t)rc;

	while (!done && !fatal_signal()) {
		rc = build_commits(cx);
		if (rc == FSL_RC_STEP_DONE) {
			cx->ncommits_needed = 0;
			done = true;
		} else if (rc != FSL_RC_STEP_ROW)
			break;

		if (cx->ncommits_needed > 0)
			cx->ncommits_needed--;

		rc = pthread_mutex_lock(&fnc__mutex);
		if (rc) {
			rc = RC_ERRNO_SET(rc, "pthread_mutex_lock");
			break;
		}

		if (*cx->first_commit_onscreen == NULL) {
			*cx->first_commit_onscreen =
			    TAILQ_FIRST(&cx->commits->head);
			*cx->selected_entry = *cx->first_commit_onscreen;
		} else if (*cx->quit)
			done = true;

		rc = pthread_cond_signal(&cx->commit_producer);
		if (rc) {
			rc = RC_ERRNO_SET(rc, "pthread_cond_signal");
			pthread_mutex_unlock(&fnc__mutex);
			break;
		}

		if (cx->ncommits_needed == 0 && fsl_cx_has_ckout(fx) &&
		    cx->ckout.state == FNC_CKOUT_STATE_UNKNOWN) {
			rc = pthread_mutex_unlock(&fnc__mutex);
			if (rc) {
				rc = RC_ERRNO_SET(rc, "pthread_mutex_unlock");
				break;
			}
			rc = fsl_ckout_changes_scan(fx);
			if (rc) {
				if (rc != FSL_RC_DB) {
					rc = RC(rc, "fsl_ckout_changes_scan");
					break;
				}
				/* checkout db is busy, try again */
			} else {
				cx->ckout.state = fsl_ckout_has_changes(fx) ?
				    FNC_CKOUT_STATE_CHANGED :
				    FNC_CKOUT_STATE_CLEAN;
			}
			rc = pthread_mutex_lock(&fnc__mutex);
			if (rc) {
				rc = RC_ERRNO_SET(rc, "pthread_mutex_lock");
				break;
			}
		}

		if (done)
			cx->ncommits_needed = 0;
		else if (cx->ncommits_needed == 0) {
			rc = pthread_cond_wait(&cx->commit_consumer,
			    &fnc__mutex);
			if (rc) {
				rc = RC_ERRNO_SET(rc, "pthread_cond_wait");
				break;
			}
			if (*cx->quit)
				done = true;
		}

		rc = pthread_mutex_unlock(&fnc__mutex);
		if (rc) {
			rc = RC_ERRNO_SET(rc, "pthread_mutex_unlock");
			break;
		}
	}

	cx->eotl = true;
	return (void *)(intptr_t)rc;
}

static int
block_main_thread_signals(void)
{
	sigset_t	set;
	int		rc;

	if (sigemptyset(&set) == -1)
		return RC_ERRNO("sigemptyset");

	/* bespoke handlers for SIGWINCH, SIGCONT, SIGINT, and SIGTERM */
	if (sigaddset(&set, SIGWINCH) == -1)
		return RC_ERRNO("sigaddset");
	if (sigaddset(&set, SIGCONT) == -1)
		return RC_ERRNO("sigaddset");
	if (sigaddset(&set, SIGINT) == -1)
		return RC_ERRNO("sigaddset");
	if (sigaddset(&set, SIGTERM) == -1)
		return RC_ERRNO("sigaddset");

	/* ncurses handles SIGTSTP. */
	if (sigaddset(&set, SIGTSTP) == -1)
		return RC_ERRNO("sigaddset");

	rc = pthread_sigmask(SIG_BLOCK, &set, NULL);
	if (rc)
		return RC_ERRNO_SET(rc, "pthread_sigmask");

	return FNC_RC_OK;
}

static inline int
idtorid(int *rid, const char *id, enum fnc_err_code rc)
{
	struct fsl_cx *f;

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	*rid = fsl_uuid_to_rid(f, id);
	if (*rid == 0)
		return RC(rc, "%s", id);
	if (*rid < 0)
		return RC_FCLI(FNC_RC_LIBFOSSIL, "fsl_uuid_to_rid: %s", id);

	return FNC_RC_OK;
}

static int
build_commits(struct fnc_tl_thread_cx *cx)
{
	int rc;

	if (cx->reset && cx->commits->ncommits > 0) {
		/*
		 * If a child view was opened, there may be cached stmts that
		 * necessitate resetting the commit builder stmt. Otherwise one
		 * of the APIs down the fsl_stmt_step() call stack fails;
		 * irrespective of whether fsl_db_prepare_cached() was used.
		 */
		size_t loaded = cx->commits->ncommits + 1;

		cx->reset = false;
		rc = fsl_stmt_reset(cx->q);
		if (rc)
			return RC_LIBF(rc, "fsl_stmt_reset");

		while (loaded != 0) {
			rc = fsl_stmt_step(cx->q);
			if (rc != FSL_RC_STEP_ROW)
				return RC(rc, "fsl_stmt_step");
			--loaded;
		}
	}
	/*
	 * Step through the given SQL query, passing each row to the commit
	 * builder to build commits for the timeline.
	 */
	do {
		struct fnc_commit_artifact	*commit = NULL;
		struct commit_entry		*entry;

		rc = commit_builder(&commit, 0, cx->q);
		if (rc)
			return rc;

		entry = malloc(sizeof(*entry));
		if (entry == NULL)
			return RC_ERRNO("malloc");

		entry->commit = commit;

		rc = pthread_mutex_lock(&fnc__mutex);
		if (rc)
			return RC_ERRNO_SET(rc, "pthread_mutex_lock");

		entry->idx = cx->commits->ncommits;
		TAILQ_INSERT_TAIL(&cx->commits->head, entry, entries);
		cx->commits->ncommits++;

		if (!cx->endjmp && *cx->searching == SEARCH_FORWARD &&
		    *cx->search_status == SEARCH_WAITING)
			if (find_commit_match(commit, cx->regex))
				*cx->search_status = SEARCH_CONTINUE;

		rc = pthread_mutex_unlock(&fnc__mutex);
		if (rc)
			return RC_ERRNO_SET(rc, "pthread_mutex_unlock");

	} while ((rc = fsl_stmt_step(cx->q)) == FSL_RC_STEP_ROW &&
	    *cx->searching == SEARCH_FORWARD &&
	    *cx->search_status == SEARCH_WAITING);

	return rc;
}

/*
 * Given prepared SQL statement q _XOR_ record ID rid, allocate and build the
 * corresponding commit artifact from the result set. The commit must
 * eventually be disposed of with fnc_commit_artifact_close().
 */
static int
commit_builder(struct fnc_commit_artifact **ptr, fsl_id_t rid, fsl_stmt *q)
{
	struct fsl_cx			*f;
	struct fsl_db			*db;
	struct fnc_commit_artifact	*commit;
	const char			*branch, *comment, *type;
	const char			*prefix = NULL;
	uint64_t			 commentlen;
	int				 rc = FNC_RC_OK;
	enum fnc_diff_type		 diff_type = FNC_DIFF_WIKI;

	*ptr = NULL;

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");
	if ((db = fsl_needs_repo(f)) == NULL)
		return RC(FNC_RC_NO_CKOUT);

	if (rid) {
		rc = fsl_db_prepare(db, q, "SELECT "
		    /* 0 */"uuid, "
		    /* 1 */"datetime(event.mtime%s), "
		    /* 2 */"coalesce(euser, user), "
		    /* 3 */"rid AS rid, "
		    /* 4 */"event.type AS eventtype, "
		    /* 5 */"(SELECT group_concat(substr(tagname,5), ',') "
		    "FROM tag, tagxref WHERE tagname GLOB 'sym-*' "
		    "AND tag.tagid=tagxref.tagid AND tagxref.rid=blob.rid "
		    "AND tagxref.tagtype > 0) as tags, "
		    /*6*/"coalesce(ecomment, comment) AS comment "
		    "FROM event JOIN blob WHERE blob.rid=%d AND event.objid=%d",
		    fnc__utc ? "" : ", 'localtime'", rid, rid);
		if (rc != 0)
			return RC_DB(db, rc, "fsl_db_prepare");

		rc = fsl_stmt_step(q);
		if (rc == FSL_RC_STEP_ROW)
			rc = FNC_RC_OK;
		else
			return RC_LIBF(rc, "fsl_stmt_step");
	}

	type = fsl_stmt_g_text(q, 4, NULL);
	comment = fsl_stmt_g_text(q, 6, &commentlen);

	switch (*type) {
	case 'c':
		type = "checkin";
		diff_type = FNC_DIFF_COMMIT;
		break;
	case 'w':
		type = "wiki";
		if (comment != NULL) {
			switch (*comment) {
			case '+':
				prefix = "Added: ";
				++comment;
				break;
			case '-':
				prefix = "Deleted: ";
				++comment;
				break;
			case ':':
				prefix = "Edited: ";
				++comment;
				break;
			default:
				break;
			}
		}
		break;
	case 'g':
		type = "tag";
		break;
	case 'e':
		type = "technote";
		break;
	case 't':
		type = "ticket";
		break;
	case 'f':
		type = "forum";
		break;
	};

	commit = calloc(1, sizeof(*commit));
	if (commit == NULL)
		return RC_ERRNO("calloc");

	/*
	 * XXX As of at least fossil 2.23, empty log messages are
	 * stored in the event table as "(no comment)" but I do not
	 * know if older versions store such cases as an empty string.
	 */
	if (comment != NULL && commentlen > 0) {
		if ((commit->comment = fsl_mprintf("%s%s",
		    prefix != NULL ? prefix : "", comment)) == NULL) {
			rc = RC_ERRNO("fsl_mprintf");
			goto end;
		}
	}

	if (!rid) {
		rc = fsl_stmt_get_id(q, 3, &rid);
		if (rc) {
			rc = RC(FNC_RC_NO_RID, "fsl_stmt_get_id");
			goto end;
		}
	}

	/* XXX is there a more efficient way to get the parent? */
	commit->puuid = fsl_db_g_text(db, NULL,
	    "SELECT uuid FROM plink, blob WHERE plink.cid=%d "
	    "AND blob.rid=plink.pid AND plink.isprim", rid);
	if (commit->puuid != NULL) {
		rc = idtorid(&commit->prid, commit->puuid, FNC_RC_NO_COMMIT);
		if (rc != 0)
			goto end;
	} else
		commit->prid = -1;  /* indicates initial root commit */

	commit->uuid = strdup(fsl_stmt_g_text(q, 0, NULL));
	if (commit->uuid == NULL) {
		rc = RC_ERRNO("strdup");
		goto end;
	}

	commit->type = strdup(type);
	if (commit->type == NULL) {
		rc = RC_ERRNO("strdup");
		goto end;
	}

	commit->timestamp = strdup(fsl_stmt_g_text(q, 1, NULL));
	if (commit->timestamp == NULL) {
		rc = RC_ERRNO("strdup");
		goto end;
	}

	commit->user = strdup(fsl_stmt_g_text(q, 2, NULL));
	if (commit->user == NULL) {
		rc = RC_ERRNO("strdup");
		goto end;
	}

	/*
	 * XXX The temporary is needed because some Fossil repos somehow have
	 * commits that are not on a branch; for example, in the Fossil repo:
	 * 6fa5570b9a2cc6e6  e9d7c5aa29c3fca8  99b1a1eae0e4a10f
	 * 7ed9c7e4a9eae1d8  70131d08e27bf47f  aeaef8fbb1d8c1a2
	 */
	branch = fsl_stmt_g_text(q, 5, NULL);
	if (branch != NULL) {
		commit->branch = strdup(branch);
		if (commit->branch == NULL) {
			rc = RC_ERRNO("strdup");
			goto end;
		}
	}

	commit->rid = rid;
	commit->diff_type = diff_type;

end:
	if (rc)
		free(commit);
	else
		*ptr = commit;
	return rc;
}

static int
signal_tl_thread(struct fnc_view *view, int wait)
{
	struct fnc_tl_thread_cx	*cx = &view->state.timeline.thread_cx;
	int			 rc = FNC_RC_OK;

	while (cx->ncommits_needed > 0) {
		if (cx->eotl)
			break;

		if (view->mode == VIEW_SPLIT_HRZN)
			cx->reset = true;

		/* wake timeline thread */
		rc = pthread_cond_signal(&cx->commit_consumer);
		if (rc)
			return RC_ERRNO_SET(rc, "pthread_cond_signal");

		/*
		 * Mutex will be released while view_loop().view_input() waits
		 * in wgetch(), at which point the timeline thread will run.
		 */
		if (!wait)
			break;

		/* show status update in timeline view */
		rc = show_timeline_view(view);
		if (rc)
			return rc;
		update_panels();
		doupdate();

		/* wait while the next commit is being loaded */
		rc = pthread_cond_wait(&cx->commit_producer, &fnc__mutex);
		if (rc)
			return RC_ERRNO_SET(rc, "pthread_cond_wait");

		/* show status update in timeline view */
		rc = show_timeline_view(view);
		if (rc)
			return rc;
		update_panels();
		doupdate();
	}

	return FNC_RC_OK;
}

static int
draw_commits(struct fnc_view *view)
{
	struct fnc_tl_view_state	*s = &view->state.timeline;
	struct fnc_tl_thread_cx		*tcx = &s->thread_cx;
	struct commit_entry		*entry = s->selected_entry;
	struct fnc_colour		*c = NULL;
	wchar_t				*wline;
	const char			*branch = NULL, *type = NULL;
	const char			*search_str = NULL, *uuid = NULL;
	char				*headln = NULL, *idxstr = NULL;
	attr_t				 rx = 0;
	int				 ncommits = 0, rc = FNC_RC_OK;
	int				 ncols_needed, maxlen = 0, wlen = 0;

	if (entry != NULL && !(view->searching != SEARCH_DONE &&
	    view->search_status == SEARCH_WAITING)) {
		uuid = entry->commit->uuid;
		branch = entry->commit->branch;
		type = entry->commit->type;
	}

	if (tcx->ncommits_needed > 0 && !tcx->eotl) {
		if ((idxstr = fsl_mprintf("[%d/%d] %s",
		    entry ? entry->idx + 1 : 0, s->commits.ncommits,
		    (view->searching && !view->search_status) ?
		    "searching..." : view->search_status == SEARCH_ABORTED ?
		    "aborted" : "loading...")) == NULL) {
			rc = RC_ERRNO("fsl_mprintf");
			goto end;
		}
	} else {
		if (view->searching) {
			switch (view->search_status) {
			case SEARCH_COMPLETE:
				search_str = "no more matches";
				break;
			case SEARCH_NO_MATCH:
				search_str = "no matches found";
				break;
			case SEARCH_WAITING:
				search_str = "searching...";
				/* FALL THROUGH */
			default:
				break;
			}
		}
		if ((idxstr = fsl_mprintf("[%d/%d]%s%s",
		    entry != NULL ? entry->idx + 1 : 0, s->commits.ncommits,
		    search_str != NULL || branch != NULL ? " " : "",
		    search_str != NULL ? search_str : branch != NULL ?
		    branch : "")) == NULL) {
			rc = RC_ERRNO("fsl_mprintf");
			goto end;
		}
	}

	/*
	 * Headline: <type> <uuid> <idxstr>
	 * If it exceeds view->ncols, truncate the hash so all other
	 * segments are drawn in full. A spinner character and 40 dots
	 * replaces "<type> " and "<uuid>", respectively, if they are NULL.
	 */
	ncols_needed = FSL_STRLEN_K256 + strlen(idxstr) + 2;
	if (type != NULL)
		ncols_needed += strlen(type);

	if (s->path && s->path[1]) {
		headln = fsl_mprintf("%s%c%.*s /%s %s", type != NULL ?
		    type : "", type != NULL ? ' ' : SPINNER[tcx->spin_idx],
		    view->ncols < ncols_needed ?
		    view->ncols - (ncols_needed - FSL_STRLEN_K256) :
		    FSL_STRLEN_K256, uuid != NULL ? uuid :
		    "........................................",
		    s->path, idxstr);
	} else {
		headln = fsl_mprintf("%s%c%.*s %s", type != NULL ? type : "",
		    type != NULL ? ' ' : SPINNER[tcx->spin_idx],
		    view->ncols < ncols_needed ?
		    view->ncols - (ncols_needed - FSL_STRLEN_K256) :
		    FSL_STRLEN_K256, uuid != NULL ? uuid :
		    "........................................", idxstr);
	}
	if (headln == NULL) {
		rc = RC_ERRNO("fsl_mprintf");
		goto end;
	}
	if (SPINNER[++tcx->spin_idx] == '\0')
		tcx->spin_idx = 0;
	rc = formatln(&wline, &wlen, NULL, headln, 0, view->ncols, 0, false);
	if (rc)
		goto end;

	werase(view->window);

	if (view_is_shared(view) || view->active)
		rx = fnc__highlight;
	if (view->colour)
		c = get_colour(&s->colours, FNC_COLOUR_COMMIT);
	if (c)
		rx |= COLOR_PAIR(c->scheme);
	wattron(view->window, rx);
	waddwstr(view->window, wline);
	while (wlen < view->ncols) {
		waddch(view->window, ' ');
		++wlen;
	}
	wattroff(view->window, rx);
	free(wline);
	if (view->nlines <= 1)
		goto end;

	/*
	 * Find the longest username and log message on the page to align the
	 * log message start column and compute max rightward scroll position.
	 */
	view->pos.maxx = 0;
	for (entry = s->first_commit_onscreen; entry != NULL;
	     entry = TAILQ_NEXT(entry, entries)) {
		wchar_t		*ws;
		char		*eol, *msg, *msg0, *user;
		int		 wlen;
		const int	 user_xpos = 12;  /* "YYYY-MM-DD " */

		if (ncommits >= view->nlines - 1)
			break;

		user = strdup(entry->commit->user);
		if (user == NULL) {
			rc = RC_ERRNO("strdup");
			goto end;
		}
		rc = formatuser(&ws, &wlen, user, view->cols, user_xpos);
		free(ws);
		free(user);
		if (rc)
			goto end;
		maxlen = MAX(maxlen, wlen);

		/* we only show log messages up to the first '\n' or '\0' */
		msg0 = strdup(entry->commit->comment);
		if (msg0 == NULL) {
			rc = RC_ERRNO("strdup");
			goto end;
		}
		msg = msg0;
		if (msg != NULL && *msg != '\0') {
			while (*msg == '\n')
				++msg;
			eol = strchr(msg, '\n');
			if (eol != NULL)
				*eol = '\0';
		}
		rc = formatln(&ws, &wlen, NULL, msg, 0, INT_MAX,
		    user_xpos + maxlen, false);
		free(ws);
		free(msg0);
		if (rc)
			goto end;
		view->pos.maxx = MAX(view->pos.maxx, wlen);
		++ncommits;
	}

	ncommits = 0;
	s->last_commit_onscreen = s->first_commit_onscreen;
	for (entry = s->first_commit_onscreen; entry != NULL;
	     entry = TAILQ_NEXT(entry, entries)) {
		bool tagged;

		if (ncommits >= MIN(view->nlines - 1, view->lines - 1))
			break;
		tagged = s->tag.one != NULL && s->tag.two == NULL &&
		    entry->commit == s->tag.one;

		if (ncommits == s->selected || tagged)
			wattr_on(view->window, fnc__highlight, NULL);
		rc = write_commit_line(view, entry, maxlen);
		if (rc)
			goto end;
		if (ncommits == s->selected || tagged)
			wattr_off(view->window, fnc__highlight, NULL);

		s->last_commit_onscreen = entry;
		++ncommits;
	}
	drawborder(view);

end:
	free(idxstr);
	free(headln);
	return rc;
}

static int
formatuser(wchar_t **ret, int *retlen, char *username, size_t limit,
    int xpos)
{
	char *lt;

	lt = strchr(username, '<');
	if (lt && lt[1] != '\0')
		username = lt + 1;
	username[strcspn(username, "@>")] = '\0';
	return formatln(ret, retlen, NULL, username, 0, limit, xpos, false);
}

static int
formatln(wchar_t **ret, int *retlen, int *retskip, const char *line, int skip,
    int wlimit, int xpos, bool expand)
{
	wchar_t		*wline = NULL;
	char		*exstr = NULL;
	size_t		 wlen;
	int		 cols, i, rc, wskip;

	*ret = NULL;
	*retlen = 0;

	if (expand) {
		rc = expand_tab(&exstr, NULL, line);
		if (rc)
			return rc;
	}

	rc = mbs2ws(&wline, &wlen, expand ? exstr : line);
	free(exstr);
	if (rc)
		return rc;

	if (wlen > 0 && wline[wlen - 1] == L'\n') {
		wline[wlen - 1] = L'\0';
		wlen--;
	}
	if (wlen > 0 && wline[wlen - 1] == L'\r') {
		wline[wlen - 1] = L'\0';
		wlen--;
	}

	wskip = span_wline(&cols, 0, wline, skip, xpos);

	i = span_wline(&cols, wskip, wline, wlimit, xpos);
	wline[i] = L'\0';

	if (retlen)
		*retlen = cols;
	if (retskip)
		*retskip = wskip;
	*ret = wline;
	return FNC_RC_OK;
}

static int
span_wline(int *ret, int offset, wchar_t *wline, int nspan, int xpos)
{
	int width, i, cols = 0;

	if (nspan == 0) {
		*ret = cols;
		return offset;
	}

	for (i = offset; wline[i] != L'\0'; ++i) {
		if (wline[i] == L'\t')
			width = TABSIZE - ((cols + xpos) % TABSIZE);
		else
			width = wcwidth(wline[i]);

		if (width == -1) {
			width = 1;
			wline[i] = L'.';
		}

		if (nspan > 0 && cols + width > nspan)
			break;
		cols += width;
	}

	*ret = cols;
	return i;
}

/*
 * Copy the string src into the statically sized dst char array, and expand
 * any tab ('\t') characters found into the equivalent number of space (' ')
 * characters. Return number of bytes written to dst minus the terminating NUL.
 */
static int
expand_tab(char **ret, size_t *retlen, const char *src)
{
	char	*dst;
	size_t	 len, n, idx = 0, sz = 0;

	*ret = NULL;
	n = len = strlen(src);
	dst = malloc(n + 1);
	if (dst == NULL)
		return RC_ERRNO("malloc");

	while (idx < len && src[idx]) {
		const char c = src[idx];

		if (c == '\t') {
			size_t nb = TABSIZE - sz % TABSIZE;
			char *p;

			p = realloc(dst, n + nb);
			if (p == NULL) {
				free(dst);
				return RC_ERRNO("realloc");

			}
			dst = p;
			n += nb;
			memset(dst + sz, ' ', nb);
			sz += nb;
		} else
			dst[sz++] = src[idx];
		++idx;
	}

	dst[sz] = '\0';
	*ret = dst;
	if (retlen != NULL)
		*retlen = sz;
	return FNC_RC_OK;
}

static int
mbs2ws(wchar_t **dst, size_t *dstlen, const char *src)
{
	char	*rep = NULL;
	int	 rc = FNC_RC_OK;

	*dst = NULL;

	/*
	 * mbstowcs POSIX extension specifies that the number of wchar that
	 * would be written are returned when first arg is a null pointer:
	 * https://en.cppreference.com/w/cpp/string/multibyte/mbstowcs
	 */
	*dstlen = mbstowcs(NULL, src, 0);
	if (*dstlen == (size_t)-1) {
		if (errno != EILSEQ)
			return RC_ERRNO("mbstowcs: %s", src);

		rc = replace_unicode(&rep, src);
		if (rc)
			return rc;

		*dstlen = mbstowcs(NULL, rep, 0);
		if (*dstlen == (size_t)-1) {
			rc = RC_ERRNO("mbstowcs: %s", src);
			goto end;
		}
	}

	*dst = calloc(*dstlen + 1, sizeof(**dst));
	if (*dst == NULL) {
		rc = RC_ERRNO("calloc");
		goto end;
	}

	if (mbstowcs(*dst, rep != NULL ? rep : src, *dstlen) != *dstlen)
		rc = RC_ERRNO("mbstowcs: %s", rep != NULL ? rep : src);

end:
	free(rep);
	if (rc) {
		free(*dst);
		*dst = NULL;
		*dstlen = 0;
	}
	return rc;
}

/*
 * Iterate mbs, writing each char to *ptr, and replace any non-printable or
 * unicode characters that are invalid in the environment's current character
 * encoding with a '?'. *ptr must eventually be disposed of by the caller.
 */
static int
replace_unicode(char **ptr, const char *mbs)
{
	const char	*src;
	char		*dst;
	wchar_t		 wc;
	int		 width, len;

	if (mbs == NULL || *mbs == '\0')
		return FNC_RC_OK;

	len = strlen(mbs);
	*ptr = malloc(len + 1);  /* NUL */
	if (*ptr == NULL)
		return RC_ERRNO("malloc");

	src = mbs;
	dst = *ptr;

	while (*src) {
		if ((len = mbtowc(&wc, src, MB_CUR_MAX)) == -1) { /* invalid */
			*dst++ = '?';
			++src;
		} else if (*src != '\r' && *src != '\n' &&
		    (width = wcwidth(wc)) == -1) {  /* not printable */
			*dst++ = '?';
			src += len;
		} else  /* valid */
			while (len-- > 0)
				*dst++ = *src++;
	}
	*dst = '\0';
	return FNC_RC_OK;
}

/*
 * When the terminal is >= 110 columns wide, the commit summary line in the
 * timeline view will take the form:
 *
 *   DATE UUID USERNAME  COMMIT-COMMENT
 *
 * Assuming an 8-character username, this scheme provides 80 characters for the
 * comment, which should be sufficient considering it's suggested good practice
 * to limit commit comment summary lines to a maximum 50 characters, and most
 * plaintext-based conventions suggest not exceeding 72-80 characters.
 *
 * When < 110 columns, the (abbreviated 9-character) UUID will be elided.
 */
static int
write_commit_line(struct fnc_view *view, struct commit_entry *ce, int maxlen)
{
	struct fnc_tl_view_state	*s = &view->state.timeline;
	struct fnc_colour		*c = NULL;
	struct fsl_cx			*fx;
	struct ckout_state		*ckout = &s->thread_cx.ckout;
	wchar_t				*wstr = NULL;
	char				*comment, *comment0 = NULL;
	char				*date, *eol;
	char				*user = NULL;
	size_t				 i = 0;
	int				 col, limit, wlen, skip;
	int				 rc = FNC_RC_OK;
	const int			 markercolumn = maxlen + 1;

	if ((fx = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	if (ce != NULL && fsl_cx_has_ckout(fx) && ckout->idx == -1) {
		fsl_id_t cid;

		fsl_ckout_version_info(fx, &cid, NULL);
		if (ce->commit->rid == cid)
			ckout->idx = ce->idx;
	}

	/* trim time component from timestamp for the date field */
	date = strdup(ce->commit->timestamp);
	if (date == NULL)
		return RC_ERRNO("strdup");
	while (!isspace((unsigned char)date[i++]))
		/* nop */;
	date[i] = '\0';
	col = MIN(view->ncols, ISO8601_DATE_ONLY + 1);  /* "YYYY-MM-DD " */
	if (view->colour)
		c = get_colour(&s->colours, FNC_COLOUR_DATE);
	if (c)
		wattr_on(view->window, COLOR_PAIR(c->scheme), NULL);
	waddnstr(view->window, date, col);
	if (c)
		wattr_off(view->window, COLOR_PAIR(c->scheme), NULL);
	if (col > view->ncols)
		goto end;

	if (view->ncols >= 110) {
		if (view->colour)
			c = get_colour(&s->colours, FNC_COLOUR_COMMIT);
		if (c)
			wattr_on(view->window, COLOR_PAIR(c->scheme), NULL);
		wprintw(view->window, "%.9s ", ce->commit->uuid);
		if (c)
			wattr_off(view->window, COLOR_PAIR(c->scheme), NULL);
		col += 10;
		if (col > view->ncols)
			goto end;
	}

	/*
	 * Parse username from emailaddr if needed, and postfix username
	 * with as much whitespace as needed to fill two spaces beyond
	 * the longest username on the screen.
	 */
	user = strdup(ce->commit->user);
	if (user == NULL) {
		rc = RC_ERRNO("strdup");
		goto end;
	}
	rc = formatuser(&wstr, &wlen, user, view->ncols - col, col);
	if (rc)
		goto end;
	if (view->colour)
		c = get_colour(&s->colours, FNC_COLOUR_USER);
	if (c)
		wattr_on(view->window, COLOR_PAIR(c->scheme), NULL);
	waddwstr(view->window, wstr);
	free(wstr);
	col += wlen;
	while (col < view->ncols && wlen < maxlen + 2) {
		if (wlen == markercolumn && ce->idx == ckout->idx &&
		    ckout->state != FNC_CKOUT_STATE_UNKNOWN) {
			/*
			 * Switch to blocking in view_input() til input is
			 * entered so views are not redrawn unnecessarily.
			 */
			view->nodelay = false;
			waddch(view->window, ckout->state);
		} else
			waddch(view->window, ' ');
		++col;
		++wlen;
	}
	if (c)
		wattr_off(view->window, COLOR_PAIR(c->scheme), NULL);
	if (col > view->ncols)
		goto end;

	/* Only show comment up to the first newline character. */
	comment0 = strdup(ce->commit->comment);
	if (comment0 == NULL) {
		rc = RC_ERRNO("strdup");
		goto end;
	}
	comment = comment0;
	while (*comment == '\n')
		++comment;
	eol = strchr(comment, '\n');
	if (eol)
		*eol = '\0';
	limit = view->ncols - col;
	rc = formatln(&wstr, &wlen, &skip, comment, view->pos.x, limit,
	    col, true);
	if (rc)
		goto end;
	waddwstr(view->window, &wstr[skip]);
	col += wlen;
	while (col < view->ncols) {
		waddch(view->window, ' ');
		++col;
	}

end:
	free(date);
	free(user);
	free(wstr);
	free(comment0);
	return rc;
}

static int
status_update(struct fnc_view *view)
{
	struct fnc_view	*v = view;
	WINDOW		*win = v->window;

	if (view_is_top_split(view)) {
		v = view->child;
		win = v->window;
	} else if (view->mode == VIEW_SPLIT_VERT && view->parent != NULL) {
		v = view->parent;
		win = stdscr;
	}

	if (wmove(win, v->nlines - 1, 0) == ERR)
		return RC(FNC_RC_CURSES, "wmove");
	if (wclrtoeol(win) == ERR)
		return RC(FNC_RC_CURSES, "wclrtoeol");
	if (wprintw(win, ":%s", view->status) == ERR) {
		/* ignore truncated string or scrollok (cf. waddch(3)) error */
		if (getcurx(win) != getmaxx(win) - 1)
			return RC(FNC_RC_CURSES, "wprintw");
	}
	if (wrefresh(win) == ERR)
		return RC(FNC_RC_CURSES, "wrefresh");

	free(view->status);
	view->status = NULL;
	return FNC_RC_OK;
}

static int
view_input(struct fnc_view **new, int *done, struct fnc_view *view,
    struct view_tailhead *views)
{
	struct fnc_view	*v;
	int		 rc, ch = 0;

	*new = NULL;

	if (view->status != NULL) {
		rc = status_update(view);
		if (rc != 0)
			return rc;
	}

	/* clear search indicator string */
	if (view->search_status == SEARCH_COMPLETE ||
	    view->search_status == SEARCH_NO_MATCH)
		view->search_status = SEARCH_CONTINUE;

	if (view->searching && view->search_status == SEARCH_WAITING) {
		rc = pthread_mutex_unlock(&fnc__mutex);
		if (rc)
			return RC_ERRNO_SET(rc, "pthread_mutex_unlock");
		sched_yield();
		rc = pthread_mutex_lock(&fnc__mutex);
		if (rc)
			return RC_ERRNO_SET(rc, "pthread_mutex_lock");
		return view->grep(view);
	}

	nodelay(view->window, view->nodelay);

	/* allow blame or log thread to work while waiting for input */
	rc = pthread_mutex_unlock(&fnc__mutex);
	if (rc)
		return RC_ERRNO_SET(rc, "pthread_mutex_unlock");
	/*
	 * XXX This check is not yet needed, but is pre-empting the NYI feature
	 * of calling fnc_stash from the diff_input_handler() with a key map.
	 */
	if (view->state.diff.diff_mode != DIFF_MODE_STASH)
		ch = wgetch(view->window);
	rc = pthread_mutex_lock(&fnc__mutex);
	if (rc)
		return RC_ERRNO_SET(rc, "pthread_mutex_lock");

	if (fnc__recv_sigwinch || fnc__recv_sigcont) {
		fnc_resizeterm();
		fnc__recv_sigwinch = 0;
		fnc__recv_sigcont = 0;
		TAILQ_FOREACH(v, views, entries) {
			rc = view_resize(v);
			if (rc)
				return rc;
			rc = v->input(new, v, KEY_RESIZE);
			if (rc)
				return rc;
			if (v->child) {
				rc = view_resize(v->child);
				if (rc)
					return rc;
				rc = v->child->input(new, v->child, KEY_RESIZE);
				if (rc)
					return rc;
				if (v->child->resized_y ||
				    v->child->resized_x) {
					rc = view_resize_split(v, 0);
					if (rc)
						return rc;
				}
			}
		}
	}

	switch (ch) {
	case '\t':
		rc = cycle_view(view);
		break;
	case KEY_F(1):
	case 'H':
	case '?':
		rc = help(view, done);
		break;
	case 'q':
		if (view->parent != NULL) {
			if (view->parent->vid == FNC_VIEW_TIMELINE) {
				rc = reset_tags(&view->parent->state.timeline);
				if (rc)
					return rc;
			}
			if (view->mode == VIEW_SPLIT_HRZN) {
				/* may need more commits to fill fullscreen */
				if (view->parent->resize != NULL) {
					rc = view->parent->resize(view->parent,
					    0);
					if (rc != FNC_RC_OK)
						break;
				}
				view_offset_scrollup(view->parent);
			}
		}
		rc = view->input(new, view, ch);
		view->egress = true;
		break;
	case 'f':
		rc = toggle_fullscreen(new, view);
		break;
	case '/':
		if (view->grep_init)
			view_search_start(view);
		else
			rc = view->input(new, view, ch);
		break;
	case 'N':
	case 'n':
		if (view->started_search && view->grep) {
			view->searching = (ch == 'n' ?
			    SEARCH_FORWARD : SEARCH_REVERSE);
			view->search_status = SEARCH_WAITING;
			rc = view->grep(view);
		} else
			rc = view->input(new, view, ch);
		break;
	case 's':
		rc = view_switch_split(view);
		break;
	case '(':
		rc = view_resize_split(view, -1);
		break;
	case ')':
		rc = view_resize_split(view, 1);
		break;
	case KEY_RESIZE:
		break;
	case ERR:
		break;
	case CTRL('c'):
	case 'Q':
		*done = 1;
		break;
	case CTRL('z'):
		raise(SIGTSTP);
		break;
	default:
		rc = view->input(new, view, ch);
		break;
	}

	if (rc == FNC_RC_BREAK)
		*done = 1;
	return rc;
}

/* Switch split mode. If view is a parent or child, draw the new splitscreen. */
static int
view_switch_split(struct fnc_view *view)
{
	struct fnc_view	*v = NULL;
	int		 rc;

	if (view->parent)
		v = view->parent;
	else
		v = view;

	if (v->mode == VIEW_SPLIT_HRZN)
		v->mode = VIEW_SPLIT_VERT;
	else
		v->mode = VIEW_SPLIT_HRZN;

	if (v->child == NULL)
		return FNC_RC_OK;
	else if (v->mode == VIEW_SPLIT_VERT && v->cols < 120)
		v->mode = VIEW_SPLIT_NONE;

	view_split_getyx(v, &v->child->begin_y, &v->child->begin_x);

	if (v->mode == VIEW_SPLIT_HRZN && v->child->resized_y)
		v->child->begin_y = v->child->resized_y;
	else if (v->mode == VIEW_SPLIT_VERT && v->child->resized_x)
		v->child->begin_x = v->child->resized_x;

	if (v->mode == VIEW_SPLIT_HRZN) {
		v->ncols = COLS;
		v->child->ncols = COLS;
		v->child->nscrolled = LINES - v->child->nlines;

		rc = view_split_horizontally(v, v->child->begin_y);
		if (rc)
			return rc;
	}
	v->child->mode = v->mode;
	v->child->nlines = v->lines - v->child->begin_y;
	v->focus_child = true;

	rc = make_fullscreen(v);
	if (rc)
		return rc;
	rc = make_splitscreen(v->child);
	if (rc)
		return rc;

	if (v->mode == VIEW_SPLIT_NONE)
		v->mode = VIEW_SPLIT_VERT;
	if (v->mode == VIEW_SPLIT_HRZN) {
		rc = view_offset_scrolldown(v);
		if (rc)
			return rc;
		rc = view_offset_scrolldown(v->child);
		if (rc)
			return rc;
	} else {
		view_offset_scrollup(v);
		view_offset_scrollup(v->child);
	}
	if (v->resize != NULL)
		rc = v->resize(v, 0);
	else if (v->child->resize != NULL)
		rc = v->child->resize(v->child, 0);

	return rc;
}

static int
view_resize_split(struct fnc_view *view, int resize)
{
	struct fnc_view	*v = NULL;
	int		 rc = FNC_RC_OK;

	if (view->parent)
		v = view->parent;
	else
		v = view;

	if (v->child == NULL || !view_is_split(v->child))
		return FNC_RC_OK;

	v->resizing = v->child->resizing = true;  /* lock for resize event */

	if (view->mode == VIEW_SPLIT_HRZN) {
		if (v->child->resized_y)
			v->child->begin_y = v->child->resized_y;
		if (view->parent)
			v->child->begin_y -= resize;
		else
			v->child->begin_y += resize;
		if (v->child->begin_y < 3) {
			v->child->begin_y = 3;
		} else if (v->child->begin_y > LINES - 1) {
			v->child->begin_y = LINES - 1;
		}
		v->ncols = COLS;
		v->child->ncols = COLS;
		view_adjust_offset(view, resize);
		rc = view_split_horizontally(v, v->child->begin_y);
		if (rc)
			return rc;
		v->child->resized_y = v->child->begin_y;
	} else {
		if (v->child->resized_x)
			v->child->begin_x = v->child->resized_x;
		if (view->parent)
			v->child->begin_x -= resize;
		else
			v->child->begin_x += resize;
		if (v->child->begin_x < 11)
			v->child->begin_x = 11;
		else if (v->child->begin_x > COLS - 1)
			v->child->begin_x = COLS - 1;
		v->child->resized_x = v->child->begin_x;
	}

	v->child->mode = v->mode;
	v->child->nlines = v->lines - v->child->begin_y;
	v->child->ncols = v->cols - v->child->begin_x;
	v->focus_child = true;

	rc = make_fullscreen(v);
	if (rc)
		return rc;
	rc = make_splitscreen(v->child);
	if (rc)
		return rc;

	if (v->mode == VIEW_SPLIT_HRZN) {
		rc = view_offset_scrolldown(v->child);
		if (rc)
			return rc;
	}

	if (v->resize != NULL)
		rc = v->resize(v, 0);
	else if (v->child->resize != NULL)
		rc = v->child->resize(v->child, 0);

	v->resizing = v->child->resizing = false;  /* unlock resize event */

	return rc;
}

static void
view_adjust_offset(struct fnc_view *view, int n)
{
	if (n == 0)
		return;

	if (view->parent != NULL && view->parent->pos.offset) {
		if (view->parent->pos.offset + n >= 0)
			view->parent->pos.offset += n;
		else
			view->parent->pos.offset = 0;
	} else if (view->pos.offset) {
		if (view->pos.offset - n >= 0)
			view->pos.offset -= n;
		else
			view->pos.offset = 0;
	}
}

static int
cycle_view(struct fnc_view *view)
{
	int rc;

	if (view->child) {
		view->active = false;
		view->child->active = true;
		view->focus_child = true;
	} else if (view->parent) {
		view->active = false;
		view->parent->active = true;
		view->parent->focus_child = false;
		if (!view_is_split(view)) {
			if (view->parent->resize != NULL) {
				rc = view->parent->resize(view->parent, 0);
				if (rc)
					return rc;
			}
			view_offset_scrollup(view->parent);
			rc = make_fullscreen(view->parent);
			if (rc)
				return rc;
		}
	}

	return FNC_RC_OK;
}

static int
toggle_fullscreen(struct fnc_view **new, struct fnc_view *view)
{
	int rc;

	if (view_is_parent(view)) {
		if (view->child == NULL)
			return FNC_RC_OK;
		if (view_is_split(view->child)) {
			rc = make_fullscreen(view->child);
			if (rc)
				return rc;
			rc = make_fullscreen(view);
			if (rc)
				return rc;
		} else {
			rc = make_splitscreen(view->child);
			if (rc)
				return rc;
		}
		rc = view->child->input(new, view->child, KEY_RESIZE);
		if (rc)
			return rc;
	} else {
		if (view_is_split(view))
			rc = make_fullscreen(view);
		else
			rc = make_splitscreen(view);
		if (rc)
			return rc;
		rc = view->input(new, view, KEY_RESIZE);
		if (rc)
			return rc;
	}
	if (view->resize != NULL) {
		rc = view->resize(view, 0);
		if (rc != FNC_RC_OK)
			return rc;
	}
	if (view->parent != NULL) {
		if (view->parent->resize != NULL) {
			rc = view->parent->resize(view->parent, 0);
			if (rc != FNC_RC_OK)
				return rc;
		}
		rc = view_offset_scrolldown(view->parent);
		if (rc)
			return rc;
	}

	return view_offset_scrolldown(view);
}

static int
stash_help(struct fnc_view *view, enum stash_mvmt scroll, int *done)
{
	char			*title = NULL;
	static const char	*keys[][2] = {
	    {"", ""},
	    {"", ""},
	    {"  b ", "  ❬b❭ "},
	    {"  m ", "  ❬m❭ "},
	    {"  y ", "  ❬y❭ "},
	    {"  n ", "  ❬n❭ "},
	    {"  a ", "  ❬a❭ "},
	    {"  k ", "  ❬k❭ "},
	    {"  A ", "  ❬A❭ "},
	    {"  K ", "  ❬K❭ "},
	    {"  Q ", "  ❬Q❭ "},
	    {"  ? ", "  ❬?❭ "},
	    {"", ""},
	    {"", ""},
	    {NULL, NULL}
	};
	static const char *desc[] = {
	    "",
	    "Stash",
	    "- scroll back to the previous page^",
	    "- show more of this hunk on the next page^",
	    "- stash this hunk",
	    "- do not stash this hunk",
	    "- stash this hunk and all remaining hunks in the file",
	    "- do not stash this hunk nor any remaining hunks in the file",
	    "- stash this hunk and all remaining hunks in the diff",
	    "- do not stash this hunk nor any remaining hunks in the diff",
	    "- abort fnc stash and discard any previous selections",
	    "- display this help screen",
	    "",
	    ""
	};
	int rc;

	title = fsl_mprintf("%s %s help ('q' to quit)\n",
	    fnc__progname, PRINT_VERSION);
	if (title == NULL)
		return RC_ERRNO("fsl_mprintf");

	rc = drawpad(view, keys, desc, title, scroll, done);
	free(title);
	return rc;
}

static int
help(struct fnc_view *view, int *done)
{
	char			*title = NULL;
	static const char	*keys[][2] = {
	    {"", ""},
	    {"", ""}, /* Global */
	    {"  H,?,F1           ", "  ❬H❭❬?❭❬F1❭      "},
	    {"  k,<Up>           ", "  ❬↑❭❬k❭          "},
	    {"  j,<Down>         ", "  ❬↓❭❬j❭          "},
	    {"  C-b,PgUp         ", "  ❬C-b❭❬PgUp❭     "},
	    {"  C-f,PgDn         ", "  ❬C-f❭❬PgDn❭     "},
	    {"  C-u,             ", "  ❬C-u❭           "},
	    {"  C-d,             ", "  ❬C-d❭           "},
	    {"  gg,Home          ", "  ❬gg❭❬Home❭      "},
	    {"  G,End            ", "  ❬G❭❬End❭        "},
	    {"  l,<Right>        ", "  ❬l❭❬→❭          "},
	    {"  h,<Left>         ", "  ❬h❭❬←❭          "},
	    {"  $                ", "  ❬$❭             "},
	    {"  0                ", "  ❬0❭             "},
	    {"  Tab              ", "  ❬TAB❭           "},
	    {"  C                ", "  ❬C❭             "},
	    {"  f                ", "  ❬f❭             "},
	    {"  s                ", "  ❬s❭             "},
	    {"  (                ", "  ❬(❭             "},
	    {"  )                ", "  ❬)❭             "},
	    {"  /                ", "  ❬/❭             "},
	    {"  n                ", "  ❬n❭             "},
	    {"  N                ", "  ❬N❭             "},
	    {"  q                ", "  ❬q❭             "},
	    {"  Q                ", "  ❬Q❭             "},
	    {"", ""},
	    {"", ""}, /* Timeline */
	    {"  <,,              ", "  ❬<❭❬,❭          "},
	    {"  >,.              ", "  ❬>❭❬.❭          "},
	    {"  Enter            ", "  ❬Enter❭         "},
	    {"  Space            ", "  ❬Space❭         "},
	    {"  b                ", "  ❬b❭             "},
	    {"  D                ", "  ❬D❭             "},
	    {"  F                ", "  ❬F❭             "},
	    {"  t                ", "  ❬t❭             "},
	    {"  <BS>             ", "  ❬⌫❭             "},
	    {"", ""},
	    {"", ""}, /* Diff */
	    {"  Space            ", "  ❬Space❭         "},
	    {"  #                ", "  ❬#❭             "},
	    {"  @                ", "  ❬@❭             "},
	    {"  C-e              ", "  ❬C-e❭           "},
	    {"  C-y              ", "  ❬C-y❭           "},
	    {"  C-p              ", "  ❬C-p❭           "},
	    {"  C-n              ", "  ❬C-n❭           "},
	    {"  [                ", "  ❬[❭             "},
	    {"  ]                ", "  ❬]❭             "},
	    {"  b                ", "  ❬b❭             "},
	    {"  B                ", "  ❬B❭             "},
	    {"  D                ", "  ❬D❭             "},
	    {"  i                ", "  ❬i❭             "},
	    {"  L                ", "  ❬L❭             "},
	    {"  P                ", "  ❬P❭             "},
	    {"  p                ", "  ❬p❭             "},
	    {"  S                ", "  ❬S❭             "},
	    {"  v                ", "  ❬v❭             "},
	    {"  W                ", "  ❬W❭             "},
	    {"  w                ", "  ❬w❭             "},
	    {"  -,_              ", "  ❬-❭❬_❭          "},
	    {"  +,=              ", "  ❬+❭❬=❭          "},
	    {"  C-k,K,<,,        ", "  ❬C-k❭❬K❭❬<❭❬,❭  "},
	    {"  C-j,J,>,.        ", "  ❬C-j❭❬J❭❬>❭❬.❭  "},
	    {"", ""},
	    {"", ""}, /* Tree */
	    {"  l,Enter,<Right>  ", "  ❬→❭❬l❭❬Enter❭   "},
	    {"  h,<BS>,<Left>    ", "  ❬←❭❬h❭❬⌫❭       "},
	    {"  b                ", "  ❬b❭             "},
	    {"  d                ", "  ❬d❭             "},
	    {"  i                ", "  ❬i❭             "},
	    {"  t                ", "  ❬t❭             "},
	    {"", ""},
	    {"", ""}, /* Blame */
	    {"  Space            ", "  ❬Space❭         "},
	    {"  Enter            ", "  ❬Enter❭         "},
	    {"  #                ", "  ❬#❭             "},
	    {"  @                ", "  ❬@❭             "},
	    {"  c                ", "  ❬c❭             "},
	    {"  p                ", "  ❬p❭             "},
	    {"  P,<BS>           ", "  ❬P❭❬⌫❭          "},
	    {"  b                ", "  ❬b❭             "},
	    {"  t                ", "  ❬t❭             "},
	    {"", ""},
	    {"", ""}, /* Branch */
	    {"  Enter,Space      ", "  ❬Enter❭❬Space❭  "},
	    {"  d                ", "  ❬d❭             "},
	    {"  i                ", "  ❬i❭             "},
	    {"  o                ", "  ❬o❭             "},
	    {"  t                ", "  ❬t❭             "},
	    {"  R,<C-l>          ", "  ❬R❭❬C-l❭        "},
	    {"", ""},
	    {"", ""},
	    {NULL, NULL}
	};
	static const char *desc[] = {
	    "",
	    "Global",
	    "Open runtime help",
	    "Move selection cursor or page up one line",
	    "Move selection cursor or page down one line",
	    "Scroll view up one page",
	    "Scroll view down one page",
	    "Scroll view up one half page",
	    "Scroll view down one half page",
	    "Jump to first line or start of the view",
	    "Jump to last line or end of the view",
	    "Scroll the view right (timeline, diff, blame, help)",
	    "Scroll the view left (timeline, diff, blame, help)",
	    "Scroll right to the end of the longest line "
	    "(timeline, diff, blame, help)",
	    "Scroll left to the beginning of the line "
	    "(timeline, diff, blame, help)",
	    "Switch focus between open views",
	    "Toggle between coloured and monochromatic output",
	    "Toggle between fullscreen and splitscreen layout",
	    "Switch splitscreen layout (horizontal/vertical)",
	    "Shrink the active horizontal or vertical split",
	    "Grow the active horizontal or vertical split",
	    "Open prompt to enter search term (not available in this view)",
	    "Find next line or token matching the current search term",
	    "Find previous line or token matching the current search term",
	    "Quit the active view",
	    "Quit the program",
	    "",
	    "Timeline",
	    "Move selection cursor up one commit",
	    "Move selection cursor down one commit",
	    "Open diff view of the selected commit",
	    "(Un)tag (or diff) the selected (against the tagged) commit",
	    "Open and populate branch view with all repository branches",
	    "Diff local changes in the checkout against selected commit",
	    "Open prompt to enter term with which to filter new timeline view",
	    "Display a tree reflecting the state of the selected commit",
	    "Cancel the current search or timeline traversal",
	    "",
	    "Diff",
	    "Scroll down one page of diff output",
	    "Toggle display of diff view line numbers",
	    "Open prompt to enter line number and navigate to line",
	    "Move line selection down one line",
	    "Move line selection up one line",
	    "Navigate to previous file in the diff",
	    "Navigate to next file in the diff",
	    "Navigate to previous hunk in the diff",
	    "Navigate to next hunk in the diff",
	    "Open and populate branch view with all repository branches",
	    "Toggle brief diff display of file indexes and hashes only",
	    "Toggle diffstat between minimal and histogram format",
	    "Toggle inversion of diff output",
	    "Toggle display of file line numbers",
	    "Toggle display of change scope in diff hunk headers",
	    "Write the currently viewed diff to a patch file",
	    "Display side-by-side formatted diff",
	    "Toggle verbosity of diff output",
	    "Toggle wrapping of lines longer than the diff view width",
	    "Toggle ignore whitespace-only changes in diff",
	    "Decrease the number of context lines",
	    "Increase the number of context lines",
	    "Display commit diff of next line in the file / timeline entry",
	    "Display commit diff of previous line in the file / timeline entry",
	    "",
	    "Tree",
	    "Blame selected file or move into the selected directory",
	    "Return to the parent directory",
	    "Open and populate branch view with all repository branches",
	    "Toggle ISO8601 modified timestamp display for each tree entry",
	    "Toggle display of file artifact SHA hash ID",
	    "Display timeline of all commits modifying the selected entry",
	    "",
	    "Blame",
	    "Scroll down one page",
	    "Display the diff of the commit corresponding to the selected line",
	    "Toggle display of file line numbers",
	    "Open prompt to enter line number and navigate to line",
	    "Blame the version of the file found in the selected line's commit",
	    "Blame the version of the file found in the selected line's parent "
	    "commit",
	    "Reload the previous blamed version of the file",
	    "Open and populate branch view with all repository branches",
	    "Open timeline view for the currently selected annotated line",
	    "",
	    "Branch",
	    "Display the timeline of the currently selected branch",
	    "Toggle display of the date when the branch last received changes",
	    "Toggle display of the SHA hash that identifies the branch",
	    "Toggle branch sort order (lexicographical -> mru -> state)",
	    "Open a tree view of the currently selected branch",
	    "Reload view with all repository branches and no filters applied",
	    "",
	    ""
	};
	int rc;

	title = fsl_mprintf("%s %s help ('q' to quit)\n",
	    fnc__progname, PRINT_VERSION);
	if (title == NULL)
		return RC_ERRNO("fsl_mprintf");

	rc = drawpad(view, keys, desc, title, STASH_MVMT_NONE, done);
	free(title);
	return rc;
}

/*
 * Create popup pad in which to write the supplied txt string and optional
 * title. The pad is contained within a window that is offset four columns in
 * and two lines down from the parent window.
 */
static int
drawpad(struct fnc_view *view, const char *keys[][2], const char **desc,
    const char *title, enum stash_mvmt stash, int *done)
{
	WINDOW		*content = NULL, *win = NULL;
	FILE		*txt;
	const char	*footnote;
	char		*line = NULL;
	size_t		 linesz;
	int		 ln, width, cury, curx, ymax, xmax, ymin, xmin;
	int		 cs, ch, endy = 0, nprinted = 0, rc = FNC_RC_OK;

	if (view->cols < 5 || view->lines < 3)
		return FNC_RC_OK;  /* nop; screen too small */

	txt = tmpfile();
	if (txt == NULL)
		return RC_ERRNO("tmpfile");

	cs = strcmp(nl_langinfo(CODESET), "UTF-8") == 0;

	if (stash == STASH_MVMT_NONE)
		footnote = "See the fnc(1) manual page for "
		    "complete documentation.";
	else
		footnote = "^conditionally available when "
		    "hunks occupy multiple pages";

	/*
	 * Format help text, and compute longest line and total number of
	 * lines in text to be displayed to determine pad dimensions.
	 */
	width = title != NULL ? strlen(title) : 0;
	for (ln = 0; keys[ln][0] != NULL; ++ln) {
		const char	*info, *key;
		int		 len = 0;

		/* only show available stash keymaps */
		if ((stash == STASH_MVMT_NONE && (ln == 2 || ln == 3)) ||
		    (stash == STASH_MVMT_DOWN && ln == 2) ||
		    (stash == STASH_MVMT_UP && ln == 3))
			continue;

		info = desc[ln];
		key = keys[ln][cs];

		if (*key != '\0')
			len = strlen(key);
		if (*info != '\0')
			len += strlen(info);
		if (len != 0)
			width = MAX(width, len);

		if (fprintf(txt, "%s%s%s", key, info,
		    keys[ln + 1][0] != NULL ? "\n" : footnote) < 0) {
			rc = RC(FNC_RC_IO, "fprintf");
			goto end;
		}
		++nprinted;
	}
	if (fseeko(txt, 0L, SEEK_SET)) {
		rc = RC_ERRNO("fseeko");
		goto end;
	}
	++width;
	++ln;

	cury = curx = 0;
	xmin = view->cols < 16 ? 1 : 4;	/* ncols before help window start */
	ymin = view->lines < 7 ? 1 : 2;	/* nlines before help window start */
	xmax = view->cols - xmin * 2;	/* help window column width */
	ymax = MIN(nprinted + 3, view->lines - ymin * 2); /* help win height */

	if ((win = newwin(ymax, xmax, ymin, xmin)) == NULL) {
		rc = RC(FNC_RC_CURSES, "newwin");
		goto end;
	}
	if ((content = newpad(ln + 1, width + 1)) == NULL) {
		rc = RC_ERRNO("newpad");
		goto end;
	}

	doupdate();
	keypad(content, TRUE);

	/* write help txt to pad */
	if (title) {
		rc = centerprint(content, 0, 0, MIN(width, xmax), title, 0);
		if (rc)
			goto end;
	}
	while (getline(&line, &linesz, txt) != -1) {
		if (waddstr(content, line) == ERR) {
			rc = RC(FNC_RC_CURSES, "waddstr");
			goto end;
		}
	}
	if (!feof(txt)) {
		rc = RC_FERROR(txt, FNC_RC_IO, "getline");
		goto end;
	}

	if (ln > ymax - 3)
		endy = ln - ymax + 3;  /* nlines past the end of the page */
	do {
		werase(win);
		box(win, 0, 0);
		wnoutrefresh(win);
		pnoutrefresh(content, cury, curx, ymin + 1, xmin + 1,
		    ymax, xmax);
		doupdate();

		switch (ch = wgetch(content)) {
		case KEY_UP:
		case 'k':
			if (cury > 0)
				--cury;
			break;
		case KEY_DOWN:
		case 'j':
			if (cury < endy)
				++cury;
			break;
		case KEY_PPAGE:
		case CTRL('b'):
			if (cury > 0) {
				cury -= ymax - 3;
				if (cury < 0)
					cury = 0;
			}
			break;
		case KEY_NPAGE:
		case CTRL('f'):
		case ' ':
			if (cury < endy) {
				cury += ymax - 3;
				if (cury > endy)
					cury = endy;
			}
			break;
		case '0':
			curx = 0;
			break;
		case '$':
			curx = MAX(width - xmax / 2, 0);
			break;
		case KEY_LEFT:
		case 'h':
			curx -= MIN(curx, 2);
			break;
		case KEY_RIGHT:
		case 'l':
			if (curx + xmax / 2 < width)
				curx += 2;
			break;
		case 'g':
			if (!fnc_home(view))
				break;
			/* FALL THROUGH */
		case KEY_HOME:
			cury = 0;
			break;
		case KEY_END:
		case 'G':
			cury = endy;
			break;
		case 'Q':
			*done = 1;
			break;
		case ERR:
		default:
			break;
		}
	} while (!*done && ch != 'q' && ch != KEY_ESCAPE && ch != ERR);

end:
	free(line);
	if (fclose(txt) == EOF)
		rc = RC_ERRNO("fclose");
	if (win != NULL) {
		werase(win);
		wrefresh(win);
		delwin(win);
	}
	if (content != NULL)
		delwin(content);
	/* restore fnc window content */
	touchwin(view->window);
	wnoutrefresh(view->window);
	doupdate();
	return rc;
}

static int
centerprint(WINDOW *win, size_t starty, size_t startx, size_t width,
    const char *str, chtype colour)
{
	size_t	x, y, len = 0;

	if (win == NULL)
		win = stdscr;
	if (str != NULL)
		len = strlen(str);

	/* start line and column of str */
	y = MAX(starty, 0);
	x = startx ? startx : width > len ? (width - len) / 2 : 0;

	wattron(win, colour | A_UNDERLINE);
	if (mvwprintw(win, y, x, "%s", str) == ERR)
		return RC(FNC_RC_CURSES, "mvwprintw");
	wattroff(win, colour | A_UNDERLINE);
	wrefresh(win);

	return FNC_RC_OK;
}

static int
tl_input_handler(struct fnc_view **new_view, struct fnc_view *view, int ch)
{
	struct fnc_tl_view_state	*s = &view->state.timeline;
	int				 rc = FNC_RC_OK;
	uint16_t			 nscroll = view->nlines - 2;

	switch (ch) {
	case '0':
		view->pos.x = 0;
		break;
	case '$':
		view->pos.x = MAX(view->pos.maxx - view->ncols / 2, 0);
		break;
	case KEY_RIGHT:
	case 'l':
		if (view->pos.x + view->ncols / 2 < view->pos.maxx)
			view->pos.x += 2;
		break;
	case KEY_LEFT:
	case 'h':
		view->pos.x -= MIN(view->pos.x, 2);
		break;
	case KEY_DOWN:
	case 'j':
	case '.':
	case '>':
		rc = move_tl_cursor_down(view, 0);
		break;
	case CTRL('d'):
		nscroll >>= 1;
		/* FALL THROUGH */
	case KEY_NPAGE:
	case CTRL('f'): {
		rc = move_tl_cursor_down(view, nscroll);
		break;
	}
	case KEY_END:
	case 'G':
		view->search_status = SEARCH_FOR_END;
		view_search_start(view);
		break;
	case 'k':
	case KEY_UP:
	case '<':
	case ',':
		move_tl_cursor_up(view, false, false);
		break;
	case CTRL('u'):
		nscroll >>= 1;
		/* FALL THROUGH */
	case KEY_PPAGE:
	case CTRL('b'):
		move_tl_cursor_up(view, nscroll, false);
		break;
	case 'g':
		if (!fnc_home(view))
			break;
		/* FALL THROUGH */
	case KEY_HOME:
		move_tl_cursor_up(view, false, true);
		break;
	case KEY_RESIZE:
		if (s->selected > view->nlines - 2)
			s->selected = view->nlines - 2;
		if (s->selected > s->commits.ncommits - 1)
			s->selected = s->commits.ncommits - 1;
		s->selected = MAX(s->selected, 0);
		select_commit(s);
		if (s->commits.ncommits < view->nlines - 1 &&
		    !s->thread_cx.eotl) {
			s->thread_cx.ncommits_needed += (view->nlines - 1) -
			    s->commits.ncommits;
			rc = signal_tl_thread(view, 1);
		}
		break;
	case 'D': {
		struct fsl_cx *f;

		if ((f = fcli_cx()) == NULL)
			return RC(FNC_RC_FATAL, "fcli_cx");

		if (*s->selected_entry->commit->type != 'c')
			return sitrep(view, SR_ALL,
			    ":requires checkin artifact");
		if (!fsl_needs_ckout(f))
			return sitrep(view, SR_ALL, ":requires work tree");

		/*
		 * XXX This is not good but I can't think of an alternative
		 * without patching libf: fsl_ckout_changes_scan() returns a
		 * db lock error via fsl_vfile_changes_scan() when versioned
		 * files are modified at runtime. Clear it and notify user.
		 */
		rc = fsl_ckout_changes_scan(f);
		if (rc) {
			if (rc != FSL_RC_DB)
				return RC(rc, "fsl_ckout_changes_scan");
			return sitrep(view, SR_ALL, ":checkout db busy");
		}

		if (!fsl_ckout_has_changes(f))
			return sitrep(view, SR_CLREOL | SR_UPDATE | SR_SLEEP,
			    ":no local changes");

		rc = reset_tags(s);
		if (rc)
			return rc;
		s->selected_entry->commit->diff_type = FNC_DIFF_CKOUT;
	}	/* FALL THROUGH */
	case ' ':
		switch (rc = tag_timeline_entry(s)) {
		case FNC_RC_BREAK:
			return FNC_RC_OK;
		case FNC_RC_SITREP:
			return sitrep(view, SR_ALL, ":%s", RCSTR(rc));
		case FNC_RC_OK:
			return view_request_new(new_view, view, FNC_VIEW_DIFF);
		default:
			return rc;
		}
	case KEY_ENTER:
	case '\r':
		rc = reset_tags(s);
		if (rc)
			return rc;
		rc = view_request_new(new_view, view, FNC_VIEW_DIFF);
		break;
	case 'b':
		rc = view_request_new(new_view, view, FNC_VIEW_BRANCH);
		break;
	case 'C':
		if (COLORS)
			view->colour = !view->colour;
		break;
	case 'F': {
		struct input input;

		memset(&input, 0, sizeof(input));
		input.prompt = "filter: ";
		input.type = INPUT_ALPHA;
		input.flags = SR_CLREOL;

		rc = fnc_prompt_input(view, &input);
		if (rc)
			return rc;
		s->glob = input.buf;
		rc = view_request_new(new_view, view, FNC_VIEW_TIMELINE);
		if (rc == FNC_RC_BREAK) {
			rc = sitrep(view, SR_ALL, ":no matching commits");
		}
		break;
	}
	case 't': {
		struct fsl_cx		*f;
		struct fsl_deck		 d = fsl_deck_empty;
		const fsl_card_F	*cf = NULL;

		if (s->selected_entry == NULL)
			break;
		if ((f = fcli_cx()) == NULL)
			return RC(FNC_RC_FATAL, "fcli_cx");
		if (!fsl_rid_is_a_checkin(f, s->selected_entry->commit->rid)) {
			sitrep(view, SR_CLREOL | SR_UPDATE | SR_SLEEP,
			    ":tree requires checkin artifact");
			break;
		}
		rc = map_version_path(s->path, s->selected_entry->commit->rid);
		if (rc != FNC_RC_OK) {
			if (rc != FNC_RC_NO_PATH)
				break;
			sitrep(view, SR_CLREOL | SR_UPDATE | SR_SLEEP,
			    ":no such entry found in tree: %s", s->path);
			rc = RC_RESET;
			break;
		}
		rc = fsl_deck_load_rid(f, &d,
		    s->selected_entry->commit->rid, FSL_SATYPE_CHECKIN);
		if (rc)
			break;
		rc = fsl_deck_F_rewind(&d);
		if (rc) {
			fsl_deck_finalize(&d);
			break;
		}
		rc = fsl_deck_F_next(&d, &cf);
		fsl_deck_finalize(&d);
		if (rc)
			break;
		if (cf == NULL)
			return sitrep(view, SR_CLREOL | SR_SLEEP | SR_UPDATE,
			    ":tree is empty");
		rc = view_request_new(new_view, view, FNC_VIEW_TREE);
		break;
	}
	case 'q':
		s->quit = 1;
		break;
	default:
		break;
	}

	return rc;
}

static int
tag_timeline_entry(struct fnc_tl_view_state *s)
{
	struct fnc_commit_artifact *c = s->selected_entry->commit;

	if (*c->type != 'c' && *c->type != 'w')
		return RC(FNC_RC_SITREP, "requires checkin or wiki artifact");

	if (s->tag.two != NULL) {
		int rc;

		rc = reset_tags(s);
		if (rc)
			return rc;
	}

	if (c->diff_type == FNC_DIFF_CKOUT || s->tag.one == NULL) {
		s->tag.one = c;
		if (s->tag.one->diff_type != FNC_DIFF_CKOUT)
			return FNC_RC_BREAK;
	} else if (*c->type != *s->tag.one->type) {
		return RC(FNC_RC_SITREP,
		    "requires matching %s artifact",
		    *s->tag.one->type  == 'c' ? "checkin" : "wiki");
	} else if (c->rid == s->tag.one->rid) {
		/* untag the selected entry */
		s->tag.one = NULL;
		return FNC_RC_BREAK;
	}

	if (*s->tag.one->type == 'w' && s->tag.one->comment != NULL &&
	    strcmp(strchr(s->tag.one->comment, ':') + 2,
	    strchr(c->comment, ':') + 2) != 0)
		return RC(FNC_RC_SITREP,
		    "requires matching %s wiki page",
		    strchr(s->tag.one->comment, ':') + 2);

	if (c->prid != s->tag.one->rid)
		s->showmeta = false;

	if (c->puuid != NULL) {
		/* initial commits have no parent, hence the check */
		s->tag.ogid = strdup(c->puuid);
		if (s->tag.ogid == NULL)
			return RC_ERRNO("strdup");
	}

	s->tag.ogrid = c->prid;

	free(c->puuid);
	c->puuid = strdup(s->tag.one->uuid);
	if (c->puuid == NULL)
		return RC_ERRNO("strdup");

	c->prid = s->tag.one->rid;
	s->tag.two = c;

	return FNC_RC_OK;
}

static int
move_tl_cursor_down(struct fnc_view *view, uint16_t page)
{
	struct fnc_tl_view_state	*s = &view->state.timeline;
	struct commit_entry		*first;
	int				 eos = view->nlines - 2;
	int				 rc = FNC_RC_OK;

	first = s->first_commit_onscreen;
	if (first == NULL)
		return rc;

	if (view_is_top_split(view))
		--eos;  /* border consumes the last line */

	if (s->thread_cx.eotl &&
	    s->selected_entry->idx >= s->commits.ncommits - 1)
		return rc;  /* Last commit already selected. */

	if (!page) {
		/* still more commits on this page to scroll down */
		if (s->selected < MIN(eos, s->commits.ncommits - 1))
			++s->selected;
		else  /* Last commit on screen is selected, need to scroll. */
			rc = timeline_scroll_down(view, 1);
	} else if (s->thread_cx.eotl) {
		/* last displayed commit is the end, jump to it */
		if (s->last_commit_onscreen->idx == s->commits.ncommits - 1)
			s->selected += MIN(s->last_commit_onscreen->idx -
			    s->selected_entry->idx, page + 1);
		else  /* Scroll the page. */
			rc = timeline_scroll_down(view, MIN(page,
			    s->commits.ncommits - s->selected_entry->idx - 1));
	} else {
		rc = timeline_scroll_down(view, page);
		if (rc)
			return rc;
		if (first == s->first_commit_onscreen && s->selected <
		    MIN(eos, s->commits.ncommits - 1)) {
			/* end of timeline/no more commits; move cursor down */
			s->selected = MIN(s->commits.ncommits - 1, page);
		}
		/*
		 * If we've overshot (necessarily possible with horizontal
		 * splits), select the final commit.
		 */
		s->selected = MIN(s->selected,
		    s->last_commit_onscreen->idx -
		    s->first_commit_onscreen->idx);
	}

	if (!rc)
		select_commit(s);
	return rc;
}

static void
move_tl_cursor_up(struct fnc_view *view, uint16_t page, bool home)
{
	struct fnc_tl_view_state *s = &view->state.timeline;

	if (s->first_commit_onscreen == NULL)
		return;

	if ((page && TAILQ_FIRST(&s->commits.head) == s->first_commit_onscreen)
	    || home)
		s->selected = home ? 0 : MAX(0, s->selected - page - 1);

	if (!page && !home && s->selected > 0)
		--s->selected;
	else
		timeline_scroll_up(s, home ?
		    s->commits.ncommits : MAX(page, 1));

	select_commit(s);
	return;
}

static int
view_request_new(struct fnc_view **requested, struct fnc_view *view,
    enum fnc_view_id request)
{
	struct fnc_view		*new_view = NULL;
	int			 y = 0, x = 0, rc = FNC_RC_OK;

	*requested = NULL;

	if (view_is_parent(view))
		view_split_getyx(view, &y, &x);

	rc = view_dispatch_request(&new_view, view, request, y, x);
	if (rc)
		return rc;

	if (view_is_parent(view) && view->mode == VIEW_SPLIT_HRZN) {
		rc = view_split_horizontally(view, y);
		if (rc)
			return rc;
	}

	view->active = false;
	new_view->active = true;
	new_view->mode = view->mode;
	new_view->nlines = view->lines - y;

	if (view_is_parent(view)) {
		view_copy_size(new_view, view);
		rc = view_close_child(view);
		if (rc)
			return rc;
		rc = view_set_child(view, new_view);
		if (rc != FNC_RC_OK)
			return rc;
		view->focus_child = true;
	} else
		*requested = new_view;

	return FNC_RC_OK;
}

static void
view_split_getyx(struct fnc_view *view, int *y, int *x)
{
	*y = 0;
	*x = 0;

	if (view->mode == VIEW_SPLIT_HRZN) {
		if (view->child != NULL && view->child->resized_y)
			*y = view->child->resized_y;
		else if (view->resized_y)
			*y = view->resized_y;
		else
			*y = view_split_gety(view->lines);
	}
	else if (view->mode == VIEW_SPLIT_VERT) {
		if (view->child != NULL && view->child->resized_x)
			*x = view->child->resized_x;
		else if (view->resized_x)
			*x = view->resized_x;
		else
			*x = view_split_getx(view->begin_x);
	}
}

static int
view_split_gety(int lines)
{
	return lines * HSPLIT_SCALE;
}

static int
view_split_getx(int x)
{
	if (x > 0 || COLS < 120)
		return 0;
	return (COLS - MAX(COLS / 2, 80));
}

static int
view_dispatch_request(struct fnc_view **new_view, struct fnc_view *view,
    enum fnc_view_id request, int y, int x)
{
	switch (request) {
	case FNC_VIEW_DIFF:
		if (view->vid == FNC_VIEW_TIMELINE) {
			struct fnc_tl_view_state *s = &view->state.timeline;

			return init_diff_view(new_view, y, x,
			    s->selected_entry->commit,
			    s->showmeta ? DIFF_MODE_META : DIFF_MODE_NORMAL,
			    -1, FNC_DIFF_PROTOTYPE | FNC_DIFF_VERBOSE,
			    view->colour, 0);
		}
		break;
	case FNC_VIEW_BLAME:
		if (view->vid == FNC_VIEW_TREE) {
			struct fnc_tree_view_state *s = &view->state.tree;

			return blame_tree_entry(new_view, y, x,
			    s->selected_entry, &s->parents, s->rid,
			    view->colour);
		}
		break;
	case FNC_VIEW_TIMELINE: {
		const char	*glob = NULL;
		int		 rid = 0;

		if (view->vid == FNC_VIEW_TREE)
			return timeline_tree_entry(new_view, y, x,
			    &view->state.tree, view->colour);

		if (view->vid == FNC_VIEW_TIMELINE) {
			glob = view->state.timeline.glob;
		} else if (view->vid == FNC_VIEW_BRANCH) {
			struct fsl_cx	*f;
			const char	*id;
			int		 rc;

			if ((f = fcli_cx()) == NULL)
				return RC(FNC_RC_FATAL, "fcli_cx");

			id = view->state.branch.selected_entry->branch->id;
			rc = idtorid(&rid, id, FNC_RC_NO_BRANCH);
			if (rc != 0)
				return rc;
		} else if (view->vid == FNC_VIEW_BLAME) {
			rid = view->state.blame.line_rid;
		} else
			break;

		return init_timeline_view(new_view, y, x, rid, NULL, NULL,
		    glob, NULL, NULL, 0, NULL, view->colour);
	}
	case FNC_VIEW_TREE:
		if (view->vid == FNC_VIEW_TIMELINE) {
			struct fnc_tl_view_state *s = &view->state.timeline;

			return browse_commit_tree(new_view, y, x,
			    s->selected_entry, s->path, view->colour);
		} else if (view->vid == FNC_VIEW_BRANCH)
			return browse_branch_tree(new_view, y, x,
			    view->state.branch.selected_entry, view->colour);
		break;
	case FNC_VIEW_BRANCH:
		switch(view->vid) {
		case FNC_VIEW_BLAME:
		case FNC_VIEW_DIFF:
		case FNC_VIEW_TIMELINE:
		case FNC_VIEW_TREE:
			*new_view = view_open(0, 0, y, x, FNC_VIEW_BRANCH);
			if (*new_view == NULL)
				return RC(FNC_RC_CURSES, "view_open");

			return open_branch_view(*new_view,
			    BRANCH_LS_OPEN_CLOSED, NULL, 0, 0, view->colour);
		default:
			break;
		}
		break;
	default:
		break;
	}

	return RC(FNC_RC_NOSUPPORT, "parent=%d,child=%d", view->vid, request);
}

/* Split view horizontally at y and offset view->state->selected line. */
static int
view_split_horizontally(struct fnc_view *view, int y)
{
	int rc;

	view->nlines = y;
	view->ncols = COLS;
	rc = view_resize(view);
	if (rc)
		return rc;

	return view_offset_scrolldown(view);
}

/*
 * If view was scrolled down to move the selected line into view when opening a
 * horizontal split, scroll back up when closing the split/toggling fullscreen.
 */
static void
view_offset_scrollup(struct fnc_view *view)
{
	switch (view->vid) {
	case FNC_VIEW_BLAME: {
		struct fnc_blame_view_state *s = &view->state.blame;

		if (s->first_line_onscreen == 1) {
			s->selected_line = MAX(
			    s->selected_line - view->pos.offset, 1);
			break;
		}
		if (s->first_line_onscreen > view->pos.offset)
			s->first_line_onscreen -= view->pos.offset;
		else
			s->first_line_onscreen = 1;
		s->selected_line += view->pos.offset;
		break;
	}
	case FNC_VIEW_TIMELINE:
		timeline_scroll_up(&view->state.timeline, view->pos.offset);
		view->state.timeline.selected += view->pos.offset;
		break;
	case FNC_VIEW_BRANCH:
		branch_scroll_up(&view->state.branch, view->pos.offset);
		view->state.branch.selected += view->pos.offset;
		break;
	case FNC_VIEW_TREE:
		tree_scroll_up(&view->state.tree, view->pos.offset);
		view->state.tree.selected += view->pos.offset;
		break;
	default:
		break;
	}

	view->pos.offset = 0;
}

/*
 * If view->state->selected line is outside the now split view, scroll offset
 * lines to move selected line into view and index its new position.
 */
static int
view_offset_scrolldown(struct fnc_view *view)
{
	int	(*scrolld)(struct fnc_view *, int);
	int	  header, offset, rc;
	int	 *selected;

	switch (view->vid) {
	case FNC_VIEW_TIMELINE: {
		struct fnc_tl_view_state *s = &view->state.timeline;

		scrolld = &timeline_scroll_down;
		header = view_is_parent(view) ? 3 : 2;
		selected = &s->selected;
		break;
	}
	case FNC_VIEW_TREE: {
		struct fnc_tree_view_state *s = &view->state.tree;

		scrolld = &tree_scroll_down;
		header = 5;
		selected = &s->selected;
		break;
	}
	case FNC_VIEW_BRANCH: {
		struct fnc_branch_view_state *s = &view->state.branch;

		scrolld = &branch_scroll_down;
		header = 3;
		selected = &s->selected;
		break;
	}
	case FNC_VIEW_BLAME: {
		struct fnc_blame_view_state *s = &view->state.blame;

		scrolld = NULL;
		selected = NULL;
		header = 3;
		if (s->selected_line > view->nlines - header) {
			offset = abs(view->nlines - s->selected_line - header);
			s->first_line_onscreen += offset;
			s->selected_line -= offset;
			view->pos.offset = offset;
		}
		break;
	}
	default:
		selected = NULL;
		scrolld = NULL;
		header = 0;
		break;
	}

	if (selected && *selected > view->nlines - header) {
		offset = ABS(view->nlines - *selected - header);
		view->pos.offset = offset;
		if (scrolld != NULL && offset != 0) {
			rc = scrolld(view, offset);
			if (rc != FNC_RC_OK)
				return rc;
			*selected -= offset;
			view->pos.y = *selected;
		}
	}

	return FNC_RC_OK;
}

static int
timeline_scroll_down(struct fnc_view *view, int maxscroll)
{
	struct fnc_tl_view_state	*s = &view->state.timeline;
	struct commit_entry		*pentry;
	int				 ncommits_needed, nscrolled = 0;
	int				 rc = FNC_RC_OK;

	if (s->last_commit_onscreen == NULL || maxscroll == 0)
		return FNC_RC_OK;

	ncommits_needed = s->last_commit_onscreen->idx + 1 + maxscroll;
	if (s->commits.ncommits < ncommits_needed && !s->thread_cx.eotl) {
		/* signal timeline thread for n commits needed */
		s->thread_cx.ncommits_needed +=
		    ncommits_needed - s->commits.ncommits;
		rc = signal_tl_thread(view, 1);
		if (rc)
			return rc;
	}

	do {
		pentry = TAILQ_NEXT(s->last_commit_onscreen, entries);
		if (pentry == NULL && view->mode != VIEW_SPLIT_HRZN)
			break;

		s->last_commit_onscreen = pentry ?
		    pentry : s->last_commit_onscreen;

		pentry = TAILQ_NEXT(s->first_commit_onscreen, entries);
		if (pentry == NULL)
			break;
		s->first_commit_onscreen = pentry;
	} while (++nscrolled < maxscroll);

	if (view->mode == VIEW_SPLIT_HRZN && !s->thread_cx.eotl)
		view->nscrolled += nscrolled;
	else
		view->nscrolled = 0;

	return FNC_RC_OK;
}

static void
timeline_scroll_up(struct fnc_tl_view_state *s, int maxscroll)
{
	struct commit_entry	*entry;
	int			 nscrolled = 0;

	entry = TAILQ_FIRST(&s->commits.head);
	if (s->first_commit_onscreen == entry)
		return;

	entry = s->first_commit_onscreen;
	while (entry && nscrolled < maxscroll) {
		entry = TAILQ_PREV(entry, commit_tailhead, entries);
		if (entry) {
			s->first_commit_onscreen = entry;
			++nscrolled;
		}
	}
}

static void
select_commit(struct fnc_tl_view_state *s)
{
	struct commit_entry	*entry;
	int			 ncommits = 0;

	entry = s->first_commit_onscreen;
	while (entry) {
		if (ncommits == s->selected) {
			s->selected_entry = entry;
			break;
		}
		entry = TAILQ_NEXT(entry, entries);
		++ncommits;
	}
}

static int
make_splitscreen(struct fnc_view *view)
{
	int rc;

	if (!view->resizing && view->mode == VIEW_SPLIT_HRZN) {
		if (view->resized_y && view->resized_y < view->lines)
			view->begin_y = view->resized_y;
		else
			view->begin_y = view_split_gety(view->nlines);
		view->begin_x = 0;
	} else if (!view->resizing) {
		if (view->resized_x && view->resized_x < view->cols - 1 &&
		    view->cols > 119)
			view->begin_x = view->resized_x;
		else
			view->begin_x = view_split_getx(0);
		view->begin_y = 0;
	}
	view->nlines = LINES - view->begin_y;
	view->ncols = COLS - view->begin_x;
	view->lines = LINES;
	view->cols = COLS;
	rc = view_resize(view);
	if (rc)
		return rc;

	if (view->parent && view->mode == VIEW_SPLIT_HRZN)
		view->parent->nlines = view->begin_y;

	if (mvwin(view->window, view->begin_y, view->begin_x) == ERR)
		return RC(FNC_RC_CURSES, "mvwin");

	return FNC_RC_OK;
}

static int
make_fullscreen(struct fnc_view *view)
{
	int rc;

	view->begin_x = 0;
	view->begin_y = view->resizing ? view->begin_y : 0;
	view->nlines = view->resizing ? view->nlines : LINES;
	view->ncols = COLS;
	view->lines = LINES;
	view->cols = COLS;

	rc = view_resize(view);
	if (rc)
		return rc;

	/*
	 * XXX Do not call mvwin(3) on a child window while resizing with
	 * view_resize_split() as we have not yet set the window's new line
	 * height with wresize(3) but have already set its new start line,
	 * so mvwin() will fail as the bottom line would be placed offscreen.
	 */
	if ((view->parent == NULL || !view->resizing) &&
	    mvwin(view->window, view->begin_y, view->begin_x) == ERR)
		return RC(FNC_RC_CURSES, "mvwin");

	return FNC_RC_OK;
}

static int
view_search_start(struct fnc_view *view)
{
	struct input	input;
	int		rc;

	memset(&input, 0, sizeof(input));
	input.type = INPUT_ALPHA;
	input.prompt = "/";
	input.flags = SR_CLREOL;

	if (view->started_search) {
		regfree(&view->regex);
		view->searching = SEARCH_DONE;
		memset(&view->regmatch, 0, sizeof(view->regmatch));
	}
	view->started_search = false;

	if (view->nlines < 1)
		return FNC_RC_OK;

	if (view->search_status == SEARCH_FOR_END) {
		view->grep_init(view);
		view->started_search = true;
		view->searching = SEARCH_FORWARD;
		view->search_status = SEARCH_WAITING;
		view->state.timeline.thread_cx.endjmp = true;
		return view->grep(view);
	}

	rc = fnc_prompt_input(view, &input);
	if (rc)
		return rc;

	if (regcomp(&view->regex, input.buf, REG_EXTENDED|REG_NEWLINE) == 0) {
		view->grep_init(view);
		view->started_search = true;
		view->searching = SEARCH_FORWARD;
		view->search_status = SEARCH_WAITING;
		return view->grep(view);
	}

	return FNC_RC_OK;
}

static void
tl_grep_init(struct fnc_view *view)
{
	struct fnc_tl_view_state *s = &view->state.timeline;

	s->matched_commit = NULL;
	s->search_commit = NULL;
}

static int
tl_search_next(struct fnc_view *view)
{
	struct fnc_tl_view_state	*s = &view->state.timeline;
	struct commit_entry		*entry;
	int				 rc = FNC_RC_OK;

	if (!s->thread_cx.ncommits_needed && view->started_search)
		halfdelay(1);

	/* show status update in timeline view */
	rc = show_timeline_view(view);
	if (rc)
		return rc;
	update_panels();
	doupdate();

	if (s->search_commit) {
		int ch;

		rc = pthread_mutex_unlock(&fnc__mutex);
		if (rc)
			return RC_ERRNO_SET(rc, "pthread_mutex_unlock");

		ch = wgetch(view->window);

		rc = pthread_mutex_lock(&fnc__mutex);
		if (rc)
			return RC_ERRNO_SET(rc, "pthread_mutex_lock");

		if (ch == KEY_BACKSPACE) {
			view->search_status = SEARCH_ABORTED;
			goto end;
		}

		if (view->searching == SEARCH_FORWARD)
			entry = TAILQ_NEXT(s->search_commit, entries);
		else
			entry = TAILQ_PREV(s->search_commit, commit_tailhead,
			    entries);
	} else if (s->matched_commit) {
		if (view->searching == SEARCH_FORWARD)
			entry = TAILQ_NEXT(s->selected_entry, entries);
		else
			entry = TAILQ_PREV(s->selected_entry, commit_tailhead,
			    entries);
	} else {
		if (view->searching == SEARCH_FORWARD)
			entry = TAILQ_FIRST(&s->commits.head);
		else
			entry = TAILQ_LAST(&s->commits.head, commit_tailhead);
	}

	while (1) {
		if (entry == NULL) {
			if (s->thread_cx.eotl && s->thread_cx.endjmp) {
				s->matched_commit = TAILQ_LAST(&s->commits.head,
				    commit_tailhead);
				view->search_status = SEARCH_COMPLETE;
				s->thread_cx.endjmp = false;
				break;
			}
			if (s->thread_cx.eotl ||
			    view->searching == SEARCH_REVERSE) {
				view->search_status = (s->matched_commit ==
				    NULL ? SEARCH_NO_MATCH : SEARCH_COMPLETE);
				s->search_commit = NULL;
				goto end;
			}
			/*
			 * Wake the timeline thread to produce more commits.
			 * Search will resume at s->search_commit upon return.
			 */
			s->search_commit = s->selected_entry;
			++s->thread_cx.ncommits_needed;
			return signal_tl_thread(view, 0);
		}

		if (!s->thread_cx.endjmp && find_commit_match(entry->commit,
		    &view->regex)) {
			view->search_status = SEARCH_CONTINUE;
			s->matched_commit = entry;
			break;
		}

		s->search_commit = entry;
		if (view->searching == SEARCH_FORWARD)
			entry = TAILQ_NEXT(entry, entries);
		else
			entry = TAILQ_PREV(entry, commit_tailhead, entries);
	}

	if (s->matched_commit) {
		int cur = s->selected_entry->idx;

		while (cur < s->matched_commit->idx) {
			rc = tl_input_handler(NULL, view, KEY_DOWN);
			if (rc)
				return rc;
			++cur;
		}
		while (cur > s->matched_commit->idx) {
			rc = tl_input_handler(NULL, view, KEY_UP);
			if (rc)
				return rc;
			--cur;
		}
	}

	s->search_commit = NULL;

end:
	cbreak();
	return rc;
}

static bool
find_commit_match(struct fnc_commit_artifact *commit, regex_t *regex)
{
	regmatch_t regmatch;

	if ((commit->branch && !regexec(regex, commit->branch, 1, &regmatch, 0))
	    || !regexec(regex, commit->user, 1, &regmatch, 0)
	    || !regexec(regex, (char *)commit->uuid, 1, &regmatch, 0)
	    || !regexec(regex, commit->comment, 1, &regmatch, 0))
		return true;

	return false;
}

static int
view_close(struct fnc_view *view)
{
	int rc = FNC_RC_OK, rc2 = FNC_RC_OK;

	if (view->child != NULL) {
		rc2 = view_close(view->child);
		view->child = NULL;
	}
	regfree(&view->regex);
	if (view->close != NULL)
		rc = view->close(view);
	if (view->panel != NULL)
		del_panel(view->panel);
	if (view->window != NULL)
		delwin(view->window);
	free(view);

	return rc ? rc : rc2;
}

static int
close_timeline_view(struct fnc_view *view)
{
	struct fnc_tl_view_state	*s = &view->state.timeline;
	int				 rc, rc2, rc3;

	rc = join_tl_thread(s);
	rc2 = fsl_stmt_finalize(s->thread_cx.q);
	if (rc2 != FNC_RC_OK)
		rc2 = RC_LIBF(rc2, "fsl_stmt_finalize");
	rc3 = reset_tags(s);	/* XXX must be before fnc_free_commits() */
	fnc_free_commits(&s->commits);
	free_colours(&s->colours);
	free(s->path);
	s->path = NULL;

	return rc ? rc : rc2 ? rc2 : rc3;
}

static int
join_tl_thread(struct fnc_tl_view_state *s)
{
	void	*err;
	int	 rc;

	if (s->thread_id != 0) {
		s->quit = 1;

		rc = pthread_cond_signal(&s->thread_cx.commit_consumer);
		if (rc)
			return RC_ERRNO_SET(rc, "pthread_cond_signal");
		rc = pthread_mutex_unlock(&fnc__mutex);
		if (rc)
			return RC_ERRNO_SET(rc, "pthread_mutex_unlock");
		rc = pthread_join(s->thread_id, &err);
		if (rc || err == PTHREAD_CANCELED)
			return RC_ERRNO_SET(rc ? rc : ECANCELED,
			    "pthread_join");
		rc = pthread_mutex_lock(&fnc__mutex);
		if (rc)
			return RC_ERRNO_SET(rc, "pthread_mutex_lock");

		s->thread_id = 0;
	}

	rc = pthread_cond_destroy(&s->thread_cx.commit_consumer);
	if (rc)
		rc = RC_ERRNO_SET(rc, "pthread_cond_destroy");

	rc = pthread_cond_destroy(&s->thread_cx.commit_producer);
	if (rc)
		rc = RC_ERRNO_SET(rc, "pthread_cond_destroy");

	return rc ? rc : (intptr_t)err;
}

static void
fnc_free_commits(struct commit_queue *commits)
{
	while (!TAILQ_EMPTY(&commits->head)) {
		struct commit_entry *entry;

		entry = TAILQ_FIRST(&commits->head);
		TAILQ_REMOVE(&commits->head, entry, entries);
		fnc_commit_artifact_close(entry->commit);
		free(entry);
		--commits->ncommits;
	}
}

static void
fnc_commit_artifact_close(struct fnc_commit_artifact *commit)
{
	if (commit == NULL)
		return;
	free(commit->branch);
	free(commit->comment);
	free(commit->timestamp);
	free(commit->type);
	free(commit->user);
	free(commit->uuid);
	free(commit->puuid);
	fsl_list_clear(&commit->changeset, fnc_file_artifact_free, NULL);
	fsl_list_reserve(&commit->changeset, 0);
	free(commit);
	commit = NULL;
}

static int
fnc_file_artifact_free(void *elem, void *state)
{
	struct fnc_file_artifact *ffa = elem;

	(void)state;
	free(ffa->fc->name);
	free(ffa->fc->uuid);
	free(ffa->fc->priorName);
	free(ffa->puuid);
	free(ffa->fc);
	free(ffa);

	return FNC_RC_OK;
}

static int
init_diff_view(struct fnc_view **new_view, int begin_y, int begin_x,
    struct fnc_commit_artifact *commit, enum fnc_diff_mode mode,
    int context, int flags, int color, int wrap)
{
	struct fnc_view	*diff_view;
	int		 rc;

	diff_view = view_open(0, 0, begin_y, begin_x, FNC_VIEW_DIFF);
	if (diff_view == NULL)
		return RC(FNC_RC_CURSES, "view_open");

	rc = open_diff_view(diff_view, commit, NULL, mode,
	    context, flags, color, wrap);
	if (rc)
		return rc;

	*new_view = diff_view;
	return FNC_RC_OK;
}

static int
open_diff_view(struct fnc_view *view, struct fnc_commit_artifact *commit,
    struct fnc_pathlist_head *paths, enum fnc_diff_mode mode,
    int context, int flags, bool colour, int wrap)
{
	struct fnc_diff_view_state	*s;
	struct fnc_diff_view_state	 s0;
	int				 rc;

	if (view != NULL)
		s = &view->state.diff;
	else {
		memset(&s0, 0, sizeof(s0));
		s = &s0;
	}

	s->diff_flags = flags;
	s->context = context;
	s->paths = paths;
	s->wrap = wrap;
	s->selected_entry = commit;
	s->first_line_onscreen = 1;
	s->last_line_onscreen = view != NULL ? view->nlines : -1;
	s->selected_line = 1;
	s->view = view;
	s->diff_mode = mode;
	s->ncols = view != NULL ? &view->ncols : view_width(0);

	rc = set_diff_opt(s, &colour);
	if (rc)
		return rc;

	if (view != NULL) {
		if (has_colors() && COLORS) {
			STAILQ_INIT(&s->colours);
			rc = set_colours(&s->colours, FNC_VIEW_DIFF);
			if (rc)
				return rc;
			view->colour = colour;
		}
		if (view->parent != NULL &&
		    view->parent->vid == FNC_VIEW_TIMELINE &&
		    view_is_split(view))
			show_timeline_view(view->parent);  /* draw vborder */
		show_diff_status(view);
	} else {
		/* 'fnc diff -o' write diff to stdout */
		rc = difftofile(stdout, s);
		free_diff_state(s);
		return rc;
	}

	rc = create_diff(s);
	if (rc) {
		if (colour)
			free_colours(&s->colours);
		return rc;
	}

	view->show = show_diff_view;
	view->input = diff_input_handler;
	view->close = close_diff_view;
	view->grep_init = diff_grep_init;
	view->grep = find_next_match;
	return FNC_RC_OK;
}

/*
 * Set diff options. Precedence is:
 *   1. CLI options passed to 'fnc diff' (see: fnc diff -h)
 *   2. repo options set via 'fnc set'
 *      - FNC_DIFF_CONTEXT: n
 *      - FNC_DIFF_FLAGS: bCDilPqsWw (see: fnc diff -h for all boolean flags)
 *      - FNC_COLOUR_HL_LINE: mono, auto
 *   3. global options set via envvars
 *      - same as (2) repo options
 *   4. fnc default options
 * Input is validated; supplant bogus values with defaults.
 * Returns nonzero on fnc_conf_getopt allocation failure.
 */
static int
set_diff_opt(struct fnc_diff_view_state *s, bool *colour)
{
	char	*opt;
	char	 ch;
	long	 ctx = DEF_DIFF_CTX;
	int	*f = &s->diff_flags;
	int	 i, rc = FNC_RC_OK;
	bool	 have_cli_opt = false;

	FLAG_SET(*f, FNC_DIFF_STRIP_EOLCR);

	if (s->context == -1) {
		rc = fnc_conf_getopt(&opt, FNC_DIFF_CONTEXT, false);
		if (rc != FNC_RC_OK)
			return rc;
		if (opt != NULL) {
			/* XXX ignore error and use default context instead */
			xstrtonum(&ctx, opt, 0, MAX_DIFF_CTX);
		}
		s->context = ctx;
		free(opt);
	}

	if (s->diff_mode == DIFF_MODE_STASH)
		return FNC_RC_OK;

	/* persistent options (i.e., 'fnc set' or envvars) */
	rc = fnc_conf_getopt(&opt, FNC_COLOUR_HL_LINE, false);
	if (rc != FNC_RC_OK)
		return rc;
	if (opt != NULL) {
		if (strcasecmp(opt, "mono") == 0)
			s->sline = SLINE_MONO;
		free(opt);
	}

	rc = fnc_conf_getopt(&opt, FNC_DIFF_FLAGS, false);
	if (rc != FNC_RC_OK)
		return rc;
	if (opt == NULL || *opt == '\0')
		return FNC_RC_OK;

	if (FLAG_CHK(*f, FNC_DIFF_SIDEBYSIDE) || FLAG_CHK(*f, FNC_DIFF_LINENO))
		have_cli_opt = true;

	for (i = 0; opt[i] != '\0'; ++i) {
		switch (ch = opt[i]) {
		case 'b':
			FLAG_SET(*f, FNC_DIFF_BRIEF);
			break;
		case 'C':
			*colour = false;
			break;
		case 'D':
			FLAG_SET(*f, FNC_DIFF_STATMIN);
			break;
		case 'i':
			FLAG_SET(*f, FNC_DIFF_INVERT);
			break;
		case 'l':
		case 's':
			if (!have_cli_opt && ch == 'l') {
				FLAG_CLR(*f, FNC_DIFF_SIDEBYSIDE);
				FLAG_SET(*f, FNC_DIFF_LINENO);
			} else if (!have_cli_opt && ch == 's') {
				FLAG_CLR(*f, FNC_DIFF_LINENO);
				FLAG_SET(*f, FNC_DIFF_SIDEBYSIDE);
			}
			break;
		case 'P':
			FLAG_CLR(*f, FNC_DIFF_PROTOTYPE);
			break;
		case 'q':
			FLAG_CLR(*f, FNC_DIFF_VERBOSE);
			break;
		case 'W':
			s->wrap = true;
			break;
		case 'w':
			FLAG_SET(*f, FNC_DIFF_IGNORE_ALLWS);
			break;
		default:
			break;
		}
	}

	free(opt);
	return FNC_RC_OK;
}

static void
show_diff_status(struct fnc_view *view)
{
	mvwaddstr(view->window, 0, 0, "generating diff...");
	updatescreen(view->window, true, true);
}

static int
create_diff(struct fnc_diff_view_state *s)
{
	char	*line = NULL;
	off_t	 off;
	ssize_t	 len;
	size_t	 sz = 0;
	int	 rc;

	s->nhunks = 0;
	s->nlines = 0;
	s->ndlines = 0;
	free(s->dlines);
	free(s->line_offsets);

	s->dlines = malloc(sizeof(*s->dlines));
	if (s->dlines == NULL)
		return RC_ERRNO("malloc");

	s->line_offsets = malloc(sizeof(*s->line_offsets));
	if (s->line_offsets == NULL)
		return RC_ERRNO("malloc");

	if (s->buf.used > 0)
		fsl_buffer_reuse(&s->buf);

	rc = dispatch_diff_request(s);
	if (rc)
		return rc;

	/*
	 * The wiki parent id (s->selected_entry->puuid) may not be set
	 * til diff_wiki() so set s->id{1,2} after dispatch_diff_request().
	 */
	if (s->selected_entry->puuid) {
		free(s->id1);
		s->id1 = strdup(s->selected_entry->puuid);
		if (s->id1 == NULL)
			return RC_ERRNO("strdup");
	} else
		s->id1 = NULL;  /* initial commit, tag, technote, etc. */

	if (s->selected_entry->uuid) {
		free(s->id2);
		s->id2 = strdup(s->selected_entry->uuid);
		if (s->id2 == NULL)
			return RC_ERRNO("strdup");
	} else
		s->id2 = NULL;  /* local work tree */

	if (s->stash) {
		/* arrived here via fnc_stash(); make diffs and return */
		rc = make_stash_diff(s, &s->buf);
		fsl_buffer_clear(&s->buf);
		return FNC_RC_OK;
	}

	/*
	 * Save line offsets for grepping and scrolling the diff view,
	 * and save the number of hunks in the diff for the stash cmd.
	 */
	off = s->line_offsets[s->nlines - 1];
	fsl_buffer_seek(&s->buf, off, FSL_BUFFER_SEEK_SET);

	while ((len = fsl_buffer_getline(&line, &sz, &s->buf)) != -1) {
		if (s->nlines < s->ndlines &&
		    s->dlines[s->nlines] == LINE_DIFF_HUNK)
			++s->nhunks;

		off += len;
		rc = add_line_offset(&s->line_offsets, &s->nlines, off);
		if (rc != 0) {
			free(line);
			return rc;
		}
	}

	free(line);
	--s->nlines;	/* don't count EOF '\n' */
	fsl_buffer_rewind(&s->buf);
	return FNC_RC_OK;
}

static int
dispatch_diff_request(struct fnc_diff_view_state *s)
{
	struct fnc_commit_artifact	*c = s->selected_entry;
	int				 rc;
	enum fnc_diff_type		 request = c->diff_type;

	if (c->changeset.used > 0) {
		rc = fsl_list_clear(&c->changeset, fnc_file_artifact_free,
		    NULL);
		if (rc != FNC_RC_OK)
			return RC_LIBF(rc, "fsl_list_clear");
		fsl_list_reserve(&c->changeset, 0);
	}

	switch (request) {
	case FNC_DIFF_WIKI:
		rc = diff_wiki(s);
		break;
	case FNC_DIFF_BLOB:
		rc = diff_file_artifact(s, c->prid, NULL, NULL,
		    FSL_CKOUT_CHANGE_MOD);
		break;
	case FNC_DIFF_COMMIT:
		rc = diff_commit(s);
		if (0) {
			/*
			 * XXX This is the previous procedure used to
			 * generate diffs of commits or between arbitrary
			 * versions. It is kept around for the time being
			 * till the new diff_commit() routine is proven.
			 */
			rc = diff_versions(s);
		}
		break;
	case FNC_DIFF_CKOUT:
		rc = diff_checkout(s);
		break;
	default:
		rc = RC(FNC_RC_BAD_OPTION, "invalid diff type: %s", request);
		break;
	}
	if (rc)
		return rc;
	/*
	 * fossil(1)'s initial empty commit and commits creating new
	 * branches or adding new wiki pages may produce empty diffs
	 * with zero dlines but should still be displayed. However, if
	 * unmodified tracked files passed to 'fnc diff' result in an
	 * empty diff, report this rather than display an empty diff.
	 */
	if (s->ndlines == 0 && c->puuid != NULL &&
	    (s->view == NULL || s->view->parent == NULL) &&
	    request != FNC_DIFF_WIKI)
		return RC_BREAK("no changes in the diff requested");

	rc = add_line_type(&s->dlines, &s->ndlines, LINE_BLANK);  /* eof \n */
	if (rc)
		return rc;

	/*
	 * Write commit metadata after the diff has been computed
	 * because diffstat metrics, which are listed with each file
	 * in the changeset, are aggregated while computing the diff.
	 */
	return alloc_commit_meta(s);
}

static int
alloc_commit_meta(struct fnc_diff_view_state *s)
{
	struct fsl_buffer	 buf = fsl_buffer_empty;
	off_t			*offsets = NULL, *o = NULL;
	enum line_type		*dlines = NULL, *d = NULL;
	size_t			 nd = 0, no = 0;
	int			 rc;

	d = malloc(sizeof(*d));
	if (d == NULL)
		return RC_ERRNO("malloc");

	o = malloc(sizeof(*o));
	if (o == NULL) {
		rc = RC_ERRNO("malloc");
		goto end;
	}

	rc = add_line_offset(&o, &no, 0);
	if (rc)
		goto end;
	rc = add_line_type(&d, &nd, LINE_BLANK);
	if (rc)
		goto end;

	if (s->diff_mode == DIFF_MODE_META) {
		rc = write_commit_meta(&buf, s->selected_entry, *s->ncols,
		    s->showln, &o, &no, &d, &nd);
		if (rc)
			goto end;
	}

	if (s->diff_mode != DIFF_MODE_STASH &&
	    s->selected_entry->diff_type != FNC_DIFF_WIKI) {
		rc = write_changeset(&buf, s, &o, &no, &d, &nd);
		if (rc)
			goto end;
	}

	offsets = reallocarray(NULL, no + s->nlines, sizeof(*offsets));
	if (offsets == NULL) {
		rc = RC_ERRNO("reallocarray");
		goto end;
	}

	memcpy(offsets, o, no * sizeof(*o));
	memcpy(offsets + no, s->line_offsets, s->nlines * sizeof(*o));
	free(s->line_offsets);
	s->line_offsets = offsets;
	s->nlines += no;

	dlines = reallocarray(NULL, nd + s->ndlines, sizeof(*dlines));
	if (dlines == NULL) {
		rc = RC_ERRNO("reallocarray");
		goto end;
	}

	memcpy(dlines, d, nd * sizeof(*d));
	memcpy(dlines + nd, s->dlines, s->ndlines * sizeof(*d));
	free(s->dlines);
	s->dlines = dlines;
	s->ndlines += nd;

	/* append diff to the commit metadata and diffstat */
	if (buf_write(&buf, s->buf.mem, s->buf.used) == -1) {
		rc = RC_LIBF(buf.errCode, "buf_write");
		goto end;
	}
	fsl_buffer_swap_free(&s->buf, &buf, 1);

end:
	free(d);
	free(o);
	return rc;
}

/*
 * Iterate lines in diff for each hunk line (i.e., @@ -w,x +y,z @@),
 * and write a patch file at $TMPDIR/fnc-XXXXXX-{stash,ckout}.diff
 * corresponding to the stash step identified by s->stash:
 *   1. HUNK_STASH: diff of all hunks selected to stash
 *   2. HUNK_CKOUT: diff of all hunks to be kept in the checkout
 */
static int
make_stash_diff(struct fnc_diff_view_state *s, struct fsl_buffer *diff)
{
	FILE		*f = NULL;
	char		*line = NULL;
	const char	*suffix;
	char		*pp0, *pp = NULL, *tmppath = NULL;
	size_t		 idx, lineno, sz = 0;
	ssize_t		 len;
	int		 i, rc;
	bool		 drop = false;

	i = strlen(fnc__tmpdir);
	while (i > 0 && fnc__tmpdir[i - 1] == '/')
		--i;

	/* make temp files for stash and ckout patches */
	if (s->stash == HUNK_STASH) {
		suffix = "-stash.diff";
		pp0 = s->scx.patch[0];
	} else {
		suffix = "-ckout.diff";
		pp0 = s->scx.patch[1];
	}

	if ((tmppath = fsl_mprintf("%.*s/fnc", i, fnc__tmpdir)) == NULL)
		return RC_ERRNO("fsl_mprintf");

	rc = fnc_open_tmpfile(&pp, &f, tmppath, suffix);
	if (rc)
		goto end;

	if (strlcpy(pp0, pp, sizeof(s->scx.patch[0])) >=
	    sizeof(s->scx.patch[0])) {
		rc = RC(FNC_RC_NO_SPACE, "strlcpy");
		goto end;
	}

	idx = 0;
	lineno = 1;
	while (lineno < s->ndlines &&
	    (len = fsl_buffer_getline(&line, &sz, diff)) != -1) {
		/*
		 * Because we don't know when seeing the file index whether
		 * the file has any hunks selected to stash, we need to write
		 * all index headers otherwise we end up with orphaned hunks
		 * (i.e., hunks with no file index), which cannot be applied.
		 * But an index with no hunks is fine (we can just ignore it).
		 */
		if (s->dlines[lineno] == LINE_DIFF_INDEX ||
		    s->dlines[lineno] == LINE_DIFF_HEADER ||
		    s->dlines[lineno] == LINE_DIFF_META)
			drop = false;
		if (s->dlines[lineno++] == LINE_DIFF_HUNK) {
			/*
			 * If creating the diff of changes selected to stash
			 * and this hunk is marked to keep in the ckout, or if
			 * creating the diff of changes selected to keep in the
			 * checkout and this hunk is marked to stash, drop it.
			 */
			if (s->stash == HUNK_STASH)
				drop = !isset(s->scx.stash, idx);
			else
				drop = isset(s->scx.stash, idx);
			++idx;
		}
		if (!drop) {
			size_t w = len;

			if (fwrite(line, 1, len, f) != w) {
				rc = RC_ERRNO("fwrite");
				goto end;
			}
		}
	}
	if (diff->errCode != 0) {
		rc = RC_LIBF(diff->errCode, "fsl_buffer_getline");
		goto end;
	}

	if (f != NULL && fflush(f) == EOF)
		rc = RC_ERRNO("fflush");

end:
	if (f != NULL && fclose(f) == EOF && rc == FNC_RC_OK)
		rc = RC_ERRNO("fclose: %s", pp);
	free(tmppath);
	free(line);
	free(pp);
	return rc;
}

static int
create_changeset(struct fnc_commit_artifact *commit, enum fnc_diff_mode mode)
{
	struct fsl_cx	*f;
	struct fsl_stmt	 st = fsl_stmt_empty;
	struct fsl_list	 changeset = fsl_list_empty;
	int		 finalize_rc, rc;

	/*
	 * Use the mlink table to generate a commit changeset, but parse
	 * manifests to create the changeset between two arbitrary versions.
	 */
	if (mode != DIFF_MODE_META)
		return parse_manifest(commit);

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	rc = fsl_cx_prepare(f, &st,
	    "SELECT name, mperm, "
	    "(SELECT uuid FROM blob WHERE rid=mlink.pid), "
	    "(SELECT uuid FROM blob WHERE rid=mlink.fid), "
	    "(SELECT name FROM filename WHERE filename.fnid=mlink.pfnid) "
	    "FROM mlink JOIN filename ON filename.fnid=mlink.fnid "
	    "WHERE mlink.mid=%d AND NOT mlink.isaux "
	    "AND (mlink.fid > 0 "
	    "OR mlink.fnid NOT IN (SELECT pfnid FROM mlink WHERE mid=%d)) "
	    "ORDER BY name", commit->rid, commit->rid);
	if (rc)
		return RC_LIBF(rc, "fsl_cx_prepare");

	while ((rc = fsl_stmt_step(&st)) == FSL_RC_STEP_ROW) {
		struct fnc_file_artifact	*ffa = NULL;
		const char			*path, *prev_path;
		const char			*uuid, *prev_uuid;
		uint64_t			 n;
		size_t				 pathlen, prev_pathlen;
		int				 perm;

		if ((rc = fsl_stmt_get_text(&st, 0, &path, &n)) != 0) {
			rc = RC_LIBF(rc, "fsl_stmt_get_text");
			goto end;
		}
		pathlen = n;
		if ((rc = fsl_stmt_get_int32(&st, 1, &perm)) != 0) {
			rc = RC_LIBF(rc, "fsl_stmt_get_int32");
			goto end;
		}
		if ((rc = fsl_stmt_get_text(&st, 2, &prev_uuid, NULL)) != 0) {
			rc = RC_LIBF(rc, "fsl_stmt_get_text");
			goto end;
		}
		if ((rc = fsl_stmt_get_text(&st, 3, &uuid, NULL)) != 0) {
			rc = RC_LIBF(rc, "fsl_stmt_get_text");
			goto end;
		}
		if ((rc = fsl_stmt_get_text(&st, 4, &prev_path, &n)) != 0) {
			rc = RC_LIBF(rc, "fsl_stmt_get_text");
			goto end;
		}
		prev_pathlen = n;

		rc = alloc_file_artifact(&ffa, path, uuid, &pathlen,
		    prev_path, prev_uuid, prev_pathlen, perm);
		if (rc)
			goto end;

		rc = fsl_list_append(&changeset, ffa);
		if (rc) {
			rc = RC_LIBF(rc, "fsl_list_append");
			goto end;
		}

		commit->maxpathlen = MAX(pathlen, commit->maxpathlen);
	}
	if (rc == FSL_RC_STEP_DONE)
		rc = FNC_RC_OK;
	else {
		if (fsl_db_err_get(f->dbMain, NULL, NULL) != FNC_RC_OK)
			rc = RC_LIBF(fsl_cx_uplift_db_error(f, f->dbMain),
			    "fsl_stmt_step");
		else
			rc = RC_LIBF(rc, "fsl_stmt_step");
		goto end;
	}

	commit->changeset = changeset;

end:
	finalize_rc = fsl_stmt_finalize(&st);
	if (finalize_rc != 0 && rc == 0)
		rc = RC_LIBF(finalize_rc, "fsl_stmt_finalize");
	if (rc != 0)
		fsl_list_clear(&changeset, fnc_file_artifact_free, NULL);
	return rc;
}

static int
parse_manifest(struct fnc_commit_artifact *commit)
{
	struct fsl_cx		*f;
	const struct fsl_card_F	*cf2, *cf1 = NULL;
	struct fsl_deck		 d1 = fsl_deck_empty, d2 = fsl_deck_empty;
	struct fsl_list		 changeset = fsl_list_empty;
	fsl_id_t		 id1;
	int			 rc;
	enum fsl_satype_e	 atype = FSL_SATYPE_CHECKIN;

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	if (commit->diff_type == FNC_DIFF_WIKI) {
		switch (*commit->type) {
		case 't':
			switch (commit->type[1]) {
			case 'a':
				atype = FSL_SATYPE_CONTROL;
				break;
			case 'e':
				atype = FSL_SATYPE_TECHNOTE;
				break;
			case 'i':
				atype = FSL_SATYPE_TICKET;
				break;
			}
			break;
		case 'f':
			atype = FSL_SATYPE_FORUMPOST;
			break;
		default:
			atype = FSL_SATYPE_WIKI;
			break;
		}
	}
	rc = fsl_deck_load_rid(f, &d2, commit->rid, atype);
	if (rc) {
		rc = RC_LIBF(rc, "fsl_deck_load_rid: %s %d",
		    commit->type, commit->rid);
		goto end;
	}
	rc = fsl_deck_F_rewind(&d2);
	if (rc) {
		rc = RC_LIBF(rc, "fsl_deck_rewind");
		goto end;
	}

	/*
	 * For the one-and-only special case of repositories, such as the
	 * canonical fnc, that do not have an "initial empty check-in", we
	 * proceed with no parent version to diff against.
	 */
	if (commit->puuid) {
		rc = symtorid(&id1, commit->puuid, atype);
		if (rc != 0)
			goto end;
		rc = fsl_deck_load_rid(f, &d1, id1, atype);
		if (rc)
			goto end;
		rc = fsl_deck_F_rewind(&d1);
		if (rc)
			goto end;
		fsl_deck_F_next(&d1, &cf1);
	}

	rc = fsl_deck_F_next(&d2, &cf2);
	if (rc) {
		rc = RC_LIBF(rc, "fsl_deck_F_next");
		goto end;
	}

	while (cf1 != NULL || cf2 != NULL) {
		struct fnc_file_artifact	*ffa = NULL;
		size_t				 len;
		int				 changed;

		if (cf1 == NULL)
			changed = 1;
		else if (cf2 == NULL)
			changed = -1;
		else
			changed = strcmp(cf1->name, cf2->name);

		if (changed > 0) {
			len = strlen(cf2->name);
			rc = alloc_file_artifact(&ffa, cf2->name, cf2->uuid,
			    &len, NULL, NULL, 0, cf2->perm);
			if (rc)
				goto end;
			rc = fsl_deck_F_next(&d2, &cf2);
			if (rc)
				goto end;
		} else if (changed < 0) {
			len = strlen(cf1->name);
			rc = alloc_file_artifact(&ffa, cf1->name, NULL, &len,
			    NULL, cf1->uuid, 0, cf1->perm);
			if (rc)
				goto end;
			rc = fsl_deck_F_next(&d1, &cf1);
			if (rc)
				goto end;
		} else if (fsl_uuidcmp(cf1->uuid, cf2->uuid) == 0) {
			rc = fsl_deck_F_next(&d1, &cf1);
			if (rc) {
				rc = RC_LIBF(rc, "fsl_deck_F_next");
				goto end;
			}
			rc = fsl_deck_F_next(&d2, &cf2);
			if (rc) {
				rc = RC_LIBF(rc, "fsl_deck_F_next");
				goto end;
			}
			continue;  /* no change */
		} else {
			len = strlen(cf1->name);
			rc = alloc_file_artifact(&ffa, cf2->name, cf2->uuid,
			    &len, cf1->name, cf1->uuid, strlen(cf2->name),
			    cf2->perm);
			if (rc)
				goto end;
			rc = fsl_deck_F_next(&d1, &cf1);
			if (rc) {
				rc = RC_LIBF(rc, "fsl_deck_F_next");
				goto end;
			}
			rc = fsl_deck_F_next(&d2, &cf2);
			if (rc) {
				rc = RC_LIBF(rc, "fsl_deck_F_next");
				goto end;
			}
		}

		rc = fsl_list_append(&changeset, ffa);
		if (rc) {
			rc = RC_LIBF(rc, "fsl_list_append");
			goto end;
		}

		commit->maxpathlen = MAX(len, commit->maxpathlen);
	}

	commit->changeset = changeset;

end:
	fsl_deck_finalize(&d1);
	fsl_deck_finalize(&d2);
	return rc;
}

static int
alloc_file_artifact(struct fnc_file_artifact **ret, const char *path,
    const char *uuid, size_t *pathlen, const char *prev_path,
    const char *prev_uuid, size_t prev_pathlen, int perm)
{
	struct fnc_file_artifact	*ffa = NULL;
	int				 rc = FNC_RC_OK;

	(void)perm;
	*ret = NULL;

	ffa = calloc(1, sizeof(*ffa));
	if (ffa == NULL)
		return RC_ERRNO("calloc");

	ffa->fc = calloc(1, sizeof(*ffa->fc));
	if (ffa->fc == NULL) {
		rc = RC_ERRNO("calloc");
		goto end;
	}

	ffa->fc->name = strdup(path);
	if (ffa->fc->name == NULL) {
		rc = RC_ERRNO("strdup");
		goto end;
	}

	if (prev_uuid != NULL) {
		ffa->puuid = strdup(prev_uuid);
		if (ffa->puuid == NULL) {
			rc = RC_ERRNO("strdup");
			goto end;
		}
	}

	if (uuid == NULL) {
		ffa->fc->uuid = strdup(prev_uuid);
		if (ffa->fc->uuid == NULL) {
			rc = RC_ERRNO("strdup");
			goto end;
		}
		ffa->change = FSL_CKOUT_CHANGE_REMOVED;
	} else if (prev_uuid == NULL) {
		ffa->fc->uuid = strdup(uuid);
		if (ffa->fc->uuid == NULL) {
			rc = RC_ERRNO("strdup");
			goto end;
		}
		ffa->change = FSL_CKOUT_CHANGE_ADDED;
	} else if (prev_path != NULL && strcmp(path, prev_path) != 0) {
		ffa->fc->uuid = strdup(uuid);
		if (ffa->fc->uuid == NULL) {
			rc = RC_ERRNO("strdup");
			goto end;
		}
		ffa->fc->priorName = strdup(prev_path);
		if (ffa->fc->priorName == NULL) {
			rc = RC_ERRNO("strdup");
			goto end;
		}
		*pathlen += prev_pathlen + 4;	/* "prev_path -> path" */
		ffa->change = FSL_CKOUT_CHANGE_RENAMED;
	} else {
		ffa->fc->uuid = strdup(uuid);
		if (ffa->fc->uuid == NULL) {
			rc = RC_ERRNO("strdup");
			goto end;
		}
		ffa->change = FSL_CKOUT_CHANGE_MOD;
	}

end:
	if (rc != FNC_RC_OK)
		fnc_file_artifact_free(ffa, NULL);
	else
		*ret = ffa;
	return rc;
}

static int
write_commit_meta(struct fsl_buffer *buf, struct fnc_commit_artifact *c,
    size_t ncols, int showln, off_t **line_offsets, size_t *nlines,
    enum line_type **dlines, size_t *ndlines)
{
	off_t	off = 0;
	int	rc;

	if ((rc = buf_printf(buf,"%s %s\n", c->type, c->uuid)) == -1)
		return RC_LIBF(buf->errCode, "buf_printf");
	off += rc;

	rc = add_line_offset(line_offsets, nlines, off);
	if (rc)
		return rc;
	rc = add_line_type(dlines, ndlines, LINE_DIFF_META);
	if (rc)
		return rc;

	if ((rc = buf_printf(buf,"user: %s\n", c->user)) == -1)
		return RC_LIBF(buf->errCode, "buf_printf");
	off += rc;

	rc = add_line_offset(line_offsets, nlines, off);
	if (rc)
		return rc;
	rc = add_line_type(dlines, ndlines, LINE_DIFF_USER);
	if (rc)
		return rc;

	if ((rc = buf_printf(buf,"tags: %s\n",
	    c->branch != NULL ? c->branch : _PATH_DEVNULL)) == -1)
		return RC_LIBF(buf->errCode, "buf_printf");
	off += rc;

	rc = add_line_offset(line_offsets, nlines, off);
	if (rc)
		return rc;
	rc = add_line_type(dlines, ndlines, LINE_DIFF_TAGS);
	if (rc)
		return rc;

	if ((rc = buf_printf(buf,"date: %s\n", c->timestamp)) == -1)
		return RC_LIBF(buf->errCode, "buf_printf");
	off += rc;

	rc = add_line_offset(line_offsets, nlines, off);
	if (rc)
		return rc;
	rc = add_line_type(dlines, ndlines, LINE_DIFF_DATE);
	if (rc)
		return rc;

	/* add blank line between end of commit metadata and start of logmsg */
	if (buf_putc(buf, '\n') == -1)
		return RC_LIBF(buf->errCode, "buf_putc");
	++off;

	rc = add_line_offset(line_offsets, nlines, off);
	if (rc)
		return rc;
	rc = add_line_type(dlines, ndlines, LINE_BLANK);
	if (rc)
		return rc;

	if (showln)
		ncols -= MIN(LINENO_WIDTH, ncols);

	if (__predict_false(ncols == 0))
		return FNC_RC_OK;

	return fmtlogmsg(buf, c->comment, ncols,
	    line_offsets, nlines, dlines, ndlines);
}

static int
fmtlogmsg(struct fsl_buffer *buf, char *logmsg, size_t ncols,
    off_t **line_offsets, size_t *nlines, enum line_type **dlines,
    size_t *ndlines)
{
	FILE	*f;
	char	*line = NULL;
	off_t	 off = (*line_offsets)[*nlines - 1];
	size_t	 col = 0, sz = 0;
	ssize_t	 len;
	int	 rc;

	f = fmemopen(logmsg, strlen(logmsg), "r");
	if (f == NULL)
		return RC_ERRNO("fmemopen");

	while ((len = getline(&line, &sz, f)) != -1) {
		rc = logmsgln(buf, line, len, ncols, line_offsets,
		    nlines, dlines, ndlines, &off, &col);
		if (rc != 0)
			goto end;
	}
	if (ferror(f)) {
		rc = RC_ERRNO("getline");
		goto end;
	}

	/* add blank line between end of comment and start of changeset */
	if (buf_putc(buf, '\n') == -1) {
		rc = RC_LIBF(buf->errCode, "buf_putc");
		goto end;
	}
	++off;

	rc = add_line_offset(line_offsets, nlines, off);
	if (rc)
		goto end;
	rc = add_line_type(dlines, ndlines, LINE_BLANK);

end:
	if (fclose(f) == EOF && rc == 0)
		rc = RC_ERRNO("fclose");
	free(line);
	return rc;
}

static int
logmsgln(struct fsl_buffer *b, char *line, size_t len, size_t ncols,
    off_t **line_offsets, size_t *nlines, enum line_type **dlines,
    size_t *ndlines, off_t *off, size_t *col)
{
	int rc, lf = line[len - 1] != '\n';

	if (len <= ncols - *col || strcspn(line, "\n") == 0) {
		do {
			if ((rc = buf_write(b, line, len)) == -1)
				return RC_LIBF(b->errCode, "buf_write");
			if (lf) {
				if (buf_putc(b, '\n') == -1)
					return RC_LIBF(b->errCode, "buf_putc");
				++rc;
			}
			*off += rc;

			rc = add_line_offset(line_offsets, nlines, *off);
			if (rc != 0)
				return rc;
			rc = add_line_type(dlines, ndlines, LINE_DIFF_COMMENT);
			if (rc != 0)
				return rc;
		} while (*col > 0 && (*col = 0, len == 1) && *line == '\n');
	} else {
		rc = wrapline(b, line, ncols, line_offsets, nlines,
		    dlines, ndlines, off, col);
		if (rc != 0)
			return rc;
		if (lf) {
			if (buf_putc(b, '\n') == -1)
				return RC_LIBF(b->errCode, "buf_putc");
			++*(off);

			rc = add_line_offset(line_offsets, nlines, *off);
			if (rc != 0)
				return rc;
			rc = add_line_type(dlines, ndlines, LINE_DIFF_COMMENT);
			if (rc != 0)
				return rc;
		}
	}
	return FNC_RC_OK;
}

#define FNC_CHANGE_MODIFIED	'~'
#define FNC_CHANGE_ADDED	'+'
#define FNC_CHANGE_REMOVED	'-'
#define FNC_CHANGE_RENAMED	'>'
#define FNC_CHANGE_UNKNOWN	'!'

static int
write_changeset(struct fsl_buffer *buf, struct fnc_diff_view_state *s,
    off_t **line_offsets, size_t *nlines, enum line_type **dlines,
    size_t *ndlines)
{
	off_t		off = (*line_offsets)[*nlines - 1];
	uint32_t	add = 0, rm = 0;
	size_t		i;
	int		rc, width = s->selected_entry->maxpathlen;

	for (i = 0; i < s->selected_entry->changeset.used; ++i) {
		struct fnc_file_artifact	*ffa;
		char				 c;

		ffa = s->selected_entry->changeset.list[i];

		if (!path_to_diff(s->paths, ffa->fc->name, ffa->fc->priorName))
			continue;

		switch (ffa->change) {
		case FSL_CKOUT_CHANGE_MOD:
			c = FNC_CHANGE_MODIFIED;
			break;
		case FSL_CKOUT_CHANGE_ADDED:
			c = FNC_CHANGE_ADDED;
			break;
		case FSL_CKOUT_CHANGE_RENAMED:
			c = FNC_CHANGE_RENAMED;
			break;
		case FSL_CKOUT_CHANGE_REMOVED:
			c = FNC_CHANGE_REMOVED;
			break;
		default:
			c = FNC_CHANGE_UNKNOWN;
			break;
		}

		if ((rc = buf_printf(buf, "[%c] %s%s%s",
		    c, c != FNC_CHANGE_RENAMED ?
		    ffa->fc->name : ffa->fc->priorName,
		    c != FNC_CHANGE_RENAMED ? "" : " -> ",
		    c != FNC_CHANGE_RENAMED ? "" : ffa->fc->name)) == -1)
			return RC_LIBF(buf->errCode, "buf_printf");
		off += rc;

		/*
		 * 6 = {c | c E changeset line, !width ^ < '|'}
		 * For example: [~] max/path.c  |
		 *              ----          --
		 */
		if ((rc = buf_printf(buf, "%*s", width + 6 - rc, "")) == -1)
			return RC_LIBF(buf->errCode, "buf_printf");
		off += rc;

		rc = plot_histogram(buf, s, &off, ffa->diffstat);
		if (rc)
			return rc;

		/* XXX Zap the histogram and only show a minimal diffstat? */
#if 0
		n = fprintf(s->f, "| %*llu+ %*llu-\n",
		    s->selected_entry->dswidths & 0xff,
		    ffa->diffstat & 0xffffffff,
		    s->selected_entry->dswidths >> 8,
		    ffa->diffstat >> 32);
		if (n < 0) {
			rc = RC(FNC_RC_IO, "fprintf");
			goto end;
		}
		off += n;
#endif

		rc = add_line_type(dlines, ndlines, LINE_DIFF_META);
		if (rc)
			return rc;

		rc = add_line_offset(line_offsets, nlines, off);
		if (rc)
			return rc;

		/*
		 * Grab the total added lines from the low 32 bits
		 * and the total removed lines from the high 32 bits.
		 */
		add += ffa->diffstat & 0xffffffff;
		rm += ffa->diffstat >> 32;
	}

	/*
	 * Add a blank line between the end of the changeset and the
	 * start of the diff (or diffstat summary line if it is present).
	 */
	if (buf_putc(buf, '\n') == -1)
		return RC_LIBF(buf->errCode, "buf_putc");
	++off;

	rc = add_line_type(dlines, ndlines, LINE_BLANK);
	if (rc)
		return rc;
	rc = add_line_offset(line_offsets, nlines, off);
	if (rc)
		return rc;

	if (i > 0) {
		if ((rc = buf_printf(buf,
		    "%lu file%s changed, %u insertions(+), %u deletions(-)\n",
		   (unsigned long)i, i > 1 ? "s" : "", add, rm)) == -1)
			return RC_LIBF(buf->errCode, "buf_printf");
		off += rc;

		rc = add_line_offset(line_offsets, nlines, off);
		if (rc)
			return rc;
		rc = add_line_type(dlines, ndlines, LINE_DIFF_META);
		if (rc)
			return rc;

		/* blank line between diffstat summary line and the diff */
		if (buf_putc(buf, '\n') == -1)
			return RC_LIBF(buf->errCode, "buf_putc");
		++off;

		rc = add_line_offset(line_offsets, nlines, off);
		if (rc)
			return rc;
		rc = add_line_type(dlines, ndlines, LINE_BLANK);
	}

	return FNC_RC_OK;
}

static int
plot_histogram(struct fsl_buffer *buf, struct fnc_diff_view_state *s,
    off_t *off, uint64_t diffstat)
{
	const char	*fmt;
	uint32_t	 ntotal, nadded, nremoved;
	long		 n;
	int		 rc, flags = s->diff_flags;
	int		 scale, plotwidth = *s->ncols;
	uint8_t		 cwt = s->selected_entry->dswidths >> 16 & 0xff;
	uint8_t		 cwa = s->selected_entry->dswidths & 0xff;
	uint8_t		 cwr = s->selected_entry->dswidths >> 24;

	nadded = diffstat & 0xffffffff;
	nremoved = diffstat >> 32;
	ntotal = nadded + nremoved;

	/* pad an extra space to the column width of each metric */
	++cwt;
	++cwa;
	++cwr;

	if (buf_putc(buf, '|') == -1)
		return RC_LIBF(buf->errCode, "buf_putc");
	++*off;

	if (!FLAG_CHK(flags, FNC_DIFF_STATMIN)) {
		if ((rc = buf_printf(buf, " %*u", cwt, ntotal)) == -1)
			return RC_LIBF(buf->errCode, "buf_printf");
		*off += rc;

		fmt = " %*u %*u ";
	} else
		fmt = " %*u+ %*u-";

	if ((rc = buf_printf(buf, fmt, cwa, nadded, cwr, nremoved)) == -1)
		return RC_LIBF(buf->errCode, "buf_printf");
	*off += rc;

	/*
	 * Columns not yet accounted in x position: 11
	 * 11 = {c | c E changeset line, !(path^cwt^cwa^cwr) ^ !histogram}
	 * [~] path  | cwt cwa cwr +++-----
	 * ....    ....  .  .  .
	 */
	plotwidth -= s->selected_entry->maxpathlen + cwt + cwa + cwr + 11;
	if (plotwidth < 10)
		plotwidth = 10;

	scale = ntotal;
	if (scale < plotwidth)
		scale = plotwidth;	/* bar scale will be 1:1 */

	if (!FLAG_CHK(flags, FNC_DIFF_STATMIN)) {
		long	fuzz = 0;
		int	rc;

		rc = plot_bar(&n, buf, nadded, '+', scale, plotwidth, &fuzz);
		if (rc)
			return rc;
		*off += n;
		rc = plot_bar(&n, buf, nremoved, '-', scale, plotwidth, &fuzz);
		if (rc)
			return rc;
		*off += n;
	}

	if (buf_putc(buf, '\n') == -1)
		return RC_LIBF(buf->errCode, "buf_putc");
	++*off;

	return FNC_RC_OK;
}

/*
 * Write 'count / scale * width' c characters to file f. Assign the
 * interpolated difference to *ev and total number of bytes written
 * to long out param *ret. Return FNC_RC_OK on success, nonzero on error.
 */
static int
plot_bar(long *ret, struct fsl_buffer *buf, long count, int c,
    int scale, int width, long *ev)
{
	long n, product;

	*ret = 0;
	if (count < 1)
		return FNC_RC_OK;

	product = width * count;

	*ret = (product + *ev) / scale;
	n = *ret;
	*ev = product - n * scale - *ev;

	while (--n >= 0) {
		if (buf_printf(buf, "%c", c) == -1)
			return RC_LIBF(buf->errCode, "buf_printf");
	}

	return FNC_RC_OK;
}

/*
 * Wrap line at the rightmost space delimiter that fits within column limit.
 * If there is no space before column limit, emit line without wrapping.
 */
static int
wrapline(struct fsl_buffer *buf, char *line, size_t limit,
    off_t **line_offsets, size_t *nlines, enum line_type **dlines,
    size_t *ndlines, off_t *off, size_t *col)
{
	char	*word;
	size_t	 wordlen, cursor = *col;
	int	 rc;

	while ((word = fnc_strsep(&line, "\n ", &wordlen)) != NULL) {
		wchar_t	*w;
		size_t	 wlen;
		int	 wcw;

		rc = mbs2ws(&w, &wlen, word);
		if (rc != 0)
			return rc;
		span_wline(&wcw, 0, w, -1, cursor);
		free(w);

		if (cursor + wcw > limit && (size_t)wcw < limit) {
			if (buf_putc(buf, '\n') == -1)
				return RC_LIBF(buf->errCode, "buf_putc");
			++(*off);

			rc = add_line_offset(line_offsets, nlines, *off);
			if (rc)
				return rc;
			rc = add_line_type(dlines, ndlines, LINE_DIFF_COMMENT);
			if (rc)
				return rc;
			cursor = 0;
		}
		if ((rc  = buf_write(buf, word, wordlen)) == -1)
			return RC_LIBF(buf->errCode, "buf_write");
		*off += rc;
		cursor += wcw;

		if (cursor < limit && line != NULL) {
			if (buf_putc(buf, ' ') == -1)
				return RC_LIBF(buf->errCode, "buf_putc");
			++(*off);
			++cursor;
		}
	}
	*col = cursor;
	return FNC_RC_OK;
}

static int
add_line_offset(off_t **line_offsets, size_t *nlines, off_t off)
{
	off_t *p;

	p = reallocarray(*line_offsets, *nlines + 1, sizeof(*p));
	if (p == NULL) {
		free(*line_offsets);
		*line_offsets = NULL;
		return RC_ERRNO("reallocarray");
	}

	*line_offsets = p;
	(*line_offsets)[*nlines] = off;
	(*nlines)++;

	return FNC_RC_OK;
}

static int
diff_commit(struct fnc_diff_view_state *s)
{
	struct fsl_cx		*f;
	struct fsl_list		*changeset = &s->selected_entry->changeset;
	struct fsl_buffer	 rhsfile = fsl_buffer_empty;
	struct fsl_buffer	 lhsfile = fsl_buffer_empty;
	size_t			 i;
	int			 rc;

	f = fcli_cx();
	if (f == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	rc = create_changeset(s->selected_entry, s->diff_mode);
	if (rc)
		return rc;

	for (i = 0; i < changeset->used; ++i) {
		struct fnc_file_artifact	*ffa = changeset->list[i];
		const char			*lhspath = NULL, *lhsid = NULL;
		const char			*rhspath = NULL, *rhsid = NULL;
		fsl_ckout_change_e		 change = ffa->change;

		lhspath = ffa->fc->priorName ? ffa->fc->priorName :
		    ffa->fc->name;
		rhspath = ffa->fc->name;
		lhsid = ffa->puuid;
		rhsid = ffa->fc->uuid;

		if (!path_to_diff(s->paths, lhspath, rhspath))
			continue;

		rc = write_diff_meta(s, lhspath, lhsid, rhspath, rhsid,
		    change);
		if (rc)
			goto end;

		if (change != FSL_CKOUT_CHANGE_REMOVED) {
			rc = fsl_content_get_sym(f, ffa->fc->uuid, &rhsfile);
			if (rc) {
				rc = RC(FNC_RC_NO_REF, "fsl_content_get_sym");
				goto end;
			}
		}

		if (change == FSL_CKOUT_CHANGE_RENAMED ||
		    change == FSL_CKOUT_CHANGE_MOD ||
		    change == FSL_CKOUT_CHANGE_REMOVED) {
			rc = fsl_content_get_sym(f, ffa->puuid, &lhsfile);
			if (rc) {
				rc = RC(FNC_RC_NO_REF, "fsl_content_get_sym");
				goto end;
			}
		}

		rc = diff_buffer_from_state(s, &lhsfile, &rhsfile, ffa);
		if (rc != FNC_RC_OK)
			goto end;

		fsl_buffer_reuse(&rhsfile);
		fsl_buffer_reuse(&lhsfile);
	}

end:
	fsl_buffer_clear(&rhsfile);
	fsl_buffer_clear(&lhsfile);
	return rc;
}

/*
 * Fill the buffer with the differences between commit->uuid and commit->puuid.
 * commit->rid (to load into deck d2) is the *this* version, and commit->puuid
 * (to be loaded into deck d1) is the version we diff against. Step through the
 * deck of F(ile) cards from both versions to determine: (1) if we have new
 * files added (i.e., no F card counterpart in d1); (2) files deleted (i.e., no
 * F card counterpart in d2); (3) or otherwise the same file (i.e., F card
 * exists in both d1 and d2). In cases (1) and (2), we call diff_file_artifact()
 * to dump the complete content of the added/deleted file if FNC_DIFF_VERBOSE is
 * set, otherwise only diff metatadata will be output. In case (3), if the
 * hash (UUID) of each F card is the same, there are no changes; if different,
 * both artifacts will be passed to diff_file_artifact() to be diffed.
 */
static int
diff_versions(struct fnc_diff_view_state *s)
{
	struct fsl_cx		*f;
	const struct fsl_card_F	*cf1 = NULL;
	const struct fsl_card_F	*cf2 = NULL;
	struct fsl_deck		 d1 = fsl_deck_empty;
	struct fsl_deck		 d2 = fsl_deck_empty;
	fsl_id_t		 id1;
	int			 rc;

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	rc = fsl_deck_load_rid(f, &d2, s->selected_entry->rid,
	    FSL_SATYPE_CHECKIN);
	if (rc) {
		rc = RC_LIBF(rc, "fsl_deck_load_rid");
		goto end;
	}
	rc = fsl_deck_F_rewind(&d2);
	if (rc) {
		rc = RC_LIBF(rc, "fsl_deck_F_rewind");
		goto end;
	}

	/*
	 * For the one-and-only special case of repositories, such as the
	 * canonical fnc, that do not have an "initial empty check-in", we
	 * proceed with no parent version to diff against.
	 */
	if (s->selected_entry->puuid) {
		rc = symtorid(&id1, s->selected_entry->puuid,
		    FSL_SATYPE_CHECKIN);
		if (rc != 0)
			goto end;
		rc = fsl_deck_load_rid(f, &d1, id1, FSL_SATYPE_CHECKIN);
		if (rc) {
			rc = RC(FNC_RC_NO_RID, "fsl_deck_load_rid");
			goto end;
		}
		rc = fsl_deck_F_rewind(&d1);
		if (rc) {
			rc = RC_LIBF(rc, "fsl_deck_F_rewind");
			goto end;
		}
		rc = fsl_deck_F_next(&d1, &cf1);
		if (rc) {
			rc = RC_LIBF(rc, "fsl_deck_F_next");
			goto end;
		}
	}

	rc = fsl_deck_F_next(&d2, &cf2);
	if (rc) {
		rc = RC_LIBF(rc, "fsl_deck_F_next");
		goto end;
	}
	while (cf1 != NULL || cf2 != NULL) {
		const fsl_card_F	*lhs = NULL, *rhs = NULL;
		fsl_ckout_change_e	 change = FSL_CKOUT_CHANGE_NONE;
		int			 different;
		bool			 diff_this;

		diff_this = path_to_diff(s->paths, cf1->name, cf2->name);

		if (cf1 == NULL)
			different = 1;	/* file added */
		else if (cf2 == NULL)
			different = -1;	/* file deleted */
		else
			different = strcmp(cf1->name, cf2->name);

		if (different) {
			if (different > 0) {
				rhs = cf2;
				change = FSL_CKOUT_CHANGE_ADDED;
				if ((rc = fsl_deck_F_next(&d2, &cf2)) != 0) {
					rc = RC_LIBF(rc, "fsl_deck_F_next");
					goto end;
				}
			} else if (different < 0) {
				lhs = cf1;
				change = FSL_CKOUT_CHANGE_REMOVED;
				if ((rc = fsl_deck_F_next(&d1, &cf1)) != 0) {
					rc = RC_LIBF(rc, "fsl_deck_F_next");
					goto end;
				}
			}

			if (diff_this) {
				rc = diff_file_artifact(s, id1, lhs, rhs,
				    change);
				if (rc)
					goto end;
			}
		} else if (fsl_uuidcmp(cf1->uuid, cf2->uuid) == 0) {
			/* no change */
			if ((rc = fsl_deck_F_next(&d1, &cf1)) != 0) {
				rc = RC_LIBF(rc, "fsl_deck_F_next");
				goto end;
			}
			if ((rc = fsl_deck_F_next(&d2, &cf2)) != 0) {
				rc = RC_LIBF(rc, "fsl_deck_F_next");
				goto end;
			}
		} else {
			change = FSL_CKOUT_CHANGE_MOD;
			if (diff_this) {
				rc = diff_file_artifact(s, id1, cf1, cf2,
				    change);
				if (rc)
					goto end;
			}
			if ((rc = fsl_deck_F_next(&d1, &cf1)) != 0) {
				rc = RC_LIBF(rc, "fsl_deck_F_next");
				goto end;
			}
			if ((rc = fsl_deck_F_next(&d2, &cf2)) != 0) {
				rc = RC_LIBF(rc, "fsl_deck_F_next");
				goto end;
			}
		}
	}

end:
	fsl_deck_finalize(&d1);
	fsl_deck_finalize(&d2);
	return rc;
}

/*
 * Iterate path list paths and return true if either:
 *   1. both lhspath and rhspath are the same and either
 *	their full path or parent dir(s) match a path in the paths list
 *   2. lhspath has been deleted and is a full or partial match as per (1)
 *   3. rhspath has been added and is a full or partial match as per (1)
 * Otherwise return false.
 */
static bool
path_to_diff(const struct fnc_pathlist_head *paths, const char *lhspath,
    const char *rhspath)
{
	struct fnc_pathlist_entry *pe;

	if (paths == NULL || TAILQ_EMPTY(paths))
		return true;

	TAILQ_FOREACH(pe, paths, entry) {
		if (lhspath != NULL && rhspath != NULL &&
		    strcmp(lhspath, rhspath) == 0 &&
		    strncmp(pe->path, lhspath, pe->pathlen) == 0) {
			/* path or parent dir matches modified file */
			return true;
		}
		if (lhspath == NULL && rhspath != NULL &&
		    strncmp(pe->path, rhspath, pe->pathlen) == 0) {
			/* path or parent dir matches added file */
			return true;
		}
		if (lhspath != NULL && rhspath == NULL &&
		    strncmp(pe->path, lhspath, pe->pathlen) == 0) {
			/* path or parent dir matches deleted file */
			return true;
		}
	}

	/* file not in requested paths to diff */
	return false;
}

/*
 * Diff local changes on disk in the current checkout against either a previous
 * commit or, if no version has been supplied, the current checkout.
 *   buf  output buffer in which diff content is appended
 *   vid  repository database record id of the version to diff against
 * diff_flags, context, and sbs are the same parameters as diff_file_artifact()
 * nb. This routine is only called with 'fnc diff [hash]'; that is, one or
 * zero args—not two—supplied to fnc's diff command line interface.
 */
static int
diff_checkout(struct fnc_diff_view_state *s)
{
	struct fsl_cx		*f;
	struct fsl_deck		 d = fsl_deck_empty;
	struct fsl_stmt		 st = fsl_stmt_empty;
	struct fsl_buffer	 sql = fsl_buffer_empty;
	struct fsl_buffer	 bminus = fsl_buffer_empty;
	struct fsl_buffer	 abspath = fsl_buffer_empty;
	char			*xminus = NULL;
	int32_t			 cid, vid;
	int			 rc, finalize_rc = FNC_RC_OK;

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	if ((rc = fsl_buffer_reserve(&sql, 1024)) != 0)
		return RC_ERRNO("malloc");

	vid = s->selected_entry->prid;
	fsl_ckout_version_info(f, &cid, NULL);

	/*
	 * If a previous version is supplied, load its vfile state to query
	 * changes. Otherwise query the current checkout state for changes.
	 */
	if (vid != cid) {
		/* keep vfile ckout state; but unload vid when finished */
		rc = fsl_vfile_load(f, vid, false, NULL);
		if (rc != 0) {
			rc = RC_LIBF(rc, "fsl_vfile_load");
			goto unload;
		}
		rc = buf_printf(&sql,
		    "SELECT v2.pathname, v2.origname,"
		    "    v2.deleted, v2.chnged, v2.rid == 0,"
		    "    v1.rid, v1.islink"
		    " FROM vfile v1, vfile v2"
		    " WHERE v1.pathname=v2.pathname"
		    " AND v1.vid=%d"
		    " AND v2.vid=%d"
		    " AND ("
		    "    v2.deleted OR v2.chnged OR v1.mrid != v2.rid"
		    " )"
		    " UNION "
		    "SELECT pathname, origname, 1, 0, 0, 0, islink"
		    " FROM vfile v1"
		    " WHERE v1.vid = %d"
		    " AND NOT EXISTS ("
		    "    SELECT 1 FROM vfile v2"
		    "    WHERE v2.vid = %d"
		    "    AND v2.pathname = v1.pathname"
		    " )"
		    " UNION "
		    "SELECT pathname, origname, 0, 0, 1, 0, islink"
		    " FROM vfile v2"
		    " WHERE v2.vid = %d"
		    " AND NOT EXISTS ("
		    "    SELECT 1 FROM vfile v1"
		    "    WHERE v1.vid = %d"
		    "    AND v1.pathname = v2.pathname"
		    " )"
		    " ORDER BY 1", vid, cid, vid, cid, cid, vid);
	} else {
		rc = buf_printf(&sql,
		    "SELECT pathname, origname, deleted, chnged,"
		    "    rid == 0, rid, islink"
		    " FROM vfile"
		    " WHERE vid = %d"
		    " AND ("
		    "    deleted OR chnged OR rid==0"
		    "    OR ("
		    "        origname IS NOT NULL AND origname<>pathname"
		    "    )"
		    " )"
		    " ORDER BY pathname", cid);
	}
	if (rc == -1) {
		rc = RC_LIBF(sql.errCode, "buf_printf");
		goto unload;
	}

	rc = fsl_cx_prepare(f, &st, "%b", &sql);
	if (rc != 0) {
		rc = RC_LIBF(rc, "fsl_cx_prepare");
		goto yield;
	}

	while ((rc = fsl_stmt_step(&st)) == FSL_RC_STEP_ROW) {
		const char		*path, *ogpath;
		int			 deleted, changed, added, fid, symlink;
		fsl_ckout_change_e	 change;

		path = fsl_stmt_g_text(&st, 0, NULL);
		ogpath = fsl_stmt_g_text(&st, 1, NULL);
		deleted = fsl_stmt_g_int32(&st, 2);
		changed = fsl_stmt_g_int32(&st, 3);
		added = fsl_stmt_g_int32(&st, 4);
		fid = fsl_stmt_g_int32(&st, 5);
		symlink = fsl_stmt_g_int32(&st, 6);
		rc = fsl_file_canonical_name2(f->db.ckout.dir, path, &abspath,
		    false);
		if (rc != 0) {
			rc = RC_LIBF(rc, "fsl_file_canonical_name2");
			goto yield;
		}

		if (deleted) {
			ogpath = path;
			change = FSL_CKOUT_CHANGE_REMOVED;
		} else if (fsl_file_access(fsl_buffer_cstr(&abspath), F_OK))
			change = FSL_CKOUT_CHANGE_MISSING;
		else if (added) {
			fid = 0;
			change = FSL_CKOUT_CHANGE_ADDED;
		} else if (changed == 3) {
			fid = 0;
			change = FSL_CKOUT_CHANGE_MERGE_ADD;
		} else if (changed == 5) {
			fid = 0;
			change = FSL_CKOUT_CHANGE_INTEGRATE_ADD;
		} else if (ogpath != NULL && strcmp(ogpath, path) != 0)
			change = FSL_CKOUT_CHANGE_RENAMED;
		else
			change = FSL_CKOUT_CHANGE_MOD;

		/*
		 * For changed files of which this checkout is already aware,
		 * grab their hash to make comparisons. For removed files, if
		 * diffing against a version other than the current checkout,
		 * load the version's manifest to parse for known versions of
		 * said files. If we don't, we risk diffing stale or bogus
		 * content. Known cases include MISSING, DELETED, and RENAMED
		 * files, which fossil(1) misses in some instances.
		 */
		if (fid > 0) {
			xminus = fsl_rid_to_uuid(f, fid);
			if (xminus == NULL) {
				rc = RC_LIBF(rc, "fsl_rid_to_uuid");
				goto yield;
			}
		} else if (vid != cid && !added) {
			const struct fsl_card_F *cf = NULL;

			rc = fsl_deck_load_rid(f, &d, vid, FSL_SATYPE_CHECKIN);
			if (rc)
				goto yield;
			rc = fsl_deck_F_rewind(&d);
			if (rc)
				goto yield;

			while (rc = fsl_deck_F_next(&d, &cf), cf != NULL) {
				if (rc != 0) {
					rc = RC_LIBF(rc, "fsl_deck_F_next");
					goto yield;
				}
				if (strcmp(cf->name, path) != 0)
					continue;

				rc = idtorid(&fid, cf->uuid, FNC_RC_NO_REF);
				if (rc != 0)
					goto yield;
				if ((xminus = strdup(cf->uuid)) == NULL) {
					rc = RC_ERRNO("strdup");
					goto yield;
				}
				break;
			}
		}

		if (xminus == NULL) {
			xminus = strdup(_PATH_DEVNULL);
			if (xminus == NULL) {
				rc = RC_ERRNO("strdup");
				goto yield;
			}
		}

		if (fid > 0 && change != FSL_CKOUT_CHANGE_ADDED) {
			rc = fsl_content_get(f, fid, &bminus);
			if (rc)
				goto yield;
		} else
			fsl_buffer_clear(&bminus);

		if (path_to_diff(s->paths, path, path)) {
			rc = diff_file(s, &bminus, ogpath, path, xminus,
			    fsl_buffer_cstr(&abspath), change, symlink);
			if (rc != FNC_RC_OK)
				goto yield;
		}

		free(xminus);
		xminus = NULL;
		fsl_buffer_reuse(&bminus);
		fsl_buffer_reuse(&abspath);
	}
	if (rc == FSL_RC_STEP_DONE)
		rc = FNC_RC_OK;
	else {
		if (fsl_db_err_get(f->dbMain, NULL, NULL) != FNC_RC_OK)
			rc = RC_LIBF(fsl_cx_uplift_db_error(f, f->dbMain),
			    "fsl_stmt_step");
		else
			rc = RC_LIBF(rc, "fsl_stmt_step");
	}

yield:
	free(xminus);
	fsl_deck_finalize(&d);
	finalize_rc = fsl_stmt_finalize(&st);
	if (finalize_rc != 0 && rc == 0)
		rc = RC_LIBF(finalize_rc, "fsl_stmt_finalize");

unload:
	fsl_buffer_clear(&sql);
	fsl_buffer_clear(&bminus);
	fsl_buffer_clear(&abspath);
	finalize_rc = fsl_vfile_unload_except(f, cid);
	if (finalize_rc != 0 && rc == 0)
		rc = RC_LIBF(finalize_rc, "fsl_vfile_unload_except");
	return rc;
}

/*
 * Write diff index line and file metadata (i.e., file paths and hashes), which
 * signify file addition, removal, or modification.
 *   buf         output buffer in which diff output will be appended
 *   zminus      file name of the file being diffed against
 *   xminus      hex hash of file named zminus
 *   zplus       file name of the file being diffed
 *   xplus       hex hash of the file named zplus
 *   diff_flags  bitwise flags to control the diff
 *   change      enum denoting the versioning change of the file
 */
static int
write_diff_meta(struct fnc_diff_view_state *s, const char *zminus,
    fsl_uuid_cstr xminus, const char *zplus, fsl_uuid_cstr xplus,
    const fsl_ckout_change_e change)
{
	const char	*index, *plus, *minus;
	char		 eq[72];
	int		 c, rc;
	enum line_type	 i;

	index = zplus ? zplus : (zminus ? zminus : _PATH_DEVNULL);

	switch (change) {
	case FSL_CKOUT_CHANGE_MERGE_ADD:
	case FSL_CKOUT_CHANGE_INTEGRATE_ADD:
	case FSL_CKOUT_CHANGE_ADDED:
		minus = _PATH_DEVNULL;
		plus = xplus;
		zminus = _PATH_DEVNULL;
		break;
	case FSL_CKOUT_CHANGE_MISSING:
	case FSL_CKOUT_CHANGE_REMOVED:
		minus = xminus;
		plus = _PATH_DEVNULL;
		zplus = _PATH_DEVNULL;
		break;
	case FSL_CKOUT_CHANGE_RENAMED:
	case FSL_CKOUT_CHANGE_MOD:
	default:
		minus = xminus;
		plus = xplus;
		break;
	}

	zminus = zminus ? zminus : zplus;

	if FLAG_CHK(s->diff_flags, FNC_DIFF_INVERT) {
		const char *tmp = minus;

		minus = plus;
		plus = tmp;
		tmp = zminus;
		zminus = zplus;
		zplus = tmp;
	}

	if (s->buf.used) {
		/* add a new line between files in the diff for readability */
		rc = add_line_type(&s->dlines, &s->ndlines, LINE_BLANK);
		if (rc)
			return rc;
		if (buf_putc(&s->buf, '\n') == -1)
			return RC_LIBF(s->buf.errCode, "buf_putc");
	}

	for (c = 1, i = LINE_DIFF_INDEX; i < LINE_DIFF_EDIT; ++c) {
		/* add INDEX, HEADER, META x2, MINUS, PLUS line types */
		rc = add_line_type(&s->dlines, &s->ndlines, i);
		if (rc)
			return rc;
		i += (c > 3 || c % 3) ? 1 : 0;
	}

	memset(eq, '=', sizeof(eq) - 1);  /* file index header */
	eq[sizeof(eq) - 1] = '\0';

	if (buf_printf(&s->buf, "Index: %s\n%s\n", index, eq) == -1)
		return RC_LIBF(s->buf.errCode, "buf_printf");
	if (buf_printf(&s->buf, "hash - %s\nhash + %s\n", minus, plus) == -1)
		return RC_LIBF(s->buf.errCode, "buf_printf");
	if (buf_printf(&s->buf, "--- %s\n+++ %s\n", zminus, zplus) == -1)
		return RC_LIBF(s->buf.errCode, "buf_printf");
	if (change == FSL_CKOUT_CHANGE_MISSING) {
		if (buf_printf(&s->buf, "\n%s\n", DIFF_FILE_MISSING) == -1)
			return RC_LIBF(s->buf.errCode, "buf_printf");
		rc = add_line_type(&s->dlines, &s->ndlines, LINE_DIFF_COMMENT);
		if (rc != FNC_RC_OK)
			return rc;
		rc = add_line_type(&s->dlines, &s->ndlines, LINE_BLANK);
		if (rc != FNC_RC_OK)
			return rc;
	}

	return FNC_RC_OK;
}

/*
 * The diff_file_artifact() counterpart that diffs actual files on disk rather
 * than file artifacts in the Fossil repository's blob table.
 *   buf      output buffer in which diff output will be appended
 *   bminus   blob containing content of the versioned file being diffed against
 *   zminus   filename of bminus
 *   xminus   hex UUID containing the SHA{1,3} hash of the file named zminus
 *   abspath  absolute path to the file on disk being diffed
 *   change   enum denoting the versioning change of the file
 * diff_flags, context, and sbs are the same parameters as diff_file_artifact()
 */
static int
diff_file(struct fnc_diff_view_state *s, fsl_buffer *bminus,
    const char *zminus, const char *zplus, fsl_uuid_cstr xminus,
    const char *abspath, const fsl_ckout_change_e change, int islink)
{
	struct fsl_cx			*f;
	struct fsl_buffer		 bplus = fsl_buffer_empty;
	struct fsl_buffer		 xplus = fsl_buffer_empty;
	struct fnc_file_artifact	*ffa;
	size_t				 pathlen;
	int				 rc;
	bool				 allow_symlinks, symlink;

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	/*
	 * If it exists, read content of abspath to diff EXCEPT for the content
	 * of 'fossil rm FILE' files because they will either: (1) have the same
	 * content as the versioned file's blob in bminus or (2) have changes.
	 * As a result, the upcoming call to fnc_diff_text_to_buffer() _will_
	 * (1) produce an empty diff or (2) show the differences; neither are
	 * expected behaviour because the SCM has been instructed to remove the
	 * file; therefore, the diff should display the versioned file content
	 * as being entirely removed. With this check, fnc now contrasts the
	 * behaviour of fossil(1), which produces the abovementioned unexpected
	 * output described in (1) and (2).
	 */
	if (change != FSL_CKOUT_CHANGE_REMOVED &&
	    change != FSL_CKOUT_CHANGE_MISSING) {
		rc = fsl_ckout_file_content(f, false, abspath, &bplus);
		if (rc)
			goto end;
		/*
		 * To replicate fossil(1)'s behaviour—where a fossil rm'd file
		 * will either show as an unchanged or edited rather than a
		 * removed file with 'fossil diff -v' output—remove the above
		 * 'if (change != FSL_CKOUT_CHANGE_REMOVED)' from the else
		 * condition and uncomment the following three lines of code.
		 */
		/* if (change == FSL_CKOUT_CHANGE_REMOVED && */
		/*     !fsl_buffer_compare(bminus, &bplus)) */
		/*	fsl_buffer_clear(&bplus); */
	}

	switch (strlen(xminus)) {
	case FSL_STRLEN_K256:
		rc = fsl_sha3sum_buffer(&bplus, &xplus);
		break;
	case FSL_STRLEN_SHA1:
		rc = fsl_sha1sum_buffer(&bplus, &xplus);
		break;
	case (sizeof(_PATH_DEVNULL) - 1):
		switch (fsl_config_get_int32(f, FSL_CONFDB_REPO,
		    FSL_HPOLICY_AUTO, "hash-policy")) {
		case FSL_HPOLICY_SHA1:
			rc = fsl_sha1sum_buffer(&bplus, &xplus);
			break;
		case FSL_HPOLICY_AUTO:
		case FSL_HPOLICY_SHA3:
		case FSL_HPOLICY_SHA3_ONLY:
			rc = fsl_sha3sum_buffer(&bplus, &xplus);
			break;
		}
		break;
	default:
		rc = RC(FNC_RC_BAD_HASH, "%s", xminus);
		goto end;
	}
	if (rc)
		goto end;

	/*
	 * XXX Edge case where the current checkout has a changed file that is
	 * now the same as it is in another version and the checkout is diffed
	 * against said version. diff_checkout() declares the file 'changed'
	 * because fsl_ckout_changes_scan() picks it up, but because it's the
	 * same as the other version the diff will be empty. As such, don't
	 * draw the index header UNLESS the file has been renamed.
	 */
	if (strlen(xminus) == xplus.used &&
	    fsl_uuidcmp(xminus, fsl_buffer_cstr(&xplus)) == 0 &&
	    change != FSL_CKOUT_CHANGE_RENAMED)
		goto end;

	pathlen = zplus != NULL ? strlen(zplus) : sizeof(_PATH_DEVNULL) - 1;
	rc = alloc_file_artifact(&ffa, zplus,
	    change != FSL_CKOUT_CHANGE_REMOVED ? fsl_buffer_str(&xplus) : NULL,
	    &pathlen, zminus, change != FSL_CKOUT_CHANGE_ADDED ? xminus : NULL,
	    zminus != NULL ? strlen(zminus) : 0, 0);
	if (rc)
		goto end;
	rc = fsl_list_append(&s->selected_entry->changeset, ffa);
	if (rc) {
		rc = RC_LIBF(rc, "fsl_list_append");
		goto end;
	}
	s->selected_entry->maxpathlen = MAX(pathlen,
	    s->selected_entry->maxpathlen);

	rc = write_diff_meta(s, zminus, xminus, zplus, fsl_buffer_cstr(&xplus),
	    change);
	if (rc)
		goto end;

	symlink = fsl_is_symlink(abspath);
	allow_symlinks = fsl_cx_allows_symlinks(f, true);
	if (!islink != !(symlink && allow_symlinks)) {
		if (buf_write(&s->buf,
		    "\ncannot diff symlink with a regular file\n", 41) ==-1) {
			rc = RC_LIBF(s->buf.errCode, "buf_write");
			goto end;
		}
		rc = add_line_type(&s->dlines, &s->ndlines, LINE_DIFF_COMMENT);
		if (rc)
			goto end;
		rc = add_line_type(&s->dlines, &s->ndlines, LINE_BLANK);
		goto end;
	}
	if (symlink && allow_symlinks) {
		fsl_buffer_reuse(&bplus);
		rc = fnc_read_symlink(&bplus, abspath);
		if (rc)
			goto end;
	}

	rc = diff_buffer_from_state(s, bminus, &bplus, ffa);

end:
	fsl_buffer_clear(&bplus);
	fsl_buffer_clear(&xplus);
	return rc;
}

static int
fnc_read_symlink(struct fsl_buffer *buf, const char *path)
{
	char	target[PATH_MAX];
	ssize_t	targetlen;

	targetlen = readlink(path, target, sizeof(target));
	if (targetlen == -1)
		return RC_ERRNO("readlink: %s", path);

	/* readlink(2) does not NUL terminate */
	target[targetlen] = '\0';
	if ((buf_write(buf, target, targetlen)) == -1)
		return RC_LIBF(buf->errCode, "buf_write");

	return FNC_RC_OK;
}

#define	NEXT_TABSTOP(x)	(x = (x + TABSIZE) & ~(TABSIZE - 1))

static int
wrap(char **ret, const char *s, size_t slen, int width)
{
	const wchar_t	*ifs = L"\t ";	/* field separators to split words */
	wchar_t		*ws;
	const char	*m, *p, *g = NULL;
	char		*t;
	wchar_t		 wc;
	size_t		 wlen, plen = slen;
	int		 col, wcw, wsz, rc = FNC_RC_OK;

	*ret = NULL;

	if ((rc = mbs2ws(&ws, &wlen, s)) != 0)
		return rc;
	span_wline(&wcw, 0, ws, -1, 0);
	free(ws);
	if (wcw <= width) {
		if ((*ret = strdup(s)) == NULL)
			rc = RC_ERRNO("strdup");
		return rc;
	}

	p = s;
	col = 0;
	while (*p != '\0') {
		if (*p == '\r' || *p == '\n') {
			++p;	/* exclude line terminator from column width */
			continue;
		}
		if ((wsz = mbtowc(&wc, p, MB_CUR_MAX)) == -1) {
			++col;
			++p;
			continue;
		}
		if (*p == '\t')
			NEXT_TABSTOP(col);
		else  {
			wcw = wcwidth(wc);
			col += wcw == -1 ? 1 : wcw;
		}
		p += wsz;
		if (col > width) {
			/*
			 * Find the last field separator, or glyph if
			 * no separators exist, that fits within width.
			 */
			while (p != s && col != 0 &&
			    (col > width || wcschr(ifs, wc) == NULL)) {
				m = p;
				do {
					wsz = mbtowc(&wc, --m, MB_CUR_MAX);
				} while (wsz == -1);
				p -= wsz;
				wcw = wcwidth(wc);
				col -= wcw == -1 ? 1 : wcw;
				if (g == NULL && col <= width)
					g = p;
			}
			if (col == 0 || p == s) {
				p = g;		/* no IFS before width */
				wsz = 0;	/* no separator to consume */
			}
			slen = p - s;
			plen -= slen;
			break;
		}
	}
	if (*p == '\0') {	/* multibyte string fits in width */
		*ret = strdup(s);
		if (*ret == NULL)
			rc = RC_ERRNO("strdup");
	} else {
		/*
		 * Multibyte string consumes more columns than width.
		 * Recursively wrap the split substring beginning at p.
		 */
		rc = wrap(ret, p + wsz, plen - wsz, width);
		if (rc != 0)
			return rc;
		t = fsl_mprintf("%.*s\n%s", slen, s, *ret);
		if (t == NULL)
			rc = RC_ERRNO("fsl_mprintf");
		free(*ret);
		*ret = t;
	}
	return rc;
}

static int
buf_wrap(struct fsl_buffer *dst, struct fsl_buffer *src, size_t width)
{
	char		*wrapped, *line = NULL;
	ssize_t		 len;
	size_t		 sz = 0;
	int		 rc = FNC_RC_OK;

	while ((len = fsl_buffer_getline(&line, &sz, src)) != -1) {
		rc = wrap(&wrapped, line, len, width);
		if (rc != 0) {
			free(wrapped);
			break;
		}
		if (wrapped == NULL)
			continue;

		if (buf_write(dst, wrapped, -1) == -1)
			rc = RC(dst->errCode, "buf_write");
		free(wrapped);
		if (rc != 0)
			break;
	}
	if (src->errCode != 0 && rc == 0)
		rc = RC_LIBF(src->errCode, "fsl_buffer_getline");
	free(line);
	return rc;
}

/*
 * Parse the deck of wiki commits to present a 'fossil ui' equivalent
 * of the corresponding artifact when selected from the timeline.
 */
static int
diff_wiki(struct fnc_diff_view_state *s)
{
	struct fsl_cx		*f;
	struct fsl_buffer	 wiki = fsl_buffer_empty;
	struct fsl_buffer	 pwiki = fsl_buffer_empty;
	struct fsl_deck		 d = fsl_deck_empty;
	size_t			 idx;
	fsl_id_t		 prid = 0;
	int			 rc;

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	fsl_deck_init(f, &d, FSL_SATYPE_ANY);
	rc = fsl_deck_load_rid(f, &d, s->selected_entry->rid, FSL_SATYPE_ANY);
	if (rc) {
		rc = RC(FNC_RC_NO_RID, "fsl_deck_load_rid: %d",
		    s->selected_entry->rid);
		goto end;
	}

	/*
	 * XXX Do we want to populate s->dlines with each diff line type
	 * (i.e., add_line_type(...) when appending lines to the buffer?
	 */
	if (d.type == FSL_SATYPE_TICKET) {
		/*
		 * Present tickets as a series of "field: value" tuples
		 * like the 'fossil ui' /info/UUID view in the browser.
		 */
		for (idx = 0; idx < d.J.used; ++idx) {
			struct fsl_card_J	*ticket = d.J.list[idx];
			bool			 icom;

			icom = strncmp(ticket->field, "icom", 4) == 0;
			if (buf_printf(&s->buf, "%lu. %s:%s%s%c\n",
			    (unsigned long)idx + 1, ticket->field,
			    icom ? "\n\n" : " ", ticket->value,
			    icom ? '\n' : ' ') == -1) {
				rc = RC_LIBF(s->buf.errCode, "buf_printf");
				goto end;
			}
		}
		goto end;
	}

	if (d.type == FSL_SATYPE_CONTROL) {
		/*
		 * Present tag artifacts as a series of "Tag N. [TYPE]"
		 * entries with tag content following on a new line.
		 */
		for (idx = 0; idx < d.T.used; ++idx) {
			struct fsl_card_T *ctl = d.T.list[idx];

			if (buf_printf(&s->buf, "Tag %lu ",
			    (unsigned long)idx + 1) == -1) {
				rc = RC_LIBF(s->buf.errCode, "buf_printf");
				goto end;
			}

			switch (ctl->type) {
			case FSL_TAGTYPE_CANCEL:
				if (buf_write(&s->buf, "[CANCEL]", 8) == -1)
					return RC_LIBF(s->buf.errCode,
					    "buf_write");
				break;
			case FSL_TAGTYPE_ADD:
				if (buf_write(&s->buf, "[ADD]", 5) == -1)
					return RC_LIBF(s->buf.errCode,
					    "buf_write");
				break;
			case FSL_TAGTYPE_PROPAGATING:
				if (buf_write(&s->buf, "[PROPAGATE]", 11) == -1)
					return RC_LIBF(s->buf.errCode,
					    "buf_write");
				break;
			case FSL_TAGTYPE_INVALID:
				rc = RC(FNC_RC_RANGE, "tag: %d", ctl->type);
				goto end;
			}

			if (ctl->uuid != NULL) {
				if (buf_printf(&s->buf,
				    "\ncheckin %s", ctl->uuid) == -1) {
					rc = RC_LIBF(s->buf.errCode,
					    "buf_printf");
					goto end;
				}
			}
			if (buf_printf(&s->buf, "\n%s", ctl->name) == -1) {
				rc = RC_LIBF(s->buf.errCode, "buf_printf");
				goto end;
			}
			if (strcmp(ctl->name, "branch") == 0 &&
			    ctl->value != NULL) {
				s->selected_entry->branch = strdup(ctl->value);
				if (s->selected_entry->branch == NULL) {
					rc = RC_ERRNO("strdup");
					goto end;
				}
			}
			if (ctl->value != NULL) {
				if (buf_printf(&s->buf, ":%c%s",
				    strncmp(ctl->name, "comment", 7) == 0 ?
				    '\n' : ' ', ctl->value) == -1) {
					rc = RC_LIBF(s->buf.errCode,
					    "buf_printf");
					goto end;
				}
			}
			if (buf_write(&s->buf, "\n\n", 2) == -1) {
				rc = RC_LIBF(s->buf.errCode, "buf_write");
				goto end;
			}
		}
		goto end;
	}

	/*
	 * The artifact is neither a ticket nor tag so
	 * assume it's a wiki, technote, or forum post.
	 */
	if (buf_write(&wiki, d.W.mem, d.W.used) == -1) {
		rc = RC_LIBF(wiki.errCode, "buf_write");
		goto end;
	}

	if (s->selected_entry->puuid == NULL) {
		if (d.P.used == 0) {
			/*
			 * It is a newly added artifact with no parent
			 * to diff against so append its entire contents.
			 */
			if (s->wrap)
				rc = buf_wrap(&s->buf, &wiki, *s->ncols);
			else {
				rc = fsl_buffer_copy(&s->buf, &wiki);
				if (rc)
					rc = RC_ERRNO("fsl_buffer_copy");
			}
			goto end;
		}
		s->selected_entry->puuid = strdup(d.P.list[0]);
		if (s->selected_entry->puuid == NULL) {
			rc = RC_ERRNO("strdup");
			goto end;
		}
	}

	/* diff the artifact against its parent */
	rc = symtorid(&prid, s->selected_entry->puuid, FSL_SATYPE_ANY);
	if (rc != 0)
		goto end;

	fsl_deck_clean(&d);
	rc = fsl_deck_load_rid(f, &d, prid, FSL_SATYPE_ANY);
	if (rc) {
		rc = RC(FNC_RC_NO_RID, "fsl_deck_load_rid: %d", prid);
		goto end;
	}
	if (buf_write(&pwiki, d.W.mem, d.W.used) == -1) {
		rc = RC_LIBF(pwiki.errCode, "buf_write");
		goto end;
	}

	rc = diff_buffer_from_state(s, &pwiki, &wiki, NULL);
	if (rc != 0)
		goto end;

	if (d.type == FSL_SATYPE_TECHNOTE) {
		/* append the technote's full content after its diff */
		if (buf_printf(&s->buf, "\n---\n\n%s", wiki.mem) == -1)
			rc = RC_LIBF(s->buf.errCode, "buf_printf");
	}

end:
	fsl_buffer_clear(&wiki);
	fsl_buffer_clear(&pwiki);
	fsl_deck_finalize(&d);
	return rc;
}

/*
 * Compute the differences between two repository file artifacts to produce the
 * set of changes necessary to convert one into the other.
 *   buf         output buffer in which diff output will be appended
 *   vid1        repo record id of the version from which artifact a belongs
 *   a           file artifact being diffed against
 *   vid2        repo record id of the version from which artifact b belongs
 *   b           file artifact being diffed
 *   change      enum denoting the versioning change of the file
 *   diff_flags  bitwise flags to control the diff
 *   context     the number of context lines to surround changes
 *   sbs	 number of columns in which to display each side-by-side diff
 */
static int
diff_file_artifact(struct fnc_diff_view_state *s, fsl_id_t vid1,
    const fsl_card_F *a, const fsl_card_F *b, const fsl_ckout_change_e change)
{
	struct fsl_cx			*f;
	struct fsl_stmt			 stmt = fsl_stmt_empty;
	struct fsl_buffer		 fbuf1 = fsl_buffer_empty;
	struct fsl_buffer		 fbuf2 = fsl_buffer_empty;
	struct fnc_file_artifact	*ffa;
	char				*zminus0 = NULL, *zplus0 = NULL;
	const char			*zplus = NULL, *zminus = NULL;
	char				*xplus0 = NULL, *xminus0 = NULL;
	char				*xplus = NULL, *xminus = NULL;
	int32_t				 vid2 = s->selected_entry->rid;
	int				 finalize_rc, rc;

	assert(vid1 != vid2);
	assert(vid2 > 0 &&
	    "local checkout should be diffed with diff_checkout()");

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	if (a != NULL) {
		rc = fsl_card_F_content(f, a, &fbuf1);
		if (rc)
			goto end;
		zminus = a->name;
		xminus = a->uuid;
	} else if (s->selected_entry->diff_type == FNC_DIFF_BLOB) {
		rc = fsl_cx_prepare(f, &stmt,
		    "SELECT name FROM filename, mlink "
		    "WHERE filename.fnid=mlink.fnid AND mlink.fid = %d", vid1);
		if (rc != 0) {
			rc = RC_LIBF(rc, "fsl_cx_prepare: %d", vid1);
			goto end;
		}
		rc = fsl_stmt_step(&stmt);
		if (rc == FSL_RC_STEP_ROW) {
			zminus0 = strdup(fsl_stmt_g_text(&stmt, 0, NULL));
			if (zminus0 == NULL) {
				rc = RC_ERRNO("strdup");
				goto end;
			}
			zminus = zminus0;
		} else if (rc != FNC_RC_OK && rc != FSL_RC_STEP_DONE) {
			rc = RC_LIBF(rc, "fsl_stmt_step");
			goto end;
		}

		xminus0 = fsl_rid_to_uuid(f, vid1);
		if (xminus0 == NULL) {
			rc = RC_LIBF(rc, "fsl_rid_to_uuid");
			goto end;
		}
		xminus = xminus0;

		rc = fsl_stmt_finalize(&stmt);
		if (rc != FNC_RC_OK) {
			rc = RC_LIBF(rc, "fsl_stmt_finalize");
			goto end;
		}

		rc = fsl_content_get(f, vid1, &fbuf1);
		if (rc) {
			rc = RC(rc, "fsl_content_get: %d", vid1);
			goto end;
		}
	}
	if (b != NULL) {
		rc = fsl_card_F_content(f, b, &fbuf2);
		if (rc)
			goto end;
		zplus = b->name;
		xplus = b->uuid;
	} else if (s->selected_entry->diff_type == FNC_DIFF_BLOB) {
		size_t pathlen;

		rc = fsl_cx_prepare(f, &stmt,
		    "SELECT name FROM filename, mlink "
		    "WHERE filename.fnid=mlink.fnid AND mlink.fid = %d", vid2);
		if (rc != 0) {
			rc = RC_LIBF(rc, "fsl_cx_prepare: %d", vid2);
			goto end;
		}
		rc = fsl_stmt_step(&stmt);
		if (rc == FSL_RC_STEP_ROW) {
			rc = FNC_RC_OK;
			zplus0 = strdup(fsl_stmt_g_text(&stmt, 0, NULL));
			if (zplus0 == NULL) {
				rc = RC_ERRNO("strdup");
				goto end;
			}
			zplus = zplus0;
		} else if (rc != FNC_RC_OK && rc != FSL_RC_STEP_DONE) {
			rc = RC_LIBF(rc, "fsl_stmt_step");
			goto end;
		}

		xplus0 = fsl_rid_to_uuid(f, vid2);
		if (xplus0 == NULL) {
			rc = RC_LIBF(rc, "fsl_rid_to_uuid");
			goto end;
		}
		xplus = xplus0;

		rc = fsl_stmt_finalize(&stmt);
		if (rc != FNC_RC_OK) {
			rc = RC_LIBF(rc, "fsl_stmt_finalize");
			goto end;
		}

		rc = fsl_content_get(f, vid2, &fbuf2);
		if (rc) {
			rc = RC(rc, "fsl_content_get: %d", vid2);
			goto end;
		}

		pathlen = zplus != NULL ?
		    strlen(zplus) : sizeof(_PATH_DEVNULL) - 1;
		rc = alloc_file_artifact(&ffa, zplus,
		    change != FSL_CKOUT_CHANGE_REMOVED ? xplus : NULL,
		    &pathlen, zminus,
		    change != FSL_CKOUT_CHANGE_ADDED ? xminus : NULL,
		    zminus != NULL ? strlen(zminus) : 0, 0);
		if (rc)
			goto end;
		rc = fsl_list_append(&s->selected_entry->changeset, ffa);
		if (rc) {
			rc = RC_LIBF(rc, "fsl_list_append");
			goto end;
		}
		s->selected_entry->maxpathlen = MAX(pathlen,
		    s->selected_entry->maxpathlen);
	}

	rc = write_diff_meta(s, zminus, xminus, zplus, xplus, change);
	if (rc)
		goto end;

	rc = diff_buffer_from_state(s, &fbuf1, &fbuf2, ffa);

end:
	free(xplus0);
	free(zplus0);
	free(xminus0);
	free(zminus0);
	fsl_buffer_clear(&fbuf1);
	fsl_buffer_clear(&fbuf2);
	finalize_rc = fsl_stmt_finalize(&stmt);
	if (finalize_rc != 0 && rc == 0)
		rc = RC_LIBF(finalize_rc, "fsl_stmt_finalize");
	return rc;
}

static inline uint64_t
diffstat_from_triples(int *cdi)
{
	uint64_t	 add = 0, rm = 0;
	int		*pr = &cdi[1], *pa = &cdi[2];

	for (; *cdi != 0 || *pr != 0 || *pa != 0; cdi += 3, pr += 3, pa += 3) {
		add += *pa;
		rm += *pr;
	}

	return add |= rm << 32;
}

static int
diff_buffer_from_state(struct fnc_diff_view_state *s, struct fsl_buffer *lhs,
    struct fsl_buffer *rhs, struct fnc_file_artifact *art)
{
	uint64_t	diffstat = 0;
	int		rc;

	if (s->wrap) {
		struct fsl_buffer t = fsl_buffer_empty;

		rc = buf_wrap(&t, lhs, *s->ncols - 1);	/* prepended '+/-/ ' */
		if (rc != 0) {
			fsl_buffer_clear(&t);
			return rc;
		}
		fsl_buffer_swap_free(lhs, &t, 1);

		rc = buf_wrap(&t, rhs, *s->ncols - 1);	/* prepended '+/-/ ' */
		if (rc != 0) {
			fsl_buffer_clear(&t);
			return rc;
		}
		fsl_buffer_swap_free(rhs, &t, 1);
	}

	if (!FLAG_CHK(s->diff_flags, FNC_DIFF_BRIEF) &&
	    (FLAG_CHK(s->diff_flags, FNC_DIFF_VERBOSE) ||
	    (lhs != NULL && rhs != NULL && lhs->used != 0 && rhs->used != 0)))
		rc = fnc_diff_text_to_buffer(&s->buf, lhs, rhs, &s->dlines,
		    &s->ndlines, art != NULL ? &diffstat : NULL,
		    s->context, s->sbs, s->diff_flags);
	else {
		int *triples = NULL;

		rc = fnc_diff_text_raw(&triples, lhs, rhs, s->diff_flags);
		if (triples != NULL) {
			diffstat = diffstat_from_triples(triples);
			free(triples);
		}
	}
	if (rc != FNC_RC_OK) {
		if (rc != FNC_RC_BREAK && rc != FSL_RC_DIFF_BINARY)
			return rc;
		RC_RESET;
		if (buf_printf(&s->buf, "\n%s\n", rc == FNC_RC_BREAK ?
		    DIFF_TOO_MANY_CHANGES : DIFF_FILE_BINARY) == -1)
			return RC_LIBF(s->buf.errCode, "buf_printf");
		rc = add_line_type(&s->dlines, &s->ndlines, LINE_DIFF_COMMENT);
		if (rc != FNC_RC_OK)
			return rc;
		rc = add_line_type(&s->dlines, &s->ndlines, LINE_BLANK);
		if (rc != FNC_RC_OK)
			return rc;
	}
	if (art != NULL)
		return fnc_file_artifact_diffstat(art->fc,
		    s->selected_entry, diffstat);

	return FNC_RC_OK;
}

static int
fnc_file_artifact_diffstat(const fsl_card_F *cf, struct fnc_commit_artifact *c,
    uint64_t diffstat)
{
	fsl_list			*changeset = &c->changeset;
	struct fnc_file_artifact	*ffa = NULL;
	int				 i;

	if (changeset == NULL || changeset->used == 0)
		return FNC_RC_OK;

	/*
	 * XXX cf->name or cf->priorName should always exist in the changeset
	 * because the changeset is derived from the commit manifest and cf is
	 * in the changeset; however, I have noted an instance of a commit in
	 * the Fossil repository that produced a missing file artifact but the
	 * commit referenced is incorrect (cf. Fossil commit ea28708f F card
	 * www/encode2.gif). After resolving this, when fsl_list_index_of()
	 * fails (returns -1), the commented error should be returned instead.
	 */
	i = fsl_list_index_of(changeset, cf->name, fnc_file_artifact_cmp);
	if (i < 0)
		return FNC_RC_OK;
		/* return RC(FNC_RC_NO_PATH, "fsl_list_index_of: %s", */
		/*     cf->name); */

	ffa = changeset->list[i];
	encode_diffstat(&ffa->diffstat, diffstat);
	encode_diffstat_widths(&c->dswidths, ffa->diffstat);

	if (cf->priorName == NULL)
		return FNC_RC_OK;

	i = fsl_list_index_of(changeset, cf->priorName, fnc_file_artifact_cmp);
	if (i < 0)
		return FNC_RC_OK;
		/* return RC(FNC_RC_NO_PATH, "fsl_list_index_of: %s", */
		/*     cf->priorName); */

	ffa = changeset->list[i];
	encode_diffstat(&ffa->diffstat, diffstat);
	encode_diffstat_widths(&c->dswidths, ffa->diffstat);

	return FNC_RC_OK;
}

static int
fnc_file_artifact_cmp(const void *target, const void *x)
{
	struct fnc_file_artifact	*ffa = (struct fnc_file_artifact *)x;
	const char			*name = (const char *)target;

	if (ffa->fc->name == NULL) {
		if (name != NULL)
			return -1;
		return 0;
	} else if (name == NULL)
		return 1;

	if (strcmp(ffa->fc->name, name) == 0)
		return 0;

	if (ffa->fc->priorName == NULL)
		return -1;

	return strcmp(ffa->fc->priorName, name);
}

/*
 * Encode the number of digits in the number of added, total changed, and
 * removed lines in the low, middle, and high 8 bits of encoded, respectively.
 */
static void
encode_diffstat_widths(uint32_t *encoded, uint64_t diffstat)
{

	uint32_t	nadded, nremoved, ntotal;
	uint16_t	current_add_digits = *encoded & 0xff;
	uint16_t	current_rm_digits = *encoded >> 24;
	uint16_t	current_total_digits = *encoded >> 16 & 0xff;
	uint8_t		add_digits = 0, rm_digits = 0, total_digits = 0;

	nadded = diffstat & 0xffffffff;
	nremoved = diffstat >> 32;
	ntotal = nadded + nremoved;

	ndigits(add_digits, nadded);
	ndigits(rm_digits, nremoved);
	ndigits(total_digits, ntotal);

	*encoded = MAX(current_add_digits, add_digits);
	*encoded |= (uint8_t)MAX(current_rm_digits, rm_digits) << 24;
	*encoded |= (uint8_t)MAX(current_total_digits, total_digits) << 16;
}

/*
 * If *encoded already contains added/removed line totals, increment with
 * added/removed totals in diffstat and reencode the new totals placing the
 * incremented added and removed line totals in the low 32 and high 32 bits,
 * respectively. Otherwise assign the already encoded diffstat to *encoded.
 */
static void
encode_diffstat(uint64_t *encoded, uint64_t diffstat)
{
	uint64_t ecx = *encoded;

	if (ecx == 0)
		ecx = diffstat;
	else {
		if (diffstat >> 32 && ecx & 0xffffffff)
			ecx |= (uint64_t)(diffstat >> 32) << 32;
		else
			ecx |= diffstat;
	}

	*encoded = ecx;
}

static int
show_diff_view(struct fnc_view *view)
{
	struct fnc_diff_view_state	*s = &view->state.diff;
	regmatch_t			*regmatch = &view->regmatch;
	struct fnc_colour		*c = NULL;
	wchar_t				*wline;
	char				*line = NULL;
	size_t				 linesz = 0;
	ssize_t				 linelen;
	off_t				 line_offset;
	attr_t				 rx = 0;
	int				 col, wlen, max_lines = view->nlines;
	int				 nlines = s->nlines;
	int				 nprinted = 0, rc;
	bool				 selected;

	s->lineno = s->first_line_onscreen - 1;
	line_offset = s->line_offsets[s->lineno];
	fsl_buffer_seek(&s->buf, line_offset, FSL_BUFFER_SEEK_SET);

	werase(view->window);

	rc = write_diff_headln(view, s, NULL);
	if (rc)
		return rc;

	if (--max_lines < 1)
		return rc;

	s->eof = false;
	view->pos.maxx = 0;
	while (max_lines > 0 && nprinted < max_lines) {
		col = wlen = 0;
		linelen = fsl_buffer_getline(&line, &linesz, &s->buf);
		if (linelen == -1) {
			if (s->buf.errCode == 0) {
				s->eof = true;
				break;
			}
			free(line);
			RC_LIBF(s->buf.errCode, "fsl_buffer_getline");
			return rc;
		}

		if (++s->lineno < s->first_line_onscreen)
			continue;
		if (s->gtl) {
			rc = gotoline(view, &s->lineno, &nprinted);
			if (rc != 0) {
				free(line);
				return rc;
			}
			if (s->gtl != 0)
				continue;
		}

		/* set maxx to longest line on the page */
		rc = formatln(&wline, &wlen, NULL, line, 0, INT_MAX, 0,
		    view->pos.x > 0 ? true : false);
		if (rc) {
			free(line);
			return rc;
		}
		free(wline);
		wline = NULL;
		view->pos.maxx = MAX(view->pos.maxx, wlen);

		rx = 0;
		if ((selected = nprinted == s->selected_line - 1))
			rx = fnc__highlight;
		if (s->showln)
			col = draw_lineno(view, nlines, s->lineno, rx);

		if (s->diff_mode == DIFF_MODE_STASH && nprinted < 8 &&
		    s->dlines[s->lineno] == LINE_DIFF_HUNK)
			rx = fnc__highlight;  /* highlight current hunk to stash */

		if (view->colour && s->ndlines > 0)
			c = get_colour(&s->colours,
			    s->dlines[MIN(s->ndlines - 1, (size_t)s->lineno)]);
		if (c && !(selected && s->sline == SLINE_MONO))
			rx |= COLOR_PAIR(c->scheme);
		if (c || selected)
			wattron(view->window, rx);

		if (s->first_line_onscreen + nprinted == s->matched_line &&
		    regmatch->rm_so >= 0 && regmatch->rm_so < regmatch->rm_eo) {
			rc = draw_matched_line(&wlen, line, view->pos.x,
			    view->ncols - col, 0, view->window, regmatch, rx);
			if (rc) {
				free(line);
				return rc;
			}
		} else {
			int skip;

			rc = formatln(&wline, &wlen, &skip, line,
			    view->pos.x, view->ncols - col, col,
			    view->pos.x > 0 ? true : false);
			if (rc) {
				free(line);
				return rc;
			}
			waddwstr(view->window, &wline[skip]);
			free(wline);
			wline = NULL;
		}
		col += wlen;

		while (col++ < view->ncols)
			waddch(view->window, ' ');

		if (c || selected)
			wattroff(view->window, rx);
		if (++nprinted == 1)
			s->first_line_onscreen = s->lineno;
	}
	free(line);
	if (nprinted >= 1)
		s->last_line_onscreen = s->first_line_onscreen + (nprinted - 1);
	else
		s->last_line_onscreen = s->first_line_onscreen;

	if (s->eof) {
		while (nprinted++ < view->nlines)
			waddch(view->window, '\n');

		wstandout(view->window);
		waddstr(view->window, "(END)");
		wstandend(view->window);
	}

	drawborder(view);
	return FNC_RC_OK;
}

/*
 * If view is non NULL, draw the diff headline to view->window. If view is not
 * set, this procedure assumes file f is a valid FILE handle to which the diff
 * headline will instead be written. The headline takes the form:
 *	$index diff $id1 $id2			$pct
 * Where $index is the current line and total lines in the diff denoted as
 * [n/N], and $pct is $index expressed as a percent; both fields are elided
 * when writing to file f. $id1 and $id2 are the first 40 bytes of the SHA
 * hash corresponding to the diffed commits, except when the diff is of the
 * local work tree, in which case $id2 is the path to the root of the tree.
 */
static int
write_diff_headln(struct fnc_view *view, struct fnc_diff_view_state *s,
    FILE *f)
{
	wchar_t		*wline;
	const char	*id1, *id2;
	char		*hdr;
	uint64_t	 len = 41;
	int		 limit, ln, n, rc;
	bool		 wt = s->selected_entry->diff_type == FNC_DIFF_CKOUT;

	/* some diffs (e.g., technote, tag) have no parent hash to display */
	id1 = s->id1 != NULL ? s->id1 : _PATH_DEVNULL;

	/* display work tree path (trim trailing /) if diffing the work tree */
	if (wt || (s->id2 != NULL && strcmp(id1, s->id2) == 0))
		id2 = fsl_cx_ckout_dir_name(fcli_cx(), &len);
	else if (s->id2 != NULL)
		id2 = s->id2;
	else
		id2 = _PATH_DEVNULL;

	/* if -o was specified, format headline without the [n/N] line index */
	if (view != NULL) {
		limit = view->ncols;
		ln = s->gtl ? s->gtl : s->lineno + s->selected_line;
		hdr = fsl_mprintf("[%d/%d] diff %.40s %.*s", ln, s->nlines,
		    id1, len - 1, id2);
	} else {
		limit = FSL_STRLEN_K256 + PATH_MAX + 8;
		hdr = fsl_mprintf("diff %.40s %.*s", id1, len - 1, id2);
	}
	if (hdr == NULL)
		return RC_ERRNO("fsl_mprintf");

	rc = formatln(&wline, &n, NULL, hdr, 0, limit, 0, false);
	if (rc)
		goto end;

	if (view != NULL) {
		double	percent = 100.00 * ln / s->nlines;
		char	pct[MAX_PCT_LEN];
		int	col, npct;
		attr_t	rx = 0;

		npct = snprintf(pct, MAX_PCT_LEN, "%.*lf%%",
		    percent > 99.99 ? 0 : 2, percent);
		if (npct < 0 || npct >= MAX_PCT_LEN) {
			rc = RC(FNC_RC_RANGE, "snprintf");
			goto end;
		}

		if (view_is_shared(view) || view->active)
			rx = fnc__highlight;
		wattron(view->window, rx);
		waddwstr(view->window, wline);
		col = n;
		while (col++ < view->ncols)
			waddch(view->window, ' ');
		if (n < view->ncols - npct)
			mvwaddstr(view->window, 0, view->ncols - npct, pct);
		wattroff(view->window, rx);
	} else if (fprintf(f, "%ls\n", wline) < 0) {
		rc = RC_FERROR(f, FNC_RC_IO, "fprintf");
		goto end;
	}

end:
	free(hdr);
	free(wline);
	return rc;
}

static bool
view_is_fullscreen(struct fnc_view *view)
{
	return view->nlines == LINES && view->ncols == COLS;
}

static bool
view_is_shared(struct fnc_view *view)
{
	if (view_is_parent(view)) {
		if (view->child == NULL || view->child->active ||
		    !view_is_split(view->child))
			return false;
	} else if (!view_is_split(view))
		return false;

	return view->active;
}

static bool
view_is_parent(struct fnc_view *view)
{
	return view->parent == NULL;
}

static bool
view_is_top_split(struct fnc_view *view)
{
	return view->mode == VIEW_SPLIT_HRZN && view->child &&
	    view_is_split(view->child);
}

static bool
view_is_split(struct fnc_view *view)
{
	return view->begin_y > 0 || view->begin_x > 0;
}

static int *
view_width(int max)
{
	struct winsize	w;
	static int	ncols;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == -1) {
		const char *c, *e;

		c = getenv("COLUMNS");
		if (c != NULL)
			ncols = strtonum(c, 0, UINT32_MAX, &e);
		if (c == NULL || e != NULL)
			ncols = 80;
	} else
		ncols = w.ws_col;

	ncols = max == 0 ? ncols : MIN(ncols, max);
	return &ncols;
}

static void
updatescreen(WINDOW *win, bool panel, bool update)
{
#ifdef __linux__
	wnoutrefresh(win);
	(void)panel;
#else
	(void)win;
	if (panel)
		update_panels();
#endif
	if (update)
		doupdate();
}

/*
 * Split line into three segments: leading, matching, and trailing substrings.
 * Draw each segment to window. If skip is nonzero, check if skip columns
 * consumes any part of each segment and only draw the visible characters.
 */
static int
draw_matched_line(int *wtotal, const char *line, int skip, int wlimit,
    int xpos, WINDOW *window, regmatch_t *regmatch, attr_t attr)
{
	wchar_t	*wline = NULL;
	char	*exstr, *seg;
	int	 rc, rme, rms, wskip;
	int	 nvis, width0, width1;

	*wtotal = 0;

	rms = regmatch->rm_so;
	rme = regmatch->rm_eo;

	rc = expand_tab(&exstr, NULL, line);
	if (rc)
		return rc;

	seg = strndup(exstr, rms);	/* leading segment */
	if (seg == NULL) {
		rc = RC_ERRNO("strndup");
		goto end;
	}

	/* get column width of the entire leading segment */
	rc = formatln(&wline, &width0, NULL, seg, 0, wlimit, xpos, true);
	if (rc)
		goto end;
	if (width0 > skip) {
		/* draw (visible part of) the leading segment */
		free(wline);
		rc = formatln(&wline, &nvis, &wskip, seg, skip, wlimit,
		    xpos, true);
		if (rc)
			goto end;
		waddwstr(window, &wline[wskip]);
		wlimit -= nvis;
		*wtotal += nvis;
	}
	if (wlimit <= 0)
		goto end;

	free(seg);
	seg = strndup(exstr + rms, rme - rms);	/* matching segment */
	if (seg == NULL) {
		rc = RC_ERRNO("strndup");
		goto end;
	}

	/* get column width of the entire matching segment */
	nvis = 0;
	free(wline);
	rc = formatln(&wline, &width1, NULL, seg, 0, wlimit, xpos, true);
	if (rc)
		goto end;
	if (skip <= width0) {
		/* the entire matching segment is visible */
		wskip = 0;
		free(wline);
		rc = formatln(&wline, &nvis, NULL, seg, 0, wlimit, xpos, true);
		if (rc)
			goto end;
	} else if (skip < width0 + width1) {
		/* only part of the matching segment is visible */
		free(wline);
		rc = formatln(&wline, &nvis, &wskip, seg, skip - width0,
		    wlimit, xpos, true);
		if (rc)
			goto end;
	}
	if (nvis > 0) {
		/* draw (visible part of) the matching segment */
		wattron(window,
		    COLOR_PAIR(FNC_COLOUR_HL_SEARCH) | fnc__highlight);
		waddwstr(window, &wline[wskip]);
		wattroff(window,
		    COLOR_PAIR(FNC_COLOUR_HL_SEARCH) | fnc__highlight);
		wlimit -= nvis;
		*wtotal += nvis;
	}
	if (wlimit <= 0)
		goto end;

	free(seg);
	seg = strdup(exstr + rme);	/* trailing segment */
	if (seg == NULL) {
		rc = RC_ERRNO("strdup");
		goto end;
	}

	nvis = 0;
	free(wline);
	if (skip <= width0 + width1) {
		/* the entire trailing segment is visible */
		wskip = 0;
		rc = formatln(&wline, &nvis, NULL, seg, 0, wlimit, xpos, true);
	} else {
		/* only part of the trailing segment may be visible */
		rc = formatln(&wline, &nvis, &wskip, seg,
		    skip - (width0 + width1), wlimit, xpos, true);
	}
	if (rc)
		goto end;
	if (nvis > 0) {
		/* draw (visible part of) the trailing segment */
		wattron(window, attr);
		waddwstr(window, &wline[wskip]);
		*wtotal += nvis;
	}

end:
	free(wline);
	free(exstr);
	free(seg);
	return rc;
}

static void
drawborder(struct fnc_view *view)
{
	const struct fnc_view	*view_above;
	char			*codeset = nl_langinfo(CODESET);
	PANEL			*panel;

	if (view->parent)
		drawborder(view->parent);

	panel = panel_above(view->panel);
	if (panel == NULL)
		return;

	view_above = panel_userptr(panel);
	if (view->mode == VIEW_SPLIT_HRZN)
		mvwhline(view->window, view_above->begin_y - 1,
		    view->begin_x, (strcmp(codeset, "UTF-8") == 0) ?
		    ACS_HLINE : '-', view->ncols);
	else
		mvwvline(view->window, view->begin_y,
		    view_above->begin_x - 1,
		    (strcmp(codeset, "UTF-8") == 0) ? ACS_VLINE : '|',
		    view->nlines);

	updatescreen(view->window, false, false);
}

static int
patchkeymap(struct fnc_view *view)
{
	struct fnc_diff_view_state	*s = &view->state.diff;
	FILE				*f;
	char				 base[PATH_MAX];
	char				*path;
	int				 i, rc;

	i = strlen(fnc__tmpdir);
	while (i > 0 && fnc__tmpdir[i - 1] == '/')
		--i;

	rc = snprintf(base, sizeof(base), "%.*s/fnc-%.8s-%.8s",
	    i, fnc__tmpdir, s->selected_entry->puuid != NULL ?
	    s->selected_entry->puuid : "void",
	    s->selected_entry->uuid);
	if (rc < 0 || (size_t)rc >= sizeof(base))
		return RC(rc < 0 ? FNC_RC_IO : FNC_RC_NO_SPACE);

	rc = fnc_open_tmpfile(&path, &f, base, ".diff");
	if (f == NULL)
		return RC_ERRNO("fopen: %s", path);

	rc = difftofile(f, s);
	if (rc != 0)
		goto end;

	if ((view->status = fsl_mprintf("diff written to %s", path)) == NULL)
		rc = RC_ERRNO("fsl_mprintf");

end:
	free(path);
	if (f != NULL && fclose(f) == EOF && rc == 0)
		rc = RC_ERRNO("fclose");
	return rc;
}

static int
diff_input_handler(struct fnc_view **new_view, struct fnc_view *view, int ch)
{
	struct fnc_diff_view_state	*s = &view->state.diff;
	char				*line = NULL;
	ssize_t				 linelen;
	size_t				 linesz = 0;
	int				 nlines, i = 0, rc = FNC_RC_OK;
	uint16_t			 nscroll = view->nlines - 2;
	bool				 down = false;

	nlines = s->nlines;
	s->lineno = s->first_line_onscreen - 1 + s->selected_line;

	switch (ch) {
	case '0':
		view->pos.x = 0;
		break;
	case '$':
		view->pos.x = MAX(view->pos.maxx - view->ncols / 2, 0);
		break;
	case KEY_RIGHT:
	case 'l':
		if (view->pos.x + view->ncols / 2 < view->pos.maxx)
			view->pos.x += 2;
		break;
	case KEY_LEFT:
	case 'h':
		view->pos.x -= MIN(view->pos.x, 2);
		break;
	case CTRL('p'):
		diff_prev_index(s, LINE_DIFF_INDEX);
		break;
	case CTRL('n'):
		diff_next_index(s, LINE_DIFF_INDEX);
		break;
	case '[':
		diff_prev_index(s, LINE_DIFF_HUNK);
		break;
	case ']':
		diff_next_index(s, LINE_DIFF_HUNK);
		break;
	case CTRL('e'):
		if (s->first_line_onscreen + s->selected_line == nlines + 2)
			break;
		if (s->selected_line < view->nlines - 1 && s->lineno != nlines)
			++s->selected_line;
		else if (s->last_line_onscreen <= nlines && !s->eof) {
			++s->first_line_onscreen;
			if (s->lineno == nlines)
				--s->selected_line;
		}
		break;
	case CTRL('y'):
		if (s->selected_line > 1)
			--s->selected_line;
		else if (s->selected_line == 1 && s->first_line_onscreen > 1)
			--s->first_line_onscreen;
		break;
	case KEY_DOWN:
	case 'j':
		if (!s->eof) {
			++s->first_line_onscreen;
			if (s->lineno == nlines)
				--s->selected_line;
		}
		break;
	case KEY_UP:
	case 'k':
		if (s->first_line_onscreen > 1)
			--s->first_line_onscreen;
		break;
	case CTRL('d'):
		nscroll >>= 1;
		/* FALL THROUGH */
	case KEY_NPAGE:
	case CTRL('f'):
	case ' ':
		if (s->eof && s->last_line_onscreen == nlines) {
			uint16_t move = nlines - s->lineno;

			s->selected_line += MIN(nscroll, move);
			break;
		}
		while (!s->eof && i++ < nscroll) {
			linelen = fsl_buffer_getline(&line, &linesz, &s->buf);
			++s->first_line_onscreen;
			if (linelen == -1) {
				if (s->buf.errCode != 0) {
					free(line);
					return RC_LIBF(s->buf.errCode,
					    "fsl_buffer_getline");
				}
				if (s->selected_line > nscroll)
					s->selected_line = view->nlines - 2;
				else
					s->selected_line = nscroll;
				s->eof = true;
				break;
			}
		}
		free(line);
		break;
	case CTRL('u'):
		nscroll >>= 1;
		/* FALL THROUGH */
	case KEY_PPAGE:
	case CTRL('b'):
		if (s->first_line_onscreen == 1) {
			uint16_t move = s->selected_line - 1;

			s->selected_line -= MIN(nscroll, move);
			break;
		}
		while (i++ < nscroll && s->first_line_onscreen > 1)
			--s->first_line_onscreen;
		break;
	case KEY_END:
	case 'G':
		if (nlines < view->nlines - 1) {
			s->selected_line = nlines;
			s->first_line_onscreen = 1;
		} else {
			s->selected_line = nscroll;
			s->first_line_onscreen = nlines - view->nlines + 3;
		}
		s->eof = true;
		break;
	case 'g':
		if (!fnc_home(view))
			break;
		/* FALL THROUGH */
	case KEY_HOME:
		s->selected_line = 1;
		s->first_line_onscreen = 1;
		break;
	case '@': {
		struct input input;

		memset(&input, 0, sizeof(input));
		input.data = (int []){1, nlines};
		input.prompt = "line: ";
		input.type = INPUT_NUMERIC;
		input.flags = SR_CLREOL;

		rc = fnc_prompt_input(view, &input);
		s->gtl = input.ret;
		break;
	}
	case '#':
		s->showln = !s->showln;
		break;
	case 'b':
		return view_request_new(new_view, view, FNC_VIEW_BRANCH);
	case 'p':
		return patchkeymap(view);
	case 'B':
	case 'C':
	case 'D':
	case 'i':
	case 'L':
	case 'P':
	case 'S':
	case 'v':
	case 'W':
	case 'w':
		if (ch == 'C' && COLORS)
			view->colour = !view->colour;
		/* bDiLpSvWw key maps don't apply to tag or ticket artifacts */
		if (*s->selected_entry->type == 't' &&
		    (s->selected_entry->type[1] == 'a' ||
		     s->selected_entry->type[1] == 'i'))
			break;
		else if (ch == 'B')
			FLAG_TOG(s->diff_flags, FNC_DIFF_BRIEF);
		else if (ch == 'D')
			FLAG_TOG(s->diff_flags, FNC_DIFF_STATMIN);
		else if (ch == 'i')
			FLAG_TOG(s->diff_flags, FNC_DIFF_INVERT);
		else if (ch == 'L')
			FLAG_TOG(s->diff_flags, FNC_DIFF_LINENO);
		else if (ch == 'P')
			FLAG_TOG(s->diff_flags, FNC_DIFF_PROTOTYPE);
		else if (ch == 'S')
			FLAG_TOG(s->diff_flags, FNC_DIFF_SIDEBYSIDE);
		else if (ch == 'v')
			FLAG_TOG(s->diff_flags, FNC_DIFF_VERBOSE);
		else if (ch == 'W')
			s->wrap = !s->wrap;
		else if (ch == 'w')
			FLAG_TOG(s->diff_flags, FNC_DIFF_IGNORE_ALLWS);
		rc = reset_diff_view(view, true);
		break;
	case '-':
	case '_':
		if (s->context > 0) {
			--s->context;
			rc = reset_diff_view(view, true);
		}
		break;
	case '+':
	case '=':
		if (s->context < MAX_DIFF_CTX) {
			++s->context;
			rc = reset_diff_view(view, true);
		}
		break;
	case CTRL('j'):
	case '>':
	case '.':
	case 'J':
		down = true;
		/* FALL THROUGH */
	case CTRL('k'):
	case '<':
	case ',':
	case 'K':
		if (view->parent == NULL)
			break;

		if (view->parent->vid == FNC_VIEW_TIMELINE) {
			struct fnc_tl_view_state	*ts;
			struct commit_entry		*prev;

			ts = &view->parent->state.timeline;
			prev = ts->selected_entry;

			rc = tl_input_handler(NULL, view->parent,
			    down ? KEY_DOWN : KEY_UP);
			if (rc)
				break;

			if (prev == ts->selected_entry)
				break;

			if (s->diff_mode == DIFF_MODE_NORMAL)
				s->diff_mode = DIFF_MODE_META;

			rc = set_selected_commit(s, ts->selected_entry);
			if (rc)
				break;
		} else if (view->parent->vid == FNC_VIEW_BLAME) {
			struct fnc_blame_view_state	*bs;
			const char			*id, *prev_id;

			bs = &view->parent->state.blame;
			prev_id = bs->selected_entry->uuid;

			rc = blame_input_handler(&view, view->parent,
			    down ? KEY_DOWN : KEY_UP);
			if (rc)
				break;

			id = get_selected_commit_id(bs->blame.lines,
			    bs->blame.nlines, bs->first_line_onscreen,
			    bs->selected_line);
			if (fsl_uuidcmp(id, prev_id) == 0)
				break;

			rc = blame_input_handler(&view, view->parent,
			    KEY_ENTER);
			if (rc)
				break;
		}
		s->selected_line = 1;
		rc = reset_diff_view(view, false);
		break;
	default:
		break;
	}

	return rc;
}

static int
difftofile(FILE *f, struct fnc_diff_view_state *s)
{
	int rc;

	rc = create_diff(s);
	if (rc)
		return rc;
	fsl_buffer_rewind(&s->buf);

	rc = write_diff_headln(NULL, s, f);
	if (rc)
		return rc;

	if (fsl_stream(fsl_input_f_buffer, &s->buf, fsl_output_f_FILE, f))
		return RC(FNC_RC_IO, "fsl_stream");

	return FNC_RC_OK;
}

static void
diff_prev_index(struct fnc_diff_view_state *s, enum line_type type)
{
	size_t start, i;

	i = start = s->first_line_onscreen - 1;

	while (s->dlines[i] != type) {
		if (i == 0)
			i = s->nlines - 1;
		if (--i == start)
			return; /* do nothing, requested type not in file */
	}

	s->selected_line = 1;
	s->first_line_onscreen = i;
}

static void
diff_next_index(struct fnc_diff_view_state *s, enum line_type type)
{
	size_t start, i;

	i = start = s->first_line_onscreen + 1;

	while (s->dlines[i] != type) {
		if (i == s->nlines - 1)
			i = 0;
		if (++i == start)
			return; /* do nothing, requested type not in file */
	}

	s->selected_line = 1;
	s->first_line_onscreen = i;
}

static int
f__stash_get(uint32_t stashid, bool pop)
{
	struct fsl_cx	*f;
	struct fsl_db	*db;
	struct fsl_stmt	 q;
	char		*ogpath = NULL, *path = NULL;
	fsl_id_t	 vid;
	int		 nadded, finalize_rc, rc;
	uint32_t	 nconflicts = 0;

	memset(&q, 0, sizeof(q));

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");
	if ((db = fsl_needs_ckout(f)) == NULL)
		return RC(FNC_RC_NO_CKOUT);

	vid = f->db.ckout.rid;

	rc = fsl_db_prepare(db, &q, "SELECT blob.rid, isRemoved, isExec,"
	    "  isLink, origname, newname, delta FROM stashfile, blob"
	    " WHERE stashid=%d AND blob.uuid=stashfile.hash UNION ALL"
	    "  SELECT 0, isRemoved, isExec, isLink, origname, newname, delta"
	    "  FROM stashfile WHERE stashid=%d AND stashfile.hash IS NULL",
	    stashid, stashid);
	if (rc != 0)
		return RC_DB(db, rc, "fsl_db_prepare");
	rc = fsl_db_exec_multi(db,
	    "CREATE TEMP TABLE sfile(pathname TEXT PRIMARY KEY %s)",
	    fsl_cx_filename_collation(f));
	if (rc) {
		rc = RC_LIBF(rc, "fsl_db_exec_multi");
		goto end;
	}

	while ((rc = fsl_stmt_step(&q)) == FSL_RC_STEP_ROW) {
		const char	*name, *ogname;
		int		 exec, link, removed, rid;

		rc = stash_get_row(&q, &ogname, &name, &rid, &removed, &exec,
		    &link);
		if (rc)
			goto end;

		ogpath = fsl_mprintf("%s%s", CKOUTDIR, ogname);
		if (ogpath == NULL) {
			rc = RC_ERRNO("fsl_mprintf");
			goto end;
		}
		path = fsl_mprintf("%s%s", CKOUTDIR, name);
		if (path == NULL) {
			rc = RC_ERRNO("fsl_mprintf");
			goto end;
		}

		if (!rid) {	/* new file */
			rc = fnc_stash_add_file(db, &q, name, path, exec);
			if (rc)
				goto end;
		} else if (removed) {
			rc = fnc_stash_rm_file(ogname);
			if (rc)
				goto end;
		} else if (fsl__ckout_safe_file_check(f, path)) {
			/* nop--ignore unsafe path */;
		} else {
			uint32_t nc;

			rc = fnc_stash_update_file(&q, rid, ogpath, path,
			    ogname, name, exec, link, &nc);
			if (rc)
				goto end;
			nconflicts += nc;
		}
		if (ogname != NULL && strcmp(ogname, name) != 0) {
			if (unlink(ogpath) == -1 && errno != ENOENT) {
				rc = RC_ERRNO("unlink: %s", ogpath);
				goto end;
			}
			rc = fsl_db_exec_multi(db,
			    "UPDATE vfile SET pathname='%q', origname='%q'"
			    " WHERE pathname='%q' %s AND vid=%d", name, ogname,
			    ogname, fsl_cx_filename_collation(f), vid);
			if (rc) {
				rc = RC_LIBF(rc, "fsl_db_exec_multi");
				goto end;
			}
		}
		free(path);
		path = NULL;
		free(ogpath);
		ogpath = NULL;
	}
	if (rc == FSL_RC_STEP_DONE)
		rc = FNC_RC_OK;
	else {
		if (fsl_db_err_get(db, NULL, NULL) != FNC_RC_OK)
			rc = RC_LIBF(fsl_cx_uplift_db_error(f, db),
			    "fsl_stmt_step");
		else
			rc = RC_LIBF(rc, "fsl_stmt_step");
		goto end;
	}

	rc = f__add_files_in_sfile(&nadded, vid);
	if (rc)
		goto end;
	if (nconflicts)
		f_out("\n>> total merge conflicts: %u\n", nconflicts);
	if (pop) {
		rc = fsl_db_exec_multi(db,
		    "DELETE FROM stash WHERE stashid=%d;"
		    "DELETE FROM stashfile WHERE stashid=%d;",
		    stashid, stashid);
		if (rc)
			rc = RC_LIBF(rc, "fsl_db_exec_multi");
	}

end:
	free(path);
	free(ogpath);
	finalize_rc = fsl_stmt_finalize(&q);
	if (finalize_rc != 0 && rc == 0)
		rc = RC_LIBF(finalize_rc, "fsl_stmt_finalize");
	return rc;
}

static int
stash_get_row(struct fsl_stmt *q, const char **ogname, const char **name,
    int *rid, int *removed, int *exec, int *link)
{
	int rc;

	*name = NULL;
	*ogname = NULL;

	rc  = fsl_stmt_get_int32(q, 0, rid);
	if (rc)
		return RC_LIBF(rc, "fsl_stmt_get_int32");
	rc = fsl_stmt_get_int32(q, 1, removed);
	if (rc)
		return RC_LIBF(rc, "fsl_stmt_get_int32");
	rc = fsl_stmt_get_int32(q, 2, exec);
	if (rc)
		return RC_LIBF(rc, "fsl_stmt_get_int32");
	rc = fsl_stmt_get_int32(q, 3, link);
	if (rc)
		return RC_LIBF(rc, "fsl_stmt_get_int32");
	rc = fsl_stmt_get_text(q, 4, ogname, NULL);
	if (rc)
		return RC_LIBF(rc, "fsl_stmt_get_text");
	rc = fsl_stmt_get_text(q, 5, name, NULL);
	if (rc)
		return RC_LIBF(rc, "fsl_stmt_get_text");

	return FNC_RC_OK;
}

static int
fnc_stash_add_file(struct fsl_db *db, struct fsl_stmt *q, const char *name,
    const char *path, int exec)
{
	struct fsl_buffer	 delta;
	const void		*blob;
	uint64_t		 len;
	int			 rc;

	memset(&delta, 0, sizeof(delta));

	rc = fsl_db_exec_multi(db,
	    "INSERT OR IGNORE INTO sfile(pathname) VALUES(%Q)", name);
	if (rc)
		return RC_LIBF(rc, "fsl_db_exec_multi");
	rc = fsl_stmt_get_blob(q, 6, &blob, &len);
	if (rc)
		return RC_LIBF(rc, "fsl_stmt_get_blob");

	fsl_buffer_external(&delta, blob, len);
	rc = fsl_buffer_to_filename(&delta, path);
	if (rc) {
		rc = RC_LIBF(rc, "fsl_buffer_to_filename: %s", path);
		goto end;
	}
	rc = fsl_file_exec_set(path, exec);
	if (rc)
		rc = RC_ERRNO("chmod: %s", path);

end:
	fsl_buffer_clear(&delta);
	return rc;
}

/*
 * XXX Unlike fossil(1), don't remove the file from disk because its removal
 * has not been committed, only stashed. Unmanage it, and let the user rm it.
 */
static int
fnc_stash_rm_file(const char *ogname)
{
	struct fsl_cx			*f;
	struct fsl_ckout_unmanage_opt	 opt;
	int				 rc;

	memset(&opt, 0, sizeof(opt));

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	opt.callback = stash_get_rm_cb;
	opt.filename = ogname;

	rc = fsl_ckout_unmanage(f, &opt);
	if (rc != 0)
		return RC_LIBF(rc, "fsl_ckout_unmanage");

	return FNC_RC_OK;
}

static int
fnc_stash_update_file(struct fsl_stmt *q, fsl_id_t rid, const char *ogpath,
    const char *path, const char *ogname, const char *name, int exec, int link,
    uint32_t *nconflicts)
{
	struct fsl_cx		*f;
	struct fsl_error	 err;
	struct fsl_buffer	 a, b, delta, disk, out;
	const void		*blob;
	uint64_t		 len;
	int			 newlink, rc;

	*nconflicts = 0;
	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	memset(&out, 0, sizeof(out));
	memset(&err, 0, sizeof(err));
	memset(&disk, 0, sizeof(disk));
	memset(&delta, 0, sizeof(delta));

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	newlink = fsl_is_symlink(ogpath);
	rc = fsl_stmt_get_blob(q, 6, &blob, &len);
	if (rc) {
		rc = RC_LIBF(rc, "fsl_stmt_get_blob");
		goto end;
	}

	fsl_buffer_external(&delta, blob, len);
	rc = fsl_buffer_fill_from_filename(&disk, ogpath);
	if (rc == FSL_RC_NOT_FOUND)
		rc = fsl_buffer_fill_from_filename(&disk, path);
	if (rc) {
		rc = RC_LIBF(rc, "fsl_buffer_fill_from_filename: %s", ogpath);
		goto end;
	}
	rc = fsl_content_get(f, rid, &a);
	if (rc) {
		rc = RC_LIBF(rc, "fsl_content_get: %d", rid);
		goto end;
	}
	if (a.mem == NULL) {
		/*
		 * XXX Empty tracked file.
		 * fsl_buffer_delta_apply::fsl_delta_apply2()
		 * requires a->mem to be initialised.
		 */
		rc = fsl_buffer_resize(&a, UINT64_C(0));
		if (rc) {
			rc = RC_ERRNO("fsl_buffer_resize");
			goto end;
		}
	}
	rc = fsl_buffer_delta_apply2(&a, &delta, &b, &err);
	if (rc) {
		if (err.code)
			rc = RC_LIBF(err.code, "%s", fsl_buffer_cstr(&err.msg));
		else
			rc = RC_LIBF(rc, "fsl_buffer_delta_apply2");
		goto end;
	}

	if (link == newlink && fsl_buffer_compare(&disk, &a) == 0) {
		/*
		 * The file in the work tree on disk is
		 * unmodified from its checked-out version
		 * and will be updated to the stashed version.
		 */
		if (link || newlink) {
			if (unlink(path) == -1 && errno != ENOENT) {
				rc = RC_ERRNO("unlink: %s", path);
				goto end;
			}
		}
		/*
		 * If path is a symlink and allow-symlinks is set, create a
		 * real symlink; else, create a file containing the link path.
		 */
		if (link) {
			bool dolink;

			dolink = fsl_config_get_bool(f, FSL_CONFDB_REPO, false,
			    "allow-symlinks");
			rc = fsl_symlink_create(fsl_buffer_cstr(&b), path,
			    dolink);
			if (rc) {
				rc = RC_ERRNO("symlink: %s", path);
				goto end;
			}
			printf("[@] %s  ->  %s\n", name, fsl_buffer_cstr(&b));
		} else {
			rc = fsl_buffer_to_filename(&b, path);
			if (rc) {
				rc = RC_LIBF(rc, "fsl_buffer_to_filename: %s",
				    path);
				goto end;
			}
		}
		rc = fsl_file_exec_set(path, exec);
		if (rc) {
			rc = RC_ERRNO("chmod: %s", name);
			goto end;
		}
		if (ogname != NULL && strcmp(ogname, name) != 0)
			printf("[>] %s  ->  %s\n", ogname, name); /* renamed */
		else
			printf("[~] %s\n", name);  /* file updated */
	} else {
		/*
		 * The modified work tree file on disk will be merged
		 * with its checked-out version and stashed version.
		 */
		if (link || newlink) {
			printf("[!] %s  ->  symlink not merged\n", name);
			goto end;
		}
		rc = fsl_buffer_merge3(&a, &disk, &b, &out, nconflicts);
		if (rc) {
			rc = RC_LIBF(rc, "fsl_buffer_merge3");
			goto end;
		}
		rc = fsl_buffer_to_filename(&out, path);
		if (rc) {
			rc = RC_LIBF(rc, "fsl_buffer_to_filename: %s", path);
			goto end;
		}
		rc = fsl_file_exec_set(path, exec);
		if (rc) {
			rc = RC_ERRNO("chmod: %s", path);
			goto end;
		}
		if (*nconflicts)
			printf("[!] %s  ->  %u merge conflict(s)\n",
			    name, *nconflicts);
		else if (ogname != NULL && strcmp(ogname, name) != 0)
			printf("[>] %s  ->  %s\n", ogname, name); /* renamed */
		else
			printf("[~] %s\n", name);  /* file merged */
	}

end:
	fsl_buffer_clear(&a);
	fsl_buffer_clear(&b);
	fsl_buffer_clear(&out);
	fsl_buffer_clear(&disk);
	return rc;
}

static int
f__add_files_in_sfile(int *nadded, int vid)
{
	struct fsl_cx		*f;
	struct fsl_db		*db;
	struct fsl_stmt		 loop = fsl_stmt_empty;
	int			 finalize_rc, rc = FNC_RC_OK;

	(void)vid;
	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");
	if ((db = fsl_needs_ckout(f)) == NULL)
		return RC(FNC_RC_NO_CKOUT);

	rc = fsl_db_prepare(db, &loop,
	    "SELECT pathname FROM sfile"
	    " WHERE pathname NOT IN"
	    "  (SELECT sfile.pathname FROM vfile, sfile"
	    "   WHERE vfile.islink AND NOT vfile.deleted"
	    "   AND sfile.pathname>(vfile.pathname||'/')"
	    "   AND sfile.pathname<(vfile.pathname||'0'))"
	    " ORDER BY pathname");
	if (rc != 0)
		return RC_DB(db, rc, "fsl_db_prepare");

	while ((rc = fsl_stmt_step(&loop)) == FSL_RC_STEP_ROW) {
		fsl_ckout_manage_opt	 opt = fsl_ckout_manage_opt_empty;
		const char		*add;

		add = fsl_stmt_g_text(&loop, 0, NULL);
		if (add == NULL || strcmp(add, REPODB) == 0 ||
		    fsl_is_reserved_fn(add, -1))
			continue;
		opt.filename = add;
		/* fnc diff uses repo "absolute" paths */
		opt.relativeToCwd = false;
		/* XXX make an 'fnc stash' ignore glob option */
		opt.checkIgnoreGlobs = true;
		opt.callback = stash_get_add_cb;
		if (fsl_ckout_manage(f, &opt)) {
			rc = RC_LIBF(rc, "fsl_ckout_manage");
			goto end;
		}
		*nadded += opt.counts.added;
	}
	if (rc == FSL_RC_STEP_DONE)
		rc = FNC_RC_OK;
	else {
		if (fsl_db_err_get(db, NULL, NULL) != FNC_RC_OK)
			rc = RC_LIBF(fsl_cx_uplift_db_error(f, db),
			    "fsl_stmt_step");
		else
			rc = RC_LIBF(rc, "fsl_stmt_step");
	}

end:
	finalize_rc = fsl_stmt_finalize(&loop);
	if (finalize_rc != 0 && rc == 0)
		rc = RC_LIBF(finalize_rc, "fsl_stmt_finalize");
	return rc;
}

static int
stash_get_rm_cb(fsl_ckout_unmanage_state const *st)
{
	f_out("[-] %s\n", st->filename);
	return FNC_RC_OK;
}

static int
stash_get_add_cb(fsl_ckout_manage_state const *cms, bool *include)
{
	*include = true;
	f_out("[+] %s\n", cms->filename);
	return FNC_RC_OK;
}

/*
 * Interactive stash is a simple algorithm:
 *   1. let user select hunks to stash
 *   2. make two patch(1) files:
 *      2.1 diff of hunks selected to stash
 *      2.2 diff of hunks to be kept in the checkout
 *   3. revert checkout
 *   4. apply patch of hunks selected to stash
 *   5. stash changes
 *   6. revert checkout
 *   7. apply patch of hunks that were not selected to stash
 * This produces a checkout with only the changes that were not selected
 * to stash, achieving the same function as 'git add -p'. The user can
 * now test the code, commit, then run 'fnc stash pop' and repeat.
 */
static int
fnc_stash(struct fnc_view *view)
{
	struct fnc_diff_view_state	*s = &view->state.diff;
	struct stash_cx			*scx = &s->scx;
	struct input			 in;
	char				*msg = NULL, *prompt = NULL;
	int				 rc;

	memset(&in, 0, sizeof(in));

	scx->stash = calloc(nbytes(s->nhunks), sizeof(*scx->stash));
	if (scx->stash == NULL)
		return RC_ERRNO("calloc");

	STAILQ_INIT(&scx->pcx.head);   /* queue of files to be patched */
	scx->pcx.context = s->context;
	scx->pcx.report = true;  /* report files with changes stashed */
	scx->pcx.report_cb = patch_reporter;

	rc = select_hunks(view);  /* 1. get hunks to stash */
	if (rc)
		goto end;

	/* Use default stash msg of "fnc stash CKOUT-HASH" if not provided. */
	msg = fsl_mprintf("fnc stash %.11s", s->id2);
	if (msg == NULL) {
		rc = RC_ERRNO("fsl_mprintf");
		goto end;
	}
	prompt = fsl_mprintf("stash message [%s]: ", msg);
	if (prompt == NULL) {
		rc = RC_ERRNO("fsl_mprintf");
		goto end;
	}

	in.prompt = prompt;
	in.type = INPUT_ALPHA;
	in.flags = SR_CLREOL;

	rc = fnc_prompt_input(view, &in);
	if (rc) {
		rc = RC(rc, "fnc_prompt_input");
		goto end;
	}
	if (in.buf[0]) {
		free(msg);
		msg = fsl_mprintf("%s", in.buf);
		if (msg == NULL) {
			rc = RC_ERRNO("fsl_mprintf");
			goto end;
		}
	}

	s->stash = HUNK_STASH;
	rc = create_diff(s);  /* 2.1 make patch of hunks selected to stash */
	if (rc)
		goto end;

	s->stash = HUNK_CKOUT;
	rc = create_diff(s);  /* 2.2 make patch of hunks to keep in ckout */
	if (rc)
		goto end;

	endwin();  /* restore tty so we can report progress to stdout */

	/* 3. revert ckout to apply patches; vfile scanned in cmd_stash() */
	rc = revert_ckout(true, false);
	if (rc)
		goto end;

	/* with revert_ckout() finished, we can revoke root dir perms */
	rc = init_unveil(
		((const char *[]){ REPODIR, CKOUTDIR, fnc__tmpdir, tzfile() }),
		((const char *[]){ "rwc", "rwc", "rwc", "r" }), 4, true
	);
	if (rc != 0)
		goto end;

	/* 4. apply patch of hunks selected to stash */
	rc = fnc_patch(&scx->pcx, scx->patch[0]);
	if (rc)
		goto end;

	scx->pcx.report = false; /* don't report changes kept in ckout */

	/* 5. stash changes */
	rc = f__stash_create(msg, s->selected_entry->rid);
	if (rc)
		goto end;

	rc = revert_ckout(false, false);  /* 6. revert checkout */
	if (rc)
		goto end;

	/* 7. apply patch of hunks that were not selected to stash */
	rc = fnc_patch(&scx->pcx, scx->patch[1]);
	if (rc == FNC_RC_NO_PATCH)
		rc = RC_RESET;	/* no hunks selected to keep in the worktree */

end:
	free(scx->stash);
	free(msg);
	free(prompt);
	return rc;
}

static int
select_hunks(struct fnc_view *view)
{
	int	rc;
	bool	stashing = false;

	rc = stash_input_handler(view, &stashing);
	if (rc || !stashing) {
		if (rc && rc != FNC_RC_BREAK)
			return rc;
		return RC(FNC_RC_BREAK, "%s: work tree unchanged",
		    rc == FNC_RC_BREAK ? "stash aborted" : "no hunks stashed");
	}
	return FNC_RC_OK;
}

/*
 * Iterate each hunk of changes in the local checkout, and prompt the user
 * for their choice with: "stash this hunk (b,m,y,n,a,k,A,K,?)? [y]"
 *   b - scroll back (only available if hunk occupies previous page)
 *   m - show more (only available if hunk occupies following page)
 *   y - stash this hunk (default choice if [return] is pressed)
 *   n - do not stash this hunk
 *   a - stash this hunk and all remaining hunks in the _file_
 *   k - do not stash this hunk nor any remaining hunks in the _file_
 *   A - stash this hunk and all remaining hunks in the _diff_
 *   K - do not stash this hunk nor any remaining hunks in the _diff_
 *   ? - display stash help dialog box
 * Key maps in the stash help dialog:
 *   q - quit the help
 *   Q - exit help and quit fnc stash _discarding_ any selections
 * XXX This input handling and the set_choice() code is tricky!
 * 2023-08-13: Tricky code was greatly simplified.
 */
static int
stash_input_handler(struct fnc_view *view, bool *stashing)
{
	struct fnc_diff_view_state	*s = &view->state.diff;
	struct input			 in;
	size_t				 last, hunk = 0;
	size_t				 line = s->first_line_onscreen;
	int				 hh, rc = FNC_RC_OK;
	enum stash_opt			 choice = STASH_CH_NONE;

	memset(&in, 0, sizeof(in));

	in.type = INPUT_ALPHA;
	in.flags = SR_CLREOL;

	while (hunk < s->nhunks) {
		char	choices[12];
		char	prompt[64];
		int	len;
		enum	stash_mvmt scroll;

		/* advance line index till we find the next hunk */
		while (s->dlines[line] != LINE_DIFF_HUNK) {
			/*
			 * If next hunk is in the next file
			 * reset sticky fle choice
			 */
			if (choice != STASH_CH_KEEP_ALL &&
			    choice != STASH_CH_STASH_ALL &&
			    s->dlines[line] == LINE_DIFF_INDEX) {
				*in.buf = '\0';
				choice = STASH_CH_NONE;
			}
			++line;
		}

		/*
		 * Place hunk header, or file Index line if showing the
		 * first hunk in the file, at the top of the screen.
		 */
		s->first_line_onscreen = line;
		if (s->dlines[line - 6] == LINE_DIFF_INDEX)
			s->first_line_onscreen = line - 6;

		s->selected_line = 1;
		hh = s->first_line_onscreen;  /* current hunk header line */
redraw:
		rc = view->show(view);
		if (rc)
			return rc;

		updatescreen(view->window, true, true);
		keypad(view->window, false);  /* don't accept arrow keys */

		memset(prompt, '\0', sizeof(prompt));
		last = s->last_line_onscreen;

		len = snprintf(prompt, sizeof(prompt),
		    "[%zu/%zu] stash this hunk (", hunk + 1, s->nhunks);
		if (len < 0 || (len > 0 && (size_t)len >= sizeof(prompt)))
			return RC(FNC_RC_RANGE, "snprintf");

		scroll = STASH_MVMT_NONE;

		/* enable 'b,m' answers if hunk occupies more than this page */
		if (s->first_line_onscreen > hh)
			scroll = STASH_MVMT_UP;
		if (hunk < s->nhunks) {
			size_t eoh = line + 1;

			while (s->dlines[eoh] != LINE_DIFF_HUNK &&
			    s->dlines[eoh] != LINE_DIFF_INDEX)
				if (eoh++ == s->nlines - 1)
					break;
			if (eoh > last)
				++scroll;	/* STASH_MVMT_DOWN{_UP} */
		} else if (last < s->nlines)
			++scroll;		/* STASH_MVMT_DOWN{_UP} */

		rc = generate_prompt(choices, prompt, sizeof(prompt), scroll);
		if (rc)
			return rc;

		in.prompt = prompt;

		while (!choice && !valid_input(*in.buf, choices)) {
			rc = fnc_prompt_input(view, &in);
			if (rc)
				return rc;

			if (*in.buf == '\0')
				*in.buf = 'y';
			else if (*in.buf == 'Q')
				return FNC_RC_BREAK;
			else if (in.buf[1])
				*in.buf = '\0';
			else if (*in.buf == '?' || *in.buf == 'H' ||
			    (int)*in.buf == (KEY_F(1))) {
				int done = 0;

				rc = stash_help(view, scroll, &done);
				if (rc || done)
					return rc ? rc : FNC_RC_BREAK;
			}

		}

		if (*in.buf == 'm' || *in.buf == 'b') {
			s->first_line_onscreen = *in.buf == 'b' ?
			    MAX(hh, s->first_line_onscreen - view->nlines + 2) :
			    s->last_line_onscreen;
			*in.buf = '\0';
			goto redraw;
		}

		set_choice(s, stashing, in.buf, hunk, &choice);
		++hunk;
		++line;
	}

	return FNC_RC_OK;
}

/*
 * Construct string ans of valid answers based on scroll, and generate the
 * corresponding prompt with the available answers from which to choose.
 */
static int
generate_prompt(char *ans, char *prompt, size_t sz, enum stash_mvmt scroll)
{
	char	a[] = "bmynakAKQ";
	size_t	ai, pi;

	/* Set valid answers. */
	switch (scroll) {
	case STASH_MVMT_UPDOWN:
		break;
	case STASH_MVMT_UP:
		memmove(a + 1, a + 2, 8);
		break;
	case STASH_MVMT_DOWN:
		memmove(a, a + 1, 9);
		break;
	case STASH_MVMT_NONE:
		memmove(a, a + 2, 8);
		break;
	}

	/* generate prompt string */
	pi = strlen(prompt);
	for (ai = 0; a[ai]; ++ai) {
		prompt[pi++] = a[ai];
		prompt[pi++] = ',';
	}

	if (memccpy(prompt + pi, "?)? [y] ", '\0', sz) == NULL)
		return RC(FNC_RC_NO_SPACE, "memccpy");

	if (memccpy(ans, a, '\0', sizeof(a)) == NULL)
		return RC(FNC_RC_NO_SPACE, "memccpy");

	return FNC_RC_OK;
}

/*
 * Return true if in is found in valid, else return false.
 * valid must be terminated with a sentinel (NULL) pointer.
 */
static bool
valid_input(const char in, char *valid)
{
	size_t idx;

	for (idx = 0; valid[idx]; ++idx)
		if (in == valid[idx])
			return true;

	return false;
}

/*
 * Set or clear the corresponding bit in bitstring s->scx.stash based on the
 * answer in in.buf. If a persistent choice was made, assign it to *ch. Advance
 * next file *nf in relation to next hunk *nh, and assign next file start line
 * to *nxt. If the current file is the last file, set *lastfile. Advance *nh.
 */
static void
set_choice(struct fnc_diff_view_state *s, bool *stashing, char *ans,
    size_t current_hunk, enum stash_opt *ch)
{
	unsigned char *bs = s->scx.stash;

	/* update bitstring based on ongoing persistent or single hunk choice */
	if (*ans != 'n' && *ans != 'k' && *ans != 'K') {
		setbit(bs, current_hunk);  /* stash hunk */
		*stashing = true;
	} else  /* 'n' 'k' 'K' */
		clrbit(bs, current_hunk);  /* keep hunk in ckout */

	/* check for a new persistent choice */
	if (*ans == 'a')
		*ch = STASH_CH_STASH_FILE;
	else if (*ans == 'k')
		*ch = STASH_CH_KEEP_FILE;
	else if (*ans == 'A')
		*ch = STASH_CH_STASH_ALL;
	else if (*ans == 'K')
		*ch = STASH_CH_KEEP_ALL;
	if (!*ch)
		*ans = '\0';  /* no persistent choice; reset */

	if ((*ch == STASH_CH_STASH_FILE || *ch == STASH_CH_KEEP_FILE) &&
	    *ans != 'y' && *ans != 'n')
		*ans = *ch == STASH_CH_STASH_FILE ? 'y' : 'n';
}

/*
 * Revert the current checkout. If renames is set, don't revert files that are
 * renamed with _no_ changes. If scan is set, scan for changes before reverting.
 */
static int
revert_ckout(bool renames, bool scan)
{
	struct fsl_cx			*f;
	struct fsl_db			*db;
	struct fsl_stmt			 q = fsl_stmt_empty;
	struct fsl_ckout_revert_opt	 opt = fsl_ckout_revert_opt_empty;
	struct fsl_id_bag		 idbag = fsl_id_bag_empty;
	int				 finalize_rc, rc;

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");
	if ((db = fsl_needs_ckout(f)) == NULL)
		return RC(FNC_RC_NO_CKOUT);

	rc = fsl_ckout_vfile_ids(f, 0, &idbag, ".", false, true);
	if (rc != FNC_RC_OK)
		return RC_LIBF(rc, "fsl_ckout_vfile_ids");

	if (renames) {
		/*
		 * XXX Don't revert renamed files with _NO_ changes because
		 * we need to stash them, but there's no way to apply them
		 * with a diff as there's no hunks; however, we can apply our
		 * stash patch to the checkout with renames in the vfile.
		 */
		rc = fsl_db_prepare(db, &q, "SELECT id, origname, pathname"
		    " FROM vfile WHERE origname IS NOT NULL"
		    " AND origname<>pathname AND chnged=0");
		if (rc != FNC_RC_OK) {
			rc = RC_DB(db, rc, "fsl_db_prepare");
			goto end;
		}
		rc = fsl_stmt_each(&q, rm_vfile_renames_cb, &idbag);
		if (rc != 0) {
			/* if not FSL_RC_DB, rm_vfile_renames_cb() set rc */
			if (rc == FSL_RC_DB)
				rc = RC_LIBF(rc, "fsl_stmt_each");
			goto end;
		}
	}

	opt.scanForChanges = scan;
	opt.filename = NULL;
	opt.callback = NULL;
	opt.callbackState = NULL;
	opt.vfileIds = &idbag;
	rc = fsl_ckout_revert(f, &opt);

end:
	fsl_id_bag_clear(&idbag);
	finalize_rc = fsl_stmt_finalize(&q);
	if (finalize_rc != 0 && rc == 0)
		rc = RC_LIBF(finalize_rc, "fsl_stmt_finalize");
	return rc;
}

static int
rm_vfile_renames_cb(fsl_stmt *stmt, void *state)
{
	struct fsl_id_bag	*bag = (fsl_id_bag *)state;
	const char		*ogname, *name;
	fsl_id_t		 id;
	int			 rc;

	rc = fsl_stmt_get_id(stmt, 0, &id);
	if (rc != 0)
		return RC_LIBF(rc, "fsl_stmt_get_id");
	rc = fsl_stmt_get_text(stmt, 1, &ogname, NULL);
	if (rc != 0)
		return RC_LIBF(rc, "fsl_stmt_get_text");
	rc = fsl_stmt_get_text(stmt, 2, &name, NULL);
	if (rc != 0)
		return RC_LIBF(rc, "fsl_stmt_get_text");

	fsl_id_bag_remove(bag, id);
	printf("[>] %s  ->  %s\n", ogname, name);

	return FNC_RC_OK;
}

/*
 * Scan patch(1) file at path for valid patches, and populate patch
 * context pcx with an fnc_patch_file queue of all diffed files, each
 * containing an fnc_patch_hunk queue representing the file's changes.
 * Iterate the file queue and apply each fnc_patch_file to the checkout.
 */
static int
fnc_patch(struct patch_cx *pcx, const char *path)
{
	struct fnc_patch_file	*patch, *t;
	FILE			*fp;
	int			 rc;

	if ((fp = fopen(path, "re")) == NULL)
		return RC_ERRNO("fopen");

	rc = scan_patch(pcx, fp);  /* read diff and construct patch context */
	if (rc != 0)
		goto end;

	STAILQ_FOREACH(patch, &pcx->head, entries) {
		pcx->pf = patch;
		rc = apply_patch(pcx, false);
		if (rc != 0)
			break;
	}

end:
	if (fclose(fp) == EOF && rc == 0)
		rc = RC_ERRNO("fclose");
	STAILQ_FOREACH_SAFE(patch, &pcx->head, entries, t) {
		STAILQ_REMOVE(&pcx->head, patch, fnc_patch_file, entries);
		free_patch(patch);
	}
	return rc;
}

/*
 * Scan patch file fp to construct an fnc_patch_file pf for each versioned
 * file found with changes, and queue them in pcx->head. Parse each pf for
 * valid hunks, and queue them in pf->head to produce a hierarchical ADT of
 * files->hunks->lines to be patched.
 */
static int
scan_patch(struct patch_cx *pcx, FILE *fp)
{
	int	rc = FNC_RC_OK;
	bool	patch_found = false;

	while (!feof(fp)) {
		struct fnc_patch_file	*pf;
		bool			 eof = false;

		rc = find_patch_file(&pf, pcx, fp);
		if (rc != 0)
			goto end;

		patch_found = true;
		STAILQ_INIT(&pf->head);  /* queue of hunks per file to patch */
		STAILQ_INSERT_TAIL(&pcx->head, pf, entries);

		while (!eof) {
			struct fnc_patch_hunk *h;

			rc = parse_hunk(&h, fp, pcx->context, &eof);
			if (rc != 0)
				goto end;
			if (h != NULL)
				STAILQ_INSERT_TAIL(&pf->head, h, entries);
		}
	}

end:
	if (rc == FNC_RC_NO_PATCH && patch_found)
		rc = RC_RESET;	/* ignore trailing index lines with no hunks */
	return rc;
}

/*
 * Find the next versioned file in patch(1) file fp by parsing the path from
 * the diff ---/+++ header line. If found, construct and assign a new
 * fnc_patch_file to *ptr, which must eventually be disposed of by the caller.
 */
static int
find_patch_file(struct fnc_patch_file **ptr, struct patch_cx *pcx, FILE *fp)
{
	struct fnc_patch_file	*pf;
	char			*old = NULL, *new = NULL;
	char			*line = NULL;
	size_t			 linesize = 0;
	ssize_t			 linelen;
	int			 create, rc = FNC_RC_OK;

	(void)pcx;
	*ptr = NULL;

	pf = calloc(1, sizeof(*pf));
	if (pf == NULL)
		return RC_ERRNO("calloc");

	while ((linelen = getline(&line, &linesize, fp)) != -1) {
		if (strncmp(line, "--- ", 4) == 0) {
			free(old);
			rc = parse_filename(&old, line + 4, 0);
		} else if (strncmp(line, "+++ ", 4) == 0) {
			free(new);
			rc = parse_filename(&new, line + 4, 0);
		}
		if (rc)
			break;

		if (strncmp(line, "@@ -", 4) == 0) {
			create = strncmp(line + 4, "0,0", 3) == 0;
			if ((old == NULL && new == NULL) ||
			    (!create && old == NULL))
				rc = RC(FNC_RC_PATCH_MALFORMED);
			else
				rc = set_patch_paths(pf, old, new);
			if (rc)
				break;

			/* rewind to previous line */
			if (fseeko(fp, -linelen, SEEK_CUR) == -1)
				rc = RC_ERRNO("fseeko");
			break;
		}
	}

	free(new);
	free(old);
	free(line);
	if (ferror(fp) && rc == 0)
		rc = RC_ERRNO("getline");
	else if (feof(fp) && rc == 0)
		rc = RC(FNC_RC_NO_PATCH);
	if (rc == 0)
		*ptr = pf;
	else
		free(pf);
	return rc;
}

static int
parse_filename(char **name, const char *at, int strip)
{
	char	*fullname, *t;
	int	 l, tab;

	*name = NULL;
	if (*at == '\0')
		return FNC_RC_OK;

	while (isspace((unsigned char)*at))
		++at;

	/* If path is /dev/null, file is being removed or created. */
	if (strncmp(at, _PATH_DEVNULL, sizeof(_PATH_DEVNULL) - 1) == 0)
		return FNC_RC_OK;

	t = strdup(at);
	if (t == NULL)
		return RC_ERRNO("strdup");

	*name = fullname = t;
	tab = strchr(t, '\t') != NULL;

	/* Strip path components and NUL-terminate. */
	for (l = strip;
	    *t != '\0' && ((tab && *t != '\t') || !isspace((unsigned char)*t));
	    ++t) {
		if (t[0] == '/' && t[1] != '/' && t[1] != '\0')
			if (--l >= 0)
				*name = t + 1;
	}
	*t = '\0';

	*name = strdup(*name);
	free(fullname);
	if (*name == NULL)
		return RC_ERRNO("strdup");

	return FNC_RC_OK;
}

static int
set_patch_paths(struct fnc_patch_file *pf, const char *old, const char *new)
{
	size_t ret = 0;

	/* prefer the new name if it's neither /dev/null nor a renamed file */
	if (new != NULL && old != NULL && strcmp(new, old) == 0)
		ret = strlcpy(pf->old, new, sizeof(pf->old));
	else if (old != NULL)
		ret = strlcpy(pf->old, old, sizeof(pf->old));
	if (ret && ret >= sizeof(pf->old))
		return RC(FNC_RC_NO_SPACE, "strlcpy");

	if (new != NULL) {
		if (strlcpy(pf->new, new, sizeof(pf->new)) >= sizeof(pf->new))
			return RC(FNC_RC_NO_SPACE, "strlcpy");
	}

	return FNC_RC_OK;
}

/*
 * Parse patch(1) file fp and extract the changed lines data from each hunk
 * header to construct an fnc_patch_hunk object and assign it to *ptr, which
 * must eventually be dispoed of by the caller. Iterate the section of changed
 * lines, and push each +/- and context line onto the hdr->lines array, which
 * must also be disposed of by the caller.
 */
static int
parse_hunk(struct fnc_patch_hunk **ptr, FILE *fp, uint8_t context, bool *eof)
{
	struct fnc_patch_hunk	*hdr = NULL;
	char			*line = NULL, ch;
	size_t			 linesize = 0;
	ssize_t			 linelen;
	long			 leftold, leftnew;
	int			 rc = FNC_RC_OK;

	(void)context;
	*ptr = NULL;

	linelen = getline(&line, &linesize, fp);
	if (linelen == -1) {
		*eof = true;	/* end of patch file */
		if (ferror(fp))
			rc = RC_ERRNO("getline");
		goto end;
	}

	hdr = calloc(1, sizeof(*hdr));
	if (hdr == NULL) {
		rc = RC_ERRNO("calloc");
		goto end;
	}

	rc = parse_hdr(line, eof, hdr);
	if (rc)
		goto end;

	if (*eof) {
		if (fseeko(fp, -linelen, SEEK_CUR) == -1)
			rc = RC_ERRNO("fseeko");
		goto end;
	}

	leftold = hdr->oldlines;
	leftnew = hdr->newlines;

	while (leftold > 0 || leftnew > 0) {
		linelen = getline(&line, &linesize, fp);
		if (linelen == -1) {
			if (ferror(fp)) {
				rc = RC_ERRNO("getline");
				goto end;
			}
			if (leftold < 3 && leftnew < 3) {
				*eof = true;	/* trim trailing newlines */
				break;
			}
			rc = RC(FNC_RC_PATCH_TRUNCATED);
			goto end;
		}
		if (line[linelen - 1] == '\n')
			line[linelen - 1] = '\0';

		ch = *line;
		if (ch == '\t' || ch == '\0')
			ch = ' ';	/* leading space got eaten */

		switch (ch) {
		case '-':
			leftold--;
			break;
		case ' ':
			leftold--;
			leftnew--;
			break;
		case '+':
			leftnew--;
			break;
		default:
			rc = RC(FNC_RC_PATCH_MALFORMED);
			goto end;
		}

		if (leftold < 0 || leftnew < 0) {
			rc = RC(FNC_RC_PATCH_MALFORMED);
			goto end;
		}

		rc = pushline(hdr, line);
		if (rc)
			goto end;

		if ((ch == '-' && leftold == 0) ||
		    (ch == '+' && leftnew == 0)) {
			rc = peek_special_line(hdr, fp, ch == '+');
			if (rc)
				goto end;
		}
	}

end:
	/*
	 * XXX Check for hdr->lines as fnc diff adds a trailing empty newline
	 * between diffs (i.e., before the next file's Index line), which, if
	 * added, will produce a false negative in patch_file().
	 */
	if (hdr != NULL) {
		if (rc == 0 && hdr->lines != NULL)
			*ptr = hdr;
		else
			free_hunk(hdr);
	}
	free(line);
	return rc;
}

/*
 * Parse hunk header line and assign corresponding new and old lines to
 * hdr->{old,new}lines, respectively.
 */
static int
parse_hdr(char *s, bool *eof, struct fnc_patch_hunk *hdr)
{
	int rc;

	if (strncmp(s, "@@ -", 4) != 0) {
		*eof = true;
		return FNC_RC_OK;
	}

	s += 4;
	if (*s == '\0')
		return FNC_RC_OK;

	rc = strtolnum(&s, &hdr->oldfrom);
	if (rc != 0)
		return rc;
	if (*s == ',') {
		s++;
		rc = strtolnum(&s, &hdr->oldlines);
		if (rc != 0)
			return rc;
	} else
		hdr->oldlines = 1;

	if (*s == ' ')
		++s;
	if (*s != '+' || *++s == '\0')
		return RC(FNC_RC_PATCH_MALFORMED);

	rc = strtolnum(&s, &hdr->newfrom);
	if (rc != 0)
		return rc;
	if (*s == ',') {
		s++;
		rc = strtolnum(&s, &hdr->newlines);
		if (rc != 0)
			return rc;
	} else
		hdr->newlines = 1;

	if (*s == ' ')
		++s;
	if (*s != '@')
		return RC(FNC_RC_PATCH_MALFORMED);

	if (hdr->oldfrom >= LONG_MAX - hdr->oldlines ||
	    hdr->newfrom >= LONG_MAX - hdr->newlines ||
	    hdr->oldlines >= LONG_MAX - hdr->newlines - 1)
		return RC(FNC_RC_PATCH_MALFORMED);

	if (hdr->oldlines == 0)
		hdr->oldfrom++;

	return FNC_RC_OK;
}

static int
strtolnum(char **str, int_least32_t *n)
{
	char		*p, c;
	const char	*errstr;

	for (p = *str; isdigit((unsigned char)*p); ++p)
		/* nop */;

	c = *p;
	*p = '\0';

	*n = strtonum(*str, 0, LONG_MAX, &errstr);
	if (errstr && *errstr != '\0')
		return RC(FNC_RC_PATCH_MALFORMED);

	*p = c;
	*str = p;
	return FNC_RC_OK;
}

static int
pushline(struct fnc_patch_hunk *hdr, const char *line)
{
	static int rc = FNC_RC_OK;
	char *p = NULL;

	if (*line != '+' && *line != '-' && *line != ' ' && *line != '\\') {
		if ((p = fsl_mprintf(" %s", line)) == NULL)
			return RC_ERRNO("fsl_mprintf");
		line = p;
	}

	rc = alloc_hunk_line(hdr, line);

	fsl_free(p);
	return rc;
}

static int
alloc_hunk_line(struct fnc_patch_hunk *h, const char *line)
{
	void	*t;
	size_t	 newsz;

	if (h->nlines == h->cap) {
		if (h->cap)
			newsz = h->cap * 1.5;
		else
			newsz = 16;

		t = reallocarray(h->lines, newsz, sizeof(*h->lines));
		if (t == NULL) {
			free(h->lines);
			return RC_ERRNO("reallocarray");
		}

		h->lines = t;
		memset(h->lines + h->cap, 0,
		    (newsz - h->cap) * sizeof(*h->lines));
		h->cap = newsz;
	}

	t = strdup(line);
	if (t == NULL)
		return RC_ERRNO("strdup");

	h->lines[h->nlines++] = t;
	return FNC_RC_OK;
}

static int
peek_special_line(struct fnc_patch_hunk *hdr, FILE *fp,
    int send)
{
	int ch, rc = FNC_RC_OK;

	ch = fgetc(fp);
	if (ch != EOF && ch != '\\') {
		ungetc(ch, fp);
		return rc;
	}

	if (ch == '\\' && send) {
		rc = pushline(hdr, "\\");
		if (rc)
			return rc;
	}

	while (ch != EOF && ch != '\n')
		ch = fgetc(fp);

	if (ch != EOF || feof(fp))
		return rc;
	return RC_ERRNO("fgetc");
}

#define FNC_FILEMODE	(S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)
#define FNC_DIRMODE	(S_IFDIR|S_IRWXU|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH)

static int
apply_patch(struct patch_cx *pcx, bool nop)
{
	struct fnc_patch_file	*p = pcx->pf;
	const char		*newpath, *oldpath;
	char			*tmppath = NULL;
	FILE			*tmp = NULL;
	int			 rc, renamed = 0;
	mode_t			 mode = FNC_FILEMODE;

	newpath = p->new;
	oldpath = p->old;

	if (oldpath[0])
		renamed = strcmp(oldpath, newpath) != 0;

	if (!nop) {
		/*
		 * XXX Because older landlock versions do not allow file
		 * reparenting (i.e., linking and renaming files into
		 * different directories), and we patch into a tmp file
		 * that is renamed to the tracked file, the tmp file path
		 * _must_ be in the same directory as the tracked file.
		 */
		rc = fnc_open_tmpfile(&tmppath, &tmp, newpath, "-patched.fnc");
		if (rc)
			goto end;
	}
	rc = patch_file(p, oldpath, tmp, nop, &mode);
	if (rc || nop)
		goto end;

	if (p->old[0] != '\0' && p->new[0] == '\0') {
		/* file deleted */
		rc = fnc_rm_vfile(pcx, oldpath, false);
		goto end;
	}

	if (fchmod(fileno(tmp), mode) == -1) {
		rc = RC_ERRNO("fchmod: %s %d", newpath, mode);
		goto end;
	}

	if (fclose(tmp) == EOF) {
		rc = RC_ERRNO("fclose");
		goto end;
	}

	if (rename(tmppath, newpath) == -1) {
		if (errno != ENOENT) {
			rc = RC_ERRNO("rename(%s, %s)", tmppath, newpath);
			goto end;
		}

		rc = fsl_mkdir_for_file(newpath, true);
		if (rc) {
			rc = RC(FNC_RC_IO, "fsl_mkdir_for_file");
			goto end;
		}
		if (rename(tmppath, newpath) == -1) {
			rc = RC_ERRNO("rename(%s, %s)", tmppath, newpath);
			goto end;
		}
	}
	free(tmppath);
	tmppath = NULL;

	if (renamed) {
		/*
		 * XXX Applying renames as a removal (of the previous filename)
		 * and an addition (of the new renamed filename) produces diffs
		 * with the entire file contents removed under the old name and
		 * then added under the new name. This makes it practically
		 * impossible to identify any edits to the renamed file. Update
		 * such cases as a rename in the vfile table for better diffs.
		 */
#if 0
		rc = fnc_rm_vfile(pcx, oldpath, false);
		if (rc)
			goto end;
		rc = fnc_add_vfile(pcx, newpath, false);
#endif
		rc = fnc_rename_vfile(oldpath, newpath);
		if (rc)
			goto end;
		if (pcx->report)
			rc = patch_reporter(p, oldpath, newpath, '~');
	} else if (p->old[0] == '\0') {
		/* file added */
		rc = fnc_add_vfile(pcx, newpath, false);
	} else if (pcx->report)
		rc = patch_reporter(p, oldpath, newpath, '~');

end:
	if (tmppath != NULL) {
		if (unlink(tmppath) == -1 && rc == FNC_RC_OK) {
			if (errno != ENOENT)
				rc = RC_ERRNO("unlink: %s", tmppath);
		}
	}
	free(tmppath);
	return rc;
}

/*
 * Open new tmp file at basepath, which is expected to be an absolute path,
 * with optional suffix. The final filename will be "basepath-XXXXXX[SUFFIX]"
 * where XXXXXX is a random character string populated by mkstemp(3) (or
 * mkstemps(3) if suffix is not NULL).
 */
static int
fnc_open_tmpfile(char **path, FILE **outfile, const char *basepath,
    const char *suffix)
{
	int fd, rc = FNC_RC_OK;

	*outfile = NULL;

	if ((*path = fsl_mprintf("%s-XXXXXXXXXX%s", basepath,
	    suffix ? suffix : "")) == NULL)
		return RC_ERRNO("fsl_mprintf");

	if (suffix != NULL)
		fd = mkstemps(*path, strlen(suffix));
	else
		fd = mkstemp(*path);
	if (fd == -1) {
		rc = RC_ERRNO("%s: %s", suffix ? "mkstemps" : "mkstemp",
		    *path);
		free(*path);
		*path = NULL;
		return rc;
	}

	*outfile = fdopen(fd, "w+");
	if (*outfile == NULL) {
		rc = RC_ERRNO("fdopen: %s", *path);
		free(*path);
		*path = NULL;
	}

	return rc;
}

/*
 * Apply patch file p to the tracked file at path by iterating and applying
 * each hunk from p->head and creating the patched file in /tmp. On success,
 * copy the newly patched file in /tmp to the tracked filename at path.
 */
static int
patch_file(struct fnc_patch_file *p, const char *path, FILE *tmp, int nop,
    mode_t *mode)
{
	struct fnc_patch_hunk	*h;
	struct stat		 sb;
	FILE			*orig;
	char			*line = NULL;
	off_t			 copypos, pos;
	ssize_t			 linelen;
	size_t			 linesize = 0;
	long			 lineno = 0;
	int			 rc;

	if (p->old[0] == '\0') {
		/* create new versioned file */
		h = STAILQ_FIRST(&p->head);
		if (h == NULL || STAILQ_NEXT(h, entries) != NULL)
			return RC(FNC_RC_PATCH_MALFORMED);
		if (nop)
			return FNC_RC_OK;
		return apply_hunk(tmp, h, &lineno);
	}

	if ((orig = fopen(path, "r")) == NULL) {
		rc = RC_ERRNO("fopen(%s, \"r\")", path);
		goto end;
	}

	if (fstat(fileno(orig), &sb) == -1) {
		rc = RC_ERRNO("fstat(%s)", path);
		goto end;
	}
	*mode = sb.st_mode;

	pos = 0;
	copypos = 0;
	STAILQ_FOREACH(h, &p->head, entries) {
		if (h->lines == NULL)
			break;

	retry:
		rc = locate_hunk(orig, h, &pos, &lineno);
		if (rc != 0) {
			if (rc == FNC_RC_HUNK_FAILED)
				h->rc = rc;
			goto end;
		}
		if (!nop) {
			rc = copyfile(tmp, orig, copypos, pos);
			if (rc)
				goto end;
		}
		copypos = pos;

		rc = test_hunk(orig, h);
		if (rc != 0) {
			if (rc != FNC_RC_HUNK_FAILED)
				goto end;
			/*
			 * Retry applying the hunk by starting the search
			 * after the previous partial match.
			 */
			if (fseeko(orig, pos, SEEK_SET) == -1) {
				rc = RC_ERRNO("fseeko");
				goto end;
			}
			linelen = getline(&line, &linesize, orig);
			if (linelen == -1) {
				rc = RC_ERRNO("getline");
				goto end;
			}
			++lineno;
			goto retry;
		}

		if (lineno + 1 != h->oldfrom)
			h->offset = lineno + 1 - h->oldfrom;

		if (!nop)
			rc = apply_hunk(tmp, h, &lineno);
		if (rc)
			goto end;

		copypos = ftello(orig);
		if (copypos == -1) {
			rc = RC_ERRNO("ftello");
			goto end;
		}
	}

	if (p->new[0] == '\0' && sb.st_size != copypos) {
		h = STAILQ_FIRST(&p->head);
		rc = h->rc = RC(FNC_RC_HUNK_FAILED);
	} else if (!nop && !feof(orig))  /* success! copy to versioned file */
		rc = copyfile(tmp, orig, copypos, -1);

end:
	if (orig != NULL && fclose(orig) == EOF && rc == 0)
		rc = RC_ERRNO("fclose");
	return rc;
}

static int
apply_hunk(FILE *tmp, struct fnc_patch_hunk *h, long *lineno)
{
	size_t	i = 0;

	for (i = 0; i < h->nlines; ++i) {
		switch (*h->lines[i]) {
		case ' ':
			if (fprintf(tmp, "%s\n", h->lines[i] + 1) < 0)
				return RC(FNC_RC_IO, "fprintf");
			/* fallthrough */
		case '-':
			(*lineno)++;
			break;
		case '+':
			if (fprintf(tmp, "%s", h->lines[i] + 1) < 0)
				return RC(FNC_RC_IO, "fprintf");
			if (i != h->nlines - 1 || !h->nonl) {
				if (fprintf(tmp, "\n") < 0)
					return RC(FNC_RC_IO, "fprintf");
			}
			break;
		}
	}
	return FNC_RC_OK;
}

static int
locate_hunk(FILE *orig, struct fnc_patch_hunk *h, off_t *pos, long *lineno)
{
	char	*line = NULL;
	char	 mode = *h->lines[0];
	ssize_t	 linelen;
	size_t	 linesize = 0;
	off_t	 match = -1;
	long	 match_lineno = -1;
	int	 rc = FNC_RC_OK;

	for (;;) {
		linelen = getline(&line, &linesize, orig);
		if (linelen == -1) {
			if (ferror(orig))
				rc = RC_ERRNO("getline");
			else if (match == -1) {
				if (h->oldlines == 0 && *lineno == 0) {
					/* empty orig file always matches */
					match_lineno = 0;
					match = 0;
				} else
					rc = RC(FNC_RC_HUNK_FAILED);
			}
			break;
		}
		if (line[linelen - 1] == '\n')
			line[linelen - 1] = '\0';
		(*lineno)++;

		if ((mode == ' ' && strcmp(h->lines[0] + 1, line) == 0) ||
		    (mode == '-' && strcmp(h->lines[0] + 1, line) == 0) ||
		    (mode == '+' && *lineno == h->oldfrom)) {
			match = ftello(orig);
			if (match == -1) {
				rc = RC_ERRNO("ftello");
				break;
			}
			match -= linelen;
			match_lineno = (*lineno) - 1;
		}

		if (*lineno >= h->oldfrom && match != -1)
			break;
	}

	if (rc == FNC_RC_OK) {
		*pos = match;
		*lineno = match_lineno;
		if (fseeko(orig, match, SEEK_SET) == -1)
			rc = RC_ERRNO("fseeko");
	}

	free(line);
	return rc;
}

/*
 * Starting at copypos until pos, copy data from orig into tmp.
 * If pos is -1, copy until EOF.
 */
static int
copyfile(FILE *tmp, FILE *orig, off_t copypos, off_t pos)
{
	char	buf[BUFSIZ];
	size_t	len, r, w;

	if (fseeko(orig, copypos, SEEK_SET) == -1)
		return RC_ERRNO("fseeko");

	while (pos == -1 || copypos < pos) {
		len = sizeof(buf);
		if (pos > 0)
			len = MIN(len, (size_t)pos - copypos);
		r = fread(buf, 1, len, orig);
		if (r != len && ferror(orig))
			return RC_ERRNO("fread");
		w = fwrite(buf, 1, r, tmp);
		if (w != r)
			return RC_ERRNO("fwrite");
		copypos += len;
		if (r != len && feof(orig)) {
			if (pos == -1)
				return FNC_RC_OK;
			return RC(FNC_RC_HUNK_FAILED);
		}
	}
	return FNC_RC_OK;
}

static int
test_hunk(FILE *orig, struct fnc_patch_hunk *h)
{
	char	*line = NULL;
	ssize_t	 linelen;
	size_t	 linesize = 0, i = 0;
	int	 rc = FNC_RC_OK;

	for (i = 0; i < h->nlines; ++i) {
		switch (*h->lines[i]) {
		case '+':
			continue;
		case ' ':
		case '-':
			linelen = getline(&line, &linesize, orig);
			if (linelen == -1) {
				if (ferror(orig))
					rc = RC_ERRNO("getline");
				else
					rc = RC(FNC_RC_HUNK_FAILED);
				goto end;
			}
			if (line[linelen - 1] == '\n')
				line[linelen - 1] = '\0';
			if (strcmp(h->lines[i] + 1, line) != 0) {
				rc = RC(FNC_RC_HUNK_FAILED);
				goto end;
			}
			break;
		}
	}

end:
	free(line);
	return rc;
}

static int
fnc_add_vfile(struct patch_cx *pcx, const char *path, bool nop)
{
	struct fsl_cx		*f;
	struct fsl_db		*db;
	fsl_ckout_manage_opt	 opt = fsl_ckout_manage_opt_empty;
	int			 rc, endtx_rc;

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");
	if ((db = fsl_needs_ckout(f)) == NULL)
		return RC(FNC_RC_NO_CKOUT);

	rc = fsl_db_txn_begin(db);
	if (rc != 0)
		return RC_LIBF(rc, "fsl_db_txn_begin");

	opt.filename = path;
	opt.relativeToCwd = false;  /* relative repo root paths from fnc diff */
	opt.checkIgnoreGlobs = true;  /* XXX make an 'fnc stash' option? */
	opt.callbackState = pcx;  /* patch context and report cb */
	opt.callback = fnc_addvfile_cb;

	rc = fsl_ckout_manage(f, &opt);
	if (rc != 0)
		rc = RC_LIBF(rc, "fsl_ckout_manage");

	endtx_rc = fsl_db_txn_end(db, nop ? true : false);
	if (endtx_rc != 0 && rc == 0)
		rc = RC_LIBF(endtx_rc, "fsl_db_txn_end");
	return rc;
}

static int
fnc_addvfile_cb(fsl_ckout_manage_state const *cx, bool *include)
{
	struct patch_cx *pcx = cx->opt->callbackState;

	if (pcx->report) {
		int rc;

		rc = pcx->report_cb(pcx->pf, pcx->pf->old, pcx->pf->new, '+');
		if (rc != 0)
			return rc;
	}
	*include = true;
	return FNC_RC_OK;
}

static int
fnc_rm_vfile(struct patch_cx *pcx, const char *path, bool nop)
{
	struct fsl_cx		*f;
	struct fsl_db		*db;
	fsl_ckout_unmanage_opt	 opt = fsl_ckout_unmanage_opt_empty;
	int			 rc, endtx_rc;

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");
	if ((db = fsl_cx_db_ckout(f)) == NULL)
		return RC(FNC_RC_NO_CKOUT);

	rc = fsl_cx_txn_begin(f);
	if (rc != 0)
		return RC_LIBF(rc, "fsl_cx_txn_begin");

	opt.filename = path;
	opt.scanForChanges = false;
	opt.vfileIds = NULL;
	opt.relativeToCwd = false;  /* relative repo root paths from fnc diff */
	opt.callback = fnc_rmvfile_cb;
	opt.callbackState = pcx;

	rc = fsl_ckout_unmanage(f, &opt);
	if (rc != 0)
		rc = RC_LIBF(rc, "fsl_ckout_unmanage");

	endtx_rc = fsl_db_txn_end(db, nop ? true : false);
	if (endtx_rc != 0 && rc == 0)
		rc = RC_LIBF(endtx_rc, "fsl_db_txn_end");
	return rc;
}

static int
fnc_rmvfile_cb(fsl_ckout_unmanage_state const *cx)
{
	struct patch_cx *pcx = cx->opt->callbackState;

	if (pcx->report)
		return pcx->report_cb(pcx->pf, pcx->pf->old, pcx->pf->new, '-');
	return FNC_RC_OK;
}

static int
fnc_rename_vfile(const char *oldpath, const char *newpath)
{
	struct fsl_cx	*f;
	struct fsl_db	*db;
	const char	*dir;
	int		 rc;

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");
	if ((db = fsl_needs_ckout(f)) == NULL)
		return RC(FNC_RC_NO_CKOUT);

	rc = fsl_db_exec_multi(db,
	    "UPDATE vfile SET pathname='%q', origname='%q'"
	    " WHERE pathname='%q' %s AND vid=%d", newpath, oldpath,
	    oldpath, fsl_cx_filename_collation(f), f->db.ckout.rid);
	if (rc) {
		unlink(newpath);
		return RC_LIBF(rc, "fsl_db_exec_multi");
	}

	dir = getdirname(oldpath, -1, false);
	if (unlink(oldpath) == -1 && errno != ENOENT)
		return RC_ERRNO("unlink: %s", oldpath);
	if (fsl_dir_is_empty(dir) == FNC_RC_OK) {
		if (fsl_rmdir(dir))
			return RC_ERRNO("fsl_rmdir: %s", dir);
	}

	return FNC_RC_OK;
}

static int
patch_reporter(struct fnc_patch_file *p, const char *old, const char *new,
    unsigned char status)
{
	struct fnc_patch_hunk	*h;
	int			 rc;

	rc = patch_report(old, new, status, 0, 0, 0, 0, 0, 0);
	if (rc)
		return rc;

	STAILQ_FOREACH(h, &p->head, entries) {
		if (h->offset == 0 && h->rc == 0)
			continue;

		rc = patch_report(old, new, 0, h->oldfrom, h->oldlines,
		    h->newfrom, h->newlines, h->offset, h->rc);
	}

	return rc;
}

static int
patch_report(const char *old, const char *new, unsigned char status,
    long oldfrom, long oldlines, long newfrom, long newlines,
    long offset, int hunkrc)
{
	const char *path = new[0] == '\0' ? old : new;

	while (*path == '/')
		path++;

	if (status != 0) {
		if (*old != '\0' && *new != '\0' && strcmp(old, new) != 0)
			printf("[%c] %s  ->  %s\n", status, old, new);
		else
			printf("[%c] %s\n", status, path);
	}
	if (offset != 0 || hunkrc != 0) {
		printf("@@ -%ld,%ld +%ld,%ld @@ ",
		    oldfrom, oldlines, newfrom, newlines);
		if (hunkrc != 0)
			printf("%s\n", RCSTR(hunkrc));
		else
			printf("applied with offset %ld\n", offset);
	}
	fflush(stdout);

	return FNC_RC_OK;
}

static void
free_patch(struct fnc_patch_file *p)
{
	struct fnc_patch_hunk *h;

	while (!STAILQ_EMPTY(&p->head)) {
		h = STAILQ_FIRST(&p->head);
		STAILQ_REMOVE_HEAD(&p->head, entries);
		free_hunk(h);
	}
	free(p);
}

static void
free_hunk(struct fnc_patch_hunk *h)
{
	size_t i;

	for (i = 0; i < h->nlines; ++i) {
		free(h->lines[i]);
		h->lines[i] = NULL;
	}
	free(h->lines);
	free(h);
	h = NULL;
}

/*
 * Create new stash of changes in checkout vid with stash message msg.
 */
static int
f__stash_create(const char *msg, int vid)
{
	struct fsl_cx	*f;
	struct fsl_db	*db;
	int		 stashid, rc;

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");
	if ((db = fsl_needs_ckout(f)) == NULL)
		return RC(FNC_RC_NO_CKOUT);

	rc = f__check_stash_tables();
	if (rc)
		return rc;

	stashid = fsl_config_get_int32(f, FSL_CONFDB_CKOUT, 1, "stash-next");
	rc = fsl_config_set_id(f, FSL_CONFDB_CKOUT, "stash-next", stashid + 1);
	if (rc != 0)
		return RC_LIBF(rc, "fsl_config_set_id");
	rc = fsl_ckout_changes_scan(f);
	if (rc != 0)
		return RC_LIBF(rc, "fsl_ckout_changes_scan");

	rc = fsl_db_exec_multi(db,
	    "INSERT INTO stash(stashid, vid, hash, comment, ctime)"
	    " VALUES(%d, %d, (SELECT uuid FROM blob WHERE rid = %d),"
	    " %Q, julianday('now'))", stashid, vid, vid, msg);
	if (rc != 0)
		return RC_LIBF(rc, "fsl_db_exec_multi");

	return f__stash_path(stashid, vid, ".");
}

/*
 * Check checkout database for up-to-date stash and stashfile tables. Create
 * or upgrade if needed.
 */
static int
f__check_stash_tables(void)
{
	static const char stashtab[] =
	    "CREATE TABLE IF NOT EXISTS stash(\n"
	    " stashid INTEGER PRIMARY KEY, -- Unique stash identifier\n"
	    " vid INTEGER,          -- Legacy baseline RID value. Do not use.\n"
	    " hash TEXT,            -- The SHA hash for the baseline\n"
	    " comment TEXT,         -- Comment for this stash.  Or NULL\n"
	    " ctime TIMESTAMP       -- When the stash was created\n"
	    ");\n"
	    "CREATE TABLE IF NOT EXISTS stashfile(\n"
	    " stashid INTEGER REFERENCES stash, -- Stash containing this file\n"
	    " isAdded BOOLEAN,       -- True if this file is added\n"
	    " isRemoved BOOLEAN,     -- True if this file is deleted\n"
	    " isExec BOOLEAN,        -- True if file is executable\n"
	    " isLink BOOLEAN,        -- True if file is a symlink\n"
	    " rid INTEGER,           -- Legacy baseline RID value. Do not use\n"
	    " hash TEXT,             -- Hash for baseline or NULL\n"
	    " origname TEXT,         -- Original filename\n"
	    " newname TEXT,          -- New name for file at next check-in\n"
	    " delta BLOB,            -- Delta from baseline or raw content\n"
	    " PRIMARY KEY(newname, stashid)\n"
	    ");\n"
	    "INSERT OR IGNORE INTO vvar(name,value) VALUES('stash-next',1);\n";
	struct fsl_cx	*f;
	struct fsl_db	*db;
	int		 rc;

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");
	if ((db = fsl_needs_ckout(f)) == NULL)
		return RC(FNC_RC_NO_CKOUT);

	if (fsl_db_table_has_column(db, "stashfile", "hash")) {
		/*
		 * Schema is up-to-date, but an older version of Fossil that
		 * doesn't know about the stash.hash and stashfile.hash columns
		 * may have run since the schema was updated, and added entries
		 * with NULL hash columns. Check for this case, and input any
		 * missing hash values.
		 */
		if (fsl_db_g_int32(db, 0, "SELECT hash IS NULL FROM stash "
		    "ORDER BY stashid DESC LIMIT 1")) {
			rc = fsl_db_exec_multi(db, "UPDATE stash"
			    " SET hash=(SELECT uuid FROM blob"
			    "  WHERE blob.rid=stash.vid)"
			    " WHERE hash IS NULL;"
			    "UPDATE stashfile"
			    " SET hash=(SELECT uuid FROM blob"
			    "  WHERE blob.rid=stashfile.rid)"
			    " WHERE hash IS NULL AND rid>0;");
			if (rc != 0)
				return RC_LIBF(rc, "fsl_db_g_int32");
		}
		return FNC_RC_OK;
	}

	if (!fsl_db_table_exists(db, FSL_DBROLE_CKOUT, "stashfile") ||
	    !fsl_db_table_exists(db, FSL_DBROLE_CKOUT, "stash")) {
		/* Tables don't exist; create them from scratch. */
		rc = fsl_db_exec(db, "DROP TABLE IF EXISTS stash;");
		if (rc != 0)
			return RC_LIBF(rc, "fsl_db_exec");
		rc = fsl_db_exec(db, "DROP TABLE IF EXISTS stashfile;");
		if (rc != 0)
			return RC_LIBF(rc, "fsl_db_exec");
		rc = fsl_db_exec_multi(db, stashtab);
		if (rc != 0)
			return RC_LIBF(rc, "fsl_db_exec_multi");
		return FNC_RC_OK;
	}

	/*
	 * Tables exist but aren't necessarily current. Upgrade to the latest
	 * format. Assume the 2011-09-01 format that includes the column
	 * stashfile.isLink. Upgrade the PRIMARY KEY change on 2016-10-16 and
	 * the addition of the "hash" columns on 2019-01-19.
	 */
	rc = fsl_db_exec_multi(db,
	    "ALTER TABLE stash RENAME TO old_stash;"
	    "ALTER TABLE stashfile RENAME TO old_stashfile;");
	if (rc != 0)
		return RC_LIBF(rc, "fsl_db_exec_multi");
	rc = fsl_db_exec_multi(db, stashtab);
	if (rc != 0)
		return RC_LIBF(rc, "fsl_db_exec_multi");
	rc = fsl_db_exec_multi(db,
	    "INSERT INTO stash(stashid,vid,hash,comment,ctime)"
	    " SELECT stashid, vid,"
	    "  (SELECT uuid FROM blob WHERE blob.rid=old_stash.vid),"
	    "  comment, ctime FROM old_stash; "
	    "DROP TABLE old_stash;");
	if (rc != 0)
		return RC_LIBF(rc, "fsl_db_exec_multi");
	rc = fsl_db_exec_multi(db,
	    "INSERT INTO stashfile(stashid, isAdded, isRemoved,"
	    " isExec, isLink, rid, hash, origname, newname, delta)"
	    " SELECT stashid, isAdded, isRemoved, isExec, isLink, rid,"
	    "  (SELECT uuid FROM blob WHERE blob.rid=old_stashfile.rid),"
	    "  origname, newname, delta FROM old_stashfile; "
	    "DROP TABLE old_stashfile;");
	if (rc != 0)
		return RC_LIBF(rc, "fsl_db_exec_multi");

	return FNC_RC_OK;
}

/*
 * Add file(s) at path to the stash changeset identified by stashid based on
 * checkout vid. If path is a directory, all files in that dir will be added.
 * If path is ".", the entire checkout will be stashed from the repository root.
 */
static int
f__stash_path(int stashid, int vid, const char *path)
{
	struct fsl_cx		*f;
	struct fsl_db		*db;
	struct fsl_buffer	 sql = fsl_buffer_empty;  /* query statement */
	struct fsl_stmt		 q = fsl_stmt_empty;	/* vfile statement */
	struct fsl_stmt		 ins = fsl_stmt_empty;	/* insert statement */
	int			 finalize_rc, rc;

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");
	if ((db = fsl_needs_ckout(f)) == NULL)
		return RC(FNC_RC_NO_CKOUT);

	if ((rc = fsl_buffer_reserve(&sql, 512)) != 0)
		return RC_ERRNO("malloc");

	if (buf_printf(&sql,
	    "SELECT deleted, isexe, islink, mrid, pathname,"
	    "  coalesce(origname,pathname)"
	    " FROM vfile WHERE vid=%d"
	    " AND ("
	    "    chnged OR deleted OR ("
	    "        origname IS NOT NULL AND origname<>pathname"
	    "    )"
	    "    OR mrid==0"
	    " )", vid) == -1)
		return RC_LIBF(sql.errCode, "buf_printf");

	if (strcmp(path, ".") != 0) {
		/* specific file(s) provided, not all changes in the ckout */
		if (fsl_buffer_appendf(&sql,
		    " AND (pathname GLOB '%q/*' OR origname GLOB '%q/*'"
		    " OR pathname=%Q OR origname=%Q)",
		    path, path, path, path) != 0) {
			rc = RC_LIBF(sql.errCode, "fsl_buffer_appendf");
			goto end;
		}
	}

	fsl_simplify_sql_buffer(&sql);
	rc = fsl_db_prepare(db, &q, "%s", fsl_buffer_cstr(&sql));
	if (rc != 0) {
		rc = RC_DB(db, rc, "fsl_db_prepare");
		goto end;
	}

	rc = fsl_db_prepare(db, &ins,
	    "INSERT INTO stashfile(stashid, isAdded, isRemoved, isExec, isLink,"
	    "  rid, hash, origname, newname, delta)"
	    "VALUES(%d,:isadd,:isrm,:isexe,:islink,:rid,"
	    "(SELECT uuid FROM blob WHERE rid=:rid),:orig,:new,:content)",
	    stashid);
	if (rc != 0) {
		rc = RC_DB(db, rc, "fsl_db_prepare");
		goto end;
	}

	while ((rc = fsl_stmt_step(&q)) == FSL_RC_STEP_ROW) {
		fsl_buffer	 content = fsl_buffer_empty;
		char		 path[PATH_MAX];
		int		 deleted;
		int		 rid;
		const char	*name, *ogname;

		deleted = fsl_stmt_g_int32(&q, 0);
		rid = fsl_stmt_g_int32(&q, 3);
		name = fsl_stmt_g_text(&q, 4, NULL);
		ogname = fsl_stmt_g_text(&q, 5, NULL);

		if (strlcpy(path, CKOUTDIR, sizeof(path)) >= sizeof(path)) {
			rc = RC(FNC_RC_NO_SPACE, "strlcpy");
			goto end;
		}
		if (strlcat(path, name, sizeof(path)) >= sizeof(path)) {
			rc = RC(FNC_RC_NO_SPACE, "strlcat");
			goto end;
		}

		rc = fsl_stmt_bind_int32_name(&ins, ":rid", rid);
		if (!rc)
			rc = fsl_stmt_bind_int32_name(&ins, ":isadd", rid==0);
		if (!rc)
			rc = fsl_stmt_bind_int32_name(&ins, ":isrm", deleted);
		if (!rc)
			rc = fsl_stmt_bind_int32_name(&ins, ":isexe",
			    fsl_stmt_g_int32(&q, 1));
		if (!rc)
			rc = fsl_stmt_bind_int32_name(&ins, ":islink",
			    fsl_stmt_g_int32(&q, 2));
		if (rc) {
			rc = RC(rc, "fsl_stmt_bind_int32_name");
			goto end;
		}

		rc = fsl_stmt_bind_text_name(&ins, ":orig", ogname, -1, false);
		if (!rc)
			rc = fsl_stmt_bind_text_name(&ins, ":new", name, -1,
			    false);
		if (rc) {
			rc = RC(rc, "fsl_stmt_bind_text_name");
			goto end;
		}

		if (!rid) {	/* new file */
			rc = fsl_buffer_fill_from_filename(&content, path);
			if (rc) {
				rc = RC(rc,
				    "fsl_buffer_fill_from_filename: %s", path);
				goto end;
			}
			rc = fsl_stmt_bind_blob_name(&ins, ":content",
			    content.mem, content.used, false);
			if (rc) {
				rc = RC(rc, "fsl_stmt_bind_blob_name");
				goto clear_delta;
			}
		} else if (deleted) {
			fsl_buffer_clear(&content);
			rc = fsl_stmt_bind_null_name(&ins, ":content");
			if (rc) {
				rc = RC(rc, "fsl_stmt_bind_null_name");
				goto end;
			}
		} else {	/* modified file */
			struct fsl_buffer orig = fsl_buffer_empty;
			struct fsl_buffer disk = fsl_buffer_empty;

			rc = fsl_buffer_fill_from_filename(&disk, path);
			if (rc) {
				rc = RC(rc,
				    "fsl_buffer_fill_from_filename: %s", path);
				goto end;
			}
			rc = fsl_content_get(f, rid, &orig);
			if (rc) {
				rc = RC(rc, "fsl_content_get: %d", rid);
				goto clear_file;
			}
			if (orig.mem == NULL) {
				/*
				 * XXX Empty tracked file.
				 * fsl_buffer_delta_create::fsl_delta_create2()
				 * requires orig->mem to be initialised.
				 */
				rc = fsl_buffer_resize(&orig, UINT64_C(0));
				if (rc) {
					rc = RC_ERRNO("fsl_buffer_resize");
					goto clear_file;
				}
			}
			rc = fsl_buffer_delta_create(&orig, &disk, &content);
			if (rc) {
				rc = RC(rc, "fsl_buffer_delta_create");
				goto clear_file;
			}
			rc = fsl_stmt_bind_blob_name(&ins, ":content",
			    content.mem, content.used, false);
			if (rc)
				rc = RC(rc, "fsl_stmt_bind_blob_name");

clear_file:
			fsl_buffer_clear(&orig);
			fsl_buffer_clear(&disk);
		}
		if (rc)
			goto clear_delta;

		rc = fsl_stmt_bind_int32_name(&ins, ":islink",
		    fsl_is_symlink(path));
		if (rc) {
			rc = RC(rc, "fsl_stmt_bind_int32_name");
			goto clear_delta;
		}

		if ((rc = fsl_stmt_step(&ins)) == FSL_RC_STEP_DONE)
			rc = FNC_RC_OK;
		else {
			if (fsl_db_err_get(db, NULL, NULL) != FNC_RC_OK)
				rc = RC_LIBF(fsl_cx_uplift_db_error(f, db),
				    "fsl_stmt_step");
			else
				rc = RC_LIBF(rc, "fsl_stmt_step");
			goto clear_delta;
		}

		rc = fsl_stmt_reset(&ins);
		if (rc != FNC_RC_OK)
			rc = RC_LIBF(rc, "fsl_stmt_reset");

clear_delta:
		fsl_buffer_clear(&content);
		if (rc != FNC_RC_OK)
			goto end;
	}
	if (rc == FSL_RC_STEP_DONE)
		rc = FNC_RC_OK;
	else {
		if (fsl_db_err_get(db, NULL, NULL) != FNC_RC_OK)
			rc = RC_LIBF(fsl_cx_uplift_db_error(f, db),
			    "fsl_stmt_step");
		else
			rc = RC_LIBF(rc, "fsl_stmt_step");
	}

end:
	fsl_buffer_clear(&sql);
	finalize_rc = fsl_stmt_finalize(&q);
	if (finalize_rc != 0 && rc == 0)
		rc = RC_LIBF(finalize_rc, "fsl_stmt_finalize");
	finalize_rc = fsl_stmt_finalize(&ins);
	if (finalize_rc != 0 && rc == 0)
		rc = RC_LIBF(finalize_rc, "fsl_stmt_finalize");
	return rc;
}

static int
reset_diff_view(struct fnc_view *view, bool scale_position)
{
	struct fnc_diff_view_state	*s = &view->state.diff;
	int				 n, rc;

	n = s->nlines;
	show_diff_status(view);
	rc = create_diff(s);
	if (rc)
		return rc;

	if (scale_position) {
		s->first_line_onscreen = MAX(1,
		    (int)(s->nlines * (double)s->first_line_onscreen / n));
		/*
		 * If the longest line on the page has reduced (e.g., diff
		 * switched from SBS to uni), and current x position is beyond
		 * the new longest line, move back to within the line limits.
		 */
		view->pos.x = MAX(0, MIN(view->pos.x,
		    view->pos.maxx - view->ncols / 2));

	} else {
		s->first_line_onscreen = 1;
		view->pos.x = 0;
	}

	s->matched_line = 0;
	s->last_line_onscreen = MIN(s->nlines,
	    (size_t)s->first_line_onscreen + view->nlines);
	s->selected_line = MIN(s->selected_line,
	    s->last_line_onscreen - s->first_line_onscreen + 1);

	return rc;
}

static int
request_tl_commits(struct fnc_view *view)
{
	struct fnc_tl_view_state	*state = &view->state.timeline;
	int				 rc;

	state->thread_cx.ncommits_needed = view->nscrolled;
	rc = signal_tl_thread(view, 1);
	view->nscrolled = 0;

	return rc;
}

static int
set_selected_commit(struct fnc_diff_view_state *s, struct commit_entry *entry)
{
	fsl_free(s->id2);
	s->id2 = strdup(entry->commit->uuid);
	if (s->id2 == NULL)
		return RC_ERRNO("strdup");

	fsl_free(s->id1);
	if (entry->commit->puuid) {
		s->id1 = strdup(entry->commit->puuid);
		if (s->id1 == NULL)
			return RC_ERRNO("strdup");
	} else
		s->id1 = NULL;

	s->selected_entry = entry->commit;

	return FNC_RC_OK;
}

static void
diff_grep_init(struct fnc_view *view)
{
	struct fnc_diff_view_state *s = &view->state.diff;

	s->matched_line = 0;
}

static int
find_next_match(struct fnc_view *view)
{
	struct fsl_buffer	*b = NULL;
	off_t			*line_offsets = NULL;
	ssize_t			 linelen;
	size_t			 nlines = 0, linesz = 0;
	int			*first, *last, *match, *selected;
	int			 lineno, rc;
	uint8_t			 col = 0;
	char			*line = NULL;

	first = last = match = selected = NULL;
	rc = grep_set_view(view, &b, &line_offsets, &nlines, &first, &last,
	    &match, &selected, &col);
	if (rc != 0)
		return rc;

	if (view->searching == SEARCH_DONE) {
		view->search_status = SEARCH_CONTINUE;
		return FNC_RC_OK;
	}

	if (*match) {
		if (view->searching == SEARCH_FORWARD)
			lineno = *first + *selected;
		else
			lineno = *first + *selected - 2;
	} else {
		if (view->searching == SEARCH_FORWARD)
			lineno = 1;
		else
			lineno = nlines;
	}

	while (1) {
		off_t offset;

		if (lineno <= 0 || (size_t)lineno > nlines) {
			if (*match == 0) {
				view->search_status = SEARCH_CONTINUE;
				break;
			}

			if (view->searching == SEARCH_FORWARD)
				lineno = 1;
			else
				lineno = nlines;
		}

		offset = line_offsets[lineno - 1];
		fsl_buffer_seek(b, offset, FSL_BUFFER_SEEK_SET);

		linelen = fsl_buffer_getline(&line, &linesz, b);
		if (linelen != -1) {
			char *exstr;

			/* expand tabs for accurate rm_so/rm_eo offsets */
			rc = expand_tab(&exstr, NULL, line);
			if (rc) {
				free(line);
				return rc;
			}
			if (regexec(&view->regex, exstr, 1,
			    &view->regmatch, 0) == 0) {
				int *xpos = &view->pos.x;

				*match = lineno;
				view->search_status = SEARCH_CONTINUE;
				/* scroll left/right till match is on-screen */
				while (*xpos > view->regmatch.rm_so)
					--(*xpos);
				while (*xpos + view->ncols <
				    view->regmatch.rm_eo + col)
					++(*xpos);
				free(exstr);
				break;
			}
			free(exstr);
		} else {
			if (b->errCode != 0) {
				free(line);
				return RC_LIBF(b->errCode,
				    "fsl_buffer_getline");
			}
			break;
		}
		if (view->searching == SEARCH_FORWARD)
			++lineno;
		else
			--lineno;
	}
	free(line);

	if (*match) {
		/* scroll view if it's not on the current page */
		if (!(*match >= *first && *match <= *last))
			*first = MAX(*match - view->nlines / 3, 1);
		*selected = *match - *first + 1;
	}

	return FNC_RC_OK;
}

static int
grep_set_view(struct fnc_view *view, struct fsl_buffer **b,
    off_t **line_offsets, size_t *nlines, int **first, int **last,
    int **match, int **selected, uint8_t *startx)
{
	if (view->vid == FNC_VIEW_DIFF) {
		struct fnc_diff_view_state *s = &view->state.diff;

		*b = &s->buf;
		*nlines = s->nlines;
		*line_offsets = s->line_offsets;
		*match = &s->matched_line;
		*first = &s->first_line_onscreen;
		*last = &s->last_line_onscreen;
		*selected = &s->selected_line;
		if (s->showln) {
			int d = s->nlines, n = 0;

			ndigits(n, d);
			*startx = n + 3;  /* {ap,pre}pended ' ' + line sep */
		}
	} else if (view->vid == FNC_VIEW_BLAME) {
		struct fnc_blame_view_state *s = &view->state.blame;

		*b = &s->blame.buf;
		*nlines = s->blame.nlines;
		*line_offsets = s->blame.line_offsets;
		*match = &s->matched_line;
		*first = &s->first_line_onscreen;
		*last = &s->last_line_onscreen;
		*selected = &s->selected_line;
		if (s->showln) {
			int d = s->blame.nlines, n = 0;

			ndigits(n, d);
			*startx = n + 3;  /* {ap,pre}pended ' ' + line sep */
		}
		*startx += 11;  /* id field */
	} else
		return RC(FNC_RC_NYI, "view %d does not support grep",
		    view->vid);

	return FNC_RC_OK;
}

static void
free_diff_state(struct fnc_diff_view_state *s)
{
	free_colours(&s->colours);

	free(s->id1);
	s->id1 = NULL;
	free(s->id2);
	s->id2 = NULL;

	free(s->line_offsets);
	s->line_offsets = NULL;
	s->nlines = 0;

	free(s->dlines);
	s->dlines = NULL;
	s->ndlines = 0;

	if (s->buf.capacity > 0)
		fsl_buffer_clear(&s->buf);
}

static int
close_diff_view(struct fnc_view *view)
{
	free_diff_state(&view->state.diff);
	return FNC_RC_OK;
}

static int
reset_tags(struct fnc_tl_view_state *s)
{
	struct timeline_tag		*t = &s->tag;
	struct fnc_commit_artifact	*c = t->two;

	t->one = NULL;

	if (!t->ogrid)
		return FNC_RC_OK;

	free(c->puuid);
	c->puuid = NULL;

	/* restore commit's original parent */
	if (t->ogid != NULL) {
		c->puuid = strdup(t->ogid);
		free(t->ogid);
		t->ogid = NULL;
		if (c->puuid == NULL)
			return RC_ERRNO("strdup");
	}

	c->dswidths = 0;
	c->maxpathlen = 0;
	c->diff_type = *c->type == 'c' ? FNC_DIFF_COMMIT : FNC_DIFF_WIKI;
	c->prid = t->ogrid;
	t->ogrid = 0;
	t->two = NULL;
	s->showmeta = true;

	return FNC_RC_OK;
}

static void
fnc_resizeterm(void)
{
	struct winsize	size;
	int		cols, lines;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) < 0) {
		cols = 80;
		lines = 24;
	} else {
		cols = size.ws_col;
		lines = size.ws_row;
	}
	resize_term(lines, cols);
}

static int
view_resize(struct fnc_view *view)
{
	int dif, nlines, ncols, rc = FNC_RC_OK;

	dif = LINES - view->lines;	/* line difference if resized */

	if (view->lines > LINES)
		nlines = view->nlines - (view->lines - LINES);
	else
		nlines = view->nlines + (LINES - view->lines);

	if (view->cols > COLS)
		ncols = view->ncols - (view->cols - COLS);
	else
		ncols = view->ncols + (COLS - view->cols);

	if (view->child) {
		int is_hsplit = view->child->begin_y;

		if (!view_is_fullscreen(view))
			view->child->begin_x =
			    view_split_getx(view->begin_x);
		if (view->mode == VIEW_SPLIT_HRZN ||
		    view->child->begin_x == 0) {
			ncols = COLS;

			rc = make_fullscreen(view->child);
			if (rc != FNC_RC_OK)
				return rc;
			if (view->child->active)
				show_panel(view->child->panel);
			else
				show_panel(view->panel);
		} else {
			ncols = view->child->begin_x;

			rc = make_splitscreen(view->child);
			if (rc != FNC_RC_OK)
				return rc;
			show_panel(view->child->panel);
		}
		/*
		 * XXX This is ugly and needs to be moved into the above
		 * logic but "works" for now and my attempts at moving it
		 * break either 'tab' or 'F' key maps in horizontal splits.
		 */
		if (is_hsplit) {
			rc = make_splitscreen(view->child);
			if (rc)
				return rc;
			if (dif < 0) { /* top split decreased */
				rc = view_offset_scrolldown(view);
				if (rc)
					return rc;
			}
			drawborder(view);
			update_panels();
			doupdate();
			show_panel(view->child->panel);
			nlines = view->nlines;
		}
	} else if (view->parent == NULL)
		ncols = COLS;

	if (view->resize != NULL && dif > 0) {
		rc = view->resize(view, dif);
		if (rc)
			return rc;
	}

	if (wresize(view->window, nlines, ncols) == ERR)
		return RC(FNC_RC_CURSES, "wresize");
	if (replace_panel(view->panel, view->window) == ERR)
		return RC(FNC_RC_CURSES, "replace_panel");
	wclear(view->window);

	view->nlines = nlines;
	view->ncols = ncols;
	view->lines = LINES;
	view->cols = COLS;

	return FNC_RC_OK;
}

static int
resize_timeline_view(struct fnc_view *view, int increase)
{
	struct fnc_tl_view_state	*s = &view->state.timeline;
	int				 n = 0;

	if (s->selected_entry)
		n = s->selected_entry->idx + view->lines - s->selected;

	/*
	 * Request commits to account for the view's increased
	 * height so we have enough to fully populate the view.
	 */
	if (s->commits.ncommits < n) {
		view->nscrolled = n - s->commits.ncommits + increase + 1;
		return request_tl_commits(view);
	}

	return FNC_RC_OK;
}

static void
sigwinch_handler(int sig)
{
	if (sig == SIGWINCH) {
		struct winsize winsz;

		ioctl(0, TIOCGWINSZ, &winsz);
		fnc__recv_sigwinch = 1;
	}
}

static void
sigpipe_handler(int sig)
{
	struct sigaction	sact;
	int			e;

	(void)sig;
	fnc__recv_sigpipe = 1;
	memset(&sact, 0, sizeof(sact));
	sact.sa_handler = SIG_IGN;
	sact.sa_flags = SA_RESTART;
	e = sigaction(SIGPIPE, &sact, NULL);
	if (e)
		err(1, "SIGPIPE");
}

static void
sigcont_handler(int sig)
{
	(void)sig;
	fnc__recv_sigcont = 1;
}

static void
sigint_handler(int signo)
{
	(void)signo;
	fnc__recv_sigint = 1;
}

static void
sigterm_handler(int signo)
{
	(void)signo;
	fnc__recv_sigterm = 1;
}

static bool
fatal_signal(void)
{
	return (fnc__recv_sigpipe || fnc__recv_sigint || fnc__recv_sigterm);
}

static void
list_commands(FILE *f)
{
	size_t i;

	fprintf(f, "commands:");
	for (i = 0; i < nitems(fnc_commands); ++i) {
		const struct fnc_cmd *cmd = &fnc_commands[i];

		if (cmd && cmd->name)
			fprintf(f, " %s", cmd->name);
	}
	fputc('\n', f);
}

/*
 * Print usage with cmd->help values. If cmd is NULL, print help_global
 * usage. If rc is zero, output full usage and help text to stdout; if
 * nonzero, only output usage to stderr. Usage takes the following form:
 *
 *   fnc cmd-name [flag-opts] [opts-with-arg] [optional-operands] operands
 *   e.g., fnc blame [-Chr] [-c commit] [-l lineno] [-n n] path
 */
__dead static void
usage(const struct fnc_cmd *cmd, int rc)
{
	const struct cmd_help	*h, *hl, *hs;
	FILE			*f = rc ? stderr : stdout;

	if (rc)
		fprintf(f, "usage: ");
	fprintf(f, "%s", fnc__progname);
	if (cmd != NULL)
		fprintf(f, " %s", cmd->name);

	h = hl = hs = cmd != NULL ? cmd->help : help_global;

	/* first, print any flag opts (that take no argument): [-abc] */
	for (; h->sopt != 0; ++h) {
		if (h->arg == NULL) {
			fprintf(f, " [-");
			for (; hs->sopt != 0; ++hs) {
				if (hs->arg == NULL)
					fputc(hs->sopt, f);
			}
			fprintf(f, "]");
			break;
		}
	}

	/* next, print opts that take an argument: [-o value] */
	for (; hl->lopt != NULL || hl->sopt != 0; ++hl) {
		if (hl->arg != NULL) {
			fprintf(f, " [-");
			if (hl->sopt != 0)
				fputc(hl->sopt, f);
			else
				fprintf(f, "-%s", hl->lopt);
			fprintf(f, " %s]", hl->arg);
		}
	}

	/* last, print any operands: [optional-operands] mandatory-operands */
	if (cmd == NULL) {
		fprintf(f, " [command] [options] [arg ...]\n");
		if (rc == FNC_RC_OK) {
			show_help(help_global, false);
			printf("note: '%s' defaults to the timeline command\n"
			    "      '%s <path>' invokes '%s timeline <path>'\n"
			    "      '%s <cmd> -h' for command specific help\n\n",
			    fnc__progname, fnc__progname,
			    fnc__progname, fnc__progname);
		}
		list_commands(f);
	} else if (cmd->f == fnc_commands[FNC_VIEW_BLAME].f)
		fprintf(f, " path\n");
	else if (cmd->f == fnc_commands[FNC_VIEW_BRANCH].f)
		fprintf(f, " [glob]\n");
	else if (cmd->f == fnc_commands[FNC_VIEW_CONFIG].f)
		fprintf(f, " [option [value]]\n");
	else if (cmd->f == fnc_commands[FNC_VIEW_DIFF].f)
		fprintf(f, " [artifact1 [artifact2]] [path ...]\n");
	else if (cmd->f == fnc_commands[FNC_VIEW_STASH].f)
		fprintf(f, " [(get|pop) [id]]\n");
	else if (cmd->f == fnc_commands[FNC_VIEW_TIMELINE].f)
		fprintf(f, " [path]\n");
	else if (cmd->f == fnc_commands[FNC_VIEW_TREE].f)
		fprintf(f, " [path]\n");

	if (cmd != NULL && rc == FNC_RC_OK) {
		if (cmd->aliases != NULL)
			fnc_cmd_aliases(cmd->aliases);
		if (cmd->help != NULL)
			show_help(cmd->help,
			    cmd->f == fnc_commands[FNC_VIEW_STASH].f);
	}

	fnc_cx_close();
	exit(rc);
}

static void
show_help(const struct cmd_help *h, bool stash)
{
	putchar('\n');
	if (stash) {
		printf("  get [<id>]\n\t"
		    "Apply the latest stash or stash <id> changeset "
		    "to the work tree.\n\n");
		printf("  pop [<id>]\n\t"
		    "Like get, but remove changeset from the stash cache.\n\n");
	}

	for (; h->sopt || h->lopt; ++h) {
		char		 b[2];
		const char	*l = h->lopt;
		const char	*a = h->arg;
		const char	*s;

		if (snprintf(b, sizeof(b), "%c", h->sopt) < 0)
			s = NULL;
		else
			s = b;

		printf("  %s%s%s%s%s%s%s%s",
		    s ? "-" : "", s ? s : "", (s && l) ? ", " : "",
		    l ? "--" : "", l ? l : "", a ? " <" : "",
		    a ? a : "", a ? ">" : "");

		if (h->txt)
			printf("\n\t%s", h->txt);

		printf("\n\n");
	}
}

static void
fnc_cmd_aliases(const char *aliases)
{
	char const *alias = aliases;

	printf("\t(aliases: ");
	while (alias && *alias) {
		printf("%s%s", alias, *(strchr(alias, 0) + 1) ?
		    ", " : ")\n");
		alias = strchr(alias, '\0') + 1;
	}
}

static int
fnc_stash_get(const char *id, bool pop)
{
	struct fsl_cx		*f;
	struct fsl_db		*db;
	char			*comment, *date;
	fsl_uuid_str		 hash;
	long			 stashid;
	int			 rc;

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");
	if ((db = fsl_needs_ckout(f)) == NULL)
		return RC(FNC_RC_NO_CKOUT);

	if (id != NULL) {
		rc = xstrtonum(&stashid, id, 1, INT_MAX);
		if (rc)
			return rc;
	} else {
		stashid = fsl_db_g_int32(db, 0,
		    "SELECT max(stashid) FROM stash");
		if (!stashid) {
			f_out("empty stash\n");
			return FNC_RC_OK;
		}
	}

	if (!fsl_db_exists(db, "SELECT 1 FROM stash WHERE stashid=%d",
	    stashid)) {
		f_out("no such stash: %d\n", stashid);
		return FNC_RC_OK;
	}

	comment = fsl_db_g_text(db, NULL,
	    "SELECT comment FROM stash WHERE stashid=%d", stashid);
	date = fsl_db_g_text(db, NULL, "SELECT datetime(ctime) "
	    "FROM stash WHERE stashid=%d", stashid);
	hash = fsl_db_g_text(db, NULL,
	    "SELECT hash FROM stash WHERE stashid=%d", stashid);

	rc = f__stash_get(stashid, pop);
	if (rc)
		goto end;

	f_out("\n%s stash:\n%5d: [%.14s] from %s\n",
	    pop ? "Popped" : "Applied", stashid, hash, date);
	if (comment && *comment)
		f_out("        %s\n", comment);

	if (pop) {
		rc = fsl_db_exec_multi(db, "DELETE FROM stash WHERE stashid=%d;"
		    "DELETE FROM stashfile WHERE stashid=%d;",
		    stashid, stashid);
	}

end:
	free(comment);
	free(date);
	free(hash);
	return rc;

}

static int
cmd_stash(int argc, char **argv)
{
	struct fsl_cx			*f;
	struct fnc_view			*view = NULL;
	struct fnc_commit_artifact	*commit = NULL;
	const char			*id;
	fsl_id_t			 rid;
	long				 context = -1;
	int				 ch, flags;
	int				 rc;
	bool				 colour = true;

	if ((rc = fnc_cx_open(NULL)) != 0)
		return rc;
	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");
	if (!fsl_cx_has_ckout(f))
		return RC(FNC_RC_NO_CKOUT);

	flags = FNC_DIFF_VERBOSE | FNC_DIFF_PROTOTYPE;

#ifdef __OpenBSD__
	if (pledge("stdio rpath wpath cpath fattr flock tty unveil",
	    NULL) == -1)
		return RC_ERRNO("pledge");
#endif
	while ((ch = getopt_long(argc, argv, "+ChPx:", stash_opt,
	    NULL)) != -1) {
		switch (ch) {
		case 'C':
			colour = false;
			break;
		case 'h':
			usage_stash(0);
			/* NOTREACHED */
		case 'P':
			FLAG_CLR(flags, FNC_DIFF_PROTOTYPE);
			break;
		case 'x':
			/* ignore invalid numbers, fallback to default */
			xstrtonum(&context, optarg, 0, MAX_DIFF_CTX);
			break;
		default:
			usage_stash(1);
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;

	if (argc  > 2) {
		usage_stash(1);
		/* NOTREACHED */
	}

	if (argc) {
		bool pop = false;

		if (strcmp(*argv, "get") != 0 && strcmp(*argv, "apply") != 0 &&
		    !(pop = strcmp(*argv, "pop") == 0)) {
			fprintf(stderr, "%s: invalid stash subcommand: %s\n",
			    fnc__progname, *argv);
			usage_stash(1);
			/* NOTREACHED */
		}

		if (chdir(CKOUTDIR) == -1)	/* XXX ugly hack! */
			return RC_ERRNO("chdir");

		return fnc_stash_get(argc == 2 ? argv[1] : NULL, pop);
	}

	fsl_ckout_version_info(f, &rid, &id);
	rc = fsl_ckout_changes_scan(f);
	if (rc)
		return RC(rc, "fsl_ckout_changes_scan");

	if (!fsl_ckout_has_changes(f)) {
		printf("no local changes\n");
		return FNC_RC_OK;
	}

	commit = calloc(1, sizeof(*commit));
	if (commit == NULL)
		return RC_ERRNO("calloc");

	commit->puuid = strdup(id);
	if (commit->puuid == NULL) {
		rc = RC_ERRNO("strdup");
		goto end;
	}

	commit->uuid = strdup(commit->puuid);
	if (commit->uuid == NULL) {
		rc = RC_ERRNO("strdup");
		goto end;
	}

	commit->type = strdup("checkin");
	if (commit->type == NULL) {
		rc = RC_ERRNO("strdup");
		goto end;
	}

	commit->rid = rid;
	commit->prid = rid;
	commit->diff_type = FNC_DIFF_CKOUT;

	rc = init_curses(colour);
	if (rc)
		goto end;

	/*
	 * XXX revert_ckout() calls fsl_ckout_revert()^, which walks the tree
	 * from / to the ckout stat(2)ing every dir so we need rx on root.
	 * Revoke privileges on root after returning from revert_ckout().
	 * ^fsl__vfile_to_ckout() -> fsl_mkdir_for_file() -> fsl_dir_check()
	 */
	rc = init_unveil(
	    ((const char *[]){ "/", REPODIR, CKOUTDIR, fnc__tmpdir, tzfile() }),
	    ((const char *[]){ "rx", "rwc", "rwc", "rwc", "r" }), 5, false
	);
	if (rc != 0)
		goto end;

	view = view_open(0, 0, 0, 0, FNC_VIEW_DIFF);
	if (view == NULL) {
		rc = RC(FNC_RC_CURSES, "view_open");
		goto end;
	}

	rc = open_diff_view(view, commit, NULL, DIFF_MODE_STASH,
	    context, flags, colour, 0);
	if (rc)
		goto end;

	rc = show_diff_view(view);
	if (rc)
		goto end;

	/* XXX ugly hack! need to normalize paths with worktree virtual root */
	if (chdir(CKOUTDIR) == -1) {
		rc = RC_ERRNO("chdir");
		goto end;
	}

	rc = fnc_stash(view);
	if (rc)
		goto end;

	/*
	 * We must check for changes based on file content--not mtime--else
	 * the lib will report files as unchanged in some cases.
	 */
	rc = fsl_vfile_changes_scan(f, rid, FSL_VFILE_CKSIG_HASH);

end:
	if (commit != NULL)
		fnc_commit_artifact_close(commit);
	if (view != NULL) {
		int crc;

		if (view->close == NULL) {
			crc = close_diff_view(view);
			if (crc != 0 && rc == 0)
				rc = crc;
		}
		crc = view_close(view);
		if (crc != 0 && rc == 0)
			rc = crc;
	}
	return rc;
}

/* order from most to least commonly wanted type */
#define FNC_OBJ_TYPE_ANY	0
#define FNC_OBJ_TYPE_COMMIT	1
#define FNC_OBJ_TYPE_BLOB	2
#define FNC_OBJ_TYPE_WIKI	3
#define FNC_OBJ_TYPE_FORUM	4
#define FNC_OBJ_TYPE_TAG	5
#define FNC_OBJ_TYPE_TECHNOTE	6
#define FNC_OBJ_TYPE_TICKET	7

/*
 * If want is set to FNC_OBJ_TYPE_ANY, return the type of the object resolved
 * by rid in out parameter *type. If want is set to a specific FNC_OBJ_TYPE_*,
 * run only the single query to test if rid is of the wanted type. If the
 * object is of the wanted type, *type will equal want, otherwise *type
 * will equal FNC_OBJ_TYPE_ANY. Return zero on success, non-zero on error.
 */
static int
object_get_type(int *type, int32_t rid, int want)
{
	static const char *queries[] = {
		/* must match FNC_OBJ_TYPE_* order */
		"SELECT 1 FROM event WHERE objid=%"PRIi32" AND type='ci'",
		"SELECT 1 FROM mlink WHERE fid = %"PRIi32,
		"SELECT 1 FROM tagxref WHERE rid = %"PRIi32" AND tagid IN"
		"    (SELECT tagid FROM tag WHERE tagname GLOB 'wiki-*')",
		"SELECT 1 FROM event WHERE objid=%"PRIi32" AND type='f'",
		"SELECT 1 FROM event WHERE objid=%"PRIi32" AND type='g'",
		"SELECT 1 FROM tagxref WHERE rid=%"PRIi32" AND tagid IN"
		"    (SELECT tagid FROM tag WHERE tagname GLOB 'event-*')",
		"SELECT 1 FROM tagxref WHERE rid=%"PRIi32" AND tagid IN"
		"    (SELECT tagid FROM tag WHERE tagname GLOB 'tkt-*')",
	};
	struct fsl_cx	*f;
	struct fsl_db	*db;
	struct fsl_stmt	 q = fsl_stmt_empty;
	int		 i, rc;

	*type = FNC_OBJ_TYPE_ANY;

	if ((f = fcli_cx()) == NULL)
		return RC_LIBF(FNC_RC_FATAL, "fcli_cx");
	if ((db = fsl_cx_db_repo(f)) == NULL)
		return RC(FNC_RC_NO_REPO);

	if (rid < 1)
		return RC(FNC_RC_RANGE, "object rid must be a positive int");

	for (i = 0; i < FNC_OBJ_TYPE_TICKET; ++i) {
		if (want != FNC_OBJ_TYPE_ANY && want != i + 1)
			continue;

		rc = fsl_db_prepare(db, &q, queries[i], rid);
		if (rc != 0)
			return RC_DB(db, rc, "fsl_db_prepare");

		if (fsl_stmt_step(&q) == FSL_RC_STEP_ROW) {
			*type = i + 1;
			break;
		}
		if (db->error.code != 0) {
			rc = RC_LIBF(db->error.code, "fsl_stmt_step");
			break;
		}

		fsl_stmt_finalize(&q);
	}

	fsl_stmt_finalize(&q);
	return rc;
}

/*
 * If there is one argument of a valid commit reference, show any changes
 * between it and the work tree. If there are two arguments and both are
 * valid references of the same type, show the differences between the two
 * objects. If path arguments are specified after one or two commit refs,
 * filter the diff to only show changes to files at the specified paths.
 * e.g.:
 *   diff file1 and file2 in the work tree vs checked-out base commit
 *	fnc diff file1 file2
 *   diff file1 in the work tree vs commit abc012
 *	fnc diff abc012 file1
 *   diff file1 and file2 between commits def345 vs ace789
 *	fnc diff def345 ace789 file1 file2
 */
static int
cmd_diff(int argc, char **argv)
{
	struct fsl_cx			*f;
	struct fnc_view			*view = NULL;
	struct fnc_commit_artifact	*commit = NULL;
	struct fnc_pathlist_head	 paths;
	struct fnc_pathlist_entry	*pe;
	struct fsl_stmt			 q = fsl_stmt_empty;
	const char			*artifact1 = NULL, *artifact2 = NULL;
	const char			*repo = NULL;
	fsl_id_t			 prid = -1, rid = -1;
	long				 context = -1;
	int				 i, ch, flags, wrap = 0;
	int				 type1 = FNC_OBJ_TYPE_ANY;
	int				 type2 = FNC_OBJ_TYPE_ANY;
	int				 finalize_rc, rc;
	bool				 colour = true;
	bool				 nocurses = false;
	enum fnc_diff_type		 diff_type = FNC_DIFF_CKOUT;
	enum fnc_diff_mode		 diff_mode = DIFF_MODE_NORMAL;

	flags = FNC_DIFF_VERBOSE | FNC_DIFF_PROTOTYPE;

	while ((ch = getopt_long(argc, argv, "+bCDhiloPqr:sWwx:z", diff_opt,
	    NULL)) != -1) {
		switch (ch) {
		case 'b':
			FLAG_SET(flags, FNC_DIFF_BRIEF);
			break;
		case 'C':
			colour = false;
			break;
		case 'D':
			FLAG_SET(flags, FNC_DIFF_STATMIN);
			break;
		case 'h':
			usage_diff(0);
			/* NOTREACHED */
		case 'i':
			FLAG_SET(flags, FNC_DIFF_INVERT);
			break;
		case 'l':
			FLAG_SET(flags, FNC_DIFF_LINENO);
			break;
		case 'o':
			nocurses = true;
			break;
		case 'P':
			FLAG_CLR(flags, FNC_DIFF_PROTOTYPE);
			break;
		case 'q':
			FLAG_CLR(flags, FNC_DIFF_VERBOSE);
			break;
		case 'r':
			repo = optarg;
			break;
		case 's':
			if (FLAG_CHK(flags, FNC_DIFF_LINENO))
				warnx("-l and -s are mutually exclusive: "
				    "ignoring -l");
			FLAG_SET(flags, FNC_DIFF_SIDEBYSIDE);
			break;
		case 'W':
			wrap = 1;
			break;
		case 'w':
			FLAG_SET(flags, FNC_DIFF_IGNORE_ALLWS);
			break;
		case 'x':
			/* ignore invalid values and fallback to default */
			xstrtonum(&context, optarg, 0, MAX_DIFF_CTX);
			break;
		case 'z':
			fnc__utc = true;
			break;
		default:
			usage_diff(1);
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;

	TAILQ_INIT(&paths);

	if ((rc = fnc_cx_open(repo)) != 0)
		return rc;
	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	/* this is where the half-smart ui tries to be friendly */
	if (argc > 0) {
		rc = symtorid(&prid, argv[0], FSL_SATYPE_ANY);
		if (rc != 0) {
			if (rc != FNC_RC_NO_COMMIT &&
			    rc != FNC_RC_NO_REF &&
			    rc != FNC_RC_NO_TAG)
				return rc;
			rc = 0;
		}
	}
	if (prid > 0) {
		rc = object_get_type(&type1, prid, FNC_OBJ_TYPE_ANY);
		if (rc != 0)
			return rc;
		artifact1 = argv[0];
		++argv;
		--argc;
	}
	if (argc > 0 && prid > 0) {
		rc = symtorid(&rid, argv[0], FSL_SATYPE_ANY);
		if (rc != 0) {
			if (rc != FNC_RC_NO_COMMIT &&
			    rc != FNC_RC_NO_REF &&
			    rc != FNC_RC_NO_TAG)
				return rc;
			rc = 0;
		}
	}
	if (rid > 0) {
		rc = object_get_type(&type2, rid, FNC_OBJ_TYPE_ANY);
		if (rc != 0)
			return rc;
		artifact2 = argv[0];
		++argv;
		--argc;
	}
	if (argc > 1) {
		/*
		 * If the first argument was not a valid hash but the second
		 * argument is, this is not a valid diff invocation: paths
		 * must come after a valid hash or with no specified hashes.
		 */
		if (fsl_sym_to_rid(f, argv[1], FSL_SATYPE_ANY, &prid) == 0)
			return RC(FNC_RC_NO_REF, "%s", argv[0]);
	}

	if (artifact1 == NULL || artifact2 == NULL) {
		if (!fsl_needs_ckout(f))
			return RC(FNC_RC_NO_CKOUT);
	}
	if (rid > 0 && prid > 0 && type1 != type2)
		return RC(FNC_RC_BAD_ARTIFACT, "object types do not match");

	switch (type1) {
	case FNC_OBJ_TYPE_COMMIT:
		if (type1 == type2)
			diff_type = FNC_DIFF_COMMIT;
		break;
	case FNC_OBJ_TYPE_BLOB:
		diff_type = FNC_DIFF_BLOB;
		break;
	case FNC_OBJ_TYPE_TAG:
	case FNC_OBJ_TYPE_WIKI:
	case FNC_OBJ_TYPE_FORUM:
	case FNC_OBJ_TYPE_TICKET:
	case FNC_OBJ_TYPE_TECHNOTE:
		diff_type = FNC_DIFF_WIKI;
		break;
	}

	if (artifact1 == NULL && diff_type != FNC_DIFF_BLOB) {
		artifact1 = "current";
		rc = symtorid(&prid, artifact1, FSL_SATYPE_CHECKIN);
		if (rc != 0)
			return rc;
	}

	if (artifact2 == NULL && diff_type != FNC_DIFF_BLOB) {
		fsl_ckout_version_info(f, &rid, NULL);
		rc = fsl_ckout_changes_scan(f);
		if (rc)
			return RC(rc, "fsl_ckout_changes_scan");
		if (diff_type == FNC_DIFF_CKOUT && !fsl_ckout_has_changes(f)) {
			fprintf(stdout, "no local changes\n");
			return FNC_RC_OK;
		}
	}

	/* parse remaining arguments as paths */
	for (i = 0; i < argc && diff_type != FNC_DIFF_BLOB; ++i) {
		struct fnc_pathlist_entry	*ins;
		char				*path;

		if (type1 != FNC_OBJ_TYPE_COMMIT && artifact2 != NULL) {
			rc = RC(FNC_RC_BAD_ARTIFACT,
			    "paths can only be specified with commit objects");
			goto end;
		}
		/* first check for path in the "from" version of the diff */
		rc = resolve_path(&path, argv[i], prid);
		if (rc != FNC_RC_OK) {
			if (rc != FNC_RC_NO_PATH) {
				if (rc == FSL_RC_TYPE && artifact2 == NULL)
					rc = RC(FNC_RC_NO_REF, "%s", argv[i]);
				goto end;
			}

			/* check if path is in the "to" version of the diff */
			rc = resolve_path(&path, argv[i],
			    artifact2 == NULL ? 0 : rid);
			if (rc != FNC_RC_OK)
				goto end;
		}
		if (path == NULL)
			break;	/* work tree root, diff the whole tree */

		rc = fnc_pathlist_insert(&ins, &paths, path, NULL);
		if (rc || ins == NULL)	/* NULL == duplicate path */
			free(path);
		if (rc)
			goto end;
	}

	if (diff_type != FNC_DIFF_BLOB && diff_type != FNC_DIFF_CKOUT) {
		rc = commit_builder(&commit, rid, &q);
		if (rc)
			goto end;

		if (commit->prid == prid)
			diff_mode = DIFF_MODE_META;
		else {
			free(commit->puuid);
			commit->puuid = fsl_rid_to_uuid(f, prid);
			if (commit->puuid == NULL) {
				rc = RC_LIBF(rc, "fsl_rid_to_uuid");
				goto end;
			}
			commit->prid = prid;
		}
	} else {
		commit = calloc(1, sizeof(*commit));
		if (commit == NULL) {
			rc = RC_ERRNO("calloc");
			goto end;
		}

		commit->puuid = fsl_rid_to_uuid(f, prid);
		if (commit->puuid == NULL) {
			rc = RC_LIBF(rc, "fsl_rid_to_uuid");
			goto end;
		}

		commit->uuid = fsl_rid_to_uuid(f, rid);
		if (commit->uuid == NULL) {
			rc = RC_LIBF(rc, "fsl_rid_to_uuid");
			goto end;
		}

		commit->type = strdup("blob");
		if (commit->type == NULL) {
			rc = RC_ERRNO("strdup");
			goto end;
		}

		commit->rid = rid;
		commit->prid = prid;
		commit->diff_type = diff_type;
	}

	if (!nocurses) {
		rc = init_curses(colour);
		if (rc)
			goto end;

		view = view_open(0, 0, 0, 0, FNC_VIEW_DIFF);
		if (view == NULL) {
			rc = RC(FNC_RC_CURSES, "view_open");
			goto end;
		}

	}

	rc = init_unveil(
		((const char *[]){ REPODB, CKOUTDIR, fnc__tmpdir, tzfile() }),
		((const char *[]){ "rw", "rwc", "rwc", "r" }), 4, 1
	);
	if (rc != 0)
		goto end;

	rc = open_diff_view(view, commit, &paths, diff_mode,
	    context, flags, colour, wrap);
	if (rc || nocurses)
		goto end;
	rc = view_loop(view);

end:
	if (commit != NULL)
		fnc_commit_artifact_close(commit);
	TAILQ_FOREACH(pe, &paths, entry)
		free((char *)pe->path);
	fnc_pathlist_free(&paths);
	finalize_rc = fsl_stmt_finalize(&q);
	if (finalize_rc != 0 && rc == 0)
		rc = RC_LIBF(finalize_rc, "fsl_stmt_finalize");
	return rc;
}

/*
 * Check that path is a valid file or dir in the tree identified by rid.
 * If rid is < 1, use the work tree if it exists else the repository tip.
 */
static int
map_version_path(const char *path, fsl_id_t rid)
{
	struct fsl_cx		*f;
	struct fsl_deck		 d = fsl_deck_empty;
	const fsl_card_F	*cf;
	size_t			 len;
	int			 rc;

	if (path == NULL || *path == '\0')
		return FNC_RC_OK;
	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	if (rid > 0) {
		rc = fsl_deck_load_rid(f, &d, rid, FSL_SATYPE_CHECKIN);
		if (rc != 0) {
			rc = RC_LIBF(rc, "fsl_deck_load_rid: %d", rid);
			goto end;
		}
	} else {
		const char *ref;

		ref = fsl_cx_has_ckout(f) ? "current" : "tip";

		rc = fsl_deck_load_sym(f, &d, ref, FSL_SATYPE_CHECKIN);
		if (rc != 0) {
			rc = RC_LIBF(rc, "fsl_deck_load_sym: %d", ref);
			goto end;
		}
	}

	cf = fsl_deck_F_search(&d, path);
	if (cf != NULL)
		goto end;

	rc = fsl_deck_F_rewind(&d);
	if (rc != 0) {
		rc = RC_LIBF(rc, "fsl_deck_F_rewind");
		goto end;
	}

	len = strlen(path);
	if (path[len - 1] == '/')
		--len;

	for (;;) {
		rc = fsl_deck_F_next(&d, &cf);
		if (rc != 0) {
			rc = RC_LIBF(rc, "fsl_deck_F_next");
			goto end;
		}
		if (cf == NULL || strncmp(path, cf->name, len) == 0)
			break;
	}

	if (cf == NULL) {
		char *id = NULL;

		if (rid <= 0)
			fsl_ckout_version_info(f, &rid, NULL);
		else if (rid > 0)
			id = fsl_rid_to_uuid(f, rid);
		if (id != NULL)
			rc = RC(FNC_RC_NO_PATH, "%s [%.10s]", path, id);
		else
			rc = RC(FNC_RC_NO_PATH, "%s", path);
		free(id);
	}

end:
	fsl_deck_finalize(&d);
	return rc;
}

static int
browse_commit_tree(struct fnc_view **new_view, int begin_y, int begin_x,
    struct commit_entry *entry, const char *path, int color)
{
	struct fnc_view	*tree_view;
	int		 rc;

	tree_view = view_open(0, 0, begin_y, begin_x, FNC_VIEW_TREE);
	if (tree_view == NULL)
		return RC(FNC_RC_CURSES, "view_open");

	rc = open_tree_view(tree_view, path, entry->commit->rid, color);
	if (rc)
		return rc;

	*new_view = tree_view;
	return FNC_RC_OK;
}

static int
cmd_tree(int argc, char **argv)
{
	struct fsl_cx	*f;
	struct fnc_view	*view;
	const char	*commit = NULL, *repo = NULL;
	char		*path = NULL;
	fsl_id_t	 rid = 0;
	int		 ch, rc;
	bool		 colour = true;

	while ((ch = getopt_long(argc, argv, "+Cc:hr:z",
	    tree_opt, NULL)) != -1) {
		switch (ch) {
		case 'C':
			colour = false;
			break;
		case 'c':
			commit = optarg;
			break;
		case 'h':
			usage_tree(0);
			/* NOTREACHED */
		case 'r':
			repo = optarg;
			break;
		case 'z':
			fnc__utc = true;
			break;
		default:
			usage_tree(1);
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;

	if (argc > 1) {
		usage_tree(1);
		/* NOTREACHED */
	}

	if ((rc = fnc_cx_open(repo)) != 0)
		return rc;
	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	if (commit != NULL) {
		rc = symtorid(&rid, commit, FSL_SATYPE_CHECKIN);
		if (rc != 0)
			return rc;
	} else if (fsl_cx_has_ckout(f)) {
		fsl_ckout_version_info(f, &rid, NULL);
	} else {
		/* 'fnc tree -r repo.db [path]' case */
		rc = symtorid(&rid, "tip", FSL_SATYPE_CHECKIN);
		if (rc != 0)
			return rc;
	}

	if (!fsl_rid_is_a_checkin(f, rid))
		return RC(FNC_RC_BAD_ARTIFACT, "commit hash required");

	if (argc == 1) {
		rc = resolve_path(&path, *argv, rid);
		if (rc != 0)
			return rc;
	}

	rc = init_curses(colour);
	if (rc)
		goto end;
	rc = init_unveil(
		((const char *[]){ REPODB, CKOUTDIR, fnc__tmpdir, tzfile() }),
		((const char *[]){ "rw", "rwc", "rwc", "r" }), 4, 1
	);
	if (rc != 0)
		goto end;

	view = view_open(0, 0, 0, 0, FNC_VIEW_TREE);
	if (view == NULL) {
		RC(FNC_RC_CURSES, "view_open");
		goto end;
	}
	rc = open_tree_view(view, path, rid, colour);
	if (rc != 0)
		goto end;

	rc = view_loop(view);

end:
	free(path);
	return rc;
}

static int
open_tree_view(struct fnc_view *view, const char *path, fsl_id_t rid,
    bool colour)
{
	struct fsl_cx			*f;
	struct fnc_tree_view_state	*s = &view->state.tree;
	int				 rc;

	TAILQ_INIT(&s->parents);

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	s->commit_id = fsl_rid_to_uuid(f, rid);
	if (s->commit_id == NULL) {
		if (f->error.code != FSL_RC_OK)
			return RC_LIBF(f->error.code,
			    "fsl_rid_to_uuid: %d", rid);
		return RC(FNC_RC_NO_RID, "fsl_rid_to_uuid");
	}
	s->rid = rid;
	s->show_id = false;

	/*
	 * Construct tree of entire repository from which all (sub)tress will
	 * be derived. This object will be released when this view closes.
	 */
	rc = create_repository_tree(&s->repo, s->rid);
	if (rc)
		goto end;
	if (s->repo->nentries == 0) {
		rc = RC(FNC_RC_EMPTY_TREE, "%.10s", s->commit_id);
		goto end;
	}

	/*
	 * Open the initial root level of the repository tree now. Subtrees
	 * opened during traversal are built and destroyed on demand.
	 */
	rc = tree_builder(&s->root, s->repo, NULL);
	if (rc)
		goto end;
	s->tree = s->root;
	/*
	 * If user has supplied a path arg (i.e., fnc tree path/in/repo), or
	 * has selected a commit from an 'fnc timeline path/in/repo' command,
	 * walk the path and open corresponding (sub)tree objects now.
	 */
	if (!fnc_path_is_root_dir(path)) {
		rc = walk_tree_path(s, path, view->nlines - 4);
		if (rc)
			goto end;
	}

	if ((s->tree_label = fsl_mprintf("checkin %s", s->commit_id)) == NULL) {
		rc = RC_ERRNO("fsl_mprintf");
		goto end;
	}

	s->first_entry_onscreen = s->selected_entry = s->tree == s->root ?
	    &s->tree->entries[0] : NULL;

	if (has_colors() && COLORS) {
		STAILQ_INIT(&s->colours);
		rc = set_colours(&s->colours, FNC_VIEW_TREE);
		if (rc)
			goto end;
		view->colour = colour;
	}

	view->show = show_tree_view;
	view->input = tree_input_handler;
	view->close = close_tree_view;
	view->grep_init = tree_grep_init;
	view->grep = tree_search_next;

end:
	if (rc) {
		if (view->close == NULL)
			close_tree_view(view);
		view_close(view);
	}
	return rc;
}

/*
 * Decompose the supplied path into its constituent components, then build,
 * open and visit each subtree segment on the way to the requested entry.
 * Display max n entries per page, so if each visited dir is more than n
 * entries deep, scroll just enough to make it the last entry on screen.
 */
static int
walk_tree_path(struct fnc_tree_view_state *s, const char *path, uint16_t n)
{
	struct fnc_tree_object	*tree = NULL;
	const char		*p, *slash;
	char			*subpath = NULL;
	int			 rc = FNC_RC_OK;

	/* Find each slash and open preceding directory segment as a tree. */
	p = path;
	while (*p) {
		struct fnc_tree_entry	*te;
		char			*te_name;

		while (p[0] == '/')
			p++;

		slash = strchr(p, '/');
		if (slash == NULL) {
			te_name = strdup(p);
			if (te_name == NULL) {
				rc = RC_ERRNO("strdup");
				break;
			}
		} else {
			te_name = strndup(p, slash - p);
			if (te_name == NULL) {
				rc = RC_ERRNO("strndup");
				break;
			}
		}

		te = find_tree_entry(s->tree, te_name, strlen(te_name));
		if (te == NULL) {
			rc = RC(FNC_RC_NO_TREE_ENTRY, "%s", te_name);
			free(te_name);
			break;
		}
		free(te_name);

		s->selected_entry = te;  /* dir matching provided path */

		/*
		 * If not in the root tree and the matching dir fits on the
		 * first page, the first displayed entry should be ".." (NULL),
		 * else scroll so the matching dir is the last entry on screen.
		 */
		s->first_entry_onscreen = &s->tree->entries[MAX(te->idx - n, 0)];
		if (__predict_true(!s->first_entry_onscreen->idx &&
		    s->selected_entry->idx < n && s->tree != s->root))
			s->first_entry_onscreen = NULL;
		if (!S_ISDIR(s->selected_entry->mode))
			break;	/* If a file, jump to this entry. */

		slash = strchr(p, '/');
		if (slash != NULL) {
			subpath = strndup(path, slash - path);
			if (subpath == NULL) {
				rc = RC_ERRNO("strndup");
				break;
			}
		} else {
			subpath = strdup(path);
			if (subpath == NULL) {
				rc = RC_ERRNO("strdup");
				break;
			}
		}

		rc = tree_builder(&tree, s->repo, subpath);
		if (rc)
			break;

		rc = visit_subtree(s, tree);
		if (rc) {
			fnc_object_tree_close(tree);
			break;
		}

		if (slash == NULL)
			break;
		free(subpath);
		subpath = NULL;
		p = slash;
	}

	free(subpath);
	return rc;
}

/*
 * Build the fnc_repository_tree doubly linked list and assign
 * to repo, which must be disposed of by the caller. From this
 * tree, all displayed (sub)trees are derived. Tree nodes are
 * derived from F cards comprising the commit identified by rid.
 */
static int
create_repository_tree(struct fnc_repository_tree **repo, fsl_id_t rid)
{
	struct fsl_cx			*f;
	struct fnc_repository_tree	*ptr;
	struct fsl_deck			 d = fsl_deck_empty;
	const struct fsl_card_F		*cf;
	int				 rc;

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	ptr = calloc(1, sizeof(*ptr));
	if (ptr == NULL)
		return RC_ERRNO("calloc");

	rc = fsl_deck_load_rid(f, &d, rid, FSL_SATYPE_CHECKIN);
	if (rc)
		goto end;

	rc = fsl_deck_F_rewind(&d);
	if (rc)
		goto end;

	rc = fsl_deck_F_next(&d, &cf);
	if (rc)
		goto end;

	while (cf != NULL) {
		time_t mtime;

		rc = fsl_mtime_of_F_card(f, rid, cf, (fsl_time_t *)&mtime);
		if (rc != 0) {
			rc = RC_LIBF(rc, "fsl_mtime_of_F_card");
			goto end;
		}

		rc = insert_tree_node(ptr, cf->name, cf->uuid, cf->perm, mtime);
		if (rc)
			goto end;

		++ptr->nentries;

		rc = fsl_deck_F_next(&d, &cf);
		if (rc)
			goto end;
	}

end:
	fsl_deck_finalize(&d);
	if (rc)
		free(ptr);
	else
		*repo = ptr;
	return rc;
}

#if 0
static void
delete_tree_node(struct fnc_tree_entry **head, struct fnc_tree_entry *del)
{
	struct fnc_tree_entry *temp = *head, *prev;

	if (temp == del) {
		*head = temp->next;
		free(temp);
		return;
	}

	while (temp != NULL && temp != del) {
		prev = temp;
		temp = temp->next;
	}

	if (temp == NULL)
		return;

	prev->next = temp->next;

	free(temp);
}
#endif

/*
 * Construct the (sub)trees that are displayed in each tree view.
 * The directory dir and its contents form a tree object returned
 * in *ptr that contains an array of tree entries and which the
 * caller must dispose of by calling fnc_object_tree_close().
 */
static int
tree_builder(struct fnc_tree_object **ptr, struct fnc_repository_tree *repo,
    const char *dir)
{
	struct fnc_tree_object		*tree;
	struct fnc_tree_entry		*te;
	struct fnc_repo_tree_node	*tn;
	int				 i = 0;

	tree = calloc(1, sizeof(*tree));
	if (tree == NULL)
		return RC_ERRNO("calloc");

	/*
	 * Count how many elements will comprise the tree to be allocated.
	 * If dir is the root of the repository tree (i.e., NULL), only tree
	 * nodes (tn) with no parent_dir belong to this tree. Otherwise, tree
	 * nodes whose parent_dir matches dir will comprise the requested tree.
	 */
	for (tn = repo->head; tn; tn = tn->next)
		if ((tn->parent_dir == NULL && dir == NULL) ||
		    (tn->parent_dir != NULL && dir != NULL &&
		    strcmp(dir, tn->parent_dir->path) == 0))
			++i;

	tree->entries = calloc(i, sizeof(*tree->entries));
	if (tree->entries == NULL) {
		free(tree);
		tree = NULL;
		return RC_ERRNO("calloc");
	}

	/* construct the tree to be displayed */
	for (tn = repo->head, i = 0; tn != NULL; tn = tn->next) {
		if ((tn->parent_dir == NULL && dir != NULL) ||
		    (tn->parent_dir != NULL && (dir == NULL ||
		    strcmp(dir, tn->parent_dir->path) != 0)))
			continue;

		te = &tree->entries[i];

		te->basename = tn->basename;
		te->mtime = tn->mtime;
		te->path = tn->path;
		te->uuid = tn->uuid;
		te->mode = tn->mode;
		te->idx = i++;
	}

	tree->nentries = i;
	*ptr = tree;
	return FNC_RC_OK;
}

/*
 * Insert nodes into fnc_repository_tree tree. Each component of path (i.e.,
 * '/'-delimited tokens) becomes a node in tree. The final component of each
 * segment is assigned to the node's basename, and its "absolute" repository
 * relative path to its path member. All files in a given directory will be
 * allocated to the directory node's children list, and to each file node's
 * sibling list such that said directory node is each file node's parent_dir.
 * Elements of each requested tree are identified by the node's parent_dir;
 * that is, each node with the same parent_dir are entries in the same tree.
 *   tree	repository tree into which nodes are inserted
 *   path	repository relative pathname of the versioned file
 *   uuid	SHA hash of the file
 *   mtime	modification time of the file
 * return FNC_RC_OK on success, non-zero on error.
 */
static int
insert_tree_node(struct fnc_repository_tree *tree, const char *path,
    const char *uuid, enum fsl_fileperm_e perm, time_t mtime)
{
	struct fnc_repo_tree_node	*parent_dir;
	fsl_buffer			 buf = fsl_buffer_empty;
	struct stat			 s;
	int				 i, rc = FNC_RC_OK;

	parent_dir = tree->tail;

	while (parent_dir != NULL &&
	    (strncmp(parent_dir->path, path, parent_dir->pathlen) != 0 ||
	    path[parent_dir->pathlen] != '/'))
		parent_dir = parent_dir->parent_dir;

	i = parent_dir != NULL ? parent_dir->pathlen + 1 : 0;

	while (path[i]) {
		struct fnc_repo_tree_node	*tn;
		int				 nodesz, slash = i;

		/* find slash to demarcate each path component */
		while (path[i] && path[i] != '/')
			i++;
		nodesz = sizeof(*tn) + i + 1;

		/*
		 * If not at end of path string, this node is a
		 * directory so don't allocate space for the hash.
		 */
		if (uuid != NULL && path[i] == '\0')
			nodesz += FSL_STRLEN_K256 + 1; /* NUL */

		tn = malloc(nodesz);
		if (tn == NULL)
			return RC_ERRNO("malloc");

		memset(tn, 0, sizeof(*tn));

		tn->path = (char *)&tn[1];
		memcpy(tn->path, path, i);
		tn->path[i] = '\0';
		tn->pathlen = i;

		if (uuid != NULL && path[i] == '\0') {
			tn->uuid = tn->path + i + 1;
			memcpy(tn->uuid, uuid, strlen(uuid) + 1);
		}

		tn->basename = tn->path + slash;

		/* insert node into list or make it the head if it is first */
		if (tree->tail != NULL) {
			tree->tail->next = tn;
			tn->prev = tree->tail;
		} else
			tree->head = tn;

		tree->tail = tn;
		tn->parent_dir = parent_dir;
		if (parent_dir != NULL) {
			if (parent_dir->children != NULL)
				parent_dir->lastchild->sibling = tn;
			else
				parent_dir->children = tn;
			tn->nparents = parent_dir->nparents + 1;
			parent_dir->lastchild = tn;
		} else {
			if (tree->rootail != NULL)
				tree->rootail->sibling = tn;
			tree->rootail = tn;
		}

		tn->mtime = mtime;
		while (path[i] == '/')
			++i;
		parent_dir = tn;

		/* stat path for tree entry annotation (i.e., /@* suffix) */
		rc = fsl_file_canonical_name2(CKOUTDIR, tn->path, &buf, false);
		if (rc)
			goto end;

		if (lstat(fsl_buffer_cstr(&buf), &s) == -1) {
			if (errno != ENOENT) {
				rc = RC_ERRNO("lstat: %s",
				    fsl_buffer_cstr(&buf));
				goto end;
			}
			/*
			 * Path is not on disk but must exist in the tree
			 * requested as either a tracked file or directory.
			 */
			if (tn->uuid != NULL && strcmp(tn->path, path) == 0) {
				switch (perm) {
				case FSL_FILE_PERM_LINK:
					tn->mode = S_IFLNK;
					break;
				case FSL_FILE_PERM_EXE:
					tn->mode = S_IXUSR;
					break;
				default:
					tn->mode = S_IFREG;
					break;
				}
			} else
				tn->mode = S_IFDIR;
		} else
			tn->mode = s.st_mode;

		fsl_buffer_reuse(&buf);
	}

	while (parent_dir != NULL && parent_dir->parent_dir != NULL) {
		if (parent_dir->parent_dir->mtime < parent_dir->mtime)
			parent_dir->parent_dir->mtime = parent_dir->mtime;
		parent_dir = parent_dir->parent_dir;
	}

end:
	fsl_buffer_clear(&buf);
	return rc;
}

static int
show_tree_view(struct fnc_view *view)
{
	struct fnc_tree_view_state	*s = &view->state.tree;
	char				*treepath;
	int				 rc;

	rc = tree_entry_path(&treepath, &s->parents, NULL);
	if (rc)
		return rc;

	rc = draw_tree(view, treepath);
	free(treepath);
	drawborder(view);

	return rc;
}

/*
 * Construct absolute repository path of the currently selected tree entry to
 * display in the tree view header, or pass to open_timeline_view() to construct
 * a timeline of all commits modifying path.
 */
static int
tree_entry_path(char **path, struct fnc_parent_trees *parents,
    struct fnc_tree_entry *te)
{
	struct fnc_parent_tree	*pt;
	size_t			 len = 1;  /* NUL */
	int			 rc = FNC_RC_OK;

	TAILQ_FOREACH(pt, parents, entry)
		len += strlen(pt->selected_entry->basename) + 1 /* slash */;
	if (te)
		len += strlen(te->basename);

	*path = calloc(1, len);
	if (path == NULL)
		return RC_ERRNO("calloc");

	pt = TAILQ_LAST(parents, fnc_parent_trees);
	while (pt) {
		const char *name = pt->selected_entry->basename;

		if (strlcat(*path, name, len) >= len) {
			rc = RC(FNC_RC_NO_SPACE, "strlcat");
			goto end;
		}
		if (strlcat(*path, "/", len) >= len) {
			rc = RC(FNC_RC_NO_SPACE, "strlcat");
			goto end;
		}
		pt = TAILQ_PREV(pt, fnc_parent_trees, entry);
	}
	if (te) {
		if (strlcat(*path, te->basename, len) >= len) {
			rc = RC(FNC_RC_NO_SPACE, "strlcat");
			goto end;
		}
	}

end:
	if (rc) {
		free(*path);
		*path = NULL;
	}
	return rc;
}

/*
 * Draw the currently visited tree. Lexicographically order nodes (cf. ls(1))
 * and annotate with an identifier corresponding to the file mode as returned
 * by lstat(2) such that the tree takes the following form:
 *
 *  checkin COMMIT-HASH
 *  [n/N] /absolute/repository/tree/path/
 *
 *   ..
 *   dir/
 *   executable*
 *   regularfile
 *   symlink@ -> /path/to/source/file
 *
 * If the 'i' and 'd' keymaps are entered, prefix each tree entry with its
 * SHA{1,3} hash and last modified date, respectively. Directories have no
 * hashes so pad with dots.
 */
static int
draw_tree(struct fnc_view *view, const char *treepath)
{
	struct fnc_tree_view_state	*s = &view->state.tree;
	struct fnc_tree_entry		*te;
	struct fnc_colour		*c = NULL;
	wchar_t				*wline;
	char				*index;
	int				 rc;
	int				 wlen, n, nentries, idx = 1;
	int				 limit = view->nlines;
	uint_fast8_t			 hashlen = FSL_UUID_STRLEN_MIN;

	s->ndisplayed = 0;
	werase(view->window);

	if (view_is_top_split(view))
		--limit;	/* account for the border */
	if (limit == 0)
		return FNC_RC_OK;

	/* highlight the headline if this view is active in a splitscreen */
	rc = formatln(&wline, &wlen, NULL, s->tree_label, 0, view->ncols,
	    0, false);
	if (rc)
		return rc;
	if (view_is_shared(view))
		wattron(view->window, fnc__highlight);
	if (view->colour)
		c = get_colour(&s->colours, FNC_COLOUR_COMMIT);
	if (c)
		wattr_on(view->window, COLOR_PAIR(c->scheme), NULL);
	waddwstr(view->window, wline);
	while (wlen < view->ncols) {
		waddch(view->window, ' ');
		++wlen;
	}
	if (c)
		wattr_off(view->window, COLOR_PAIR(c->scheme), NULL);
	if (view_is_shared(view))
		wattroff(view->window, fnc__highlight);
	free(wline);
	wline = NULL;
	if (--limit <= 0)
		return FNC_RC_OK;

	idx += s->selected;
	if (s->first_entry_onscreen != NULL) {
		idx += s->first_entry_onscreen->idx;
		if (s->tree != s->root)
			++idx;
	}
	nentries = s->tree->nentries;
	if ((index = fsl_mprintf("[%d/%d] /%s", idx,
	    nentries + (s->tree == s->root ? 0 : 1),
	    treepath != NULL ? treepath : "")) == NULL)
		return RC_ERRNO("fsl_mprintf");
	rc = formatln(&wline, &wlen, NULL, index, 0, view->ncols, 0, false);
	free(index);
	if (rc)
		return rc;
	wprintw(view->window, "%ls", wline);
	free(wline);
	wline = NULL;
	if (wlen < view->ncols)
		waddch(view->window, '\n');
	if (--limit <= 0)
		return FNC_RC_OK;
	waddch(view->window, '\n');
	if (--limit <= 0)
		return FNC_RC_OK;

	/* write parent dir entry (i.e., "..") if the treetop is in view */
	n = 0;
	if (s->first_entry_onscreen == NULL) {
		te = &s->tree->entries[0];
		if (s->selected == 0) {
			wattr_on(view->window, fnc__highlight, NULL);
			s->selected_entry = NULL;
		}
		if (treepath != NULL) {
			waddstr(view->window, "  ..\n");
			n = 1;
		}
		if (s->selected == 0)
			wattr_off(view->window, fnc__highlight, NULL);
		++s->ndisplayed;
		if (--limit <= 0)
			return FNC_RC_OK;
	} else
		te = s->first_entry_onscreen;

	/*
	 * Determine whether SHA1 or SHA3 (or both) hashes are used so
	 * we know what length to pad directories (and the shorter SHA1
	 * strings if both hashes are used) when showing hash ID strings.
	 */
	for (idx = 0; idx < nentries; ++idx) {
		if (s->tree->entries[idx].uuid == NULL)
			continue;
		hashlen = MAX(strlen(s->tree->entries[idx].uuid), hashlen);
	}

	for (idx = te->idx; idx < nentries; ++idx) {
		char		 id[FSL_STRLEN_K256 + 1];  /* NUL */
		char		 iso8601[ISO8601_DATE_HHMM + 1];  /* NUL */
		char		*line = NULL, *link = NULL;
		const char	*idstr = "", *modestr = "", *linkstr = "";
		const char	*tsstr = "";
		size_t		 padlen, idlen = 0;
		mode_t		 mode;

		if (idx < 0 || idx >= s->tree->nentries)
			return rc;

		te = &s->tree->entries[idx];
		mode = te->mode;

		if (s->show_id) {
			idstr = te->uuid;
			if (idstr != NULL)
				idlen = strlen(idstr);

			/* pad directories and sha1 in mixed repos with dots */
			padlen = hashlen - idlen;
			if (padlen > 0) {
				char pad[padlen + 1];  /* NUL */

				memset(pad, '.', padlen);
				pad[padlen] = '\0';

				if (idstr != NULL && memccpy(&id, idstr, '\0',
				    sizeof(id)) == NULL) {
					rc = RC(FNC_RC_NO_SPACE, "memccpy");
					goto end;
				}
				if (memccpy(&id[idlen], pad, '\0',
				    sizeof(id) - idlen) == NULL) {
					rc = RC(FNC_RC_NO_SPACE, "memccpy");
					goto end;
				}

				id[hashlen] = '\0';
				idstr = id;
			}
		}

		if (S_ISLNK(mode)) {
			size_t ch, linklen;

			rc = tree_entry_get_symlink_target(&link, &linklen,
			    te);
			if (rc)
				goto end;

			for (ch = 0; ch < linklen; ++ch) {
				if (isprint((unsigned char)link[ch]) == 0)
					link[ch] = '?';
			}
			linkstr = link;
			modestr = "@";
		} else if (S_ISDIR(mode))
			modestr = "/";
		else if (mode & S_IXUSR)
			modestr = "*";

		if (s->show_date) {
			rc = fnc_strftime(&*iso8601, sizeof(iso8601),
			    "%G-%m-%d %R", te->mtime);
			if (rc != FNC_RC_OK)
				goto end;
			tsstr = iso8601;
		}

		line = fsl_mprintf("%s%s%s  %s%s%s%s", idstr,
		    (s->show_date && idstr) ? "  " : "", tsstr,
		    te->basename, modestr, link ? " -> " : "", linkstr);
		if (line == NULL) {
			rc = RC_ERRNO("fsl_mprintf");
			goto end;
		}

		rc = formatln(&wline, &wlen, NULL, line, 0, view->ncols, 0,
		    false);
		if (rc)
			goto end;

		if (n == s->selected) {
			wattr_on(view->window, fnc__highlight, NULL);
			s->selected_entry = te;
		}
		if (view->colour)
			c = match_colour(&s->colours, line);
		if (c)
			wattr_on(view->window, COLOR_PAIR(c->scheme), NULL);
		waddwstr(view->window, wline);
		if (c)
			wattr_off(view->window, COLOR_PAIR(c->scheme), NULL);
		if (wlen < view->ncols)
			waddch(view->window, '\n');
		if (n == s->selected)
			wattr_off(view->window, fnc__highlight, NULL);

end:
		free(line);
		free(link);
		free(wline);
		wline = NULL;
		if (rc)
			return rc;
		++n;
		++s->ndisplayed;
		s->last_entry_onscreen = te;
		if (--limit <= 0)
			break;
	}

	return FNC_RC_OK;
}

static int
tree_entry_get_symlink_target(char **ret, size_t *retlen,
    struct fnc_tree_entry *te)
{
	struct fsl_cx		*f;
	struct fsl_buffer	 buf;
	struct stat		 sb;
	char			*linkpath = NULL, *target = NULL;
	ssize_t			 targetlen;
	fsl_id_t		 rid;
	int			 rc;

	*ret = NULL;
	*retlen = 0;
	memset(&buf, 0, sizeof(buf));

	f = fcli_cx();
	if (f == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	if (!((te->mode & (S_IFDIR | S_IFLNK)) == S_IFLNK))
		return RC(FNC_RC_BAD_PATH, "not a symbolic link: %s",
		    te->path);

	if (fsl_cx_has_ckout(f)) {
		rc = fsl_file_canonical_name2(CKOUTDIR, te->path, &buf, false);
		if (rc != FNC_RC_OK) {
			rc = RC(FNC_RC_BAD_PATH,
			    "fsl_file_canonical_name2: %s", te->path);
			goto end;
		}

		linkpath = fsl_buffer_take(&buf);
		if (linkpath == NULL) {
			rc = RC_ERRNO("malloc");
			goto end;
		}
		if (lstat(linkpath, &sb) == -1) {
			if (errno != ENOENT) {
				rc = RC_ERRNO("lstat: %s", linkpath);
				goto end;
			}

			rc = idtorid(&rid, te->uuid, FNC_RC_NO_REF);
			if (rc != 0)
				goto end;

			rc = fnc_blob_get_content(&target, &targetlen, rid);
			goto end;
		}

		rc = fnc_read_symlink(&buf, linkpath);
		if (rc != FNC_RC_OK)
			goto end;

		targetlen = buf.used;
		target = fsl_buffer_take(&buf);
		if (target == NULL) {
			rc = RC_ERRNO("malloc");
			goto end;
		}
	} else {
		rc = idtorid(&rid, te->uuid, FNC_RC_NO_REF);
		if (rc != 0)
			goto end;

		rc = fnc_blob_get_content(&target, &targetlen, rid);
		if (rc != FNC_RC_OK)
			goto end;

		if (targetlen > 0 && target[targetlen - 1] == '\n') {
			target[targetlen - 1] = '\0';
			--targetlen;
		}
	}

end:
	free(linkpath);
	fsl_buffer_clear(&buf);
	if (rc != FNC_RC_OK)
		free(target);
	else {
		*ret = target;
		*retlen = targetlen;
	}
	return rc;
}

static int
fnc_blob_get_content(char **ret, ssize_t *retlen, fsl_id_t rid)
{
	struct fsl_cx		*f;
	struct fsl_buffer	 buf = fsl_buffer_empty;
	int			 rc;

	*ret = NULL;
	*retlen = 0;

	if (rid == 0)
		return RC(FNC_RC_RANGE, "%s", rid);

	f = fcli_cx();
	if (f == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	rc = fsl_content_raw(f, rid, &buf);
	if (rc != FNC_RC_OK) {
		fsl_buffer_clear(&buf);
		return RC_LIBF(rc, "fsl_content_raw");
	}

	*retlen = buf.used;
	*ret = fsl_buffer_take(&buf);
	if (*ret == NULL)
		rc = RC_ERRNO("malloc");
	return rc;
}

static int
tree_input_handler(struct fnc_view **new_view, struct fnc_view *view, int ch)
{
	struct fnc_tree_view_state	*s = &view->state.tree;
	struct fnc_tree_entry		*te;
	int				 n, rc = FNC_RC_OK;
	uint16_t			 eos, nscroll;

	eos = nscroll = view->nlines - 3;

	switch (ch) {
	case 'b':
		return view_request_new(new_view, view, FNC_VIEW_BRANCH);
	case 'C':
		if (COLORS)
			view->colour = !view->colour;
		break;
	case 'd':
		s->show_date = !s->show_date;
		break;
	case 'i':
		s->show_id = !s->show_id;
		break;
	case 't':
		if (!s->selected_entry)
			break;
		return view_request_new(new_view, view, FNC_VIEW_TIMELINE);
	case 'g':
		if (!fnc_home(view))
			break;
		/* FALL THROUGH */
	case KEY_HOME:
		s->selected = 0;
		if (s->tree == s->root)
			s->first_entry_onscreen = &s->tree->entries[0];
		else
			s->first_entry_onscreen = NULL;
		break;
	case KEY_END:
	case 'G':
		if (view_is_top_split(view))
			--eos;	/* account for the border */

		s->selected = 0;
		te = &s->tree->entries[s->tree->nentries - 1];

		for (n = 0; n < eos; ++n) {
			if (te == NULL) {
				if(s->tree != s->root) {
					s->first_entry_onscreen = NULL;
					++n;
				}
				break;
			}
			s->first_entry_onscreen = te;
			te = get_tree_entry(s->tree, te->idx - 1);
		}
		if (n > 0)
			s->selected = n - 1;
		break;
	case KEY_DOWN:
	case 'j':
		if (s->selected < s->ndisplayed - 1) {
			++s->selected;
			break;
		}
		if (get_tree_entry(s->tree, s->last_entry_onscreen->idx + 1)
		    == NULL)
			break;	/* reached the last displayed entry onscreen */
		tree_scroll_down(view, 1);
		break;
	case KEY_UP:
	case 'k':
		if (s->selected > 0) {
			--s->selected;
			break;
		}
		tree_scroll_up(s, 1);
		break;
	case CTRL('d'):
		nscroll >>= 1;
		/* FALL THROUGH */
	case KEY_NPAGE:
	case CTRL('f'):
		if (get_tree_entry(s->tree, s->last_entry_onscreen->idx + 1)
		    == NULL) {
			/*
			 * When the last entry on screen is the last node in the
			 * tree move cursor to it instead of scrolling the view.
			 */
			if (s->selected < s->ndisplayed - 1)
				s->selected += MIN(nscroll,
				    s->ndisplayed - s->selected - 1);
			break;
		}
		tree_scroll_down(view, nscroll);
		break;
	case CTRL('u'):
		nscroll >>= 1;
		/* FALL THROUGH */
	case KEY_PPAGE:
	case CTRL('b'):
		if (s->tree == s->root) {
			if (&s->tree->entries[0] == s->first_entry_onscreen)
				s->selected -= MIN(s->selected, nscroll);
		} else {
			if (s->first_entry_onscreen == NULL)
				s->selected -= MIN(s->selected, nscroll);
		}
		tree_scroll_up(s, nscroll);
		break;
	case KEY_BACKSPACE:
	case KEY_ENTER:
	case KEY_LEFT:
	case KEY_RIGHT:
	case '\r':
	case 'h':
	case 'l':
		/*
		 * h/backspace/arrow-left: return to parent dir irrespective
		 * of selected entry type (unless already at root).
		 * l/arrow-right: move into selected dir entry.
		 */
		if (ch != KEY_RIGHT && ch != 'l' && (s->selected_entry == NULL
		    || ch == 'h' || ch == KEY_BACKSPACE || ch == KEY_LEFT)) {
			struct fnc_parent_tree	*parent;
			/* h/backspace/left-arrow pressed or ".." selected. */
			if (s->tree == s->root)
				break;
			parent = TAILQ_FIRST(&s->parents);
			TAILQ_REMOVE(&s->parents, parent,
			    entry);
			fnc_object_tree_close(s->tree);
			s->tree = parent->tree;
			s->first_entry_onscreen = parent->first_entry_onscreen;
			s->selected_entry = parent->selected_entry;
			s->selected = parent->selected;
			if (s->selected > view->nlines - 3)
				view_offset_scrolldown(view);
			fsl_free(parent);
		} else if (s->selected_entry != NULL &&
		    S_ISDIR(s->selected_entry->mode)) {
			struct fnc_tree_object *subtree = NULL;

			rc = tree_builder(&subtree, s->repo,
			    s->selected_entry->path);
			if (rc)
				break;

			rc = visit_subtree(s, subtree);
			if (rc) {
				fnc_object_tree_close(subtree);
				break;
			}
		} else if (s->selected_entry != NULL &&
		    (S_ISREG(s->selected_entry->mode) ||
		     S_ISLNK(s->selected_entry->mode) ||
		     s->selected_entry->mode & S_IXUSR))
			rc = blame_selected_file(new_view, view);
		break;
	case KEY_RESIZE:
		if (view->nlines >= 4 && s->selected >= view->nlines - 3)
			s->selected = view->nlines - 4;
		break;
	default:
		break;
	}

	return rc;
}

static int
blame_selected_file(struct fnc_view **new_view, struct fnc_view *view)
{
	struct fsl_cx			*f;
	struct fnc_tree_view_state	*s = &view->state.tree;
	struct fsl_buffer		 buf = fsl_buffer_empty;
	fsl_id_t			 fid;
	int				 rc;

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	rc = idtorid(&fid, s->selected_entry->uuid, FNC_RC_NO_REF);
	if (rc != 0)
		return rc;

	rc = fsl_content_get(f, fid, &buf);
	if (rc != 0) {
		fsl_buffer_clear(&buf);  /* may be partially populated */
		return RC_LIBF(rc, "fsl_content_get: %d", fid);
	}

	if (fsl_looks_like_binary(&buf))
		return sitrep(view, SR_UPDATE | SR_SLEEP,
		    ":cannot blame binary file");

	return view_request_new(new_view, view, FNC_VIEW_BLAME);
}

static int
timeline_tree_entry(struct fnc_view **new_view, int y, int x,
    struct fnc_tree_view_state *s, int color)
{
	struct fnc_view	*timeline_view;
	char		*path;
	int		 rc;

	*new_view = NULL;

	timeline_view = view_open(0, 0, y, x, FNC_VIEW_TIMELINE);
	if (timeline_view == NULL)
		return RC(FNC_RC_CURSES, "view_open");

	/* construct repository relative path for timeline query */
	rc = tree_entry_path(&path, &s->parents, s->selected_entry);
	if (rc)
		return rc;

	rc = open_timeline_view(timeline_view, s->rid, path,
	    NULL, NULL, NULL, NULL, 0, NULL, color);
	if (rc)
		view_close(timeline_view);
	else
		*new_view = timeline_view;

	free(path);
	return rc;
}

static void
tree_scroll_up(struct fnc_tree_view_state *s, int maxscroll)
{
	struct fnc_tree_entry	*te;
	int			 isroot, i = 0;

	isroot = s->tree == s->root;

	if (s->first_entry_onscreen == NULL)
		return;

	te = get_tree_entry(s->tree, s->first_entry_onscreen->idx - 1);
	while (i++ < maxscroll) {
		if (te == NULL) {
			if (!isroot)
				s->first_entry_onscreen = NULL;
			break;
		}
		s->first_entry_onscreen = te;
		te = get_tree_entry(s->tree, te->idx - 1);
	}
}

static int
tree_scroll_down(struct fnc_view *view, int maxscroll)
{
	struct fnc_tree_view_state	*s = &view->state.tree;
	struct fnc_tree_entry		*next, *last;
	int				 n = 0;

	if (s->first_entry_onscreen)
		next = get_tree_entry(s->tree,
		    s->first_entry_onscreen->idx + 1);
	else
		next = &s->tree->entries[0];

	last = s->last_entry_onscreen;
	while (next != NULL && n++ < maxscroll) {
		if (last) {
			s->last_entry_onscreen = last;
			last = get_tree_entry(s->tree, last->idx + 1);
		}
		if (last || (view->mode == VIEW_SPLIT_HRZN && next)) {
			s->first_entry_onscreen = next;
			next = get_tree_entry(s->tree, next->idx + 1);
		}
	}

	return FNC_RC_OK;
}

/*
 * Visit subtree by assigning the current tree, selected and first displayed
 * entries, and selected line index to a new parent tree node to be inserted
 * into the parents linked list. Then make subtree the current tree.
 */
static int
visit_subtree(struct fnc_tree_view_state *s, struct fnc_tree_object *subtree)
{
	struct fnc_parent_tree	*parent;

	parent = calloc(1, sizeof(*parent));
	if (parent == NULL)
		return RC_ERRNO("calloc");

	parent->tree = s->tree;
	parent->first_entry_onscreen = s->first_entry_onscreen;
	parent->selected_entry = s->selected_entry;
	/*
	 * If not the first page of entries (".." isn't visible), the line to
	 * select is the difference betwixt selected & first displayed entries.
	 */
	parent->selected = s->first_entry_onscreen ?
	    s->selected_entry->idx - s->first_entry_onscreen->idx :
	    s->selected_entry->idx + 1;

	TAILQ_INSERT_HEAD(&s->parents, parent, entry);
	s->tree = subtree;
	s->selected = 0;
	s->first_entry_onscreen = NULL;

	return FNC_RC_OK;
}

static int
blame_tree_entry(struct fnc_view **new_view, int begin_y, int begin_x,
    struct fnc_tree_entry *te, struct fnc_parent_trees *parents,
    fsl_id_t rid, int color)
{
	struct fnc_view	*blame_view;
	char		*path;
	int		 rc;

	*new_view = NULL;

	rc = tree_entry_path(&path, parents, te);
	if (rc)
		return rc;

	blame_view = view_open(0, 0, begin_y, begin_x, FNC_VIEW_BLAME);
	if (blame_view == NULL) {
		rc = RC(FNC_RC_CURSES, "view_open");
		goto end;
	}

	rc = open_blame_view(blame_view, path, rid, 0, 0, NULL, color);
	if (rc)
		view_close(blame_view);
	else
		*new_view = blame_view;

end:
	free(path);
	return rc;
}

static void
tree_grep_init(struct fnc_view *view)
{
	struct fnc_tree_view_state *s = &view->state.tree;

	s->matched_entry = NULL;
}

static int
tree_search_next(struct fnc_view *view)
{
	struct fnc_tree_view_state	*s = &view->state.tree;
	struct fnc_tree_entry		*te = NULL;

	if (view->searching == SEARCH_DONE) {
		view->search_status = SEARCH_CONTINUE;
		return FNC_RC_OK;
	}

	if (s->matched_entry) {
		if (view->searching == SEARCH_FORWARD) {
			if (s->selected_entry)
				te = get_tree_entry(s->tree,
				    s->selected_entry->idx + 1);
			else
				te = &s->tree->entries[0];
		} else {
			if (s->selected_entry == NULL)
				te = &s->tree->entries[s->tree->nentries - 1];
			else
				te = get_tree_entry(s->tree,
				    s->selected_entry->idx - 1);
		}
	} else {
		if (s->selected_entry)
			te = s->selected_entry;
		if (view->searching == SEARCH_FORWARD)
			te = &s->tree->entries[0];
		else
			te = &s->tree->entries[s->tree->nentries - 1];
	}

	while (1) {
		if (te == NULL) {
			if (s->matched_entry == NULL) {
				view->search_status = SEARCH_CONTINUE;
				return FNC_RC_OK;
			}
			if (view->searching == SEARCH_FORWARD)
				te = &s->tree->entries[0];
			else
				te = &s->tree->entries[s->tree->nentries - 1];
		}

		if (match_tree_entry(te, &view->regex)) {
			view->search_status = SEARCH_CONTINUE;
			s->matched_entry = te;
			break;
		}

		if (view->searching == SEARCH_FORWARD)
			te = get_tree_entry(s->tree, te->idx + 1);
		else
			te = get_tree_entry(s->tree, te->idx - 1);
	}

	if (s->matched_entry) {
		int	idx = s->matched_entry->idx;
		bool	parent = !s->first_entry_onscreen;

		if (idx >= (parent ? 0 : s->first_entry_onscreen->idx) &&
		    idx <= s->last_entry_onscreen->idx)
			s->selected = idx - (parent ? - 1 :
			    s->first_entry_onscreen->idx);
		else {
			s->first_entry_onscreen = s->matched_entry;
			s->selected = 0;
		}
	}

	return FNC_RC_OK;
}

static int
match_tree_entry(struct fnc_tree_entry *te, regex_t *regex)
{
	regmatch_t regmatch;

	return regexec(regex, te->basename, 1, &regmatch, 0) == 0;
}

struct fnc_tree_entry *
get_tree_entry(struct fnc_tree_object *tree, int i)
{
	if (i < 0 || i >= tree->nentries)
		return NULL;

	return &tree->entries[i];
}

/* Find entry in tree with basename name. */
static struct fnc_tree_entry *
find_tree_entry(struct fnc_tree_object *tree, const char *name, size_t len)
{
	int idx;

	/* Entries are sorted in strcmp() order. */
	for (idx = 0; idx < tree->nentries; ++idx) {
		struct fnc_tree_entry	*te = &tree->entries[idx];
		int			 cmp;

		cmp = strncmp(te->basename, name, len);
		if (cmp < 0)
			continue;
		if (cmp > 0)
			break;
		if (te->basename[len] == '\0')
			return te;
	}
	return NULL;
}

static int
close_tree_view(struct fnc_view *view)
{
	struct fnc_tree_view_state	*s = &view->state.tree;

	free_colours(&s->colours);

	fsl_free(s->tree_label);
	s->tree_label = NULL;
	fsl_free(s->commit_id);
	s->commit_id = NULL;

	while (!TAILQ_EMPTY(&s->parents)) {
		struct fnc_parent_tree *parent;

		parent = TAILQ_FIRST(&s->parents);
		TAILQ_REMOVE(&s->parents, parent, entry);
		if (parent->tree != s->root)
			fnc_object_tree_close(parent->tree);
		free(parent);

	}

	if (s->tree != NULL && s->tree != s->root)
		fnc_object_tree_close(s->tree);
	if (s->root)
		fnc_object_tree_close(s->root);
	if (s->repo)
		fnc_close_repo_tree(s->repo);

	return FNC_RC_OK;
}

static void
fnc_object_tree_close(struct fnc_tree_object *tree)
{
	fsl_free(tree->entries);
	fsl_free(tree);
}

static void
fnc_close_repo_tree(struct fnc_repository_tree *repo)
{
	struct fnc_repo_tree_node *next, *tn;

	tn = repo->head;
	while (tn) {
		next = tn->next;
		fsl_free(tn);
		tn = next;
	}
	fsl_free(repo);
}

static int
cmd_config(int argc, char **argv)
{
	const char	*opt = NULL, *repo = NULL, *value = NULL;
	char		*prev = NULL, *v = NULL;
	enum fnc_opt_id	 setid;
	int		 ch, rc;
	bool		 ls = false, unset = false;

	while ((ch = getopt_long(argc, argv, "+hlr:u", config_opt,
	    NULL)) != -1) {
		switch (ch) {
		case 'h':
			usage_config(0);
			/* NOTREACHED */
		case 'l':
			ls = true;
			break;
		case 'r':
			repo = optarg;
			break;
		case 'u':
			unset = true;
			break;
		default:
			usage_config(1);
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;

	if (argc > 2) {
		usage_config(1);
		/* NOTREACHED */
	}

	if ((rc = fnc_cx_open(repo)) != 0)
		return rc;

	if (argc == 0 || ls) {
		if (unset)
			return RC(FNC_RC_NOSUPPORT,
			    "-u|--unset requires <option>");
		return fnc_conf_lsopt(ls);
	}

	opt = argv[0];

	rc = init_unveil(
		((const char *[]){ REPODIR, CKOUTDIR, fnc__tmpdir, tzfile() }),
		((const char *[]){ "rwc", "rwc", "rwc", "r" }), 4, 1
	);
	if (rc != 0)
		return rc;

	setid = fnc_conf_str2enum(opt);
	if (!setid)
		return RC(FNC_RC_BAD_OPTION, "%s", opt);

	if (argc == 2 || unset) {
		if (argc == 2 && unset)
			return RC(FNC_RC_NOSUPPORT, "--unset or set %s to %s?",
			    opt, argv[1]);
		value = argv[1];
		rc = fnc_conf_getopt(&prev, setid, true);
		if (rc != FNC_RC_OK)
			goto end;
		rc = fnc_conf_setopt(setid, value, unset);
		if (rc)
			goto end;
		f_out("%s: %s -> %s (local)\n", fnc_conf_enum2str(setid),
		    prev ? prev : "default", value ? value : "default");
	} else {
		rc = fnc_conf_getopt(&v, setid, true);
		if (rc != FNC_RC_OK)
			goto end;
		f_out("%s = %s\n", fnc_conf_enum2str(setid),
		    v ? v : "default");
	}

end:
	free(v);
	free(prev);
	return rc;
}

static int
fnc_conf_lsopt(bool all)
{
	char	*value;
	int	 id, rc, maxlen = 0;

	for (id = FNC_START_SETTINGS + 1; id < FNC_EOF_SETTINGS; ++id) {
		rc = fnc_conf_getopt(&value, id, true);
		if (rc != FNC_RC_OK)
			return rc;
		if (all || value != NULL) {
			int len;

			len = strlen(fnc_opt_name[id]);
			maxlen = MAX(len, maxlen);
		}
		free(value);
	}

	if (maxlen == 0 && !all) {
		printf("No user-defined options: "
		    "'%s config --ls' for list of available options.\n",
		    fnc__progname);
		return FNC_RC_OK;
	}

	for (id = FNC_START_SETTINGS + 1; id < FNC_EOF_SETTINGS; ++id) {
		rc = fnc_conf_getopt(&value, id, true);
		if (rc != FNC_RC_OK)
			return rc;
		if (all || value != NULL)
			printf("%-*s%s%s\n", maxlen + 2, fnc_opt_name[id],
			    value ? " = " : "", value ? value : "");
		free(value);
	}

	return FNC_RC_OK;
}

static enum fnc_opt_id
fnc_conf_str2enum(const char *str)
{
	enum fnc_opt_id	idx;

	if (str == NULL || *str == '\0')
		return FNC_START_SETTINGS;

	for (idx = FNC_START_SETTINGS + 1;  idx < FNC_EOF_SETTINGS;  ++idx)
		if (strcasecmp(str, fnc_opt_name[idx]) == 0)
			return idx;

	return FNC_START_SETTINGS;
}

static const char *
fnc_conf_enum2str(enum fnc_opt_id id)
{
	if (id <= FNC_START_SETTINGS || id >= FNC_EOF_SETTINGS)
		return NULL;

	return fnc_opt_name[id];
}

static int
view_close_child(struct fnc_view *view)
{
	int	rc = FNC_RC_OK;

	if (view->child == NULL)
		return rc;

	rc = view_close(view->child);
	view->child = NULL;

	return rc;
}

static int
view_set_child(struct fnc_view *view, struct fnc_view *child)
{
	int rc;

	view->child = child;
	child->parent = view;

	/*
	 * If the timeline is open and has not yet loaded /all/ commits, cached
	 * stmts require resetting the commit builder stmt before restepping.
	 */
	if (view->vid == FNC_VIEW_TIMELINE) {
		struct fnc_tl_thread_cx *tcx = &view->state.timeline.thread_cx;

		if (tcx != NULL && !tcx->eotl)
			tcx->reset = true;
	}

	rc = view_resize(view);
	if (rc != FNC_RC_OK)
		return rc;

	if (view->child->resized_y || view->child->resized_x)
		rc = view_resize_split(view, 0);

	return rc;
}

static void
view_copy_size(struct fnc_view *dst, struct fnc_view *src)
{
	struct fnc_view *v = src->child != NULL ? src->child : src;

	dst->resized_y = v->resized_y;
	dst->resized_x = v->resized_x;
}

static int
set_colours(struct fnc_colours *s, enum fnc_view_id vid)
{
	int fgc, rc;

	rc = init_colour(&fgc, FNC_COLOUR_HL_SEARCH);
	if (rc != FNC_RC_OK)
		return rc;
	if (init_pair(FNC_COLOUR_HL_SEARCH, fgc, -1) == ERR)
		return RC(FNC_RC_CURSES, "init_pair");

	switch (vid) {
	case FNC_VIEW_DIFF: {
		const int pairs_diff[][2] = {
			{LINE_DIFF_META, FNC_COLOUR_DIFF_META},
			{LINE_DIFF_USER, FNC_COLOUR_USER},
			{LINE_DIFF_DATE, FNC_COLOUR_DATE},
			{LINE_DIFF_TAGS, FNC_COLOUR_DIFF_TAGS},
			{LINE_DIFF_MINUS, FNC_COLOUR_DIFF_MINUS},
			{LINE_DIFF_PLUS, FNC_COLOUR_DIFF_PLUS},
			{LINE_DIFF_HUNK, FNC_COLOUR_DIFF_HUNK},
			{LINE_DIFF_EDIT, FNC_COLOUR_DIFF_SBS_EDIT}
		};

		return set_colour_scheme(s, pairs_diff, NULL,
		    nitems(pairs_diff));
	}
	case FNC_VIEW_TREE: {
		static const char *regexp_tree[] = {"@ ->", "/$", "\\*$", "^$"};
		const int pairs_tree[][2] = {
			{FNC_COLOUR_TREE_LINK, FNC_COLOUR_TREE_LINK},
			{FNC_COLOUR_TREE_DIR, FNC_COLOUR_TREE_DIR},
			{FNC_COLOUR_TREE_EXEC, FNC_COLOUR_TREE_EXEC},
			{FNC_COLOUR_COMMIT, FNC_COLOUR_COMMIT}
		};

		return set_colour_scheme(s, pairs_tree, regexp_tree,
		    nitems(regexp_tree));
	}
	case FNC_VIEW_TIMELINE: {
		static const char *regexp_timeline[] = {"^$", "^$", "^$"};
		const int pairs_timeline[][2] = {
			{FNC_COLOUR_COMMIT, FNC_COLOUR_COMMIT},
			{FNC_COLOUR_USER, FNC_COLOUR_USER},
			{FNC_COLOUR_DATE, FNC_COLOUR_DATE}
		};

		return set_colour_scheme(s, pairs_timeline, regexp_timeline,
		    nitems(regexp_timeline));
	}
	case FNC_VIEW_BLAME: {
		static const char *regexp_blame[] = {"^"};
		const int pairs_blame[][2] = {
			{FNC_COLOUR_COMMIT, FNC_COLOUR_COMMIT}
		};

		return set_colour_scheme(s, pairs_blame, regexp_blame,
		    nitems(regexp_blame));
	}
	case FNC_VIEW_BRANCH: {
		static const char *regexp_branch[] = {
			"^\\[[+]] ", "^\\[[-]] ", "@$", "\\*$"
		};
		const int pairs_branch[][2] = {
			{FNC_COLOUR_BRANCH_OPEN, FNC_COLOUR_BRANCH_OPEN},
			{FNC_COLOUR_BRANCH_CLOSED, FNC_COLOUR_BRANCH_CLOSED},
			{FNC_COLOUR_BRANCH_CURRENT, FNC_COLOUR_BRANCH_CURRENT},
			{FNC_COLOUR_BRANCH_PRIVATE, FNC_COLOUR_BRANCH_PRIVATE}
		};

		return set_colour_scheme(s, pairs_branch, regexp_branch,
		    nitems(regexp_branch));
	}
	default:
		return RC(FNC_RC_BAD_OPTION, "%d", vid);
	}
}

static int
set_colour_scheme(struct fnc_colours *colours, const int (*pairs)[2],
    const char **regexp, int n)
{
	int i;

	for (i = 0; i < n; ++i) {
		struct fnc_colour	*colour;
		int			 fgc, rc;

		colour = calloc(1, sizeof(*colour));
		if (colour == NULL)
			return RC_ERRNO("calloc");

		if (regexp != NULL) {
			rc = regcomp(&colour->regex, regexp[i],
			    REG_EXTENDED | REG_NEWLINE | REG_NOSUB);
			if (rc) {
				static char regerr[512];

				regerror(rc, &colour->regex, regerr,
				    sizeof(regerr));
				free(colour);
				return RC(FNC_RC_REGEX, "regcomp: %s: %s",
				    regexp[i], regerr);
			}
		}

		rc = init_colour(&fgc, pairs[i][1]);
		if (rc != FNC_RC_OK) {
			free(colour);
			return rc;
		}
		if (init_pair(pairs[i][0], fgc, -1) == ERR) {
			free(colour);
			return RC(FNC_RC_CURSES, "init_pair");
		}
		colour->scheme = pairs[i][0];
		STAILQ_INSERT_HEAD(colours, colour, entries);
	}

	return FNC_RC_OK;
}

static int
init_colour(int *colour, enum fnc_opt_id id)
{
	char	*val;
	int	 rc;

	*colour = default_colour(id);

	rc = fnc_conf_getopt(&val, id, false);
	if (rc != FNC_RC_OK)
		return rc;

	if (val == NULL)
		return FNC_RC_OK;

	if (strcasecmp(val, "black") == 0)
		*colour = COLOR_BLACK;
	else if (strcasecmp(val, "red") == 0)
		*colour = COLOR_RED;
	else if (strcasecmp(val, "green") == 0)
		*colour = COLOR_GREEN;
	else if (strcasecmp(val, "yellow") == 0)
		*colour = COLOR_YELLOW;
	else if (strcasecmp(val, "blue") == 0)
		*colour = COLOR_BLUE;
	else if (strcasecmp(val, "magenta") == 0)
		*colour = COLOR_MAGENTA;
	else if (strcasecmp(val, "cyan") == 0)
		*colour = COLOR_CYAN;
	else if (strcasecmp(val, "white") == 0)
		*colour = COLOR_WHITE;
	else if (strcasecmp(val, "default") == 0)
		*colour = -1;	/* default terminal foreground colour */

	free(val);
	return FNC_RC_OK;
}

/*
 * Return in *ret the local or global value of option id.
 * If ls is set, *ret will contain local and global values
 * for pretty printing. In either case, *ret must be freed.
 * If id has not been set or an error occurs, *ret will be NULL.
 * Return nonzero if id is not a valid option or on allocation error.
 */
static int
fnc_conf_getopt(char **ret, enum fnc_opt_id id, bool ls)
{
	struct fsl_cx	*f;
	struct fsl_db	*db;
	const char	*option, *envval = NULL;
	char		*optval;

	*ret = NULL;

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	option = fnc_conf_enum2str(id);
	if (option == NULL)
		return RC(FNC_RC_BAD_OPTION, "%d", id);

	db = fsl_needs_repo(f);
	if (db == NULL) {
		/* theoretically, this shouldn't happen */
		return RC(FNC_RC_NO_REPO, "fsl_needs_repo");
	}

	optval = fsl_db_g_text(db, NULL,
	    "SELECT value FROM config WHERE name=%Q", option);
	if (optval == NULL || ls) {
		envval = getenv(option);
		if (envval == NULL) {
			char *lower, *t;

			lower = strdup(option);
			if (lower == NULL) {
				free(optval);
				return RC_ERRNO("strdup");
			}
			for (t = lower; *t != '\0'; ++t)
				*t = tolower((unsigned char)*t);
			envval = getenv(lower);
			free(lower);
		}
	}

	if (ls && (optval != NULL || envval != NULL)) {
		char *showopt;

		showopt = fsl_mprintf("%s%s%s%s%s",
		    optval ? optval : "", optval ? " (local)" : "",
		    optval && envval ? ", " : "",
		    envval ? envval : "", envval ? " (envvar)" : "");
		free(optval);
		if (showopt == NULL)
			return RC_ERRNO("fsl_mprintf");
		optval = showopt;
	}

	if (ls || optval != NULL)
		*ret = optval;
	else if (envval != NULL) {
		*ret = strdup(envval);
		if (*ret == NULL)
			return RC_ERRNO("strdup");
	}

	return FNC_RC_OK;
}

static int
default_colour(enum fnc_opt_id id)
{
	switch (id) {
	case FNC_COLOUR_COMMIT:
	case FNC_COLOUR_DIFF_META:
	case FNC_COLOUR_TREE_EXEC:
	case FNC_COLOUR_BRANCH_CURRENT:
		return COLOR_GREEN;
	case FNC_COLOUR_USER:
	case FNC_COLOUR_DIFF_PLUS:
	case FNC_COLOUR_TREE_DIR:
	case FNC_COLOUR_BRANCH_OPEN:
		return COLOR_CYAN;
	case FNC_COLOUR_DATE:
	case FNC_COLOUR_DIFF_HUNK:
	case FNC_COLOUR_BRANCH_PRIVATE:
		return COLOR_YELLOW;
	case FNC_COLOUR_DIFF_MINUS:
	case FNC_COLOUR_DIFF_TAGS:
	case FNC_COLOUR_TREE_LINK:
	case FNC_COLOUR_BRANCH_CLOSED:
		return COLOR_MAGENTA;
	case FNC_COLOUR_HL_SEARCH:
		return COLOR_YELLOW;
	case FNC_COLOUR_DIFF_SBS_EDIT:
		return COLOR_RED;
	default:
		return -1;  /* Terminal default foreground colour. */
	}
}

static int
fnc_conf_setopt(enum fnc_opt_id id, const char *val, bool unset)
{
	struct fsl_cx	*f;
	struct fsl_db	*db;
	int		 rc;

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");
	if ((db = fsl_needs_repo(f)) == NULL)
		return RC(FNC_RC_NO_REPO);

	if (unset)
		rc = fsl_db_exec(db, "DELETE FROM config WHERE name=%Q",
		    fnc_conf_enum2str(id));
	else
		rc = fsl_db_exec(db,
		    "INSERT OR REPLACE INTO config(name, value, mtime) "
		    "VALUES(%Q, %Q, now())", fnc_conf_enum2str(id), val);

	return rc ? RC_LIBF(rc, "fsl_db_exec") : FNC_RC_OK;
}

struct fnc_colour *
get_colour(struct fnc_colours *colours, int scheme)
{
	struct fnc_colour *c = NULL;

	STAILQ_FOREACH(c, colours, entries) {
		if (c->scheme == scheme)
			return c;
	}

	return NULL;
}

struct fnc_colour *
match_colour(struct fnc_colours *colours, const char *line)
{
	struct fnc_colour *c = NULL;

	STAILQ_FOREACH(c, colours, entries) {
		if (match_line(line, &c->regex, 0, NULL))
			return c;
	}

	return NULL;
}

static int
match_line(const char *line, regex_t *regex, size_t nmatch,
    regmatch_t *regmatch)
{
	return regexec(regex, line, nmatch, regmatch, 0) == 0;
}

static void
free_colours(struct fnc_colours *colours)
{
	struct fnc_colour *c;

	while (!STAILQ_EMPTY(colours)) {
		c = STAILQ_FIRST(colours);
		STAILQ_REMOVE_HEAD(colours, entries);
		regfree(&c->regex);
		fsl_free(c);
	}
}

/*
 * Emulate vim(1) gg: User has 1 sec to follow first 'g' keypress with another.
 */
static bool
fnc_home(struct fnc_view *view)
{
	bool	home = true;

	halfdelay(10);	/* Block for 1 second, then return ERR. */
	if (wgetch(view->window) != 'g')
		home = false;
	cbreak();	/* Return to blocking mode on user input. */

	return home;
}

static int
cmd_blame(int argc, char **argv)
{
	struct fsl_cx	*f;
	struct fnc_view	*view;
	const char	*commit = NULL, *lineno = NULL, *repo = NULL;
	char		*path = NULL;
	fsl_id_t	 oid = 0, rid = 0;
	long		 nlimit = 0;
	int		 ch, rc = FNC_RC_OK;
	bool		 colour = true, reverse = false;

	while ((ch = getopt_long(argc, argv, "+Cc:hl:n:Rr:z", blame_opt,
	    NULL)) != -1) {
		switch (ch) {
		case 'C':
			colour = false;
			break;
		case 'c':
			commit = optarg;
			break;
		case 'h':
			usage_blame(0);
			/* NOTREACHED */
		case 'l':
			lineno = optarg;
			break;
		case 'n': {
			char	*n = optarg;
			size_t	 len;
			bool	 timed;

			len = strlen(n);
			if (len > 0 && n[len - 1] == 's') {
				n[len - 1] = '\0';
				timed = true;
			}
			rc = xstrtonum(&nlimit, n, 1, INT_MAX);
			if (rc)
				return rc;
			if (timed)
				nlimit *= -1;
			break;
		}
		case 'R':
			reverse = true;
			break;
		case 'r':
			repo = optarg;
			break;
		case 'z':
			fnc__utc = true;
			break;
		default:
			usage_blame(1);
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 1) {
		usage_blame(1);
		/* NOTREACHED */
	}

	if ((rc = fnc_cx_open(repo)) != 0)
		return rc;
	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	if (commit != NULL) {
		rc = symtorid(&rid, commit, FSL_SATYPE_CHECKIN);
		if (rc != 0)
			return rc;
	}
	if (rid == 0 || reverse) {
		if (reverse) {
			/*
			 * -r reverses the operation to go from the
			 * version specified with -c forward in time
			 * to the latest version, annotating each line
			 * with the version that first changed it
			 * (cf. fossil blame --origin 689182448 CHANGES.md).
			 */
			if (rid == 0)
				return RC(FNC_RC_BAD_OPTION,
				    "-r requires -c option");
			oid = rid;
		}
		fsl_ckout_version_info(f, &rid, NULL);
		if (rid == 0) {
			/* no work tree: -R|--repo option was used */
			rc = symtorid(&rid, "tip", FSL_SATYPE_CHECKIN);
			if (rc != 0)
				return rc;
		}
	}

	rc = resolve_path(&path, *argv, rid);
	if (rc != 0)
		return rc;

	rc = init_curses(colour);
	if (rc)
		goto end;

	rc = init_unveil(
		((const char *[]){ REPODIR, CKOUTDIR, fnc__tmpdir, tzfile() }),
		((const char *[]){ "rwc", "rwc", "rwc", "r" }), 4, 1
	);
	if (rc != 0)
		goto end;

	view = view_open(0, 0, 0, 0, FNC_VIEW_BLAME);
	if (view == NULL) {
		rc = RC(FNC_RC_CURSES, "view_open");
		goto end;
	}

	rc = open_blame_view(view, path, rid, oid, nlimit, lineno, colour);
	if (rc)
		goto end;

	rc = view_loop(view);

end:
	free(path);
	return rc;
}

static int
open_blame_view(struct fnc_view *view, char *path, fsl_id_t rid,
    fsl_id_t oid, int nlimit, const char *lineno, bool colour)
{
	struct fnc_blame_view_state	*s = &view->state.blame;
	int				 rc;

	SQ(INIT)(&s->blamed_commits);

	s->path = strdup(path);
	if (s->path == NULL)
		return RC_ERRNO("strdup");

	/* fsl_rid_to_uuid() returns NULL if fcli_cx() is NULL */
	s->commit_id = fsl_rid_to_uuid(fcli_cx(), rid);
	if (s->commit_id == NULL)
		return RC(FNC_RC_NO_RID, "fsl_rid_to_uuid");

	rc = fnc_commit_qid_alloc(&s->blamed_commit, s->commit_id);
	if (rc) {
		free(s->path);
		return rc;
	}

	SQ(INSERT_HEAD)(&s->blamed_commits, s->blamed_commit, entry);

	memset(&s->blame, 0, sizeof(s->blame));
	s->first_line_onscreen = 1;
	s->last_line_onscreen = view->nlines;
	s->selected_line = 1;
	s->blame_complete = false;
	s->blame.origin = oid;
	s->blame.nlimit = nlimit;
	s->spin_idx = 0;
	s->lineno = lineno;

	if (has_colors() && COLORS) {
		STAILQ_INIT(&s->colours);
		rc = set_colours(&s->colours, FNC_VIEW_BLAME);
		if (rc)
			return rc;
		view->colour = colour;
	}

	view->show = show_blame_view;
	view->input = blame_input_handler;
	view->close = close_blame_view;
	view->grep_init = blame_grep_init;
	view->grep = find_next_match;

	return run_blame(view);
}

static int
run_blame(struct fnc_view *view)
{
	struct fsl_cx			*f;
	struct fnc_blame_view_state	*s = &view->state.blame;
	struct fnc_blame		*blame = &s->blame;
	struct fsl_deck			 d = fsl_deck_empty;
	struct fsl_annotate_opt		*opt;
	const fsl_card_F		*cf;
	size_t				 i, len, nlines = 0;
	int				 rc;

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	rc = fsl_deck_load_sym(f, &d, s->blamed_commit->id,
	    FSL_SATYPE_CHECKIN);
	if (rc)
		return RC(FNC_RC_NO_REF, "fsl_deck_load_sym: %s",
		    s->blamed_commit->id);

	cf = fsl_deck_F_search(&d, s->path);
	if (cf == NULL) {
		rc = RC(FNC_RC_NO_TREE_ENTRY, "%s [%.10s]",
		    s->path, s->blamed_commit->id);
		goto end;
	}
	rc = fsl_card_F_content(f, cf, &blame->buf);
	if (rc) {
		rc = RC_ERRNO("fsl_card_F_content");
		goto end;
	}
	if (fsl_looks_like_binary(&blame->buf)) {
		rc = RC(FNC_RC_BLAME_BINARY);
		goto end;
	}

	opt = &blame->thread_cx.blame_opt;
	opt->filename = s->path;
	rc = symtorid(&opt->versionRid, s->blamed_commit->id,
	    FSL_SATYPE_CHECKIN);
	if (rc != 0)
		goto end;
	opt->originRid = blame->origin;  /* -c version when -r is passed */
	if (blame->nlimit < 0)
		opt->limitMs = abs(blame->nlimit) * 1000;
	else
		opt->limitVersions = blame->nlimit;
	opt->out = blame_cb;
	opt->outState = &blame->cb_cx;

	blame->line_offsets = malloc(sizeof(*blame->line_offsets));
	if (blame->line_offsets == NULL) {
		rc = RC_ERRNO("malloc");
		goto end;
	}
	rc = add_line_offset(&blame->line_offsets, &nlines, 0);
	if (rc != FNC_RC_OK)
		goto end;

	for (i = 0; i < blame->buf.used; i += len) {
		const unsigned char *lf;

		lf = memchr(&blame->buf.mem[i], '\n', blame->buf.used - i);
		if (lf == NULL)
			len = blame->buf.used - i;
		else
			len = (lf - &blame->buf.mem[i]) + 1;

		rc = add_line_offset(&blame->line_offsets, &nlines, i + len);
		if (rc != FNC_RC_OK)
			goto end;
	}
	if (nlines == 0) {
		s->blame_complete = true;
		goto end;
	}
	blame->nlines = nlines;

	/* don't include EOF \n in blame line count */
	if (blame->line_offsets[blame->nlines - 1] == (off_t)blame->buf.used)
		--blame->nlines;

	if (s->lineno) {
		long ln;

		rc = xstrtonum(&ln, s->lineno, 1, blame->nlines);
		if (rc)
			goto end;
		s->gtl = ln;
	}

	blame->lines = calloc(blame->nlines, sizeof(*blame->lines));
	if (blame->lines == NULL) {
		rc = RC_ERRNO("calloc");
		goto end;
	}

	blame->cb_cx.view = view;
	blame->cb_cx.lines = blame->lines;
	blame->cb_cx.nlines = blame->nlines;
	blame->cb_cx.commit_id = s->blamed_commit->id;
	blame->cb_cx.quit = &s->done;

	blame->thread_cx.path = s->path;
	blame->thread_cx.cb_cx = &blame->cb_cx;
	blame->thread_cx.complete = &s->blame_complete;
	blame->thread_cx.cancel_cb = cancel_blame;
	blame->thread_cx.cancel_cx = &s->done;
	s->blame_complete = false;

	if (s->first_line_onscreen + view->nlines - 1 > blame->nlines) {
		s->first_line_onscreen = 1;
		s->last_line_onscreen = view->nlines;
		s->selected_line = 1;
	}
	s->matched_line = 0;

end:
	fsl_deck_finalize(&d);
	if (rc)
		stop_blame(blame);
	return rc;
}

static int
show_blame_view(struct fnc_view *view)
{
	struct fnc_blame_view_state	*s = &view->state.blame;
	int				 rc;

	/*
	 * pthread_t is a pointer type to a struct pthread on OpenBSD but is
	 * an arithmetic type on linux so compare to 0 to work in both cases.
	 */
	if (!s->blame_complete && s->blame.thread_id == 0) {
		rc = pthread_create(&s->blame.thread_id, NULL, blame_thread,
		    &s->blame.thread_cx);
		if (rc)
			return RC_ERRNO_SET(rc, "pthread_create");

		halfdelay(1);	/* fast refresh rate while annotating  */
	}

	if (s->blame_complete)
		cbreak();	/* return to blocking mode */

	rc = draw_blame(view);
	drawborder(view);

	return rc;
}

static void *
blame_thread(void *state)
{
	struct fsl_cx			*f;
	struct fnc_blame_thread_cx	*cx = state;
	int				 rc, rcpt;

	if ((f = fcli_cx()) == NULL) {
		rc = RC(FNC_RC_FATAL, "fcli_cx");
		goto end;
	}

	rc = block_main_thread_signals();
	if (rc)
		goto end;

	rc = fsl_annotate(f, &cx->blame_opt);
	if (rc) {
		if (rc == FNC_RC_CANCELED)
			rc = RC_RESET;
		else
			rc = RC_LIBF(rc, "fsl_annotate");
	}

	rcpt = pthread_mutex_lock(&fnc__mutex);
	if (rcpt && rc == FNC_RC_OK) {
		rc = RC_ERRNO_SET(rcpt, "pthread_mutex_lock");
		goto end;
	}

	*cx->complete = true;

	rcpt = pthread_mutex_unlock(&fnc__mutex);
	if (rcpt && rc == FNC_RC_OK)
		rc = RC_ERRNO_SET(rcpt, "pthread_mutex_unlock");

end:
	return (void *)(intptr_t)rc;
}

static int
blame_cb(void *state, fsl_annotate_opt const * const opt,
    fsl_annotate_step const * const step)
{
	struct fnc_blame_cb_cx	*cx = state;
	struct fnc_blame_line	*line;
	int			 rcpt, rc = FNC_RC_OK;

	rcpt = pthread_mutex_lock(&fnc__mutex);
	if (rcpt)
		return RC_ERRNO_SET(rcpt, "pthread_mutex_lock");

	if (*cx->quit) {
		rc = RC(FNC_RC_CANCELED);
		goto end;
	}

	line = &cx->lines[step->lineNumber - 1];
	if (line->annotated)
		goto end;

	if (step->mtime) {
		line->id = strdup(step->versionHash);
		if (line->id == NULL) {
			rc = RC_ERRNO("strdup");
			goto end;
		}
		line->annotated = true;
	} else
		line->id = NULL;

	/* -r can return lines with no version (cf. fossil(1)) */
	if (opt->originRid && !line->id)
		line->annotated = false;

	line->lineno = step->lineNumber;
	++cx->nlines;

end:
	rcpt = pthread_mutex_unlock(&fnc__mutex);
	if (rcpt && rc == FNC_RC_OK)
		rc = RC_ERRNO_SET(rcpt, "pthread_mutex_unlock");
	return rc;
}

static int
draw_blame(struct fnc_view *view)
{
	struct fnc_blame_view_state	*s = &view->state.blame;
	struct fnc_blame		*blame = &s->blame;
	struct fnc_blame_line		*blame_line;
	regmatch_t			*regmatch = &view->regmatch;
	struct fnc_colour		*c = NULL;
	wchar_t				*wline;
	char				*line = NULL;
	fsl_uuid_cstr			 prev_id = NULL;
	ssize_t				 linelen;
	size_t				 linesz = 0;
	int				 width, lineno = 0, nprinted = 0;
	int				 rc = FNC_RC_OK;
	const int			 idfield = 11;  /* Prefix + space. */
	bool				 selected;

	fsl_buffer_rewind(&blame->buf);
	werase(view->window);

	if ((line = fsl_mprintf("checkin %s", s->blamed_commit->id)) == NULL) {
		rc = RC_ERRNO("fsl_mprintf");
		return rc;
	}

	rc = formatln(&wline, &width, NULL, line, 0, view->ncols, 0, false);
	free(line);
	line = NULL;
	if (rc)
		return rc;
	if (view_is_shared(view))
		wattron(view->window, fnc__highlight);
	if (view->colour)
		c = get_colour(&s->colours, FNC_COLOUR_COMMIT);
	if (c)
		wattr_on(view->window, COLOR_PAIR(c->scheme), NULL);
	waddwstr(view->window, wline);
	while (width < view->ncols) {
		waddch(view->window, ' ');
		++width;
	}
	if (c)
		wattr_off(view->window, COLOR_PAIR(c->scheme), NULL);
	if (view_is_shared(view))
		wattroff(view->window, fnc__highlight);
	free(wline);
	wline = NULL;

	line = fsl_mprintf("[%d/%d] %s%s %c", s->gtl ? s->gtl :
	    MIN(blame->nlines, s->first_line_onscreen - 1 + s->selected_line),
	    blame->nlines, s->blame_complete ? "/" : "annotating... /",
	    s->path, s->blame_complete ? ' ' : SPINNER[s->spin_idx]);
	if (SPINNER[++s->spin_idx] == '\0')
		s->spin_idx = 0;
	rc = formatln(&wline, &width, NULL, line, 0, view->ncols, 0, false);
	free(line);
	line = NULL;
	if (rc)
		return rc;
	waddwstr(view->window, wline);
	free(wline);
	wline = NULL;
	if (width < view->ncols - 1)
		waddch(view->window, '\n');

	s->eof = false;
	view->pos.maxx = 0;
	while (nprinted < view->nlines - 2) {
		int	col = 0;
		attr_t	rx = 0;

		width = 0;

		linelen = fsl_buffer_getline(&line, &linesz, &blame->buf);
		if (linelen == -1) {
			if (blame->buf.errCode == 0) {
				s->eof = true;
				break;
			}
			free(line);
			return RC_LIBF(blame->buf.errCode,
			    "fsl_buffer_getline");
		}

		if (++lineno < s->first_line_onscreen)
			continue;
		if (s->gtl) {
			rc = gotoline(view, &lineno, &nprinted);
			if (rc != 0) {
				free(line);
				return rc;
			}
			if (s->gtl != 0)
				continue;
		}

		if ((selected = nprinted == s->selected_line - 1)) {
			rx = fnc__highlight;
			wattron(view->window, rx);
		}

		if (blame->nlines > 0) {
			blame_line = &blame->lines[lineno - 1];
			if (blame_line->annotated && prev_id && !selected &&
			    fsl_uuidcmp(prev_id, blame_line->id) == 0) {
				waddstr(view->window, "          ");
			} else if (blame_line->annotated) {
				char *id_str;

				id_str = strndup(blame_line->id, idfield - 1);
				if (id_str == NULL) {
					free(line);
					return RC_ERRNO("strndup");
				}
				if (view->colour)
					c = get_colour(&s->colours,
					    FNC_COLOUR_COMMIT);
				if (c)
					wattr_on(view->window,
					    COLOR_PAIR(c->scheme), NULL);
				wprintw(view->window, "%.*s", idfield - 1,
				    id_str);
				if (c)
					wattr_off(view->window,
					    COLOR_PAIR(c->scheme), NULL);
				free(id_str);
				prev_id = blame_line->id;
			} else {
				waddstr(view->window, "..........");
				prev_id = NULL;
			}
			if (s->showln)
				col = draw_lineno(view, blame->nlines,
				    blame_line->lineno, rx);
		} else {
			waddstr(view->window, "..........");
			prev_id = NULL;
		}
		col += idfield;

		/* set maxx to longest line on the page */
		rc = formatln(&wline, &width, NULL, line, 0, INT_MAX, col,
		    true);
		if (rc) {
			free(line);
			return rc;
		}
		free(wline);
		wline = NULL;
		view->pos.maxx = MAX(view->pos.maxx, width);

		if (selected)
			wattroff(view->window, rx);
		waddch(view->window, ' ');

		if (view->ncols <= idfield) {
			wline = wcsdup(L"");
			if (wline == NULL) {
				rc = RC_ERRNO("wcsdup");
				free(line);
				return rc;
			}
		} else if (s->first_line_onscreen + nprinted == s->matched_line
		    && regmatch->rm_so >= 0 &&
		    regmatch->rm_so < regmatch->rm_eo) {
			rc = draw_matched_line(&width, line, view->pos.x,
			    view->ncols - col, 0, view->window, regmatch, 0);
			if (rc != 0) {
				free(line);
				return rc;
			}
		} else {
			int skip;

			rc = formatln(&wline, &width, &skip, line,
			    view->pos.x, view->ncols - col, col, true);
			if (rc) {
				free(line);
				return rc;
			}
			waddwstr(view->window, &wline[skip]);
			free(wline);
			wline = NULL;
		}
		col += MAX(width - view->pos.x, 0);

		if (col < view->ncols)
			waddch(view->window, '\n');
		if (++nprinted == 1)
			s->first_line_onscreen = lineno;
	}
	free(line);
	s->last_line_onscreen = lineno;
	drawborder(view);
	return rc;
}

/*
 * Draw column of line numbers up to nlines for the given view.
 */
static int
draw_lineno(struct fnc_view *view, int nlines, int lineno, attr_t rx)
{
	int npad = 0;

	ndigits(npad, nlines);  /* Number of digits to pad. */

	wattron(view->window, rx | A_BOLD);
	wprintw(view->window, " %*d ", npad, lineno);
	if (view->vid == FNC_VIEW_BLAME)  /* Don't highlight separator. */
		wattroff(view->window, fnc__highlight);
	waddch(view->window, (strcmp(nl_langinfo(CODESET), "UTF-8") == 0) ?
	    ACS_VLINE : '|');
	wattroff(view->window, rx | A_BOLD);

	npad += 3;  /* {ap,pre}pended ' ' + line separator */

	return npad;
}

static int
gotoline(struct fnc_view *view, int *lineno, int *nprinted)
{
	struct fsl_buffer	*b;
	int			*first, *selected, *gtl;
	bool			*eof;

	if (view->vid == FNC_VIEW_BLAME) {
		struct fnc_blame_view_state *s = &view->state.blame;

		first = &s->first_line_onscreen;
		selected = &s->selected_line;
		gtl = &s->gtl;
		eof = &s->eof;
		b = &s->blame.buf;
	} else if (view->vid == FNC_VIEW_DIFF) {
		struct fnc_diff_view_state *s = &view->state.diff;

		first = &s->first_line_onscreen;
		selected = &s->selected_line;
		gtl = &s->gtl;
		eof = &s->eof;
		b = &s->buf;
	} else
		return RC(FNC_RC_BAD_OPTION, "gotoline");

	if (*first != 1 && (*lineno >= *gtl - (view->nlines - 3) / 2)) {
		/* requested line is before this page, rewind */
		fsl_buffer_rewind(b);
		*nprinted = 0;
		*eof = false;
		*first = 1;
		*lineno = 0;
	} else if (!(*lineno < *gtl - (view->nlines - 3) / 2)) {
		/* requested line is on the 1st half of the page, select it */
		*selected = *gtl <= (view->nlines - 3) / 2 ?
		    *gtl : (view->nlines - 3) / 2 + 1;
		*gtl = 0;
	}
	return FNC_RC_OK;
}

static int
blame_input_handler(struct fnc_view **alt_view, struct fnc_view *view, int ch)
{
	struct fsl_cx			*f;
	struct fnc_view			*diff_view;
	struct fnc_blame_view_state	*s = &view->state.blame;
	const char			*id;
	int				 y = 0, x = 0, rc = FNC_RC_OK;
	uint16_t			 eos, nscroll;

	if ((f = fcli_cx()) == NULL)
		rc = RC(FNC_RC_FATAL, "fcli_cx");

	eos = nscroll = view->nlines - 2;
	if (view_is_top_split(view))
		--eos;	/* account for the border */

	switch (ch) {
	case '0':
		view->pos.x = 0;
		break;
	case '$':
		view->pos.x = view->pos.maxx - view->ncols / 2;
		break;
	case KEY_RIGHT:
	case 'l':
		if (view->pos.x + view->ncols / 2 < view->pos.maxx)
			view->pos.x += 2;
		break;
	case KEY_LEFT:
	case 'h':
		view->pos.x -= MIN(view->pos.x, 2);
		break;
	case 'q':
		s->done = true;
		break;
	case 'C':
		if (COLORS)
			view->colour = !view->colour;
		break;
	case 'g':
		if (!fnc_home(view))
			break;
		/* FALL THROUGH */
	case KEY_HOME:
		s->selected_line = 1;
		s->first_line_onscreen = 1;
		break;
	case KEY_END:
	case 'G':
		if (s->blame.nlines < eos) {
			s->selected_line = s->blame.nlines;
			s->first_line_onscreen = 1;
		} else {
			s->selected_line = eos;
			s->first_line_onscreen = s->blame.nlines - (eos - 1);
		}
		break;
	case KEY_DOWN:
	case 'j':
		if (s->selected_line < eos && s->first_line_onscreen +
		    s->selected_line <= s->blame.nlines)
			++s->selected_line;
		else if (s->last_line_onscreen < s->blame.nlines - eos + 1)
			++s->first_line_onscreen;
		break;
	case KEY_UP:
	case 'k':
		if (s->selected_line > 1)
			--s->selected_line;
		else if (s->selected_line == 1 && s->first_line_onscreen > 1)
			--s->first_line_onscreen;
		break;
	case CTRL('d'):
		nscroll >>= 1;
		/* FALL THROUGH */
	case KEY_NPAGE:
	case CTRL('f'):
	case ' ':
		if (s->last_line_onscreen >= s->blame.nlines) {
			if (s->selected_line >= MIN(s->blame.nlines,
			    view->nlines - 2))
				break;
			s->selected_line += MIN(nscroll, s->last_line_onscreen -
			    s->first_line_onscreen - s->selected_line + 1);
			break;
		}
		if (s->last_line_onscreen + nscroll <= s->blame.nlines)
			s->first_line_onscreen += nscroll;
		else
			s->first_line_onscreen =
			    s->blame.nlines - (view->nlines - 3);
		break;
	case CTRL('u'):
		nscroll >>= 1;
		/* FALL THROUGH */
	case KEY_PPAGE:
	case CTRL('b'):
		if (s->first_line_onscreen == 1) {
			s->selected_line = MAX(1, s->selected_line - nscroll);
			break;
		}
		if (s->first_line_onscreen > nscroll)
			s->first_line_onscreen -= nscroll;
		else
			s->first_line_onscreen = 1;
		break;
	case '@': {
		struct input input;

		memset(&input, 0, sizeof(input));
		input.data = (int []){1, s->blame.nlines};
		input.prompt = "line: ";
		input.type = INPUT_NUMERIC;
		input.flags = SR_CLREOL;

		rc = fnc_prompt_input(view, &input);
		if (rc)
			return rc;
		s->gtl = input.ret;
		break;
	}
	case '#':
		s->showln = !s->showln;
		break;
	case 'c':
	case 'p':
		id = get_selected_commit_id(s->blame.lines, s->blame.nlines,
		    s->first_line_onscreen, s->selected_line);
		if (id == NULL)
			break;
		if (ch == 'p') {
			struct fsl_deck	 d = fsl_deck_empty;
			struct fsl_db	*db;
			char		*pid;
			fsl_id_t	 rid;

			if ((db = fsl_needs_repo(f)) == NULL)
				return RC(FNC_RC_NO_REPO);

			rc = idtorid(&rid, id, FNC_RC_NO_COMMIT);
			if (rc != 0)
				return rc;

			if ((rc = fsl_db_get_text(db, &pid, NULL,
			    "SELECT uuid FROM plink, blob "
			    "WHERE plink.cid=%d "
			    "AND blob.rid=plink.pid "
			    "AND plink.isprim", rid)) != 0) {
				rc = RC_LIBF(rc, "fsl_db_get_text");
				break;
			}
			if (pid == NULL) {
				sitrep(view, SR_ALL ^ SR_RESET,
				    ":parent of [%.12s] not found", id);
				break;
			}

			/* Check file exists in parent check-in. */
			rc = fsl_deck_load_sym(f, &d, pid, FSL_SATYPE_CHECKIN);
			if (rc != 0) {
				free(pid);
				return RC_LIBF(rc, "fsl_deck_load_sym");
			}
			rc = fsl_deck_F_rewind(&d);
			if (rc != 0) {
				free(pid);
				return RC_LIBF(rc, "fsl_deck_rewind");
			}
			if (fsl_deck_F_search(&d, s->path) == NULL) {
				sitrep(view, SR_ALL ^ SR_RESET,
				    ":/%s not in [%.12s]", s->path, pid);
				free(pid);
				break;
			}
			rc = fnc_commit_qid_alloc(&s->blamed_commit, pid);
			fsl_deck_finalize(&d);
			free(pid);
			if (rc != 0)
				return rc;
		} else {
			if (fsl_uuidcmp(id, s->blamed_commit->id) == 0)
				break;
			rc = fnc_commit_qid_alloc(&s->blamed_commit, id);
			if (rc != 0)
				return rc;
		}
		s->done = true;
		rc = stop_blame(&s->blame);
		if (rc)
			return rc;
		s->done = false;
		SQ(INSERT_HEAD)(&s->blamed_commits, s->blamed_commit, entry);
		rc = run_blame(view);
		break;
	case KEY_BACKSPACE:
	case 'P': {
		struct fnc_commit_qid *first;

		first = SQ(FIRST)(&s->blamed_commits);
		if (fsl_uuidcmp(first->id, s->commit_id) == 0)
			break;
		s->done = true;
		rc = stop_blame(&s->blame);
		s->done = false;
		if (rc)
			break;
		SQ(REMOVE_HEAD)(&s->blamed_commits, entry);
		fnc_commit_qid_free(s->blamed_commit);
		s->blamed_commit = SQ(FIRST)(&s->blamed_commits);
		rc = run_blame(view);
		break;
	}
	case 'b':
		return view_request_new(alt_view, view, FNC_VIEW_BRANCH);
	case 't':
		id = get_selected_commit_id(s->blame.lines, s->blame.nlines,
		    s->first_line_onscreen, s->selected_line);
		if (id == NULL)
			break;

		rc = idtorid(&s->line_rid, id, FNC_RC_NO_COMMIT);
		if (rc != 0)
			return rc;

		return view_request_new(alt_view, view, FNC_VIEW_TIMELINE);
	case KEY_ENTER:
	case '\r': {
		struct fnc_commit_artifact	*commit;
		struct fsl_stmt			 q = fsl_stmt_empty;
		int				 finalize_rc, rid;

		id = get_selected_commit_id(s->blame.lines, s->blame.nlines,
		    s->first_line_onscreen, s->selected_line);
		if (id == NULL)
			break;
		if (s->selected_entry)
			fnc_commit_artifact_close(s->selected_entry);

		rc = idtorid(&rid, id, FNC_RC_NO_COMMIT);
		if (rc != 0)
			return rc;

		rc = commit_builder(&commit, rid, &q);
		finalize_rc = fsl_stmt_finalize(&q);
		if (finalize_rc != 0 && rc == 0)
			rc = RC_LIBF(finalize_rc, "fsl_stmt_finalize");
		if (rc != 0) {
			fnc_commit_artifact_close(commit);
			return rc;
		}

		if (*alt_view != NULL) {
			/* release diff resources before opening anew */
			rc = close_diff_view(*alt_view);
			if (rc)
				break;
			diff_view = *alt_view;
		} else {
			if (view_is_parent(view))
				view_split_getyx(view, &y, &x);
			diff_view = view_open(0, 0, y, x, FNC_VIEW_DIFF);
			if (diff_view == NULL) {
				fnc_commit_artifact_close(commit);
				rc = RC(FNC_RC_CURSES, "view_open");
				break;
			}
		}
		rc = open_diff_view(diff_view, commit, NULL, DIFF_MODE_META,
		    -1, FNC_DIFF_PROTOTYPE | FNC_DIFF_VERBOSE, view->colour, 0);
		s->selected_entry = commit;
		if (rc) {
			fnc_commit_artifact_close(commit);
			view_close(diff_view);
			break;
		}
		if (*alt_view)  /* view is already active */
			break;
		if (view_is_parent(view) && view->mode == VIEW_SPLIT_HRZN) {
			rc = view_split_horizontally(view, y);
			if (rc != FNC_RC_OK)
				break;
		}
		view->active = false;
		diff_view->active = true;
		diff_view->mode = view->mode;
		diff_view->nlines = view->lines - y;
		if (view_is_parent(view)) {
			view_copy_size(diff_view, view);
			rc = view_close_child(view);
			if (rc != FNC_RC_OK)
				return rc;
			rc = view_set_child(view, diff_view);
			if (rc != FNC_RC_OK)
				return rc;
			view->focus_child = true;
		} else
			*alt_view = diff_view;
		break;
	}
	case KEY_RESIZE:
		if (s->selected_line > view->nlines - 2) {
			s->selected_line = MIN(s->blame.nlines,
			    view->nlines - 2);
		}
		/* FALL THROUGH */
	default:
		break;
	}
	return rc;
}

static void
blame_grep_init(struct fnc_view *view)
{
	struct fnc_blame_view_state *s = &view->state.blame;

	s->matched_line = 0;
}

static const char *
get_selected_commit_id(struct fnc_blame_line *lines, int nlines,
    int first_line_onscreen, int selected_line)
{
	struct fnc_blame_line *line;

	if (nlines <= 0)
		return NULL;

	line = &lines[first_line_onscreen - 1 + selected_line - 1];
	if (!line->annotated)
		return NULL;

	return line->id;
}

static int
fnc_commit_qid_alloc(struct fnc_commit_qid **qid, fsl_uuid_cstr id)
{
	*qid = calloc(1, sizeof(**qid));
	if (*qid == NULL)
		return RC_ERRNO("calloc");

	memcpy((*qid)->id, id, sizeof((*qid)->id));
	return FNC_RC_OK;
}

static int
close_blame_view(struct fnc_view *view)
{
	struct fnc_blame_view_state	*s = &view->state.blame;
	int				 rc;

	rc = stop_blame(&s->blame);

	while (!SQ(EMPTY)(&s->blamed_commits)) {
		struct fnc_commit_qid *blamed_commit;

		blamed_commit = SQ(FIRST)(&s->blamed_commits);
		SQ(REMOVE_HEAD)(&s->blamed_commits, entry);
		fnc_commit_qid_free(blamed_commit);
	}

	free(s->path);
	free(s->commit_id);
	free_colours(&s->colours);
	if (s->selected_entry)
		fnc_commit_artifact_close(s->selected_entry);

	return rc;
}

static int
stop_blame(struct fnc_blame *blame)
{
	void	*err = 0;
	int	 idx, rc = FNC_RC_OK;

	if (blame->thread_id != 0) {
		rc = pthread_mutex_unlock(&fnc__mutex);
		if (rc)
			return RC_ERRNO_SET(rc, "pthread_mutex_unlock");
		rc = pthread_join(blame->thread_id, &err);
		if (rc || err == PTHREAD_CANCELED)
			return RC_ERRNO_SET(rc ? rc : ECANCELED,
			    "pthread_join");
		rc = pthread_mutex_lock(&fnc__mutex);
		if (rc)
			return RC_ERRNO_SET(rc, "pthread_mutex_lock");

		blame->thread_id = 0;
	}
	fsl_buffer_clear(&blame->buf);
	if (blame->lines) {
		for (idx = 0; idx < blame->nlines; ++idx)
			free(blame->lines[idx].id);
		free(blame->lines);
		blame->lines = NULL;
	}
	free(blame->line_offsets);
	blame->line_offsets = NULL;
	return rc ? rc : (intptr_t)err;
}

static int
cancel_blame(void *state)
{
	int	*done = state;
	int	 rcpt, rc = FNC_RC_OK;

	rcpt = pthread_mutex_lock(&fnc__mutex);
	if (rcpt)
		return RC_ERRNO_SET(rcpt, "pthread_mutex_lock");

	if (*done)
		rc = RC(FNC_RC_CANCELED);

	rcpt = pthread_mutex_unlock(&fnc__mutex);
	if (rcpt && rc == FNC_RC_OK)
		rc = RC_ERRNO_SET(rcpt, "pthread_mutex_unlock");
	return rc;
}

static void
fnc_commit_qid_free(struct fnc_commit_qid *qid)
{
	free(qid);
	qid = NULL;
}

static int
cmd_branch(int argc, char **argv)
{
	struct fnc_view	*view;
	const char	*glob = NULL, *repo = NULL;
	double		 dateline;
	int		 ch, rc, when = 0;
	int		 branch_flags = BRANCH_LS_OPEN_CLOSED;
	bool		 colour = true;

	while ((ch = getopt_long(argc, argv, "+a:b:CchopRr:s:z", branch_opt,
	    NULL)) != -1) {
		switch (ch) {
		case 'a':
			if (when)
				return RC(FNC_RC_BAD_OPTION,
				    "-a and -b are mutually exclusive");
			when = 1;
			rc = fnc_date_to_mtime(&dateline, optarg, when);
			if (rc)
				return rc;
			break;
		case 'b':
			if (when)
				return RC(FNC_RC_BAD_OPTION,
				    "-a and -b are mutually exclusive");
			when = -1;
			rc = fnc_date_to_mtime(&dateline, optarg, when);
			if (rc)
				return rc;
			break;
		case 'C':
			colour = false;
			break;
		case 'c':
			if (branch_flags == BRANCH_LS_OPEN_ONLY)
				return RC(FNC_RC_BAD_OPTION,
				    "-c and -o are mutually exclusive");
			branch_flags = BRANCH_LS_CLOSED_ONLY;
			break;
		case 'h':
			usage_branch(0);
			/* NOTREACHED */
		case 'o':
			if (branch_flags == BRANCH_LS_CLOSED_ONLY)
				return RC(FNC_RC_BAD_OPTION,
				    "-c and -o are mutually exclusive");
			branch_flags = BRANCH_LS_OPEN_ONLY;
			break;
		case 'p':
			FLAG_SET(branch_flags, BRANCH_LS_NO_PRIVATE);
			break;
		case 'R':
			FLAG_SET(branch_flags, BRANCH_SORT_REVERSE);
			break;
		case 'r':
			repo = optarg;
			break;
		case 's':
			if (strcmp(optarg, "mru") == 0)
				FLAG_SET(branch_flags, BRANCH_SORT_MTIME);
			else if (strcmp(optarg, "state") == 0)
				FLAG_SET(branch_flags, BRANCH_SORT_STATUS);
			else {
				fprintf(stderr, "%s: 'config -s' order "
				    "must be one of \"mru\" or \"state\"\n",
				    fnc__progname);
				return RC(FNC_RC_BAD_OPTION, "%s", optarg);
			}
			break;
		case 'z':
			fnc__utc = true;
			break;
		default:
			usage_branch(1);
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;

	if (argc > 1) {
		usage_branch(1);
		/* NOTREACHED */
	}

	if (argc == 1)
		glob = argv[0];

	if ((rc = fnc_cx_open(repo)) != 0)
		return rc;

	rc = init_curses(colour);
	if (rc)
		return rc;

	rc = init_unveil(
		((const char *[]){ REPODB, CKOUTDIR, fnc__tmpdir, tzfile() }),
		((const char *[]){ "rw", "rwc", "rwc", "r" }), 4, 1
	);
	if (rc != 0)
		return rc;

	view = view_open(0, 0, 0, 0, FNC_VIEW_BRANCH);
	if (view == NULL)
		return RC(FNC_RC_CURSES, "view_open");

	rc = open_branch_view(view, branch_flags, glob, dateline, when, colour);
	if (rc)
		return rc;

	return view_loop(view);
}

static int
open_branch_view(struct fnc_view *view, int branch_flags, const char *glob,
    double dateline, int when, bool colour)
{
	struct fnc_branch_view_state	*s = &view->state.branch;
	int				 rc;

	s->selected_entry = 0;
	s->branch_flags = branch_flags;
	s->branch_glob = glob;
	s->dateline = dateline;
	s->when = when;

	rc = fnc_load_branches(s);
	if (rc)
		goto end;

	if (has_colors() && COLORS) {
		STAILQ_INIT(&s->colours);
		rc = set_colours(&s->colours, FNC_VIEW_BRANCH);
		if (rc)
			goto end;
		view->colour = colour;
	}

	view->show = show_branch_view;
	view->input = branch_input_handler;
	view->close = close_branch_view;
	view->grep_init = branch_grep_init;
	view->grep = branch_search_next;

end:
	if (rc)
		fnc_free_branches(&s->branches);
	return rc;
}

static int
fnc_load_branches(struct fnc_branch_view_state *s)
{
	struct fsl_cx		*f;
	struct fsl_buffer	 sql = fsl_buffer_empty;
	struct fsl_stmt		 stmt = fsl_stmt_empty;
	char			*curr_branch = NULL;
	fsl_id_t		 ckoutrid;
	int			 finalize_rc, rc;

	s->nbranches = 0;
	TAILQ_INIT(&s->branches);

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	rc = create_tmp_branchlist_table();
	if (rc)
		return rc;

	if ((rc = fsl_buffer_reserve(&sql, 256)) != 0)
		return RC_ERRNO("malloc");

	switch (FLAG_CHK(s->branch_flags, BRANCH_LS_BITMASK)) {
	case BRANCH_LS_OPEN_CLOSED:
		buf_write(&sql,
		    "SELECT name, isprivate, isclosed, mtime"
		    " FROM tmp_brlist WHERE 1", -1);
		break;
	case BRANCH_LS_OPEN_ONLY:
		buf_write(&sql,
		    "SELECT name, isprivate, isclosed, mtime"
		    " FROM tmp_brlist WHERE NOT isclosed", -1);
		break;
	case BRANCH_LS_CLOSED_ONLY:
		buf_write(&sql,
		    "SELECT name, isprivate, isclosed, mtime"
		    " FROM tmp_brlist WHERE isclosed", -1);
		break;
	}
	if (sql.errCode != 0) {
		rc = RC_LIBF(sql.errCode, "buf_write");
		goto end;
	}

	if (s->branch_glob) {
		char *op, *str;

		rc = fnc_make_sql_glob(&op, &str, s->branch_glob);
		if (rc)
			goto end;
		rc = fsl_buffer_appendf(&sql, " AND name %q %Q", op, str);
		free(op);
		free(str);
		if (rc != 0) {
			rc = RC_LIBF(rc, "fsl_buffer_appendf");
			goto end;
		}
	}

	if (FLAG_CHK(s->branch_flags, BRANCH_LS_NO_PRIVATE)) {
		if (buf_write(&sql, " AND NOT isprivate", 18) == -1) {
			rc = RC_LIBF(sql.errCode, "buf_write");
			goto end;
		}
	}

	if (FLAG_CHK(s->branch_flags, BRANCH_SORT_MTIME))
		buf_write(&sql, " ORDER BY -mtime", 16);
	else if (FLAG_CHK(s->branch_flags, BRANCH_SORT_STATUS))
		buf_write(&sql, " ORDER BY isclosed", 18);
	else
		buf_write(&sql, " ORDER BY name COLLATE nocase", 29);
	if (sql.errCode != 0) {
		rc = RC_LIBF(sql.errCode, "buf_write");
		goto end;
	}

	if (FLAG_CHK(s->branch_flags, BRANCH_SORT_REVERSE)) {
		if (buf_write(&sql," DESC", 5) == -1) {
			rc = RC_LIBF(sql.errCode, "buf_write");
			goto end;
		}
	}

	rc = fsl_cx_prepare(f, &stmt, fsl_buffer_cstr(&sql));
	if (rc != 0) {
		rc = RC_LIBF(rc, "fsl_cx_prepare");
		goto end;
	}

	fsl_ckout_version_info(f, &ckoutrid, NULL);
	curr_branch = fsl_db_g_text(fsl_needs_repo(f), NULL,
	    "SELECT value FROM tagxref WHERE rid=%d AND tagid=%d",
	    ckoutrid, 8);

	while ((rc = fsl_stmt_step(&stmt)) == FSL_RC_STEP_ROW) {
		struct fnc_branch		*new_branch;
		struct fnc_branchlist_entry	*be;
		const char			*name;
		double				 mtime;
		bool				 curr, open, priv;

		name = fsl_stmt_g_text(&stmt, 0, NULL);
		priv = curr_branch != NULL && fsl_stmt_g_int32(&stmt, 1) == 1;
		open = fsl_stmt_g_int32(&stmt, 2) == 0;
		mtime = fsl_stmt_g_int64(&stmt, 3);
		curr = curr_branch != NULL && strcmp(curr_branch, name) == 0;
		if (name == NULL || *name == '\0' ||
		    (s->when > 0 && mtime < s->dateline) ||
		    (s->when < 0 && mtime > s->dateline))
			continue;
		rc = alloc_branch(&new_branch, name, mtime, open, priv, curr);
		if (rc)
			goto end;
		rc = fnc_branchlist_insert(&be, &s->branches, new_branch);
		if (rc)
			goto end;
		if (be != NULL)
			be->idx = s->nbranches++;
	}
	if (rc == FSL_RC_STEP_DONE)
		rc = FNC_RC_OK;
	else {
		if (fsl_db_err_get(f->dbMain, NULL, NULL) != FNC_RC_OK)
			rc = RC_LIBF(fsl_cx_uplift_db_error(f, f->dbMain),
			    "fsl_stmt_step");
		else
			rc = RC_LIBF(rc, "fsl_stmt_step");
		goto end;
	}

	s->first_branch_onscreen = TAILQ_FIRST(&s->branches);
	if (stmt.rowCount == 0)
		rc = RC(FNC_RC_NO_MATCH, "%s", s->branch_glob);

end:
	free(curr_branch);
	fsl_buffer_clear(&sql);
	finalize_rc = fsl_stmt_finalize(&stmt);
	if (finalize_rc != 0 && rc == 0)
		rc = RC_LIBF(finalize_rc, "fsl_stmt_finalize");
	return rc;
}

static int
create_tmp_branchlist_table(void)
{
	struct fsl_cx		*f;
	struct fsl_db		*db;  /* -R|--repo option */
	static const char	 tmp_branchlist_table[] =
	    "CREATE TEMP TABLE IF NOT EXISTS tmp_brlist AS "
	    "SELECT tagxref.value AS name,"
	    " max(event.mtime) AS mtime,"
	    " EXISTS(SELECT 1 FROM tagxref AS tx WHERE tx.rid=tagxref.rid"
	    "  AND tx.tagid=(SELECT tagid FROM tag WHERE tagname='closed')"
	    "  AND tx.tagtype > 0) AS isclosed,"
	    " (SELECT tagxref.value FROM plink CROSS JOIN tagxref"
	    "  WHERE plink.pid=event.objid"
	    "  AND tagxref.rid=plink.cid"
	    "  AND tagxref.tagid=(SELECT tagid FROM tag WHERE tagname='branch')"
	    "  AND tagtype>0) AS mergeto,"
	    " count(*) AS nckin,"
	    " (SELECT uuid FROM blob WHERE rid=tagxref.rid) AS ckin,"
	    " event.bgcolor AS bgclr,"
	    " EXISTS(SELECT 1 FROM private WHERE rid=tagxref.rid) AS isprivate "
	    "FROM tagxref, tag, event "
	    "WHERE tagxref.tagid=tag.tagid"
	    " AND tagxref.tagtype>0"
	    " AND tag.tagname='branch'"
	    " AND event.objid=tagxref.rid "
	    "GROUP BY 1;";
	int rc;

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");
	if ((db = fsl_needs_repo(f)) == NULL)
		return RC(FNC_RC_NO_REPO);

	rc = fsl_db_exec(db, tmp_branchlist_table);
	return rc ? RC_LIBF(rc, "fsl_db_exec") : FNC_RC_OK;
}

static int
alloc_branch(struct fnc_branch **ret, const char *name, double mtime,
    bool open, bool priv, bool curr)
{
	struct fsl_cx		*f;
	struct fnc_branch	*branch;
	char			*id = NULL;
	int			 rc;

	*ret = NULL;

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	branch = calloc(1, sizeof(*branch));
	if (branch == NULL)
		return RC_ERRNO("calloc");

	rc = fsl_sym_to_uuid(f, name, FSL_SATYPE_ANY, &id, NULL);
	if (rc || id == NULL) {
		rc = RC(FNC_RC_NO_BRANCH, "fsl_sym_to_uuid: %s", name);
		goto end;
	}

	branch->name = strdup(name);
	if (branch->name == NULL) {
		rc = RC_ERRNO("strdup");
		goto end;
	}

	branch->id = id;
	branch->mtime = fsl_julian_to_unix(mtime);

	if (open)
		FLAG_SET(branch->state, BRANCH_STATE_OPEN);
	if (priv)
		FLAG_SET(branch->state, BRANCH_STATE_PRIV);
	if (curr)
		FLAG_SET(branch->state, BRANCH_STATE_CURR);

end:
	if (rc) {
		fnc_branch_close(branch);
		branch = NULL;
	} else
		*ret = branch;
	return rc;
}

static int
fnc_branchlist_insert(struct fnc_branchlist_entry **newp,
    struct fnc_branchlist_head *branches, struct fnc_branch *branch)
{
	struct fnc_branchlist_entry *new, *be;

	*newp = NULL;

	/*
	 * Deduplicate (extremely unlikely or impossible?) entries on insert.
	 * Don't force lexicographical order; we already retrieved the branch
	 * names from the database using a query to obtain (a) lexicographical
	 * or (b) user-specified sorted results (i.e., MRU or LRU).
	 */
	be = TAILQ_FIRST(branches);
	while (be != NULL) {
		if (strcmp(be->branch->name, branch->name) == 0)
			return FNC_RC_OK;
		be = TAILQ_NEXT(be, entries);
	}

	new = malloc(sizeof(*new));
	if (new == NULL)
		return RC_ERRNO("malloc");

	new->branch = branch;
	TAILQ_INSERT_TAIL(branches, new, entries);
	*newp = new;
	return FNC_RC_OK;
}

static int
show_branch_view(struct fnc_view *view)
{
	struct fnc_branch_view_state	*s = &view->state.branch;
	struct fnc_branchlist_entry	*be;
	struct fnc_colour		*c = NULL;
	char				*line;
	wchar_t				*wline;
	int				 limit, n, width, rc;
	uint_fast8_t			 idmaxlen = FSL_UUID_STRLEN_MIN;
	attr_t				 rx = 0;

	s->ndisplayed = 0;
	werase(view->window);

	limit = view->nlines;
	if (view_is_top_split(view))
		--limit;	/* account for the border */
	if (limit == 0)
		return FNC_RC_OK;

	/*
	 * Determine whether SHA1 or SHA3 or both hashes are used
	 * so we know what length to pad branches hashed with the
	 * shorter SHA1 string when showing hash IDs ('i' keymap).
	 */
	be = TAILQ_FIRST(&s->branches);
	while (be != NULL) {
		idmaxlen = MAX(strlen(be->branch->id), idmaxlen);
		be = TAILQ_NEXT(be, entries);
	}

	be = s->first_branch_onscreen;

	if ((line = fsl_mprintf("branches [%d/%d]", be->idx + s->selected + 1,
	    s->nbranches)) == NULL)
		return RC_ERRNO("fsl_mprintf");

	rc = formatln(&wline, &width, NULL, line, 0, view->ncols, 0, false);
	if (rc) {
		free(line);
		return rc;
	}
	if (view_is_shared(view) || view->active)
		rx = fnc__highlight;
	if (view->colour)
		c = get_colour(&s->colours, FNC_COLOUR_BRANCH_CURRENT);
	if (c)
		rx |= COLOR_PAIR(c->scheme);
	wattron(view->window, rx);
	waddwstr(view->window, wline);
	while (width < view->ncols) {
		waddch(view->window, ' ');
		++width;
	}
	wattroff(view->window, rx);
	free(wline);
	wline = NULL;
	free(line);
	line = NULL;
	if (width < view->ncols - 1)
		waddch(view->window, '\n');
	if (--limit <= 0)
		return FNC_RC_OK;

	n = 0;
	while (be != NULL && limit > 0) {
		char	*idstr;
		char	 id[FSL_STRLEN_K256 + 1];  /* NUL */
		char	 iso8601[ISO8601_DATE_ONLY + 1];  /* NUL */
		size_t	 idlen, npad;

		/* pad directories and sha1 in mixed repos with dots */
		idstr = be->branch->id;
		idlen = strlen(idstr);
		npad = idmaxlen - idlen;
		if (npad > 0) {
			char pad[npad + 1];  /* NUL */

			memset(pad, '.', npad);
			pad[npad] = '\0';

			if (idstr != NULL && memccpy(&id, idstr, '\0',
			    sizeof(id)) == NULL)
				return RC(FNC_RC_NO_SPACE, "memccpy");
			if (memccpy(&id[idlen], pad, '\0',
			    sizeof(id) - idlen) == NULL)
				return RC(FNC_RC_NO_SPACE, "memccpy");

			id[idmaxlen] = '\0';
			idstr = id;
		}
		if (s->show_date) {
			rc = fnc_strftime(&*iso8601, sizeof(iso8601),
			    "%G-%m-%d", be->branch->mtime);
			if (rc != FNC_RC_OK)
				return rc;
		}

		line = fsl_mprintf("[%c] %s%s%s%s%s%s%s",
		    FLAG_CHK(be->branch->state, BRANCH_STATE_OPEN) ? '+' : '-',
		    s->show_id ? idstr : "", s->show_id ? "  " : "",
		    s->show_date ? iso8601 : "", s->show_date ? "  " : "",
		    be->branch->name,
		    FLAG_CHK(be->branch->state, BRANCH_STATE_PRIV) ? "*" : "",
		    FLAG_CHK(be->branch->state, BRANCH_STATE_CURR) ? "@" : "");
		if (line == NULL)
			return RC_ERRNO("fsl_mprintf");

		if (view->colour)
			c = match_colour(&s->colours, line);

		rc = formatln(&wline, &width, NULL, line, 0, view->ncols, 0,
		    false);
		if (rc) {
			free(line);
			return rc;
		}

		if (n == s->selected) {
			if (view->active)
				wattr_on(view->window, fnc__highlight, NULL);
			s->selected_entry = be;
		}
		if (c)
			wattr_on(view->window, COLOR_PAIR(c->scheme), NULL);
		waddwstr(view->window, wline);
		if (c)
			wattr_off(view->window, COLOR_PAIR(c->scheme), NULL);
		if (width < view->ncols)
			waddch(view->window, '\n');
		if (n == s->selected && view->active)
			wattr_off(view->window, fnc__highlight, NULL);

		free(line);
		free(wline);
		wline = NULL;
		++n;
		++s->ndisplayed;
		--limit;
		s->last_branch_onscreen = be;
		be = TAILQ_NEXT(be, entries);
	}

	drawborder(view);
	return rc;
}

static int
branch_input_handler(struct fnc_view **new_view, struct fnc_view *view, int ch)
{
	struct fnc_branch_view_state	*s = &view->state.branch;
	struct fnc_branchlist_entry	*be;
	int				 n, rc = FNC_RC_OK;
	uint16_t			 eos, nscroll;

	eos = nscroll = view->nlines - 1;

	switch (ch) {
	case 'C':
		if (COLORS)
			view->colour = !view->colour;
		break;
	case 'd':
		s->show_date = !s->show_date;
		break;
	case 'i':
		s->show_id = !s->show_id;
		break;
	case KEY_ENTER:
	case '\r':
	case ' ':
		if (!s->selected_entry)
			break;
		rc = view_request_new(new_view, view, FNC_VIEW_TIMELINE);
		break;
	case 'o':
		/*
		 * Toggle branch list sort order (cf. branch --sort option):
		 * lexicographical (default) -> most recently used -> state
		 */
		if (FLAG_CHK(s->branch_flags, BRANCH_SORT_MTIME)) {
			FLAG_CLR(s->branch_flags, BRANCH_SORT_MTIME);
			FLAG_SET(s->branch_flags, BRANCH_SORT_STATUS);
		} else if (FLAG_CHK(s->branch_flags, BRANCH_SORT_STATUS))
			FLAG_CLR(s->branch_flags, BRANCH_SORT_STATUS);
		else
			FLAG_SET(s->branch_flags, BRANCH_SORT_MTIME);
		fnc_free_branches(&s->branches);
		rc = fnc_load_branches(s);
		break;
	case 't':
		if (s->selected_entry == NULL)
			break;
		return view_request_new(new_view, view, FNC_VIEW_TREE);
	case 'g':
		if (!fnc_home(view))
			break;
		/* FALL THROUGH */
	case KEY_HOME:
		s->selected = 0;
		s->first_branch_onscreen = TAILQ_FIRST(&s->branches);
		break;
	case KEY_END:
	case 'G':
		if (view_is_top_split(view))
			--eos;	/* account for the border */

		s->selected = 0;
		be = TAILQ_LAST(&s->branches, fnc_branchlist_head);

		for (n = 0; n < eos; ++n) {
			if (be == NULL)
				break;
			s->first_branch_onscreen = be;
			be = TAILQ_PREV(be, fnc_branchlist_head, entries);
		}
		if (n > 0)
			s->selected = n - 1;
		break;
	case KEY_UP:
	case 'k':
		if (s->selected > 0) {
			--s->selected;
			break;
		}
		branch_scroll_up(s, 1);
		break;
	case KEY_DOWN:
	case 'j':
		if (s->selected < s->ndisplayed - 1) {
			++s->selected;
			break;
		}
		if (TAILQ_NEXT(s->last_branch_onscreen, entries) == NULL)
			/* Reached last entry. */
			break;
		branch_scroll_down(view, 1);
		break;
	case CTRL('u'):
		nscroll >>= 1;
		/* FALL THROUGH */
	case KEY_PPAGE:
	case CTRL('b'):
		if (s->first_branch_onscreen == TAILQ_FIRST(&s->branches))
			s->selected -= MIN(nscroll, s->selected);
		branch_scroll_up(s, nscroll);
		break;
	case CTRL('d'):
		nscroll >>= 1;
		/* FALL THROUGH */
	case KEY_NPAGE:
	case CTRL('f'):
		if (TAILQ_NEXT(s->last_branch_onscreen, entries) == NULL) {
			/* No more entries off-page; move cursor down. */
			if (s->selected < s->ndisplayed - 1)
				s->selected += MIN(nscroll,
				    s->ndisplayed - s->selected - 1);
			break;
		}
		branch_scroll_down(view, nscroll);
		break;
	case CTRL('l'):
	case 'R':
		fnc_free_branches(&s->branches);
		s->branch_glob = NULL; /* Shared pointer. */
		s->when = 0;
		s->branch_flags = BRANCH_LS_OPEN_CLOSED;
		rc = fnc_load_branches(s);
		break;
	case KEY_RESIZE:
		if (view->nlines >= 2 && s->selected >= view->nlines - 1)
			s->selected = view->nlines - 2;
		break;
	default:
		break;
	}

	return rc;
}

static int
browse_branch_tree(struct fnc_view **new_view, int y, int x,
    struct fnc_branchlist_entry *be, int color)
{
	struct fsl_cx	*f;
	struct fnc_view	*tree_view;
	fsl_id_t	 rid;
	int		 rc;

	*new_view = NULL;

	if ((f = fcli_cx()) == NULL)
		return RC(FNC_RC_FATAL, "fcli_cx");

	rc = idtorid(&rid, be->branch->id, FNC_RC_NO_BRANCH);
	if (rc != 0)
		return rc;

	tree_view = view_open(0, 0, y, x, FNC_VIEW_TREE);
	if (tree_view == NULL)
		return RC(FNC_RC_CURSES, "view_open");

	rc = open_tree_view(tree_view, NULL, rid, color);
	if (rc)
		return rc;

	*new_view = tree_view;
	return FNC_RC_OK;
}

static void
branch_scroll_up(struct fnc_branch_view_state *s, int maxscroll)
{
	struct fnc_branchlist_entry	*be;
	int				 idx = 0;

	if (s->first_branch_onscreen == TAILQ_FIRST(&s->branches))
		return;

	be = TAILQ_PREV(s->first_branch_onscreen, fnc_branchlist_head, entries);
	while (idx++ < maxscroll) {
		if (be == NULL)
			break;
		s->first_branch_onscreen = be;
		be = TAILQ_PREV(be, fnc_branchlist_head, entries);
	}
}

static int
branch_scroll_down(struct fnc_view *view, int maxscroll)
{
	struct fnc_branch_view_state	*s = &view->state.branch;
	struct fnc_branchlist_entry	*next, *last;
	int				 idx = 0;

	if (s->first_branch_onscreen)
		next = TAILQ_NEXT(s->first_branch_onscreen, entries);
	else
		next = TAILQ_FIRST(&s->branches);

	last = s->last_branch_onscreen;
	while (next && last && idx++ < maxscroll) {
		last = TAILQ_NEXT(last, entries);
		if (last) {
			s->first_branch_onscreen = next;
			next = TAILQ_NEXT(next, entries);
		}
	}

	return FNC_RC_OK;
}

static void
branch_grep_init(struct fnc_view *view)
{
	struct fnc_branch_view_state *s = &view->state.branch;

	s->matched_branch = NULL;
}

static int
branch_search_next(struct fnc_view *view)
{
	struct fnc_branch_view_state	*s = &view->state.branch;
	struct fnc_branchlist_entry	*be;

	if (view->searching == SEARCH_DONE) {
		view->search_status = SEARCH_CONTINUE;
		return FNC_RC_OK;
	}

	if (s->matched_branch) {
		if (view->searching == SEARCH_FORWARD) {
			if (s->selected_entry)
				be = TAILQ_NEXT(s->selected_entry, entries);
			else
				be = TAILQ_PREV(s->selected_entry,
				    fnc_branchlist_head, entries);
		} else {
			if (s->selected_entry == NULL)
				be = TAILQ_LAST(&s->branches,
				    fnc_branchlist_head);
			else
				be = TAILQ_PREV(s->selected_entry,
				    fnc_branchlist_head, entries);
		}
	} else {
		if (view->searching == SEARCH_FORWARD)
			be = TAILQ_FIRST(&s->branches);
		else
			be = TAILQ_LAST(&s->branches, fnc_branchlist_head);
	}

	while (1) {
		if (be == NULL) {
			if (s->matched_branch == NULL) {
				view->search_status = SEARCH_CONTINUE;
				return FNC_RC_OK;
			}
			if (view->searching == SEARCH_FORWARD)
				be = TAILQ_FIRST(&s->branches);
			else
				be = TAILQ_LAST(&s->branches,
				    fnc_branchlist_head);
		}

		if (match_branchlist_entry(be, &view->regex)) {
			view->search_status = SEARCH_CONTINUE;
			s->matched_branch = be;
			break;
		}

		if (view->searching == SEARCH_FORWARD)
			be = TAILQ_NEXT(be, entries);
		else
			be = TAILQ_PREV(be, fnc_branchlist_head, entries);
	}

	if (s->matched_branch) {
		int idx = s->matched_branch->idx;

		if (idx >= s->first_branch_onscreen->idx &&
		    idx <= s->last_branch_onscreen->idx)
			s->selected = idx - s->first_branch_onscreen->idx;
		else {
			s->first_branch_onscreen = s->matched_branch;
			s->selected = 0;
		}
	}

	return FNC_RC_OK;
}

static int
match_branchlist_entry(struct fnc_branchlist_entry *be, regex_t *regex)
{
	regmatch_t regmatch;

	return regexec(regex, be->branch->name, 1, &regmatch, 0) == 0;
}

static int
close_branch_view(struct fnc_view *view)
{
	struct fnc_branch_view_state *s = &view->state.branch;

	fnc_free_branches(&s->branches);
	free_colours(&s->colours);

	return FNC_RC_OK;
}

static void
fnc_free_branches(struct fnc_branchlist_head *branches)
{
	struct fnc_branchlist_entry *be;

	while (!TAILQ_EMPTY(branches)) {
		be = TAILQ_FIRST(branches);
		TAILQ_REMOVE(branches, be, entries);
		fnc_branch_close(be->branch);
		free(be);
	}
}

static void
fnc_branch_close(struct fnc_branch *branch)
{
	free(branch->name);
	free(branch->id);
	free(branch);
}

/*
 * Assign path to **inserted->path, with optional ->data assignment, and insert
 * in lexicographically sorted order into the doubly-linked list rooted at
 * *pathlist. If path is not unique, return without adding a duplicate entry.
 */
static int
fnc_pathlist_insert(struct fnc_pathlist_entry **inserted,
    struct fnc_pathlist_head *pathlist, const char *path, void *data)
{
	struct fnc_pathlist_entry	*new, *pe;
	size_t				 len;
	int				 cmp = 0;

	if (inserted != NULL)
		*inserted = NULL;

	len = strlen(path);

	/*
	 * Most likely, supplied paths will be sorted (e.g., fnc diff *.c), so
	 * post-order traversal will be more efficient when inserting entries.
	 */
	pe = TAILQ_LAST(pathlist, fnc_pathlist_head);
	while (pe != NULL) {
		cmp = fnc_path_cmp(pe->path, path, pe->pathlen, len);
		if (cmp == 0)
			return FNC_RC_OK;
		if (cmp < 0)
			break;
		pe = TAILQ_PREV(pe, fnc_pathlist_head, entry);
	}

	new = malloc(sizeof(*new));
	if (new == NULL)
		return RC_ERRNO("malloc");

	new->path = path;
	new->pathlen = len;
	new->data = data;

	if (cmp < 0)
		TAILQ_INSERT_AFTER(pathlist, pe, new, entry);
	else
		TAILQ_INSERT_HEAD(pathlist, new, entry);

	if (inserted != NULL)
		*inserted = new;
	return FNC_RC_OK;
}

static int
fnc_path_cmp(const char *path1, const char *path2, size_t len1, size_t len2)
{
	size_t	minlen;
	size_t	idx = 0;

	/* Trim any leading path separators. */
	while (path1[0] == '/') {
		++path1;
		--len1;
	}
	while (path2[0] == '/') {
		++path2;
		--len2;
	}
	minlen = MIN(len1, len2);

	/* Skip common prefix. */
	while (idx < minlen && path1[idx] == path2[idx])
		++idx;

	/* Are path lengths exactly equal (exluding path separators)? */
	if (len1 == len2 && idx >= minlen)
		return 0;

	/* Trim any redundant trailing path seperators. */
	while (path1[idx] == '/' && path1[idx + 1] == '/')
		++path1;
	while (path2[idx] == '/' && path2[idx + 1] == '/')
		++path2;

	/* Ignore trailing path separators. */
	if (path1[idx] == '/' && path1[idx + 1] == '\0' && path2[idx] == '\0')
		return 0;
	if (path2[idx] == '/' && path2[idx + 1] == '\0' && path1[idx] == '\0')
		return 0;

	/* Order children in subdirectories directly after their parents. */
	if (path1[idx] == '/' && path2[idx] == '\0')
		return 1;
	if (path2[idx] == '/' && path1[idx] == '\0')
		return -1;
	if (path1[idx] == '/' && path2[idx] != '\0')
		return -1;
	if (path2[idx] == '/' && path1[idx] != '\0')
		return 1;

	/* Character immediately after the common prefix determines order. */
	return (unsigned char)path1[idx] < (unsigned char)path2[idx] ? -1 : 1;
}

static void
fnc_pathlist_free(struct fnc_pathlist_head *pathlist)
{
	struct fnc_pathlist_entry *pe;

	while ((pe = TAILQ_FIRST(pathlist)) != NULL) {
		TAILQ_REMOVE(pathlist, pe, entry);
		free(pe);
	}
}

static int
fnc_show_version(void)
{
	if (printf("%s %s [%.10s] %.19s UTC\n", fnc__progname, PRINT_VERSION,
	    PRINT_HASH, PRINT_DATE) < 0)
		return 1;

	return FNC_RC_OK;
}

static int
xstrtonum(long *ret, const char *nstr, const long min, const long max)
{
	const char	*err = NULL;
	long		 n;

	errno = 0;

	n = strtonum(nstr, min, max, &err);
	if (errno == EINVAL)
		return RC(FNC_RC_BAD_NUMBER, "%s", nstr);
	else if (errno != 0 || errno == ERANGE)
		return RC(FNC_RC_RANGE, "%s", nstr);
	else if (err && *err != '\0')
		return RC(FNC_RC_BAD_NUMBER, "%s", nstr);

	*ret = n;
	return FNC_RC_OK;
}

static int
fnc_prompt_input(struct fnc_view *view, struct input *input)
{
	int rc;

	if (input->prompt)
		sitrep(view, input->flags, "%s", input->prompt);

	rc = cook_input(input->buf, sizeof(input->buf), view->window);
	if (rc || !input->buf[0])
		return rc;

	if (input->type == INPUT_NUMERIC) {
		long min = LONG_MIN, max = LONG_MAX, n = 0;

		if (input->data) {
			min = *(int *)input->data;
			max = ((int *)input->data)[1];
		}

		rc = xstrtonum(&n, input->buf, min, max);
		if (rc)
			return sitrep(view, SR_ALL, ":%s", RCSTR(rc));

		input->ret = n;
	}

	return FNC_RC_OK;
}

static int
cook_input(char *ret, int sz, WINDOW *win)
{
	int rc;

	nocbreak();
	noraw();
	echo();
	rc = wgetnstr(win, ret, sz);
	cbreak();
	noecho();
	raw();

	return rc == ERR ? RC(FNC_RC_CURSES, "wgetnstr") : FNC_RC_OK;
}

static int PRINTFV(3, 4)
sitrep(struct fnc_view *view, int flags, const char *msg, ...)
{
	va_list args;

	va_start(args, msg);
	/* vw_printw(view->window, msg, args); */
	wattr_on(view->window, A_BOLD, NULL);
	wmove(view->window, view->nlines - 1, 0);
	vw_printw(view->window, msg, args);
	if (FLAG_CHK(flags, SR_CLREOL))
		wclrtoeol(view->window);
	wattr_off(view->window, A_BOLD, NULL);
	va_end(args);
	if (FLAG_CHK(flags, SR_UPDATE)) {
		update_panels();
		doupdate();
	}
	if (FLAG_CHK(flags, SR_RESET))
		RC_RESET;
	if (FLAG_CHK(flags, SR_SLEEP))
		sleep(1);

	return FNC_RC_OK;
 }

static int
buf_printf(struct fsl_buffer *b, const char *fmt, ...)
{
	size_t	n = b->used;
	va_list	ap;

	va_start(ap, fmt);
	fsl_buffer_appendfv(b, fmt, ap);
	va_end(ap);
	if (b->errCode != 0)
		return -1;

	return b->used - n;
}

static int
buf_write(struct fsl_buffer *b, const void *data, ssize_t len)
{
	size_t n = b->used;

	fsl_buffer_append(b, data, len);
	if (b->errCode != 0)
		return -1;

	return b->used - n;
}

static int
buf_putc(struct fsl_buffer *b, int c)
{
	unsigned char *bp;

	if (b->capacity == 0 || b->used >= b->capacity - 1) {
		fsl_buffer_reserve(b, b->used + 128);
		if (b->errCode != 0)
			return -1;
	}
	bp = b->mem + b->used;
	*bp = (unsigned char)c;
	*++bp = '\0';
	++b->used;
	return c;
}

/*
 * Parse string d as a date in one of the following formats and convert into
 * an mtime returned in the double out parameter *ret:
 *
 *   1. YYYY-MM-DD
 *   2. DD/MM/YYYY
 *   3. MM/DD/YYYY
 *
 * In the first form, trailing garbage (e.g., 2023-10-10 10:10, 1944-08-08x)
 * is ignored. Forms two and three must be unambiguous (e.g., 10/10/2023 is
 * invalid). If the when flag is >0 or <0, adjust the converted date to one
 * second before or after midnight, respectively, of the provided date d.
 * Return zero on success, nonzero on error.
 */
static int
fnc_date_to_mtime(double *ret, const char *d, int when)
{
	struct tm	tm;
	char		iso8601[ISO8601_TIMESTAMP + 1];  /* NUL */

	memset(&tm, 0, sizeof(tm));

	if (strptime(d, "%Y-%m-%d", &tm) == NULL) {
		/* if not YYYY-MM-DD, try MM/DD/YYYY and DD/MM/YYYY */
		if (strptime(d, "%D", &tm) != NULL) {
			/* if MM/DD/YYYY, could it be DD/MM/YYYY too? */
			if (strptime(d, "%d/%m/%Y", &tm) != NULL)
				return RC(FNC_RC_AMBIGUOUS_DATE, "%s", d);
		} else if (strptime(d, "%d/%m/%Y", &tm) != NULL) {
			/* if DD/MM/YYYY, could it be MM/DD/YYYY too? */
			if (strptime(d, "%D", &tm) != NULL)
				return RC(FNC_RC_AMBIGUOUS_DATE, "%s", d);
		} else
			return RC(FNC_RC_BAD_DATE, "%s", d);
	}

	/* convert to mtime */
	if (when > 0)	/* after date d */
		strftime(iso8601, sizeof(iso8601), "%FT23:59:59", &tm);
	else		/* before date d */
		strftime(iso8601, sizeof(iso8601), "%FT00:00:01", &tm);
	if (!fsl_iso8601_to_julian(iso8601, ret))
		return RC(FNC_RC_BAD_DATE, "fsl_iso8601_to_julian: %s",
		    iso8601);

	return FNC_RC_OK;
}

static int
fnc_strftime(char *dst, size_t dstlen, const char *fmt, time_t mtime)
{
	struct tm tm;

	if (fnc__utc) {
		if (gmtime_r(&mtime, &tm) == NULL)
			return RC_ERRNO("gmtime_r");
	} else {
		if (localtime_r(&mtime, &tm) == NULL)
			return RC_ERRNO("localtime_r");
	}
	if (strftime(dst, dstlen, fmt, &tm) == 0)
		return RC(FNC_RC_NO_SPACE);

	return FNC_RC_OK;
}

/*
 * Like strsep(3) but optionally assign the length
 * (excluding NUL) of the returned token in *toklen.
 */
static char *
fnc_strsep(char **ptr, const char *sep, size_t *toklen)
{
	char		*s, *tok;
	const char	*nulset;
	int		 c, sc;

	if (toklen != NULL)
		*toklen = 0;

	if (*ptr == NULL)
		return NULL;

	s = tok = *ptr;
	for (;;) {
		c = *s;
		nulset = sep;
		for (;;) {
			sc = *nulset;
			if (sc == c) {
				if (toklen != NULL)
					*toklen = s - *ptr;
				if (c == '\0')
					s = NULL;
				else
					*s++ = '\0';
				*ptr = s;
				return tok;
			} else if (sc == '\0')
				break;
			++nulset;
		}
		++s;
	}
	/* NOTREACHED */
}

static bool
fnc_str_has_upper(const char *str)
{
	int	idx;

	for (idx = 0; str[idx]; ++idx)
		if (fsl_isupper(str[idx]))
			return true;

	return false;
}

/*
 * If str is lowercase, construct a pairing for SQL queries with the
 * SQLite LIKE operator to fold case for a case-insensitive query:
 *   *op = "LIKE"
 *   *glob = "%%%%str%%%%"
 * Otherwise, construct a case-sensitive pairing:
 *   *op = "GLOB"
 *   *glob = "*str*"
 * Both *op and *glob must be disposed of by the caller.
 * Return non-zero on allocation failure, else return zero.
 */
static int
fnc_make_sql_glob(char **op, char **glob, const char *str)
{
	*op = NULL;
	*glob = NULL;

	if (!fnc_str_has_upper(str)) {
		*op = strdup("LIKE");
		if (*op == NULL)
			return RC_ERRNO("strdup");

		*glob = fsl_mprintf("%%%%%s%%%%", str);
		if (*glob == NULL) {
			free(*op);
			*op = NULL;
			return RC_ERRNO("fsl_mprintf");
		}
	} else {
		*op = strdup("GLOB");
		if (*op == NULL)
			return RC_ERRNO("strdup");

		*glob = fsl_mprintf("*%s*", str);
		if (*glob == NULL) {
			free(*op);
			*op = NULL;
			return RC_ERRNO("fsl_mprintf");
		}
	}

	return FNC_RC_OK;
}

static const char *
getdirname(const char *path, int len, bool slash)
{
	const char	*p;
	static char	 ret[PATH_MAX];
	size_t		 n;

	if (path == NULL || *path == '\0')
		goto dot;

	if (len > 0)
		n = len;
	else
		n = strlen(path);

	/* strip any trailing slashes */
	p = path + n - 1;
	while (p > path && *p == '/')
		--p;

	while (p > path) {
		if (*p == '/') {
			if (!slash)
				--p;
			break;
		}
		--p;
	}

	if (p == path && *path != '/')
		goto dot;

	n = p - path + 1;
	if (n >= sizeof(ret)) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	memcpy(ret, path, n);
	ret[n] = '\0';
	return ret;

dot:
	ret[0] = '.';
	ret[1] = '\0';
	return ret;
}

#ifndef HAVE_LANDLOCK
static int
init_unveil(const char **paths, const char **perms, int n, bool disable)
{
#ifdef __OpenBSD__
	int i;

	for (i = 0; i < n; ++i) {
		if (paths[i] && unveil(paths[i], perms[i]) == -1)
			return RC_ERRNO("unveil(%s, \"%s\")",
			    paths[i], perms[i]);
	}

	if (disable && unveil(NULL, NULL) == -1)
		return RC_ERRNO("unveil");

#endif  /* __OpenBSD__ */
	return FNC_RC_OK;
}
#endif  /* HAVE_LANDLOCK */

static const char *
tzfile(void)
{
	static char	 ret[PATH_MAX];
	const char	*tzdir, *tz;
	size_t		 n;

	if ((tz = getenv("TZ"))) {
		if ((tzdir = getenv("TZDIR"))) {
			n = strlcpy(ret, tzdir, sizeof(ret));
			if (n >= sizeof(ret))
				return NULL; /* bogus TZDIR exceeds PATH_MAX */
			if (ret[n - 1] != '/') {
				if (strlcpy(ret + n, "/", sizeof(ret) - n) >=
				    sizeof(ret) - n)
					return NULL;
				++n;
			}
		} else
			n = strlcpy(ret, "/usr/share/zoneinfo/", sizeof(ret));
		if (strlcpy(ret + n, tz, sizeof(ret) - n) >=
		    sizeof(ret) - n)
			return NULL;  /* bogus (TZDIR)TZ exceeds PATH_MAX */
	} else
		strlcpy(ret, "/etc/localtime", sizeof(ret));

	if (fsl_file_size(ret) == -1)
		return NULL;	/* file doesn't exist */

	return ret;
}

/*
 * Sans libc wrappers, use the following shims provided by Landlock authors.
 * https://www.kernel.org/doc/html/latest/userspace-api/landlock.html
 */
#ifdef HAVE_LANDLOCK
#ifndef landlock_create_ruleset
static inline int
landlock_create_ruleset(const struct landlock_ruleset_attr *const attr,
    const size_t size, const __u32 flags)
{
	return syscall(__NR_landlock_create_ruleset, attr, size, flags);
}
#endif

#ifndef landlock_add_rule
static inline int
landlock_add_rule(const int rfd, const enum landlock_rule_type type,
    const void *const attr, const __u32 flags)
{
	return syscall(__NR_landlock_add_rule, rfd, type, attr, flags);
}
#endif

#ifndef landlock_restrict_self
static inline int
landlock_restrict_self(const int rfd, const __u32 flags)
{
	return syscall(__NR_landlock_restrict_self, rfd, flags);
}
#endif

static int
init_landlock(const char **paths, const char **perms, const int n)
{
#define LANDLOCK_ACCESS_DIR	(LANDLOCK_ACCESS_FS_READ_FILE |		\
				LANDLOCK_ACCESS_FS_WRITE_FILE |		\
				LANDLOCK_ACCESS_FS_REMOVE_FILE |	\
				LANDLOCK_ACCESS_FS_READ_DIR |		\
				LANDLOCK_ACCESS_FS_MAKE_REG)
	/*
	 * Define default block list of _all_ possible operations.
	 * XXX Due to landlock's fail-open design, set all the bits to avoid
	 * following Landlock for new ops to add to this deny-by-default list.
	 */
	(void)perms;
	struct landlock_ruleset_attr attr = {
		.handled_access_fs = ((LANDLOCK_ACCESS_FS_MAKE_SYM << 1) - 1)
	};
	struct landlock_path_beneath_attr	path_beneath;
	int					i, rfd, rc = FNC_RC_OK;

	rfd = landlock_create_ruleset(&attr, sizeof(attr), 0);
	if (rfd == -1) {
		/* Landlock is not supported or disabled by the kernel. */
		if (errno == ENOSYS || errno == EOPNOTSUPP)
			return FNC_RC_OK;
		return RC_ERRNO("landlock: failed to create ruleset");
	}

	/* Iterate paths to grant fs permissions. */
	for (i = 0; i < n; ++i) {
		struct stat sb;

		if (paths[i] == NULL)
			continue;

		path_beneath.parent_fd = open(paths[i], O_RDONLY | O_CLOEXEC);
		if (path_beneath.parent_fd == -1) {
			rc = RC_ERRNO("open: %s", paths[i]);
			goto end;
		}

		if (fstat(path_beneath.parent_fd, &sb) == -1) {
			rc = RC_ERRNO("fstat: %s", paths[i]);
			goto end;
		}

		if (S_ISDIR(sb.st_mode))
			path_beneath.allowed_access = LANDLOCK_ACCESS_DIR;
		else
			path_beneath.allowed_access =
			    LANDLOCK_ACCESS_FS_READ_FILE;

		if (landlock_add_rule(rfd, LANDLOCK_RULE_PATH_BENEATH,
		    &path_beneath, 0)) {
			rc = RC_ERRNO("landlock_add_rule: %s", paths[i]);
			goto end;
		}
		close(path_beneath.parent_fd);
		path_beneath.parent_fd = -1;
	}

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == -1) {
		rc = RC_ERRNO("prctl");
		goto end;
	}

	if (landlock_restrict_self(rfd, 0))
		rc = RC_ERRNO("landlock_restrict_self");

end:
	if (path_beneath.parent_fd != -1 &&
	    close(path_beneath.parent_fd) == -1 && rc == FNC_RC_OK)
		rc = RC_ERRNO("close");
	if (close(rfd) == -1 && rc == FNC_RC_OK)
		rc = RC_ERRNO("close");
	return rc;
}
#endif  /* HAVE_LANDLOCK */
