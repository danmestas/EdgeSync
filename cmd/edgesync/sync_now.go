package main

import (
	"fmt"
	"syscall"

	"github.com/danmestas/go-libfossil/cli"
)

type SyncNowCmd struct {
	PID int `arg:"" help:"PID of running agent to signal"`
}

func (c *SyncNowCmd) Run(g *cli.Globals) error {
	if c.PID <= 0 {
		return fmt.Errorf("invalid PID: %d", c.PID)
	}
	return syscall.Kill(c.PID, syscall.SIGUSR1)
}
