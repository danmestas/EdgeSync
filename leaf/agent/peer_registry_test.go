package agent

import (
	"testing"

	"github.com/danmestas/go-libfossil/repo"
	"github.com/danmestas/go-libfossil/simio"
	"github.com/danmestas/go-libfossil/sync"
)

func TestPeerRegistryEnsureAndSeed(t *testing.T) {
	r, projCode, srvCode := openTestRepo(t)

	mt := &sync.MockTransport{}
	clock := simio.NewSimClock()

	a := NewFromParts(Config{
		PeerID:          "test-peer-1",
		ServeHTTPAddr:   ":8080",
		SubjectPrefix:   "fossil",
		Clock:           clock,
	}, r, mt, projCode, srvCode)

	// Ensure schema
	if err := a.ensurePeerRegistry(); err != nil {
		t.Fatalf("ensurePeerRegistry: %v", err)
	}

	// Seed the registry
	if err := a.seedPeerRegistry(); err != nil {
		t.Fatalf("seedPeerRegistry: %v", err)
	}

	// Verify the row exists
	pkValues := map[string]any{"peer_id": "test-peer-1"}
	pkColDefs := []repo.ColumnDef{{Name: "peer_id", Type: "text", PK: true}}
	pkHash := repo.PKHash(pkColDefs, pkValues)

	row, mtime, err := repo.LookupXRow(r.DB(), "peer_registry", peerRegistryDef, pkHash)
	if err != nil {
		t.Fatalf("LookupXRow: %v", err)
	}
	if row == nil {
		t.Fatal("expected row to exist, got nil")
	}

	// Check fields
	if row["peer_id"] != "test-peer-1" {
		t.Errorf("peer_id = %v, want test-peer-1", row["peer_id"])
	}
	if row["last_sync"] != clock.Now().Unix() {
		t.Errorf("last_sync = %v, want %v", row["last_sync"], clock.Now().Unix())
	}
	if row["version"] != "dev" {
		t.Errorf("version = %v, want dev", row["version"])
	}
	if row["platform"] == "" {
		t.Error("platform should not be empty")
	}
	if row["nats_subject"] != "fossil" {
		t.Errorf("nats_subject = %v, want fossil", row["nats_subject"])
	}
	if row["addr"] != ":8080" {
		t.Errorf("addr = %v, want :8080", row["addr"])
	}
	if mtime != clock.Now().Unix() {
		t.Errorf("mtime = %v, want %v", mtime, clock.Now().Unix())
	}

	// Check capabilities
	caps := row["capabilities"].(string)
	if caps != "serve-http,push,pull" {
		t.Errorf("capabilities = %q, want serve-http,push,pull", caps)
	}
}

func TestPeerRegistryUpdateAfterSync(t *testing.T) {
	r, projCode, srvCode := openTestRepo(t)

	mt := &sync.MockTransport{}
	clock := simio.NewSimClock()

	a := NewFromParts(Config{
		PeerID: "test-peer-2",
		Clock:  clock,
	}, r, mt, projCode, srvCode)

	// Ensure and seed
	if err := a.ensurePeerRegistry(); err != nil {
		t.Fatalf("ensurePeerRegistry: %v", err)
	}
	if err := a.seedPeerRegistry(); err != nil {
		t.Fatalf("seedPeerRegistry: %v", err)
	}

	// Advance clock
	clock.Advance(60)

	// Update after sync
	if err := a.updatePeerRegistryAfterSync(); err != nil {
		t.Fatalf("updatePeerRegistryAfterSync: %v", err)
	}

	// Verify last_sync was updated
	pkValues := map[string]any{"peer_id": "test-peer-2"}
	pkColDefs := []repo.ColumnDef{{Name: "peer_id", Type: "text", PK: true}}
	pkHash := repo.PKHash(pkColDefs, pkValues)

	row, mtime, err := repo.LookupXRow(r.DB(), "peer_registry", peerRegistryDef, pkHash)
	if err != nil {
		t.Fatalf("LookupXRow: %v", err)
	}
	if row == nil {
		t.Fatal("expected row to exist, got nil")
	}

	if row["last_sync"] != clock.Now().Unix() {
		t.Errorf("last_sync = %v, want %v (should be updated)", row["last_sync"], clock.Now().Unix())
	}
	if mtime != clock.Now().Unix() {
		t.Errorf("mtime = %v, want %v", mtime, clock.Now().Unix())
	}
}

func TestPeerRegistryCapabilities(t *testing.T) {
	r, projCode, srvCode := openTestRepo(t)
	mt := &sync.MockTransport{}

	tests := []struct {
		name     string
		config   Config
		wantCaps string
	}{
		{
			name:     "http-only",
			config:   Config{ServeHTTPAddr: ":8080"},
			wantCaps: "serve-http,push,pull",
		},
		{
			name:     "nats-only",
			config:   Config{ServeNATSEnabled: true},
			wantCaps: "serve-nats,push,pull",
		},
		{
			name:     "both",
			config:   Config{ServeHTTPAddr: ":8080", ServeNATSEnabled: true},
			wantCaps: "serve-http,serve-nats,push,pull",
		},
		{
			name:     "none",
			config:   Config{},
			wantCaps: "push,pull",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			tt.config.PeerID = "test-peer-caps"
			tt.config.Clock = simio.NewSimClock()

			a := NewFromParts(tt.config, r, mt, projCode, srvCode)

			caps := a.capabilities()
			got := ""
			if len(caps) > 0 {
				got = caps[0]
				for i := 1; i < len(caps); i++ {
					got += "," + caps[i]
				}
			}

			if got != tt.wantCaps {
				t.Errorf("capabilities = %q, want %q", got, tt.wantCaps)
			}
		})
	}
}
