package telemetry

import (
	"bytes"
	"context"
	"errors"
	"strings"
	"testing"

	libsync "github.com/dmestas/edgesync/go-libfossil/sync"
)

func TestVerboseObserver(t *testing.T) {
	var buf bytes.Buffer
	obs := NewVerboseObserver(&buf, nil)
	ctx := context.Background()

	ctx = obs.Started(ctx, libsync.SessionStart{
		Operation: "sync", Push: true, Pull: true, UV: false, ProjectCode: "abc123",
	})
	ctx = obs.RoundStarted(ctx, 1)
	obs.RoundCompleted(ctx, 1, libsync.RoundStats{
		FilesSent: 3, FilesReceived: 2, GimmesSent: 1, BytesSent: 1024,
	})
	obs.Completed(ctx, libsync.SessionEnd{
		Operation: "sync", Rounds: 1, FilesSent: 3, FilesRecvd: 2,
	}, nil)

	output := buf.String()
	for _, want := range []string{
		"sync started",
		"round 1 started",
		"round 1 completed",
		"sent=3",
		"recv=2",
		"sync completed",
		"rounds=1",
	} {
		if !strings.Contains(output, want) {
			t.Errorf("output missing %q\ngot:\n%s", want, output)
		}
	}
}

func TestVerboseObserverError(t *testing.T) {
	var buf bytes.Buffer
	obs := NewVerboseObserver(&buf, nil)

	obs.Error(context.Background(), errors.New("test error"))
	if !strings.Contains(buf.String(), "test error") {
		t.Errorf("error not logged: %s", buf.String())
	}
}

func TestVerboseObserverDelegatesToInner(t *testing.T) {
	var buf bytes.Buffer
	var innerCalled bool
	inner := &mockObserver{onStarted: func() { innerCalled = true }}

	obs := NewVerboseObserver(&buf, inner)
	obs.Started(context.Background(), libsync.SessionStart{Operation: "sync"})

	if !innerCalled {
		t.Error("inner observer not called")
	}
	if buf.Len() == 0 {
		t.Error("verbose output empty")
	}
}

type mockObserver struct {
	onStarted func()
}

func (m *mockObserver) Started(ctx context.Context, _ libsync.SessionStart) context.Context {
	if m.onStarted != nil {
		m.onStarted()
	}
	return ctx
}
func (m *mockObserver) RoundStarted(ctx context.Context, _ int) context.Context { return ctx }
func (m *mockObserver) RoundCompleted(_ context.Context, _ int, _ libsync.RoundStats) {}
func (m *mockObserver) Completed(_ context.Context, _ libsync.SessionEnd, _ error)    {}
func (m *mockObserver) Error(_ context.Context, _ error)                               {}
func (m *mockObserver) HandleStarted(ctx context.Context, _ libsync.HandleStart) context.Context {
	return ctx
}
func (m *mockObserver) HandleCompleted(_ context.Context, _ libsync.HandleEnd)    {}
func (m *mockObserver) TableSyncStarted(_ context.Context, _ libsync.TableSyncStart)  {}
func (m *mockObserver) TableSyncCompleted(_ context.Context, _ libsync.TableSyncEnd)  {}
