package verify

import (
	"fmt"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/deck"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

// rebuildManifests walks all non-phantom blobs, parses checkin manifests,
// and inserts event/plink/mlink/filename rows.
func rebuildManifests(r *repo.Repo, tx *db.Tx, report *Report) error {
	if r == nil {
		panic("rebuildManifests: nil *repo.Repo")
	}
	if tx == nil {
		panic("rebuildManifests: nil *db.Tx")
	}
	if report == nil {
		panic("rebuildManifests: nil *Report")
	}

	entries, err := collectBlobEntries(tx)
	if err != nil {
		return err
	}

	for _, e := range entries {
		data, err := content.Expand(tx, e.rid)
		if err != nil {
			report.BlobsSkipped++
			continue // not expandable — corrupt, raw data blob, or phantom
		}
		d, err := deck.Parse(data)
		if err != nil {
			continue // not a manifest — normal for file blobs
		}
		if d.Type != deck.Checkin {
			continue
		}
		if err := rebuildCheckin(r, tx, e.rid, d, report); err != nil {
			return fmt.Errorf("rebuildManifests rid=%d: %w", e.rid, err)
		}
	}
	return nil
}

// blobEntry holds a blob's rid and uuid for rebuild iteration.
type blobEntry struct {
	rid  libfossil.FslID
	uuid string
}

// collectBlobEntries reads all non-phantom blob rid/uuid pairs.
func collectBlobEntries(q db.Querier) ([]blobEntry, error) {
	rows, err := q.Query("SELECT rid, uuid FROM blob WHERE size >= 0")
	if err != nil {
		return nil, fmt.Errorf("collectBlobEntries: %w", err)
	}
	defer rows.Close()

	var entries []blobEntry
	for rows.Next() {
		var e blobEntry
		if err := rows.Scan(&e.rid, &e.uuid); err != nil {
			return nil, fmt.Errorf("collectBlobEntries scan: %w", err)
		}
		entries = append(entries, e)
	}
	return entries, rows.Err()
}

// rebuildCheckin inserts event, plink, and mlink rows for one checkin manifest.
func rebuildCheckin(r *repo.Repo, tx *db.Tx, rid libfossil.FslID, d *deck.Deck, report *Report) error {
	if tx == nil {
		panic("rebuildCheckin: nil *db.Tx")
	}
	if d == nil {
		panic("rebuildCheckin: nil *deck.Deck")
	}

	mtime := libfossil.TimeToJulian(d.D)

	// Insert event row
	if _, err := tx.Exec(
		"INSERT OR IGNORE INTO event(type, mtime, objid, user, comment) VALUES('ci', ?, ?, ?, ?)",
		mtime, rid, d.U, d.C,
	); err != nil {
		return fmt.Errorf("event: %w", err)
	}

	// Insert plink rows for parent(s)
	if err := rebuildPlinks(tx, rid, d, mtime, report); err != nil {
		return err
	}

	// Insert mlink/filename rows for file cards.
	// Uses manifest.ListFiles for delta manifests (B-card) to get the
	// full file set, not just the abbreviated delta F-cards.
	if err := rebuildMlinks(r, tx, rid, d); err != nil {
		return err
	}

	return nil
}

// rebuildPlinks inserts plink rows for each parent in the manifest.
func rebuildPlinks(tx *db.Tx, rid libfossil.FslID, d *deck.Deck, mtime float64, report *Report) error {
	for i, parentUUID := range d.P {
		parentRID, ok := blob.Exists(tx, parentUUID)
		if !ok {
			report.MissingRefs++
			report.addIssue(Issue{
				Kind:    IssueMissingReference,
				RID:     rid,
				UUID:    parentUUID,
				Table:   "plink",
				Message: fmt.Sprintf("rid=%d parent %s not found", rid, parentUUID),
			})
			continue
		}
		isPrim := 0
		if i == 0 {
			isPrim = 1
		}
		if _, err := tx.Exec(
			"INSERT OR IGNORE INTO plink(pid, cid, isprim, mtime) VALUES(?, ?, ?, ?)",
			parentRID, rid, isPrim, mtime,
		); err != nil {
			return fmt.Errorf("plink: %w", err)
		}
	}
	return nil
}

// rebuildMlinks inserts mlink and filename rows for each file in the manifest.
// For delta manifests (d.B != ""), uses manifest.ListFiles to expand the full
// file set by merging baseline F-cards with delta F-cards. Without this,
// only changed files would get mlink rows — inherited files would be lost.
func rebuildMlinks(r *repo.Repo, tx *db.Tx, rid libfossil.FslID, d *deck.Deck) error {
	type fileRef struct {
		name string
		uuid string
	}

	var files []fileRef
	if d.B != "" {
		// Delta manifest — expand to full file set via manifest.ListFiles.
		entries, err := manifest.ListFiles(r, rid)
		if err != nil {
			return fmt.Errorf("expand delta manifest: %w", err)
		}
		for _, e := range entries {
			files = append(files, fileRef{name: e.Name, uuid: e.UUID})
		}
	} else {
		// Full manifest — use F-cards directly.
		for _, f := range d.F {
			if f.UUID == "" {
				continue
			}
			files = append(files, fileRef{name: f.Name, uuid: f.UUID})
		}
	}

	for _, f := range files {
		fnid, err := rebuildEnsureFilename(tx, f.name)
		if err != nil {
			return fmt.Errorf("filename %q: %w", f.name, err)
		}
		fileRID, ok := blob.Exists(tx, f.uuid)
		if !ok {
			continue // file blob missing — phantom or not yet received
		}
		if _, err := tx.Exec(
			"INSERT OR IGNORE INTO mlink(mid, fid, fnid) VALUES(?, ?, ?)",
			rid, fileRID, fnid,
		); err != nil {
			return fmt.Errorf("mlink: %w", err)
		}
	}
	return nil
}

// rebuildEnsureFilename ensures a filename row exists and returns its fnid.
func rebuildEnsureFilename(tx *db.Tx, name string) (int64, error) {
	if tx == nil {
		panic("rebuildEnsureFilename: nil *db.Tx")
	}
	if name == "" {
		panic("rebuildEnsureFilename: empty name")
	}

	var fnid int64
	err := tx.QueryRow("SELECT fnid FROM filename WHERE name=?", name).Scan(&fnid)
	if err == nil {
		return fnid, nil
	}
	result, err := tx.Exec("INSERT INTO filename(name) VALUES(?)", name)
	if err != nil {
		return 0, fmt.Errorf("insert filename %q: %w", name, err)
	}
	return result.LastInsertId()
}
