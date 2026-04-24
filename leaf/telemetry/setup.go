//go:build !wasip1 && !js

// Package telemetry provides OpenTelemetry instrumentation for the leaf agent.
package telemetry

import (
	"context"
	"errors"
	"io"
	"log/slog"
	"os"
	"strings"

	"go.opentelemetry.io/contrib/bridges/otelslog"
	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/exporters/otlp/otlplog/otlploghttp"
	"go.opentelemetry.io/otel/exporters/otlp/otlpmetric/otlpmetrichttp"
	"go.opentelemetry.io/otel/exporters/otlp/otlptrace/otlptracehttp"
	"go.opentelemetry.io/otel/log/global"
	sdklog "go.opentelemetry.io/otel/sdk/log"
	sdkmetric "go.opentelemetry.io/otel/sdk/metric"
	"go.opentelemetry.io/otel/sdk/resource"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	semconv "go.opentelemetry.io/otel/semconv/v1.26.0"
)

// TelemetryConfig configures the OTel SDK.
// Empty fields fall back to standard OTEL_* environment variables.
type TelemetryConfig struct {
	ServiceName string
	Endpoint    string
	Headers     map[string]string
}

// stderrSink is the io.Writer used for the stderr leg of the tee'd slog
// handler installed by Setup. It is a package-level var so tests can
// substitute a buffer via setStderrSinkForTest.
var stderrSink io.Writer = os.Stderr

// setStderrSinkForTest swaps stderrSink and returns a restore function.
// Only used from tests.
func setStderrSinkForTest(w io.Writer) func() {
	prev := stderrSink
	stderrSink = w
	return func() { stderrSink = prev }
}

// Setup initializes the OTel SDK (traces, metrics, logs) and configures
// slog with trace correlation. Returns a shutdown function that flushes
// all pending telemetry.
func Setup(ctx context.Context, cfg TelemetryConfig) (shutdown func(context.Context) error, err error) {
	serviceName := cfg.ServiceName
	if serviceName == "" {
		serviceName = "edgesync-leaf"
	}

	res, err := resource.Merge(
		resource.Default(),
		resource.NewSchemaless(
			semconv.ServiceName(serviceName),
		),
	)
	if err != nil {
		return nil, err
	}

	var shutdowns []func(context.Context) error

	// Trace provider
	traceOpts := []otlptracehttp.Option{}
	if cfg.Endpoint != "" {
		traceOpts = append(traceOpts, traceEndpointOpt(cfg.Endpoint))
	}
	if len(cfg.Headers) > 0 {
		traceOpts = append(traceOpts, otlptracehttp.WithHeaders(cfg.Headers))
	}
	traceExp, err := otlptracehttp.New(ctx, traceOpts...)
	if err != nil {
		return nil, err
	}
	tp := sdktrace.NewTracerProvider(
		sdktrace.WithBatcher(traceExp),
		sdktrace.WithResource(res),
	)
	otel.SetTracerProvider(tp)
	shutdowns = append(shutdowns, tp.Shutdown)

	// Metric provider
	metricOpts := []otlpmetrichttp.Option{}
	if cfg.Endpoint != "" {
		metricOpts = append(metricOpts, metricEndpointOpt(cfg.Endpoint))
	}
	if len(cfg.Headers) > 0 {
		metricOpts = append(metricOpts, otlpmetrichttp.WithHeaders(cfg.Headers))
	}
	metricExp, err := otlpmetrichttp.New(ctx, metricOpts...)
	if err != nil {
		return nil, err
	}
	mp := sdkmetric.NewMeterProvider(
		sdkmetric.WithReader(sdkmetric.NewPeriodicReader(metricExp)),
		sdkmetric.WithResource(res),
	)
	otel.SetMeterProvider(mp)
	shutdowns = append(shutdowns, mp.Shutdown)

	// Log provider + slog bridge
	logOpts := []otlploghttp.Option{}
	if cfg.Endpoint != "" {
		logOpts = append(logOpts, logEndpointOpt(cfg.Endpoint))
	}
	if len(cfg.Headers) > 0 {
		logOpts = append(logOpts, otlploghttp.WithHeaders(cfg.Headers))
	}
	logExp, err := otlploghttp.New(ctx, logOpts...)
	if err != nil {
		return nil, err
	}
	lp := sdklog.NewLoggerProvider(
		sdklog.WithProcessor(sdklog.NewBatchProcessor(logExp)),
		sdklog.WithResource(res),
	)
	global.SetLoggerProvider(lp)
	shutdowns = append(shutdowns, lp.Shutdown)

	// Tee slog between stderr (always visible) and the OTel log bridge.
	// Without the stderr leg, if the collector is unreachable or the batcher
	// drops, slog.Error lines disappear and failures become invisible.
	stderrH := slog.NewTextHandler(stderrSink, nil)
	otelH := otelslog.NewHandler("edgesync-leaf", otelslog.WithLoggerProvider(lp))
	slog.SetDefault(slog.New(NewCtxHandler(teeHandler{primary: stderrH, secondary: otelH})))

	return func(ctx context.Context) error {
		var errs []error
		for i := len(shutdowns) - 1; i >= 0; i-- {
			if err := shutdowns[i](ctx); err != nil {
				errs = append(errs, err)
			}
		}
		return errors.Join(errs...)
	}, nil
}

// hasScheme reports whether endpoint begins with "http://" or "https://".
// The OTLP *HTTP exporters' WithEndpoint expects a bare host:port; a full
// URL must go through WithEndpointURL instead.
func hasScheme(endpoint string) bool {
	return strings.HasPrefix(endpoint, "http://") || strings.HasPrefix(endpoint, "https://")
}

func traceEndpointOpt(endpoint string) otlptracehttp.Option {
	if hasScheme(endpoint) {
		return otlptracehttp.WithEndpointURL(endpoint)
	}
	return otlptracehttp.WithEndpoint(endpoint)
}

func metricEndpointOpt(endpoint string) otlpmetrichttp.Option {
	if hasScheme(endpoint) {
		return otlpmetrichttp.WithEndpointURL(endpoint)
	}
	return otlpmetrichttp.WithEndpoint(endpoint)
}

func logEndpointOpt(endpoint string) otlploghttp.Option {
	if hasScheme(endpoint) {
		return otlploghttp.WithEndpointURL(endpoint)
	}
	return otlploghttp.WithEndpoint(endpoint)
}

// teeHandler fans a single slog.Record out to two underlying handlers.
// Used by Setup to keep stderr visibility while also feeding the OTel
// log pipeline.
type teeHandler struct {
	primary   slog.Handler
	secondary slog.Handler
}

func (h teeHandler) Enabled(ctx context.Context, l slog.Level) bool {
	return h.primary.Enabled(ctx, l) || h.secondary.Enabled(ctx, l)
}

func (h teeHandler) Handle(ctx context.Context, r slog.Record) error {
	// Handlers mutate records internally, so hand each its own copy.
	var errs []error
	if h.primary.Enabled(ctx, r.Level) {
		if err := h.primary.Handle(ctx, r.Clone()); err != nil {
			errs = append(errs, err)
		}
	}
	if h.secondary.Enabled(ctx, r.Level) {
		if err := h.secondary.Handle(ctx, r.Clone()); err != nil {
			errs = append(errs, err)
		}
	}
	return errors.Join(errs...)
}

func (h teeHandler) WithAttrs(attrs []slog.Attr) slog.Handler {
	return teeHandler{primary: h.primary.WithAttrs(attrs), secondary: h.secondary.WithAttrs(attrs)}
}

func (h teeHandler) WithGroup(name string) slog.Handler {
	return teeHandler{primary: h.primary.WithGroup(name), secondary: h.secondary.WithGroup(name)}
}
