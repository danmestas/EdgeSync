package agent

// TODO(v0.2.0): Autosync tests require checkout.Checkout and manifest.Checkin
// which are now internal to libfossil. These tests will be re-enabled when
// the public API exposes checkout operations.
//
// Skipped tests:
// - TestAutosyncCommit_OffMode
// - TestAutosyncCommit_WouldForkAborts
// - TestAutosyncCommit_AllowForkOverride
// - TestAutosyncCommit_BranchBypassesForkCheck
// - TestEnsureClientID
//
// The clientid tests can be restored when ensureClientID is updated.

import (
	"bytes"
	"testing"

	libfossil "github.com/danmestas/libfossil"
	_ "github.com/danmestas/libfossil/db/driver/modernc"
	"github.com/danmestas/libfossil/simio"
)

func TestEnsureClientID(t *testing.T) {
	path := t.TempDir() + "/test.fossil"
	r, err := libfossil.Create(path, libfossil.CreateOpts{User: "test"})
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	defer r.Close()

	// Use deterministic RNG for reproducibility.
	rng := bytes.NewReader(bytes.Repeat([]byte{0xab}, 16))

	id1, err := ensureClientID(r, rng)
	if err != nil {
		t.Fatalf("ensureClientID: %v", err)
	}
	if id1 == "" {
		t.Fatal("clientID should not be empty")
	}
	t.Logf("generated clientID: %s", id1)

	// Second call should return the same ID (from DB, not generate new).
	id2, err := ensureClientID(r, rng)
	if err != nil {
		t.Fatalf("ensureClientID (second): %v", err)
	}
	if id2 != id1 {
		t.Fatalf("clientID changed: %q -> %q", id1, id2)
	}
}

// Verify FslID is used correctly (compile-time check).
var _ libfossil.FslID

// Verify SyncObserver interface is satisfied by VerboseObserver (compile-time).
var _ libfossil.SyncObserver = (*mockSyncObserver)(nil)

type mockSyncObserver struct{}

func (*mockSyncObserver) Started(libfossil.SessionStart)              {}
func (*mockSyncObserver) RoundStarted(int)                            {}
func (*mockSyncObserver) RoundCompleted(int, libfossil.RoundStats)    {}
func (*mockSyncObserver) Completed(libfossil.SessionEnd)              {}
func (*mockSyncObserver) Error(error)                                 {}
func (*mockSyncObserver) HandleStarted(libfossil.HandleStart)         {}
func (*mockSyncObserver) HandleCompleted(libfossil.HandleEnd)         {}
func (*mockSyncObserver) TableSyncStarted(libfossil.TableSyncStart)   {}
func (*mockSyncObserver) TableSyncCompleted(libfossil.TableSyncEnd)   {}

// Silence unused import warnings.
var _ = simio.RealClock{}
