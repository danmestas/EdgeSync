package agent

import (
	"context"
	"fmt"
	"time"

	"github.com/danmestas/EdgeSync/leaf/agent/internal/natshdr"
	"github.com/nats-io/nats.go"
	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/trace"
)

// NATSTransport implements libfossil.Transport over NATS request/reply.
// The subject follows the pattern "fossil.<project-code>.sync".
type NATSTransport struct {
	conn    *nats.Conn
	subject string        // e.g. "fossil.myproj.sync"
	timeout time.Duration // default 30s
	tracer  trace.Tracer
}

// NewNATSTransport creates a transport that exchanges raw sync payloads over
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

// RoundTrip sends a raw xfer payload as a NATS request and returns the raw reply.
func (t *NATSTransport) RoundTrip(ctx context.Context, payload []byte) ([]byte, error) {
	ctx, span := t.tracer.Start(ctx, "nats.request",
		trace.WithSpanKind(trace.SpanKindClient),
		trace.WithAttributes(
			attribute.String("messaging.system", "nats"),
			attribute.String("messaging.destination.name", t.subject),
		),
	)
	defer span.End()

	span.SetAttributes(attribute.Int("messaging.message.body.size", len(payload)))

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
	msg.Data = payload
	injectTraceContext(ctx, msg)

	reply, err := t.conn.RequestMsgWithContext(ctx, msg)
	if err != nil {
		span.RecordError(err)
		return nil, fmt.Errorf("nats: request on %s: %w", t.subject, err)
	}

	return reply.Data, nil
}

// injectTraceContext propagates W3C trace context into NATS message headers.
func injectTraceContext(ctx context.Context, msg *nats.Msg) {
	if msg.Header == nil {
		msg.Header = nats.Header{}
	}
	otel.GetTextMapPropagator().Inject(ctx, natshdr.Carrier(msg.Header))
}

// extractTraceContext extracts W3C trace context from NATS message headers.
func extractTraceContext(ctx context.Context, msg *nats.Msg) context.Context {
	if msg.Header == nil {
		return ctx
	}
	return otel.GetTextMapPropagator().Extract(ctx, natshdr.Carrier(msg.Header))
}
