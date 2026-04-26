//go:build !wasip1 && !js

package telemetry

import (
	"bytes"
	"context"
	"log/slog"
	"strings"
	"testing"
)

// TestSetup_EndpointWithScheme verifies Setup accepts an endpoint with an
// http:// scheme without producing a "double http" URL parse error.
func TestSetup_EndpointWithScheme(t *testing.T) {
	ctx := context.Background()
	cfg := TelemetryConfig{
		ServiceName: "test-leaf",
		Endpoint:    "http://signoz.example.com:4317",
	}
	shutdown, err := Setup(ctx, cfg)
	if err != nil {
		t.Fatalf("Setup with http:// scheme returned error: %v", err)
	}
	if shutdown == nil {
		t.Fatalf("Setup returned nil shutdown fn")
	}
	// Shutdown immediately; exporters shouldn't dial on construction.
	_ = shutdown(ctx)
}

// TestSetup_EndpointWithHTTPSScheme verifies https:// is handled symmetrically.
func TestSetup_EndpointWithHTTPSScheme(t *testing.T) {
	ctx := context.Background()
	cfg := TelemetryConfig{
		ServiceName: "test-leaf",
		Endpoint:    "https://otel.example.com:4318",
	}
	shutdown, err := Setup(ctx, cfg)
	if err != nil {
		t.Fatalf("Setup with https:// scheme returned error: %v", err)
	}
	_ = shutdown(ctx)
}

// TestSetup_EndpointHostPort verifies bare host:port still works
// (the existing behavior pre-fix).
func TestSetup_EndpointHostPort(t *testing.T) {
	ctx := context.Background()
	cfg := TelemetryConfig{
		ServiceName: "test-leaf",
		Endpoint:    "signoz.example.com:4317",
	}
	shutdown, err := Setup(ctx, cfg)
	if err != nil {
		t.Fatalf("Setup with bare host:port returned error: %v", err)
	}
	_ = shutdown(ctx)
}

// TestSetup_EmptyEndpoint verifies an empty endpoint still falls back to
// default/env handling without error.
func TestSetup_EmptyEndpoint(t *testing.T) {
	ctx := context.Background()
	cfg := TelemetryConfig{
		ServiceName: "test-leaf",
		Endpoint:    "",
	}
	shutdown, err := Setup(ctx, cfg)
	if err != nil {
		t.Fatalf("Setup with empty endpoint returned error: %v", err)
	}
	_ = shutdown(ctx)
}

// TestTeeHandler_WritesToBothSinks verifies a record routed through teeHandler
// reaches both attached handlers.
func TestTeeHandler_WritesToBothSinks(t *testing.T) {
	var aBuf, bBuf bytes.Buffer
	a := slog.NewTextHandler(&aBuf, nil)
	b := slog.NewTextHandler(&bBuf, nil)
	h := teeHandler{primary: a, secondary: b}

	logger := slog.New(h)
	logger.Error("hello-tee", "k", "v")

	if !strings.Contains(aBuf.String(), "hello-tee") {
		t.Errorf("primary handler did not receive record: %q", aBuf.String())
	}
	if !strings.Contains(bBuf.String(), "hello-tee") {
		t.Errorf("secondary handler did not receive record: %q", bBuf.String())
	}
	if !strings.Contains(aBuf.String(), "k=v") || !strings.Contains(bBuf.String(), "k=v") {
		t.Errorf("attribute missing: primary=%q secondary=%q", aBuf.String(), bBuf.String())
	}
}

// TestTeeHandler_WithAttrsAndGroup verifies WithAttrs/WithGroup propagate to
// both underlying handlers.
func TestTeeHandler_WithAttrsAndGroup(t *testing.T) {
	var aBuf, bBuf bytes.Buffer
	a := slog.NewTextHandler(&aBuf, nil)
	b := slog.NewTextHandler(&bBuf, nil)
	h := teeHandler{primary: a, secondary: b}

	logger := slog.New(h).With("component", "leaf").WithGroup("sub")
	logger.Info("grouped-msg", "x", 1)

	for name, buf := range map[string]*bytes.Buffer{"primary": &aBuf, "secondary": &bBuf} {
		s := buf.String()
		if !strings.Contains(s, "grouped-msg") {
			t.Errorf("%s: missing message: %q", name, s)
		}
		if !strings.Contains(s, "component=leaf") {
			t.Errorf("%s: missing WithAttrs attr: %q", name, s)
		}
		if !strings.Contains(s, "sub.x=1") {
			t.Errorf("%s: missing grouped attr: %q", name, s)
		}
	}
}

// TestSetup_DefaultLoggerWritesToStderr verifies that after Setup, the default
// slog logger writes to the supplied stderr sink (so errors don't vanish into
// the OTel void when the collector is unreachable).
//
// We inject the stderr sink via a package-level var hook to avoid having to
// manipulate os.Stderr in tests.
func TestSetup_DefaultLoggerWritesToStderr(t *testing.T) {
	var buf bytes.Buffer
	restore := setStderrSinkForTest(&buf)
	defer restore()

	// Save & restore the real default logger so we don't pollute other tests.
	prev := slog.Default()
	defer slog.SetDefault(prev)

	ctx := context.Background()
	shutdown, err := Setup(ctx, TelemetryConfig{ServiceName: "test-leaf"})
	if err != nil {
		t.Fatalf("Setup: %v", err)
	}
	defer func() { _ = shutdown(ctx) }()

	slog.Error("stderr-visible-error", "reason", "boom")

	got := buf.String()
	if !strings.Contains(got, "stderr-visible-error") {
		t.Errorf("stderr sink did not receive slog.Error: %q", got)
	}
	if !strings.Contains(got, "reason=boom") {
		t.Errorf("stderr sink missing attribute: %q", got)
	}
}
