package sim

import (
	"context"
	"fmt"
	"os"
	"testing"
	"time"

	libsync "github.com/dmestas/edgesync/go-libfossil/sync"
	"github.com/dmestas/edgesync/leaf/telemetry"
)

// testObserver is the package-level observer used by all sim tests.
// It is nil (interface nil) when OTEL_EXPORTER_OTLP_ENDPOINT is not set,
// so resolveObserver correctly falls back to nopObserver.
var testObserver libsync.Observer

// telemetryShutdown flushes and shuts down the OTel pipeline.
var telemetryShutdown func(context.Context) error

func TestMain(m *testing.M) {
	if os.Getenv("OTEL_EXPORTER_OTLP_ENDPOINT") != "" {
		ctx := context.Background()
		shutdown, err := telemetry.Setup(ctx, telemetry.TelemetryConfig{
			ServiceName: "edgesync-sim",
		})
		if err != nil {
			fmt.Fprintf(os.Stderr, "telemetry setup: %v\n", err)
			os.Exit(1)
		}
		telemetryShutdown = shutdown
		testObserver = telemetry.NewOTelObserver(nil, nil)
	}

	code := m.Run()

	if telemetryShutdown != nil {
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		if err := telemetryShutdown(ctx); err != nil {
			fmt.Fprintf(os.Stderr, "telemetry shutdown: %v\n", err)
		}
		cancel()
	}

	os.Exit(code)
}
