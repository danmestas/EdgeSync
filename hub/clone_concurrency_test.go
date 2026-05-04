package hub

import (
	"context"
	"path/filepath"
	"sync"
	"testing"
	"time"

	libfossil "github.com/danmestas/libfossil"
)

// TestHub_ConcurrentClones reproduces issue #120: the leaf v0.0.7
// `applySQLiteTuning` call sets MaxOpenConns(1), which deadlocks
// libfossil's clone path because the clone has internal goroutines
// that all need the DB connection.
//
// Bones reported the deadlock from `examples/hub-leaf-e2e/` with N
// concurrent clones against a shared hub repo. Reproduced here with
// 3 concurrent clones; the deadlock is deterministic so 3 is enough.
//
// This test must complete in < 30s. Without the fix it hangs forever
// (all clone goroutines wait on the single shared conn).
func TestHub_ConcurrentClones(t *testing.T) {
	h := newTestHub(t)

	// Seed the hub repo with one commit so there's something to clone.
	if _, err := h.Commit(context.Background(), CommitOpts{
		Files:   []FileToCommit{{Name: "seed.txt", Content: []byte("v1")}},
		Message: "seed",
		Author:  "hub",
	}); err != nil {
		t.Fatalf("seed Commit: %v", err)
	}

	// Add a user the clone clients can authenticate as. Default caps for
	// "anonymous" are too narrow for clone in some builds; explicit user
	// avoids that variability.
	if err := h.AddUser(User{Login: "client", Caps: "asghbcofkimnqru"}); err != nil {
		t.Fatalf("AddUser: %v", err)
	}

	// Run hub HTTP serving in a goroutine.
	serveCtx, cancelServe := context.WithCancel(context.Background())
	t.Cleanup(cancelServe)
	go func() { _ = h.ServeHTTP(serveCtx) }()

	// Wait briefly for the listener to be live.
	time.Sleep(50 * time.Millisecond)

	hubURL := "http://" + h.HTTPAddr() + "/"
	dir := t.TempDir()

	const N = 3
	done := make(chan error, N)
	var wg sync.WaitGroup
	wg.Add(N)
	for i := range N {
		go func() {
			defer wg.Done()
			cloneCtx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
			defer cancel()
			path := filepath.Join(dir, "leaf-"+itoa(i)+".fossil")
			r, _, err := libfossil.Clone(cloneCtx, path, libfossil.NewHTTPTransport(hubURL), libfossil.CloneOpts{
				User:     "client",
				Password: "",
			})
			if r != nil {
				_ = r.Close()
			}
			done <- err
		}()
	}

	// Whole test must complete within 25s; without the fix all N clones hang.
	hardDeadline := time.After(25 * time.Second)
	completed := 0
	for completed < N {
		select {
		case err := <-done:
			if err != nil {
				t.Errorf("clone error: %v", err)
			}
			completed++
		case <-hardDeadline:
			t.Fatalf("deadlock: only %d/%d clones completed within 25s", completed, N)
		}
	}
	wg.Wait()
}

func itoa(i int) string {
	switch i {
	case 0:
		return "0"
	case 1:
		return "1"
	case 2:
		return "2"
	case 3:
		return "3"
	}
	return "n"
}
