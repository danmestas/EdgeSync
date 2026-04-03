//go:build !wasip1 && !js

package telemetry

import (
	"strconv"

	"github.com/danmestas/go-libfossil/checkout"
	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/trace"
)

// CheckoutOTelObserver implements libfossil.CheckoutObserver using OpenTelemetry spans.
type CheckoutOTelObserver struct {
	tracer trace.Tracer
	span   trace.Span
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

func (o *CheckoutOTelObserver) ExtractStarted(e checkout.ExtractStart) {
	_, o.span = o.tracer.Start(nil, "checkout.extract",
		trace.WithAttributes(
			attribute.Int64("checkout.target_rid", int64(e.TargetRID)),
			attribute.String("checkout.operation", e.Operation),
		),
	)
}

func (o *CheckoutOTelObserver) ExtractFileCompleted(name string, change checkout.UpdateChange) {
	if o.span != nil {
		o.span.AddEvent("checkout.file", trace.WithAttributes(
			attribute.String("checkout.file.name", name),
			attribute.String("checkout.file.change", strconv.Itoa(int(change))),
		))
	}
}

func (o *CheckoutOTelObserver) ExtractCompleted(e checkout.ExtractEnd) {
	if o.span != nil {
		o.span.SetAttributes(
			attribute.Int("checkout.files_written", e.FilesWritten),
		)
		o.span.End()
		o.span = nil
	}
}

func (o *CheckoutOTelObserver) ScanStarted(dir string) {
	_, o.span = o.tracer.Start(nil, "checkout.scan")
}

func (o *CheckoutOTelObserver) ScanCompleted(e checkout.ScanEnd) {
	if o.span != nil {
		o.span.SetAttributes(
			attribute.Int("checkout.scan.files_scanned", e.FilesScanned),
		)
		o.span.End()
		o.span = nil
	}
}

func (o *CheckoutOTelObserver) CommitStarted(e checkout.CommitStart) {
	_, o.span = o.tracer.Start(nil, "checkout.commit",
		trace.WithAttributes(
			attribute.Int("checkout.commit.files", e.FilesEnqueued),
			attribute.String("checkout.commit.user", e.User),
		),
	)
}

func (o *CheckoutOTelObserver) CommitCompleted(e checkout.CommitEnd) {
	if o.span != nil {
		o.span.SetAttributes(
			attribute.String("checkout.commit.uuid", e.UUID),
			attribute.Int64("checkout.commit.rid", int64(e.RID)),
		)
		o.span.End()
		o.span = nil
	}
}

func (o *CheckoutOTelObserver) Error(err error) {
	if err != nil && o.span != nil {
		o.span.RecordError(err)
	}
}
