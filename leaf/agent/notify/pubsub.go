package notify

import (
	"encoding/json"
	"fmt"
	"sync"

	"github.com/nats-io/nats.go"
)

// Publish sends a message to its NATS subject. Free function — no Publisher type needed.
func Publish(conn *nats.Conn, msg Message) error {
	data, err := json.Marshal(msg)
	if err != nil {
		return fmt.Errorf("notify: marshal for publish: %w", err)
	}
	subject := msg.NATSSubject()
	if err := conn.Publish(subject, data); err != nil {
		return fmt.Errorf("notify: publish to %s: %w", subject, err)
	}
	return conn.Flush()
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

// subscribe wires up a NATS subscription with JSON decoding and dedup.
func (s *Subscriber) subscribe(subject string, cb func(Message)) error {
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
		return fmt.Errorf("notify: subscribe %s: %w", subject, err)
	}
	s.subs = append(s.subs, sub)
	return s.conn.Flush()
}

// Unsubscribe removes all active subscriptions.
func (s *Subscriber) Unsubscribe() {
	for _, sub := range s.subs {
		sub.Unsubscribe()
	}
	s.subs = nil
}
