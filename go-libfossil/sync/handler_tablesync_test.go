package sync

import (
	"context"
	"encoding/json"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

func TestHandleSchemaCard(t *testing.T) {
	r := setupSyncTestRepo(t)
	def := repo.TableDef{
		Columns:  []repo.ColumnDef{{Name: "peer_id", Type: "text", PK: true}},
		Conflict: "mtime-wins",
	}
	defJSON, _ := json.Marshal(def)
	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.PushCard{ServerCode: "s", ProjectCode: "p"},
		&xfer.PullCard{ServerCode: "s", ProjectCode: "p"},
		&xfer.SchemaCard{Table: "test_table", Version: 1, Hash: "abc", MTime: 100, Content: defJSON},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}
	_ = resp
	var count int
	if err := r.DB().QueryRow("SELECT count(*) FROM x_test_table").Scan(&count); err != nil {
		t.Fatalf("x_test_table should exist: %v", err)
	}
}

func TestHandleXIGotXGimmeXRow(t *testing.T) {
	r := setupSyncTestRepo(t)
	def := repo.TableDef{
		Columns:  []repo.ColumnDef{{Name: "id", Type: "text", PK: true}, {Name: "val", Type: "text"}},
		Conflict: "mtime-wins",
	}
	repo.EnsureSyncSchema(r.DB())
	repo.RegisterSyncedTable(r.DB(), "kv", def, 100)
	repo.UpsertXRow(r.DB(), "kv", map[string]any{"id": "local-key", "val": "local-val"}, 200)
	localPK := repo.PKHash(map[string]any{"id": "local-key"})
	remotePK := repo.PKHash(map[string]any{"id": "remote-key"})
	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.PushCard{ServerCode: "s", ProjectCode: "p"},
		&xfer.PullCard{ServerCode: "s", ProjectCode: "p"},
		&xfer.XIGotCard{Table: "kv", PKHash: remotePK, MTime: 300},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}
	gimmes := findCards[*xfer.XGimmeCard](resp)
	if len(gimmes) == 0 {
		t.Fatal("expected xgimme for missing remote row")
	}
	if gimmes[0].PKHash != remotePK {
		t.Fatalf("xgimme pk = %q, want %q", gimmes[0].PKHash, remotePK)
	}
	igots := findCards[*xfer.XIGotCard](resp)
	found := false
	for _, ig := range igots {
		if ig.PKHash == localPK {
			found = true
		}
	}
	if !found {
		t.Fatal("expected xigot for local row")
	}
}
