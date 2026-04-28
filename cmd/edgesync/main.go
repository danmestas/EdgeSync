package main

import (
	"fmt"
	"os"

	"github.com/alecthomas/kong"
	_ "github.com/danmestas/libfossil/db/driver/modernc"
)

// Build-time variables, populated via -ldflags by GoReleaser.
var (
	version = "dev"
	commit  = "none"
	date    = "unknown"
)

func main() {
	// Handle --version / -V at the top level. Done before Kong.Parse so
	// it doesn't collide with sub-flags like `repo extract --version`.
	if len(os.Args) == 2 && (os.Args[1] == "--version" || os.Args[1] == "-V") {
		fmt.Printf("edgesync %s (commit %s, built %s)\n", version, commit, date)
		return
	}

	var c CLI
	ctx := kong.Parse(&c,
		kong.Name("edgesync"),
		kong.Description("EdgeSync — Fossil repo operations, NATS sync, and bridge"),
		kong.UsageOnError(),
	)
	err := ctx.Run(&c.Globals)
	ctx.FatalIfErrorf(err)
}
