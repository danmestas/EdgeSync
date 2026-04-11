# EdgeSync Notify Phase 2 — Pairing CLI Commands

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `edgesync notify pair`, `notify unpair`, and `notify devices` CLI commands. These let a hub generate one-time pairing tokens (displayed as QR + text), validate them from a connecting device, and manage a persistent device registry in `notify.fossil`.

**Architecture:** A `leaf/agent/notify/pair.go` file provides token generation, validation, and device registry CRUD — all as free functions operating on `*libfossil.Repo` (consistent with Phase 1's store.go pattern). Tokens are 12-char base32 (no ambiguous chars), single-use, 10-minute expiry. The device registry is a JSON config file committed to `notify.fossil`. CLI commands in `cmd/edgesync/notify.go` wire it together via Kong. QR codes are rendered to the terminal using `github.com/skip2/go-qrcode`.

**Tech Stack:** Go 1.26, go-libfossil v0.2.x, Kong CLI, `github.com/skip2/go-qrcode`, stdlib `crypto/rand`, `crypto/sha256`, `encoding/base32`

**Spec:** User-provided requirements (no separate spec document).

**No Claude attribution on EdgeSync PRs.**

---

## File Structure

### New Files

| File | Responsibility |
|------|---------------|
| `leaf/agent/notify/pair.go` | Token generation (`GenerateToken`), token hashing (`HashToken`), token validation (`ValidateToken`), device registry CRUD (`AddDevice`, `RemoveDevice`, `ListDevices`), QR payload formatting |
| `leaf/agent/notify/pair_test.go` | Unit tests for all pairing functions |

### Modified Files

| File | Change |
|------|--------|
| `cmd/edgesync/notify.go` | Add `PairCmd`, `UnpairCmd`, `DevicesCmd` structs and `Run` methods |
| `cmd/edgesync/notify_test.go` | Add CLI end-to-end tests for pair/unpair/devices |
| `leaf/go.mod` | Add `github.com/skip2/go-qrcode` dependency |

---

## Data Structures

### Token format

The text token is 12 alphanumeric characters in groups of 4 separated by dashes: `AXKF-9M2P-VR3T`. Generated from 8 random bytes encoded with a custom base32 alphabet that excludes ambiguous characters (0, O, 1, I, l).

```go
// Alphabet: 2-9, A-H, J-K, M, N, P-T, V-Z (32 chars, no 0/O/1/I/l)
const tokenAlphabet = "23456789ABCDEFGHJKMNPQRSTVWXYZ"
// Wait — that's 29 chars. Use a proper 32-char set:
const tokenAlphabet = "23456789ABCDEFGHJKMNPQRSTVWXYZ+="
// Actually, keep it simple: use crypto/rand to pick 12 chars from the safe set.
```

Concrete approach: generate 12 random characters from a 29-char safe alphabet (`23456789ABCDEFGHJKMNPQRSTVWXYZ`). This gives ~58 bits of entropy — more than enough for a 10-minute single-use token. Format with dashes for display: `XXXX-XXXX-XXXX`.

### QR payload

```
edgesync-pair://v1/<hub-iroh-endpoint-id>/<nats-addr>/<one-time-secret>
```

- `hub-iroh-endpoint-id`: the hub's iroh `EndpointId` (from sidecar or config)
- `nats-addr`: the NATS address the device should connect to (e.g. `nats://100.78.32.45:4222`)
- `one-time-secret`: the raw 12-char token (no dashes)

### PendingToken (in-memory or committed to repo)

```go
type PendingToken struct {
    TokenHash  string    `json:"token_hash"`  // SHA-256 hex of raw token
    CreatedAt  time.Time `json:"created_at"`
    ExpiresAt  time.Time `json:"expires_at"`
    DeviceName string    `json:"device_name"` // name provided at pair time
}
```

Stored as `_notify/pending_tokens.json` in the notify repo (committed so it survives restarts). Expired/used tokens are pruned on each operation.

### Device (registry entry)

```go
type Device struct {
    Name       string    `json:"name"`
    EndpointID string    `json:"endpoint_id"` // iroh EndpointId of the paired device
    PairedAt   time.Time `json:"paired_at"`
}
```

Stored as `_notify/devices.json` in the notify repo — a JSON array of Device structs, committed on each mutation.

### DeviceRegistry (the full file)

```go
type DeviceRegistry struct {
    Devices []Device `json:"devices"`
}
```

---

## Task 1: Token Generation and Hashing

**Files:**
- Create: `leaf/agent/notify/pair.go`
- Create: `leaf/agent/notify/pair_test.go`

- [ ] **Step 1: Write failing test for token generation**

Create `leaf/agent/notify/pair_test.go`:

```go
package notify

import (
    "strings"
    "testing"
)

func TestGenerateToken(t *testing.T) {
    tok, err := GenerateToken()
    if err != nil {
        t.Fatal(err)
    }

    // Format: XXXX-XXXX-XXXX
    if len(tok) != 14 { // 12 chars + 2 dashes
        t.Errorf("token length = %d, want 14", len(tok))
    }
    parts := strings.Split(tok, "-")
    if len(parts) != 3 {
        t.Fatalf("token parts = %d, want 3", len(parts))
    }
    for i, p := range parts {
        if len(p) != 4 {
            t.Errorf("part[%d] length = %d, want 4", i, len(p))
        }
    }

    // No ambiguous characters.
    raw := strings.ReplaceAll(tok, "-", "")
    for _, c := range raw {
        switch c {
        case '0', 'O', '1', 'I', 'l':
            t.Errorf("token contains ambiguous char %q", string(c))
        }
    }

    // Two tokens should differ.
    tok2, _ := GenerateToken()
    if tok == tok2 {
        t.Error("two generated tokens should not be equal")
    }
}
```

- [ ] **Step 2: Implement `GenerateToken` and `HashToken`**

Create `leaf/agent/notify/pair.go`:

```go
package notify

import (
    "crypto/rand"
    "crypto/sha256"
    "fmt"
    "strings"
    "time"
)

// Safe alphabet: 29 chars, no 0/O/1/I/l.
const tokenAlphabet = "23456789ABCDEFGHJKMNPQRSTVWXYZ"

// GenerateToken creates a 12-char alphanumeric token formatted as XXXX-XXXX-XXXX.
func GenerateToken() (string, error) {
    b := make([]byte, 12)
    if _, err := rand.Read(b); err != nil {
        return "", fmt.Errorf("generate token: %w", err)
    }
    chars := make([]byte, 12)
    for i, v := range b {
        chars[i] = tokenAlphabet[int(v)%len(tokenAlphabet)]
    }
    return fmt.Sprintf("%s-%s-%s", string(chars[0:4]), string(chars[4:8]), string(chars[8:12])), nil
}

// RawToken strips dashes from a formatted token.
func RawToken(formatted string) string {
    return strings.ReplaceAll(formatted, "-", "")
}

// HashToken returns the hex-encoded SHA-256 hash of the raw (no dashes) token.
func HashToken(formatted string) string {
    raw := RawToken(formatted)
    h := sha256.Sum256([]byte(raw))
    return fmt.Sprintf("%x", h)
}
```

- [ ] **Step 3: Write and pass test for `HashToken`**

Add to `pair_test.go`:

```go
func TestHashToken(t *testing.T) {
    tok := "AXKF-9M2P-VR3T"
    h := HashToken(tok)

    if len(h) != 64 { // SHA-256 hex
        t.Errorf("hash length = %d, want 64", len(h))
    }

    // Same input = same hash.
    if HashToken(tok) != h {
        t.Error("hash should be deterministic")
    }

    // Different input = different hash.
    if HashToken("ZZZZ-ZZZZ-ZZZZ") == h {
        t.Error("different tokens should produce different hashes")
    }
}

func TestRawToken(t *testing.T) {
    if got := RawToken("AXKF-9M2P-VR3T"); got != "AXKF9M2PVR3T" {
        t.Errorf("RawToken = %q, want %q", got, "AXKF9M2PVR3T")
    }
}
```

**Verify:** `cd /Users/dmestas/projects/EdgeSync && go test ./leaf/agent/notify/ -run TestGenerate -v && go test ./leaf/agent/notify/ -run TestHash -v && go test ./leaf/agent/notify/ -run TestRaw -v`

---

## Task 2: QR Payload Formatting

**Files:**
- Modify: `leaf/agent/notify/pair.go`
- Modify: `leaf/agent/notify/pair_test.go`
- Modify: `leaf/go.mod` (add `github.com/skip2/go-qrcode`)

- [ ] **Step 1: Write failing test for QR payload**

Add to `pair_test.go`:

```go
func TestFormatPairURL(t *testing.T) {
    url := FormatPairURL("abc123endpointid", "nats://100.78.32.45:4222", "AXKF-9M2P-VR3T")
    want := "edgesync-pair://v1/abc123endpointid/nats%3A%2F%2F100.78.32.45%3A4222/AXKF9M2PVR3T"
    if url != want {
        t.Errorf("FormatPairURL =\n  %q\nwant:\n  %q", url, want)
    }
}
```

- [ ] **Step 2: Implement `FormatPairURL` and `ParsePairURL`**

Add to `pair.go`:

```go
import "net/url"

// FormatPairURL builds the QR payload string.
// The NATS address is URL-encoded to avoid slash ambiguity.
func FormatPairURL(endpointID, natsAddr, token string) string {
    return fmt.Sprintf("edgesync-pair://v1/%s/%s/%s",
        endpointID,
        url.PathEscape(natsAddr),
        RawToken(token),
    )
}

// PairInfo is the parsed content of a pairing URL.
type PairInfo struct {
    EndpointID string
    NATSAddr   string
    Token      string // raw, no dashes
}

// ParsePairURL parses an edgesync-pair:// URL into its components.
func ParsePairURL(raw string) (PairInfo, error) {
    // ...parse and validate...
}
```

- [ ] **Step 3: Add `go-qrcode` dependency and implement `RenderQR`**

```bash
cd /Users/dmestas/projects/EdgeSync/leaf && go get github.com/skip2/go-qrcode
```

Add to `pair.go`:

```go
import qrcode "github.com/skip2/go-qrcode"

// RenderQR returns a terminal-friendly QR code string for the given content.
func RenderQR(content string) (string, error) {
    qr, err := qrcode.New(content, qrcode.Medium)
    if err != nil {
        return "", fmt.Errorf("generate QR: %w", err)
    }
    return qr.ToSmallString(false), nil
}
```

- [ ] **Step 4: Write and pass test for `ParsePairURL` round-trip**

```go
func TestPairURLRoundTrip(t *testing.T) {
    url := FormatPairURL("abc123endpointid", "nats://100.78.32.45:4222", "AXKF-9M2P-VR3T")
    info, err := ParsePairURL(url)
    if err != nil {
        t.Fatal(err)
    }
    if info.EndpointID != "abc123endpointid" {
        t.Errorf("EndpointID = %q", info.EndpointID)
    }
    if info.NATSAddr != "nats://100.78.32.45:4222" {
        t.Errorf("NATSAddr = %q", info.NATSAddr)
    }
    if info.Token != "AXKF9M2PVR3T" {
        t.Errorf("Token = %q", info.Token)
    }
}
```

**Verify:** `cd /Users/dmestas/projects/EdgeSync && go test ./leaf/agent/notify/ -run TestPairURL -v && go test ./leaf/agent/notify/ -run TestFormatPair -v`

---

## Task 3: Device Registry — CRUD on notify.fossil

**Files:**
- Modify: `leaf/agent/notify/pair.go`
- Modify: `leaf/agent/notify/pair_test.go`

The device registry is a JSON file at `_notify/devices.json` in the notify repo. It uses the same `CommitMessage`-style pattern from `store.go` — read the current file, mutate, commit the new version.

- [ ] **Step 1: Write failing tests for `AddDevice`, `ListDevices`, `RemoveDevice`**

```go
func TestDeviceRegistryCRUD(t *testing.T) {
    r := createTestNotifyRepo(t) // helper: creates a temp notify.fossil
    defer r.Close()

    // Initially empty.
    devices, err := ListDevices(r)
    if err != nil {
        t.Fatal(err)
    }
    if len(devices) != 0 {
        t.Fatalf("expected 0 devices, got %d", len(devices))
    }

    // Add a device.
    dev := Device{
        Name:       "dan-iphone",
        EndpointID: "iroh-endpoint-abc123",
        PairedAt:   time.Now().UTC().Truncate(time.Second),
    }
    if err := AddDevice(r, dev); err != nil {
        t.Fatal(err)
    }

    devices, err = ListDevices(r)
    if err != nil {
        t.Fatal(err)
    }
    if len(devices) != 1 {
        t.Fatalf("expected 1 device, got %d", len(devices))
    }
    if devices[0].Name != "dan-iphone" {
        t.Errorf("name = %q", devices[0].Name)
    }

    // Duplicate name rejected.
    if err := AddDevice(r, dev); err == nil {
        t.Error("expected error for duplicate device name")
    }

    // Remove.
    if err := RemoveDevice(r, "dan-iphone"); err != nil {
        t.Fatal(err)
    }

    devices, err = ListDevices(r)
    if err != nil {
        t.Fatal(err)
    }
    if len(devices) != 0 {
        t.Fatalf("expected 0 devices after remove, got %d", len(devices))
    }

    // Remove non-existent = error.
    if err := RemoveDevice(r, "nonexistent"); err == nil {
        t.Error("expected error for removing non-existent device")
    }
}
```

- [ ] **Step 2: Implement `ListDevices`, `AddDevice`, `RemoveDevice`**

```go
const devicesFilePath = "_notify/devices.json"

// ListDevices reads the device registry from the notify repo.
// Returns empty slice (not error) if the file doesn't exist yet.
func ListDevices(r *libfossil.Repo) ([]Device, error) {
    // Read _notify/devices.json via readFileContent (from store.go)
    // If not found, return empty slice.
    // Otherwise unmarshal DeviceRegistry and return .Devices.
}

// AddDevice adds a device to the registry. Rejects duplicate names.
func AddDevice(r *libfossil.Repo, dev Device) error {
    // Read current, check for duplicate name, append, commit.
}

// RemoveDevice removes a device by name. Returns error if not found.
func RemoveDevice(r *libfossil.Repo, name string) error {
    // Read current, find by name, remove, commit.
}
```

The commit pattern mirrors `CommitMessage` from `store.go` — serialize JSON, call `r.Commit()` with a `FileToCommit`.

- [ ] **Step 3: Implement `createTestNotifyRepo` helper**

```go
func createTestNotifyRepo(t *testing.T) *libfossil.Repo {
    t.Helper()
    path := filepath.Join(t.TempDir(), "notify.fossil")
    r, err := InitNotifyRepo(path)
    if err != nil {
        t.Fatal(err)
    }
    return r
}
```

**Verify:** `cd /Users/dmestas/projects/EdgeSync && go test ./leaf/agent/notify/ -run TestDeviceRegistry -v`

---

## Task 4: Pending Token Storage and Validation

**Files:**
- Modify: `leaf/agent/notify/pair.go`
- Modify: `leaf/agent/notify/pair_test.go`

Pending tokens are stored as `_notify/pending_tokens.json` in the notify repo. Each token stores only the hash. Expired tokens are pruned on every read.

- [ ] **Step 1: Write failing tests for `StorePendingToken`, `ValidateToken`**

```go
func TestPendingTokenLifecycle(t *testing.T) {
    r := createTestNotifyRepo(t)
    defer r.Close()

    tok, _ := GenerateToken()

    // Store the token.
    err := StorePendingToken(r, PendingToken{
        TokenHash:  HashToken(tok),
        DeviceName: "dan-iphone",
        CreatedAt:  time.Now().UTC(),
        ExpiresAt:  time.Now().UTC().Add(10 * time.Minute),
    })
    if err != nil {
        t.Fatal(err)
    }

    // Validate with correct token.
    pt, err := ValidateToken(r, tok)
    if err != nil {
        t.Fatal(err)
    }
    if pt.DeviceName != "dan-iphone" {
        t.Errorf("device name = %q", pt.DeviceName)
    }

    // Token is consumed — second validation fails.
    _, err = ValidateToken(r, tok)
    if err == nil {
        t.Error("expected error: token already consumed")
    }
}

func TestExpiredTokenRejected(t *testing.T) {
    r := createTestNotifyRepo(t)
    defer r.Close()

    tok, _ := GenerateToken()

    err := StorePendingToken(r, PendingToken{
        TokenHash:  HashToken(tok),
        DeviceName: "dan-iphone",
        CreatedAt:  time.Now().UTC().Add(-20 * time.Minute),
        ExpiresAt:  time.Now().UTC().Add(-10 * time.Minute), // already expired
    })
    if err != nil {
        t.Fatal(err)
    }

    _, err = ValidateToken(r, tok)
    if err == nil {
        t.Error("expected error for expired token")
    }
}
```

- [ ] **Step 2: Implement `StorePendingToken` and `ValidateToken`**

```go
const pendingTokensFilePath = "_notify/pending_tokens.json"

// StorePendingToken adds a pending token to the repo. Prunes expired tokens.
func StorePendingToken(r *libfossil.Repo, pt PendingToken) error {
    // Read existing, prune expired, append new, commit.
}

// ValidateToken checks a raw token against pending tokens.
// On success, removes the token (single-use) and returns the PendingToken metadata.
// Returns error if token is invalid, expired, or already consumed.
func ValidateToken(r *libfossil.Repo, formattedToken string) (PendingToken, error) {
    // Read pending, hash the input, find match, check expiry, remove, commit.
}
```

**Verify:** `cd /Users/dmestas/projects/EdgeSync && go test ./leaf/agent/notify/ -run TestPendingToken -v && go test ./leaf/agent/notify/ -run TestExpiredToken -v`

---

## Task 5: CLI Commands — `pair`, `unpair`, `devices`

**Files:**
- Modify: `cmd/edgesync/notify.go`
- Modify: `cmd/edgesync/notify_test.go`

- [ ] **Step 1: Add command structs to `NotifyCmd`**

In `cmd/edgesync/notify.go`, add three fields to `NotifyCmd`:

```go
type NotifyCmd struct {
    Init    NotifyInitCmd    `cmd:"" help:"Initialize the notify repo"`
    Send    NotifySendCmd    `cmd:"" help:"Send a notification message"`
    Ask     NotifyAskCmd     `cmd:"" help:"Send a message and wait for a reply"`
    Watch   NotifyWatchCmd   `cmd:"" help:"Watch for incoming messages"`
    Threads NotifyThreadsCmd `cmd:"" help:"List active threads"`
    Log     NotifyLogCmd     `cmd:"" help:"Show thread message history"`
    Status  NotifyStatusCmd  `cmd:"" help:"Show connection state and unread counts"`
    Pair    NotifyPairCmd    `cmd:"" help:"Generate a one-time pairing token and QR code"`
    Unpair  NotifyUnpairCmd  `cmd:"" help:"Revoke a paired device"`
    Devices NotifyDevicesCmd `cmd:"" help:"List paired devices"`
}
```

- [ ] **Step 2: Implement `NotifyPairCmd`**

```go
type NotifyPairCmd struct {
    Name     string `help:"Device name for the pairing" required:""`
    NATS     string `help:"NATS server URL for the QR payload" env:"EDGESYNC_NATS"`
    Endpoint string `help:"Hub iroh endpoint ID for the QR payload" env:"EDGESYNC_IROH_ENDPOINT"`
}

func (c *NotifyPairCmd) Run(g *cli.Globals) error {
    if g.Repo == "" {
        return fmt.Errorf("repository required (use -R <path>)")
    }
    r, err := openNotifyRepo(g)
    if err != nil {
        return err
    }
    defer r.Close()

    // Generate token.
    tok, err := notify.GenerateToken()
    if err != nil {
        return err
    }

    // Store pending token (hash only).
    err = notify.StorePendingToken(r, notify.PendingToken{
        TokenHash:  notify.HashToken(tok),
        DeviceName: c.Name,
        CreatedAt:  time.Now().UTC(),
        ExpiresAt:  time.Now().UTC().Add(10 * time.Minute),
    })
    if err != nil {
        return err
    }

    // Build QR payload (only if endpoint + NATS provided).
    if c.Endpoint != "" && c.NATS != "" {
        pairURL := notify.FormatPairURL(c.Endpoint, c.NATS, tok)
        qr, err := notify.RenderQR(pairURL)
        if err != nil {
            return err
        }
        fmt.Print(qr)
        fmt.Fprintf(os.Stderr, "pair-url:%s\n", pairURL)
    }

    // Always print the text token.
    fmt.Println(tok)
    fmt.Fprintf(os.Stderr, "expires: 10 minutes\n")
    fmt.Fprintf(os.Stderr, "device: %s\n", c.Name)
    return nil
}
```

- [ ] **Step 3: Implement `NotifyUnpairCmd`**

```go
type NotifyUnpairCmd struct {
    Name string `arg:"" help:"Device name to unpair"`
}

func (c *NotifyUnpairCmd) Run(g *cli.Globals) error {
    if g.Repo == "" {
        return fmt.Errorf("repository required (use -R <path>)")
    }
    r, err := openNotifyRepo(g)
    if err != nil {
        return err
    }
    defer r.Close()

    if err := notify.RemoveDevice(r, c.Name); err != nil {
        return err
    }

    fmt.Fprintf(os.Stderr, "unpaired: %s\n", c.Name)
    return nil
}
```

- [ ] **Step 4: Implement `NotifyDevicesCmd`**

```go
type NotifyDevicesCmd struct{}

func (c *NotifyDevicesCmd) Run(g *cli.Globals) error {
    if g.Repo == "" {
        return fmt.Errorf("repository required (use -R <path>)")
    }
    r, err := openNotifyRepo(g)
    if err != nil {
        return err
    }
    defer r.Close()

    devices, err := notify.ListDevices(r)
    if err != nil {
        return err
    }

    if len(devices) == 0 {
        fmt.Println("no paired devices")
        return nil
    }

    for _, d := range devices {
        fmt.Printf("%s  endpoint:%s  paired:%s\n",
            d.Name, d.EndpointID, d.PairedAt.UTC().Format(time.RFC3339))
    }
    return nil
}
```

**Verify:** `cd /Users/dmestas/projects/EdgeSync && go build -buildvcs=false ./cmd/edgesync/`

---

## Task 6: CLI End-to-End Tests

**Files:**
- Modify: `cmd/edgesync/notify_test.go`

- [ ] **Step 1: Add `TestNotifyCLIPairAndDevices`**

```go
func TestNotifyCLIPairAndDevices(t *testing.T) {
    bin := buildBinary(t)
    tmp := t.TempDir()
    fakeRepo := filepath.Join(tmp, "project.fossil")

    // Init.
    cmd := exec.Command(bin, "-R", fakeRepo, "notify", "init")
    out, err := cmd.CombinedOutput()
    if err != nil {
        t.Fatalf("init: %s\n%s", err, out)
    }

    // Pair (no endpoint/NATS = text token only, no QR).
    cmd = exec.Command(bin, "-R", fakeRepo, "notify", "pair", "--name", "dan-iphone")
    out, err = cmd.CombinedOutput()
    if err != nil {
        t.Fatalf("pair: %s\n%s", err, out)
    }
    pairOutput := string(out)

    // Should contain a token in XXXX-XXXX-XXXX format.
    if !strings.Contains(pairOutput, "-") {
        t.Errorf("pair output should contain token with dashes, got: %q", pairOutput)
    }
    if !strings.Contains(pairOutput, "expires") {
        t.Errorf("pair output should mention expiry, got: %q", pairOutput)
    }

    // Devices — should be empty (token pending, not yet validated).
    cmd = exec.Command(bin, "-R", fakeRepo, "notify", "devices")
    out, err = cmd.CombinedOutput()
    if err != nil {
        t.Fatalf("devices: %s\n%s", err, out)
    }
    if !strings.Contains(string(out), "no paired devices") {
        t.Errorf("devices should be empty before validation, got: %q", string(out))
    }
}
```

- [ ] **Step 2: Add `TestNotifyCLIUnpair`**

```go
func TestNotifyCLIUnpair(t *testing.T) {
    bin := buildBinary(t)
    tmp := t.TempDir()
    fakeRepo := filepath.Join(tmp, "project.fossil")

    // Init.
    cmd := exec.Command(bin, "-R", fakeRepo, "notify", "init")
    if out, err := cmd.CombinedOutput(); err != nil {
        t.Fatalf("init: %s\n%s", err, out)
    }

    // Unpair a device that doesn't exist = error.
    cmd = exec.Command(bin, "-R", fakeRepo, "notify", "unpair", "nonexistent")
    if out, err := cmd.CombinedOutput(); err == nil {
        t.Fatalf("unpair nonexistent should fail, got: %s", out)
    }
}
```

**Verify:** `cd /Users/dmestas/projects/EdgeSync && go test -buildvcs=false ./cmd/edgesync/ -run TestNotifyCLIPair -v && go test -buildvcs=false ./cmd/edgesync/ -run TestNotifyCLIUnpair -v`

---

## Task 7: Full Test Suite Pass

- [ ] **Step 1: Run all notify package tests**

```bash
cd /Users/dmestas/projects/EdgeSync && go test ./leaf/agent/notify/ -v
```

- [ ] **Step 2: Run all CLI tests**

```bash
cd /Users/dmestas/projects/EdgeSync && go test -buildvcs=false ./cmd/edgesync/ -v
```

- [ ] **Step 3: Run full CI test suite**

```bash
cd /Users/dmestas/projects/EdgeSync && make test
```

- [ ] **Step 4: Build all binaries**

```bash
cd /Users/dmestas/projects/EdgeSync && make build
```
