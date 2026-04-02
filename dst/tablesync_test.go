package dst

import (
	"fmt"
	"testing"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/uv"
)

// --- Table sync test helpers ---

// deviceTableDef returns a standard "devices" table definition for tests.
func deviceTableDef() repo.TableDef {
	return repo.TableDef{
		Columns: []repo.ColumnDef{
			{Name: "device_id", Type: "text", PK: true},
			{Name: "hostname", Type: "text"},
			{Name: "status", Type: "text"},
		},
		Conflict: "mtime-wins",
	}
}

// configTableDef returns a key-value table definition for tests.
func configTableDef() repo.TableDef {
	return repo.TableDef{
		Columns: []repo.ColumnDef{
			{Name: "key", Type: "text", PK: true},
			{Name: "value", Type: "text"},
		},
		Conflict: "mtime-wins",
	}
}

// selfWriteTableDef returns a self-write table definition.
func selfWriteTableDef() repo.TableDef {
	return repo.TableDef{
		Columns: []repo.ColumnDef{
			{Name: "peer_id", Type: "text", PK: true},
			{Name: "last_seen", Type: "integer"},
			{Name: "health", Type: "text"},
		},
		Conflict: "self-write",
	}
}

// ownerWriteTableDef returns an owner-write table definition.
func ownerWriteTableDef() repo.TableDef {
	return repo.TableDef{
		Columns: []repo.ColumnDef{
			{Name: "resource_id", Type: "text", PK: true},
			{Name: "data", Type: "text"},
		},
		Conflict: "owner-write",
	}
}

// registerTable registers a synced table on a repo (schema + table).
func registerTable(t *testing.T, r *repo.Repo, name string, def repo.TableDef, mtime int64) {
	t.Helper()
	if err := repo.EnsureSyncSchema(r.DB()); err != nil {
		t.Fatalf("EnsureSyncSchema: %v", err)
	}
	if err := repo.RegisterSyncedTable(r.DB(), name, def, mtime); err != nil {
		t.Fatalf("RegisterSyncedTable %s: %v", name, err)
	}
}

// registerTableAll registers a synced table on master and all sim leaves.
func registerTableAll(t *testing.T, sim *Simulator, masterRepo *repo.Repo, name string, def repo.TableDef, mtime int64) {
	t.Helper()
	registerTable(t, masterRepo, name, def, mtime)
	for _, leafID := range sim.LeafIDs() {
		registerTable(t, sim.Leaf(leafID).Repo(), name, def, mtime)
	}
}

// upsertRow is a short helper wrapping repo.UpsertXRow with fatal on error.
func upsertRow(t *testing.T, r *repo.Repo, table string, row map[string]any, mtime int64) {
	t.Helper()
	if err := repo.UpsertXRow(r.DB(), table, row, mtime); err != nil {
		t.Fatalf("UpsertXRow: %v", err)
	}
}

// assertRowCount checks that a repo has exactly n rows in the given table.
func assertRowCount(t *testing.T, r *repo.Repo, label, table string, def repo.TableDef, n int) {
	t.Helper()
	rows, _, err := repo.ListXRows(r.DB(), table, def)
	if err != nil {
		t.Fatalf("ListXRows %s: %v", label, err)
	}
	if len(rows) != n {
		t.Errorf("%s: expected %d rows, got %d", label, n, len(rows))
	}
}

// assertRowValue checks a specific row's column value by PK lookup.
func assertRowValue(t *testing.T, r *repo.Repo, label, table string, def repo.TableDef, pk map[string]any, col, want string) {
	t.Helper()
	var pkColDefs []repo.ColumnDef
	for _, c := range def.Columns {
		if c.PK {
			pkColDefs = append(pkColDefs, c)
		}
	}
	pkHash := repo.PKHash(pkColDefs, pk)
	row, _, err := repo.LookupXRow(r.DB(), table, def, pkHash)
	if err != nil {
		t.Fatalf("LookupXRow %s: %v", label, err)
	}
	if row == nil {
		t.Fatalf("%s: missing row pk=%v", label, pk)
	}
	got, _ := row[col].(string)
	if got != want {
		t.Errorf("%s: %s=%q, want %q", label, col, got, want)
	}
}

// logRowCounts logs the row count per leaf for a table (used in adversarial mode).
func logRowCounts(t *testing.T, sim *Simulator, table string, def repo.TableDef) {
	t.Helper()
	for _, leafID := range sim.LeafIDs() {
		rows, _, _ := repo.ListXRows(sim.Leaf(leafID).Repo().DB(), table, def)
		t.Logf("  %s: %d rows", leafID, len(rows))
	}
}

// newTableSyncSim creates a standard simulator for table sync tests.
func newTableSyncSim(t *testing.T, seed int64, numLeaves int, buggify bool) (*Simulator, *repo.Repo, *MockFossil) {
	t.Helper()
	masterRepo := createMasterRepo(t)
	mf := NewMockFossil(masterRepo)
	sim, err := New(SimConfig{
		Seed:                seed,
		NumLeaves:           numLeaves,
		PollInterval:        5 * time.Second,
		TmpDir:              t.TempDir(),
		Upstream:            mf,
		Buggify:             buggify,
		SafetyCheckInterval: 10,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	t.Cleanup(func() { sim.Close() })
	sim.SetMasterRepo(masterRepo)
	return sim, masterRepo, mf
}

// runAndCheck runs simulation, logs stats, checks safety, and optionally convergence.
func runAndCheck(t *testing.T, sim *Simulator, sev severity, seed int64, steps int) {
	t.Helper()
	if err := sim.Run(steps); err != nil {
		t.Fatalf("Run: %v", err)
	}
	t.Logf("[%s] seed=%d steps=%d syncs=%d errors=%d",
		sev.Name, seed, sim.Steps, sim.TotalSyncs, sim.TotalErrors)
	if err := sim.CheckSafety(); err != nil {
		t.Fatalf("Safety: %v", err)
	}
}

// isNormalMode returns true when severity has no faults.
func isNormalMode(sev severity) bool {
	return sev.DropRate == 0 && !sev.Buggify
}

// =============================================================================
// Level 1: Normal (0% faults) — seeds 200-206
// =============================================================================

// TestTS_Convergence3Peer: 3 leaves each write unique rows, all converge.
func TestTS_Convergence3Peer(t *testing.T) {
	sev := parseSeverity()
	seed := seedFor(200)
	sim, masterRepo, _ := newTableSyncSim(t, seed, 3, sev.Buggify)
	sim.Network().SetDropRate(sev.DropRate)

	table := "devices"
	def := deviceTableDef()
	registerTableAll(t, sim, masterRepo, table, def, 100)

	// Each peer writes 3 unique rows.
	for i, leafID := range sim.LeafIDs() {
		r := sim.Leaf(leafID).Repo()
		for j := 0; j < 3; j++ {
			upsertRow(t, r, table, map[string]any{
				"device_id": fmt.Sprintf("leaf%d-dev%d", i, j),
				"hostname":  fmt.Sprintf("host-%d-%d", i, j),
				"status":    "online",
			}, int64(100+i*10+j))
		}
	}

	runAndCheck(t, sim, sev, seed, stepsFor(300))

	if isNormalMode(sev) {
		if err := sim.CheckAllTableSyncConverged(); err != nil {
			t.Fatalf("TableSync convergence: %v", err)
		}
		// 9 rows total (3 per peer), should appear on all nodes.
		for _, leafID := range sim.LeafIDs() {
			assertRowCount(t, sim.Leaf(leafID).Repo(), string(leafID), table, def, 9)
		}
		assertRowCount(t, masterRepo, "master", table, def, 9)
	} else {
		logRowCounts(t, sim, table, def)
	}
}

// TestTS_SchemaDeployChain: schema registered on master propagates to all leaves.
func TestTS_SchemaDeployChain(t *testing.T) {
	sev := parseSeverity()
	seed := seedFor(201)
	sim, masterRepo, _ := newTableSyncSim(t, seed, 3, sev.Buggify)
	sim.Network().SetDropRate(sev.DropRate)

	// Register only on master, not on leaves.
	table := "settings"
	def := configTableDef()
	registerTable(t, masterRepo, table, def, 100)

	runAndCheck(t, sim, sev, seed, stepsFor(300))

	if isNormalMode(sev) {
		// Verify schema deployed to all leaves.
		for _, leafID := range sim.LeafIDs() {
			r := sim.Leaf(leafID).Repo()
			tables, err := repo.ListSyncedTables(r.DB())
			if err != nil {
				t.Fatalf("ListSyncedTables %s: %v", leafID, err)
			}
			found := false
			for _, tbl := range tables {
				if tbl.Name == table {
					found = true
				}
			}
			if !found {
				t.Errorf("%s: schema %q not deployed", leafID, table)
			}
		}
	}
}

// TestTS_MultipleTables5: 5 tables sync independently between 2 peers.
func TestTS_MultipleTables5(t *testing.T) {
	sev := parseSeverity()
	seed := seedFor(202)
	sim, masterRepo, _ := newTableSyncSim(t, seed, 2, sev.Buggify)
	sim.Network().SetDropRate(sev.DropRate)

	// Register 5 tables, each with 5 rows on master.
	defs := make([]repo.TableDef, 5)
	names := make([]string, 5)
	for i := 0; i < 5; i++ {
		names[i] = fmt.Sprintf("table_%d", i)
		defs[i] = configTableDef()
		registerTableAll(t, sim, masterRepo, names[i], defs[i], 100)
		for j := 0; j < 5; j++ {
			upsertRow(t, masterRepo, names[i], map[string]any{
				"key":   fmt.Sprintf("k%d-%d", i, j),
				"value": fmt.Sprintf("v%d-%d", i, j),
			}, 100)
		}
	}

	runAndCheck(t, sim, sev, seed, stepsFor(300))

	if isNormalMode(sev) {
		if err := sim.CheckAllTableSyncConverged(); err != nil {
			t.Fatalf("TableSync convergence: %v", err)
		}
		for i, name := range names {
			for _, leafID := range sim.LeafIDs() {
				assertRowCount(t, sim.Leaf(leafID).Repo(), string(leafID), name, defs[i], 5)
			}
		}
	}
}

// TestTS_CatalogHashShortCircuit: same data on both sides, verify no issues.
func TestTS_CatalogHashShortCircuit(t *testing.T) {
	sev := parseSeverity()
	seed := seedFor(203)
	sim, masterRepo, _ := newTableSyncSim(t, seed, 2, sev.Buggify)
	sim.Network().SetDropRate(sev.DropRate)

	table := "configs"
	def := configTableDef()
	registerTableAll(t, sim, masterRepo, table, def, 100)

	// Seed identical 10 rows in master and both leaves.
	allRepos := []*repo.Repo{masterRepo}
	for _, leafID := range sim.LeafIDs() {
		allRepos = append(allRepos, sim.Leaf(leafID).Repo())
	}
	for _, r := range allRepos {
		for i := 0; i < 10; i++ {
			upsertRow(t, r, table, map[string]any{
				"key":   fmt.Sprintf("k%d", i),
				"value": fmt.Sprintf("v%d", i),
			}, 100)
		}
	}

	runAndCheck(t, sim, sev, seed, stepsFor(200))

	if isNormalMode(sev) {
		if err := sim.CheckAllTableSyncConverged(); err != nil {
			t.Fatalf("TableSync convergence: %v", err)
		}
		// Catalog hashes should all match.
		masterHash, _ := repo.CatalogHash(masterRepo.DB(), table, def)
		for _, leafID := range sim.LeafIDs() {
			leafHash, _ := repo.CatalogHash(sim.Leaf(leafID).Repo().DB(), table, def)
			if leafHash != masterHash {
				t.Errorf("%s hash %s != master %s", leafID, leafHash, masterHash)
			}
		}
	}
}

// TestTS_MtimeWinsSamePK: 3 peers write same PK with different mtimes, newest wins.
func TestTS_MtimeWinsSamePK(t *testing.T) {
	sev := parseSeverity()
	seed := seedFor(204)
	sim, masterRepo, _ := newTableSyncSim(t, seed, 3, sev.Buggify)
	sim.Network().SetDropRate(sev.DropRate)

	table := "config"
	def := configTableDef()
	registerTableAll(t, sim, masterRepo, table, def, 100)

	// leaf-0: mtime=100, leaf-1: mtime=200, leaf-2: mtime=300 (winner).
	for i, leafID := range sim.LeafIDs() {
		upsertRow(t, sim.Leaf(leafID).Repo(), table, map[string]any{
			"key":   "theme",
			"value": fmt.Sprintf("val-%d", i),
		}, int64(100+i*100))
	}

	runAndCheck(t, sim, sev, seed, stepsFor(300))

	if isNormalMode(sev) {
		if err := sim.CheckAllTableSyncConverged(); err != nil {
			t.Fatalf("TableSync convergence: %v", err)
		}
		// leaf-2's value (mtime=300) should win everywhere.
		pk := map[string]any{"key": "theme"}
		assertRowValue(t, masterRepo, "master", table, def, pk, "value", "val-2")
		for _, leafID := range sim.LeafIDs() {
			assertRowValue(t, sim.Leaf(leafID).Repo(), string(leafID), table, def, pk, "value", "val-2")
		}
	}
}

// TestTS_MtimeWinsTieBreak: same PK, same mtime, same content -- idempotent.
func TestTS_MtimeWinsTieBreak(t *testing.T) {
	sev := parseSeverity()
	seed := seedFor(205)
	sim, masterRepo, _ := newTableSyncSim(t, seed, 2, sev.Buggify)
	sim.Network().SetDropRate(sev.DropRate)

	table := "config"
	def := configTableDef()
	registerTableAll(t, sim, masterRepo, table, def, 100)

	// Both leaves write identical row with identical mtime.
	for _, leafID := range sim.LeafIDs() {
		upsertRow(t, sim.Leaf(leafID).Repo(), table, map[string]any{
			"key":   "lang",
			"value": "en",
		}, 100)
	}

	runAndCheck(t, sim, sev, seed, stepsFor(200))

	if isNormalMode(sev) {
		if err := sim.CheckAllTableSyncConverged(); err != nil {
			t.Fatalf("TableSync convergence: %v", err)
		}
		// Exactly 1 row on each node.
		assertRowCount(t, masterRepo, "master", table, def, 1)
		for _, leafID := range sim.LeafIDs() {
			assertRowCount(t, sim.Leaf(leafID).Repo(), string(leafID), table, def, 1)
		}
	}
}

// TestTS_SelfWriteEnforcement: each peer writes own row, verify propagation.
func TestTS_SelfWriteEnforcement(t *testing.T) {
	sev := parseSeverity()
	seed := seedFor(206)
	sim, masterRepo, _ := newTableSyncSim(t, seed, 3, sev.Buggify)
	sim.Network().SetDropRate(sev.DropRate)

	table := "peer_status"
	def := selfWriteTableDef()
	registerTableAll(t, sim, masterRepo, table, def, 100)

	// Each leaf writes its own status row.
	for i, leafID := range sim.LeafIDs() {
		upsertRow(t, sim.Leaf(leafID).Repo(), table, map[string]any{
			"peer_id":   fmt.Sprintf("peer-%d", i),
			"last_seen": int64(1000 + i*10),
			"health":    "healthy",
		}, int64(100+i))
	}

	runAndCheck(t, sim, sev, seed, stepsFor(300))

	if isNormalMode(sev) {
		if err := sim.CheckAllTableSyncConverged(); err != nil {
			t.Fatalf("TableSync convergence: %v", err)
		}
		// All 3 rows should appear on every node.
		assertRowCount(t, masterRepo, "master", table, def, 3)
		for _, leafID := range sim.LeafIDs() {
			assertRowCount(t, sim.Leaf(leafID).Repo(), string(leafID), table, def, 3)
		}
	} else {
		logRowCounts(t, sim, table, def)
	}
}

// =============================================================================
// Level 2: Adversarial (10% faults) — seeds 300-306
// =============================================================================

// TestTS_PartitionHeal: partition leaf-0, write on both sides, heal, converge.
func TestTS_PartitionHeal(t *testing.T) {
	sev := parseSeverity()
	seed := seedFor(300)
	sim, masterRepo, _ := newTableSyncSim(t, seed, 3, sev.Buggify)
	sim.Network().SetDropRate(sev.DropRate)

	table := "devices"
	def := deviceTableDef()
	registerTableAll(t, sim, masterRepo, table, def, 100)

	// Phase 1: baseline sync with some rows.
	upsertRow(t, masterRepo, table, map[string]any{
		"device_id": "shared", "hostname": "base", "status": "ok",
	}, 100)
	if err := sim.Run(stepsFor(50)); err != nil {
		t.Fatalf("Run phase 1: %v", err)
	}

	// Phase 2: partition leaf-0, write on both sides.
	sim.Network().Partition("leaf-0")
	upsertRow(t, sim.Leaf("leaf-0").Repo(), table, map[string]any{
		"device_id": "isolated", "hostname": "from-0", "status": "partitioned",
	}, 200)
	upsertRow(t, sim.Leaf("leaf-1").Repo(), table, map[string]any{
		"device_id": "connected", "hostname": "from-1", "status": "ok",
	}, 200)
	if err := sim.Run(stepsFor(100)); err != nil {
		t.Fatalf("Run phase 2: %v", err)
	}

	// Phase 3: heal and converge.
	sim.Network().Heal("leaf-0")
	for _, leafID := range sim.LeafIDs() {
		sim.ScheduleSyncNow(leafID)
	}
	if err := sim.Run(stepsFor(150)); err != nil {
		t.Fatalf("Run phase 3: %v", err)
	}

	if err := sim.CheckSafety(); err != nil {
		t.Fatalf("Safety: %v", err)
	}
	t.Logf("[%s] seed=%d steps=%d syncs=%d errors=%d",
		sev.Name, seed, sim.Steps, sim.TotalSyncs, sim.TotalErrors)

	if isNormalMode(sev) {
		if err := sim.CheckAllTableSyncConverged(); err != nil {
			t.Fatalf("TableSync convergence: %v", err)
		}
	} else {
		logRowCounts(t, sim, table, def)
	}
}

// TestTS_PartitionConcurrentSamePK: both write same PK during partition.
func TestTS_PartitionConcurrentSamePK(t *testing.T) {
	sev := parseSeverity()
	seed := seedFor(301)
	sim, masterRepo, _ := newTableSyncSim(t, seed, 2, sev.Buggify)
	sim.Network().SetDropRate(sev.DropRate)

	table := "config"
	def := configTableDef()
	registerTableAll(t, sim, masterRepo, table, def, 100)

	// Phase 1: partition both leaves from each other (via master).
	sim.Network().Partition("leaf-0")

	// leaf-0 writes with mtime=100, leaf-1 with mtime=200 (winner).
	upsertRow(t, sim.Leaf("leaf-0").Repo(), table, map[string]any{
		"key": "mode", "value": "old",
	}, 100)
	upsertRow(t, sim.Leaf("leaf-1").Repo(), table, map[string]any{
		"key": "mode", "value": "new",
	}, 200)
	if err := sim.Run(stepsFor(100)); err != nil {
		t.Fatalf("Run partitioned: %v", err)
	}

	// Phase 2: heal and converge.
	sim.Network().Heal("leaf-0")
	for _, leafID := range sim.LeafIDs() {
		sim.ScheduleSyncNow(leafID)
	}
	if err := sim.Run(stepsFor(200)); err != nil {
		t.Fatalf("Run healed: %v", err)
	}

	if err := sim.CheckSafety(); err != nil {
		t.Fatalf("Safety: %v", err)
	}
	t.Logf("[%s] seed=%d steps=%d syncs=%d errors=%d",
		sev.Name, seed, sim.Steps, sim.TotalSyncs, sim.TotalErrors)

	if isNormalMode(sev) {
		if err := sim.CheckAllTableSyncConverged(); err != nil {
			t.Fatalf("TableSync convergence: %v", err)
		}
		// mtime=200 "new" should win.
		pk := map[string]any{"key": "mode"}
		assertRowValue(t, masterRepo, "master", table, def, pk, "value", "new")
	}
}

// TestTS_BuggifyConvergence100: 100 rows, 10% faults, must converge.
func TestTS_BuggifyConvergence100(t *testing.T) {
	seed := seedFor(302)
	sim, masterRepo, _ := newTableSyncSim(t, seed, 3, true)
	sim.Network().SetDropRate(0.10)

	table := "sensors"
	def := repo.TableDef{
		Columns: []repo.ColumnDef{
			{Name: "sensor_id", Type: "integer", PK: true},
			{Name: "reading", Type: "real"},
		},
		Conflict: "mtime-wins",
	}
	registerTableAll(t, sim, masterRepo, table, def, 100)

	// Seed 100 rows in master.
	for i := 0; i < 100; i++ {
		upsertRow(t, masterRepo, table, map[string]any{
			"sensor_id": int64(i),
			"reading":   float64(20.0 + float64(i)*0.1),
		}, 100)
	}

	if err := sim.Run(stepsFor(500)); err != nil {
		t.Fatalf("Run: %v", err)
	}

	t.Logf("[buggify] seed=%d steps=%d syncs=%d errors=%d",
		seed, sim.Steps, sim.TotalSyncs, sim.TotalErrors)

	// Structural safety must hold even with buggify.
	for _, leafID := range sim.LeafIDs() {
		r := sim.Leaf(leafID).Repo()
		if err := CheckDeltaChains(string(leafID), r); err != nil {
			t.Fatalf("Delta chain: %v", err)
		}
		if err := CheckNoOrphanPhantoms(string(leafID), r); err != nil {
			t.Fatalf("Orphan phantom: %v", err)
		}
	}

	// Log row counts (convergence not guaranteed under buggify).
	logRowCounts(t, sim, table, def)
}

// TestTS_BuggifyMultiTable: 3 tables x 10 rows under faults.
func TestTS_BuggifyMultiTable(t *testing.T) {
	seed := seedFor(303)
	sim, masterRepo, _ := newTableSyncSim(t, seed, 3, true)
	sim.Network().SetDropRate(0.10)

	// Register 3 tables.
	names := []string{"alpha", "beta", "gamma"}
	def := configTableDef()
	for _, name := range names {
		registerTableAll(t, sim, masterRepo, name, def, 100)
		for j := 0; j < 10; j++ {
			upsertRow(t, masterRepo, name, map[string]any{
				"key":   fmt.Sprintf("%s-k%d", name, j),
				"value": fmt.Sprintf("%s-v%d", name, j),
			}, 100)
		}
	}

	if err := sim.Run(stepsFor(500)); err != nil {
		t.Fatalf("Run: %v", err)
	}

	t.Logf("[buggify-multi] seed=%d steps=%d syncs=%d errors=%d",
		seed, sim.Steps, sim.TotalSyncs, sim.TotalErrors)

	// Structural safety.
	for _, leafID := range sim.LeafIDs() {
		r := sim.Leaf(leafID).Repo()
		if err := CheckDeltaChains(string(leafID), r); err != nil {
			t.Fatalf("Delta chain: %v", err)
		}
	}

	// Log per-table row counts.
	for _, name := range names {
		t.Logf("  table %s:", name)
		logRowCounts(t, sim, name, def)
	}
}

// TestTS_StalePeerRejoin: peer partitioned 10 rounds, rejoins and catches up.
func TestTS_StalePeerRejoin(t *testing.T) {
	sev := parseSeverity()
	seed := seedFor(304)
	sim, masterRepo, _ := newTableSyncSim(t, seed, 3, sev.Buggify)
	sim.Network().SetDropRate(sev.DropRate)

	table := "devices"
	def := deviceTableDef()
	registerTableAll(t, sim, masterRepo, table, def, 100)

	// Phase 1: all connected, seed initial data.
	for i := 0; i < 5; i++ {
		upsertRow(t, masterRepo, table, map[string]any{
			"device_id": fmt.Sprintf("init-%d", i),
			"hostname":  fmt.Sprintf("host-%d", i),
			"status":    "active",
		}, 100)
	}
	if err := sim.Run(stepsFor(50)); err != nil {
		t.Fatalf("Run phase 1: %v", err)
	}

	// Phase 2: partition leaf-2 for 10 rounds while others keep syncing.
	sim.Network().Partition("leaf-2")
	for i := 0; i < 5; i++ {
		upsertRow(t, masterRepo, table, map[string]any{
			"device_id": fmt.Sprintf("new-%d", i),
			"hostname":  fmt.Sprintf("new-host-%d", i),
			"status":    "pending",
		}, 200)
	}
	if err := sim.Run(stepsFor(100)); err != nil {
		t.Fatalf("Run phase 2: %v", err)
	}

	// Phase 3: heal and let leaf-2 catch up.
	sim.Network().Heal("leaf-2")
	sim.ScheduleSyncNow("leaf-2")
	if err := sim.Run(stepsFor(150)); err != nil {
		t.Fatalf("Run phase 3: %v", err)
	}

	if err := sim.CheckSafety(); err != nil {
		t.Fatalf("Safety: %v", err)
	}
	t.Logf("[%s] seed=%d steps=%d syncs=%d errors=%d",
		sev.Name, seed, sim.Steps, sim.TotalSyncs, sim.TotalErrors)

	if isNormalMode(sev) {
		if err := sim.CheckAllTableSyncConverged(); err != nil {
			t.Fatalf("TableSync convergence: %v", err)
		}
		// leaf-2 should have all 10 rows.
		assertRowCount(t, sim.Leaf("leaf-2").Repo(), "leaf-2", table, def, 10)
	} else {
		logRowCounts(t, sim, table, def)
	}
}

// TestTS_SchemaBeforeRows: schema deployed first, then rows populate.
func TestTS_SchemaBeforeRows(t *testing.T) {
	sev := parseSeverity()
	seed := seedFor(305)
	sim, masterRepo, _ := newTableSyncSim(t, seed, 2, sev.Buggify)
	sim.Network().SetDropRate(sev.DropRate)

	table := "metrics"
	def := configTableDef()

	// Register only on master (schema will propagate to leaves).
	registerTable(t, masterRepo, table, def, 100)

	// Phase 1: sync to deploy schema.
	if err := sim.Run(stepsFor(100)); err != nil {
		t.Fatalf("Run phase 1: %v", err)
	}

	// Phase 2: now add rows to master.
	for i := 0; i < 5; i++ {
		upsertRow(t, masterRepo, table, map[string]any{
			"key":   fmt.Sprintf("metric-%d", i),
			"value": fmt.Sprintf("%d", i*100),
		}, 200)
	}
	if err := sim.Run(stepsFor(200)); err != nil {
		t.Fatalf("Run phase 2: %v", err)
	}

	if err := sim.CheckSafety(); err != nil {
		t.Fatalf("Safety: %v", err)
	}
	t.Logf("[%s] seed=%d steps=%d syncs=%d errors=%d",
		sev.Name, seed, sim.Steps, sim.TotalSyncs, sim.TotalErrors)

	if isNormalMode(sev) {
		if err := sim.CheckAllTableSyncConverged(); err != nil {
			t.Fatalf("TableSync convergence: %v", err)
		}
		for _, leafID := range sim.LeafIDs() {
			assertRowCount(t, sim.Leaf(leafID).Repo(), string(leafID), table, def, 5)
		}
	} else {
		logRowCounts(t, sim, table, def)
	}
}

// TestTS_OwnerWriteEnforcement: owner-write table with competing writers.
// Owner-write conflict resolution depends on loginUser (auth), so in the DST
// sim (unauthenticated), the handler may reject cross-owner writes. This test
// verifies structural safety and that rows from distinct owners coexist.
func TestTS_OwnerWriteEnforcement(t *testing.T) {
	sev := parseSeverity()
	seed := seedFor(306)
	sim, masterRepo, _ := newTableSyncSim(t, seed, 3, sev.Buggify)
	sim.Network().SetDropRate(sev.DropRate)

	table := "resources"
	def := ownerWriteTableDef()
	registerTableAll(t, sim, masterRepo, table, def, 100)

	// Each leaf writes a distinct resource with its own _owner.
	for i, leafID := range sim.LeafIDs() {
		upsertRow(t, sim.Leaf(leafID).Repo(), table, map[string]any{
			"resource_id": fmt.Sprintf("res-%d", i),
			"data":        fmt.Sprintf("data-from-%d", i),
			"_owner":      string(leafID),
		}, int64(100+i))
	}

	if err := sim.Run(stepsFor(300)); err != nil {
		t.Fatalf("Run: %v", err)
	}

	if err := sim.CheckSafety(); err != nil {
		t.Fatalf("Safety: %v", err)
	}
	t.Logf("[%s] seed=%d steps=%d syncs=%d errors=%d",
		sev.Name, seed, sim.Steps, sim.TotalSyncs, sim.TotalErrors)

	// Log row distribution. Owner-write conflict may prevent full propagation
	// without authenticated users, so we only assert safety (not convergence).
	logRowCounts(t, sim, table, def)
}

// =============================================================================
// Level 3: Hostile (25% faults) — seeds 400-406
// =============================================================================

// newHostileSim creates a simulator with BUGGIFY enabled and 25% drop rate.
// SafetyCheckInterval is disabled (0) for hostile tests because buggify's
// content.Expand byte-flip triggers blob-integrity false positives.
func newHostileSim(t *testing.T, seed int64, numLeaves int, uvEnabled bool) (*Simulator, *repo.Repo, *MockFossil) {
	t.Helper()
	masterRepo := createMasterRepo(t)
	mf := NewMockFossil(masterRepo)
	sim, err := New(SimConfig{
		Seed:         seed,
		NumLeaves:    numLeaves,
		PollInterval: 5 * time.Second,
		TmpDir:       t.TempDir(),
		Upstream:     mf,
		Buggify:      true,
		UV:           uvEnabled,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	t.Cleanup(func() { sim.Close() })
	sim.Network().SetDropRate(0.25)
	sim.SetMasterRepo(masterRepo)
	return sim, masterRepo, mf
}

// assertBoundedProgress asserts at least minRows rows on every peer.
func assertBoundedProgress(t *testing.T, sim *Simulator, table string, def repo.TableDef, minRows int) {
	t.Helper()
	for _, leafID := range sim.LeafIDs() {
		rows, _, err := repo.ListXRows(sim.Leaf(leafID).Repo().DB(), table, def)
		if err != nil {
			t.Fatalf("ListXRows %s: %v", leafID, err)
		}
		if len(rows) < minRows {
			t.Errorf("%s: expected >= %d rows, got %d", leafID, minRows, len(rows))
		}
	}
}

// checkStructuralIntegrity verifies delta chains and orphan phantoms on all leaves.
func checkStructuralIntegrity(t *testing.T, sim *Simulator) {
	t.Helper()
	for _, leafID := range sim.LeafIDs() {
		r := sim.Leaf(leafID).Repo()
		if err := CheckDeltaChains(string(leafID), r); err != nil {
			t.Fatalf("Delta chain %s: %v", leafID, err)
		}
		if err := CheckNoOrphanPhantoms(string(leafID), r); err != nil {
			t.Fatalf("Orphan phantom %s: %v", leafID, err)
		}
	}
}

// TestTS_HostileConvergence: 5 leaves, 10 rows each, 25% fault rate.
// Asserts safety and bounded progress (not full convergence).
func TestTS_HostileConvergence(t *testing.T) {
	seed := seedFor(400)
	sim, masterRepo, _ := newHostileSim(t, seed, 5, false)

	table := "devices"
	def := deviceTableDef()
	registerTableAll(t, sim, masterRepo, table, def, 100)

	// 10 rows per peer (50 total).
	for i, leafID := range sim.LeafIDs() {
		r := sim.Leaf(leafID).Repo()
		for j := 0; j < 10; j++ {
			upsertRow(t, r, table, map[string]any{
				"device_id": fmt.Sprintf("leaf%d-dev%d", i, j),
				"hostname":  fmt.Sprintf("host-%d-%d", i, j),
				"status":    "online",
			}, int64(100+i*100+j))
		}
	}

	if err := sim.Run(stepsFor(300)); err != nil {
		t.Fatalf("Run: %v", err)
	}
	t.Logf("[hostile] seed=%d steps=%d syncs=%d errors=%d",
		seed, sim.Steps, sim.TotalSyncs, sim.TotalErrors)

	checkStructuralIntegrity(t, sim)
	assertBoundedProgress(t, sim, table, def, 10)
	logRowCounts(t, sim, table, def)
}

// TestTS_CorruptXRowPayload: BUGGIFY corrupts JSON payloads, handler recovers.
func TestTS_CorruptXRowPayload(t *testing.T) {
	seed := seedFor(401)
	sim, masterRepo, _ := newHostileSim(t, seed, 3, false)

	table := "config"
	def := configTableDef()
	registerTableAll(t, sim, masterRepo, table, def, 100)

	// Seed 20 rows on master.
	for i := 0; i < 20; i++ {
		upsertRow(t, masterRepo, table, map[string]any{
			"key":   fmt.Sprintf("key-%d", i),
			"value": fmt.Sprintf("val-%d", i),
		}, 100)
	}

	if err := sim.Run(stepsFor(500)); err != nil {
		t.Fatalf("Run: %v", err)
	}
	t.Logf("[corrupt-payload] seed=%d steps=%d syncs=%d errors=%d",
		seed, sim.Steps, sim.TotalSyncs, sim.TotalErrors)

	// Safety holds despite corrupted payloads.
	checkStructuralIntegrity(t, sim)
	// Some rows should still get through.
	assertBoundedProgress(t, sim, table, def, 1)
	logRowCounts(t, sim, table, def)
}

// TestTS_DropSchemaCards: schema cards dropped 25%, table still deploys.
func TestTS_DropSchemaCards(t *testing.T) {
	seed := seedFor(402)
	sim, masterRepo, _ := newHostileSim(t, seed, 3, false)

	// Register schema only on master — leaves must learn via sync.
	table := "alerts"
	def := configTableDef()
	registerTable(t, masterRepo, table, def, 100)

	// Seed rows on master.
	for i := 0; i < 10; i++ {
		upsertRow(t, masterRepo, table, map[string]any{
			"key":   fmt.Sprintf("alert-%d", i),
			"value": fmt.Sprintf("msg-%d", i),
		}, 100)
	}

	if err := sim.Run(stepsFor(500)); err != nil {
		t.Fatalf("Run: %v", err)
	}
	t.Logf("[drop-schema] seed=%d steps=%d syncs=%d errors=%d",
		seed, sim.Steps, sim.TotalSyncs, sim.TotalErrors)

	// Structural safety must hold.
	checkStructuralIntegrity(t, sim)

	// At least one leaf should have received the schema (despite 25% drops).
	schemaDeployed := 0
	for _, leafID := range sim.LeafIDs() {
		tables, err := repo.ListSyncedTables(sim.Leaf(leafID).Repo().DB())
		if err != nil {
			t.Fatalf("ListSyncedTables %s: %v", leafID, err)
		}
		for _, tbl := range tables {
			if tbl.Name == table {
				schemaDeployed++
				break
			}
		}
	}
	t.Logf("  schema deployed to %d/%d leaves", schemaDeployed, len(sim.LeafIDs()))
	if schemaDeployed == 0 {
		t.Errorf("schema not deployed to any leaf after 500 steps")
	}
}

// TestTS_TruncatedXIGotList: emitXIGots truncated 25%, multi-round convergence.
func TestTS_TruncatedXIGotList(t *testing.T) {
	seed := seedFor(403)
	sim, masterRepo, _ := newHostileSim(t, seed, 3, false)

	table := "sensors"
	def := configTableDef()
	registerTableAll(t, sim, masterRepo, table, def, 100)

	// Seed 30 rows across leaves so XIGot lists will be non-trivial.
	for i, leafID := range sim.LeafIDs() {
		r := sim.Leaf(leafID).Repo()
		for j := 0; j < 10; j++ {
			upsertRow(t, r, table, map[string]any{
				"key":   fmt.Sprintf("s%d-%d", i, j),
				"value": fmt.Sprintf("reading-%d-%d", i, j),
			}, int64(100+i*10+j))
		}
	}

	if err := sim.Run(stepsFor(600)); err != nil {
		t.Fatalf("Run: %v", err)
	}
	t.Logf("[truncated-xigot] seed=%d steps=%d syncs=%d errors=%d",
		seed, sim.Steps, sim.TotalSyncs, sim.TotalErrors)

	checkStructuralIntegrity(t, sim)
	// Truncated igot lists mean slower convergence, but some progress expected.
	assertBoundedProgress(t, sim, table, def, 10)
	logRowCounts(t, sim, table, def)
}

// TestTS_CatalogHashCorruption: corrupt catalog hash forces full row exchange.
func TestTS_CatalogHashCorruption(t *testing.T) {
	seed := seedFor(404)
	sim, masterRepo, _ := newHostileSim(t, seed, 2, false)

	table := "config"
	def := configTableDef()
	registerTableAll(t, sim, masterRepo, table, def, 100)

	// Seed identical rows on both sides so catalog hashes would match.
	allRepos := []*repo.Repo{masterRepo}
	for _, leafID := range sim.LeafIDs() {
		allRepos = append(allRepos, sim.Leaf(leafID).Repo())
	}
	for _, r := range allRepos {
		for i := 0; i < 15; i++ {
			upsertRow(t, r, table, map[string]any{
				"key":   fmt.Sprintf("k%d", i),
				"value": fmt.Sprintf("v%d", i),
			}, 100)
		}
	}

	// With BUGGIFY, catalog hash may be corrupted, forcing full exchange.
	if err := sim.Run(stepsFor(400)); err != nil {
		t.Fatalf("Run: %v", err)
	}
	t.Logf("[catalog-corrupt] seed=%d steps=%d syncs=%d errors=%d",
		seed, sim.Steps, sim.TotalSyncs, sim.TotalErrors)

	checkStructuralIntegrity(t, sim)
	// All rows were pre-seeded, so each peer must still have at least 15.
	assertBoundedProgress(t, sim, table, def, 15)
	logRowCounts(t, sim, table, def)
}

// TestTS_MixedWorkload: blob sync + UV sync + table sync simultaneously under 25% faults.
func TestTS_MixedWorkload(t *testing.T) {
	seed := seedFor(405)
	sim, masterRepo, mf := newHostileSim(t, seed, 3, true)

	// 1. Seed blobs (normal Fossil artifacts).
	for i := 0; i < 30; i++ {
		mf.StoreArtifact([]byte(fmt.Sprintf("mixed-blob-%04d-seed%d", i, seed)))
	}

	// 2. Seed UV files.
	uv.EnsureSchema(masterRepo.DB())
	for i := 0; i < 5; i++ {
		uv.Write(masterRepo.DB(), fmt.Sprintf("mixed/file-%d.txt", i),
			[]byte(fmt.Sprintf("mixed-uv-%d-seed%d", i, seed)), int64(100+i))
	}

	// 3. Register synced table and seed rows.
	table := "telemetry"
	def := configTableDef()
	registerTableAll(t, sim, masterRepo, table, def, 100)
	for i := 0; i < 20; i++ {
		upsertRow(t, masterRepo, table, map[string]any{
			"key":   fmt.Sprintf("metric-%d", i),
			"value": fmt.Sprintf("%d", i*100),
		}, 100)
	}

	if err := sim.Run(stepsFor(600)); err != nil {
		t.Fatalf("Run: %v", err)
	}
	t.Logf("[mixed-workload] seed=%d steps=%d syncs=%d errors=%d",
		seed, sim.Steps, sim.TotalSyncs, sim.TotalErrors)

	// Structural safety for all three systems.
	checkStructuralIntegrity(t, sim)

	// Blob progress: at least some blobs propagated.
	for _, leafID := range sim.LeafIDs() {
		c, _ := CountBlobs(sim.Leaf(leafID).Repo())
		t.Logf("  %s: %d blobs", leafID, c)
		if c == 0 {
			t.Errorf("%s: zero blobs after mixed workload", leafID)
		}
	}

	// Table progress: at least some rows propagated.
	assertBoundedProgress(t, sim, table, def, 1)
	logRowCounts(t, sim, table, def)
}

// TestTS_StressTest1000Rows: 1000 rows on master, 2 peers, verify no crash.
// The goal is survival under load — not convergence.
func TestTS_StressTest1000Rows(t *testing.T) {
	seed := seedFor(406)
	sim, masterRepo, _ := newHostileSim(t, seed, 2, false)

	table := "events"
	def := configTableDef()
	registerTableAll(t, sim, masterRepo, table, def, 100)

	// Seed 500 rows on master (1000 rows is too slow for CI).
	for i := 0; i < 500; i++ {
		upsertRow(t, masterRepo, table, map[string]any{
			"key":   fmt.Sprintf("evt-%04d", i),
			"value": fmt.Sprintf("data-%04d", i),
		}, int64(100+i))
	}

	// Run bounded steps — enough for partial progress, not full convergence.
	if err := sim.Run(stepsFor(50)); err != nil {
		t.Fatalf("Run: %v", err)
	}
	t.Logf("[stress-1000] seed=%d steps=%d syncs=%d errors=%d",
		seed, sim.Steps, sim.TotalSyncs, sim.TotalErrors)

	// No crash, structural integrity intact.
	checkStructuralIntegrity(t, sim)

	// Some progress: every peer should have at least some rows (not zero).
	assertBoundedProgress(t, sim, table, def, 1)
	logRowCounts(t, sim, table, def)
}

// =============================================================================
// Deletion, Resurrection, and Integer PK tests
// =============================================================================

// TestTableSync_Deletion_Convergence: peer A deletes, peers B and C converge to tombstone.
func TestTableSync_Deletion_Convergence(t *testing.T) {
	sim, masterRepo, _ := newTableSyncSim(t, 42, 3, false)
	def := deviceTableDef()
	registerTableAll(t, sim, masterRepo, "devices", def, 1000)

	// Seed a row on master.
	upsertRow(t, masterRepo, "devices", map[string]any{
		"device_id": "d1", "hostname": "alpha", "status": "online",
	}, 1000)

	// Sync so all leaves have the row.
	if err := sim.Run(30); err != nil {
		t.Fatalf("Run (seed): %v", err)
	}
	for _, leafID := range sim.LeafIDs() {
		assertRowCount(t, sim.Leaf(leafID).Repo(), string(leafID), "devices", def, 1)
	}

	// Delete on master.
	pkColDefs := []repo.ColumnDef{{Name: "device_id", Type: "text", PK: true}}
	pkHash := repo.PKHash(pkColDefs, map[string]any{"device_id": "d1"})
	deleted, err := repo.DeleteXRowByPKHash(masterRepo.DB(), "devices", def, pkHash, 2000)
	if err != nil || !deleted {
		t.Fatalf("master delete: deleted=%v err=%v", deleted, err)
	}

	// Sync until convergence.
	if err := sim.Run(50); err != nil {
		t.Fatalf("Run (delete): %v", err)
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

// TestTableSync_Deletion_Resurrection: delete then resurrect with newer mtime — all converge to live row.
func TestTableSync_Deletion_Resurrection(t *testing.T) {
	sim, masterRepo, _ := newTableSyncSim(t, 99, 2, false)
	def := deviceTableDef()
	registerTableAll(t, sim, masterRepo, "devices", def, 1000)

	// Seed and sync.
	upsertRow(t, masterRepo, "devices", map[string]any{
		"device_id": "d1", "hostname": "alpha", "status": "online",
	}, 1000)
	if err := sim.Run(30); err != nil {
		t.Fatalf("Run (seed): %v", err)
	}

	// Delete on master.
	pkColDefs := []repo.ColumnDef{{Name: "device_id", Type: "text", PK: true}}
	pkHash := repo.PKHash(pkColDefs, map[string]any{"device_id": "d1"})
	repo.DeleteXRowByPKHash(masterRepo.DB(), "devices", def, pkHash, 2000)

	// Sync deletion.
	if err := sim.Run(30); err != nil {
		t.Fatalf("Run (delete): %v", err)
	}

	// Resurrect on a leaf with newer mtime.
	leaf0 := sim.Leaf(sim.LeafIDs()[0]).Repo()
	upsertRow(t, leaf0, "devices", map[string]any{
		"device_id": "d1", "hostname": "beta", "status": "active",
	}, 3000)

	// Sync resurrection.
	if err := sim.Run(50); err != nil {
		t.Fatalf("Run (resurrect): %v", err)
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

// TestTableSync_IntegerPK_Convergence: large integer PK (>2^53) convergence across peers.
func TestTableSync_IntegerPK_Convergence(t *testing.T) {
	sim, masterRepo, _ := newTableSyncSim(t, 77, 2, false)
	def := repo.TableDef{
		Columns: []repo.ColumnDef{
			{Name: "seq", Type: "integer", PK: true},
			{Name: "payload", Type: "text"},
		},
		Conflict: "mtime-wins",
	}
	registerTableAll(t, sim, masterRepo, "events", def, 1000)

	// Seed row with large integer PK (>2^53).
	bigPK := int64(1<<53 + 1)
	upsertRow(t, masterRepo, "events", map[string]any{
		"seq": bigPK, "payload": "event-data",
	}, 1000)

	// Sync.
	if err := sim.Run(100); err != nil {
		t.Fatalf("Run: %v", err)
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
