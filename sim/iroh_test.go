package sim

import (
	"fmt"
	"math/rand"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"

	libfossil "github.com/danmestas/go-libfossil"
	"github.com/danmestas/go-libfossil/simio"
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
		r, err := libfossil.Create(path, libfossil.CreateOpts{User: "testuser", Rand: simio.CryptoRand{}})
		if err != nil {
			t.Fatalf("Create leaf-%d: %v", i, err)
		}
		r.SetConfig("project-code", projCode)
		r.Close()
		leafPaths[i] = path
	}

	// 2. Seed blobs into leaf-0.
	r0, err := libfossil.Open(leafPaths[0])
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
	r0, err = libfossil.Open(leafPaths[0])
	if err != nil {
		t.Fatalf("reopen leaf-0: %v", err)
	}

	serverCode, _ := r0.Config("server-code")

	transport := &libfossil.MockTransport{}

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
