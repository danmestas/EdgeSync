//go:build !wasip1 && !js

// Package telemetry provides OpenTelemetry instrumentation for the leaf agent.
package telemetry

import (
	"context"
	"errors"
	"log/slog"
	"os"
	"time"

	"go.opentelemetry.io/contrib/bridges/otelslog"
	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/exporters/otlp/otlplog/otlploggrpc"
	"go.opentelemetry.io/otel/exporters/otlp/otlpmetric/otlpmetricgrpc"
	"go.opentelemetry.io/otel/exporters/otlp/otlptrace/otlptracegrpc"
	"go.opentelemetry.io/otel/log/global"
	"go.opentelemetry.io/otel/propagation"
	sdklog "go.opentelemetry.io/otel/sdk/log"
	sdkmetric "go.opentelemetry.io/otel/sdk/metric"
	"go.opentelemetry.io/otel/sdk/resource"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	semconv "go.opentelemetry.io/otel/semconv/v1.26.0"
)

// TelemetryConfig configures the OTel SDK.
// Empty fields fall back to standard OTEL_* environment variables.
type TelemetryConfig struct {
	ServiceName    string
	Endpoint       string         // gRPC endpoint, e.g. "100.105.156.92:4317"
	Insecure       bool           // skip TLS (for Tailscale-internal endpoints)
	Environment    string         // deployment.environment resource attribute
	Version        string         // service.version resource attribute
	InstanceID     string         // service.instance.id (PeerID or hostname)
	RepoPath       string         // fossil repo path (resource attribute)
	SampleRatio    float64        // 0.0-1.0, default 1.0 (always sample)
	MetricInterval time.Duration  // metric export interval (0 defaults to 60s)
}

// Setup initializes the OTel SDK (traces, metrics, logs) and configures
// slog with trace correlation. Returns a shutdown function that flushes
// all pending telemetry.
func Setup(ctx context.Context, cfg TelemetryConfig) (shutdown func(context.Context) error, err error) {
	serviceName := cfg.ServiceName
	if serviceName == "" {
		serviceName = "edgesync-leaf"
	}

	// Build resource attributes.
	attrs := []attribute.KeyValue{
		semconv.ServiceName(serviceName),
		semconv.ServiceNamespace("edgesync"),
	}
	if cfg.Version != "" {
		attrs = append(attrs, semconv.ServiceVersion(cfg.Version))
	}
	if cfg.InstanceID != "" {
		attrs = append(attrs, semconv.ServiceInstanceID(cfg.InstanceID))
	} else if hostname, herr := os.Hostname(); herr == nil && hostname != "" {
		attrs = append(attrs, semconv.ServiceInstanceID(hostname))
	}
	if cfg.Environment != "" {
		attrs = append(attrs, semconv.DeploymentEnvironment(cfg.Environment))
	}
	if hostname, herr := os.Hostname(); herr == nil && hostname != "" {
		attrs = append(attrs, semconv.HostName(hostname))
	}
	if cfg.RepoPath != "" {
		attrs = append(attrs, attribute.String("edgesync.repo.path", cfg.RepoPath))
	}

	res, err := resource.Merge(
		resource.Default(),
		resource.NewSchemaless(attrs...),
	)
	if err != nil {
		return nil, err
	}

	// Set up W3C trace context propagation for cross-service linking.
	otel.SetTextMapPropagator(propagation.NewCompositeTextMapPropagator(
		propagation.TraceContext{},
		propagation.Baggage{},
	))

	var shutdowns []func(context.Context) error

	// Trace provider (gRPC)
	traceOpts := []otlptracegrpc.Option{}
	if cfg.Endpoint != "" {
		traceOpts = append(traceOpts, otlptracegrpc.WithEndpoint(cfg.Endpoint))
	}
	if cfg.Insecure {
		traceOpts = append(traceOpts, otlptracegrpc.WithInsecure())
	}
	traceExp, err := otlptracegrpc.New(ctx, traceOpts...)
	if err != nil {
		return nil, err
	}

	sampler := sdktrace.AlwaysSample()
	if cfg.SampleRatio > 0 && cfg.SampleRatio < 1.0 {
		sampler = sdktrace.ParentBased(sdktrace.TraceIDRatioBased(cfg.SampleRatio))
	}

	tp := sdktrace.NewTracerProvider(
		sdktrace.WithBatcher(traceExp),
		sdktrace.WithResource(res),
		sdktrace.WithSampler(sampler),
	)
	otel.SetTracerProvider(tp)
	shutdowns = append(shutdowns, tp.Shutdown)

	// Metric provider (gRPC)
	metricOpts := []otlpmetricgrpc.Option{}
	if cfg.Endpoint != "" {
		metricOpts = append(metricOpts, otlpmetricgrpc.WithEndpoint(cfg.Endpoint))
	}
	if cfg.Insecure {
		metricOpts = append(metricOpts, otlpmetricgrpc.WithInsecure())
	}
	metricExp, err := otlpmetricgrpc.New(ctx, metricOpts...)
	if err != nil {
		return nil, err
	}
	readerOpts := []sdkmetric.PeriodicReaderOption{}
	if cfg.MetricInterval > 0 {
		readerOpts = append(readerOpts, sdkmetric.WithInterval(cfg.MetricInterval))
	}
	mp := sdkmetric.NewMeterProvider(
		sdkmetric.WithReader(sdkmetric.NewPeriodicReader(metricExp, readerOpts...)),
		sdkmetric.WithResource(res),
	)
	otel.SetMeterProvider(mp)
	shutdowns = append(shutdowns, mp.Shutdown)

	// Log provider (gRPC) + slog bridge
	logOpts := []otlploggrpc.Option{}
	if cfg.Endpoint != "" {
		logOpts = append(logOpts, otlploggrpc.WithEndpoint(cfg.Endpoint))
	}
	if cfg.Insecure {
		logOpts = append(logOpts, otlploggrpc.WithInsecure())
	}
	logExp, err := otlploggrpc.New(ctx, logOpts...)
	if err != nil {
		return nil, err
	}
	lp := sdklog.NewLoggerProvider(
		sdklog.WithProcessor(sdklog.NewBatchProcessor(logExp)),
		sdklog.WithResource(res),
	)
	global.SetLoggerProvider(lp)
	shutdowns = append(shutdowns, lp.Shutdown)

	logger := otelslog.NewLogger("edgesync-leaf")
	slog.SetDefault(logger)

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
