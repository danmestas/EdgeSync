package notify

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"sync"

	"github.com/nats-io/nats.go"
	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/propagation"
)

// natsHeaderCarrier adapts nats.Header to propagation.TextMapCarrier so
// OpenTelemetry propagators can read/write trace context directly on a
// NATS message's headers. Duplicates the sibling carrier in the agent
// package; notify is a subpackage and the type is private there, so
// copying 15 lines is cheaper than exporting and re-routing a shared
// helper at this stage.
//
// NATS preserves header case on the send side but delivers headers
// lower-cased on the receive side, so Get must be case-insensitive.
// Set writes the canonical form so both pub and sub end up converging
// on the same key regardless of direction.
type natsHeaderCarrier nats.Header

func (c natsHeaderCarrier) Get(key string) string {
	// Try canonical form first (matches what Set writes on the send side
	// when the message has not crossed the wire yet).
	if v := http.Header(c).Get(key); v != "" {
		return v
	}
	// Fall back to literal lookups to tolerate case changes introduced
	// by the NATS wire protocol or other writers.
	if vs, ok := c[key]; ok && len(vs) > 0 {
		return vs[0]
	}
	lower := strings.ToLower(key)
	if vs, ok := c[lower]; ok && len(vs) > 0 {
		return vs[0]
	}
	// Last resort: case-insensitive scan. Header maps are tiny (≤ a few
	// entries), so this is O(n) over a small n.
	for k, vs := range c {
		if strings.EqualFold(k, key) && len(vs) > 0 {
			return vs[0]
		}
	}
	return ""
}

func (c natsHeaderCarrier) Set(key, value string) {
	http.Header(c).Set(key, value)
}

func (c natsHeaderCarrier) Keys() []string {
	keys := make([]string, 0, len(c))
	for k := range c {
		keys = append(keys, k)
	}
	return keys
}

// Verify natsHeaderCarrier implements TextMapCarrier at compile time.
var _ propagation.TextMapCarrier = natsHeaderCarrier(nil)

// PublishCtx marshals msg and publishes it on its NATS subject with any
// W3C trace context in ctx injected into the message headers. Subscribers
// that call ExtractFromMsg (or use the headers via propagation.HeaderCarrier)
// will see the publisher's span as the parent of their handler span.
//
// If ctx has no active span, this still publishes successfully — the
// traceparent header simply won't be set.
func PublishCtx(ctx context.Context, conn *nats.Conn, msg Message) error {
	data, err := json.Marshal(msg)
	if err != nil {
		return fmt.Errorf("notify: marshal for publish: %w", err)
	}
	natsMsg := &nats.Msg{
		Subject: msg.NATSSubject(),
		Data:    data,
		Header:  nats.Header{},
	}
	otel.GetTextMapPropagator().Inject(ctx, natsHeaderCarrier(natsMsg.Header))
	if err := conn.PublishMsg(natsMsg); err != nil {
		return fmt.Errorf("notify: publish to %s: %w", natsMsg.Subject, err)
	}
	return conn.Flush()
}

// Publish delegates to PublishCtx with a background ctx. Preserved for
// backwards compatibility with callers that lack a traced context.
func Publish(conn *nats.Conn, msg Message) error {
	return PublishCtx(context.Background(), conn, msg)
}

// maxSeenIDs is the cap on the dedup map. When exceeded, the oldest half is evicted.
const maxSeenIDs = 10000

// Subscriber receives messages from NATS subjects.
type Subscriber struct {
	conn     *nats.Conn
	subs     []*nats.Subscription
	seen     map[string]struct{}
	seenOrder []string // insertion order for FIFO eviction
	dedup    bool
	mu       sync.Mutex
}

// NewSubscriber creates a Subscriber for the given NATS connection.
func NewSubscriber(conn *nats.Conn) *Subscriber {
	return &Subscriber{conn: conn}
}

// EnableDedup turns on message ID deduplication.
func (s *Subscriber) EnableDedup() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.dedup = true
	s.seen = make(map[string]struct{})
	s.seenOrder = nil
}

func (s *Subscriber) isDuplicate(id string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.dedup {
		return false
	}
	if _, ok := s.seen[id]; ok {
		return true
	}
	s.seen[id] = struct{}{}
	s.seenOrder = append(s.seenOrder, id)

	// Evict oldest half when cap is exceeded.
	if len(s.seenOrder) > maxSeenIDs {
		half := len(s.seenOrder) / 2
		for _, old := range s.seenOrder[:half] {
			delete(s.seen, old)
		}
		s.seenOrder = s.seenOrder[half:]
	}
	return false
}

// Subscribe listens for all notify messages in a project (wildcard).
func (s *Subscriber) Subscribe(project string, cb func(Message)) error {
	return s.subscribe("notify."+project+".*", cb)
}

// SubscribeThread listens for messages in a specific thread.
func (s *Subscriber) SubscribeThread(project, threadShort string, cb func(Message)) error {
	return s.subscribe("notify."+project+"."+threadShort, cb)
}

// subscribeForWatch is the Watch-path variant that returns the nats.Subscription
// handle so Service.Watch can Unsubscribe precisely that subscription before
// closing its output channel (preventing send-on-closed-channel panics when
// a NATS message arrives during ctx-cancel teardown).
func (s *Subscriber) subscribeForWatch(subject string, cb func(Message)) (*nats.Subscription, error) {
	return s.subscribeReturn(subject, cb)
}

// subscribe wires up a NATS subscription with JSON decoding and dedup.
func (s *Subscriber) subscribe(subject string, cb func(Message)) error {
	_, err := s.subscribeReturn(subject, cb)
	return err
}

// ExtractFromMsg pulls W3C trace context from the NATS message headers,
// returning a context that carries the publisher's span as its parent.
// If the message has no headers, the passed-in ctx is returned unchanged.
// Exported so subscribers that want to start a child span under the
// publisher's trace can do so from outside this package.
func ExtractFromMsg(ctx context.Context, m *nats.Msg) context.Context {
	if m == nil || m.Header == nil {
		return ctx
	}
	return otel.GetTextMapPropagator().Extract(ctx, natsHeaderCarrier(m.Header))
}

// subscribeReturn is subscribe's variant that returns the nats.Subscription
// handle so the caller can Unsubscribe it independently. Used by Watch so
// per-Watch teardown can drop ONE subscription without killing every other
// active watcher. The returned handle is also appended to s.subs so that
// Service.Close (which calls Subscriber.Unsubscribe) still tears down
// everything for callers that do not manage handles individually.
func (s *Subscriber) subscribeReturn(
	subject string, cb func(Message),
) (*nats.Subscription, error) {
	sub, err := s.conn.Subscribe(subject, func(natsMsg *nats.Msg) {
		var msg Message
		if err := json.Unmarshal(natsMsg.Data, &msg); err != nil {
			return
		}
		if s.isDuplicate(msg.ID) {
			return
		}
		cb(msg)
	})
	if err != nil {
		return nil, fmt.Errorf("notify: subscribe %s: %w", subject, err)
	}
	s.subs = append(s.subs, sub)
	if err := s.conn.Flush(); err != nil {
		return sub, err
	}
	return sub, nil
}

// Unsubscribe removes all active subscriptions.
func (s *Subscriber) Unsubscribe() {
	for _, sub := range s.subs {
		sub.Unsubscribe()
	}
	s.subs = nil
}
