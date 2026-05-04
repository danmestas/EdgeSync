package notify

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"strings"
	"sync"
	"time"

	libfossil "github.com/danmestas/libfossil"
	"github.com/nats-io/nats.go"
)

// ServiceConfig holds configuration for the notify Service.
type ServiceConfig struct {
	Repo     *libfossil.Repo // Already-opened notify repo
	NATSConn *nats.Conn      // May be nil for repo-only mode
	From     string          // iroh endpoint ID
	FromName string          // Human-readable name
}

// Service holds *libfossil.Repo + *nats.Conn + Subscriber directly.
type Service struct {
	repo      *libfossil.Repo
	conn      *nats.Conn
	sub       *Subscriber
	config    ServiceConfig
	threadMap map[string]string // thread-short -> full thread ID cache
	mu        sync.Mutex
	ownsRepo  bool // true when the Service was constructed via NewServiceFromPath
}

// NewService creates a new notify Service. Repo must be non-nil.
// Does NOT close the repo — caller owns it.
func NewService(cfg ServiceConfig) (*Service, error) {
	if cfg.Repo == nil {
		return nil, fmt.Errorf("notify: ServiceConfig.Repo must not be nil")
	}
	var sub *Subscriber
	if cfg.NATSConn != nil {
		sub = NewSubscriber(cfg.NATSConn)
	}
	return &Service{
		repo:      cfg.Repo,
		conn:      cfg.NATSConn,
		sub:       sub,
		config:    cfg,
		threadMap: make(map[string]string),
	}, nil
}

// ServiceFromPathConfig holds configuration for NewServiceFromPath.
type ServiceFromPathConfig struct {
	RepoPath string     // notify.fossil path; created if absent
	NATSConn *nats.Conn // May be nil for repo-only mode
	From     string     // iroh endpoint ID
	FromName string     // Human-readable name
}

// NewServiceFromPath creates a Service that opens (or creates) the notify
// repo at the given path. Unlike NewService — which expects a pre-opened
// repo and leaves ownership with the caller — this constructor owns the
// repo lifecycle: Close() will close it.
//
// If the repo already exists at RepoPath, it's opened. If not, it's
// created via InitNotifyRepo.
func NewServiceFromPath(cfg ServiceFromPathConfig) (*Service, error) {
	if cfg.RepoPath == "" {
		return nil, fmt.Errorf("notify: ServiceFromPathConfig.RepoPath is required")
	}
	repo, err := libfossil.Open(cfg.RepoPath)
	if err != nil {
		repo, err = InitNotifyRepo(cfg.RepoPath)
		if err != nil {
			return nil, fmt.Errorf("notify: open or create notify repo at %s: %w", cfg.RepoPath, err)
		}
	}
	s, err := NewService(ServiceConfig{
		Repo:     repo,
		NATSConn: cfg.NATSConn,
		From:     cfg.From,
		FromName: cfg.FromName,
	})
	if err != nil {
		repo.Close()
		return nil, err
	}
	s.ownsRepo = true
	return s, nil
}

// Close unsubscribes from NATS. When the Service was constructed via
// NewServiceFromPath, also closes the underlying repo. When constructed
// via NewService, the repo is left open (caller retains ownership).
func (s *Service) Close() error {
	if s.sub != nil {
		s.sub.Unsubscribe()
	}
	if s.ownsRepo && s.repo != nil {
		err := s.repo.Close()
		s.repo = nil
		if err != nil {
			return fmt.Errorf("notify: close repo: %w", err)
		}
	}
	return nil
}

// Repo returns the underlying libfossil repo handle.
//
// New code should prefer the libfossil-hidden Service methods
// (ListThreads, ReadThread, MessageCount, ListDevices, AddDevice,
// RemoveDevice, CreatePairingToken, ValidateToken) which take/return
// only stdlib types or types defined in this package. Repo() stays
// available for sim/test harnesses and advanced consumers that need
// libfossil features the Service surface doesn't yet cover.
func (s *Service) Repo() *libfossil.Repo {
	return s.repo
}

// SendOpts are the options for sending a message.
type SendOpts struct {
	Project     string
	Body        string
	Priority    Priority
	Actions     []Action
	Media       []string
	ThreadShort string // if set, send to existing thread
}

// Send creates a message, commits to repo, publishes to NATS (best-effort).
func (s *Service) Send(opts SendOpts) (Message, error) {
	msg := NewMessage(MessageOpts{
		Project:  opts.Project,
		From:     s.config.From,
		FromName: s.config.FromName,
		Body:     opts.Body,
		Priority: opts.Priority,
		Actions:  opts.Actions,
		Media:    opts.Media,
	})

	if opts.ThreadShort != "" {
		threadID, err := s.resolveThread(opts.Project, opts.ThreadShort)
		if err != nil {
			return Message{}, fmt.Errorf("notify: resolve thread %q: %w", opts.ThreadShort, err)
		}
		msg.Thread = threadID
	}

	// Commit to repo.
	if err := CommitMessage(s.repo, msg); err != nil {
		return Message{}, fmt.Errorf("notify: commit: %w", err)
	}

	// Cache thread mapping.
	s.mu.Lock()
	s.threadMap[msg.ThreadShort()] = msg.Thread
	s.mu.Unlock()

	// Publish to NATS (best-effort — nil conn means repo-only mode).
	if s.conn != nil {
		if err := Publish(s.conn, msg); err != nil {
			slog.Warn("notify: NATS publish failed (message committed to repo)", "error", err, "msg_id", msg.ID)
		}
	}

	return msg, nil
}

// WatchOpts are the options for watching messages.
type WatchOpts struct {
	Project     string
	ThreadShort string
}

// Watch returns a channel of messages, closed when ctx is cancelled.
//
// Teardown is mutex-gated: every callback dispatch holds `mu` while
// it does its select, and the close goroutine takes `mu` before
// `close(ch)`. Neither Unsubscribe nor Drain synchronizes with NATS'
// per-subscription dispatch goroutine — both can return while a
// callback is mid-select. Without the mutex, a callback that already
// committed to `case ch <- msg` would race the close and panic on a
// closed channel (#100). The `closed` flag keeps callbacks scheduled
// after teardown from re-entering the select once ch is closed; in
// practice Unsubscribe stops most of them, the mutex and flag handle
// the residual.
func (s *Service) Watch(ctx context.Context, opts WatchOpts) <-chan Message {
	ch := make(chan Message, 16)

	if s.sub == nil {
		close(ch)
		return ch
	}

	subject := "notify." + opts.Project
	if opts.ThreadShort != "" {
		subject += "." + opts.ThreadShort
	} else {
		subject += ".*"
	}

	var (
		mu     sync.Mutex
		closed bool
	)

	natsSub, subErr := s.sub.subscribeForWatch(subject, func(msg Message) {
		mu.Lock()
		defer mu.Unlock()
		if closed {
			return
		}
		select {
		case ch <- msg:
		case <-ctx.Done():
		}
	})

	if subErr != nil {
		close(ch)
		return ch
	}

	go func() {
		<-ctx.Done()
		_ = natsSub.Unsubscribe()
		mu.Lock()
		closed = true
		close(ch)
		mu.Unlock()
	}()

	return ch
}

// resolveThread finds the full thread ID from a short ID.
// Checks cache first, then scans repo threads.
func (s *Service) resolveThread(project, threadShort string) (string, error) {
	s.mu.Lock()
	if full, ok := s.threadMap[threadShort]; ok {
		s.mu.Unlock()
		return full, nil
	}
	s.mu.Unlock()

	// Scan repo for matching thread.
	files, err := allFileNames(s.repo)
	if err != nil {
		return "", err
	}

	prefix := project + "/threads/" + threadShort + "/"
	for _, f := range files {
		if strings.HasPrefix(f, prefix) && strings.HasSuffix(f, ".json") {
			data, err := readFileContent(s.repo, f)
			if err != nil {
				continue
			}
			var msg Message
			if err := json.Unmarshal(data, &msg); err != nil {
				continue
			}
			s.mu.Lock()
			s.threadMap[threadShort] = msg.Thread
			s.mu.Unlock()
			return msg.Thread, nil
		}
	}

	return "", fmt.Errorf("thread %q not found in project %q", threadShort, project)
}

// FormatWatchLine formats a message for CLI output.
// Action: [<timestamp>] thread:<short> from:<name> action:<body>
// Text:   [<timestamp>] thread:<short> from:<name> text:<body>
func FormatWatchLine(msg Message) string {
	ts := msg.Timestamp.UTC().Format(time.RFC3339)
	threadShort := shortID(msg.Thread, "thread-")
	if msg.ActionResponse {
		return fmt.Sprintf("[%s] thread:%s from:%s action:%s", ts, threadShort, msg.FromName, msg.Body)
	}
	return fmt.Sprintf("[%s] thread:%s from:%s text:%s", ts, threadShort, msg.FromName, msg.Body)
}
