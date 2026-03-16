package main

import (
	"fmt"
	"os"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
)

type RepoCatCmd struct {
	Artifact string `arg:"" help:"Artifact UUID or prefix"`
	Raw      bool   `help:"Output raw blob (no delta expansion)"`
}

func (c *RepoCatCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	rid, err := resolveRID(r, c.Artifact)
	if err != nil {
		return err
	}

	var data []byte
	if c.Raw {
		data, err = blob.Load(r.DB(), rid)
	} else {
		data, err = content.Expand(r.DB(), rid)
	}
	if err != nil {
		return fmt.Errorf("reading artifact: %w", err)
	}

	os.Stdout.Write(data)
	return nil
}
