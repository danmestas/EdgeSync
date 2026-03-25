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
	h2 := PKHash(map[string]any{"peer_id": "leaf-01"})
	if h != h2 {
		t.Fatalf("PKHash not deterministic: %q vs %q", h, h2)
	}
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
	var count int
	if err := r.DB().QueryRow("SELECT count(*) FROM x_peer_registry").Scan(&count); err != nil {
		t.Fatalf("x_peer_registry should exist: %v", err)
	}
}

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
	h2, _ := CatalogHash(r.DB(), "cfg", def)
	if h1 != h2 {
		t.Fatal("catalog hash not deterministic")
	}
	UpsertXRow(r.DB(), "cfg", map[string]any{"peer_id": "c"}, 300)
	h3, _ := CatalogHash(r.DB(), "cfg", def)
	if h1 == h3 {
		t.Fatal("catalog hash unchanged after insert")
	}
}
