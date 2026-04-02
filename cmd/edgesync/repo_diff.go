package main

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/danmestas/go-libfossil/blob"
	"github.com/danmestas/go-libfossil/content"
	"github.com/danmestas/go-libfossil/manifest"
	"github.com/danmestas/go-libfossil/repo"
	"github.com/hexops/gotextdiff"
	"github.com/hexops/gotextdiff/myers"
	"github.com/hexops/gotextdiff/span"
)

type RepoDiffCmd struct {
	Version string `arg:"" optional:"" help:"Version to diff against (default: tip)"`
	Dir     string `short:"d" help:"Working directory to compare" default:"."`
	Unified int    `short:"U" help:"Lines of context" default:"3"`
}

func (c *RepoDiffCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	rid, err := resolveRID(r, c.Version)
	if err != nil {
		return err
	}

	files, err := manifest.ListFiles(r, rid)
	if err != nil {
		return err
	}

	hasDiff := false
	for _, f := range files {
		diskPath := filepath.Join(c.Dir, f.Name)
		diskData, err := os.ReadFile(diskPath)
		if err != nil {
			if os.IsNotExist(err) {
				fmt.Printf("--- a/%s\n+++ /dev/null\n@@ deleted @@\n", f.Name)
				hasDiff = true
				continue
			}
			return err
		}

		repoContent, err := expandFile(r, f.UUID)
		if err != nil {
			return fmt.Errorf("%s: %w", f.Name, err)
		}

		repoStr := string(repoContent)
		diskStr := string(diskData)

		if repoStr == diskStr {
			continue
		}

		edits := myers.ComputeEdits(span.URIFromPath(f.Name), repoStr, diskStr)
		diff := fmt.Sprint(gotextdiff.ToUnified("a/"+f.Name, "b/"+f.Name, repoStr, edits))
		if diff != "" {
			fmt.Print(diff)
			hasDiff = true
		}
	}

	if !hasDiff {
		fmt.Println("no changes")
	}
	return nil
}

func expandFile(r *repo.Repo, uuid string) ([]byte, error) {
	rid, ok := blob.Exists(r.DB(), uuid)
	if !ok {
		return nil, fmt.Errorf("blob %s not found", uuid)
	}
	return content.Expand(r.DB(), rid)
}
