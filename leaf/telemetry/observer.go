//go:build !wasip1 && !js

package telemetry

import (
	"sync"
	"time"

	libfossil "github.com/danmestas/libfossil"
	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/metric"
	"go.opentelemetry.io/otel/trace"
)

const instrumentationName = "edgesync-leaf"

// OTelObserver implements libfossil.SyncObserver using OpenTelemetry spans and metrics.
type OTelObserver struct {
	tracer trace.Tracer

	sessionsTotal metric.Int64Counter
	errorsTotal   metric.Int64Counter
	duration      metric.Float64Histogram
	rounds        metric.Int64Histogram
	filesSent     metric.Int64Histogram
	filesRecvd    metric.Int64Histogram
	bytesSent     metric.Int64Histogram
	bytesRecvd    metric.Int64Histogram

	mu        sync.Mutex
	startTime time.Time
	span      trace.Span
}

// NewOTelObserver creates an OTelObserver. Pass nil for either provider
// to use the globally registered provider (set by Setup).
func NewOTelObserver(tp trace.TracerProvider, mp metric.MeterProvider) *OTelObserver {
	if tp == nil {
		tp = otel.GetTracerProvider()
	}
	if mp == nil {
		mp = otel.GetMeterProvider()
	}
	m := mp.Meter(instrumentationName)
	obs := &OTelObserver{
		tracer: tp.Tracer(instrumentationName),
	}
	obs.sessionsTotal, _ = m.Int64Counter("sync.sessions.total",
		metric.WithDescription("Total sync/clone sessions"))
	obs.errorsTotal, _ = m.Int64Counter("sync.errors.total",
		metric.WithDescription("Sessions ending with error"))
	obs.duration, _ = m.Float64Histogram("sync.duration.seconds",
		metric.WithDescription("End-to-end session duration"),
		metric.WithUnit("s"),
		metric.WithExplicitBucketBoundaries(0.01, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10, 30, 60, 120))
	obs.rounds, _ = m.Int64Histogram("sync.rounds",
		metric.WithDescription("Rounds to convergence"))
	obs.filesSent, _ = m.Int64Histogram("sync.files.sent",
		metric.WithDescription("Files sent per session"))
	obs.filesRecvd, _ = m.Int64Histogram("sync.files.received",
		metric.WithDescription("Files received per session"))
	obs.bytesSent, _ = m.Int64Histogram("sync.bytes.sent",
		metric.WithDescription("Bytes sent per session"),
		metric.WithUnit("By"),
		metric.WithExplicitBucketBoundaries(1024, 4096, 16384, 65536, 262144, 1048576, 4194304, 16777216))
	obs.bytesRecvd, _ = m.Int64Histogram("sync.bytes.received",
		metric.WithDescription("Bytes received per session"),
		metric.WithUnit("By"),
		metric.WithExplicitBucketBoundaries(1024, 4096, 16384, 65536, 262144, 1048576, 4194304, 16777216))
	return obs
}

func (o *OTelObserver) Started(info libfossil.SessionStart) {
	o.mu.Lock()
	defer o.mu.Unlock()
	o.startTime = time.Now()
	_, o.span = o.tracer.Start(nil, "sync.session",
		trace.WithAttributes(
			attribute.String("sync.project_code", info.ProjectCode),
			attribute.Bool("sync.push", info.Push),
			attribute.Bool("sync.pull", info.Pull),
			attribute.Bool("sync.uv", info.UV),
		),
	)
}

func (o *OTelObserver) RoundStarted(round int) {}

func (o *OTelObserver) RoundCompleted(round int, stats libfossil.RoundStats) {}

func (o *OTelObserver) Completed(info libfossil.SessionEnd) {
	o.mu.Lock()
	defer o.mu.Unlock()

	attrs := metric.WithAttributes(
		attribute.String("sync.operation", "sync"),
	)
	o.sessionsTotal.Add(nil, 1, attrs)
	if !o.startTime.IsZero() {
		o.duration.Record(nil, time.Since(o.startTime).Seconds(), attrs)
	}
	o.rounds.Record(nil, int64(info.Rounds), attrs)
	o.filesSent.Record(nil, int64(info.FilesSent), attrs)
	o.filesRecvd.Record(nil, int64(info.FilesRecvd), attrs)

	if o.span != nil {
		o.span.SetAttributes(
			attribute.Int("sync.rounds", info.Rounds),
			attribute.Int("sync.files_sent", info.FilesSent),
			attribute.Int("sync.files_received", info.FilesRecvd),
		)
		o.span.End()
		o.span = nil
	}
}

func (o *OTelObserver) Error(err error) {
	if err == nil {
		return
	}
	o.mu.Lock()
	defer o.mu.Unlock()
	o.errorsTotal.Add(nil, 1)
	if o.span != nil {
		o.span.RecordError(err)
	}
}

func (o *OTelObserver) HandleStarted(info libfossil.HandleStart)   {}
func (o *OTelObserver) HandleCompleted(info libfossil.HandleEnd)   {}
func (o *OTelObserver) TableSyncStarted(info libfossil.TableSyncStart)   {}
func (o *OTelObserver) TableSyncCompleted(info libfossil.TableSyncEnd)   {}
