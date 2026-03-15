package sim

import (
	"fmt"
	"math/rand"
	"sort"
	"strings"
	"time"
)

type FaultType int

const (
	FaultPartition     FaultType = iota
	FaultHealPartition
	FaultLatency
	FaultHealLatency
	FaultDropConns
	FaultBridgeRestart
	FaultLeafRestart
	FaultHealAll
)

func (f FaultType) String() string {
	switch f {
	case FaultPartition:
		return "partition"
	case FaultHealPartition:
		return "heal-partition"
	case FaultLatency:
		return "latency"
	case FaultHealLatency:
		return "heal-latency"
	case FaultDropConns:
		return "drop-connections"
	case FaultBridgeRestart:
		return "bridge-restart"
	case FaultLeafRestart:
		return "leaf-restart"
	case FaultHealAll:
		return "heal-all"
	}
	return "unknown"
}

type FaultEvent struct {
	Time   time.Duration
	Type   FaultType
	Target string
	Param  time.Duration
}

func (e FaultEvent) String() string {
	s := fmt.Sprintf("t=%s %s", e.Time, e.Type)
	if e.Target != "" {
		s += " " + e.Target
	}
	if e.Param > 0 {
		s += fmt.Sprintf(" (%s)", e.Param)
	}
	return s
}

type FaultSchedule struct {
	Events []FaultEvent
}

func (s *FaultSchedule) String() string {
	var lines []string
	for _, e := range s.Events {
		lines = append(lines, e.String())
	}
	return strings.Join(lines, "\n")
}

func GenerateSchedule(seed int64, severity Level, faultDuration time.Duration, numLeaves int) *FaultSchedule {
	if severity == LevelNormal {
		return &FaultSchedule{}
	}

	rng := rand.New(rand.NewSource(seed))
	var events []FaultEvent

	var numFaults int
	switch severity {
	case LevelAdversarial:
		numFaults = 2 + rng.Intn(4)
	case LevelHostile:
		numFaults = 5 + rng.Intn(6)
	}

	for range numFaults {
		offset := time.Duration(rng.Int63n(int64(faultDuration)))

		var ft FaultType
		switch severity {
		case LevelAdversarial:
			switch rng.Intn(3) {
			case 0:
				ft = FaultPartition
			case 1:
				ft = FaultLatency
			case 2:
				ft = FaultDropConns
			}
		case LevelHostile:
			switch rng.Intn(5) {
			case 0:
				ft = FaultPartition
			case 1:
				ft = FaultLatency
			case 2:
				ft = FaultDropConns
			case 3:
				ft = FaultBridgeRestart
			case 4:
				ft = FaultLeafRestart
			}
		}

		target := fmt.Sprintf("leaf-%d", rng.Intn(numLeaves))
		if ft == FaultBridgeRestart {
			target = "bridge"
		}

		param := time.Duration(0)
		if ft == FaultLatency {
			param = time.Duration(100+rng.Intn(900)) * time.Millisecond
		}

		events = append(events, FaultEvent{
			Time:   offset,
			Type:   ft,
			Target: target,
			Param:  param,
		})

		if ft == FaultPartition {
			healOffset := offset + time.Duration(1+rng.Intn(5))*time.Second
			if healOffset > faultDuration {
				healOffset = faultDuration
			}
			events = append(events, FaultEvent{
				Time:   healOffset,
				Type:   FaultHealPartition,
				Target: target,
			})
		}
		if ft == FaultLatency {
			healOffset := offset + time.Duration(1+rng.Intn(3))*time.Second
			if healOffset > faultDuration {
				healOffset = faultDuration
			}
			events = append(events, FaultEvent{
				Time:   healOffset,
				Type:   FaultHealLatency,
			})
		}
	}

	events = append(events, FaultEvent{
		Time: faultDuration,
		Type: FaultHealAll,
	})

	sort.Slice(events, func(i, j int) bool {
		return events[i].Time < events[j].Time
	})

	return &FaultSchedule{Events: events}
}
