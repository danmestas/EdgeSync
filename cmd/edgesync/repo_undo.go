package main

import "github.com/dmestas/edgesync/go-libfossil/undo"

type RepoUndoCmd struct {
	Dir string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoUndoCmd) Run(g *Globals) error {
	ckout, err := openCheckout(c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()
	return undo.Undo(ckout, c.Dir)
}

type RepoRedoCmd struct {
	Dir string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoRedoCmd) Run(g *Globals) error {
	ckout, err := openCheckout(c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()
	return undo.Redo(ckout, c.Dir)
}
