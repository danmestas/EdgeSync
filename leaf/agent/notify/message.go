// Package notify implements the EdgeSync bidirectional messaging system.
// Messages are JSON files stored in a Fossil repo and delivered in real-time via NATS.
package notify

import (
	"crypto/rand"
	"fmt"
	"time"
)

// Priority levels for message urgency.
type Priority string

const (
	PriorityInfo           Priority = "info"
	PriorityActionRequired Priority = "action_required"
	PriorityUrgent         Priority = "urgent"
)

// Action represents a quick-response button on a message.
type Action struct {
	ID    string `json:"id"`
	Label string `json:"label"`
}

// Message is a single notification message stored as a JSON file in the notify repo.
type Message struct {
	V              int       `json:"v"`
	ID             string    `json:"id"`
	Thread         string    `json:"thread"`
	Project        string    `json:"project"`
	From           string    `json:"from"`
	FromName       string    `json:"from_name"`
	Timestamp      time.Time `json:"timestamp"`
	Body           string    `json:"body"`
	Priority       Priority  `json:"priority,omitempty"`
	Actions        []Action  `json:"actions,omitempty"`
	ReplyTo        string    `json:"reply_to,omitempty"`
	Media          []string  `json:"media,omitempty"`
	ActionResponse bool      `json:"action_response,omitempty"`
}

// MessageOpts are the caller-provided fields for creating a new message.
type MessageOpts struct {
	Project  string
	From     string
	FromName string
	Body     string
	Priority Priority
	Actions  []Action
	Media    []string
}

// ReplyOpts are the caller-provided fields for creating a reply.
type ReplyOpts struct {
	From     string
	FromName string
	Body     string
	Media    []string
}

// NewMessage creates a new message with a generated ID, thread ID, and timestamp.
func NewMessage(opts MessageOpts) Message {
	pri := opts.Priority
	if pri == "" {
		pri = PriorityInfo
	}
	return Message{
		V:         1,
		ID:        "msg-" + newUUID(),
		Thread:    "thread-" + newUUID(),
		Project:   opts.Project,
		From:      opts.From,
		FromName:  opts.FromName,
		Timestamp: time.Now().UTC(),
		Body:      opts.Body,
		Priority:  pri,
		Actions:   opts.Actions,
		Media:     opts.Media,
	}
}

// NewReply creates a reply to an existing message, preserving the thread.
func NewReply(original Message, opts ReplyOpts) Message {
	return Message{
		V:         1,
		ID:        "msg-" + newUUID(),
		Thread:    original.Thread,
		Project:   original.Project,
		From:      opts.From,
		FromName:  opts.FromName,
		Timestamp: time.Now().UTC(),
		Body:      opts.Body,
		ReplyTo:   original.ID,
		Media:     opts.Media,
	}
}

// No NewActionReply — callers use NewReply and set ActionResponse = true directly.

// FilePath returns the repo-relative path for this message file.
func (m Message) FilePath() string {
	threadShort := shortID(m.Thread, "thread-")
	msgShort := shortID(m.ID, "msg-")
	return fmt.Sprintf("%s/threads/%s/%d-%s.json", m.Project, threadShort, m.Timestamp.Unix(), msgShort)
}

// ThreadShort returns the first 8 characters of the thread UUID (after "thread-" prefix).
func (m Message) ThreadShort() string {
	return shortID(m.Thread, "thread-")
}

// NATSSubject returns the NATS subject for this message.
func (m Message) NATSSubject() string {
	return "notify." + m.Project + "." + m.ThreadShort()
}

// shortID strips a prefix and returns the first 8 characters.
func shortID(id, prefix string) string {
	s := id
	if len(prefix) > 0 && len(s) > len(prefix) {
		s = s[len(prefix):]
	}
	if len(s) > 8 {
		s = s[:8]
	}
	return s
}

// newUUID generates a random UUID (v4-like, 32 hex chars).
func newUUID() string {
	b := make([]byte, 16)
	_, _ = rand.Read(b)
	return fmt.Sprintf("%x", b)
}
