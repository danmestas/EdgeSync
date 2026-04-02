//go:build wasip1 || js

package telemetry

import (
	"context"

	"github.com/danmestas/go-libfossil/checkout"
)

// CheckoutOTelObserver is a no-op on WASM builds.
type CheckoutOTelObserver struct{}

// NewCheckoutOTelObserver returns a no-op observer on WASM builds.
func NewCheckoutOTelObserver(_ any) *CheckoutOTelObserver {
	return &CheckoutOTelObserver{}
}

func (*CheckoutOTelObserver) ExtractStarted(ctx context.Context, _ checkout.ExtractStart) context.Context {
	return ctx
}
func (*CheckoutOTelObserver) ExtractFileCompleted(_ context.Context, _ string, _ checkout.UpdateChange) {
}
func (*CheckoutOTelObserver) ExtractCompleted(_ context.Context, _ checkout.ExtractEnd)   {}
func (*CheckoutOTelObserver) ScanStarted(ctx context.Context) context.Context             { return ctx }
func (*CheckoutOTelObserver) ScanCompleted(_ context.Context, _ checkout.ScanEnd)         {}
func (*CheckoutOTelObserver) CommitStarted(ctx context.Context, _ checkout.CommitStart) context.Context {
	return ctx
}
func (*CheckoutOTelObserver) CommitCompleted(_ context.Context, _ checkout.CommitEnd) {}
func (*CheckoutOTelObserver) Error(_ context.Context, _ error)                        {}
