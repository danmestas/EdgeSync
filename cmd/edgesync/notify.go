package main

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	libfossil "github.com/danmestas/go-libfossil"
	"github.com/danmestas/go-libfossil/cli"
	"github.com/dmestas/edgesync/leaf/agent/notify"
	"github.com/nats-io/nats.go"
)

// NotifyCmd is the top-level "notify" command group.
type NotifyCmd struct {
	Init    NotifyInitCmd    `cmd:"" help:"Initialize the notify repo"`
	Send    NotifySendCmd    `cmd:"" help:"Send a notification message"`
	Ask     NotifyAskCmd     `cmd:"" help:"Send a message and wait for a reply"`
	Watch   NotifyWatchCmd   `cmd:"" help:"Watch for incoming messages"`
	Threads NotifyThreadsCmd `cmd:"" help:"List active threads"`
	Log     NotifyLogCmd     `cmd:"" help:"Show thread message history"`
	Status  NotifyStatusCmd  `cmd:"" help:"Show connection state and unread counts"`
}

// notifyRepoPath returns the path to notify.fossil next to the -R repo.
func notifyRepoPath(g *cli.Globals) string {
	dir := filepath.Dir(g.Repo)
	return filepath.Join(dir, "notify.fossil")
}

// openNotifyRepo opens the notify.fossil repo with a clear error if missing.
func openNotifyRepo(g *cli.Globals) (*libfossil.Repo, error) {
	path := notifyRepoPath(g)
	if _, err := os.Stat(path); os.IsNotExist(err) {
		return nil, fmt.Errorf("notify repo not found at %s (run: edgesync notify init -R <repo>)", path)
	}
	r, err := libfossil.Open(path)
	if err != nil {
		return nil, fmt.Errorf("open notify repo: %w", err)
	}
	return r, nil
}

// connectNATS optionally connects to a NATS server. Returns nil conn if url is empty.
func connectNATS(url string) (*nats.Conn, error) {
	if url == "" {
		return nil, nil
	}
	nc, err := nats.Connect(url,
		nats.Name("edgesync-notify-cli"),
		nats.Timeout(5*time.Second),
	)
	if err != nil {
		return nil, fmt.Errorf("connect to NATS %s: %w", url, err)
	}
	return nc, nil
}

// parseActions splits a comma-separated string into Action slices.
func parseActions(s string) []notify.Action {
	if s == "" {
		return nil
	}
	parts := strings.Split(s, ",")
	actions := make([]notify.Action, len(parts))
	for i, p := range parts {
		p = strings.TrimSpace(p)
		actions[i] = notify.Action{
			ID:    strings.ToLower(strings.ReplaceAll(p, " ", "_")),
			Label: p,
		}
	}
	return actions
}

// --- Init ---

type NotifyInitCmd struct{}

func (c *NotifyInitCmd) Run(g *cli.Globals) error {
	if g.Repo == "" {
		return fmt.Errorf("repository required (use -R <path>)")
	}
	path := notifyRepoPath(g)
	r, err := notify.InitNotifyRepo(path)
	if err != nil {
		return fmt.Errorf("init notify repo: %w", err)
	}
	r.Close()
	fmt.Println(path)
	return nil
}

// --- Send ---

type NotifySendCmd struct {
	Project  string `help:"Project code" required:""`
	Thread   string `help:"Existing thread short ID (omit to start a new thread)"`
	Actions  string `help:"Comma-separated action labels"`
	Priority string `help:"Priority level (info, action_required, urgent)" default:"info" enum:"info,action_required,urgent"`
	NATS     string `help:"NATS server URL (enables real-time delivery)" env:"EDGESYNC_NATS"`
	Body     string `arg:"" help:"Message body"`
}

func (c *NotifySendCmd) Run(g *cli.Globals) error {
	if g.Repo == "" {
		return fmt.Errorf("repository required (use -R <path>)")
	}
	r, err := openNotifyRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	nc, err := connectNATS(c.NATS)
	if err != nil {
		return err
	}
	if nc != nil {
		defer nc.Close()
	}

	svc, err := notify.NewService(notify.ServiceConfig{
		Repo:     r,
		NATSConn: nc,
	})
	if err != nil {
		return err
	}
	defer svc.Close()

	msg, err := svc.Send(notify.SendOpts{
		Project:     c.Project,
		Body:        c.Body,
		Priority:    notify.Priority(c.Priority),
		Actions:     parseActions(c.Actions),
		ThreadShort: c.Thread,
	})
	if err != nil {
		return err
	}

	fmt.Fprintf(os.Stderr, "thread:%s\n", msg.ThreadShort())
	fmt.Println(msg.ID)
	return nil
}

// --- Ask ---

type NotifyAskCmd struct {
	Project  string        `help:"Project code" required:""`
	Thread   string        `help:"Existing thread short ID (omit to start a new thread)"`
	Actions  string        `help:"Comma-separated action labels"`
	Priority string        `help:"Priority level (info, action_required, urgent)" default:"action_required" enum:"info,action_required,urgent"`
	Timeout  time.Duration `help:"Timeout waiting for reply (0 = forever)" default:"0s"`
	NATS     string        `help:"NATS server URL (required for receiving replies)" env:"EDGESYNC_NATS"`
	Body     string        `arg:"" help:"Message body"`
}

func (c *NotifyAskCmd) Run(g *cli.Globals) error {
	if g.Repo == "" {
		return fmt.Errorf("repository required (use -R <path>)")
	}
	r, err := openNotifyRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	nc, err := connectNATS(c.NATS)
	if err != nil {
		return err
	}
	if nc != nil {
		defer nc.Close()
	}

	svc, err := notify.NewService(notify.ServiceConfig{
		Repo:     r,
		NATSConn: nc,
	})
	if err != nil {
		return err
	}
	defer svc.Close()

	msg, err := svc.Send(notify.SendOpts{
		Project:     c.Project,
		Body:        c.Body,
		Priority:    notify.Priority(c.Priority),
		Actions:     parseActions(c.Actions),
		ThreadShort: c.Thread,
	})
	if err != nil {
		return err
	}

	fmt.Fprintf(os.Stderr, "thread:%s\n", msg.ThreadShort())

	// Watch for reply.
	ctx := context.Background()
	if c.Timeout > 0 {
		var cancel context.CancelFunc
		ctx, cancel = context.WithTimeout(ctx, c.Timeout)
		defer cancel()
	}

	ch := svc.Watch(ctx, notify.WatchOpts{
		Project:     c.Project,
		ThreadShort: msg.ThreadShort(),
	})

	reply, ok := <-ch
	if !ok {
		if c.Timeout > 0 {
			fmt.Fprintln(os.Stderr, "timeout waiting for reply")
			os.Exit(2)
		}
		// No NATS connection — channel closed immediately in repo-only mode.
		fmt.Fprintln(os.Stderr, "no NATS connection — watch not available in repo-only mode")
		return nil
	}

	fmt.Println(notify.FormatWatchLine(reply))
	return nil
}

// --- Watch ---

type NotifyWatchCmd struct {
	Project string `help:"Project code" required:""`
	Thread  string `help:"Filter to specific thread short ID"`
	NATS    string `help:"NATS server URL (required for real-time delivery)" env:"EDGESYNC_NATS"`
}

func (c *NotifyWatchCmd) Run(g *cli.Globals) error {
	if g.Repo == "" {
		return fmt.Errorf("repository required (use -R <path>)")
	}
	r, err := openNotifyRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	nc, err := connectNATS(c.NATS)
	if err != nil {
		return err
	}
	if nc != nil {
		defer nc.Close()
	}

	svc, err := notify.NewService(notify.ServiceConfig{
		Repo:     r,
		NATSConn: nc,
	})
	if err != nil {
		return err
	}
	defer svc.Close()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	ch := svc.Watch(ctx, notify.WatchOpts{
		Project:     c.Project,
		ThreadShort: c.Thread,
	})

	for msg := range ch {
		fmt.Println(notify.FormatWatchLine(msg))
	}

	// If channel closed immediately (no NATS), inform user.
	fmt.Fprintln(os.Stderr, "no NATS connection — watch not available in repo-only mode")
	return nil
}

// --- Threads ---

type NotifyThreadsCmd struct {
	Project string `help:"Project code" required:""`
}

func (c *NotifyThreadsCmd) Run(g *cli.Globals) error {
	if g.Repo == "" {
		return fmt.Errorf("repository required (use -R <path>)")
	}
	r, err := openNotifyRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	threads, err := notify.ListThreads(r, c.Project)
	if err != nil {
		return err
	}

	if len(threads) == 0 {
		fmt.Println("no threads")
		return nil
	}

	for _, t := range threads {
		ts := t.LastActivity.UTC().Format(time.RFC3339)
		fmt.Printf("%s  %s  msgs:%d  priority:%s  last:%s\n",
			t.ThreadShort, ts, t.MessageCount, t.Priority, truncate(t.LastMessage.Body, 60))
	}
	return nil
}

// --- Log ---

type NotifyLogCmd struct {
	Project string `help:"Project code" required:""`
	Thread  string `arg:"" help:"Thread short ID"`
}

func (c *NotifyLogCmd) Run(g *cli.Globals) error {
	if g.Repo == "" {
		return fmt.Errorf("repository required (use -R <path>)")
	}
	r, err := openNotifyRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	messages, err := notify.ReadThread(r, c.Project, c.Thread)
	if err != nil {
		return err
	}

	if len(messages) == 0 {
		fmt.Println("no messages")
		return nil
	}

	for _, msg := range messages {
		fmt.Println(notify.FormatWatchLine(msg))
	}
	return nil
}

// --- Status ---

type NotifyStatusCmd struct{}

func (c *NotifyStatusCmd) Run(g *cli.Globals) error {
	if g.Repo == "" {
		return fmt.Errorf("repository required (use -R <path>)")
	}
	path := notifyRepoPath(g)
	if _, err := os.Stat(path); os.IsNotExist(err) {
		fmt.Printf("notify repo: not initialized\n")
		fmt.Printf("  path: %s\n", path)
		fmt.Printf("  run: edgesync notify init -R <repo>\n")
		return nil
	}

	fmt.Printf("notify repo: %s\n", path)
	fmt.Printf("connection: repo-only (no NATS)\n")
	return nil
}

// truncate shortens a string to max length, adding "..." if truncated.
func truncate(s string, max int) string {
	if len(s) <= max {
		return s
	}
	return s[:max-3] + "..."
}
