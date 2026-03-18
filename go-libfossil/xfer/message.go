package xfer

import (
	"bufio"
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"fmt"
	"io"
)

// Message is a sequence of cards forming one xfer request or response.
type Message struct {
	Cards []Card
}

// Encode serializes all cards and zlib-compresses the result.
// Uses Fossil's compression format: 4-byte big-endian uncompressed size prefix
// followed by standard zlib data.
func (m *Message) Encode() ([]byte, error) {
	if m == nil {
		panic("xfer.Message.Encode: m must not be nil")
	}
	raw, err := m.EncodeUncompressed()
	if err != nil {
		return nil, err
	}
	var zbuf bytes.Buffer
	// 4-byte big-endian uncompressed size prefix (Fossil's blob_compress format)
	var sizePrefix [4]byte
	binary.BigEndian.PutUint32(sizePrefix[:], uint32(len(raw)))
	zbuf.Write(sizePrefix[:])
	zw := zlib.NewWriter(&zbuf)
	if _, err := zw.Write(raw); err != nil {
		return nil, fmt.Errorf("xfer: message zlib write: %w", err)
	}
	if err := zw.Close(); err != nil {
		return nil, fmt.Errorf("xfer: message zlib close: %w", err)
	}
	return zbuf.Bytes(), nil
}

// EncodeUncompressed serializes all cards without zlib compression.
func (m *Message) EncodeUncompressed() ([]byte, error) {
	var buf bytes.Buffer
	for i, c := range m.Cards {
		if err := EncodeCard(&buf, c); err != nil {
			return nil, fmt.Errorf("xfer: encode card %d (%T): %w", i, c, err)
		}
	}
	return buf.Bytes(), nil
}

// Decode decodes an xfer message. It first tries zlib-compressed format
// (4-byte BE size prefix + zlib data, used by normal sync), then falls
// back to uncompressed (used by clone protocol v3, which sends
// Content-Type: application/x-fossil-uncompressed).
func Decode(data []byte) (*Message, error) {
	if len(data) < 4 {
		return nil, fmt.Errorf("xfer: message too short (%d bytes)", len(data))
	}
	// Try zlib first: skip 4-byte size prefix, attempt zlib decompression.
	zlibData := data[4:]
	zr, err := zlib.NewReader(bytes.NewReader(zlibData))
	if err == nil {
		raw, readErr := io.ReadAll(zr)
		zr.Close()
		if readErr == nil {
			return DecodeUncompressed(raw)
		}
	}
	// Fallback: treat entire payload as uncompressed card data.
	return DecodeUncompressed(data)
}

// DecodeUncompressed decodes cards from uncompressed data.
func DecodeUncompressed(data []byte) (*Message, error) {
	r := bufio.NewReader(bytes.NewReader(data))
	msg := &Message{}
	for {
		card, err := DecodeCard(r)
		if err == io.EOF {
			return msg, nil
		}
		if err != nil {
			return nil, fmt.Errorf("xfer: decode card %d: %w", len(msg.Cards), err)
		}
		msg.Cards = append(msg.Cards, card)
	}
}
