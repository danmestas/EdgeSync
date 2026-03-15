package manifest

import (
	"fmt"
	"time"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/deck"
	"github.com/dmestas/edgesync/go-libfossil/hash"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

type CheckinOpts struct {
	Files   []File
	Comment string
	User    string
	Parent  libfossil.FslID
	Delta   bool
	Time    time.Time
}

type File struct {
	Name    string
	Content []byte
	Perm    string
}

func Checkin(r *repo.Repo, opts CheckinOpts) (libfossil.FslID, string, error) {
	if opts.Time.IsZero() {
		opts.Time = time.Now().UTC()
	}

	var manifestRid libfossil.FslID
	var manifestUUID string

	err := r.WithTx(func(tx *db.Tx) error {
		// Store file blobs, build F-cards
		fCards := make([]deck.FileCard, len(opts.Files))
		for i, f := range opts.Files {
			_, uuid, err := blob.Store(tx, f.Content)
			if err != nil {
				return fmt.Errorf("storing file %q: %w", f.Name, err)
			}
			fCards[i] = deck.FileCard{Name: f.Name, UUID: uuid, Perm: f.Perm}
		}

		// Build deck
		d := &deck.Deck{
			Type: deck.Checkin,
			C:    opts.Comment,
			D:    opts.Time,
			F:    fCards,
			U:    opts.User,
		}

		// Parent
		if opts.Parent > 0 {
			var parentUUID string
			if err := tx.QueryRow("SELECT uuid FROM blob WHERE rid=?", opts.Parent).Scan(&parentUUID); err != nil {
				return fmt.Errorf("parent uuid: %w", err)
			}
			d.P = []string{parentUUID}
		}

		// Tags for initial checkin
		if opts.Parent == 0 {
			d.T = []deck.TagCard{
				{Type: deck.TagPropagating, Name: "branch", UUID: "*", Value: "trunk"},
				{Type: deck.TagSingleton, Name: "sym-trunk", UUID: "*"},
			}
		}

		// Delta manifest support
		if opts.Delta && opts.Parent > 0 {
			if err := applyDelta(tx, d, fCards, opts.Parent); err != nil {
				return err
			}
		}

		// R-card (always over full file set)
		rDeck := &deck.Deck{F: fCards}
		getContent := func(uuid string) ([]byte, error) {
			rid, ok := blob.Exists(tx, uuid)
			if !ok {
				return nil, fmt.Errorf("blob not found: %s", uuid)
			}
			return content.Expand(tx, rid)
		}
		rHash, err := rDeck.ComputeR(getContent)
		if err != nil {
			return fmt.Errorf("R-card: %w", err)
		}
		d.R = rHash

		// Marshal and store manifest
		manifestBytes, err := d.Marshal()
		if err != nil {
			return fmt.Errorf("marshal: %w", err)
		}
		manifestRid, manifestUUID, err = blob.Store(tx, manifestBytes)
		if err != nil {
			return fmt.Errorf("store manifest: %w", err)
		}

		// filename + mlink
		for _, f := range opts.Files {
			fnid, err := ensureFilename(tx, f.Name)
			if err != nil {
				return fmt.Errorf("filename %q: %w", f.Name, err)
			}
			fileUUID := hash.SHA1(f.Content)
			fileRid, _ := blob.Exists(tx, fileUUID)
			var pmid, pid int64
			if opts.Parent > 0 {
				pmid = int64(opts.Parent)
				tx.QueryRow("SELECT fid FROM mlink WHERE mid=? AND fnid=?", opts.Parent, fnid).Scan(&pid)
			}
			if _, err := tx.Exec(
				"INSERT INTO mlink(mid, fid, pmid, pid, fnid) VALUES(?, ?, ?, ?, ?)",
				manifestRid, fileRid, pmid, pid, fnid,
			); err != nil {
				return fmt.Errorf("mlink: %w", err)
			}
		}

		// plink
		if opts.Parent > 0 {
			if _, err := tx.Exec(
				"INSERT INTO plink(pid, cid, isprim, mtime) VALUES(?, ?, 1, ?)",
				opts.Parent, manifestRid, libfossil.TimeToJulian(opts.Time),
			); err != nil {
				return fmt.Errorf("plink: %w", err)
			}
		}

		// event
		if _, err := tx.Exec(
			"INSERT INTO event(type, mtime, objid, user, comment) VALUES('ci', ?, ?, ?, ?)",
			libfossil.TimeToJulian(opts.Time), manifestRid, opts.User, opts.Comment,
		); err != nil {
			return fmt.Errorf("event: %w", err)
		}

		// leaf
		tx.Exec("INSERT OR IGNORE INTO leaf(rid) VALUES(?)", manifestRid)
		if opts.Parent > 0 {
			tx.Exec("DELETE FROM leaf WHERE rid=?", opts.Parent)
		}

		// unclustered + unsent
		tx.Exec("INSERT OR IGNORE INTO unclustered(rid) VALUES(?)", manifestRid)
		tx.Exec("INSERT OR IGNORE INTO unsent(rid) VALUES(?)", manifestRid)

		return nil
	})
	if err != nil {
		return 0, "", fmt.Errorf("manifest.Checkin: %w", err)
	}
	return manifestRid, manifestUUID, nil
}

func ensureFilename(tx *db.Tx, name string) (int64, error) {
	var fnid int64
	err := tx.QueryRow("SELECT fnid FROM filename WHERE name=?", name).Scan(&fnid)
	if err == nil {
		return fnid, nil
	}
	result, err := tx.Exec("INSERT INTO filename(name) VALUES(?)", name)
	if err != nil {
		return 0, err
	}
	return result.LastInsertId()
}

func GetManifest(r *repo.Repo, rid libfossil.FslID) (*deck.Deck, error) {
	data, err := content.Expand(r.DB(), rid)
	if err != nil {
		return nil, fmt.Errorf("manifest.GetManifest: %w", err)
	}
	return deck.Parse(data)
}

func applyDelta(tx *db.Tx, d *deck.Deck, fullFCards []deck.FileCard, parentRid libfossil.FslID) error {
	parentData, err := content.Expand(tx, parentRid)
	if err != nil {
		return fmt.Errorf("expand parent: %w", err)
	}
	parentDeck, err := deck.Parse(parentData)
	if err != nil {
		return fmt.Errorf("parse parent: %w", err)
	}

	baselineUUID := parentDeck.B
	if baselineUUID == "" {
		var puuid string
		tx.QueryRow("SELECT uuid FROM blob WHERE rid=?", parentRid).Scan(&puuid)
		baselineUUID = puuid
	}

	baseRid, ok := blob.Exists(tx, baselineUUID)
	if !ok {
		return fmt.Errorf("baseline %s not found", baselineUUID)
	}
	baseData, err := content.Expand(tx, baseRid)
	if err != nil {
		return fmt.Errorf("expand baseline: %w", err)
	}
	baseDeck, err := deck.Parse(baseData)
	if err != nil {
		return fmt.Errorf("parse baseline: %w", err)
	}

	baseFiles := make(map[string]string)
	for _, f := range baseDeck.F {
		baseFiles[f.Name] = f.UUID
	}

	var deltaFCards []deck.FileCard
	currentFiles := make(map[string]bool)
	for _, f := range fullFCards {
		currentFiles[f.Name] = true
		if baseUUID, exists := baseFiles[f.Name]; !exists || baseUUID != f.UUID {
			deltaFCards = append(deltaFCards, f)
		}
	}
	for name := range baseFiles {
		if !currentFiles[name] {
			deltaFCards = append(deltaFCards, deck.FileCard{Name: name})
		}
	}

	if len(deltaFCards) < len(fullFCards) {
		d.B = baselineUUID
		d.F = deltaFCards
	}
	return nil
}
