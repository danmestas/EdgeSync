//go:build !wasip1 && !js

package telemetry

import (
	"context"

	"github.com/dmestas/edgesync/go-libfossil/checkout"
	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/codes"
	"go.opentelemetry.io/otel/trace"
)

// CheckoutOTelObserver implements checkout.Observer using OpenTelemetry spans.
type CheckoutOTelObserver struct {
	tracer trace.Tracer
}

// NewCheckoutOTelObserver creates a CheckoutOTelObserver. Pass nil for tp
// to use the globally registered TracerProvider.
func NewCheckoutOTelObserver(tp trace.TracerProvider) *CheckoutOTelObserver {
	if tp == nil {
		tp = otel.GetTracerProvider()
	}
	return &CheckoutOTelObserver{
		tracer: tp.Tracer(instrumentationName),
	}
}

func (o *CheckoutOTelObserver) ExtractStarted(ctx context.Context, e checkout.ExtractStart) context.Context {
	ctx, _ = o.tracer.Start(ctx, "checkout.extract",
		trace.WithAttributes(
			attribute.String("checkout.operation", e.Operation),
			attribute.Int64("checkout.target_rid", int64(e.TargetRID)),
		),
	)
	return ctx
}

func (o *CheckoutOTelObserver) ExtractFileCompleted(ctx context.Context, name string, change checkout.UpdateChange) {
	span := trace.SpanFromContext(ctx)
	span.AddEvent("checkout.file", trace.WithAttributes(
		attribute.String("checkout.file.name", name),
		attribute.Int("checkout.file.change", int(change)),
	))
}

func (o *CheckoutOTelObserver) ExtractCompleted(ctx context.Context, e checkout.ExtractEnd) {
	span := trace.SpanFromContext(ctx)
	span.SetAttributes(
		attribute.Int("checkout.files_written", e.FilesWritten),
		attribute.Int("checkout.files_removed", e.FilesRemoved),
		attribute.Int("checkout.conflicts", e.Conflicts),
	)
	if e.Err != nil {
		span.RecordError(e.Err)
		span.SetStatus(codes.Error, e.Err.Error())
	}
	span.End()
}

func (o *CheckoutOTelObserver) ScanStarted(ctx context.Context) context.Context {
	ctx, _ = o.tracer.Start(ctx, "checkout.scan")
	return ctx
}

func (o *CheckoutOTelObserver) ScanCompleted(ctx context.Context, e checkout.ScanEnd) {
	span := trace.SpanFromContext(ctx)
	span.SetAttributes(
		attribute.Int("checkout.scan.files_scanned", e.FilesScanned),
		attribute.Int("checkout.scan.files_changed", e.FilesChanged),
		attribute.Int("checkout.scan.files_missing", e.FilesMissing),
		attribute.Int("checkout.scan.files_extra", e.FilesExtra),
	)
	span.End()
}

func (o *CheckoutOTelObserver) CommitStarted(ctx context.Context, e checkout.CommitStart) context.Context {
	ctx, _ = o.tracer.Start(ctx, "checkout.commit",
		trace.WithAttributes(
			attribute.Int("checkout.commit.files_enqueued", e.FilesEnqueued),
			attribute.String("checkout.commit.branch", e.Branch),
			attribute.String("checkout.commit.user", e.User),
		),
	)
	return ctx
}

func (o *CheckoutOTelObserver) CommitCompleted(ctx context.Context, e checkout.CommitEnd) {
	span := trace.SpanFromContext(ctx)
	span.SetAttributes(
		attribute.String("checkout.commit.uuid", e.UUID),
		attribute.Int("checkout.commit.files_commit", e.FilesCommit),
	)
	if e.Err != nil {
		span.RecordError(e.Err)
		span.SetStatus(codes.Error, e.Err.Error())
	}
	span.End()
}

func (o *CheckoutOTelObserver) Error(ctx context.Context, err error) {
	if err == nil {
		return
	}
	span := trace.SpanFromContext(ctx)
	span.AddEvent("checkout.error", trace.WithAttributes(
		attribute.String("error.message", err.Error()),
	))
}
