package agent

// TODO(v0.2.0): AutosyncCommit requires checkout.Checkout which is now internal
// to go-libfossil. The public API needs a Checkout handle or the autosync workflow
// needs to be reimplemented using libfossil.Repo methods.
//
// The autosync workflow is: pull -> ci-lock -> fork-check -> commit -> push.
// This depends on checkout.Version(), checkout.WouldFork(), checkout.Commit(),
// and sync.Sync() with CkinLock support.
//
// For now, the core agent sync (push/pull) works via libfossil.Repo.Sync().
// Autosync commit is deferred until the public API exposes checkout operations.

import "errors"

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
	// ErrCkinLockHeld is returned when another client holds the check-in lock.
	ErrCkinLockHeld = errors.New("check-in lock held by another client")
	// ErrAutosyncNotImplemented is returned when autosync is attempted with v0.2.0.
	ErrAutosyncNotImplemented = errors.New("autosync commit not yet available with libfossil v0.2.0 handle API")
)
