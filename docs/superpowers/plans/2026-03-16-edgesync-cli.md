# EdgeSync Unified CLI Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `edgesync`, a single binary with kong-based grouped subcommands for repo operations (new, ci, co, ls, timeline, cat, info), sync agent (start, now), and bridge (serve).

**Architecture:** Kong struct-tag CLI with one file per command in `cmd/edgesync/`. Commands delegate to existing go-libfossil packages. Shared `resolveRID` and `openRepo` helpers handle repo discovery and version resolution.

**Tech Stack:** Go 1.23, `github.com/alecthomas/kong`, go-libfossil packages, leaf/agent, bridge/bridge

**Spec:** `docs/superpowers/specs/2026-03-16-edgesync-cli-design.md`

---

## File Structure

### New Files

| File | Responsibility |
|------|---------------|
| `cmd/edgesync/main.go` | Entry point: kong.Parse + ctx.Run |
| `cmd/edgesync/cli.go` | CLI struct tree, Globals, RepoCmd, SyncCmd, BridgeCmd, helpers (resolveRID, openRepo) |
| `cmd/edgesync/repo_new.go` | RepoNewCmd — create repository |
| `cmd/edgesync/repo_ci.go` | RepoCiCmd — checkin files |
| `cmd/edgesync/repo_co.go` | RepoCoCmd — checkout version |
| `cmd/edgesync/repo_ls.go` | RepoLsCmd — list files |
| `cmd/edgesync/repo_timeline.go` | RepoTimelineCmd — show history |
| `cmd/edgesync/repo_cat.go` | RepoCatCmd — dump artifact |
| `cmd/edgesync/repo_info.go` | RepoInfoCmd — repo statistics |
| `cmd/edgesync/sync_start.go` | SyncStartCmd — start leaf agent |
| `cmd/edgesync/sync_now.go` | SyncNowCmd — signal running agent |
| `cmd/edgesync/bridge_serve.go` | BridgeServeCmd — start bridge |

---

## Task 1: Kong dependency + main.go + cli.go scaffold

**Files:**
- Create: `cmd/edgesync/main.go`
- Create: `cmd/edgesync/cli.go`

- [ ] **Step 1: Add kong dependency**

Run: `go get github.com/alecthomas/kong@latest`

- [ ] **Step 2: Create main.go**

```go
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
```

- [ ] **Step 3: Create cli.go with struct tree and helpers**

```go
package main

import (
	"fmt"
	"os/user"
	"time"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

type CLI struct {
	Globals

	Repo   RepoCmd   `cmd:"" help:"Repository operations"`
	Sync   SyncCmd   `cmd:"" help:"Leaf agent sync"`
	Bridge BridgeCmd `cmd:"" help:"NATS-to-Fossil bridge"`
}

type Globals struct {
	Repo    string `short:"R" help:"Path to repository file" type:"path"`
	Verbose bool   `short:"v" help:"Verbose output"`
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

// openRepo opens the repository specified by -R flag.
func openRepo(g *Globals) (*repo.Repo, error) {
	if g.Repo == "" {
		return nil, fmt.Errorf("no repository specified (use -R <path>)")
	}
	return repo.Open(g.Repo)
}

// resolveRID resolves a version string to a rid.
// Empty string = tip (most recent checkin). Otherwise UUID or prefix.
func resolveRID(r *repo.Repo, version string) (libfossil.FslID, error) {
	if version == "" {
		var rid int64
		err := r.DB().QueryRow(
			"SELECT objid FROM event WHERE type='ci' ORDER BY mtime DESC LIMIT 1",
		).Scan(&rid)
		if err != nil {
			return 0, fmt.Errorf("no checkins found")
		}
		return libfossil.FslID(rid), nil
	}
	var rid int64
	err := r.DB().QueryRow(
		"SELECT rid FROM blob WHERE uuid LIKE ?", version+"%",
	).Scan(&rid)
	if err != nil {
		return 0, fmt.Errorf("artifact %q not found", version)
	}
	return libfossil.FslID(rid), nil
}

// currentUser returns the OS username for default checkin user.
func currentUser() string {
	if u, err := user.Current(); err == nil {
		return u.Username
	}
	return "anonymous"
}
```

Note: The command structs (`RepoNewCmd`, etc.) are defined in their own files. For now, create stubs so it compiles:

- [ ] **Step 4: Create stub command files**

Create each of the 10 command files with minimal structs and `Run` methods that return `nil`. Example for `repo_new.go`:

```go
package main

type RepoNewCmd struct {
	Path string `arg:"" help:"Path for new repository file"`
	User string `help:"Default user name" default:""`
}

func (c *RepoNewCmd) Run(g *Globals) error {
	return fmt.Errorf("not implemented")
}
```

Do the same for all 10 commands — just the struct + `Run` returning "not implemented". This lets the CLI scaffold compile and show help.

- [ ] **Step 5: Verify build and help output**

Run: `go build ./cmd/edgesync/ && ./edgesync --help`
Expected: Shows help with repo, sync, bridge groups.

Run: `./edgesync repo --help`
Expected: Shows new, ci, co, ls, timeline, cat, info subcommands.

- [ ] **Step 6: Commit**

```bash
git add cmd/edgesync/ go.mod go.sum
git commit -m "edgesync: scaffold CLI with kong, struct tree, and stub commands"
```

---

## Task 2: repo new + repo info

**Files:**
- Modify: `cmd/edgesync/repo_new.go`
- Modify: `cmd/edgesync/repo_info.go`

- [ ] **Step 1: Implement repo new**

```go
package main

import (
	"fmt"

	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
)

type RepoNewCmd struct {
	Path string `arg:"" help:"Path for new repository file"`
	User string `help:"Default user name" default:""`
}

func (c *RepoNewCmd) Run(g *Globals) error {
	user := c.User
	if user == "" {
		user = currentUser()
	}
	r, err := repo.Create(c.Path, user, simio.CryptoRand{})
	if err != nil {
		return err
	}
	r.Close()
	fmt.Printf("created repository: %s\n", c.Path)
	return nil
}
```

- [ ] **Step 2: Implement repo info**

```go
package main

import "fmt"

type RepoInfoCmd struct{}

func (c *RepoInfoCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	var blobCount, deltaCount, phantomCount int
	var totalSize int64
	var projectCode, serverCode string

	r.DB().QueryRow("SELECT count(*) FROM blob WHERE size >= 0").Scan(&blobCount)
	r.DB().QueryRow("SELECT count(*) FROM delta").Scan(&deltaCount)
	r.DB().QueryRow("SELECT count(*) FROM phantom").Scan(&phantomCount)
	r.DB().QueryRow("SELECT coalesce(sum(size),0) FROM blob WHERE size >= 0").Scan(&totalSize)
	r.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projectCode)
	r.DB().QueryRow("SELECT value FROM config WHERE name='server-code'").Scan(&serverCode)

	fmt.Printf("project-code:  %s\n", projectCode)
	fmt.Printf("server-code:   %s\n", serverCode)
	fmt.Printf("blobs:         %d\n", blobCount)
	fmt.Printf("deltas:        %d\n", deltaCount)
	fmt.Printf("phantoms:      %d\n", phantomCount)
	fmt.Printf("total size:    %d bytes\n", totalSize)
	return nil
}
```

- [ ] **Step 3: Test manually**

Run: `go build ./cmd/edgesync/ && ./edgesync repo new /tmp/test-cli.fossil && ./edgesync -R /tmp/test-cli.fossil repo info && rm /tmp/test-cli.fossil`

Expected: Creates repo, shows stats (1 blob from initial config, 0 deltas, etc.)

- [ ] **Step 4: Commit**

```bash
git add cmd/edgesync/repo_new.go cmd/edgesync/repo_info.go
git commit -m "edgesync: implement repo new and repo info commands"
```

---

## Task 3: repo ls + repo timeline + repo cat

**Files:**
- Modify: `cmd/edgesync/repo_ls.go`
- Modify: `cmd/edgesync/repo_timeline.go`
- Modify: `cmd/edgesync/repo_cat.go`

- [ ] **Step 1: Implement repo ls**

```go
package main

import (
	"fmt"

	"github.com/dmestas/edgesync/go-libfossil/manifest"
)

type RepoLsCmd struct {
	Version string `arg:"" optional:"" help:"Version to list (default: tip)"`
	Long    bool   `short:"l" help:"Show sizes and hashes"`
}

func (c *RepoLsCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	rid, err := resolveRID(r, c.Version)
	if err != nil {
		return err
	}

	files, err := manifest.ListFiles(r, rid)
	if err != nil {
		return err
	}

	for _, f := range files {
		if c.Long {
			fmt.Printf("%s  %s  %s\n", f.UUID[:10], f.Perm, f.Name)
		} else {
			fmt.Println(f.Name)
		}
	}
	return nil
}
```

- [ ] **Step 2: Implement repo timeline**

```go
package main

import (
	"fmt"

	"github.com/dmestas/edgesync/go-libfossil/manifest"
)

type RepoTimelineCmd struct {
	Limit int `short:"n" default:"20" help:"Number of entries"`
}

func (c *RepoTimelineCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	tipRid, err := resolveRID(r, "")
	if err != nil {
		return err
	}

	entries, err := manifest.Log(r, manifest.LogOpts{Start: tipRid, Limit: c.Limit})
	if err != nil {
		return err
	}

	for _, e := range entries {
		uuid := e.UUID
		if len(uuid) > 10 {
			uuid = uuid[:10]
		}
		fmt.Printf("%s  %s  %s  %s\n", uuid, e.Time.Format("2006-01-02 15:04"), e.User, e.Comment)
	}
	return nil
}
```

- [ ] **Step 3: Implement repo cat**

```go
package main

import (
	"fmt"
	"os"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
)

type RepoCatCmd struct {
	Artifact string `arg:"" help:"Artifact UUID or prefix"`
	Raw      bool   `help:"Output raw blob (no delta expansion)"`
}

func (c *RepoCatCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	rid, err := resolveRID(r, c.Artifact)
	if err != nil {
		return err
	}

	var data []byte
	if c.Raw {
		data, err = blob.Load(r.DB(), rid)
	} else {
		data, err = content.Expand(r.DB(), rid)
	}
	if err != nil {
		return fmt.Errorf("reading artifact: %w", err)
	}

	os.Stdout.Write(data)
	return nil
}
```

- [ ] **Step 4: Build and verify**

Run: `go build ./cmd/edgesync/`
Expected: Compiles.

- [ ] **Step 5: Commit**

```bash
git add cmd/edgesync/repo_ls.go cmd/edgesync/repo_timeline.go cmd/edgesync/repo_cat.go
git commit -m "edgesync: implement repo ls, timeline, and cat commands"
```

---

## Task 4: repo ci + repo co

**Files:**
- Modify: `cmd/edgesync/repo_ci.go`
- Modify: `cmd/edgesync/repo_co.go`

- [ ] **Step 1: Implement repo ci**

```go
package main

import (
	"fmt"
	"os"
	"path/filepath"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/manifest"
)

type RepoCiCmd struct {
	Message string   `short:"m" required:"" help:"Checkin comment"`
	Files   []string `arg:"" required:"" help:"Files to checkin"`
	User    string   `help:"Checkin user (default: OS username)"`
	Parent  string   `help:"Parent version UUID (default: tip)"`
}

func (c *RepoCiCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	// Resolve parent
	var parentRid libfossil.FslID
	if c.Parent != "" {
		parentRid, err = resolveRID(r, c.Parent)
		if err != nil {
			return fmt.Errorf("resolving parent: %w", err)
		}
	}
	// If no parent specified and repo has checkins, use tip
	if c.Parent == "" {
		parentRid, _ = resolveRID(r, "") // ignore error for initial checkin
	}

	// Read files
	files := make([]manifest.File, len(c.Files))
	for i, path := range c.Files {
		data, err := os.ReadFile(path)
		if err != nil {
			return fmt.Errorf("reading %s: %w", path, err)
		}
		files[i] = manifest.File{
			Name:    filepath.Base(path),
			Content: data,
		}
	}

	user := c.User
	if user == "" {
		user = currentUser()
	}

	rid, uuid, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   files,
		Comment: c.Message,
		User:    user,
		Parent:  parentRid,
		Time:    time.Now().UTC(),
	})
	if err != nil {
		return err
	}

	fmt.Printf("checkin %s (rid=%d)\n", uuid[:10], rid)
	return nil
}
```

Note: Add `libfossil "github.com/dmestas/edgesync/go-libfossil"` to imports for the `FslID` type.

- [ ] **Step 2: Implement repo co**

```go
package main

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
)

type RepoCoCmd struct {
	Version string `arg:"" optional:"" help:"Version to checkout (default: tip)"`
	Dir     string `short:"d" help:"Output directory (default: current dir)" default:"."`
	Force   bool   `help:"Overwrite existing files"`
}

func (c *RepoCoCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	rid, err := resolveRID(r, c.Version)
	if err != nil {
		return err
	}

	files, err := manifest.ListFiles(r, rid)
	if err != nil {
		return err
	}

	for _, f := range files {
		fileRid, ok := blob.Exists(r.DB(), f.UUID)
		if !ok {
			return fmt.Errorf("blob %s not found for file %s", f.UUID, f.Name)
		}
		data, err := content.Expand(r.DB(), fileRid)
		if err != nil {
			return fmt.Errorf("expanding %s: %w", f.Name, err)
		}

		outPath := filepath.Join(c.Dir, f.Name)
		if err := os.MkdirAll(filepath.Dir(outPath), 0o755); err != nil {
			return err
		}

		if !c.Force {
			if _, err := os.Stat(outPath); err == nil {
				return fmt.Errorf("file exists: %s (use --force to overwrite)", outPath)
			}
		}

		perm := os.FileMode(0o644)
		if f.Perm == "x" {
			perm = 0o755
		}
		if err := os.WriteFile(outPath, data, perm); err != nil {
			return err
		}

		fmt.Printf("  %s\n", f.Name)
	}

	fmt.Printf("checked out %d files\n", len(files))
	return nil
}
```

- [ ] **Step 3: Build and verify**

Run: `go build ./cmd/edgesync/`
Expected: Compiles.

- [ ] **Step 4: Manual end-to-end test**

```bash
./edgesync repo new /tmp/cli-test.fossil
echo "hello world" > /tmp/hello.txt
./edgesync -R /tmp/cli-test.fossil repo ci -m "initial commit" /tmp/hello.txt
./edgesync -R /tmp/cli-test.fossil repo ls
./edgesync -R /tmp/cli-test.fossil repo timeline
mkdir -p /tmp/cli-checkout
./edgesync -R /tmp/cli-test.fossil repo co -d /tmp/cli-checkout
cat /tmp/cli-checkout/hello.txt
rm -rf /tmp/cli-test.fossil /tmp/hello.txt /tmp/cli-checkout
```

Expected: Full lifecycle works — create, commit, list, timeline, checkout.

- [ ] **Step 5: Commit**

```bash
git add cmd/edgesync/repo_ci.go cmd/edgesync/repo_co.go
git commit -m "edgesync: implement repo ci and co commands"
```

---

## Task 5: sync start + sync now + bridge serve

**Files:**
- Modify: `cmd/edgesync/sync_start.go`
- Modify: `cmd/edgesync/sync_now.go`
- Modify: `cmd/edgesync/bridge_serve.go`

- [ ] **Step 1: Implement sync start**

```go
package main

import (
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/dmestas/edgesync/leaf/agent"
)

type SyncStartCmd struct {
	NATSUrl      string        `help:"NATS server URL" default:"nats://localhost:4222"`
	PollInterval time.Duration `help:"Sync poll interval" default:"5s"`
	Push         bool          `help:"Enable push" default:"true" negatable:""`
	Pull         bool          `help:"Enable pull" default:"true" negatable:""`
}

func (c *SyncStartCmd) Run(g *Globals) error {
	if g.Repo == "" {
		return fmt.Errorf("repository required (use -R <path>)")
	}

	a, err := agent.New(agent.Config{
		RepoPath:     g.Repo,
		NATSUrl:      c.NATSUrl,
		PollInterval: c.PollInterval,
		Push:         c.Push,
		Pull:         c.Pull,
	})
	if err != nil {
		return fmt.Errorf("agent: %w", err)
	}

	if err := a.Start(); err != nil {
		return fmt.Errorf("start: %w", err)
	}

	log.Printf("edgesync sync agent running (repo=%s nats=%s poll=%s)", g.Repo, c.NATSUrl, c.PollInterval)

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	<-sig

	log.Printf("shutting down...")
	return a.Stop()
}
```

- [ ] **Step 2: Implement sync now**

```go
package main

import (
	"fmt"
	"syscall"
)

type SyncNowCmd struct {
	PID int `arg:"" help:"PID of running agent to signal"`
}

func (c *SyncNowCmd) Run(g *Globals) error {
	if c.PID <= 0 {
		return fmt.Errorf("invalid PID: %d", c.PID)
	}
	return syscall.Kill(c.PID, syscall.SIGUSR1)
}
```

- [ ] **Step 3: Implement bridge serve**

```go
package main

import (
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"

	"github.com/dmestas/edgesync/bridge/bridge"
)

type BridgeServeCmd struct {
	NATSUrl   string `help:"NATS server URL" default:"nats://localhost:4222"`
	FossilURL string `required:"" help:"Fossil server HTTP URL"`
	Project   string `required:"" help:"Project code for NATS subject"`
}

func (c *BridgeServeCmd) Run(g *Globals) error {
	b, err := bridge.New(bridge.Config{
		NATSUrl:     c.NATSUrl,
		FossilURL:   c.FossilURL,
		ProjectCode: c.Project,
	})
	if err != nil {
		return fmt.Errorf("bridge: %w", err)
	}

	if err := b.Start(); err != nil {
		return fmt.Errorf("start: %w", err)
	}

	log.Printf("edgesync bridge running (nats=%s fossil=%s project=%s)", c.NATSUrl, c.FossilURL, c.Project)

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	<-sig

	log.Printf("shutting down...")
	return b.Stop()
}
```

- [ ] **Step 4: Build**

Run: `go build ./cmd/edgesync/`
Expected: Compiles. Don't run sync/bridge without NATS — just verify build.

- [ ] **Step 5: Verify help**

Run: `./edgesync sync --help && ./edgesync bridge --help`
Expected: Shows start/now and serve subcommands with flags.

- [ ] **Step 6: Commit**

```bash
git add cmd/edgesync/sync_start.go cmd/edgesync/sync_now.go cmd/edgesync/bridge_serve.go
git commit -m "edgesync: implement sync start, sync now, and bridge serve commands"
```

---

## Task 6: Full integration test

- [ ] **Step 1: Run the full lifecycle**

```bash
go build ./cmd/edgesync/

# Create repo
./edgesync repo new /tmp/e2e-test.fossil

# Check info
./edgesync -R /tmp/e2e-test.fossil repo info

# Commit a file
echo "test content" > /tmp/testfile.txt
./edgesync -R /tmp/e2e-test.fossil repo ci -m "first commit" /tmp/testfile.txt

# List files
./edgesync -R /tmp/e2e-test.fossil repo ls

# Timeline
./edgesync -R /tmp/e2e-test.fossil repo timeline

# Cat the checkin manifest
./edgesync -R /tmp/e2e-test.fossil repo cat <UUID-from-timeline>

# Checkout
mkdir -p /tmp/e2e-checkout
./edgesync -R /tmp/e2e-test.fossil repo co -d /tmp/e2e-checkout --force
cat /tmp/e2e-checkout/testfile.txt

# Verify fossil CLI can read our repo
fossil info /tmp/e2e-test.fossil

# Cleanup
rm -rf /tmp/e2e-test.fossil /tmp/testfile.txt /tmp/e2e-checkout
```

- [ ] **Step 2: Commit final state**

```bash
git add -A
git commit -m "edgesync: CLI MVP complete — repo, sync, bridge commands"
```
