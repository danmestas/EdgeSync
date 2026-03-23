package tag

import (
	"path/filepath"
	"testing"
	"time"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
)

func setupTestRepo(t *testing.T) *repo.Repo {
	t.Helper()
	path := filepath.Join(t.TempDir(), "test.fossil")
	r, err := repo.Create(path, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("repo.Create: %v", err)
	}
	t.Cleanup(func() { r.Close() })
	return r
}

func TestAddTag(t *testing.T) {
	r := setupTestRepo(t)

	// Create a checkin to tag
	rid, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "hello.txt", Content: []byte("hello")}},
		Comment: "initial commit",
		User:    "testuser",
		Time:    time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("Checkin: %v", err)
	}

	// Add a singleton tag
	tagRid, err := AddTag(r, TagOpts{
		TargetRID: rid,
		TagName:   "testlabel",
		TagType:   TagSingleton,
		Value:     "myvalue",
		User:      "testuser",
		Time:      time.Date(2024, 1, 15, 11, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("AddTag: %v", err)
	}
	if tagRid <= 0 {
		t.Fatalf("tagRid = %d, want > 0", tagRid)
	}

	// Verify tagxref has the correct entry
	var tagtype int
	var value string
	err = r.DB().QueryRow(
		`SELECT tagtype, value FROM tagxref
		 JOIN tag ON tag.tagid = tagxref.tagid
		 WHERE tag.tagname = ? AND tagxref.rid = ?`,
		"testlabel", rid,
	).Scan(&tagtype, &value)
	if err != nil {
		t.Fatalf("tagxref query: %v", err)
	}
	if tagtype != TagSingleton {
		t.Fatalf("tagtype = %d, want %d (singleton)", tagtype, TagSingleton)
	}
	if value != "myvalue" {
		t.Fatalf("value = %q, want %q", value, "myvalue")
	}
}

func TestCancelTag(t *testing.T) {
	r := setupTestRepo(t)

	// Create a checkin (auto-gets sym-trunk tag via propagation in manifest)
	rid, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "hello.txt", Content: []byte("hello")}},
		Comment: "initial commit",
		User:    "testuser",
		Time:    time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("Checkin: %v", err)
	}

	// Cancel the sym-trunk tag
	cancelRid, err := AddTag(r, TagOpts{
		TargetRID: rid,
		TagName:   "sym-trunk",
		TagType:   TagCancel,
		User:      "testuser",
		Time:      time.Date(2024, 1, 15, 11, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("AddTag cancel: %v", err)
	}
	if cancelRid <= 0 {
		t.Fatalf("cancelRid = %d, want > 0", cancelRid)
	}

	// Verify tagxref has tagtype=0 (cancel)
	var tagtype int
	err = r.DB().QueryRow(
		`SELECT tagtype FROM tagxref
		 JOIN tag ON tag.tagid = tagxref.tagid
		 WHERE tag.tagname = ? AND tagxref.rid = ?`,
		"sym-trunk", rid,
	).Scan(&tagtype)
	if err != nil {
		t.Fatalf("tagxref query: %v", err)
	}
	if tagtype != TagCancel {
		t.Fatalf("tagtype = %d, want %d (cancel)", tagtype, TagCancel)
	}
}

// makeCheckin is a test helper that creates a checkin with one file.
func makeCheckin(t *testing.T, r *repo.Repo, parent int64, name, content, comment string) int64 {
	t.Helper()
	rid, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: name, Content: []byte(content)}},
		Comment: comment,
		User:    "testuser",
		Parent:  libfossil.FslID(parent),
		Time:    time.Now().UTC(),
	})
	if err != nil {
		t.Fatalf("Checkin: %v", err)
	}
	return int64(rid)
}

func TestPropagateChain(t *testing.T) {
	r := setupTestRepo(t)

	// Create chain A→B→C
	ridA := makeCheckin(t, r, 0, "a.txt", "content A", "commit A")
	ridB := makeCheckin(t, r, ridA, "b.txt", "content B", "commit B")
	ridC := makeCheckin(t, r, ridB, "c.txt", "content C", "commit C")

	// Add propagating "branch" tag to A with value "feature"
	_, err := AddTag(r, TagOpts{
		TargetRID: libfossil.FslID(ridA),
		TagName:   "branch",
		TagType:   TagPropagating,
		Value:     "feature",
		User:      "testuser",
		Time:      time.Now().UTC(),
	})
	if err != nil {
		t.Fatalf("AddTag: %v", err)
	}

	// Verify B has the propagated tag (srcid=0, correct value)
	var srcidb, tagtypeB int
	var valueB string
	err = r.DB().QueryRow(`
		SELECT srcid, tagtype, value FROM tagxref
		JOIN tag ON tag.tagid = tagxref.tagid
		WHERE tag.tagname = 'branch' AND tagxref.rid = ?
	`, ridB).Scan(&srcidb, &tagtypeB, &valueB)
	if err != nil {
		t.Fatalf("tagxref query for B: %v", err)
	}
	if srcidb != 0 {
		t.Errorf("B srcid = %d, want 0 (propagated)", srcidb)
	}
	if tagtypeB != TagPropagating {
		t.Errorf("B tagtype = %d, want %d", tagtypeB, TagPropagating)
	}
	if valueB != "feature" {
		t.Errorf("B value = %q, want %q", valueB, "feature")
	}

	// Verify C has the propagated tag
	var srcidC, tagtypeC int
	var valueC string
	err = r.DB().QueryRow(`
		SELECT srcid, tagtype, value FROM tagxref
		JOIN tag ON tag.tagid = tagxref.tagid
		WHERE tag.tagname = 'branch' AND tagxref.rid = ?
	`, ridC).Scan(&srcidC, &tagtypeC, &valueC)
	if err != nil {
		t.Fatalf("tagxref query for C: %v", err)
	}
	if srcidC != 0 {
		t.Errorf("C srcid = %d, want 0 (propagated)", srcidC)
	}
	if tagtypeC != TagPropagating {
		t.Errorf("C tagtype = %d, want %d", tagtypeC, TagPropagating)
	}
	if valueC != "feature" {
		t.Errorf("C value = %q, want %q", valueC, "feature")
	}
}

func TestCancelPropagation(t *testing.T) {
	r := setupTestRepo(t)

	// Create chain A→B→C
	ridA := makeCheckin(t, r, 0, "a.txt", "content A", "commit A")
	ridB := makeCheckin(t, r, ridA, "b.txt", "content B", "commit B")
	ridC := makeCheckin(t, r, ridB, "c.txt", "content C", "commit C")

	// Add propagating tag to A
	_, err := AddTag(r, TagOpts{
		TargetRID: libfossil.FslID(ridA),
		TagName:   "testprop",
		TagType:   TagPropagating,
		Value:     "propvalue",
		User:      "testuser",
		Time:      time.Now().UTC(),
	})
	if err != nil {
		t.Fatalf("AddTag propagating: %v", err)
	}

	// Cancel at B
	_, err = AddTag(r, TagOpts{
		TargetRID: libfossil.FslID(ridB),
		TagName:   "testprop",
		TagType:   TagCancel,
		User:      "testuser",
		Time:      time.Now().UTC(),
	})
	if err != nil {
		t.Fatalf("AddTag cancel: %v", err)
	}

	// Verify B has no active tags (count of tagtype>0 should be 0)
	var countB int
	err = r.DB().QueryRow(`
		SELECT COUNT(*) FROM tagxref
		JOIN tag ON tag.tagid = tagxref.tagid
		WHERE tag.tagname = 'testprop' AND tagxref.rid = ? AND tagxref.tagtype > 0
	`, ridB).Scan(&countB)
	if err != nil {
		t.Fatalf("count query for B: %v", err)
	}
	if countB != 0 {
		t.Errorf("B has %d active tags, want 0", countB)
	}

	// Verify C has no tagxref row for this tag at all
	var countC int
	err = r.DB().QueryRow(`
		SELECT COUNT(*) FROM tagxref
		JOIN tag ON tag.tagid = tagxref.tagid
		WHERE tag.tagname = 'testprop' AND tagxref.rid = ?
	`, ridC).Scan(&countC)
	if err != nil {
		t.Fatalf("count query for C: %v", err)
	}
	if countC != 0 {
		t.Errorf("C has %d tagxref rows, want 0", countC)
	}
}

func TestPropagateBgcolor(t *testing.T) {
	r := setupTestRepo(t)

	// Create A→B
	ridA := makeCheckin(t, r, 0, "a.txt", "content A", "commit A")
	ridB := makeCheckin(t, r, ridA, "b.txt", "content B", "commit B")

	// Run crosslink to populate event table
	_, err := manifest.Crosslink(r)
	if err != nil {
		t.Fatalf("Crosslink: %v", err)
	}

	// Add propagating "bgcolor" tag to A
	_, err = AddTag(r, TagOpts{
		TargetRID: libfossil.FslID(ridA),
		TagName:   "bgcolor",
		TagType:   TagPropagating,
		Value:     "#ff0000",
		User:      "testuser",
		Time:      time.Now().UTC(),
	})
	if err != nil {
		t.Fatalf("AddTag bgcolor: %v", err)
	}

	// Verify event.bgcolor updated at B
	var bgcolor string
	err = r.DB().QueryRow("SELECT bgcolor FROM event WHERE objid=?", ridB).Scan(&bgcolor)
	if err != nil {
		t.Fatalf("event query for B: %v", err)
	}
	if bgcolor != "#ff0000" {
		t.Errorf("B bgcolor = %q, want %q", bgcolor, "#ff0000")
	}
}

func TestApplyTag(t *testing.T) {
	r := setupTestRepo(t)

	ridA := makeCheckin(t, r, 0, "a.txt", "aaa", "commit A")
	ridB := makeCheckin(t, r, ridA, "a.txt", "bbb", "commit B")

	err := ApplyTag(r, ApplyOpts{
		TargetRID: libfossil.FslID(ridA),
		SrcRID:    999,
		TagName:   "sym-trunk",
		TagType:   TagPropagating,
		Value:     "",
		MTime:     libfossil.TimeToJulian(time.Date(2024, 1, 15, 12, 0, 0, 0, time.UTC)),
	})
	if err != nil {
		t.Fatalf("ApplyTag: %v", err)
	}

	// Verify tagxref at A has srcid=999.
	var srcid int64
	r.DB().QueryRow(
		"SELECT srcid FROM tagxref JOIN tag USING(tagid) WHERE tagname='sym-trunk' AND rid=?", ridA,
	).Scan(&srcid)
	if srcid != 999 {
		t.Errorf("A srcid=%d, want 999", srcid)
	}

	// Verify propagated to B with srcid=0.
	r.DB().QueryRow(
		"SELECT srcid FROM tagxref JOIN tag USING(tagid) WHERE tagname='sym-trunk' AND rid=?", ridB,
	).Scan(&srcid)
	if srcid != 0 {
		t.Errorf("B srcid=%d, want 0 (propagated)", srcid)
	}
}
