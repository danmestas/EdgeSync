//go:build !wasip1 && !js

package telemetry

import (
	"context"
	"time"

	libsync "github.com/danmestas/go-libfossil/sync"
	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/codes"
	"go.opentelemetry.io/otel/metric"
	"go.opentelemetry.io/otel/trace"
)

const instrumentationName = "edgesync-leaf"

type operationKey struct{}
type startTimeKey struct{}

func withOperation(ctx context.Context, op string) context.Context {
	return context.WithValue(ctx, operationKey{}, op)
}

func operationFromContext(ctx context.Context) string {
	if op, ok := ctx.Value(operationKey{}).(string); ok {
		return op
	}
	return "sync"
}

// OTelObserver implements sync.Observer using OpenTelemetry spans and metrics.
type OTelObserver struct {
	tracer trace.Tracer

	sessionsTotal metric.Int64Counter
	errorsTotal   metric.Int64Counter
	duration      metric.Float64Histogram
	rounds        metric.Int64Histogram
	filesSent     metric.Int64Histogram
	filesRecvd    metric.Int64Histogram
	uvFilesSent   metric.Int64Histogram
	uvFilesRecvd  metric.Int64Histogram
	bytesSent     metric.Int64Histogram
	bytesRecvd    metric.Int64Histogram
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
	obs.uvFilesSent, _ = m.Int64Histogram("sync.uv.files.sent",
		metric.WithDescription("UV files sent per session"))
	obs.uvFilesRecvd, _ = m.Int64Histogram("sync.uv.files.received",
		metric.WithDescription("UV files received per session"))
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

func (o *OTelObserver) Started(ctx context.Context, info libsync.SessionStart) context.Context {
	ctx = withOperation(ctx, info.Operation)
	ctx = context.WithValue(ctx, startTimeKey{}, time.Now())
	spanAttrs := []attribute.KeyValue{
		attribute.String("sync.operation", info.Operation),
		attribute.Bool("sync.push", info.Push),
		attribute.Bool("sync.pull", info.Pull),
		attribute.Bool("sync.uv", info.UV),
		attribute.String("sync.project_code", info.ProjectCode),
	}
	if info.PeerID != "" {
		spanAttrs = append(spanAttrs, attribute.String("sync.peer_id", info.PeerID))
	}
	ctx, _ = o.tracer.Start(ctx, info.Operation+".session",
		trace.WithAttributes(spanAttrs...),
	)
	return ctx
}

func (o *OTelObserver) RoundStarted(ctx context.Context, round int) context.Context {
	op := operationFromContext(ctx)
	ctx, _ = o.tracer.Start(ctx, op+".round",
		trace.WithAttributes(
			attribute.Int("sync.round", round),
		),
	)
	return ctx
}

func (o *OTelObserver) RoundCompleted(ctx context.Context, round int, stats libsync.RoundStats) {
	span := trace.SpanFromContext(ctx)
	span.SetAttributes(
		attribute.Int("sync.round.files_sent", stats.FilesSent),
		attribute.Int("sync.round.files_received", stats.FilesReceived),
		attribute.Int("sync.round.gimmes_sent", stats.GimmesSent),
		attribute.Int("sync.round.igots_sent", stats.IgotsSent),
		attribute.Int64("sync.round.bytes_sent", stats.BytesSent),
		attribute.Int64("sync.round.bytes_received", stats.BytesReceived),
	)
	span.End()
}

func (o *OTelObserver) Completed(ctx context.Context, info libsync.SessionEnd, err error) {
	span := trace.SpanFromContext(ctx)
	span.SetAttributes(
		attribute.Int("sync.rounds", info.Rounds),
		attribute.Int("sync.files_sent", info.FilesSent),
		attribute.Int("sync.files_received", info.FilesRecvd),
		attribute.Int("sync.uv_files_sent", info.UVFilesSent),
		attribute.Int("sync.uv_files_received", info.UVFilesRecvd),
		attribute.Int64("sync.bytes_sent", info.BytesSent),
		attribute.Int64("sync.bytes_received", info.BytesRecvd),
		attribute.Int("sync.errors_count", len(info.Errors)),
	)
	if err != nil {
		span.RecordError(err)
		span.SetStatus(codes.Error, err.Error())
	}
	for _, errMsg := range info.Errors {
		span.AddEvent("sync.protocol_error", trace.WithAttributes(
			attribute.String("error.message", errMsg),
		))
	}
	span.End()

	attrs := metric.WithAttributes(
		attribute.String("sync.operation", info.Operation),
		attribute.String("sync.project_code", info.ProjectCode),
	)
	o.sessionsTotal.Add(ctx, 1, attrs)
	if err != nil {
		o.errorsTotal.Add(ctx, 1, attrs)
	}
	if startTime, ok := ctx.Value(startTimeKey{}).(time.Time); ok {
		o.duration.Record(ctx, time.Since(startTime).Seconds(), attrs)
	}
	o.rounds.Record(ctx, int64(info.Rounds), attrs)
	o.filesSent.Record(ctx, int64(info.FilesSent), attrs)
	o.filesRecvd.Record(ctx, int64(info.FilesRecvd), attrs)
	o.uvFilesSent.Record(ctx, int64(info.UVFilesSent), attrs)
	o.uvFilesRecvd.Record(ctx, int64(info.UVFilesRecvd), attrs)
	o.bytesSent.Record(ctx, info.BytesSent, attrs)
	o.bytesRecvd.Record(ctx, info.BytesRecvd, attrs)
}

func (o *OTelObserver) Error(ctx context.Context, err error) {
	if err == nil {
		return
	}
	span := trace.SpanFromContext(ctx)
	span.AddEvent("sync.error", trace.WithAttributes(
		attribute.String("error.message", err.Error()),
	))
}

func (o *OTelObserver) HandleStarted(ctx context.Context, info libsync.HandleStart) context.Context {
	ctx, _ = o.tracer.Start(ctx, "sync.handle",
		trace.WithAttributes(
			attribute.String("sync.operation", info.Operation),
			attribute.String("sync.project_code", info.ProjectCode),
			attribute.String("net.peer.addr", info.RemoteAddr),
		),
		trace.WithSpanKind(trace.SpanKindServer),
	)
	return ctx
}

func (o *OTelObserver) HandleCompleted(ctx context.Context, info libsync.HandleEnd) {
	span := trace.SpanFromContext(ctx)
	span.SetAttributes(
		attribute.Int("sync.handle.cards_processed", info.CardsProcessed),
		attribute.Int("sync.handle.files_sent", info.FilesSent),
		attribute.Int("sync.handle.files_received", info.FilesReceived),
	)
	if info.Err != nil {
		span.RecordError(info.Err)
		span.SetStatus(codes.Error, info.Err.Error())
	}
	span.End()
}

func (o *OTelObserver) TableSyncStarted(ctx context.Context, info libsync.TableSyncStart) {
	span := trace.SpanFromContext(ctx)
	span.AddEvent("sync.table_sync.started", trace.WithAttributes(
		attribute.String("sync.table", info.Table),
		attribute.Int("sync.table.local_rows", info.LocalRows),
	))
}

func (o *OTelObserver) TableSyncCompleted(ctx context.Context, info libsync.TableSyncEnd) {
	span := trace.SpanFromContext(ctx)
	span.AddEvent("sync.table_sync.completed", trace.WithAttributes(
		attribute.String("sync.table", info.Table),
		attribute.Int("sync.table.rows_sent", info.Sent),
		attribute.Int("sync.table.rows_received", info.Received),
	))
}
