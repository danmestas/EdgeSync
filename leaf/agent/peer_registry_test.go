package agent

import (
	"testing"

	libfossil "github.com/danmestas/go-libfossil"
	"github.com/danmestas/go-libfossil/simio"
)

func TestPeerRegistryEnsureAndSeed(t *testing.T) {
	r, projCode, srvCode := openTestRepo(t)

	mt := &libfossil.MockTransport{}
	clock := simio.NewSimClock()

	a := NewFromParts(Config{
		PeerID:        "test-peer-1",
		ServeHTTPAddr: ":8080",
		SubjectPrefix: "fossil",
		Clock:         clock,
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
	var peerID, version, platform, natsSubject, addr string
	var lastSync int64
	err := r.DB().QueryRow(
		"SELECT peer_id, last_sync, version, platform, nats_subject, addr FROM peer_registry WHERE peer_id=?",
		"test-peer-1",
	).Scan(&peerID, &lastSync, &version, &platform, &natsSubject, &addr)
	if err != nil {
		t.Fatalf("query peer_registry: %v", err)
	}

	if peerID != "test-peer-1" {
		t.Errorf("peer_id = %v, want test-peer-1", peerID)
	}
	if lastSync != clock.Now().Unix() {
		t.Errorf("last_sync = %v, want %v", lastSync, clock.Now().Unix())
	}
	if version != "dev" {
		t.Errorf("version = %v, want dev", version)
	}
	if platform == "" {
		t.Error("platform should not be empty")
	}
	if natsSubject != "fossil" {
		t.Errorf("nats_subject = %v, want fossil", natsSubject)
	}
	if addr != ":8080" {
		t.Errorf("addr = %v, want :8080", addr)
	}
}

func TestPeerRegistryUpdateAfterSync(t *testing.T) {
	r, projCode, srvCode := openTestRepo(t)

	mt := &libfossil.MockTransport{}
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
	var lastSync int64
	err := r.DB().QueryRow(
		"SELECT last_sync FROM peer_registry WHERE peer_id=?",
		"test-peer-2",
	).Scan(&lastSync)
	if err != nil {
		t.Fatalf("query: %v", err)
	}
	if lastSync != clock.Now().Unix() {
		t.Errorf("last_sync = %v, want %v (should be updated)", lastSync, clock.Now().Unix())
	}
}

func TestPeerRegistryCapabilities(t *testing.T) {
	r, projCode, srvCode := openTestRepo(t)
	mt := &libfossil.MockTransport{}

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
