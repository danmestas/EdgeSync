package hub

import (
	"bytes"
	"context"
	"net"
	"path/filepath"
	"testing"
	"time"

	libfossil "github.com/danmestas/libfossil"
	_ "github.com/danmestas/libfossil/db/driver/modernc"
)

// sharedProjectCode is a fixed 40-char lowercase hex string used as the
// project-code across the two hubs in cross-leaf tests. Format matches
// libfossil's validation (^[0-9a-f]{40}$).
const sharedProjectCode = "1111111111111111111111111111111111111111"

// TestCrossLeaf_SharedProjectCode_PropagatesCommit asserts the headline
// fix for issue #156: two hubs with a shared project-code, linked over a
// NATS leafnode connection, see each other's commits via the
// .commit → pull-on-notify flow.
func TestCrossLeaf_SharedProjectCode_PropagatesCommit(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	dirA := t.TempDir()
	hubA, err := NewHub(ctx, Config{
		RepoPath:    filepath.Join(dirA, "hubA.fossil"),
		ProjectCode: sharedProjectCode,
		NobodyCaps:  "gio", // allow unauthenticated xfer
	})
	if err != nil {
		t.Fatalf("NewHub A: %v", err)
	}
	t.Cleanup(func() { _ = hubA.Stop() })

	httpCtxA, httpCancelA := context.WithCancel(context.Background())
	t.Cleanup(httpCancelA)
	go func() { _ = hubA.ServeHTTP(httpCtxA) }()

	dirB := t.TempDir()
	hubB, err := NewHub(ctx, Config{
		RepoPath:     filepath.Join(dirB, "hubB.fossil"),
		ProjectCode:  sharedProjectCode,
		LeafUpstream: hubA.LeafURL(),
		NobodyCaps:   "gio",
	})
	if err != nil {
		t.Fatalf("NewHub B: %v", err)
	}
	t.Cleanup(func() { _ = hubB.Stop() })

	httpCtxB, httpCancelB := context.WithCancel(context.Background())
	t.Cleanup(httpCancelB)
	go func() { _ = hubB.ServeHTTP(httpCtxB) }()

	waitFor(t, 5*time.Second, "leaf link A↔B", func() bool {
		return hubA.NumLeafs() >= 1
	})

	// Commit on A, expect B to see the file via pull-on-notify.
	if _, err := hubA.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "from-a.txt", Content: []byte("hello-from-A")}},
		Message: "commit on A",
		Author:  "hub",
	}); err != nil {
		t.Fatalf("hubA.Commit: %v", err)
	}

	waitFor(t, 10*time.Second, "B sees from-a.txt at trunk", func() bool {
		got, readErr := hubB.Read(ctx, "from-a.txt")
		return readErr == nil && bytes.Equal(got, []byte("hello-from-A"))
	})

	// Reverse direction: commit on B, expect A to see it.
	if _, err := hubB.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "from-b.txt", Content: []byte("hello-from-B")}},
		Message: "commit on B",
		Author:  "hub",
	}); err != nil {
		t.Fatalf("hubB.Commit: %v", err)
	}

	waitFor(t, 10*time.Second, "A sees from-b.txt at trunk", func() bool {
		got, readErr := hubA.Read(ctx, "from-b.txt")
		return readErr == nil && bytes.Equal(got, []byte("hello-from-B"))
	})
}

// TestCrossLeaf_SeedFromUpstream_PropagatesCommit covers the other half
// of the issue #156 fix: hub B bootstraps via SeedFromUpstream pointing at
// hub A's HTTP xfer endpoint, inheriting A's project-code through the
// clone. Once leaf-linked, A's commits should propagate to B over NATS
// sync, same as the shared-ProjectCode path.
func TestCrossLeaf_SeedFromUpstream_PropagatesCommit(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	dirA := t.TempDir()
	hubA, err := NewHub(ctx, Config{
		RepoPath:    filepath.Join(dirA, "hubA.fossil"),
		ProjectCode: sharedProjectCode,
		NobodyCaps:  "gio", // allow unauthenticated clone
	})
	if err != nil {
		t.Fatalf("NewHub A: %v", err)
	}
	t.Cleanup(func() { _ = hubA.Stop() })

	httpCtxA, httpCancelA := context.WithCancel(context.Background())
	t.Cleanup(httpCancelA)
	go func() { _ = hubA.ServeHTTP(httpCtxA) }()

	// Bootstrap B by cloning from A's HTTP xfer endpoint. Deliberately
	// omit ProjectCode on B — the whole point of SeedFromUpstream is that
	// the project-code rides in via the clone.
	dirB := t.TempDir()
	hubB, err := NewHub(ctx, Config{
		RepoPath:         filepath.Join(dirB, "hubB.fossil"),
		SeedFromUpstream: "http://" + hubA.HTTPAddr() + "/",
		LeafUpstream:     hubA.LeafURL(),
		NobodyCaps:       "gio",
	})
	if err != nil {
		t.Fatalf("NewHub B: %v", err)
	}
	t.Cleanup(func() { _ = hubB.Stop() })

	if hubB.FossilSyncSubject() != hubA.FossilSyncSubject() {
		t.Fatalf("after seed, sync subjects diverge: A=%q B=%q",
			hubA.FossilSyncSubject(), hubB.FossilSyncSubject())
	}

	httpCtxB, httpCancelB := context.WithCancel(context.Background())
	t.Cleanup(httpCancelB)
	go func() { _ = hubB.ServeHTTP(httpCtxB) }()

	waitFor(t, 5*time.Second, "leaf link A↔B after seed", func() bool {
		return hubA.NumLeafs() >= 1
	})

	if _, err := hubA.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "seeded.txt", Content: []byte("seeded-then-synced")}},
		Message: "commit on A after seed",
		Author:  "hub",
	}); err != nil {
		t.Fatalf("hubA.Commit: %v", err)
	}

	waitFor(t, 10*time.Second, "B sees seeded.txt at trunk", func() bool {
		got, readErr := hubB.Read(ctx, "seeded.txt")
		return readErr == nil && bytes.Equal(got, []byte("seeded-then-synced"))
	})
}

// TestCrossLeaf_HTTPPush_PropagatesCommit covers issue #160: commits that
// arrive at a hub via the HTTP xfer push endpoint (an external `fossil
// push <hub-http>` from a CLI agent) write artifacts straight into the
// SQLite repo, bypassing Repo.Commit and therefore the .commit
// auto-publish hook. The wrapper installed by Hub.ServeHTTP must close
// that gap so peers still see the commit.
//
// Topology: two hubs A and B share a project-code and are leaf-linked, so
// .commit notifications on A's NATS subject reach B and trigger a pull.
// A third standalone libfossil.Repo plays the external CLI agent — it
// clones from A's HTTP endpoint, commits locally, then pushes back to A
// via the same HTTP endpoint. The push lands through A's XferHandler.
// Before the fix, the test would time out waiting for B to see the file.
func TestCrossLeaf_HTTPPush_PropagatesCommit(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	dirA := t.TempDir()
	hubA, err := NewHub(ctx, Config{
		RepoPath:    filepath.Join(dirA, "hubA.fossil"),
		ProjectCode: sharedProjectCode,
		NobodyCaps:  "gio", // allow unauthenticated clone + push from the agent
	})
	if err != nil {
		t.Fatalf("NewHub A: %v", err)
	}
	t.Cleanup(func() { _ = hubA.Stop() })

	httpCtxA, httpCancelA := context.WithCancel(context.Background())
	t.Cleanup(httpCancelA)
	go func() { _ = hubA.ServeHTTP(httpCtxA) }()

	dirB := t.TempDir()
	hubB, err := NewHub(ctx, Config{
		RepoPath:     filepath.Join(dirB, "hubB.fossil"),
		ProjectCode:  sharedProjectCode,
		LeafUpstream: hubA.LeafURL(),
		NobodyCaps:   "gio",
	})
	if err != nil {
		t.Fatalf("NewHub B: %v", err)
	}
	t.Cleanup(func() { _ = hubB.Stop() })

	httpCtxB, httpCancelB := context.WithCancel(context.Background())
	t.Cleanup(httpCancelB)
	go func() { _ = hubB.ServeHTTP(httpCtxB) }()

	waitFor(t, 5*time.Second, "leaf link A↔B", func() bool {
		return hubA.NumLeafs() >= 1
	})

	// External CLI-agent equivalent: a libfossil.Repo with no hub, no
	// NATS connection. Clones from A via HTTP, commits a file, pushes the
	// commit back to A via HTTP. Mirrors the `fossil clone $HTTP && fossil
	// commit && fossil push $HTTP` flow from the issue's reproduction.
	agentPath := filepath.Join(t.TempDir(), "agent.fossil")
	httpURL := "http://" + hubA.HTTPAddr() + "/"
	agentRepo, _, err := libfossil.Clone(ctx, agentPath,
		libfossil.NewHTTPTransport(httpURL),
		libfossil.CloneOpts{ProjectCode: sharedProjectCode},
	)
	if err != nil {
		t.Fatalf("agent clone from %s: %v", httpURL, err)
	}
	t.Cleanup(func() { _ = agentRepo.Close() })

	if _, _, err := agentRepo.Commit(libfossil.CommitOpts{
		Files: []libfossil.FileToCommit{
			{Name: "from-agent.txt", Content: []byte("hello-from-agent")},
		},
		Comment: "external agent commit",
		User:    "agent",
	}); err != nil {
		t.Fatalf("agent Commit: %v", err)
	}

	if _, err := agentRepo.Sync(ctx,
		libfossil.NewHTTPTransport(httpURL),
		libfossil.SyncOpts{
			Push:        true,
			ProjectCode: sharedProjectCode,
			PeerID:      "agent",
		},
	); err != nil {
		t.Fatalf("agent push to %s: %v", httpURL, err)
	}

	// Without the fix, B never sees the file — the HTTP push lands in A's
	// repo but A doesn't publish on .commit, so B has no trigger to pull.
	waitFor(t, 10*time.Second, "B sees from-agent.txt at trunk", func() bool {
		got, readErr := hubB.Read(ctx, "from-agent.txt")
		return readErr == nil && bytes.Equal(got, []byte("hello-from-agent"))
	})
}

// TestCrossLeaf_HubRestart_RepublishesUnannouncedCommits covers the
// crash-safety half of the #161 follow-up: if a commit lands in the hub
// repo but the .commit notification never goes out (process killed mid-
// publish, or commit made by an offline tool against the repo while the
// hub is down), the next hub startup must scan past the persisted
// watermark and republish the backlog so peers catch up.
//
// Topology: hubA leaf-binds on a fixed port so hubB's LeafUpstream URL
// survives hubA's restart; hubB leaf-links and stays running the whole
// time. The "unannounced commit" is simulated by opening hubA's repo
// file directly via libfossil after Stop — no hub plumbing, no publish
// hook, no NATS — and committing through that handle. Before the
// watermark-persistence fix, the restart snapshots max-rid into a fresh
// in-memory watermark and the offline commit is never broadcast; peers
// stay stale until some future commit triggers a publish, and even then
// only by the lucky idempotency of pull.
func TestCrossLeaf_HubRestart_RepublishesUnannouncedCommits(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	// Reserve a stable leaf port. hubA picks it on first boot and reuses
	// it on restart so hubB's leafnode reconnect lands on the same URL.
	leafLn, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("reserve leaf port: %v", err)
	}
	leafPort := leafLn.Addr().(*net.TCPAddr).Port
	if err := leafLn.Close(); err != nil {
		t.Fatalf("close reserved leaf listener: %v", err)
	}

	dirA := t.TempDir()
	repoPath := filepath.Join(dirA, "hubA.fossil")

	hubA1, err := NewHub(ctx, Config{
		RepoPath:     repoPath,
		ProjectCode:  sharedProjectCode,
		NATSLeafPort: leafPort,
		NobodyCaps:   "gio",
	})
	if err != nil {
		t.Fatalf("NewHub A first boot: %v", err)
	}

	dirB := t.TempDir()
	hubB, err := NewHub(ctx, Config{
		RepoPath:     filepath.Join(dirB, "hubB.fossil"),
		ProjectCode:  sharedProjectCode,
		LeafUpstream: hubA1.LeafURL(),
		NobodyCaps:   "gio",
	})
	if err != nil {
		t.Fatalf("NewHub B: %v", err)
	}
	t.Cleanup(func() { _ = hubB.Stop() })

	waitFor(t, 5*time.Second, "leaf link A1↔B", func() bool {
		return hubA1.NumLeafs() >= 1
	})

	// Stop A so we can edit the repo file while NATS is down. This is
	// the moral equivalent of "A's process was killed after a libfossil
	// write but before the .commit publish went out" — the difference
	// is procedurally how the unannounced commit got in, not where the
	// crash-safety gap lives.
	if err := hubA1.Stop(); err != nil {
		t.Fatalf("Stop A first boot: %v", err)
	}

	offline, err := libfossil.Open(repoPath)
	if err != nil {
		t.Fatalf("libfossil.Open offline: %v", err)
	}
	if _, _, err := offline.Commit(libfossil.CommitOpts{
		Files: []libfossil.FileToCommit{
			{Name: "while-down.txt", Content: []byte("offline-commit")},
		},
		Comment: "commit while hub A was down",
		User:    "ghost",
	}); err != nil {
		_ = offline.Close()
		t.Fatalf("offline Commit: %v", err)
	}
	if err := offline.Close(); err != nil {
		t.Fatalf("offline Close: %v", err)
	}

	// Restart A with the same RepoPath and the same leaf port. The
	// startup catchup in startCommitSubscriber must read the persisted
	// watermark (which is at the pre-offline max), then republish for
	// every rid past it — picking up the offline commit and pushing it
	// out on the .commit subject so hubB pulls and converges.
	hubA2, err := NewHub(ctx, Config{
		RepoPath:     repoPath,
		ProjectCode:  sharedProjectCode,
		NATSLeafPort: leafPort,
		NobodyCaps:   "gio",
	})
	if err != nil {
		t.Fatalf("NewHub A restart: %v", err)
	}
	t.Cleanup(func() { _ = hubA2.Stop() })

	waitFor(t, 10*time.Second, "leaf link A2↔B after restart", func() bool {
		return hubA2.NumLeafs() >= 1
	})

	waitFor(t, 15*time.Second, "B sees while-down.txt via startup catchup", func() bool {
		got, readErr := hubB.Read(ctx, "while-down.txt")
		return readErr == nil && bytes.Equal(got, []byte("offline-commit"))
	})
}

// TestCrossLeaf_SubjectsAlign asserts that two hubs declaring the same
// ProjectCode end up subscribed to the same .sync and .commit subjects.
// Cheap structural check separate from the propagation gold-path.
func TestCrossLeaf_SubjectsAlign(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	dirA := t.TempDir()
	hubA, err := NewHub(ctx, Config{
		RepoPath:    filepath.Join(dirA, "hubA.fossil"),
		ProjectCode: sharedProjectCode,
	})
	if err != nil {
		t.Fatalf("NewHub A: %v", err)
	}
	t.Cleanup(func() { _ = hubA.Stop() })

	dirB := t.TempDir()
	hubB, err := NewHub(ctx, Config{
		RepoPath:    filepath.Join(dirB, "hubB.fossil"),
		ProjectCode: sharedProjectCode,
	})
	if err != nil {
		t.Fatalf("NewHub B: %v", err)
	}
	t.Cleanup(func() { _ = hubB.Stop() })

	if hubA.FossilSyncSubject() != hubB.FossilSyncSubject() {
		t.Errorf("sync subjects diverge: A=%q B=%q", hubA.FossilSyncSubject(), hubB.FossilSyncSubject())
	}
	if hubA.FossilCommitSubject() != hubB.FossilCommitSubject() {
		t.Errorf("commit subjects diverge: A=%q B=%q", hubA.FossilCommitSubject(), hubB.FossilCommitSubject())
	}
	wantSync := "fossil." + sharedProjectCode + ".sync"
	wantCommit := "fossil." + sharedProjectCode + ".commit"
	if hubA.FossilSyncSubject() != wantSync {
		t.Errorf("sync subject = %q, want %q", hubA.FossilSyncSubject(), wantSync)
	}
	if hubA.FossilCommitSubject() != wantCommit {
		t.Errorf("commit subject = %q, want %q", hubA.FossilCommitSubject(), wantCommit)
	}
}

// TestNewHub_ProjectCodeMismatchOnExistingRepo asserts the drift-guard:
// reopening a repo with a different ProjectCode than what's on disk
// fails fast.
func TestNewHub_ProjectCodeMismatchOnExistingRepo(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	dir := t.TempDir()
	path := filepath.Join(dir, "hub.fossil")

	h, err := NewHub(ctx, Config{
		RepoPath:    path,
		ProjectCode: sharedProjectCode,
	})
	if err != nil {
		t.Fatalf("first NewHub: %v", err)
	}
	if err := h.Stop(); err != nil {
		t.Fatalf("Stop: %v", err)
	}

	const otherCode = "2222222222222222222222222222222222222222"
	if _, err := NewHub(ctx, Config{
		RepoPath:    path,
		ProjectCode: otherCode,
	}); err == nil {
		t.Fatal("NewHub with mismatched ProjectCode returned nil err; want config-drift error")
	}
}

// waitFor polls cond every 50ms until it returns true or timeout fires.
// Reports the description on timeout for diagnostic context.
func waitFor(t *testing.T, timeout time.Duration, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(50 * time.Millisecond)
	}
	t.Fatalf("timeout after %v waiting for: %s", timeout, what)
}
