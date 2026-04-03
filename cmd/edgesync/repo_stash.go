package main

import (
	"database/sql"
	"fmt"
	"os"

	"github.com/danmestas/go-libfossil/stash"
	"github.com/danmestas/go-libfossil/undo"
)

type RepoStashCmd struct {
	Save  RepoStashSaveCmd  `cmd:"" help:"Stash working changes"`
	Pop   RepoStashPopCmd   `cmd:"" help:"Apply top stash and drop it"`
	Apply RepoStashApplyCmd `cmd:"" help:"Apply stash without dropping"`
	Ls    RepoStashLsCmd    `cmd:"" help:"List stash entries"`
	Show  RepoStashShowCmd  `cmd:"" help:"Show stash diff"`
	Drop  RepoStashDropCmd  `cmd:"" help:"Remove stash entry"`
	Clear RepoStashClearCmd `cmd:"" help:"Remove all stash entries"`
}

type RepoStashSaveCmd struct {
	Message string `short:"m" help:"Stash message" default:""`
	Dir     string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoStashSaveCmd) Run(g *Globals) error {
	ckout, err := openCheckout(c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()

	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	if err := stash.Save(ckout, r.DB().SqlDB(), c.Dir, c.Message); err != nil {
		return err
	}
	fmt.Println("changes stashed")
	return nil
}

type RepoStashPopCmd struct {
	Dir string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoStashPopCmd) Run(g *Globals) error {
	ckout, err := openCheckout(c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()

	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	if err := undo.Save(ckout, c.Dir, nil); err != nil {
		fmt.Fprintf(os.Stderr, "warning: undo save: %v\n", err)
	}

	if err := stash.Pop(ckout, r.DB().SqlDB(), c.Dir); err != nil {
		return err
	}
	fmt.Println("stash popped")
	return nil
}

type RepoStashApplyCmd struct {
	ID  int64  `arg:"" optional:"" help:"Stash ID to apply (default: latest)"`
	Dir string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoStashApplyCmd) Run(g *Globals) error {
	ckout, err := openCheckout(c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()

	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	stashID := c.ID
	if stashID == 0 {
		// Default to latest stash.
		err := ckout.QueryRow("SELECT stashid FROM stash ORDER BY stashid DESC LIMIT 1").Scan(&stashID)
		if err != nil {
			if err == sql.ErrNoRows {
				return fmt.Errorf("no stash entries")
			}
			return fmt.Errorf("query latest stash: %w", err)
		}
	}

	if err := undo.Save(ckout, c.Dir, nil); err != nil {
		fmt.Fprintf(os.Stderr, "warning: undo save: %v\n", err)
	}

	if err := stash.Apply(ckout, r.DB().SqlDB(), c.Dir, stashID); err != nil {
		return err
	}
	fmt.Printf("stash %d applied\n", stashID)
	return nil
}

type RepoStashLsCmd struct {
	Dir string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoStashLsCmd) Run(g *Globals) error {
	ckout, err := openCheckout(c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()

	entries, err := stash.List(ckout)
	if err != nil {
		return err
	}
	if len(entries) == 0 {
		fmt.Println("no stash entries")
		return nil
	}
	for _, e := range entries {
		fmt.Printf("%3d: %s  %s\n", e.ID, e.CTime, e.Comment)
	}
	return nil
}

type RepoStashShowCmd struct {
	ID  int64  `arg:"" optional:"" help:"Stash ID to show (default: latest)"`
	Dir string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoStashShowCmd) Run(g *Globals) error {
	ckout, err := openCheckout(c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()

	stashID := c.ID
	if stashID == 0 {
		err := ckout.QueryRow("SELECT stashid FROM stash ORDER BY stashid DESC LIMIT 1").Scan(&stashID)
		if err != nil {
			if err == sql.ErrNoRows {
				return fmt.Errorf("no stash entries")
			}
			return fmt.Errorf("query latest stash: %w", err)
		}
	}

	rows, err := ckout.Query("SELECT isAdded, isRemoved, newname FROM stashfile WHERE stashid=?", stashID)
	if err != nil {
		return fmt.Errorf("query stashfile: %w", err)
	}
	defer rows.Close()

	found := false
	for rows.Next() {
		found = true
		var isAdded, isRemoved bool
		var name string
		if err := rows.Scan(&isAdded, &isRemoved, &name); err != nil {
			return fmt.Errorf("scan stashfile: %w", err)
		}
		switch {
		case isAdded:
			fmt.Printf("ADDED    %s\n", name)
		case isRemoved:
			fmt.Printf("REMOVED  %s\n", name)
		default:
			fmt.Printf("EDITED   %s\n", name)
		}
	}
	if err := rows.Err(); err != nil {
		return err
	}
	if !found {
		return fmt.Errorf("stash %d not found", stashID)
	}
	return nil
}

type RepoStashDropCmd struct {
	ID  int64  `arg:"" required:"" help:"Stash ID to drop"`
	Dir string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoStashDropCmd) Run(g *Globals) error {
	ckout, err := openCheckout(c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()

	if err := stash.Drop(ckout, c.ID); err != nil {
		return err
	}
	fmt.Printf("stash %d dropped\n", c.ID)
	return nil
}

type RepoStashClearCmd struct {
	Dir string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoStashClearCmd) Run(g *Globals) error {
	ckout, err := openCheckout(c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()

	if err := stash.Clear(ckout); err != nil {
		return err
	}
	fmt.Println("all stash entries cleared")
	return nil
}
