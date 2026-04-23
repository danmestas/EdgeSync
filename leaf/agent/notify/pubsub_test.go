package notify

import (
	"encoding/json"
	"sync"
	"testing"
	"time"

	"github.com/nats-io/nats.go"
	natsserver "github.com/nats-io/nats-server/v2/server"
)

func startTestNATS(t *testing.T) string {
	t.Helper()
	opts := &natsserver.Options{
		Host: "127.0.0.1",
		Port: -1, // random port
	}
	ns, err := natsserver.NewServer(opts)
	if err != nil {
		t.Fatalf("new nats server: %v", err)
	}
	ns.Start()
	if !ns.ReadyForConnections(2 * time.Second) {
		t.Fatal("nats server not ready")
	}
	t.Cleanup(ns.Shutdown)
	return ns.ClientURL()
}

func TestPublishAndSubscribe(t *testing.T) {
	url := startTestNATS(t)

	pub, err := nats.Connect(url)
	if err != nil {
		t.Fatalf("connect pub: %v", err)
	}
	defer pub.Close()

	sub, err := nats.Connect(url)
	if err != nil {
		t.Fatalf("connect sub: %v", err)
	}
	defer sub.Close()

	msg := NewMessage(MessageOpts{
		Project:  "edgesync",
		From:     "endpoint-abc123",
		FromName: "claude-macbook",
		Body:     "Build started",
	})

	var received Message
	var wg sync.WaitGroup
	wg.Add(1)

	subscriber := NewSubscriber(sub)
	defer subscriber.Unsubscribe()

	if err := subscriber.SubscribeThread(msg.Project, msg.ThreadShort(), func(m Message) {
		received = m
		wg.Done()
	}); err != nil {
		t.Fatalf("subscribe: %v", err)
	}

	if err := Publish(pub, msg); err != nil {
		t.Fatalf("publish: %v", err)
	}

	done := make(chan struct{})
	go func() {
		wg.Wait()
		close(done)
	}()

	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("timeout waiting for message")
	}

	if received.ID != msg.ID {
		t.Errorf("received ID = %q, want %q", received.ID, msg.ID)
	}
	if received.Body != msg.Body {
		t.Errorf("received Body = %q, want %q", received.Body, msg.Body)
	}
}

func TestSubscribeWildcard(t *testing.T) {
	url := startTestNATS(t)

	pub, err := nats.Connect(url)
	if err != nil {
		t.Fatalf("connect pub: %v", err)
	}
	defer pub.Close()

	subConn, err := nats.Connect(url)
	if err != nil {
		t.Fatalf("connect sub: %v", err)
	}
	defer subConn.Close()

	msg1 := NewMessage(MessageOpts{
		Project:  "edgesync",
		From:     "endpoint-abc123",
		FromName: "claude-macbook",
		Body:     "Thread 1",
	})
	msg2 := NewMessage(MessageOpts{
		Project:  "edgesync",
		From:     "endpoint-abc123",
		FromName: "claude-macbook",
		Body:     "Thread 2",
	})

	var mu sync.Mutex
	var received []Message
	var wg sync.WaitGroup
	wg.Add(2)

	subscriber := NewSubscriber(subConn)
	defer subscriber.Unsubscribe()

	if err := subscriber.Subscribe("edgesync", func(m Message) {
		mu.Lock()
		received = append(received, m)
		mu.Unlock()
		wg.Done()
	}); err != nil {
		t.Fatalf("subscribe: %v", err)
	}

	if err := Publish(pub, msg1); err != nil {
		t.Fatalf("publish msg1: %v", err)
	}
	if err := Publish(pub, msg2); err != nil {
		t.Fatalf("publish msg2: %v", err)
	}

	done := make(chan struct{})
	go func() {
		wg.Wait()
		close(done)
	}()

	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("timeout waiting for messages")
	}

	mu.Lock()
	defer mu.Unlock()
	if len(received) != 2 {
		t.Fatalf("received %d messages, want 2", len(received))
	}

	bodies := map[string]bool{received[0].Body: true, received[1].Body: true}
	if !bodies["Thread 1"] || !bodies["Thread 2"] {
		t.Errorf("received bodies = %v, want Thread 1 and Thread 2", bodies)
	}
}

func TestSubscribeDedup(t *testing.T) {
	url := startTestNATS(t)

	pub, err := nats.Connect(url)
	if err != nil {
		t.Fatalf("connect pub: %v", err)
	}
	defer pub.Close()

	subConn, err := nats.Connect(url)
	if err != nil {
		t.Fatalf("connect sub: %v", err)
	}
	defer subConn.Close()

	msg := NewMessage(MessageOpts{
		Project:  "edgesync",
		From:     "endpoint-abc123",
		FromName: "claude-macbook",
		Body:     "Dedup test",
	})

	var count int
	var mu sync.Mutex

	subscriber := NewSubscriber(subConn)
	subscriber.EnableDedup()
	defer subscriber.Unsubscribe()

	if err := subscriber.SubscribeThread(msg.Project, msg.ThreadShort(), func(m Message) {
		mu.Lock()
		count++
		mu.Unlock()
	}); err != nil {
		t.Fatalf("subscribe: %v", err)
	}

	// Publish the same message raw 3 times (same ID).
	data, _ := json.Marshal(msg)
	subject := msg.NATSSubject()
	for i := 0; i < 3; i++ {
		if err := pub.Publish(subject, data); err != nil {
			t.Fatalf("publish %d: %v", i, err)
		}
	}
	pub.Flush()

	// Wait for messages to be delivered.
	time.Sleep(200 * time.Millisecond)

	mu.Lock()
	defer mu.Unlock()
	if count != 1 {
		t.Errorf("callback count = %d, want 1 (dedup should filter duplicates)", count)
	}
}
