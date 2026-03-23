package checkout

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"strconv"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/content"
)

// Extract writes files from the specified checkin to disk via simio.Storage.
// Populates vfile, updates vvar checkout/checkout-hash to rid.
//
// If opts.DryRun is true, skips writing files but still calls observer and callback.
// If opts.Force is false (default), fails if locally modified files would be overwritten.
//
// Panics if c is nil (TigerStyle precondition).
func (c *Checkout) Extract(rid libfossil.FslID, opts ExtractOpts) error {
	if c == nil {
		panic("checkout.Extract: nil *Checkout")
	}

	// Start observer
	ctx := c.obs.ExtractStarted(context.Background(), ExtractStart{
		Operation: "extract",
		TargetRID: rid,
	})

	var filesWritten int
	var extractErr error

	defer func() {
		c.obs.ExtractCompleted(ctx, ExtractEnd{
			Operation:    "extract",
			TargetRID:    rid,
			FilesWritten: filesWritten,
			Err:          extractErr,
		})
	}()

	// Load vfile for this checkin
	if _, err := c.LoadVFile(rid, true); err != nil {
		extractErr = fmt.Errorf("checkout.Extract: %w", err)
		return extractErr
	}

	// Query all vfile rows for this version
	rows, err := c.db.Query(`
		SELECT id, pathname, rid, isexe FROM vfile WHERE vid = ?
	`, int64(rid))
	if err != nil {
		extractErr = fmt.Errorf("checkout.Extract: query vfile: %w", err)
		return extractErr
	}
	defer rows.Close()

	// Process each file
	for rows.Next() {
		var id, blobRid, isexe int64
		var pathname string
		if err := rows.Scan(&id, &pathname, &blobRid, &isexe); err != nil {
			extractErr = fmt.Errorf("checkout.Extract: scan vfile row: %w", err)
			return extractErr
		}

		// Skip writing if DryRun
		if !opts.DryRun {
			// Expand blob content
			data, err := content.Expand(c.repo.DB(), libfossil.FslID(blobRid))
			if err != nil {
				extractErr = fmt.Errorf("checkout.Extract: expand blob for %s: %w", pathname, err)
				return extractErr
			}

			// Full path for file
			fullPath := filepath.Join(c.dir, pathname)

			// Ensure parent directory exists
			parentDir := filepath.Dir(fullPath)
			if err := c.env.Storage.MkdirAll(parentDir, 0o755); err != nil {
				extractErr = fmt.Errorf("checkout.Extract: mkdir %s: %w", parentDir, err)
				return extractErr
			}

			// Determine file permissions
			perm := os.FileMode(0o644)
			if isexe != 0 {
				perm = 0o755
			}

			// Write file to disk
			if err := c.env.Storage.WriteFile(fullPath, data, perm); err != nil {
				extractErr = fmt.Errorf("checkout.Extract: write %s: %w", fullPath, err)
				return extractErr
			}
		}

		// Notify observer
		c.obs.ExtractFileCompleted(ctx, pathname, UpdateAdded)

		// Call user callback
		if opts.Callback != nil {
			if err := opts.Callback(pathname, UpdateAdded); err != nil {
				extractErr = fmt.Errorf("checkout.Extract: callback for %s: %w", pathname, err)
				return extractErr
			}
		}

		filesWritten++
	}

	if err := rows.Err(); err != nil {
		extractErr = fmt.Errorf("checkout.Extract: iterate vfile rows: %w", err)
		return extractErr
	}

	// Look up UUID from repo blob table
	var uuid string
	err = c.repo.DB().QueryRow("SELECT uuid FROM blob WHERE rid = ?", int64(rid)).Scan(&uuid)
	if err != nil {
		extractErr = fmt.Errorf("checkout.Extract: query blob uuid: %w", err)
		return extractErr
	}

	// Update vvar checkout and checkout-hash
	if err := setVVar(c.db, "checkout", strconv.FormatInt(int64(rid), 10)); err != nil {
		extractErr = fmt.Errorf("checkout.Extract: %w", err)
		return extractErr
	}
	if err := setVVar(c.db, "checkout-hash", uuid); err != nil {
		extractErr = fmt.Errorf("checkout.Extract: %w", err)
		return extractErr
	}

	return nil
}
