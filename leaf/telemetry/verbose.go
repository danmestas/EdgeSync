package telemetry

import (
	"context"
	"fmt"
	"io"

	libsync "github.com/danmestas/go-libfossil/sync"
)

// VerboseObserver wraps another Observer and logs human-readable sync
// lifecycle events to the given writer (typically os.Stderr).
// The inner observer receives all callbacks unchanged.
type VerboseObserver struct {
	inner libsync.Observer
	w     io.Writer
}

// NewVerboseObserver returns an observer that logs to w and delegates to inner.
// If inner is nil, only logging is performed (no telemetry forwarding).
func NewVerboseObserver(w io.Writer, inner libsync.Observer) *VerboseObserver {
	return &VerboseObserver{inner: inner, w: w}
}

func (v *VerboseObserver) Started(ctx context.Context, info libsync.SessionStart) context.Context {
	fmt.Fprintf(v.w, "[verbose] %s started  push=%t pull=%t uv=%t project=%s\n",
		info.Operation, info.Push, info.Pull, info.UV, info.ProjectCode)
	if v.inner != nil {
		return v.inner.Started(ctx, info)
	}
	return ctx
}

func (v *VerboseObserver) RoundStarted(ctx context.Context, round int) context.Context {
	fmt.Fprintf(v.w, "[verbose] round %d started\n", round)
	if v.inner != nil {
		return v.inner.RoundStarted(ctx, round)
	}
	return ctx
}

func (v *VerboseObserver) RoundCompleted(ctx context.Context, round int, stats libsync.RoundStats) {
	fmt.Fprintf(v.w, "[verbose] round %d completed  sent=%d recv=%d gimmes=%d igots=%d bytes_out=%d bytes_in=%d\n",
		round, stats.FilesSent, stats.FilesReceived, stats.GimmesSent, stats.IgotsSent, stats.BytesSent, stats.BytesReceived)
	if v.inner != nil {
		v.inner.RoundCompleted(ctx, round, stats)
	}
}

func (v *VerboseObserver) Completed(ctx context.Context, info libsync.SessionEnd, err error) {
	if err != nil {
		fmt.Fprintf(v.w, "[verbose] %s completed  rounds=%d sent=%d recv=%d uv_sent=%d uv_recv=%d err=%v\n",
			info.Operation, info.Rounds, info.FilesSent, info.FilesRecvd, info.UVFilesSent, info.UVFilesRecvd, err)
	} else {
		fmt.Fprintf(v.w, "[verbose] %s completed  rounds=%d sent=%d recv=%d uv_sent=%d uv_recv=%d\n",
			info.Operation, info.Rounds, info.FilesSent, info.FilesRecvd, info.UVFilesSent, info.UVFilesRecvd)
	}
	if len(info.Errors) > 0 {
		for _, e := range info.Errors {
			fmt.Fprintf(v.w, "[verbose]   protocol error: %s\n", e)
		}
	}
	if v.inner != nil {
		v.inner.Completed(ctx, info, err)
	}
}

func (v *VerboseObserver) Error(ctx context.Context, err error) {
	if err != nil {
		fmt.Fprintf(v.w, "[verbose] error: %v\n", err)
	}
	if v.inner != nil {
		v.inner.Error(ctx, err)
	}
}

func (v *VerboseObserver) HandleStarted(ctx context.Context, info libsync.HandleStart) context.Context {
	fmt.Fprintf(v.w, "[verbose] handle started  op=%s project=%s remote=%s\n",
		info.Operation, info.ProjectCode, info.RemoteAddr)
	if v.inner != nil {
		return v.inner.HandleStarted(ctx, info)
	}
	return ctx
}

func (v *VerboseObserver) HandleCompleted(ctx context.Context, info libsync.HandleEnd) {
	if info.Err != nil {
		fmt.Fprintf(v.w, "[verbose] handle completed  cards=%d sent=%d recv=%d err=%v\n",
			info.CardsProcessed, info.FilesSent, info.FilesReceived, info.Err)
	} else {
		fmt.Fprintf(v.w, "[verbose] handle completed  cards=%d sent=%d recv=%d\n",
			info.CardsProcessed, info.FilesSent, info.FilesReceived)
	}
	if v.inner != nil {
		v.inner.HandleCompleted(ctx, info)
	}
}

func (v *VerboseObserver) TableSyncStarted(ctx context.Context, info libsync.TableSyncStart) {
	fmt.Fprintf(v.w, "[verbose] table sync started  table=%s local_rows=%d\n", info.Table, info.LocalRows)
	if v.inner != nil {
		v.inner.TableSyncStarted(ctx, info)
	}
}

func (v *VerboseObserver) TableSyncCompleted(ctx context.Context, info libsync.TableSyncEnd) {
	fmt.Fprintf(v.w, "[verbose] table sync completed  table=%s sent=%d recv=%d\n", info.Table, info.Sent, info.Received)
	if v.inner != nil {
		v.inner.TableSyncCompleted(ctx, info)
	}
}
