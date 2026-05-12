package hub

import (
	"context"
	"errors"
	"fmt"
	"log"
	"sync"

	libfossil "github.com/danmestas/libfossil"
)

// Repo is a hub fossil repo opened for direct operations — user mgmt,
// commits, reads. No embedded NATS server, no HTTP listener; it's the
// right handle for one-off CLI invocations like `<binary> hub user list`
// where spinning up the full hub daemon would be wasteful.
//
// For the daemon use case (running hub serving leaves), construct a
// *Hub via NewHub. A *Hub holds a *Repo internally; access it via
// Hub.Repo if you need to share the handle with another component
// in the same process. Don't call Close on a *Repo returned by
// Hub.Repo while the Hub is still serving — Hub.Stop is the right
// teardown for that case.
type Repo struct {
	handle  *libfossil.Repo
	closeMu sync.Mutex
	closed  bool

	// publish, when non-nil, is invoked after a successful Commit with the
	// new commit's rid and uuid. NewHub wires this to a NATS publisher on
	// "<prefix>.<project-code>.commit" so peer hubs can pull the new
	// commit. Standalone OpenRepo callers leave it nil (no publish).
	//
	// Implementations MUST be non-blocking — Commit calls publish on the
	// foreground path and a slow publisher would back-pressure writers.
	// Best-effort by contract: publish failures must not fail the commit.
	publish func(rid int64, uuid string)

	// publishMu serializes publishNewCommits so concurrent callers (the
	// local Commit path and the HTTP xfer-push wrapper) cannot interleave
	// scans of the event table and race the publishedRID watermark.
	publishMu sync.Mutex

	// publishedRID is the highest 'ci' event rid that has been emitted on
	// the .commit subject. publishNewCommits only emits for rids strictly
	// greater than this, then advances it. Initialised at hub startup
	// (in startCommitSubscriber) to the current max ci rid so any commits
	// that landed via SeedFromUpstream are not republished.
	publishedRID int64
}

// OpenRepo opens an existing hub repo at path. Returns an error if the
// path doesn't exist or isn't a valid fossil repo. The caller owns the
// returned *Repo and must Close it when done.
func OpenRepo(path string) (*Repo, error) {
	if path == "" {
		return nil, errors.New("hub: OpenRepo: path is required")
	}
	h, err := libfossil.Open(path)
	if err != nil {
		return nil, fmt.Errorf("hub: open repo at %s: %w", path, err)
	}
	applySQLiteTuning(h)
	return &Repo{handle: h}, nil
}

// newRepoFromHandle wraps an already-opened libfossil handle as a *Repo.
// Used internally by NewHub so the Hub and direct Repo callers share
// method bodies.
func newRepoFromHandle(h *libfossil.Repo) *Repo {
	return &Repo{handle: h}
}

// Close releases the underlying libfossil handle. Idempotent — second
// and subsequent calls return nil.
func (r *Repo) Close() error {
	r.closeMu.Lock()
	defer r.closeMu.Unlock()
	if r.closed {
		return nil
	}
	r.closed = true
	if err := r.handle.Close(); err != nil {
		return fmt.Errorf("hub: close repo: %w", err)
	}
	return nil
}

// AddUser creates a fossil user. Errors if Login is empty or the user
// already exists.
func (r *Repo) AddUser(u User) error {
	if u.Login == "" {
		return errors.New("hub: AddUser: Login is required")
	}
	if err := r.handle.CreateUser(libfossil.UserOpts{Login: u.Login, Caps: u.Caps}); err != nil {
		return fmt.Errorf("hub: add user %q: %w", u.Login, err)
	}
	return nil
}

// GetUser returns the user with the given login. Errors if no such user
// exists.
func (r *Repo) GetUser(login string) (User, error) {
	u, err := r.handle.GetUser(login)
	if err != nil {
		return User{}, fmt.Errorf("hub: get user %q: %w", login, err)
	}
	return User{Login: u.Login, Caps: u.Caps}, nil
}

// HasUser reports whether a user with the given login exists.
func (r *Repo) HasUser(login string) bool {
	_, err := r.handle.GetUser(login)
	return err == nil
}

// ListUsers returns all users on the hub repo.
func (r *Repo) ListUsers() ([]User, error) {
	users, err := r.handle.ListUsers()
	if err != nil {
		return nil, fmt.Errorf("hub: list users: %w", err)
	}
	out := make([]User, len(users))
	for i, u := range users {
		out[i] = User{Login: u.Login, Caps: u.Caps}
	}
	return out, nil
}

// RemoveUser deletes the user with the given login.
func (r *Repo) RemoveUser(login string) error {
	if err := r.handle.DeleteUser(login); err != nil {
		return fmt.Errorf("hub: remove user %q: %w", login, err)
	}
	return nil
}

// SetUserCaps replaces the caps string on an existing user. Errors if the
// user doesn't exist or the underlying SetCaps fails.
func (r *Repo) SetUserCaps(login, caps string) error {
	if login == "" {
		return errors.New("hub: SetUserCaps: login is required")
	}
	if err := r.handle.SetCaps(login, caps); err != nil {
		return fmt.Errorf("hub: set caps for %q: %w", login, err)
	}
	return nil
}

// Commit commits a set of files to the hub repo with the given message
// and author. Returns the new manifest UUID as an opaque RevID.
func (r *Repo) Commit(ctx context.Context, opts CommitOpts) (RevID, error) {
	if opts.Author == "" {
		return "", errors.New("hub: Commit: Author is required")
	}
	files := make([]libfossil.FileToCommit, len(opts.Files))
	for i, f := range opts.Files {
		files[i] = libfossil.FileToCommit{Name: f.Name, Content: f.Content, Perm: f.Perm}
	}
	var tags []libfossil.TagSpec
	if len(opts.Tags) > 0 {
		tags = make([]libfossil.TagSpec, len(opts.Tags))
		for i, t := range opts.Tags {
			tags[i] = libfossil.TagSpec{Name: t.Name, Value: t.Value}
		}
	}
	_, uuid, err := r.handle.Commit(libfossil.CommitOpts{
		Files:   files,
		Comment: opts.Message,
		User:    opts.Author,
		Time:    opts.Time,
		Tags:    tags,
	})
	if err != nil {
		return "", fmt.Errorf("hub: commit: %w", err)
	}
	r.publishNewCommits()
	return RevID(uuid), nil
}

// publishNewCommits invokes r.publish for every 'ci' event whose rid is
// strictly greater than r.publishedRID, in rid order, then advances the
// watermark. No-op when r.publish is nil (standalone OpenRepo, or hub
// constructed with Config.DisableFossilSyncOverNATS=true).
//
// Both code paths that can land a new commit funnel through here:
//
//   - Repo.Commit (local, in-process commits): calls this after libfossil
//     has written the new event; the watermark scan picks up exactly the
//     one rid that just landed.
//   - The HTTP xfer-push wrapper installed by Hub.ServeHTTP: calls this
//     after every xfer request so commits arriving via `fossil push
//     <hub-http>` from an external CLI agent emit the same .commit
//     notification a Repo.Commit caller would have. Without that wrapper,
//     HTTP-pushed commits land in the repo but peers never get notified
//     (issue #160). A single multi-commit push emits one notification per
//     new ci event so each lands on the subject in rid order.
//
// publishMu serialises the scan/publish/advance triple so concurrent
// callers cannot republish the same rid. Publish errors are logged inside
// the publish hook (see startCommitSubscriber) and never bubble up — both
// call sites treat publish as best-effort.
func (r *Repo) publishNewCommits() {
	r.publishMu.Lock()
	defer r.publishMu.Unlock()
	if r.publish == nil {
		return
	}
	rows, err := r.handle.DB().Query(
		"SELECT objid FROM event WHERE type='ci' AND objid > ? ORDER BY objid",
		r.publishedRID,
	)
	if err != nil {
		log.Printf("hub publish: enumerate new commits since rid=%d: %v", r.publishedRID, err)
		return
	}
	var newRIDs []int64
	for rows.Next() {
		var rid int64
		if scanErr := rows.Scan(&rid); scanErr != nil {
			_ = rows.Close()
			log.Printf("hub publish: scan rid: %v", scanErr)
			return
		}
		newRIDs = append(newRIDs, rid)
	}
	_ = rows.Close()
	if err := rows.Err(); err != nil {
		log.Printf("hub publish: enumerate new commits since rid=%d: rows error: %v", r.publishedRID, err)
		return
	}
	for _, rid := range newRIDs {
		uuid, err := r.handle.UUIDFromRID(rid)
		if err != nil {
			log.Printf("hub publish: uuid for rid=%d: %v", rid, err)
			continue
		}
		r.publish(rid, uuid)
		r.publishedRID = rid
	}
}

// initPublishWatermark snapshots the current max 'ci' event rid into
// publishedRID. NewHub calls this in startCommitSubscriber immediately
// after wiring r.publish — that's the boundary at which any commits
// already in the repo (e.g. from SeedFromUpstream's clone) are considered
// "already known to peers" by definition, and future publishNewCommits
// scans must not re-emit them.
//
// Best-effort: a query error leaves publishedRID at 0, which would cause
// the first publishNewCommits call to (harmlessly) emit notifications for
// every commit in the repo. Logged so the divergence is visible.
func (r *Repo) initPublishWatermark() {
	r.publishMu.Lock()
	defer r.publishMu.Unlock()
	var maxRID int64
	err := r.handle.DB().QueryRow(
		"SELECT COALESCE(MAX(objid), 0) FROM event WHERE type='ci'",
	).Scan(&maxRID)
	if err != nil {
		log.Printf("hub publish: init watermark: %v", err)
		return
	}
	r.publishedRID = maxRID
}

// Read returns the contents of path at the current trunk tip.
func (r *Repo) Read(ctx context.Context, path string) ([]byte, error) {
	return r.handle.ReadFileAt("trunk", path)
}

// ReadAt returns the contents of path at rev. rev may be a branch name
// (e.g. "trunk"), a tag, or a RevID returned from Commit.
func (r *Repo) ReadAt(ctx context.Context, rev RevID, path string) ([]byte, error) {
	if rev == "" {
		return nil, errors.New("hub: ReadAt: rev is required (use Read for current tip)")
	}
	return r.handle.ReadFileAt(string(rev), path)
}
