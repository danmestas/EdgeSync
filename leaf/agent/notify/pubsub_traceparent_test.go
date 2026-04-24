package notify_test

import (
	"context"
	"testing"
	"time"

	"github.com/danmestas/EdgeSync/leaf/agent/notify"
	"github.com/nats-io/nats.go"
	natsserver "github.com/nats-io/nats-server/v2/server"
	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/propagation"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	"go.opentelemetry.io/otel/trace"
)

// startTestNATSForTP mirrors startTestNATS in pubsub_test.go but lives in the
// _test package so it can be reused across external test files without
// exporting the helper.
func startTestNATSForTP(t *testing.T) string {
	t.Helper()
	opts := &natsserver.Options{
		Host: "127.0.0.1",
		Port: -1,
	}
	ns, err := natsserver.NewServer(opts)
	if err != nil {
		t.Fatalf("new nats server: %v", err)
	}
	ns.Start()
	if !ns.ReadyForConnections(2 * time.Second) {
		t.Fatal("nats server not ready")
	}
	t.Cleanup(ns.Shutdown)
	return ns.ClientURL()
}

// TestPublishCtx_InjectsTraceparent verifies that notify.PublishCtx writes a
// W3C traceparent header onto the NATS message so subscribers can extract the
// publisher's span context and chain a child span off of it.
func TestPublishCtx_InjectsTraceparent(t *testing.T) {
	tp := sdktrace.NewTracerProvider()
	defer tp.Shutdown(context.Background())
	otel.SetTracerProvider(tp)
	otel.SetTextMapPropagator(propagation.TraceContext{})

	url := startTestNATSForTP(t)

	pub, err := nats.Connect(url)
	if err != nil {
		t.Fatalf("connect pub: %v", err)
	}
	defer pub.Close()

	sub, err := nats.Connect(url)
	if err != nil {
		t.Fatalf("connect sub: %v", err)
	}
	defer sub.Close()

	msg := notify.NewMessage(notify.MessageOpts{
		Project:  "edgesync",
		From:     "endpoint-abc123",
		FromName: "claude-macbook",
		Body:     "traceparent test",
	})

	captured := make(chan *nats.Msg, 1)
	if _, err := sub.Subscribe(msg.NATSSubject(), func(m *nats.Msg) {
		captured <- m
	}); err != nil {
		t.Fatalf("subscribe: %v", err)
	}
	if err := sub.Flush(); err != nil {
		t.Fatalf("flush sub: %v", err)
	}

	ctx, span := tp.Tracer("test").Start(context.Background(), "parent")
	defer span.End()

	if err := notify.PublishCtx(ctx, pub, msg); err != nil {
		t.Fatalf("PublishCtx: %v", err)
	}

	var got *nats.Msg
	select {
	case got = <-captured:
	case <-time.After(2 * time.Second):
		t.Fatalf("no message received")
	}

	if got.Header == nil {
		t.Fatalf("expected headers on captured message, got nil")
	}
	if got.Header.Get("traceparent") == "" {
		t.Fatalf("expected traceparent in headers, got none (headers=%v)", got.Header)
	}

	// Use the same public extraction helper subscribers will use. This also
	// exercises natsHeaderCarrier's case-insensitive Get path, since NATS
	// delivers header keys in lowercase on the wire regardless of send case.
	ext := notify.ExtractFromMsg(context.Background(), got)
	sc := trace.SpanContextFromContext(ext)
	if !sc.TraceID().IsValid() {
		t.Fatalf("extracted span context has invalid TraceID (traceparent=%q)",
			got.Header.Get("traceparent"))
	}
	if sc.TraceID() != span.SpanContext().TraceID() {
		t.Fatalf("trace id mismatch: extracted=%s, original=%s",
			sc.TraceID(), span.SpanContext().TraceID())
	}
}

// TestPublishCtx_Roundtrip verifies that a subscriber using ExtractFromMsg
// recovers the publisher's trace id, and that starting a child span off the
// extracted context makes the subscriber span a child of the publisher span
// (same TraceID, subscriber span's parent == publisher span ID).
func TestPublishCtx_Roundtrip(t *testing.T) {
	tp := sdktrace.NewTracerProvider()
	defer tp.Shutdown(context.Background())
	otel.SetTracerProvider(tp)
	otel.SetTextMapPropagator(propagation.TraceContext{})

	url := startTestNATSForTP(t)

	pub, err := nats.Connect(url)
	if err != nil {
		t.Fatalf("connect pub: %v", err)
	}
	defer pub.Close()

	sub, err := nats.Connect(url)
	if err != nil {
		t.Fatalf("connect sub: %v", err)
	}
	defer sub.Close()

	msg := notify.NewMessage(notify.MessageOpts{
		Project:  "edgesync",
		From:     "endpoint-abc123",
		FromName: "claude-macbook",
		Body:     "roundtrip test",
	})

	type result struct {
		traceID  trace.TraceID
		parentID trace.SpanID
	}
	got := make(chan result, 1)
	if _, err := sub.Subscribe(msg.NATSSubject(), func(m *nats.Msg) {
		extCtx := notify.ExtractFromMsg(context.Background(), m)
		_, childSpan := tp.Tracer("sub").Start(extCtx, "notify.handle")
		defer childSpan.End()
		got <- result{
			traceID:  childSpan.SpanContext().TraceID(),
			parentID: trace.SpanContextFromContext(extCtx).SpanID(),
		}
	}); err != nil {
		t.Fatalf("subscribe: %v", err)
	}
	if err := sub.Flush(); err != nil {
		t.Fatalf("flush: %v", err)
	}

	pubCtx, pubSpan := tp.Tracer("pub").Start(context.Background(), "publisher")
	defer pubSpan.End()

	if err := notify.PublishCtx(pubCtx, pub, msg); err != nil {
		t.Fatalf("PublishCtx: %v", err)
	}

	var r result
	select {
	case r = <-got:
	case <-time.After(2 * time.Second):
		t.Fatalf("no message received")
	}

	if r.traceID != pubSpan.SpanContext().TraceID() {
		t.Fatalf("child span trace id = %s, want %s (publisher trace)",
			r.traceID, pubSpan.SpanContext().TraceID())
	}
	if r.parentID != pubSpan.SpanContext().SpanID() {
		t.Fatalf("extracted parent span id = %s, want %s (publisher span id)",
			r.parentID, pubSpan.SpanContext().SpanID())
	}
}

// TestPublish_NoTraceparentWhenNoSpan verifies the backward-compat Publish
// wrapper still works when no span is on the context and no propagator state
// exists. The message should publish successfully even if no traceparent is
// set.
func TestPublish_NoTraceparentWhenNoSpan(t *testing.T) {
	// Reset propagator to a no-op; Publish uses Background ctx so there is
	// no span to inject. A missing traceparent header is acceptable.
	otel.SetTextMapPropagator(propagation.TraceContext{})

	url := startTestNATSForTP(t)

	pub, err := nats.Connect(url)
	if err != nil {
		t.Fatalf("connect pub: %v", err)
	}
	defer pub.Close()

	sub, err := nats.Connect(url)
	if err != nil {
		t.Fatalf("connect sub: %v", err)
	}
	defer sub.Close()

	msg := notify.NewMessage(notify.MessageOpts{
		Project:  "edgesync",
		From:     "endpoint-abc123",
		FromName: "claude-macbook",
		Body:     "no-span compat",
	})

	received := make(chan *nats.Msg, 1)
	if _, err := sub.Subscribe(msg.NATSSubject(), func(m *nats.Msg) {
		received <- m
	}); err != nil {
		t.Fatalf("subscribe: %v", err)
	}
	if err := sub.Flush(); err != nil {
		t.Fatalf("flush: %v", err)
	}

	if err := notify.Publish(pub, msg); err != nil {
		t.Fatalf("Publish: %v", err)
	}

	select {
	case <-received:
	case <-time.After(2 * time.Second):
		t.Fatalf("no message received via Publish")
	}
}
