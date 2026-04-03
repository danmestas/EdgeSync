package main

import (
	"github.com/danmestas/go-libfossil/cli"
)

type CLI struct {
	cli.Globals

	Repo   cli.RepoCmd `cmd:"" help:"Repository operations"`
	Sync   SyncCmd     `cmd:"" help:"Leaf agent sync"`
	Bridge BridgeCmd   `cmd:"" help:"NATS-to-Fossil bridge"`
	Doctor DoctorCmd   `cmd:"" help:"Check development environment health"`
}

type SyncCmd struct {
	Start SyncStartCmd `cmd:"" help:"Start leaf agent daemon"`
	Now   SyncNowCmd   `cmd:"" help:"Trigger immediate sync"`
}

type BridgeCmd struct {
	Serve BridgeServeCmd `cmd:"" help:"Start NATS-to-Fossil bridge"`
}
