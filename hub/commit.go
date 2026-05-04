package hub

import "context"

// RevID is an opaque commit handle.
type RevID string

// FileToCommit names a file and its content for inclusion in a commit.
type FileToCommit struct {
	Name    string
	Content []byte
}

// CommitOpts configures Hub.Commit / Repo.Commit.
type CommitOpts struct {
	Files   []FileToCommit
	Message string
	Author  string // fossil user; required
}

// Commit commits a set of files to the hub repo.
func (h *Hub) Commit(ctx context.Context, opts CommitOpts) (RevID, error) {
	return h.repo.Commit(ctx, opts)
}

// Read returns the contents of path at the current trunk tip.
func (h *Hub) Read(ctx context.Context, path string) ([]byte, error) {
	return h.repo.Read(ctx, path)
}

// ReadAt returns the contents of path at rev. rev may be a branch name
// (e.g. "trunk"), a tag, or a RevID returned from Commit.
func (h *Hub) ReadAt(ctx context.Context, rev RevID, path string) ([]byte, error) {
	return h.repo.ReadAt(ctx, rev, path)
}
