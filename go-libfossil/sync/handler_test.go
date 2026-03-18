package sync

import (
	"context"
	"fmt"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/hash"
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

func TestHandlePull(t *testing.T) {
	r := setupSyncTestRepo(t)
	data := []byte("pull me")
	uuid := hash.SHA1(data)
	StoreBlob(r.DB(), uuid, "", data)

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
	uuid := hash.SHA1(data)
	StoreBlob(r.DB(), uuid, "", data)

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
	for i := 0; i < 5; i++ {
		data := []byte(fmt.Sprintf("clone test %d", i))
		uuid := hash.SHA1(data)
		StoreBlob(r.DB(), uuid, "", data)
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
		uuid := hash.SHA1(data)
		StoreBlob(r.DB(), uuid, "", data)
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

	// Page 2 should have no seqno (all blobs sent)
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
