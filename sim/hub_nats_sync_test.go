package sim

import (
	"bytes"
	"context"
	"path/filepath"
	"testing"
	"time"

	libfossil "github.com/danmestas/libfossil"
	_ "github.com/danmestas/libfossil/db/driver/modernc"
	"github.com/nats-io/nats.go"

	"github.com/danmestas/EdgeSync/hub"
	"github.com/danmestas/EdgeSync/leaf/agent"
)

// TestHubNATSFossilSync_LeafPushLandsOnHub proves that a hub created via
// hub.NewHub (with no leaf-agent serve-mode helper running on top) accepts
// fossil-sync xfer requests over NATS and applies them to the hub repo.
//
// This is the contract that lets downstream consumers drop libfossil from
// their go.mod — previously they had to spin up a leaf-agent in serve mode
// (Pull=false, Push=false, Autosync=AutosyncOff, ServeNATSEnabled=true)
// to get a NATS subscriber on fossil.<code>.sync, transitively pulling
// libfossil into their import graph.
func TestHubNATSFossilSync_LeafPushLandsOnHub(t *testing.T) {
	if testing.Short() {
		t.Skip("hub bootstrap is too slow for -short")
	}

	dir := t.TempDir()
	hubPath := filepath.Join(dir, "hub.fossil")
	leafPath := filepath.Join(dir, "leaf.fossil")

	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)

	h, err := hub.NewHub(ctx, hub.Config{RepoPath: hubPath})
	if err != nil {
		t.Fatalf("hub.NewHub: %v", err)
	}
	t.Cleanup(func() { _ = h.Stop() })

	if h.FossilSyncSubject() == "" {
		t.Fatal("hub.FossilSyncSubject() empty: subscriber should be enabled by default")
	}
	if err := h.AddUser(hub.User{Login: "leaf", Caps: "asghbcofkimnqru"}); err != nil {
		t.Fatalf("hub.AddUser: %v", err)
	}

	go func() { _ = h.ServeHTTP(ctx) }()
	if !waitForHTTP(t, "http://"+h.HTTPAddr()+"/", 2*time.Second) {
		t.Fatal("hub HTTP not ready within 2s")
	}
	hubURL := "http://" + h.HTTPAddr() + "/"

	cloneCtx, cloneCancel := context.WithTimeout(ctx, 15*time.Second)
	leafRepo, _, err := libfossil.Clone(cloneCtx, leafPath, libfossil.NewHTTPTransport(hubURL), libfossil.CloneOpts{
		User: "leaf",
	})
	cloneCancel()
	if err != nil {
		t.Fatalf("libfossil.Clone: %v", err)
	}
	t.Cleanup(func() { _ = leafRepo.Close() })

	projectCode, err := leafRepo.Config("project-code")
	if err != nil {
		t.Fatalf("leaf project-code: %v", err)
	}

	if _, _, err := leafRepo.Commit(libfossil.CommitOpts{
		Files:   []libfossil.FileToCommit{{Name: "leaf-nats-push.txt", Content: []byte("hello-via-nats\n")}},
		Comment: "leaf push over nats",
		User:    "leaf",
	}); err != nil {
		t.Fatalf("leaf commit: %v", err)
	}

	nc, err := nats.Connect(h.NATSURL(), nats.Timeout(2*time.Second))
	if err != nil {
		t.Fatalf("connect leaf NATS client to hub: %v", err)
	}
	t.Cleanup(nc.Close)

	transport := agent.NewNATSTransport(nc, projectCode, 5*time.Second, "fossil")

	syncCtx, syncCancel := context.WithTimeout(ctx, 15*time.Second)
	res, err := leafRepo.Sync(syncCtx, transport, libfossil.SyncOpts{
		Push:        true,
		Pull:        true,
		ProjectCode: projectCode,
		User:        "leaf",
		PeerID:      "leaf-test",
	})
	syncCancel()
	if err != nil {
		t.Fatalf("leaf Sync over NATS to hub: %v", err)
	}
	if res == nil {
		t.Fatal("leaf Sync returned nil result")
	}

	got, err := h.Read(ctx, "leaf-nats-push.txt")
	if err != nil {
		t.Fatalf("hub.Read(leaf-nats-push.txt): %v", err)
	}
	if !bytes.Equal(got, []byte("hello-via-nats\n")) {
		t.Errorf("hub got %q, want %q", got, "hello-via-nats\n")
	}
}

// TestHubNATSFossilSync_DisableSkipsSubscriber proves that
// Config.DisableFossilSyncOverNATS turns the subscriber off. Used by
// callers that want a read-only HTTP-only mirror.
func TestHubNATSFossilSync_DisableSkipsSubscriber(t *testing.T) {
	if testing.Short() {
		t.Skip("hub bootstrap is too slow for -short")
	}

	dir := t.TempDir()
	h, err := hub.NewHub(context.Background(), hub.Config{
		RepoPath:                  filepath.Join(dir, "hub.fossil"),
		DisableFossilSyncOverNATS: true,
	})
	if err != nil {
		t.Fatalf("hub.NewHub: %v", err)
	}
	t.Cleanup(func() { _ = h.Stop() })

	if h.FossilSyncSubject() != "" {
		t.Errorf("FossilSyncSubject() = %q with DisableFossilSyncOverNATS=true; want empty", h.FossilSyncSubject())
	}
}
