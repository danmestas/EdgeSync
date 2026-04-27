package main

import (
	edgecli "github.com/danmestas/EdgeSync/cli"
	"github.com/danmestas/libfossil/cli"
)

type CLI struct {
	cli.Globals

	Repo   cli.RepoCmd       `cmd:"" help:"Repository operations"`
	Sync   edgecli.SyncCmd   `cmd:"" help:"Leaf agent sync"`
	Bridge edgecli.BridgeCmd `cmd:"" help:"NATS-to-Fossil bridge"`
	Notify edgecli.NotifyCmd `cmd:"" help:"Bidirectional notification messaging"`
	Doctor edgecli.DoctorCmd `cmd:"" help:"Check development environment health"`
}
