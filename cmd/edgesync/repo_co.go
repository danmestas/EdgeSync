package main

import (
	"database/sql"
	"fmt"
	"os"
	"path/filepath"

	libfossil "github.com/danmestas/go-libfossil"
	"github.com/danmestas/go-libfossil/blob"
	"github.com/danmestas/go-libfossil/content"
	"github.com/danmestas/go-libfossil/manifest"
	"github.com/danmestas/go-libfossil/repo"
	"github.com/danmestas/go-libfossil/undo"
)

type RepoCoCmd struct {
	Version string `arg:"" optional:"" help:"Version to checkout (default: tip)"`
	Dir     string `short:"d" help:"Output directory (default: current dir)" default:"."`
	Force   bool   `help:"Overwrite existing files"`
}

func (c *RepoCoCmd) Run(g *Globals) error {
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

	for _, f := range files {
		fileRid, ok := blob.Exists(r.DB(), f.UUID)
		if !ok {
			return fmt.Errorf("blob %s not found for file %s", f.UUID, f.Name)
		}
		data, err := content.Expand(r.DB(), fileRid)
		if err != nil {
			return fmt.Errorf("expanding %s: %w", f.Name, err)
		}

		outPath := filepath.Join(c.Dir, f.Name)
		if err := os.MkdirAll(filepath.Dir(outPath), 0o755); err != nil {
			return err
		}

		if !c.Force {
			if _, err := os.Stat(outPath); err == nil {
				return fmt.Errorf("file exists: %s (use --force to overwrite)", outPath)
			}
		}

		perm := os.FileMode(0o644)
		if f.Perm == "x" {
			perm = 0o755
		}
		if err := os.WriteFile(outPath, data, perm); err != nil {
			return err
		}

		fmt.Printf("  %s\n", f.Name)
	}

	fmt.Printf("checked out %d files\n", len(files))

	// Update checkout DB if one exists.
	ckout, err := openCheckout(c.Dir)
	if err == nil {
		defer ckout.Close()
		if err := undo.Save(ckout, c.Dir, nil); err != nil {
			fmt.Fprintf(os.Stderr, "warning: undo save: %v\n", err)
		}
		var uuid string
		r.DB().QueryRow("SELECT uuid FROM blob WHERE rid=?", rid).Scan(&uuid)

		if err := updateCheckoutDB(ckout, r, rid, uuid, files); err != nil {
			return fmt.Errorf("updating checkout DB: %w", err)
		}
	}

	return nil
}

// updateCheckoutDB replaces vfile rows and updates vvar to reflect a new checked-out version.
func updateCheckoutDB(ckout *sql.DB, r *repo.Repo, rid libfossil.FslID, uuid string, files []manifest.FileEntry) error {
	tx, err := ckout.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()

	// Clear existing vfile rows.
	if _, err := tx.Exec("DELETE FROM vfile"); err != nil {
		return err
	}

	// Insert new vfile rows.
	for _, f := range files {
		isExe := f.Perm == "x"
		var fileRid int64
		r.DB().QueryRow("SELECT rid FROM blob WHERE uuid=?", f.UUID).Scan(&fileRid)
		if _, err := tx.Exec(
			`INSERT INTO vfile(vid, chnged, deleted, isexe, islink, rid, mrid, pathname, mhash)
			 VALUES(?, 0, 0, ?, 0, ?, ?, ?, ?)`,
			rid, isExe, fileRid, fileRid, f.Name, f.UUID,
		); err != nil {
			return err
		}
	}

	// Update vvar checkout and checkout-hash.
	if _, err := tx.Exec("UPDATE vvar SET value=? WHERE name='checkout'", fmt.Sprintf("%d", rid)); err != nil {
		return err
	}
	if _, err := tx.Exec("UPDATE vvar SET value=? WHERE name='checkout-hash'", uuid); err != nil {
		return err
	}

	return tx.Commit()
}
