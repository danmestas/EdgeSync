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
func (a *Agent) Commit(ctx context.Context, opts CommitOpts) (RevID, error) {
	if opts.Author == "" {
		return "", errors.New("agent: Commit: Author is required")
	}
	files := make([]libfossil.FileToCommit, len(opts.Files))
	for i, f := range opts.Files {
		files[i] = libfossil.FileToCommit{Name: f.Name, Content: f.Content}
	}
	_, uuid, err := a.repo.Commit(libfossil.CommitOpts{
		Files:   files,
		Comment: opts.Message,
		User:    opts.Author,
	})
	if err != nil {
		return "", fmt.Errorf("agent: commit: %w", err)
	}
	return RevID(uuid), nil
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
	filesA, err := a.repo.ListFiles(ridA)
	if err != nil {
		return nil, fmt.Errorf("agent: diff: list files at %q: %w", revA, err)
	}
	filesB, err := a.repo.ListFiles(ridB)
	if err != nil {
		return nil, fmt.Errorf("agent: diff: list files at %q: %w", revB, err)
	}
	uuidByName := make(map[string]string, len(filesA)+len(filesB))
	for _, f := range filesA {
		uuidByName[f.Name] = f.UUID
	}
	names := make([]string, 0, len(uuidByName)+len(filesB))
	seen := make(map[string]bool, len(uuidByName)+len(filesB))
	for _, f := range filesA {
		if !seen[f.Name] {
			names = append(names, f.Name)
			seen[f.Name] = true
		}
	}
	for _, f := range filesB {
		if changed := uuidByName[f.Name] != f.UUID; changed && !seen[f.Name] {
			names = append(names, f.Name)
			seen[f.Name] = true
		}
	}
	var out strings.Builder
	for _, name := range names {
		entries, derr := a.repo.Diff(ridA, ridB, name)
		if derr != nil {
			return nil, fmt.Errorf("agent: diff %q: %w", name, derr)
		}
		for _, e := range entries {
			out.WriteString(e.Unified)
		}
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
	uuid, err := ridToUUID(a.repo, rid)
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
		if uuid, uerr := ridToUUID(a.repo, forks[0].LocalTip); uerr == nil {
			out.BranchID = uuid
		}
	}
	return out
}

func ridToUUID(r *libfossil.Repo, rid int64) (string, error) {
	var uuid string
	if err := r.DB().QueryRow(`SELECT uuid FROM blob WHERE rid = ?`, rid).Scan(&uuid); err != nil {
		return "", fmt.Errorf("rid→uuid: %w", err)
	}
	return uuid, nil
}

// applySQLiteTuning applies the per-connection SQLite settings the agent
// needs for safe concurrent writes against its repo. libfossil PRAGMAs are
// per-connection, so we cap MaxOpenConns at 1 to make them stick.
func applySQLiteTuning(r *libfossil.Repo) {
	r.DB().SqlDB().SetMaxOpenConns(1)
	_, _ = r.DB().Exec(`PRAGMA busy_timeout = 30000`)
}
