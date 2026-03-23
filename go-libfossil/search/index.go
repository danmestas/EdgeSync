package search

import (
	"bytes"
	"fmt"
	"strconv"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
)

// maxBinaryProbe is the number of bytes checked for null bytes to detect binary files.
const maxBinaryProbe = 8192

// isBinary returns true if data contains a null byte in the first maxBinaryProbe bytes.
func isBinary(data []byte) bool {
	probe := data
	if len(probe) > maxBinaryProbe {
		probe = probe[:maxBinaryProbe]
	}
	return bytes.ContainsRune(probe, 0)
}

// RebuildIndex walks the trunk tip manifest, expands blob content,
// skips binaries and phantoms, and populates fts_content.
// No-ops if already current.
//
// Panics if idx is nil (TigerStyle precondition).
func (idx *Index) RebuildIndex() error {
	if idx == nil {
		panic("search.RebuildIndex: nil *Index")
	}

	db := idx.repo.DB()

	tip, err := trunkTip(db)
	if err != nil {
		return fmt.Errorf("search.RebuildIndex: %w", err)
	}
	if tip == 0 {
		return nil // empty repo, nothing to index
	}

	current, err := indexedRID(db)
	if err != nil {
		return fmt.Errorf("search.RebuildIndex: %w", err)
	}
	if tip == current {
		return nil // already up to date
	}

	// Full replace: delete all, re-insert
	if _, err := db.Exec("DELETE FROM fts_content"); err != nil {
		return fmt.Errorf("search.RebuildIndex: clear: %w", err)
	}

	files, err := manifest.ListFiles(idx.repo, tip)
	if err != nil {
		return fmt.Errorf("search.RebuildIndex: list files: %w", err)
	}

	for _, f := range files {
		rid, ok := blob.Exists(db, f.UUID)
		if !ok {
			continue // phantom — blob not yet received
		}

		data, err := content.Expand(db, rid)
		if err != nil {
			// Phantom or corrupt — skip
			continue
		}

		if isBinary(data) {
			continue
		}

		if _, err := db.Exec(
			"INSERT INTO fts_content(path, content) VALUES(?, ?)",
			f.Name, string(data),
		); err != nil {
			return fmt.Errorf("search.RebuildIndex: insert %s: %w", f.Name, err)
		}
	}

	// Update indexed_rid
	if _, err := db.Exec(
		"INSERT OR REPLACE INTO fts_meta(key, value) VALUES('indexed_rid', ?)",
		strconv.FormatInt(int64(tip), 10),
	); err != nil {
		return fmt.Errorf("search.RebuildIndex: update meta: %w", err)
	}

	return nil
}
