package sync_test

import (
	"context"
	"os"
	"path/filepath"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/hash"
	"github.com/dmestas/edgesync/go-libfossil/sync"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

// mockCloneTransport wraps a handler function for testing.
type mockCloneTransport struct {
	handler func(round int, req *xfer.Message) *xfer.Message
	round   int
}

func (m *mockCloneTransport) Exchange(ctx context.Context, req *xfer.Message) (*xfer.Message, error) {
	resp := m.handler(m.round, req)
	m.round++
	return resp, nil
}

// TestCloneBasic verifies a successful clone with 3 blobs in one round.
func TestCloneBasic(t *testing.T) {
	content1 := []byte("test content 1")
	content2 := []byte("test content 2")
	content3 := []byte("test content 3")
	uuid1 := hash.SHA1(content1)
	uuid2 := hash.SHA1(content2)
	uuid3 := hash.SHA1(content3)

	transport := &mockCloneTransport{
		handler: func(round int, req *xfer.Message) *xfer.Message {
			if round == 0 {
				// First round: push card + 3 file cards + clone_seqno 0
				return &xfer.Message{
					Cards: []xfer.Card{
						&xfer.PushCard{
							ServerCode:  "test-server-code",
							ProjectCode: "test-project-code",
						},
						&xfer.FileCard{UUID: uuid1, Content: content1},
						&xfer.FileCard{UUID: uuid2, Content: content2},
						&xfer.FileCard{UUID: uuid3, Content: content3},
						&xfer.CloneSeqNoCard{SeqNo: 0},
					},
				}
			}
			// Round 1+: empty response with seqno 0
			return &xfer.Message{
				Cards: []xfer.Card{
					&xfer.CloneSeqNoCard{SeqNo: 0},
				},
			}
		},
	}

	tmpDir := t.TempDir()
	repoPath := filepath.Join(tmpDir, "test.fossil")

	r, result, err := sync.Clone(context.Background(), repoPath, transport, sync.CloneOpts{})
	if err != nil {
		t.Fatalf("Clone failed: %v", err)
	}
	defer r.Close()

	// Verify result
	if result.BlobsRecvd != 3 {
		t.Errorf("BlobsRecvd = %d, want 3", result.BlobsRecvd)
	}
	if result.ProjectCode != "test-project-code" {
		t.Errorf("ProjectCode = %q, want %q", result.ProjectCode, "test-project-code")
	}
	if result.Rounds < 2 {
		t.Errorf("Rounds = %d, want >= 2 (min 2 rounds rule)", result.Rounds)
	}

	// Verify blobs exist
	if _, exists := blob.Exists(r.DB(), uuid1); !exists {
		t.Errorf("blob %s not found", uuid1)
	}
	if _, exists := blob.Exists(r.DB(), uuid2); !exists {
		t.Errorf("blob %s not found", uuid2)
	}
	if _, exists := blob.Exists(r.DB(), uuid3); !exists {
		t.Errorf("blob %s not found", uuid3)
	}
}

// TestCloneMultiRound verifies clone with multiple rounds.
func TestCloneMultiRound(t *testing.T) {
	content1 := []byte("test content 1")
	content2 := []byte("test content 2")
	content3 := []byte("test content 3")
	uuid1 := hash.SHA1(content1)
	uuid2 := hash.SHA1(content2)
	uuid3 := hash.SHA1(content3)

	transport := &mockCloneTransport{
		handler: func(round int, req *xfer.Message) *xfer.Message {
			if round == 0 {
				// Round 0: push card + 2 files + seqno 3 (more data coming)
				return &xfer.Message{
					Cards: []xfer.Card{
						&xfer.PushCard{
							ServerCode:  "server-abc",
							ProjectCode: "project-xyz",
						},
						&xfer.FileCard{UUID: uuid1, Content: content1},
						&xfer.FileCard{UUID: uuid2, Content: content2},
						&xfer.CloneSeqNoCard{SeqNo: 3},
					},
				}
			} else if round == 1 {
				// Round 1: final file + seqno 0
				return &xfer.Message{
					Cards: []xfer.Card{
						&xfer.FileCard{UUID: uuid3, Content: content3},
						&xfer.CloneSeqNoCard{SeqNo: 0},
					},
				}
			}
			// Round 2+: no files, seqno 0 (done signal)
			return &xfer.Message{
				Cards: []xfer.Card{
					&xfer.CloneSeqNoCard{SeqNo: 0},
				},
			}
		},
	}

	tmpDir := t.TempDir()
	repoPath := filepath.Join(tmpDir, "multi.fossil")

	r, result, err := sync.Clone(context.Background(), repoPath, transport, sync.CloneOpts{})
	if err != nil {
		t.Fatalf("Clone failed: %v", err)
	}
	defer r.Close()

	// Verify 3 total blobs
	if result.BlobsRecvd != 3 {
		t.Errorf("BlobsRecvd = %d, want 3", result.BlobsRecvd)
	}
	if result.Rounds < 2 {
		t.Errorf("Rounds = %d, want >= 2", result.Rounds)
	}

	// Verify all blobs exist
	if _, exists := blob.Exists(r.DB(), uuid1); !exists {
		t.Errorf("blob %s not found", uuid1)
	}
	if _, exists := blob.Exists(r.DB(), uuid2); !exists {
		t.Errorf("blob %s not found", uuid2)
	}
	if _, exists := blob.Exists(r.DB(), uuid3); !exists {
		t.Errorf("blob %s not found", uuid3)
	}
}

// TestCloneErrorCleansUp verifies that Clone cleans up on server error.
func TestCloneErrorCleansUp(t *testing.T) {
	transport := &mockCloneTransport{
		handler: func(round int, req *xfer.Message) *xfer.Message {
			return &xfer.Message{
				Cards: []xfer.Card{
					&xfer.ErrorCard{Message: "not authorized to clone"},
				},
			}
		},
	}

	tmpDir := t.TempDir()
	repoPath := filepath.Join(tmpDir, "error.fossil")

	r, _, err := sync.Clone(context.Background(), repoPath, transport, sync.CloneOpts{})
	if err == nil {
		r.Close()
		t.Fatal("Clone should have failed with error card")
	}

	// Verify repo file was deleted
	if _, statErr := os.Stat(repoPath); !os.IsNotExist(statErr) {
		t.Errorf("repo file should be deleted after error, but still exists")
	}
}

// TestCloneExistingPath verifies that Clone fails when path already exists.
func TestCloneExistingPath(t *testing.T) {
	tmpDir := t.TempDir()
	repoPath := filepath.Join(tmpDir, "existing.fossil")

	// Create a file at the target path
	if err := os.WriteFile(repoPath, []byte("existing file"), 0644); err != nil {
		t.Fatalf("failed to create existing file: %v", err)
	}

	handlerCalled := false
	transport := &mockCloneTransport{
		handler: func(round int, req *xfer.Message) *xfer.Message {
			handlerCalled = true
			return &xfer.Message{
				Cards: []xfer.Card{
					&xfer.PushCard{ProjectCode: "test"},
					&xfer.CloneSeqNoCard{SeqNo: 0},
				},
			}
		},
	}

	r, _, err := sync.Clone(context.Background(), repoPath, transport, sync.CloneOpts{})
	if err == nil {
		r.Close()
		t.Fatal("Clone should fail when path already exists")
	}
	if handlerCalled {
		t.Error("transport handler should not be called when path exists")
	}
}

// TestCloneCancelledContext verifies cleanup when context is cancelled.
func TestCloneCancelledContext(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())

	transport := &mockCloneTransport{
		handler: func(round int, req *xfer.Message) *xfer.Message {
			if round == 0 {
				return &xfer.Message{
					Cards: []xfer.Card{
						&xfer.PushCard{ProjectCode: "test-project"},
						&xfer.CloneSeqNoCard{SeqNo: 99}, // Pretend more data coming
					},
				}
			}
			// Cancel after round 0 completes
			cancel()
			return &xfer.Message{
				Cards: []xfer.Card{
					&xfer.CloneSeqNoCard{SeqNo: 99},
				},
			}
		},
	}

	tmpDir := t.TempDir()
	repoPath := filepath.Join(tmpDir, "cancelled.fossil")

	r, _, err := sync.Clone(ctx, repoPath, transport, sync.CloneOpts{})
	if err == nil {
		r.Close()
		t.Fatal("Clone should fail with cancelled context")
	}

	// Verify repo file was deleted
	if _, statErr := os.Stat(repoPath); !os.IsNotExist(statErr) {
		t.Errorf("repo file should be deleted after cancellation, but still exists")
	}
}
