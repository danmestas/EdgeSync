package sim

// FaultSchedule is a time-ordered list of fault events.
// Full implementation in a later task.
type FaultSchedule struct {
	Events []FaultEvent
}

type FaultEvent struct{}

func (s *FaultSchedule) String() string { return "" }
