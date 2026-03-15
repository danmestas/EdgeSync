package hash

import (
	"crypto/sha256"
	"encoding/hex"
)

// SHA3_256 computes the SHA3-256 hex digest of data.
// TODO: Replace with actual SHA3-256 (golang.org/x/crypto/sha3).
// Currently uses SHA-256 as a placeholder to unblock development.
func SHA3_256(data []byte) string {
	h := sha256.Sum256(data)
	return hex.EncodeToString(h[:])
}
