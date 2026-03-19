package sync

import "context"

// SessionStart describes the beginning of a sync or clone operation.
type SessionStart struct {
	Operation   string // "sync" or "clone"
	Push, Pull  bool
	UV          bool
	ProjectCode string
}

// SessionEnd describes the result of a sync or clone operation.
type SessionEnd struct {
	Operation                     string
	Rounds                        int
	FilesSent, FilesRecvd         int
	UVFilesSent, UVFilesRecvd     int
	UVGimmesSent                  int
	ProjectCode                   string
	Errors                        []string
}

// Observer receives lifecycle callbacks during sync and clone operations.
// A single Observer instance may be shared across multiple concurrent sessions.
// Pass nil for no-op default.
type Observer interface {
	Started(ctx context.Context, info SessionStart) context.Context
	RoundStarted(ctx context.Context, round int) context.Context
	RoundCompleted(ctx context.Context, round int, sent, recvd int)
	Completed(ctx context.Context, info SessionEnd, err error)
}

// nopObserver is the default observer that does nothing.
type nopObserver struct{}

func (nopObserver) Started(ctx context.Context, _ SessionStart) context.Context    { return ctx }
func (nopObserver) RoundStarted(ctx context.Context, _ int) context.Context        { return ctx }
func (nopObserver) RoundCompleted(_ context.Context, _ int, _, _ int)              {}
func (nopObserver) Completed(_ context.Context, _ SessionEnd, _ error)             {}

// resolveObserver returns obs if non-nil, otherwise nopObserver{}.
func resolveObserver(obs Observer) Observer {
	if obs == nil {
		return nopObserver{}
	}
	return obs
}
