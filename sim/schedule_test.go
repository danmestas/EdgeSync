package sim

import (
	"testing"
	"time"
)

func TestGenerateScheduleNormal(t *testing.T) {
	s := GenerateSchedule(42, LevelNormal, 20*time.Second, 2)
	if len(s.Events) != 0 {
		t.Fatalf("normal severity should have 0 events, got %d", len(s.Events))
	}
}

func TestGenerateScheduleAdversarial(t *testing.T) {
	s := GenerateSchedule(42, LevelAdversarial, 20*time.Second, 2)
	if len(s.Events) == 0 {
		t.Fatal("adversarial should have events")
	}
	last := s.Events[len(s.Events)-1]
	if last.Type != FaultHealAll {
		t.Fatalf("last event should be heal-all, got %s", last.Type)
	}
	t.Log(s.String())
}

func TestGenerateScheduleDeterministic(t *testing.T) {
	s1 := GenerateSchedule(99, LevelHostile, 20*time.Second, 3)
	s2 := GenerateSchedule(99, LevelHostile, 20*time.Second, 3)
	if s1.String() != s2.String() {
		t.Fatal("same seed should produce same schedule")
	}
}
