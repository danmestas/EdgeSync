package main_test

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// buildBinary builds the edgesync binary into a temp dir and returns its path.
func buildBinary(t *testing.T) string {
	t.Helper()
	bin := filepath.Join(t.TempDir(), "edgesync")
	cmd := exec.Command("go", "build", "-buildvcs=false", "-o", bin, "./cmd/edgesync/")
	// Build from the repo root (where go.mod lives).
	cmd.Dir = repoRoot(t)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("build failed: %s\n%s", err, out)
	}
	return bin
}

// repoRoot returns the root of the EdgeSync repo (parent of cmd/).
func repoRoot(t *testing.T) string {
	t.Helper()
	// This test file is in cmd/edgesync/, so go two levels up.
	wd, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	// When running tests, cwd is the package dir (cmd/edgesync/).
	// Go up to repo root.
	root := filepath.Dir(filepath.Dir(wd))
	if _, err := os.Stat(filepath.Join(root, "go.mod")); err != nil {
		t.Fatalf("could not find repo root from %s", wd)
	}
	return root
}

func TestNotifyCLIInit(t *testing.T) {
	bin := buildBinary(t)
	tmp := t.TempDir()

	// -R points to a fake repo path — init creates notify.fossil next to it.
	fakeRepo := filepath.Join(tmp, "project.fossil")

	cmd := exec.Command(bin, "-R", fakeRepo, "notify", "init")
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("notify init failed: %s\n%s", err, out)
	}

	expected := filepath.Join(tmp, "notify.fossil")
	got := strings.TrimSpace(string(out))
	if got != expected {
		t.Errorf("init output = %q, want %q", got, expected)
	}

	// Verify the file was created.
	if _, err := os.Stat(expected); os.IsNotExist(err) {
		t.Errorf("notify.fossil not created at %s", expected)
	}
}

func TestNotifyCLISendAndThreads(t *testing.T) {
	bin := buildBinary(t)
	tmp := t.TempDir()
	fakeRepo := filepath.Join(tmp, "project.fossil")

	// Init first.
	cmd := exec.Command(bin, "-R", fakeRepo, "notify", "init")
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("notify init failed: %s\n%s", err, out)
	}

	// Send a message.
	cmd = exec.Command(bin, "-R", fakeRepo, "notify", "send",
		"--project", "testproj",
		"--priority", "urgent",
		"Hello from CLI test")
	out, err = cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("notify send failed: %s\n%s", err, out)
	}
	sendOutput := string(out)

	// stdout should contain the message ID (msg-...).
	if !strings.Contains(sendOutput, "msg-") {
		t.Errorf("send output should contain message ID, got: %q", sendOutput)
	}
	// stderr has thread info — combined output should contain "thread:".
	if !strings.Contains(sendOutput, "thread:") {
		t.Errorf("send output should contain thread info, got: %q", sendOutput)
	}

	// List threads.
	cmd = exec.Command(bin, "-R", fakeRepo, "notify", "threads", "--project", "testproj")
	out, err = cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("notify threads failed: %s\n%s", err, out)
	}
	threadsOutput := string(out)

	if !strings.Contains(threadsOutput, "msgs:1") {
		t.Errorf("threads should show msgs:1, got: %q", threadsOutput)
	}
	if !strings.Contains(threadsOutput, "priority:urgent") {
		t.Errorf("threads should show priority:urgent, got: %q", threadsOutput)
	}
	if !strings.Contains(threadsOutput, "Hello from CLI test") {
		t.Errorf("threads should show message body, got: %q", threadsOutput)
	}

	// Status.
	cmd = exec.Command(bin, "-R", fakeRepo, "notify", "status")
	out, err = cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("notify status failed: %s\n%s", err, out)
	}
	statusOutput := string(out)

	notifyPath := filepath.Join(tmp, "notify.fossil")
	if !strings.Contains(statusOutput, notifyPath) {
		t.Errorf("status should contain notify repo path %s, got: %q", notifyPath, statusOutput)
	}
	if !strings.Contains(statusOutput, "repo-only") {
		t.Errorf("status should show repo-only connection, got: %q", statusOutput)
	}
}
