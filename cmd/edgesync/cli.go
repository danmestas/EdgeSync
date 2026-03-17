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
	Hash     RepoHashCmd     `cmd:"" help:"Hash files (SHA1 or SHA3)"`
	Delta    RepoDeltaCmd    `cmd:"" help:"Delta create/apply operations"`
	Config   RepoConfigCmd   `cmd:"" help:"Repository configuration"`
	Query    RepoQueryCmd    `cmd:"" help:"Execute SQL against repository"`
	Verify   RepoVerifyCmd   `cmd:"" help:"Verify repository integrity"`
	Resolve  RepoResolveCmd  `cmd:"" help:"Resolve symbolic name to UUID"`
	Extract  RepoExtractCmd  `cmd:"" help:"Extract files from a version"`
	Wiki     RepoWikiCmd     `cmd:"" help:"Wiki page operations"`
	Tag      RepoTagCmd      `cmd:"" help:"Tag operations"`
	Open     RepoOpenCmd     `cmd:"" help:"Open a checkout in a directory"`
	Status   RepoStatusCmd   `cmd:"" help:"Show working directory changes"`
	Add      RepoAddCmd      `cmd:"" help:"Stage files for addition"`
	Rm       RepoRmCmd       `cmd:"" help:"Stage files for removal"`
	Rename   RepoRenameCmd   `cmd:"" help:"Rename a tracked file"`
	Revert   RepoRevertCmd   `cmd:"" help:"Undo staging changes"`
	Diff      RepoDiffCmd         `cmd:"" help:"Show changes vs a version"`
	Merge      RepoMergeCmd        `cmd:"" help:"Merge a divergent version"`
	Conflicts  RepoConflictsCmd    `cmd:"" help:"List unresolved conflicts"`
	Unresolve  RepoMergeResolveCmd `cmd:"" name:"mark-resolved" help:"Mark a conflict as resolved"`
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

// resolveRID resolves a version string to a rid.
// Accepts: empty/"tip" (most recent checkin), "trunk" (tagged trunk tip),
// UUID prefix (min 4 chars), or full UUID.
func resolveRID(r *repo.Repo, version string) (libfossil.FslID, error) {
	switch version {
	case "", "tip":
		var rid int64
		err := r.DB().QueryRow(
			"SELECT objid FROM event WHERE type='ci' ORDER BY mtime DESC LIMIT 1",
		).Scan(&rid)
		if err != nil {
			return 0, fmt.Errorf("no checkins found")
		}
		return libfossil.FslID(rid), nil

	case "trunk":
		// trunk = most recent checkin tagged "trunk"
		var rid int64
		err := r.DB().QueryRow(`
			SELECT tagxref.rid FROM tagxref
			JOIN tag ON tag.tagid=tagxref.tagid
			WHERE tag.tagname='sym-trunk'
			  AND tagxref.tagtype>0
			ORDER BY tagxref.mtime DESC LIMIT 1`,
		).Scan(&rid)
		if err != nil {
			// Fallback to tip if no trunk tag
			return resolveRID(r, "tip")
		}
		return libfossil.FslID(rid), nil

	default:
		// UUID or prefix lookup
		var rid int64
		err := r.DB().QueryRow(
			"SELECT rid FROM blob WHERE uuid LIKE ?", version+"%",
		).Scan(&rid)
		if err != nil {
			return 0, fmt.Errorf("artifact %q not found", version)
		}
		return libfossil.FslID(rid), nil
	}
}

func currentUser() string {
	if u, err := user.Current(); err == nil {
		return u.Username
	}
	return "anonymous"
}
