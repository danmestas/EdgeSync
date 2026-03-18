package bridge

import (
	"context"
	"io"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	libsync "github.com/dmestas/edgesync/go-libfossil/sync"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
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

// startMockFossil creates an httptest server that accepts POST /xfer,
// reads the zlib-compressed xfer request, and responds with a canned
// xfer response containing an igot card.
func startMockFossil(t *testing.T) *httptest.Server {
	t.Helper()
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// Fossil routes to /xfer by Content-Type, not URL path — accept any path.
		io.ReadAll(r.Body)
		r.Body.Close()

		// Build a canned response with an igot card.
		resp := &xfer.Message{
			Cards: []xfer.Card{
				&xfer.IGotCard{UUID: "mock-fossil-uuid"},
			},
		}
		data, err := resp.Encode()
		if err != nil {
			t.Errorf("mock fossil encode: %v", err)
			http.Error(w, "encode error", 500)
			return
		}
		w.Header().Set("Content-Type", "application/x-fossil")
		w.Write(data)
	}))
	t.Cleanup(func() { srv.Close() })
	return srv
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
	fossilSrv := startMockFossil(t)

	b, err := New(Config{
		NATSUrl:     natsURL,
		FossilURL:   fossilSrv.URL,
		ProjectCode: "proj1",
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	if err := b.Start(); err != nil {
		t.Fatalf("Start: %v", err)
	}
	defer b.Stop()

	// Connect a client and send a request via NATS.
	nc, err := nats.Connect(natsURL)
	if err != nil {
		t.Fatalf("client connect: %v", err)
	}
	defer nc.Close()

	req := &xfer.Message{
		Cards: []xfer.Card{
			&xfer.PullCard{ServerCode: "srv1", ProjectCode: "proj1"},
		},
	}
	reqData, err := req.Encode()
	if err != nil {
		t.Fatalf("encode request: %v", err)
	}

	msg, err := nc.Request("fossil.proj1.sync", reqData, 5*time.Second)
	if err != nil {
		t.Fatalf("NATS request: %v", err)
	}

	resp, err := xfer.Decode(msg.Data)
	if err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if len(resp.Cards) != 1 {
		t.Fatalf("expected 1 card, got %d", len(resp.Cards))
	}
	igot, ok := resp.Cards[0].(*xfer.IGotCard)
	if !ok {
		t.Fatalf("expected *IGotCard, got %T", resp.Cards[0])
	}
	if igot.UUID != "mock-fossil-uuid" {
		t.Errorf("UUID = %q, want %q", igot.UUID, "mock-fossil-uuid")
	}
}

func TestBridgeHandlesBadPayload(t *testing.T) {
	natsURL := startEmbeddedNATS(t)
	fossilSrv := startMockFossil(t)

	b, err := New(Config{
		NATSUrl:     natsURL,
		FossilURL:   fossilSrv.URL,
		ProjectCode: "proj2",
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
	// The bridge may forward garbage to the Fossil server and return
	// whatever the server sends back, or return an error/empty response.
	// The key invariant: the bridge does NOT crash or hang.
	msg, err := nc.Request("fossil.proj2.sync", []byte("not valid zlib"), 5*time.Second)
	if err != nil {
		t.Fatalf("NATS request: %v", err)
	}
	t.Logf("bridge responded with %d bytes (did not crash)", len(msg.Data))
}

func TestBridgeCustomSubjectPrefix(t *testing.T) {
	natsURL := startEmbeddedNATS(t)
	fossilSrv := startMockFossil(t)

	b, err := New(Config{
		NATSUrl:       natsURL,
		FossilURL:     fossilSrv.URL,
		ProjectCode:   "proj3",
		SubjectPrefix: "edgesync",
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

	req := &xfer.Message{
		Cards: []xfer.Card{
			&xfer.PullCard{ServerCode: "srv1", ProjectCode: "proj3"},
		},
	}
	reqData, err := req.Encode()
	if err != nil {
		t.Fatalf("encode request: %v", err)
	}

	// Use the custom prefix subject.
	msg, err := nc.Request("edgesync.proj3.sync", reqData, 5*time.Second)
	if err != nil {
		t.Fatalf("NATS request with custom prefix: %v", err)
	}

	resp, err := xfer.Decode(msg.Data)
	if err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if len(resp.Cards) != 1 {
		t.Fatalf("expected 1 card, got %d", len(resp.Cards))
	}

	// Default prefix should NOT work.
	_, err = nc.Request("fossil.proj3.sync", reqData, 500*time.Millisecond)
	if err == nil {
		t.Error("expected timeout on default prefix, but got a response")
	}
}

// --- HandleRequest (state machine) tests ---

func TestHandleRequestForwards(t *testing.T) {
	mt := &libsync.MockTransport{
		Handler: func(req *xfer.Message) *xfer.Message {
			return &xfer.Message{Cards: []xfer.Card{
				&xfer.IGotCard{UUID: "from-upstream"},
			}}
		},
	}

	b := NewFromParts(Config{ProjectCode: "test"}, mt)

	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.PullCard{ServerCode: "srv1", ProjectCode: "test"},
	}}

	resp, err := b.HandleRequest(context.Background(), req)
	if err != nil {
		t.Fatalf("HandleRequest: %v", err)
	}
	if len(resp.Cards) != 1 {
		t.Fatalf("expected 1 card, got %d", len(resp.Cards))
	}
	igot, ok := resp.Cards[0].(*xfer.IGotCard)
	if !ok {
		t.Fatalf("expected *IGotCard, got %T", resp.Cards[0])
	}
	if igot.UUID != "from-upstream" {
		t.Errorf("UUID = %q, want %q", igot.UUID, "from-upstream")
	}
}

func TestHandleRequestEmptyResponse(t *testing.T) {
	mt := &libsync.MockTransport{} // nil handler returns empty message

	b := NewFromParts(Config{ProjectCode: "test"}, mt)

	resp, err := b.HandleRequest(context.Background(), &xfer.Message{})
	if err != nil {
		t.Fatalf("HandleRequest: %v", err)
	}
	if len(resp.Cards) != 0 {
		t.Fatalf("expected 0 cards, got %d", len(resp.Cards))
	}
}

func TestNewFromPartsNoNATS(t *testing.T) {
	mt := &libsync.MockTransport{}
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
		Upstream:    &libsync.MockTransport{},
	}
	if err := c.validate(); err != nil {
		t.Fatalf("expected no error with Upstream set, got: %v", err)
	}
}
