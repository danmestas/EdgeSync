// Package notify provides message types and serialization for the EdgeSync
// notification system. Messages are organized into threads and routed via NATS
// subjects, with each message persisted as a JSON file in the UV store.
package notify

import (
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"time"
)

// Action classifies what kind of event a message represents.
type Action string

const (
	ActionCheckin Action = "checkin"
	ActionSync    Action = "sync"
	ActionAlert   Action = "alert"
	ActionComment Action = "comment"
)

// Priority indicates the urgency of a message.
type Priority string

const (
	PriorityInfo           Priority = "info"
	PriorityActionRequired Priority = "action_required"
	PriorityUrgent         Priority = "urgent"
)

// Message is a single notification event. It is the unit of storage and
// transport in the notify system. Callers construct messages via NewMessage or
// NewReply — never by direct struct literal, to ensure IDs and timestamps are
// always populated.
type Message struct {
	// ID is a 32-char hex UUID uniquely identifying this message.
	ID string `json:"id"`
	// ThreadID groups related messages. Format: "thread-<32-hex>".
	ThreadID string `json:"thread_id"`
	// Project is the Fossil project code this message belongs to.
	Project string `json:"project"`
	// Action describes the event type.
	Action Action `json:"action"`
	// Body is the human-readable message text.
	Body string `json:"body"`
	// Priority controls delivery urgency.
	Priority Priority `json:"priority"`
	// ReplyTo is the ID of the parent message, empty for root messages.
	ReplyTo string `json:"reply_to,omitempty"`
	// ActionResponse marks this message as a response to an action request.
	// Callers set this on replies that close an action loop.
	ActionResponse bool `json:"action_response,omitempty"`
	// Timestamp is Unix seconds (UTC) when the message was created.
	Timestamp int64 `json:"timestamp"`
}

// NewMessage creates a root message (no thread parent). A new ThreadID is
// generated for the message.
func NewMessage(project string, action Action, body string) Message {
	return Message{
		ID:        newUUID(),
		ThreadID:  "thread-" + newUUID(),
		Project:   project,
		Action:    action,
		Body:      body,
		Priority:  PriorityInfo,
		Timestamp: time.Now().Unix(),
	}
}

// NewReply creates a reply message in the same thread as parent. The caller
// may set ActionResponse=true on the returned message to mark it as closing an
// action request.
func NewReply(parent Message, body string) Message {
	return Message{
		ID:        newUUID(),
		ThreadID:  parent.ThreadID,
		Project:   parent.Project,
		Action:    parent.Action,
		Body:      body,
		Priority:  PriorityInfo,
		ReplyTo:   parent.ID,
		Timestamp: time.Now().Unix(),
	}
}

// ThreadShort returns the first 8 characters of the thread UUID (the part
// after the "thread-" prefix). Used in file paths and NATS subjects.
func (m Message) ThreadShort() string {
	return shortID(m.ThreadID[len("thread-"):])
}

// FilePath returns the relative path where this message should be stored.
// Format: <project>/threads/<thread-short>/<unix-timestamp>-<msg-short>.json
func (m Message) FilePath() string {
	return fmt.Sprintf("%s/threads/%s/%d-%s.json",
		m.Project, m.ThreadShort(), m.Timestamp, shortID(m.ID))
}

// NATSSubject returns the NATS subject for publishing or subscribing to this
// message's thread.
// Format: notify.<project>.<thread-short>
func (m Message) NATSSubject() string {
	return fmt.Sprintf("notify.%s.%s", m.Project, m.ThreadShort())
}

// shortID returns the first 8 characters of id.
func shortID(id string) string {
	if len(id) <= 8 {
		return id
	}
	return id[:8]
}

// newUUID generates a 32-character lowercase hex string from crypto/rand.
func newUUID() string {
	b := make([]byte, 16)
	if _, err := rand.Read(b); err != nil {
		panic("notify: crypto/rand unavailable: " + err.Error())
	}
	return hex.EncodeToString(b)
}
