package agent

import (
	"context"
	"fmt"

	libfossil "github.com/danmestas/libfossil"
)

// cloneFromHub clones a fossil repo from hubURL into repoPath. The cloned
// handle is closed before returning so subsequent libfossil.Open in New
// reopens it through the same code path as a pre-existing repo.
//
// Used by New when Config.CloneFromHubURL is set and Config.RepoPath does
// not yet exist on disk. Callers that need a clone helper independent of
// agent construction can use this function directly via CloneRepo.
func cloneFromHub(repoPath, hubURL string) error {
	return CloneRepo(context.Background(), repoPath, hubURL)
}

// CloneRepo clones a fossil repo from hubURL into repoPath. Standalone
// helper that does not require an Agent instance — callers that need to
// bootstrap a leaf repo before any agent exists can use it directly,
// though most callers should prefer Config.CloneFromHubURL on agent.New.
//
// The cloned repo is closed before returning. Reopen via libfossil.Open
// or simply construct an Agent that points at the same path.
func CloneRepo(ctx context.Context, repoPath, hubURL string) error {
	if repoPath == "" {
		return fmt.Errorf("agent.CloneRepo: repoPath is required")
	}
	if hubURL == "" {
		return fmt.Errorf("agent.CloneRepo: hubURL is required")
	}
	transport := libfossil.NewHTTPTransport(hubURL)
	r, _, err := libfossil.Clone(ctx, repoPath, transport, libfossil.CloneOpts{})
	if err != nil {
		return fmt.Errorf("clone %s -> %s: %w", hubURL, repoPath, err)
	}
	if r != nil {
		_ = r.Close()
	}
	return nil
}
