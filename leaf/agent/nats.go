package agent

import (
	"context"
	"fmt"
	"net/http"
	"strings"
	"time"

	"github.com/nats-io/nats.go"
	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/propagation"
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
//
// NATS preserves header case on the send side but delivers headers
// lower-cased on the receive side, so Get must be case-insensitive;
// otherwise subscribers miss injected "traceparent" because the wire
// delivers it as "traceparent" while http.Header.Get canonicalizes the
// lookup key to "Traceparent".
type natsHeaderCarrier nats.Header

func (c natsHeaderCarrier) Get(key string) string {
	if v := http.Header(c).Get(key); v != "" {
		return v
	}
	if vs, ok := c[key]; ok && len(vs) > 0 {
		return vs[0]
	}
	lower := strings.ToLower(key)
	if vs, ok := c[lower]; ok && len(vs) > 0 {
		return vs[0]
	}
	for k, vs := range c {
		if strings.EqualFold(k, key) && len(vs) > 0 {
			return vs[0]
		}
	}
	return ""
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
