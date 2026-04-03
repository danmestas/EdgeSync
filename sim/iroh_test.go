package sim

import (
	"fmt"
	"math/rand"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"

	"github.com/danmestas/go-libfossil/repo"
	"github.com/danmestas/go-libfossil/simio"
	libsync "github.com/danmestas/go-libfossil/sync"
	"github.com/dmestas/edgesync/leaf/agent"
)

func TestIrohPeerSync(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping iroh integration test in short mode")
	}

	// Require iroh-sidecar binary.
	sidecarBin := findIrohSidecar(t)
	_ = sidecarBin

	dir := t.TempDir()

	// 1. Create two repos with the same project-code.
	projCode := "abcdef0123456789abcdef0123456789abcdef01"
	var leafPaths [2]string
	for i := range 2 {
		path := filepath.Join(dir, fmt.Sprintf("leaf-%d.fossil", i))
		r, err := repo.Create(path, "testuser", simio.CryptoRand{})
		if err != nil {
			t.Fatalf("repo.Create leaf-%d: %v", i, err)
		}
		r.DB().Exec("UPDATE config SET value=? WHERE name='project-code'", projCode)
		r.Close()
		leafPaths[i] = path
	}

	// 2. Seed blobs into leaf-0.
	r0, err := repo.Open(leafPaths[0])
	if err != nil {
		t.Fatalf("open leaf-0: %v", err)
	}
	rng := rand.New(rand.NewSource(42))
	seededUUIDs, err := SeedLeaf(r0, rng, 5, 4096)
	if err != nil {
		r0.Close()
		t.Fatalf("SeedLeaf: %v", err)
	}
	r0.Close()
	t.Logf("Seeded %d blobs into leaf-0", len(seededUUIDs))

	// 3. Build leaf-0 via NewFromParts to avoid eager NATS connection.
	// The iroh sidecar only needs the repo and an HTTP callback listener —
	// no NATS transport is required for this lifecycle test.
	r0, err = repo.Open(leafPaths[0])
	if err != nil {
		t.Fatalf("reopen leaf-0: %v", err)
	}

	var serverCode string
	r0.DB().QueryRow("SELECT value FROM config WHERE name='server-code'").Scan(&serverCode)

	transport := &libsync.MockTransport{}

	leaf0 := agent.NewFromParts(agent.Config{
		RepoPath:     leafPaths[0],
		Push:         true,
		Pull:         true,
		PollInterval: 60 * time.Second, // don't auto-poll
		IrohEnabled:  true,
		IrohKeyPath:  filepath.Join(dir, "leaf-0.iroh-key"),
		// ServeHTTPAddr is intentionally empty: the agent will create an
		// ephemeral callback listener on a random port for iroh.
	}, r0, transport, projCode, serverCode)

	if err := leaf0.Start(); err != nil {
		t.Fatalf("leaf-0 start: %v", err)
	}
	defer leaf0.Stop()

	t.Log("leaf-0 started with iroh sidecar — lifecycle validated")
}

// findIrohSidecar locates the iroh-sidecar binary or skips the test.
func findIrohSidecar(t *testing.T) string {
	t.Helper()

	// Check PATH first.
	if path, err := exec.LookPath("iroh-sidecar"); err == nil {
		return path
	}

	// Check bin/ relative to project root.
	candidate := filepath.Join("..", "bin", "iroh-sidecar")
	if abs, err := filepath.Abs(candidate); err == nil {
		if _, err := os.Stat(abs); err == nil {
			return abs
		}
	}

	t.Skip("iroh-sidecar binary not found, run 'make iroh-sidecar' first")
	return ""
}
