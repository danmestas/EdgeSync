//go:build wasip1 || js

package telemetry

import (
	"github.com/danmestas/go-libfossil/checkout"
)

// CheckoutOTelObserver is a no-op on WASM builds.
type CheckoutOTelObserver struct{}

// NewCheckoutOTelObserver returns a no-op observer on WASM builds.
func NewCheckoutOTelObserver(_ any) *CheckoutOTelObserver {
	return &CheckoutOTelObserver{}
}

func (*CheckoutOTelObserver) ExtractStarted(_ checkout.ExtractStart)            {}
func (*CheckoutOTelObserver) ExtractFileCompleted(_ string, _ checkout.UpdateChange) {}
func (*CheckoutOTelObserver) ExtractCompleted(_ checkout.ExtractEnd)             {}
func (*CheckoutOTelObserver) ScanStarted(_ string)                                {}
func (*CheckoutOTelObserver) ScanCompleted(_ checkout.ScanEnd)                   {}
func (*CheckoutOTelObserver) CommitStarted(_ checkout.CommitStart)               {}
func (*CheckoutOTelObserver) CommitCompleted(_ checkout.CommitEnd)               {}
func (*CheckoutOTelObserver) Error(_ error)                                       {}
