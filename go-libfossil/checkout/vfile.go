package checkout

import (
	"fmt"
	"strings"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
)

// LoadVFile populates vfile table with entries from the specified checkin manifest.
// If clear=true, deletes all vfile rows for OTHER versions (keeps only vid=rid).
// Returns the count of missing blobs (files whose content is not in the repo).
//
// Panics if c is nil (TigerStyle precondition).
func (c *Checkout) LoadVFile(rid libfossil.FslID, clear bool) (missing uint32, err error) {
	if c == nil {
		panic("checkout.LoadVFile: nil *Checkout")
	}

	// Clear other versions if requested
	if clear {
		if _, err := c.db.Exec("DELETE FROM vfile WHERE vid != ?", int64(rid)); err != nil {
			return 0, fmt.Errorf("checkout.LoadVFile: clear: %w", err)
		}
	}

	// Get file list from manifest
	files, err := manifest.ListFiles(c.repo, rid)
	if err != nil {
		return 0, fmt.Errorf("checkout.LoadVFile: %w", err)
	}

	// Insert each file into vfile
	for _, file := range files {
		// Look up blob RID
		blobRID, exists := blob.Exists(c.repo.DB(), file.UUID)
		if !exists {
			// Blob not found - increment missing count
			missing++
			// Insert with rid=0 to mark as missing
			blobRID = 0
		}

		// Determine isexe flag
		isexe := 0
		if strings.Contains(file.Perm, "x") {
			isexe = 1
		}

		// Insert vfile row (INSERT OR IGNORE handles duplicates)
		_, err := c.db.Exec(`
			INSERT OR IGNORE INTO vfile(vid, pathname, rid, mrid, mhash, isexe, islink)
			VALUES(?, ?, ?, ?, ?, ?, ?)`,
			int64(rid),
			file.Name,
			int64(blobRID),
			int64(blobRID),
			file.UUID,
			isexe,
			0, // islink - symlinks not tracked yet
		)
		if err != nil {
			return 0, fmt.Errorf("checkout.LoadVFile: insert %s: %w", file.Name, err)
		}
	}

	return missing, nil
}

// UnloadVFile removes all vfile entries for the specified version.
//
// Panics if c is nil (TigerStyle precondition).
func (c *Checkout) UnloadVFile(rid libfossil.FslID) error {
	if c == nil {
		panic("checkout.UnloadVFile: nil *Checkout")
	}

	_, err := c.db.Exec("DELETE FROM vfile WHERE vid = ?", int64(rid))
	if err != nil {
		return fmt.Errorf("checkout.UnloadVFile: %w", err)
	}

	return nil
}
