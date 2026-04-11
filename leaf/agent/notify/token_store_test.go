package notify

import (
	"testing"
	"time"

	_ "github.com/danmestas/go-libfossil/db/driver/modernc"
)

func TestPendingTokenLifecycle(t *testing.T) {
	r := createTestRepo(t)

	tok, err := GenerateToken()
	if err != nil {
		t.Fatal(err)
	}

	// Store the token.
	err = StorePendingToken(r, PendingToken{
		TokenHash:  HashToken(tok),
		DeviceName: "dan-iphone",
		CreatedAt:  time.Now().UTC(),
		ExpiresAt:  time.Now().UTC().Add(10 * time.Minute),
	})
	if err != nil {
		t.Fatal(err)
	}

	// Validate with correct token.
	pt, err := ValidateToken(r, tok)
	if err != nil {
		t.Fatal(err)
	}
	if pt.DeviceName != "dan-iphone" {
		t.Errorf("device name = %q", pt.DeviceName)
	}

	// Token is consumed — second validation fails.
	_, err = ValidateToken(r, tok)
	if err == nil {
		t.Error("expected error: token already consumed")
	}
}

func TestExpiredTokenRejected(t *testing.T) {
	r := createTestRepo(t)

	tok, err := GenerateToken()
	if err != nil {
		t.Fatal(err)
	}

	err = StorePendingToken(r, PendingToken{
		TokenHash:  HashToken(tok),
		DeviceName: "dan-iphone",
		CreatedAt:  time.Now().UTC().Add(-20 * time.Minute),
		ExpiresAt:  time.Now().UTC().Add(-10 * time.Minute), // already expired
	})
	if err != nil {
		t.Fatal(err)
	}

	_, err = ValidateToken(r, tok)
	if err == nil {
		t.Error("expected error for expired token")
	}
}

func TestCreatePairingToken(t *testing.T) {
	r := createTestRepo(t)

	tok, err := CreatePairingToken(r, "test-device")
	if err != nil {
		t.Fatal(err)
	}

	// Token should be in XXXX-XXXX-XXXX format.
	if len(tok) != 14 {
		t.Errorf("token length = %d, want 14", len(tok))
	}

	// Should be validatable.
	pt, err := ValidateToken(r, tok)
	if err != nil {
		t.Fatal(err)
	}
	if pt.DeviceName != "test-device" {
		t.Errorf("device name = %q", pt.DeviceName)
	}
}
