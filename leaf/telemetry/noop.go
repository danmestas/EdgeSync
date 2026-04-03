//go:build wasip1 || js

package telemetry

import (
	"context"
	"time"

	libfossil "github.com/danmestas/go-libfossil"
)

// TelemetryConfig is a stub for WASM builds.
type TelemetryConfig struct {
	ServiceName    string
	Endpoint       string
	Insecure       bool
	Environment    string
	Version        string
	InstanceID     string
	RepoPath       string
	SampleRatio    float64
	MetricInterval time.Duration
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

func (*OTelObserver) Started(_ libfossil.SessionStart)              {}
func (*OTelObserver) RoundStarted(_ int)                            {}
func (*OTelObserver) RoundCompleted(_ int, _ libfossil.RoundStats)  {}
func (*OTelObserver) Completed(_ libfossil.SessionEnd)              {}
func (*OTelObserver) Error(_ error)                                 {}
func (*OTelObserver) HandleStarted(_ libfossil.HandleStart)         {}
func (*OTelObserver) HandleCompleted(_ libfossil.HandleEnd)         {}
func (*OTelObserver) TableSyncStarted(_ libfossil.TableSyncStart)   {}
func (*OTelObserver) TableSyncCompleted(_ libfossil.TableSyncEnd)   {}
