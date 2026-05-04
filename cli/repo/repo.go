// Package repo exposes a kong-compatible command tree for fossil repository
// operations (ci, co, timeline, diff, cat, config, merge, …) that consumers
// can embed in their CLI struct without importing libfossil.
//
// # Usage
//
// Replace this in your CLI:
//
//	import libfossilcli "github.com/danmestas/libfossil/cli"
//
//	type CLI struct {
//		libfossilcli.Globals
//		Repo libfossilcli.RepoCmd `cmd:"" group:"repo" help:"Fossil repository operations"`
//	}
//
// with this:
//
//	import repocli "github.com/danmestas/EdgeSync/cli/repo"
//
//	type CLI struct {
//		repocli.Globals
//		Repo repocli.Cmd `cmd:"" group:"repo" help:"Fossil repository operations"`
//	}
//
// The flag surface, subcommand set, and behavior are identical — every
// subcommand libfossil/cli.RepoCmd exposes today is reachable through
// repocli.Cmd with the same UX.
//
// # Trade-off: type aliases
//
// Cmd and Globals are type aliases for their libfossil/cli counterparts.
// Re-declaring them as wrapper structs would require mirroring ~38
// subcommands' worth of kong flags by hand, brittle to libfossil flag
// additions/renames. Aliases give consumers source-level isolation
// (no "github.com/danmestas/libfossil/cli" import needed in consumer code)
// while reusing libfossil's tested CLI implementation.
//
// Because aliases resolve to the same type, the libfossil/cli package is
// reachable transitively through Go's type identity, but consumer source
// code never names it. This is the minimum-viable shim that achieves the
// architectural goal: drop the libfossil import from downstream consumers
// once they've migrated to the new agent and hub APIs.
package repo

import (
	libfossilcli "github.com/danmestas/libfossil/cli"
)

// Globals holds flags shared by all repo subcommands (-R repo path, -v
// verbose). Re-export of libfossil/cli.Globals so consumers can write
// `repocli.Globals` instead of importing libfossil/cli.
type Globals = libfossilcli.Globals

// Cmd is the embeddable command tree for fossil repo operations. Re-export
// of libfossil/cli.RepoCmd. Embed it in your CLI struct with the kong tag
// `cmd:""` and your binary picks up the full fossil repo subcommand set
// (ci, co, timeline, diff, cat, config, merge, …).
type Cmd = libfossilcli.RepoCmd
