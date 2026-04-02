package agent

import (
	"bytes"
	"context"
	"errors"
	"os"
	"path/filepath"
	"testing"
	"time"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/checkout"
	_ "github.com/dmestas/edgesync/go-libfossil/db/driver/modernc"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	libsync "github.com/dmestas/edgesync/go-libfossil/sync"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

// newTestCheckout creates a repo with an initial checkin, opens a checkout,
// and returns both. Cleanup is registered via t.Cleanup.
func newTestCheckout(t *testing.T) (*repo.Repo, *checkout.Checkout) {
	t.Helper()
	dir := t.TempDir()
	repoPath := filepath.Join(dir, "test.fossil")
	r, err := repo.Create(repoPath, "test", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("repo.Create: %v", err)
	}
	t.Cleanup(func() { r.Close() })

	// Create initial checkin so the checkout has a parent.
	_, _, err = manifest.Checkin(r, manifest.CheckinOpts{
		Files: []manifest.File{
			{Name: "hello.txt", Content: []byte("hello world\n")},
		},
		Comment: "initial checkin",
		User:    "test",
		Parent:  0,
		Time:    time.Date(2026, 1, 1, 0, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("manifest.Checkin: %v", err)
	}

	coDir := filepath.Join(dir, "co")
	co, err := checkout.Create(r, coDir, checkout.CreateOpts{})
	if err != nil {
		t.Fatalf("checkout.Create: %v", err)
	}
	t.Cleanup(func() { co.Close() })

	// Extract files so the checkout is populated.
	rid, _, err := co.Version()
	if err != nil {
		t.Fatalf("co.Version: %v", err)
	}
	if err := co.Extract(rid, checkout.ExtractOpts{}); err != nil {
		t.Fatalf("co.Extract: %v", err)
	}

	return r, co
}

func nopTransport() libsync.Transport {
	return &libsync.MockTransport{
		Handler: func(req *xfer.Message) *xfer.Message {
			return &xfer.Message{}
		},
	}
}

func TestAutosyncCommit_OffMode(t *testing.T) {
	_, co := newTestCheckout(t)

	// Write a new file so there's something to commit.
	if err := os.WriteFile(filepath.Join(co.Dir(), "new.txt"), []byte("new"), 0644); err != nil {
		t.Fatal(err)
	}

	rid, uuid, err := AutosyncCommit(context.Background(), co,
		checkout.CommitOpts{Message: "off mode commit", User: "test"},
		AutosyncOpts{Mode: AutosyncOff},
	)
	if err != nil {
		t.Fatalf("AutosyncCommit: %v", err)
	}
	if rid == 0 {
		t.Fatal("rid should not be 0")
	}
	if uuid == "" {
		t.Fatal("uuid should not be empty")
	}
}

func TestAutosyncCommit_WouldForkAborts(t *testing.T) {
	r, co := newTestCheckout(t)

	// Create a second commit to move the checkout forward.
	if err := os.WriteFile(filepath.Join(co.Dir(), "a.txt"), []byte("a"), 0644); err != nil {
		t.Fatal(err)
	}
	rid1, _, err := co.Commit(checkout.CommitOpts{Message: "second", User: "test"})
	if err != nil {
		t.Fatalf("Commit: %v", err)
	}

	// Re-extract so checkout is at the new commit.
	if err := co.Extract(rid1, checkout.ExtractOpts{}); err != nil {
		t.Fatalf("Extract: %v", err)
	}

	// Get the initial checkin RID (the event before rid1) and re-insert as leaf.
	var origRID int64
	err = r.DB().QueryRow("SELECT min(objid) FROM event WHERE objid != ?", int64(rid1)).Scan(&origRID)
	if err != nil {
		t.Fatalf("find original RID: %v", err)
	}
	if _, err := r.DB().Exec("INSERT OR IGNORE INTO leaf(rid) VALUES(?)", origRID); err != nil {
		t.Fatalf("insert fake leaf: %v", err)
	}

	// Write a file so commit has changes.
	if err := os.WriteFile(filepath.Join(co.Dir(), "b.txt"), []byte("b"), 0644); err != nil {
		t.Fatal(err)
	}

	_, _, err = AutosyncCommit(context.Background(), co,
		checkout.CommitOpts{Message: "should fail", User: "test"},
		AutosyncOpts{
			Mode:      AutosyncOn,
			Transport: nopTransport(),
			ClientID:  "test-client",
		},
	)
	if !errors.Is(err, ErrWouldFork) {
		t.Fatalf("expected ErrWouldFork, got: %v", err)
	}
}

func TestAutosyncCommit_AllowForkOverride(t *testing.T) {
	r, co := newTestCheckout(t)

	// Create second commit and move checkout forward.
	if err := os.WriteFile(filepath.Join(co.Dir(), "a.txt"), []byte("a"), 0644); err != nil {
		t.Fatal(err)
	}
	rid1, _, err := co.Commit(checkout.CommitOpts{Message: "second", User: "test"})
	if err != nil {
		t.Fatalf("Commit: %v", err)
	}
	if err := co.Extract(rid1, checkout.ExtractOpts{}); err != nil {
		t.Fatalf("Extract: %v", err)
	}

	// Insert fake leaf to create fork condition.
	var origRID int64
	err = r.DB().QueryRow("SELECT min(objid) FROM event").Scan(&origRID)
	if err != nil {
		t.Fatalf("find original RID: %v", err)
	}
	if _, err := r.DB().Exec("INSERT OR IGNORE INTO leaf(rid) VALUES(?)", origRID); err != nil {
		t.Fatalf("insert fake leaf: %v", err)
	}

	// Write a file so commit has changes.
	if err := os.WriteFile(filepath.Join(co.Dir(), "c.txt"), []byte("c"), 0644); err != nil {
		t.Fatal(err)
	}

	rid, uuid, err := AutosyncCommit(context.Background(), co,
		checkout.CommitOpts{Message: "allow fork", User: "test"},
		AutosyncOpts{
			Mode:      AutosyncOn,
			Transport: nopTransport(),
			AllowFork: true,
			ClientID:  "test-client",
		},
	)
	if err != nil {
		t.Fatalf("AutosyncCommit: %v", err)
	}
	if rid == 0 {
		t.Fatal("rid should not be 0")
	}
	if uuid == "" {
		t.Fatal("uuid should not be empty")
	}
}

func TestAutosyncCommit_BranchBypassesForkCheck(t *testing.T) {
	r, co := newTestCheckout(t)

	// Create second commit and move checkout forward.
	if err := os.WriteFile(filepath.Join(co.Dir(), "a.txt"), []byte("a"), 0644); err != nil {
		t.Fatal(err)
	}
	rid1, _, err := co.Commit(checkout.CommitOpts{Message: "second", User: "test"})
	if err != nil {
		t.Fatalf("Commit: %v", err)
	}
	if err := co.Extract(rid1, checkout.ExtractOpts{}); err != nil {
		t.Fatalf("Extract: %v", err)
	}

	// Insert fake leaf to create fork condition.
	var origRID int64
	err = r.DB().QueryRow("SELECT min(objid) FROM event").Scan(&origRID)
	if err != nil {
		t.Fatalf("find original RID: %v", err)
	}
	if _, err := r.DB().Exec("INSERT OR IGNORE INTO leaf(rid) VALUES(?)", origRID); err != nil {
		t.Fatalf("insert fake leaf: %v", err)
	}

	// Write a file so commit has changes.
	if err := os.WriteFile(filepath.Join(co.Dir(), "d.txt"), []byte("d"), 0644); err != nil {
		t.Fatal(err)
	}

	rid, uuid, err := AutosyncCommit(context.Background(), co,
		checkout.CommitOpts{
			Message: "branch bypass",
			User:    "test",
			Branch:  "feature-x",
		},
		AutosyncOpts{
			Mode:      AutosyncOn,
			Transport: nopTransport(),
			ClientID:  "test-client",
		},
	)
	if err != nil {
		t.Fatalf("AutosyncCommit: %v", err)
	}
	if rid == 0 {
		t.Fatal("rid should not be 0")
	}
	if uuid == "" {
		t.Fatal("uuid should not be empty")
	}
}

func TestEnsureClientID(t *testing.T) {
	dir := t.TempDir()
	repoPath := filepath.Join(dir, "test.fossil")
	r, err := repo.Create(repoPath, "test", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("repo.Create: %v", err)
	}
	defer r.Close()

	// Use deterministic RNG for reproducibility.
	rng := bytes.NewReader(bytes.Repeat([]byte{0xab}, 16))

	id1, err := ensureClientID(r, rng)
	if err != nil {
		t.Fatalf("ensureClientID: %v", err)
	}
	if id1 == "" {
		t.Fatal("clientID should not be empty")
	}
	t.Logf("generated clientID: %s", id1)

	// Second call should return the same ID (from DB, not generate new).
	id2, err := ensureClientID(r, rng)
	if err != nil {
		t.Fatalf("ensureClientID (second): %v", err)
	}
	if id2 != id1 {
		t.Fatalf("clientID changed: %q -> %q", id1, id2)
	}
}

// Verify FslID is used correctly (compile-time check).
var _ libfossil.FslID
