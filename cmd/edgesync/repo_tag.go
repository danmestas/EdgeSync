package main

import (
	"fmt"
)

type RepoTagCmd struct {
	Ls  RepoTagLsCmd  `cmd:"" help:"List tags on an artifact"`
	Add RepoTagAddCmd `cmd:"" help:"Add a tag to an artifact"`
}

type RepoTagLsCmd struct {
	Version string `arg:"" optional:"" help:"Version to list tags for (default: tip)"`
}

func (c *RepoTagLsCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	rid, err := resolveRID(r, c.Version)
	if err != nil {
		return err
	}

	rows, err := r.DB().Query(`
		SELECT t.tagname, tx.value,
		       CASE tx.tagtype WHEN 1 THEN '+' WHEN 2 THEN '*' ELSE '-' END as type
		FROM tagxref tx
		JOIN tag t ON t.tagid=tx.tagid
		WHERE tx.rid=? AND tx.tagtype>0
		ORDER BY t.tagname`, rid)
	if err != nil {
		return err
	}
	defer rows.Close()

	for rows.Next() {
		var name, value, tagtype string
		rows.Scan(&name, &value, &tagtype)
		if value != "" {
			fmt.Printf("%s%s=%s\n", tagtype, name, value)
		} else {
			fmt.Printf("%s%s\n", tagtype, name)
		}
	}
	return rows.Err()
}

type RepoTagAddCmd struct {
	Tag     string `arg:"" help:"Tag name"`
	Value   string `arg:"" optional:"" help:"Tag value (optional)"`
	Version string `help:"Version to tag (default: tip)"`
}

func (c *RepoTagAddCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	rid, err := resolveRID(r, c.Version)
	if err != nil {
		return err
	}

	// Get or create the tag ID.
	tagName := "sym-" + c.Tag
	var tagID int64
	err = r.DB().QueryRow("SELECT tagid FROM tag WHERE tagname=?", tagName).Scan(&tagID)
	if err != nil {
		result, err := r.DB().Exec("INSERT INTO tag(tagname) VALUES(?)", tagName)
		if err != nil {
			return fmt.Errorf("creating tag: %w", err)
		}
		tagID, _ = result.LastInsertId()
	}

	// Insert tagxref: tagtype 1 = singleton (+), 2 = propagating (*)
	_, err = r.DB().Exec(`
		INSERT OR REPLACE INTO tagxref(tagid, tagtype, srcid, rid, mtime, value)
		VALUES(?, 1, 0, ?, julianday('now'), ?)`,
		tagID, rid, c.Value)
	if err != nil {
		return fmt.Errorf("applying tag: %w", err)
	}

	var uuid string
	r.DB().QueryRow("SELECT uuid FROM blob WHERE rid=?", rid).Scan(&uuid)
	if len(uuid) > 10 {
		uuid = uuid[:10]
	}
	fmt.Printf("tagged %s with %s\n", uuid, c.Tag)
	return nil
}
