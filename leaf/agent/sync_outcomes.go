package agent

import (
	"context"
	"errors"
	"log/slog"

	libfossil "github.com/danmestas/libfossil"
	"github.com/nats-io/nats.go"
)

// syncOutcome carries the result of a single per-target sync attempt
// inside one poll cycle. Either result or err is set (never both).
type syncOutcome struct {
	target syncTarget
	result *libfossil.SyncResult
	err    error
}

// emitSyncOutcomes logs the per-target outcomes from one poll cycle
// and invokes PostSyncHook on every successful outcome.
//
// Failed outcomes log at ERROR when no target in this cycle succeeded
// (sync as a whole failed; operator action likely needed). When at
// least one target succeeded, failed outcomes log at DEBUG instead —
// sync as a whole succeeded, isolated transport-level noise shouldn't
// trip alerting.
//
// Independently, errors classified as benign by isBenignSyncError
// (today: nats.ErrNoResponders, context.Canceled) log at DEBUG
// regardless of whether other targets succeeded. ErrNoResponders is
// the round-0 subject-interest race, expected at every cold-start;
// context.Canceled is the agent-shutdown teardown artifact. Neither
// is user-actionable.
//
// The legacy `a.logf("sync error [%s]: %v", ...)` callback fires
// regardless of slog level so harness-supplied loggers stay
// informed without slog depending on them.
func (a *Agent) emitSyncOutcomes(ctx context.Context, outcomes []syncOutcome) {
	anySuccess := false
	for _, o := range outcomes {
		if o.err == nil {
			anySuccess = true
			break
		}
	}

	for _, o := range outcomes {
		if o.err != nil {
			a.logf("sync error [%s]: %v", o.target.label, o.err)
			level := slog.LevelError
			if anySuccess || isBenignSyncError(o.err) {
				level = slog.LevelDebug
			}
			slog.LogAttrs(ctx, level, "sync error",
				slog.String("target", o.target.label),
				slog.Any("error", o.err),
			)
			continue
		}
		a.logf("sync done [%s]: ↑%d ↓%d rounds=%d",
			o.target.label, o.result.FilesSent, o.result.FilesRecvd, o.result.Rounds)
		slog.DebugContext(ctx, "sync details",
			"target", o.target.label,
			"rounds", o.result.Rounds,
			"files_sent", o.result.FilesSent,
			"files_recv", o.result.FilesRecvd,
			"bytes_sent", o.result.BytesSent,
			"bytes_recv", o.result.BytesRecvd,
			"uv_sent", o.result.UVFilesSent,
			"uv_recv", o.result.UVFilesRecvd,
			"errors", len(o.result.Errors),
		)
		for _, e := range o.result.Errors {
			a.logf("sync warning [%s]: %s", o.target.label, e)
		}
		if a.config.PostSyncHook != nil {
			a.config.PostSyncHook(o.result)
		}
	}
}

// isBenignSyncError reports whether a sync error is known to be
// non-actionable and should always log at DEBUG, even if it is the
// only error in the cycle. Today's classifications:
//
//   - nats.ErrNoResponders: round-0 NATS request fired before the
//     hub's serve-nats subscriber finished propagating its
//     subject-interest through the leaf-node mesh. Expected on every
//     cold-start; the next round resolves.
//   - context.Canceled: agent shutdown interrupted an in-flight sync.
//     The operator chose to stop; the in-flight result is moot.
//
// Both classifications are matched via errors.Is so wrapped errors
// from libfossil's transport layer are caught.
func isBenignSyncError(err error) bool {
	if err == nil {
		return false
	}
	if errors.Is(err, nats.ErrNoResponders) {
		return true
	}
	if errors.Is(err, context.Canceled) {
		return true
	}
	return false
}
