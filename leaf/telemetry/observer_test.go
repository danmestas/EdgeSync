//go:build !wasip1 && !js

package telemetry

import (
	"context"
	"testing"

	libsync "github.com/dmestas/edgesync/go-libfossil/sync"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	"go.opentelemetry.io/otel/sdk/trace/tracetest"
)

func TestOTelObserverCreatesSpans(t *testing.T) {
	exporter := tracetest.NewInMemoryExporter()
	tp := sdktrace.NewTracerProvider(
		sdktrace.WithSyncer(exporter),
	)

	obs := NewOTelObserver(tp, nil)

	ctx := context.Background()
	ctx = obs.Started(ctx, libsync.SessionStart{
		Operation:   "sync",
		Push:        true,
		Pull:        true,
		UV:          false,
		ProjectCode: "abc123",
	})
	roundCtx := obs.RoundStarted(ctx, 0)
	obs.RoundCompleted(roundCtx, 0, libsync.RoundStats{FilesSent: 5, FilesReceived: 3})
	obs.Completed(ctx, libsync.SessionEnd{
		Operation:  "sync",
		Rounds:     1,
		FilesSent:  5,
		FilesRecvd: 3,
	}, nil)

	spans := exporter.GetSpans()
	if len(spans) != 2 {
		t.Fatalf("got %d spans, want 2 (session + round)", len(spans))
	}

	roundSpan := spans[0]
	sessionSpan := spans[1]

	if roundSpan.Name != "sync.round" {
		t.Errorf("round span name = %q, want %q", roundSpan.Name, "sync.round")
	}
	if sessionSpan.Name != "sync.session" {
		t.Errorf("session span name = %q, want %q", sessionSpan.Name, "sync.session")
	}
}

func TestOTelObserverCloneSpanNames(t *testing.T) {
	exporter := tracetest.NewInMemoryExporter()
	tp := sdktrace.NewTracerProvider(
		sdktrace.WithSyncer(exporter),
	)

	obs := NewOTelObserver(tp, nil)

	ctx := context.Background()
	ctx = obs.Started(ctx, libsync.SessionStart{Operation: "clone"})
	roundCtx := obs.RoundStarted(ctx, 0)
	obs.RoundCompleted(roundCtx, 0, libsync.RoundStats{FilesReceived: 10})
	obs.Completed(ctx, libsync.SessionEnd{Operation: "clone", Rounds: 1, FilesRecvd: 10}, nil)

	spans := exporter.GetSpans()
	if len(spans) != 2 {
		t.Fatalf("got %d spans, want 2", len(spans))
	}
	if spans[0].Name != "clone.round" {
		t.Errorf("round span name = %q, want %q", spans[0].Name, "clone.round")
	}
	if spans[1].Name != "clone.session" {
		t.Errorf("session span name = %q, want %q", spans[1].Name, "clone.session")
	}
}
