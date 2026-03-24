package verify_test

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/deck"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	"github.com/dmestas/edgesync/go-libfossil/testutil"
	"github.com/dmestas/edgesync/go-libfossil/verify"
)

func newTestRepo(t *testing.T) *repo.Repo {
	t.Helper()
	dir := t.TempDir()
	r, err := repo.Create(dir+"/test.fossil", "test", simio.CryptoRand{})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { r.Close() })
	return r
}

func TestVerify_EmptyRepo(t *testing.T) {
	r := newTestRepo(t)
	report, err := verify.Verify(r)
	if err != nil {
		t.Fatal(err)
	}
	if !report.OK() {
		t.Fatalf("expected clean verify on empty repo, got %d issues", len(report.Issues))
	}
	if report.BlobsChecked != 0 {
		t.Fatalf("expected 0 blobs checked on empty repo, got %d", report.BlobsChecked)
	}
}

func TestVerify_CleanRepo(t *testing.T) {
	r := newTestRepo(t)
	_, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "hello.txt", Content: []byte("hello world")}},
		Comment: "initial",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}
	report, err := verify.Verify(r)
	if err != nil {
		t.Fatal(err)
	}
	if !report.OK() {
		for _, iss := range report.Issues {
			t.Logf("issue: %s", iss.Message)
		}
		t.Fatalf("expected clean verify, got %d issues", len(report.Issues))
	}
	if report.BlobsChecked == 0 {
		t.Fatal("expected blobs to be checked")
	}
	if report.BlobsOK != report.BlobsChecked {
		t.Fatalf("expected all blobs OK, got %d/%d", report.BlobsOK, report.BlobsChecked)
	}
}

func TestVerify_DetectsHashMismatch(t *testing.T) {
	r := newTestRepo(t)
	_, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("good content")}},
		Comment: "initial",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}
	// Corrupt a file blob
	_, err = r.DB().Exec(`UPDATE blob SET content = X'0000000000' WHERE rid = (SELECT MAX(rid) FROM blob WHERE size >= 0)`)
	if err != nil {
		t.Fatal(err)
	}
	report, err := verify.Verify(r)
	if err != nil {
		t.Fatal(err)
	}
	if report.OK() {
		t.Fatal("expected issues after corruption")
	}
	if report.BlobsFailed == 0 {
		t.Fatal("expected at least one failed blob")
	}
	found := false
	for _, iss := range report.Issues {
		if iss.Kind == verify.IssueHashMismatch || iss.Kind == verify.IssueBlobCorrupt {
			found = true
			break
		}
	}
	if !found {
		t.Fatal("expected IssueHashMismatch or IssueBlobCorrupt")
	}
}

func TestVerify_ReportsAll(t *testing.T) {
	r := newTestRepo(t)
	rid1, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("alpha")}},
		Comment: "first",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}
	_, _, err = manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("alpha")}, {Name: "b.txt", Content: []byte("bravo")}},
		Comment: "second",
		User:    "test",
		Parent:  rid1,
	})
	if err != nil {
		t.Fatal(err)
	}
	// Corrupt ALL non-phantom blobs
	_, err = r.DB().Exec("UPDATE blob SET content = X'0000' WHERE size >= 0")
	if err != nil {
		t.Fatal(err)
	}
	report, err := verify.Verify(r)
	if err != nil {
		t.Fatal(err)
	}
	if len(report.Issues) < 2 {
		t.Fatalf("expected multiple issues (report-all), got %d", len(report.Issues))
	}
}

func TestVerify_DetectsDanglingDelta(t *testing.T) {
	r := newTestRepo(t)
	_, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("content")}},
		Comment: "initial",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}
	// Insert a delta row pointing to nonexistent blobs
	_, err = r.DB().Exec("INSERT INTO delta(rid, srcid) VALUES(999999, 888888)")
	if err != nil {
		t.Fatal(err)
	}
	report, err := verify.Verify(r)
	if err != nil {
		t.Fatal(err)
	}
	found := false
	for _, iss := range report.Issues {
		if iss.Kind == verify.IssueDeltaDangling {
			found = true
			break
		}
	}
	if !found {
		t.Fatal("expected IssueDeltaDangling")
	}
}

func TestVerify_DetectsOrphanPhantom(t *testing.T) {
	r := newTestRepo(t)
	// Insert a phantom row with no corresponding blob
	_, err := r.DB().Exec("INSERT INTO phantom(rid) VALUES(999999)")
	if err != nil {
		t.Fatal(err)
	}
	report, err := verify.Verify(r)
	if err != nil {
		t.Fatal(err)
	}
	found := false
	for _, iss := range report.Issues {
		if iss.Kind == verify.IssuePhantomOrphan {
			found = true
			break
		}
	}
	if !found {
		t.Fatal("expected IssuePhantomOrphan")
	}
}

func TestVerify_DetectsMissingEvent(t *testing.T) {
	r := newTestRepo(t)
	_, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("content")}},
		Comment: "initial",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}
	_, err = r.DB().Exec("DELETE FROM event")
	if err != nil {
		t.Fatal(err)
	}
	report, err := verify.Verify(r)
	if err != nil {
		t.Fatal(err)
	}
	found := false
	for _, iss := range report.Issues {
		if iss.Kind == verify.IssueEventMissing {
			found = true
			break
		}
	}
	if !found {
		for _, iss := range report.Issues {
			t.Logf("issue: kind=%d %s", iss.Kind, iss.Message)
		}
		t.Fatal("expected IssueEventMissing")
	}
}

func TestVerify_DetectsMissingPlink(t *testing.T) {
	r := newTestRepo(t)
	rid1, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("alpha")}},
		Comment: "first",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}
	_, _, err = manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("alpha")}, {Name: "b.txt", Content: []byte("bravo")}},
		Comment: "second",
		User:    "test",
		Parent:  rid1,
	})
	if err != nil {
		t.Fatal(err)
	}
	_, err = r.DB().Exec("DELETE FROM plink")
	if err != nil {
		t.Fatal(err)
	}
	report, err := verify.Verify(r)
	if err != nil {
		t.Fatal(err)
	}
	found := false
	for _, iss := range report.Issues {
		if iss.Kind == verify.IssuePlinkMissing {
			found = true
			break
		}
	}
	if !found {
		for _, iss := range report.Issues {
			t.Logf("issue: kind=%d %s", iss.Kind, iss.Message)
		}
		t.Fatal("expected IssuePlinkMissing")
	}
}

func TestRebuild_ReconstructsFromScratch(t *testing.T) {
	r := newTestRepo(t)
	rid1, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("alpha")}},
		Comment: "first",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}
	_, _, err = manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("alpha")}, {Name: "b.txt", Content: []byte("bravo")}},
		Comment: "second",
		User:    "test",
		Parent:  rid1,
	})
	if err != nil {
		t.Fatal(err)
	}

	// Snapshot counts before
	var eventCount, plinkCount int
	r.DB().QueryRow("SELECT count(*) FROM event").Scan(&eventCount)
	r.DB().QueryRow("SELECT count(*) FROM plink").Scan(&plinkCount)

	// Delete all derived tables
	for _, tbl := range []string{"event", "mlink", "plink", "tagxref", "filename", "leaf", "unclustered", "unsent"} {
		r.DB().Exec("DELETE FROM " + tbl)
	}

	report, err := verify.Rebuild(r)
	if err != nil {
		t.Fatal(err)
	}
	if len(report.TablesRebuilt) == 0 {
		t.Fatal("expected TablesRebuilt")
	}

	var newEventCount, newPlinkCount int
	r.DB().QueryRow("SELECT count(*) FROM event").Scan(&newEventCount)
	r.DB().QueryRow("SELECT count(*) FROM plink").Scan(&newPlinkCount)
	if newEventCount != eventCount {
		t.Fatalf("event: want %d got %d", eventCount, newEventCount)
	}
	if newPlinkCount != plinkCount {
		t.Fatalf("plink: want %d got %d", plinkCount, newPlinkCount)
	}
}

func TestRebuild_Idempotent(t *testing.T) {
	r := newTestRepo(t)
	_, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("hello")}},
		Comment: "initial",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}

	report1, err := verify.Rebuild(r)
	if err != nil {
		t.Fatal(err)
	}
	report2, err := verify.Rebuild(r)
	if err != nil {
		t.Fatal(err)
	}
	if report1.BlobsChecked != report2.BlobsChecked {
		t.Fatalf("blobs: %d vs %d", report1.BlobsChecked, report2.BlobsChecked)
	}
}

func TestRebuild_ReconstructsTags(t *testing.T) {
	r := newTestRepo(t)
	_, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("content")}},
		Comment: "initial on trunk",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}

	var tagxrefCount int
	r.DB().QueryRow("SELECT count(*) FROM tagxref").Scan(&tagxrefCount)
	if tagxrefCount == 0 {
		t.Fatal("expected tagxref rows after checkin with trunk tags")
	}

	for _, tbl := range []string{"event", "mlink", "plink", "tagxref", "filename", "leaf", "unclustered", "unsent"} {
		r.DB().Exec("DELETE FROM " + tbl)
	}

	report, err := verify.Rebuild(r)
	if err != nil {
		t.Fatal(err)
	}
	_ = report

	var newTagxrefCount int
	r.DB().QueryRow("SELECT count(*) FROM tagxref").Scan(&newTagxrefCount)
	if newTagxrefCount == 0 {
		t.Fatal("expected tagxref rows after rebuild")
	}

	// Verify repo is clean
	vReport, err := verify.Verify(r)
	if err != nil {
		t.Fatal(err)
	}
	if !vReport.OK() {
		for _, iss := range vReport.Issues {
			t.Logf("issue: %s", iss.Message)
		}
		t.Fatalf("expected clean verify after rebuild, got %d issues", len(vReport.Issues))
	}
}

func TestVerify_DetectsIncorrectLeaf(t *testing.T) {
	r := newTestRepo(t)
	_, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("content")}},
		Comment: "initial",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}
	_, err = r.DB().Exec("DELETE FROM leaf")
	if err != nil {
		t.Fatal(err)
	}
	report, err := verify.Verify(r)
	if err != nil {
		t.Fatal(err)
	}
	found := false
	for _, iss := range report.Issues {
		if iss.Kind == verify.IssueLeafIncorrect {
			found = true
			break
		}
	}
	if !found {
		for _, iss := range report.Issues {
			t.Logf("issue: kind=%d %s", iss.Kind, iss.Message)
		}
		t.Fatal("expected IssueLeafIncorrect")
	}
}

func TestRebuild_BuggifyResilience(t *testing.T) {
	r := newTestRepo(t)

	// Create repo BEFORE enabling BUGGIFY
	var files []manifest.File
	for i := 0; i < 20; i++ {
		files = append(files, manifest.File{
			Name:    fmt.Sprintf("file%d.txt", i),
			Content: []byte(fmt.Sprintf("content %d", i)),
		})
	}
	_, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   files,
		Comment: "buggify test",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}

	// Enable BUGGIFY after checkin
	simio.EnableBuggify(42)
	defer simio.DisableBuggify()

	report, err := verify.Rebuild(r)
	if err != nil {
		// Rebuild may error if manifest blob is corrupted by BUGGIFY — acceptable
		t.Logf("Rebuild under BUGGIFY returned error (expected): %v", err)
		return
	}

	if len(report.TablesRebuilt) == 0 {
		t.Fatal("expected TablesRebuilt after successful rebuild")
	}
	t.Logf("BUGGIFY rebuild: %d blobs checked, %d failed, %d skipped",
		report.BlobsChecked, report.BlobsFailed, report.BlobsSkipped)
}

func TestVerify_AfterRebuild_IsClean(t *testing.T) {
	r := newTestRepo(t)

	rid1, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("alpha")}},
		Comment: "first",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}
	_, _, err = manifest.Checkin(r, manifest.CheckinOpts{
		Files: []manifest.File{
			{Name: "a.txt", Content: []byte("alpha modified")},
			{Name: "b.txt", Content: []byte("bravo")},
		},
		Comment: "second",
		User:    "test",
		Parent:  rid1,
	})
	if err != nil {
		t.Fatal(err)
	}

	// Rebuild from scratch
	_, err = verify.Rebuild(r)
	if err != nil {
		t.Fatal(err)
	}

	// Verify should be completely clean
	report, err := verify.Verify(r)
	if err != nil {
		t.Fatal(err)
	}
	if !report.OK() {
		for _, iss := range report.Issues {
			t.Logf("issue: kind=%d table=%s %s", iss.Kind, iss.Table, iss.Message)
		}
		t.Fatalf("expected clean verify after rebuild, got %d issues", len(report.Issues))
	}
}

// TestRebuild_DeltaManifest verifies that rebuild correctly handles delta
// manifests (B-card). The delta's F-cards only contain changed files.
// Rebuild should create mlink rows for changed files only — matching
// fossil rebuild behavior (not expanding to the full file set).
func TestRebuild_DeltaManifest(t *testing.T) {
	r := newTestRepo(t)

	// First checkin: two files
	rid1, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files: []manifest.File{
			{Name: "a.txt", Content: []byte("alpha")},
			{Name: "b.txt", Content: []byte("bravo")},
		},
		Comment: "first",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}

	// Second checkin: delta manifest (only a.txt changes, b.txt inherited)
	_, _, err = manifest.Checkin(r, manifest.CheckinOpts{
		Files: []manifest.File{
			{Name: "a.txt", Content: []byte("alpha modified")},
			{Name: "b.txt", Content: []byte("bravo")},
		},
		Comment: "second (delta)",
		User:    "test",
		Parent:  rid1,
		Delta:   true,
	})
	if err != nil {
		t.Fatal(err)
	}

	// Delete all derived tables
	for _, tbl := range []string{"event", "mlink", "plink", "tagxref", "filename", "leaf", "unclustered", "unsent"} {
		r.DB().Exec("DELETE FROM " + tbl)
	}

	// Rebuild
	_, err = verify.Rebuild(r)
	if err != nil {
		t.Fatal(err)
	}

	// After rebuild, mlink should exist and repo should be clean
	var mlinkCount int
	r.DB().QueryRow("SELECT count(*) FROM mlink").Scan(&mlinkCount)
	if mlinkCount == 0 {
		t.Fatal("expected mlink rows after rebuild with delta manifest")
	}

	// Verify clean
	vReport, err := verify.Verify(r)
	if err != nil {
		t.Fatal(err)
	}
	if !vReport.OK() {
		for _, iss := range vReport.Issues {
			t.Logf("issue: %s", iss.Message)
		}
		t.Fatalf("expected clean verify after delta rebuild, got %d issues", len(vReport.Issues))
	}
}

// ---------- Rebuild Equivalence Tests ----------
//
// These tests prove that verify.Rebuild produces the same derived tables
// as fossil rebuild. Methodology:
//   1. Create a rich repo (multiple checkins, branches, deltas)
//   2. Snapshot the clean derived table state
//   3. Corrupt derived tables (delete rows, add garbage)
//   4. Prove the dirty state differs from clean (effectual)
//   5. Copy the dirty repo to two files
//   6. Run verify.Rebuild on copy A
//   7. Run fossil rebuild on copy B
//   8. Prove: dirty ≠ go-rebuilt (our rebuild changed something)
//   9. Prove: go-rebuilt == fossil-rebuilt (functionally equivalent)

// tableSnapshot captures the state of derived tables for comparison.
type tableSnapshot struct {
	eventRows   []string // "objid|type|user|comment" sorted
	plinkRows   []string // "pid|cid|isprim" sorted
	mlinkRows   []string // "mid|fid|fnid" sorted
	tagxrefRows []string // "tagid|tagtype|rid|value" sorted
	leafRows    []string // "rid" sorted
}

// snapshotDerived reads derived tables from a repo path using fossil sql.
// Uses the fossil binary so both Go-rebuilt and fossil-rebuilt repos
// are queried the same way (no driver differences).
func snapshotDerived(t *testing.T, repoPath string) tableSnapshot {
	t.Helper()
	tr := testutil.NewTestRepoFromPath(t, repoPath)

	snap := tableSnapshot{
		eventRows:   fossilQuery(t, tr, "SELECT objid, type, user, comment FROM event ORDER BY objid"),
		plinkRows:   fossilQuery(t, tr, "SELECT pid, cid, isprim FROM plink ORDER BY pid, cid"),
		mlinkRows:   fossilQuery(t, tr, "SELECT mid, fid, fnid FROM mlink ORDER BY mid, fid"),
		tagxrefRows: fossilQuery(t, tr, "SELECT tagid, tagtype, rid, value FROM tagxref ORDER BY tagid, rid"),
		leafRows:    fossilQuery(t, tr, "SELECT rid FROM leaf ORDER BY rid"),
	}
	return snap
}

// fossilQuery runs a SQL query via `fossil sql` and returns sorted rows.
func fossilQuery(t *testing.T, tr *testutil.TestRepo, query string) []string {
	t.Helper()
	raw := tr.FossilSQL(t, query)
	if raw == "" {
		return nil
	}
	rows := strings.Split(raw, "\n")
	sort.Strings(rows)
	return rows
}

// snapshotsEqual compares two table snapshots.
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

// diffSnapshots returns a human-readable diff between two snapshots.
func diffSnapshots(t *testing.T, label string, a, b tableSnapshot) {
	t.Helper()
	diffSlices(t, label+".event", a.eventRows, b.eventRows)
	diffSlices(t, label+".plink", a.plinkRows, b.plinkRows)
	diffSlices(t, label+".mlink", a.mlinkRows, b.mlinkRows)
	diffSlices(t, label+".tagxref", a.tagxrefRows, b.tagxrefRows)
	diffSlices(t, label+".leaf", a.leafRows, b.leafRows)
}

func diffSlices(t *testing.T, label string, a, b []string) {
	t.Helper()
	if slicesEqual(a, b) {
		return
	}
	t.Logf("%s: count %d vs %d", label, len(a), len(b))
	aSet := make(map[string]bool, len(a))
	for _, r := range a {
		aSet[r] = true
	}
	for _, r := range b {
		if !aSet[r] {
			t.Logf("  %s: only in B: %s", label, r)
		}
	}
	bSet := make(map[string]bool, len(b))
	for _, r := range b {
		bSet[r] = true
	}
	for _, r := range a {
		if !bSet[r] {
			t.Logf("  %s: only in A: %s", label, r)
		}
	}
}

// TestRebuildEquivalence_FossilComparison creates a rich repo, corrupts it,
// then proves our rebuild produces the same outcome as fossil rebuild.
func TestRebuildEquivalence_FossilComparison(t *testing.T) {
	if !testutil.HasFossil() {
		t.Skip("fossil binary not found — equivalence test requires fossil")
	}

	// Step 1: Create a rich repo with multiple checkins, a branch, and a delta.
	r := newTestRepo(t)

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

	// Delta manifest — tests B-card expansion
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

	// Branch — tests tag propagation and control artifacts
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

	// Close so we can copy the file.
	repoPath := r.Path()
	r.Close()

	// Step 2: Snapshot clean state.
	cleanSnap := snapshotDerived(t, repoPath)
	if len(cleanSnap.eventRows) == 0 {
		t.Fatal("clean repo has no events — test setup broken")
	}
	t.Logf("clean state: %d events, %d plinks, %d mlinks, %d tagxrefs, %d leaves",
		len(cleanSnap.eventRows), len(cleanSnap.plinkRows), len(cleanSnap.mlinkRows),
		len(cleanSnap.tagxrefRows), len(cleanSnap.leafRows))

	// Step 3: Make two copies of the repo file.
	repoData, err := os.ReadFile(repoPath)
	if err != nil {
		t.Fatal(err)
	}
	goRepoPath := filepath.Join(t.TempDir(), "go-rebuild.fossil")
	fossilRepoPath := filepath.Join(t.TempDir(), "fossil-rebuild.fossil")
	if err := os.WriteFile(goRepoPath, repoData, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(fossilRepoPath, repoData, 0o644); err != nil {
		t.Fatal(err)
	}

	// Step 4: Corrupt derived tables in BOTH copies identically.
	// Delete some event rows, all tagxref, mess up leaf table.
	corruptSQL := []string{
		"DELETE FROM event WHERE rowid IN (SELECT rowid FROM event LIMIT 2)",
		"DELETE FROM plink",
		"DELETE FROM tagxref",
		"DELETE FROM leaf",
		"DELETE FROM mlink WHERE rowid IN (SELECT rowid FROM mlink LIMIT 3)",
	}
	for _, path := range []string{goRepoPath, fossilRepoPath} {
		rr, err := repo.Open(path)
		if err != nil {
			t.Fatal(err)
		}
		for _, sql := range corruptSQL {
			if _, err := rr.DB().Exec(sql); err != nil {
				t.Fatalf("corrupt %s: %v", path, err)
			}
		}
		rr.Close()
	}

	// Step 5: Snapshot dirty state — prove it's different from clean.
	dirtySnap := snapshotDerived(t, goRepoPath)
	if snapshotsEqual(cleanSnap, dirtySnap) {
		t.Fatal("dirty snapshot equals clean — corruption had no effect")
	}
	t.Logf("dirty state: %d events, %d plinks, %d mlinks, %d tagxrefs, %d leaves",
		len(dirtySnap.eventRows), len(dirtySnap.plinkRows), len(dirtySnap.mlinkRows),
		len(dirtySnap.tagxrefRows), len(dirtySnap.leafRows))

	// Step 6: Run verify.Rebuild on Go copy.
	goRepo, err := repo.Open(goRepoPath)
	if err != nil {
		t.Fatal(err)
	}
	goReport, err := verify.Rebuild(goRepo)
	if err != nil {
		t.Fatal("Go rebuild failed:", err)
	}
	goRepo.Close()
	t.Logf("Go rebuild: %d blobs checked, %d tables rebuilt", goReport.BlobsChecked, len(goReport.TablesRebuilt))

	// Step 7: Run fossil rebuild on fossil copy.
	if err := testutil.FossilRebuild(fossilRepoPath); err != nil {
		t.Fatal("fossil rebuild failed:", err)
	}

	// Step 8: Snapshot both rebuilt states.
	goSnap := snapshotDerived(t, goRepoPath)
	fossilSnap := snapshotDerived(t, fossilRepoPath)

	// Step 9: Prove go-rebuilt ≠ dirty (rebuild was effectual).
	if snapshotsEqual(goSnap, dirtySnap) {
		t.Fatal("Go rebuild snapshot equals dirty — rebuild had no effect")
	}
	t.Log("PASS: Go rebuild changed the derived tables (effectual)")

	// Step 10: Prove go-rebuilt == fossil-rebuilt (equivalent).
	if !snapshotsEqual(goSnap, fossilSnap) {
		t.Error("FAIL: Go rebuild ≠ fossil rebuild")
		diffSnapshots(t, "go-vs-fossil", goSnap, fossilSnap)
		t.FailNow()
	}
	t.Log("PASS: Go rebuild == fossil rebuild (functionally equivalent)")
}
