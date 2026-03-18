package main

import (
	"context"
	"fmt"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/sync"
)

type RepoCloneCmd struct {
	URL  string `arg:"" help:"Remote Fossil server URL"`
	Path string `arg:"" help:"Local path for new repository file"`
	User string `short:"u" help:"Username for clone auth"`
	Pass string `short:"p" help:"Password for clone auth"`
}

func (c *RepoCloneCmd) Run(g *Globals) error {
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
