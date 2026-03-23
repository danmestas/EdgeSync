//go:build js

package main

import (
	"encoding/json"
	"fmt"
	"math/rand"
	"sync"
	"syscall/js"
	"time"

	"github.com/nats-io/nats.go"
)

const (
	chatSubject       = "edgesync.chat"
	presenceSubject   = "edgesync.presence"
	notifySubject     = "edgesync.notify" // Published after commit, triggers instant pull.
	heartbeatInterval = 5 * time.Second
	presenceTimeout   = 15 * time.Second // Evict peers silent longer than this.
)

type ChatMessage struct {
	From string `json:"from"`
	Text string `json:"text"`
	Time string `json:"time"`
}

type PresenceHeartbeat struct {
	ID   string `json:"id"`
	User string `json:"user"`
	Time string `json:"time"`
}

var (
	// myPeerID is a random 4-hex identifier for this browser tab.
	// Collisions are acceptable for a spike — not a production identifier.
	myPeerID string

	chatSub       *nats.Subscription
	presenceSub   *nats.Subscription
	notifySub     *nats.Subscription
	heartbeatStop chan struct{}

	// peerLastSeen tracks when each peer last sent a heartbeat.
	// Protected by peerMu — accessed from NATS callback goroutine
	// and the presence loop goroutine.
	peerMu       sync.Mutex
	peerLastSeen map[string]time.Time
)

func init() {
	myPeerID = fmt.Sprintf("%04x", rand.Intn(0xFFFF))
	peerLastSeen = make(map[string]time.Time)
}

// startChat subscribes to the chat subject and publishes a join message.
func startChat(nc *nats.Conn, user string) error {
	if nc == nil {
		panic("startChat: nc must not be nil")
	}
	if user == "" {
		panic("startChat: user must not be empty")
	}
	var err error
	chatSub, err = nc.Subscribe(chatSubject, func(msg *nats.Msg) {
		var cm ChatMessage
		if err := json.Unmarshal(msg.Data, &cm); err != nil {
			// Malformed message from another peer — discard silently.
			// This is expected when peers use different message formats.
			return
		}
		postResult("chatMsg", toJSON(cm))
	})
	if err != nil {
		return fmt.Errorf("chat subscribe: %w", err)
	}
	sendChat(nc, user, fmt.Sprintf("%s joined", user))
	return nil
}

func sendChat(nc *nats.Conn, user, text string) {
	if nc == nil {
		return
	}
	cm := ChatMessage{
		From: user,
		Text: text,
		Time: time.Now().UTC().Format(time.RFC3339),
	}
	data, err := json.Marshal(cm)
	if err != nil {
		log(fmt.Sprintf("[chat] marshal error: %v", err))
		return
	}
	if err := nc.Publish(chatSubject, data); err != nil {
		log(fmt.Sprintf("[chat] publish error: %v", err))
	}
}

// startPresence begins heartbeat publishing and peer tracking.
func startPresence(nc *nats.Conn, user string) error {
	if nc == nil {
		panic("startPresence: nc must not be nil")
	}
	var err error
	presenceSub, err = nc.Subscribe(presenceSubject, func(msg *nats.Msg) {
		var hb PresenceHeartbeat
		if err := json.Unmarshal(msg.Data, &hb); err != nil {
			// Malformed heartbeat — discard.
			return
		}
		peerMu.Lock()
		peerLastSeen[hb.ID] = time.Now()
		peerMu.Unlock()
		broadcastPeers()
	})
	if err != nil {
		return fmt.Errorf("presence subscribe: %w", err)
	}

	heartbeatStop = make(chan struct{})
	go presenceLoop(nc, user)
	return nil
}

// presenceLoop publishes heartbeats and evicts stale peers.
// Runs until heartbeatStop is closed (when agent stops).
func presenceLoop(nc *nats.Conn, user string) {
	publishHeartbeat(nc, user)
	broadcastPeers()

	ticker := time.NewTicker(heartbeatInterval)
	defer ticker.Stop()
	for {
		select {
		case <-ticker.C:
			publishHeartbeat(nc, user)
			evictStalePeers()
			broadcastPeers()
		case <-heartbeatStop:
			return
		}
	}
}

func publishHeartbeat(nc *nats.Conn, user string) {
	hb := PresenceHeartbeat{
		ID:   myPeerID,
		User: user,
		Time: time.Now().UTC().Format(time.RFC3339),
	}
	data, err := json.Marshal(hb)
	if err != nil {
		log(fmt.Sprintf("[presence] marshal error: %v", err))
		return
	}
	if err := nc.Publish(presenceSubject, data); err != nil {
		log(fmt.Sprintf("[presence] publish error: %v", err))
	}
}

func evictStalePeers() {
	peerMu.Lock()
	defer peerMu.Unlock()
	now := time.Now()
	for id, lastSeen := range peerLastSeen {
		if now.Sub(lastSeen) > presenceTimeout {
			delete(peerLastSeen, id)
		}
	}
}

func broadcastPeers() {
	peerMu.Lock()
	defer peerMu.Unlock()
	type peerInfo struct {
		ID   string `json:"id"`
		IsMe bool   `json:"isMe"`
	}
	// Always include self.
	peerLastSeen[myPeerID] = time.Now()
	var list []peerInfo
	for id := range peerLastSeen {
		list = append(list, peerInfo{ID: id, IsMe: id == myPeerID})
	}
	postResult("peers", toJSON(list))
}

// startNotify subscribes to commit notifications from other peers.
// When a notification arrives, it triggers an immediate sync via SyncNow.
func startNotify(nc *nats.Conn) error {
	if nc == nil {
		panic("startNotify: nc must not be nil")
	}
	var err error
	notifySub, err = nc.Subscribe(notifySubject, func(msg *nats.Msg) {
		// Another peer committed — trigger immediate sync instead of
		// waiting for the 10s poll timer.
		if currentAgent != nil {
			log("[notify] peer committed, syncing now...")
			currentAgent.SyncNow()
		}
	})
	if err != nil {
		return fmt.Errorf("notify subscribe: %w", err)
	}
	return nil
}

// publishNotify announces a new commit to all connected peers.
// Called after CommitAll succeeds.
func publishNotify(nc *nats.Conn, uuid string) {
	if nc == nil {
		return
	}
	data := []byte(fmt.Sprintf(`{"from":"%s","uuid":"%s"}`, myPeerID, uuid))
	if err := nc.Publish(notifySubject, data); err != nil {
		log(fmt.Sprintf("[notify] publish error: %v", err))
	}
}

func stopSocial() {
	if chatSub != nil {
		chatSub.Unsubscribe()
		chatSub = nil
	}
	if presenceSub != nil {
		presenceSub.Unsubscribe()
		presenceSub = nil
	}
	if notifySub != nil {
		notifySub.Unsubscribe()
		notifySub = nil
	}
	if heartbeatStop != nil {
		close(heartbeatStop)
		heartbeatStop = nil
	}
	peerLastSeen = make(map[string]time.Time)
}

// registerSocialCallbacks exposes chat to JS via _sendChat(text, user).
func registerSocialCallbacks() {
	js.Global().Set("_sendChat", js.FuncOf(func(_ js.Value, args []js.Value) any {
		if currentNATS == nil {
			return nil
		}
		if len(args) < 1 {
			return nil
		}
		text := args[0].String()
		user := "browser"
		if len(args) > 1 && args[1].String() != "" {
			user = args[1].String()
		}
		go sendChat(currentNATS, user, text)
		return nil
	}))
}
