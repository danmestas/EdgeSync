package sync

import (
	"context"
	"testing"
)

// recordingObserver records all lifecycle calls for test assertions.
type recordingObserver struct {
	started        int
	roundsStarted  []int
	roundsCompleted []int
	completed      int
	lastInfo       SessionStart
	lastEnd        SessionEnd
	lastErr        error
}

func (r *recordingObserver) Started(ctx context.Context, info SessionStart) context.Context {
	r.started++
	r.lastInfo = info
	return ctx
}

func (r *recordingObserver) RoundStarted(ctx context.Context, round int) context.Context {
	r.roundsStarted = append(r.roundsStarted, round)
	return ctx
}

func (r *recordingObserver) RoundCompleted(ctx context.Context, round int, sent, recvd int) {
	r.roundsCompleted = append(r.roundsCompleted, round)
}

func (r *recordingObserver) Completed(ctx context.Context, info SessionEnd, err error) {
	r.completed++
	r.lastEnd = info
	r.lastErr = err
}

func TestNopObserverDoesNotPanic(t *testing.T) {
	var obs nopObserver
	ctx := context.Background()
	ctx = obs.Started(ctx, SessionStart{Operation: "sync"})
	ctx = obs.RoundStarted(ctx, 0)
	obs.RoundCompleted(ctx, 0, 5, 3)
	obs.Completed(ctx, SessionEnd{Operation: "sync", Rounds: 1}, nil)
}

func TestResolveObserverNil(t *testing.T) {
	obs := resolveObserver(nil)
	if obs == nil {
		t.Fatal("resolveObserver(nil) should return nopObserver, not nil")
	}
	// Should not panic
	ctx := obs.Started(context.Background(), SessionStart{})
	obs.RoundStarted(ctx, 0)
}
