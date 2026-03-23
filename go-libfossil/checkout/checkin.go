package checkout

import (
	"context"
	"database/sql"
	"fmt"
	"path/filepath"
	"strconv"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
)

// Enqueue adds files to the commit staging queue. If the queue is empty (nil),
// all changed files are implicitly enqueued. Once Enqueue is called, only
// explicitly enqueued files will be committed.
func (c *Checkout) Enqueue(opts EnqueueOpts) error {
	if c == nil {
		panic("checkout.Enqueue: c must not be nil")
	}
	if c.checkinQueue == nil {
		c.checkinQueue = make(map[string]bool)
	}
	for _, p := range opts.Paths {
		c.checkinQueue[p] = true
		if opts.Callback != nil {
			if err := opts.Callback(p); err != nil {
				return fmt.Errorf("checkout.Enqueue: callback for %s: %w", p, err)
			}
		}
	}
	return nil
}

// Dequeue removes files from the commit staging queue. If opts.Paths is empty,
// clears the entire queue (restoring implicit all-files behavior).
func (c *Checkout) Dequeue(opts DequeueOpts) error {
	if c == nil {
		panic("checkout.Dequeue: c must not be nil")
	}
	if len(opts.Paths) == 0 {
		c.checkinQueue = nil // dequeue all
		return nil
	}
	for _, p := range opts.Paths {
		delete(c.checkinQueue, p)
	}
	return nil
}

// IsEnqueued returns true if the named file will be included in the next commit.
// If the queue is nil (never initialized), all changed files are implicitly enqueued.
// If the queue exists but is empty (len == 0), nothing is enqueued.
func (c *Checkout) IsEnqueued(name string) (bool, error) {
	if c == nil {
		panic("checkout.IsEnqueued: c must not be nil")
	}
	if c.checkinQueue == nil {
		return true, nil // nil queue = all changed files implicitly enqueued
	}
	return c.checkinQueue[name], nil
}

// DiscardQueue clears the commit staging queue, restoring implicit all-files behavior.
func (c *Checkout) DiscardQueue() error {
	if c == nil {
		panic("checkout.DiscardQueue: c must not be nil")
	}
	c.checkinQueue = nil
	return nil
}

// Commit creates a new checkin from staged files in the checkout.
// Returns the new manifest RID and UUID.
func (c *Checkout) Commit(opts CommitOpts) (libfossil.FslID, string, error) {
	if c == nil {
		panic("checkout.Commit: c must not be nil")
	}

	// Get current checkout version (parent)
	parentRID, _, err := c.Version()
	if err != nil {
		return 0, "", fmt.Errorf("checkout.Commit: %w", err)
	}

	// Scan for changes (with hashing)
	if err := c.ScanChanges(ScanHash); err != nil {
		return 0, "", fmt.Errorf("checkout.Commit: scan: %w", err)
	}

	// Build the complete file list for the new checkin.
	// Fossil manifests list ALL files, not just changed ones.
	// We need to:
	// 1. Get all files from the parent manifest
	// 2. Apply changes from vfile (modified/deleted)
	// 3. Filter by queue if non-empty

	// Get parent manifest files
	parentFiles, err := manifest.ListFiles(c.repo, parentRID)
	if err != nil {
		return 0, "", fmt.Errorf("checkout.Commit: list parent files: %w", err)
	}

	// Build a map of parent files
	fileMap := make(map[string]manifest.File)
	for _, pf := range parentFiles {
		// We'll read content later if this file hasn't changed
		fileMap[pf.Name] = manifest.File{
			Name: pf.Name,
			Perm: pf.Perm,
		}
	}

	// Query vfile for all files in this checkout
	rows, err := c.db.Query(`
		SELECT pathname, chnged, deleted, rid
		FROM vfile
		WHERE vid = ?
	`, parentRID)
	if err != nil {
		return 0, "", fmt.Errorf("checkout.Commit: query vfile: %w", err)
	}
	defer rows.Close()

	var changedFiles []string
	var deletedFiles []string
	vfileEntries := make(map[string]struct {
		changed bool
		deleted bool
		rid     int64
	})

	for rows.Next() {
		var pathname string
		var chnged, deleted int
		var rid sql.NullInt64
		if err := rows.Scan(&pathname, &chnged, &deleted, &rid); err != nil {
			return 0, "", fmt.Errorf("checkout.Commit: scan vfile: %w", err)
		}
		vfileEntries[pathname] = struct {
			changed bool
			deleted bool
			rid     int64
		}{
			changed: chnged > 0,
			deleted: deleted > 0,
			rid:     rid.Int64,
		}

		if deleted > 0 {
			deletedFiles = append(deletedFiles, pathname)
		} else if chnged > 0 {
			changedFiles = append(changedFiles, pathname)
		}
	}
	if err := rows.Err(); err != nil {
		return 0, "", fmt.Errorf("checkout.Commit: vfile rows: %w", err)
	}

	// Apply queue filter if non-empty
	queueActive := c.checkinQueue != nil && len(c.checkinQueue) > 0
	shouldInclude := func(name string) bool {
		if !queueActive {
			return true // no queue = include all
		}
		return c.checkinQueue[name]
	}

	// Count enqueued files for observer
	enqueuedCount := 0
	if queueActive {
		enqueuedCount = len(c.checkinQueue)
	} else {
		enqueuedCount = len(changedFiles)
	}

	// Start observer
	ctx := c.obs.CommitStarted(context.Background(), CommitStart{
		FilesEnqueued: enqueuedCount,
		Branch:        opts.Branch,
		User:          opts.User,
	})

	var result CommitEnd
	defer func() {
		c.obs.CommitCompleted(ctx, result)
	}()

	// Apply changes to fileMap
	// 1. Remove deleted files
	for _, name := range deletedFiles {
		if shouldInclude(name) {
			delete(fileMap, name)
		}
	}

	// 2. Update changed files with new content from disk
	for _, name := range changedFiles {
		if !shouldInclude(name) {
			continue // skip files not in queue
		}

		// Read content from Storage
		fullPath := filepath.Join(c.dir, name)
		content, err := c.env.Storage.ReadFile(fullPath)
		if err != nil {
			result.Err = fmt.Errorf("checkout.Commit: read %s: %w", name, err)
			return 0, "", result.Err
		}

		// Get perm from existing entry or default to empty
		perm := ""
		if existing, ok := fileMap[name]; ok {
			perm = existing.Perm
		}

		fileMap[name] = manifest.File{
			Name:    name,
			Content: content,
			Perm:    perm,
		}
	}

	// 3. For unchanged files, we need to read their content from the repo
	for name, entry := range fileMap {
		if len(entry.Content) > 0 {
			continue // already have content (was changed)
		}

		// Get the file's RID from vfile or parent manifest
		var fileRID int64
		if ve, ok := vfileEntries[name]; ok {
			fileRID = ve.rid
		} else {
			// File is in parent but not in vfile (shouldn't happen in normal flow)
			// Find it from parent manifest
			for _, pf := range parentFiles {
				if pf.Name == name {
					// Look up RID by UUID
					var rid int64
					err := c.repo.DB().QueryRow("SELECT rid FROM blob WHERE uuid = ?", pf.UUID).Scan(&rid)
					if err != nil {
						result.Err = fmt.Errorf("checkout.Commit: resolve RID for %s: %w", name, err)
						return 0, "", result.Err
					}
					fileRID = rid
					break
				}
			}
		}

		if fileRID == 0 {
			result.Err = fmt.Errorf("checkout.Commit: no RID for unchanged file %s", name)
			return 0, "", result.Err
		}

		// Read content from repo
		content, err := content.Expand(c.repo.DB(), libfossil.FslID(fileRID))
		if err != nil {
			result.Err = fmt.Errorf("checkout.Commit: expand %s: %w", name, err)
			return 0, "", result.Err
		}

		fileMap[name] = manifest.File{
			Name:    name,
			Content: content,
			Perm:    entry.Perm,
		}
	}

	// Convert fileMap to slice
	var commitFiles []manifest.File
	for _, f := range fileMap {
		commitFiles = append(commitFiles, f)
	}

	// Determine commit time
	commitTime := opts.Time
	if commitTime.IsZero() {
		commitTime = c.env.Clock.Now()
	}

	// Call manifest.Checkin
	newRID, newUUID, err := manifest.Checkin(c.repo, manifest.CheckinOpts{
		Files:   commitFiles,
		Comment: opts.Message,
		User:    opts.User,
		Parent:  parentRID,
		Time:    commitTime,
		Delta:   opts.Delta,
		Tags:    nil, // TODO: handle opts.Branch and opts.Tags
	})
	if err != nil {
		result.Err = fmt.Errorf("checkout.Commit: checkin: %w", err)
		return 0, "", result.Err
	}

	// Update vvar checkout and checkout-hash
	if err := setVVar(c.db, "checkout", strconv.FormatInt(int64(newRID), 10)); err != nil {
		result.Err = fmt.Errorf("checkout.Commit: set checkout vvar: %w", err)
		return 0, "", result.Err
	}
	if err := setVVar(c.db, "checkout-hash", newUUID); err != nil {
		result.Err = fmt.Errorf("checkout.Commit: set checkout-hash vvar: %w", err)
		return 0, "", result.Err
	}

	// Reload vfile to reflect the new version
	if _, err := c.LoadVFile(newRID, true); err != nil {
		result.Err = fmt.Errorf("checkout.Commit: reload vfile: %w", err)
		return 0, "", result.Err
	}

	// Clear the checkin queue
	c.checkinQueue = nil

	// Set result for observer
	result.RID = newRID
	result.UUID = newUUID
	result.FilesCommit = len(commitFiles)
	result.Err = nil

	return newRID, newUUID, nil
}
