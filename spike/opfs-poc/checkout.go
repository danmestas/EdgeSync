//go:build js

package main

import (
	"encoding/json"
	"fmt"
	"sort"
	"strings"
	"time"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/hash"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

type Checkout struct {
	repo   *repo.Repo
	tipRID libfossil.FslID
}

type DirEntry struct {
	Name  string `json:"name"`
	IsDir bool   `json:"isDir"`
	Size  int    `json:"size"`
}

type FileChange struct {
	Name   string `json:"name"`
	Status string `json:"status"` // "modified", "added", "deleted"
}

func NewCheckout(r *repo.Repo, tip libfossil.FslID) *Checkout {
	if r == nil {
		panic("NewCheckout: repo must not be nil")
	}
	if tip <= 0 {
		panic("NewCheckout: tip must be positive")
	}
	return &Checkout{repo: r, tipRID: tip}
}

// Materialize extracts all files from the tip checkin to OPFS.
func (co *Checkout) Materialize() (int, error) {
	log("materialize: clearing old checkout...")
	if _, err := coCall("_opfs_co_clear"); err != nil {
		return 0, fmt.Errorf("clear: %w", err)
	}
	log("materialize: cleared. listing files...")

	files, err := manifest.ListFiles(co.repo, co.tipRID)
	if err != nil {
		return 0, fmt.Errorf("list files: %w", err)
	}
	log(fmt.Sprintf("materialize: %d files to write", len(files)))

	for _, f := range files {
		fileRid, ok := blob.Exists(co.repo.DB(), f.UUID)
		if !ok {
			return 0, fmt.Errorf("blob not found: %s", f.UUID)
		}
		data, err := content.Expand(co.repo.DB(), fileRid)
		if err != nil {
			return 0, fmt.Errorf("expand %s: %w", f.Name, err)
		}
		log(fmt.Sprintf("materialize: writing %s (%d bytes)...", f.Name, len(data)))
		if _, err := coCall("_opfs_co_write", f.Name, toJSUint8Array(data)); err != nil {
			return 0, fmt.Errorf("write %s: %w", f.Name, err)
		}
	}

	meta := fmt.Sprintf("%d", co.tipRID)
	if _, err := coCall("_opfs_co_write", ".fossil-checkout", toJSUint8Array([]byte(meta))); err != nil {
		return 0, fmt.Errorf("write metadata: %w", err)
	}

	return len(files), nil
}

// ListDir lists entries in an OPFS directory.
func (co *Checkout) ListDir(path string) ([]DirEntry, error) {
	data, err := coCall("_opfs_co_list", path)
	if err != nil {
		return nil, err
	}
	var entries []DirEntry
	if err := json.Unmarshal([]byte(data), &entries); err != nil {
		return nil, fmt.Errorf("parse list: %w", err)
	}
	sort.Slice(entries, func(i, j int) bool {
		if entries[i].IsDir != entries[j].IsDir {
			return entries[i].IsDir
		}
		return entries[i].Name < entries[j].Name
	})
	return entries, nil
}

// ReadFile reads a file from OPFS.
func (co *Checkout) ReadFile(path string) (string, error) {
	data, err := coCall("_opfs_co_read", path)
	if err != nil {
		return "", err
	}
	return data, nil
}

// WriteFile writes content to an OPFS file.
func (co *Checkout) WriteFile(path, fileContent string) error {
	_, err := coCall("_opfs_co_write", path, toJSUint8Array([]byte(fileContent)))
	return err
}

// DeleteFile removes a file from OPFS.
func (co *Checkout) DeleteFile(path string) error {
	_, err := coCall("_opfs_co_delete", path)
	return err
}

// walkTree recursively lists all files in the OPFS checkout.
func (co *Checkout) walkTree(prefix string) (map[string]int, error) {
	entries, err := co.ListDir(prefix)
	if err != nil {
		return nil, err
	}
	files := make(map[string]int)
	for _, e := range entries {
		path := e.Name
		if prefix != "" {
			path = prefix + "/" + e.Name
		}
		if e.IsDir {
			sub, err := co.walkTree(path)
			if err != nil {
				return nil, err
			}
			for k, v := range sub {
				files[k] = v
			}
		} else {
			files[path] = e.Size
		}
	}
	return files, nil
}

// Status compares the OPFS working tree against the tip manifest.
func (co *Checkout) Status() ([]FileChange, error) {
	tipFiles, err := manifest.ListFiles(co.repo, co.tipRID)
	if err != nil {
		return nil, fmt.Errorf("list tip files: %w", err)
	}
	tipMap := make(map[string]string, len(tipFiles))
	for _, f := range tipFiles {
		tipMap[f.Name] = f.UUID
	}

	opfsFiles, err := co.walkTree("")
	if err != nil {
		return nil, fmt.Errorf("walk tree: %w", err)
	}

	var changes []FileChange

	for name := range opfsFiles {
		uuid, inManifest := tipMap[name]
		if !inManifest {
			changes = append(changes, FileChange{Name: name, Status: "added"})
			continue
		}
		fileContent, err := co.ReadFile(name)
		if err != nil {
			// File exists in OPFS listing but can't be read — treat as modified
			// since we can't verify content matches the manifest.
			changes = append(changes, FileChange{Name: name, Status: "modified"})
			continue
		}
		computed := hash.ContentHash([]byte(fileContent), uuid)
		if computed != uuid {
			changes = append(changes, FileChange{Name: name, Status: "modified"})
		}
	}

	for name := range tipMap {
		if _, inOPFS := opfsFiles[name]; !inOPFS {
			changes = append(changes, FileChange{Name: name, Status: "deleted"})
		}
	}

	sort.Slice(changes, func(i, j int) bool { return changes[i].Name < changes[j].Name })
	return changes, nil
}

// CommitAll reads all files from OPFS and creates a Fossil checkin.
func (co *Checkout) CommitAll(comment, user string) (libfossil.FslID, string, error) {
	changes, err := co.Status()
	if err != nil {
		return 0, "", fmt.Errorf("status: %w", err)
	}
	if len(changes) == 0 {
		return 0, "", fmt.Errorf("nothing to commit")
	}

	opfsFiles, err := co.walkTree("")
	if err != nil {
		return 0, "", fmt.Errorf("walk: %w", err)
	}

	var names []string
	for name := range opfsFiles {
		names = append(names, name)
	}
	sort.Strings(names)

	var commitFiles []manifest.File
	for _, name := range names {
		data, err := co.ReadFile(name)
		if err != nil {
			return 0, "", fmt.Errorf("read %s: %w", name, err)
		}
		content := []byte(data)
		if len(content) == 0 {
			// blob.Store requires non-empty content. Use a single newline
			// for empty files (matches Fossil behavior for empty files).
			content = []byte("\n")
		}
		commitFiles = append(commitFiles, manifest.File{
			Name:    name,
			Content: content,
		})
	}

	if user == "" {
		user = "browser"
	}
	if comment == "" {
		comment = "edit from browser"
	}

	rid, uuid, err := manifest.Checkin(co.repo, manifest.CheckinOpts{
		Files:   commitFiles,
		Comment: comment,
		User:    user,
		Parent:  co.tipRID,
		Time:    time.Now().UTC(),
	})
	if err != nil {
		return 0, "", fmt.Errorf("checkin: %w", err)
	}

	co.tipRID = rid
	meta := fmt.Sprintf("%d", rid)
	if _, writeErr := coCall("_opfs_co_write", ".fossil-checkout", toJSUint8Array([]byte(meta))); writeErr != nil {
		log(fmt.Sprintf("warning: failed to update .fossil-checkout: %v", writeErr))
	}

	return rid, uuid, nil
}

// HasCheckout checks if a checkout exists in OPFS.
func HasCheckout() bool {
	data, err := coCall("_opfs_co_stat", ".fossil-checkout")
	if err != nil {
		return false
	}
	var stat struct {
		Exists bool `json:"exists"`
	}
	if err := json.Unmarshal([]byte(data), &stat); err != nil {
		return false
	}
	return stat.Exists
}

// ReadCheckoutTipRID reads the stored tip RID from OPFS metadata.
func ReadCheckoutTipRID() (libfossil.FslID, error) {
	data, err := coCall("_opfs_co_read", ".fossil-checkout")
	if err != nil {
		return 0, err
	}
	var rid int64
	n, _ := fmt.Sscanf(strings.TrimSpace(data), "%d", &rid)
	if n != 1 || rid <= 0 {
		return 0, fmt.Errorf("invalid tip RID in .fossil-checkout")
	}
	return libfossil.FslID(rid), nil
}
