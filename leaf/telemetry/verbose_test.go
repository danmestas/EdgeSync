package telemetry

import (
	"bytes"
	"errors"
	"strings"
	"testing"

	libfossil "github.com/danmestas/go-libfossil"
)

func TestVerboseObserver(t *testing.T) {
	var buf bytes.Buffer
	obs := NewVerboseObserver(&buf, nil)

	obs.Started(libfossil.SessionStart{
		Push: true, Pull: true, UV: false, ProjectCode: "abc123",
	})
	obs.RoundStarted(1)
	obs.RoundCompleted(1, libfossil.RoundStats{
		FilesSent: 3, FilesRecvd: 2, Gimmes: 1, BytesSent: 1024,
	})
	obs.Completed(libfossil.SessionEnd{
		Rounds: 1, FilesSent: 3, FilesRecvd: 2,
	})

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

	obs.Error(errors.New("test error"))
	if !strings.Contains(buf.String(), "test error") {
		t.Errorf("error not logged: %s", buf.String())
	}
}

func TestVerboseObserverDelegatesToInner(t *testing.T) {
	var buf bytes.Buffer
	var innerCalled bool
	inner := &mockObserver{onStarted: func() { innerCalled = true }}

	obs := NewVerboseObserver(&buf, inner)
	obs.Started(libfossil.SessionStart{})

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

func (m *mockObserver) Started(_ libfossil.SessionStart) {
	if m.onStarted != nil {
		m.onStarted()
	}
}
func (m *mockObserver) RoundStarted(_ int)                            {}
func (m *mockObserver) RoundCompleted(_ int, _ libfossil.RoundStats)  {}
func (m *mockObserver) Completed(_ libfossil.SessionEnd)              {}
func (m *mockObserver) Error(_ error)                                 {}
func (m *mockObserver) HandleStarted(_ libfossil.HandleStart)         {}
func (m *mockObserver) HandleCompleted(_ libfossil.HandleEnd)         {}
func (m *mockObserver) TableSyncStarted(_ libfossil.TableSyncStart)   {}
func (m *mockObserver) TableSyncCompleted(_ libfossil.TableSyncEnd)   {}
