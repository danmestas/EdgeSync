package sim

import (
	"fmt"
	"math/rand"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"

	libfossil "github.com/danmestas/libfossil"
	_ "github.com/danmestas/libfossil/db/driver/modernc"
	"github.com/danmestas/libfossil/simio"
	"github.com/danmestas/EdgeSync/leaf/agent"
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

// TestIrohConvergence starts two agents with iroh sidecars, seeds blobs into
// one, and verifies they converge via peer-to-peer iroh sync.
func TestIrohConvergence(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping iroh integration test in short mode")
	}
	findIrohSidecar(t)

	dir := t.TempDir()

	// 1. Create two repos with matching project-code + server-code.
	projCode := "abcdef0123456789abcdef0123456789abcdef01"
	srvCode := "fedcba9876543210fedcba9876543210fedcba98"
	var leafPaths [2]string
	for i := range 2 {
		path := filepath.Join(dir, fmt.Sprintf("leaf-%d.fossil", i))
		r, err := libfossil.Create(path, libfossil.CreateOpts{User: "testuser", Rand: simio.CryptoRand{}})
		if err != nil {
			t.Fatalf("Create leaf-%d: %v", i, err)
		}
		r.SetConfig("project-code", projCode)
		r.SetConfig("server-code", srvCode)
		r.Close()
		leafPaths[i] = path
	}

	// 2. Seed blobs into leaf-0.
	r0, err := libfossil.Open(leafPaths[0])
	if err != nil {
		t.Fatalf("open leaf-0: %v", err)
	}
	rng := rand.New(rand.NewSource(99))
	seededUUIDs, err := SeedLeaf(r0, rng, 10, 2048)
	if err != nil {
		r0.Close()
		t.Fatalf("SeedLeaf: %v", err)
	}
	r0.Close()
	t.Logf("Seeded %d blobs into leaf-0", len(seededUUIDs))

	// 3. Start both agents with iroh sidecars (no NATS, no upstream).
	// We start them without IrohPeers first to get their EndpointIds,
	// then inject the peer IDs and trigger sync.
	type leafState struct {
		agent *agent.Agent
		repo  *libfossil.Repo
	}
	var leaves [2]leafState

	for i := range 2 {
		r, err := libfossil.Open(leafPaths[i])
		if err != nil {
			t.Fatalf("open leaf-%d: %v", i, err)
		}
		a := agent.NewFromParts(agent.Config{
			RepoPath:     leafPaths[i],
			Push:         true,
			Pull:         true,
			PollInterval: 60 * time.Second, // no auto-poll
			IrohEnabled:  true,
			IrohKeyPath:  filepath.Join(dir, fmt.Sprintf("leaf-%d.iroh-key", i)),
		}, r, &libfossil.MockTransport{}, projCode, srvCode)

		if err := a.Start(); err != nil {
			r.Close()
			t.Fatalf("leaf-%d start: %v", i, err)
		}
		leaves[i] = leafState{agent: a, repo: r}
	}
	defer func() {
		for i := range 2 {
			leaves[i].agent.Stop()
		}
	}()

	// 4. Wait for both sidecars to be ready and exchange EndpointIds.
	var endpoints [2]string
	for i := range 2 {
		deadline := time.Now().Add(15 * time.Second)
		for time.Now().Before(deadline) {
			id := leaves[i].agent.IrohEndpointID()
			if id != "" {
				endpoints[i] = id
				break
			}
			time.Sleep(200 * time.Millisecond)
		}
		if endpoints[i] == "" {
			t.Fatalf("leaf-%d: iroh endpoint ID not available after 15s", i)
		}
		t.Logf("leaf-%d endpoint_id=%s", i, endpoints[i])
	}

	// 5. Sync leaf-0 → leaf-1 via iroh: leaf-0 pushes to leaf-1's endpoint.
	ctx := t.Context()
	irohTransport := agent.NewIrohTransport(
		leaves[0].agent.IrohSocketPath(), // leaf-0's sidecar socket
		endpoints[1],                      // target leaf-1's endpoint
	)

	// Give sidecars time to discover each other via relay/STUN.
	// Local peers may need up to 10s for relay registration.
	time.Sleep(5 * time.Second)

	var result *libfossil.SyncResult
	for attempt := 0; attempt < 3; attempt++ {
		result, err = leaves[0].repo.Sync(ctx, irohTransport, libfossil.SyncOpts{
			Push:        true,
			Pull:        true,
			ProjectCode: projCode,
			ServerCode:  srvCode,
		})
		if err == nil {
			break
		}
		t.Logf("attempt %d: %v (retrying...)", attempt+1, err)
		time.Sleep(3 * time.Second)
	}
	if err != nil {
		t.Fatalf("iroh sync leaf-0 → leaf-1 failed after 3 attempts: %v", err)
	}
	t.Logf("sync result: ↑%d ↓%d rounds=%d", result.FilesSent, result.FilesRecvd, result.Rounds)

	// 6. Verify convergence: leaf-1 should have all seeded blobs.
	var leaf1Blobs int
	leaves[1].repo.DB().QueryRow("SELECT count(*) FROM blob WHERE size>0").Scan(&leaf1Blobs)

	var leaf0Blobs int
	leaves[0].repo.DB().QueryRow("SELECT count(*) FROM blob WHERE size>0").Scan(&leaf0Blobs)

	t.Logf("leaf-0: %d blobs, leaf-1: %d blobs", leaf0Blobs, leaf1Blobs)

	if leaf1Blobs < len(seededUUIDs) {
		t.Errorf("leaf-1 has %d blobs, want >= %d (seeded)", leaf1Blobs, len(seededUUIDs))
	}

	// Verify specific UUIDs.
	for _, uuid := range seededUUIDs {
		var exists int
		leaves[1].repo.DB().QueryRow("SELECT count(*) FROM blob WHERE uuid=?", uuid).Scan(&exists)
		if exists == 0 {
			t.Errorf("leaf-1 missing blob %s", uuid[:16])
		}
	}

	t.Logf("PASS: iroh convergence — %d blobs synced peer-to-peer", len(seededUUIDs))
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
