package sim

import (
	"context"
	"fmt"
	"math/rand"
	"path/filepath"
	"testing"
	"time"

	libfossil "github.com/danmestas/libfossil"
	_ "github.com/danmestas/libfossil/db/driver/modernc"
	"github.com/danmestas/libfossil/simio"
	"github.com/danmestas/EdgeSync/leaf/agent"
)

// TestNATSOverIrohSync proves two peer agents created via agent.New() (with
// embedded NATSMesh, iroh sidecars, and no external NATS) can sync blobs
// peer-to-peer via iroh QUIC transport.
//
// This exercises the full agent lifecycle introduced in Task 6:
//   - agent.New() → NATSMesh.Start() → embedded NATS server per agent
//   - agent.Start() → sidecar launch → EndpointId discovery
//   - IrohTransport sync through sidecar QUIC tunnels
//
// The agents use NATSRole=peer and ServeNATSEnabled=true, proving the mesh
// boots correctly alongside the iroh sidecar. The actual data sync uses
// IrohTransport (xfer-over-QUIC) which routes through the sidecars.
func TestNATSOverIrohSync(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping NATS-over-iroh integration test in short mode")
	}

	sidecarBin := findIrohSidecar(t)

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
		t.Fatalf("open leaf-0 for seeding: %v", err)
	}
	rng := rand.New(rand.NewSource(77))
	seededUUIDs, err := SeedLeaf(r0, rng, 8, 2048)
	if err != nil {
		r0.Close()
		t.Fatalf("SeedLeaf: %v", err)
	}
	r0.Close()
	t.Logf("seeded %d blobs into leaf-0", len(seededUUIDs))

	// 3. Create two agents via agent.New() — full lifecycle with NATSMesh.
	type leafState struct {
		agent *agent.Agent
	}
	var leaves [2]leafState

	for i := range 2 {
		idx := i // capture for closure
		cfg := agent.Config{
			RepoPath:         leafPaths[i],
			NATSRole:         agent.NATSRolePeer,
			Push:             true,
			Pull:             true,
			PollInterval:     60 * time.Second, // no auto-poll
			IrohEnabled:      true,
			IrohKeyPath:      filepath.Join(dir, fmt.Sprintf("leaf-%d.iroh-key", i)),
			IrohBinaryPath:   sidecarBin,
			ServeNATSEnabled: true,
			PeerID:           fmt.Sprintf("test-peer-%d", i),
			Logger: func(msg string) {
				t.Logf("[leaf-%d] %s", idx, msg)
			},
		}

		a, err := agent.New(cfg)
		if err != nil {
			t.Fatalf("agent.New leaf-%d: %v", i, err)
		}
		leaves[i] = leafState{agent: a}
	}

	// Start both agents (mesh + sidecar + NATS serve listener).
	for i := range 2 {
		if err := leaves[i].agent.Start(); err != nil {
			for j := 0; j < i; j++ {
				leaves[j].agent.Stop()
			}
			t.Fatalf("leaf-%d start: %v", i, err)
		}
	}
	defer func() {
		for i := range 2 {
			leaves[i].agent.Stop()
		}
	}()

	// 4. Wait for both sidecars to report EndpointIds.
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

	// 5. Sync leaf-0 → leaf-1 via IrohTransport (xfer over sidecar QUIC).
	// Give sidecars time to register with relay/STUN for peer discovery.
	time.Sleep(5 * time.Second)

	irohTransport := agent.NewIrohTransport(
		leaves[0].agent.IrohSocketPath(),
		endpoints[1],
	)

	var result *libfossil.SyncResult
	for attempt := 0; attempt < 5; attempt++ {
		syncCtx, syncCancel := context.WithTimeout(t.Context(), 15*time.Second)
		result, err = leaves[0].agent.Repo().Sync(syncCtx, irohTransport, libfossil.SyncOpts{
			Push:        true,
			Pull:        true,
			ProjectCode: projCode,
			ServerCode:  srvCode,
		})
		syncCancel()
		if err == nil {
			break
		}
		t.Logf("sync attempt %d: %v (retrying...)", attempt+1, err)
		time.Sleep(3 * time.Second)
	}
	if err != nil {
		t.Fatalf("iroh sync leaf-0 → leaf-1 failed after 5 attempts: %v", err)
	}
	t.Logf("sync result: sent=%d recv=%d rounds=%d", result.FilesSent, result.FilesRecvd, result.Rounds)

	// 6. Verify convergence: leaf-1 should have all seeded blobs.
	var leaf0Blobs, leaf1Blobs int
	leaves[0].agent.Repo().DB().QueryRow("SELECT count(*) FROM blob WHERE size>0").Scan(&leaf0Blobs)
	leaves[1].agent.Repo().DB().QueryRow("SELECT count(*) FROM blob WHERE size>0").Scan(&leaf1Blobs)
	t.Logf("leaf-0: %d blobs, leaf-1: %d blobs", leaf0Blobs, leaf1Blobs)

	if leaf1Blobs < len(seededUUIDs) {
		t.Errorf("leaf-1 has %d blobs, want >= %d (seeded)", leaf1Blobs, len(seededUUIDs))
	}

	// Verify specific UUIDs.
	for _, uuid := range seededUUIDs {
		var exists int
		leaves[1].agent.Repo().DB().QueryRow("SELECT count(*) FROM blob WHERE uuid=?", uuid).Scan(&exists)
		if exists == 0 {
			t.Errorf("leaf-1 missing blob %s", uuid[:16])
		}
	}

	t.Logf("PASS: peer agents with NATSMesh + iroh sidecar — %d blobs synced peer-to-peer", len(seededUUIDs))
}
