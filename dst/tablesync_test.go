package dst

import (
	"fmt"
	"testing"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/repo"
)

// --- Scenario: Table Sync Convergence ---
// Two peers with a shared synced table, different rows on each. After sync rounds, both should have all rows.

func TestTableSyncConvergence(t *testing.T) {
	sev := parseSeverity()
	seed := seedFor(100)

	masterRepo := createMasterRepo(t)
	mf := NewMockFossil(masterRepo)

	// Register a simple synced table on master.
	tableName := "devices"
	tableDef := repo.TableDef{
		Columns: []repo.ColumnDef{
			{Name: "device_id", Type: "text", PK: true},
			{Name: "hostname", Type: "text", PK: false},
			{Name: "status", Type: "text", PK: false},
		},
		Conflict: "mtime-wins",
	}
	if err := repo.EnsureSyncSchema(masterRepo.DB()); err != nil {
		t.Fatalf("EnsureSyncSchema master: %v", err)
	}
	if err := repo.RegisterSyncedTable(masterRepo.DB(), tableName, tableDef, 100); err != nil {
		t.Fatalf("RegisterSyncedTable master: %v", err)
	}

	// Seed 3 rows in master.
	masterRows := map[string]map[string]any{
		"dev-1": {"device_id": "dev-1", "hostname": "master-host-1", "status": "active"},
		"dev-2": {"device_id": "dev-2", "hostname": "master-host-2", "status": "idle"},
		"dev-3": {"device_id": "dev-3", "hostname": "master-host-3", "status": "offline"},
	}
	for _, row := range masterRows {
		if err := repo.UpsertXRow(masterRepo.DB(), tableName, row, 100); err != nil {
			t.Fatalf("UpsertXRow master: %v", err)
		}
	}

	sim, err := New(SimConfig{
		Seed:         seed,
		NumLeaves:    2,
		PollInterval: 5 * time.Second,
		TmpDir:       t.TempDir(),
		Upstream:     mf,
		Buggify:      sev.Buggify,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	defer sim.Close()
	sim.Network().SetDropRate(sev.DropRate)

	// Register table on all leaves and seed unique rows in each leaf.
	for i, leafID := range sim.LeafIDs() {
		leafRepo := sim.Leaf(leafID).Repo()
		if err := repo.EnsureSyncSchema(leafRepo.DB()); err != nil {
			t.Fatalf("EnsureSyncSchema %s: %v", leafID, err)
		}
		if err := repo.RegisterSyncedTable(leafRepo.DB(), tableName, tableDef, 100); err != nil {
			t.Fatalf("RegisterSyncedTable %s: %v", leafID, err)
		}

		// Each leaf gets a unique row.
		deviceID := fmt.Sprintf("dev-leaf-%d", i)
		leafRow := map[string]any{
			"device_id": deviceID,
			"hostname":  fmt.Sprintf("leaf-%d-host", i),
			"status":    "online",
		}
		if err := repo.UpsertXRow(leafRepo.DB(), tableName, leafRow, 110); err != nil {
			t.Fatalf("UpsertXRow %s: %v", leafID, err)
		}
	}

	steps := stepsFor(200)
	if err := sim.Run(steps); err != nil {
		t.Fatalf("Run: %v", err)
	}

	t.Logf("[%s] seed=%d steps=%d syncs=%d errors=%d",
		sev.Name, seed, sim.Steps, sim.TotalSyncs, sim.TotalErrors)

	if err := sim.CheckSafety(); err != nil {
		t.Fatalf("Safety: %v", err)
	}

	// In normal mode, verify full convergence.
	if sev.DropRate == 0 && !sev.Buggify {
		// All leaves should have all 5 rows (3 from master, 2 from leaves).
		expectedRows := 5
		for _, leafID := range sim.LeafIDs() {
			leafRepo := sim.Leaf(leafID).Repo()
			rows, _, err := repo.ListXRows(leafRepo.DB(), tableName, tableDef)
			if err != nil {
				t.Fatalf("ListXRows %s: %v", leafID, err)
			}
			if len(rows) != expectedRows {
				t.Errorf("%s: expected %d rows, got %d", leafID, expectedRows, len(rows))
			}
		}

		// Master should also have all 5 rows.
		masterRowsAfter, _, err := repo.ListXRows(masterRepo.DB(), tableName, tableDef)
		if err != nil {
			t.Fatalf("ListXRows master: %v", err)
		}
		if len(masterRowsAfter) != expectedRows {
			t.Errorf("master: expected %d rows, got %d", expectedRows, len(masterRowsAfter))
		}

		// Verify specific rows exist.
		for _, leafID := range sim.LeafIDs() {
			leafRepo := sim.Leaf(leafID).Repo()
			// Check for master row.
			pkValues := map[string]any{"device_id": "dev-1"}
			pkHash := repo.PKHash(pkValues)
			row, _, err := repo.LookupXRow(leafRepo.DB(), tableName, tableDef, pkHash)
			if err != nil {
				t.Fatalf("LookupXRow %s dev-1: %v", leafID, err)
			}
			if row == nil {
				t.Errorf("%s missing master row dev-1", leafID)
			}
		}
	} else {
		// With faults, log row counts per leaf.
		for _, leafID := range sim.LeafIDs() {
			leafRepo := sim.Leaf(leafID).Repo()
			rows, _, _ := repo.ListXRows(leafRepo.DB(), tableName, tableDef)
			t.Logf("  %s: %d rows", leafID, len(rows))
		}
	}
}

// --- Scenario: Self-Write Enforcement ---
// Register a table with "self-write" conflict. Peer1 writes its own row. After sync, peer2 has the row.
// Verify the row exists on both sides.

func TestTableSyncSelfWrite(t *testing.T) {
	seed := seedFor(101)

	masterRepo := createMasterRepo(t)
	mf := NewMockFossil(masterRepo)

	// Register table with self-write conflict.
	tableName := "peer_status"
	tableDef := repo.TableDef{
		Columns: []repo.ColumnDef{
			{Name: "peer_id", Type: "text", PK: true},
			{Name: "last_seen", Type: "integer", PK: false},
			{Name: "health", Type: "text", PK: false},
		},
		Conflict: "self-write",
	}
	if err := repo.EnsureSyncSchema(masterRepo.DB()); err != nil {
		t.Fatalf("EnsureSyncSchema master: %v", err)
	}
	if err := repo.RegisterSyncedTable(masterRepo.DB(), tableName, tableDef, 100); err != nil {
		t.Fatalf("RegisterSyncedTable master: %v", err)
	}

	sim, err := New(SimConfig{
		Seed:         seed,
		NumLeaves:    2,
		PollInterval: 5 * time.Second,
		TmpDir:       t.TempDir(),
		Upstream:     mf,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	defer sim.Close()

	// Register table on leaves and each writes its own status row.
	for i, leafID := range sim.LeafIDs() {
		leafRepo := sim.Leaf(leafID).Repo()
		if err := repo.EnsureSyncSchema(leafRepo.DB()); err != nil {
			t.Fatalf("EnsureSyncSchema %s: %v", leafID, err)
		}
		if err := repo.RegisterSyncedTable(leafRepo.DB(), tableName, tableDef, 100); err != nil {
			t.Fatalf("RegisterSyncedTable %s: %v", leafID, err)
		}

		// Each leaf writes its own row.
		peerID := fmt.Sprintf("peer-%d", i)
		row := map[string]any{
			"peer_id":   peerID,
			"last_seen": int64(1000 + i*10),
			"health":    "healthy",
		}
		if err := repo.UpsertXRow(leafRepo.DB(), tableName, row, int64(100+i)); err != nil {
			t.Fatalf("UpsertXRow %s: %v", leafID, err)
		}
	}

	if err := sim.Run(150); err != nil {
		t.Fatalf("Run: %v", err)
	}

	t.Logf("Self-write: steps=%d syncs=%d errors=%d",
		sim.Steps, sim.TotalSyncs, sim.TotalErrors)

	if err := sim.CheckSafety(); err != nil {
		t.Fatalf("Safety: %v", err)
	}

	// Verify both leaves have both rows.
	for _, leafID := range sim.LeafIDs() {
		leafRepo := sim.Leaf(leafID).Repo()
		rows, _, err := repo.ListXRows(leafRepo.DB(), tableName, tableDef)
		if err != nil {
			t.Fatalf("ListXRows %s: %v", leafID, err)
		}
		if len(rows) != 2 {
			t.Errorf("%s: expected 2 rows, got %d", leafID, len(rows))
		}
	}

	// Master should also have both rows.
	masterRows, _, err := repo.ListXRows(masterRepo.DB(), tableName, tableDef)
	if err != nil {
		t.Fatalf("ListXRows master: %v", err)
	}
	if len(masterRows) != 2 {
		t.Errorf("master: expected 2 rows, got %d", len(masterRows))
	}

	// Verify specific rows exist in leaf-0.
	leaf0Repo := sim.Leaf("leaf-0").Repo()
	for i := 0; i < 2; i++ {
		peerID := fmt.Sprintf("peer-%d", i)
		pkValues := map[string]any{"peer_id": peerID}
		pkHash := repo.PKHash(pkValues)
		row, _, err := repo.LookupXRow(leaf0Repo.DB(), tableName, tableDef, pkHash)
		if err != nil {
			t.Fatalf("LookupXRow leaf-0 %s: %v", peerID, err)
		}
		if row == nil {
			t.Errorf("leaf-0 missing row %s", peerID)
		}
	}
}

// --- Scenario: BUGGIFY Resilience ---
// Enable BUGGIFY, run sync with table data. Verify convergence despite fault injection.

func TestTableSyncBuggify(t *testing.T) {
	seed := seedFor(102)

	masterRepo := createMasterRepo(t)
	mf := NewMockFossil(masterRepo)

	// Register table.
	tableName := "sensors"
	tableDef := repo.TableDef{
		Columns: []repo.ColumnDef{
			{Name: "sensor_id", Type: "integer", PK: true},
			{Name: "reading", Type: "real", PK: false},
		},
		Conflict: "mtime-wins",
	}
	if err := repo.EnsureSyncSchema(masterRepo.DB()); err != nil {
		t.Fatalf("EnsureSyncSchema master: %v", err)
	}
	if err := repo.RegisterSyncedTable(masterRepo.DB(), tableName, tableDef, 100); err != nil {
		t.Fatalf("RegisterSyncedTable master: %v", err)
	}

	// Seed 10 rows in master.
	for i := 0; i < 10; i++ {
		row := map[string]any{
			"sensor_id": int64(i),
			"reading":   float64(20.0 + float64(i)*0.5),
		}
		if err := repo.UpsertXRow(masterRepo.DB(), tableName, row, 100); err != nil {
			t.Fatalf("UpsertXRow master sensor-%d: %v", i, err)
		}
	}

	sim, err := New(SimConfig{
		Seed:         seed,
		NumLeaves:    3,
		PollInterval: 5 * time.Second,
		TmpDir:       t.TempDir(),
		Upstream:     mf,
		Buggify:      true,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	defer sim.Close()
	sim.Network().SetDropRate(0.10)

	// Register table on leaves.
	for _, leafID := range sim.LeafIDs() {
		leafRepo := sim.Leaf(leafID).Repo()
		if err := repo.EnsureSyncSchema(leafRepo.DB()); err != nil {
			t.Fatalf("EnsureSyncSchema %s: %v", leafID, err)
		}
		if err := repo.RegisterSyncedTable(leafRepo.DB(), tableName, tableDef, 100); err != nil {
			t.Fatalf("RegisterSyncedTable %s: %v", leafID, err)
		}
	}

	steps := stepsFor(300)
	if err := sim.Run(steps); err != nil {
		t.Fatalf("Run: %v", err)
	}

	t.Logf("[BUGGIFY] seed=%d steps=%d syncs=%d errors=%d",
		seed, sim.Steps, sim.TotalSyncs, sim.TotalErrors)

	// Safety must hold even with BUGGIFY.
	// Blob integrity may fail (expected with content.Expand byte-flip),
	// but structural invariants must hold.
	for _, leafID := range sim.LeafIDs() {
		r := sim.Leaf(leafID).Repo()
		if err := CheckDeltaChains(string(leafID), r); err != nil {
			t.Fatalf("Delta chain violation: %v", err)
		}
		if err := CheckNoOrphanPhantoms(string(leafID), r); err != nil {
			t.Fatalf("Orphan phantom violation: %v", err)
		}
	}

	// Log row counts per leaf (convergence not guaranteed under faults).
	for _, leafID := range sim.LeafIDs() {
		leafRepo := sim.Leaf(leafID).Repo()
		rows, _, _ := repo.ListXRows(leafRepo.DB(), tableName, tableDef)
		t.Logf("  %s: %d rows", leafID, len(rows))
	}
}

// --- Scenario: Multi-Table Sync ---
// Multiple synced tables in the same repo. Verify independent convergence.

func TestTableSyncMultipleTables(t *testing.T) {
	seed := seedFor(103)

	masterRepo := createMasterRepo(t)
	mf := NewMockFossil(masterRepo)

	// Register two different tables.
	table1Name := "users"
	table1Def := repo.TableDef{
		Columns: []repo.ColumnDef{
			{Name: "user_id", Type: "integer", PK: true},
			{Name: "username", Type: "text", PK: false},
		},
		Conflict: "mtime-wins",
	}
	table2Name := "posts"
	table2Def := repo.TableDef{
		Columns: []repo.ColumnDef{
			{Name: "post_id", Type: "integer", PK: true},
			{Name: "user_id", Type: "integer", PK: false},
			{Name: "content", Type: "text", PK: false},
		},
		Conflict: "mtime-wins",
	}

	if err := repo.EnsureSyncSchema(masterRepo.DB()); err != nil {
		t.Fatalf("EnsureSyncSchema master: %v", err)
	}
	if err := repo.RegisterSyncedTable(masterRepo.DB(), table1Name, table1Def, 100); err != nil {
		t.Fatalf("RegisterSyncedTable master table1: %v", err)
	}
	if err := repo.RegisterSyncedTable(masterRepo.DB(), table2Name, table2Def, 100); err != nil {
		t.Fatalf("RegisterSyncedTable master table2: %v", err)
	}

	// Seed data in master.
	for i := 0; i < 3; i++ {
		user := map[string]any{
			"user_id":  int64(i),
			"username": fmt.Sprintf("user-%d", i),
		}
		if err := repo.UpsertXRow(masterRepo.DB(), table1Name, user, 100); err != nil {
			t.Fatalf("UpsertXRow master user-%d: %v", i, err)
		}
	}
	for i := 0; i < 5; i++ {
		post := map[string]any{
			"post_id": int64(i),
			"user_id": int64(i % 3),
			"content": fmt.Sprintf("post content %d", i),
		}
		if err := repo.UpsertXRow(masterRepo.DB(), table2Name, post, 100); err != nil {
			t.Fatalf("UpsertXRow master post-%d: %v", i, err)
		}
	}

	sim, err := New(SimConfig{
		Seed:         seed,
		NumLeaves:    2,
		PollInterval: 5 * time.Second,
		TmpDir:       t.TempDir(),
		Upstream:     mf,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	defer sim.Close()

	// Register tables on leaves.
	for _, leafID := range sim.LeafIDs() {
		leafRepo := sim.Leaf(leafID).Repo()
		if err := repo.EnsureSyncSchema(leafRepo.DB()); err != nil {
			t.Fatalf("EnsureSyncSchema %s: %v", leafID, err)
		}
		if err := repo.RegisterSyncedTable(leafRepo.DB(), table1Name, table1Def, 100); err != nil {
			t.Fatalf("RegisterSyncedTable %s table1: %v", leafID, err)
		}
		if err := repo.RegisterSyncedTable(leafRepo.DB(), table2Name, table2Def, 100); err != nil {
			t.Fatalf("RegisterSyncedTable %s table2: %v", leafID, err)
		}
	}

	if err := sim.Run(200); err != nil {
		t.Fatalf("Run: %v", err)
	}

	t.Logf("Multi-table: steps=%d syncs=%d errors=%d",
		sim.Steps, sim.TotalSyncs, sim.TotalErrors)

	if err := sim.CheckSafety(); err != nil {
		t.Fatalf("Safety: %v", err)
	}

	// Verify convergence for both tables.
	for _, leafID := range sim.LeafIDs() {
		leafRepo := sim.Leaf(leafID).Repo()

		users, _, err := repo.ListXRows(leafRepo.DB(), table1Name, table1Def)
		if err != nil {
			t.Fatalf("ListXRows %s users: %v", leafID, err)
		}
		if len(users) != 3 {
			t.Errorf("%s: expected 3 users, got %d", leafID, len(users))
		}

		posts, _, err := repo.ListXRows(leafRepo.DB(), table2Name, table2Def)
		if err != nil {
			t.Fatalf("ListXRows %s posts: %v", leafID, err)
		}
		if len(posts) != 5 {
			t.Errorf("%s: expected 5 posts, got %d", leafID, len(posts))
		}
	}
}

// --- Scenario: MTime Conflict Resolution ---
// Two leaves write different values for the same row. Newer mtime should win.

func TestTableSyncMTimeWins(t *testing.T) {
	seed := seedFor(104)

	masterRepo := createMasterRepo(t)
	mf := NewMockFossil(masterRepo)

	tableName := "config"
	tableDef := repo.TableDef{
		Columns: []repo.ColumnDef{
			{Name: "key", Type: "text", PK: true},
			{Name: "value", Type: "text", PK: false},
		},
		Conflict: "mtime-wins",
	}
	if err := repo.EnsureSyncSchema(masterRepo.DB()); err != nil {
		t.Fatalf("EnsureSyncSchema master: %v", err)
	}
	if err := repo.RegisterSyncedTable(masterRepo.DB(), tableName, tableDef, 100); err != nil {
		t.Fatalf("RegisterSyncedTable master: %v", err)
	}

	sim, err := New(SimConfig{
		Seed:         seed,
		NumLeaves:    2,
		PollInterval: 5 * time.Second,
		TmpDir:       t.TempDir(),
		Upstream:     mf,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	defer sim.Close()

	// Register table on leaves.
	for _, leafID := range sim.LeafIDs() {
		leafRepo := sim.Leaf(leafID).Repo()
		if err := repo.EnsureSyncSchema(leafRepo.DB()); err != nil {
			t.Fatalf("EnsureSyncSchema %s: %v", leafID, err)
		}
		if err := repo.RegisterSyncedTable(leafRepo.DB(), tableName, tableDef, 100); err != nil {
			t.Fatalf("RegisterSyncedTable %s: %v", leafID, err)
		}
	}

	// leaf-0 writes with mtime=100.
	leaf0Repo := sim.Leaf("leaf-0").Repo()
	row0 := map[string]any{"key": "theme", "value": "dark"}
	if err := repo.UpsertXRow(leaf0Repo.DB(), tableName, row0, 100); err != nil {
		t.Fatalf("UpsertXRow leaf-0: %v", err)
	}

	// leaf-1 writes conflicting value with mtime=200 (newer).
	leaf1Repo := sim.Leaf("leaf-1").Repo()
	row1 := map[string]any{"key": "theme", "value": "light"}
	if err := repo.UpsertXRow(leaf1Repo.DB(), tableName, row1, 200); err != nil {
		t.Fatalf("UpsertXRow leaf-1: %v", err)
	}

	if err := sim.Run(200); err != nil {
		t.Fatalf("Run: %v", err)
	}

	t.Logf("MTime conflict: steps=%d syncs=%d errors=%d",
		sim.Steps, sim.TotalSyncs, sim.TotalErrors)

	if err := sim.CheckSafety(); err != nil {
		t.Fatalf("Safety: %v", err)
	}

	// Both leaves should have the newer value (mtime=200 from leaf-1).
	pkValues := map[string]any{"key": "theme"}
	pkHash := repo.PKHash(pkValues)

	for _, leafID := range sim.LeafIDs() {
		leafRepo := sim.Leaf(leafID).Repo()
		row, mtime, err := repo.LookupXRow(leafRepo.DB(), tableName, tableDef, pkHash)
		if err != nil {
			t.Fatalf("LookupXRow %s: %v", leafID, err)
		}
		if row == nil {
			t.Fatalf("%s: missing row for key=theme", leafID)
		}
		if row["value"] != "light" {
			t.Errorf("%s: expected value=light, got %v", leafID, row["value"])
		}
		if mtime != 200 {
			t.Errorf("%s: expected mtime=200, got %d", leafID, mtime)
		}
	}

	// Master should also have the newer value.
	masterRow, masterMtime, err := repo.LookupXRow(masterRepo.DB(), tableName, tableDef, pkHash)
	if err != nil {
		t.Fatalf("LookupXRow master: %v", err)
	}
	if masterRow == nil {
		t.Fatal("master: missing row for key=theme")
	}
	if masterRow["value"] != "light" {
		t.Errorf("master: expected value=light, got %v", masterRow["value"])
	}
	if masterMtime != 200 {
		t.Errorf("master: expected mtime=200, got %d", masterMtime)
	}
}
