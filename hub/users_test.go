package hub

import (
	"sync"
	"testing"
)

// TestHub_EnsureUser covers the contract documented on Hub.EnsureUser:
// first-time registration, idempotent re-call, caps-change-preserves-
// existing, concurrent racing callers, and the empty-login failure mode.
//
// Test gap: the "underlying repo unreachable / fossil-lock contention"
// failure mode isn't covered here — exercising it cleanly requires either
// closing the hub mid-call (data race on the libfossil handle) or
// fault-injection at the libfossil layer that isn't currently exposed.
// Acceptable: the cap-empty / login-empty path proves error wiring, and
// the underlying CreateUser/GetUser error paths are already covered by
// libfossil's own user_test.go.
func TestHub_EnsureUser(t *testing.T) {
	t.Run("first-time registration creates user with caps", func(t *testing.T) {
		h := newTestHub(t)
		if err := h.EnsureUser("alice", "i"); err != nil {
			t.Fatalf("EnsureUser(alice, i): %v", err)
		}
		if !h.HasUser("alice") {
			t.Fatal("HasUser(alice) = false after EnsureUser")
		}
		got, err := h.GetUser("alice")
		if err != nil {
			t.Fatalf("GetUser(alice): %v", err)
		}
		if got.Login != "alice" || got.Caps != "i" {
			t.Errorf("GetUser = %+v, want {Login:alice Caps:i}", got)
		}
	})

	t.Run("duplicate call with same caps is a no-op", func(t *testing.T) {
		h := newTestHub(t)
		if err := h.EnsureUser("bob", "i"); err != nil {
			t.Fatalf("EnsureUser bob first: %v", err)
		}
		if err := h.EnsureUser("bob", "i"); err != nil {
			t.Fatalf("EnsureUser bob second: %v", err)
		}
		if !h.HasUser("bob") {
			t.Fatal("HasUser(bob) = false after two EnsureUser calls")
		}
		got, err := h.GetUser("bob")
		if err != nil {
			t.Fatalf("GetUser(bob): %v", err)
		}
		if got.Caps != "i" {
			t.Errorf("GetUser caps = %q, want %q", got.Caps, "i")
		}
	})

	t.Run("caps change on duplicate preserves original caps", func(t *testing.T) {
		h := newTestHub(t)
		if err := h.EnsureUser("carol", "i"); err != nil {
			t.Fatalf("EnsureUser carol i: %v", err)
		}
		// Second call with different caps must NOT mutate the row.
		if err := h.EnsureUser("carol", "a"); err != nil {
			t.Fatalf("EnsureUser carol a (duplicate): %v", err)
		}
		got, err := h.GetUser("carol")
		if err != nil {
			t.Fatalf("GetUser(carol): %v", err)
		}
		if got.Caps != "i" {
			t.Errorf("GetUser caps = %q, want %q (caps must be preserved on duplicate)", got.Caps, "i")
		}
	})

	t.Run("concurrent callers all return nil and land exactly one user", func(t *testing.T) {
		h := newTestHub(t)
		const n = 5
		var wg sync.WaitGroup
		errs := make([]error, n)
		start := make(chan struct{})
		for i := 0; i < n; i++ {
			wg.Add(1)
			go func(i int) {
				defer wg.Done()
				<-start
				errs[i] = h.EnsureUser("dana", "i")
			}(i)
		}
		close(start)
		wg.Wait()
		for i, err := range errs {
			if err != nil {
				t.Errorf("goroutine %d: EnsureUser: %v", i, err)
			}
		}
		users, err := h.ListUsers()
		if err != nil {
			t.Fatalf("ListUsers: %v", err)
		}
		var danaCount int
		for _, u := range users {
			if u.Login == "dana" {
				danaCount++
			}
		}
		if danaCount != 1 {
			t.Errorf("ListUsers count(dana) = %d, want 1", danaCount)
		}
	})

	t.Run("empty login is rejected", func(t *testing.T) {
		h := newTestHub(t)
		if err := h.EnsureUser("", "i"); err == nil {
			t.Fatal("EnsureUser with empty login should error")
		}
	})
}
