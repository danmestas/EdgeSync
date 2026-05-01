package agent

import (
	"bytes"
	"context"
	"errors"
	"fmt"
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

// TestEmitSyncOutcomes_PromotesWhenAllFail pins the inverse of the
// "another target succeeded" rule: if no target succeeded AND the
// failures aren't benign-classified, ERROR level is retained so
// operators (and alerting) still see a real problem.
func TestEmitSyncOutcomes_PromotesWhenAllFail(t *testing.T) {
	buf := captureSlog(t)

	a := &Agent{config: Config{}}
	outcomes := []syncOutcome{
		{target: syncTarget{label: "nats"}, err: errors.New("authentication failed")},
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

// TestEmitSyncOutcomes_DemotesNoRespondersWhenSoleTarget pins that
// a single-syncTarget agent (the bones swarm leaf shape — only
// "nats" is registered; HTTP push is bones' separate concern) does
// not emit ERROR for the round-0 nats.ErrNoResponders race. The
// "demote when another target succeeded" rule cannot help when
// there are no other targets; benign-classification handles it.
func TestEmitSyncOutcomes_DemotesNoRespondersWhenSoleTarget(t *testing.T) {
	buf := captureSlog(t)

	a := &Agent{config: Config{}}
	outcomes := []syncOutcome{
		{target: syncTarget{label: "nats"}, err: nats.ErrNoResponders},
	}
	a.emitSyncOutcomes(context.Background(), outcomes)

	out := buf.String()
	if strings.Contains(out, `level=ERROR msg="sync error"`) {
		t.Errorf("expected no ERROR for sole-target ErrNoResponders, got:\n%s", out)
	}
	if !strings.Contains(out, "target=nats") {
		t.Errorf("expected the nats failure logged at lower level, got:\n%s", out)
	}
}

// TestEmitSyncOutcomes_DemotesContextCanceled pins the same shape
// for context.Canceled — when the agent is being shut down and an
// in-flight sync's ctx is canceled, the resulting "sync error" is
// a teardown artifact, not user-actionable.
func TestEmitSyncOutcomes_DemotesContextCanceled(t *testing.T) {
	buf := captureSlog(t)

	a := &Agent{config: Config{}}
	outcomes := []syncOutcome{
		{target: syncTarget{label: "nats"}, err: context.Canceled},
	}
	a.emitSyncOutcomes(context.Background(), outcomes)

	out := buf.String()
	if strings.Contains(out, `level=ERROR msg="sync error"`) {
		t.Errorf("expected no ERROR for context.Canceled, got:\n%s", out)
	}
}

// TestEmitSyncOutcomes_DemotesWrappedNoResponders ensures wrapped
// sentinels (libfossil's "sync: exchange round 0: <wrapped>" shape)
// are still recognized as benign via errors.Is.
func TestEmitSyncOutcomes_DemotesWrappedNoResponders(t *testing.T) {
	buf := captureSlog(t)

	wrapped := fmt.Errorf("libfossil: sync: exchange round 0: %w", nats.ErrNoResponders)

	a := &Agent{config: Config{}}
	outcomes := []syncOutcome{
		{target: syncTarget{label: "nats"}, err: wrapped},
	}
	a.emitSyncOutcomes(context.Background(), outcomes)

	out := buf.String()
	if strings.Contains(out, `level=ERROR msg="sync error"`) {
		t.Errorf("expected no ERROR for wrapped ErrNoResponders, got:\n%s", out)
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
