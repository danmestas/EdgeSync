package agent

import (
	"context"
	"path/filepath"
	"testing"
	"time"

	libfossil "github.com/danmestas/go-libfossil"
	_ "github.com/danmestas/go-libfossil/db/driver/modernc"
	"github.com/danmestas/go-libfossil/simio"
)

func TestConfigDefaults(t *testing.T) {
	c := Config{RepoPath: "/tmp/test.fossil"}
	c.applyDefaults()

	if c.NATSUpstream != "" {
		t.Errorf("NATSUpstream = %q, want empty (no upstream by default)", c.NATSUpstream)
	}
	if c.NATSRole != NATSRolePeer {
		t.Errorf("NATSRole = %q, want %q", c.NATSRole, NATSRolePeer)
	}
	if c.PollInterval != 5*time.Second {
		t.Errorf("PollInterval = %v, want %v", c.PollInterval, 5*time.Second)
	}
	if c.User != "" {
		t.Errorf("User = %q, want empty (no auth by default)", c.User)
	}
	if !c.Push {
		t.Error("Push should default to true")
	}
	if !c.Pull {
		t.Error("Pull should default to true")
	}
	if c.SubjectPrefix != "fossil" {
		t.Errorf("SubjectPrefix = %q, want %q", c.SubjectPrefix, "fossil")
	}
}

func TestConfigDefaultsPreserveExplicit(t *testing.T) {
	c := Config{
		RepoPath:      "/tmp/test.fossil",
		NATSUpstream:  "nats://custom:4223",
		NATSRole:      NATSRoleHub,
		PollInterval:  10 * time.Second,
		User:          "alice",
		Push:          true,
		Pull:          false,
		SubjectPrefix: "edgesync",
	}
	c.applyDefaults()

	if c.NATSUpstream != "nats://custom:4223" {
		t.Errorf("NATSUpstream = %q, want %q", c.NATSUpstream, "nats://custom:4223")
	}
	if c.NATSRole != NATSRoleHub {
		t.Errorf("NATSRole = %q, want %q", c.NATSRole, NATSRoleHub)
	}
	if c.PollInterval != 10*time.Second {
		t.Errorf("PollInterval = %v, want %v", c.PollInterval, 10*time.Second)
	}
	if c.User != "alice" {
		t.Errorf("User = %q, want %q", c.User, "alice")
	}
	// When at least one of Push/Pull is set, applyDefaults should not override.
	if !c.Push {
		t.Error("Push should remain true")
	}
	if c.Pull {
		t.Error("Pull should remain false when Push is explicitly set")
	}
	if c.SubjectPrefix != "edgesync" {
		t.Errorf("SubjectPrefix = %q, want %q", c.SubjectPrefix, "edgesync")
	}
}

func TestConfigValidateRequiresRepoPath(t *testing.T) {
	c := Config{}
	c.applyDefaults()
	err := c.validate()
	if err == nil {
		t.Fatal("expected error for missing RepoPath")
	}
	if err.Error() != "agent: config: RepoPath is required" {
		t.Errorf("unexpected error: %v", err)
	}
}

func TestConfigValidateOK(t *testing.T) {
	c := Config{RepoPath: "/tmp/test.fossil"}
	c.applyDefaults()
	if err := c.validate(); err != nil {
		t.Errorf("unexpected error: %v", err)
	}
}

// createTestRepo creates a temporary Fossil repo and returns its path.
// The repo is cleaned up when the test ends.
func createTestRepo(t *testing.T) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "test.fossil")
	r, err := libfossil.Create(path, libfossil.CreateOpts{User: "testuser"})
	if err != nil {
		t.Fatalf("libfossil.Create: %v", err)
	}
	r.Close()
	return path
}

// openTestRepo creates and opens a temporary Fossil repo for state machine tests.
func openTestRepo(t *testing.T) (*libfossil.Repo, string, string) {
	t.Helper()
	path := filepath.Join(t.TempDir(), "test.fossil")
	r, err := libfossil.Create(path, libfossil.CreateOpts{User: "testuser"})
	if err != nil {
		t.Fatalf("libfossil.Create: %v", err)
	}
	t.Cleanup(func() { r.Close() })

	projCode, err := r.Config("project-code")
	if err != nil {
		t.Fatalf("read project-code: %v", err)
	}
	srvCode, err := r.Config("server-code")
	if err != nil {
		t.Fatalf("read server-code: %v", err)
	}
	return r, projCode, srvCode
}

func TestAgentNewAndStop(t *testing.T) {
	repoPath := createTestRepo(t)
	natsURL := startEmbeddedNATS(t)

	a, err := New(Config{
		RepoPath: repoPath,
		NATSUpstream:  natsURL,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}

	if a.projectCode == "" {
		t.Error("projectCode should not be empty")
	}
	if a.serverCode == "" {
		t.Error("serverCode should not be empty")
	}

	if err := a.Stop(); err != nil {
		t.Fatalf("Stop: %v", err)
	}
}

func TestAgentStartAndStop(t *testing.T) {
	repoPath := createTestRepo(t)
	natsURL := startEmbeddedNATS(t)

	a, err := New(Config{
		RepoPath:     repoPath,
		NATSUpstream:      natsURL,
		PollInterval: 50 * time.Millisecond,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}

	if err := a.Start(); err != nil {
		t.Fatalf("Start: %v", err)
	}

	// Let at least one poll tick fire.
	time.Sleep(100 * time.Millisecond)

	if err := a.Stop(); err != nil {
		t.Fatalf("Stop: %v", err)
	}
}

func TestAgentNewBadRepoPath(t *testing.T) {
	natsURL := startEmbeddedNATS(t)

	_, err := New(Config{
		RepoPath: "/nonexistent/path/to/repo.fossil",
		NATSUpstream:  natsURL,
	})
	if err == nil {
		t.Fatal("expected error for bad repo path, got nil")
	}
	t.Logf("got expected error: %v", err)
}

func TestAgentSyncNowNonBlocking(t *testing.T) {
	repoPath := createTestRepo(t)
	natsURL := startEmbeddedNATS(t)

	a, err := New(Config{
		RepoPath:     repoPath,
		NATSUpstream:      natsURL,
		PollInterval: 10 * time.Second, // long interval so only SyncNow triggers
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}

	// SyncNow should not block or panic even before Start.
	a.SyncNow()
	a.SyncNow() // second call should also be safe (buffered channel)

	if err := a.Start(); err != nil {
		t.Fatalf("Start: %v", err)
	}

	// SyncNow while running should not block.
	a.SyncNow()
	a.SyncNow()

	time.Sleep(50 * time.Millisecond)

	if err := a.Stop(); err != nil {
		t.Fatalf("Stop: %v", err)
	}
}

// --- State machine (Tick) tests ---

func TestTickSyncNowConverges(t *testing.T) {
	r, projCode, srvCode := openTestRepo(t)

	mt := &libfossil.MockTransport{
		Handler: func(req []byte) []byte {
			return []byte{} // empty = converge immediately
		},
	}

	a := NewFromParts(Config{
		Clock: simio.NewSimClock(),
	}, r, mt, projCode, srvCode)

	act := a.Tick(context.Background(), EventSyncNow)
	if act.Type != ActionSynced {
		t.Fatalf("Type = %d, want ActionSynced", act.Type)
	}
	if act.Err != nil {
		t.Fatalf("Err = %v", act.Err)
	}
	if act.Result == nil {
		t.Fatal("Result should not be nil")
	}
	if act.Result.Rounds != 1 {
		t.Fatalf("Rounds = %d, want 1", act.Result.Rounds)
	}
}

func TestTickTimerEvent(t *testing.T) {
	r, projCode, srvCode := openTestRepo(t)

	exchangeCount := 0
	mt := &libfossil.MockTransport{
		Handler: func(req []byte) []byte {
			exchangeCount++
			return []byte{}
		},
	}

	a := NewFromParts(Config{
		Clock: simio.NewSimClock(),
	}, r, mt, projCode, srvCode)

	// Timer event should trigger a sync, same as SyncNow
	act := a.Tick(context.Background(), EventTimer)
	if act.Type != ActionSynced {
		t.Fatalf("Type = %d, want ActionSynced", act.Type)
	}
	if exchangeCount != 1 {
		t.Fatalf("exchangeCount = %d, want 1", exchangeCount)
	}
}

func TestTickStopEvent(t *testing.T) {
	r, projCode, srvCode := openTestRepo(t)
	mt := &libfossil.MockTransport{}

	a := NewFromParts(Config{
		Clock: simio.NewSimClock(),
	}, r, mt, projCode, srvCode)

	act := a.Tick(context.Background(), EventStop)
	if act.Type != ActionStopped {
		t.Fatalf("Type = %d, want ActionStopped", act.Type)
	}
	if act.Result != nil {
		t.Fatal("Result should be nil for stop")
	}
}

func TestTickSyncError(t *testing.T) {
	r, projCode, srvCode := openTestRepo(t)

	// Cancel context before Tick so Sync returns immediately
	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	mt := &libfossil.MockTransport{
		Handler: func(req []byte) []byte {
			return []byte{}
		},
	}

	a := NewFromParts(Config{
		Clock: simio.NewSimClock(),
	}, r, mt, projCode, srvCode)

	act := a.Tick(ctx, EventSyncNow)
	if act.Type != ActionSynced {
		t.Fatalf("Type = %d, want ActionSynced", act.Type)
	}
	if act.Err == nil {
		t.Fatal("expected error from cancelled context")
	}
}

func TestNewFromPartsRepo(t *testing.T) {
	r, projCode, srvCode := openTestRepo(t)
	mt := &libfossil.MockTransport{}

	a := NewFromParts(Config{}, r, mt, projCode, srvCode)

	if a.Repo() != r {
		t.Fatal("Repo() should return the injected repo")
	}
	if a.projectCode != projCode {
		t.Fatalf("projectCode = %q, want %q", a.projectCode, projCode)
	}
	if a.serverCode != srvCode {
		t.Fatalf("serverCode = %q, want %q", a.serverCode, srvCode)
	}
}
