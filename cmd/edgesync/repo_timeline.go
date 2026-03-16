package main

import (
	"fmt"

	"github.com/dmestas/edgesync/go-libfossil/manifest"
)

type RepoTimelineCmd struct {
	Limit int `short:"n" default:"20" help:"Number of entries"`
}

func (c *RepoTimelineCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	tipRid, err := resolveRID(r, "")
	if err != nil {
		return err
	}

	entries, err := manifest.Log(r, manifest.LogOpts{Start: tipRid, Limit: c.Limit})
	if err != nil {
		return err
	}

	for _, e := range entries {
		uuid := e.UUID
		if len(uuid) > 10 {
			uuid = uuid[:10]
		}
		fmt.Printf("%s  %s  %s  %s\n", uuid, e.Time.Format("2006-01-02 15:04"), e.User, e.Comment)
	}
	return nil
}
