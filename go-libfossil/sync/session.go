package sync

import (
	"context"
	"fmt"

	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	"github.com/dmestas/edgesync/go-libfossil/uv"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

const (
	// DefaultMaxSend is the default byte budget per round for file payloads.
	DefaultMaxSend = 250000
	// MaxRounds caps the number of sync rounds before giving up.
	MaxRounds = 100
	// MaxGimmeBase is the minimum gimme cap per round.
	MaxGimmeBase = 200
)

// BuggifyChecker is an optional fault injection interface.
// Pass nil in production — implementations should be nil-safe.
type BuggifyChecker interface {
	Check(site string, probability float64) bool
}

// SyncOpts configures a sync session.
type SyncOpts struct {
	Push, Pull              bool
	ProjectCode, ServerCode string
	User, Password          string
	MaxSend                 int
	UV                      bool              // enable unversioned file sync
	Env                     *simio.Env        // nil defaults to RealEnv
	Buggify                 BuggifyChecker    // nil in production
	Observer                Observer          // nil defaults to no-op
}

// SyncResult reports what happened during a sync.
type SyncResult struct {
	Rounds, FilesSent, FilesRecvd int
	UVFilesSent, UVFilesRecvd     int
	UVGimmesSent                  int
	Errors                        []string
}

// session holds the mutable state of a running sync.
type session struct {
	repo                *repo.Repo
	env                 *simio.Env
	opts                SyncOpts
	result              SyncResult
	cookie              string
	remoteHas           map[string]bool
	phantoms            map[string]bool
	pendingSend         map[string]bool
	filesRecvdLastRound int
	igotSentThisRound   int
	maxSend             int
	phantomAge          map[string]int // UUID -> consecutive rounds gimme'd without delivery
	uvHashSent          bool
	uvPushOK            bool
	uvPullOnly          bool
	uvToSend            map[string]bool // name -> true=full content, false=mtime-only
	uvGimmes            map[string]bool
	nUvGimmeSent        int
	nUvFileRcvd         int
}

func newSession(r *repo.Repo, opts SyncOpts) *session {
	if r == nil {
		panic("sync.newSession: r must not be nil")
	}
	ms := opts.MaxSend
	if ms <= 0 {
		ms = DefaultMaxSend
	}
	env := opts.Env
	if env == nil {
		env = simio.RealEnv()
	}
	s := &session{
		repo:        r,
		env:         env,
		opts:        opts,
		maxSend:     ms,
		remoteHas:   make(map[string]bool),
		phantoms:    make(map[string]bool),
		pendingSend: make(map[string]bool),
		phantomAge:  make(map[string]int),
	}

	// Pre-populate uvToSend with all local non-tombstone UV files.
	if opts.UV {
		if err := uv.EnsureSchema(r.DB()); err != nil {
			panic(fmt.Sprintf("sync.newSession: uv.EnsureSchema: %v", err))
		}
		entries, err := uv.List(r.DB())
		if err != nil {
			panic(fmt.Sprintf("sync.newSession: uv.List: %v", err))
		}
		s.uvToSend = make(map[string]bool)
		s.uvGimmes = make(map[string]bool)
		for _, e := range entries {
			// Include both live files and tombstones so deletions propagate.
			s.uvToSend[e.Name] = true
		}
	}

	return s
}

// Sync runs the client sync loop against the given transport.
// It returns once the protocol has converged or a fatal error occurs.
func Sync(ctx context.Context, r *repo.Repo, t Transport, opts SyncOpts) (result *SyncResult, err error) {
	if r == nil {
		panic("sync.Sync: r must not be nil")
	}
	if t == nil {
		panic("sync.Sync: t must not be nil")
	}
	defer func() {
		if err == nil && result == nil {
			panic("sync.Sync: result must not be nil on success")
		}
	}()

	obs := resolveObserver(opts.Observer)
	s := newSession(r, opts)

	ctx = obs.Started(ctx, SessionStart{
		Operation:   "sync",
		Push:        opts.Push,
		Pull:        opts.Pull,
		UV:          opts.UV,
		ProjectCode: opts.ProjectCode,
	})

	for cycle := 0; ; cycle++ {
		select {
		case <-ctx.Done():
			obs.Completed(ctx, sessionEndFromSync(&s.result, opts.ProjectCode), ctx.Err())
			return &s.result, ctx.Err()
		default:
		}
		if cycle >= MaxRounds {
			err := fmt.Errorf("sync: exceeded %d rounds", MaxRounds)
			obs.Completed(ctx, sessionEndFromSync(&s.result, opts.ProjectCode), err)
			return &s.result, err
		}

		roundCtx := obs.RoundStarted(ctx, cycle)

		req, err := s.buildRequest(cycle)
		if err != nil {
			obs.RoundCompleted(roundCtx, cycle, 0, 0)
			obs.Completed(ctx, sessionEndFromSync(&s.result, opts.ProjectCode), err)
			return &s.result, fmt.Errorf("sync: buildRequest round %d: %w", cycle, err)
		}
		resp, err := t.Exchange(ctx, req)
		if err != nil {
			obs.RoundCompleted(roundCtx, cycle, 0, 0)
			obs.Completed(ctx, sessionEndFromSync(&s.result, opts.ProjectCode), err)
			return &s.result, fmt.Errorf("sync: exchange round %d: %w", cycle, err)
		}

		sentBefore := s.result.FilesSent
		recvdBefore := s.result.FilesRecvd

		done, err := s.processResponse(resp)
		if err != nil {
			obs.RoundCompleted(roundCtx, cycle, s.result.FilesSent-sentBefore, s.result.FilesRecvd-recvdBefore)
			obs.Completed(ctx, sessionEndFromSync(&s.result, opts.ProjectCode), err)
			return &s.result, fmt.Errorf("sync: processResponse round %d: %w", cycle, err)
		}
		s.result.Rounds = cycle + 1

		obs.RoundCompleted(roundCtx, cycle, s.result.FilesSent-sentBefore, s.result.FilesRecvd-recvdBefore)

		if done {
			break
		}
	}

	obs.Completed(ctx, sessionEndFromSync(&s.result, opts.ProjectCode), nil)
	return &s.result, nil
}

// sessionEndFromSync builds a SessionEnd from a SyncResult.
func sessionEndFromSync(r *SyncResult, projectCode string) SessionEnd {
	return SessionEnd{
		Operation:    "sync",
		Rounds:       r.Rounds,
		FilesSent:    r.FilesSent,
		FilesRecvd:   r.FilesRecvd,
		UVFilesSent:  r.UVFilesSent,
		UVFilesRecvd: r.UVFilesRecvd,
		UVGimmesSent: r.UVGimmesSent,
		ProjectCode:  projectCode,
		Errors:       r.Errors,
	}
}

// cardSummary returns a short string describing a card (for trace logging).
func cardSummary(c xfer.Card) string {
	switch v := c.(type) {
	case *xfer.PullCard:
		return fmt.Sprintf("srv=%s proj=%s", v.ServerCode[:8], v.ProjectCode[:8])
	case *xfer.PushCard:
		return fmt.Sprintf("srv=%s proj=%s", v.ServerCode[:8], v.ProjectCode[:8])
	case *xfer.IGotCard:
		return v.UUID[:16] + "..."
	case *xfer.GimmeCard:
		return v.UUID[:16] + "..."
	case *xfer.FileCard:
		return fmt.Sprintf("uuid=%s len=%d delta=%s", v.UUID[:16], len(v.Content), v.DeltaSrc)
	case *xfer.CFileCard:
		return fmt.Sprintf("uuid=%s len=%d delta=%s", v.UUID[:16], len(v.Content), v.DeltaSrc)
	case *xfer.LoginCard:
		return fmt.Sprintf("user=%s", v.User)
	case *xfer.CookieCard:
		return v.Value
	case *xfer.ErrorCard:
		return v.Message
	case *xfer.MessageCard:
		return v.Message
	case *xfer.PragmaCard:
		return v.Name
	default:
		return ""
	}
}

// cardsByType is a helper that filters cards by type for testing/debugging.
func cardsByType(msg *xfer.Message, ct xfer.CardType) []xfer.Card {
	var out []xfer.Card
	for _, c := range msg.Cards {
		if c.Type() == ct {
			out = append(out, c)
		}
	}
	return out
}
