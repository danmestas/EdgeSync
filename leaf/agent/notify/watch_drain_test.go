package notify

import (
	"context"
	"sync"
	"testing"
	"time"
)

// TestServiceWatch_NoPanicOnCancelWithInFlightMessages stress-tests
// the Watch teardown path for the race fixed by closing channels
// only after the underlying NATS subscription has drained its
// pending callbacks. Before the fix, an in-flight callback that
// passed through `case <-ctx.Done()` and entered `case ch <- msg`
// could race with `close(ch)` and panic with "send on closed
// channel" (danmestas/EdgeSync#100).
//
// The race is timing-dependent so this is run under repeated
// iteration with a tight cancel window. If it ever panics the
// whole test process dies, which is exactly the regression we
// are pinning out.
func TestServiceWatch_NoPanicOnCancelWithInFlightMessages(t *testing.T) {
	ts := newTestService(t)

	const iterations = 25
	const sendsPerIter = 8

	for i := 0; i < iterations; i++ {
		ctx, cancel := context.WithCancel(context.Background())
		ch := ts.svc.Watch(ctx, WatchOpts{Project: "stress"})

		// Drain the channel concurrently so the buffered chan
		// doesn't backpressure the callback into the ctx.Done
		// branch. We want the race window — callback in
		// `case ch <- msg:` while teardown closes ch.
		drainDone := make(chan struct{})
		go func() {
			defer close(drainDone)
			for range ch {
			}
		}()

		// Fire several sends. Each commits to the repo and
		// publishes via NATS; the watcher's callback receives
		// each one.
		var sendWG sync.WaitGroup
		for j := 0; j < sendsPerIter; j++ {
			sendWG.Add(1)
			go func() {
				defer sendWG.Done()
				_, _ = ts.svc.Send(SendOpts{Project: "stress", Body: "x"})
			}()
		}

		// Tight cancel window — sometimes before sends complete,
		// sometimes during, sometimes after. The variance is the
		// point: we want the race to fire across iterations.
		time.Sleep(time.Duration(i%4) * time.Millisecond)
		cancel()

		// Wait for the channel to close and the consumer goroutine
		// to exit. If a panic fires in the NATS callback it will
		// crash the process; reaching this point means the iteration
		// didn't panic.
		<-drainDone
		sendWG.Wait()
	}
}
