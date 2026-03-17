package main

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/merge"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

type RepoMergeCmd struct {
	Version  string             `arg:"" help:"Version to merge into current checkout"`
	Strategy string             `help:"Override merge strategy for all files"`
	DryRun   bool               `help:"Show what would be merged without writing"`
	Dir      string             `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoMergeCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	// Resolve local (tip) and remote versions.
	localRid, err := resolveRID(r, "tip")
	if err != nil {
		return fmt.Errorf("resolving local tip: %w", err)
	}
	remoteRid, err := resolveRID(r, c.Version)
	if err != nil {
		return fmt.Errorf("resolving remote version: %w", err)
	}

	// Find common ancestor.
	ancestorRid, err := merge.FindCommonAncestor(r, localRid, remoteRid)
	if err != nil {
		return fmt.Errorf("finding common ancestor: %w", err)
	}

	var ancestorUUID string
	r.DB().QueryRow("SELECT uuid FROM blob WHERE rid=?", ancestorRid).Scan(&ancestorUUID)
	if len(ancestorUUID) > 10 {
		ancestorUUID = ancestorUUID[:10]
	}
	fmt.Printf("common ancestor: %s (rid=%d)\n", ancestorUUID, ancestorRid)

	// Load file lists for all three versions.
	baseFiles, err := manifest.ListFiles(r, ancestorRid)
	if err != nil {
		return fmt.Errorf("listing ancestor files: %w", err)
	}
	localFiles, err := manifest.ListFiles(r, localRid)
	if err != nil {
		return fmt.Errorf("listing local files: %w", err)
	}
	remoteFiles, err := manifest.ListFiles(r, remoteRid)
	if err != nil {
		return fmt.Errorf("listing remote files: %w", err)
	}

	// Build maps: filename → UUID.
	baseMap := fileMap(baseFiles)
	localMap := fileMap(localFiles)
	remoteMap := fileMap(remoteFiles)

	// Load resolver for strategy selection.
	resolver := merge.LoadResolver(r, localRid)

	// Open checkout DB for vfile updates.
	ckout, err := openCheckout(c.Dir)
	if err != nil && !c.DryRun {
		return err
	}
	if ckout != nil {
		defer ckout.Close()
	}
	vid, _ := checkoutVid(ckout)

	merged, conflicts := 0, 0

	// Find files that differ between local and remote.
	allFiles := make(map[string]bool)
	for name := range localMap {
		allFiles[name] = true
	}
	for name := range remoteMap {
		allFiles[name] = true
	}

	for name := range allFiles {
		localUUID := localMap[name]
		remoteUUID := remoteMap[name]
		baseUUID := baseMap[name]

		// Skip if both sides have the same content.
		if localUUID == remoteUUID {
			continue
		}

		// Determine strategy.
		stratName := c.Strategy
		if stratName == "" {
			stratName = resolver.Resolve(name)
		}
		strat, ok := merge.StrategyByName(stratName)
		if !ok {
			return fmt.Errorf("unknown strategy %q for %s", stratName, name)
		}

		// Load content for the three versions.
		baseContent := loadBlobByUUID(r, baseUUID)
		localContent := loadBlobByUUID(r, localUUID)
		remoteContent := loadBlobByUUID(r, remoteUUID)

		if c.DryRun {
			fmt.Printf("  [%s] %s\n", stratName, name)
			continue
		}

		result, err := strat.Merge(baseContent, localContent, remoteContent)
		if err != nil {
			return fmt.Errorf("merging %s: %w", name, err)
		}

		outPath := filepath.Join(c.Dir, name)
		os.MkdirAll(filepath.Dir(outPath), 0o755)

		if result.Clean {
			os.WriteFile(outPath, result.Content, 0o644)
			if ckout != nil {
				ckout.Exec("UPDATE vfile SET chnged=1 WHERE pathname=? AND vid=?", name, vid)
			}
			fmt.Printf("  [merged]   %s (%s)\n", name, stratName)
			merged++
		} else if strat.Name() == "conflict-fork" {
			// ConflictFork: write to conflict table, keep local in working dir.
			merge.EnsureConflictTable(r)
			var baseRID, localRID, remoteRID int64
			r.DB().QueryRow("SELECT rid FROM blob WHERE uuid=?", baseUUID).Scan(&baseRID)
			r.DB().QueryRow("SELECT rid FROM blob WHERE uuid=?", localUUID).Scan(&localRID)
			r.DB().QueryRow("SELECT rid FROM blob WHERE uuid=?", remoteUUID).Scan(&remoteRID)
			merge.RecordConflictFork(r, name, baseRID, localRID, remoteRID)
			fmt.Printf("  [fork]     %s (conflict-fork: all versions preserved)\n", name)
			conflicts++
		} else {
			// Standard conflict: markers + sidecar files + vfile.chnged=5.
			os.WriteFile(outPath, result.Content, 0o644)
			os.WriteFile(outPath+".LOCAL", localContent, 0o644)
			os.WriteFile(outPath+".BASELINE", baseContent, 0o644)
			os.WriteFile(outPath+".MERGE", remoteContent, 0o644)
			if ckout != nil {
				ckout.Exec("UPDATE vfile SET chnged=5 WHERE pathname=? AND vid=?", name, vid)
			}
			fmt.Printf("  [CONFLICT] %s (%s, %d regions)\n", name, stratName, len(result.Conflicts))
			conflicts++
		}
	}

	fmt.Printf("\n%d files merged, %d conflicts\n", merged, conflicts)
	return nil
}

func fileMap(files []manifest.FileEntry) map[string]string {
	m := make(map[string]string)
	for _, f := range files {
		m[f.Name] = f.UUID
	}
	return m
}

func loadBlobByUUID(r *repo.Repo, uuid string) []byte {
	if uuid == "" {
		return nil
	}
	rid, ok := blob.Exists(r.DB(), uuid)
	if !ok {
		return nil
	}
	data, err := content.Expand(r.DB(), rid)
	if err != nil {
		return nil
	}
	return data
}
