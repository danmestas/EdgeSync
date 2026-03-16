package main

import (
	"fmt"

	"github.com/dmestas/edgesync/go-libfossil/manifest"
)

type RepoLsCmd struct {
	Version string `arg:"" optional:"" help:"Version to list (default: tip)"`
	Long    bool   `short:"l" help:"Show sizes and hashes"`
}

func (c *RepoLsCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	rid, err := resolveRID(r, c.Version)
	if err != nil {
		return err
	}

	files, err := manifest.ListFiles(r, rid)
	if err != nil {
		return err
	}

	for _, f := range files {
		if c.Long {
			fmt.Printf("%s  %s  %s\n", f.UUID[:10], f.Perm, f.Name)
		} else {
			fmt.Println(f.Name)
		}
	}
	return nil
}
