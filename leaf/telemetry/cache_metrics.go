//go:build !wasip1 && !js

package telemetry

import (
	"context"

	"github.com/danmestas/go-libfossil/content"
	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/metric"
)

// RegisterCacheMetrics registers asynchronous OTel instruments that report
// content cache statistics. The cache is read-only from the metric callback,
// keeping go-libfossil free of OTel dependencies.
//
// Pass nil for mp to use the globally registered MeterProvider.
func RegisterCacheMetrics(mp metric.MeterProvider, cache *content.Cache) {
	if cache == nil {
		return
	}
	if mp == nil {
		mp = otel.GetMeterProvider()
	}
	m := mp.Meter(instrumentationName)

	m.Float64ObservableGauge("content.cache.hit_ratio",
		metric.WithDescription("Content cache hit ratio (hits / total lookups)"),
		metric.WithFloat64Callback(func(_ context.Context, o metric.Float64Observer) error {
			s := cache.Stats()
			total := s.Hits + s.Misses
			if total == 0 {
				o.Observe(0)
			} else {
				o.Observe(float64(s.Hits) / float64(total))
			}
			return nil
		}),
	)

	m.Int64ObservableGauge("content.cache.entries",
		metric.WithDescription("Number of entries in the content cache"),
		metric.WithInt64Callback(func(_ context.Context, o metric.Int64Observer) error {
			o.Observe(int64(cache.Stats().Entries))
			return nil
		}),
	)

	m.Int64ObservableGauge("content.cache.size_bytes",
		metric.WithDescription("Current size of the content cache in bytes"),
		metric.WithUnit("By"),
		metric.WithInt64Callback(func(_ context.Context, o metric.Int64Observer) error {
			o.Observe(cache.Stats().Size)
			return nil
		}),
	)

	m.Int64ObservableGauge("content.cache.max_size_bytes",
		metric.WithDescription("Maximum size of the content cache in bytes"),
		metric.WithUnit("By"),
		metric.WithInt64Callback(func(_ context.Context, o metric.Int64Observer) error {
			o.Observe(cache.Stats().MaxSize)
			return nil
		}),
	)

	m.Int64ObservableCounter("content.cache.hits.total",
		metric.WithDescription("Total content cache hits"),
		metric.WithInt64Callback(func(_ context.Context, o metric.Int64Observer) error {
			o.Observe(cache.Stats().Hits)
			return nil
		}),
	)

	m.Int64ObservableCounter("content.cache.misses.total",
		metric.WithDescription("Total content cache misses"),
		metric.WithInt64Callback(func(_ context.Context, o metric.Int64Observer) error {
			o.Observe(cache.Stats().Misses)
			return nil
		}),
	)
}
