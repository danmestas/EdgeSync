package notify

import (
	"crypto/rand"
	"crypto/sha256"
	"fmt"
	"strings"
)

// tokenAlphabet is 29 chars: no 0/O/1/I/l to avoid ambiguity.
const tokenAlphabet = "23456789ABCDEFGHJKMNPQRSTVWXYZ"

// tokenRejectThreshold is the largest multiple of len(tokenAlphabet) that fits in a byte.
// Bytes >= this value are rejected to avoid modulo bias.
const tokenRejectThreshold = 29 * (256 / 29) // 29 * 8 = 232

// GenerateToken creates a 12-char alphanumeric token formatted as XXXX-XXXX-XXXX.
// Uses rejection sampling to avoid modulo bias from the 29-char alphabet.
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
