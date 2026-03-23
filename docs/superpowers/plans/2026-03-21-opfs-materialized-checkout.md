# OPFS Materialized Checkout Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Materialize a Fossil repo's working tree as individual OPFS files with full directory structure, enabling browse/edit/status/commit from the browser.

**Architecture:** JS functions in worker.js expose async OPFS file operations (write/read/list/delete) to Go WASM via a callback bridge. Go's `checkout.go` manages materialize/status/commit. The playground UI reads from OPFS instead of blob storage.

**Tech Stack:** Go WASM (`GOOS=js`), OPFS async API (`getDirectoryHandle`, `createWritable`, `getFile`), `syscall/js`, existing go-libfossil manifest/content/blob packages.

**Spec:** `docs/superpowers/specs/2026-03-21-opfs-materialized-checkout-design.md`

---

### File Structure

| File | Action | Responsibility |
|------|--------|---------------|
| `spike/opfs-poc/bridge.go` | Create | Go↔JS async callback bridge (`coCall`, `_go_co_resolve`) |
| `spike/opfs-poc/checkout.go` | Create | Checkout manager (Materialize, Status, CommitAll, ListDir, ReadFile, WriteFile, DeleteFile) |
| `spike/opfs-poc/worker.js` | Modify | Add `_opfs_co_*` JS functions + `_go_co_resolve` callback + checkout message handlers |
| `spike/opfs-poc/main.go` | Modify | Wire checkout callbacks (`_checkout`, `_co_files`, `_co_read`, `_co_write`, `_co_delete`, `_co_status`, `_co_commit`) |
| `spike/opfs-poc/index.html` | Modify | Replace manifest-based file browser with OPFS-based; add Checkout/Status buttons, directory tree |

---

### Task 1: Async Bridge (bridge.go)

**Files:**
- Create: `spike/opfs-poc/bridge.go`

The foundation — Go goroutines call JS async functions and block until resolved.

- [ ] **Step 1: Create bridge.go with callback infrastructure**

```go
//go:build js

package main

import (
	"errors"
	"sync"
	"sync/atomic"
	"syscall/js"
)

var (
	coNextID  atomic.Int64
	coPending sync.Map // int64 → chan coResult
)

type coResult struct {
	data string
	err  error
}

func init() {
	js.Global().Set("_go_co_resolve", js.FuncOf(goCoResolve))
}

func goCoResolve(_ js.Value, args []js.Value) any {
	id := int64(args[0].Int())
	v, ok := coPending.LoadAndDelete(id)
	if !ok {
		return nil
	}
	ch := v.(chan coResult)
	if args[1].IsNull() || args[1].IsUndefined() {
		data := ""
		if len(args) > 2 && !args[2].IsNull() && !args[2].IsUndefined() {
			data = args[2].String()
		}
		ch <- coResult{data: data}
	} else {
		ch <- coResult{err: errors.New(args[1].String())}
	}
	return nil
}

// coCall invokes a JS function with (id, ...args) and blocks until resolved.
func coCall(fn string, args ...any) (string, error) {
	id := coNextID.Add(1)
	ch := make(chan coResult, 1)
	coPending.Store(id, ch)
	callArgs := make([]any, 0, len(args)+1)
	callArgs = append(callArgs, id)
	callArgs = append(callArgs, args...)
	js.Global().Call(fn, callArgs...)
	res := <-ch
	return res.data, res.err
}

// toJSUint8Array converts Go []byte to a JS Uint8Array.
func toJSUint8Array(data []byte) js.Value {
	arr := js.Global().Get("Uint8Array").New(len(data))
	js.CopyBytesToJS(arr, data)
	return arr
}
```

- [ ] **Step 2: Verify build**

Run: `cd /Users/dmestas/projects/EdgeSync/.worktrees/opfs-spike && GOOS=js GOARCH=wasm go build -buildvcs=false -tags ncruces ./spike/opfs-poc/`
Expected: Build succeeds

- [ ] **Step 3: Commit**

```bash
git add spike/opfs-poc/bridge.go
git commit -m "spike: add Go↔JS async callback bridge for OPFS checkout"
```

---

### Task 2: JS OPFS Checkout Functions (worker.js)

**Files:**
- Modify: `spike/opfs-poc/worker.js`

Add the `_opfs_co_*` functions that operate on the `checkout/` OPFS directory.

- [ ] **Step 1: Add OPFS checkout helpers to worker.js**

After the existing `init()` function, before `self.onmessage`, add:

```javascript
// --- OPFS Checkout File Manager ---

let checkoutRoot = null;

async function getCheckoutRoot() {
    if (checkoutRoot) return checkoutRoot;
    const root = await navigator.storage.getDirectory();
    checkoutRoot = await root.getDirectoryHandle("checkout", { create: true });
    return checkoutRoot;
}

// Navigate to a parent directory, creating intermediate dirs.
async function navigateTo(root, path) {
    const parts = path.split("/").filter(p => p && p !== "." && p !== "..");
    let dir = root;
    for (const part of parts) {
        dir = await dir.getDirectoryHandle(part, { create: true });
    }
    return dir;
}

// Navigate to parent dir of a file path, return [parentDir, fileName].
async function navigateToParent(root, path) {
    const parts = path.split("/").filter(p => p && p !== "." && p !== "..");
    const fileName = parts.pop();
    let dir = root;
    for (const part of parts) {
        dir = await dir.getDirectoryHandle(part, { create: true });
    }
    return [dir, fileName];
}

function _opfs_co_write(id, path, contentArray) {
    (async () => {
        try {
            const root = await getCheckoutRoot();
            const [dir, name] = await navigateToParent(root, path);
            const fh = await dir.getFileHandle(name, { create: true });
            const writable = await fh.createWritable();
            await writable.write(contentArray);
            await writable.close();
            _go_co_resolve(id, null, null);
        } catch (e) {
            _go_co_resolve(id, e.message, null);
        }
    })();
}

function _opfs_co_read(id, path) {
    (async () => {
        try {
            const root = await getCheckoutRoot();
            const [dir, name] = await navigateToParent(root, path);
            const fh = await dir.getFileHandle(name);
            const file = await fh.getFile();
            const buf = await file.arrayBuffer();
            const arr = new Uint8Array(buf);
            // Convert to string for the bridge (text files only in spike).
            const text = new TextDecoder().decode(arr);
            _go_co_resolve(id, null, text);
        } catch (e) {
            _go_co_resolve(id, e.message, null);
        }
    })();
}

function _opfs_co_list(id, dirPath) {
    (async () => {
        try {
            const root = await getCheckoutRoot();
            let dir = root;
            if (dirPath && dirPath !== "" && dirPath !== ".") {
                dir = await navigateTo(root, dirPath);
            }
            const entries = [];
            for await (const [name, handle] of dir.entries()) {
                if (name === ".fossil-checkout") continue; // skip metadata
                if (handle.kind === "file") {
                    const file = await handle.getFile();
                    entries.push({ name, isDir: false, size: file.size });
                } else {
                    entries.push({ name, isDir: true, size: 0 });
                }
            }
            _go_co_resolve(id, null, JSON.stringify(entries));
        } catch (e) {
            _go_co_resolve(id, e.message, null);
        }
    })();
}

function _opfs_co_delete(id, path) {
    (async () => {
        try {
            const root = await getCheckoutRoot();
            const [dir, name] = await navigateToParent(root, path);
            await dir.removeEntry(name);
            _go_co_resolve(id, null, null);
        } catch (e) {
            _go_co_resolve(id, e.message, null);
        }
    })();
}

function _opfs_co_clear(id) {
    (async () => {
        try {
            const opfsRoot = await navigator.storage.getDirectory();
            try {
                await opfsRoot.removeEntry("checkout", { recursive: true });
            } catch (e) {
                // Might not exist yet — that's fine.
            }
            checkoutRoot = null;
            _go_co_resolve(id, null, null);
        } catch (e) {
            _go_co_resolve(id, e.message, null);
        }
    })();
}

function _opfs_co_stat(id, path) {
    (async () => {
        try {
            const root = await getCheckoutRoot();
            if (!path || path === "" || path === ".") {
                _go_co_resolve(id, null, JSON.stringify({ exists: true, isDir: true, size: 0 }));
                return;
            }
            // Try as directory first.
            const parts = path.split("/").filter(p => p && p !== ".");
            let dir = root;
            try {
                for (const part of parts) {
                    dir = await dir.getDirectoryHandle(part);
                }
                _go_co_resolve(id, null, JSON.stringify({ exists: true, isDir: true, size: 0 }));
                return;
            } catch (e) {}
            // Try as file.
            try {
                const [parentDir, name] = await navigateToParent(root, path);
                const fh = await parentDir.getFileHandle(name);
                const file = await fh.getFile();
                _go_co_resolve(id, null, JSON.stringify({ exists: true, isDir: false, size: file.size }));
            } catch (e) {
                _go_co_resolve(id, null, JSON.stringify({ exists: false, isDir: false, size: 0 }));
            }
        } catch (e) {
            _go_co_resolve(id, e.message, null);
        }
    })();
}
```

- [ ] **Step 2: Add checkout message handlers to self.onmessage**

Update the switch in `self.onmessage` to add new checkout commands:

```javascript
case "checkout":  _checkout(); break;
case "coFiles":   _co_files(msg.path || ""); break;
case "coRead":    _co_read(msg.path); break;
case "coWrite":   _co_write(msg.path, msg.content); break;
case "coDelete":  _co_delete(msg.path); break;
case "coStatus":  _co_status(); break;
case "coCommit":  _co_commit(msg.comment, msg.user); break;
```

- [ ] **Step 3: Verify build**

Run: `cd /Users/dmestas/projects/EdgeSync/.worktrees/opfs-spike && GOOS=js GOARCH=wasm go build -buildvcs=false -tags ncruces ./spike/opfs-poc/`
Expected: Build succeeds

- [ ] **Step 4: Commit**

```bash
git add spike/opfs-poc/worker.js
git commit -m "spike: add OPFS checkout file manager JS functions"
```

---

### Task 3: Checkout Manager (checkout.go)

**Files:**
- Create: `spike/opfs-poc/checkout.go`

Core Go logic: materialize, list, read, write, delete, status, commit.

- [ ] **Step 1: Create checkout.go**

```go
//go:build js

package main

import (
	"encoding/json"
	"fmt"
	"sort"
	"strings"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/hash"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

type Checkout struct {
	repo   *repo.Repo
	tipRID libfossil.FslID
}

type DirEntry struct {
	Name  string `json:"name"`
	IsDir bool   `json:"isDir"`
	Size  int    `json:"size"`
}

type FileChange struct {
	Name   string `json:"name"`
	Status string `json:"status"` // "modified", "added", "deleted"
}

func NewCheckout(r *repo.Repo, tip libfossil.FslID) *Checkout {
	return &Checkout{repo: r, tipRID: tip}
}

// Materialize extracts all files from the tip checkin to OPFS.
func (co *Checkout) Materialize() (int, error) {
	// Clear existing checkout.
	if _, err := coCall("_opfs_co_clear"); err != nil {
		return 0, fmt.Errorf("clear: %w", err)
	}

	files, err := manifest.ListFiles(co.repo, co.tipRID)
	if err != nil {
		return 0, fmt.Errorf("list files: %w", err)
	}

	for _, f := range files {
		fileRid, ok := blob.Exists(co.repo.DB(), f.UUID)
		if !ok {
			return 0, fmt.Errorf("blob not found: %s", f.UUID)
		}
		data, err := content.Expand(co.repo.DB(), fileRid)
		if err != nil {
			return 0, fmt.Errorf("expand %s: %w", f.Name, err)
		}
		if _, err := coCall("_opfs_co_write", f.Name, toJSUint8Array(data)); err != nil {
			return 0, fmt.Errorf("write %s: %w", f.Name, err)
		}
	}

	// Write tip RID metadata.
	meta := fmt.Sprintf("%d", co.tipRID)
	if _, err := coCall("_opfs_co_write", ".fossil-checkout", toJSUint8Array([]byte(meta))); err != nil {
		return 0, fmt.Errorf("write metadata: %w", err)
	}

	return len(files), nil
}

// ListDir lists entries in an OPFS directory.
func (co *Checkout) ListDir(path string) ([]DirEntry, error) {
	data, err := coCall("_opfs_co_list", path)
	if err != nil {
		return nil, err
	}
	var entries []DirEntry
	if err := json.Unmarshal([]byte(data), &entries); err != nil {
		return nil, fmt.Errorf("parse list: %w", err)
	}
	sort.Slice(entries, func(i, j int) bool {
		// Dirs first, then alphabetical.
		if entries[i].IsDir != entries[j].IsDir {
			return entries[i].IsDir
		}
		return entries[i].Name < entries[j].Name
	})
	return entries, nil
}

// ReadFile reads a file from OPFS.
func (co *Checkout) ReadFile(path string) (string, error) {
	data, err := coCall("_opfs_co_read", path)
	if err != nil {
		return "", err
	}
	return data, nil
}

// WriteFile writes content to an OPFS file.
func (co *Checkout) WriteFile(path, content string) error {
	_, err := coCall("_opfs_co_write", path, toJSUint8Array([]byte(content)))
	return err
}

// DeleteFile removes a file from OPFS.
func (co *Checkout) DeleteFile(path string) error {
	_, err := coCall("_opfs_co_delete", path)
	return err
}

// walkTree recursively lists all files in the OPFS checkout.
func (co *Checkout) walkTree(prefix string) (map[string]int, error) {
	entries, err := co.ListDir(prefix)
	if err != nil {
		return nil, err
	}
	files := make(map[string]int)
	for _, e := range entries {
		path := e.Name
		if prefix != "" {
			path = prefix + "/" + e.Name
		}
		if e.IsDir {
			sub, err := co.walkTree(path)
			if err != nil {
				return nil, err
			}
			for k, v := range sub {
				files[k] = v
			}
		} else {
			files[path] = e.Size
		}
	}
	return files, nil
}

// Status compares the OPFS working tree against the tip manifest.
func (co *Checkout) Status() ([]FileChange, error) {
	// Get tip manifest files.
	tipFiles, err := manifest.ListFiles(co.repo, co.tipRID)
	if err != nil {
		return nil, fmt.Errorf("list tip files: %w", err)
	}
	tipMap := make(map[string]string, len(tipFiles)) // name → UUID
	for _, f := range tipFiles {
		tipMap[f.Name] = f.UUID
	}

	// Walk OPFS tree.
	opfsFiles, err := co.walkTree("")
	if err != nil {
		return nil, fmt.Errorf("walk tree: %w", err)
	}

	var changes []FileChange

	// Check OPFS files against manifest.
	for name := range opfsFiles {
		uuid, inManifest := tipMap[name]
		if !inManifest {
			changes = append(changes, FileChange{Name: name, Status: "added"})
			continue
		}
		// Read and hash to detect modifications.
		content, err := co.ReadFile(name)
		if err != nil {
			continue
		}
		computed := hash.SHA1([]byte(content))
		if computed != uuid {
			changes = append(changes, FileChange{Name: name, Status: "modified"})
		}
	}

	// Check manifest files not in OPFS.
	for name := range tipMap {
		if _, inOPFS := opfsFiles[name]; !inOPFS {
			changes = append(changes, FileChange{Name: name, Status: "deleted"})
		}
	}

	sort.Slice(changes, func(i, j int) bool { return changes[i].Name < changes[j].Name })
	return changes, nil
}

// CommitAll reads all files from OPFS and creates a Fossil checkin.
func (co *Checkout) CommitAll(comment, user string) (libfossil.FslID, string, error) {
	changes, err := co.Status()
	if err != nil {
		return 0, "", fmt.Errorf("status: %w", err)
	}
	if len(changes) == 0 {
		return 0, "", fmt.Errorf("nothing to commit")
	}

	// Read all files from OPFS (full tree needed for R-card).
	opfsFiles, err := co.walkTree("")
	if err != nil {
		return 0, "", fmt.Errorf("walk: %w", err)
	}

	// Sort file names for deterministic manifest.
	var names []string
	for name := range opfsFiles {
		names = append(names, name)
	}
	sort.Strings(names)

	var commitFiles []manifest.File
	for _, name := range names {
		data, err := co.ReadFile(name)
		if err != nil {
			return 0, "", fmt.Errorf("read %s: %w", name, err)
		}
		commitFiles = append(commitFiles, manifest.File{
			Name:    name,
			Content: []byte(data),
		})
	}

	if user == "" {
		user = "browser"
	}
	if comment == "" {
		comment = "edit from browser"
	}

	rid, uuid, err := manifest.Checkin(co.repo, manifest.CheckinOpts{
		Files:   commitFiles,
		Comment: comment,
		User:    user,
		Parent:  co.tipRID,
	})
	if err != nil {
		return 0, "", fmt.Errorf("checkin: %w", err)
	}

	co.tipRID = rid

	// Update metadata file.
	meta := fmt.Sprintf("%d", rid)
	coCall("_opfs_co_write", ".fossil-checkout", toJSUint8Array([]byte(meta)))

	return rid, uuid, nil
}

// HasCheckout checks if a checkout exists in OPFS.
func HasCheckout() bool {
	data, err := coCall("_opfs_co_stat", ".fossil-checkout")
	if err != nil {
		return false
	}
	var stat struct{ Exists bool `json:"exists"` }
	if err := json.Unmarshal([]byte(data), &stat); err != nil {
		return false
	}
	return stat.Exists
}

// ReadCheckoutTipRID reads the stored tip RID from OPFS metadata.
func ReadCheckoutTipRID() (libfossil.FslID, error) {
	data, err := coCall("_opfs_co_read", ".fossil-checkout")
	if err != nil {
		return 0, err
	}
	var rid int64
	fmt.Sscanf(strings.TrimSpace(data), "%d", &rid)
	if rid <= 0 {
		return 0, fmt.Errorf("invalid tip RID in .fossil-checkout")
	}
	return libfossil.FslID(rid), nil
}
```

- [ ] **Step 2: Verify build**

Run: `cd /Users/dmestas/projects/EdgeSync/.worktrees/opfs-spike && GOOS=js GOARCH=wasm go build -buildvcs=false -tags ncruces ./spike/opfs-poc/`
Expected: Build succeeds

- [ ] **Step 3: Commit**

```bash
git add spike/opfs-poc/checkout.go
git commit -m "spike: add OPFS checkout manager — materialize, status, commit"
```

---

### Task 4: Wire Checkout into main.go

**Files:**
- Modify: `spike/opfs-poc/main.go`

Replace the old manifest-based file operations with OPFS checkout operations.

- [ ] **Step 1: Add checkout variable and doCheckout function**

Add after `var currentRepo *repo.Repo`:

```go
var currentCheckout *Checkout
```

Add new handler functions:

```go
func doCheckout() {
	if !ensureRepo() {
		postError("no repo — clone first")
		return
	}
	tip, err := tipRID()
	if err != nil {
		postError(err.Error())
		return
	}

	co := NewCheckout(currentRepo, tip)
	n, err := co.Materialize()
	if err != nil {
		postError(fmt.Sprintf("checkout failed: %v", err))
		return
	}
	currentCheckout = co
	log(fmt.Sprintf("checked out %d files to OPFS", n))
	postResult("checkout", toJSON(map[string]any{"files": n}))
}

func doCoFiles(path string) {
	if currentCheckout == nil {
		postError("no checkout — click Checkout first")
		return
	}
	entries, err := currentCheckout.ListDir(path)
	if err != nil {
		postError(fmt.Sprintf("list dir failed: %v", err))
		return
	}
	postResult("coFiles", toJSON(entries))
}

func doCoRead(path string) {
	if currentCheckout == nil {
		postError("no checkout")
		return
	}
	data, err := currentCheckout.ReadFile(path)
	if err != nil {
		postError(fmt.Sprintf("read failed: %v", err))
		return
	}
	postResult("coRead", toJSON(map[string]any{"path": path, "content": data}))
}

func doCoWrite(path, content string) {
	if currentCheckout == nil {
		postError("no checkout")
		return
	}
	if err := currentCheckout.WriteFile(path, content); err != nil {
		postError(fmt.Sprintf("write failed: %v", err))
		return
	}
	log(fmt.Sprintf("saved %s to OPFS (%d bytes)", path, len(content)))
	postResult("coWrite", toJSON(map[string]any{"path": path, "size": len(content)}))
}

func doCoDelete(path string) {
	if currentCheckout == nil {
		postError("no checkout")
		return
	}
	if err := currentCheckout.DeleteFile(path); err != nil {
		postError(fmt.Sprintf("delete failed: %v", err))
		return
	}
	log(fmt.Sprintf("deleted %s from OPFS", path))
	postResult("coDelete", toJSON(map[string]any{"path": path}))
}

func doCoStatus() {
	if currentCheckout == nil {
		postError("no checkout")
		return
	}
	changes, err := currentCheckout.Status()
	if err != nil {
		postError(fmt.Sprintf("status failed: %v", err))
		return
	}
	postResult("coStatus", toJSON(changes))
}

func doCoCommit(comment, user string) {
	if currentCheckout == nil {
		postError("no checkout")
		return
	}
	rid, uuid, err := currentCheckout.CommitAll(comment, user)
	if err != nil {
		postError(fmt.Sprintf("commit failed: %v", err))
		return
	}
	short := uuid
	if len(short) > 12 { short = short[:12] }
	log(fmt.Sprintf("committed from OPFS: rid=%d uuid=%s", rid, short))
	postResult("coCommit", toJSON(map[string]any{"rid": int64(rid), "uuid": uuid}))
}
```

- [ ] **Step 2: Register JS callbacks in main()**

Add to the callback registration block in `main()`:

```go
js.Global().Set("_checkout", js.FuncOf(func(_ js.Value, _ []js.Value) any {
	go doCheckout()
	return nil
}))
js.Global().Set("_co_files", js.FuncOf(func(_ js.Value, args []js.Value) any {
	path := ""
	if len(args) > 0 { path = args[0].String() }
	go doCoFiles(path)
	return nil
}))
js.Global().Set("_co_read", js.FuncOf(func(_ js.Value, args []js.Value) any {
	if len(args) < 1 { postError("_co_read requires path"); return nil }
	go doCoRead(args[0].String())
	return nil
}))
js.Global().Set("_co_write", js.FuncOf(func(_ js.Value, args []js.Value) any {
	if len(args) < 2 { postError("_co_write requires path, content"); return nil }
	go doCoWrite(args[0].String(), args[1].String())
	return nil
}))
js.Global().Set("_co_delete", js.FuncOf(func(_ js.Value, args []js.Value) any {
	if len(args) < 1 { postError("_co_delete requires path"); return nil }
	go doCoDelete(args[0].String())
	return nil
}))
js.Global().Set("_co_status", js.FuncOf(func(_ js.Value, _ []js.Value) any {
	go doCoStatus()
	return nil
}))
js.Global().Set("_co_commit", js.FuncOf(func(_ js.Value, args []js.Value) any {
	comment, user := "", ""
	if len(args) > 0 { comment = args[0].String() }
	if len(args) > 1 { user = args[1].String() }
	go doCoCommit(comment, user)
	return nil
}))
```

- [ ] **Step 3: Auto-checkout after clone**

In `doClone()`, after the crosslink block, add:

```go
// Auto-checkout tip to OPFS.
tip, tipErr := tipRID()
if tipErr == nil {
	co := NewCheckout(r, tip)
	n, coErr := co.Materialize()
	if coErr != nil {
		log(fmt.Sprintf("auto-checkout warning: %v", coErr))
	} else {
		currentCheckout = co
		log(fmt.Sprintf("checked out %d files to OPFS", n))
	}
}
```

- [ ] **Step 4: Restore checkout on reload**

In `doStatus()`, after `ensureRepo()`, add checkout restoration:

```go
// Restore checkout from OPFS if it exists.
if currentCheckout == nil && HasCheckout() {
	storedTip, err := ReadCheckoutTipRID()
	if err == nil {
		currentCheckout = NewCheckout(currentRepo, storedTip)
		log("restored checkout from OPFS")
	}
}
```

Add `hasCheckout` field to the status result:

```go
postResult("status", toJSON(map[string]any{
	"open":        true,
	"blobs":       blobCount,
	"checkins":    checkinCount,
	"projectCode": projectCode,
	"hasCheckout": currentCheckout != nil,
}))
```

- [ ] **Step 5: Verify build**

Run: `cd /Users/dmestas/projects/EdgeSync/.worktrees/opfs-spike && GOOS=js GOARCH=wasm go build -buildvcs=false -tags ncruces ./spike/opfs-poc/`
Expected: Build succeeds

- [ ] **Step 6: Commit**

```bash
git add spike/opfs-poc/main.go
git commit -m "spike: wire OPFS checkout into main.go callbacks"
```

---

### Task 5: Playground UI (index.html)

**Files:**
- Modify: `spike/opfs-poc/index.html`

Replace manifest-based file browser with OPFS tree; add Checkout/Status/Save buttons.

- [ ] **Step 1: Update UI**

Key changes to `index.html`:

1. Add "Checkout" button next to Clone/Sync
2. Replace "Files" panel with OPFS directory tree (expandable dirs)
3. Add "Save" button next to editor (writes to OPFS immediately)
4. Add "Status" button that shows modified/added/deleted files
5. Replace commit flow: reads from OPFS, not from JS `pendingEdits`
6. Add `clearOPFS` to also clear the `checkout/` directory
7. Add new result handlers: `checkout`, `coFiles`, `coRead`, `coWrite`, `coStatus`, `coCommit`
8. Show directory breadcrumb for navigation
9. Style changes: modified files in yellow, added in green, deleted in red

The file list now comes from `_co_files` (OPFS) instead of `_files` (manifest). Clicking a directory navigates into it. Clicking a file loads from OPFS via `_co_read`.

The commit button sends `coCommit` instead of the old `commit` — no more `pendingEdits` JS object. Edits are saved to OPFS on "Save" button click, so they persist across reloads.

Status button calls `_co_status` and displays the diff in a panel above the editor.

- [ ] **Step 2: Add new worker.js message handlers**

In `self.onmessage` switch, add:

```javascript
case "checkout":  _checkout(); break;
case "coFiles":   _co_files(msg.path || ""); break;
case "coRead":    _co_read(msg.path); break;
case "coWrite":   _co_write(msg.path, msg.content); break;
case "coDelete":  _co_delete(msg.path); break;
case "coStatus":  _co_status(); break;
case "coCommit":  _co_commit(msg.comment, msg.user); break;
```

- [ ] **Step 3: Verify full build and manual test**

Run: `cd /Users/dmestas/projects/EdgeSync/.worktrees/opfs-spike && GOOS=js GOARCH=wasm go build -buildvcs=false -tags ncruces ./spike/opfs-poc/`
Expected: Build succeeds

Manual test:
1. Start `fossil serve /tmp/test-leaf.fossil --port 9090`
2. Start playground: `cd spike/opfs-poc && go run -buildvcs=false -tags ncruces . -target http://localhost:9090 -repo /tmp/test-leaf.fossil`
3. Click Clone → should auto-checkout files
4. File tree shows OPFS files (directories expandable)
5. Click a file → content loads from OPFS
6. Edit → click Save → saved to OPFS
7. Reload page → files still there (persistence!)
8. Click Status → shows modified/added/deleted
9. Click Commit → creates Fossil checkin from OPFS tree
10. Click Sync → pushes to remote
11. Click Remote → verify new commit appears

- [ ] **Step 4: Commit**

```bash
git add spike/opfs-poc/index.html spike/opfs-poc/worker.js
git commit -m "spike: OPFS materialized checkout playground UI"
```

---

### Task 6: Clear OPFS Fix

**Files:**
- Modify: `spike/opfs-poc/index.html`

- [ ] **Step 1: Update clearOPFS to remove checkout/ too**

```javascript
async function clearOPFS() {
    try {
        const root = await navigator.storage.getDirectory();
        try { await root.removeEntry("sqlite3-opfs", { recursive: true }); } catch (e) {}
        try { await root.removeEntry("checkout", { recursive: true }); } catch (e) {}
        log("OPFS cleared (db + checkout). Reload to start fresh.");
    } catch (e) {
        log("Clear: " + e.message);
    }
}
```

- [ ] **Step 2: Commit**

```bash
git add spike/opfs-poc/index.html
git commit -m "spike: clear both sqlite3-opfs and checkout dirs"
```
