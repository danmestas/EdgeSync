package xfer

import "testing"

func TestSchemaCardType(t *testing.T) {
	c := &SchemaCard{Table: "peer_registry", Version: 1, Hash: "abc", MTime: 100, Content: []byte(`{}`)}
	if c.Type() != CardSchema {
		t.Fatalf("got %v, want CardSchema", c.Type())
	}
}

func TestXIGotCardType(t *testing.T) {
	c := &XIGotCard{Table: "peer_registry", PKHash: "abc", MTime: 100}
	if c.Type() != CardXIGot {
		t.Fatalf("got %v, want CardXIGot", c.Type())
	}
}

func TestXGimmeCardType(t *testing.T) {
	c := &XGimmeCard{Table: "peer_registry", PKHash: "abc"}
	if c.Type() != CardXGimme {
		t.Fatalf("got %v, want CardXGimme", c.Type())
	}
}

func TestXRowCardType(t *testing.T) {
	c := &XRowCard{Table: "peer_registry", PKHash: "abc", MTime: 100, Content: []byte(`{}`)}
	if c.Type() != CardXRow {
		t.Fatalf("got %v, want CardXRow", c.Type())
	}
}
