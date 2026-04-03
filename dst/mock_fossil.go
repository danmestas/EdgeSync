package dst

import (
	"context"

	libfossil "github.com/danmestas/go-libfossil"
)

// MockFossil simulates a Fossil master server using the real HandleSync
// handler. It manages its own repo and implements libfossil.Transport by
// dispatching raw xfer payloads to HandleSyncWithOpts.
type MockFossil struct {
	repo    *libfossil.Repo
	buggify libfossil.BuggifyChecker
}

// Verify interface compliance at compile time.
var _ libfossil.Transport = (*MockFossil)(nil)

// NewMockFossil creates a MockFossil backed by the given repo.
func NewMockFossil(r *libfossil.Repo) *MockFossil {
	if r == nil {
		panic("dst.NewMockFossil: r must not be nil")
	}
	return &MockFossil{repo: r}
}

// SetBuggify configures fault injection for the handler.
func (f *MockFossil) SetBuggify(b libfossil.BuggifyChecker) {
	f.buggify = b
}

// Repo returns the mock fossil's repository (for seeding and invariants).
func (f *MockFossil) Repo() *libfossil.Repo {
	return f.repo
}

// RoundTrip handles one xfer request/response round by delegating to
// the real HandleSyncWithOpts. This ensures the DST tests exercise the
// same code path as production servers.
func (f *MockFossil) RoundTrip(ctx context.Context, payload []byte) ([]byte, error) {
	if payload == nil {
		panic("MockFossil.RoundTrip: payload must not be nil")
	}
	return f.repo.HandleSyncWithOpts(ctx, payload, libfossil.HandleOpts{
		Buggify: f.buggify,
	})
}

// StoreArtifact adds a raw artifact to the mock fossil's repo using SQL.
// Returns the UUID. Used by tests to seed the master with content.
func (f *MockFossil) StoreArtifact(data []byte) (string, error) {
	// Use Repo.Commit to add content through the public API.
	// For raw blob storage, we'd need blob.Store which is internal.
	// For now, use Commit as the public way to add content.
	_, uuid, err := f.repo.Commit(libfossil.CommitOpts{
		Files: []libfossil.FileToCommit{
			{Name: "artifact.bin", Content: data},
		},
		Comment: "seeded artifact",
		User:    "sim",
	})
	return uuid, err
}
