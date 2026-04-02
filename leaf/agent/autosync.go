package agent

import (
	"context"
	"errors"
	"fmt"
	"log/slog"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/checkout"
	libsync "github.com/dmestas/edgesync/go-libfossil/sync"
)

// AutosyncMode controls the autosync behavior around commit.
type AutosyncMode int

const (
	AutosyncOff      AutosyncMode = iota // no sync, commit directly
	AutosyncOn                           // pull before, push after
	AutosyncPullOnly                     // pull before, no push after
)

// AutosyncOpts configures the AutosyncCommit workflow.
type AutosyncOpts struct {
	Mode      AutosyncMode
	Transport libsync.Transport
	SyncOpts  libsync.SyncOpts
	AllowFork bool   // skip fork check and ci-lock
	ClientID  string // identifies this agent for ci-lock
}

var (
	// ErrWouldFork is returned when committing would create a fork.
	ErrWouldFork = errors.New("would fork: run update first, or use --allow-fork or --branch")
	// ErrCkinLockHeld is returned when another client holds the check-in lock.
	ErrCkinLockHeld = errors.New("check-in lock held by another client")
)

// AutosyncCommit wraps checkout.Commit with Fossil's autosync workflow:
// pull → ci-lock → fork-check → commit → push.
func AutosyncCommit(ctx context.Context, co *checkout.Checkout,
	commitOpts checkout.CommitOpts, auto AutosyncOpts,
) (libfossil.FslID, string, error) {
	if auto.Mode == AutosyncOff {
		return co.Commit(commitOpts)
	}

	// --- pre-pull with optional ci-lock ---
	syncOpts := auto.SyncOpts
	syncOpts.Pull = true
	syncOpts.Push = false

	if !auto.AllowFork && commitOpts.Branch == "" {
		_, parentUUID, err := co.Version()
		if err != nil {
			return 0, "", fmt.Errorf("autosync: get version: %w", err)
		}
		syncOpts.CkinLock = &libsync.CkinLockReq{
			ParentUUID: parentUUID,
			ClientID:   auto.ClientID,
		}
	}

	result, err := libsync.Sync(ctx, co.Repo(), auto.Transport, syncOpts)
	if err != nil {
		return 0, "", fmt.Errorf("autosync pre-pull: %w", err)
	}

	if result.CkinLockFail != nil && !auto.AllowFork {
		return 0, "", fmt.Errorf("%w: held by %q since %v",
			ErrCkinLockHeld, result.CkinLockFail.HeldBy, result.CkinLockFail.Since)
	}

	// --- fork check via PreCommitCheck ---
	if !auto.AllowFork && commitOpts.Branch == "" {
		commitOpts.PreCommitCheck = func() error {
			forked, err := co.WouldFork()
			if err != nil {
				return err
			}
			if forked {
				return ErrWouldFork
			}
			return nil
		}
	}

	rid, uuid, err := co.Commit(commitOpts)
	if err != nil {
		return 0, "", err
	}

	// --- post-push ---
	if auto.Mode == AutosyncOn {
		syncOpts.Pull = true
		syncOpts.Push = true
		syncOpts.CkinLock = nil
		if _, postErr := libsync.Sync(ctx, co.Repo(), auto.Transport, syncOpts); postErr != nil {
			slog.Warn("autosync post-push failed", "err", postErr)
		}
	}

	// Warn if a fork was detected despite passing the check.
	if forked, _ := co.WouldFork(); forked {
		slog.Warn("fork detected after commit")
	}

	return rid, uuid, nil
}
