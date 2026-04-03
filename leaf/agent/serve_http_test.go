package agent

import (
	"context"
	"io"
	"net"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/danmestas/go-libfossil/repo"
	"github.com/danmestas/go-libfossil/simio"
	"github.com/danmestas/go-libfossil/xfer"
	_ "github.com/danmestas/go-libfossil/db/driver/modernc"
)

func TestServeHTTPHealthz(t *testing.T) {
	r := newTestRepo(t)
	transport := &mockTransport{}
	addr := freeAddr(t)

	a := NewFromParts(Config{
		RepoPath:      r.Path(),
		ServeHTTPAddr: addr,
		Clock:         simio.RealClock{},
	}, r, transport, "test-project", "test-server")

	if err := a.Start(); err != nil {
		t.Fatal(err)
	}
	defer a.Stop()
	time.Sleep(100 * time.Millisecond)

	resp, err := http.Get("http://" + addr + "/healthz")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		t.Fatalf("healthz: got status %d, want 200", resp.StatusCode)
	}
	body, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(body), "ok") {
		t.Fatalf("healthz: body %q does not contain 'ok'", body)
	}
}

func freeAddr(t *testing.T) string {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	addr := ln.Addr().String()
	ln.Close()
	return addr
}

func newTestRepo(t *testing.T) *repo.Repo {
	t.Helper()
	path := t.TempDir() + "/test.fossil"
	r, err := repo.Create(path, "test", simio.CryptoRand{})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { r.Close() })
	return r
}

type mockTransport struct{}

func (m *mockTransport) Exchange(_ context.Context, _ *xfer.Message) (*xfer.Message, error) {
	return &xfer.Message{}, nil
}
