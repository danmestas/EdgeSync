package main

import (
	"fmt"

	libfossil "github.com/danmestas/go-libfossil"
	ann "github.com/danmestas/go-libfossil/annotate"
)

type RepoAnnotateCmd struct {
	File    string `arg:"" required:"" help:"File to annotate"`
	Version string `help:"Starting version (default: tip)"`
	Limit   int    `short:"n" help:"Max versions to walk (0=unlimited)" default:"0"`
	Origin  string `help:"Stop at this version"`
}

func (c *RepoAnnotateCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	startRID, err := resolveRID(r, c.Version)
	if err != nil {
		return err
	}

	var originRID libfossil.FslID
	if c.Origin != "" {
		originRID, err = resolveRID(r, c.Origin)
		if err != nil {
			return err
		}
	}

	lines, err := ann.Annotate(r, ann.Options{
		FilePath:  c.File,
		StartRID:  startRID,
		Limit:     c.Limit,
		OriginRID: originRID,
	})
	if err != nil {
		return err
	}

	for _, l := range lines {
		uuid := l.Version.UUID
		if len(uuid) > 10 {
			uuid = uuid[:10]
		}
		fmt.Printf("%s %8s %s | %s\n",
			uuid, l.Version.User, l.Version.Date.Format("2006-01-02"), l.Text)
	}
	return nil
}

// RepoBlameCmd is an alias for annotate.
type RepoBlameCmd struct {
	RepoAnnotateCmd
}
