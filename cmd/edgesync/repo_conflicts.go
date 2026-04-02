package main

import (
	"fmt"
	"os"
	"path/filepath"

	libfossil "github.com/danmestas/go-libfossil"
	"github.com/danmestas/go-libfossil/blob"
	"github.com/danmestas/go-libfossil/content"
	"github.com/danmestas/go-libfossil/manifest"
	"github.com/danmestas/go-libfossil/merge"
	"github.com/danmestas/go-libfossil/repo"
)

type RepoConflictsCmd struct {
	Ls      RepoConflictsLsCmd      `cmd:"" default:"1" help:"List all conflicts"`
	Show    RepoConflictsShowCmd    `cmd:"" help:"Show all versions of a conflicted file"`
	Pick    RepoConflictsPickCmd    `cmd:"" help:"Resolve by picking one version"`
	Merge   RepoConflictsMergeCmd   `cmd:"" help:"Resolve by re-merging with a different strategy"`
	Extract RepoConflictsExtractCmd `cmd:"" help:"Extract all versions to disk for manual editing"`
	Dir     string                  `short:"d" help:"Checkout directory" default:"."`
}

type RepoConflictsLsCmd struct{}

func (c *RepoConflictsLsCmd) Run(g *Globals) error {
	// Delegate to parent's listing logic.
	return (&RepoConflictsCmd{Dir: "."}).list(g)
}

func (c *RepoConflictsCmd) list(g *Globals) error {
	found := 0

	// Standard merge conflicts (vfile.chnged=5).
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

	// Conflict-fork entries.
	r, err := openRepo(g)
	if err == nil {
		defer r.Close()
		entries, err := listConflictForkDetails(r)
		if err == nil {
			for _, e := range entries {
				fmt.Printf("FORK      %s  (base=%d local=%d remote=%d)\n",
					e.filename, e.baseRid, e.localRid, e.remoteRid)
				found++
			}
		}
	}

	if found == 0 {
		fmt.Println("no conflicts")
	}
	return nil
}

type conflictForkEntry struct {
	filename  string
	baseRid   int64
	localRid  int64
	remoteRid int64
}

func listConflictForkDetails(r *repo.Repo) ([]conflictForkEntry, error) {
	var count int
	if r.DB().QueryRow("SELECT count(*) FROM sqlite_master WHERE type='table' AND name='conflict'").Scan(&count); count == 0 {
		return nil, nil
	}
	rows, err := r.DB().Query("SELECT filename, base_rid, local_rid, remote_rid FROM conflict ORDER BY mtime DESC")
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var entries []conflictForkEntry
	for rows.Next() {
		var e conflictForkEntry
		rows.Scan(&e.filename, &e.baseRid, &e.localRid, &e.remoteRid)
		entries = append(entries, e)
	}
	return entries, rows.Err()
}

func loadConflictFork(r *repo.Repo, filename string) (*conflictForkEntry, error) {
	var count int
	if r.DB().QueryRow("SELECT count(*) FROM sqlite_master WHERE type='table' AND name='conflict'").Scan(&count); count == 0 {
		return nil, fmt.Errorf("%s: no conflict-fork entries", filename)
	}
	var e conflictForkEntry
	e.filename = filename
	err := r.DB().QueryRow("SELECT base_rid, local_rid, remote_rid FROM conflict WHERE filename=?", filename).
		Scan(&e.baseRid, &e.localRid, &e.remoteRid)
	if err != nil {
		return nil, fmt.Errorf("%s: not found in conflict table", filename)
	}
	return &e, nil
}

func expandForkFile(r *repo.Repo, checkinRid int64, filename string) ([]byte, error) {
	if checkinRid <= 0 {
		return nil, nil
	}
	files, err := manifest.ListFiles(r, libfossil.FslID(checkinRid))
	if err != nil {
		return nil, err
	}
	for _, f := range files {
		if f.Name == filename {
			frid, ok := blob.Exists(r.DB(), f.UUID)
			if !ok {
				return nil, fmt.Errorf("blob %s not found", f.UUID)
			}
			return content.Expand(r.DB(), frid)
		}
	}
	return nil, fmt.Errorf("file %s not found in checkin %d", filename, checkinRid)
}

// --- Show ---

type RepoConflictsShowCmd struct {
	File string `arg:"" help:"Conflicted file to show"`
}

func (c *RepoConflictsShowCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	entry, err := loadConflictFork(r, c.File)
	if err != nil {
		return err
	}

	base, _ := expandForkFile(r, entry.baseRid, c.File)
	local, _ := expandForkFile(r, entry.localRid, c.File)
	remote, _ := expandForkFile(r, entry.remoteRid, c.File)

	fmt.Printf("=== BASE (ancestor, rid=%d) ===\n", entry.baseRid)
	os.Stdout.Write(base)
	fmt.Printf("\n=== LOCAL (your version, rid=%d) ===\n", entry.localRid)
	os.Stdout.Write(local)
	fmt.Printf("\n=== REMOTE (their version, rid=%d) ===\n", entry.remoteRid)
	os.Stdout.Write(remote)
	fmt.Println()
	return nil
}

// --- Pick ---

type RepoConflictsPickCmd struct {
	File   string `arg:"" help:"Conflicted file to resolve"`
	Local  bool   `help:"Keep local version" xor:"version"`
	Remote bool   `help:"Keep remote version" xor:"version"`
	Base   bool   `help:"Revert to base version" xor:"version"`
	Dir    string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoConflictsPickCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	entry, err := loadConflictFork(r, c.File)
	if err != nil {
		return err
	}

	var picked []byte
	var label string
	switch {
	case c.Remote:
		picked, _ = expandForkFile(r, entry.remoteRid, c.File)
		label = "remote"
	case c.Base:
		picked, _ = expandForkFile(r, entry.baseRid, c.File)
		label = "base"
	default:
		picked, _ = expandForkFile(r, entry.localRid, c.File)
		label = "local"
	}

	outPath := filepath.Join(c.Dir, c.File)
	os.MkdirAll(filepath.Dir(outPath), 0o755)
	if err := os.WriteFile(outPath, picked, 0o644); err != nil {
		return err
	}

	merge.ResolveConflictFork(r, c.File)
	fmt.Printf("resolved: %s (picked %s)\n", c.File, label)
	return nil
}

// --- Merge ---

type RepoConflictsMergeCmd struct {
	File     string `arg:"" help:"Conflicted file to re-merge"`
	Strategy string `help:"Strategy to use" default:"three-way"`
	Dir      string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoConflictsMergeCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	entry, err := loadConflictFork(r, c.File)
	if err != nil {
		return err
	}

	strat, ok := merge.StrategyByName(c.Strategy)
	if !ok {
		return fmt.Errorf("unknown strategy: %s", c.Strategy)
	}

	base, _ := expandForkFile(r, entry.baseRid, c.File)
	local, _ := expandForkFile(r, entry.localRid, c.File)
	remote, _ := expandForkFile(r, entry.remoteRid, c.File)

	result, err := strat.Merge(base, local, remote)
	if err != nil {
		return err
	}

	outPath := filepath.Join(c.Dir, c.File)
	os.MkdirAll(filepath.Dir(outPath), 0o755)
	if err := os.WriteFile(outPath, result.Content, 0o644); err != nil {
		return err
	}

	if result.Clean {
		merge.ResolveConflictFork(r, c.File)
		fmt.Printf("resolved: %s (merged with %s, clean)\n", c.File, c.Strategy)
	} else {
		// Write sidecar files for remaining conflicts.
		os.WriteFile(outPath+".LOCAL", local, 0o644)
		os.WriteFile(outPath+".BASELINE", base, 0o644)
		os.WriteFile(outPath+".MERGE", remote, 0o644)
		fmt.Printf("merged: %s (%s, %d conflicts remain — edit and run mark-resolved)\n",
			c.File, c.Strategy, len(result.Conflicts))
	}
	return nil
}

// --- Extract ---

type RepoConflictsExtractCmd struct {
	File string `arg:"" help:"Conflicted file to extract"`
	Dir  string `short:"d" help:"Output directory" default:"."`
}

func (c *RepoConflictsExtractCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	entry, err := loadConflictFork(r, c.File)
	if err != nil {
		return err
	}

	base, _ := expandForkFile(r, entry.baseRid, c.File)
	local, _ := expandForkFile(r, entry.localRid, c.File)
	remote, _ := expandForkFile(r, entry.remoteRid, c.File)

	os.MkdirAll(c.Dir, 0o755)

	basePath := filepath.Join(c.Dir, c.File+".BASE")
	localPath := filepath.Join(c.Dir, c.File+".LOCAL")
	remotePath := filepath.Join(c.Dir, c.File+".REMOTE")

	os.MkdirAll(filepath.Dir(basePath), 0o755)
	os.WriteFile(basePath, base, 0o644)
	os.WriteFile(localPath, local, 0o644)
	os.WriteFile(remotePath, remote, 0o644)

	fmt.Printf("  %s\n", basePath)
	fmt.Printf("  %s\n", localPath)
	fmt.Printf("  %s\n", remotePath)
	return nil
}
