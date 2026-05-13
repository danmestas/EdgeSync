package hub

import (
	"context"
	"testing"
	"time"

	libfossil "github.com/danmestas/libfossil"
)

func TestHub_FileToCommit_PermXMarksExecutable(t *testing.T) {
	h := newTestHub(t)
	ctx := context.Background()

	rev, err := h.Commit(ctx, CommitOpts{
		Files: []FileToCommit{
			{Name: "script.sh", Content: []byte("#!/bin/sh\necho hi\n"), Perm: "x"},
			{Name: "data.txt", Content: []byte("hi\n")},
		},
		Message: "perm round-trip",
		Author:  "hub",
	})
	if err != nil {
		t.Fatalf("Commit: %v", err)
	}

	r := h.Repo().handle
	rid, err := r.ResolveVersion(string(rev))
	if err != nil {
		t.Fatalf("ResolveVersion(%s): %v", rev, err)
	}

	entries, err := r.ListFiles(rid)
	if err != nil {
		t.Fatalf("ListFiles: %v", err)
	}

	got := map[string]string{}
	for _, e := range entries {
		got[e.Name] = e.Perm
	}
	if got["script.sh"] != "x" {
		t.Errorf("script.sh perm = %q, want %q", got["script.sh"], "x")
	}
	if got["data.txt"] != "" {
		t.Errorf("data.txt perm = %q, want empty (default)", got["data.txt"])
	}
}

func TestHub_CommitOpts_TimeIsHonoured(t *testing.T) {
	h := newTestHub(t)
	ctx := context.Background()

	want := time.Date(2024, 6, 15, 12, 30, 0, 0, time.UTC)

	rev, err := h.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "f.txt", Content: []byte("x")}},
		Message: "explicit time",
		Author:  "hub",
		Time:    want,
	})
	if err != nil {
		t.Fatalf("Commit: %v", err)
	}

	r := h.Repo().handle
	rid, err := r.ResolveVersion(string(rev))
	if err != nil {
		t.Fatalf("ResolveVersion: %v", err)
	}
	entries, err := r.Timeline(libfossil.LogOpts{Start: rid, Limit: 1})
	if err != nil {
		t.Fatalf("Timeline: %v", err)
	}
	if len(entries) == 0 {
		t.Fatal("Timeline returned no entries")
	}
	got := entries[0].Time.UTC()
	if !got.Equal(want) {
		t.Errorf("commit time = %s, want %s", got, want)
	}
}

func TestHub_CommitOpts_ZeroTimeDefaultsToNow(t *testing.T) {
	h := newTestHub(t)
	ctx := context.Background()

	before := time.Now().UTC().Add(-1 * time.Second)
	rev, err := h.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "f.txt", Content: []byte("x")}},
		Message: "default time",
		Author:  "hub",
	})
	if err != nil {
		t.Fatalf("Commit: %v", err)
	}
	after := time.Now().UTC().Add(1 * time.Second)

	r := h.Repo().handle
	rid, err := r.ResolveVersion(string(rev))
	if err != nil {
		t.Fatalf("ResolveVersion: %v", err)
	}
	entries, err := r.Timeline(libfossil.LogOpts{Start: rid, Limit: 1})
	if err != nil {
		t.Fatalf("Timeline: %v", err)
	}
	if len(entries) == 0 {
		t.Fatal("Timeline returned no entries")
	}
	got := entries[0].Time.UTC()
	if got.Before(before) || got.After(after) {
		t.Errorf("commit time = %s, want in [%s, %s]", got, before, after)
	}
}
