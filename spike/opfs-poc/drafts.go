//go:build js

// Package main provides NATS-based draft sync for collaborative editing.
// When multiple peers have the same file open, edits propagate via
// NATS pub/sub on edgesync.draft.<path>. This is faster than UV sync
// (direct pub/sub, no protocol overhead) and avoids UV convergence issues.
package main

import (
	"encoding/json"
	"fmt"

	"github.com/nats-io/nats.go"
)

const draftSubjectPrefix = "edgesync.draft."

// DraftMessage is published when a peer saves a file they're co-editing.
type DraftMessage struct {
	From    string `json:"from"`
	Path    string `json:"path"`
	Content string `json:"content"`
}

var draftSub *nats.Subscription

// startDraftSync subscribes to all draft messages from peers.
func startDraftSync(nc *nats.Conn) error {
	if nc == nil {
		panic("startDraftSync: nc must not be nil")
	}
	var err error
	draftSub, err = nc.Subscribe(draftSubjectPrefix+">", func(msg *nats.Msg) {
		var dm DraftMessage
		if err := json.Unmarshal(msg.Data, &dm); err != nil {
			return // Malformed — discard.
		}
		// Ignore our own drafts (we already wrote to OPFS).
		if dm.From == myPeerID {
			return
		}
		// Write the peer's content to our OPFS checkout.
		log(fmt.Sprintf("[draft] received %s from %s (%d bytes)", dm.Path, dm.From, len(dm.Content)))
		if currentCheckout != nil {
			if err := currentCheckout.WriteFile(dm.Path, dm.Content); err != nil {
				log(fmt.Sprintf("[draft] apply %s failed: %v", dm.Path, err))
				return
			}
			postResult("draftReceived", toJSON(map[string]any{
				"path": dm.Path,
				"size": len(dm.Content),
			}))
		}
	})
	if err != nil {
		return fmt.Errorf("draft subscribe: %w", err)
	}
	return nil
}

// publishDraft sends file content to all peers via NATS pub/sub.
// Only called when co-editors are present for this file.
func publishDraft(path, content string) {
	if path == "" {
		panic("publishDraft: path must not be empty")
	}
	if currentNATS == nil {
		return
	}
	dm := DraftMessage{
		From:    myPeerID,
		Path:    path,
		Content: content,
	}
	data, err := json.Marshal(dm)
	if err != nil {
		log(fmt.Sprintf("[draft] marshal error: %v", err))
		return
	}
	subject := draftSubjectPrefix + path
	if err := currentNATS.Publish(subject, data); err != nil {
		log(fmt.Sprintf("[draft] publish %s error: %v", path, err))
	}
}

func stopDraftSync() {
	if draftSub != nil {
		draftSub.Unsubscribe()
		draftSub = nil
	}
}
