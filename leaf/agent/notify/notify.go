package notify

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"strings"
	"sync"
	"time"

	libfossil "github.com/danmestas/go-libfossil"
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

// Close unsubscribes from NATS. Does NOT close the repo.
func (s *Service) Close() error {
	if s.sub != nil {
		s.sub.Unsubscribe()
	}
	return nil
}

// Repo returns the underlying Fossil repo.
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
// Teardown order is subscription-first, channel-close-second: when ctx
// cancels the close goroutine Unsubscribes from NATS before close(ch),
// so no late-arriving callback can race the close and panic on a
// closed channel. Without this ordering, a message arriving between
// ctx.Done and the channel-close window would trigger a send on a
// closed channel.
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

	natsSub, subErr := s.sub.subscribeForWatch(subject, func(msg Message) {
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
		close(ch)
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
