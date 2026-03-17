// Package stash saves and restores working directory changes, storing deltas
// against baseline blobs in the checkout database (.fslckout).
package stash

import (
	"database/sql"
	"fmt"
	"os"
	"path/filepath"
	"strconv"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/delta"
)

// Entry represents a single stash entry.
type Entry struct {
	ID      int64
	Hash    string // UUID of checkout manifest (baseline version)
	Comment string
	CTime   string
}

// EnsureTables creates the stash and stashfile tables if they don't exist.
func EnsureTables(ckout *sql.DB) error {
	stmts := []string{
		`CREATE TABLE IF NOT EXISTS stash(
			stashid INTEGER PRIMARY KEY,
			hash    TEXT,
			comment TEXT,
			ctime   TIMESTAMP DEFAULT CURRENT_TIMESTAMP
		)`,
		`CREATE TABLE IF NOT EXISTS stashfile(
			stashid   INTEGER REFERENCES stash,
			isAdded   BOOLEAN,
			isRemoved BOOLEAN,
			isExec    BOOLEAN,
			isLink    BOOLEAN,
			hash      TEXT,
			origname  TEXT,
			newname   TEXT,
			delta     BLOB,
			PRIMARY KEY(newname, stashid)
		)`,
	}
	for _, s := range stmts {
		if _, err := ckout.Exec(s); err != nil {
			return fmt.Errorf("stash.EnsureTables: %w", err)
		}
	}
	return nil
}

// nextStashID reads and increments the stash-next vvar counter.
func nextStashID(tx *sql.Tx) (int64, error) {
	var val string
	err := tx.QueryRow("SELECT value FROM vvar WHERE name='stash-next'").Scan(&val)
	if err != nil {
		if err == sql.ErrNoRows {
			// First stash: start at 1.
			if _, err := tx.Exec("INSERT INTO vvar(name,value) VALUES('stash-next','2')"); err != nil {
				return 0, fmt.Errorf("stash: init stash-next: %w", err)
			}
			return 1, nil
		}
		return 0, fmt.Errorf("stash: read stash-next: %w", err)
	}
	id, err := strconv.ParseInt(val, 10, 64)
	if err != nil {
		return 0, fmt.Errorf("stash: parse stash-next %q: %w", val, err)
	}
	if _, err := tx.Exec("REPLACE INTO vvar(name,value) VALUES('stash-next',?)", strconv.FormatInt(id+1, 10)); err != nil {
		return 0, fmt.Errorf("stash: bump stash-next: %w", err)
	}
	return id, nil
}

// Save stashes all changed files in the checkout, then reverts the working directory.
func Save(ckout *sql.DB, repoDB *sql.DB, dir string, comment string) error {
	if err := EnsureTables(ckout); err != nil {
		return err
	}

	tx, err := ckout.Begin()
	if err != nil {
		return fmt.Errorf("stash.Save: begin tx: %w", err)
	}
	defer tx.Rollback()

	// Get checkout hash (manifest UUID).
	var checkoutHash string
	err = tx.QueryRow("SELECT value FROM vvar WHERE name='checkout-hash'").Scan(&checkoutHash)
	if err != nil {
		// Fall back to checkout rid if checkout-hash not available.
		checkoutHash = ""
	}

	stashID, err := nextStashID(tx)
	if err != nil {
		return err
	}

	// Insert stash header.
	if _, err := tx.Exec("INSERT INTO stash(stashid, hash, comment) VALUES(?,?,?)",
		stashID, checkoutHash, comment); err != nil {
		return fmt.Errorf("stash.Save: insert stash: %w", err)
	}

	// Query vfile for changed files: chnged=1 OR deleted=1 OR rid=0 (added).
	rows, err := tx.Query(`SELECT pathname, rid, chnged, deleted, isexe, islink
		FROM vfile WHERE chnged=1 OR deleted=1 OR rid=0`)
	if err != nil {
		return fmt.Errorf("stash.Save: query vfile: %w", err)
	}

	type changedFile struct {
		pathname string
		rid      int64
		chnged   int
		deleted  int
		isExec   bool
		isLink   bool
	}
	var files []changedFile
	for rows.Next() {
		var f changedFile
		if err := rows.Scan(&f.pathname, &f.rid, &f.chnged, &f.deleted, &f.isExec, &f.isLink); err != nil {
			rows.Close()
			return fmt.Errorf("stash.Save: scan vfile: %w", err)
		}
		files = append(files, f)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return fmt.Errorf("stash.Save: rows iteration: %w", err)
	}

	if len(files) == 0 {
		return fmt.Errorf("stash.Save: no changes to stash")
	}

	ins, err := tx.Prepare(`INSERT INTO stashfile(stashid, isAdded, isRemoved, isExec, isLink, hash, origname, newname, delta)
		VALUES(?,?,?,?,?,?,?,?,?)`)
	if err != nil {
		return fmt.Errorf("stash.Save: prepare insert: %w", err)
	}
	defer ins.Close()

	for _, f := range files {
		fullPath := filepath.Join(dir, f.pathname)
		isAdded := f.rid == 0
		isRemoved := f.deleted == 1

		var baselineHash string
		var deltaBytes []byte

		if isAdded {
			// Added file: store raw content, no baseline hash.
			data, err := os.ReadFile(fullPath)
			if err != nil {
				return fmt.Errorf("stash.Save: read added %s: %w", f.pathname, err)
			}
			deltaBytes = data
		} else if isRemoved {
			// Removed file: get baseline hash, empty delta.
			var uuid string
			err := repoDB.QueryRow("SELECT uuid FROM blob WHERE rid=?", f.rid).Scan(&uuid)
			if err != nil {
				return fmt.Errorf("stash.Save: get uuid for rid=%d: %w", f.rid, err)
			}
			baselineHash = uuid
			deltaBytes = []byte{}
		} else {
			// Modified file: compute delta from baseline to working content.
			var uuid string
			err := repoDB.QueryRow("SELECT uuid FROM blob WHERE rid=?", f.rid).Scan(&uuid)
			if err != nil {
				return fmt.Errorf("stash.Save: get uuid for rid=%d: %w", f.rid, err)
			}
			baselineHash = uuid

			baseline, err := content.Expand(repoDB, libfossil.FslID(f.rid))
			if err != nil {
				return fmt.Errorf("stash.Save: expand rid=%d: %w", f.rid, err)
			}

			working, err := os.ReadFile(fullPath)
			if err != nil {
				return fmt.Errorf("stash.Save: read %s: %w", f.pathname, err)
			}

			deltaBytes = delta.Create(baseline, working)
		}

		if _, err := ins.Exec(stashID, isAdded, isRemoved, f.isExec, f.isLink,
			nullStr(baselineHash), f.pathname, f.pathname, deltaBytes); err != nil {
			return fmt.Errorf("stash.Save: insert stashfile %s: %w", f.pathname, err)
		}

		// Revert working file.
		if isAdded {
			// Remove added file.
			os.Remove(fullPath)
			// Remove from vfile.
			if _, err := tx.Exec("DELETE FROM vfile WHERE pathname=?", f.pathname); err != nil {
				return fmt.Errorf("stash.Save: delete vfile %s: %w", f.pathname, err)
			}
		} else if isRemoved {
			// Restore deleted file from baseline.
			baseline, err := content.Expand(repoDB, libfossil.FslID(f.rid))
			if err != nil {
				return fmt.Errorf("stash.Save: expand rid=%d for revert: %w", f.rid, err)
			}
			if err := os.MkdirAll(filepath.Dir(fullPath), 0o755); err != nil {
				return fmt.Errorf("stash.Save: mkdir for %s: %w", f.pathname, err)
			}
			if err := os.WriteFile(fullPath, baseline, 0o644); err != nil {
				return fmt.Errorf("stash.Save: write %s: %w", f.pathname, err)
			}
			if _, err := tx.Exec("UPDATE vfile SET deleted=0, chnged=0 WHERE pathname=?", f.pathname); err != nil {
				return fmt.Errorf("stash.Save: update vfile %s: %w", f.pathname, err)
			}
		} else {
			// Restore modified file from baseline.
			baseline, err := content.Expand(repoDB, libfossil.FslID(f.rid))
			if err != nil {
				return fmt.Errorf("stash.Save: expand rid=%d for revert: %w", f.rid, err)
			}
			if err := os.WriteFile(fullPath, baseline, 0o644); err != nil {
				return fmt.Errorf("stash.Save: write %s: %w", f.pathname, err)
			}
			if _, err := tx.Exec("UPDATE vfile SET chnged=0 WHERE pathname=?", f.pathname); err != nil {
				return fmt.Errorf("stash.Save: update vfile %s: %w", f.pathname, err)
			}
		}
	}

	return tx.Commit()
}

// Apply restores stashed files to the working directory without removing the stash entry.
func Apply(ckout *sql.DB, repoDB *sql.DB, dir string, stashID int64) error {
	rows, err := ckout.Query(`SELECT isAdded, isRemoved, hash, newname, delta
		FROM stashfile WHERE stashid=?`, stashID)
	if err != nil {
		return fmt.Errorf("stash.Apply: query stashfile: %w", err)
	}
	defer rows.Close()

	found := false
	for rows.Next() {
		found = true
		var isAdded, isRemoved bool
		var hashStr sql.NullString
		var newname string
		var deltaBytes []byte

		if err := rows.Scan(&isAdded, &isRemoved, &hashStr, &newname, &deltaBytes); err != nil {
			return fmt.Errorf("stash.Apply: scan stashfile: %w", err)
		}

		fullPath := filepath.Join(dir, newname)

		if isAdded {
			// Write raw content.
			if err := os.MkdirAll(filepath.Dir(fullPath), 0o755); err != nil {
				return fmt.Errorf("stash.Apply: mkdir for %s: %w", newname, err)
			}
			if err := os.WriteFile(fullPath, deltaBytes, 0o644); err != nil {
				return fmt.Errorf("stash.Apply: write %s: %w", newname, err)
			}
		} else if isRemoved {
			// Delete the file.
			if err := os.Remove(fullPath); err != nil && !os.IsNotExist(err) {
				return fmt.Errorf("stash.Apply: remove %s: %w", newname, err)
			}
		} else {
			// Modified: apply delta against baseline.
			if !hashStr.Valid {
				return fmt.Errorf("stash.Apply: missing baseline hash for %s", newname)
			}
			rid, ok := blob.Exists(repoDB, hashStr.String)
			if !ok {
				return fmt.Errorf("stash.Apply: baseline blob %s not found", hashStr.String)
			}
			baseline, err := content.Expand(repoDB, rid)
			if err != nil {
				return fmt.Errorf("stash.Apply: expand baseline %s: %w", hashStr.String, err)
			}
			result, err := delta.Apply(baseline, deltaBytes)
			if err != nil {
				return fmt.Errorf("stash.Apply: apply delta for %s: %w", newname, err)
			}
			if err := os.WriteFile(fullPath, result, 0o644); err != nil {
				return fmt.Errorf("stash.Apply: write %s: %w", newname, err)
			}
		}
	}
	if err := rows.Err(); err != nil {
		return fmt.Errorf("stash.Apply: rows iteration: %w", err)
	}
	if !found {
		return fmt.Errorf("stash.Apply: stash %d not found", stashID)
	}
	return nil
}

// Pop applies the most recent stash entry and removes it.
func Pop(ckout *sql.DB, repoDB *sql.DB, dir string) error {
	var stashID int64
	err := ckout.QueryRow("SELECT stashid FROM stash ORDER BY stashid DESC LIMIT 1").Scan(&stashID)
	if err != nil {
		if err == sql.ErrNoRows {
			return fmt.Errorf("stash.Pop: no stash entries")
		}
		return fmt.Errorf("stash.Pop: query top stash: %w", err)
	}

	if err := Apply(ckout, repoDB, dir, stashID); err != nil {
		return err
	}
	return Drop(ckout, stashID)
}

// List returns all stash entries ordered by ID descending (most recent first).
func List(ckout *sql.DB) ([]Entry, error) {
	if err := EnsureTables(ckout); err != nil {
		return nil, err
	}

	rows, err := ckout.Query("SELECT stashid, hash, comment, ctime FROM stash ORDER BY stashid DESC")
	if err != nil {
		return nil, fmt.Errorf("stash.List: %w", err)
	}
	defer rows.Close()

	var entries []Entry
	for rows.Next() {
		var e Entry
		var h, c sql.NullString
		var ct sql.NullString
		if err := rows.Scan(&e.ID, &h, &c, &ct); err != nil {
			return nil, fmt.Errorf("stash.List: scan: %w", err)
		}
		e.Hash = h.String
		e.Comment = c.String
		e.CTime = ct.String
		entries = append(entries, e)
	}
	return entries, rows.Err()
}

// Drop removes a specific stash entry and its files.
func Drop(ckout *sql.DB, stashID int64) error {
	tx, err := ckout.Begin()
	if err != nil {
		return fmt.Errorf("stash.Drop: begin tx: %w", err)
	}
	defer tx.Rollback()

	if _, err := tx.Exec("DELETE FROM stashfile WHERE stashid=?", stashID); err != nil {
		return fmt.Errorf("stash.Drop: delete stashfile: %w", err)
	}
	res, err := tx.Exec("DELETE FROM stash WHERE stashid=?", stashID)
	if err != nil {
		return fmt.Errorf("stash.Drop: delete stash: %w", err)
	}
	n, _ := res.RowsAffected()
	if n == 0 {
		return fmt.Errorf("stash.Drop: stash %d not found", stashID)
	}
	return tx.Commit()
}

// Clear removes all stash entries.
func Clear(ckout *sql.DB) error {
	tx, err := ckout.Begin()
	if err != nil {
		return fmt.Errorf("stash.Clear: begin tx: %w", err)
	}
	defer tx.Rollback()

	if _, err := tx.Exec("DELETE FROM stashfile"); err != nil {
		return fmt.Errorf("stash.Clear: delete stashfile: %w", err)
	}
	if _, err := tx.Exec("DELETE FROM stash"); err != nil {
		return fmt.Errorf("stash.Clear: delete stash: %w", err)
	}
	return tx.Commit()
}

// nullStr returns a sql.NullString: valid if s is non-empty.
func nullStr(s string) sql.NullString {
	if s == "" {
		return sql.NullString{}
	}
	return sql.NullString{String: s, Valid: true}
}
