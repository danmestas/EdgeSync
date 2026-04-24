package natshdr

import (
	"sort"
	"testing"

	"github.com/nats-io/nats.go"
	"go.opentelemetry.io/otel/propagation"
)

// Compile-time assertion mirrors the one in carrier.go; having it in the
// test file too guards against accidental deletion.
var _ propagation.TextMapCarrier = Carrier(nil)

func TestCarrier_GetFindsHeaderRegardlessOfCase(t *testing.T) {
	// Simulate the wire-delivered (lower-cased) shape that NATS produces
	// on the receive side, plus a canonical-cased variant that Set would
	// produce on the send side before flushing.
	cases := []struct {
		name    string
		storeAs string
	}{
		{"lowercase-wire", "traceparent"},
		{"canonical-case", "Traceparent"},
		{"upper-case", "TRACEPARENT"},
		{"mixed-case", "TraceParent"},
	}

	const val = "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01"

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			c := Carrier(nats.Header{})
			c[tc.storeAs] = []string{val}

			for _, lookup := range []string{"traceparent", "Traceparent", "TRACEPARENT"} {
				if got := c.Get(lookup); got != val {
					t.Errorf("Get(%q) stored as %q = %q, want %q",
						lookup, tc.storeAs, got, val)
				}
			}
		})
	}
}

func TestCarrier_GetReturnsEmptyForMissing(t *testing.T) {
	c := Carrier(nats.Header{})
	if got := c.Get("traceparent"); got != "" {
		t.Errorf("Get on empty carrier = %q, want \"\"", got)
	}

	c["other-key"] = []string{"v"}
	if got := c.Get("traceparent"); got != "" {
		t.Errorf("Get missing key = %q, want \"\"", got)
	}
}

func TestCarrier_GetReturnsEmptyForEmptySlice(t *testing.T) {
	c := Carrier(nats.Header{})
	c["traceparent"] = nil
	if got := c.Get("traceparent"); got != "" {
		t.Errorf("Get on nil-valued key = %q, want \"\"", got)
	}
	c["traceparent"] = []string{}
	if got := c.Get("traceparent"); got != "" {
		t.Errorf("Get on zero-length slice = %q, want \"\"", got)
	}
}

func TestCarrier_SetAndGet(t *testing.T) {
	c := Carrier(nats.Header{})
	c.Set("Traceparent", "abc")

	// Get must find it regardless of the case the caller uses on Set or
	// Get. That is the whole point of the carrier.
	if got := c.Get("traceparent"); got != "abc" {
		t.Errorf("Get after Set = %q, want %q", got, "abc")
	}
	if got := c.Get("Traceparent"); got != "abc" {
		t.Errorf("Get after Set (exact) = %q, want %q", got, "abc")
	}
}

func TestCarrier_KeysReturnsAllHeaders(t *testing.T) {
	c := Carrier(nats.Header{})
	c.Set("Traceparent", "tp")
	c.Set("Tracestate", "ts")

	got := c.Keys()
	sort.Strings(got)
	want := []string{"Tracestate", "Traceparent"}
	sort.Strings(want)

	if len(got) != len(want) {
		t.Fatalf("Keys = %v, want %v", got, want)
	}
	for i := range got {
		if got[i] != want[i] {
			t.Errorf("Keys[%d] = %q, want %q", i, got[i], want[i])
		}
	}
}
