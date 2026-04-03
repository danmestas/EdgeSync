package main

import (
	"fmt"
	"time"

	libfossil "github.com/danmestas/go-libfossil"
	"github.com/danmestas/go-libfossil/deck"
	"github.com/danmestas/go-libfossil/manifest"
	"github.com/danmestas/go-libfossil/tag"
)

type RepoBranchCmd struct {
	Ls    RepoBranchLsCmd    `cmd:"" help:"List branches"`
	New   RepoBranchNewCmd   `cmd:"" help:"Create new branch"`
	Close RepoBranchCloseCmd `cmd:"" help:"Close a branch"`
}

type RepoBranchLsCmd struct {
	Closed bool `help:"Show only closed branches"`
	All    bool `help:"Show all branches (open and closed)"`
}

func (c *RepoBranchLsCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	var filter string
	switch {
	case c.All:
		// no filter
	case c.Closed:
		filter = " AND tx.tagtype = 0"
	default:
		filter = " AND tx.tagtype > 0"
	}

	query := `
		SELECT DISTINCT substr(t.tagname, 5) AS branch, b.uuid,
		       datetime(e.mtime), e.user
		FROM tag t
		JOIN tagxref tx ON tx.tagid = t.tagid
		JOIN blob b ON b.rid = tx.rid
		LEFT JOIN event e ON e.objid = tx.rid
		WHERE t.tagname LIKE 'sym-%'` + filter + `
		ORDER BY e.mtime DESC`

	rows, err := r.DB().Query(query)
	if err != nil {
		return err
	}
	defer rows.Close()

	count := 0
	for rows.Next() {
		var branch, uuid string
		var mtime, user *string
		if err := rows.Scan(&branch, &uuid, &mtime, &user); err != nil {
			return err
		}
		short := uuid
		if len(short) > 10 {
			short = short[:10]
		}
		mt := ""
		if mtime != nil {
			mt = *mtime
		}
		u := ""
		if user != nil {
			u = *user
		}
		fmt.Printf("%-20s %s  %s  %s\n", branch, short, mt, u)
		count++
	}
	if err := rows.Err(); err != nil {
		return err
	}
	if count == 0 {
		fmt.Println("no branches found")
	}
	return nil
}

type RepoBranchNewCmd struct {
	Name    string `arg:"" help:"Branch name"`
	From    string `help:"Parent version (default: tip)"`
	Message string `short:"m" help:"Checkin comment"`
	User    string `help:"Checkin user (default: OS username)"`
}

func (c *RepoBranchNewCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	parentRid, err := resolveRID(r, c.From)
	if err != nil {
		return fmt.Errorf("resolving parent: %w", err)
	}

	// Read all files from the parent manifest.
	entries, err := manifest.ListFiles(r, parentRid)
	if err != nil {
		return fmt.Errorf("listing parent files: %w", err)
	}

	files := make([]manifest.File, len(entries))
	for i, e := range entries {
		data, err := expandFile(r, e.UUID)
		if err != nil {
			return fmt.Errorf("expanding %s: %w", e.Name, err)
		}
		files[i] = manifest.File{
			Name:    e.Name,
			Content: data,
			Perm:    e.Perm,
		}
	}

	user := c.User
	if user == "" {
		user = currentUser()
	}

	comment := c.Message
	if comment == "" {
		comment = "Create branch " + c.Name
	}

	rid, uuid, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   files,
		Comment: comment,
		User:    user,
		Parent:  parentRid,
		Time:    time.Now().UTC(),
		Tags: []deck.TagCard{
			{Type: deck.TagPropagating, Name: "branch", UUID: "*", Value: c.Name},
			{Type: deck.TagSingleton, Name: "sym-" + c.Name, UUID: "*"},
		},
	})
	if err != nil {
		return err
	}

	short := uuid
	if len(short) > 10 {
		short = short[:10]
	}
	fmt.Printf("branch %s created at %s (rid=%d)\n", c.Name, short, rid)
	return nil
}

type RepoBranchCloseCmd struct {
	Name string `arg:"" help:"Branch name to close"`
	User string `help:"User for control artifacts (default: OS username)"`
}

func (c *RepoBranchCloseCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	// Find the branch tip: most recent rid with the sym-<name> tag active.
	tagName := "sym-" + c.Name
	var tipRID int64
	err = r.DB().QueryRow(`
		SELECT tx.rid FROM tagxref tx
		JOIN tag t ON t.tagid = tx.tagid
		WHERE t.tagname = ? AND tx.tagtype > 0
		ORDER BY tx.mtime DESC LIMIT 1`, tagName).Scan(&tipRID)
	if err != nil {
		return fmt.Errorf("branch %q not found or already closed", c.Name)
	}

	user := c.User
	if user == "" {
		user = currentUser()
	}

	fslID := libfossil.FslID(tipRID)

	// Cancel the sym-<name> tag.
	if _, err := tag.AddTag(r, tag.TagOpts{
		TargetRID: fslID,
		TagName:   tagName,
		TagType:   tag.TagCancel,
		User:      user,
	}); err != nil {
		return fmt.Errorf("cancelling branch tag: %w", err)
	}

	// Add "closed" singleton tag.
	if _, err := tag.AddTag(r, tag.TagOpts{
		TargetRID: fslID,
		TagName:   "closed",
		TagType:   tag.TagSingleton,
		User:      user,
	}); err != nil {
		return fmt.Errorf("adding closed tag: %w", err)
	}

	fmt.Printf("branch %s closed\n", c.Name)
	return nil
}
