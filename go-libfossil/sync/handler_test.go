package sync

import (
	"context"
	"fmt"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/hash"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

// findCards returns all cards of type T from a message.
func findCards[T xfer.Card](msg *xfer.Message) []T {
	var out []T
	for _, c := range msg.Cards {
		if tc, ok := c.(T); ok {
			out = append(out, tc)
		}
	}
	return out
}

// storeTestBlob stores a blob and returns its UUID.
func storeTestBlob(t *testing.T, r *repo.Repo, data []byte) string {
	t.Helper()
	uuid := hash.SHA1(data)
	if err := storeReceivedFile(r, uuid, "", data); err != nil {
		t.Fatalf("storeReceivedFile: %v", err)
	}
	return uuid
}


func TestHandlePull(t *testing.T) {
	r := setupSyncTestRepo(t)
	uuid := storeTestBlob(t, r, []byte("pull me"))

	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.PullCard{ServerCode: "test", ProjectCode: "test"},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}

	igots := findCards[*xfer.IGotCard](resp)
	found := false
	for _, ig := range igots {
		if ig.UUID == uuid {
			found = true
		}
	}
	if !found {
		t.Fatalf("pull response missing igot for %s", uuid)
	}
}

func TestHandleIGotGimme(t *testing.T) {
	r := setupSyncTestRepo(t)
	unknownUUID := "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.PullCard{ServerCode: "test", ProjectCode: "test"},
		&xfer.IGotCard{UUID: unknownUUID},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}

	gimmes := findCards[*xfer.GimmeCard](resp)
	found := false
	for _, g := range gimmes {
		if g.UUID == unknownUUID {
			found = true
		}
	}
	if !found {
		t.Fatal("expected gimme for unknown UUID")
	}
}

func TestHandleIGotWithoutPull(t *testing.T) {
	r := setupSyncTestRepo(t)

	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.IGotCard{UUID: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}

	gimmes := findCards[*xfer.GimmeCard](resp)
	if len(gimmes) > 0 {
		t.Fatal("should not gimme without pull card")
	}
}

func TestHandleGimme(t *testing.T) {
	r := setupSyncTestRepo(t)
	data := []byte("gimme this")
	uuid := storeTestBlob(t, r, data)

	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.PullCard{ServerCode: "test", ProjectCode: "test"},
		&xfer.GimmeCard{UUID: uuid},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}

	files := findCards[*xfer.FileCard](resp)
	found := false
	for _, f := range files {
		if f.UUID == uuid && string(f.Content) == string(data) {
			found = true
		}
	}
	if !found {
		t.Fatal("expected file card with correct content")
	}
}

func TestHandleGimmeMissing(t *testing.T) {
	r := setupSyncTestRepo(t)

	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.GimmeCard{UUID: "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}

	files := findCards[*xfer.FileCard](resp)
	if len(files) > 0 {
		t.Fatal("should not return file for missing blob")
	}
}

func TestHandlePushFile(t *testing.T) {
	r := setupSyncTestRepo(t)
	data := []byte("push this")
	uuid := hash.SHA1(data)

	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.PushCard{ServerCode: "test", ProjectCode: "test"},
		&xfer.FileCard{UUID: uuid, Content: data},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}

	errs := findCards[*xfer.ErrorCard](resp)
	if len(errs) > 0 {
		t.Fatalf("unexpected error: %s", errs[0].Message)
	}

	_, ok := blob.Exists(r.DB(), uuid)
	if !ok {
		t.Fatal("pushed blob not stored")
	}
}

func TestHandleFileWithoutPush(t *testing.T) {
	r := setupSyncTestRepo(t)
	data := []byte("no push card")
	uuid := hash.SHA1(data)

	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.FileCard{UUID: uuid, Content: data},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}

	errs := findCards[*xfer.ErrorCard](resp)
	if len(errs) == 0 {
		t.Fatal("expected error for file without push")
	}
}

func TestHandleClone(t *testing.T) {
	r := setupSyncTestRepo(t)
	stored := map[string]bool{}
	for i := range 5 {
		data := []byte(fmt.Sprintf("clone test %d", i))
		uuid := storeTestBlob(t, r, data)
		stored[uuid] = true
	}

	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.CloneCard{Version: 1},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}

	files := findCards[*xfer.FileCard](resp)
	for _, f := range files {
		delete(stored, f.UUID)
	}
	if len(stored) > 0 {
		t.Fatalf("clone missing blobs: %v", stored)
	}
}

func TestHandleClonePagination(t *testing.T) {
	r := setupSyncTestRepo(t)
	for i := range DefaultCloneBatchSize + 5 {
		data := []byte(fmt.Sprintf("page blob %d", i))
		storeTestBlob(t, r, data)
	}

	// Page 1
	req1 := &xfer.Message{Cards: []xfer.Card{&xfer.CloneCard{Version: 1}}}
	resp1, err := HandleSync(context.Background(), r, req1)
	if err != nil {
		t.Fatalf("page 1: %v", err)
	}

	files1 := findCards[*xfer.FileCard](resp1)
	seqnos := findCards[*xfer.CloneSeqNoCard](resp1)
	if len(seqnos) == 0 {
		t.Fatal("page 1: expected clone_seqno card for continuation")
	}
	if len(files1) != DefaultCloneBatchSize {
		t.Fatalf("page 1: got %d files, want %d", len(files1), DefaultCloneBatchSize)
	}

	// Page 2
	req2 := &xfer.Message{Cards: []xfer.Card{
		&xfer.CloneCard{Version: 1},
		&xfer.CloneSeqNoCard{SeqNo: seqnos[0].SeqNo},
	}}
	resp2, err := HandleSync(context.Background(), r, req2)
	if err != nil {
		t.Fatalf("page 2: %v", err)
	}

	files2 := findCards[*xfer.FileCard](resp2)
	if len(files2) < 5 {
		t.Fatalf("page 2: got %d files, want >= 5", len(files2))
	}

	seqnos2 := findCards[*xfer.CloneSeqNoCard](resp2)
	if len(seqnos2) > 0 {
		t.Fatal("page 2: should not have clone_seqno (all blobs served)")
	}
}

func TestHandleReqConfig(t *testing.T) {
	r := setupSyncTestRepo(t)

	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.ReqConfigCard{Name: "project-code"},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}

	configs := findCards[*xfer.ConfigCard](resp)
	found := false
	for _, c := range configs {
		if c.Name == "project-code" && len(c.Content) > 0 {
			found = true
		}
	}
	if !found {
		t.Fatal("expected config card for project-code")
	}
}

func TestHandlePushFileStoreFails(t *testing.T) {
	r := setupSyncTestRepo(t)
	// File with valid push but bad hash → storeReceivedFile returns error → ErrorCard
	badUUID := "cccccccccccccccccccccccccccccccccccccccc"
	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.PushCard{ServerCode: "test", ProjectCode: "test"},
		&xfer.FileCard{UUID: badUUID, Content: []byte("wrong hash content")},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}
	errs := findCards[*xfer.ErrorCard](resp)
	if len(errs) == 0 {
		t.Fatal("expected error card for bad hash")
	}
}

func TestHandleCFileCard(t *testing.T) {
	r := setupSyncTestRepo(t)
	data := []byte("cfile content")
	uuid := hash.SHA1(data)

	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.PushCard{ServerCode: "test", ProjectCode: "test"},
		&xfer.CFileCard{UUID: uuid, Content: data, USize: len(data)},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}
	errs := findCards[*xfer.ErrorCard](resp)
	if len(errs) > 0 {
		t.Fatalf("unexpected error: %s", errs[0].Message)
	}
	_, ok := blob.Exists(r.DB(), uuid)
	if !ok {
		t.Fatal("cfile blob not stored")
	}
}

func TestHandleLoginAndPragma(t *testing.T) {
	r := setupSyncTestRepo(t)
	storeTestBlob(t, r, []byte("login pragma test"))

	// Login + pragma should be accepted without error
	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.LoginCard{User: "test", Nonce: "abc", Signature: "def"},
		&xfer.PragmaCard{Name: "client-version", Values: []string{"22800"}},
		&xfer.PragmaCard{Name: "unknown-pragma", Values: []string{"ignored"}},
		&xfer.PullCard{ServerCode: "test", ProjectCode: "test"},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}
	errs := findCards[*xfer.ErrorCard](resp)
	if len(errs) > 0 {
		t.Fatalf("unexpected error: %s", errs[0].Message)
	}
	igots := findCards[*xfer.IGotCard](resp)
	if len(igots) == 0 {
		t.Fatal("expected igot cards after login+pragma+pull")
	}
}

func TestHandleCloneSeqNo(t *testing.T) {
	r := setupSyncTestRepo(t)
	// Store blobs, then clone with a seqno that skips them
	for i := range 3 {
		data := []byte(fmt.Sprintf("seqno test %d", i))
		storeTestBlob(t, r, data)
	}

	// Get all blobs to find the max rid
	req1 := &xfer.Message{Cards: []xfer.Card{&xfer.CloneCard{Version: 1}}}
	resp1, _ := HandleSync(context.Background(), r, req1)
	files1 := findCards[*xfer.FileCard](resp1)

	// Now clone with seqno past all blobs — should get nothing
	req2 := &xfer.Message{Cards: []xfer.Card{
		&xfer.CloneCard{Version: 1},
		&xfer.CloneSeqNoCard{SeqNo: 9999},
	}}
	resp2, err := HandleSync(context.Background(), r, req2)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}
	files2 := findCards[*xfer.FileCard](resp2)
	if len(files2) > 0 {
		t.Fatalf("expected no files with high seqno, got %d", len(files2))
	}

	_ = files1 // used for context
}

func TestHandleEmptyRequest(t *testing.T) {
	r := setupSyncTestRepo(t)
	req := &xfer.Message{Cards: nil}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}
	if len(resp.Cards) != 0 {
		t.Fatalf("expected empty response for empty request, got %d cards", len(resp.Cards))
	}
}

func TestHandleReqConfigMissing(t *testing.T) {
	r := setupSyncTestRepo(t)

	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.ReqConfigCard{Name: "nonexistent-config"},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}

	configs := findCards[*xfer.ConfigCard](resp)
	if len(configs) > 0 {
		t.Fatal("should not return config for nonexistent key")
	}
}

// TestHandleSyncNoSpuriousGimmeForReceivedFile verifies that HandleSync
// does NOT emit a GimmeCard for a blob that was delivered as a FileCard
// in the same request. Regression test for the igot-before-file bug:
// if IGotCard is processed before FileCard, blob.Exists returns false
// and a spurious GimmeCard is emitted, causing infinite sync loops.
func TestHandleSyncNoSpuriousGimmeForReceivedFile(t *testing.T) {
	r := setupSyncTestRepo(t)
	defer r.Close()

	// Create a blob to push to the handler.
	content := []byte("test content for spurious gimme check")
	uuid := hash.SHA1(content)

	// Build a request with BOTH IGotCard and FileCard for the same blob.
	// This mimics what a sync client sends when pushing a new blob:
	// it announces via igot AND delivers the file in the same round.
	req := &xfer.Message{
		Cards: []xfer.Card{
			&xfer.PushCard{
				ServerCode:  "test-server",
				ProjectCode: "test-project",
			},
			&xfer.PullCard{
				ServerCode:  "test-server",
				ProjectCode: "test-project",
			},
			&xfer.IGotCard{UUID: uuid},
			&xfer.FileCard{UUID: uuid, Content: content},
		},
	}

	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}

	// Check response for spurious gimme.
	for _, card := range resp.Cards {
		if g, ok := card.(*xfer.GimmeCard); ok && g.UUID == uuid {
			t.Errorf("HandleSync emitted GimmeCard for %s which was delivered as FileCard in the same request — this causes infinite sync loops", uuid[:12])
		}
	}

	// Verify the blob was actually stored.
	if _, exists := blob.Exists(r.DB(), uuid); !exists {
		t.Errorf("blob %s was not stored by HandleSync", uuid[:12])
	}

	// Verify the server acknowledges it via igot in the response.
	found := false
	for _, card := range resp.Cards {
		if ig, ok := card.(*xfer.IGotCard); ok && ig.UUID == uuid {
			found = true
			break
		}
	}
	if !found {
		t.Errorf("HandleSync did not emit IGotCard for %s in response", uuid[:12])
	}
}
