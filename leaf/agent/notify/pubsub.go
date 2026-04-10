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

// Subscriber receives messages from NATS subjects.
type Subscriber struct {
	conn  *nats.Conn
	subs  []*nats.Subscription
	seen  map[string]struct{}
	dedup bool
	mu    sync.Mutex
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
	return false
}

// Subscribe listens for all notify messages in a project (wildcard).
func (s *Subscriber) Subscribe(project string, cb func(Message)) error {
	subject := "notify." + project + ".*"
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

// SubscribeThread listens for messages in a specific thread.
func (s *Subscriber) SubscribeThread(project, threadShort string, cb func(Message)) error {
	subject := "notify." + project + "." + threadShort
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
