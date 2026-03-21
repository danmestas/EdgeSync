package manifest

import (
	"fmt"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/deck"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

// Crosslink scans all blobs not yet in the event table, tries to parse
// each as a checkin manifest, and populates event/plink/leaf tables.
// This is the Go equivalent of Fossil's manifest_crosslink.
func Crosslink(r *repo.Repo) (int, error) {
	if r == nil {
		panic("manifest.Crosslink: r must not be nil")
	}

	// Find blobs not yet crosslinked: blobs with no event entry.
	rows, err := r.DB().Query(`
		SELECT b.rid, b.uuid FROM blob b
		WHERE b.size >= 0
		AND NOT EXISTS (SELECT 1 FROM event e WHERE e.objid = b.rid)
	`)
	if err != nil {
		return 0, fmt.Errorf("manifest.Crosslink query: %w", err)
	}
	defer rows.Close()

	type candidate struct {
		rid  libfossil.FslID
		uuid string
	}
	var candidates []candidate
	for rows.Next() {
		var c candidate
		if err := rows.Scan(&c.rid, &c.uuid); err != nil {
			return 0, fmt.Errorf("manifest.Crosslink scan: %w", err)
		}
		candidates = append(candidates, c)
	}
	if err := rows.Err(); err != nil {
		return 0, fmt.Errorf("manifest.Crosslink rows: %w", err)
	}

	linked := 0
	for _, c := range candidates {
		data, err := content.Expand(r.DB(), c.rid)
		if err != nil {
			continue // not expandable, skip
		}

		d, err := deck.Parse(data)
		if err != nil {
			continue // not a valid manifest, skip
		}

		if d.Type != deck.Checkin {
			continue // only crosslink checkin manifests
		}

		if err := crosslinkOne(r, c.rid, d); err != nil {
			return linked, fmt.Errorf("manifest.Crosslink rid=%d: %w", c.rid, err)
		}
		linked++
	}

	return linked, nil
}

func crosslinkOne(r *repo.Repo, rid libfossil.FslID, d *deck.Deck) error {
	return r.WithTx(func(tx *db.Tx) error {
		// event
		if _, err := tx.Exec(
			"INSERT OR IGNORE INTO event(type, mtime, objid, user, comment) VALUES('ci', ?, ?, ?, ?)",
			libfossil.TimeToJulian(d.D), rid, d.U, d.C,
		); err != nil {
			return fmt.Errorf("event: %w", err)
		}

		// plink — link to parent(s)
		for i, parentUUID := range d.P {
			var parentRid int64
			if err := tx.QueryRow("SELECT rid FROM blob WHERE uuid=?", parentUUID).Scan(&parentRid); err != nil {
				continue // parent blob missing, skip
			}
			isPrim := 0
			if i == 0 {
				isPrim = 1
			}
			if _, err := tx.Exec(
				"INSERT OR IGNORE INTO plink(pid, cid, isprim, mtime) VALUES(?, ?, ?, ?)",
				parentRid, rid, isPrim, libfossil.TimeToJulian(d.D),
			); err != nil {
				return fmt.Errorf("plink: %w", err)
			}
		}

		// leaf — this is a leaf if no child points to it
		if _, err := tx.Exec("INSERT OR IGNORE INTO leaf(rid) VALUES(?)", rid); err != nil {
			return fmt.Errorf("leaf insert: %w", err)
		}
		// Remove parent from leaf table (it now has a child)
		for _, parentUUID := range d.P {
			var parentRid int64
			if err := tx.QueryRow("SELECT rid FROM blob WHERE uuid=?", parentUUID).Scan(&parentRid); err != nil {
				continue
			}
			tx.Exec("DELETE FROM leaf WHERE rid=?", parentRid)
		}

		// mlink — file mappings
		for _, f := range d.F {
			if f.UUID == "" {
				continue // deleted file in delta manifest
			}
			fnid, err := ensureFilename(tx, f.Name)
			if err != nil {
				return fmt.Errorf("filename %q: %w", f.Name, err)
			}
			var fileRid int64
			if err := tx.QueryRow("SELECT rid FROM blob WHERE uuid=?", f.UUID).Scan(&fileRid); err != nil {
				continue // file blob missing
			}
			if _, err := tx.Exec(
				"INSERT OR IGNORE INTO mlink(mid, fid, fnid) VALUES(?, ?, ?)",
				rid, fileRid, fnid,
			); err != nil {
				return fmt.Errorf("mlink: %w", err)
			}
		}

		return nil
	})
}
