# Table Sync Deletion & PK Hash Fix — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add row deletion support (xdelete card) and fix PK hash type coercion in the remote schema sync feature.

**Architecture:** Two changes to go-libfossil's table sync: (1) type-aware PK hash normalization replacing `json.Marshal`-based hashing, and (2) a new `xdelete` card type with UV-style in-place tombstones (all non-PK value columns set to NULL). Both changes are in `go-libfossil/` (repo, xfer, sync packages) with DST coverage in `dst/`.

**Tech Stack:** Go, SQLite, SHA1 hashing, xfer card protocol

**Spec:** `docs/superpowers/specs/2026-04-02-table-sync-deletion-and-pk-hash-design.md`

---

## File Structure

| Action | File | Responsibility |
|--------|------|----------------|
| Modify | `go-libfossil/repo/tablesync.go` | `PKHash` signature change, `DeleteXRow`, tombstone detection, nullable columns in DDL, catalog hash exclusion |
| Modify | `go-libfossil/repo/tablesync_test.go` | PK hash tests, DeleteXRow tests, catalog hash tombstone exclusion |
| Modify | `go-libfossil/xfer/card.go:33` | Add `CardXDelete = 24` |
| Modify | `go-libfossil/xfer/card_tablesync.go` | Add `XDeleteCard` struct |
| Modify | `go-libfossil/xfer/card_tablesync_test.go` | XDeleteCard type + round-trip tests, fuzz test |
| Modify | `go-libfossil/xfer/encode.go:282` | Add `encodeXDelete` |
| Modify | `go-libfossil/xfer/decode.go:143` | Add `"xdelete"` case + `parseXDelete` |
| Modify | `go-libfossil/sync/handler_tablesync.go` | `handleXDelete`, tombstone-aware `handleXIGot`, `sendXDelete` |
| Modify | `go-libfossil/sync/handler.go:323` | Add `*xfer.XDeleteCard` case to `handleDataCard` |
| Modify | `go-libfossil/sync/client_tablesync.go` | `processXTableCard` dispatch for XDeleteCard, `handleXDeleteResponse`, tombstone-aware `buildTableSendCards` |
| Modify | `go-libfossil/sync/client.go:551` | Add `*xfer.XDeleteCard` to the table sync dispatch case |
| Modify | `go-libfossil/sync/handler_tablesync_test.go` | Handler-level xdelete tests |
| Modify | `go-libfossil/sync/client_tablesync_test.go` | End-to-end xdelete convergence |
| Modify | `dst/tablesync_test.go` | DST: multi-peer deletion, resurrection, integer PK convergence |

---

### Task 1: Fix PK hash — type-aware normalization

**Files:**
- Modify: `go-libfossil/repo/tablesync.go:228-240`
- Modify: `go-libfossil/repo/tablesync_test.go:36-49`

- [ ] **Step 1: Write failing tests for type-aware PKHash**

Add to `go-libfossil/repo/tablesync_test.go`:

```go
func TestPKHashTypeAware(t *testing.T) {
	pkCols := []ColumnDef{{Name: "id", Type: "integer", PK: true}}

	// int64 and float64 of same value must produce identical hashes.
	h1 := PKHash(pkCols, map[string]any{"id": int64(42)})
	h2 := PKHash(pkCols, map[string]any{"id": float64(42)})
	if h1 != h2 {
		t.Fatalf("int64 vs float64: %q != %q", h1, h2)
	}

	// Large integer (>2^53) must not lose precision.
	big := int64(1<<53 + 1) // 9007199254740993
	h3 := PKHash(pkCols, map[string]any{"id": big})
	h4 := PKHash(pkCols, map[string]any{"id": float64(big)})
	// float64 loses precision at 2^53+1, so these should still match
	// because we normalize float64 → int64 before formatting.
	if h3 != h4 {
		t.Fatalf("large int: %q != %q", h3, h4)
	}

	// Composite PK with mixed types.
	mixedCols := []ColumnDef{
		{Name: "org", Type: "text", PK: true},
		{Name: "seq", Type: "integer", PK: true},
	}
	h5 := PKHash(mixedCols, map[string]any{"org": "acme", "seq": int64(7)})
	h6 := PKHash(mixedCols, map[string]any{"org": "acme", "seq": float64(7)})
	if h5 != h6 {
		t.Fatalf("composite mixed types: %q != %q", h5, h6)
	}

	// Text PK must still work.
	textCols := []ColumnDef{{Name: "name", Type: "text", PK: true}}
	h7 := PKHash(textCols, map[string]any{"name": "hello"})
	if h7 == "" {
		t.Fatal("text PK hash is empty")
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd /Users/dmestas/projects/EdgeSync-tablesync-deletion && go test ./go-libfossil/repo/ -run TestPKHashTypeAware -v`
Expected: FAIL — `PKHash` doesn't accept `[]ColumnDef` parameter.

- [ ] **Step 3: Implement type-aware PKHash**

Replace `PKHash` in `go-libfossil/repo/tablesync.go:228-240` with:

```go
// normalizeValue converts a value to its canonical string representation
// based on the declared column type. This ensures PK hashes are identical
// regardless of how the value arrived (JSON unmarshal, SQLite scan, etc.).
func normalizeValue(colType string, v any) string {
	switch colType {
	case "integer":
		switch n := v.(type) {
		case int64:
			return strconv.FormatInt(n, 10)
		case float64:
			return strconv.FormatInt(int64(n), 10)
		case int:
			return strconv.FormatInt(int64(n), 10)
		default:
			return fmt.Sprintf("%v", v)
		}
	case "real":
		switch n := v.(type) {
		case float64:
			return strconv.FormatFloat(n, 'f', -1, 64)
		case int64:
			return strconv.FormatFloat(float64(n), 'f', -1, 64)
		default:
			return fmt.Sprintf("%v", v)
		}
	case "text":
		s, _ := v.(string)
		return s
	case "blob":
		b, _ := v.([]byte)
		return hex.EncodeToString(b)
	default:
		return fmt.Sprintf("%v", v)
	}
}

// PKHash computes a deterministic SHA1 hash of the primary key values.
// Values are normalized to canonical strings based on declared column types
// to avoid JSON round-trip coercion issues (e.g., int64 → float64).
func PKHash(pkCols []ColumnDef, pkValues map[string]any) string {
	if pkCols == nil {
		panic("repo.PKHash: pkCols must not be nil")
	}
	if pkValues == nil {
		panic("repo.PKHash: pkValues must not be nil")
	}

	// Sort PK columns lexicographically for determinism.
	sorted := make([]ColumnDef, len(pkCols))
	copy(sorted, pkCols)
	sort.Slice(sorted, func(i, j int) bool {
		return sorted[i].Name < sorted[j].Name
	})

	// Build canonical representation: name=normalizedValue joined by \x00.
	var parts []string
	for _, col := range sorted {
		v := pkValues[col.Name]
		parts = append(parts, col.Name+"="+normalizeValue(col.Type, v))
	}
	canonical := strings.Join(parts, "\x00")
	return hash.SHA1([]byte(canonical))
}
```

Add `"encoding/hex"` and `"strconv"` to the imports in `tablesync.go`.

- [ ] **Step 4: Update existing TestPKHash to use new signature**

Update `go-libfossil/repo/tablesync_test.go:36-49`:

```go
func TestPKHash(t *testing.T) {
	pkCols := []ColumnDef{{Name: "peer_id", Type: "text", PK: true}}
	h := PKHash(pkCols, map[string]any{"peer_id": "leaf-01"})
	if h == "" {
		t.Fatal("PKHash returned empty")
	}
	h2 := PKHash(pkCols, map[string]any{"peer_id": "leaf-01"})
	if h != h2 {
		t.Fatalf("PKHash not deterministic: %q vs %q", h, h2)
	}
	h3 := PKHash(pkCols, map[string]any{"peer_id": "leaf-02"})
	if h == h3 {
		t.Fatal("different inputs produced same hash")
	}
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd /Users/dmestas/projects/EdgeSync-tablesync-deletion && go test ./go-libfossil/repo/ -run TestPKHash -v`
Expected: PASS for both `TestPKHash` and `TestPKHashTypeAware`.

- [ ] **Step 6: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync-tablesync-deletion
git add go-libfossil/repo/tablesync.go go-libfossil/repo/tablesync_test.go
git commit -m "fix(repo): type-aware PK hash normalization (CDG-171)

Replace json.Marshal-based PKHash with canonical string normalization
per declared column type. Prevents hash divergence on JSON round-trip
(int64 → float64 coercion) for integer PKs."
```

---

### Task 2: Update all PKHash callers

**Files:**
- Modify: `go-libfossil/repo/tablesync.go:337-440` (LookupXRow, CatalogHash)
- Modify: `go-libfossil/sync/handler_tablesync.go:246-260,375-391` (verifyXRowPKHash, emitXIGotsForTable)
- Modify: `go-libfossil/sync/client_tablesync.go:83-104` (buildTableXIGotCards)
- Modify: `dst/tablesync_test.go:101-115` (assertRowValue)

- [ ] **Step 1: Add helper to extract PK ColumnDefs (not just names)**

Add to `go-libfossil/sync/client_tablesync.go`, replacing the existing `extractPKColumns` at line 276:

```go
// extractPKColumns returns the names of all primary key columns from a TableDef.
// This is a package-level function shared by both client and handler.
func extractPKColumns(def repo.TableDef) []string {
	var pkCols []string
	for _, col := range def.Columns {
		if col.PK {
			pkCols = append(pkCols, col.Name)
		}
	}
	return pkCols
}

// extractPKColumnDefs returns the ColumnDef of all PK columns from a TableDef.
func extractPKColumnDefs(def repo.TableDef) []repo.ColumnDef {
	var pkCols []repo.ColumnDef
	for _, col := range def.Columns {
		if col.PK {
			pkCols = append(pkCols, col)
		}
	}
	return pkCols
}
```

- [ ] **Step 2: Update LookupXRow to pass pkCols**

In `go-libfossil/repo/tablesync.go`, update `LookupXRow` (lines 352-382). Replace the two `PKHash(pkValues)` calls at lines 367 and 376 with:

```go
		computed := PKHash(pkColDefs, pkValues)
```

And change the PK extraction at lines 354-358 to:

```go
	// Extract PK column definitions (not just names — needed for type-aware hash).
	var pkColDefs []ColumnDef
	var pkNames []string
	for _, col := range def.Columns {
		if col.PK {
			pkColDefs = append(pkColDefs, col)
			pkNames = append(pkNames, col.Name)
		}
	}
```

And update the `pkValues` loop to use `pkNames`:

```go
		pkValues := make(map[string]any)
		for _, pk := range pkNames {
			pkValues[pk] = row[pk]
		}
		computed := PKHash(pkColDefs, pkValues)
```

Apply the same for the verify block at lines 370-377:

```go
			verifyPK := make(map[string]any)
			for _, pk := range pkNames {
				verifyPK[pk] = row[pk]
			}
			if PKHash(pkColDefs, verifyPK) != pkHash {
```

- [ ] **Step 3: Update CatalogHash to pass pkCols**

In `go-libfossil/repo/tablesync.go`, update `CatalogHash` (lines 400-406 and 414-422):

```go
	// Extract PK column definitions.
	var pkColDefs []ColumnDef
	var pkNames []string
	for _, col := range def.Columns {
		if col.PK {
			pkColDefs = append(pkColDefs, col)
			pkNames = append(pkNames, col.Name)
		}
	}
```

And update the hash computation:

```go
		pkValues := make(map[string]any)
		for _, pk := range pkNames {
			pkValues[pk] = row[pk]
		}
		entries = append(entries, entry{
			pkHash: PKHash(pkColDefs, pkValues),
			mtime:  mtimes[i],
		})
```

- [ ] **Step 4: Update handler_tablesync.go callers**

In `go-libfossil/sync/handler_tablesync.go`:

Update `verifyXRowPKHash` (line 251):
```go
	pkColDefs := extractPKColumnDefs(st.Def)
	pkCols := extractPKColumns(st.Def)
	pkValues := make(map[string]any)
	for _, col := range pkCols {
		pkValues[col] = row[col]
	}
	computedPK := repo.PKHash(pkColDefs, pkValues)
```

Update `emitXIGotsForTable` (lines 375-382):
```go
	pkCols := extractPKColumns(st.Def)
	pkColDefs := extractPKColumnDefs(st.Def)

	for i, row := range rows {
		pkValues := make(map[string]any)
		for _, col := range pkCols {
			pkValues[col] = row[col]
		}
		pkHash := repo.PKHash(pkColDefs, pkValues)
```

- [ ] **Step 5: Update client_tablesync.go callers**

In `go-libfossil/sync/client_tablesync.go`, update `buildTableXIGotCards` (lines 83-90):
```go
	pkCols := extractPKColumns(info.Def)
	pkColDefs := extractPKColumnDefs(info.Def)
	var cards []xfer.Card
	for i, row := range rows {
		pkValues := make(map[string]any)
		for _, col := range pkCols {
			pkValues[col] = row[col]
		}
		pkHash := repo.PKHash(pkColDefs, pkValues)
```

- [ ] **Step 6: Update DST test helpers**

In `dst/tablesync_test.go`, update `assertRowValue` (line 103):
```go
	var pkColDefs []repo.ColumnDef
	for _, col := range def.Columns {
		if col.PK {
			pkColDefs = append(pkColDefs, col)
		}
	}
	pkHash := repo.PKHash(pkColDefs, pk)
```

- [ ] **Step 7: Run full test suite**

Run: `cd /Users/dmestas/projects/EdgeSync-tablesync-deletion && go test ./go-libfossil/... ./dst/ -v -count=1 2>&1 | tail -30`
Expected: All existing tests pass with the new PKHash signature.

- [ ] **Step 8: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync-tablesync-deletion
git add go-libfossil/repo/tablesync.go go-libfossil/sync/handler_tablesync.go go-libfossil/sync/client_tablesync.go dst/tablesync_test.go
git commit -m "refactor(sync): update all PKHash callers to pass ColumnDefs

Thread PK ColumnDefs through LookupXRow, CatalogHash, handler, client,
and DST helpers. Adds extractPKColumnDefs() shared helper."
```

---

### Task 3: Make non-PK columns nullable in extension tables

**Files:**
- Modify: `go-libfossil/repo/tablesync.go:152-192`
- Modify: `go-libfossil/repo/tablesync_test.go`

- [ ] **Step 1: Write failing test for nullable columns**

Add to `go-libfossil/repo/tablesync_test.go`:

```go
func TestExtensionTableNullableValueColumns(t *testing.T) {
	r := setupTSRepo(t)
	EnsureSyncSchema(r.DB())
	def := TableDef{
		Columns:  []ColumnDef{{Name: "id", Type: "text", PK: true}, {Name: "data", Type: "text"}},
		Conflict: "mtime-wins",
	}
	if err := RegisterSyncedTable(r.DB(), "nullable_test", def, 1000); err != nil {
		t.Fatal(err)
	}
	// Insert row with NULL value column — should succeed for tombstone support.
	_, err := r.DB().Exec("INSERT INTO x_nullable_test(id, data, mtime) VALUES('k1', NULL, 1000)")
	if err != nil {
		t.Fatalf("insert with NULL value column should succeed: %v", err)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /Users/dmestas/projects/EdgeSync-tablesync-deletion && go test ./go-libfossil/repo/ -run TestExtensionTableNullableValueColumns -v`
Expected: FAIL — `NOT NULL constraint failed: x_nullable_test.data`

- [ ] **Step 3: Change createExtensionTable to only enforce NOT NULL on PK columns**

In `go-libfossil/repo/tablesync.go`, update `createExtensionTable` (line 165):

```go
		// PK columns are NOT NULL; value columns are nullable (for tombstone support).
		if col.PK {
			cols = append(cols, fmt.Sprintf("%s %s NOT NULL", col.Name, sqlType))
		} else {
			cols = append(cols, fmt.Sprintf("%s %s", col.Name, sqlType))
		}
```

- [ ] **Step 4: Run tests**

Run: `cd /Users/dmestas/projects/EdgeSync-tablesync-deletion && go test ./go-libfossil/repo/ -run TestExtensionTableNullableValueColumns -v`
Expected: PASS

Note: existing repos with `NOT NULL` columns created before this change will need `ALTER TABLE` or re-creation. Since table sync is still pre-production, this is acceptable — no migration needed.

- [ ] **Step 5: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync-tablesync-deletion
git add go-libfossil/repo/tablesync.go go-libfossil/repo/tablesync_test.go
git commit -m "fix(repo): make non-PK extension table columns nullable

Required for UV-style tombstones where deleted rows have all value
columns set to NULL. PK columns remain NOT NULL."
```

---

### Task 4: Add tombstone helpers — IsTombstone, DeleteXRow, catalog hash exclusion

**Files:**
- Modify: `go-libfossil/repo/tablesync.go`
- Modify: `go-libfossil/repo/tablesync_test.go`

- [ ] **Step 1: Write failing tests**

Add to `go-libfossil/repo/tablesync_test.go`:

```go
func TestIsTombstone(t *testing.T) {
	def := TableDef{
		Columns: []ColumnDef{
			{Name: "id", Type: "text", PK: true},
			{Name: "data", Type: "text"},
			{Name: "count", Type: "integer"},
		},
		Conflict: "mtime-wins",
	}
	// Live row — not a tombstone.
	if IsTombstone(def, map[string]any{"id": "k1", "data": "hello", "count": int64(5)}) {
		t.Error("live row should not be tombstone")
	}
	// Tombstone — all non-PK value columns are nil.
	if !IsTombstone(def, map[string]any{"id": "k1", "data": nil, "count": nil}) {
		t.Error("row with all nil values should be tombstone")
	}
	// Partial nil — not a tombstone.
	if IsTombstone(def, map[string]any{"id": "k1", "data": nil, "count": int64(5)}) {
		t.Error("partial nil should not be tombstone")
	}
}

func TestDeleteXRow(t *testing.T) {
	r := setupTSRepo(t)
	EnsureSyncSchema(r.DB())
	def := TableDef{
		Columns:  []ColumnDef{{Name: "id", Type: "text", PK: true}, {Name: "data", Type: "text"}},
		Conflict: "mtime-wins",
	}
	RegisterSyncedTable(r.DB(), "del_test", def, 1000)

	// Seed a live row.
	UpsertXRow(r.DB(), "del_test", map[string]any{"id": "k1", "data": "hello"}, 1000)

	// Delete with newer mtime.
	deleted, err := DeleteXRow(r.DB(), "del_test", def, "k1", 2000)
	if err != nil {
		t.Fatalf("DeleteXRow: %v", err)
	}
	if !deleted {
		t.Fatal("expected deletion to apply")
	}

	// Verify tombstone.
	pkColDefs := []ColumnDef{{Name: "id", Type: "text", PK: true}}
	pkHash := PKHash(pkColDefs, map[string]any{"id": "k1"})
	row, mtime, err := LookupXRow(r.DB(), "del_test", def, pkHash)
	if err != nil {
		t.Fatalf("LookupXRow: %v", err)
	}
	if row == nil {
		t.Fatal("tombstone row should still exist")
	}
	if mtime != 2000 {
		t.Errorf("mtime = %d, want 2000", mtime)
	}
	if !IsTombstone(def, row) {
		t.Error("row should be tombstone after deletion")
	}

	// Delete with older mtime — should be rejected.
	deleted, err = DeleteXRow(r.DB(), "del_test", def, "k1", 1500)
	if err != nil {
		t.Fatalf("DeleteXRow (older): %v", err)
	}
	if deleted {
		t.Error("older delete should be rejected")
	}

	// Resurrection — UpsertXRow with newer mtime.
	UpsertXRow(r.DB(), "del_test", map[string]any{"id": "k1", "data": "revived"}, 3000)
	row, _, _ = LookupXRow(r.DB(), "del_test", def, pkHash)
	if IsTombstone(def, row) {
		t.Error("row should be live after resurrection")
	}
}

func TestCatalogHashExcludesTombstones(t *testing.T) {
	r := setupTSRepo(t)
	EnsureSyncSchema(r.DB())
	def := TableDef{
		Columns:  []ColumnDef{{Name: "id", Type: "text", PK: true}, {Name: "data", Type: "text"}},
		Conflict: "mtime-wins",
	}
	RegisterSyncedTable(r.DB(), "cat_test", def, 1000)

	// Seed two rows.
	UpsertXRow(r.DB(), "cat_test", map[string]any{"id": "k1", "data": "a"}, 1000)
	UpsertXRow(r.DB(), "cat_test", map[string]any{"id": "k2", "data": "b"}, 1000)
	hashBefore, _ := CatalogHash(r.DB(), "cat_test", def)

	// Delete k1.
	DeleteXRow(r.DB(), "cat_test", def, "k1", 2000)
	hashAfter, _ := CatalogHash(r.DB(), "cat_test", def)

	if hashBefore == hashAfter {
		t.Error("catalog hash should change after deletion")
	}

	// Delete k2 — catalog should now match a fresh empty table.
	DeleteXRow(r.DB(), "cat_test", def, "k2", 2000)
	hashEmpty, _ := CatalogHash(r.DB(), "cat_test", def)

	// Register a fresh empty table for comparison.
	RegisterSyncedTable(r.DB(), "cat_empty", def, 1000)
	hashFresh, _ := CatalogHash(r.DB(), "cat_empty", def)
	if hashEmpty != hashFresh {
		t.Errorf("all-tombstone table hash %q != empty table hash %q", hashEmpty, hashFresh)
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd /Users/dmestas/projects/EdgeSync-tablesync-deletion && go test ./go-libfossil/repo/ -run "TestIsTombstone|TestDeleteXRow|TestCatalogHashExcludesTombstones" -v`
Expected: FAIL — `IsTombstone` and `DeleteXRow` not defined.

- [ ] **Step 3: Implement IsTombstone**

Add to `go-libfossil/repo/tablesync.go`:

```go
// IsTombstone returns true if all non-PK, non-mtime, non-_owner value columns
// in the row are nil. A tombstone represents a deleted row.
func IsTombstone(def TableDef, row map[string]any) bool {
	for _, col := range def.Columns {
		if col.PK {
			continue
		}
		if row[col.Name] != nil {
			return false
		}
	}
	return true
}
```

- [ ] **Step 4: Implement DeleteXRow**

Add to `go-libfossil/repo/tablesync.go`:

```go
// DeleteXRow tombstones a row by setting all non-PK value columns to NULL
// and updating mtime. Returns true if the deletion was applied, false if
// the local row has a newer mtime. Uses pkValue (a single PK column value
// for simple PKs) to look up the row by primary key name directly.
func DeleteXRow(d *db.DB, tableName string, def TableDef, pkValue string, mtime int64) (bool, error) {
	if d == nil {
		panic("repo.DeleteXRow: d must not be nil")
	}

	// Find PK column names.
	var pkCols []string
	for _, col := range def.Columns {
		if col.PK {
			pkCols = append(pkCols, col.Name)
		}
	}
	if len(pkCols) != 1 {
		return false, fmt.Errorf("repo.DeleteXRow: simple API only supports single-PK tables, got %d PK cols", len(pkCols))
	}

	// Check current mtime.
	var currentMtime int64
	err := d.QueryRow(
		fmt.Sprintf("SELECT mtime FROM x_%s WHERE %s = ?", tableName, pkCols[0]),
		pkValue,
	).Scan(&currentMtime)
	if err == sql.ErrNoRows {
		return false, nil // row doesn't exist locally — caller handles via PKData
	}
	if err != nil {
		return false, fmt.Errorf("repo.DeleteXRow: lookup mtime: %w", err)
	}

	// mtime-wins: reject if local is newer.
	if currentMtime > mtime {
		return false, nil
	}

	// Build SET clause: NULL all non-PK, non-mtime value columns.
	var setClauses []string
	for _, col := range def.Columns {
		if col.PK {
			continue
		}
		setClauses = append(setClauses, fmt.Sprintf("%s = NULL", col.Name))
	}
	setClauses = append(setClauses, "mtime = ?")

	sql := fmt.Sprintf(
		"UPDATE x_%s SET %s WHERE %s = ?",
		tableName,
		strings.Join(setClauses, ", "),
		pkCols[0],
	)
	_, err = d.Exec(sql, mtime, pkValue)
	if err != nil {
		return false, fmt.Errorf("repo.DeleteXRow: update: %w", err)
	}
	return true, nil
}

// DeleteXRowByPKHash tombstones a row identified by PK hash. Falls back to
// scanning all rows to find the match (same as LookupXRow). Returns true
// if the deletion was applied.
func DeleteXRowByPKHash(d *db.DB, tableName string, def TableDef, pkHash string, mtime int64) (bool, error) {
	if d == nil {
		panic("repo.DeleteXRowByPKHash: d must not be nil")
	}

	// Find the row's PK values by scanning (same strategy as LookupXRow).
	row, currentMtime, err := LookupXRow(d, tableName, def, pkHash)
	if err != nil {
		return false, err
	}
	if row == nil {
		return false, nil // row doesn't exist locally
	}

	// mtime-wins: reject if local is newer.
	if currentMtime > mtime {
		return false, nil
	}

	// Extract PK column names for WHERE clause.
	var pkCols []string
	var pkValues []any
	for _, col := range def.Columns {
		if col.PK {
			pkCols = append(pkCols, col.Name)
			pkValues = append(pkValues, row[col.Name])
		}
	}

	// Build SET clause: NULL all non-PK value columns, update mtime.
	var setClauses []string
	for _, col := range def.Columns {
		if col.PK {
			continue
		}
		setClauses = append(setClauses, fmt.Sprintf("%s = NULL", col.Name))
	}
	setClauses = append(setClauses, "mtime = ?")

	// Build WHERE clause.
	var whereClauses []string
	for _, pk := range pkCols {
		whereClauses = append(whereClauses, fmt.Sprintf("%s = ?", pk))
	}

	args := []any{mtime}
	args = append(args, pkValues...)

	sqlStr := fmt.Sprintf(
		"UPDATE x_%s SET %s WHERE %s",
		tableName,
		strings.Join(setClauses, ", "),
		strings.Join(whereClauses, " AND "),
	)
	_, err = d.Exec(sqlStr, args...)
	if err != nil {
		return false, fmt.Errorf("repo.DeleteXRowByPKHash: update: %w", err)
	}
	return true, nil
}
```

- [ ] **Step 5: Update CatalogHash to exclude tombstones**

In `go-libfossil/repo/tablesync.go`, update `CatalogHash` (around line 414) to skip tombstone rows:

```go
	for i, row := range rows {
		// Exclude tombstones from catalog hash (UV convention).
		if IsTombstone(def, row) {
			continue
		}
		pkValues := make(map[string]any)
		for _, pk := range pkNames {
			pkValues[pk] = row[pk]
		}
```

- [ ] **Step 6: Run tests**

Run: `cd /Users/dmestas/projects/EdgeSync-tablesync-deletion && go test ./go-libfossil/repo/ -run "TestIsTombstone|TestDeleteXRow|TestCatalogHashExcludesTombstones" -v`
Expected: PASS

- [ ] **Step 7: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync-tablesync-deletion
git add go-libfossil/repo/tablesync.go go-libfossil/repo/tablesync_test.go
git commit -m "feat(repo): add IsTombstone, DeleteXRow, tombstone-aware CatalogHash

UV-style tombstones: deleted rows keep PK columns, NULL all value
columns, update mtime. CatalogHash excludes tombstone rows."
```

---

### Task 5: Add XDeleteCard to xfer protocol

**Files:**
- Modify: `go-libfossil/xfer/card.go:33`
- Modify: `go-libfossil/xfer/card_tablesync.go`
- Modify: `go-libfossil/xfer/card_tablesync_test.go`
- Modify: `go-libfossil/xfer/encode.go`
- Modify: `go-libfossil/xfer/decode.go`

- [ ] **Step 1: Write failing round-trip test**

Add to `go-libfossil/xfer/card_tablesync_test.go`:

```go
func TestXDeleteCardType(t *testing.T) {
	c := &XDeleteCard{Table: "devices", PKHash: "abc123", MTime: 2000, PKData: []byte(`{"device_id":"d1"}`)}
	if c.Type() != CardXDelete {
		t.Fatalf("got %v, want CardXDelete", c.Type())
	}
}

func TestXDeleteCard_RoundTrip(t *testing.T) {
	original := &XDeleteCard{
		Table:  "devices",
		PKHash: "abc123def456",
		MTime:  1711300000,
		PKData: []byte(`{"device_id":"d1"}`),
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
	xd, ok := got.(*XDeleteCard)
	if !ok {
		t.Fatalf("got %T, want *XDeleteCard", got)
	}
	if xd.Table != original.Table || xd.PKHash != original.PKHash || xd.MTime != original.MTime {
		t.Errorf("fields mismatch: got %+v", xd)
	}
	if string(xd.PKData) != string(original.PKData) {
		t.Errorf("PKData = %q, want %q", xd.PKData, original.PKData)
	}
}

func FuzzParseXDelete(f *testing.F) {
	f.Add("devices abc123 1711300000 18\n{\"device_id\":\"d1\"}\n")
	f.Add("t a 0 0\n\n")
	f.Add("")
	f.Fuzz(func(t *testing.T, input string) {
		r := bufio.NewReader(bytes.NewReader([]byte("xdelete " + input)))
		_, _ = DecodeCard(r) // must not panic
	})
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /Users/dmestas/projects/EdgeSync-tablesync-deletion && go test ./go-libfossil/xfer/ -run "TestXDeleteCard" -v`
Expected: FAIL — `XDeleteCard` not defined.

- [ ] **Step 3: Add CardXDelete constant**

In `go-libfossil/xfer/card.go`, after line 33 (`CardXRow`), add:

```go
	CardXDelete                    // 24 — xdelete (table sync row deletion)
```

- [ ] **Step 4: Add XDeleteCard struct**

In `go-libfossil/xfer/card_tablesync.go`, add after the XRowCard definition:

```go
// XDeleteCard marks a table sync row as deleted (tombstone).
// Wire: xdelete TABLE PK_HASH MTIME SIZE\nJSON_PK_DATA
type XDeleteCard struct {
	Table  string
	PKHash string
	MTime  int64
	PKData []byte // JSON-encoded PK column values
}

func (c *XDeleteCard) Type() CardType { return CardXDelete }
```

- [ ] **Step 5: Add encoder**

In `go-libfossil/xfer/encode.go`, add after `encodeXRow` (after line 282), and add the case to `EncodeCard`:

```go
func encodeXDelete(w *bytes.Buffer, c *XDeleteCard) error {
	fmt.Fprintf(w, "xdelete %s %s %d %d\n", c.Table, c.PKHash, c.MTime, len(c.PKData))
	w.Write(c.PKData)
	w.WriteByte('\n')
	return nil
}
```

Add the case to the `EncodeCard` switch (find the existing `*XRowCard` case and add after it):

```go
	case *XDeleteCard:
		return encodeXDelete(w, c)
```

- [ ] **Step 6: Add decoder**

In `go-libfossil/xfer/decode.go`, add `parseXDelete` after `parseXRow`:

```go
func parseXDelete(r *bufio.Reader, args []string) (Card, error) {
	if len(args) != 4 {
		return nil, fmt.Errorf("xfer: xdelete requires 4 args, got %d", len(args))
	}
	mtime, err := strconv.ParseInt(args[2], 10, 64)
	if err != nil {
		return nil, fmt.Errorf("xfer: xdelete mtime: %w", err)
	}
	size, err := strconv.Atoi(args[3])
	if err != nil {
		return nil, fmt.Errorf("xfer: xdelete size: %w", err)
	}
	pkData, err := readPayloadWithTrailingNewline(r, size)
	if err != nil {
		return nil, fmt.Errorf("xfer: xdelete payload: %w", err)
	}
	return &XDeleteCard{Table: args[0], PKHash: args[1], MTime: mtime, PKData: pkData}, nil
}
```

Add the case to the decode switch (after the `"xrow"` case, around line 143):

```go
	case "xdelete":
		return parseXDelete(r, args)
```

- [ ] **Step 7: Run tests**

Run: `cd /Users/dmestas/projects/EdgeSync-tablesync-deletion && go test ./go-libfossil/xfer/ -run "TestXDeleteCard" -v`
Expected: PASS

Run fuzz briefly: `cd /Users/dmestas/projects/EdgeSync-tablesync-deletion && go test ./go-libfossil/xfer/ -fuzz FuzzParseXDelete -fuzztime 10s`
Expected: No panics found.

- [ ] **Step 8: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync-tablesync-deletion
git add go-libfossil/xfer/card.go go-libfossil/xfer/card_tablesync.go go-libfossil/xfer/card_tablesync_test.go go-libfossil/xfer/encode.go go-libfossil/xfer/decode.go
git commit -m "feat(xfer): add XDeleteCard for table sync row deletion (CDG-168)

Wire format: xdelete TABLE PK_HASH MTIME SIZE followed by JSON PK data.
Includes round-trip test and fuzz test for the parser."
```

---

### Task 6: Server-side xdelete handling

**Files:**
- Modify: `go-libfossil/sync/handler.go:290-326`
- Modify: `go-libfossil/sync/handler_tablesync.go`
- Modify: `go-libfossil/sync/handler_tablesync_test.go`

- [ ] **Step 1: Write failing handler test**

Add to `go-libfossil/sync/handler_tablesync_test.go`:

```go
func TestHandleXDelete(t *testing.T) {
	// Setup: two repos, register table, seed a row on server.
	serverRepo := createTestRepo(t)
	clientRepo := createTestRepo(t)
	def := repo.TableDef{
		Columns:  []repo.ColumnDef{{Name: "id", Type: "text", PK: true}, {Name: "data", Type: "text"}},
		Conflict: "mtime-wins",
	}
	repo.EnsureSyncSchema(serverRepo.DB())
	repo.RegisterSyncedTable(serverRepo.DB(), "del_test", def, 1000)
	repo.UpsertXRow(serverRepo.DB(), "del_test", map[string]any{"id": "k1", "data": "hello"}, 1000)

	// Client sends xdelete with newer mtime.
	pkColDefs := []repo.ColumnDef{{Name: "id", Type: "text", PK: true}}
	pkHash := repo.PKHash(pkColDefs, map[string]any{"id": "k1"})
	msg := &xfer.Message{
		Cards: []xfer.Card{
			&xfer.PushCard{ServerCode: "sc", ProjectCode: "pc"},
			&xfer.PullCard{ServerCode: "sc", ProjectCode: "pc"},
			&xfer.XDeleteCard{
				Table:  "del_test",
				PKHash: pkHash,
				MTime:  2000,
				PKData: []byte(`{"id":"k1"}`),
			},
		},
	}

	resp, err := HandleSync(serverRepo, msg)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}

	// Verify server tombstoned the row.
	row, mtime, _ := repo.LookupXRow(serverRepo.DB(), "del_test", def, pkHash)
	if row == nil {
		t.Fatal("row should still exist as tombstone")
	}
	if !repo.IsTombstone(def, row) {
		t.Error("row should be tombstone")
	}
	if mtime != 2000 {
		t.Errorf("mtime = %d, want 2000", mtime)
	}

	// Verify no error cards in response.
	for _, card := range resp.Cards {
		if ec, ok := card.(*xfer.ErrorCard); ok {
			t.Errorf("unexpected error card: %s", ec.Message)
		}
	}
}

func TestHandleXIGotEmitsXDeleteForTombstone(t *testing.T) {
	serverRepo := createTestRepo(t)
	def := repo.TableDef{
		Columns:  []repo.ColumnDef{{Name: "id", Type: "text", PK: true}, {Name: "data", Type: "text"}},
		Conflict: "mtime-wins",
	}
	repo.EnsureSyncSchema(serverRepo.DB())
	repo.RegisterSyncedTable(serverRepo.DB(), "del_test", def, 1000)

	// Seed a row then delete it (tombstone).
	repo.UpsertXRow(serverRepo.DB(), "del_test", map[string]any{"id": "k1", "data": "hello"}, 1000)
	pkColDefs := []repo.ColumnDef{{Name: "id", Type: "text", PK: true}}
	pkHash := repo.PKHash(pkColDefs, map[string]any{"id": "k1"})
	repo.DeleteXRowByPKHash(serverRepo.DB(), "del_test", def, pkHash, 2000)

	// Client sends xigot for same row with older mtime.
	msg := &xfer.Message{
		Cards: []xfer.Card{
			&xfer.PushCard{ServerCode: "sc", ProjectCode: "pc"},
			&xfer.PullCard{ServerCode: "sc", ProjectCode: "pc"},
			&xfer.XIGotCard{Table: "del_test", PKHash: pkHash, MTime: 1000},
		},
	}

	resp, err := HandleSync(serverRepo, msg)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}

	// Server should emit xdelete (not xrow) since its row is a tombstone.
	var foundXDelete bool
	for _, card := range resp.Cards {
		if xd, ok := card.(*xfer.XDeleteCard); ok && xd.Table == "del_test" && xd.PKHash == pkHash {
			foundXDelete = true
			if xd.MTime != 2000 {
				t.Errorf("xdelete mtime = %d, want 2000", xd.MTime)
			}
		}
		if xr, ok := card.(*xfer.XRowCard); ok && xr.Table == "del_test" {
			t.Error("server should emit xdelete, not xrow, for tombstone")
		}
	}
	if !foundXDelete {
		t.Error("server should emit xdelete for tombstone row")
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd /Users/dmestas/projects/EdgeSync-tablesync-deletion && go test ./go-libfossil/sync/ -run "TestHandleXDelete" -v`
Expected: FAIL — no `handleXDelete` method, no `XDeleteCard` dispatch.

- [ ] **Step 3: Add handleXDelete to handler_tablesync.go**

Add to `go-libfossil/sync/handler_tablesync.go`:

```go
// handleXDelete processes a table sync deletion card.
func (h *handler) handleXDelete(c *xfer.XDeleteCard) error {
	if c == nil {
		panic("handler.handleXDelete: c must not be nil")
	}
	if !h.pushOK {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("xdelete %s/%s rejected: no push card", c.Table, c.PKHash),
		})
		return nil
	}

	st, ok := h.syncedTables[c.Table]
	if !ok {
		return nil
	}

	deleted, err := repo.DeleteXRowByPKHash(h.repo.DB(), c.Table, st.Def, c.PKHash, c.MTime)
	if err != nil {
		return fmt.Errorf("handler.handleXDelete: %w", err)
	}

	// If row didn't exist locally, insert tombstone using PKData.
	if !deleted {
		// Check if row exists at all.
		existingRow, _, lookupErr := repo.LookupXRow(h.repo.DB(), c.Table, st.Def, c.PKHash)
		if lookupErr != nil {
			return fmt.Errorf("handler.handleXDelete: lookup: %w", lookupErr)
		}
		if existingRow == nil && len(c.PKData) > 0 {
			// Insert tombstone from PKData.
			var pkValues map[string]any
			if err := json.Unmarshal(c.PKData, &pkValues); err != nil {
				h.resp = append(h.resp, &xfer.ErrorCard{
					Message: fmt.Sprintf("xdelete %s/%s: bad PKData: %v", c.Table, c.PKHash, err),
				})
				return nil
			}
			if err := repo.UpsertXRow(h.repo.DB(), c.Table, pkValues, c.MTime); err != nil {
				return fmt.Errorf("handler.handleXDelete: insert tombstone: %w", err)
			}
		}
		// If row exists with newer mtime, deleted==false is correct — no-op.
	}

	return nil
}

// sendXDelete sends a tombstone deletion card to the client.
func (h *handler) sendXDelete(table string, st *SyncedTable, pkHash string, mtime int64, row map[string]any) {
	// Build PKData from the row's PK values.
	pkCols := extractPKColumns(st.Def)
	pkValues := make(map[string]any)
	for _, col := range pkCols {
		pkValues[col] = row[col]
	}
	pkData, err := json.Marshal(pkValues)
	if err != nil {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("xdelete %s/%s: marshal PKData: %v", table, pkHash, err),
		})
		return
	}

	h.resp = append(h.resp, &xfer.XDeleteCard{
		Table:  table,
		PKHash: pkHash,
		MTime:  mtime,
		PKData: pkData,
	})
}
```

- [ ] **Step 4: Update handleXIGot to emit xdelete for tombstones**

In `go-libfossil/sync/handler_tablesync.go`, update `handleXIGot` (around line 149-152). Replace the existing `localMtime > c.MTime` block:

```go
	// Compare mtimes: if local is newer, push it (or send xdelete if tombstone).
	if localMtime > c.MTime {
		if repo.IsTombstone(st.Def, localRow) {
			h.sendXDelete(c.Table, st, c.PKHash, localMtime, localRow)
			return nil
		}
		return h.sendXRow(c.Table, st, c.PKHash)
	}
```

- [ ] **Step 5: Add XDeleteCard dispatch to handleDataCard**

In `go-libfossil/sync/handler.go`, add to the `handleDataCard` switch (after the `*xfer.XRowCard` case at line 323):

```go
	case *xfer.XDeleteCard:
		return h.handleXDelete(c)
```

- [ ] **Step 6: Run tests**

Run: `cd /Users/dmestas/projects/EdgeSync-tablesync-deletion && go test ./go-libfossil/sync/ -run "TestHandleXDelete" -v`
Expected: PASS

- [ ] **Step 7: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync-tablesync-deletion
git add go-libfossil/sync/handler.go go-libfossil/sync/handler_tablesync.go go-libfossil/sync/handler_tablesync_test.go
git commit -m "feat(sync): server-side xdelete handling and tombstone-aware xigot

handleXDelete applies mtime-wins deletion or inserts tombstone via PKData.
handleXIGot emits xdelete (not xrow) when server has a tombstone."
```

---

### Task 7: Client-side xdelete handling

**Files:**
- Modify: `go-libfossil/sync/client_tablesync.go`
- Modify: `go-libfossil/sync/client.go:551`
- Modify: `go-libfossil/sync/client_tablesync_test.go`

- [ ] **Step 1: Write failing end-to-end test**

Add to `go-libfossil/sync/client_tablesync_test.go`:

```go
func TestTableSyncDeletion(t *testing.T) {
	// Setup: server has a tombstoned row, client has the live row.
	serverRepo := createTestRepo(t)
	clientRepo := createTestRepo(t)
	def := repo.TableDef{
		Columns:  []repo.ColumnDef{{Name: "id", Type: "text", PK: true}, {Name: "data", Type: "text"}},
		Conflict: "mtime-wins",
	}

	// Register on both.
	repo.EnsureSyncSchema(serverRepo.DB())
	repo.RegisterSyncedTable(serverRepo.DB(), "del_test", def, 1000)
	repo.EnsureSyncSchema(clientRepo.DB())
	repo.RegisterSyncedTable(clientRepo.DB(), "del_test", def, 1000)

	// Both start with the row.
	repo.UpsertXRow(serverRepo.DB(), "del_test", map[string]any{"id": "k1", "data": "hello"}, 1000)
	repo.UpsertXRow(clientRepo.DB(), "del_test", map[string]any{"id": "k1", "data": "hello"}, 1000)

	// Server deletes the row.
	pkColDefs := []repo.ColumnDef{{Name: "id", Type: "text", PK: true}}
	pkHash := repo.PKHash(pkColDefs, map[string]any{"id": "k1"})
	repo.DeleteXRowByPKHash(serverRepo.DB(), "del_test", def, pkHash, 2000)

	// Sync client → server.
	transport := &MockTransport{Server: serverRepo}
	result, err := Sync(clientRepo, SyncOpts{
		Transport: transport,
		Push:      true,
		Pull:      true,
	})
	if err != nil {
		t.Fatalf("Sync: %v", err)
	}
	_ = result

	// Verify client's row is now a tombstone.
	row, mtime, _ := repo.LookupXRow(clientRepo.DB(), "del_test", def, pkHash)
	if row == nil {
		t.Fatal("row should still exist as tombstone on client")
	}
	if !repo.IsTombstone(def, row) {
		t.Error("client row should be tombstone after sync")
	}
	if mtime != 2000 {
		t.Errorf("client mtime = %d, want 2000", mtime)
	}
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd /Users/dmestas/projects/EdgeSync-tablesync-deletion && go test ./go-libfossil/sync/ -run TestTableSyncDeletion -v`
Expected: FAIL — client doesn't process XDeleteCard.

- [ ] **Step 3: Add handleXDeleteResponse to client_tablesync.go**

Add to `go-libfossil/sync/client_tablesync.go`:

```go
// handleXDeleteResponse processes an xdelete from the server response.
func (s *session) handleXDeleteResponse(c *xfer.XDeleteCard) error {
	if c == nil {
		panic("session.handleXDeleteResponse: c must not be nil")
	}

	if err := repo.EnsureSyncSchema(s.repo.DB()); err != nil {
		return fmt.Errorf("handleXDeleteResponse: ensure schema: %w", err)
	}

	def, err := s.getXTableDef(c.Table)
	if err != nil {
		return fmt.Errorf("handleXDeleteResponse: get table def %s: %w", c.Table, err)
	}
	if def == nil {
		return nil
	}

	deleted, err := repo.DeleteXRowByPKHash(s.repo.DB(), c.Table, *def, c.PKHash, c.MTime)
	if err != nil {
		return fmt.Errorf("handleXDeleteResponse: delete %s/%s: %w", c.Table, c.PKHash, err)
	}

	// If row didn't exist locally, insert tombstone using PKData.
	if !deleted {
		existingRow, _, lookupErr := repo.LookupXRow(s.repo.DB(), c.Table, *def, c.PKHash)
		if lookupErr != nil {
			return fmt.Errorf("handleXDeleteResponse: lookup: %w", lookupErr)
		}
		if existingRow == nil && len(c.PKData) > 0 {
			var pkValues map[string]any
			if err := json.Unmarshal(c.PKData, &pkValues); err != nil {
				return fmt.Errorf("handleXDeleteResponse: bad PKData: %w", err)
			}
			if err := repo.UpsertXRow(s.repo.DB(), c.Table, pkValues, c.MTime); err != nil {
				return fmt.Errorf("handleXDeleteResponse: insert tombstone: %w", err)
			}
		}
	}

	// Remove from gimmes if present.
	if gimmes, ok := s.xTableGimmes[c.Table]; ok {
		delete(gimmes, c.PKHash)
	}

	return nil
}
```

- [ ] **Step 4: Add XDeleteCard dispatch to processXTableCard**

In `go-libfossil/sync/client_tablesync.go`, update `processXTableCard` (around line 150):

```go
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
	case *xfer.XDeleteCard:
		return s.handleXDeleteResponse(c)
	}
	return nil
}
```

- [ ] **Step 5: Add XDeleteCard to client.go dispatch**

In `go-libfossil/sync/client.go`, update the type switch at line 551:

```go
		case *xfer.SchemaCard, *xfer.XIGotCard, *xfer.XGimmeCard, *xfer.XRowCard, *xfer.XDeleteCard:
```

- [ ] **Step 6: Update buildTableSendCards to emit xdelete for local tombstones**

In `go-libfossil/sync/client_tablesync.go`, update `buildTableSendCards` (around line 121-143). Add tombstone detection before emitting xrow:

```go
	// Emit xrow or xdelete for rows queued to send.
	if sends, ok := s.xTableToSend[info.Name]; ok {
		pkColDefs := extractPKColumnDefs(info.Def)
		for pkHash := range sends {
			row, mtime, err := repo.LookupXRow(s.repo.DB(), info.Name, info.Def, pkHash)
			if err != nil {
				return nil, fmt.Errorf("buildTableSendCards: lookup %s/%s: %w", info.Name, pkHash, err)
			}
			if row == nil {
				delete(sends, pkHash)
				continue
			}
			if repo.IsTombstone(info.Def, row) {
				// Send xdelete with PK data.
				pkCols := extractPKColumns(info.Def)
				pkValues := make(map[string]any)
				for _, col := range pkCols {
					pkValues[col] = row[col]
				}
				pkData, err := json.Marshal(pkValues)
				if err != nil {
					return nil, fmt.Errorf("buildTableSendCards: marshal pk %s/%s: %w", info.Name, pkHash, err)
				}
				cards = append(cards, &xfer.XDeleteCard{
					Table:  info.Name,
					PKHash: pkHash,
					MTime:  mtime,
					PKData: pkData,
				})
			} else {
				rowJSON, err := json.Marshal(row)
				if err != nil {
					return nil, fmt.Errorf("buildTableSendCards: marshal row %s/%s: %w", info.Name, pkHash, err)
				}
				cards = append(cards, &xfer.XRowCard{
					Table:   info.Name,
					PKHash:  pkHash,
					MTime:   mtime,
					Content: rowJSON,
				})
			}
			delete(sends, pkHash)
		}
	}
```

- [ ] **Step 7: Run tests**

Run: `cd /Users/dmestas/projects/EdgeSync-tablesync-deletion && go test ./go-libfossil/sync/ -run "TestTableSyncDeletion|TestTableSyncEndToEnd|TestTableSyncSchemaDeployment" -v`
Expected: All PASS.

- [ ] **Step 8: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync-tablesync-deletion
git add go-libfossil/sync/client.go go-libfossil/sync/client_tablesync.go go-libfossil/sync/client_tablesync_test.go
git commit -m "feat(sync): client-side xdelete handling and tombstone-aware sends

Client processes incoming xdelete cards with mtime-wins. buildTableSendCards
emits xdelete (not xrow) for tombstoned rows."
```

---

### Task 8: DST tests — multi-peer deletion, resurrection, integer PKs

**Files:**
- Modify: `dst/tablesync_test.go`

- [ ] **Step 1: Write multi-peer deletion test**

Add to `dst/tablesync_test.go`:

```go
func TestTableSync_Deletion_Convergence(t *testing.T) {
	sim, masterRepo, mf := newTableSyncSim(t, 42, 3, false)
	def := deviceTableDef()
	registerTableAll(t, sim, masterRepo, "devices", def, 1000)

	// Seed a row on all peers via master.
	upsertRow(t, masterRepo, "devices", map[string]any{
		"device_id": "d1", "hostname": "alpha", "status": "online",
	}, 1000)

	// Sync so all leaves have the row.
	for i := 0; i < 3; i++ {
		sim.RunRound(t, mf)
	}
	for _, leafID := range sim.LeafIDs() {
		assertRowCount(t, sim.Leaf(leafID).Repo(), leafID, "devices", def, 1)
	}

	// Delete on master with newer mtime.
	pkColDefs := []repo.ColumnDef{{Name: "device_id", Type: "text", PK: true}}
	pkHash := repo.PKHash(pkColDefs, map[string]any{"device_id": "d1"})
	deleted, err := repo.DeleteXRowByPKHash(masterRepo.DB(), "devices", def, pkHash, 2000)
	if err != nil || !deleted {
		t.Fatalf("master delete: deleted=%v err=%v", deleted, err)
	}

	// Sync until convergence.
	for i := 0; i < 5; i++ {
		sim.RunRound(t, mf)
	}

	// All leaves should have a tombstone.
	for _, leafID := range sim.LeafIDs() {
		row, mtime, _ := repo.LookupXRow(sim.Leaf(leafID).Repo().DB(), "devices", def, pkHash)
		if row == nil {
			t.Errorf("%s: row should exist as tombstone", leafID)
			continue
		}
		if !repo.IsTombstone(def, row) {
			t.Errorf("%s: row should be tombstone", leafID)
		}
		if mtime != 2000 {
			t.Errorf("%s: mtime=%d, want 2000", leafID, mtime)
		}
	}
}

func TestTableSync_Deletion_Resurrection(t *testing.T) {
	sim, masterRepo, mf := newTableSyncSim(t, 99, 2, false)
	def := deviceTableDef()
	registerTableAll(t, sim, masterRepo, "devices", def, 1000)

	// Seed and sync.
	upsertRow(t, masterRepo, "devices", map[string]any{
		"device_id": "d1", "hostname": "alpha", "status": "online",
	}, 1000)
	for i := 0; i < 3; i++ {
		sim.RunRound(t, mf)
	}

	// Delete on master.
	pkColDefs := []repo.ColumnDef{{Name: "device_id", Type: "text", PK: true}}
	pkHash := repo.PKHash(pkColDefs, map[string]any{"device_id": "d1"})
	repo.DeleteXRowByPKHash(masterRepo.DB(), "devices", def, pkHash, 2000)

	// Sync deletion.
	for i := 0; i < 3; i++ {
		sim.RunRound(t, mf)
	}

	// Resurrect on a leaf with newer mtime.
	leaf0 := sim.Leaf(sim.LeafIDs()[0]).Repo()
	upsertRow(t, leaf0, "devices", map[string]any{
		"device_id": "d1", "hostname": "beta", "status": "active",
	}, 3000)

	// Sync resurrection.
	for i := 0; i < 5; i++ {
		sim.RunRound(t, mf)
	}

	// All peers should have the live row with mtime 3000.
	for _, leafID := range sim.LeafIDs() {
		row, mtime, _ := repo.LookupXRow(sim.Leaf(leafID).Repo().DB(), "devices", def, pkHash)
		if row == nil {
			t.Errorf("%s: row missing after resurrection", leafID)
			continue
		}
		if repo.IsTombstone(def, row) {
			t.Errorf("%s: row should be live after resurrection", leafID)
		}
		if mtime != 3000 {
			t.Errorf("%s: mtime=%d, want 3000", leafID, mtime)
		}
	}
	// Master too.
	row, mtime, _ := repo.LookupXRow(masterRepo.DB(), "devices", def, pkHash)
	if row == nil || repo.IsTombstone(def, row) || mtime != 3000 {
		t.Errorf("master: row=%v tombstone=%v mtime=%d", row != nil, row != nil && repo.IsTombstone(def, row), mtime)
	}
}

func TestTableSync_IntegerPK_Convergence(t *testing.T) {
	sim, masterRepo, mf := newTableSyncSim(t, 77, 2, false)
	def := repo.TableDef{
		Columns: []repo.ColumnDef{
			{Name: "seq", Type: "integer", PK: true},
			{Name: "payload", Type: "text"},
		},
		Conflict: "mtime-wins",
	}
	registerTableAll(t, sim, masterRepo, "events", def, 1000)

	// Seed rows with large integer PKs (>2^53).
	bigPK := int64(1<<53 + 1)
	upsertRow(t, masterRepo, "events", map[string]any{
		"seq": bigPK, "payload": "event-data",
	}, 1000)

	// Sync.
	for i := 0; i < 3; i++ {
		sim.RunRound(t, mf)
	}

	// Verify all peers have matching catalog hashes.
	masterHash, _ := repo.CatalogHash(masterRepo.DB(), "events", def)
	for _, leafID := range sim.LeafIDs() {
		leafHash, _ := repo.CatalogHash(sim.Leaf(leafID).Repo().DB(), "events", def)
		if leafHash != masterHash {
			t.Errorf("%s: catalog hash %q != master %q", leafID, leafHash, masterHash)
		}
	}
}
```

- [ ] **Step 2: Run DST tests**

Run: `cd /Users/dmestas/projects/EdgeSync-tablesync-deletion && go test ./dst/ -run "TestTableSync_Deletion|TestTableSync_IntegerPK" -v -count=1`
Expected: All PASS.

- [ ] **Step 3: Run full test suite**

Run: `cd /Users/dmestas/projects/EdgeSync-tablesync-deletion && go test ./go-libfossil/... ./dst/ -count=1 2>&1 | tail -20`
Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync-tablesync-deletion
git add dst/tablesync_test.go
git commit -m "test(dst): add deletion convergence, resurrection, and integer PK tests

Multi-peer deletion propagation, delete-then-resurrect via newer mtime,
and large integer PK (>2^53) convergence across peers."
```

---

### Task 9: Final validation — run all tests and sim

- [ ] **Step 1: Run full test suite including sim**

Run: `cd /Users/dmestas/projects/EdgeSync-tablesync-deletion && make test`
Expected: All pass.

- [ ] **Step 2: Run existing DST table sync tests to check for regressions**

Run: `cd /Users/dmestas/projects/EdgeSync-tablesync-deletion && go test ./dst/ -run TestTableSync -v -count=1 2>&1 | tail -40`
Expected: All existing table sync DST tests pass alongside new ones.

- [ ] **Step 3: Verify build**

Run: `cd /Users/dmestas/projects/EdgeSync-tablesync-deletion && make build`
Expected: Clean build of edgesync, leaf, bridge binaries.
