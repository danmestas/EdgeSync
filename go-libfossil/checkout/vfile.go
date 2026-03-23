package checkout

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/hash"
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

// ScanChanges detects modified and missing files in the checkout.
// Walks the vfile table, checks each file on disk, and updates vfile.chnged accordingly.
//
// If flags includes ScanHash, hashes file content and compares to vfile.mhash.
// Otherwise, uses mtime-based detection (future enhancement).
//
// Panics if c is nil (TigerStyle precondition).
func (c *Checkout) ScanChanges(flags ScanFlags) error {
	if c == nil {
		panic("checkout.ScanChanges: nil *Checkout")
	}

	// Start observer
	ctx := c.obs.ScanStarted(context.Background())

	var filesScanned, filesChanged, filesMissing int

	defer func() {
		c.obs.ScanCompleted(ctx, ScanEnd{
			FilesScanned: filesScanned,
			FilesChanged: filesChanged,
			FilesMissing: filesMissing,
		})
	}()

	// Get current checkout version
	rid, _, err := c.Version()
	if err != nil {
		return fmt.Errorf("checkout.ScanChanges: %w", err)
	}

	// Query all vfile rows for this version
	rows, err := c.db.Query(`
		SELECT id, pathname, rid, mhash, chnged, deleted FROM vfile WHERE vid = ?
	`, int64(rid))
	if err != nil {
		return fmt.Errorf("checkout.ScanChanges: query vfile: %w", err)
	}

	// Collect all vfile entries first (to avoid database lock during iteration)
	type vfileEntry struct {
		id       int64
		pathname string
		blobRid  int64
		mhash    string
		chnged   int64
		deleted  int64
	}
	var entries []vfileEntry

	for rows.Next() {
		var e vfileEntry
		if err := rows.Scan(&e.id, &e.pathname, &e.blobRid, &e.mhash, &e.chnged, &e.deleted); err != nil {
			rows.Close()
			return fmt.Errorf("checkout.ScanChanges: scan vfile row: %w", err)
		}
		entries = append(entries, e)
	}
	rows.Close()

	if err := rows.Err(); err != nil {
		return fmt.Errorf("checkout.ScanChanges: iterate vfile rows: %w", err)
	}

	// Process each vfile entry
	for _, e := range entries {
		filesScanned++

		// Skip if already deleted
		if e.deleted != 0 {
			continue
		}

		// Full path for file
		fullPath := filepath.Join(c.dir, e.pathname)

		// Check if file exists on disk
		data, err := c.env.Storage.ReadFile(fullPath)
		if err != nil {
			if os.IsNotExist(err) {
				// File is missing — note it but don't update chnged
				filesMissing++
				continue
			}
			// Other read error
			return fmt.Errorf("checkout.ScanChanges: read %s: %w", fullPath, err)
		}

		// If ScanHash flag is set, hash the file content
		if flags&ScanHash != 0 {
			// Compute content hash using the same algorithm as mhash
			diskHash := hash.ContentHash(data, e.mhash)

			// Compare hashes
			if diskHash != e.mhash {
				// File has been modified
				if e.chnged == 0 {
					// Mark as changed
					_, err := c.db.Exec("UPDATE vfile SET chnged = 1 WHERE id = ?", e.id)
					if err != nil {
						return fmt.Errorf("checkout.ScanChanges: update chnged for %s: %w", e.pathname, err)
					}
					filesChanged++
				}
			} else {
				// File matches — reset chnged if it was set
				if e.chnged != 0 {
					_, err := c.db.Exec("UPDATE vfile SET chnged = 0 WHERE id = ?", e.id)
					if err != nil {
						return fmt.Errorf("checkout.ScanChanges: reset chnged for %s: %w", e.pathname, err)
					}
				}
			}
		}
	}

	return nil
}

// walkDir recursively lists all files under a directory via Storage.ReadDir.
// Returns a map of relative paths (relative to c.dir) to true.
// This helper prepares for detecting EXTRA files (files on disk not in vfile).
//
// Panics if c is nil (TigerStyle precondition).
func (c *Checkout) walkDir(dir string) (map[string]bool, error) {
	if c == nil {
		panic("checkout.walkDir: nil *Checkout")
	}

	result := make(map[string]bool)

	var walk func(string) error
	walk = func(currentPath string) error {
		entries, err := c.env.Storage.ReadDir(currentPath)
		if err != nil {
			if os.IsNotExist(err) {
				// Directory doesn't exist — not an error
				return nil
			}
			return fmt.Errorf("walkDir: read %s: %w", currentPath, err)
		}

		for _, entry := range entries {
			fullPath := filepath.Join(currentPath, entry.Name())

			// Skip checkout database
			if entry.Name() == ".fslckout" {
				continue
			}

			if entry.IsDir() {
				// Recurse into subdirectory
				if err := walk(fullPath); err != nil {
					return err
				}
			} else {
				// Regular file — compute relative path
				relPath, err := filepath.Rel(c.dir, fullPath)
				if err != nil {
					return fmt.Errorf("walkDir: compute relative path for %s: %w", fullPath, err)
				}
				result[relPath] = true
			}
		}

		return nil
	}

	if err := walk(dir); err != nil {
		return nil, err
	}

	return result, nil
}
