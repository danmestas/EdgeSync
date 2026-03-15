# The f-apps

The source tree contains a number of test/demo apps which are named
`f-something` (the "f" is for "fossil", of
course). Collectively, these apps demonstrate the functionality which
is currently ported in from fossil(1) (or is new) and working. If a
fossil(1) feature is not represented here, it's probably not yet
ported. Note that libfossil is still very much in development, so
any or all of these may be... *fussy*... at times. Also note that these
are primarily for testing and demonstrating the library, and may not
be suitable as apps for every-day use. *That said*, the active libfossil
developers tend to use these applications as "daily drivers" for their
fossil work, where appropriate. e.g. `f-ci` is used for performing most
checkins into this repository.

In alphabetical order:

   *  [`f-_template.c`](/finfo?name=f-apps/f-_template.c) is a documented
      template from which new fcli-using apps can be quickly created.
      It performs all of the bootstrapping necessary to get an app up and
      running with very little effort.
   *  [`f-acat`](/finfo?name=f-apps/f-acat.c) ("artifact cat") can
      output arbitrary artifacts from a repository.
   *  [`f-add`](/finfo?name=f-apps/f-add.c) queues file for addition
      in the next commit.
   *  [`f-adiff`](/finfo?name=f-apps/f-adiff.c) ("artifact diff") can
      output diffs of any two arbitrary blobs from a repository. It
      does not do checkin-level diffs. Supports various diff
      generation options.
   *  [`f-annotate`](/finfo?name=f-apps/f-annotate.c) provides features
      comparable to fossil's `annotate` and `praise` commands.
   *  [`f-aparse`](/finfo?name=f-apps/f-aparse.c) (formerly
      `f-mfparse`) reads in a Fossil structural artifact, converts it
      to the library's internal high-level form, and optionally saves
      it back out to a file. This is used for testing the manifest
      parsing and generation code and to test fidelity and
      compatibility with fossil manifests.
   *  [`f-ci`](/finfo?name=f-apps/f-ci.c) checks in file changes to a
      repository. It works but the lack of network supports
      means fossil(1) is still needed for synching. Most of the checkins
      made on the libfossil repository are made using this app. (The rest
      are mostly cases where ancient muscle memory has instead invoked
      fossil.)
   *  [`f-ciwoco`](/finfo?name=f-apps/f-ciwoco.c) checks in files to a
      repository without requiring a checkout. ciwoco is short for
      CheckIn WithOut CheckOut.
   *  [`f-co`](/finfo?name=f-apps/f-co.c) checks out a version, analog
      to `fossil co`.
   *  [`f-config`](/finfo?name=f-apps/f-config.c) lists and modifies
      the contents of the various configuration tables (global, repo,
      and local checkout).
   *  [`f-extract`](/finfo?name=f-apps/f-extract.c) extracts individual
      files from a repository.
   *  [`f-ls`](/finfo?name=f-apps/f-ls.c) lists files from a
      repository.
   *  [`f-merge`](/finfo?name=f-apps/f-merge.c) merges a version
      of a repository into the current checkout.
   *  [`f-mv`](/finfo?name=f-apps/f-mv.c) renames ("moves") managed
      files.
   *  [`f-new`](/finfo?name=f-apps/f-new.c) creates new/empty
      repository databases.
   *  [`f-open`](/finfo/f-apps/f-rm.c) is analogous to `fossil open`
      except that it only deals with local files, not remote URLs.
   *  [`f-parseparty`](/finfo?name=f-apps/f-parseparty.c) throws a
      "parsing party," parsing (and optionally crosslinking) all
      fossil artifacts in a repository (for library testing purposes).
   *  [`f-query`](/finfo?name=f-apps/f-query.c) runs SQL commands
      against a repo/checkout db. SQL run this way has access to the
      [libfossil-specific SQL functions](/doc/ckout/doc/db-udf.md). Be careful!
   *  [`f-rebuild`](/finfo?name=f-apps/f-rebuild.c) deletes a repository's
      transient state and recreates it from the immutable artifact state.
   *  [`f-rename`](/finfo?name=f-apps/f-rename.c) renames SCM-managed
      files in a checkout, analog to fossil's `mv/rename` command.
   *  [`f-repostat`](/finfo?name=f-apps/f-repostat.c) provides some statistics
      about a repository database, analog to fossil's `dbstat` command.
   *  [`f-resolve`](/finfo?name=f-apps/f-resolve.c) resolves symbolic
      checkin names (like "trunk", "current", and "prev") and partial
      UUIDs to their full UUIDs and RIDs.
   *  [`f-revert`](/finfo?name=f-apps/f-revert.c) cancels pending changes
      by un-queuing uncommited ADDs, REMOVEs, and RENAMEs, and
      restores the contents of modified files to their checked-out
      versions.
   *  [`f-rm`](/finfo?name=f-apps/f-rm.c) queues files for removal at
      the next commit or cancels uncommitted "add" commands.
   *  [`f-sanity`](/finfo?name=f-apps/f-sanity.c) runs a number of
      sanity checks on the library. Library developers should
      run/amend this occasionally.
   *  [`f-status`](/finfo?name=f-apps/f-status.c) behaves essentially like
      fossil's `status` command.
   *  [`f-tag`](/finfo?name=f-apps/f-tag.c) adds tags to artifacts.
   *  [`f-timeline`](/finfo?name=f-apps/f-timeline.c) provides a
      timeline of recent repository activity.
   *  [`f-update`](/finfo?name=f-apps/f-update.c) updates a checkout
      to a different version, merging any local edits, analog to
      `fossil update`.
   *  [`f-vdiff`](/finfo?name=f-apps/f-vdiff.c) ("version diff")
      generates diffs of arbitrary repository versions.
   *  [`f-wiki`](/finfo?name=f-apps/f-wiki.c) is a generic wiki
      manipulation tool, capable of listing, exporting, and importing
      wiki pages.
   *  [`f-zip`](/finfo?name=f-apps/f-zip.c) creates ZIP files from a
      given version of repository content.

All of these applications accept `-?`, `--help`, or a first non-flag
argument of `help` to show their help text.

There are also [several apps named `f-test-SOMETHING`](/dir/f-apps)
which exist for testing specific APIs and may also be interesting for
would-be users of the library.

These applications provide demonstrations of using the library and
give devs a place to test new features. [`fcli`](FossilApp) provides a
mini-framework which takes care of bootstrapping fossil for these
applications, making it pretty simple to create new ones. fcli handles
the CLI parsing, global flags, opening of a repo/checkout, and other
bootstrapping bits. If you've ever hacked on fossil(1), adding a
new f-* app is very similar to adding a new command to fossil, with
a few more lines of bootstrap code (because each "command" is in
its own app).

## Demos

```
# f-wiki ls
Timestamp           UUID     Name
2013-08-22@22:26:59 86afdaa5 AmalgamationBuild
2013-08-23@18:37:26 e451926f building
2013-08-22@14:54:47 042b30df download
2013-08-24@11:29:34 0f083370 f-tools
...

# f-tag -a tip -t demo-tag="demo for prosperity purposes" --trace-sql 
SQL TRACE #1: SELECT '/home/stephan/cvs/fossil/f2' || '/' || value FROM vvar WHERE name='repository';
SQL TRACE #2: ATTACH DATABASE '/home/stephan/cvs/fossil/f2.fsl' AS repo;
SQL TRACE #3: SELECT julianday('now');
SQL TRACE #4: SELECT objid FROM event WHERE type='ci' ORDER BY event.mtime DESC LIMIT 1;
SQL TRACE #5: SELECT uuid FROM blob WHERE rid=1226;
SQL TRACE #6: SELECT strftime('%Y-%m-%dT%H:%M:%f',2456517.117559);
SQL TRACE #7: BEGIN TRANSACTION;
...
SQL TRACE #21: COMMIT;
SQL TRACE #22: DETACH DATABASE repo;


# f-timeline -n 1
g  [daa063582c2c] @ 2013-08-12 16:49:17 by [stephan]
	Edit [2a84ad39]: Add "demo-tag" with value "demo for prosperity purposes".


# f-timeline -n 1 --trace-sql
SQL TRACE #1: SELECT '/home/stephan/cvs/fossil/f2' || '/' || value FROM vvar WHERE name='repository';
SQL TRACE #2: ATTACH DATABASE '/home/stephan/cvs/fossil/f2.fsl' AS repo;
SQL TRACE #3: SELECT substr(uuid,1,12) AS uuid, &lt;...BIG SNIP...&gt; ORDER BY event.mtime DESC LIMIT 1;
g  [daa063582c2c] @ 2013-08-12 16:49:17 by [stephan]
	Edit [2a84ad39]: Add "demo-tag" with value "demo for prosperity purposes".
SQL TRACE #4: DETACH DATABASE repo;


# f-resolve current prev 
b170fb24fe5d94a5b7fa7a00a9911083bf69b27b    1466 current
1ec904fa6a54b45b0a35cd14bab41af138584e45    1461 prev
```

## f-new

`f-new` can be used to create a new, empty repository, analog to fossil's "new" command. It has one feature fossil(1) does not: it allows the client to specify the commit message for the "initial empty checkin." (Sidebar: the library has a mechanism to disable creation of the initial empty checkin, but fossil/libfossil are largely untested with such repos.)

Example usage:

```
# f-new -m 'hi there' --config ../fossil.fsl my.fsl --user fred
Copying configuration from: ../fossil.fsl
Created repository: my.fsl
server-code    = a3d6ac6448017382fa1277bbee7e74b96ce385cb
project-code   = e0e38c4a0fb01a2ea277ed8af4e5742d68dbdc60
admin-user     = fred (password=084fdd)

# f-timeline -R my.fsl 
ci [3a661da86ae5] @ 2013-08-28 23:30:39 by [fred] in branch [trunk]
	hi there

# f-acat trunk -R my.fsl 
C hi\sthere
D 2013-08-28T21:30:39.000
R d41d8cd98f00b204e9800998ecf8427e
T *branch * trunk
T *sym-trunk *
U fred
Z 52ef222ec505c27615d80a8552690272

```

## f-tag

f-tag can be used to add and cancel tags on arbitrary repository artifacts. Tags in fossil are actually key/value pairs, but the value is optional. The ability to tag arbitrary artifacts, including other tags, allows (in principal) for applying arbitrary key/value-pair metadata to arbitrary artifacts. This can be used for a great many things above and beyond what fossil currently uses tags for. For example, tags could be used to implement a simple form of comment thread system, by implementing comments as tags, and replies as tags to those comment tags, ad nauseum.

Anyway... this tool is a very basic test/demonstration of this library which allows one to apply tags to artifacts. It allows some things the current fossil UI/CLI cannot make much sense of or do anything useful with (e.g. tags on tags or "cancel" tags with values), but nothing it does is incompatible with how fossil works internally.


> <strong><em>FOREWARNINGS</em></strong>: changing branch tags or other "fossil-special" tags (e.g. Wiki pages or tickets) with this tool is as yet untested, is not yet expected to work (it's an ongoing process), <em>and might break things</em> (relationships between data records). Try such things at your own risk. Better yet, don't try them at all until this warning message is gone.


'Private' content which gets tagged will result in the tag being private as well (to avoid that it synchronized without its tagged content).

Examples of setting and cancelling flags:

```
# Change the color of a single commit:
# f-tag -a current -t bgcolor='#ffff33'

# Propagate the bg color starting at a commit:
# f-tag -a current -t '*bgcolor'='#ffff33'

# Cancel that color:
# f-tag -a current -t=-bgcolor='that was uglier than i thought it would be'

# Change the checkin comment on the current checkout:
# f-tag -a current -t comment='...new comment...'
```

Values are always optional but Fossil internally treats some tag names specially and may require (or expect) a value. The `bgcolor` tag is one example. When cancelling a tag, the value is always optional, regardless of whether or not it is a special tag.

When adding a tag, the tag name may optionally be prefixed with a `+` sign, for symmetry with the cancel (`-`) and propagate (`*`) markers. Cancel tags might <em>look</em> like options/flags because they start with a minus, but they are not interpreted as a flag due to a happy accident of design. When using propagating tags, it is wise to enclose the tag in quotes to prevent any unwanted side-effects of shell globbing. Optionally, use `-t=*tagname`, which would only match a glob in the most unusual of circumstances. Likewise, `-t=-tag-to-cancel` can be used if having the tag value look like a flag seems disturbing to you.

Note that the +/-/* prefixes were not chosen arbitrarily: they reflect how Fossil internally stores and recognizes the type of a tag: `+` represents an "add tag", `-` a "cancel tag" (a.k.a. "anti-tag"), and `*` a propagating tag.


## f-acat

The "artifact cat" tool outputs content from the content. If given a checkin name or the ID of some other fossil-internal structure, it outputs that structure (as opposed to doing complex rendering of the structure-specific type). By default it applies any necessary deltas to produce the desired version of a blob, but it can also be told (using the `--raw` flag) that it should do no undeltification. It always uncompresses the data (assuming it was compressed, which it normally, but not always, is).

```
# The current checkout version's manifest:
# f-acat current
B 200d1cd898f4e05591e78f82b2c0f2bc4c8db998
C Added\s--raw,\s--output,\sand\s--artifact\sflags\sto\sf-acat.
D 2013-09-08T12:25:31.660
F Makefile.in 41c00b5876e09e6bef5eaf4a68e885926eeca159
...

# The previous version's manifest, in its raw form (in this case
# a fossil-format delta):
# f-acat prev --raw 
II
i@0,3P:Moved\sfsl_memory_allocator<SNIP>.
D 2013-09-08T12:01:11.6142g@24,9d@5Z,1u:04923c4435ee082205979a2bc0a9b518e1ff3d80
R a5f73027c19c1b33364845d4808bca6d
U stephan
Z 6721ef25586956f13ef59a4dcb77e339
```

## f-query

`f-query` can be used to run simple SQL commands against a repo/db, kind of like a castrated form of fossil(1)'s sqlite command. The one advantage to using this vs. using the sqlite shell (or fossil(1)) is that this one provides a few fossil-specific SQL functions which those do not. For example, `FSL_DIRPART()` can be used to figure out the list of directories in a repository:

Example usage:

```
# f-query -h -e "select distinct(fsl_dirpart(name,1)) dname
    from filename where dname IS NOT NULL order by dname"
.fossil-settings/
.settings/
autosetup/
autosetup/lib/
doc/
...

### Simulate f-acat:
# f-query -h -e "select fsl_content('rid:1')"
C initial\sempty\scheck-in
D 2013-07-23T13:56:36.037
P
R d41d8cd98f00b204e9800998ecf8427e
T *branch * trunk
...
```

An `-e` value may either a single SQL statement or the name of a file
containing a single SQL statement. Such statements, if they return
rows, will have their results dumped to stdout. Contrariwise, the `-E`
flag may be a file or multiple semicolon-separated statements, all of
which are executed but none of which will present any results to the
user. Both `-e` and `-E` may be specified multiple times.


## f-wiki

`f-wiki` can list wiki pages and export/import wiki page content.

Example usage:

```
# f-wiki ls
Time (local time)    UUID          Size   Name
2013-08-23 00:26:59  86afdaa51885  2470   AmalgamationBuild
2013-08-23 20:37:26  e451926f4c03  1626   building
2013-09-24 19:18:21  a21e5daee961  1827   DbFunctions
...

# f-wiki ls --names
AmalgamationBuild
building
DbFunctions
...

# f-wiki export AmalgamationBuild | head
&lt;h1>The Amalgamation Build&lt;/h1>

See also: [building]
...
```
