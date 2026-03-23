package checkout

import (
	"context"
	"database/sql"
	"fmt"
	"os"
	"path/filepath"
	"strconv"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/merge"
)

// CalcUpdateVersion finds the latest leaf checkin to update to.
// Returns 0 if the checkout is already at the latest version.
//
// Panics if c is nil (TigerStyle precondition).
func (c *Checkout) CalcUpdateVersion() (libfossil.FslID, error) {
	if c == nil {
		panic("checkout.CalcUpdateVersion: nil *Checkout")
	}

	currentRID, _, err := c.Version()
	if err != nil {
		return 0, fmt.Errorf("checkout.CalcUpdateVersion: %w", err)
	}

	var tipRID int64
	err = c.repo.DB().QueryRow(`
		SELECT l.rid FROM leaf l
		JOIN event e ON e.objid=l.rid
		WHERE e.type='ci'
		ORDER BY e.mtime DESC LIMIT 1
	`).Scan(&tipRID)
	if err != nil {
		if err == sql.ErrNoRows {
			return 0, nil // no checkins at all
		}
		return 0, fmt.Errorf("checkout.CalcUpdateVersion: %w", err)
	}

	if libfossil.FslID(tipRID) == currentRID {
		return 0, nil // already at latest
	}

	return libfossil.FslID(tipRID), nil
}

// Update updates the checkout to a new version, performing 3-way merge where
// needed to preserve local modifications.
//
// If opts.TargetRID is 0, CalcUpdateVersion is used to find the target.
// If the target equals the current version, Update returns nil (nothing to do).
//
// Panics if c is nil (TigerStyle precondition).
func (c *Checkout) Update(opts UpdateOpts) error {
	if c == nil {
		panic("checkout.Update: nil *Checkout")
	}

	// Determine target
	target := opts.TargetRID
	if target == 0 {
		var err error
		target, err = c.CalcUpdateVersion()
		if err != nil {
			return fmt.Errorf("checkout.Update: %w", err)
		}
		if target == 0 {
			return nil // nothing to update
		}
	}

	// Get current version
	currentRID, _, err := c.Version()
	if err != nil {
		return fmt.Errorf("checkout.Update: %w", err)
	}

	if currentRID == target {
		return nil // already at target
	}

	// Start observer
	ctx := c.obs.ExtractStarted(context.Background(), ExtractStart{
		Operation: "update",
		TargetRID: target,
	})

	var filesWritten, filesRemoved, conflicts int
	var updateErr error

	defer func() {
		c.obs.ExtractCompleted(ctx, ExtractEnd{
			Operation:    "update",
			TargetRID:    target,
			FilesWritten: filesWritten,
			FilesRemoved: filesRemoved,
			Conflicts:    conflicts,
			Err:          updateErr,
		})
	}()

	// Get file lists for current and target
	currentFiles, err := manifest.ListFiles(c.repo, currentRID)
	if err != nil {
		updateErr = fmt.Errorf("checkout.Update: list current files: %w", err)
		return updateErr
	}
	targetFiles, err := manifest.ListFiles(c.repo, target)
	if err != nil {
		updateErr = fmt.Errorf("checkout.Update: list target files: %w", err)
		return updateErr
	}

	// Find common ancestor
	ancestor, err := merge.FindCommonAncestor(c.repo, currentRID, target)
	if err != nil {
		// If no common ancestor found, treat current as ancestor (linear update)
		ancestor = currentRID
	}

	// Get ancestor files (if ancestor differs from current)
	var ancestorFiles []manifest.FileEntry
	if ancestor == currentRID {
		ancestorFiles = currentFiles
	} else {
		ancestorFiles, err = manifest.ListFiles(c.repo, ancestor)
		if err != nil {
			updateErr = fmt.Errorf("checkout.Update: list ancestor files: %w", err)
			return updateErr
		}
	}

	// Build file maps (name → UUID)
	currentMap := make(map[string]string, len(currentFiles))
	for _, f := range currentFiles {
		currentMap[f.Name] = f.UUID
	}
	targetMap := make(map[string]string, len(targetFiles))
	for _, f := range targetFiles {
		targetMap[f.Name] = f.UUID
	}
	ancestorMap := make(map[string]string, len(ancestorFiles))
	for _, f := range ancestorFiles {
		ancestorMap[f.Name] = f.UUID
	}

	// Collect all file names across all three versions
	allNames := make(map[string]bool)
	for name := range currentMap {
		allNames[name] = true
	}
	for name := range targetMap {
		allNames[name] = true
	}

	// Get the 3-way merge strategy
	strategy, ok := merge.StrategyByName("three-way")
	if !ok {
		updateErr = fmt.Errorf("checkout.Update: three-way merge strategy not registered")
		return updateErr
	}

	// Process each file
	for name := range allNames {
		curUUID, inCurrent := currentMap[name]
		tgtUUID, inTarget := targetMap[name]
		ancUUID := ancestorMap[name]

		change, err := c.updateFile(name, curUUID, tgtUUID, ancUUID, inCurrent, inTarget, strategy, opts.DryRun)
		if err != nil {
			updateErr = fmt.Errorf("checkout.Update: %w", err)
			return updateErr
		}

		if change == UpdateNone {
			continue
		}

		// Track stats
		switch change {
		case UpdateAdded, UpdateUpdated, UpdateMerged:
			filesWritten++
		case UpdateConflictMerged:
			filesWritten++
			conflicts++
		case UpdateRemoved:
			filesRemoved++
		}

		// Notify observer
		c.obs.ExtractFileCompleted(ctx, name, change)

		// Call user callback
		if opts.Callback != nil {
			if err := opts.Callback(name, change); err != nil {
				updateErr = fmt.Errorf("checkout.Update: callback for %s: %w", name, err)
				return updateErr
			}
		}
	}

	// Reload vfile for target RID
	if _, err := c.LoadVFile(target, true); err != nil {
		updateErr = fmt.Errorf("checkout.Update: reload vfile: %w", err)
		return updateErr
	}

	// Look up target UUID
	var targetUUID string
	err = c.repo.DB().QueryRow("SELECT uuid FROM blob WHERE rid = ?", int64(target)).Scan(&targetUUID)
	if err != nil {
		updateErr = fmt.Errorf("checkout.Update: query target uuid: %w", err)
		return updateErr
	}

	// Update vvar checkout and checkout-hash
	if err := setVVar(c.db, "checkout", strconv.FormatInt(int64(target), 10)); err != nil {
		updateErr = fmt.Errorf("checkout.Update: %w", err)
		return updateErr
	}
	if err := setVVar(c.db, "checkout-hash", targetUUID); err != nil {
		updateErr = fmt.Errorf("checkout.Update: %w", err)
		return updateErr
	}

	return nil
}

// updateFile handles the update logic for a single file across three versions.
// Returns the UpdateChange type applied, or UpdateNone if the file was skipped.
func (c *Checkout) updateFile(
	name string,
	curUUID, tgtUUID, ancUUID string,
	inCurrent, inTarget bool,
	strategy merge.Strategy,
	dryRun bool,
) (UpdateChange, error) {
	switch {
	case !inCurrent && inTarget:
		// New file in target — extract and write
		if !dryRun {
			if err := c.writeFileFromUUID(name, tgtUUID); err != nil {
				return UpdateNone, fmt.Errorf("add %s: %w", name, err)
			}
		}
		return UpdateAdded, nil

	case inCurrent && !inTarget:
		// File removed in target — delete from disk
		if !dryRun {
			fullPath := filepath.Join(c.dir, name)
			if err := c.env.Storage.Remove(fullPath); err != nil {
				// Ignore errors if file already missing
				if !os.IsNotExist(err) {
					return UpdateNone, fmt.Errorf("remove %s: %w", name, err)
				}
			}
		}
		return UpdateRemoved, nil

	case inCurrent && inTarget:
		if curUUID == tgtUUID {
			// Same content — skip
			return UpdateNone, nil
		}

		// Different content — determine who changed
		if curUUID == ancUUID {
			// Current matches ancestor, target changed — fast-forward
			if !dryRun {
				if err := c.writeFileFromUUID(name, tgtUUID); err != nil {
					return UpdateNone, fmt.Errorf("update %s: %w", name, err)
				}
			}
			return UpdateUpdated, nil
		}

		if tgtUUID == ancUUID {
			// Target matches ancestor, current changed — keep current
			return UpdateNone, nil
		}

		// Both changed — 3-way merge needed
		return c.mergeFile(name, ancUUID, curUUID, tgtUUID, strategy, dryRun)

	default:
		// Not in current, not in target — impossible given allNames construction
		return UpdateNone, nil
	}
}

// mergeFile performs a 3-way merge of a file that was modified in both current and target.
func (c *Checkout) mergeFile(
	name, ancUUID, curUUID, tgtUUID string,
	strategy merge.Strategy,
	dryRun bool,
) (UpdateChange, error) {
	// Expand all three versions
	ancData, err := c.expandUUID(ancUUID)
	if err != nil {
		return UpdateNone, fmt.Errorf("merge %s: expand ancestor: %w", name, err)
	}
	curData, err := c.expandUUID(curUUID)
	if err != nil {
		return UpdateNone, fmt.Errorf("merge %s: expand current: %w", name, err)
	}
	tgtData, err := c.expandUUID(tgtUUID)
	if err != nil {
		return UpdateNone, fmt.Errorf("merge %s: expand target: %w", name, err)
	}

	result, err := strategy.Merge(ancData, curData, tgtData)
	if err != nil {
		return UpdateNone, fmt.Errorf("merge %s: %w", name, err)
	}

	if !dryRun {
		fullPath := filepath.Join(c.dir, name)
		parentDir := filepath.Dir(fullPath)
		if err := c.env.Storage.MkdirAll(parentDir, os.FileMode(0o755)); err != nil {
			return UpdateNone, fmt.Errorf("merge %s: mkdir: %w", name, err)
		}
		if err := c.env.Storage.WriteFile(fullPath, result.Content, os.FileMode(0o644)); err != nil {
			return UpdateNone, fmt.Errorf("merge %s: write: %w", name, err)
		}
	}

	if result.Clean {
		return UpdateMerged, nil
	}
	return UpdateConflictMerged, nil
}

// writeFileFromUUID expands a blob by UUID and writes it to the checkout directory.
func (c *Checkout) writeFileFromUUID(name, uuid string) error {
	data, err := c.expandUUID(uuid)
	if err != nil {
		return fmt.Errorf("writeFileFromUUID %s: %w", name, err)
	}

	fullPath := filepath.Join(c.dir, name)
	parentDir := filepath.Dir(fullPath)
	if err := c.env.Storage.MkdirAll(parentDir, os.FileMode(0o755)); err != nil {
		return fmt.Errorf("mkdir %s: %w", parentDir, err)
	}
	if err := c.env.Storage.WriteFile(fullPath, data, os.FileMode(0o644)); err != nil {
		return fmt.Errorf("write %s: %w", fullPath, err)
	}
	return nil
}

// expandUUID resolves a UUID to its expanded blob content.
func (c *Checkout) expandUUID(uuid string) ([]byte, error) {
	rid, ok := blob.Exists(c.repo.DB(), uuid)
	if !ok {
		return nil, fmt.Errorf("blob not found for uuid %s", uuid)
	}
	data, err := content.Expand(c.repo.DB(), rid)
	if err != nil {
		return nil, fmt.Errorf("expand rid %d (uuid %s): %w", rid, uuid, err)
	}
	return data, nil
}
