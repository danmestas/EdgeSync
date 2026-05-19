package main_test

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// sharedBinary is the path to the edgesync binary built once by TestMain
// and reused across every test in this package. Building only once avoids a
// build-storm — without this, each test's buildBinary() invocation ran its own
// `go build`, and under cold-cache / parallel-load the cumulative cost pushed
// the package past `go test -timeout=30s` (see issue #170).
var sharedBinary string

func TestMain(m *testing.M) {
	// Build into a temp dir that survives until the process exits.
	dir, err := os.MkdirTemp("", "edgesync-test-bin-")
	if err != nil {
		fmt.Fprintf(os.Stderr, "tempdir: %s\n", err)
		os.Exit(2)
	}
	defer os.RemoveAll(dir)

	root, err := findRepoRoot()
	if err != nil {
		fmt.Fprintf(os.Stderr, "find repo root: %s\n", err)
		os.Exit(2)
	}

	sharedBinary = filepath.Join(dir, "edgesync")
	cmd := exec.Command("go", "build", "-buildvcs=false", "-o", sharedBinary, "./cmd/edgesync/")
	cmd.Dir = root
	if out, err := cmd.CombinedOutput(); err != nil {
		fmt.Fprintf(os.Stderr, "build edgesync binary: %s\n%s", err, out)
		os.Exit(2)
	}

	os.Exit(m.Run())
}

// findRepoRoot locates the EdgeSync repo root (parent of cmd/) starting from
// the test's working directory.
func findRepoRoot() (string, error) {
	wd, err := os.Getwd()
	if err != nil {
		return "", err
	}
	// When `go test` runs, cwd is the package dir (cmd/edgesync/).
	root := filepath.Dir(filepath.Dir(wd))
	if _, err := os.Stat(filepath.Join(root, "go.mod")); err != nil {
		return "", fmt.Errorf("could not find repo root from %s: %w", wd, err)
	}
	return root, nil
}

// edgesyncBin returns the shared edgesync binary path.
func edgesyncBin(t *testing.T) string {
	t.Helper()
	if sharedBinary == "" {
		t.Fatal("sharedBinary is empty — TestMain did not run")
	}
	return sharedBinary
}

func TestNotifyCLIInit(t *testing.T) {
	bin := edgesyncBin(t)
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

func TestNotifyCLIPairAndDevices(t *testing.T) {
	bin := edgesyncBin(t)
	tmp := t.TempDir()
	fakeRepo := filepath.Join(tmp, "project.fossil")

	// Init.
	cmd := exec.Command(bin, "-R", fakeRepo, "notify", "init")
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("init: %s\n%s", err, out)
	}

	// Pair (no endpoint/NATS = text token only, no QR).
	cmd = exec.Command(bin, "-R", fakeRepo, "notify", "pair", "--name", "dan-iphone")
	out, err = cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("pair: %s\n%s", err, out)
	}
	pairOutput := string(out)

	// Should contain a token in XXXX-XXXX-XXXX format.
	if !strings.Contains(pairOutput, "-") {
		t.Errorf("pair output should contain token with dashes, got: %q", pairOutput)
	}
	if !strings.Contains(pairOutput, "expires") {
		t.Errorf("pair output should mention expiry, got: %q", pairOutput)
	}

	// Devices — should be empty (token pending, not yet validated).
	cmd = exec.Command(bin, "-R", fakeRepo, "notify", "devices")
	out, err = cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("devices: %s\n%s", err, out)
	}
	if !strings.Contains(string(out), "no paired devices") {
		t.Errorf("devices should be empty before validation, got: %q", string(out))
	}
}

func TestNotifyCLIUnpair(t *testing.T) {
	bin := edgesyncBin(t)
	tmp := t.TempDir()
	fakeRepo := filepath.Join(tmp, "project.fossil")

	// Init.
	cmd := exec.Command(bin, "-R", fakeRepo, "notify", "init")
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("init: %s\n%s", err, out)
	}

	// Unpair a device that doesn't exist = error.
	cmd = exec.Command(bin, "-R", fakeRepo, "notify", "unpair", "nonexistent")
	if out, err := cmd.CombinedOutput(); err == nil {
		t.Fatalf("unpair nonexistent should fail, got: %s", out)
	}
}

func TestNotifyCLISendAndThreads(t *testing.T) {
	bin := edgesyncBin(t)
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
