package verify_test

import (
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
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
