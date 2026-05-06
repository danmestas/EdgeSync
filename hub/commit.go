package hub

import (
	"context"
	"time"
)

// RevID is an opaque commit handle.
type RevID string

// FileToCommit names a file and its content for inclusion in a commit.
type FileToCommit struct {
	Name    string
	Content []byte

	// Perm is the libfossil-style file permission marker. The empty string
	// means default (regular file); "x" marks the file executable so the
	// resulting manifest's F-card carries the executable bit. Other libfossil
	// perm markers (e.g. "l" for symlinks) are passed through verbatim.
	Perm string
}

// CommitOpts configures Hub.Commit / Repo.Commit.
type CommitOpts struct {
	Files   []FileToCommit
	Message string
	Author  string // fossil user; required

	// Time is the commit timestamp. The zero value defaults to time.Now().UTC()
	// at commit time; callers that need reproducible seeded commits (e.g. a
	// production daemon bootstrapping a hub repo from on-disk content) should
	// pass an explicit value.
	Time time.Time

	// Tags, when non-empty, are attached to the resulting checkin manifest.
	// Tags are libfossil's primitive — fossil "branches" are themselves
	// propagating "branch=<name>" + "sym-<name>" tag pairs. To land a commit
	// on branch "agent/abc123":
	//
	//   Tags: []TagSpec{
	//       {Name: "branch",            Value: "agent/abc123"},
	//       {Name: "sym-agent/abc123",  Value: "*"},
	//   }
	//
	// Empty preserves current behavior (commit to current branch, no extra tags).
	Tags []TagSpec
}

// TagSpec describes a tag attached at commit time. Mirrors libfossil's
// underlying tag primitive; pass these via CommitOpts.Tags to attach
// arbitrary tags (including the propagating branch-tag pair that creates
// or advances a fossil branch) on the resulting checkin.
type TagSpec struct {
	Name  string
	Value string
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
