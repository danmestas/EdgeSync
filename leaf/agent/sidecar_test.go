package agent

import (
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestSidecarLifecycle(t *testing.T) {
	dir := t.TempDir()
	stubPath := filepath.Join(dir, "iroh-sidecar")
	stubScript := "#!/bin/sh\nsleep 60\n"
	if err := os.WriteFile(stubPath, []byte(stubScript), 0755); err != nil {
		t.Fatalf("write stub: %v", err)
	}

	sock := filepath.Join(dir, "test.sock")
	keyPath := filepath.Join(dir, "test-key")

	sc := &sidecar{
		binPath:     stubPath,
		socketPath:  sock,
		keyPath:     keyPath,
		callbackURL: "http://127.0.0.1:8080",
		alpn:        "/edgesync/xfer/1",
	}

	if err := sc.spawn(); err != nil {
		t.Fatalf("spawn: %v", err)
	}

	if sc.cmd == nil || sc.cmd.Process == nil {
		t.Fatal("expected process to be running")
	}
	pid := sc.cmd.Process.Pid
	t.Logf("sidecar pid: %d", pid)

	sc.kill()

	time.Sleep(100 * time.Millisecond)

	// Verify process is gone (on Unix, Signal(nil) checks liveness without sending).
	proc, err := os.FindProcess(pid)
	if err == nil && proc != nil {
		if sigErr := proc.Signal(nil); sigErr == nil {
			t.Error("process still alive after kill")
		}
	}
}

func TestSidecarBinaryNotFound(t *testing.T) {
	sc := &sidecar{
		binPath:     "/nonexistent/iroh-sidecar",
		socketPath:  "/tmp/test.sock",
		keyPath:     "/tmp/test-key",
		callbackURL: "http://127.0.0.1:8080",
		alpn:        "/edgesync/xfer/1",
	}

	err := sc.spawn()
	if err == nil {
		t.Fatal("expected error for missing binary")
	}
	t.Logf("error (expected): %v", err)
}
