package agent

import (
	"context"
	"log/slog"

	libfossil "github.com/danmestas/libfossil"
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
// sync as a whole succeeded, isolated transport-level noise (e.g.
// the round-0 NATS subject-interest race) shouldn't trip alerting.
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
	failedLevel := slog.LevelError
	if anySuccess {
		failedLevel = slog.LevelDebug
	}

	for _, o := range outcomes {
		if o.err != nil {
			a.logf("sync error [%s]: %v", o.target.label, o.err)
			slog.LogAttrs(ctx, failedLevel, "sync error",
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
