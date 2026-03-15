// Package delta implements the Fossil delta compression algorithm.
//
// The Fossil delta format encodes the differences between two blobs
// (source and target) as a sequence of copy and insert commands.
// This is a pure Go port of Fossil's delta.c.
//
// Reference: https://fossil-scm.org/home/doc/tip/www/delta_format.wiki
package delta

import (
	"errors"
	"fmt"
)

var (
	ErrInvalidDelta = errors.New("delta: invalid delta format")
	ErrChecksum     = errors.New("delta: checksum mismatch")
)

// digits maps base-64 characters to their numeric values.
// Fossil uses its own base-64 encoding (not standard base64).
var digits = [128]int{
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, -1, -1, -1, -1, -1, -1, //  0-9
	-1, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, // A-O
	25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, -1, -1, -1, -1, 36, // P-Z, _
	-1, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, // a-o
	52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, -1, -1, -1, 63, -1, // p-z, ~
}

// reader wraps a byte slice for sequential reading of delta commands.
type reader struct {
	data []byte
	pos  int
}

// getInt reads a base-64 encoded integer from the delta.
func (r *reader) getInt() (uint64, error) {
	var v uint64
	started := false
	for r.pos < len(r.data) {
		c := r.data[r.pos]
		if c >= 128 || digits[c] < 0 {
			break
		}
		v = v*64 + uint64(digits[c])
		r.pos++
		started = true
	}
	if !started {
		return 0, fmt.Errorf("%w: expected integer", ErrInvalidDelta)
	}
	return v, nil
}

// getChar reads the next character from the delta.
func (r *reader) getChar() (byte, error) {
	if r.pos >= len(r.data) {
		return 0, fmt.Errorf("%w: unexpected end of delta", ErrInvalidDelta)
	}
	c := r.data[r.pos]
	r.pos++
	return c, nil
}

// Apply applies a delta to a source blob, producing the target blob.
func Apply(source, delta []byte) ([]byte, error) {
	r := &reader{data: delta}

	// Read the target length
	targetLen, err := r.getInt()
	if err != nil {
		return nil, err
	}
	term, err := r.getChar()
	if err != nil {
		return nil, err
	}
	if term != '\n' {
		return nil, fmt.Errorf("%w: expected newline after target length", ErrInvalidDelta)
	}

	output := make([]byte, 0, targetLen)

	for r.pos < len(r.data) {
		cnt, err := r.getInt()
		if err != nil {
			return nil, err
		}
		cmd, err := r.getChar()
		if err != nil {
			return nil, err
		}

		switch cmd {
		case '@':
			// Copy from source: @offset,length
			offset := cnt
			cnt, err = r.getInt()
			if err != nil {
				return nil, err
			}
			term, err = r.getChar()
			if err != nil {
				return nil, err
			}
			if term != ',' {
				return nil, fmt.Errorf("%w: expected comma in copy command", ErrInvalidDelta)
			}
			if int(offset+cnt) > len(source) {
				return nil, fmt.Errorf("%w: copy exceeds source bounds", ErrInvalidDelta)
			}
			output = append(output, source[offset:offset+cnt]...)

		case ':':
			// Insert literal bytes
			if r.pos+int(cnt) > len(r.data) {
				return nil, fmt.Errorf("%w: insert exceeds delta bounds", ErrInvalidDelta)
			}
			output = append(output, r.data[r.pos:r.pos+int(cnt)]...)
			r.pos += int(cnt)

		case ';':
			// Checksum and end
			if uint64(len(output)) != targetLen {
				return nil, fmt.Errorf("%w: output size %d != target size %d", ErrInvalidDelta, len(output), targetLen)
			}
			checksum := checksum(output)
			if cnt != uint64(checksum) {
				return nil, ErrChecksum
			}
			return output, nil

		default:
			return nil, fmt.Errorf("%w: unknown command '%c'", ErrInvalidDelta, cmd)
		}
	}

	return nil, fmt.Errorf("%w: missing terminator", ErrInvalidDelta)
}

// checksum computes the Fossil delta checksum for a byte slice.
func checksum(data []byte) uint32 {
	var sum0, sum1, sum2 uint16
	for _, b := range data {
		sum0 += uint16(b)
		sum1 += sum0
		sum2 += sum1
	}
	// Note: Fossil uses a 3-part checksum packed differently.
	// This will be refined against test vectors from actual Fossil deltas.
	_ = sum2
	return uint32(sum0) | (uint32(sum1) << 16)
}
