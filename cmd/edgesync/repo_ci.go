package main

import (
	"fmt"
	"os"
	"path/filepath"
	"time"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
)

type RepoCiCmd struct {
	Message      string   `short:"m" required:"" help:"Checkin comment"`
	Files        []string `arg:"" required:"" help:"Files to checkin"`
	User         string   `help:"Checkin user (default: OS username)"`
	Parent       string   `help:"Parent version UUID (default: tip)"`
	Branch       string   `help:"Branch name for this checkin"`
	Autosync     string   `enum:"on,off,pullonly" default:"off" help:"Autosync mode: on, off, pullonly"`
	AllowFork    bool     `help:"Bypass fork and lock checks"`
	OverrideLock bool     `help:"Ignore lock conflicts (implies --allow-fork)"`
}

func (c *RepoCiCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	var parentRid libfossil.FslID
	if c.Parent != "" {
		parentRid, err = resolveRID(r, c.Parent)
		if err != nil {
			return fmt.Errorf("resolving parent: %w", err)
		}
	} else {
		parentRid, _ = resolveRID(r, "") // ignore error for initial checkin
	}

	files := make([]manifest.File, len(c.Files))
	for i, path := range c.Files {
		data, err := os.ReadFile(path)
		if err != nil {
			return fmt.Errorf("reading %s: %w", path, err)
		}
		files[i] = manifest.File{
			Name:    filepath.Base(path),
			Content: data,
		}
	}

	user := c.User
	if user == "" {
		user = currentUser()
	}

	rid, uuid, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   files,
		Comment: c.Message,
		User:    user,
		Parent:  parentRid,
		Time:    time.Now().UTC(),
	})
	if err != nil {
		return err
	}

	fmt.Printf("checkin %s (rid=%d)\n", uuid[:10], rid)
	return nil
}
