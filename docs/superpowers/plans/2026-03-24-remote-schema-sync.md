# Remote Schema Sync Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a generic table sync primitive to EdgeSync — schema cards deploy SQLite tables across peers, `xigot`/`xgimme`/`xrow` cards keep them incrementally synced. Peer registry as first consumer.

**Architecture:** Four new xfer card types (`schema`, `xigot`, `xgimme`, `xrow`) mirror the UV sync pattern. Schemas stored in `_sync_schema` table, extension tables prefixed `x_`. Handler processes schema cards in control phase, data cards in data phase. Conflict strategies are pluggable per-table (`self-write`, `mtime-wins`, `owner-write`).

**Tech Stack:** Go, SQLite, xfer card protocol, existing sync/handler/observer infrastructure

**Spec:** `docs/superpowers/specs/2026-03-24-remote-schema-sync-design.md`

---

### Task 1: xfer Card Types

**Files:**
- Create: `go-libfossil/xfer/card_tablesync.go`
- Modify: `go-libfossil/xfer/card.go` (add 4 new CardType constants)
- Test: `go-libfossil/xfer/card_tablesync_test.go`

- [ ] **Step 1: Write failing tests for new card types**

Create `go-libfossil/xfer/card_tablesync_test.go`:

```go
package xfer

import "testing"

func TestSchemaCardType(t *testing.T) {
	c := &SchemaCard{Table: "peer_registry", Version: 1, Hash: "abc", MTime: 100, Content: []byte(`{}`)}
	if c.Type() != CardSchema {
		t.Fatalf("got %v, want CardSchema", c.Type())
	}
}

func TestXIGotCardType(t *testing.T) {
	c := &XIGotCard{Table: "peer_registry", PKHash: "abc", MTime: 100}
	if c.Type() != CardXIGot {
		t.Fatalf("got %v, want CardXIGot", c.Type())
	}
}

func TestXGimmeCardType(t *testing.T) {
	c := &XGimmeCard{Table: "peer_registry", PKHash: "abc"}
	if c.Type() != CardXGimme {
		t.Fatalf("got %v, want CardXGimme", c.Type())
	}
}

func TestXRowCardType(t *testing.T) {
	c := &XRowCard{Table: "peer_registry", PKHash: "abc", MTime: 100, Content: []byte(`{}`)}
	if c.Type() != CardXRow {
		t.Fatalf("got %v, want CardXRow", c.Type())
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd go-libfossil && go test -buildvcs=false ./xfer/ -run 'TestSchemaCardType|TestXIGotCardType|TestXGimmeCardType|TestXRowCardType' -v`
Expected: FAIL — types not defined

- [ ] **Step 3: Add CardType constants to card.go**

Add after `CardUnknown` (line 29 of `go-libfossil/xfer/card.go`):

```go
CardSchema  // 20 — schema (table sync)
CardXIGot   // 21 — xigot (table sync row announcement)
CardXGimme  // 22 — xgimme (table sync row request)
CardXRow    // 23 — xrow (table sync row payload)
```

- [ ] **Step 4: Create card_tablesync.go with struct definitions**

Create `go-libfossil/xfer/card_tablesync.go`:

```go
package xfer

// SchemaCard declares a synced extension table.
// Wire: schema TABLE VERSION HASH MTIME SIZE\nJSON
type SchemaCard struct {
	Table   string // table name (without x_ prefix)
	Version int
	Hash    string // SHA1 of canonical schema definition
	MTime   int64
	Content []byte // raw JSON schema definition
}

func (c *SchemaCard) Type() CardType { return CardSchema }

// XIGotCard announces possession of a table sync row.
// Wire: xigot TABLE PK_HASH MTIME
type XIGotCard struct {
	Table  string
	PKHash string
	MTime  int64
}

func (c *XIGotCard) Type() CardType { return CardXIGot }

// XGimmeCard requests a table sync row.
// Wire: xgimme TABLE PK_HASH
type XGimmeCard struct {
	Table  string
	PKHash string
}

func (c *XGimmeCard) Type() CardType { return CardXGimme }

// XRowCard carries a table sync row payload.
// Wire: xrow TABLE PK_HASH MTIME SIZE\nJSON
type XRowCard struct {
	Table   string
	PKHash  string
	MTime   int64
	Content []byte // raw JSON row data
}

func (c *XRowCard) Type() CardType { return CardXRow }
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd go-libfossil && go test -buildvcs=false ./xfer/ -run 'TestSchemaCardType|TestXIGotCardType|TestXGimmeCardType|TestXRowCardType' -v`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add go-libfossil/xfer/card.go go-libfossil/xfer/card_tablesync.go go-libfossil/xfer/card_tablesync_test.go
git commit -m "feat(xfer): add SchemaCard, XIGotCard, XGimmeCard, XRowCard types"
```

---

### Task 2: xfer Encode/Decode for Table Sync Cards

**Files:**
- Modify: `go-libfossil/xfer/encode.go` (add cases to `EncodeCard` switch + encoder functions)
- Modify: `go-libfossil/xfer/decode.go` (add cases to `parseLine` switch + parser functions)
- Test: `go-libfossil/xfer/card_tablesync_test.go` (append round-trip tests)

- [ ] **Step 1: Write failing round-trip tests**

Append to `go-libfossil/xfer/card_tablesync_test.go`:

```go
import (
	"bufio"
	"bytes"
	"testing"
)

func TestSchemaCard_RoundTrip(t *testing.T) {
	original := &SchemaCard{
		Table:   "peer_registry",
		Version: 1,
		Hash:    "abc123",
		MTime:   1711300000,
		Content: []byte(`{"columns":[{"name":"id","type":"text","pk":true}],"conflict":"self-write"}`),
	}
	var buf bytes.Buffer
	if err := EncodeCard(&buf, original); err != nil {
		t.Fatalf("encode: %v", err)
	}
	r := bufio.NewReader(&buf)
	got, err := DecodeCard(r)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	sc, ok := got.(*SchemaCard)
	if !ok {
		t.Fatalf("got %T, want *SchemaCard", got)
	}
	if sc.Table != "peer_registry" || sc.Version != 1 || sc.Hash != "abc123" || sc.MTime != 1711300000 {
		t.Fatalf("header mismatch: %+v", sc)
	}
	if !bytes.Equal(sc.Content, original.Content) {
		t.Fatalf("content mismatch: got %q", sc.Content)
	}
}

func TestXIGotCard_RoundTrip(t *testing.T) {
	original := &XIGotCard{Table: "peer_registry", PKHash: "abc123", MTime: 1711300000}
	var buf bytes.Buffer
	if err := EncodeCard(&buf, original); err != nil {
		t.Fatalf("encode: %v", err)
	}
	r := bufio.NewReader(&buf)
	got, err := DecodeCard(r)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	xig, ok := got.(*XIGotCard)
	if !ok {
		t.Fatalf("got %T, want *XIGotCard", got)
	}
	if xig.Table != "peer_registry" || xig.PKHash != "abc123" || xig.MTime != 1711300000 {
		t.Fatalf("mismatch: %+v", xig)
	}
}

func TestXGimmeCard_RoundTrip(t *testing.T) {
	original := &XGimmeCard{Table: "peer_registry", PKHash: "abc123"}
	var buf bytes.Buffer
	if err := EncodeCard(&buf, original); err != nil {
		t.Fatalf("encode: %v", err)
	}
	r := bufio.NewReader(&buf)
	got, err := DecodeCard(r)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	xg, ok := got.(*XGimmeCard)
	if !ok {
		t.Fatalf("got %T, want *XGimmeCard", got)
	}
	if xg.Table != "peer_registry" || xg.PKHash != "abc123" {
		t.Fatalf("mismatch: %+v", xg)
	}
}

func TestXRowCard_RoundTrip(t *testing.T) {
	original := &XRowCard{
		Table:   "peer_registry",
		PKHash:  "abc123",
		MTime:   1711300000,
		Content: []byte(`{"peer_id":"leaf-01","version":"0.5.0"}`),
	}
	var buf bytes.Buffer
	if err := EncodeCard(&buf, original); err != nil {
		t.Fatalf("encode: %v", err)
	}
	r := bufio.NewReader(&buf)
	got, err := DecodeCard(r)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	xr, ok := got.(*XRowCard)
	if !ok {
		t.Fatalf("got %T, want *XRowCard", got)
	}
	if xr.Table != "peer_registry" || xr.PKHash != "abc123" || xr.MTime != 1711300000 {
		t.Fatalf("header mismatch: %+v", xr)
	}
	if !bytes.Equal(xr.Content, original.Content) {
		t.Fatalf("content mismatch: got %q", xr.Content)
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd go-libfossil && go test -buildvcs=false ./xfer/ -run 'TestSchemaCard_RoundTrip|TestXIGotCard_RoundTrip|TestXGimmeCard_RoundTrip|TestXRowCard_RoundTrip' -v`
Expected: FAIL — encode/decode not implemented

- [ ] **Step 3: Add encoder functions to encode.go**

Add cases in `EncodeCard` switch (after `*UVFileCard` case):

```go
case *SchemaCard:
	return encodeSchema(w, v)
case *XIGotCard:
	return encodeXIGot(w, v)
case *XGimmeCard:
	return encodeXGimme(w, v)
case *XRowCard:
	return encodeXRow(w, v)
```

Add encoder functions at bottom of `encode.go`:

```go
// encodeSchema writes: schema TABLE VERSION HASH MTIME SIZE\nJSON\n
// Follows config card pattern: trailing newline after JSON payload.
func encodeSchema(w *bytes.Buffer, c *SchemaCard) error {
	fmt.Fprintf(w, "schema %s %d %s %d %d\n", c.Table, c.Version, c.Hash, c.MTime, len(c.Content))
	w.Write(c.Content)
	w.WriteByte('\n')
	return nil
}

func encodeXIGot(w *bytes.Buffer, c *XIGotCard) error {
	fmt.Fprintf(w, "xigot %s %s %d\n", c.Table, c.PKHash, c.MTime)
	return nil
}

func encodeXGimme(w *bytes.Buffer, c *XGimmeCard) error {
	fmt.Fprintf(w, "xgimme %s %s\n", c.Table, c.PKHash)
	return nil
}

// encodeXRow writes: xrow TABLE PK_HASH MTIME SIZE\nJSON\n
// Follows config card pattern: trailing newline after JSON payload.
func encodeXRow(w *bytes.Buffer, c *XRowCard) error {
	fmt.Fprintf(w, "xrow %s %s %d %d\n", c.Table, c.PKHash, c.MTime, len(c.Content))
	w.Write(c.Content)
	w.WriteByte('\n')
	return nil
}
```

- [ ] **Step 4: Add decoder functions to decode.go**

Add cases in `parseLine` switch (after `"uvfile"` case):

```go
case "schema":
	return parseSchema(r, args)
case "xigot":
	return parseXIGot(args)
case "xgimme":
	return parseXGimme(args)
case "xrow":
	return parseXRow(r, args)
```

Add parser functions at bottom of `decode.go`:

```go
// parseSchema decodes: schema TABLE VERSION HASH MTIME SIZE\nJSON\n
func parseSchema(r *bufio.Reader, args []string) (Card, error) {
	if len(args) != 5 {
		return nil, fmt.Errorf("xfer: schema requires 5 args, got %d", len(args))
	}
	version, err := strconv.Atoi(args[1])
	if err != nil {
		return nil, fmt.Errorf("xfer: schema version: %w", err)
	}
	mtime, err := strconv.ParseInt(args[3], 10, 64)
	if err != nil {
		return nil, fmt.Errorf("xfer: schema mtime: %w", err)
	}
	size, err := strconv.Atoi(args[4])
	if err != nil {
		return nil, fmt.Errorf("xfer: schema size: %w", err)
	}
	content, err := readPayloadWithTrailingNewline(r, size)
	if err != nil {
		return nil, fmt.Errorf("xfer: schema payload: %w", err)
	}
	return &SchemaCard{
		Table:   args[0],
		Version: version,
		Hash:    args[2],
		MTime:   mtime,
		Content: content,
	}, nil
}

func parseXIGot(args []string) (Card, error) {
	if len(args) != 3 {
		return nil, fmt.Errorf("xfer: xigot requires 3 args, got %d", len(args))
	}
	mtime, err := strconv.ParseInt(args[2], 10, 64)
	if err != nil {
		return nil, fmt.Errorf("xfer: xigot mtime: %w", err)
	}
	return &XIGotCard{Table: args[0], PKHash: args[1], MTime: mtime}, nil
}

func parseXGimme(args []string) (Card, error) {
	if len(args) != 2 {
		return nil, fmt.Errorf("xfer: xgimme requires 2 args, got %d", len(args))
	}
	return &XGimmeCard{Table: args[0], PKHash: args[1]}, nil
}

// parseXRow decodes: xrow TABLE PK_HASH MTIME SIZE\nJSON\n
func parseXRow(r *bufio.Reader, args []string) (Card, error) {
	if len(args) != 4 {
		return nil, fmt.Errorf("xfer: xrow requires 4 args, got %d", len(args))
	}
	mtime, err := strconv.ParseInt(args[2], 10, 64)
	if err != nil {
		return nil, fmt.Errorf("xfer: xrow mtime: %w", err)
	}
	size, err := strconv.Atoi(args[3])
	if err != nil {
		return nil, fmt.Errorf("xfer: xrow size: %w", err)
	}
	content, err := readPayloadWithTrailingNewline(r, size)
	if err != nil {
		return nil, fmt.Errorf("xfer: xrow payload: %w", err)
	}
	return &XRowCard{
		Table:   args[0],
		PKHash:  args[1],
		MTime:   mtime,
		Content: content,
	}, nil
}
```

- [ ] **Step 5: Run round-trip tests to verify they pass**

Run: `cd go-libfossil && go test -buildvcs=false ./xfer/ -run 'TestSchemaCard_RoundTrip|TestXIGotCard_RoundTrip|TestXGimmeCard_RoundTrip|TestXRowCard_RoundTrip' -v`
Expected: PASS

- [ ] **Step 6: Run full xfer test suite**

Run: `cd go-libfossil && go test -buildvcs=false ./xfer/ -v`
Expected: All existing + new tests pass

- [ ] **Step 7: Commit**

```bash
git add go-libfossil/xfer/encode.go go-libfossil/xfer/decode.go go-libfossil/xfer/card_tablesync_test.go
git commit -m "feat(xfer): encode/decode for schema, xigot, xgimme, xrow cards"
```

---

### Task 3: Table Sync Core (repo layer)

**Files:**
- Create: `go-libfossil/repo/tablesync.go`
- Test: `go-libfossil/repo/tablesync_test.go`

This task implements the DB-level operations: schema table management, extension table DDL, row CRUD, PK hash computation, and catalog hash.

- [ ] **Step 1: Write failing tests for schema CRUD and table name validation**

Create `go-libfossil/repo/tablesync_test.go`:

```go
package repo

import (
	"path/filepath"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/simio"
)

func setupTSRepo(t *testing.T) *Repo {
	t.Helper()
	path := filepath.Join(t.TempDir(), "ts.fossil")
	r, err := Create(path, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	t.Cleanup(func() { r.Close() })
	return r
}

func TestValidateTableName(t *testing.T) {
	valid := []string{"peer_registry", "config", "a", "abc_123"}
	for _, name := range valid {
		if err := ValidateTableName(name); err != nil {
			t.Errorf("ValidateTableName(%q) = %v, want nil", name, err)
		}
	}
	invalid := []string{"", "Peer", "123abc", "drop;table", "blob", "delta", "event", "unversioned", "x_reserved"}
	for _, name := range invalid {
		if err := ValidateTableName(name); err == nil {
			t.Errorf("ValidateTableName(%q) = nil, want error", name)
		}
	}
}

func TestPKHash(t *testing.T) {
	h := PKHash(map[string]any{"peer_id": "leaf-01"})
	if h == "" {
		t.Fatal("PKHash returned empty")
	}
	// Same input should produce same hash
	h2 := PKHash(map[string]any{"peer_id": "leaf-01"})
	if h != h2 {
		t.Fatalf("PKHash not deterministic: %q vs %q", h, h2)
	}
	// Different input should differ
	h3 := PKHash(map[string]any{"peer_id": "leaf-02"})
	if h == h3 {
		t.Fatal("different inputs produced same hash")
	}
}

func TestEnsureSyncSchema(t *testing.T) {
	r := setupTSRepo(t)
	if err := EnsureSyncSchema(r.DB()); err != nil {
		t.Fatalf("EnsureSyncSchema: %v", err)
	}
	// Idempotent
	if err := EnsureSyncSchema(r.DB()); err != nil {
		t.Fatalf("EnsureSyncSchema (2nd): %v", err)
	}
}

func TestRegisterAndListSyncedTables(t *testing.T) {
	r := setupTSRepo(t)
	if err := EnsureSyncSchema(r.DB()); err != nil {
		t.Fatal(err)
	}

	def := TableDef{
		Columns:  []ColumnDef{{Name: "peer_id", Type: "text", PK: true}, {Name: "addr", Type: "text"}},
		Conflict: "self-write",
	}
	if err := RegisterSyncedTable(r.DB(), "peer_registry", def, 1711300000); err != nil {
		t.Fatalf("RegisterSyncedTable: %v", err)
	}

	tables, err := ListSyncedTables(r.DB())
	if err != nil {
		t.Fatalf("ListSyncedTables: %v", err)
	}
	if len(tables) != 1 || tables[0].Name != "peer_registry" {
		t.Fatalf("got %+v, want 1 table named peer_registry", tables)
	}

	// Verify extension table was created
	var count int
	if err := r.DB().QueryRow("SELECT count(*) FROM x_peer_registry").Scan(&count); err != nil {
		t.Fatalf("x_peer_registry should exist: %v", err)
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd go-libfossil && go test -buildvcs=false ./repo/ -run 'TestValidateTableName|TestPKHash|TestEnsureSyncSchema|TestRegisterAndListSyncedTables' -v`
Expected: FAIL — functions not defined

- [ ] **Step 3: Implement tablesync.go**

Create `go-libfossil/repo/tablesync.go`:

```go
package repo

import (
	"crypto/sha1"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"github.com/dmestas/edgesync/go-libfossil/db"
)

// reservedTables are Fossil core tables that cannot be used as extension table names.
var reservedTables = map[string]bool{
	"blob": true, "delta": true, "event": true, "mlink": true,
	"unversioned": true, "config": true, "shun": true, "rcvfrom": true,
	"unclustered": true, "unsent": true, "phantom": true, "tag": true,
	"tagxref": true, "leaf": true, "filename": true, "mlink": true,
}

var nameRe = regexp.MustCompile(`^[a-z_][a-z0-9_]*$`)

// ValidateTableName checks that name is a safe, non-reserved table name.
// The x_ prefix is added automatically — user-supplied names must not include it.
func ValidateTableName(name string) error {
	if name == "" {
		return fmt.Errorf("table name must not be empty")
	}
	if !nameRe.MatchString(name) {
		return fmt.Errorf("table name %q must match [a-z_][a-z0-9_]*", name)
	}
	if reservedTables[name] {
		return fmt.Errorf("table name %q is reserved", name)
	}
	if strings.HasPrefix(name, "x_") {
		return fmt.Errorf("table name %q must not start with x_ (prefix added automatically)", name)
	}
	return nil
}

// ValidateColumnName checks that a column name is safe for SQL use.
func ValidateColumnName(name string) error {
	if name == "" {
		return fmt.Errorf("column name must not be empty")
	}
	if !nameRe.MatchString(name) {
		return fmt.Errorf("column name %q must match [a-z_][a-z0-9_]*", name)
	}
	return nil
}

// ColumnDef defines a column in a synced extension table.
type ColumnDef struct {
	Name string `json:"name"`
	Type string `json:"type"`
	PK   bool   `json:"pk,omitempty"`
}

// TableDef defines a synced extension table schema.
type TableDef struct {
	Columns  []ColumnDef `json:"columns"`
	Conflict string      `json:"conflict"`
}

// SyncedTable is a registered synced table loaded from _sync_schema.
type SyncedTable struct {
	Name     string
	Version  int
	Def      TableDef
	MTime    int64
	Hash     string
}

const schemaSyncSchema = `CREATE TABLE IF NOT EXISTS _sync_schema (
	table_name TEXT PRIMARY KEY,
	version    INTEGER NOT NULL DEFAULT 1,
	columns    TEXT NOT NULL,
	conflict   TEXT NOT NULL,
	mtime      INTEGER NOT NULL,
	hash       TEXT NOT NULL,
	origin     TEXT NOT NULL DEFAULT 'local'
);`

// EnsureSyncSchema creates the _sync_schema table if it doesn't exist.
func EnsureSyncSchema(d *db.DB) error {
	if d == nil {
		panic("repo.EnsureSyncSchema: d must not be nil")
	}
	_, err := d.Exec(schemaSyncSchema)
	return err
}

// RegisterSyncedTable registers a table definition and creates the extension table.
func RegisterSyncedTable(d *db.DB, name string, def TableDef, mtime int64) error {
	if d == nil {
		panic("repo.RegisterSyncedTable: d must not be nil")
	}
	if err := ValidateTableName(name); err != nil {
		return err
	}
	for _, c := range def.Columns {
		if err := ValidateColumnName(c.Name); err != nil {
			return fmt.Errorf("table %s: %w", name, err)
		}
	}

	colJSON, err := json.Marshal(def.Columns)
	if err != nil {
		return fmt.Errorf("marshal columns: %w", err)
	}
	defJSON, err := json.Marshal(def)
	if err != nil {
		return fmt.Errorf("marshal def: %w", err)
	}
	hash := sha1Hex(defJSON)

	_, err = d.Exec(
		`INSERT OR REPLACE INTO _sync_schema(table_name, version, columns, conflict, mtime, hash, origin)
		 VALUES(?, 1, ?, ?, ?, ?, 'local')`,
		name, string(colJSON), def.Conflict, mtime, hash,
	)
	if err != nil {
		return fmt.Errorf("register: %w", err)
	}

	return createExtensionTable(d, name, def)
}

// createExtensionTable runs CREATE TABLE IF NOT EXISTS for x_<name>.
func createExtensionTable(d *db.DB, name string, def TableDef) error {
	var cols []string
	var pkCols []string
	for _, c := range def.Columns {
		cols = append(cols, fmt.Sprintf("%s %s", c.Name, strings.ToUpper(c.Type)))
		if c.PK {
			pkCols = append(pkCols, c.Name)
		}
	}
	// Injected columns
	cols = append(cols, "mtime INTEGER NOT NULL")
	if def.Conflict == "owner-write" {
		cols = append(cols, "_owner TEXT NOT NULL DEFAULT ''")
	}

	ddl := fmt.Sprintf("CREATE TABLE IF NOT EXISTS x_%s (\n  %s", name, strings.Join(cols, ",\n  "))
	if len(pkCols) > 0 {
		ddl += fmt.Sprintf(",\n  PRIMARY KEY (%s)", strings.Join(pkCols, ", "))
	}
	ddl += "\n)"

	_, err := d.Exec(ddl)
	return err
}

// ListSyncedTables returns all registered synced tables.
func ListSyncedTables(d *db.DB) ([]SyncedTable, error) {
	if d == nil {
		panic("repo.ListSyncedTables: d must not be nil")
	}
	rows, err := d.Query("SELECT table_name, version, columns, conflict, mtime, hash FROM _sync_schema ORDER BY table_name")
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var tables []SyncedTable
	for rows.Next() {
		var st SyncedTable
		var colJSON string
		if err := rows.Scan(&st.Name, &st.Version, &colJSON, &st.Def.Conflict, &st.MTime, &st.Hash); err != nil {
			return nil, err
		}
		if err := json.Unmarshal([]byte(colJSON), &st.Def.Columns); err != nil {
			return nil, fmt.Errorf("unmarshal columns for %s: %w", st.Name, err)
		}
		tables = append(tables, st)
	}
	return tables, rows.Err()
}

// PKHash computes the primary key hash for a row.
// Input: map of PK column names to values. Keys are sorted before hashing.
func PKHash(pk map[string]any) string {
	data, _ := json.Marshal(sortedMap(pk))
	return sha1Hex(data)
}

// UpsertXRow inserts or replaces a row in x_<table>.
func UpsertXRow(d *db.DB, table string, row map[string]any, mtime int64) error {
	if d == nil {
		panic("repo.UpsertXRow: d must not be nil")
	}
	if err := ValidateTableName(table); err != nil {
		return err
	}

	row["mtime"] = mtime

	var cols []string
	var placeholders []string
	var vals []any
	// Sort keys for deterministic column order
	keys := sortedKeys(row)
	for _, k := range keys {
		cols = append(cols, k)
		placeholders = append(placeholders, "?")
		vals = append(vals, row[k])
	}

	sql := fmt.Sprintf("INSERT OR REPLACE INTO x_%s(%s) VALUES(%s)",
		table, strings.Join(cols, ","), strings.Join(placeholders, ","))
	_, err := d.Exec(sql, vals...)
	return err
}

// ListXRows returns all rows from x_<table> as maps.
func ListXRows(d *db.DB, table string, def TableDef) ([]map[string]any, error) {
	if d == nil {
		panic("repo.ListXRows: d must not be nil")
	}
	rows, err := d.Query(fmt.Sprintf("SELECT * FROM x_%s ORDER BY mtime", table))
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	colNames, err := rows.Columns()
	if err != nil {
		return nil, err
	}

	var result []map[string]any
	for rows.Next() {
		vals := make([]any, len(colNames))
		ptrs := make([]any, len(colNames))
		for i := range vals {
			ptrs[i] = &vals[i]
		}
		if err := rows.Scan(ptrs...); err != nil {
			return nil, err
		}
		m := make(map[string]any, len(colNames))
		for i, name := range colNames {
			m[name] = vals[i]
		}
		result = append(result, m)
	}
	return result, rows.Err()
}

// LookupXRow looks up a single row by PK hash.
func LookupXRow(d *db.DB, table string, def TableDef, pkHash string) (map[string]any, int64, error) {
	allRows, err := ListXRows(d, table, def)
	if err != nil {
		return nil, 0, err
	}
	pkCols := pkColumns(def)
	for _, row := range allRows {
		pk := make(map[string]any)
		for _, c := range pkCols {
			pk[c] = row[c]
		}
		if PKHash(pk) == pkHash {
			mtime, _ := row["mtime"].(int64)
			return row, mtime, nil
		}
	}
	return nil, 0, nil
}

// CatalogHash computes a SHA1 catalog hash for all rows in x_<table>.
// Format: "pk_hash mtime\n" per row, sorted by pk_hash.
func CatalogHash(d *db.DB, table string, def TableDef) (string, error) {
	rows, err := ListXRows(d, table, def)
	if err != nil {
		return "", err
	}
	pkCols := pkColumns(def)

	type entry struct {
		hash  string
		mtime int64
	}
	var entries []entry
	for _, row := range rows {
		pk := make(map[string]any)
		for _, c := range pkCols {
			pk[c] = row[c]
		}
		mtime, _ := row["mtime"].(int64)
		entries = append(entries, entry{hash: PKHash(pk), mtime: mtime})
	}
	sort.Slice(entries, func(i, j int) bool { return entries[i].hash < entries[j].hash })

	h := sha1.New()
	for _, e := range entries {
		fmt.Fprintf(h, "%s %s\n", e.hash, strconv.FormatInt(e.mtime, 10))
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

func pkColumns(def TableDef) []string {
	var pks []string
	for _, c := range def.Columns {
		if c.PK {
			pks = append(pks, c.Name)
		}
	}
	return pks
}

func sortedKeys(m map[string]any) []string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return keys
}

func sortedMap(m map[string]any) map[string]any {
	return m // json.Marshal sorts keys by default
}

func sha1Hex(data []byte) string {
	h := sha1.Sum(data)
	return hex.EncodeToString(h[:])
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd go-libfossil && go test -buildvcs=false ./repo/ -run 'TestValidateTableName|TestPKHash|TestEnsureSyncSchema|TestRegisterAndListSyncedTables' -v`
Expected: PASS

- [ ] **Step 5: Write and run tests for UpsertXRow, LookupXRow, CatalogHash**

Append to `go-libfossil/repo/tablesync_test.go`:

```go
func TestUpsertAndLookupXRow(t *testing.T) {
	r := setupTSRepo(t)
	EnsureSyncSchema(r.DB())
	def := TableDef{
		Columns:  []ColumnDef{{Name: "peer_id", Type: "text", PK: true}, {Name: "addr", Type: "text"}},
		Conflict: "self-write",
	}
	RegisterSyncedTable(r.DB(), "peer_registry", def, 1711300000)

	row := map[string]any{"peer_id": "leaf-01", "addr": "10.0.0.1:9000"}
	if err := UpsertXRow(r.DB(), "peer_registry", row, 1711300000); err != nil {
		t.Fatalf("UpsertXRow: %v", err)
	}

	pkHash := PKHash(map[string]any{"peer_id": "leaf-01"})
	got, mtime, err := LookupXRow(r.DB(), "peer_registry", def, pkHash)
	if err != nil {
		t.Fatalf("LookupXRow: %v", err)
	}
	if got == nil {
		t.Fatal("row not found")
	}
	if mtime != 1711300000 {
		t.Fatalf("mtime = %d, want 1711300000", mtime)
	}
}

func TestCatalogHash(t *testing.T) {
	r := setupTSRepo(t)
	EnsureSyncSchema(r.DB())
	def := TableDef{
		Columns:  []ColumnDef{{Name: "peer_id", Type: "text", PK: true}},
		Conflict: "mtime-wins",
	}
	RegisterSyncedTable(r.DB(), "cfg", def, 100)

	UpsertXRow(r.DB(), "cfg", map[string]any{"peer_id": "a"}, 100)
	UpsertXRow(r.DB(), "cfg", map[string]any{"peer_id": "b"}, 200)

	h1, err := CatalogHash(r.DB(), "cfg", def)
	if err != nil {
		t.Fatal(err)
	}
	if h1 == "" {
		t.Fatal("empty catalog hash")
	}

	// Same data = same hash
	h2, _ := CatalogHash(r.DB(), "cfg", def)
	if h1 != h2 {
		t.Fatal("catalog hash not deterministic")
	}

	// Change data = different hash
	UpsertXRow(r.DB(), "cfg", map[string]any{"peer_id": "c"}, 300)
	h3, _ := CatalogHash(r.DB(), "cfg", def)
	if h1 == h3 {
		t.Fatal("catalog hash unchanged after insert")
	}
}
```

Run: `cd go-libfossil && go test -buildvcs=false ./repo/ -run 'TestUpsertAndLookupXRow|TestCatalogHash' -v`
Expected: PASS

- [ ] **Step 6: Run full repo test suite**

Run: `cd go-libfossil && go test -buildvcs=false ./repo/ -v`
Expected: All pass

- [ ] **Step 7: Commit**

```bash
git add go-libfossil/repo/tablesync.go go-libfossil/repo/tablesync_test.go
git commit -m "feat(repo): table sync core — schema registry, extension table DDL, row CRUD, catalog hash"
```

---

### Task 4: Handler-Side Table Sync

**Files:**
- Create: `go-libfossil/sync/handler_tablesync.go`
- Modify: `go-libfossil/sync/handler.go` (add fields to `handler` struct, wire into `handleControlCard` and `handleDataCard`)
- Test: `go-libfossil/sync/handler_tablesync_test.go`

- [ ] **Step 1: Write failing tests for handler schema card processing**

Create `go-libfossil/sync/handler_tablesync_test.go`:

```go
package sync

import (
	"context"
	"encoding/json"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

func TestHandleSchemaCard(t *testing.T) {
	r := setupSyncTestRepo(t)

	def := repo.TableDef{
		Columns:  []repo.ColumnDef{{Name: "peer_id", Type: "text", PK: true}},
		Conflict: "mtime-wins",
	}
	defJSON, _ := json.Marshal(def)

	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.PushCard{ServerCode: "s", ProjectCode: "p"},
		&xfer.PullCard{ServerCode: "s", ProjectCode: "p"},
		&xfer.SchemaCard{
			Table: "test_table", Version: 1, Hash: "abc",
			MTime: 100, Content: defJSON,
		},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}
	_ = resp

	// Verify table was created
	var count int
	if err := r.DB().QueryRow("SELECT count(*) FROM x_test_table").Scan(&count); err != nil {
		t.Fatalf("x_test_table should exist: %v", err)
	}
}

func TestHandleXIGotXGimmeXRow(t *testing.T) {
	r := setupSyncTestRepo(t)

	// Register table first
	def := repo.TableDef{
		Columns:  []repo.ColumnDef{{Name: "id", Type: "text", PK: true}, {Name: "val", Type: "text"}},
		Conflict: "mtime-wins",
	}
	repo.EnsureSyncSchema(r.DB())
	repo.RegisterSyncedTable(r.DB(), "kv", def, 100)

	// Insert a row locally
	repo.UpsertXRow(r.DB(), "kv", map[string]any{"id": "local-key", "val": "local-val"}, 200)
	localPK := repo.PKHash(map[string]any{"id": "local-key"})

	// Simulate remote sending xigot for a row we don't have
	remotePK := repo.PKHash(map[string]any{"id": "remote-key"})
	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.PushCard{ServerCode: "s", ProjectCode: "p"},
		&xfer.PullCard{ServerCode: "s", ProjectCode: "p"},
		&xfer.XIGotCard{Table: "kv", PKHash: remotePK, MTime: 300},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}

	// Should emit xgimme for the remote row we're missing
	gimmes := findCards[*xfer.XGimmeCard](resp)
	if len(gimmes) == 0 {
		t.Fatal("expected xgimme for missing remote row")
	}
	if gimmes[0].PKHash != remotePK {
		t.Fatalf("xgimme pk = %q, want %q", gimmes[0].PKHash, remotePK)
	}

	// Should emit xigot for local row the remote doesn't have
	igots := findCards[*xfer.XIGotCard](resp)
	found := false
	for _, ig := range igots {
		if ig.PKHash == localPK {
			found = true
		}
	}
	if !found {
		t.Fatal("expected xigot for local row")
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd go-libfossil && go test -buildvcs=false ./sync/ -run 'TestHandleSchemaCard|TestHandleXIGotXGimmeXRow' -v`
Expected: FAIL

- [ ] **Step 3: Add handler struct fields and wire control/data dispatch**

Modify `go-libfossil/sync/handler.go`:

Add to `handler` struct:
```go
syncedTables map[string]*repo.SyncedTable
xrowsSent    int
xrowsRecvd   int
loginUser    string
```

Add to `handleControlCard` switch:
```go
case *xfer.LoginCard:
	h.loginUser = c.User
case *xfer.SchemaCard:
	h.handleSchemaCard(c)
```

Also update the existing LoginCard case (replace `_ = c`).

Add to `handleDataCard` switch:
```go
case *xfer.XIGotCard:
	return h.handleXIGot(c)
case *xfer.XGimmeCard:
	return h.handleXGimme(c)
case *xfer.XRowCard:
	return h.handleXRow(c)
```

Add to `process()`, after UV catalog and before returning response — emit xigot for all synced table rows:
```go
if h.pullOK {
	if err := h.emitXIGots(); err != nil {
		return nil, err
	}
}
```

- [ ] **Step 4: Create handler_tablesync.go with handler methods**

Create `go-libfossil/sync/handler_tablesync.go`:

```go
package sync

import (
	"encoding/json"
	"fmt"

	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

func (h *handler) loadSyncedTables() {
	if err := repo.EnsureSyncSchema(h.repo.DB()); err != nil {
		return
	}
	tables, err := repo.ListSyncedTables(h.repo.DB())
	if err != nil {
		return
	}
	h.syncedTables = make(map[string]*repo.SyncedTable, len(tables))
	for i := range tables {
		h.syncedTables[tables[i].Name] = &tables[i]
	}
}

func (h *handler) handleSchemaCard(c *xfer.SchemaCard) {
	if c == nil {
		panic("handler.handleSchemaCard: c must not be nil")
	}
	if err := repo.ValidateTableName(c.Table); err != nil {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("schema %s: %v", c.Table, err),
		})
		return
	}

	var def repo.TableDef
	if err := json.Unmarshal(c.Content, &def); err != nil {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("schema %s: invalid JSON: %v", c.Table, err),
		})
		return
	}

	if err := repo.EnsureSyncSchema(h.repo.DB()); err != nil {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("schema %s: %v", c.Table, err),
		})
		return
	}

	// Check if we already have this table at same or newer version
	if existing, ok := h.syncedTables[c.Table]; ok && existing.Version >= c.Version {
		return
	}

	if err := repo.RegisterSyncedTable(h.repo.DB(), c.Table, def, c.MTime); err != nil {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("schema %s: register: %v", c.Table, err),
		})
		return
	}

	// Refresh local cache
	st := &repo.SyncedTable{Name: c.Table, Version: c.Version, Def: def, MTime: c.MTime, Hash: c.Hash}
	h.syncedTables[c.Table] = st
}

func (h *handler) handlePragmaXTableHash(table, hash string) {
	st, ok := h.syncedTables[table]
	if !ok {
		return
	}
	localHash, err := repo.CatalogHash(h.repo.DB(), table, st.Def)
	if err != nil || localHash == hash {
		return // in sync or error
	}
	// Not in sync — emit our catalog
	h.emitXIGotsForTable(st)
}

func (h *handler) handleXIGot(c *xfer.XIGotCard) error {
	if c == nil {
		panic("handler.handleXIGot: c must not be nil")
	}
	st, ok := h.syncedTables[c.Table]
	if !ok {
		return nil
	}
	local, localMtime, err := repo.LookupXRow(h.repo.DB(), c.Table, st.Def, c.PKHash)
	if err != nil {
		return fmt.Errorf("handler.handleXIGot: %w", err)
	}

	if local == nil {
		// We don't have this row — request it
		h.resp = append(h.resp, &xfer.XGimmeCard{Table: c.Table, PKHash: c.PKHash})
	} else if localMtime > c.MTime {
		// We have a newer version — send ours
		if err := h.sendXRow(c.Table, st, c.PKHash); err != nil {
			return err
		}
	}
	// If local mtime <= remote mtime, remote has same or newer — do nothing
	return nil
}

func (h *handler) handleXGimme(c *xfer.XGimmeCard) error {
	if c == nil {
		panic("handler.handleXGimme: c must not be nil")
	}
	st, ok := h.syncedTables[c.Table]
	if !ok {
		return nil
	}
	// BUGGIFY: 5% chance skip sending row to test client retry.
	if h.buggify != nil && h.buggify.Check("handler.handleXGimme.skip", 0.05) {
		return nil
	}
	return h.sendXRow(c.Table, st, c.PKHash)
}

func (h *handler) handleXRow(c *xfer.XRowCard) error {
	if c == nil {
		panic("handler.handleXRow: c must not be nil")
	}
	st, ok := h.syncedTables[c.Table]
	if !ok {
		return nil
	}
	if !h.pushOK {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("xrow %s rejected: no push card", c.Table),
		})
		return nil
	}

	// Conflict enforcement
	switch st.Def.Conflict {
	case "self-write":
		expectedPK := repo.PKHash(map[string]any{pkColumn(st.Def): h.loginUser})
		if c.PKHash != expectedPK {
			h.resp = append(h.resp, &xfer.ErrorCard{
				Message: fmt.Sprintf("self-write violation: %s", c.Table),
			})
			return nil
		}
	case "mtime-wins":
		local, localMtime, err := repo.LookupXRow(h.repo.DB(), c.Table, st.Def, c.PKHash)
		if err != nil {
			return err
		}
		if local != nil && localMtime >= c.MTime {
			return nil // local is newer
		}
	case "owner-write":
		local, _, err := repo.LookupXRow(h.repo.DB(), c.Table, st.Def, c.PKHash)
		if err != nil {
			return err
		}
		if local != nil {
			if owner, ok := local["_owner"]; ok && owner != "" && owner != h.loginUser {
				h.resp = append(h.resp, &xfer.ErrorCard{
					Message: fmt.Sprintf("owner-write violation: %s", c.Table),
				})
				return nil
			}
		}
	}

	// BUGGIFY: 3% chance reject a valid xrow to test client re-push.
	if h.buggify != nil && h.buggify.Check("handler.handleXRow.reject", 0.03) {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("buggify: rejected xrow %s/%s", c.Table, c.PKHash),
		})
		return nil
	}

	// Upsert
	var row map[string]any
	if err := json.Unmarshal(c.Content, &row); err != nil {
		return fmt.Errorf("handler.handleXRow: unmarshal: %w", err)
	}
	if st.Def.Conflict == "owner-write" {
		row["_owner"] = h.loginUser
	}
	if err := repo.UpsertXRow(h.repo.DB(), c.Table, row, c.MTime); err != nil {
		return fmt.Errorf("handler.handleXRow: upsert: %w", err)
	}
	h.xrowsRecvd++
	return nil
}

func (h *handler) sendXRow(table string, st *repo.SyncedTable, pkHash string) error {
	row, mtime, err := repo.LookupXRow(h.repo.DB(), table, st.Def, pkHash)
	if err != nil || row == nil {
		return err
	}
	content, err := json.Marshal(row)
	if err != nil {
		return err
	}
	h.resp = append(h.resp, &xfer.XRowCard{
		Table: table, PKHash: pkHash, MTime: mtime, Content: content,
	})
	h.xrowsSent++
	return nil
}

func (h *handler) emitXIGots() error {
	for _, st := range h.syncedTables {
		h.emitXIGotsForTable(st)
	}
	return nil
}

func (h *handler) emitXIGotsForTable(st *repo.SyncedTable) {
	rows, err := repo.ListXRows(h.repo.DB(), st.Name, st.Def)
	if err != nil {
		return
	}
	pkCols := make([]string, 0)
	for _, c := range st.Def.Columns {
		if c.PK {
			pkCols = append(pkCols, c.Name)
		}
	}
	for _, row := range rows {
		pk := make(map[string]any)
		for _, c := range pkCols {
			pk[c] = row[c]
		}
		mtime, _ := row["mtime"].(int64)
		h.resp = append(h.resp, &xfer.XIGotCard{
			Table: st.Name, PKHash: repo.PKHash(pk), MTime: mtime,
		})
	}
}

func pkColumn(def repo.TableDef) string {
	for _, c := range def.Columns {
		if c.PK {
			return c.Name
		}
	}
	return ""
}
```

- [ ] **Step 5: Wire into handler.go process()**

In `handler.go`, add `h.loadSyncedTables()` at the start of `process()` (after the existing nil checks).

Add pragma dispatch in `handleControlCard` for `xtable-hash`:
```go
case *xfer.PragmaCard:
	if c.Name == "uv-hash" && len(c.Values) >= 1 {
		// ... existing ...
	} else if c.Name == "xtable-hash" && len(c.Values) >= 2 {
		h.handlePragmaXTableHash(c.Values[0], c.Values[1])
	}
```

Add `emitXIGots()` call after `emitIGots()` in process():
```go
if h.pullOK {
	if err := h.emitXIGots(); err != nil {
		return nil, err
	}
}
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `cd go-libfossil && go test -buildvcs=false ./sync/ -run 'TestHandleSchemaCard|TestHandleXIGotXGimmeXRow' -v`
Expected: PASS

- [ ] **Step 7: Run full sync test suite**

Run: `cd go-libfossil && go test -buildvcs=false ./sync/ -v`
Expected: All pass

- [ ] **Step 8: Commit**

```bash
git add go-libfossil/sync/handler_tablesync.go go-libfossil/sync/handler.go go-libfossil/sync/handler_tablesync_test.go
git commit -m "feat(sync): handler-side table sync — schema, xigot, xgimme, xrow processing"
```

---

### Task 5: Client-Side Table Sync

**Files:**
- Create: `go-libfossil/sync/client_tablesync.go`
- Modify: `go-libfossil/sync/session.go` (add fields to `session` struct)
- Modify: `go-libfossil/sync/client.go` (wire into `buildRequest` and `processResponse`)
- Test: `go-libfossil/sync/client_tablesync_test.go`

- [ ] **Step 1: Write failing end-to-end test**

Create `go-libfossil/sync/client_tablesync_test.go`:

```go
package sync

import (
	"encoding/json"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/repo"
)

func TestTableSyncEndToEnd(t *testing.T) {
	server := setupSyncTestRepo(t)
	client := setupSyncTestRepo(t)

	// Register table on both sides
	def := repo.TableDef{
		Columns:  []repo.ColumnDef{{Name: "id", Type: "text", PK: true}, {Name: "val", Type: "text"}},
		Conflict: "mtime-wins",
	}
	repo.EnsureSyncSchema(server.DB())
	repo.RegisterSyncedTable(server.DB(), "kv", def, 100)
	repo.EnsureSyncSchema(client.DB())
	repo.RegisterSyncedTable(client.DB(), "kv", def, 100)

	// Insert a row on server
	repo.UpsertXRow(server.DB(), "kv", map[string]any{"id": "key1", "val": "server-val"}, 200)

	// Insert a different row on client
	repo.UpsertXRow(client.DB(), "kv", map[string]any{"id": "key2", "val": "client-val"}, 300)

	// Sync
	result, err := Sync(client, &MockTransport{Repo: server}, SyncOpts{
		Push: true, Pull: true,
		ProjectCode: "test", ServerCode: "test",
	})
	if err != nil {
		t.Fatalf("Sync: %v", err)
	}
	_ = result

	// Verify server has client's row
	rows, _ := repo.ListXRows(server.DB(), "kv", def)
	if len(rows) != 2 {
		t.Fatalf("server has %d rows, want 2", len(rows))
	}

	// Verify client has server's row
	rows, _ = repo.ListXRows(client.DB(), "kv", def)
	if len(rows) != 2 {
		t.Fatalf("client has %d rows, want 2", len(rows))
	}
}

func TestTableSyncSchemaDeployment(t *testing.T) {
	server := setupSyncTestRepo(t)
	client := setupSyncTestRepo(t)

	// Register table ONLY on server
	def := repo.TableDef{
		Columns:  []repo.ColumnDef{{Name: "id", Type: "text", PK: true}},
		Conflict: "mtime-wins",
	}
	defJSON, _ := json.Marshal(def)
	repo.EnsureSyncSchema(server.DB())
	repo.RegisterSyncedTable(server.DB(), "deployed", def, 100)
	_ = defJSON

	// Sync — client should receive schema card and create the table
	_, err := Sync(client, &MockTransport{Repo: server}, SyncOpts{
		Push: true, Pull: true,
		ProjectCode: "test", ServerCode: "test",
	})
	if err != nil {
		t.Fatalf("Sync: %v", err)
	}

	// Verify client has the table
	var count int
	if err := client.DB().QueryRow("SELECT count(*) FROM x_deployed").Scan(&count); err != nil {
		t.Fatalf("x_deployed should exist on client: %v", err)
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd go-libfossil && go test -buildvcs=false ./sync/ -run 'TestTableSyncEndToEnd|TestTableSyncSchemaDeployment' -v`
Expected: FAIL

- [ ] **Step 3: Add session fields**

In `go-libfossil/sync/session.go`, add to `session` struct:

```go
// Table sync state
xTableHashSent map[string]bool   // table -> true if pragma xtable-hash sent
xTableGimmes   map[string]map[string]bool // table -> pkHash -> true
xTableToSend   map[string]map[string]bool // table -> pkHash -> true
```

Initialize in `newSession`:
```go
xTableHashSent: make(map[string]bool),
xTableGimmes:   make(map[string]map[string]bool),
xTableToSend:   make(map[string]map[string]bool),
```

- [ ] **Step 4: Create client_tablesync.go**

Create `go-libfossil/sync/client_tablesync.go`:

```go
package sync

import (
	"encoding/json"
	"fmt"

	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

// buildXTableCards builds schema, pragma xtable-hash, and xigot cards.
func (s *session) buildXTableCards() ([]xfer.Card, error) {
	if err := repo.EnsureSyncSchema(s.repo.DB()); err != nil {
		return nil, err
	}
	tables, err := repo.ListSyncedTables(s.repo.DB())
	if err != nil {
		return nil, err
	}

	var cards []xfer.Card

	for _, st := range tables {
		// Emit schema card (every round — idempotent on receiver)
		defJSON, err := json.Marshal(st.Def)
		if err != nil {
			return nil, fmt.Errorf("marshal schema %s: %w", st.Name, err)
		}
		cards = append(cards, &xfer.SchemaCard{
			Table:   st.Name,
			Version: st.Version,
			Hash:    st.Hash,
			MTime:   st.MTime,
			Content: defJSON,
		})

		// Emit pragma xtable-hash on first round
		if !s.xTableHashSent[st.Name] {
			catalogHash, err := repo.CatalogHash(s.repo.DB(), st.Name, st.Def)
			if err != nil {
				return nil, err
			}
			cards = append(cards, &xfer.PragmaCard{
				Name:   "xtable-hash",
				Values: []string{st.Name, catalogHash},
			})
			s.xTableHashSent[st.Name] = true
		}

		// Emit xigot for all local rows
		rows, err := repo.ListXRows(s.repo.DB(), st.Name, st.Def)
		if err != nil {
			return nil, err
		}
		for _, row := range rows {
			pk := extractPK(st.Def, row)
			mtime, _ := row["mtime"].(int64)
			cards = append(cards, &xfer.XIGotCard{
				Table: st.Name, PKHash: repo.PKHash(pk), MTime: mtime,
			})
		}
	}

	// Emit xgimme cards
	for table, gimmes := range s.xTableGimmes {
		for pkHash := range gimmes {
			cards = append(cards, &xfer.XGimmeCard{Table: table, PKHash: pkHash})
			delete(gimmes, pkHash)
		}
	}

	// Emit xrow cards for requested sends
	for table, sends := range s.xTableToSend {
		tables, _ := repo.ListSyncedTables(s.repo.DB())
		for _, st := range tables {
			if st.Name != table {
				continue
			}
			for pkHash := range sends {
				row, mtime, err := repo.LookupXRow(s.repo.DB(), table, st.Def, pkHash)
				if err != nil || row == nil {
					continue
				}
				content, _ := json.Marshal(row)
				cards = append(cards, &xfer.XRowCard{
					Table: table, PKHash: pkHash, MTime: mtime, Content: content,
				})
				delete(sends, pkHash)
			}
		}
	}

	return cards, nil
}

// processXTableResponse handles table sync cards in a server response.
func (s *session) processXTableCard(card xfer.Card) error {
	switch c := card.(type) {
	case *xfer.SchemaCard:
		return s.handleXSchemaCard(c)
	case *xfer.XIGotCard:
		return s.handleXIGotResponse(c)
	case *xfer.XGimmeCard:
		return s.handleXGimmeResponse(c)
	case *xfer.XRowCard:
		return s.handleXRowResponse(c)
	}
	return nil
}

func (s *session) handleXSchemaCard(c *xfer.SchemaCard) error {
	if err := repo.ValidateTableName(c.Table); err != nil {
		return nil // skip invalid
	}
	var def repo.TableDef
	if err := json.Unmarshal(c.Content, &def); err != nil {
		return nil
	}
	if err := repo.EnsureSyncSchema(s.repo.DB()); err != nil {
		return err
	}
	return repo.RegisterSyncedTable(s.repo.DB(), c.Table, def, c.MTime)
}

func (s *session) handleXIGotResponse(c *xfer.XIGotCard) error {
	tables, _ := repo.ListSyncedTables(s.repo.DB())
	for _, st := range tables {
		if st.Name != c.Table {
			continue
		}
		local, localMtime, err := repo.LookupXRow(s.repo.DB(), c.Table, st.Def, c.PKHash)
		if err != nil {
			return err
		}
		if local == nil {
			// We don't have it — request it
			if s.xTableGimmes[c.Table] == nil {
				s.xTableGimmes[c.Table] = make(map[string]bool)
			}
			s.xTableGimmes[c.Table][c.PKHash] = true
		} else if localMtime > c.MTime {
			// We have newer — queue send
			if s.xTableToSend[c.Table] == nil {
				s.xTableToSend[c.Table] = make(map[string]bool)
			}
			s.xTableToSend[c.Table][c.PKHash] = true
		}
	}
	return nil
}

func (s *session) handleXGimmeResponse(c *xfer.XGimmeCard) error {
	if s.xTableToSend[c.Table] == nil {
		s.xTableToSend[c.Table] = make(map[string]bool)
	}
	s.xTableToSend[c.Table][c.PKHash] = true
	return nil
}

func (s *session) handleXRowResponse(c *xfer.XRowCard) error {
	var row map[string]any
	if err := json.Unmarshal(c.Content, &row); err != nil {
		return fmt.Errorf("xrow unmarshal: %w", err)
	}
	if err := repo.UpsertXRow(s.repo.DB(), c.Table, row, c.MTime); err != nil {
		return fmt.Errorf("xrow upsert: %w", err)
	}
	// Remove from gimmes
	delete(s.xTableGimmes[c.Table], c.PKHash)
	return nil
}

func extractPK(def repo.TableDef, row map[string]any) map[string]any {
	pk := make(map[string]any)
	for _, c := range def.Columns {
		if c.PK {
			pk[c.Name] = row[c.Name]
		}
	}
	return pk
}
```

- [ ] **Step 5: Wire into buildRequest and processResponse in client.go**

In `buildRequest` (after UV gimme cards, before login card):

```go
// Table sync cards
xTableCards, err := s.buildXTableCards()
if err != nil {
	return nil, fmt.Errorf("buildRequest xtable: %w", err)
}
cards = append(cards, xTableCards...)
```

In `processResponse`, add cases to the switch:

```go
case *xfer.SchemaCard, *xfer.XIGotCard, *xfer.XGimmeCard, *xfer.XRowCard:
	if err := s.processXTableCard(card); err != nil {
		return false, err
	}
```

Add table sync convergence check (after UV convergence):
```go
// Table sync convergence
for _, gimmes := range s.xTableGimmes {
	if len(gimmes) > 0 {
		return false, nil
	}
}
for _, sends := range s.xTableToSend {
	if len(sends) > 0 {
		return false, nil
	}
}
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `cd go-libfossil && go test -buildvcs=false ./sync/ -run 'TestTableSyncEndToEnd|TestTableSyncSchemaDeployment' -v`
Expected: PASS

- [ ] **Step 7: Run full test suite**

Run: `make test`
Expected: All pass

- [ ] **Step 8: Commit**

```bash
git add go-libfossil/sync/client_tablesync.go go-libfossil/sync/session.go go-libfossil/sync/client.go go-libfossil/sync/client_tablesync_test.go
git commit -m "feat(sync): client-side table sync — schema deployment, xigot/xgimme/xrow exchange"
```

---

### Task 6: Observer Integration

**Files:**
- Modify: `go-libfossil/sync/observer.go` (add struct types + methods)
- Modify: `leaf/telemetry/observer.go` (OTelObserver implementation)
- Modify: `leaf/telemetry/noop.go` (WASM stub)

- [ ] **Step 1: Add struct types and methods to Observer interface**

In `go-libfossil/sync/observer.go`, add struct types:

```go
// TableSyncStart describes the beginning of table sync for one table.
type TableSyncStart struct {
	Table     string
	LocalRows int
}

// TableSyncEnd describes the result of table sync for one table.
type TableSyncEnd struct {
	Table    string
	Sent     int
	Received int
}
```

Add to `Observer` interface:

```go
TableSyncStarted(ctx context.Context, info TableSyncStart)
TableSyncCompleted(ctx context.Context, info TableSyncEnd)
```

Add to `nopObserver`:

```go
func (nopObserver) TableSyncStarted(_ context.Context, _ TableSyncStart)  {}
func (nopObserver) TableSyncCompleted(_ context.Context, _ TableSyncEnd)  {}
```

- [ ] **Step 2: Update OTelObserver in leaf/telemetry/observer.go**

Add to `OTelObserver` (after `HandleCompleted`):

```go
func (o *OTelObserver) TableSyncStarted(ctx context.Context, info libsync.TableSyncStart) {
	span := trace.SpanFromContext(ctx)
	span.AddEvent("sync.table_sync.started", trace.WithAttributes(
		attribute.String("sync.table", info.Table),
		attribute.Int("sync.table.local_rows", info.LocalRows),
	))
}

func (o *OTelObserver) TableSyncCompleted(ctx context.Context, info libsync.TableSyncEnd) {
	span := trace.SpanFromContext(ctx)
	span.AddEvent("sync.table_sync.completed", trace.WithAttributes(
		attribute.String("sync.table", info.Table),
		attribute.Int("sync.table.rows_sent", info.Sent),
		attribute.Int("sync.table.rows_received", info.Received),
	))
}
```

- [ ] **Step 3: Update WASM stub in leaf/telemetry/noop.go**

Add to `OTelObserver` WASM stub:

```go
func (*OTelObserver) TableSyncStarted(_ context.Context, _ libsync.TableSyncStart) {}
func (*OTelObserver) TableSyncCompleted(_ context.Context, _ libsync.TableSyncEnd) {}
```

- [ ] **Step 4: Verify all modules compile**

Run: `go build -buildvcs=false ./...`
Expected: Clean build

- [ ] **Step 5: Wire observer calls in handler_tablesync.go**

In `emitXIGotsForTable`, add at start:
```go
obs.TableSyncStarted(ctx, TableSyncStart{Table: st.Name, LocalRows: len(rows)})
```

In `HandleSyncWithOpts` (handler.go), add table sync counts to `HandleEnd`:
```go
obs.HandleCompleted(ctx, HandleEnd{
	// ... existing fields ...
	XRowsSent:    h.xrowsSent,
	XRowsReceived: h.xrowsRecvd,
})
```

- [ ] **Step 6: Run full test suite**

Run: `make test`
Expected: All pass

- [ ] **Step 7: Commit**

```bash
git add go-libfossil/sync/observer.go leaf/telemetry/observer.go leaf/telemetry/noop.go go-libfossil/sync/handler_tablesync.go
git commit -m "feat(sync): observer hooks for table sync — TableSyncStarted, TableSyncCompleted"
```

---

### Task 7: Peer Registry (First Consumer)

**Files:**
- Create: `leaf/agent/peer_registry.go`
- Modify: `leaf/agent/config.go` (add `PeerID` field + default)
- Test: `leaf/agent/peer_registry_test.go`

- [ ] **Step 1: Add PeerID to Config**

In `leaf/agent/config.go`, add field to `Config` struct:

```go
// PeerID uniquely identifies this leaf agent in the peer registry.
// Defaults to hostname if empty.
PeerID string
```

In `applyDefaults()`, add:

```go
if c.PeerID == "" {
	if h, err := os.Hostname(); err == nil {
		c.PeerID = h
	} else {
		c.PeerID = "unknown"
	}
}
```

- [ ] **Step 2: Write failing test for peer registry**

Create `leaf/agent/peer_registry_test.go`:

```go
package agent

import (
	"path/filepath"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
)

func TestEnsureAndSeedPeerRegistry(t *testing.T) {
	repoPath := filepath.Join(t.TempDir(), "test.fossil")
	r, err := repo.Create(repoPath, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()

	a := &Agent{
		repo:   r,
		env:    simio.RealEnv(),
		config: Config{PeerID: "test-peer", ServeHTTPAddr: ":9000"},
	}

	if err := a.ensurePeerRegistry(); err != nil {
		t.Fatalf("ensurePeerRegistry: %v", err)
	}
	if err := a.seedPeerRegistry(); err != nil {
		t.Fatalf("seedPeerRegistry: %v", err)
	}

	// Verify row exists
	pkHash := repo.PKHash(map[string]any{"peer_id": "test-peer"})
	row, _, err := repo.LookupXRow(r.DB(), "peer_registry", peerRegistryDef, pkHash)
	if err != nil {
		t.Fatal(err)
	}
	if row == nil {
		t.Fatal("peer registry row not found")
	}
}
```

- [ ] **Step 3: Run test to verify it fails**

Run: `cd leaf && go test -buildvcs=false ./agent/ -run TestEnsureAndSeedPeerRegistry -v`
Expected: FAIL

- [ ] **Step 4: Implement peer_registry.go**

Create `leaf/agent/peer_registry.go`:

```go
package agent

import (
	"fmt"
	"runtime"
	"strings"

	"github.com/dmestas/edgesync/go-libfossil/repo"
)

var peerRegistryDef = repo.TableDef{
	Columns: []repo.ColumnDef{
		{Name: "peer_id", Type: "text", PK: true},
		{Name: "last_sync", Type: "int"},
		{Name: "repo_hash", Type: "text"},
		{Name: "version", Type: "text"},
		{Name: "platform", Type: "text"},
		{Name: "capabilities", Type: "text"},
		{Name: "nats_subject", Type: "text"},
		{Name: "addr", Type: "text"},
	},
	Conflict: "self-write",
}

var buildVersion = "dev"

func (a *Agent) ensurePeerRegistry() error {
	if err := repo.EnsureSyncSchema(a.repo.DB()); err != nil {
		return fmt.Errorf("ensurePeerRegistry: %w", err)
	}
	return repo.RegisterSyncedTable(a.repo.DB(), "peer_registry", peerRegistryDef, a.env.Clock.Now().Unix())
}

func (a *Agent) seedPeerRegistry() error {
	row := map[string]any{
		"peer_id":      a.config.PeerID,
		"last_sync":    a.env.Clock.Now().Unix(),
		"version":      buildVersion,
		"platform":     runtime.GOOS + "/" + runtime.GOARCH,
		"capabilities": strings.Join(a.capabilities(), ","),
		"nats_subject": a.natsSubject(),
		"addr":         a.config.ServeHTTPAddr,
	}
	return repo.UpsertXRow(a.repo.DB(), "peer_registry", row, a.env.Clock.Now().Unix())
}

// updatePeerRegistryAfterSync updates last_sync and repo_hash after successful sync.
// Called from PostSyncHook.
func (a *Agent) updatePeerRegistryAfterSync() error {
	row := map[string]any{
		"peer_id":   a.config.PeerID,
		"last_sync": a.env.Clock.Now().Unix(),
	}
	return repo.UpsertXRow(a.repo.DB(), "peer_registry", row, a.env.Clock.Now().Unix())
}

func (a *Agent) capabilities() []string {
	var caps []string
	if a.config.ServeHTTPAddr != "" {
		caps = append(caps, "serve-http")
	}
	if a.config.ServeNATSEnabled {
		caps = append(caps, "serve-nats")
	}
	caps = append(caps, "push", "pull")
	return caps
}

func (a *Agent) natsSubject() string {
	if a.config.SubjectPrefix != "" {
		return a.config.SubjectPrefix + ".*.sync"
	}
	return ""
}
```

- [ ] **Step 5: Wire into agent startup and PostSyncHook**

In the agent's `Start()` method (or `New()`), add after repo is opened:

```go
if err := a.ensurePeerRegistry(); err != nil {
	a.logf("warn: peer registry: %v", err)
}
if err := a.seedPeerRegistry(); err != nil {
	a.logf("warn: seed peer registry: %v", err)
}
```

In the sync loop (agent.go around line 255 where PostSyncHook is called), chain the peer registry update:

```go
if a.config.PostSyncHook != nil {
	a.config.PostSyncHook(act.Result)
}
// Update peer registry after successful sync
if err := a.updatePeerRegistryAfterSync(); err != nil {
	a.logf("warn: update peer registry: %v", err)
}
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `cd leaf && go test -buildvcs=false ./agent/ -run TestEnsureAndSeedPeerRegistry -v`
Expected: PASS

- [ ] **Step 7: Run full agent test suite**

Run: `cd leaf && go test -buildvcs=false ./agent/ -v`
Expected: All pass

- [ ] **Step 8: Commit**

```bash
git add leaf/agent/peer_registry.go leaf/agent/config.go leaf/agent/peer_registry_test.go leaf/agent/agent.go
git commit -m "feat(agent): peer registry — auto-seed on startup, PostSyncHook update, first consumer of table sync"
```

---

### Task 8: DST Coverage

**Files:**
- Create: `dst/tablesync_test.go`

- [ ] **Step 1: Write convergence test**

Create `dst/tablesync_test.go`. Use existing DST patterns (check `dst/` for `MockFossil`, `SimNetwork`, `runSync` helpers):

```go
package dst

import (
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/repo"
)

func TestTableSync_Convergence(t *testing.T) {
	// Setup: 2 peers with same synced table
	env := newDSTEnv(t, 42)
	peer1 := env.newPeer("peer1")
	peer2 := env.newPeer("peer2")

	def := repo.TableDef{
		Columns:  []repo.ColumnDef{{Name: "id", Type: "text", PK: true}, {Name: "val", Type: "text"}},
		Conflict: "mtime-wins",
	}
	for _, p := range []*dstPeer{peer1, peer2} {
		repo.EnsureSyncSchema(p.repo.DB())
		repo.RegisterSyncedTable(p.repo.DB(), "kv", def, 100)
	}

	// Insert different rows
	repo.UpsertXRow(peer1.repo.DB(), "kv", map[string]any{"id": "a", "val": "from-1"}, 200)
	repo.UpsertXRow(peer2.repo.DB(), "kv", map[string]any{"id": "b", "val": "from-2"}, 300)

	// Sync until convergence
	env.syncUntilConverged(peer1, peer2)

	// Assert both have both rows
	for _, p := range []*dstPeer{peer1, peer2} {
		rows, err := repo.ListXRows(p.repo.DB(), "kv", def)
		if err != nil {
			t.Fatal(err)
		}
		if len(rows) != 2 {
			t.Fatalf("%s has %d rows, want 2", p.name, len(rows))
		}
	}
}

func TestTableSync_SelfWriteEnforcement(t *testing.T) {
	env := newDSTEnv(t, 42)
	peer1 := env.newPeer("peer1")
	peer2 := env.newPeer("peer2")

	def := repo.TableDef{
		Columns:  []repo.ColumnDef{{Name: "peer_id", Type: "text", PK: true}, {Name: "val", Type: "text"}},
		Conflict: "self-write",
	}
	for _, p := range []*dstPeer{peer1, peer2} {
		repo.EnsureSyncSchema(p.repo.DB())
		repo.RegisterSyncedTable(p.repo.DB(), "reg", def, 100)
	}

	// Peer1 writes its own row
	repo.UpsertXRow(peer1.repo.DB(), "reg", map[string]any{"peer_id": "peer1", "val": "hello"}, 200)

	// Sync — peer2 should receive peer1's row
	env.syncUntilConverged(peer1, peer2)

	// Peer2 should have peer1's row but should NOT be able to modify it
	// (enforcement is server-side in HandleSync via loginUser check)
	rows, _ := repo.ListXRows(peer2.repo.DB(), "reg", def)
	if len(rows) != 1 {
		t.Fatalf("peer2 has %d rows, want 1", len(rows))
	}
}

func TestTableSync_Buggify(t *testing.T) {
	// Use BUGGIFY-enabled env — sync should still converge
	env := newDSTEnvWithBuggify(t, 42)
	peer1 := env.newPeer("peer1")
	peer2 := env.newPeer("peer2")

	def := repo.TableDef{
		Columns:  []repo.ColumnDef{{Name: "id", Type: "text", PK: true}},
		Conflict: "mtime-wins",
	}
	for _, p := range []*dstPeer{peer1, peer2} {
		repo.EnsureSyncSchema(p.repo.DB())
		repo.RegisterSyncedTable(p.repo.DB(), "cfg", def, 100)
	}

	repo.UpsertXRow(peer1.repo.DB(), "cfg", map[string]any{"id": "x"}, 200)

	// Should converge despite buggify dropping xgimme cards
	env.syncUntilConverged(peer1, peer2)

	rows, _ := repo.ListXRows(peer2.repo.DB(), "cfg", def)
	if len(rows) != 1 {
		t.Fatalf("peer2 has %d rows, want 1 (buggify should not prevent convergence)", len(rows))
	}
}
```

Note: Adapt the test helper names (`newDSTEnv`, `dstPeer`, `syncUntilConverged`) to match existing DST patterns in `dst/`. The intent is clear — the implementing agent should match the existing test infrastructure.

- [ ] **Step 2: Run DST suite**

Run: `make dst`
Expected: All pass

- [ ] **Step 3: Commit**

```bash
git add dst/tablesync_test.go
git commit -m "test(dst): table sync scenarios — convergence, BUGGIFY, self-write enforcement"
```

---

### Task 9: Full Integration Test & Cleanup

**Files:**
- Run all test suites, fix any issues

- [ ] **Step 1: Run full test suite**

Run: `make test`
Expected: All pass

- [ ] **Step 2: Run DST full**

Run: `make dst-full`
Expected: All pass

- [ ] **Step 3: Verify existing Fossil interop tests still pass**

Run: `go test ./sim/ -run 'TestServeHTTP|TestLeafToLeaf' -count=1 -timeout=120s -v`
Expected: All pass (new card types should be ignored by Fossil since they're `UnknownCard`)

- [ ] **Step 4: Final commit if any fixes needed**

```bash
git commit -m "fix: integration test fixes for table sync"
```

---

### Task 10: CLI Commands

**Files:**
- Create: `cmd/edgesync/schema.go`
- Modify: `cmd/edgesync/cli.go` (add `Schema` to `RepoCmd`)

- [ ] **Step 1: Create schema.go with kong command structs**

Create `cmd/edgesync/schema.go`:

```go
package main

import (
	"encoding/json"
	"fmt"
	"strings"

	"github.com/dmestas/edgesync/go-libfossil/repo"
)

type SchemaCmd struct {
	Add    SchemaAddCmd    `cmd:"" help:"Register a new synced table"`
	List   SchemaListCmd   `cmd:"" help:"List registered synced tables"`
	Show   SchemaShowCmd   `cmd:"" help:"Show table schema details"`
	Remove SchemaRemoveCmd `cmd:"" help:"Remove a synced table registration"`
}

type SchemaAddCmd struct {
	Name     string `arg:"" help:"Table name (without x_ prefix)"`
	Columns  string `required:"" help:"Column definitions: name:type[:pk],... (e.g. peer_id:text:pk,addr:text)"`
	Conflict string `default:"mtime-wins" enum:"self-write,mtime-wins,owner-write" help:"Conflict resolution strategy"`
}

func (cmd *SchemaAddCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	def, err := parseColumnDefs(cmd.Columns)
	if err != nil {
		return err
	}
	def.Conflict = cmd.Conflict

	if err := repo.EnsureSyncSchema(r.DB()); err != nil {
		return err
	}
	if err := repo.RegisterSyncedTable(r.DB(), cmd.Name, def, now()); err != nil {
		return err
	}
	fmt.Printf("Registered synced table x_%s (%d columns, conflict=%s)\n", cmd.Name, len(def.Columns), cmd.Conflict)
	return nil
}

type SchemaListCmd struct{}

func (cmd *SchemaListCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	if err := repo.EnsureSyncSchema(r.DB()); err != nil {
		return err
	}
	tables, err := repo.ListSyncedTables(r.DB())
	if err != nil {
		return err
	}
	if len(tables) == 0 {
		fmt.Println("No synced tables registered.")
		return nil
	}
	for _, t := range tables {
		fmt.Printf("x_%s  v%d  conflict=%s  columns=%d\n", t.Name, t.Version, t.Def.Conflict, len(t.Def.Columns))
	}
	return nil
}

type SchemaShowCmd struct {
	Name string `arg:"" help:"Table name (without x_ prefix)"`
}

func (cmd *SchemaShowCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	if err := repo.EnsureSyncSchema(r.DB()); err != nil {
		return err
	}
	tables, err := repo.ListSyncedTables(r.DB())
	if err != nil {
		return err
	}
	for _, t := range tables {
		if t.Name == cmd.Name {
			out, _ := json.MarshalIndent(t.Def, "", "  ")
			fmt.Printf("Table: x_%s  Version: %d  Hash: %s\n%s\n", t.Name, t.Version, t.Hash, out)
			return nil
		}
	}
	return fmt.Errorf("table %q not found", cmd.Name)
}

type SchemaRemoveCmd struct {
	Name string `arg:"" help:"Table name (without x_ prefix)"`
}

func (cmd *SchemaRemoveCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	if err := repo.EnsureSyncSchema(r.DB()); err != nil {
		return err
	}
	_, err = r.DB().Exec("DELETE FROM _sync_schema WHERE table_name = ?", cmd.Name)
	if err != nil {
		return err
	}
	// Drop extension table
	_, err = r.DB().Exec(fmt.Sprintf("DROP TABLE IF EXISTS x_%s", cmd.Name))
	if err != nil {
		return err
	}
	fmt.Printf("Removed synced table x_%s\n", cmd.Name)
	return nil
}

func parseColumnDefs(s string) (repo.TableDef, error) {
	var def repo.TableDef
	for _, col := range strings.Split(s, ",") {
		parts := strings.Split(col, ":")
		if len(parts) < 2 {
			return def, fmt.Errorf("invalid column %q: need name:type[:pk]", col)
		}
		cd := repo.ColumnDef{Name: parts[0], Type: parts[1]}
		if len(parts) >= 3 && parts[2] == "pk" {
			cd.PK = true
		}
		def.Columns = append(def.Columns, cd)
	}
	return def, nil
}

func now() int64 {
	return time.Now().Unix()
}
```

- [ ] **Step 2: Add Schema to RepoCmd in cli.go**

In `cmd/edgesync/cli.go`, add to `RepoCmd` struct:

```go
Schema SchemaCmd `cmd:"" help:"Synced table schema operations"`
```

- [ ] **Step 3: Verify build**

Run: `go build -buildvcs=false ./cmd/edgesync/`
Expected: Clean build

- [ ] **Step 4: Manual smoke test**

```bash
bin/edgesync repo schema list -R /tmp/test.fossil
bin/edgesync repo schema add test_table --columns "id:text:pk,val:text" --conflict mtime-wins -R /tmp/test.fossil
bin/edgesync repo schema show test_table -R /tmp/test.fossil
bin/edgesync repo schema remove test_table -R /tmp/test.fossil
```

- [ ] **Step 5: Commit**

```bash
git add cmd/edgesync/schema.go cmd/edgesync/cli.go
git commit -m "feat(cli): edgesync schema add/list/show/remove commands"
```
