package bridge

import (
	"context"
	"fmt"
	"log/slog"
	"time"

	"github.com/nats-io/nats.go"
	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/codes"
	"go.opentelemetry.io/otel/metric"
	"go.opentelemetry.io/otel/trace"

	libfossil "github.com/danmestas/libfossil"
)

const instrumentationName = "edgesync-bridge"

// Bridge subscribes to a NATS subject and proxies sync requests to a Fossil
// HTTP server via libfossil's Transport.
type Bridge struct {
	config   Config
	upstream libfossil.Transport
	conn     *nats.Conn // nil when created via NewFromParts
	sub      *nats.Subscription
	ctx      context.Context
	cancel   context.CancelFunc
	tracer   trace.Tracer

	requestsTotal metric.Int64Counter
	errorsTotal   metric.Int64Counter
	duration      metric.Float64Histogram
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
		upstream = libfossil.NewHTTPTransport(cfg.FossilURL)
	}

	b := &Bridge{config: cfg, upstream: upstream, conn: nc}
	b.initTelemetry()
	return b, nil
}

// NewFromParts creates a Bridge from pre-built components without performing
// any I/O. Used by tests and the deterministic simulation harness.
func NewFromParts(cfg Config, upstream libfossil.Transport) *Bridge {
	cfg.applyDefaults()
	b := &Bridge{config: cfg, upstream: upstream}
	b.initTelemetry()
	return b
}

// initTelemetry sets up the tracer and metric instruments.
func (b *Bridge) initTelemetry() {
	b.tracer = otel.Tracer(instrumentationName)
	m := otel.GetMeterProvider().Meter(instrumentationName)
	b.requestsTotal, _ = m.Int64Counter("bridge.requests.total",
		metric.WithDescription("Total bridge proxy requests"))
	b.errorsTotal, _ = m.Int64Counter("bridge.errors.total",
		metric.WithDescription("Bridge proxy requests ending with error"))
	b.duration, _ = m.Float64Histogram("bridge.duration.seconds",
		metric.WithDescription("Bridge proxy request duration"),
		metric.WithUnit("s"),
		metric.WithExplicitBucketBoundaries(0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10))
}

// HandleRequest processes a single raw sync request by forwarding it to
// the upstream Transport and returning the raw response bytes.
func (b *Bridge) HandleRequest(ctx context.Context, payload []byte) ([]byte, error) {
	return b.upstream.RoundTrip(ctx, payload)
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
	// Flush ensures the subscription is acknowledged by the server before
	// Start returns, preventing "no responders" races in tests/CI.
	if err := b.conn.Flush(); err != nil {
		return fmt.Errorf("bridge: flush after subscribe: %w", err)
	}
	slog.Info("bridge started", "subject", subject, "fossil", b.config.FossilURL)
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
	slog.Info("bridge stopped")
	return nil
}

// handleMessage is the NATS subscription callback. It forwards the raw
// payload to the upstream transport and replies with the raw response.
func (b *Bridge) handleMessage(msg *nats.Msg) {
	start := time.Now()
	ctx, span := b.tracer.Start(b.ctx, "bridge.proxy",
		trace.WithSpanKind(trace.SpanKindServer),
		trace.WithAttributes(
			attribute.String("messaging.system", "nats"),
			attribute.String("bridge.project_code", b.config.ProjectCode),
			attribute.Int("messaging.message.body.size", len(msg.Data)),
		),
	)
	defer func() {
		span.End()
		b.requestsTotal.Add(ctx, 1)
		b.duration.Record(ctx, time.Since(start).Seconds())
	}()

	// BUGGIFY: return an empty reply to simulate a garbled or lost response.
	if b.config.Buggify != nil && b.config.Buggify.Check("bridge.handleMessage.emptyReply", 0.03) {
		msg.Respond([]byte{})
		return
	}

	resp, err := b.HandleRequest(ctx, msg.Data)
	if err != nil {
		span.RecordError(err)
		span.SetStatus(codes.Error, err.Error())
		slog.Error("bridge: upstream error", "error", err)
		b.errorsTotal.Add(ctx, 1)
		msg.Respond([]byte{})
		return
	}

	msg.Respond(resp)
}
