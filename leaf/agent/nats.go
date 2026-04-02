package agent

import (
	"context"
	"fmt"
	"net/http"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/xfer"
	"github.com/nats-io/nats.go"
	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/propagation"
	"go.opentelemetry.io/otel/trace"
)

// NATSTransport implements sync.Transport over NATS request/reply.
// The subject follows the pattern "fossil.<project-code>.sync".
type NATSTransport struct {
	conn    *nats.Conn
	subject string        // e.g. "fossil.myproj.sync"
	timeout time.Duration // default 30s
	tracer  trace.Tracer
}

// NewNATSTransport creates a transport that exchanges xfer messages over
// the given NATS connection. The prefix defaults to "fossil" if empty.
// If timeout is zero, 30 seconds is used.
func NewNATSTransport(conn *nats.Conn, projectCode string, timeout time.Duration, prefix string) *NATSTransport {
	if timeout == 0 {
		timeout = 30 * time.Second
	}
	if prefix == "" {
		prefix = "fossil"
	}
	return &NATSTransport{
		conn:    conn,
		subject: prefix + "." + projectCode + ".sync",
		timeout: timeout,
		tracer:  otel.Tracer("edgesync-leaf"),
	}
}

// Exchange encodes the request as zlib-compressed xfer bytes, sends it as
// a NATS request, and decodes the zlib-compressed reply.
func (t *NATSTransport) Exchange(ctx context.Context, req *xfer.Message) (*xfer.Message, error) {
	ctx, span := t.tracer.Start(ctx, "nats.request",
		trace.WithSpanKind(trace.SpanKindClient),
		trace.WithAttributes(
			attribute.String("messaging.system", "nats"),
			attribute.String("messaging.destination.name", t.subject),
		),
	)
	defer span.End()

	data, err := req.Encode()
	if err != nil {
		span.RecordError(err)
		return nil, fmt.Errorf("nats: encode request: %w", err)
	}
	span.SetAttributes(attribute.Int("messaging.message.body.size", len(data)))

	// Use a child context with the transport timeout if the parent has no
	// earlier deadline.
	deadline, ok := ctx.Deadline()
	if !ok || time.Until(deadline) > t.timeout {
		var cancel context.CancelFunc
		ctx, cancel = context.WithTimeout(ctx, t.timeout)
		defer cancel()
	}

	// Build NATS message with trace context in headers.
	msg := nats.NewMsg(t.subject)
	msg.Data = data
	injectTraceContext(ctx, msg)

	reply, err := t.conn.RequestMsgWithContext(ctx, msg)
	if err != nil {
		span.RecordError(err)
		return nil, fmt.Errorf("nats: request on %s: %w", t.subject, err)
	}

	resp, err := xfer.Decode(reply.Data)
	if err != nil {
		span.RecordError(err)
		return nil, fmt.Errorf("nats: decode reply: %w", err)
	}
	return resp, nil
}

// injectTraceContext propagates W3C trace context into NATS message headers.
func injectTraceContext(ctx context.Context, msg *nats.Msg) {
	if msg.Header == nil {
		msg.Header = nats.Header{}
	}
	carrier := natsHeaderCarrier(msg.Header)
	otel.GetTextMapPropagator().Inject(ctx, carrier)
}

// extractTraceContext extracts W3C trace context from NATS message headers.
func extractTraceContext(ctx context.Context, msg *nats.Msg) context.Context {
	if msg.Header == nil {
		return ctx
	}
	carrier := natsHeaderCarrier(msg.Header)
	return otel.GetTextMapPropagator().Extract(ctx, carrier)
}

// natsHeaderCarrier adapts nats.Header to propagation.TextMapCarrier.
type natsHeaderCarrier nats.Header

func (c natsHeaderCarrier) Get(key string) string {
	return http.Header(c).Get(key)
}

func (c natsHeaderCarrier) Set(key, value string) {
	http.Header(c).Set(key, value)
}

func (c natsHeaderCarrier) Keys() []string {
	keys := make([]string, 0, len(c))
	for k := range c {
		keys = append(keys, k)
	}
	return keys
}

// Verify natsHeaderCarrier implements TextMapCarrier at compile time.
var _ propagation.TextMapCarrier = natsHeaderCarrier(nil)
