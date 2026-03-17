package main

import (
	"fmt"
)

type RepoRevertCmd struct {
	Files []string `arg:"" optional:"" help:"Files to revert (default: all)"`
	Dir   string   `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoRevertCmd) Run(g *Globals) error {
	ckout, err := openCheckout(c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()

	vid, err := checkoutVid(ckout)
	if err != nil {
		return err
	}

	if len(c.Files) == 0 {
		// Revert all: reset changed/deleted flags, remove untracked additions.
		result, err := ckout.Exec("UPDATE vfile SET chnged=0, deleted=0, origname=NULL WHERE vid=? AND (chnged=1 OR deleted=1)", vid)
		if err != nil {
			return err
		}
		affected, _ := result.RowsAffected()

		// Remove files that were newly added (rid=0 means not in repo yet).
		removed, err := ckout.Exec("DELETE FROM vfile WHERE vid=? AND rid=0", vid)
		if err != nil {
			return err
		}
		removedCount, _ := removed.RowsAffected()

		fmt.Printf("reverted %d files, removed %d staged additions\n", affected, removedCount)
	} else {
		for _, name := range c.Files {
			var id, rid int64
			err := ckout.QueryRow("SELECT id, rid FROM vfile WHERE pathname=? AND vid=?", name, vid).Scan(&id, &rid)
			if err != nil {
				return fmt.Errorf("%s: not tracked in checkout", name)
			}
			if rid == 0 {
				// Newly added — remove from vfile.
				ckout.Exec("DELETE FROM vfile WHERE id=?", id)
				fmt.Printf("UNSTAGED %s\n", name)
			} else {
				// Existing — reset flags.
				ckout.Exec("UPDATE vfile SET chnged=0, deleted=0, origname=NULL WHERE id=?", id)
				fmt.Printf("REVERTED %s\n", name)
			}
		}
	}
	return nil
}
