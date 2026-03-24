package sim

// Rebuild equivalence tests prove that verify.Rebuild produces the same
// derived tables as `fossil rebuild`. These are integration tests that
// require the fossil binary — they belong in sim/, not in unit tests.
//
// Methodology:
//   1. Create a rich repo (multiple checkins, branches, deltas)
//   2. Snapshot the clean derived table state via `fossil sql`
//   3. Corrupt derived tables (delete rows to force an effectual rebuild)
//   4. Copy the dirty repo to two files
//   5. Run verify.Rebuild on copy A (Go)
//   6. Run `fossil rebuild` on copy B (C)
//   7. Prove: dirty ≠ go-rebuilt (rebuild changed something)
//   8. Prove: go-rebuilt == fossil-rebuilt (functionally equivalent)

import (
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/deck"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	"github.com/dmestas/edgesync/go-libfossil/testutil"
	"github.com/dmestas/edgesync/go-libfossil/verify"
)

// tableSnapshot captures derived table state for comparison.
type tableSnapshot struct {
	eventRows   []string // "objid|type|user|comment" sorted
	plinkRows   []string // "pid|cid|isprim" sorted
	mlinkRows   []string // "mid|fid|fnid" sorted
	tagxrefRows []string // "tagid|tagtype|rid|value" sorted
	leafRows    []string // "rid" sorted
}

// snapshotDerived reads derived tables via `fossil sql` so both Go-rebuilt
// and fossil-rebuilt repos are queried identically (no driver differences).
func snapshotDerived(t *testing.T, repoPath string) tableSnapshot {
	t.Helper()
	tr := testutil.NewTestRepoFromPath(t, repoPath)
	return tableSnapshot{
		eventRows:   fossilQueryRows(t, tr, "SELECT objid, type, user, comment FROM event ORDER BY objid"),
		plinkRows:   fossilQueryRows(t, tr, "SELECT pid, cid, isprim FROM plink ORDER BY pid, cid"),
		mlinkRows:   fossilQueryRows(t, tr, "SELECT mid, fid, fnid FROM mlink ORDER BY mid, fid"),
		tagxrefRows: fossilQueryRows(t, tr, "SELECT tagid, tagtype, rid, value FROM tagxref ORDER BY tagid, rid"),
		leafRows:    fossilQueryRows(t, tr, "SELECT rid FROM leaf ORDER BY rid"),
	}
}

func fossilQueryRows(t *testing.T, tr *testutil.TestRepo, query string) []string {
	t.Helper()
	raw := tr.FossilSQL(t, query)
	if raw == "" {
		return nil
	}
	rows := strings.Split(raw, "\n")
	sort.Strings(rows)
	return rows
}

func snapshotsEqual(a, b tableSnapshot) bool {
	return slicesEqual(a.eventRows, b.eventRows) &&
		slicesEqual(a.plinkRows, b.plinkRows) &&
		slicesEqual(a.mlinkRows, b.mlinkRows) &&
		slicesEqual(a.tagxrefRows, b.tagxrefRows) &&
		slicesEqual(a.leafRows, b.leafRows)
}

func slicesEqual(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func diffSnapshots(t *testing.T, label string, a, b tableSnapshot) {
	t.Helper()
	for _, pair := range []struct {
		name string
		a, b []string
	}{
		{"event", a.eventRows, b.eventRows},
		{"plink", a.plinkRows, b.plinkRows},
		{"mlink", a.mlinkRows, b.mlinkRows},
		{"tagxref", a.tagxrefRows, b.tagxrefRows},
		{"leaf", a.leafRows, b.leafRows},
	} {
		if slicesEqual(pair.a, pair.b) {
			continue
		}
		t.Logf("%s.%s: count %d vs %d", label, pair.name, len(pair.a), len(pair.b))
		aSet := make(map[string]bool, len(pair.a))
		for _, r := range pair.a {
			aSet[r] = true
		}
		for _, r := range pair.b {
			if !aSet[r] {
				t.Logf("  %s.%s: only in fossil: %s", label, pair.name, r)
			}
		}
		bSet := make(map[string]bool, len(pair.b))
		for _, r := range pair.b {
			bSet[r] = true
		}
		for _, r := range pair.a {
			if !bSet[r] {
				t.Logf("  %s.%s: only in go: %s", label, pair.name, r)
			}
		}
	}
}

// createRichRepo builds a repo with 4 checkins exercising different manifest
// types: initial, full with new files, delta manifest, and branch with tags.
// Returns the closed repo path.
func createRichRepo(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	path := filepath.Join(dir, "rich.fossil")
	r, err := repo.Create(path, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatal(err)
	}

	// Checkin 1: initial (creates sym-trunk, branch=trunk inline T-cards)
	rid1, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files: []manifest.File{
			{Name: "README.md", Content: []byte("# Project\n")},
			{Name: "src/main.go", Content: []byte("package main\n\nfunc main() {}\n")},
		},
		Comment: "initial commit",
		User:    "alice",
	})
	if err != nil {
		t.Fatal(err)
	}

	// Checkin 2: full manifest, adds a file
	rid2, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files: []manifest.File{
			{Name: "README.md", Content: []byte("# Project\n\nUpdated.\n")},
			{Name: "src/main.go", Content: []byte("package main\n\nfunc main() {}\n")},
			{Name: "src/lib.go", Content: []byte("package main\n\nfunc helper() {}\n")},
		},
		Comment: "add lib.go",
		User:    "alice",
		Parent:  rid1,
	})
	if err != nil {
		t.Fatal(err)
	}

	// Checkin 3: delta manifest (B-card) — only README changed
	_, _, err = manifest.Checkin(r, manifest.CheckinOpts{
		Files: []manifest.File{
			{Name: "README.md", Content: []byte("# Project\n\nUpdated again.\n")},
			{Name: "src/main.go", Content: []byte("package main\n\nfunc main() {}\n")},
			{Name: "src/lib.go", Content: []byte("package main\n\nfunc helper() {}\n")},
		},
		Comment: "update readme (delta)",
		User:    "bob",
		Parent:  rid2,
		Delta:   true,
	})
	if err != nil {
		t.Fatal(err)
	}

	// Checkin 4: branch off rid2 — tests tag propagation
	_, _, err = manifest.Checkin(r, manifest.CheckinOpts{
		Files: []manifest.File{
			{Name: "README.md", Content: []byte("# Project\n\nUpdated.\n")},
			{Name: "src/main.go", Content: []byte("package main\n\nfunc main() {}\n")},
			{Name: "src/lib.go", Content: []byte("package main\n\nfunc helper() {}\n")},
			{Name: "feature.go", Content: []byte("package main\n\nfunc feature() {}\n")},
		},
		Comment: "feature branch",
		User:    "bob",
		Parent:  rid2,
		Tags: []deck.TagCard{
			{Type: deck.TagPropagating, Name: "branch", UUID: "*", Value: "feature-x"},
			{Type: deck.TagSingleton, Name: "sym-feature-x", UUID: "*"},
		},
	})
	if err != nil {
		t.Fatal(err)
	}

	r.Close()
	return path
}

// corruptDerivedTables injects targeted corruption into derived tables.
// Deletes some event rows, all plinks, all tagxref, all leaves, and
// some mlink rows — forcing rebuild to reconstruct real data.
func corruptDerivedTables(t *testing.T, repoPath string) {
	t.Helper()
	r, err := repo.Open(repoPath)
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()

	corruptSQL := []string{
		"DELETE FROM event WHERE rowid IN (SELECT rowid FROM event LIMIT 2)",
		"DELETE FROM plink",
		"DELETE FROM tagxref",
		"DELETE FROM leaf",
		"DELETE FROM mlink WHERE rowid IN (SELECT rowid FROM mlink LIMIT 3)",
	}
	for _, sql := range corruptSQL {
		if _, err := r.DB().Exec(sql); err != nil {
			t.Fatalf("corrupt: %v", err)
		}
	}
}

// copyFile copies src to dst (byte-level copy).
func copyFile(t *testing.T, src, dst string) {
	t.Helper()
	data, err := os.ReadFile(src)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(dst, data, 0o644); err != nil {
		t.Fatal(err)
	}
}

// TestRebuildEquivalence proves our Go rebuild produces the same derived
// tables as fossil rebuild. This is the core correctness proof for the
// verify.Rebuild implementation.
func TestRebuildEquivalence(t *testing.T) {
	requireFossil(t)
	if testing.Short() {
		t.Skip("skipping in short mode")
	}

	// Step 1: Create rich repo and snapshot clean state.
	repoPath := createRichRepo(t)
	cleanSnap := snapshotDerived(t, repoPath)
	if len(cleanSnap.eventRows) == 0 {
		t.Fatal("clean repo has no events — setup broken")
	}
	t.Logf("clean: %d events, %d plinks, %d mlinks, %d tagxrefs, %d leaves",
		len(cleanSnap.eventRows), len(cleanSnap.plinkRows), len(cleanSnap.mlinkRows),
		len(cleanSnap.tagxrefRows), len(cleanSnap.leafRows))

	// Step 2: Make two copies.
	goPath := filepath.Join(t.TempDir(), "go-rebuild.fossil")
	fossilPath := filepath.Join(t.TempDir(), "fossil-rebuild.fossil")
	copyFile(t, repoPath, goPath)
	copyFile(t, repoPath, fossilPath)

	// Step 3: Corrupt both copies identically.
	corruptDerivedTables(t, goPath)
	corruptDerivedTables(t, fossilPath)

	// Step 4: Prove dirty state differs from clean.
	dirtySnap := snapshotDerived(t, goPath)
	if snapshotsEqual(cleanSnap, dirtySnap) {
		t.Fatal("dirty == clean — corruption had no effect")
	}
	t.Logf("dirty: %d events, %d plinks, %d mlinks, %d tagxrefs, %d leaves",
		len(dirtySnap.eventRows), len(dirtySnap.plinkRows), len(dirtySnap.mlinkRows),
		len(dirtySnap.tagxrefRows), len(dirtySnap.leafRows))

	// Step 5: Go rebuild.
	goRepo, err := repo.Open(goPath)
	if err != nil {
		t.Fatal(err)
	}
	goReport, err := verify.Rebuild(goRepo)
	if err != nil {
		t.Fatal("Go rebuild:", err)
	}
	goRepo.Close()
	t.Logf("Go rebuild: %d blobs checked, %d tables rebuilt",
		goReport.BlobsChecked, len(goReport.TablesRebuilt))

	// Step 6: fossil rebuild.
	if err := testutil.FossilRebuild(fossilPath); err != nil {
		t.Fatal("fossil rebuild:", err)
	}

	// Step 7: Snapshot both rebuilt states.
	goSnap := snapshotDerived(t, goPath)
	fossilSnap := snapshotDerived(t, fossilPath)

	// Step 8: Prove rebuild was effectual (go-rebuilt ≠ dirty).
	if snapshotsEqual(goSnap, dirtySnap) {
		t.Fatal("Go rebuild == dirty — rebuild had no effect")
	}
	t.Log("PROVED: rebuild was effectual (go-rebuilt ≠ dirty)")

	// Step 9: Prove equivalence (go-rebuilt == fossil-rebuilt).
	if !snapshotsEqual(goSnap, fossilSnap) {
		t.Error("Go rebuild ≠ fossil rebuild")
		diffSnapshots(t, "equivalence", goSnap, fossilSnap)
		t.FailNow()
	}
	t.Log("PROVED: go-rebuilt == fossil-rebuilt (functionally equivalent)")
	t.Logf("rebuilt: %d events, %d plinks, %d mlinks, %d tagxrefs, %d leaves",
		len(goSnap.eventRows), len(goSnap.plinkRows), len(goSnap.mlinkRows),
		len(goSnap.tagxrefRows), len(goSnap.leafRows))
}

// TestRebuildEquivalence_TotalCorruption proves rebuild works when ALL
// derived tables are completely empty — maximum reconstruction.
func TestRebuildEquivalence_TotalCorruption(t *testing.T) {
	requireFossil(t)
	if testing.Short() {
		t.Skip("skipping in short mode")
	}

	repoPath := createRichRepo(t)

	goPath := filepath.Join(t.TempDir(), "go-total.fossil")
	fossilPath := filepath.Join(t.TempDir(), "fossil-total.fossil")
	copyFile(t, repoPath, goPath)
	copyFile(t, repoPath, fossilPath)

	// Nuke ALL derived tables completely.
	nukeSQL := []string{
		"DELETE FROM event",
		"DELETE FROM mlink",
		"DELETE FROM plink",
		"DELETE FROM tagxref",
		"DELETE FROM filename",
		"DELETE FROM leaf",
		"DELETE FROM unclustered",
		"DELETE FROM unsent",
	}
	for _, path := range []string{goPath, fossilPath} {
		r, err := repo.Open(path)
		if err != nil {
			t.Fatal(err)
		}
		for _, sql := range nukeSQL {
			r.DB().Exec(sql)
		}
		r.Close()
	}

	// Go rebuild.
	goRepo, err := repo.Open(goPath)
	if err != nil {
		t.Fatal(err)
	}
	_, err = verify.Rebuild(goRepo)
	if err != nil {
		t.Fatal("Go rebuild:", err)
	}
	goRepo.Close()

	// fossil rebuild.
	if err := testutil.FossilRebuild(fossilPath); err != nil {
		t.Fatal("fossil rebuild:", err)
	}

	goSnap := snapshotDerived(t, goPath)
	fossilSnap := snapshotDerived(t, fossilPath)

	if len(goSnap.eventRows) == 0 {
		t.Fatal("Go rebuild produced no events from total corruption")
	}

	if !snapshotsEqual(goSnap, fossilSnap) {
		t.Error("Total corruption: Go rebuild ≠ fossil rebuild")
		diffSnapshots(t, "total", goSnap, fossilSnap)
		t.FailNow()
	}
	t.Logf("PROVED: total corruption recovery equivalent (%d events, %d tagxrefs)",
		len(goSnap.eventRows), len(goSnap.tagxrefRows))
}

// TestRebuildEquivalence_VerifyAfterRebuild proves that after Go rebuild,
// verify.Verify reports zero issues.
func TestRebuildEquivalence_VerifyAfterRebuild(t *testing.T) {
	requireFossil(t)
	if testing.Short() {
		t.Skip("skipping in short mode")
	}

	repoPath := createRichRepo(t)
	goPath := filepath.Join(t.TempDir(), "go-verify.fossil")
	copyFile(t, repoPath, goPath)
	corruptDerivedTables(t, goPath)

	// Rebuild.
	goRepo, err := repo.Open(goPath)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := verify.Rebuild(goRepo); err != nil {
		t.Fatal(err)
	}

	// Verify must be clean.
	report, err := verify.Verify(goRepo)
	goRepo.Close()
	if err != nil {
		t.Fatal(err)
	}
	if !report.OK() {
		for _, iss := range report.Issues {
			t.Logf("issue: kind=%d table=%s %s", iss.Kind, iss.Table, iss.Message)
		}
		t.Fatalf("expected clean verify after rebuild, got %d issues", len(report.Issues))
	}
	t.Logf("PROVED: verify clean after rebuild (%d blobs checked, %d OK)",
		report.BlobsChecked, report.BlobsOK)
}

// Ensure libfossil import is used (needed for FslID in createRichRepo).
var _ libfossil.FslID
