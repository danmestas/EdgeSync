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

func TestFormatPairURL(t *testing.T) {
	u := FormatPairURL("abc123endpointid", "nats://100.64.0.1:4222", "AXKF-9M2P-VR3T")
	// url.PathEscape escapes '/' but not ':' — the exact encoding is:
	want := "edgesync-pair://v1/abc123endpointid/nats:%2F%2F100.64.0.1:4222/AXKF9M2PVR3T"
	if u != want {
		t.Errorf("FormatPairURL =\n  %q\nwant:\n  %q", u, want)
	}
}

func TestPairURLRoundTrip(t *testing.T) {
	u := FormatPairURL("abc123endpointid", "nats://100.64.0.1:4222", "AXKF-9M2P-VR3T")
	info, err := ParsePairURL(u)
	if err != nil {
		t.Fatal(err)
	}
	if info.EndpointID != "abc123endpointid" {
		t.Errorf("EndpointID = %q", info.EndpointID)
	}
	if info.NATSAddr != "nats://100.64.0.1:4222" {
		t.Errorf("NATSAddr = %q", info.NATSAddr)
	}
	if info.Token != "AXKF9M2PVR3T" {
		t.Errorf("Token = %q", info.Token)
	}
}

func TestParsePairURLInvalid(t *testing.T) {
	if _, err := ParsePairURL("bogus"); err == nil {
		t.Error("expected error for invalid URL")
	}
	if _, err := ParsePairURL("edgesync-pair://v1/only-two"); err == nil {
		t.Error("expected error for too few segments")
	}
}

func TestRenderQR(t *testing.T) {
	qr, err := RenderQR("edgesync-pair://v1/test/nats%3A%2F%2Flocalhost%3A4222/ABCD1234EFGH")
	if err != nil {
		t.Fatal(err)
	}
	if len(qr) == 0 {
		t.Error("QR output should not be empty")
	}
}
