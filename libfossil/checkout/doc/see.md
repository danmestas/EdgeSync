# Encrypted Repositories

Licensees of the [SQLite Encryption Extension][see], a.k.a. SEE, can
build libfossil using their preferred SEE variant (which differs for
each encryption type) and use that to encrypt their repository and
checkout databases.

Building with SEE support depends on how libfossil is being built:

1. In the canonical source tree, simply drop a copy of
   `sqlite3-see-XYZ.c` into the `extsrc` directory, where `XYZ` is the
   desired encryption mode (SEE has, as of this writing, 9 variants).

2. When building against the amalgamation (preferred), simply compile
   the desired `sqlite3-see-XYZ.c` into the application.

Minor caveat: when linking against a DLL build of SEE, the API will
have to be activated by the client using `sqlite3_activate_see()`, as
covered in the SEE documentation. That does not apply when linking in
the `sqlite3-...` object file directly into the application.

libfossil does not actually know whether it's been built against an
SEE-capable sqlite3, and uses the SEE-related pragmas to set a
database's encryption key. For non-encrypted databases and non-SEE
builds, those pragmas will be harmless no-ops.

The [f-apps](/dir/f-apps) family of applications, via the `fcli`
mini-framework, all support several CLI flags for setting the key:

- `--see-key KEY` sets the plain-text key `KEY` as the encryption key.
- `--see-hexkey KEY` uses the given hex-encoded key.
- `--see-textkey KEY` works as per the `textkey` sqlite3 shell option
  in the SEE documentation.

For example:

> ```
$ f-new my.repo --see-key foo -m "My new encrypted repo."
```

Will create a new repository encrypted with the given password. Opening
it requires providing that same password:

> ```
$ f-open --see-key foo /path/to/my.repo
# Equivalent:
$ f-open --see-hexkey 666f6f /path/to/my.repo
```

Library clients can enable this support when initializing their
`fsl_cx` context, using the `fsl_cx_config::see` member to define a
callback and optional state for that callback. If the callback is not
`NULL` then it is invoked when the library opens a checkout or
repository database, in order to collect the encryption key from the
client. How the client actually fetches that key is up to the client.
Note that a checkout and a repository must have the same encryption
key.  Opening an encrypted repository into a new checkout will encrypt
the checkout with the same key automatically.

Only repository and checkout databases are subject to encryption.  The
global config database is not encrypted, nor are arbitrary databases
opened using the `fsl_db` API (though clients are free to invoke the
appropriate pragmas to encrypt or unlock them).


[see]: https://sqlite.org/see
