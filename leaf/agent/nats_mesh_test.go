package agent

import (
	"testing"
	"time"

	"github.com/nats-io/nats.go"
)

func TestNATSMeshStartStop(t *testing.T) {
	mesh := &NATSMesh{role: NATSRolePeer}
	clientURL, err := mesh.Start()
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
