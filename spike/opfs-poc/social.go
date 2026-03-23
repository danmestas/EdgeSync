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
	File string `json:"file"` // Currently open file path, "" if none.
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

	// peerMu protects peerLastSeen and peerFiles.
	// Accessed from NATS callback goroutine and presence loop goroutine.
	peerMu       sync.Mutex
	peerLastSeen map[string]time.Time
	peerFiles    map[string]string // peerID → currently open file path

	// myOpenFile is the file path currently open in this tab's editor.
	myOpenFile string
)

func init() {
	myPeerID = fmt.Sprintf("%04x", rand.Intn(0xFFFF))
	peerLastSeen = make(map[string]time.Time)
	peerFiles = make(map[string]string)
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
		peerFiles[hb.ID] = hb.File
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
		File: myOpenFile,
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
	hadCoEditors := fileHasCoEditorsLocked(myOpenFile)
	now := time.Now()
	for id, lastSeen := range peerLastSeen {
		if now.Sub(lastSeen) > presenceTimeout {
			delete(peerLastSeen, id)
			delete(peerFiles, id)
		}
	}
	hasCoEditors := fileHasCoEditorsLocked(myOpenFile)
	peerMu.Unlock()

	// If we lost our last co-editor, auto-commit the OPFS working tree.
	if hadCoEditors && !hasCoEditors {
		go func() {
			if currentCheckout == nil {
				return
			}
			changes, err := currentCheckout.Status()
			if err != nil || len(changes) == 0 {
				return
			}
			log("[drafts] collaboration ended, auto-committing...")
			_, uuid, err := currentCheckout.CommitAll("auto: collaboration ended", "browser-"+myPeerID)
			if err != nil {
				log(fmt.Sprintf("[drafts] auto-commit failed: %v", err))
				return
			}
			short := uuid
			if len(short) > 12 {
				short = short[:12]
			}
			log(fmt.Sprintf("[drafts] auto-committed: %s", short))
			postResult("coCommit", toJSON(map[string]any{"rid": 0, "uuid": uuid}))
			publishNotify(currentNATS, uuid)
		}()
	}
}

// fileHasCoEditorsLocked checks for co-editors without taking the lock.
// Caller must hold peerMu.
func fileHasCoEditorsLocked(path string) bool {
	if path == "" {
		return false
	}
	for id, file := range peerFiles {
		if id != myPeerID && file == path {
			return true
		}
	}
	return false
}

func broadcastPeers() {
	peerMu.Lock()
	defer peerMu.Unlock()
	type peerInfo struct {
		ID   string `json:"id"`
		IsMe bool   `json:"isMe"`
		File string `json:"file"`
	}
	// Always include self.
	peerLastSeen[myPeerID] = time.Now()
	peerFiles[myPeerID] = myOpenFile
	var list []peerInfo
	for id := range peerLastSeen {
		list = append(list, peerInfo{
			ID:   id,
			IsMe: id == myPeerID,
			File: peerFiles[id],
		})
	}
	postResult("peers", toJSON(list))
}

// fileHasCoEditors returns true if another peer has the given file open.
func fileHasCoEditors(path string) bool {
	if path == "" {
		return false
	}
	peerMu.Lock()
	defer peerMu.Unlock()
	for id, file := range peerFiles {
		if id != myPeerID && file == path {
			return true
		}
	}
	return false
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
	peerFiles = make(map[string]string)
}

// registerSocialCallbacks exposes chat, drafts, and file presence to JS.
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

	// _setOpenFile updates which file this peer has open (for presence).
	js.Global().Set("_setOpenFile", js.FuncOf(func(_ js.Value, args []js.Value) any {
		path := ""
		if len(args) > 0 {
			path = args[0].String()
		}
		myOpenFile = path
		// Publish updated presence immediately so peers see the change.
		if currentNATS != nil {
			go publishHeartbeat(currentNATS, "browser-"+myPeerID)
		}
		return nil
	}))

	// _saveDraft publishes content to co-editors via NATS (only when co-editing).
	js.Global().Set("_saveDraft", js.FuncOf(func(_ js.Value, args []js.Value) any {
		if len(args) < 2 {
			return nil
		}
		path := args[0].String()
		content := args[1].String()
		// Only publish draft if another peer has this file open.
		if !fileHasCoEditors(path) {
			return nil
		}
		go publishDraft(path, content)
		return nil
	}))

	// _hasCoEditors checks if the given file has other peers editing it.
	js.Global().Set("_hasCoEditors", js.FuncOf(func(_ js.Value, args []js.Value) any {
		if len(args) < 1 {
			return js.ValueOf(false)
		}
		return js.ValueOf(fileHasCoEditors(args[0].String()))
	}))
}
