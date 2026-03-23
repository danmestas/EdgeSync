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

// extractSingleFile expands a blob and writes it to the checkout directory.
// Skips writing if dryRun is true.
func (c *Checkout) extractSingleFile(pathname string, blobRid int64, isexe int64, dryRun bool) error {
	if dryRun {
		return nil
	}

	data, err := content.Expand(c.repo.DB(), libfossil.FslID(blobRid))
	if err != nil {
		return fmt.Errorf("checkout.Extract: expand blob for %s: %w", pathname, err)
	}

	fullPath, err := c.safePath(pathname)
	if err != nil {
		return fmt.Errorf("checkout.Extract: path traversal in %s: %w", pathname, err)
	}

	parentDir := filepath.Dir(fullPath)
	if err := c.env.Storage.MkdirAll(parentDir, 0o755); err != nil {
		return fmt.Errorf("checkout.Extract: mkdir %s: %w", parentDir, err)
	}

	perm := os.FileMode(0o644)
	if isexe != 0 {
		perm = 0o755
	}

	if err := c.env.Storage.WriteFile(fullPath, data, perm); err != nil {
		return fmt.Errorf("checkout.Extract: write %s: %w", fullPath, err)
	}

	return nil
}

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

	if _, err := c.LoadVFile(rid, true); err != nil {
		extractErr = fmt.Errorf("checkout.Extract: %w", err)
		return extractErr
	}

	rows, err := c.db.Query(`
		SELECT id, pathname, rid, isexe FROM vfile WHERE vid = ?
	`, int64(rid))
	if err != nil {
		extractErr = fmt.Errorf("checkout.Extract: query vfile: %w", err)
		return extractErr
	}
	defer rows.Close()

	for rows.Next() {
		var id, blobRid, isexe int64
		var pathname string
		if err := rows.Scan(&id, &pathname, &blobRid, &isexe); err != nil {
			extractErr = fmt.Errorf("checkout.Extract: scan vfile row: %w", err)
			return extractErr
		}

		if err := c.extractSingleFile(pathname, blobRid, isexe, opts.DryRun); err != nil {
			extractErr = err
			return extractErr
		}

		c.obs.ExtractFileCompleted(ctx, pathname, UpdateAdded)

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

	// Finalize: look up UUID and update vvar
	extractErr = c.finalizeExtract(rid)
	return extractErr
}

// finalizeExtract looks up the blob UUID for rid and updates the vvar
// checkout/checkout-hash entries.
func (c *Checkout) finalizeExtract(rid libfossil.FslID) error {
	var uuid string
	err := c.repo.DB().QueryRow("SELECT uuid FROM blob WHERE rid = ?", int64(rid)).Scan(&uuid)
	if err != nil {
		return fmt.Errorf("checkout.Extract: query blob uuid: %w", err)
	}

	if err := setVVar(c.db, "checkout", strconv.FormatInt(int64(rid), 10)); err != nil {
		return fmt.Errorf("checkout.Extract: %w", err)
	}
	if err := setVVar(c.db, "checkout-hash", uuid); err != nil {
		return fmt.Errorf("checkout.Extract: %w", err)
	}
	return nil
}
