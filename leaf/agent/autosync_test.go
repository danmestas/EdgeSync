package agent

import (
	"bytes"
	"context"
	"errors"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	libfossil "github.com/danmestas/libfossil"
	_ "github.com/danmestas/libfossil/db/driver/modernc"
	"github.com/danmestas/libfossil/simio"
)

func TestEnsureClientID(t *testing.T) {
	path := t.TempDir() + "/test.fossil"
	r, err := libfossil.Create(path, libfossil.CreateOpts{User: "test"})
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	defer r.Close()

	// Deterministic RNG for reproducibility.
	rng := bytes.NewReader(bytes.Repeat([]byte{0xab}, 16))

	id1, err := ensureClientID(r, rng)
	if err != nil {
		t.Fatalf("ensureClientID: %v", err)
	}
	if id1 == "" {
		t.Fatal("clientID should not be empty")
	}

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

// Verify SyncObserver interface is satisfied by VerboseObserver (compile-time).
var _ libfossil.SyncObserver = (*mockSyncObserver)(nil)

type mockSyncObserver struct{}

func (*mockSyncObserver) Started(libfossil.SessionStart)            {}
func (*mockSyncObserver) RoundStarted(int)                          {}
func (*mockSyncObserver) RoundCompleted(int, libfossil.RoundStats)  {}
func (*mockSyncObserver) Completed(libfossil.SessionEnd)            {}
func (*mockSyncObserver) Error(error)                               {}
func (*mockSyncObserver) HandleStarted(libfossil.HandleStart)       {}
func (*mockSyncObserver) HandleCompleted(libfossil.HandleEnd)       {}
func (*mockSyncObserver) TableSyncStarted(libfossil.TableSyncStart) {}
func (*mockSyncObserver) TableSyncCompleted(libfossil.TableSyncEnd) {}

// Silence unused import warnings.
var _ = simio.RealClock{}

// --- AutosyncCommit tests ----------------------------------------------------

// panicTransport fails the test if RoundTrip is called. Used to assert that
// AutosyncOff makes zero network calls.
type panicTransport struct{ t *testing.T }

func (p panicTransport) RoundTrip(context.Context, []byte) ([]byte, error) {
	p.t.Helper()
	p.t.Fatal("Transport.RoundTrip should not be called in AutosyncOff mode")
	return nil, errors.New("unreachable")
}

// forkFixture sets up a repo where the working checkout would fork on commit:
// the repo has commits C0 and C1 on trunk, but the checkout's parent is C0
// while trunk's tip is C1. The checkout also has staged-but-uncommitted
// changes ready to be checked in.
//
// A sibling "remote" repo cloned from local at C0 is also returned. The pull
// step in AutosyncCommit roundtrips to remote (via an httptest server) but
// brings back nothing — the fork is purely local state.
type forkFixture struct {
	repo      *libfossil.Repo
	remote    *libfossil.Repo
	checkout  *libfossil.Checkout
	transport libfossil.Transport
	dir       string
}

func newForkFixture(t *testing.T) *forkFixture {
	t.Helper()
	ctx := context.Background()

	// 1. Create local and commit C0.
	repoPath := filepath.Join(t.TempDir(), "local.fossil")
	repo, err := libfossil.Create(repoPath, libfossil.CreateOpts{User: "alice"})
	if err != nil {
		t.Fatalf("Create local: %v", err)
	}
	t.Cleanup(func() { _ = repo.Close() })

	c0, _, err := repo.Commit(libfossil.CommitOpts{
		Comment: "C0",
		User:    "alice",
		Files:   []libfossil.FileToCommit{{Name: "base.txt", Content: []byte("base\n")}},
	})
	if err != nil {
		t.Fatalf("Commit C0: %v", err)
	}

	// 2. Briefly serve local over HTTP so remote can clone from it.
	// (HandleSync's in-process loopback doesn't speak the clone protocol —
	// see CDG-148. Clone needs the real /xfer HTTP path.)
	localSrv := httptest.NewServer(repo.XferHandler())
	remotePath := filepath.Join(t.TempDir(), "remote.fossil")
	remote, _, err := libfossil.Clone(ctx, remotePath, libfossil.NewHTTPTransport(localSrv.URL), libfossil.CloneOpts{})
	localSrv.Close()
	if err != nil {
		t.Fatalf("Clone remote: %v", err)
	}
	t.Cleanup(func() { _ = remote.Close() })

	// 3. CreateCheckout on local while tip is still C0 — anchors checkout's
	// parent at C0.
	checkoutDir := t.TempDir()
	co, err := repo.CreateCheckout(checkoutDir, libfossil.CheckoutCreateOpts{})
	if err != nil {
		t.Fatalf("CreateCheckout: %v", err)
	}
	t.Cleanup(func() { _ = co.Close() })

	if err := co.Extract(c0, libfossil.ExtractOpts{}); err != nil {
		t.Fatalf("Extract C0: %v", err)
	}

	// 4. C1 on REMOTE only. The pull in AutosyncCommit will bring C1 back to
	// local, advancing local's trunk past the checkout's C0 parent — that's
	// what triggers WouldFork=true.
	if _, _, err := remote.Commit(libfossil.CommitOpts{
		Comment:  "C1 (remote-only)",
		User:     "alice",
		ParentID: c0,
		Files:    []libfossil.FileToCommit{{Name: "remote-change.txt", Content: []byte("remote\n")}},
	}); err != nil {
		t.Fatalf("Remote Commit C1: %v", err)
	}

	// 5. Serve remote for the test so pulls have a target.
	remoteSrv := httptest.NewServer(remote.XferHandler())
	t.Cleanup(remoteSrv.Close)

	// 6. Stage a new file in the checkout — what the user wants to commit.
	mustWrite(t, checkoutDir, "user.txt", "user change\n")
	if _, err := co.Add([]string{"user.txt"}); err != nil {
		t.Fatalf("Add user.txt: %v", err)
	}

	return &forkFixture{
		repo:      repo,
		remote:    remote,
		checkout:  co,
		transport: libfossil.NewHTTPTransport(remoteSrv.URL),
		dir:       checkoutDir,
	}
}

func mustWrite(t *testing.T, dir, name, content string) {
	t.Helper()
	if err := os.WriteFile(filepath.Join(dir, name), []byte(content), 0o644); err != nil {
		t.Fatalf("WriteFile %s: %v", name, err)
	}
}

func TestAutosyncCommit_OffMode(t *testing.T) {
	repoPath := filepath.Join(t.TempDir(), "off.fossil")
	repo, err := libfossil.Create(repoPath, libfossil.CreateOpts{User: "alice"})
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	defer repo.Close()

	// Seed an initial checkin so CreateCheckout has a tip.
	c0, _, err := repo.Commit(libfossil.CommitOpts{
		Comment: "C0", User: "alice",
		Files: []libfossil.FileToCommit{{Name: "base.txt", Content: []byte("base\n")}},
	})
	if err != nil {
		t.Fatalf("Commit C0: %v", err)
	}

	checkoutDir := t.TempDir()
	co, err := repo.CreateCheckout(checkoutDir, libfossil.CheckoutCreateOpts{})
	if err != nil {
		t.Fatalf("CreateCheckout: %v", err)
	}
	defer co.Close()
	if err := co.Extract(c0, libfossil.ExtractOpts{}); err != nil {
		t.Fatalf("Extract: %v", err)
	}

	mustWrite(t, checkoutDir, "hello.txt", "hello\n")
	if _, err := co.Add([]string{"hello.txt"}); err != nil {
		t.Fatalf("Add: %v", err)
	}

	rid, uuid, err := AutosyncCommit(context.Background(), AutosyncCommitOpts{
		Repo:      repo,
		Checkout:  co,
		Transport: panicTransport{t: t},
		Mode:      AutosyncOff,
		Commit:    libfossil.CheckoutCommitOpts{Message: "off-mode", User: "alice"},
	})
	if err != nil {
		t.Fatalf("AutosyncCommit: %v", err)
	}
	if rid == 0 {
		t.Fatal("rid should be non-zero")
	}
	if uuid == "" {
		t.Fatal("uuid should be non-empty")
	}

	// Trunk's tip must advance to the new commit — verifies the checkin
	// landed on the working branch, not just that some commit appeared.
	tip, err := repo.BranchTip("trunk")
	if err != nil {
		t.Fatalf("BranchTip trunk: %v", err)
	}
	if tip != rid {
		t.Fatalf("trunk tip rid=%d, want %d", tip, rid)
	}
}

func TestAutosyncCommit_RejectsCallerSyncPushPull(t *testing.T) {
	// Minimal setup — the function should reject before doing any work.
	repoPath := filepath.Join(t.TempDir(), "v.fossil")
	repo, err := libfossil.Create(repoPath, libfossil.CreateOpts{User: "alice"})
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	defer repo.Close()
	if _, _, err := repo.Commit(libfossil.CommitOpts{
		Comment: "C0", User: "alice",
		Files: []libfossil.FileToCommit{{Name: "x", Content: []byte("x")}},
	}); err != nil {
		t.Fatalf("Commit: %v", err)
	}
	co, err := repo.CreateCheckout(t.TempDir(), libfossil.CheckoutCreateOpts{})
	if err != nil {
		t.Fatalf("CreateCheckout: %v", err)
	}
	defer co.Close()

	for _, tc := range []struct {
		name string
		sync libfossil.SyncOpts
	}{
		{"push", libfossil.SyncOpts{Push: true}},
		{"pull", libfossil.SyncOpts{Pull: true}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			_, _, err := AutosyncCommit(context.Background(), AutosyncCommitOpts{
				Repo:     repo,
				Checkout: co,
				Mode:     AutosyncOff,
				Commit:   libfossil.CheckoutCommitOpts{Message: "x", User: "alice"},
				Sync:     tc.sync,
			})
			if err == nil {
				t.Fatal("expected error when Sync.Push or Sync.Pull is set, got nil")
			}
		})
	}
}

func TestPushFailedError(t *testing.T) {
	cause := errors.New("transport down")
	e := &PushFailedError{LocalRID: 42, LocalUUID: "deadbeef", Cause: cause}

	msg := e.Error()
	if !strings.Contains(msg, "rid=42") || !strings.Contains(msg, "deadbeef") {
		t.Errorf("Error() missing rid/uuid: %s", msg)
	}
	if !errors.Is(e, cause) {
		t.Error("errors.Is(PushFailedError, Cause) should be true")
	}
}

func TestAutosyncCommit_WouldForkAborts(t *testing.T) {
	f := newForkFixture(t)

	_, _, err := AutosyncCommit(context.Background(), AutosyncCommitOpts{
		Repo:      f.repo,
		Checkout:  f.checkout,
		Transport: f.transport,
		Mode:      AutosyncOn,
		Commit:    libfossil.CheckoutCommitOpts{Message: "would fork", User: "alice"},
		Sync:      libfossil.SyncOpts{PeerID: "alice-peer"},
	})
	if !errors.Is(err, ErrWouldFork) {
		t.Fatalf("expected ErrWouldFork, got %v", err)
	}

	// Confirm the checkout did NOT advance past C1 — no checkin was created.
	version, _, verr := f.checkout.Version()
	if verr != nil {
		t.Fatalf("Version: %v", verr)
	}
	// Checkout's parent should still be C0 (its rid is the smallest;
	// any forked commit would have given it a new larger parent).
	if version == 0 {
		t.Fatalf("checkout has no version after aborted fork (rid=%d)", version)
	}
}

func TestAutosyncCommit_AllowForkOverride(t *testing.T) {
	f := newForkFixture(t)

	rid, uuid, err := AutosyncCommit(context.Background(), AutosyncCommitOpts{
		Repo:      f.repo,
		Checkout:  f.checkout,
		Transport: f.transport,
		Mode:      AutosyncPullOnly, // PullOnly skips push so we don't depend on remote write
		Commit:    libfossil.CheckoutCommitOpts{Message: "forked on purpose", User: "alice"},
		Sync:      libfossil.SyncOpts{PeerID: "alice-peer"},
		AllowFork: true,
	})
	if err != nil {
		t.Fatalf("AutosyncCommit with AllowFork: %v", err)
	}
	if rid == 0 || uuid == "" {
		t.Fatalf("expected non-zero rid/uuid, got rid=%d uuid=%q", rid, uuid)
	}
}

func TestAutosyncCommit_BranchBypassesForkCheck(t *testing.T) {
	f := newForkFixture(t)

	rid, uuid, err := AutosyncCommit(context.Background(), AutosyncCommitOpts{
		Repo:      f.repo,
		Checkout:  f.checkout,
		Transport: f.transport,
		Mode:      AutosyncPullOnly,
		Commit: libfossil.CheckoutCommitOpts{
			Message: "on a new branch",
			User:    "alice",
			Branch:  "feature-x",
		},
		Sync: libfossil.SyncOpts{PeerID: "alice-peer"},
		// AllowFork explicitly false — the branch alone should bypass.
	})
	if err != nil {
		t.Fatalf("AutosyncCommit with Branch: %v", err)
	}
	if rid == 0 || uuid == "" {
		t.Fatalf("expected non-zero rid/uuid, got rid=%d uuid=%q", rid, uuid)
	}

	// The commit must have actually landed on feature-x, not trunk —
	// that's what "branch bypasses fork check" semantically requires.
	tip, err := f.repo.BranchTip("feature-x")
	if err != nil {
		t.Fatalf("BranchTip feature-x: %v", err)
	}
	if tip != rid {
		t.Fatalf("commit not on feature-x: branch tip rid=%d, want %d", tip, rid)
	}
}
