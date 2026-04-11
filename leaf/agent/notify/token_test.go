package notify

import (
	"strings"
	"testing"
)

func TestGenerateToken(t *testing.T) {
	tok, err := GenerateToken()
	if err != nil {
		t.Fatal(err)
	}

	// Format: XXXX-XXXX-XXXX
	if len(tok) != 14 { // 12 chars + 2 dashes
		t.Errorf("token length = %d, want 14", len(tok))
	}
	parts := strings.Split(tok, "-")
	if len(parts) != 3 {
		t.Fatalf("token parts = %d, want 3", len(parts))
	}
	for i, p := range parts {
		if len(p) != 4 {
			t.Errorf("part[%d] length = %d, want 4", i, len(p))
		}
	}

	// No ambiguous characters.
	raw := strings.ReplaceAll(tok, "-", "")
	for _, c := range raw {
		switch c {
		case '0', 'O', '1', 'I', 'l':
			t.Errorf("token contains ambiguous char %q", string(c))
		}
	}

	// Two tokens should differ.
	tok2, _ := GenerateToken()
	if tok == tok2 {
		t.Error("two generated tokens should not be equal")
	}
}

func TestHashToken(t *testing.T) {
	tok := "AXKF-9M2P-VR3T"
	h := HashToken(tok)

	if len(h) != 64 { // SHA-256 hex
		t.Errorf("hash length = %d, want 64", len(h))
	}

	// Same input = same hash.
	if HashToken(tok) != h {
		t.Error("hash should be deterministic")
	}

	// Different input = different hash.
	if HashToken("ZZZZ-ZZZZ-ZZZZ") == h {
		t.Error("different tokens should produce different hashes")
	}
}

func TestRawToken(t *testing.T) {
	if got := RawToken("AXKF-9M2P-VR3T"); got != "AXKF9M2PVR3T" {
		t.Errorf("RawToken = %q, want %q", got, "AXKF9M2PVR3T")
	}
}
