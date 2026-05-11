package hub

import (
	"bytes"
	"context"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"slices"
	"testing"
	"time"

	_ "github.com/danmestas/libfossil/db/driver/modernc"
	"github.com/nats-io/nats.go"
)

func newTestHub(t *testing.T) *Hub {
	t.Helper()
	dir := t.TempDir()
	h, err := NewHub(context.Background(), Config{
		RepoPath: filepath.Join(dir, "hub.fossil"),
	})
	if err != nil {
		t.Fatalf("NewHub: %v", err)
	}
	t.Cleanup(func() { _ = h.Stop() })
	return h
}

func TestNewHub_BootstrapAndAddresses(t *testing.T) {
	h := newTestHub(t)

	if h.HTTPAddr() == "" {
		t.Error("HTTPAddr empty after NewHub")
	}
	if h.NATSURL() == "" {
		t.Error("NATSURL empty after NewHub")
	}
	if h.LeafURL() == "" {
		t.Error("LeafURL empty after NewHub")
	}
}

func TestNewHub_AutoPortsAreDistinct(t *testing.T) {
	h := newTestHub(t)

	natsURL := h.NATSURL()         // nats://127.0.0.1:NNNN
	leafURL := h.LeafURL()         // nats-leaf://127.0.0.1:LLLL
	httpAddr := h.HTTPAddr()       // 127.0.0.1:HHHH

	if natsURL == leafURL || natsURL == httpAddr || leafURL == httpAddr {
		t.Errorf("expected three distinct addresses, got NATS=%s leaf=%s http=%s", natsURL, leafURL, httpAddr)
	}
}

func TestHub_NATSURL_Connects(t *testing.T) {
	h := newTestHub(t)

	conn, err := nats.Connect(h.NATSURL(), nats.Timeout(2*time.Second))
	if err != nil {
		t.Fatalf("nats.Connect(%s): %v", h.NATSURL(), err)
	}
	defer conn.Close()
	if !conn.IsConnected() {
		t.Error("nats connection not established")
	}
}

func TestHub_OpenExistingRepo(t *testing.T) {
	dir := t.TempDir()
	repoPath := filepath.Join(dir, "hub.fossil")

	h1, err := NewHub(context.Background(), Config{RepoPath: repoPath})
	if err != nil {
		t.Fatalf("NewHub (bootstrap): %v", err)
	}
	if _, err := h1.Commit(context.Background(), CommitOpts{
		Files:   []FileToCommit{{Name: "f", Content: []byte("v1")}},
		Message: "first",
		Author:  "hub",
	}); err != nil {
		t.Fatalf("Commit: %v", err)
	}
	_ = h1.Stop()

	h2, err := NewHub(context.Background(), Config{RepoPath: repoPath})
	if err != nil {
		t.Fatalf("NewHub (open): %v", err)
	}
	defer h2.Stop()

	got, err := h2.Read(context.Background(), "f")
	if err != nil {
		t.Fatalf("Read after reopen: %v", err)
	}
	if !bytes.Equal(got, []byte("v1")) {
		t.Errorf("Read = %q, want %q", got, "v1")
	}
}

func TestHub_CommitReadReadAtRoundtrip(t *testing.T) {
	h := newTestHub(t)
	ctx := context.Background()

	rev, err := h.Commit(ctx, CommitOpts{
		Files: []FileToCommit{
			{Name: "a.txt", Content: []byte("alpha")},
			{Name: "dir/b.txt", Content: []byte("beta")},
		},
		Message: "initial",
		Author:  "hub",
	})
	if err != nil {
		t.Fatalf("Commit: %v", err)
	}

	got, err := h.Read(ctx, "a.txt")
	if err != nil {
		t.Fatalf("Read: %v", err)
	}
	if !bytes.Equal(got, []byte("alpha")) {
		t.Errorf("Read a.txt = %q, want %q", got, "alpha")
	}

	gotRev, err := h.ReadAt(ctx, rev, "dir/b.txt")
	if err != nil {
		t.Fatalf("ReadAt: %v", err)
	}
	if !bytes.Equal(gotRev, []byte("beta")) {
		t.Errorf("ReadAt dir/b.txt = %q, want %q", gotRev, "beta")
	}

	gotTrunk, err := h.ReadAt(ctx, "trunk", "a.txt")
	if err != nil {
		t.Fatalf("ReadAt trunk: %v", err)
	}
	if !bytes.Equal(gotTrunk, []byte("alpha")) {
		t.Errorf("ReadAt trunk a.txt = %q, want %q", gotTrunk, "alpha")
	}
}

func TestHub_Commit_RejectsEmptyAuthor(t *testing.T) {
	h := newTestHub(t)
	if _, err := h.Commit(context.Background(), CommitOpts{
		Files:   []FileToCommit{{Name: "x", Content: []byte("y")}},
		Message: "no author",
	}); err == nil {
		t.Fatal("Commit with empty Author returned nil")
	}
}

func TestHub_ReadAt_RejectsEmptyRev(t *testing.T) {
	h := newTestHub(t)
	if _, err := h.ReadAt(context.Background(), "", "any"); err == nil {
		t.Fatal("ReadAt with empty rev returned nil")
	}
}

func TestHub_UserMgmt_RoundTrip(t *testing.T) {
	h := newTestHub(t)

	if err := h.AddUser(User{Login: "alice", Caps: "admin"}); err != nil {
		t.Fatalf("AddUser: %v", err)
	}
	if !h.HasUser("alice") {
		t.Error("HasUser(alice) = false after AddUser")
	}
	if h.HasUser("definitely-not-a-real-user-zzz") {
		t.Error("HasUser(definitely-not-a-real-user-zzz) = true; want false")
	}

	got, err := h.GetUser("alice")
	if err != nil {
		t.Fatalf("GetUser: %v", err)
	}
	if got.Login != "alice" || got.Caps != "admin" {
		t.Errorf("GetUser alice = %+v, want {alice admin}", got)
	}

	users, err := h.ListUsers()
	if err != nil {
		t.Fatalf("ListUsers: %v", err)
	}
	hasAlice := slices.ContainsFunc(users, func(u User) bool { return u.Login == "alice" })
	if !hasAlice {
		t.Errorf("ListUsers = %+v, want one with Login=alice", users)
	}

	if err := h.RemoveUser("alice"); err != nil {
		t.Fatalf("RemoveUser: %v", err)
	}
	if h.HasUser("alice") {
		t.Error("HasUser(alice) = true after RemoveUser")
	}
}

func TestHub_AddUser_RejectsEmptyLogin(t *testing.T) {
	h := newTestHub(t)
	if err := h.AddUser(User{}); err == nil {
		t.Fatal("AddUser with empty Login returned nil")
	}
}

func TestHub_ServeHTTP_RespondsAndShutsDownOnContextCancel(t *testing.T) {
	h := newTestHub(t)

	ctx, cancel := context.WithCancel(context.Background())
	serveDone := make(chan error, 1)
	go func() { serveDone <- h.ServeHTTP(ctx) }()

	// Wait for server to come up.
	deadline := time.Now().Add(2 * time.Second)
	var resp *http.Response
	var err error
	for time.Now().Before(deadline) {
		resp, err = http.Get("http://" + h.HTTPAddr() + "/")
		if err == nil {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	if err != nil {
		t.Fatalf("HTTP GET %s: %v", h.HTTPAddr(), err)
	}
	resp.Body.Close()
	if resp.StatusCode == 0 {
		t.Errorf("HTTP GET = status %d", resp.StatusCode)
	}

	cancel()
	select {
	case err := <-serveDone:
		if err != nil {
			t.Errorf("ServeHTTP returned error: %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Error("ServeHTTP did not shut down within 2s of ctx cancel")
	}
}

func TestHub_Stop_IsIdempotent(t *testing.T) {
	dir := t.TempDir()
	h, err := NewHub(context.Background(), Config{
		RepoPath: filepath.Join(dir, "hub.fossil"),
	})
	if err != nil {
		t.Fatalf("NewHub: %v", err)
	}

	if err := h.Stop(); err != nil {
		t.Errorf("first Stop: %v", err)
	}
	if err := h.Stop(); err != nil {
		t.Errorf("second Stop: %v", err)
	}
}

// TestHub_StopLeavesRepoVanillaReadable asserts the contract that after
// Hub.Stop returns nil, hub.fossil is a self-contained file: no WAL/SHM
// sidecar required, and a fresh hub.OpenRepo against the path succeeds.
// This is the behavior consumers like bones rely on for vanilla-fossil
// interop. Regression for #143.
func TestHub_StopLeavesRepoVanillaReadable(t *testing.T) {
	dir := t.TempDir()
	repoPath := filepath.Join(dir, "hub.fossil")

	h, err := NewHub(context.Background(), Config{RepoPath: repoPath})
	if err != nil {
		t.Fatalf("NewHub: %v", err)
	}
	if _, err := h.Commit(context.Background(), CommitOpts{
		Files:   []FileToCommit{{Name: "f", Content: bytes.Repeat([]byte("x"), 8192)}},
		Message: "trigger WAL traffic",
		Author:  "hub",
	}); err != nil {
		t.Fatalf("Commit: %v", err)
	}
	if err := h.Stop(); err != nil {
		t.Fatalf("Stop: %v", err)
	}

	walPath := repoPath + "-wal"
	if info, err := os.Stat(walPath); err == nil && info.Size() != 0 {
		t.Errorf("WAL still present with size %d after Stop; expected 0 or absent", info.Size())
	}

	r, err := OpenRepo(repoPath)
	if err != nil {
		t.Fatalf("OpenRepo on stopped hub.fossil: %v", err)
	}
	t.Cleanup(func() { _ = r.Close() })

	got, err := r.Read(context.Background(), "f")
	if err != nil {
		t.Fatalf("Read after reopen: %v", err)
	}
	if !bytes.Equal(got, bytes.Repeat([]byte("x"), 8192)) {
		t.Errorf("Read after reopen: content mismatch (len=%d)", len(got))
	}
}

// TestHub_PeriodicCheckpointPopulatesMainFile asserts the periodic PASSIVE
// checkpoint goroutine runs while the hub serves: after several commits and
// a few ticker periods, the main hub.fossil file holds more than the SQLite
// stub. Without periodic checkpointing the main file stays at the stub size
// (~4096 bytes) until SQLite's own auto-checkpoint at ~4 MiB fires, and
// vanilla fossil rejects it. Regression for #143.
func TestHub_PeriodicCheckpointPopulatesMainFile(t *testing.T) {
	dir := t.TempDir()
	repoPath := filepath.Join(dir, "hub.fossil")

	h, err := NewHub(context.Background(), Config{
		RepoPath:           repoPath,
		CheckpointInterval: 20 * time.Millisecond,
	})
	if err != nil {
		t.Fatalf("NewHub: %v", err)
	}
	t.Cleanup(func() { _ = h.Stop() })

	for i := range 10 {
		if _, err := h.Commit(context.Background(), CommitOpts{
			Files:   []FileToCommit{{Name: fmt.Sprintf("f%d", i), Content: bytes.Repeat([]byte("x"), 4096)}},
			Message: fmt.Sprintf("commit %d", i),
			Author:  "hub",
		}); err != nil {
			t.Fatalf("Commit %d: %v", i, err)
		}
	}

	time.Sleep(200 * time.Millisecond)

	info, err := os.Stat(repoPath)
	if err != nil {
		t.Fatalf("Stat %s: %v", repoPath, err)
	}
	if info.Size() <= 4096 {
		t.Errorf("main file size = %d after 10 commits + 200ms; expected > 4096 (WAL not being checkpointed mid-flight)", info.Size())
	}
}
