package main

import (
	"github.com/alecthomas/kong"
	_ "github.com/danmestas/libfossil/db/driver/modernc"
)

func main() {
	var c CLI
	ctx := kong.Parse(&c,
		kong.Name("edgesync"),
		kong.Description("EdgeSync — Fossil repo operations, NATS sync, and bridge"),
		kong.UsageOnError(),
	)
	err := ctx.Run(&c.Globals)
	ctx.FatalIfErrorf(err)
}
