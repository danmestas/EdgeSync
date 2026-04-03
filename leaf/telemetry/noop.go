//go:build wasip1 || js

package telemetry

import (
	"context"

	libsync "github.com/danmestas/go-libfossil/sync"
)

// TelemetryConfig is a stub for WASM builds.
type TelemetryConfig struct {
	ServiceName string
	Endpoint    string
	Headers     map[string]string
}

// Setup is a no-op on WASM — returns a no-op shutdown function.
func Setup(_ context.Context, _ TelemetryConfig) (func(context.Context) error, error) {
	return func(context.Context) error { return nil }, nil
}

// OTelObserver is a no-op on WASM builds.
type OTelObserver struct{}

// NewOTelObserver returns a no-op observer on WASM builds.
func NewOTelObserver(_ any, _ any) *OTelObserver {
	return &OTelObserver{}
}

func (*OTelObserver) Started(ctx context.Context, _ libsync.SessionStart) context.Context {
	return ctx
}

func (*OTelObserver) RoundStarted(ctx context.Context, _ int) context.Context {
	return ctx
}

func (*OTelObserver) RoundCompleted(_ context.Context, _ int, _ libsync.RoundStats) {}

func (*OTelObserver) Completed(_ context.Context, _ libsync.SessionEnd, _ error) {}

func (*OTelObserver) Error(_ context.Context, _ error) {}

func (*OTelObserver) HandleStarted(ctx context.Context, _ libsync.HandleStart) context.Context {
	return ctx
}

func (*OTelObserver) HandleCompleted(_ context.Context, _ libsync.HandleEnd) {}

func (*OTelObserver) TableSyncStarted(_ context.Context, _ libsync.TableSyncStart) {}

func (*OTelObserver) TableSyncCompleted(_ context.Context, _ libsync.TableSyncEnd) {}
