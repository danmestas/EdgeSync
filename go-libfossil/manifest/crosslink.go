package manifest

import (
	"fmt"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/deck"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/tag"
)

type pendingItem struct {
	Type byte   // 'w' = wiki backlink, 't' = ticket rebuild
	ID   string
}

// Crosslink scans all blobs not yet crosslinked in event/tagxref/forumpost/attachment tables,
// parses them as manifests, and populates cross-reference tables (event/plink/leaf/mlink/tagxref).
// This is the Go equivalent of Fossil's manifest_crosslink.
func Crosslink(r *repo.Repo) (int, error) {
	if r == nil {
		panic("manifest.Crosslink: r must not be nil")
	}

	// Pass 1: Discover and crosslink all uncrosslinked artifacts.
	rows, err := r.DB().Query(`
		SELECT b.rid, b.uuid FROM blob b
		WHERE b.size >= 0
		  AND NOT EXISTS (SELECT 1 FROM event e WHERE e.objid = b.rid)
		  AND NOT EXISTS (SELECT 1 FROM tagxref tx WHERE tx.srcid = b.rid)
		  AND NOT EXISTS (SELECT 1 FROM forumpost fp WHERE fp.fpid = b.rid)
		  AND NOT EXISTS (SELECT 1 FROM attachment a WHERE a.attachid = b.rid)
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
	var pending []pendingItem
	for _, c := range candidates {
		data, err := content.Expand(r.DB(), c.rid)
		if err != nil {
			continue // not expandable, skip
		}

		d, err := deck.Parse(data)
		if err != nil {
			continue // not a valid manifest, skip
		}

		var linkErr error
		var p []pendingItem

		switch d.Type {
		case deck.Checkin:
			linkErr = crosslinkCheckin(r, c.rid, d)
		case deck.Wiki:
			p, linkErr = crosslinkWiki(r, c.rid, d)
		case deck.Ticket:
			p, linkErr = crosslinkTicket(r, c.rid, d)
		case deck.Event:
			p, linkErr = crosslinkEvent(r, c.rid, d)
		case deck.Attachment:
			linkErr = crosslinkAttachment(r, c.rid, d)
		case deck.Cluster:
			linkErr = crosslinkCluster(r, c.rid, d)
		case deck.ForumPost:
			linkErr = crosslinkForum(r, c.rid, d)
		case deck.Control:
			linkErr = crosslinkControl(r, c.rid, d)
		default:
			continue
		}

		if linkErr != nil {
			return linked, fmt.Errorf("manifest.Crosslink rid=%d type=%d: %w", c.rid, d.Type, linkErr)
		}
		linked++
		pending = append(pending, p...)
	}

	// Pass 2: Process pending items (wiki backlinks, ticket rebuilds).
	for _, item := range pending {
		_ = item // Stubs return nil, nothing to process yet.
	}

	return linked, nil
}

func crosslinkCheckin(r *repo.Repo, rid libfossil.FslID, d *deck.Deck) error {
	// First, crosslink event/plink/leaf/mlink/cherrypick in a transaction
	err := r.WithTx(func(tx *db.Tx) error {
		// event
		if _, err := tx.Exec(
			"INSERT OR IGNORE INTO event(type, mtime, objid, user, comment) VALUES('ci', ?, ?, ?, ?)",
			libfossil.TimeToJulian(d.D), rid, d.U, d.C,
		); err != nil {
			return fmt.Errorf("event: %w", err)
		}

		// Resolve baseid for plink if B-card present
		var baseid interface{} = nil
		if d.B != "" {
			var baseRid int64
			if err := tx.QueryRow("SELECT rid FROM blob WHERE uuid=?", d.B).Scan(&baseRid); err == nil {
				baseid = baseRid
			}
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
				"INSERT OR IGNORE INTO plink(pid, cid, isprim, mtime, baseid) VALUES(?, ?, ?, ?, ?)",
				parentRid, rid, isPrim, libfossil.TimeToJulian(d.D), baseid,
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
			if _, err := tx.Exec("DELETE FROM leaf WHERE rid=?", parentRid); err != nil {
				return fmt.Errorf("leaf delete parent %d: %w", parentRid, err)
			}
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

		// cherrypick — Q-cards (cherrypick/backout)
		for _, cp := range d.Q {
			target := cp.Target
			isExclude := 0
			if cp.IsBackout {
				isExclude = 1
			}
			var parentRid int64
			if err := tx.QueryRow("SELECT rid FROM blob WHERE uuid=?", target).Scan(&parentRid); err != nil {
				continue // target blob missing, skip
			}
			if _, err := tx.Exec(
				"REPLACE INTO cherrypick(parentid, childid, isExclude) VALUES(?, ?, ?)",
				parentRid, rid, isExclude,
			); err != nil {
				return fmt.Errorf("cherrypick: %w", err)
			}
		}

		return nil
	})
	if err != nil {
		return err
	}

	// Process inline T-cards (UUID="*" means this checkin) after transaction completes.
	// Each tag.ApplyTag call starts its own transaction.
	mtime := libfossil.TimeToJulian(d.D)
	for _, tc := range d.T {
		if tc.UUID != "*" {
			continue
		}
		var tagType int
		switch tc.Type {
		case deck.TagPropagating:
			tagType = tag.TagPropagating
		case deck.TagSingleton:
			tagType = tag.TagSingleton
		case deck.TagCancel:
			tagType = tag.TagCancel
		default:
			continue
		}

		if err := tag.ApplyTag(r, tag.ApplyOpts{
			TargetRID: rid,
			SrcRID:    rid, // inline: checkin is its own source
			TagName:   tc.Name,
			TagType:   tagType,
			Value:     tc.Value,
			MTime:     mtime,
		}); err != nil {
			return fmt.Errorf("inline tag %q: %w", tc.Name, err)
		}
	}

	// PropagateAll from primary parent (if checkin has parents)
	if len(d.P) > 0 {
		var primaryParentRid int64
		if err := r.DB().QueryRow("SELECT rid FROM blob WHERE uuid=?", d.P[0]).Scan(&primaryParentRid); err == nil {
			if err := tag.PropagateAll(r.DB(), libfossil.FslID(primaryParentRid)); err != nil {
				return fmt.Errorf("propagate from parent: %w", err)
			}
		}
	}

	return nil
}

func crosslinkControl(r *repo.Repo, srcRID libfossil.FslID, d *deck.Deck) error {
	mtime := libfossil.TimeToJulian(d.D)
	for _, tc := range d.T {
		if tc.UUID == "*" {
			continue // self-referencing — handled in crosslinkCheckin
		}
		var targetRID int64
		if err := r.DB().QueryRow("SELECT rid FROM blob WHERE uuid=?", tc.UUID).Scan(&targetRID); err != nil {
			continue // target not found
		}
		var tagType int
		switch tc.Type {
		case deck.TagPropagating:
			tagType = tag.TagPropagating
		case deck.TagSingleton:
			tagType = tag.TagSingleton
		case deck.TagCancel:
			tagType = tag.TagCancel
		default:
			continue
		}
		if err := tag.ApplyTag(r, tag.ApplyOpts{
			TargetRID: libfossil.FslID(targetRID),
			SrcRID:    srcRID,
			TagName:   tc.Name,
			TagType:   tagType,
			Value:     tc.Value,
			MTime:     mtime,
		}); err != nil {
			return fmt.Errorf("apply tag %q to rid=%d: %w", tc.Name, targetRID, err)
		}
	}
	return nil
}

// addFWTPlink handles plink insertion and tag propagation for wiki/forum/technote/ticket.
// Shared helper for artifact types that use P-cards (parents) but not the full checkin flow.
func addFWTPlink(r *repo.Repo, rid libfossil.FslID, d *deck.Deck) error {
	if r == nil {
		panic("manifest.addFWTPlink: r must not be nil")
	}
	if rid <= 0 {
		panic("manifest.addFWTPlink: rid must be positive")
	}

	mtime := libfossil.TimeToJulian(d.D)
	var primaryParentRid libfossil.FslID

	for i, parentUUID := range d.P {
		var parentRid int64
		if err := r.DB().QueryRow("SELECT rid FROM blob WHERE uuid=?", parentUUID).Scan(&parentRid); err != nil {
			continue // parent blob missing, skip
		}
		isPrim := 0
		if i == 0 {
			isPrim = 1
			primaryParentRid = libfossil.FslID(parentRid)
		}
		if _, err := r.DB().Exec(
			"INSERT OR IGNORE INTO plink(pid, cid, isprim, mtime) VALUES(?, ?, ?, ?)",
			parentRid, rid, isPrim, mtime,
		); err != nil {
			return fmt.Errorf("addFWTPlink: %w", err)
		}
	}

	// Propagate tags from primary parent
	if primaryParentRid > 0 {
		if err := tag.PropagateAll(r.DB(), primaryParentRid); err != nil {
			return fmt.Errorf("addFWTPlink propagate: %w", err)
		}
	}

	return nil
}

func crosslinkWiki(r *repo.Repo, rid libfossil.FslID, d *deck.Deck) ([]pendingItem, error) {
	if err := addFWTPlink(r, rid, d); err != nil {
		return nil, fmt.Errorf("wiki plink: %w", err)
	}

	title := d.L
	if title == "" {
		return nil, fmt.Errorf("wiki manifest missing title (L-card)")
	}

	// Apply wiki-<title> tag with value = content length
	wikiLen := fmt.Sprintf("%d", len(d.W))
	if err := tag.ApplyTag(r, tag.ApplyOpts{
		TargetRID: rid,
		SrcRID:    rid,
		TagName:   fmt.Sprintf("wiki-%s", title),
		TagType:   tag.TagSingleton,
		Value:     wikiLen,
		MTime:     libfossil.TimeToJulian(d.D),
	}); err != nil {
		return nil, fmt.Errorf("wiki tag: %w", err)
	}

	// Insert event row with prefix: '+' = new, ':' = edit, '-' = delete
	var prefix byte
	if len(d.W) == 0 {
		prefix = '-' // deletion
	} else if len(d.P) == 0 {
		prefix = '+' // new page
	} else {
		prefix = ':' // edit
	}
	comment := fmt.Sprintf("%c%s", prefix, title)

	if _, err := r.DB().Exec(
		"REPLACE INTO event(type, mtime, objid, user, comment) VALUES('w', ?, ?, ?, ?)",
		libfossil.TimeToJulian(d.D), rid, d.U, comment,
	); err != nil {
		return nil, fmt.Errorf("wiki event: %w", err)
	}

	return []pendingItem{{Type: 'w', ID: title}}, nil
}

func crosslinkTicket(r *repo.Repo, rid libfossil.FslID, d *deck.Deck) ([]pendingItem, error) {
	return nil, nil
}

func crosslinkEvent(r *repo.Repo, rid libfossil.FslID, d *deck.Deck) ([]pendingItem, error) {
	return nil, nil
}

func crosslinkAttachment(r *repo.Repo, rid libfossil.FslID, d *deck.Deck) error {
	return nil
}

func crosslinkCluster(r *repo.Repo, rid libfossil.FslID, d *deck.Deck) error {
	return nil
}

func crosslinkForum(r *repo.Repo, rid libfossil.FslID, d *deck.Deck) error {
	return nil
}
