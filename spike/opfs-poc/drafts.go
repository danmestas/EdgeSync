//go:build js

// Package main provides UV-based draft sync for collaborative editing.
// When multiple peers have the same file open, edits propagate via
// Fossil UV files (mtime-wins, auto-sync). Manual or auto-commit
// promotes drafts to versioned checkins.
package main

import (
	"fmt"
	"strings"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/uv"
)

const draftPrefix = "draft/"

// saveDraft writes file content to a UV draft entry.
// Only called when co-editors are present for this file.
func saveDraft(path, content string) error {
	if path == "" {
		panic("saveDraft: path must not be empty")
	}
	if currentRepo == nil {
		return fmt.Errorf("no repo open")
	}
	name := draftPrefix + path
	now := time.Now().Unix()
	return uv.Write(currentRepo.DB(), name, []byte(content), now)
}

// listDrafts returns all UV entries with the draft/ prefix.
func listDrafts() (map[string][]byte, error) {
	if currentRepo == nil {
		return nil, fmt.Errorf("no repo open")
	}
	entries, err := uv.List(currentRepo.DB())
	if err != nil {
		return nil, fmt.Errorf("list UV: %w", err)
	}
	drafts := make(map[string][]byte)
	for _, e := range entries {
		if !strings.HasPrefix(e.Name, draftPrefix) {
			continue
		}
		path := strings.TrimPrefix(e.Name, draftPrefix)
		content, _, _, err := uv.Read(currentRepo.DB(), e.Name)
		if err != nil {
			continue
		}
		if content != nil {
			drafts[path] = content
		}
	}
	return drafts, nil
}

// commitDrafts reads all UV drafts, creates a Fossil checkin, and
// deletes the draft UV entries. Returns the number of files committed.
func commitDrafts(comment, user string) (int, string, error) {
	if currentRepo == nil {
		return 0, "", fmt.Errorf("no repo open")
	}
	if currentCheckout == nil {
		return 0, "", fmt.Errorf("no checkout")
	}

	drafts, err := listDrafts()
	if err != nil {
		return 0, "", err
	}
	if len(drafts) == 0 {
		return 0, "", nil // Nothing to commit.
	}

	// Apply drafts to OPFS before committing so CommitAll sees them.
	for path, content := range drafts {
		if err := currentCheckout.WriteFile(path, string(content)); err != nil {
			log(fmt.Sprintf("[drafts] write %s to OPFS failed: %v", path, err))
		}
	}

	// Commit from OPFS (includes drafts + any other changes).
	if user == "" {
		user = "browser-" + myPeerID
	}
	if comment == "" {
		comment = fmt.Sprintf("auto: %d file(s) from collaboration", len(drafts))
	}
	_, uuid, err := currentCheckout.CommitAll(comment, user)
	if err != nil {
		return 0, "", fmt.Errorf("checkin: %w", err)
	}

	// Delete UV draft entries so they don't persist.
	now := time.Now().Unix()
	for path := range drafts {
		name := draftPrefix + path
		if err := uv.Delete(currentRepo.DB(), name, now); err != nil {
			log(fmt.Sprintf("[drafts] delete UV %s failed: %v", name, err))
		}
	}

	return len(drafts), uuid, nil
}

// applyReceivedDrafts checks for new UV drafts after a sync pull
// and updates OPFS checkout files + UI accordingly.
func applyReceivedDrafts() {
	if currentRepo == nil || currentCheckout == nil {
		return
	}
	drafts, err := listDrafts()
	if err != nil {
		return
	}
	for path, content := range drafts {
		if err := currentCheckout.WriteFile(path, string(content)); err != nil {
			log(fmt.Sprintf("[drafts] apply %s failed: %v", path, err))
			continue
		}
		// Notify UI that a file was updated by a peer.
		postResult("draftReceived", toJSON(map[string]any{
			"path": path,
			"size": len(content),
		}))
	}
}

// hasDrafts returns true if there are any UV draft entries.
func hasDrafts() bool {
	if currentRepo == nil {
		return false
	}
	entries, err := uv.List(currentRepo.DB())
	if err != nil {
		return false
	}
	for _, e := range entries {
		if strings.HasPrefix(e.Name, draftPrefix) {
			return true
		}
	}
	return false
}

// ensureUVSchema makes sure the unversioned table exists.
func ensureUVSchema() {
	if currentRepo == nil {
		return
	}
	if err := uv.EnsureSchema(currentRepo.DB()); err != nil {
		log(fmt.Sprintf("[drafts] ensure UV schema: %v", err))
	}
}

// autoCommitDrafts is called when collaboration ends (peers left).
// Waits a grace period, then commits remaining drafts.
func autoCommitDrafts() {
	if !hasDrafts() {
		return
	}
	log("[drafts] collaboration ended, auto-committing drafts...")
	n, uuid, err := commitDrafts("", "")
	if err != nil {
		log(fmt.Sprintf("[drafts] auto-commit failed: %v", err))
		return
	}
	if n > 0 {
		short := uuid
		if len(short) > 12 {
			short = short[:12]
		}
		log(fmt.Sprintf("[drafts] auto-committed %d file(s): %s", n, short))
		postResult("coCommit", toJSON(map[string]any{"rid": 0, "uuid": uuid}))
		publishNotify(currentNATS, uuid)
	}
}

// Ensure uv and manifest are used.
var _ = uv.Write
var _ = manifest.Checkin
