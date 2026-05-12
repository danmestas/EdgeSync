package hub

import (
	"context"
	"fmt"
	"net/url"
	"os"

	libfossil "github.com/danmestas/libfossil"
)

// seedFromUpstream clones a fossil repo at path from the given HTTP xfer
// endpoint URL. The cloned repo carries the upstream's project-code, so
// it can join the upstream's NATS fossil-sync subject after open.
//
// Validates that the URL is well-formed and has an http(s) scheme.
// NATS-native clone is deferred until libfossil HandleSync supports the
// clone protocol upstream (CDG-148).
//
// If expectedProjectCode is non-empty, asserts the cloned repo's
// project-code matches it. The cloned repo is removed before returning
// the mismatch error so the caller can retry or give up cleanly.
func seedFromUpstream(ctx context.Context, path, upstreamURL, bootstrapUser, expectedProjectCode string) error {
	u, err := url.Parse(upstreamURL)
	if err != nil {
		return fmt.Errorf("hub: parse SeedFromUpstream %q: %w", upstreamURL, err)
	}
	if u.Scheme != "http" && u.Scheme != "https" {
		return fmt.Errorf("hub: SeedFromUpstream %q: only http(s) is supported (NATS-clone deferred to libfossil HandleSync support)", upstreamURL)
	}

	transport := libfossil.NewHTTPTransport(upstreamURL)
	repo, result, err := libfossil.Clone(ctx, path, transport, libfossil.CloneOpts{
		User:        bootstrapUser,
		ProjectCode: expectedProjectCode,
	})
	if err != nil {
		return fmt.Errorf("hub: clone from %s: %w", upstreamURL, err)
	}
	if err := repo.Close(); err != nil {
		return fmt.Errorf("hub: close cloned repo: %w", err)
	}

	if expectedProjectCode != "" && result.ProjectCode != expectedProjectCode {
		_ = os.Remove(path)
		return fmt.Errorf("hub: cloned repo has project-code %q, want %q (config drift between hub and upstream)", result.ProjectCode, expectedProjectCode)
	}
	return nil
}
