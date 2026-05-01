package agent

import (
	"bytes"
	"context"
	"errors"
	"log/slog"
	"strings"
	"testing"

	libfossil "github.com/danmestas/libfossil"
	"github.com/nats-io/nats.go"
)

// TestEmitSyncOutcomes_DemotesWhenAnotherTargetSucceeds pins the
// fix for danmestas/EdgeSync#96 / danmestas/bones#118: when the
// per-target sync loop has at least one successful target, failures
// from sibling targets must NOT log at ERROR level. Sync as a whole
// succeeded; isolated transport failures (e.g. round-0 NATS
// "no responders" race) are at most warnings, not errors.
func TestEmitSyncOutcomes_DemotesWhenAnotherTargetSucceeds(t *testing.T) {
	buf := captureSlog(t)

	a := &Agent{config: Config{}}
	outcomes := []syncOutcome{
		{target: syncTarget{label: "nats"}, err: nats.ErrNoResponders},
		{target: syncTarget{label: "http"}, result: &libfossil.SyncResult{Rounds: 1}},
	}
	a.emitSyncOutcomes(context.Background(), outcomes)

	out := buf.String()
	if strings.Contains(out, `level=ERROR msg="sync error"`) {
		t.Errorf("expected no ERROR-level sync error when another target succeeded, got:\n%s", out)
	}
	if !strings.Contains(out, "target=nats") {
		t.Errorf("expected the nats failure to still be logged (at lower level), got:\n%s", out)
	}
}

// TestEmitSyncOutcomes_PromotesWhenAllFail pins the inverse: if no
// target succeeded, the failures retain ERROR level so operators
// (and alerting) still see a real problem.
func TestEmitSyncOutcomes_PromotesWhenAllFail(t *testing.T) {
	buf := captureSlog(t)

	a := &Agent{config: Config{}}
	outcomes := []syncOutcome{
		{target: syncTarget{label: "nats"}, err: nats.ErrNoResponders},
		{target: syncTarget{label: "http"}, err: errors.New("connection refused")},
	}
	a.emitSyncOutcomes(context.Background(), outcomes)

	out := buf.String()
	if !strings.Contains(out, `level=ERROR msg="sync error" target=nats`) {
		t.Errorf("expected ERROR-level nats sync error when no target succeeded, got:\n%s", out)
	}
	if !strings.Contains(out, `level=ERROR msg="sync error" target=http`) {
		t.Errorf("expected ERROR-level http sync error when no target succeeded, got:\n%s", out)
	}
}

// TestEmitSyncOutcomes_AllSuccessNoErrors pins that a fully-clean
// poll cycle emits no error or warning records at all.
func TestEmitSyncOutcomes_AllSuccessNoErrors(t *testing.T) {
	buf := captureSlog(t)

	a := &Agent{config: Config{}}
	outcomes := []syncOutcome{
		{target: syncTarget{label: "nats"}, result: &libfossil.SyncResult{Rounds: 1}},
		{target: syncTarget{label: "http"}, result: &libfossil.SyncResult{Rounds: 1}},
	}
	a.emitSyncOutcomes(context.Background(), outcomes)

	out := buf.String()
	if strings.Contains(out, `msg="sync error"`) {
		t.Errorf("expected no sync error records on fully-successful cycle, got:\n%s", out)
	}
}

// captureSlog redirects slog.Default() to a bytes.Buffer at Debug
// level for the duration of the test, restoring the prior default
// in t.Cleanup. Returns the buffer for assertion.
func captureSlog(t *testing.T) *bytes.Buffer {
	t.Helper()
	var buf bytes.Buffer
	prev := slog.Default()
	slog.SetDefault(slog.New(slog.NewTextHandler(&buf, &slog.HandlerOptions{Level: slog.LevelDebug})))
	t.Cleanup(func() { slog.SetDefault(prev) })
	return &buf
}
