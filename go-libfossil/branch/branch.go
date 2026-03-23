package branch

import (
	"fmt"
	"time"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/deck"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/tag"
)

type CreateOpts struct {
	Name   string
	Parent libfossil.FslID
	User   string
	Time   time.Time
	Color  string
}

func Create(r *repo.Repo, opts CreateOpts) (libfossil.FslID, string, error) {
	if r == nil {
		panic("branch.Create: r must not be nil")
	}
	if opts.Name == "" {
		panic("branch.Create: opts.Name must not be empty")
	}
	if opts.Parent <= 0 {
		panic("branch.Create: opts.Parent must be positive")
	}
	if opts.User == "" {
		panic("branch.Create: opts.User must not be empty")
	}
	if opts.Time.IsZero() {
		opts.Time = time.Now().UTC()
	}

	// 1. Get parent's file list.
	entries, err := manifest.ListFiles(r, opts.Parent)
	if err != nil {
		return 0, "", fmt.Errorf("branch.Create list files: %w", err)
	}
	var files []manifest.File
	for _, e := range entries {
		frid, ok := blob.Exists(r.DB(), e.UUID)
		if !ok {
			return 0, "", fmt.Errorf("branch.Create: file blob %s not found", e.UUID)
		}
		data, err := content.Expand(r.DB(), frid)
		if err != nil {
			return 0, "", fmt.Errorf("branch.Create: expand %s: %w", e.Name, err)
		}
		files = append(files, manifest.File{Name: e.Name, Content: data, Perm: e.Perm})
	}

	// 2. Query existing sym-* tags on parent for cancel cards.
	rows, err := r.DB().Query(
		"SELECT tag.tagname FROM tagxref JOIN tag USING(tagid) WHERE tagxref.rid=? AND tagxref.tagtype>0 AND tag.tagname GLOB 'sym-*'",
		opts.Parent,
	)
	if err != nil {
		return 0, "", fmt.Errorf("branch.Create query sym tags: %w", err)
	}
	defer rows.Close()
	var oldSyms []string
	for rows.Next() {
		var name string
		if err := rows.Scan(&name); err != nil {
			return 0, "", err
		}
		oldSyms = append(oldSyms, name)
	}
	if err := rows.Err(); err != nil {
		return 0, "", err
	}

	// 3. Build T-cards.
	var tags []deck.TagCard
	tags = append(tags, deck.TagCard{Type: deck.TagPropagating, Name: "branch", UUID: "*", Value: opts.Name})
	tags = append(tags, deck.TagCard{Type: deck.TagPropagating, Name: "sym-" + opts.Name, UUID: "*"})
	for _, old := range oldSyms {
		tags = append(tags, deck.TagCard{Type: deck.TagCancel, Name: old, UUID: "*"})
	}
	if opts.Color != "" {
		tags = append(tags, deck.TagCard{Type: deck.TagPropagating, Name: "bgcolor", UUID: "*", Value: opts.Color})
	}

	// 4. Create checkin with branch tags.
	rid, uuid, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   files,
		Comment: fmt.Sprintf("Create branch %s", opts.Name),
		User:    opts.User,
		Parent:  opts.Parent,
		Time:    opts.Time,
		Tags:    tags,
	})
	if err != nil {
		return 0, "", fmt.Errorf("branch.Create checkin: %w", err)
	}

	// 5. Process inline T-cards to apply tags.
	// Checkin already inserted event/plink/mlink, so we just need to process tags.
	if err := processInlineTags(r, rid, tags, opts.Time); err != nil {
		return 0, "", fmt.Errorf("branch.Create process tags: %w", err)
	}

	return rid, uuid, nil
}

func processInlineTags(r *repo.Repo, rid libfossil.FslID, tags []deck.TagCard, mtime time.Time) error {
	julianMtime := libfossil.TimeToJulian(mtime)
	for _, tc := range tags {
		if tc.UUID != "*" {
			continue // only process inline tags (UUID="*" means this checkin)
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
			MTime:     julianMtime,
		}); err != nil {
			return fmt.Errorf("apply tag %q: %w", tc.Name, err)
		}
	}
	return nil
}
