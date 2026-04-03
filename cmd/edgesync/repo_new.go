package main

import (
	"fmt"

	"github.com/danmestas/go-libfossil/repo"
	"github.com/danmestas/go-libfossil/simio"
)

type RepoNewCmd struct {
	Path string `arg:"" help:"Path for new repository file"`
	User string `help:"Default user name" default:""`
}

func (c *RepoNewCmd) Run(g *Globals) error {
	user := c.User
	if user == "" {
		user = currentUser()
	}
	r, err := repo.Create(c.Path, user, simio.CryptoRand{})
	if err != nil {
		return err
	}
	r.Close()
	fmt.Printf("created repository: %s\n", c.Path)
	return nil
}
