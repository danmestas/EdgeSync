package hash

import (
	"crypto/sha1"
	"encoding/hex"

	"golang.org/x/crypto/sha3"
)

func SHA1(data []byte) string {
	if data == nil {
		panic("hash.SHA1: data must not be nil")
	}
	h := sha1.Sum(data)
	return hex.EncodeToString(h[:])
}

func SHA3(data []byte) string {
	if data == nil {
		panic("hash.SHA3: data must not be nil")
	}
	h := sha3.Sum256(data)
	return hex.EncodeToString(h[:])
}

func HashSize(hashType string) int {
	if hashType == "" {
		panic("hash.HashSize: hashType must not be empty")
	}
	switch hashType {
	case "sha1":
		return sha1.Size * 2
	case "sha3":
		return 64
	default:
		return 0
	}
}

func IsValidHash(h string) bool {
	if h == "" {
		panic("hash.IsValidHash: h must not be empty")
	}
	if len(h) != 40 && len(h) != 64 {
		return false
	}
	for _, c := range h {
		if !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
			return false
		}
	}
	return true
}
