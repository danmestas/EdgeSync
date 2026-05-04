package hub

import (
	"context"
	"errors"
	"fmt"
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

// Commit commits a set of files to the hub repo with the given message
// and author. Returns the new manifest UUID as an opaque RevID.
func (r *Repo) Commit(ctx context.Context, opts CommitOpts) (RevID, error) {
	if opts.Author == "" {
		return "", errors.New("hub: Commit: Author is required")
	}
	files := make([]libfossil.FileToCommit, len(opts.Files))
	for i, f := range opts.Files {
		files[i] = libfossil.FileToCommit{Name: f.Name, Content: f.Content}
	}
	_, uuid, err := r.handle.Commit(libfossil.CommitOpts{
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
