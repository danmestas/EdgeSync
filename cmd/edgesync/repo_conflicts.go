package main

import (
	"fmt"

	"github.com/dmestas/edgesync/go-libfossil/merge"
)

type RepoConflictsCmd struct {
	Dir string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoConflictsCmd) Run(g *Globals) error {
	found := 0

	// Check vfile.chnged=5 for standard merge conflicts.
	ckout, err := openCheckout(c.Dir)
	if err == nil {
		defer ckout.Close()
		vid, _ := checkoutVid(ckout)
		rows, err := ckout.Query("SELECT pathname FROM vfile WHERE chnged=5 AND vid=?", vid)
		if err == nil {
			defer rows.Close()
			for rows.Next() {
				var name string
				rows.Scan(&name)
				fmt.Printf("CONFLICT  %s\n", name)
				found++
			}
		}
	}

	// Check conflict table for conflict-fork entries.
	r, err := openRepo(g)
	if err == nil {
		defer r.Close()
		forks, err := merge.ListConflictForks(r)
		if err == nil {
			for _, name := range forks {
				fmt.Printf("FORK      %s\n", name)
				found++
			}
		}
	}

	if found == 0 {
		fmt.Println("no conflicts")
	}
	return nil
}
