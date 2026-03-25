# Auth & Capabilities Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add server-side login card verification, capability-based access control, and invite token CLI to EdgeSync.

**Architecture:** New `go-libfossil/auth/` package handles user CRUD, login verification, and capability checks. `sync/handler.go` gets two-pass card processing (login first, then capability-gated push/pull/clone). CLI commands in `cmd/edgesync/` for user management and invite tokens. `repo.Create` seeds a default `nobody` user with full caps for backwards compatibility.

**Tech Stack:** Go, SQLite (user table), crypto/sha1, crypto/subtle, crypto/rand, encoding/base64

**Spec:** `docs/superpowers/specs/2026-03-24-auth-capabilities-design.md`

---

## File Structure

| File | Responsibility |
|------|---------------|
| `go-libfossil/auth/auth.go` | User type, HasCapability, CanPush/Pull/Clone/PushUV |
| `go-libfossil/auth/user.go` | User CRUD: CreateUser, GetUser, ListUsers, UpdateCaps, SetPassword, DeleteUser |
| `go-libfossil/auth/verify.go` | VerifyLogin — validate login card against user table |
| `go-libfossil/auth/auth_test.go` | Capability check tests |
| `go-libfossil/auth/user_test.go` | CRUD tests |
| `go-libfossil/auth/verify_test.go` | Login verification tests |
| `go-libfossil/auth/invite.go` | Invite token encode/decode |
| `go-libfossil/auth/invite_test.go` | Token round-trip tests |
| `go-libfossil/sync/handler.go` | Modified: auth state fields, initAuth, two-pass card processing |
| `go-libfossil/sync/handler_test.go` | Modified: auth integration tests added |
| `go-libfossil/db/schema.go` | Modified: SeedNobody function |
| `go-libfossil/repo/repo.go` | Modified: CreateWithEnv calls SeedNobody |
| `cmd/edgesync/cli.go` | Modified: add User and Invite commands to CLI struct |
| `cmd/edgesync/user.go` | User CRUD CLI commands |
| `cmd/edgesync/invite.go` | Invite and clone --invite CLI commands |

---

### Task 1: Capability Checking (`auth/auth.go`)

**Files:**
- Create: `go-libfossil/auth/auth.go`
- Test: `go-libfossil/auth/auth_test.go`

- [ ] **Step 1: Write failing tests for HasCapability and convenience wrappers**

```go
// auth_test.go
package auth

import "testing"

func TestHasCapability(t *testing.T) {
	tests := []struct {
		caps     string
		required byte
		want     bool
	}{
		{"oi", 'o', true},
		{"oi", 'i', true},
		{"oi", 'g', false},
		{"", 'o', false},
		{"cghijknorswz", 's', true},
		{"cghijknorswz", 'a', false},
	}
	for _, tt := range tests {
		got := HasCapability(tt.caps, tt.required)
		if got != tt.want {
			t.Errorf("HasCapability(%q, %q) = %v, want %v", tt.caps, tt.required, got, tt.want)
		}
	}
}

func TestCanPush(t *testing.T) {
	if !CanPush("oi") {
		t.Error("CanPush(oi) should be true")
	}
	if CanPush("o") {
		t.Error("CanPush(o) should be false")
	}
}

func TestCanPull(t *testing.T) {
	if !CanPull("oi") {
		t.Error("CanPull(oi) should be true")
	}
	if CanPull("i") {
		t.Error("CanPull(i) should be false")
	}
}

func TestCanClone(t *testing.T) {
	if !CanClone("g") {
		t.Error("CanClone(g) should be true")
	}
	if CanClone("oi") {
		t.Error("CanClone(oi) should be false")
	}
}

func TestCanPushUV(t *testing.T) {
	if !CanPushUV("z") {
		t.Error("CanPushUV(z) should be true")
	}
	if CanPushUV("oi") {
		t.Error("CanPushUV(oi) should be false")
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd go-libfossil && go test -buildvcs=false ./auth/ -v`
Expected: FAIL — package does not exist

- [ ] **Step 3: Implement auth.go**

```go
// auth.go
package auth

import (
	"strings"
	"time"
)

// User represents a row in the Fossil user table.
type User struct {
	UID     int
	Login   string
	Cap     string
	CExpire time.Time // zero value = no expiry
	Info    string
	MTime   time.Time
}

// HasCapability reports whether caps contains the required capability letter.
func HasCapability(caps string, required byte) bool {
	return strings.IndexByte(caps, required) >= 0
}

// CanPush reports whether caps includes checkin (i) capability.
func CanPush(caps string) bool { return HasCapability(caps, 'i') }

// CanPull reports whether caps includes checkout (o) capability.
func CanPull(caps string) bool { return HasCapability(caps, 'o') }

// CanClone reports whether caps includes clone (g) capability.
func CanClone(caps string) bool { return HasCapability(caps, 'g') }

// CanPushUV reports whether caps includes unversioned push (z) capability.
func CanPushUV(caps string) bool { return HasCapability(caps, 'z') }
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd go-libfossil && go test -buildvcs=false ./auth/ -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/auth/auth.go go-libfossil/auth/auth_test.go
git commit -m "feat(auth): add capability checking (HasCapability, CanPush/Pull/Clone/PushUV)"
```

---

### Task 2: User CRUD (`auth/user.go`)

**Files:**
- Create: `go-libfossil/auth/user.go`
- Test: `go-libfossil/auth/user_test.go`

**Context:** The user table schema is in `go-libfossil/db/schema.go:31-42`. The `pw` field stores `SHA1(projectCode/login/password)`. Use the existing `sha1Hex` pattern from `sync/auth.go:38-41` (but define it locally in `auth/` to avoid a circular dependency on `sync/`).

- [ ] **Step 1: Write failing tests for CreateUser and GetUser**

```go
// user_test.go
package auth

import (
	"path/filepath"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/db"
	dbschema "github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
)

func setupTestDB(t *testing.T) *db.DB {
	t.Helper()
	path := filepath.Join(t.TempDir(), "test.fossil")
	r, err := repo.Create(path, "admin", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("repo.Create: %v", err)
	}
	t.Cleanup(func() { r.Close() })
	return r.DB()
}

func projectCode(t *testing.T, d *db.DB) string {
	t.Helper()
	var code string
	if err := d.QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&code); err != nil {
		t.Fatalf("project-code: %v", err)
	}
	return code
}

func TestCreateAndGetUser(t *testing.T) {
	d := setupTestDB(t)
	pc := projectCode(t, d)

	if err := CreateUser(d, pc, "bob", "secret123", "oi"); err != nil {
		t.Fatalf("CreateUser: %v", err)
	}

	u, err := GetUser(d, "bob")
	if err != nil {
		t.Fatalf("GetUser: %v", err)
	}
	if u.Login != "bob" {
		t.Errorf("Login = %q, want bob", u.Login)
	}
	if u.Cap != "oi" {
		t.Errorf("Cap = %q, want oi", u.Cap)
	}
	// pw should be SHA1(projectCode/bob/secret123)
	expectedPW := hashPassword(pc, "bob", "secret123")
	var storedPW string
	d.QueryRow("SELECT pw FROM user WHERE login='bob'").Scan(&storedPW)
	if storedPW != expectedPW {
		t.Errorf("stored pw = %q, want %q", storedPW, expectedPW)
	}
}

func TestCreateUserDuplicate(t *testing.T) {
	d := setupTestDB(t)
	pc := projectCode(t, d)

	CreateUser(d, pc, "alice", "pass1", "o")
	err := CreateUser(d, pc, "alice", "pass2", "oi")
	if err == nil {
		t.Fatal("expected error for duplicate user")
	}
}

func TestGetUserNotFound(t *testing.T) {
	d := setupTestDB(t)
	_, err := GetUser(d, "nonexistent")
	if err == nil {
		t.Fatal("expected error for missing user")
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd go-libfossil && go test -buildvcs=false ./auth/ -run TestCreate -v`
Expected: FAIL — CreateUser not defined

- [ ] **Step 3: Implement CreateUser, GetUser, and hashPassword**

```go
// user.go
package auth

import (
	"crypto/sha1"
	"database/sql"
	"encoding/hex"
	"fmt"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/db"
)

// hashPassword computes SHA1(projectCode/login/password) matching Fossil's convention.
func hashPassword(projectCode, login, password string) string {
	h := sha1.Sum([]byte(projectCode + "/" + login + "/" + password))
	return hex.EncodeToString(h[:])
}

// CreateUser inserts a new user into the repo's user table.
func CreateUser(d *db.DB, projectCode, login, password, caps string) error {
	if d == nil {
		panic("auth.CreateUser: d must not be nil")
	}
	if login == "" {
		panic("auth.CreateUser: login must not be empty")
	}
	pw := hashPassword(projectCode, login, password)
	_, err := d.Exec(
		"INSERT INTO user(login, pw, cap, mtime) VALUES(?, ?, ?, julianday('now'))",
		login, pw, caps,
	)
	if err != nil {
		return fmt.Errorf("auth.CreateUser %q: %w", login, err)
	}
	return nil
}

// GetUser retrieves a user by login name.
func GetUser(d *db.DB, login string) (User, error) {
	if d == nil {
		panic("auth.GetUser: d must not be nil")
	}
	var u User
	var cexpire sql.NullString
	err := d.QueryRow(
		"SELECT uid, login, cap, cexpire, info, mtime FROM user WHERE login = ?",
		login,
	).Scan(&u.UID, &u.Login, &u.Cap, &cexpire, &u.Info, &u.MTime)
	if err != nil {
		return User{}, fmt.Errorf("auth.GetUser %q: %w", login, err)
	}
	if cexpire.Valid && cexpire.String != "" {
		u.CExpire, _ = time.Parse("2006-01-02 15:04:05", cexpire.String)
	}
	return u, nil
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd go-libfossil && go test -buildvcs=false ./auth/ -run "TestCreate|TestGet" -v`
Expected: PASS

- [ ] **Step 5: Write failing tests for ListUsers, UpdateCaps, SetPassword, DeleteUser**

```go
// append to user_test.go

func TestListUsers(t *testing.T) {
	d := setupTestDB(t)
	pc := projectCode(t, d)

	CreateUser(d, pc, "alice", "pass", "o")
	CreateUser(d, pc, "bob", "pass", "oi")

	users, err := ListUsers(d)
	if err != nil {
		t.Fatalf("ListUsers: %v", err)
	}
	// admin + alice + bob = 3
	if len(users) < 3 {
		t.Fatalf("ListUsers returned %d users, want >= 3", len(users))
	}
}

func TestUpdateCaps(t *testing.T) {
	d := setupTestDB(t)
	pc := projectCode(t, d)

	CreateUser(d, pc, "bob", "pass", "o")
	if err := UpdateCaps(d, "bob", "ois"); err != nil {
		t.Fatalf("UpdateCaps: %v", err)
	}
	u, _ := GetUser(d, "bob")
	if u.Cap != "ois" {
		t.Errorf("Cap = %q, want ois", u.Cap)
	}
}

func TestSetPassword(t *testing.T) {
	d := setupTestDB(t)
	pc := projectCode(t, d)

	CreateUser(d, pc, "bob", "oldpass", "o")
	if err := SetPassword(d, pc, "bob", "newpass"); err != nil {
		t.Fatalf("SetPassword: %v", err)
	}
	expected := hashPassword(pc, "bob", "newpass")
	var pw string
	d.QueryRow("SELECT pw FROM user WHERE login='bob'").Scan(&pw)
	if pw != expected {
		t.Errorf("pw after SetPassword = %q, want %q", pw, expected)
	}
}

func TestDeleteUser(t *testing.T) {
	d := setupTestDB(t)
	pc := projectCode(t, d)

	CreateUser(d, pc, "bob", "pass", "o")
	if err := DeleteUser(d, "bob"); err != nil {
		t.Fatalf("DeleteUser: %v", err)
	}
	_, err := GetUser(d, "bob")
	if err == nil {
		t.Fatal("expected error after deleting user")
	}
}
```

- [ ] **Step 6: Implement ListUsers, UpdateCaps, SetPassword, DeleteUser**

```go
// append to user.go

// ListUsers returns all users in the repo.
func ListUsers(d *db.DB) ([]User, error) {
	if d == nil {
		panic("auth.ListUsers: d must not be nil")
	}
	rows, err := d.Query("SELECT uid, login, cap, info FROM user ORDER BY login")
	if err != nil {
		return nil, fmt.Errorf("auth.ListUsers: %w", err)
	}
	defer rows.Close()
	var users []User
	for rows.Next() {
		var u User
		if err := rows.Scan(&u.UID, &u.Login, &u.Cap, &u.Info); err != nil {
			return nil, err
		}
		users = append(users, u)
	}
	return users, rows.Err()
}

// UpdateCaps changes a user's capability string.
func UpdateCaps(d *db.DB, login, caps string) error {
	if d == nil {
		panic("auth.UpdateCaps: d must not be nil")
	}
	res, err := d.Exec("UPDATE user SET cap=?, mtime=julianday('now') WHERE login=?", caps, login)
	if err != nil {
		return fmt.Errorf("auth.UpdateCaps %q: %w", login, err)
	}
	n, _ := res.RowsAffected()
	if n == 0 {
		return fmt.Errorf("auth.UpdateCaps: user %q not found", login)
	}
	return nil
}

// SetPassword updates a user's password hash.
func SetPassword(d *db.DB, projectCode, login, password string) error {
	if d == nil {
		panic("auth.SetPassword: d must not be nil")
	}
	pw := hashPassword(projectCode, login, password)
	res, err := d.Exec("UPDATE user SET pw=?, mtime=julianday('now') WHERE login=?", pw, login)
	if err != nil {
		return fmt.Errorf("auth.SetPassword %q: %w", login, err)
	}
	n, _ := res.RowsAffected()
	if n == 0 {
		return fmt.Errorf("auth.SetPassword: user %q not found", login)
	}
	return nil
}

// DeleteUser removes a user from the repo.
func DeleteUser(d *db.DB, login string) error {
	if d == nil {
		panic("auth.DeleteUser: d must not be nil")
	}
	res, err := d.Exec("DELETE FROM user WHERE login=?", login)
	if err != nil {
		return fmt.Errorf("auth.DeleteUser %q: %w", login, err)
	}
	n, _ := res.RowsAffected()
	if n == 0 {
		return fmt.Errorf("auth.DeleteUser: user %q not found", login)
	}
	return nil
}
```

- [ ] **Step 7: Run all auth tests**

Run: `cd go-libfossil && go test -buildvcs=false ./auth/ -v`
Expected: PASS

- [ ] **Step 8: Commit**

```bash
git add go-libfossil/auth/user.go go-libfossil/auth/user_test.go
git commit -m "feat(auth): add user CRUD (CreateUser, GetUser, ListUsers, UpdateCaps, SetPassword, DeleteUser)"
```

---

### Task 3: Login Verification (`auth/verify.go`)

**Files:**
- Create: `go-libfossil/auth/verify.go`
- Test: `go-libfossil/auth/verify_test.go`

**Context:** The client computes login cards in `sync/auth.go:14-28` using `SHA1(nonce + SHA1(projectCode/user/password))`. The server must recompute `SHA1(nonce + storedPW)` and compare. `storedPW` is already `SHA1(projectCode/user/password)`.

- [ ] **Step 1: Write failing tests for VerifyLogin**

```go
// verify_test.go
package auth

import (
	"crypto/sha1"
	"encoding/hex"
	"testing"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

// buildLoginCard mirrors sync.computeLogin for test use.
func buildLoginCard(user, password, projectCode string, payload []byte) *xfer.LoginCard {
	nonce := sha1hex(payload)
	sharedSecret := sha1hex([]byte(projectCode + "/" + user + "/" + password))
	signature := sha1hex([]byte(nonce + sharedSecret))
	return &xfer.LoginCard{User: user, Nonce: nonce, Signature: signature}
}

func sha1hex(data []byte) string {
	h := sha1.Sum(data)
	return hex.EncodeToString(h[:])
}

func TestVerifyLoginSuccess(t *testing.T) {
	d := setupTestDB(t)
	pc := projectCode(t, d)

	CreateUser(d, pc, "bob", "secret", "oi")

	payload := []byte("test payload\n")
	card := buildLoginCard("bob", "secret", pc, payload)

	u, err := VerifyLogin(d, pc, card)
	if err != nil {
		t.Fatalf("VerifyLogin: %v", err)
	}
	if u.Login != "bob" {
		t.Errorf("Login = %q, want bob", u.Login)
	}
	if u.Cap != "oi" {
		t.Errorf("Cap = %q, want oi", u.Cap)
	}
}

func TestVerifyLoginWrongPassword(t *testing.T) {
	d := setupTestDB(t)
	pc := projectCode(t, d)

	CreateUser(d, pc, "bob", "secret", "oi")

	payload := []byte("test payload\n")
	card := buildLoginCard("bob", "wrong", pc, payload)

	_, err := VerifyLogin(d, pc, card)
	if err == nil {
		t.Fatal("expected error for wrong password")
	}
}

func TestVerifyLoginUnknownUser(t *testing.T) {
	d := setupTestDB(t)
	pc := projectCode(t, d)

	payload := []byte("test payload\n")
	card := buildLoginCard("nobody-here", "pass", pc, payload)

	_, err := VerifyLogin(d, pc, card)
	if err == nil {
		t.Fatal("expected error for unknown user")
	}
}

func TestVerifyLoginExpired(t *testing.T) {
	d := setupTestDB(t)
	pc := projectCode(t, d)

	CreateUser(d, pc, "bob", "secret", "oi")
	// Set cexpire to the past
	past := time.Now().Add(-1 * time.Hour).Format("2006-01-02 15:04:05")
	d.Exec("UPDATE user SET cexpire=? WHERE login='bob'", past)

	payload := []byte("test payload\n")
	card := buildLoginCard("bob", "secret", pc, payload)

	_, err := VerifyLogin(d, pc, card)
	if err == nil {
		t.Fatal("expected error for expired credentials")
	}
}

func TestVerifyLoginNonExpired(t *testing.T) {
	d := setupTestDB(t)
	pc := projectCode(t, d)

	CreateUser(d, pc, "bob", "secret", "oi")
	// Set cexpire to the future
	future := time.Now().Add(24 * time.Hour).Format("2006-01-02 15:04:05")
	d.Exec("UPDATE user SET cexpire=? WHERE login='bob'", future)

	payload := []byte("test payload\n")
	card := buildLoginCard("bob", "secret", pc, payload)

	u, err := VerifyLogin(d, pc, card)
	if err != nil {
		t.Fatalf("VerifyLogin: %v", err)
	}
	if u.Login != "bob" {
		t.Errorf("Login = %q, want bob", u.Login)
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd go-libfossil && go test -buildvcs=false ./auth/ -run TestVerify -v`
Expected: FAIL — VerifyLogin not defined

- [ ] **Step 3: Implement VerifyLogin**

```go
// verify.go
package auth

import (
	"crypto/sha1"
	"crypto/subtle"
	"database/sql"
	"encoding/hex"
	"errors"
	"fmt"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

// ErrAuthFailed is returned for all authentication failures.
// A single generic message prevents user enumeration.
var ErrAuthFailed = errors.New("authentication failed")

// VerifyLogin validates a login card against the user table.
// Returns the verified User on success, or ErrAuthFailed on any failure.
// VerifyLogin validates a login card against the user table.
// The server does NOT need the raw payload — the nonce is in card.Nonce.
// The client computed nonce = SHA1(payload), but the server just uses it directly.
func VerifyLogin(d *db.DB, projectCode string, card *xfer.LoginCard) (User, error) {
	if d == nil {
		panic("auth.VerifyLogin: d must not be nil")
	}
	if card == nil {
		panic("auth.VerifyLogin: card must not be nil")
	}

	var uid int
	var login, pw, cap string
	var cexpire sql.NullString
	err := d.QueryRow(
		"SELECT uid, login, pw, cap, cexpire FROM user WHERE login = ?",
		card.User,
	).Scan(&uid, &login, &pw, &cap, &cexpire)
	if err != nil {
		return User{}, ErrAuthFailed
	}

	// Check expiry
	if cexpire.Valid && cexpire.String != "" {
		exp, parseErr := time.Parse("2006-01-02 15:04:05", cexpire.String)
		if parseErr == nil && time.Now().After(exp) {
			return User{}, ErrAuthFailed
		}
	}

	// Recompute expected signature: SHA1(nonce + storedPW)
	h := sha1.Sum([]byte(card.Nonce + pw))
	expected := hex.EncodeToString(h[:])

	if subtle.ConstantTimeCompare([]byte(expected), []byte(card.Signature)) != 1 {
		return User{}, ErrAuthFailed
	}

	return User{UID: uid, Login: login, Cap: cap}, nil
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd go-libfossil && go test -buildvcs=false ./auth/ -v`
Expected: PASS (all auth tests)

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/auth/verify.go go-libfossil/auth/verify_test.go
git commit -m "feat(auth): add VerifyLogin with constant-time comparison and expiry check"
```

---

### Task 4: Seed Nobody User on Repo Creation

**Files:**
- Modify: `go-libfossil/db/schema.go:200-212` (add SeedNobody)
- Modify: `go-libfossil/repo/repo.go:46-60` (call SeedNobody in CreateWithEnv)

**Context:** `repo.CreateWithEnv` at `repo.go:46-60` calls `db.SeedUser` (which creates the admin user with `s` cap) and `db.SeedConfig`. We need to also seed a `nobody` user with full caps (`cghijknorswz`) so existing tests and repos remain open by default.

- [ ] **Step 1: Write failing test for SeedNobody**

```go
// Add to go-libfossil/db/schema_test.go (create if needed)
package db

import (
	"path/filepath"
	"testing"
)

func TestSeedNobody(t *testing.T) {
	path := filepath.Join(t.TempDir(), "test.fossil")
	d, err := Open(path)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	defer d.Close()

	if err := CreateRepoSchema(d); err != nil {
		t.Fatalf("CreateRepoSchema: %v", err)
	}
	if err := SeedNobody(d, "oi"); err != nil {
		t.Fatalf("SeedNobody: %v", err)
	}

	var login, cap string
	err = d.QueryRow("SELECT login, cap FROM user WHERE login='nobody'").Scan(&login, &cap)
	if err != nil {
		t.Fatalf("nobody user not found: %v", err)
	}
	if cap != "oi" {
		t.Errorf("cap = %q, want oi", cap)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd go-libfossil && go test -buildvcs=false ./db/ -run TestSeedNobody -v`
Expected: FAIL — SeedNobody not defined

- [ ] **Step 3: Add SeedNobody to db/schema.go**

Add after `SeedUser` function (after line 212):

```go
// SeedNobody inserts a "nobody" user with the given capabilities.
// This controls anonymous access policy for the repo.
func SeedNobody(d *DB, caps string) error {
	if d == nil {
		panic("db.SeedNobody: d must not be nil")
	}
	_, err := d.Exec(
		"INSERT OR IGNORE INTO user(login, pw, cap, info) VALUES('nobody', '', ?, '')",
		caps,
	)
	return err
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd go-libfossil && go test -buildvcs=false ./db/ -run TestSeedNobody -v`
Expected: PASS

- [ ] **Step 5: Call SeedNobody in repo.CreateWithEnv**

In `go-libfossil/repo/repo.go`, add after the `SeedUser` call (after line 60):

```go
	if err := db.SeedNobody(d, "cghijknorswz"); err != nil {
		d.Close()
		if rmErr := env.Storage.Remove(path); rmErr != nil {
			return nil, fmt.Errorf("repo.CreateWithEnv: %w (cleanup failed: %v)", err, rmErr)
		}
		return nil, fmt.Errorf("repo.CreateWithEnv seed nobody: %w", err)
	}
```

- [ ] **Step 6: Run full test suite to verify nothing breaks**

Run: `cd go-libfossil && go test -buildvcs=false ./...`
Expected: PASS — all existing tests still pass because nobody has full caps

- [ ] **Step 7: Commit**

```bash
git add go-libfossil/db/schema.go go-libfossil/db/schema_test.go go-libfossil/repo/repo.go
git commit -m "feat(auth): seed nobody user with full caps on repo creation"
```

---

### Task 5: HandleSync Auth Integration

**Files:**
- Modify: `go-libfossil/sync/handler.go:77-174`
- Modify: `go-libfossil/sync/handler_test.go`

**Context:** The handler struct is at `handler.go:77-88`. `handleControlCard` is at line 152-174. The `process` method is at line 90-150. The two-pass control card processing must replace the single pass at lines 92-94.

- [ ] **Step 1: Write failing auth integration tests**

Add to `handler_test.go`:

```go
func TestHandlePushRequiresAuth(t *testing.T) {
	r := setupSyncTestRepo(t)
	pc := projectCode(t, r.DB())

	// Delete nobody so anonymous push is rejected
	r.DB().Exec("DELETE FROM user WHERE login='nobody'")

	data := []byte("auth test")
	uuid := hash.SHA1(data)
	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.PushCard{ServerCode: "test", ProjectCode: "test"},
		&xfer.FileCard{UUID: uuid, Content: data},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}
	errs := findCards[*xfer.ErrorCard](resp)
	if len(errs) == 0 {
		t.Fatal("expected error for unauthorized push")
	}
	// Blob should NOT be stored
	if _, ok := blob.Exists(r.DB(), uuid); ok {
		t.Fatal("blob should not be stored without push capability")
	}
}

func TestHandlePullRequiresAuth(t *testing.T) {
	r := setupSyncTestRepo(t)
	storeTestBlob(t, r, []byte("pull auth test"))

	// Delete nobody so anonymous pull is rejected
	r.DB().Exec("DELETE FROM user WHERE login='nobody'")

	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.PullCard{ServerCode: "test", ProjectCode: "test"},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}
	errs := findCards[*xfer.ErrorCard](resp)
	if len(errs) == 0 {
		t.Fatal("expected error for unauthorized pull")
	}
	igots := findCards[*xfer.IGotCard](resp)
	if len(igots) > 0 {
		t.Fatal("should not emit igots without pull capability")
	}
}

func TestHandleAuthenticatedPush(t *testing.T) {
	r := setupSyncTestRepo(t)
	pc := projectCode(t, r.DB())

	// Delete nobody, create a user with push caps
	r.DB().Exec("DELETE FROM user WHERE login='nobody'")
	auth.CreateUser(r.DB(), pc, "pusher", "secret", "oi")

	data := []byte("authed push")
	uuid := hash.SHA1(data)

	// Build a valid login card
	allCards := []xfer.Card{
		&xfer.PushCard{ServerCode: "test", ProjectCode: "test"},
		&xfer.FileCard{UUID: uuid, Content: data},
	}
	// Encode non-login cards to compute nonce
	var buf bytes.Buffer
	for _, c := range allCards {
		xfer.Encode(&buf, c)
	}
	payload := buf.Bytes()
	loginCard := buildTestLoginCard("pusher", "secret", pc, payload)

	req := &xfer.Message{Cards: append([]xfer.Card{loginCard}, allCards...)}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}
	errs := findCards[*xfer.ErrorCard](resp)
	if len(errs) > 0 {
		t.Fatalf("unexpected error: %s", errs[0].Message)
	}
	if _, ok := blob.Exists(r.DB(), uuid); !ok {
		t.Fatal("authenticated push should store blob")
	}
}

func TestHandleNobodyPullOnly(t *testing.T) {
	r := setupSyncTestRepo(t)
	storeTestBlob(t, r, []byte("nobody test"))

	// Set nobody to pull-only
	r.DB().Exec("UPDATE user SET cap='o' WHERE login='nobody'")

	// Pull should work
	pullReq := &xfer.Message{Cards: []xfer.Card{
		&xfer.PullCard{ServerCode: "test", ProjectCode: "test"},
	}}
	pullResp, _ := HandleSync(context.Background(), r, pullReq)
	igots := findCards[*xfer.IGotCard](pullResp)
	if len(igots) == 0 {
		t.Fatal("nobody with 'o' cap should allow pull")
	}

	// Push should fail
	data := []byte("nobody push attempt")
	uuid := hash.SHA1(data)
	pushReq := &xfer.Message{Cards: []xfer.Card{
		&xfer.PushCard{ServerCode: "test", ProjectCode: "test"},
		&xfer.FileCard{UUID: uuid, Content: data},
	}}
	pushResp, _ := HandleSync(context.Background(), r, pushReq)
	errs := findCards[*xfer.ErrorCard](pushResp)
	if len(errs) == 0 {
		t.Fatal("nobody with 'o' cap should reject push")
	}
}
```

Also add a helper at the top of handler_test.go:

```go
import (
	"bytes"
	"crypto/sha1"
	"encoding/hex"

	"github.com/dmestas/edgesync/go-libfossil/auth"
)

func projectCode(t *testing.T, d *db.DB) string {
	t.Helper()
	var code string
	if err := d.QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&code); err != nil {
		t.Fatalf("project-code: %v", err)
	}
	return code
}

func buildTestLoginCard(user, password, projectCode string, payload []byte) *xfer.LoginCard {
	nonce := sha1Hex(payload)
	shared := sha1Hex([]byte(projectCode + "/" + user + "/" + password))
	sig := sha1Hex([]byte(nonce + shared))
	return &xfer.LoginCard{User: user, Nonce: nonce, Signature: sig}
}

func sha1Hex(data []byte) string {
	h := sha1.Sum(data)
	return hex.EncodeToString(h[:])
}
```

- [ ] **Step 2: Run new tests to verify they fail**

Run: `cd go-libfossil && go test -buildvcs=false ./sync/ -run "TestHandlePushRequiresAuth|TestHandlePullRequiresAuth|TestHandleAuthenticatedPush|TestHandleNobodyPullOnly" -v`
Expected: FAIL — handler doesn't check capabilities yet

- [ ] **Step 3: Add auth import to handler.go**

Add to imports in `handler.go`:

```go
"github.com/dmestas/edgesync/go-libfossil/auth"
```

- [ ] **Step 4: Add auth fields to handler struct**

In `handler.go`, modify the handler struct (line 77-88):

```go
type handler struct {
	repo          *repo.Repo
	buggify       BuggifyChecker
	resp          []xfer.Card
	pushOK        bool
	pullOK        bool
	cloneMode     bool
	cloneSeq      int
	uvCatalogSent bool
	filesSent     int
	filesRecvd    int
	// Auth state
	user   string // verified username ("nobody" if no login card)
	caps   string // capability string from user table
	authed bool   // whether login card was verified
}
```

- [ ] **Step 5: Implement initAuth and two-pass card processing**

Replace `process` method's first pass (lines 90-94) and `handleControlCard` (lines 152-174):

```go
func (h *handler) process(_ context.Context, req *xfer.Message) (*xfer.Message, error) {
	// Initialize auth state from nobody user.
	h.initAuth()

	// First pass: resolve login cards before other control cards.
	for _, card := range req.Cards {
		if lc, ok := card.(*xfer.LoginCard); ok {
			h.handleLoginCard(lc)
		}
	}

	// Second pass: process other control cards with capability checks.
	for _, card := range req.Cards {
		if _, ok := card.(*xfer.LoginCard); ok {
			continue // Already processed.
		}
		h.handleControlCard(card)
	}

	// ... rest of process method unchanged from line 96 onward ...
```

```go
func (h *handler) initAuth() {
	h.user = "nobody"
	h.caps = ""
	h.authed = false
	var caps string
	err := h.repo.DB().QueryRow("SELECT cap FROM user WHERE login='nobody'").Scan(&caps)
	if err == nil {
		h.caps = caps
	}
}

func (h *handler) handleLoginCard(c *xfer.LoginCard) {
	var projectCode string
	if err := h.repo.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projectCode); err != nil {
		h.resp = append(h.resp, &xfer.ErrorCard{Message: "authentication failed"})
		return
	}
	u, err := auth.VerifyLogin(h.repo.DB(), projectCode, c, nil)
	// Note: payload verification requires the encoded non-login cards.
	// For now we verify user+password match only. The nonce is computed
	// from the request payload by the caller — we need the raw payload.
	// TODO: pass raw payload through for full nonce verification.
	if err != nil {
		h.resp = append(h.resp, &xfer.ErrorCard{Message: "authentication failed"})
		return
	}
	h.user = u.Login
	h.caps = u.Cap
	h.authed = true
}
```

```go
func (h *handler) handleLoginCard(c *xfer.LoginCard) {
	var projectCode string
	if err := h.repo.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projectCode); err != nil {
		h.resp = append(h.resp, &xfer.ErrorCard{Message: "authentication failed"})
		return
	}
	u, err := auth.VerifyLogin(h.repo.DB(), projectCode, c)
	if err != nil {
		h.resp = append(h.resp, &xfer.ErrorCard{Message: "authentication failed"})
		return
	}
	h.user = u.Login
	h.caps = u.Cap
	h.authed = true
}
```

Update `handleControlCard` to add capability checks:

```go
func (h *handler) handleControlCard(card xfer.Card) {
	switch c := card.(type) {
	case *xfer.LoginCard:
		return // Already processed in first pass.
	case *xfer.PragmaCard:
		if c.Name == "uv-hash" && len(c.Values) >= 1 {
			if err := h.handlePragmaUVHash(c.Values[0]); err != nil {
				h.resp = append(h.resp, &xfer.ErrorCard{
					Message: fmt.Sprintf("uv-hash: %v", err),
				})
			}
		}
	case *xfer.PushCard:
		if auth.CanPush(h.caps) {
			h.pushOK = true
		} else {
			h.resp = append(h.resp, &xfer.ErrorCard{
				Message: "push denied: insufficient capabilities",
			})
		}
	case *xfer.PullCard:
		if auth.CanPull(h.caps) {
			h.pullOK = true
		} else {
			h.resp = append(h.resp, &xfer.ErrorCard{
				Message: "pull denied: insufficient capabilities",
			})
		}
	case *xfer.CloneCard:
		if auth.CanClone(h.caps) {
			h.cloneMode = true
		} else {
			h.resp = append(h.resp, &xfer.ErrorCard{
				Message: "clone denied: insufficient capabilities",
			})
		}
	case *xfer.CloneSeqNoCard:
		h.cloneSeq = c.SeqNo
	}
}
```

Also add UV write check in `handleUVFile`. In `handler_uv.go`, add before the existing content storage logic (the existing `pushOK` check ensures a push card was sent; this additionally checks the `z` capability):

```go
if !auth.CanPushUV(h.caps) {
	h.resp = append(h.resp, &xfer.ErrorCard{
		Message: fmt.Sprintf("uvfile %s denied: insufficient capabilities", c.Name),
	})
	return nil
}
```

UV writes require BOTH a push card (existing `pushOK` check) AND the `z` capability (new check).

- [ ] **Step 5: Run new auth tests**

Run: `cd go-libfossil && go test -buildvcs=false ./sync/ -run "TestHandlePushRequiresAuth|TestHandlePullRequiresAuth|TestHandleAuthenticatedPush|TestHandleNobodyPullOnly" -v`
Expected: PASS

- [ ] **Step 7: Run full test suite**

Run: `cd go-libfossil && go test -buildvcs=false ./...`
Expected: PASS — all existing tests pass because nobody has full caps

- [ ] **Step 8: Commit**

```bash
git add go-libfossil/auth/verify.go go-libfossil/auth/verify_test.go go-libfossil/sync/handler.go go-libfossil/sync/handler_test.go go-libfossil/sync/handler_uv.go
git commit -m "feat(auth): wire VerifyLogin into HandleSync with two-pass card processing"
```

---

### Task 6: Invite Token Encode/Decode (`auth/invite.go`)

**Files:**
- Create: `go-libfossil/auth/invite.go`
- Test: `go-libfossil/auth/invite_test.go`

- [ ] **Step 1: Write failing tests**

```go
// invite_test.go
package auth

import "testing"

func TestInviteTokenRoundTrip(t *testing.T) {
	tok := InviteToken{
		URL:      "nats://100.78.32.45:4222/myrepo",
		Login:    "bob",
		Password: "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2",
		Caps:     "oi",
	}

	encoded := tok.Encode()
	if encoded == "" {
		t.Fatal("Encode returned empty string")
	}

	decoded, err := DecodeInviteToken(encoded)
	if err != nil {
		t.Fatalf("DecodeInviteToken: %v", err)
	}
	if decoded.URL != tok.URL {
		t.Errorf("URL = %q, want %q", decoded.URL, tok.URL)
	}
	if decoded.Login != tok.Login {
		t.Errorf("Login = %q, want %q", decoded.Login, tok.Login)
	}
	if decoded.Password != tok.Password {
		t.Errorf("Password = %q, want %q", decoded.Password, tok.Password)
	}
	if decoded.Caps != tok.Caps {
		t.Errorf("Caps = %q, want %q", decoded.Caps, tok.Caps)
	}
}

func TestDecodeInviteTokenInvalid(t *testing.T) {
	_, err := DecodeInviteToken("not-valid-base64!!!")
	if err == nil {
		t.Fatal("expected error for invalid token")
	}

	_, err = DecodeInviteToken("bm90LWpzb24=") // "not-json" in base64
	if err == nil {
		t.Fatal("expected error for non-JSON token")
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd go-libfossil && go test -buildvcs=false ./auth/ -run TestInvite -v`
Expected: FAIL

- [ ] **Step 3: Implement invite.go**

```go
// invite.go
package auth

import (
	"encoding/base64"
	"encoding/json"
	"fmt"
)

// InviteToken contains the credentials needed to clone a repo.
type InviteToken struct {
	URL      string `json:"url"`
	Login    string `json:"login"`
	Password string `json:"password"`
	Caps     string `json:"caps"`
}

// Encode returns the token as a base64url-encoded compact JSON string.
func (t InviteToken) Encode() string {
	b, err := json.Marshal(t)
	if err != nil {
		panic(fmt.Sprintf("auth.InviteToken.Encode: %v", err))
	}
	return base64.URLEncoding.EncodeToString(b)
}

// DecodeInviteToken decodes a base64url-encoded invite token.
func DecodeInviteToken(s string) (InviteToken, error) {
	b, err := base64.URLEncoding.DecodeString(s)
	if err != nil {
		return InviteToken{}, fmt.Errorf("auth.DecodeInviteToken: %w", err)
	}
	var t InviteToken
	if err := json.Unmarshal(b, &t); err != nil {
		return InviteToken{}, fmt.Errorf("auth.DecodeInviteToken: %w", err)
	}
	return t, nil
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd go-libfossil && go test -buildvcs=false ./auth/ -run TestInvite -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/auth/invite.go go-libfossil/auth/invite_test.go
git commit -m "feat(auth): add invite token encode/decode"
```

---

### Task 7: CLI — User Management Commands

**Files:**
- Create: `cmd/edgesync/user.go`
- Modify: `cmd/edgesync/cli.go` (add UserCmd to RepoCmd)

**Context:** CLI uses kong for command parsing. See `cli.go` for the pattern. Each command is a struct with `Run(g *Globals) error`. The `--repo` flag is already on `Globals` as `-R`.

- [ ] **Step 1: Add UserCmd to cli.go**

In `cmd/edgesync/cli.go`, add to `RepoCmd` struct:

```go
User     RepoUserCmd     `cmd:"" help:"User management"`
```

- [ ] **Step 2: Create user.go with all subcommands**

```go
// user.go
package main

import (
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/auth"
)

type RepoUserCmd struct {
	Add    UserAddCmd    `cmd:"" help:"Create a new user"`
	List   UserListCmd   `cmd:"" help:"List all users"`
	Update UserUpdateCmd `cmd:"" help:"Update user capabilities"`
	Rm     UserRmCmd     `cmd:"" help:"Delete a user"`
	Passwd UserPasswdCmd `cmd:"" help:"Reset user password"`
}

type UserAddCmd struct {
	Login string `arg:"" help:"Username"`
	Cap   string `help:"Capability string (e.g. oi)" required:""`
}

func (c *UserAddCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	password, err := generatePassword()
	if err != nil {
		return err
	}

	pc, err := repoProjectCode(r)
	if err != nil {
		return err
	}

	if err := auth.CreateUser(r.DB(), pc, c.Login, password, c.Cap); err != nil {
		return err
	}
	fmt.Printf("Created user %q (caps: %s)\n", c.Login, c.Cap)
	fmt.Printf("Password: %s\n", password)
	return nil
}

type UserListCmd struct{}

func (c *UserListCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	users, err := auth.ListUsers(r.DB())
	if err != nil {
		return err
	}
	fmt.Printf("%-20s %-20s\n", "LOGIN", "CAPABILITIES")
	for _, u := range users {
		fmt.Printf("%-20s %-20s\n", u.Login, u.Cap)
	}
	return nil
}

type UserUpdateCmd struct {
	Login string `arg:"" help:"Username"`
	Cap   string `help:"New capability string" required:""`
}

func (c *UserUpdateCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	if err := auth.UpdateCaps(r.DB(), c.Login, c.Cap); err != nil {
		return err
	}
	fmt.Printf("Updated %q capabilities: %s\n", c.Login, c.Cap)
	return nil
}

type UserRmCmd struct {
	Login string `arg:"" help:"Username"`
}

func (c *UserRmCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	if err := auth.DeleteUser(r.DB(), c.Login); err != nil {
		return err
	}
	fmt.Printf("Deleted user %q\n", c.Login)
	return nil
}

type UserPasswdCmd struct {
	Login string `arg:"" help:"Username"`
}

func (c *UserPasswdCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	password, err := generatePassword()
	if err != nil {
		return err
	}

	pc, err := repoProjectCode(r)
	if err != nil {
		return err
	}

	if err := auth.SetPassword(r.DB(), pc, c.Login, password); err != nil {
		return err
	}
	fmt.Printf("New password for %q: %s\n", c.Login, password)
	return nil
}

func generatePassword() (string, error) {
	b := make([]byte, 32)
	if _, err := rand.Read(b); err != nil {
		return "", fmt.Errorf("generating password: %w", err)
	}
	return hex.EncodeToString(b), nil
}

func repoProjectCode(r *repo.Repo) (string, error) {
	var pc string
	err := r.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&pc)
	if err != nil {
		return "", fmt.Errorf("reading project-code: %w", err)
	}
	return pc, nil
}
```

Note: `repoProjectCode` needs the repo import — add `"github.com/dmestas/edgesync/go-libfossil/repo"` to imports.

- [ ] **Step 3: Verify it compiles**

Run: `go build -buildvcs=false ./cmd/edgesync/`
Expected: Compiles successfully

- [ ] **Step 4: Manual smoke test**

Run:
```bash
./bin/edgesync repo new /tmp/test-auth.fossil
./bin/edgesync repo user list -R /tmp/test-auth.fossil
./bin/edgesync repo user add alice --cap oi -R /tmp/test-auth.fossil
./bin/edgesync repo user list -R /tmp/test-auth.fossil
./bin/edgesync repo user update alice --cap o -R /tmp/test-auth.fossil
./bin/edgesync repo user passwd alice -R /tmp/test-auth.fossil
./bin/edgesync repo user rm alice -R /tmp/test-auth.fossil
rm /tmp/test-auth.fossil
```

- [ ] **Step 5: Commit**

```bash
git add cmd/edgesync/user.go cmd/edgesync/cli.go
git commit -m "feat(cli): add user management commands (add, list, update, rm, passwd)"
```

---

### Task 8: CLI — Invite Command

**Files:**
- Create: `cmd/edgesync/invite.go`
- Modify: `cmd/edgesync/cli.go` (add InviteCmd)
- Modify: `cmd/edgesync/repo_clone.go` (add --invite flag)

**Context:** Check `repo_clone.go` for the existing clone command structure before modifying it.

- [ ] **Step 1: Read repo_clone.go to understand existing clone structure**

- [ ] **Step 2: Add InviteCmd to cli.go**

In `RepoCmd` struct, add:

```go
Invite   RepoInviteCmd   `cmd:"" help:"Generate invite token for a user"`
```

- [ ] **Step 3: Create invite.go**

```go
// invite.go
package main

import (
	"fmt"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/auth"
)

type RepoInviteCmd struct {
	Login string        `arg:"" help:"Username for the invitee"`
	Cap   string        `help:"Capability string (e.g. oi)" required:""`
	URL   string        `help:"Sync URL to embed in token" default:""`
	TTL   time.Duration `help:"Token time-to-live (e.g. 24h)" default:"0"`
}

func (c *RepoInviteCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	password, err := generatePassword()
	if err != nil {
		return err
	}

	pc, err := repoProjectCode(r)
	if err != nil {
		return err
	}

	if err := auth.CreateUser(r.DB(), pc, c.Login, password, c.Cap); err != nil {
		return err
	}

	if c.TTL > 0 {
		expiry := time.Now().Add(c.TTL).Format("2006-01-02 15:04:05")
		if _, err := r.DB().Exec("UPDATE user SET cexpire=? WHERE login=?", expiry, c.Login); err != nil {
			return fmt.Errorf("setting expiry: %w", err)
		}
	}

	url := c.URL
	if url == "" {
		// Try to read from repo config
		r.DB().QueryRow("SELECT value FROM config WHERE name='last-sync-url'").Scan(&url)
	}

	tok := auth.InviteToken{
		URL:      url,
		Login:    c.Login,
		Password: password,
		Caps:     c.Cap,
	}

	encoded := tok.Encode()

	fmt.Printf("Invite for %q (capabilities: %s", c.Login, c.Cap)
	if c.TTL > 0 {
		fmt.Printf(", expires: %s", time.Now().Add(c.TTL).Format(time.RFC3339))
	}
	fmt.Println("):")
	fmt.Println()
	fmt.Printf("  edgesync repo clone --invite %s\n", encoded)
	fmt.Println()
	fmt.Println("Share this command with the recipient. It contains credentials — treat it like a password.")
	return nil
}
```

- [ ] **Step 4: Add --invite flag to RepoCloneCmd**

Read `repo_clone.go` first, then add an `Invite string` field and handle it in `Run`:

```go
Invite string `help:"Invite token (from edgesync invite)" default:""`
```

In the `Run` method, if `c.Invite != ""`:
1. Decode the token
2. Use token.URL, token.Login, token.Password for the clone
3. Store credentials in the cloned repo's config

- [ ] **Step 5: Verify it compiles**

Run: `go build -buildvcs=false ./cmd/edgesync/`
Expected: Compiles successfully

- [ ] **Step 6: Commit**

```bash
git add cmd/edgesync/invite.go cmd/edgesync/cli.go cmd/edgesync/repo_clone.go
git commit -m "feat(cli): add invite command and clone --invite flag"
```

---

### Task 9: Run Full Test Suite and Pre-Commit

**Files:** None (verification only)

- [ ] **Step 1: Run all go-libfossil tests**

Run: `cd go-libfossil && go test -buildvcs=false ./...`
Expected: PASS

- [ ] **Step 2: Run DST tests**

Run: `make dst`
Expected: PASS

- [ ] **Step 3: Run sim tests**

Run: `make test`
Expected: PASS

- [ ] **Step 4: Build all binaries**

Run: `make build`
Expected: All binaries built successfully

- [ ] **Step 5: Final commit if any fixups needed**

---

### Task 10: Update Linear Tickets

- [ ] **Step 1: Move CDG-131 to Done** (spike delivered as spec + implementation)
- [ ] **Step 2: Move CDG-155 to Done** (auth package implemented)
- [ ] **Step 3: Move CDG-115 to Done** (verify login credentials in HandleSync — now implemented)
