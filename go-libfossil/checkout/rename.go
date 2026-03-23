package checkout

import (
	"database/sql"
	"fmt"
	"os"
	"path/filepath"
)

// Rename marks a file as renamed in vfile by updating pathname and origname.
// Sets chnged=1 to indicate the file has been modified.
//
// If opts.DoFsMove is true, also moves the file in Storage from old to new path.
//
// Panics if c is nil, or if From or To is empty (TigerStyle precondition).
func (c *Checkout) Rename(opts RenameOpts) error {
	if c == nil {
		panic("checkout.Rename: nil *Checkout")
	}
	if opts.From == "" {
		panic("checkout.Rename: empty From")
	}
	if opts.To == "" {
		panic("checkout.Rename: empty To")
	}

	// Get current version
	vid, _, err := c.Version()
	if err != nil {
		return fmt.Errorf("checkout.Rename: %w", err)
	}

	// Verify From exists in vfile
	var vfileID int64
	err = c.db.QueryRow("SELECT id FROM vfile WHERE vid=? AND pathname=?", int64(vid), opts.From).Scan(&vfileID)
	if err == sql.ErrNoRows {
		return fmt.Errorf("checkout.Rename: file %s not found in vfile", opts.From)
	}
	if err != nil {
		return fmt.Errorf("checkout.Rename: query vfile for %s: %w", opts.From, err)
	}

	// Verify To does NOT exist in vfile (uniqueness check)
	var existingID int64
	err = c.db.QueryRow("SELECT id FROM vfile WHERE vid=? AND pathname=?", int64(vid), opts.To).Scan(&existingID)
	if err != nil && err != sql.ErrNoRows {
		return fmt.Errorf("checkout.Rename: query vfile for %s: %w", opts.To, err)
	}
	if err == nil {
		return fmt.Errorf("checkout.Rename: target file %s already exists in vfile", opts.To)
	}

	// Update vfile: set new pathname, store old pathname in origname, mark as changed
	_, err = c.db.Exec(
		"UPDATE vfile SET pathname=?, origname=?, chnged=1 WHERE id=?",
		opts.To, opts.From, vfileID,
	)
	if err != nil {
		return fmt.Errorf("checkout.Rename: update vfile: %w", err)
	}

	// If DoFsMove, move the file in Storage
	if opts.DoFsMove {
		oldPath := filepath.Join(c.dir, opts.From)
		newPath := filepath.Join(c.dir, opts.To)

		// Read old file
		data, err := c.env.Storage.ReadFile(oldPath)
		if err != nil {
			return fmt.Errorf("checkout.Rename: read old file %s: %w", oldPath, err)
		}

		// Ensure parent directory exists for new path
		newParentDir := filepath.Dir(newPath)
		if err := c.env.Storage.MkdirAll(newParentDir, 0o755); err != nil {
			return fmt.Errorf("checkout.Rename: mkdir %s: %w", newParentDir, err)
		}

		// Write to new path
		// Query file permissions from vfile
		var isexe int64
		err = c.db.QueryRow("SELECT isexe FROM vfile WHERE id=?", vfileID).Scan(&isexe)
		if err != nil {
			return fmt.Errorf("checkout.Rename: query vfile permissions: %w", err)
		}

		perm := os.FileMode(0o644)
		if isexe != 0 {
			perm = 0o755
		}

		if err := c.env.Storage.WriteFile(newPath, data, perm); err != nil {
			return fmt.Errorf("checkout.Rename: write new file %s: %w", newPath, err)
		}

		// Remove old file
		if err := c.env.Storage.Remove(oldPath); err != nil {
			// Best effort — don't fail if removal fails
		}
	}

	// Call callback if provided
	if opts.Callback != nil {
		if err := opts.Callback(opts.From, opts.To); err != nil {
			return fmt.Errorf("checkout.Rename: callback: %w", err)
		}
	}

	return nil
}

// RevertRename restores a renamed file to its original pathname.
// Returns (true, nil) if the revert succeeded, (false, nil) if there was nothing to revert.
//
// Panics if c is nil or name is empty (TigerStyle precondition).
func (c *Checkout) RevertRename(name string, doFsMove bool) (bool, error) {
	if c == nil {
		panic("checkout.RevertRename: nil *Checkout")
	}
	if name == "" {
		panic("checkout.RevertRename: empty name")
	}

	// Get current version
	vid, _, err := c.Version()
	if err != nil {
		return false, fmt.Errorf("checkout.RevertRename: %w", err)
	}

	// Look up vfile entry
	var vfileID int64
	var origname sql.NullString
	err = c.db.QueryRow("SELECT id, origname FROM vfile WHERE vid=? AND pathname=?", int64(vid), name).Scan(&vfileID, &origname)
	if err == sql.ErrNoRows {
		return false, fmt.Errorf("checkout.RevertRename: file %s not found in vfile", name)
	}
	if err != nil {
		return false, fmt.Errorf("checkout.RevertRename: query vfile for %s: %w", name, err)
	}

	// If origname is empty/NULL, nothing to revert
	if !origname.Valid || origname.String == "" {
		return false, nil
	}

	oldOrigName := origname.String

	// Update vfile: restore origname as pathname, clear origname, reset chnged
	_, err = c.db.Exec(
		"UPDATE vfile SET pathname=?, origname=NULL, chnged=0 WHERE id=?",
		oldOrigName, vfileID,
	)
	if err != nil {
		return false, fmt.Errorf("checkout.RevertRename: update vfile: %w", err)
	}

	// If doFsMove, move the file back in Storage
	if doFsMove {
		currentPath := filepath.Join(c.dir, name)
		originalPath := filepath.Join(c.dir, oldOrigName)

		// Read current file
		data, err := c.env.Storage.ReadFile(currentPath)
		if err != nil {
			return false, fmt.Errorf("checkout.RevertRename: read current file %s: %w", currentPath, err)
		}

		// Ensure parent directory exists for original path
		originalParentDir := filepath.Dir(originalPath)
		if err := c.env.Storage.MkdirAll(originalParentDir, 0o755); err != nil {
			return false, fmt.Errorf("checkout.RevertRename: mkdir %s: %w", originalParentDir, err)
		}

		// Write to original path
		// Query file permissions from vfile
		var isexe int64
		err = c.db.QueryRow("SELECT isexe FROM vfile WHERE id=?", vfileID).Scan(&isexe)
		if err != nil {
			return false, fmt.Errorf("checkout.RevertRename: query vfile permissions: %w", err)
		}

		perm := os.FileMode(0o644)
		if isexe != 0 {
			perm = 0o755
		}

		if err := c.env.Storage.WriteFile(originalPath, data, perm); err != nil {
			return false, fmt.Errorf("checkout.RevertRename: write original file %s: %w", originalPath, err)
		}

		// Remove current file
		if err := c.env.Storage.Remove(currentPath); err != nil {
			// Best effort — don't fail if removal fails
		}
	}

	return true, nil
}
