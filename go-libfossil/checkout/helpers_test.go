package checkout

import (
	"testing"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	_ "github.com/dmestas/edgesync/go-libfossil/internal/testdriver"
)

func newTestRepoWithCheckin(t *testing.T) (*repo.Repo, func()) {
	t.Helper()
	dir := t.TempDir()
	path := dir + "/test.fossil"
	r, err := repo.CreateWithEnv(path, "test", simio.RealEnv())
	if err != nil {
		t.Fatal(err)
	}
	_, _, err = manifest.Checkin(r, manifest.CheckinOpts{
		Files: []manifest.File{
			{Name: "hello.txt", Content: []byte("hello world\n")},
			{Name: "src/main.go", Content: []byte("package main\n")},
			{Name: "README.md", Content: []byte("# Test\n")},
		},
		Comment: "initial checkin",
		User:    "test",
		Parent:  0,
		Time:    time.Date(2026, 1, 1, 0, 0, 0, 0, time.UTC),
	})
	if err != nil {
		r.Close()
		t.Fatal(err)
	}
	return r, func() { r.Close() }
}
