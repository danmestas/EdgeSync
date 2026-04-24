package notify

import (
	"crypto/rand"
	"crypto/sha256"
	"fmt"
	"net/url"
	"strings"

	qrcode "github.com/skip2/go-qrcode"
)

// tokenAlphabet is 30 chars: no 0/O/1/I/l to avoid ambiguity.
const tokenAlphabet = "23456789ABCDEFGHJKMNPQRSTVWXYZ"

// tokenRejectThreshold is the largest multiple of len(tokenAlphabet) that fits in a byte.
// Bytes >= this value are rejected to avoid modulo bias.
const tokenRejectThreshold = 30 * (256 / 30) // 30 * 8 = 240

// GenerateToken creates a 12-char alphanumeric token formatted as XXXX-XXXX-XXXX.
// Uses rejection sampling to avoid modulo bias from the 30-char alphabet.
func GenerateToken() (string, error) {
	chars := make([]byte, 12)
	for i := 0; i < 12; {
		b := make([]byte, 1)
		if _, err := rand.Read(b); err != nil {
			return "", fmt.Errorf("generate token: %w", err)
		}
		if int(b[0]) >= tokenRejectThreshold {
			continue
		}
		chars[i] = tokenAlphabet[int(b[0])%len(tokenAlphabet)]
		i++
	}
	return fmt.Sprintf("%s-%s-%s", string(chars[0:4]), string(chars[4:8]), string(chars[8:12])), nil
}

// RawToken strips dashes from a formatted token.
func RawToken(formatted string) string {
	return strings.ReplaceAll(formatted, "-", "")
}

// HashToken returns the hex-encoded SHA-256 hash of the raw (no dashes) token.
func HashToken(formatted string) string {
	raw := RawToken(formatted)
	h := sha256.Sum256([]byte(raw))
	return fmt.Sprintf("%x", h)
}

// FormatPairURL builds the QR payload string.
// The NATS address is URL-encoded to avoid slash ambiguity.
func FormatPairURL(endpointID, natsAddr, token string) string {
	return fmt.Sprintf("edgesync-pair://v1/%s/%s/%s",
		endpointID,
		url.PathEscape(natsAddr),
		RawToken(token),
	)
}

// PairInfo is the parsed content of a pairing URL.
type PairInfo struct {
	EndpointID string
	NATSAddr   string
	Token      string // raw, no dashes
}

// ParsePairURL parses an edgesync-pair:// URL into its components.
func ParsePairURL(raw string) (PairInfo, error) {
	const prefix = "edgesync-pair://v1/"
	if !strings.HasPrefix(raw, prefix) {
		return PairInfo{}, fmt.Errorf("invalid pair URL: missing prefix")
	}
	rest := raw[len(prefix):]
	// Split into exactly 3 parts: endpointID / encoded-nats-addr / token
	parts := strings.SplitN(rest, "/", 3)
	if len(parts) != 3 {
		return PairInfo{}, fmt.Errorf("invalid pair URL: expected 3 path segments, got %d", len(parts))
	}
	natsAddr, err := url.PathUnescape(parts[1])
	if err != nil {
		return PairInfo{}, fmt.Errorf("invalid pair URL: unescape NATS addr: %w", err)
	}
	return PairInfo{
		EndpointID: parts[0],
		NATSAddr:   natsAddr,
		Token:      parts[2],
	}, nil
}

// RenderQR returns a terminal-friendly QR code string for the given content.
func RenderQR(content string) (string, error) {
	qr, err := qrcode.New(content, qrcode.Medium)
	if err != nil {
		return "", fmt.Errorf("generate QR: %w", err)
	}
	return qr.ToSmallString(false), nil
}
