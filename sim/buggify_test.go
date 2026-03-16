package sim

import "testing"

func TestBuggifyNilSafe(t *testing.T) {
	var b *Buggify
	if b.Check("anything", 1.0) {
		t.Fatal("nil Buggify should always return false")
	}
}

func TestBuggifyDeterministic(t *testing.T) {
	b1 := NewBuggify(42)
	b2 := NewBuggify(42)

	for i := 0; i < 100; i++ {
		r1 := b1.Check("sync.buildFileCards.skip", 0.5)
		r2 := b2.Check("sync.buildFileCards.skip", 0.5)
		if r1 != r2 {
			t.Fatalf("iteration %d: same seed should produce same result", i)
		}
	}
}

func TestBuggifySiteEnablement(t *testing.T) {
	b := NewBuggify(42)
	active := b.ActiveSites()
	t.Logf("Active sites for seed 42: %v", active)
}
