package main

import (
	"context"
	"fmt"
	"time"

	"github.com/danmestas/go-libfossil/auth"
	"github.com/danmestas/go-libfossil/sync"
)

type RepoCloneCmd struct {
	URL    string `arg:"" optional:"" help:"Remote Fossil server URL"`
	Path   string `arg:"" optional:"" help:"Local path for new repository file"`
	User   string `short:"u" help:"Username for clone auth"`
	Pass   string `short:"p" help:"Password for clone auth"`
	Invite string `help:"Invite token (from edgesync invite)"`
}

func (c *RepoCloneCmd) Run(g *Globals) error {
	if c.Invite != "" {
		token, err := auth.DecodeInviteToken(c.Invite)
		if err != nil {
			return fmt.Errorf("invalid invite token: %w", err)
		}
		if c.URL == "" {
			c.URL = token.URL
		}
		if c.User == "" {
			c.User = token.Login
		}
		if c.Pass == "" {
			c.Pass = token.Password
		}
	}
	if c.URL == "" {
		return fmt.Errorf("URL required (provide as argument or via --invite token)")
	}
	if c.Path == "" {
		return fmt.Errorf("path required")
	}

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Minute)
	defer cancel()

	transport := &sync.HTTPTransport{URL: c.URL}
	opts := sync.CloneOpts{
		User:     c.User,
		Password: c.Pass,
	}

	r, result, err := sync.Clone(ctx, c.Path, transport, opts)
	if err != nil {
		return fmt.Errorf("clone failed: %w", err)
	}
	defer r.Close()

	fmt.Printf("Cloned into %s\n", c.Path)
	fmt.Printf("  Rounds:       %d\n", result.Rounds)
	fmt.Printf("  Blobs:        %d\n", result.BlobsRecvd)
	fmt.Printf("  Project-code: %s\n", result.ProjectCode)
	return nil
}
