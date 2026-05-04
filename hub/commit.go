package hub

import (
	"context"
	"errors"
	"fmt"

	libfossil "github.com/danmestas/libfossil"
)

// RevID is an opaque commit handle.
type RevID string

// FileToCommit names a file and its content for inclusion in a commit.
type FileToCommit struct {
	Name    string
	Content []byte
}

// CommitOpts configures Hub.Commit.
type CommitOpts struct {
	Files   []FileToCommit
	Message string
	Author  string // fossil user; required
}

// Commit commits a set of files to the hub repo.
func (h *Hub) Commit(ctx context.Context, opts CommitOpts) (RevID, error) {
	if opts.Author == "" {
		return "", errors.New("hub: Commit: Author is required")
	}
	files := make([]libfossil.FileToCommit, len(opts.Files))
	for i, f := range opts.Files {
		files[i] = libfossil.FileToCommit{Name: f.Name, Content: f.Content}
	}
	_, uuid, err := h.repo.Commit(libfossil.CommitOpts{
		Files:   files,
		Comment: opts.Message,
		User:    opts.Author,
	})
	if err != nil {
		return "", fmt.Errorf("hub: commit: %w", err)
	}
	return RevID(uuid), nil
}

// Read returns the contents of path at the current trunk tip.
func (h *Hub) Read(ctx context.Context, path string) ([]byte, error) {
	return h.repo.ReadFileAt("trunk", path)
}

// ReadAt returns the contents of path at rev. rev may be a branch name
// (e.g. "trunk"), a tag, or a RevID returned from Commit.
func (h *Hub) ReadAt(ctx context.Context, rev RevID, path string) ([]byte, error) {
	if rev == "" {
		return nil, errors.New("hub: ReadAt: rev is required (use Read for current tip)")
	}
	return h.repo.ReadFileAt(string(rev), path)
}
