Libfossil Tcl API
========================================

**Achtung**: These are docs for an in-development tool and are not
definitive. Like the coresponding code, they are a work in progress.

This extension provides a command named `fossil` which serves
two roles:

- Provide some basic global (non-fossil-repo-specific) services.

- To act as a default fossil context instance, as a wrapper to
  libfossil's `fcli`. This can simplify usage when an app needs only a
  single context active.

Notable limitations:

- Purely single-threaded (both this extension and libfossil). It is
  (mostly) legal to use the library from multiple threads, provided no
  threads use the same fossil context objects at the same time. When
  using multiple threads, never use the default context provided by
  the `fossil` command - instead create new instances (see [the open
  subcommand](#fossil.open)).

The most extensive example code is [this project's unit
tests](/file/bindings/tcl/teaish.test.tcl.in), and these docs are
light on examples because all (or most) of these features are
demonstrated there.


<a id="flags"></a>
## About Command `-flag` Parsing and `--naming` `---conventions`

The naming convention for command `-flags` is that their number of
leading dashes is equal to the number of arguments they consume,
minus 1. Thus `-flag` is a boolean flag, `--flag` consumes its following
argument, and `---flag` consumes the next two arguments.

Unconventional, sure, but flexible and easy to remember.

Most commands support a `-?` flag which causes them to return (without
an error) a brief usage string based on their subcommands and/or
flags.  That flag is not documented in the API docs below because it's
_nearly_ ubiquitous and it just adds noise.

Unless documented otherwise, e.g. in [db query](#db.query), commands
only look for `-flags` until they see the first "non-flag argument,"
which means any argument which is not one of that command's known
flags.  The order of flags is only relevant for those few which may be
used multiple times, e.g. [`db query -bind`](#db.query--bind). Some
few commands, for reasons of ergonomics, accept flags and arguments in
any order.


`fossil` Global Subcommands
========================================

This command is the entrypoint to all of the library's
functionality.

Passing the `--new-instance varName|-` flag, as described for [the
`open` subcommand](#fossil.open), behaves as if `open` had been called.

Aside from that, it generally behaves like any other fossil instance...


<a id="fossil"></a>
Fossil Instance Subcommands
========================================

The `fossil` command can act as a fossil instance, and has access
to all of an instance's subcommands.  New instances can be created most
easily in the following ways:

>
```
fossil --new-instance f
# or:
set f [fossil --new-instance -]
# or:
set f [fossil -new-instance]
```

After that, `f` is used like a command but needs to be dereferenced
when used:

>
```
$f open .
...
$f close
```

Note the leading `$`. The value held by `f` is the name of an
instance-specific command object.  That object will remain active, and
eating up system resources, until either its [`close`
subcommand](#fossil.close) is called or it is "deleted" in the manner
appropriate to a proc: `rename $f ""`.  Simply using `unset f` will
not have that effect, and will effectively leak the proc unless the
script still has its name (`$f`) stashed away somewhere.

The available subcommands are listed alphabetically in the following
subsections...

<a id="fossil.checkout"></a>
## `checkout`

This command has subcommands specifically for working with an opened
checkout. Calling it without an opened checkout will trigger an error.

The available subcommand is currently only:

- **`scan`** re-runs the scan for changes in the current checkout
  relative to the checked-out version. This internally updates the
  `vfile` table of the checkout database.

With a handful more TODO.


<a id="fossil.close"></a>
## `close`

Usage: `close ?-keep?`

Closes any SCM-related databases opened by this instance.

By default, this function both unsets the var name (if any) specified
via `open --new-instance varName` and deletes the proc associated with
this instance, freeing all resources. If `-keep` is used, the var (if
any) is retained and the proc stays registered, so that the instance
can again be used with [the `open` subcommand](#fossil.open).

> Achtung: if an instance created using `open --new-instance varName`
  is closed from a scope other than the one in which it was created,
  _not entirely the right thing_ happens: the var will be unset in
  _that_ scope, and the one in the original scope (if it's still
  acitve) then refers to a deleted proc so cannot be used without
  triggering an error. The workaround for that "wrong unset" is to use
  `--new-instance -` instead, which puts the lifetime of the var name
  in the caller's control instead of fossl's.


<a id="fossil.db"></a><a id="db"></a>
## `db`

Each fossil instance wraps a database handle associated with its
repository and checkout (which are separate database files). This
function is a proxy for [the db instance subcommands](#db), acting
upon this object's db handle.

<a id="fossil.file"></a>
## `file`

This section is a placeholder for an as-yet hypothetical series of
file-related subcommands.


<a id="fossil.ls"></a>
## `ls`

Usage: `ls ?flags?`

Flags:

- **`--glob pattern`** filters the resulting list of files on the
  given glob pattern(s). It supports the libfossil glob APIs, so each
  pattern may be a list of space-, newline-, or comma-separated
  globs. May be specified multiple times to add to the list of globs.\  
  Recall that fossil's globbing rules treat its inputs as opaque
  strings, i.e. they place no special segnificant on the `/`
  character, so a `*` will match one.
- and **`--match pattern`** works like `--glob` except that it uses
  Tcl's `string match` patterns instead of libfossil globs. Both flags
  may be used together.
- **`--version artifact-name`** specifies the checkin version to
  use. By the default is `trunk` unless a checkout is opened, in which
  case the default is `:checkout:`, which (intuitively enough) will list
  files from the current checkout.\  
  **FIXME:** `:checkout:` is not yet implemented. It requires a whole other
  way of doing things internally than the from-repo listing does.
- **`-ckout`** is equivalent `--version :checkout:`
- **`-not-found-ok`** means that if the given version is not found,
  return an empty value instead of failing.
- **`-rescan-ckout`** tells libfossil to re-scan the checkout for any
  local changes. This is needed if changes are made to files since the
  last time it was scanned. It's only used when listing the current
  checkout.
- **`-verbose`** changes the result format from a list of names to a
  list of dict-like values, each containing the keys (`name`, `uuid`,
  `rename`, `perm`), plus possibly other fields toggled by
  flags listed below.
- **`-size`** adds the `size` field to the file information. This
  requires an extra query per file, so is off by default. This flag
  implies `-verbose`.
- **`-time`** and **`-localtime`** adds the `time` field to the file.
  The former uses UTC and the latter translates to local time. This is
  somewhat expensive to compute so is off by default. This flag
  implies `-verbose`.\  
  Sidebar: file timestamps in fossil mean the time of the checkin
  which most recently modified the file, starting at the specified
  `-version` and looking backwards in time.
- **`-renames`** adds a `rename` field to the results which holds the
  name of the file in the previous checkin, or an empty string if the
  file was not renamed. This flag implies `-verbose`.
- **`---eval varName script`** evals the given script in the caller's
  scope once per matched filename. `varName` is a call-local array
  with the same fields as described for `-verbose`, plus a `fossil`
  entry which holds the name of the fossil command object which
  triggered this, with which the callback can make further API calls
  to get more info about the files.\  
  TODO: support a `varName` of `-$` to install vars named after the
  columns.

<!--
- TODO?: **`--apply lambda`** calls the given lambda once for each file
  in the list. `lambda` must be a lambda expression with a parameter
  signature compatible with `{?what?}`. Maybe pass it a name of a
  call-specific array?
-->

By default it returns a list of results, but if `---eval` is used then
it returns an empty string, under the assumption that the eval script
will collect or process whatever it needs.

All filenames in the listing are relative to the top of the
repository.

<a id="fossil.open"></a>
## `open`

Usage: `open ?-noparents? ?--new-instance varName|-? ?-new-instance? ?repoFileOrCkoutDir?`

This is unrelated to [db open](#db.open).

A non-flag argument is assumed to be either a repository db (if it's a
file) or a checkout (if it's a directory). Opening a checkout will
recursively search parent directories for a fossil checkout unless the
`-noparents` flag is used.

If called with no arguments this returns a bitmask which indicates
which database(s) is/are opened:

- 0 = none
- 0x01 = a repository
- 0x02 = a checkout
- 0x04 = global config

`--new-instance varName|-` causes it to create a new fossil context
object and assign it to a var named `varName` or, if `-` is the name,
to set it only as the result value.  If this flag is not used, the
current fossil instance is opened (if a repo/db name was
provided). See [the close subcommand](#fossil.close) for why the
`--new-instance -` form may generally be preferrable to
`--new-instance varName`.

`-new-instance` is equivalent to `--new-instance -`.


(Sidebar: new instances inherit certain settings from the instance
which opened them. Figuring out, and documenting, which settings those
are is TODO.)

This function throws if the object is already open. Use
[`close`](#fossil.close) to close it first.


<a id="fossil.option"></a>
## `option`

TBD

Where to draw the API line (or whether to draw a line) between
context-instance-specific settings, like verbosity level, and the
fossil config db is TDB.


<a id="fossil.info"></a>
## `info`

Usage: `info -flag`?

This command fetches information about the repository. It expects one flag:

- **`-repo-db`** returns the name of the repository db, if it's
  opened, else it returns an empty string.
- **`-checkout-db`** returns the name of the checkout db, if it's
  opened, else it returns an empty string.
- **`-checkout-dir`** returns the name of the checkout directory, if it's
  opened, else it returns an empty string.
- **`-lib-version`** returns a string containing version info of this
  extension and libfossil.


<a href="fossil.resolve"></a>
## `resolve`

Usage: `resolve ?-rid? ?-noerr? symbolic-name`

This subcommand resolves symbolic names to their hashes or, with the
`-rid` flag, their so-called RIDs (the `blob.rid` field in the
database).

By default it will error out if it gets no result or an ambiguous one,
but the `-noerr` flag causes it to return an empty string unless
`-rid` is in effect, in which case it returns `0`.


<a id="db"></a><a id="fossil.db"></a>
Db Instance Subcommands
========================================

Usage: `db subcommand args...`

In this API, database connections are normally owned by a fossil
context instance (which actually encapsulates multiple databases - a
repository and (optionaly) a checkout), but it's also possible to open
other databases, unlreated to fossil. The `fossil db` subcommand can
be used to work with fossil-bound databases as well as with arbitrary
non-fossil SQLite3 databases.

This command offers the following features:

- Access to a fossil instance's databases, including [fossil-specific
  database functions](/doc/$CURRENT/doc/db-udf.md).

- Creating standalone db instances with the same API but minus any
  association with a fossil instance. These can be used with arbitrary
  SQLite3 databases.

The available subcommands, in alphabetical order...

<a id="db.batch"></a>
## db batch

Usage: `batch ?-file? ?-no-transaction? ?-dry-run? sql`

Runs any number of SQL statements but does not provide any way to bind
SQL parameters or fetch the results. The `-file` flag causes it to
read the SQL from the given file.

By default it will use a transaction, but the `-no-transaction`
flag disables that unless it's trumped by...

The `-dry-run` flag always uses a transaction and always rolls it
back, even if it succeeds.


<a id="db.changes"></a>
## db changes

Usage: `changes ?-total?`

This behaves like [`sqlite3_changes()`][sq3_changes] or, if the
`-total` flag is used, [`sqlite3_total_changes()`][sq3_total_changes].


[sq3_changes]: https://sqlite.org/c3ref/changes.html
[sq3_total_changes]: https://sqlite.org/c3ref/total_changes.html

<a id="db.close"></a>
## db close

Usage: `close`

This is distinct from [`fossil close`](#fossil.close). Databases tied
to fossil instances do not have this subcommand, only databases opened
with [db open](#db.open) do.

This closes the database and frees the command object associated with
it. If this instance was created using `open --set-var X` then var `X`
is also unset, but see [fossil.close](#fossil.close) for a caveat when
doing so - the same one applies here.

Returns 1 if the db _was_ opened when this call was made, else 0.

See also: [](#db.stmt.finalize)

<a id="db.config"></a>
## db config

Usage: `config ?flags?`

This function allows getting and setting various options. The flags
are:

- **`--null-string X`** sets the representation of SQL NULL result
  values to `X` and returns the old value.
- **`-null-string`** returns the current SQL NULL string
  representation (which defaults to an empty string).

If passed multiple flags, the return result is unspecified - any of
them might have set it in an arbitrary order.

Potential TODO: switch from flags to subcommands. Flags are much
easier to implement for little things like the current functionality,
but subcommands are more extensible.


<a id="db.function"></a>
## db function

Usage: `function ?flags? name`

This subcommand installs an SQL function with the given name, with the
function body being provided as a lambda expression (something which
can be passed to Tcl's `apply` command).

It accepts its flags and non-flag arguments in any order. i.e., the
`name` argument may appear before, after, or between flags.

Its flags are:

- One of the following is required:
  - **`--scalar xFunc`** defines a scalar function, as [detailed
    below](#db.function--scalar).
  - **`--aggregate xStep xFinal`** defines an aggregate function, as
    [detailed below](#db.function---aggregate).
  - **`-----window xStep xFinal xValue xInverse`** defines a window
    function, as [detailed below](#db.function-----window).
- Optional
  - **`--return type`** specifies an SQL data type to coerce the
    result of the function to. If used, it must be one of: `text`,
    `blob`, `real`, `integer`, `null`, `any`. By default (the `any`
    type) it will try to determine the closest match.
  - **`-return-{}-as-null`** indicates that an empty return value from
    the UDF should be returned to SQL as an SQL NULL, rather than an
    empty string or (for the numeric types) 0.
  - **`-deterministic`** sets the `SQLITE_DETERMINISTIC` flag.
  - **`-direct-only`** sets the `SQLITE_DIRECTONLY` flag.
  - **`-innocuous`** sets the `SQLITE_INNOCUOUS` flag.
  - **`-bad-numbers-as-0`** if `--return` specifies a numeric type, this
    tells the function-result-to-SQL conversion to treat an invalid
    numeric value as 0. It defaults to treating them as SQL NULL.

The required flags are documented in the following subsections...

<a id="db.function--scalar"></a>
#### `--scalar xFunc`

Installs the given lambda expression as an [SQL scalar
function][sqlite3_create_function]. (The name `xFunc`, and the names
of its sibling APIs for aggregate and window functions, comes from the
C API documentation.) The return value of the function will be
converted to its SQL equivalent, possibly guided by the
`--return type` flag.

The lambda must have a signature matching:

>
```tcl
{{db args} {...body...}}
```

The SQL-side arity of the function will be determined automatically
based on the structure of the first element in the lambda. The
lambda's first argument is the database command's name so that the
function has a way to access the db without having to use intermediary
global- or namespace-scope symbols.

For example:

>
```tcl
$db function foo --scalar {db {a b {c 3} {d 4}} {

}}
```

That SQL function requires at least two arguments (`a` and `b`) and
accepts up to 4 (`c` and `d`). The binding makes note of that at
function-creation time and enforces that limit when the function is
invoked via SQL.

<a id="db.function---aggregate"></a>
#### `---aggregate xStep xFinal`

Yes, that's _three dashes_. See [these docs](#flags) for why.

This creates an [SQL aggregate function][sqlite3_create_function] using
a pair of callbacks. Each callback must be a lambda in the same format
as described for [the `--scalar` flag](#db.function--scalar) with
the addition that each has a second required argument:

>
```tcl
set xStep {{db aggCx args} {...body...}}
set xFinal {{db aggCx} {...body...}}; # is never passed any arguments
```

The `db` argument is the name of the associated db command object.

The `aggCx` value is each call's "aggregate context", a distinct key
which lets aggregates properly collect and group their result
data.  For example, the following SQL calls an aggregte function named
`foo` multiple times:

>
```sql
select foo(a), foo(b), foo(c) from t;
```

Each call to `foo(X)` has to know which data set (which result column)
it's working with, and that's what the aggregate context tells us.
Each one is a value unique to a given group of calls. There are three
calls, ergo three such groups, in the above example, enabling the
callback to collect and assign their results properly.

The technical details of aggregate contexts are [found in the SQLite
documentation][sqlite3_aggregate_context]. The differences from that
in this API are:

- This API does not provide a pointer to the context, but assigns a
  distinct key to each context based on that pointer. The keys may,
  and frequently will, be recycled between SQL statements, and must be
  considered only useful in the context of a given call to a UDF
  callback. They will be stable only within the context of the
  execution of a single SQL statement.

- This API passes the context key to all callbacks which can use it.
  The alternative would be to provide a db subcommand to fetch it.  In
  fact, the first approach tried was that, but it's computationally
  expensive (i.e. "slow") for UDF callbacks to have to call back in to
  C to get that. Since the context key is required for writing proper
  aggregate functions, this API provides it automatically.

The `xStep` callback is called for each result row of data, and is
passed all arguments which the SQL call passed. Its result value is
ignored, as SQLite collects the final accumulated result via a single
call to `xFinal`, made at the end of the result set.  The result value
from `xFinal` becomes the SQL result.

The aggregate context is passed to both `xStep` and `xFinal`, but it
will be an empty string if `xFinal` is called with no preceding call
to `xStep`, as can happen in a query like:

>
```sql
select foo(1) where (... some clause which matches no rows ...)
```

That has no result rows, so never calls `xStep`, so `xFinal` will be
passed an empty string for the aggregate context.


<a id="db.function-----window"></a>
#### `-----window xStep xFinal xValue xInverse`

Yes, that's _five dashes_. See [these docs](#flags) for why.

This creates an [SQL window function][sqlite3_window] using a set of
four callbacks. Each callback must be a lambda in the same format as
described for [the `---aggregate` flag](#db.function---aggregate).
Their signatures and conventional names are:

>
```tcl
set xStep    {{db aggCx args} {...body...}}
set xFinal   {{db aggCx} {...body...}}
set xValue   {{db aggCx} {...body...}}
set xInverse {{db aggCx args} {...body...}}
```

They need not accept SQL arguments and may specify a fixed or variable
number of them, [just like for scalar
functions](#db.function--scalar).

The `db` argument is the name of the associated db command object.

The `aggCx` value is each call's "aggregate context", as described
for [the `---aggregate` flag](#db.function---aggregate).

See [SQL aggregate function][sqlite3_create_function] for more
information about the responsibilities of each of the callbacks. In short:

- `xStep` and `xInverse` perform work but do not set a result. Their
  return values are ignored. They use the `aggCx` key to stash their
  work (somewhere) so that it can be collected and finally accessed
  via...
- `xFinal` and `xValue` return results computed via the other calls.
  Both will be passed an empty `$aggCx` value if they are used in
  statements which have no result rows.


[sqlite3_create_function]: https://sqlite.org/c3ref/create_function.html
[sqlite3_aggregate_context]: https://sqlite.org/c3ref/aggregate_context.html
[sqlite3_window]: https://sqlite.org/windowfunctions.html#udfwinfunc

<a id="db.info"></a>
## db info

Usage: `info -file|-name`

If called on a closed fossil instance it always returns an empty
string, but on standalone db instances its return value depends on
which flag is passed to it:

- **`-file`** returns the db's file name, which may be on eof
  SQLite's special names, like `:memory:`.
- **`-name`** returns the name of the db main schema. For standalone
  (non-fossil-bound) dbs this is normally "main", but fossil-bound dbs
  will return one of "repo" or "ckout" here.

<a id="db.open"></a>
## db open

Usage: `open ?-no-create? ?-read-only? ?-trace-sql? ?--set varName? ?filename?`

This is unrelated to [fossil open](#fossil.open).

This command creates new db instances by opening databases. It does
not operate on the current database.

If passed no arguments, this function returns 1 if this object has
an opened db, 0 if it does not.

If passed a filename (including any special names supported by SQLite,
e.g. `:memory:` and an empty string), the result of this function is
the name of a new proc which owns a new database handle. The new
object provides the same subcommands as [the fossil db
command](#db), plus [the db close subcommand](#db.close).

The `--set varName` flag causes the return value to also be set in a
scope-local variable with the given name. The [`close`
subcommand](#db.close) will unset that var but see
[fossil.close](#fossil.close) for a caveat when doing so - the same
one applies here.


<a id="db.option"></a>
## db option

TODO: a function for getting/setting per-db-connection options, most
pressingly being the string representation to use for SQL NULLs (which
is internally configurable but is not currently exposed to scripts).

<a id="db.query"></a>
## db query

Usage: `query ?many flags (see below)? sql`

This command runs a single SQL query, providing the ability to bind
SQL parameters and to optionally fetch results via the return value.
It can be used to execute any type of query, but is limited to a
single prepared statement. If passed more, the ones after the first
are ignored.

This command accepts its flags and non-flag arguments in any
order. i.e., the `sql` argument may appear before, after, or between
flags.

This command has many flags, described in the following subsections,
in alphabetical order, and here are some obligatory notes:

- None of the different `-bind/--bind*` variants may be used together.
- [`-prepare`](#db.stmt.prepare) does not mix with all of the other
  flags.

<a id="db.query--bind"></a>
#### `--bind value`

Binds the "next" unnamed parameter. Can be used multiple times to bind
a series of parameters. Alternately...


<a id="db.query--bind-list"></a>
#### `--bind-list {values...}`

Binds a list of positional SQL parameters, with flexible
type-conversion. The list contains values to bind, in their binding
order, eacho one optionally prefixed with a type-specifier flag:

>
```
{
  value1
  -type value2
  -- value3
}
```

The parsing of such flags can be disabled entirely with
[`-no-type-flags`](#db.query-no-type-flags) available to
[`db query`](#db.query) and [`db.stmt bind`](#db.stmt.bind). When that
flag is active, such flags will be treated as bind-column names or
values, as appropriate.

By default the SQL bind-type of a given value will be determined
automatically based on its appearance (e.g. if it looks like an
integer or double, it will be bound as such). Clients may specify the
SQL bind-type for any given value using one the `-type` flags:

- **`-text`** will unconditionally bind as text.
- **`-blob`** will unconditionally bind as a blob.
- **`-real`** will coerce the value to a floating-point number.
- **`-integer`** will coerce the value to an integer.
- **`-nullable`** means that if the value is empty, it should be bound as
  SQL NULL, else an empty string is saved as such.
- **`--`** means that the following value must be interpretted as a value,
  not a potential flag. That value's SQL bind-type is determined
  automatically.  If there is any real chance that passed-in bind
  values may resemble a `-type` flag, prefixing those columns with
  this flag will ensure that they are not treated as flags.

One caveat to the "unconditionally" qualifiers mentioned above: if
[`-bind-{}-as-null`](#db.query-bind-{}-as-null) is in effect, empty
values of any type will be bound as SQL NULL.


<a id="db.query--bind-map"></a>
#### `--bind-map {map...}`

Binds named SQL parameters. The map's basic syntax is identical to
that of [`--bind-list`](#db-query--bind-list) except that it prefixes
each entry with a column-binding name with a prefix of `:`, `@`, or
`$`:

>
```
{
  :X value1
  @Y -type value2
  $Z -- value3
}
```

The `:`, `@`, and `$` prefixes are not arbitrarily chosen: they're
SQLite's syntax for bind-column names, and they must match the ones
used in the associated SQL.  Each such key may be followed by an
optional `-type` flag, as described for
[`--bind-list`](#db.query--bind-list), before the a value.


<a id="db.query-bind-{}-as-null"></a>
#### `-bind-{}-as-null`

Tells the `--bind` variants to bind any empty strings as SQL
NULL. This effect can be achieved on a per-column basis using the
`-nullable` flag with [`--bind-list`](#db.query--bind-list) or
[`--bind-map`](#db.query--bind-map).



<a id="db.query---eval"></a>
#### `---eval varName|-$|-# {script}`

Evals the given script once per result row. The results are
communicated to the script in one of the following ways, depending on
the value of the `---eval` flag's first argument:

- `-$` causes eval-local variables to be set with names matching those
  of the SQL result columns. Pneumonic: the `$` is used to derefrence
  vars.

- `-#` causes eval-local variables to be set with names matching their
  result column indexes, so `$0` is the first one and `$2` is the
  third. In this mode, the column names (a list of numbers) are stored
  in `${*}` unless `-no-column-names` is used. Pneumonic: `#` refers
  to the result set column number.

- Anything else is assumed to be the name of an array object.  The
  array has a member named `*` which contains the column name list,
  and one member for each result column, named after that column. The
  `-no-column-names` flag causes the `*` entry to be elided.

The script is run in the caller's scope. If `--result` is not used, or
has the value `none`, then the most recent result from the eval will
become the function's return result.

<a id="db.query-no-column-names"></a>
#### `-no-column-names`

Tells _some_ of the [`--return` policies](#db.query--return) that they
should elide the column-name entry. Figuring out which ones do and
don't (or should and shouldn't) account for this is TODO.

<a id="db.query-no-type-flags"></a>
#### `-no-type-flags`

Disables the parsing of data type flags in
[`--bind-list`](#db.query--bind-list) and
[`--bind-map`](#db.query--bind-map). That is: if the input contains
any such flags, they are treated as values or binding keys, as
appropriate.

When used with [`-prepare`](#db.stmt), this becomes the policy for
that statement, such that future calls to
[`db.stmt.bind`](#db.stmt.bind) will use that policy. If this flag is
_not_ used in the call the `query -prepare`, the same flag can be
passed to `$stmt bind` on a case-by-case basis to disable that for any
given [`db.stmt.bind`](#db.stmt.bind) invocation.

<a id="db.query--null-string"></a>
#### `--null-string value`

Sets the string reprentation of SQL NULL for query result columns.
This defaults to whatever the db's [current setting](#db.config) is.


<a id="db.query-prepare"></a>
#### `-prepare` and `--prepare varName`

This is a much larger topic, not suitable for a level-4 subheading.
See [](#db.stmt) for details.


<a id="db.query--return"></a>
#### `--return policy`

Specifies what the result of the function should be. Its value has
many options, not all of which are intuitively named:

- **`#n`** returns single value from SQL column `n` (0-based integer)
  of the _first_ result row. (TODO? Support a column name here, maybe
  with a `:` prefix?)
- **`*n`** returns a list of values from SQL column `n` (0-based
  integer) from all rows.
- **`row1-list`** sets the result to the _first_ result row in the form of
  a simple list of values.
- **`*`** works like `row1-list` but it also sets the `-no-column-names`
  flag.
- **`row1-dict`** returns the first result row in the form of a dict, with
  the keys being the SQL column names.
- **`row1-lists`** like `row1-list` but the list has two sub-lists:
  the first is a list of the column names, in their given order, and
  the second is the list of all columns' values.
- **`rows-lists`** works like `row1-lists` but has an additional row for
  every result set row after the first.
- **`**`** works like `rows-lists` but it also sets the `-no-column-names`
flag.
- **`column-names`** returns only the column names. It does not execute
  the query.
- **`eval`** means to use the result of the `---eval` block. This is
  the default if `---eval` is used in but `--return` is not.
- **`exists`** returns 1 if the query has any result rows, else 0.
- **`none`** is the default if no other default has been implied via
  one of the other options (e.g. `eval`). This option does what it
  says: set no result value from the query results.


<a id="db.query-unset-null"></a>
#### `-unset-null`

Tells [`---eval`](#db.query---eval) that it should _unset_ entries
which have SQL NULL values, rather that setting them to [the SQL NULL
string value](#db.query--null-string). That applies to the array mode,
`-$`, and `-#`.


<a id="db.stmt"></a>
## db query Part 2: db.stmt

Queries can be prepared for later (re)execution using:

>
```
$db query --prepare q {...sql...}
# or
set q [$db query -prepare {...sql...}]
```

Those both create a new prepared statement object in the local var
named `q` and bind it to a command, the name of which is the result
value of this function (a Command object).  These docs call such
instances `db.stmt`, for lack of a better name.

The difference between `-prepare` and `--prepare varName` is that the
former returns the name of the new command object whereas the latter
_also_ sets it in a local variable with the given name. For brevity's
sake, the docs below refer to both of these as `-prepare`.

The `db.query` `--return` flag cannot be used with either form of
`-prepare`, and only one of those forms may be used for a given
`query` invocation.

Each prepared statement has its own set of methods, the single most
important of which is [`finalize`](#db.stmt.finalize), which the
caller is obligated to eventually call to free up C-level resources.

If a database is closed while `db.stmt` commands reference it, all
such commands will be invalidated, such that the next invocation of
any of their APIs, aside from `finalize`, will throw an error. If a db
is closed while such commands are in active use, all bets are off -
_Undefined Behavior_. (A potential TODO is cause `db close` to fail
loudly if any queries or [UDFs](#db.function) are currently running.)

The available subcommands are listed in alphabetical order in
the following subsections...


<a id='db.stmt.bind'></a>
### db.stmt bind

Usage: `bind ?flags? bindValues...`

This function has the following uses:

- **`-clear`** clears all bound values and returns an empty string.
- **`-count`** returns the number of bound parameters in the query.
  `-clear` and `-count` may be used together and will return the value
  for `-count`.
- **`-reset`** resets the statement before applying
  bindings. Attempting to bind to a running query (one which has been
  [`step`ped](#db.stmt.step) without a subsequent call to
  [`reset`](#db.stmt.reset) will trigger an error from SQLite. This
  command does not reset automatically to help avoid that client code
  does things like binding from within a query loop. Can be used
  together with `-clear` and/or `-count`.
- **`-no-type-flags`** tells the various binding options (see below)
  to not look for type flags in the input list, as documented
  for [](#db.query-no-type-flags).
- Binding values, as described below.

The syntax for binding values is similar to how they are bound in
[](#db.query):

- `$stmt bind value1 ... valueN` as for as for [](#db.query--bind),
  but without the `--bind` flag.
- `$stmt bind --list {...}` as for [](#db.query--bind-list)
- `$stmt bind --map  {...}` as for [](#db.query--bind-map)

Only one binding option may be used as a time.


<a id='db.stmt.finalize'></a>
### db.stmt finalize

Finalizes the underlying prepared statement, uninstalls its command
object, and frees all C-level resources associated with this
object. If it was prepared using `--prepare varName`, that var is also
unset.

If a [database is closed](#db.close) while statements created through
this API are still alive, the db will invalidate those instances, such
that all of their subcommands, except for `finalize`, will throw an
error if they're called. SQLite prepared statements originating in
native code (and not exposed to Tcl) do not get such treatment, and
finalizing those _after_ their db has been closed leads to undefined
results.

In the tradition of "finalizers do not throw", `finalize` is the only
`db.stmt` subcommand which will not throw an error if its underlying
db handle has already been closed.

<a id='db.stmt.get'></a>
### db.stmt get

Usage: `get ?flags? columnNumber`

Returns result columns from a result row.

With very few exceptions (see below), it is strictly illegal to call
this without a call to [`step`](#db.stmt.step) having returned 1 first.

In the absence of flags which modify this, it normally requires a
single argument: a 0-based result column index for the current query.
It throws if the number is out of range. Some flags change the result
type and preclude the use of an argument.

Flags:

- **`-column-names`** (\*) returns all column names as a list.
- **`-count`** (\*) returns the column count of the result set.
- **`-list`** returns all columns in a single list.
- **`-dict`** returns all columns in a dict-like list, suitable for
  passing to `array set` or for use with Tcl's `dict` command. The
  keys and values are in the same order as they are in the query.
- **`--null-string X`** sets the string value of SQL NULL results. The
  default NULL string is inherited from the [`db query` flag of the
  same name](#db.query--null-string) but can be overridden at get-time
  on a case-by-case basis.

Flags marked with (\*) are legal without a prior succeeding call to
[`step`](#db.stmt.step). The others are not.


<a id='db.stmt.parameter'></a>
### db.stmt parameter

TODO

Subcommands and/or flags:

- `count`: returns the number of bound parameters in the query.
- `index NAME`: returns the index of the given paramter name, or 0
  for an out-of-range value.


<a id='db.stmt.reset'></a>
### db.stmt reset

Resets the statement so that further calls to `step` (see below) will
start at the beginning.


<a id='db.stmt.step'></a>
### db.stmt step

Steps the prepared statement a single time, returning 1 if the query
produced a result row, else 0. Use [the `get`
subcommand](#db.stmt.get) to fetch values from it.


### db.stmt TODOs

Subcommands:

- `step ---eval rowName {...}`. By default a statement inherits the
  eval script, if any, from the [`query`](#db.query) invocation which
  used the `-prepare` flag.


<a id="db.transaction"></a>
## db transaction

Usage: `transaction ?-dry-run? ?-sql? ?-file? ?script?`

> Achtung: it is _never_ legal to use SQL BEGIN/COMMIT/ROLLBACK using
  this API's db connections becuse transactions do not support nesting
  and they will confuse fossil's state of the transaction level.
  Libfossil implements its own transaction API which simulates the
  capability of nested transactions, at the cost that random SQL code
  is prohibited from using those commands (^There's an exception to
  that, but it's beyond the current scope.).


If passed an argument, this function starts an SQL transaction (or
increases the transaction level if one is already active through this
API), processes the given script in the caller's scope, then either
commits (if the script succeeds) or rolls back the transaction (if it
triggers an error).

If the `-file` flag is specified, the single argument is expected to
be a filename. The contents of that file replace the argument for
purposes of further process.

By default it will `eval` the given script, but the `-sql` flag tells
it to treat the script as an SQL blob suitable for use with the
[`db batch` subcommand](#db.batch).

If the script succeeds, its result becomes the result of this function
(noting that `-sql` scripts have no result, even if they run a
`SELECT` query).

If called without a script argument, it returns the current
transaction level. If the value is negative, its absolute value
represents the depth but indicates that a rollback is pending. If it
is positive, the transaction is still in a "good" state. If it is 0,
no transaction is active.

The `-dry-run` flag causes the transaction to roll back even if it
succeeds. In the C API this capability has frequently proven useful
for implementing "dry-run" modes, where any number of db changes are
made and then rolled back without generating an error (unless the
rolling back itself fails, but that "never happens").

> Sidebar: Historically, libfossil used pseudo-nested transactions,
  essentially a reference count for transaction levels which committed
  or rolled back when the refcount reached zero. As of 2025-08 it uses
  SQLite's SAVEPOINT feature, enabling what amounts to properly-nested
  transactions. One advantage to this change is that a rollback of a
  sub-transaction does not automatically force all higher transaction
  levels to roll back when they're eventually popped from the stack.


# TODOs and Plans

Noting that plans and get-dones often differ...

- Filename-related commands for the currently-opened version:
  - `fossil file size ?-checkout? $filename`
  - `fossil file mtime ?-checkout? $filename`
  - `fossil file relative $filename` (make relative to the checkout root)
  - `fossil file dirname ?-noslash? $filename`
  - `fossil file glob $aGlob` (like [fossil.ls](#fossil.ls) but without all
    of the options)
  - `fossil file touch ?--glob* pattern? timestamp|checkin|checkout` (similar
    to fossil's `touch` command)
- Generic filesystem-related commands:
  - `fossil file normalize $filename`
  - `fossil file tail $filename`
- Various commands for working with checkouts: update, revert, merge,
  status, ...
- A new command object type for working with checkins. Checkins are
  incremental, requiring several, perhaps numerous, API calls to stage
  them, so a new subcommand would be useful. We now have
  infrastructure for such objects (used by [](#db.stmt)), so there's
  no technical hurdle blocking this.
- Probably much more.
