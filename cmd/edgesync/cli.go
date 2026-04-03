package main

import (
	"database/sql"
	"fmt"
	"os"
	"os/user"
	"path/filepath"

	libfossil "github.com/danmestas/go-libfossil"
	"github.com/danmestas/go-libfossil/repo"
)

type CLI struct {
	Globals

	Repo   RepoCmd   `cmd:"" help:"Repository operations"`
	Sync   SyncCmd   `cmd:"" help:"Leaf agent sync"`
	Bridge BridgeCmd `cmd:"" help:"NATS-to-Fossil bridge"`
	Doctor DoctorCmd `cmd:"" help:"Check development environment health"`
}

type Globals struct {
	Repo    string `short:"R" help:"Path to repository file" type:"path"`
	Verbose bool   `short:"v" help:"Verbose output"`
}

type RepoCmd struct {
	New      RepoNewCmd      `cmd:"" help:"Create a new repository"`
	Clone    RepoCloneCmd    `cmd:"" help:"Clone a remote repository"`
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
	Conflicts   RepoConflictsCmd    `cmd:"" help:"List/manage unresolved conflicts"`
	MarkResolved RepoMergeResolveCmd `cmd:"" name:"mark-resolved" help:"Mark a conflict as resolved"`
	Undo         RepoUndoCmd         `cmd:"" help:"Undo last operation"`
	Redo         RepoRedoCmd         `cmd:"" help:"Redo undone operation"`
	Stash        RepoStashCmd        `cmd:"" help:"Stash working changes"`
	Bisect       RepoBisectCmd       `cmd:"" help:"Binary search for bugs"`
	Annotate     RepoAnnotateCmd     `cmd:"" help:"Annotate file lines with version history"`
	Blame        RepoBlameCmd        `cmd:"" help:"Alias for annotate"`
	Branch       RepoBranchCmd       `cmd:"" help:"Branch operations"`
	UV           RepoUVCmd           `cmd:"" name:"uv" help:"Unversioned file operations"`
	Schema       SchemaCmd           `cmd:"" help:"Synced table schema operations"`
	User         RepoUserCmd         `cmd:"" help:"User management"`
	Invite       RepoInviteCmd       `cmd:"" help:"Generate invite token for a user"`
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
		found, err := findRepo()
		if err != nil {
			return nil, fmt.Errorf("no repository specified (use -R <path>)")
		}
		g.Repo = found
	}
	return repo.Open(g.Repo)
}

// findRepo searches the current directory and its parents for a .fossil file
// or a .fslckout checkout database that points to a repo.
func findRepo() (string, error) {
	dir, err := os.Getwd()
	if err != nil {
		return "", err
	}
	for {
		// Check for .fslckout (checkout database with repo pointer).
		ckout := filepath.Join(dir, ".fslckout")
		if _, err := os.Stat(ckout); err == nil {
			return repoFromCheckout(ckout)
		}

		// Check for a single .fossil file in this directory.
		matches, _ := filepath.Glob(filepath.Join(dir, "*.fossil"))
		if len(matches) == 1 {
			return matches[0], nil
		}

		parent := filepath.Dir(dir)
		if parent == dir {
			break
		}
		dir = parent
	}
	return "", fmt.Errorf("no .fossil file found")
}

// repoFromCheckout reads the repository path from a .fslckout database.
func repoFromCheckout(ckoutPath string) (string, error) {
	db, err := sql.Open("sqlite", ckoutPath+"?mode=ro")
	if err != nil {
		return "", err
	}
	defer db.Close()
	var repoPath string
	err = db.QueryRow("SELECT value FROM vvar WHERE name='repository'").Scan(&repoPath)
	if err != nil {
		return "", fmt.Errorf("checkout %s: no repository path found", ckoutPath)
	}
	return repoPath, nil
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
