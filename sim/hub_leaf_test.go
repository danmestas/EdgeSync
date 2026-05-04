package sim

import (
	"context"
	"net/http"
	"path/filepath"
	"testing"
	"time"

	libfossil "github.com/danmestas/libfossil"
	_ "github.com/danmestas/libfossil/db/driver/modernc"

	"github.com/danmestas/EdgeSync/hub"
	"github.com/danmestas/EdgeSync/leaf/agent"
)

// TestHubLeafE2E proves the hub + leaf agent contract end-to-end:
//
//  1. Construct a *hub.Hub in-process (auto-allocated ports).
//  2. Hub seeds an initial commit so a leaf can clone from it.
//  3. Clone a leaf repo from the hub via libfossil.Clone.
//  4. Build a real *agent.Agent against the leaf repo.
//  5. Leaf commits a file via Agent.Commit.
//  6. Leaf pushes to the hub via Agent.SyncTo.
//  7. Confirm the file appears on the hub via Hub.Read.
//
// Locks the hub-leaf transport contract: any future change that breaks
// the path (e.g. issue #120's SetMaxOpenConns deadlock) breaks this test.
func TestHubLeafE2E(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping hub-leaf e2e in short mode")
	}

	dir := t.TempDir()
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)

	// 1. Hub.
	h, err := hub.NewHub(ctx, hub.Config{
		RepoPath: filepath.Join(dir, "hub.fossil"),
	})
	if err != nil {
		t.Fatalf("hub.NewHub: %v", err)
	}
	t.Cleanup(func() { _ = h.Stop() })

	// 2. Seed + add a client user with push/pull caps.
	if _, err := h.Commit(ctx, hub.CommitOpts{
		Files:   []hub.FileToCommit{{Name: "seed.txt", Content: []byte("seed\n")}},
		Message: "seed",
		Author:  "hub",
	}); err != nil {
		t.Fatalf("hub seed Commit: %v", err)
	}
	if err := h.AddUser(hub.User{Login: "leaf", Caps: "asghbcofkimnqru"}); err != nil {
		t.Fatalf("hub AddUser: %v", err)
	}

	// 3. Run hub HTTP server in a goroutine so leaves can clone/sync.
	serveDone := make(chan error, 1)
	go func() { serveDone <- h.ServeHTTP(ctx) }()
	if !waitForHTTP(t, "http://"+h.HTTPAddr()+"/", 2*time.Second) {
		t.Fatal("hub HTTP not ready within 2s")
	}
	hubURL := "http://" + h.HTTPAddr() + "/"

	// 4. Clone leaf repo from hub.
	leafPath := filepath.Join(dir, "leaf.fossil")
	cloneCtx, cancelClone := context.WithTimeout(ctx, 15*time.Second)
	defer cancelClone()
	leafRepo, _, err := libfossil.Clone(cloneCtx, leafPath, libfossil.NewHTTPTransport(hubURL), libfossil.CloneOpts{
		User: "leaf",
	})
	if err != nil {
		t.Fatalf("libfossil.Clone: %v", err)
	}
	leafRepo.Close() // Agent will reopen.

	// 5. Build a real Agent against the cloned leaf repo. No NATS — we
	// exercise the HTTP sync path via Agent.SyncTo, which constructs its
	// own transport and doesn't need the mesh.
	a, err := agent.New(agent.Config{
		RepoPath: leafPath,
	})
	if err != nil {
		t.Fatalf("agent.New: %v", err)
	}
	t.Cleanup(func() { _ = a.Stop() })

	// 6. Leaf commits a file.
	commitCtx, cancelCommit := context.WithTimeout(ctx, 5*time.Second)
	rev, err := a.Commit(commitCtx, agent.CommitOpts{
		Files:   []agent.FileToCommit{{Name: "leaf-update.txt", Content: []byte("hello-from-leaf\n")}},
		Message: "leaf update",
		Author:  "leaf",
	})
	cancelCommit()
	if err != nil {
		t.Fatalf("Agent.Commit: %v", err)
	}
	if rev == "" {
		t.Fatal("Agent.Commit returned empty RevID")
	}

	// 7. Leaf pushes to the hub.
	syncCtx, cancelSync := context.WithTimeout(ctx, 15*time.Second)
	res, err := a.SyncTo(syncCtx, hubURL, agent.SyncOpts{
		Push: true,
		Pull: true,
		User: "leaf",
	})
	cancelSync()
	if err != nil {
		t.Fatalf("Agent.SyncTo: %v", err)
	}
	if res == nil {
		t.Fatal("Agent.SyncTo returned nil result")
	}

	// 8. Confirm the file landed on the hub.
	got, err := h.ReadAt(ctx, "trunk", "leaf-update.txt")
	if err != nil {
		t.Fatalf("hub Hub.ReadAt: %v", err)
	}
	if string(got) != "hello-from-leaf\n" {
		t.Errorf("hub Hub.ReadAt = %q, want %q", got, "hello-from-leaf\n")
	}

	// 9. Tear down ServeHTTP cleanly.
	cancel()
	select {
	case err := <-serveDone:
		if err != nil {
			t.Errorf("Hub.ServeHTTP returned error: %v", err)
		}
	case <-time.After(3 * time.Second):
		t.Error("Hub.ServeHTTP did not shut down within 3s of ctx cancel")
	}
}

func waitForHTTP(t *testing.T, url string, deadline time.Duration) bool {
	t.Helper()
	client := &http.Client{Timeout: 100 * time.Millisecond}
	end := time.Now().Add(deadline)
	for time.Now().Before(end) {
		resp, err := client.Get(url)
		if err == nil {
			resp.Body.Close()
			return true
		}
		time.Sleep(20 * time.Millisecond)
	}
	return false
}
