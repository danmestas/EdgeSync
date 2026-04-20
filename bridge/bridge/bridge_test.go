package bridge

import (
	"context"
	"testing"
	"time"

	libfossil "github.com/danmestas/libfossil"
	"github.com/nats-io/nats.go"
	natsserver "github.com/nats-io/nats-server/v2/server"
)

// --- helpers ---

func startEmbeddedNATS(t *testing.T) string {
	t.Helper()
	opts := &natsserver.Options{Port: -1}
	ns, err := natsserver.NewServer(opts)
	if err != nil {
		t.Fatalf("nats-server new: %v", err)
	}
	ns.Start()
	if !ns.ReadyForConnections(5 * time.Second) {
		t.Fatal("nats-server not ready within 5s")
	}
	t.Cleanup(func() { ns.Shutdown() })
	return ns.ClientURL()
}

// --- Config tests ---

func TestConfigDefaults(t *testing.T) {
	c := Config{FossilURL: "http://localhost:8080", ProjectCode: "proj1"}
	c.applyDefaults()

	if c.NATSUrl != "nats://localhost:4222" {
		t.Errorf("NATSUrl = %q, want %q", c.NATSUrl, "nats://localhost:4222")
	}
	if c.SubjectPrefix != "fossil" {
		t.Errorf("SubjectPrefix = %q, want %q", c.SubjectPrefix, "fossil")
	}
}

func TestConfigDefaultsPreserveExplicit(t *testing.T) {
	c := Config{
		NATSUrl:       "nats://custom:4223",
		FossilURL:     "http://fosshost:9090",
		ProjectCode:   "myproj",
		SubjectPrefix: "edgesync",
	}
	c.applyDefaults()

	if c.NATSUrl != "nats://custom:4223" {
		t.Errorf("NATSUrl = %q, want %q", c.NATSUrl, "nats://custom:4223")
	}
	if c.SubjectPrefix != "edgesync" {
		t.Errorf("SubjectPrefix = %q, want %q", c.SubjectPrefix, "edgesync")
	}
}

func TestConfigValidateMissingFossilURL(t *testing.T) {
	c := Config{ProjectCode: "proj1"}
	if err := c.validate(); err == nil {
		t.Fatal("expected error for missing FossilURL")
	}
}

func TestConfigValidateMissingProjectCode(t *testing.T) {
	c := Config{FossilURL: "http://localhost:8080"}
	if err := c.validate(); err == nil {
		t.Fatal("expected error for missing ProjectCode")
	}
}

func TestConfigValidateOK(t *testing.T) {
	c := Config{FossilURL: "http://localhost:8080", ProjectCode: "proj1"}
	if err := c.validate(); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
}

// --- Bridge lifecycle tests ---

func TestBridgeNewAndStop(t *testing.T) {
	natsURL := startEmbeddedNATS(t)

	b, err := New(Config{
		NATSUrl:     natsURL,
		FossilURL:   "http://localhost:9999",
		ProjectCode: "test",
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}

	if err := b.Start(); err != nil {
		t.Fatalf("Start: %v", err)
	}

	if err := b.Stop(); err != nil {
		t.Fatalf("Stop: %v", err)
	}
}

// --- Bridge proxy tests ---

func TestBridgeProxiesNATSMessage(t *testing.T) {
	natsURL := startEmbeddedNATS(t)

	// Use a mock transport that returns a canned response.
	mt := &libfossil.MockTransport{
		Handler: func(req []byte) []byte {
			return []byte("canned-response")
		},
	}

	b, err := New(Config{
		NATSUrl:     natsURL,
		FossilURL:   "http://unused",
		ProjectCode: "proj1",
		Upstream:    mt,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	if err := b.Start(); err != nil {
		t.Fatalf("Start: %v", err)
	}
	defer b.Stop()

	nc, err := nats.Connect(natsURL)
	if err != nil {
		t.Fatalf("client connect: %v", err)
	}
	defer nc.Close()

	msg, err := nc.Request("fossil.proj1.sync", []byte("test-request"), 5*time.Second)
	if err != nil {
		t.Fatalf("NATS request: %v", err)
	}
	if string(msg.Data) != "canned-response" {
		t.Errorf("response = %q, want %q", msg.Data, "canned-response")
	}
}

func TestBridgeHandlesBadPayload(t *testing.T) {
	natsURL := startEmbeddedNATS(t)

	mt := &libfossil.MockTransport{
		Handler: func(req []byte) []byte {
			return []byte("response")
		},
	}

	b, err := New(Config{
		NATSUrl:     natsURL,
		FossilURL:   "http://unused",
		ProjectCode: "proj2",
		Upstream:    mt,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	if err := b.Start(); err != nil {
		t.Fatalf("Start: %v", err)
	}
	defer b.Stop()

	nc, err := nats.Connect(natsURL)
	if err != nil {
		t.Fatalf("client connect: %v", err)
	}
	defer nc.Close()

	// Send garbage data -- bridge should respond gracefully, not crash.
	msg, err := nc.Request("fossil.proj2.sync", []byte("not valid zlib"), 5*time.Second)
	if err != nil {
		t.Fatalf("NATS request: %v", err)
	}
	t.Logf("bridge responded with %d bytes (did not crash)", len(msg.Data))
}

func TestBridgeCustomSubjectPrefix(t *testing.T) {
	natsURL := startEmbeddedNATS(t)

	mt := &libfossil.MockTransport{
		Handler: func(req []byte) []byte {
			return []byte("custom-prefix-response")
		},
	}

	b, err := New(Config{
		NATSUrl:       natsURL,
		FossilURL:     "http://unused",
		ProjectCode:   "proj3",
		SubjectPrefix: "edgesync",
		Upstream:      mt,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	if err := b.Start(); err != nil {
		t.Fatalf("Start: %v", err)
	}
	defer b.Stop()

	nc, err := nats.Connect(natsURL)
	if err != nil {
		t.Fatalf("client connect: %v", err)
	}
	defer nc.Close()

	// Use the custom prefix subject.
	msg, err := nc.Request("edgesync.proj3.sync", []byte("test"), 5*time.Second)
	if err != nil {
		t.Fatalf("NATS request with custom prefix: %v", err)
	}
	if string(msg.Data) != "custom-prefix-response" {
		t.Errorf("response = %q, want %q", msg.Data, "custom-prefix-response")
	}

	// Default prefix should NOT work.
	_, err = nc.Request("fossil.proj3.sync", []byte("test"), 500*time.Millisecond)
	if err == nil {
		t.Error("expected timeout on default prefix, but got a response")
	}
}

// --- HandleRequest (state machine) tests ---

func TestHandleRequestForwards(t *testing.T) {
	mt := &libfossil.MockTransport{
		Handler: func(req []byte) []byte {
			return []byte("from-upstream")
		},
	}

	b := NewFromParts(Config{ProjectCode: "test"}, mt)

	resp, err := b.HandleRequest(context.Background(), []byte("test-request"))
	if err != nil {
		t.Fatalf("HandleRequest: %v", err)
	}
	if string(resp) != "from-upstream" {
		t.Errorf("response = %q, want %q", resp, "from-upstream")
	}
}

func TestHandleRequestEmptyResponse(t *testing.T) {
	mt := &libfossil.MockTransport{} // nil handler returns empty bytes

	b := NewFromParts(Config{ProjectCode: "test"}, mt)

	resp, err := b.HandleRequest(context.Background(), []byte{})
	if err != nil {
		t.Fatalf("HandleRequest: %v", err)
	}
	if len(resp) != 0 {
		t.Fatalf("expected 0 bytes, got %d", len(resp))
	}
}

func TestNewFromPartsNoNATS(t *testing.T) {
	mt := &libfossil.MockTransport{}
	b := NewFromParts(Config{ProjectCode: "test"}, mt)

	// conn should be nil — Stop should not panic.
	if b.conn != nil {
		t.Fatal("conn should be nil for NewFromParts")
	}
	if err := b.Stop(); err != nil {
		t.Fatalf("Stop: %v", err)
	}
}

func TestConfigValidateUpstreamOverridesFossilURL(t *testing.T) {
	// With Upstream set, FossilURL is not required.
	c := Config{
		ProjectCode: "test",
		Upstream:    &libfossil.MockTransport{},
	}
	if err := c.validate(); err != nil {
		t.Fatalf("expected no error with Upstream set, got: %v", err)
	}
}
