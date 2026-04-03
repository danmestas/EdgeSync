//go:build wasip1 || js

package telemetry

import (
	libfossil "github.com/danmestas/go-libfossil"
)

// CheckoutOTelObserver is a no-op on WASM builds.
type CheckoutOTelObserver struct{}

// NewCheckoutOTelObserver returns a no-op observer on WASM builds.
func NewCheckoutOTelObserver(_ any) *CheckoutOTelObserver {
	return &CheckoutOTelObserver{}
}

func (*CheckoutOTelObserver) ExtractStarted(_ libfossil.ExtractStart)            {}
func (*CheckoutOTelObserver) ExtractFileCompleted(_ string, _ libfossil.UpdateChange) {}
func (*CheckoutOTelObserver) ExtractCompleted(_ libfossil.ExtractEnd)             {}
func (*CheckoutOTelObserver) ScanStarted(_ string)                                {}
func (*CheckoutOTelObserver) ScanCompleted(_ libfossil.ScanEnd)                   {}
func (*CheckoutOTelObserver) CommitStarted(_ libfossil.CommitStart)               {}
func (*CheckoutOTelObserver) CommitCompleted(_ libfossil.CommitEnd)               {}
func (*CheckoutOTelObserver) Error(_ error)                                       {}
