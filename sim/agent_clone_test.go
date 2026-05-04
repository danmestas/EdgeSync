package sim

import (
	"context"
	"path/filepath"
	"testing"
	"time"

	libfossil "github.com/danmestas/libfossil"
	_ "github.com/danmestas/libfossil/db/driver/modernc"

	"github.com/danmestas/EdgeSync/hub"
	"github.com/danmestas/EdgeSync/leaf/agent"
)

// TestAgentNew_ClonesWhenRepoAbsent proves that agent.New, given
// Config.CloneFromHubURL, clones the leaf repo from the hub before
// opening it. Locks the contract that downstream consumers (bones)
// rely on to avoid pulling libfossil into their import graph just for
// the pre-agent clone step.
func TestAgentNew_ClonesWhenRepoAbsent(t *testing.T) {
	if testing.Short() {
		t.Skip("hub bootstrap is too slow for -short")
	}

	dir := t.TempDir()
	hubPath := filepath.Join(dir, "hub.fossil")
	leafPath := filepath.Join(dir, "leaf.fossil")

	h, err := hub.NewHub(context.Background(), hub.Config{RepoPath: hubPath})
	if err != nil {
		t.Fatalf("hub.NewHub: %v", err)
	}
	t.Cleanup(func() { _ = h.Stop() })

	serveCtx, serveCancel := context.WithCancel(context.Background())
	defer serveCancel()
	go func() { _ = h.ServeHTTP(serveCtx) }()
	if !waitForHTTP(t, "http://"+h.HTTPAddr()+"/", 2*time.Second) {
		t.Fatal("hub HTTP not ready within 2s")
	}

	hubURL := "http://" + h.HTTPAddr() + "/"

	a, err := agent.New(agent.Config{
		RepoPath:        leafPath,
		CloneFromHubURL: hubURL,
		User:            "leaf",
	})
	if err != nil {
		t.Fatalf("agent.New with CloneFromHubURL: %v", err)
	}
	t.Cleanup(func() { _ = a.Stop() })

	leaf, err := libfossil.Open(leafPath)
	if err != nil {
		t.Fatalf("open cloned leaf repo: %v", err)
	}
	defer leaf.Close()

	leafCode, err := leaf.Config("project-code")
	if err != nil {
		t.Fatalf("leaf project-code: %v", err)
	}

	hubRepo, err := libfossil.Open(hubPath)
	if err != nil {
		t.Fatalf("open hub repo: %v", err)
	}
	defer hubRepo.Close()
	hubCode, err := hubRepo.Config("project-code")
	if err != nil {
		t.Fatalf("hub project-code: %v", err)
	}

	if leafCode == "" {
		t.Fatal("leaf project-code is empty after clone")
	}
	if leafCode != hubCode {
		t.Errorf("project-code mismatch: leaf=%q hub=%q", leafCode, hubCode)
	}
}

// TestAgentNew_RejectsMissingRepoWithoutCloneURL preserves prior behavior:
// when CloneFromHubURL is empty and RepoPath does not exist, agent.New
// surfaces the libfossil.Open error rather than silently bootstrapping.
func TestAgentNew_RejectsMissingRepoWithoutCloneURL(t *testing.T) {
	dir := t.TempDir()
	leafPath := filepath.Join(dir, "leaf.fossil")

	if _, err := agent.New(agent.Config{RepoPath: leafPath}); err == nil {
		t.Fatal("agent.New on missing repo with no CloneFromHubURL returned nil error")
	}
}
