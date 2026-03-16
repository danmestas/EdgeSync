package bridge

import (
	"context"
	"fmt"
	"log"

	"github.com/nats-io/nats.go"

	libsync "github.com/dmestas/edgesync/go-libfossil/sync"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

// Bridge subscribes to a NATS subject and proxies sync requests to a Fossil
// HTTP server via go-libfossil's HTTPTransport.
type Bridge struct {
	config   Config
	upstream libsync.Transport
	conn     *nats.Conn // nil when created via NewFromParts
	sub      *nats.Subscription
	ctx      context.Context
	cancel   context.CancelFunc
}

// New creates a Bridge with the given config and connects to NATS.
func New(cfg Config) (*Bridge, error) {
	cfg.applyDefaults()
	if err := cfg.validate(); err != nil {
		return nil, err
	}
	nc, err := nats.Connect(cfg.NATSUrl)
	if err != nil {
		return nil, fmt.Errorf("bridge: nats connect: %w", err)
	}

	upstream := cfg.Upstream
	if upstream == nil {
		upstream = &libsync.HTTPTransport{URL: cfg.FossilURL}
	}

	return &Bridge{config: cfg, upstream: upstream, conn: nc}, nil
}

// NewFromParts creates a Bridge from pre-built components without performing
// any I/O. Used by tests and the deterministic simulation harness.
func NewFromParts(cfg Config, upstream libsync.Transport) *Bridge {
	cfg.applyDefaults()
	return &Bridge{config: cfg, upstream: upstream}
}

// HandleRequest processes a single xfer sync request by forwarding it to
// the upstream Fossil transport and returning the response. This is the
// core bridge logic, usable by both the NATS adapter and the simulator.
func (b *Bridge) HandleRequest(ctx context.Context, req *xfer.Message) (*xfer.Message, error) {
	return b.upstream.Exchange(ctx, req)
}

// Start subscribes to the NATS subject and begins proxying messages.
func (b *Bridge) Start() error {
	b.ctx, b.cancel = context.WithCancel(context.Background())
	subject := fmt.Sprintf("%s.%s.sync", b.config.SubjectPrefix, b.config.ProjectCode)

	var err error
	b.sub, err = b.conn.Subscribe(subject, b.handleMessage)
	if err != nil {
		return fmt.Errorf("bridge: subscribe: %w", err)
	}
	log.Printf("bridge started: subject=%s fossil=%s", subject, b.config.FossilURL)
	return nil
}

// Stop unsubscribes, cancels the context, and drains the NATS connection.
func (b *Bridge) Stop() error {
	if b.sub != nil {
		b.sub.Unsubscribe()
	}
	if b.cancel != nil {
		b.cancel()
	}
	if b.conn != nil {
		b.conn.Drain()
		b.conn.Close()
	}
	log.Println("bridge stopped")
	return nil
}

// handleMessage is the NATS subscription callback. It decodes the request,
// calls HandleRequest, encodes the response, and replies.
func (b *Bridge) handleMessage(msg *nats.Msg) {
	// BUGGIFY: return an empty reply to simulate a garbled or lost response.
	if b.config.Buggify != nil && b.config.Buggify.Check("bridge.handleMessage.emptyReply", 0.03) {
		emptyMsg := &xfer.Message{}
		data, _ := emptyMsg.Encode()
		msg.Respond(data)
		return
	}

	req, err := xfer.Decode(msg.Data)
	if err != nil {
		log.Printf("bridge: decode error: %v", err)
		b.respondEmpty(msg)
		return
	}

	resp, err := b.HandleRequest(b.ctx, req)
	if err != nil {
		log.Printf("bridge: fossil error: %v", err)
		b.respondEmpty(msg)
		return
	}

	data, err := resp.Encode()
	if err != nil {
		log.Printf("bridge: encode error: %v", err)
		b.respondEmpty(msg)
		return
	}
	msg.Respond(data)
}

func (b *Bridge) respondEmpty(msg *nats.Msg) {
	empty := &xfer.Message{}
	data, _ := empty.Encode()
	msg.Respond(data)
}
