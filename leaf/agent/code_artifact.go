package agent

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"strings"

	libfossil "github.com/danmestas/libfossil"
)

// RevID is an opaque commit handle. Treat as opaque; do not parse.
type RevID string

// FileToCommit names a file and its content for inclusion in a commit.
type FileToCommit struct {
	Name    string
	Content []byte
}

// CommitOpts configures Agent.Commit.
type CommitOpts struct {
	Files   []FileToCommit
	Message string
	Author  string // fossil user; required

	// ParentID is the primary parent rid for the new commit. When 0,
	// Agent.Commit resolves the current trunk tip and uses that — the
	// natural "chain onto trunk" behavior matching `fossil ci`. On a
	// fresh repo with no trunk tip, the resolution falls through to 0
	// and the commit is a legitimate orphan root (the first commit).
	// Set explicitly to chain onto a non-trunk branch via Agent.Tip.
	ParentID int64

	// MergeParents lists additional parents for a merge commit. Each
	// contributes a secondary entry on the manifest's P-card.
	MergeParents []int64

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

// SyncOpts configures Agent.SyncTo.
type SyncOpts struct {
	Push        bool
	Pull        bool
	User        string
	Password    string
	ProjectCode string // optional; auto-derived from agent's repo if empty
}

// SyncResult summarizes a sync session.
type SyncResult struct {
	Pushed   int
	Pulled   int
	Forked   bool
	BranchID string // local fork tip's UUID; empty if no fork
}

// Commit commits a set of files to the agent's repo with the given message
// and author. Returns the new manifest UUID as an opaque RevID.
//
// opts.Files is interpreted with `fossil ci` semantics — "files this
// commit changes" — and is merged on top of the parent commit's tracked
// files (supplied wins by name). Files at the parent that are not in
// opts.Files are carried forward unchanged. Without that merge,
// libfossil.Commit would emit a partial manifest containing only the
// supplied subset, silently dropping every other file the parent tracked
// (#152, chains to libfossil#30). There is no delete-file API today;
// once libfossil grows one, plumb it through CommitOpts.
//
// When opts.ParentID is 0, the current trunk tip is resolved and used as
// the parent — matching `fossil ci`'s default chain-onto-tip behavior. On
// an empty repo (no trunk tip yet), the resolution returns 0 and the
// commit is a legitimate orphan root with no parent files to inherit.
func (a *Agent) Commit(ctx context.Context, opts CommitOpts) (RevID, error) {
	if opts.Author == "" {
		return "", errors.New("agent: Commit: Author is required")
	}
	parentID, err := a.resolveCommitParent(opts.ParentID)
	if err != nil {
		return "", err
	}
	files, err := a.mergeWithParent(parentID, opts.Files)
	if err != nil {
		return "", err
	}
	var tags []libfossil.TagSpec
	if len(opts.Tags) > 0 {
		tags = make([]libfossil.TagSpec, len(opts.Tags))
		for i, t := range opts.Tags {
			tags[i] = libfossil.TagSpec{Name: t.Name, Value: t.Value}
		}
	}
	_, uuid, err := a.repo.Commit(libfossil.CommitOpts{
		Files:        files,
		Comment:      opts.Message,
		User:         opts.Author,
		ParentID:     parentID,
		MergeParents: opts.MergeParents,
		Tags:         tags,
	})
	if err != nil {
		return "", fmt.Errorf("agent: commit: %w", err)
	}
	return RevID(uuid), nil
}

// mergeWithParent computes the full F-card set for a new commit. Starts
// from the parent commit's tracked files (preserving each file's Perm),
// then overlays the supplied opts.Files — supplied wins by name. Parent
// files not overridden are carried forward by reading their content out
// of the parent checkin.
//
// parentID == 0 is the orphan-root case (first commit in a fresh repo);
// nothing to inherit, so the result is just opts.Files in the libfossil
// shape.
//
// Reading each preserved file out of the parent is O(parent file count)
// per commit. Acceptable for the workloads agent.Commit targets; if
// large-tree repos surface as a hotspot, a future libfossil API that
// emits a manifest by referencing parent F-card UUIDs without rehydrating
// the content would be the right place to optimise — agent.Commit can
// stay shaped the way it is.
func (a *Agent) mergeWithParent(parentID int64, supplied []FileToCommit) ([]libfossil.FileToCommit, error) {
	suppliedByName := make(map[string]struct{}, len(supplied))
	for _, f := range supplied {
		suppliedByName[f.Name] = struct{}{}
	}

	var merged []libfossil.FileToCommit
	if parentID != 0 {
		parentFiles, err := a.repo.ListFiles(parentID)
		if err != nil {
			return nil, fmt.Errorf("agent: commit: list parent rid=%d files: %w", parentID, err)
		}
		merged = make([]libfossil.FileToCommit, 0, len(parentFiles)+len(supplied))
		for _, pf := range parentFiles {
			if _, overridden := suppliedByName[pf.Name]; overridden {
				continue
			}
			content, err := a.repo.ReadFile(parentID, pf.Name)
			if err != nil {
				return nil, fmt.Errorf("agent: commit: read parent file %q at rid=%d: %w", pf.Name, parentID, err)
			}
			merged = append(merged, libfossil.FileToCommit{
				Name:    pf.Name,
				Content: content,
				Perm:    pf.Perm,
			})
		}
	} else {
		merged = make([]libfossil.FileToCommit, 0, len(supplied))
	}
	for _, sf := range supplied {
		// agent.FileToCommit doesn't carry Perm — supplied files get
		// libfossil's default. Parent-inherited entries above preserve
		// the parent's Perm explicitly so file modes don't silently
		// regress across a commit that didn't touch them.
		merged = append(merged, libfossil.FileToCommit{
			Name:    sf.Name,
			Content: sf.Content,
		})
	}
	return merged, nil
}

// resolveCommitParent maps the caller-supplied ParentID to the rid the
// commit should chain onto. Non-zero is used verbatim. Zero auto-resolves
// trunk's tip; on an empty repo with no trunk tip yet, returns 0 (which
// libfossil treats as orphan root — correct first-commit behavior).
func (a *Agent) resolveCommitParent(supplied int64) (int64, error) {
	if supplied != 0 {
		return supplied, nil
	}
	rid, err := a.repo.BranchTip("trunk")
	if err != nil {
		if errors.Is(err, sql.ErrNoRows) || strings.Contains(err.Error(), "no rows in result set") {
			return 0, nil
		}
		return 0, fmt.Errorf("agent: commit: resolve trunk tip: %w", err)
	}
	return rid, nil
}

// Sync runs one sync round against the agent's configured upstream/transport.
// Returns nil result if no transport is configured.
func (a *Agent) Sync(ctx context.Context) (*SyncResult, error) {
	if a.transport == nil {
		return nil, errors.New("agent: Sync: no transport configured")
	}
	res, err := a.repo.Sync(ctx, a.transport, a.buildSyncOpts())
	if err != nil {
		return nil, fmt.Errorf("agent: sync: %w", err)
	}
	return a.toSyncResult(res), nil
}

// SyncTo runs one sync round against an explicit hub URL. The transport is
// constructed internally; callers do not see libfossil.Transport.
func (a *Agent) SyncTo(ctx context.Context, hubURL string, opts SyncOpts) (*SyncResult, error) {
	if hubURL == "" {
		return nil, errors.New("agent: SyncTo: hubURL is required")
	}
	transport := libfossil.NewHTTPTransport(hubURL)
	projectCode := opts.ProjectCode
	if projectCode == "" {
		projectCode = a.projectCode
	}
	res, err := a.repo.Sync(ctx, transport, libfossil.SyncOpts{
		Push:        opts.Push,
		Pull:        opts.Pull,
		ProjectCode: projectCode,
		User:        opts.User,
		Password:    opts.Password,
	})
	if err != nil {
		return nil, fmt.Errorf("agent: syncTo %s: %w", hubURL, err)
	}
	return a.toSyncResult(res), nil
}

// Read returns the contents of path at the current trunk tip.
func (a *Agent) Read(ctx context.Context, path string) ([]byte, error) {
	return a.repo.ReadFileAt("trunk", path)
}

// Files lists the file names at the current trunk tip. An empty repo (no
// checkins on trunk) returns (nil, nil).
func (a *Agent) Files(ctx context.Context) ([]string, error) {
	rid, err := a.repo.BranchTip("trunk")
	if err != nil {
		return nil, fmt.Errorf("agent: files: %w", err)
	}
	if rid == 0 {
		return nil, nil
	}
	entries, err := a.repo.ListFiles(rid)
	if err != nil {
		return nil, fmt.Errorf("agent: files: %w", err)
	}
	names := make([]string, len(entries))
	for i, e := range entries {
		names[i] = e.Name
	}
	return names, nil
}

// Diff returns the unified diff between revA and revB, concatenated across
// all changed files. Either rev may be a branch name, tag, or RevID.
func (a *Agent) Diff(ctx context.Context, revA, revB RevID) ([]byte, error) {
	ridA, err := a.repo.ResolveVersion(string(revA))
	if err != nil {
		return nil, fmt.Errorf("agent: diff: resolve %q: %w", revA, err)
	}
	ridB, err := a.repo.ResolveVersion(string(revB))
	if err != nil {
		return nil, fmt.Errorf("agent: diff: resolve %q: %w", revB, err)
	}
	entries, err := a.repo.Diff(ridA, ridB, "")
	if err != nil {
		return nil, fmt.Errorf("agent: diff: %w", err)
	}
	var out strings.Builder
	for _, e := range entries {
		out.WriteString(e.Unified)
	}
	return []byte(out.String()), nil
}

// Tip returns the manifest UUID at the tip of the named branch. An unknown
// or empty branch (no checkins) returns "" with nil error; an error
// indicates a substrate fault, not an empty branch.
func (a *Agent) Tip(ctx context.Context, branch string) (RevID, error) {
	rid, err := a.repo.BranchTip(branch)
	if err != nil {
		if errors.Is(err, sql.ErrNoRows) || strings.Contains(err.Error(), "no rows in result set") {
			return "", nil
		}
		return "", fmt.Errorf("agent: tip %q: %w", branch, err)
	}
	if rid == 0 {
		return "", nil
	}
	uuid, err := a.repo.UUIDFromRID(rid)
	if err != nil {
		return "", fmt.Errorf("agent: tip %q: %w", branch, err)
	}
	return RevID(uuid), nil
}

// ExtractTo populates dir with the files at rev. dir must already exist.
// rev may be a branch name, tag, or RevID returned from Commit. Empty rev
// is a no-op.
func (a *Agent) ExtractTo(ctx context.Context, dir string, rev RevID) error {
	if rev == "" {
		return nil
	}
	rid, err := a.repo.ResolveVersion(string(rev))
	if err != nil {
		return fmt.Errorf("agent: extractTo %q: resolve %q: %w", dir, rev, err)
	}
	co, err := a.repo.CreateCheckout(dir, libfossil.CheckoutCreateOpts{})
	if err != nil {
		return fmt.Errorf("agent: extractTo %q: create checkout: %w", dir, err)
	}
	defer co.Close()
	if err := co.Extract(rid, libfossil.ExtractOpts{}); err != nil {
		return fmt.Errorf("agent: extractTo %q: extract: %w", dir, err)
	}
	return nil
}

// Config reads a fossil config value (e.g. "project-code").
func (a *Agent) Config(key string) (string, error) {
	return a.repo.Config(key)
}

func (a *Agent) toSyncResult(r *libfossil.SyncResult) *SyncResult {
	out := &SyncResult{}
	if r != nil {
		out.Pushed = r.FilesSent
		out.Pulled = r.FilesRecvd
	}
	if forks, err := a.repo.DetectForks(); err == nil && len(forks) > 0 {
		out.Forked = true
		if uuid, uerr := a.repo.UUIDFromRID(forks[0].LocalTip); uerr == nil {
			out.BranchID = uuid
		}
	}
	return out
}

// applySQLiteTuning applies SQLite settings the agent needs for safe
// concurrent operation against its repo. The busy_timeout PRAGMA tells
// SQLite to retry on SQLITE_BUSY for up to 30s before giving up, which
// is what production callers want under contention.
//
// We do NOT cap MaxOpenConns at 1 here, even though libfossil PRAGMAs
// are per-connection. Capping the pool to 1 deadlocks libfossil's clone
// path, which has internal goroutines that all need a DB connection
// (issue #120). If PRAGMA stickiness across pooled connections matters
// later, route it through the modernc driver's per-connection init
// hook rather than a pool cap.
func applySQLiteTuning(r *libfossil.Repo) {
	_, _ = r.DB().Exec(`PRAGMA busy_timeout = 30000`)
}
