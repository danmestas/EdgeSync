package sync

import (
	"context"
	"fmt"
	"net"
	"testing"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/hash"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

func freePort(t *testing.T) string {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	addr := ln.Addr().String()
	ln.Close()
	return addr
}

func TestServeHTTPRoundTrip(t *testing.T) {
	r := setupSyncTestRepo(t)
	data := []byte("http test blob")
	uuid := hash.SHA1(data)
	StoreBlob(r.DB(), uuid, "", data)

	addr := freePort(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	go ServeHTTP(ctx, addr, r, HandleSync)
	time.Sleep(100 * time.Millisecond)

	transport := &HTTPTransport{URL: fmt.Sprintf("http://%s", addr)}
	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.PullCard{ServerCode: "test", ProjectCode: "test"},
		&xfer.GimmeCard{UUID: uuid},
	}}

	resp, err := transport.Exchange(ctx, req)
	if err != nil {
		t.Fatalf("exchange: %v", err)
	}

	files := findCards[*xfer.FileCard](resp)
	found := false
	for _, f := range files {
		if f.UUID == uuid && string(f.Content) == string(data) {
			found = true
		}
	}
	if !found {
		t.Fatal("expected file card in HTTP response")
	}
}

func TestServeHTTPPushPull(t *testing.T) {
	r := setupSyncTestRepo(t)
	addr := freePort(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	go ServeHTTP(ctx, addr, r, HandleSync)
	time.Sleep(100 * time.Millisecond)

	transport := &HTTPTransport{URL: fmt.Sprintf("http://%s", addr)}

	// Push a blob
	data := []byte("pushed via http")
	uuid := hash.SHA1(data)

	pushReq := &xfer.Message{Cards: []xfer.Card{
		&xfer.PushCard{ServerCode: "test", ProjectCode: "test"},
		&xfer.FileCard{UUID: uuid, Content: data},
	}}
	pushResp, err := transport.Exchange(ctx, pushReq)
	if err != nil {
		t.Fatalf("push exchange: %v", err)
	}
	errs := findCards[*xfer.ErrorCard](pushResp)
	if len(errs) > 0 {
		t.Fatalf("push error: %s", errs[0].Message)
	}

	// Pull it back
	pullReq := &xfer.Message{Cards: []xfer.Card{
		&xfer.PullCard{ServerCode: "test", ProjectCode: "test"},
		&xfer.GimmeCard{UUID: uuid},
	}}
	pullResp, err := transport.Exchange(ctx, pullReq)
	if err != nil {
		t.Fatalf("pull exchange: %v", err)
	}

	files := findCards[*xfer.FileCard](pullResp)
	found := false
	for _, f := range files {
		if f.UUID == uuid && string(f.Content) == string(data) {
			found = true
		}
	}
	if !found {
		t.Fatal("pushed blob not available via pull")
	}
}

func TestServeHTTPClone(t *testing.T) {
	r := setupSyncTestRepo(t)
	stored := map[string]bool{}
	for i := 0; i < 3; i++ {
		data := []byte(fmt.Sprintf("clone http %d", i))
		uuid := hash.SHA1(data)
		StoreBlob(r.DB(), uuid, "", data)
		stored[uuid] = true
	}

	addr := freePort(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	go ServeHTTP(ctx, addr, r, HandleSync)
	time.Sleep(100 * time.Millisecond)

	transport := &HTTPTransport{URL: fmt.Sprintf("http://%s", addr)}
	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.CloneCard{Version: 1},
	}}
	resp, err := transport.Exchange(ctx, req)
	if err != nil {
		t.Fatalf("clone exchange: %v", err)
	}

	files := findCards[*xfer.FileCard](resp)
	for _, f := range files {
		delete(stored, f.UUID)
	}
	if len(stored) > 0 {
		t.Fatalf("clone missing blobs: %v", stored)
	}
}
