//go:build wasip1 || js

package telemetry

import "github.com/dmestas/edgesync/go-libfossil/content"

// RegisterCacheMetrics is a no-op on WASM builds.
func RegisterCacheMetrics(_ any, _ *content.Cache) {}
