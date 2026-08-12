//go:build !wasip1 && !js

package telemetry

import (
	"testing"

	libfossil "github.com/danmestas/go-libfossil"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	"go.opentelemetry.io/otel/sdk/trace/tracetest"
)

func TestOTelObserverCreatesSpans(t *testing.T) {
	exporter := tracetest.NewInMemoryExporter()
	tp := sdktrace.NewTracerProvider(
		sdktrace.WithSyncer(exporter),
	)

	obs := NewOTelObserver(tp, nil)

	obs.Started(libfossil.SessionStart{
		Push:        true,
		Pull:        true,
		UV:          false,
		ProjectCode: "abc123",
	})
	obs.RoundStarted(0)
	obs.RoundCompleted(0, libfossil.RoundStats{FilesSent: 5, FilesRecvd: 3})
	obs.Completed(libfossil.SessionEnd{
		Rounds:     1,
		FilesSent:  5,
		FilesRecvd: 3,
	})

	spans := exporter.GetSpans()
	if len(spans) != 1 {
		t.Fatalf("got %d spans, want 1 (session)", len(spans))
	}

	sessionSpan := spans[0]
	if sessionSpan.Name != "sync.session" {
		t.Errorf("session span name = %q, want %q", sessionSpan.Name, "sync.session")
	}
}
