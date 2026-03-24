package verify_test

import (
	"testing"

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
