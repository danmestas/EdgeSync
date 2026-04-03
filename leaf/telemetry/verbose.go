package telemetry

import (
	"fmt"
	"io"

	libfossil "github.com/danmestas/go-libfossil"
)

// VerboseObserver wraps another SyncObserver and logs human-readable sync
// lifecycle events to the given writer (typically os.Stderr).
// The inner observer receives all callbacks unchanged.
type VerboseObserver struct {
	inner libfossil.SyncObserver
	w     io.Writer
}

// NewVerboseObserver returns an observer that logs to w and delegates to inner.
// If inner is nil, only logging is performed (no telemetry forwarding).
func NewVerboseObserver(w io.Writer, inner libfossil.SyncObserver) *VerboseObserver {
	return &VerboseObserver{inner: inner, w: w}
}

func (v *VerboseObserver) Started(info libfossil.SessionStart) {
	fmt.Fprintf(v.w, "[verbose] sync started  push=%t pull=%t uv=%t project=%s\n",
		info.Push, info.Pull, info.UV, info.ProjectCode)
	if v.inner != nil {
		v.inner.Started(info)
	}
}

func (v *VerboseObserver) RoundStarted(round int) {
	fmt.Fprintf(v.w, "[verbose] round %d started\n", round)
	if v.inner != nil {
		v.inner.RoundStarted(round)
	}
}

func (v *VerboseObserver) RoundCompleted(round int, stats libfossil.RoundStats) {
	fmt.Fprintf(v.w, "[verbose] round %d completed  sent=%d recv=%d gimmes=%d igots=%d bytes_out=%d bytes_in=%d\n",
		round, stats.FilesSent, stats.FilesRecvd, stats.Gimmes, stats.IGots, stats.BytesSent, stats.BytesRecvd)
	if v.inner != nil {
		v.inner.RoundCompleted(round, stats)
	}
}

func (v *VerboseObserver) Completed(info libfossil.SessionEnd) {
	fmt.Fprintf(v.w, "[verbose] sync completed  rounds=%d sent=%d recv=%d\n",
		info.Rounds, info.FilesSent, info.FilesRecvd)
	if v.inner != nil {
		v.inner.Completed(info)
	}
}

func (v *VerboseObserver) Error(err error) {
	if err != nil {
		fmt.Fprintf(v.w, "[verbose] error: %v\n", err)
	}
	if v.inner != nil {
		v.inner.Error(err)
	}
}

func (v *VerboseObserver) HandleStarted(info libfossil.HandleStart) {
	fmt.Fprintf(v.w, "[verbose] handle started  remote=%s\n", info.RemoteAddr)
	if v.inner != nil {
		v.inner.HandleStarted(info)
	}
}

func (v *VerboseObserver) HandleCompleted(info libfossil.HandleEnd) {
	fmt.Fprintf(v.w, "[verbose] handle completed  sent=%d recv=%d\n",
		info.FilesSent, info.FilesRecvd)
	if v.inner != nil {
		v.inner.HandleCompleted(info)
	}
}

func (v *VerboseObserver) TableSyncStarted(info libfossil.TableSyncStart) {
	fmt.Fprintf(v.w, "[verbose] table sync started  table=%s\n", info.Table)
	if v.inner != nil {
		v.inner.TableSyncStarted(info)
	}
}

func (v *VerboseObserver) TableSyncCompleted(info libfossil.TableSyncEnd) {
	fmt.Fprintf(v.w, "[verbose] table sync completed  table=%s sent=%d recv=%d\n",
		info.Table, info.RowsSent, info.RowsRecvd)
	if v.inner != nil {
		v.inner.TableSyncCompleted(info)
	}
}
