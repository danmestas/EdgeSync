package agent

import (
	"fmt"
	"net"
	"strings"
	"testing"
	"time"

	"github.com/nats-io/nats.go"
)

func TestNATSMeshStartStop(t *testing.T) {
	mesh := &NATSMesh{role: NATSRolePeer}
	clientURL, err := mesh.Start(nil) // no sidecar
	if err != nil {
		t.Fatalf("Start: %v", err)
	}
	defer mesh.Stop()

	if clientURL == "" {
		t.Fatal("clientURL is empty")
	}

	nc, err := nats.Connect(clientURL, nats.Timeout(2*time.Second))
	if err != nil {
		t.Fatalf("nats.Connect: %v", err)
	}
	defer nc.Close()

	if err := nc.Publish("test.subject", []byte("hello")); err != nil {
		t.Fatalf("Publish: %v", err)
	}
	nc.Flush()
	t.Logf("mesh started at %s, publish OK", clientURL)
}

func TestNATSMeshUsesConfiguredClientPort(t *testing.T) {
	port := freeTCPPort(t)
	mesh := &NATSMesh{
		role:       NATSRolePeer,
		clientPort: port,
	}
	clientURL, err := mesh.Start(nil)
	if err != nil {
		t.Fatalf("Start: %v", err)
	}
	defer mesh.Stop()

	want := fmt.Sprintf("nats://127.0.0.1:%d", port)
	if clientURL != want {
		t.Fatalf("clientURL = %q, want %q", clientURL, want)
	}

	nc, err := nats.Connect(clientURL, nats.Timeout(2*time.Second))
	if err != nil {
		t.Fatalf("nats.Connect: %v", err)
	}
	nc.Close()
}

func freeTCPPort(t *testing.T) int {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	defer ln.Close()
	return ln.Addr().(*net.TCPAddr).Port
}

func TestNATSMeshJetStreamEnabled(t *testing.T) {
	storeDir := t.TempDir()
	mesh := &NATSMesh{
		role:     NATSRolePeer,
		storeDir: storeDir,
	}
	opts := mesh.buildServerOpts()

	if !opts.JetStream {
		t.Errorf("opts.JetStream = false, want true")
	}
	if opts.StoreDir != storeDir {
		t.Errorf("opts.StoreDir = %q, want %q", opts.StoreDir, storeDir)
	}
	if opts.JetStreamDomain != "" {
		t.Errorf("opts.JetStreamDomain = %q, want empty", opts.JetStreamDomain)
	}
}

func TestNATSMeshJetStreamRequiresStoreDir(t *testing.T) {
	// When storeDir is empty, buildServerOpts should still enable JetStream
	// but point it at a sensible fallback beneath the CWD so the caller
	// doesn't accidentally share state with other workspaces via /tmp.
	mesh := &NATSMesh{role: NATSRolePeer}
	opts := mesh.buildServerOpts()

	if !opts.JetStream {
		t.Errorf("opts.JetStream = false, want true (always on)")
	}
	if opts.StoreDir == "" {
		t.Errorf("opts.StoreDir is empty; want a non-empty fallback path")
	}
	if strings.HasPrefix(opts.StoreDir, "/tmp") || strings.HasPrefix(opts.StoreDir, "/var/folders") {
		// Fallback should not live in a shared tmp dir that could collide
		// with other workspaces or leave stale state behind.
		t.Errorf("opts.StoreDir = %q; fallback must not live under /tmp or /var/folders", opts.StoreDir)
	}
}

func TestNATSMeshJetStreamStartStop(t *testing.T) {
	storeDir := t.TempDir()
	mesh := &NATSMesh{
		role:     NATSRolePeer,
		storeDir: storeDir,
	}
	clientURL, err := mesh.Start(nil)
	if err != nil {
		t.Fatalf("Start: %v", err)
	}
	defer mesh.Stop()

	nc, err := nats.Connect(clientURL, nats.Timeout(2*time.Second))
	if err != nil {
		t.Fatalf("nats.Connect: %v", err)
	}
	defer nc.Close()

	// Confirm JetStream is actually reachable by creating a KV bucket.
	js, err := nc.JetStream()
	if err != nil {
		t.Fatalf("JetStream: %v", err)
	}
	if _, err := js.CreateKeyValue(&nats.KeyValueConfig{Bucket: "smoke"}); err != nil {
		t.Fatalf("CreateKeyValue: %v", err)
	}
}

func TestNATSMeshRoleConfig(t *testing.T) {
	for _, role := range []NATSRole{NATSRolePeer, NATSRoleHub, NATSRoleLeaf} {
		t.Run(string(role), func(t *testing.T) {
			mesh := &NATSMesh{role: role}
			opts := mesh.buildServerOpts()

			if role == NATSRoleLeaf {
				if opts.LeafNode.Port != 0 {
					t.Errorf("leaf role should not set LeafNode.Port, got %d", opts.LeafNode.Port)
				}
			} else {
				if opts.LeafNode.Port != -1 {
					t.Errorf("%s role should set LeafNode.Port to -1 (random), got %d", role, opts.LeafNode.Port)
				}
			}
		})
	}
}

func TestShouldSolicit(t *testing.T) {
	tests := []struct {
		role     NATSRole
		myID     string
		peerID   string
		expected bool
	}{
		{NATSRoleLeaf, "zzz", "aaa", true},
		{NATSRoleHub, "aaa", "zzz", false},
		{NATSRolePeer, "aaa", "zzz", true},
		{NATSRolePeer, "zzz", "aaa", false},
		{NATSRolePeer, "aaa", "aaa", false},
	}
	for _, tt := range tests {
		name := string(tt.role) + "_" + tt.myID + "_vs_" + tt.peerID
		t.Run(name, func(t *testing.T) {
			got := shouldSolicit(tt.role, tt.myID, tt.peerID)
			if got != tt.expected {
				t.Errorf("shouldSolicit(%s, %s, %s) = %v, want %v",
					tt.role, tt.myID, tt.peerID, got, tt.expected)
			}
		})
	}
}
