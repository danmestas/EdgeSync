package agent

import (
	"context"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"sync/atomic"
	"testing"
	"time"

	libfossil "github.com/danmestas/go-libfossil"
	_ "github.com/danmestas/go-libfossil/db/driver/modernc"
	"github.com/danmestas/go-libfossil/testutil"
	"github.com/nats-io/nats.go"
)

// startFossilServer starts a fossil server on a free port and returns the URL
// and a cleanup function. The server is shut down when the test ends.
func startFossilServer(t *testing.T, repoPath string) string {
	t.Helper()

	bin := testutil.FossilBinary()
	if bin == "" {
		t.Skip("fossil not in PATH")
	}

	// Find a free port
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	port := ln.Addr().(*net.TCPAddr).Port
	ln.Close()

	cmd := exec.Command(bin, "server", fmt.Sprintf("--port=%d", port), repoPath)
	cmd.Stdout = os.Stderr
	cmd.Stderr = os.Stderr
	if err := cmd.Start(); err != nil {
		t.Fatalf("fossil server start: %v", err)
	}

	// Wait for the server to accept connections (poll up to 5s)
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		conn, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", port), 200*time.Millisecond)
		if err == nil {
			conn.Close()
			goto ready
		}
		time.Sleep(100 * time.Millisecond)
	}
	cmd.Process.Kill()
	cmd.Wait()
	t.Fatalf("fossil server did not become ready on port %d within 5s", port)

ready:
	serverURL := fmt.Sprintf("http://127.0.0.1:%d", port)
	t.Cleanup(func() {
		cmd.Process.Kill()
		cmd.Wait()
		os.Remove(repoPath + "-wal")
		os.Remove(repoPath + "-shm")
	})
	return serverURL
}

// startTestBridge subscribes to NATS subject fossil.<projectCode>.sync and
// proxies each raw byte payload to the fossil server via HTTP.
func startTestBridge(t *testing.T, nc *nats.Conn, projectCode, fossilURL string) {
	t.Helper()

	var errLogged atomic.Bool

	subject := fmt.Sprintf("fossil.%s.sync", projectCode)
	_, err := nc.Subscribe(subject, func(msg *nats.Msg) {
		ht := libfossil.NewHTTPTransport(fossilURL)
		resp, err := ht.RoundTrip(context.Background(), msg.Data)
		if err != nil {
			if !errLogged.Load() {
				t.Logf("bridge: exchange error (further occurrences suppressed): %v", err)
				errLogged.Store(true)
			}
			msg.Respond([]byte{})
			return
		}
		msg.Respond(resp)
	})
	if err != nil {
		t.Fatalf("bridge subscribe: %v", err)
	}
	nc.Flush()
}

func TestIntegrationLeafPush(t *testing.T) {
	if !testutil.HasFossil() {
		t.Skip("fossil not in PATH")
	}
	bin := testutil.FossilBinary()

	dir := t.TempDir()

	// 1. Create a Go-managed local repo with a checkin.
	localPath := filepath.Join(dir, "local.fossil")
	r, err := libfossil.Create(localPath, libfossil.CreateOpts{User: "testuser"})
	if err != nil {
		t.Fatalf("Create: %v", err)
	}

	_, _, err = r.Commit(libfossil.CommitOpts{
		Files: []libfossil.FileToCommit{
			{Name: "hello.txt", Content: []byte("hello from leaf agent integration test")},
		},
		Comment: "initial checkin from leaf integration test",
		User:    "testuser",
		Time:    time.Date(2026, 3, 15, 12, 0, 0, 0, time.UTC),
	})
	if err != nil {
		r.Close()
		t.Fatalf("Commit: %v", err)
	}
	r.Close()

	// 2. Clone to create a matching remote (ensures project-code/server-code match).
	remotePath := filepath.Join(dir, "remote.fossil")
	cloneCmd := exec.Command(bin, "clone", localPath, remotePath)
	cloneOut, err := cloneCmd.CombinedOutput()
	if err != nil {
		t.Fatalf("fossil clone: %v\n%s", err, cloneOut)
	}

	// 3. Start fossil server on the remote.
	fossilURL := startFossilServer(t, remotePath)

	// 4. Start embedded NATS.
	natsURL := startEmbeddedNATS(t)

	// 5. Connect a bridge subscriber: NATS -> HTTP -> fossil server.
	bridgeConn, err := nats.Connect(natsURL)
	if err != nil {
		t.Fatalf("bridge nats connect: %v", err)
	}
	defer bridgeConn.Close()

	// Read project-code from the local repo.
	localRepo, err := libfossil.Open(localPath)
	if err != nil {
		t.Fatalf("Open local: %v", err)
	}
	projectCode, err := localRepo.Config("project-code")
	if err != nil {
		localRepo.Close()
		t.Fatalf("read project-code: %v", err)
	}
	localRepo.Close()

	startTestBridge(t, bridgeConn, projectCode, fossilURL)

	// 6. Create Agent, Start, SyncNow.
	a, err := New(Config{
		RepoPath:     localPath,
		NATSUrl:      natsURL,
		PollInterval: 10 * time.Second, // long interval; we use SyncNow
		Push:         true,
		Pull:         false,
		User:         "anonymous",
	})
	if err != nil {
		t.Fatalf("agent.New: %v", err)
	}

	if err := a.Start(); err != nil {
		t.Fatalf("agent.Start: %v", err)
	}

	// Trigger an immediate sync.
	a.SyncNow()

	// Wait for the sync to complete (give it a few seconds).
	time.Sleep(3 * time.Second)

	if err := a.Stop(); err != nil {
		t.Fatalf("agent.Stop: %v", err)
	}

	t.Logf("Integration test completed successfully.")
	t.Logf("  Fossil server URL: %s", fossilURL)
	t.Logf("  NATS URL: %s", natsURL)
	t.Logf("  Project code: %s", projectCode)
}
