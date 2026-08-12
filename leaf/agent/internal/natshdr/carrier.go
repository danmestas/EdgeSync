// Package natshdr provides a propagation.TextMapCarrier adapter for
// nats.Header so OpenTelemetry propagators can read/write W3C trace
// context directly on a NATS message's headers.
//
// Header key case on the wire varies: nats.go before v1.50 delivered
// keys lower-cased, later versions preserve send case, and nats.Header's
// own Get is exact-match. Get here is case-insensitive so subscribers
// find "traceparent" regardless of which variant arrives.
package natshdr

import (
	"net/http"
	"strings"

	"github.com/nats-io/nats.go"
	"go.opentelemetry.io/otel/propagation"
)

// Carrier adapts nats.Header to propagation.TextMapCarrier.
type Carrier nats.Header

// Get returns the first value for the header keyed by any case-variant
// of key, or "" if none is set. Header maps are tiny (typically ≤3
// entries), so an EqualFold scan is simple and demonstrably correct.
func (c Carrier) Get(key string) string {
	for k, v := range c {
		if len(v) > 0 && strings.EqualFold(k, key) {
			return v[0]
		}
	}
	return ""
}

// Set writes the canonical form so both publishers and subscribers
// converge on the same key regardless of direction.
func (c Carrier) Set(key, value string) {
	http.Header(c).Set(key, value)
}

// Keys returns the header names currently present on the carrier.
func (c Carrier) Keys() []string {
	keys := make([]string, 0, len(c))
	for k := range c {
		keys = append(keys, k)
	}
	return keys
}

// Verify Carrier implements TextMapCarrier at compile time.
var _ propagation.TextMapCarrier = Carrier(nil)
