# Fossil's Sync System

This doc describes, at a high level, libfossil's support for
fossil(1)'s sync protocol. The complete docs of that protocol
[can be found on the fossil site][sync.wiki].

Work on sync support started in mid-2025. As of this writing, there is
still much more to do but the underlying pieces to support sync are in
place and have an appropriately pleasing shape. What remains to
be done is _lots_ of detail work to plug it all together.

As a library, libfossil has different constraints than a monolithic
app for sync, including, but not limited to:

- Status output cannot simply be sent to stdout.
- Interactive prompting for, e.g., a sync password requires app-level
  support.
- Whereas fossil is very specifically focused on network traffic via
  IP addresses, the library does not want to have to know how to deal
  with any specifics of the I/O channel, to the point of not caring
  whether it's network-based or not, so it outsources all I/O-channel
  details to a subsystem which is extensible at the level of client
  code.

The fundamental architectural difference between libfossil's sync support is
that it has an inviolate separation between the responsibilities of
the library and the transport layer:

- Library: generates and processes all sync protocol content. The I/O
  channel for transporting that content, however...
- Client: provides the I/O channel. In essence, any streaming-capable
  I/O channel can work (random access is not necessary, but the
  ability to "rewind" and re-read a stream has proven useful (the
  library internals do not rely on this - they have a strictly one-way
  streaming view of all sync traffic)).  Though this is, from the
  perspective of the library, a "client-level" component, most clients
  won't want to provide implementations for such channels, so...
- Library: provides "default implementations" of channels [described
  below](#sc-impls). One API requirement is that library-side of the
  API be ignorant of all transport-level specifics, and these
  implementations demonstrate that that's the case.

Given a URL, the library can determine whether one of its built-in
channels can support it. For custom URLs, custom channel objects can
be provided.

In this API we call the library part the "xfer subsystem" or "sync
subsystem", which is mostly implemented as library-private pieces which
client code cannot directly interact with (see the `fsl__xfer` class).
The I/O layer is called the "sync channel" API (see the `fsl_sc`
class).  The I/O layer's sole job is shoveling _opaque_ (to it) bytes
arounds. There is one small exception where sync protocol specifics
"leak" into the I/O objects: the "login card" for authenticated
requests is built by the library but how it is _emitted_ is
necessarily channel-specific. The login info's inbound processing, on
the other hand, is channel-independent and handled entirely by the
library.

# I/O Channels (`fsl_sc`)

The `fsl_sc` interface describes the requirements for all I/O
channels. Any compatible implementation should be a drop-in
replacement for any other. ("sc" is short for "sync channel", and the
class was initially named `fsl_sync_channel`.)

The sync channel operations are:

- **`init()`** sets up any resources needed by the implementation and
  may perform implementation-specific validation.
- **`append()`** writes an opaque blob of memory to the channel, and
  it is called many times during the creation of a request. Where the
  channel stores this is its own business. As this message is
  constructed, the library keeps track of both the total output size
  and the incrementally-calculated SHA1 hash of the sync message, as
  those info are necessary for composing the authentication signature
  for the login card[^side-effect1].\  
  Trivia: somewhat surprisingly, using temp files has proven to be
  _significantly_ faster than in-memory buffers for this: anywhere
  from 20-50% faster in a variety of tests.
- **`submit()`** is called when the library is done composing (via
  `append()`) a sync message. That is the part where the channel
  initiates a connection and posts the message to an HTTP endpoint.
  The message includes HTTP headers, and their generation can either
  be done automatically by the library or, for channels which have
  different requirements, can be handled by the channel object
  itself. e.g. the current `curl`-based impl generates the HTTP pieces
  on its own, whereas the `test-http` channel is more generic and can
  make use of the library-provided headers. Each channel has a flag
  telling the library which HTTP header-generation policy to use.
- **`read()`** pulls input from the response into the library. This
  has three specific sub-operations:\  
  1) **Read a single line** because the sync protocol is line-oriented.
  2) **Read a fixed amount** of bytes because the binary payloads of
     sync messages are embedded between those lines and their sizes
     are encoded in the line-oriented parts.
  3) **Real _all_ bytes** at once. This is used by the library to take
     over handling of _compressed_ responses from the server. It does
     this by buffering the whole response, decompressing it, and
     installing a proxy I/O channel object which servers that
     decompressed buffer via its `read()` method. This keeps the
     individual I/O channels from having to detect and deal with the
     compression on their own. Additionally, it moves most of the
     handling of that uncompressed buffer out of the sync internals
     and into a small `fsl_sc` implemenation (which itself is an
     internal detail, but it's being interacted with via its public
     `fsl_sc` API).
- **`cleanup()`** is the destructor.

Errors triggered from within those APIs are reported via the
corresponding libfossil context object (`fsl_cx`) and are propagated
back through to the client level.

The sync process essentially looks like this:

- Client configures an object which describes the specifics of the
  sync, e.g. do we want to clone or just fetch specific pieces of the
  remote configuration, etc.\  
  Sidebar: this configuration _currently_ does not include a specific
  I/O channel, instead leaving the library to select one based on the
  configured sync URL. As the sync API matures, the plan is to
  eventually enable client-level code to provide their own I/O channel
  object.  Not because anyone would ever _want_ to, but _because we
  can_.
- Client passes that configuration to the library.
- The library selects a channel object based on the configuration's
  URL.
- It then transforms the configuration object into a sync message
  payload, which it incrementally `append()`s to the output channel
  and eventually `submit()`s.  When the response arrives, it is parsed
  and processed, which may result in the library initiating follow-up
  traffic with the channel, e.g. to handle the multiple round-trips
  involved in cloning. The same channel object is reused for each
  round trip and `fsl_sc::init()` supports different "levels" of
  initialization to account for this.
  - During this process, the sync routines will emit messages about
    the progress, which clients can subscribe to via a callback. This
    callback can also be used to cancel an in-progress sync by
    returning non-0.
- When complete, control returns to the client, who now has very
  little idea about what _exactly_ happened in that transaction, e.g.
  whether any new content was received. The amount of information
  about _precisely_ which work was performed is currently very
  limited, but the APIs will eventually be fleshed out to provide some
  useful amount of detail to the client. The information is all there,
  it's just a matter of formulating it for client-side use.

<a id="sc-impls"></a>
## Current Implementations

Most of the [current I/O channels](/file/src/sc.c) are part of a
family in which one abstract base class (`fsl_sc_popen`) manages most
of the details and an external application provides the actual
transport. The former takes up the overwhelmingly vast majority of the
code, and the latter requires implementing a single function per
external binary, to build up an appropriate command-line
invocation. That method is not part of the `fsl_sc` interface, but is
an extension of the base class.

### Channel: Local Files

`file://` URLs and local file names use the same sync channel as
fossil(1) itself does: it shells out to the system's `fossil` binary
and invokes it as `fossil test-http ...`, effectively mimicing
communication with a web server. In this mode, fossil does not honor
any repository-level permissions. i.e. the user has full admin access
to the repo.


### Channel: HTTP(s)

`http://` and `https://` URLs are currently handled by shelling out to
the system's `curl` binary.

Using a `curl` binary, instead of a networking library, (A) is a
stopgap measure, easier (for me) to implement than in-library
networking and (B) was a proof-of-concept and important testing ground
for the I/O API abstraction. The plan is to eventually port over
fossil(1)'s HTTP support.

### Channel: SSH

`ssh://...` URLs are handled the same way they are in fossil(1): by
shelling out to the system's `ssh` binary. Once connected to the
remote, its `fossil test-http` command is run, exactly as described
for `file://` URLs above.

Peculiarities of Fossil sync over SSH:

- For purposes of user permissions, fossil treats `ssh` access the
  same as local file access, meaning that any connection gets full
  admin access. When using SSH aliases together with key-based logins,
  neither a user name nor a password are needed for SSH URLs.
- The user name part of a URL is for `ssh` itself, not the fossil user
  (as fossil does not care who the user is for local-file and SSH
  access).
- Conversely, the password part of a URL is for fossil, not `ssh`.

### Channel: Tracer

This pseudo-channel plugs in as a proxy for a another channel and
emits debug output (to a client-defined callback) as each `fsl_sc`
method is called. It's intended only for debugging purposes to help
trace the lifecycle and activity of channels.

Sidebar: the addition of this channel revealed an interesting hole in
the `fsl_sc` interface. The library should not know it is using a
proxy, but certain internals, like checking a channel's flags and URL,
require access to the _original_ channel. The `fsl_sc::self()` method
was added for that purpose: when called on a channel object, it
returns either that channel object or, if that channel is a proxy of
some sort, the channel it is proxying (recursively, to account for
multiple proxies). This enables the library to call the proper
(most-derived) `fsl_sc` methods but also get access to the original
object's state like flags and URL. (The URL of a sync is currently
stored in the channel, as that's where it seems to best fit within the
API's constellation.)


# Passwords

When given a URL which contains a password part, the library will use
it as-is. When given a URL with a user name but no password, it will
attempt to look up a stored password for this URL. If it finds no
password, it will continue to sync, but will later fail if the remote
requires a login for a given operation.

As a special case, if given a password of `""` or `"?"` (without the
quoted) it will use a client-provided callback to prompt for a
password, provided one is available (if not, it will fail, complaining
about the lack of a way to fetch the password). Client applications
can install a callback suitable for their app, and CLI applications
can get away with using `fsl_pw_f_getpass()`, an implementation which
uses the deprecated/obsolete Unix
[`getpass(3)`](https://man7.org/linux/man-pages/man3/getpass.3.html).

Syncing will optionally store a password, if provided one, in the
global fossil config database with a key in the form
`libfossil-sync-pw:THE_URL` (shorn of any password part). Its value is
the fossil-hashed form of the password.

If a single URL is used for multiple repositories (i.e. multiple
project codes), logging in will fail for all but the most-recently
saved one. In such cases, a prompt can be forced using an empty
password or `"?"`, as described above.


# Open Design Issues

- **TMI**? Telling clients exactly what is going on in a sync
  operation, e.g. whether new content was pulled, is not currently
  well-covered.  The sync layer's message framework provides some
  level of information, but others, like metrics, need to be passed on
  in other ways.

- **Receive-IDs**: this API's separation of responsibilities means
  that the library has no way of knowing the IP of a remote
  connection. That is by very intentional design but it means that the
  library cannot maintain the "rcvid" info which fossil stores in
  order ot keep track of where any pushed-in blobs came from. That
  info requires the remote IPs, but the library doesn't even know what
  an IP is. Similarly, channel implementations may not have that info
  (none of the three current ones do, because they hand off the URL to
  external applications to deal with, but one of them is only useful
  for localhost, so we could fake 127.0.0.1 there).


# Reminders to Self

- We may want to port over fossil(1)'s "db protect push/pop" mechanism
  to keep I/O channels from manipulating the db directly. e.g. a
  malicious one could _hypothetically_ do something like replace a
  user's password, and thereby be able to sign traffic in place of the
  library (so long as that password matched on the remote). That's all
  wildly hypothetical, of course. Anyone can write their own C code to
  manipulate a fossil client or database, though, so the library does
  not introduce any _new_ attack vector. e.g. even "protected" this
  way, an I/O channel could open a second instance of that db, either
  with libfossil's API or the SQLite API, and manipulate it. Such
  protections would be limited to code which does not go out of its
  way to bypass them, i.e. it would (as it repeatedly has in fossil)
  help protect against dev-time accidents as opposed to intentional db
  manipulation.


[sync.wiki]: fossil:/doc/trunk/www/sync.wiki

[^side-effect1]:
  One interesting, but admittedly unintentional, side-effect of the
  specifics of the API's separation of responsibilities is that even
  though "client-level" code is responsible for the underlying
  communication, the _library_ has full control over the _content_ of
  that communication. That is, because of how this API is shaped and
  how fossil's signing of authenticated messages works, it's
  "impossible" for that client-level code to "misinteract", either
  intentionally or otherwise, in such a way as to compromise the
  integrity of that traffic. If the I/O channel mis-writes a single
  byte, or adds/removes content to/from the stream, the signing which
  is done at the fossil protocol level ensures that neither the
  library nor fossil will accept such "tainted" traffic.  This aspect
  is specifically limited to _authenticated_ traffic.  Anonymous
  traffic could hypothetically be modified by I/O channels, but anyone
  can post any anonymous traffic they want with `curl`, something
  neither this library nor fossil can do anything about.  If that
  traffic is syntactically valid, both will use it without any concern
  for its source.
