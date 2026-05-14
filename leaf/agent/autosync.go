package agent

import (
	"context"
	"errors"
	"fmt"

	libfossil "github.com/danmestas/libfossil"
)

// AutosyncMode controls the autosync behavior around commit.
type AutosyncMode int

const (
	AutosyncOff      AutosyncMode = iota // no sync, commit directly
	AutosyncOn                           // pull before, push after
	AutosyncPullOnly                     // pull before, no push after
)

var (
	// ErrWouldFork is returned when committing would create a fork.
	ErrWouldFork = errors.New("would fork: run update first, or use --allow-fork or --branch")

	// ErrAutosyncNotImplemented is retained as a sentinel for callers that
	// pre-date the v0.6.2 reimplementation.
	ErrAutosyncNotImplemented = errors.New("autosync commit not implemented")
)

// PushFailedError is returned when the local checkin succeeded but the
// subsequent push failed. LocalRID and LocalUUID identify the local commit
// the caller can choose to retry the push for, abandon, or surface to the
// user. Unwrap exposes the underlying sync error.
type PushFailedError struct {
	LocalRID  int64
	LocalUUID string
	Cause     error
}

func (e *PushFailedError) Error() string {
	return fmt.Sprintf("autosync: push (local checkin rid=%d uuid=%s succeeded): %v", e.LocalRID, e.LocalUUID, e.Cause)
}

func (e *PushFailedError) Unwrap() error { return e.Cause }

// AutosyncCommitOpts bundles the inputs to AutosyncCommit. The split between
// Commit (what to commit) and the surrounding fields (how to sync around it)
// mirrors libfossil's own Checkin/Sync split.
type AutosyncCommitOpts struct {
	Repo      *libfossil.Repo
	Checkout  *libfossil.Checkout
	Transport libfossil.Transport
	Mode      AutosyncMode
	Commit    libfossil.CheckoutCommitOpts

	// Sync carries the non-Push/Pull fields used during pull and push
	// (ProjectCode, User, Password, PeerID, Observer, etc.). Push and Pull
	// MUST be zero — AutosyncCommit drives them per Mode and rejects
	// caller-supplied values to avoid silent override.
	Sync libfossil.SyncOpts

	// AllowFork bypasses the fork check. An explicit branch in
	// Commit.Branch also bypasses it (committing onto a different branch
	// is not a fork by definition).
	AllowFork bool
}

// AutosyncCommit performs a checkin from opts.Checkout with autosync
// semantics around it:
//
//   - AutosyncOff      — checkin only, no network I/O, no fork check.
//   - AutosyncOn       — pull → fork check → checkin → push.
//   - AutosyncPullOnly — pull → fork check → checkin (no push).
//
// On fork detection, returns ErrWouldFork without creating a checkin,
// unless opts.AllowFork is true or opts.Commit.Branch is non-empty.
//
// If the local checkin succeeds but the subsequent push fails, the returned
// error is a *PushFailedError carrying the local commit's rid/uuid so the
// caller can decide whether to retry, abandon, or surface to the user.
func AutosyncCommit(ctx context.Context, opts AutosyncCommitOpts) (rid int64, uuid string, err error) {
	if opts.Repo == nil {
		return 0, "", errors.New("autosync: nil Repo")
	}
	if opts.Checkout == nil {
		return 0, "", errors.New("autosync: nil Checkout")
	}
	switch opts.Mode {
	case AutosyncOff, AutosyncOn, AutosyncPullOnly:
	default:
		return 0, "", fmt.Errorf("autosync: invalid Mode %d", opts.Mode)
	}
	if opts.Sync.Push || opts.Sync.Pull {
		return 0, "", errors.New("autosync: Sync.Push and Sync.Pull must be unset; AutosyncCommit drives them per Mode")
	}

	needsTransport := opts.Mode != AutosyncOff
	if needsTransport && opts.Transport == nil {
		return 0, "", errors.New("autosync: nil Transport for non-Off mode")
	}

	if opts.Mode == AutosyncOn || opts.Mode == AutosyncPullOnly {
		pullOpts := opts.Sync
		pullOpts.Pull = true
		if _, perr := opts.Repo.Sync(ctx, opts.Transport, pullOpts); perr != nil {
			return 0, "", fmt.Errorf("autosync: pull: %w", perr)
		}
	}

	skipForkCheck := opts.Mode == AutosyncOff || opts.AllowFork || opts.Commit.Branch != ""
	if !skipForkCheck {
		wouldFork, ferr := opts.Checkout.WouldFork()
		if ferr != nil {
			return 0, "", fmt.Errorf("autosync: WouldFork: %w", ferr)
		}
		if wouldFork {
			return 0, "", ErrWouldFork
		}
	}

	rid, uuid, err = opts.Checkout.Checkin(opts.Commit)
	if err != nil {
		return 0, "", fmt.Errorf("autosync: Checkin: %w", err)
	}

	if opts.Mode == AutosyncOn {
		pushOpts := opts.Sync
		pushOpts.Push = true
		if _, perr := opts.Repo.Sync(ctx, opts.Transport, pushOpts); perr != nil {
			return rid, uuid, &PushFailedError{LocalRID: rid, LocalUUID: uuid, Cause: perr}
		}
	}

	return rid, uuid, nil
}
