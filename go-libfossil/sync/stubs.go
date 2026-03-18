package sync

import (
	"context"

	"github.com/dmestas/edgesync/go-libfossil/repo"
)

// CloneOpts configures a clone operation.
type CloneOpts struct {
	ProjectCode string
	ServerCode  string
	User        string
	Password    string
	Version     int
}

// Clone performs a full repository clone from a remote.
// NOT IMPLEMENTED — panics.
func Clone(ctx context.Context, r *repo.Repo, t Transport, opts CloneOpts) error {
	panic("sync.Clone: not implemented — planned for Phase G")
}
