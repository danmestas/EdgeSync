//go:build wasip1 || js

package telemetry

// RegisterCacheMetrics is a no-op on WASM builds.
func RegisterCacheMetrics(_ any, _ any) {}
