# Auth & Capabilities Design

**Date:** 2026-03-24
**Tickets:** CDG-131 (spike: user access control & capabilities), CDG-155 (implement user auth and capabilities)
**Status:** Draft

## Problem

HandleSync (`handler.go:154`) ignores login cards — any client can push to any server. Multi-user sync is unusable without access control. Sharing repos between users requires emailing passwords.

## Design Principles

- **Repo-sovereign auth.** Each repo's `user` table is the single source of truth for who can do what. No external identity provider required.
- **Fossil-native at the core.** Login card verification uses Fossil's existing HMAC scheme: `SHA1(nonce + SHA1(projectCode/user/password))`. No wire protocol changes.
- **CLI sugar for smooth sharing.** Invite tokens eliminate password management for the common case.
- **Path to more later.** The `auth/` package is a clean boundary. An IdP (Clerk, NATS JWT, etc.) can be layered on top in the future by provisioning user table entries through the same API.

## Architecture

```
+---------------------------+
| CLI (invite, user CRUD)   |  edgesync invite / edgesync user add
+---------------------------+
| auth/ package             |  VerifyLogin, HasCapability, user CRUD
+---------------------------+
| user table (per-repo DB)  |  uid, login, pw, cap, cexpire
+---------------------------+
| HandleSync                |  calls auth.VerifyLogin, checks caps
+---------------------------+
| sync/auth.go (client)     |  computeLogin (unchanged)
+---------------------------+
```

Client-side sync (`computeLogin` in `sync/auth.go`) is unchanged. All new work is server-side verification and the `auth/` package.

## Fossil's User Table

Already exists in the repo schema (`db/schema.go:31-42`):

```sql
CREATE TABLE user(
  uid INTEGER PRIMARY KEY,
  login TEXT UNIQUE,
  pw TEXT,              -- SHA1(projectCode/login/password)
  cap TEXT,             -- capability string e.g. "oi"
  cookie TEXT,
  ipaddr TEXT,
  cexpire DATETIME,     -- credential expiry (used by invite TTL)
  info TEXT,
  mtime DATE,
  photo BLOB
);
```

The `pw` field stores `SHA1(projectCode/login/password)`, not the raw password. The `cap` field is a string of single-letter capability flags.

## Fossil Capability Flags

| Flag | Meaning |
|------|---------|
| `a` | Admin (can add/remove users) |
| `c` | Append (checkin via push) |
| `d` | Delete artifacts |
| `e` | View email addresses |
| `g` | Clone |
| `h` | Hyperlinks (view web UI) |
| `i` | Checkin (push artifacts) |
| `j` | Read wiki |
| `k` | Write wiki |
| `n` | New ticket |
| `o` | Checkout (pull artifacts) |
| `p` | Change own password |
| `r` | Read tickets |
| `s` | Setup (superuser) |
| `t` | Manage tickets |
| `w` | Write tickets |
| `z` | Push unversioned files |

For sync operations, the relevant subset is:
- **Push** requires `i` (checkin)
- **Pull** requires `o` (checkout)
- **Clone** requires `g` (clone)
- **UV write** requires `z` (push unversioned)

## Component 1: `go-libfossil/auth/` Package

### User CRUD

```go
package auth

type User struct {
    UID     int
    Login   string
    Cap     string
    CExpire time.Time // zero value = no expiry
    Info    string
    MTime   time.Time
}

func CreateUser(db *db.DB, projectCode, login, password, caps string) error
func GetUser(db *db.DB, login string) (User, error)
func ListUsers(db *db.DB) ([]User, error)
func UpdateCaps(db *db.DB, login, caps string) error
func SetPassword(db *db.DB, projectCode, login, password string) error
func DeleteUser(db *db.DB, login string) error
```

`CreateUser` stores `pw` as `SHA1(projectCode/login/password)` — matching Fossil's convention. The raw password is never stored.

### Login Verification

```go
func VerifyLogin(db *db.DB, projectCode string, card *xfer.LoginCard, payload []byte) (User, error)
```

Steps:
1. Look up user by `card.User` in the `user` table
2. If not found, return error `"authentication failed"` (generic to prevent user enumeration)
3. If `cexpire` is non-zero and in the past (`time.Now().After(cexpire)`), return error `"authentication failed"`
4. Recompute: `expected = SHA1(card.Nonce + user.PW)`
5. Compare `expected` to `card.Signature` using `crypto/subtle.ConstantTimeCompare`
6. If mismatch, return error `"authentication failed"`
7. Return the `User` (caller uses `User.Cap` for authorization)

All failure cases return the same generic error message to prevent user enumeration.

### Capability Checking

```go
func HasCapability(caps string, required byte) bool

// Convenience wrappers
func CanPush(caps string) bool   // HasCapability(caps, 'i')
func CanPull(caps string) bool   // HasCapability(caps, 'o')
func CanClone(caps string) bool  // HasCapability(caps, 'g')
func CanPushUV(caps string) bool // HasCapability(caps, 'z')
```

## Component 2: HandleSync Integration

### Handler State

Add fields to the `handler` struct:

```go
type handler struct {
    // ... existing fields ...
    user   string  // verified username ("nobody" if no login card)
    caps   string  // capability string from user table
    authed bool    // whether login card was verified
}
```

### Auth State Initialization

Before processing any cards, the handler initializes auth state from the `nobody` user:

```go
func (h *handler) initAuth() {
    h.user = "nobody"
    h.caps = ""
    h.authed = false
    var caps string
    err := h.db.QueryRow("SELECT cap FROM user WHERE login='nobody'").Scan(&caps)
    if err == nil {
        h.caps = caps
    }
    // No nobody row = empty caps = all operations rejected
}
```

If a `LoginCard` is received later, it overrides this default state.

### Card Processing Order

Login cards must be resolved before push/pull/clone cards. The current handler processes control cards in a single pass, which does not guarantee ordering. The fix is a **two-pass** approach over control cards:

1. **First pass (login only):** Scan for `LoginCard`, call `auth.VerifyLogin()`. On success, set `h.user`, `h.caps`, `h.authed`. On failure, emit `ErrorCard` with `"authentication failed"` (generic message to prevent user enumeration).
2. **Second pass (everything else):** Process remaining control cards with capability checks:

- **PushCard**: Check `auth.CanPush(h.caps)`. If yes, set `h.pushOK`. If no, emit `ErrorCard`.
- **PullCard**: Check `auth.CanPull(h.caps)`. If yes, set `h.pullOK`. If no, emit `ErrorCard`.
- **CloneCard**: Check `auth.CanClone(h.caps)`. If yes, set `h.cloneMode`. If no, emit `ErrorCard` and leave `h.cloneMode = false`.

Auth errors are **non-fatal**: the handler emits an `ErrorCard` for the denied operation but continues processing remaining cards. This matches the existing error handling pattern (e.g., `handlePragmaUVHash`).

**UVFileCard** capability check happens in `handleUVFile` (data card pass, not control card pass): check `auth.CanPushUV(h.caps)` before accepting UV writes. Applies to all incoming `UVFileCard` instances regardless of flags.

### Anonymous Access (No Login Card)

Handled by `initAuth()` above. The `nobody` user row is the anonymous access policy:
- If `nobody` exists with `cap = "o"`, anonymous pull is allowed
- If `nobody` does not exist, all operations are rejected (empty caps)

No extra config fields needed. This matches Fossil's behavior.

### HandleOpts

No new fields on `HandleOpts`. Auth uses the repo DB that the handler already has access to.

## Component 3: Invite Tokens

### Token Format

Compact JSON (no whitespace), base64url-encoded with standard padding:

```json
{"url":"nats://100.78.32.45:4222/my-repo","login":"bob","password":"a1b2c3...","caps":"oi"}
```

The password inside the token is the secret — 32 bytes from `crypto/rand`, hex-encoded (64 chars). All CLI-generated passwords use `crypto/rand` (never `simio.Rand`).

The token is meant to be shared once over an out-of-band channel (Slack, text, QR code). It is not signed or encrypted — the password it contains is the credential.

### `edgesync invite`

```
edgesync invite <login> --cap <caps> --repo <path> [--url <sync-url>] [--ttl <duration>]
```

1. Generate a random password (32 bytes hex)
2. Call `auth.CreateUser(db, projectCode, login, password, caps)`
3. If `--ttl` is set, update `cexpire` on the user row
4. Encode the invite token
5. Print:
   ```
   Invite for "bob" (capabilities: oi, expires: 2026-03-25T12:00:00Z):

   edgesync clone --invite eY0aGlzI3Rlc3Q...

   Share this command with the recipient. It contains credentials — treat it like a password.
   ```

The `--url` flag provides the sync URL to embed in the token. If omitted, it is read from the repo's config (e.g., a stored remote URL).

### `edgesync clone --invite <token>`

1. Decode the token
2. Clone the repo from `token.URL` using `token.Login` and `token.Password`
3. Store credentials in the local clone's config for future syncs

After clone, `edgesync sync` uses the stored credentials automatically. Bob never types or sees a password.

## Component 4: User Management CLI

```
edgesync user add <login> --cap <caps> --repo <path>
edgesync user list --repo <path>
edgesync user update <login> --cap <caps> --repo <path>
edgesync user rm <login> --repo <path>
edgesync user passwd <login> --repo <path>
```

`user add` generates a random password and prints it once (same as invite, but without the token wrapper). Useful for scripting or when the admin wants to communicate credentials manually.

`user passwd` generates a new random password, updates the hash, and prints the new password.

## Testing Strategy

### Unit Tests (`auth/`)

- CRUD: create user, verify pw hash matches `SHA1(projectCode/login/password)`, list, update caps, delete
- VerifyLogin: valid card succeeds, wrong password fails, unknown user fails, expired user (`cexpire` in the past) fails
- Capability checks: `HasCapability("oi", 'i')` true, `HasCapability("o", 'i')` false
- Constant-time comparison on signature check

### Integration Tests (`sync/`)

- Extend existing integration tests with user table setup
- Push rejected without `i` cap, pull works with `o` cap, clone works with `g` cap
- Anonymous: `nobody` with `o` cap allows pull, rejects push. No `nobody` user rejects all.
- Wrong password triggers `ErrorCard`, no data exchanged

### DST Tests

- Add auth scenarios: verified users sync normally, unauthorized push fails cleanly
- BUGGIFY corrupted nonce (already exists in `buildLoginCard`) now triggers real auth failure on the server side instead of being silently accepted

### Invite Token Tests

- Round-trip: generate invite token, decode, credentials match
- TTL: create invite with expiry, verify login fails after `cexpire`
- Clone with invite: end-to-end using `MockTransport`

## Migration / Backwards Compatibility

- **No wire protocol changes.** Login cards are already sent by the client. The server just starts checking them.
- **Existing repos** that have no users in their `user` table: all operations will be rejected (no `nobody` user = no anonymous access).
- **`repo.Create` seeds a default `nobody` user with `cghijknorswz` (full caps)** — this preserves current behavior where everything is open by default. Admins restrict access by updating nobody's caps (e.g., `edgesync user update nobody --cap o` for read-only anonymous).
- **Existing tests** keep passing because `repo.Create` seeds permissive caps. Tests that verify auth rejection explicitly delete the nobody user or create users with restricted caps.

## Future Extensions (Out of Scope)

- **IdP integration** (Clerk, Auth0): provisions user table entries via JWT verification. The `auth/` package API is the integration point.
- **NATS JWT auth**: NATS transport verifies identity, maps to user table entry. HandleSync receives verified identity from transport metadata.
- **Cookie sessions** (CDG-116): reduce igot traffic by caching auth state. Uses the existing `cookie` and `cexpire` columns.
- **Private branches** (CDG-117): capability-gated access to specific branches.

## Files Changed

| File | Change |
|------|--------|
| `go-libfossil/auth/` (new) | User CRUD, VerifyLogin, capability checks |
| `go-libfossil/sync/handler.go` | Wire in auth verification, capability checks on push/pull/clone |
| `go-libfossil/repo/create.go` | Seed default `nobody` user on repo creation |
| `cmd/edgesync/` | Add `user` and `invite` subcommands |
| `go-libfossil/sync/handler_test.go` | Auth integration tests |
| `dst/` | Auth scenarios in deterministic sim |
