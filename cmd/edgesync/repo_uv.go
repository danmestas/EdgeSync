package main

import (
	"fmt"
	"os"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/uv"
)

type RepoUVCmd struct {
	Ls     RepoUVLsCmd     `cmd:"" help:"List unversioned files"`
	Put    RepoUVPutCmd    `cmd:"" help:"Add or update an unversioned file"`
	Get    RepoUVGetCmd    `cmd:"" help:"Retrieve an unversioned file"`
	Delete RepoUVDeleteCmd `cmd:"" help:"Delete an unversioned file (creates tombstone)"`
}

type RepoUVLsCmd struct{}

func (c *RepoUVLsCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	if err := uv.EnsureSchema(r.DB()); err != nil {
		return err
	}

	entries, err := uv.List(r.DB())
	if err != nil {
		return err
	}

	if len(entries) == 0 {
		fmt.Println("(no unversioned files)")
		return nil
	}

	for _, e := range entries {
		ts := time.Unix(e.MTime, 0).UTC().Format("2006-01-02 15:04:05")
		if e.Hash == "" {
			fmt.Printf("%-40s %s  [deleted]\n", e.Name, ts)
		} else {
			fmt.Printf("%-40s %s  %6d  %.10s\n", e.Name, ts, e.Size, e.Hash)
		}
	}
	return nil
}

type RepoUVPutCmd struct {
	Name string `arg:"" help:"Name of the unversioned file"`
	File string `arg:"" help:"Local file to upload"`
}

func (c *RepoUVPutCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	if err := uv.EnsureSchema(r.DB()); err != nil {
		return err
	}

	data, err := os.ReadFile(c.File)
	if err != nil {
		return fmt.Errorf("read %s: %w", c.File, err)
	}

	mtime := time.Now().Unix()
	if err := uv.Write(r.DB(), c.Name, data, mtime); err != nil {
		return err
	}

	fmt.Printf("wrote %s (%d bytes)\n", c.Name, len(data))
	return nil
}

type RepoUVGetCmd struct {
	Name   string `arg:"" help:"Name of the unversioned file"`
	Output string `short:"o" help:"Output file (default: stdout)"`
}

func (c *RepoUVGetCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	if err := uv.EnsureSchema(r.DB()); err != nil {
		return err
	}

	data, _, hash, err := uv.Read(r.DB(), c.Name)
	if err != nil {
		return err
	}
	if data == nil && hash == "" {
		return fmt.Errorf("unversioned file %q not found", c.Name)
	}
	if hash == "" {
		return fmt.Errorf("unversioned file %q has been deleted", c.Name)
	}

	if c.Output != "" {
		return os.WriteFile(c.Output, data, 0o644)
	}
	_, err = os.Stdout.Write(data)
	return err
}

type RepoUVDeleteCmd struct {
	Name string `arg:"" help:"Name of the unversioned file to delete"`
}

func (c *RepoUVDeleteCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	if err := uv.EnsureSchema(r.DB()); err != nil {
		return err
	}

	mtime := time.Now().Unix()
	if err := uv.Delete(r.DB(), c.Name, mtime); err != nil {
		return err
	}

	fmt.Printf("deleted %s\n", c.Name)
	return nil
}
