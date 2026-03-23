package main

import (
	"database/sql"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"

	_ "github.com/dmestas/edgesync/go-libfossil/db/driver/modernc"
)

func TestRepoCoUpdatesCheckoutDB(t *testing.T) {
	tmp := t.TempDir()
	repoPath := filepath.Join(tmp, "test.fossil")

	// Create repo with two checkins.
	r, err := repo.Create(repoPath, "test-user", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("create repo: %v", err)
	}

	rid1, uuid1, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("alpha")}},
		Comment: "first",
		User:    "test-user",
		Time:    time.Date(2026, 1, 1, 0, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("checkin 1: %v", err)
	}

	rid2, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "b.txt", Content: []byte("bravo")}},
		Comment: "second",
		User:    "test-user",
		Parent:  rid1,
		Time:    time.Date(2026, 1, 2, 0, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("checkin 2: %v", err)
	}
	r.Close()

	// Open a checkout (creates .fslckout pointing at tip = rid2).
	coDir := filepath.Join(tmp, "checkout")
	if err := os.MkdirAll(coDir, 0o755); err != nil {
		t.Fatal(err)
	}

	g := &Globals{Repo: repoPath}
	openCmd := &RepoOpenCmd{Dir: coDir}
	if err := openCmd.Run(g); err != nil {
		t.Fatalf("repo open: %v", err)
	}

	// Verify checkout is at rid2 initially.
	ckout, err := openCheckout(coDir)
	if err != nil {
		t.Fatalf("open checkout: %v", err)
	}
	vid, err := checkoutVid(ckout)
	if err != nil {
		t.Fatalf("checkoutVid: %v", err)
	}
	if vid != int64(rid2) {
		t.Fatalf("expected initial vid=%d, got %d", rid2, vid)
	}
	ckout.Close()

	// Now checkout rid1 with Force=true.
	coCmd := &RepoCoCmd{Version: uuid1, Dir: coDir, Force: true}
	if err := coCmd.Run(g); err != nil {
		t.Fatalf("repo co: %v", err)
	}

	// Verify vvar.checkout was updated to rid1.
	ckout2, err := openCheckout(coDir)
	if err != nil {
		t.Fatalf("open checkout after co: %v", err)
	}
	defer ckout2.Close()

	vid2, err := checkoutVid(ckout2)
	if err != nil {
		t.Fatalf("checkoutVid after co: %v", err)
	}
	if vid2 != int64(rid1) {
		t.Errorf("vvar checkout: want %d, got %d", rid1, vid2)
	}

	// Verify checkout-hash updated.
	var hash string
	if err := ckout2.QueryRow("SELECT value FROM vvar WHERE name='checkout-hash'").Scan(&hash); err != nil {
		t.Fatalf("reading checkout-hash: %v", err)
	}
	if hash != uuid1 {
		t.Errorf("vvar checkout-hash: want %s, got %s", uuid1, hash)
	}

	// Verify vfile has only rid1's files (a.txt), not rid2's (b.txt).
	rows, err := ckout2.Query("SELECT pathname FROM vfile")
	if err != nil {
		t.Fatalf("query vfile: %v", err)
	}
	defer rows.Close()

	var files []string
	for rows.Next() {
		var name string
		if err := rows.Scan(&name); err != nil {
			t.Fatal(err)
		}
		files = append(files, name)
	}
	if len(files) != 1 || files[0] != "a.txt" {
		t.Errorf("vfile files: want [a.txt], got %v", files)
	}

	// Also verify the vid column is set correctly in vfile.
	var vfileVid int64
	if err := ckout2.QueryRow("SELECT vid FROM vfile WHERE pathname='a.txt'").Scan(&vfileVid); err != nil {
		t.Fatalf("query vfile vid: %v", err)
	}
	if vfileVid != int64(rid1) {
		t.Errorf("vfile vid: want %d, got %d", rid1, vfileVid)
	}

	_ = sql.ErrNoRows // ensure sql import used
}
