// Package hash provides content-addressing hashes for Fossil artifacts.
//
// Fossil uses SHA1 for legacy repos and SHA3-256 for newer repos.
// The hash of an artifact's content is its unique identifier (UUID).
package hash

import (
	"crypto/sha1"
	"encoding/hex"
	"fmt"
)

// SHA1 computes the SHA1 hex digest of data.
// This is Fossil's primary hash for legacy repositories.
func SHA1(data []byte) string {
	h := sha1.Sum(data)
	return hex.EncodeToString(h[:])
}

// Verify checks that data matches the expected hex hash.
// Supports SHA1 (40 hex chars) and SHA3-256 (64 hex chars).
func Verify(data []byte, expected string) error {
	switch len(expected) {
	case 40:
		got := SHA1(data)
		if got != expected {
			return fmt.Errorf("hash mismatch: got %s, want %s", got, expected)
		}
		return nil
	case 64:
		got := SHA3_256(data)
		if got != expected {
			return fmt.Errorf("hash mismatch: got %s, want %s", got, expected)
		}
		return nil
	default:
		return fmt.Errorf("unsupported hash length: %d", len(expected))
	}
}
