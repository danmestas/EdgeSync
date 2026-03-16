package main

import "github.com/alecthomas/kong"

func main() {
	var cli CLI
	ctx := kong.Parse(&cli,
		kong.Name("edgesync"),
		kong.Description("EdgeSync — Fossil repo operations, NATS sync, and bridge"),
		kong.UsageOnError(),
	)
	err := ctx.Run(&cli.Globals)
	ctx.FatalIfErrorf(err)
}
