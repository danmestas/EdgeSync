package main

import (
	"fmt"
	"os/user"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

type CLI struct {
	Globals

	Repo   RepoCmd   `cmd:"" help:"Repository operations"`
	Sync   SyncCmd   `cmd:"" help:"Leaf agent sync"`
	Bridge BridgeCmd `cmd:"" help:"NATS-to-Fossil bridge"`
}

type Globals struct {
	Repo    string `short:"R" help:"Path to repository file" type:"path"`
	Verbose bool   `short:"v" help:"Verbose output"`
}

type RepoCmd struct {
	New      RepoNewCmd      `cmd:"" help:"Create a new repository"`
	Ci       RepoCiCmd       `cmd:"" help:"Checkin file changes"`
	Co       RepoCoCmd       `cmd:"" help:"Checkout a version"`
	Ls       RepoLsCmd       `cmd:"" help:"List files in a version"`
	Timeline RepoTimelineCmd `cmd:"" help:"Show repository history"`
	Cat      RepoCatCmd      `cmd:"" help:"Output artifact content"`
	Info     RepoInfoCmd     `cmd:"" help:"Repository statistics"`
}

type SyncCmd struct {
	Start SyncStartCmd `cmd:"" help:"Start leaf agent daemon"`
	Now   SyncNowCmd   `cmd:"" help:"Trigger immediate sync"`
}

type BridgeCmd struct {
	Serve BridgeServeCmd `cmd:"" help:"Start NATS-to-Fossil bridge"`
}

func openRepo(g *Globals) (*repo.Repo, error) {
	if g.Repo == "" {
		return nil, fmt.Errorf("no repository specified (use -R <path>)")
	}
	return repo.Open(g.Repo)
}

func resolveRID(r *repo.Repo, version string) (libfossil.FslID, error) {
	if version == "" {
		var rid int64
		err := r.DB().QueryRow(
			"SELECT objid FROM event WHERE type='ci' ORDER BY mtime DESC LIMIT 1",
		).Scan(&rid)
		if err != nil {
			return 0, fmt.Errorf("no checkins found")
		}
		return libfossil.FslID(rid), nil
	}
	var rid int64
	err := r.DB().QueryRow(
		"SELECT rid FROM blob WHERE uuid LIKE ?", version+"%",
	).Scan(&rid)
	if err != nil {
		return 0, fmt.Errorf("artifact %q not found", version)
	}
	return libfossil.FslID(rid), nil
}

func currentUser() string {
	if u, err := user.Current(); err == nil {
		return u.Username
	}
	return "anonymous"
}
