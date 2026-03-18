package sync

import (
	"context"

	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

// ServerHandler processes an incoming sync request and produces a response.
type ServerHandler interface {
	HandleSync(ctx context.Context, r *repo.Repo, request *xfer.Message) (*xfer.Message, error)
}

// CloneOpts configures a clone operation.
type CloneOpts struct {
	User     string     // Credentials for clone auth (also default admin user)
	Password string
	Version  int        // Protocol version (default 3)
	Env      *simio.Env // nil defaults to RealEnv
}

// CloneResult reports what happened during a clone.
type CloneResult struct {
	Rounds      int
	BlobsRecvd  int
	ProjectCode string
	ServerCode  string
}
