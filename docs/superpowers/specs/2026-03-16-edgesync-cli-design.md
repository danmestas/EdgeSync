# EdgeSync Unified CLI

**Date:** 2026-03-16
**Status:** Draft
**Scope:** `cmd/edgesync/` — single binary for repo ops, sync agent, and bridge

## Problem

The project has three separate entry points (`cmd/leaf/main.go`, `cmd/bridge/main.go`, and ad-hoc repo operations via tests). There's no unified CLI for developers, admins, or platform builders to manage repos, inspect content, run sync, and operate the bridge.

## Goals

Build `edgesync`, a single binary that:
1. Provides grouped subcommands for repo operations, sync, and bridge
2. Uses kong for struct-tag-based command dispatch
3. Wraps existing go-libfossil packages — no new domain logic
4. Replaces the need for the `fossil` binary for basic repo operations
5. Becomes the primary entry point for the EdgeSync stack

## Non-Goals

- Replacing `fossil` for merge, diff, annotate, or blame (Phase 3+)
- Checkout DB staging operations (add/rm — Phase 2)
- Web UI or TUI

## Command Structure

```
edgesync repo new <path>              Create a new repository
edgesync repo ci -m "..."             Checkin file changes
edgesync repo co [<version>]          Checkout a version
edgesync repo ls [<version>]          List files in a version
edgesync repo timeline [-n 20]        Show repository history
edgesync repo cat <artifact>          Output artifact content
edgesync repo info                    Repository statistics

edgesync sync start                   Start leaf agent daemon
edgesync sync now                     Trigger immediate sync

edgesync bridge serve                 Start NATS-to-Fossil bridge
```

## Architecture

### Framework: Kong

Kong uses struct tags for command definitions. The CLI is a tree of Go structs, each with a `Run()` method. Kong handles parsing, help generation, shell completion, and validation.

### File Layout

```
cmd/edgesync/
    main.go              Entry point: kong.Parse + Run
    cli.go               CLI struct tree (Globals, RepoCmd, SyncCmd, BridgeCmd)
    repo_new.go          RepoNewCmd
    repo_ci.go           RepoCiCmd
    repo_co.go           RepoCoCmd
    repo_ls.go           RepoLsCmd
    repo_timeline.go     RepoTimelineCmd
    repo_cat.go          RepoCatCmd
    repo_info.go         RepoInfoCmd
    sync_start.go        SyncStartCmd
    sync_now.go          SyncNowCmd
    bridge_serve.go      BridgeServeCmd
```

One file per command. Each command struct is small (20-60 lines) and delegates to go-libfossil packages.

### Struct Tree

```go
type CLI struct {
    Globals

    Repo    RepoCmd    `cmd:"" help:"Repository operations"`
    Sync    SyncCmd    `cmd:"" help:"Leaf agent sync"`
    Bridge  BridgeCmd  `cmd:"" help:"NATS-to-Fossil bridge"`
}

type Globals struct {
    Repo     string `short:"R" help:"Path to repository file" type:"path"`
    Checkout string `short:"C" help:"Path to checkout directory" type:"path"`
    Verbose  bool   `short:"v" help:"Verbose output"`
}

type RepoCmd struct {
    New      RepoNewCmd      `cmd:"" help:"Create a new repository"`
    Ci       RepoCiCmd       `cmd:"" help:"Checkin file changes"`
    Co       RepoCoCmd       `cmd:"" help:"Checkout a version"`
    Ls       RepoLsCmd       `cmd:"" help:"List files in a version"`
    Timeline RepoTimelineCmd `cmd:"" help:"Show repository history"`
    Cat      RepoCatCmd      `cmd:"" help:"Output artifact content"`
    Info     RepoInfoCmd     `cmd:"" help:"Repository statistics"`
}

type SyncCmd struct {
    Start SyncStartCmd `cmd:"" help:"Start leaf agent daemon"`
    Now   SyncNowCmd   `cmd:"" help:"Trigger immediate sync"`
}

type BridgeCmd struct {
    Serve BridgeServeCmd `cmd:"" help:"Start NATS-to-Fossil bridge"`
}
```

## Shared Helpers

### resolveRID

Multiple commands accept a version string (UUID, UUID prefix, or symbolic name) but the go-libfossil APIs take `libfossil.FslID` (integer rid). A shared helper resolves this:

```go
// resolveRID resolves a version string to an rid.
// Accepts: full UUID, UUID prefix (minimum 4 chars), or empty string (tip).
func resolveRID(r *repo.Repo, version string) (libfossil.FslID, error) {
    if version == "" {
        // Resolve tip: most recent checkin
        var rid int64
        err := r.DB().QueryRow(
            "SELECT objid FROM event WHERE type='ci' ORDER BY mtime DESC LIMIT 1",
        ).Scan(&rid)
        if err != nil {
            return 0, fmt.Errorf("no checkins found")
        }
        return libfossil.FslID(rid), nil
    }
    // UUID or prefix lookup
    var rid int64
    err := r.DB().QueryRow(
        "SELECT rid FROM blob WHERE uuid LIKE ?", version+"%",
    ).Scan(&rid)
    if err != nil {
        return 0, fmt.Errorf("artifact %q not found", version)
    }
    return libfossil.FslID(rid), nil
}
```

Used by: `repo ls`, `repo co`, `repo cat`, `repo timeline`.

### openRepo

Opens a repo from the `-R` flag. All go-libfossil query APIs (`content.Expand`, `blob.Load`, `blob.Exists`, `manifest.ListFiles`, `manifest.Log`) take `db.Querier` or `*repo.Repo`. The querier is obtained via `r.DB()`.

```go
func openRepo(globals *Globals) (*repo.Repo, error) {
    if globals.Repo == "" {
        return nil, fmt.Errorf("no repository specified (use -R)")
    }
    return repo.Open(globals.Repo)
}
```

## Command Specifications

### repo new

```go
type RepoNewCmd struct {
    Path string `arg:"" help:"Path for new repository file"`
    User string `help:"Default user name" default:""`
}
```

Creates a new Fossil repository via `repo.Create(path, user, simio.CryptoRand{})`. If user is empty, uses the OS username.

### repo ci

```go
type RepoCiCmd struct {
    Message string   `short:"m" required:"" help:"Checkin comment"`
    Files   []string `arg:"" required:"" help:"Files to checkin"`
    User    string   `help:"Checkin user (default: repo's default user)"`
    Parent  string   `help:"Parent version UUID (default: tip)"`
}
```

The `Run()` implementation:
1. Opens repo via `openRepo()`
2. Resolves parent rid via `resolveRID()` (defaults to tip if not specified)
3. Reads each file path from disk into `manifest.File{Name, Content, Perm}` structs
4. Calls `manifest.Checkin(r, manifest.CheckinOpts{Files, Comment, User, Parent, Time: time.Now()})`

**Note:** `--branch` and `--tag` flags are deferred to Phase 2. The current `manifest.Checkin` API hardcodes "trunk" for initial checkins and does not support custom branch/tag cards. These require extending the manifest package first.

### repo co

```go
type RepoCoCmd struct {
    Version string `arg:"" optional:"" help:"Version to checkout (default: tip)"`
    Force   bool   `help:"Overwrite modified files"`
}
```

Resolves version via `resolveRID()`, reads the manifest via `manifest.GetManifest(r, rid)`, iterates files, extracts each via `content.Expand(r.DB(), fileRid)`, writes to checkout directory (from `-C` flag or CWD).

### repo ls

```go
type RepoLsCmd struct {
    Version string `arg:"" optional:"" help:"Version to list (default: tip)"`
    Long    bool   `short:"l" help:"Show sizes and hashes"`
}
```

Resolves version via `resolveRID()`, calls `manifest.ListFiles(r, rid)`. Long format shows UUID and size per file.

### repo timeline

```go
type RepoTimelineCmd struct {
    Limit int `short:"n" default:"20" help:"Number of entries"`
}
```

Resolves tip rid via `resolveRID(r, "")`, calls `manifest.Log(r, manifest.LogOpts{Start: tipRid, Limit: n})`. Formats each entry with UUID (first 10 chars), user, date, and comment.

### repo cat

```go
type RepoCatCmd struct {
    Artifact string `arg:"" help:"Artifact UUID or prefix"`
    Raw      bool   `help:"Output raw blob (no delta expansion)"`
}
```

Resolves UUID via `resolveRID()`. Raw mode calls `blob.Load(r.DB(), rid)`, normal mode calls `content.Expand(r.DB(), rid)`. Output to stdout.

### repo info

```go
type RepoInfoCmd struct{}
```

Queries repo stats: blob count, total uncompressed size, delta count, phantom count, project-code, server-code. Pure SQL against `blob`, `delta`, `phantom`, `config` tables.

### sync start

```go
type SyncStartCmd struct {
    NATSUrl      string        `help:"NATS server URL" default:"nats://localhost:4222"`
    PollInterval time.Duration `help:"Sync poll interval" default:"5s"`
    Push         bool          `help:"Enable push" default:"true" negatable:""`
    Pull         bool          `help:"Enable pull" default:"true" negatable:""`
}
```

Creates `agent.New()` with `agent.Config{RepoPath: globals.Repo, NATSUrl, PollInterval, Push, Pull}`, calls `Start()`, blocks on signal (SIGINT/SIGTERM).

**Note:** `agent.Config.applyDefaults()` flips both Push and Pull to true when both are false. The `Run()` implementation should pass Push/Pull directly to the config without relying on defaulting — kong handles the defaults via struct tags.

This is the first real entry point using `agent.New()` (the existing `cmd/leaf/main.go` is a stub).

### sync now

```go
type SyncNowCmd struct {
    PID int `help:"PID of running agent to signal"`
}
```

Sends SIGUSR1 to the running agent process to trigger an immediate sync cycle.

### bridge serve

```go
type BridgeServeCmd struct {
    NATSUrl   string `help:"NATS server URL" default:"nats://localhost:4222"`
    FossilURL string `required:"" help:"Fossil server HTTP URL"`
    Project   string `required:"" help:"Project code for NATS subject"`
}
```

Creates `bridge.New()` with `bridge.Config{NATSUrl, FossilURL, ProjectCode: Project}`, calls `Start()`, blocks on signal (SIGINT/SIGTERM).

Note: maps `BridgeServeCmd.Project` to `bridge.Config.ProjectCode`. This is the first real entry point using `bridge.New()` (the existing `cmd/bridge/main.go` is a stub).

## Global Flags

`-R <path>` — explicitly specify repository file. If omitted, search upward from CWD for `.fslckout` or `_FOSSIL_` (matching fossil's convention).

`-C <path>` — specify checkout directory. Defaults to CWD.

`-v` — verbose output (log sync rounds, SQL queries, etc).

## Repo Discovery

When `-R` is not provided, the CLI searches for the repo:
1. Look for `.fslckout` or `_FOSSIL_` in CWD
2. Walk parent directories until found or root reached
3. If found, read the repo path from the checkout DB
4. If not found, error with "no repository found (use -R to specify)"

This is the same algorithm fossil uses.

## Dependencies

- `github.com/alecthomas/kong` — CLI framework
- Existing go-libfossil packages (repo, manifest, content, blob, hash, db)
- Existing leaf/agent and bridge/bridge packages

## Backward Compatibility

`cmd/leaf/main.go` and `cmd/bridge/main.go` remain as-is. `cmd/edgesync/` is a new binary that wraps them. Eventually the old entry points can be deprecated.

## Build

```bash
go build ./cmd/edgesync/
# Produces: ./edgesync

# Usage:
./edgesync repo new myrepo.fossil
./edgesync -R myrepo.fossil repo ls
./edgesync -R myrepo.fossil repo timeline -n 5
./edgesync sync start -R myrepo.fossil --nats-url nats://localhost:4222
./edgesync bridge serve --fossil-url http://localhost:8080 --project abc123
```
