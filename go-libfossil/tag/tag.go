package tag

import (
	"fmt"
	"time"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/deck"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

const (
	TagCancel      = 0
	TagSingleton   = 1
	TagPropagating = 2
)

// TagOpts describes a tag operation on a target artifact.
type TagOpts struct {
	TargetRID libfossil.FslID
	TagName   string
	TagType   int // TagCancel, TagSingleton, or TagPropagating
	Value     string
	User      string
	Time      time.Time
}

// AddTag creates a control artifact that adds or cancels a tag on a target checkin.
// It stores the artifact as a blob, ensures the tag name exists in the tag table,
// and inserts/replaces a row in the tagxref table.
func AddTag(r *repo.Repo, opts TagOpts) (libfossil.FslID, error) {
	if r == nil {
		panic("tag.AddTag: r must not be nil")
	}
	if opts.TagName == "" {
		panic("tag.AddTag: opts.TagName must not be empty")
	}
	if opts.Time.IsZero() {
		opts.Time = time.Now().UTC()
	}

	var controlRid libfossil.FslID

	err := r.WithTx(func(tx *db.Tx) error {
		// Look up target UUID
		var targetUUID string
		if err := tx.QueryRow("SELECT uuid FROM blob WHERE rid=?", opts.TargetRID).Scan(&targetUUID); err != nil {
			return fmt.Errorf("target uuid lookup: %w", err)
		}

		// Map our integer tag type to deck.TagType byte
		var deckTagType deck.TagType
		switch opts.TagType {
		case TagCancel:
			deckTagType = deck.TagCancel
		case TagSingleton:
			deckTagType = deck.TagSingleton
		case TagPropagating:
			deckTagType = deck.TagPropagating
		default:
			return fmt.Errorf("invalid tag type: %d", opts.TagType)
		}

		// Build control artifact deck
		d := &deck.Deck{
			Type: deck.Control,
			D:    opts.Time,
			T: []deck.TagCard{
				{
					Type:  deckTagType,
					Name:  opts.TagName,
					UUID:  targetUUID,
					Value: opts.Value,
				},
			},
			U: opts.User,
		}

		// Marshal and store as blob
		manifestBytes, err := d.Marshal()
		if err != nil {
			return fmt.Errorf("marshal control artifact: %w", err)
		}
		rid, _, err := blob.Store(tx, manifestBytes)
		if err != nil {
			return fmt.Errorf("store control artifact: %w", err)
		}
		controlRid = rid

		// Ensure tag name exists in tag table
		tagid, err := ensureTag(tx, opts.TagName)
		if err != nil {
			return fmt.Errorf("ensure tag %q: %w", opts.TagName, err)
		}

		// Insert or replace tagxref row
		mtime := libfossil.TimeToJulian(opts.Time)
		if _, err := tx.Exec(
			`INSERT OR REPLACE INTO tagxref(tagid, tagtype, srcid, origid, value, mtime, rid)
			 VALUES(?, ?, ?, ?, ?, ?, ?)`,
			tagid, opts.TagType, controlRid, opts.TargetRID, opts.Value, mtime, opts.TargetRID,
		); err != nil {
			return fmt.Errorf("tagxref insert: %w", err)
		}

		// Mark control artifact as unsent so sync pushes it (unclustered is handled by blob.Store).
		if _, err := tx.Exec("INSERT OR IGNORE INTO unsent(rid) VALUES(?)", controlRid); err != nil {
			return fmt.Errorf("tag.AddTag: unsent: %w", err)
		}

		return nil
	})
	if err != nil {
		return 0, fmt.Errorf("tag.AddTag: %w", err)
	}
	return controlRid, nil
}

// ensureTag returns the tagid for the given tag name, creating it if it doesn't exist.
func ensureTag(tx *db.Tx, name string) (int64, error) {
	if tx == nil {
		panic("tag.ensureTag: tx must not be nil")
	}
	if name == "" {
		panic("tag.ensureTag: name must not be empty")
	}
	var tagid int64
	err := tx.QueryRow("SELECT tagid FROM tag WHERE tagname=?", name).Scan(&tagid)
	if err == nil {
		return tagid, nil
	}
	result, err := tx.Exec("INSERT INTO tag(tagname) VALUES(?)", name)
	if err != nil {
		return 0, err
	}
	return result.LastInsertId()
}
