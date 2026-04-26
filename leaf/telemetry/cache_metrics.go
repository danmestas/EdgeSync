//go:build !wasip1 && !js

package telemetry

// RegisterCacheMetrics is a no-op placeholder. Content caching is now handled
// internally by libfossil and does not expose cache statistics through the
// public API.
func RegisterCacheMetrics(_ any, _ any) {}
