package sync

import "github.com/dmestas/edgesync/go-libfossil/simio"

// CloneOpts configures a clone operation.
type CloneOpts struct {
	User     string     // Credentials for clone auth (also default admin user)
	Password string
	Version  int        // Protocol version (default 3)
	Env      *simio.Env // nil defaults to RealEnv
	Observer Observer   // nil defaults to no-op
}

// CloneResult reports what happened during a clone.
type CloneResult struct {
	Rounds         int
	BlobsRecvd     int
	ArtifactsLinked int     // Manifests crosslinked into event table
	ProjectCode    string
	ServerCode     string
	Messages       []string // Informational messages from server
}
