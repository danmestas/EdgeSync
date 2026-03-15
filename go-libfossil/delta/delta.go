package delta

import (
	"errors"
	"fmt"
)

var (
	ErrInvalidDelta = errors.New("delta: invalid format")
	ErrChecksum     = errors.New("delta: checksum mismatch")
)

var digits = [128]int{
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, -1, -1, -1, -1, -1, -1,
	-1, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
	25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, -1, -1, -1, -1, 36,
	-1, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51,
	52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, -1, -1, -1, 63, -1,
}

type reader struct {
	data []byte
	pos  int
}

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
		return 0, fmt.Errorf("%w: expected integer at pos %d", ErrInvalidDelta, r.pos)
	}
	return v, nil
}

func (r *reader) getChar() (byte, error) {
	if r.pos >= len(r.data) {
		return 0, fmt.Errorf("%w: unexpected end at pos %d", ErrInvalidDelta, r.pos)
	}
	c := r.data[r.pos]
	r.pos++
	return c, nil
}

func Apply(source, delta []byte) ([]byte, error) {
	if len(delta) == 0 {
		return nil, fmt.Errorf("%w: empty delta", ErrInvalidDelta)
	}

	r := &reader{data: delta}

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
				return nil, fmt.Errorf("%w: copy exceeds source bounds (offset=%d, cnt=%d, srclen=%d)",
					ErrInvalidDelta, offset, cnt, len(source))
			}
			output = append(output, source[offset:offset+cnt]...)

		case ':':
			if r.pos+int(cnt) > len(r.data) {
				return nil, fmt.Errorf("%w: insert exceeds delta bounds", ErrInvalidDelta)
			}
			output = append(output, r.data[r.pos:r.pos+int(cnt)]...)
			r.pos += int(cnt)

		case ';':
			if uint64(len(output)) != targetLen {
				return nil, fmt.Errorf("%w: output size %d != target size %d",
					ErrInvalidDelta, len(output), targetLen)
			}
			if cnt != uint64(Checksum(output)) {
				return nil, fmt.Errorf("%w: expected %d, got %d",
					ErrChecksum, Checksum(output), cnt)
			}
			return output, nil

		default:
			return nil, fmt.Errorf("%w: unknown command '%c' at pos %d",
				ErrInvalidDelta, cmd, r.pos-1)
		}
	}

	return nil, fmt.Errorf("%w: missing terminator", ErrInvalidDelta)
}

func Checksum(data []byte) uint32 {
	var sum0, sum1 uint16
	for _, b := range data {
		sum0 += uint16(b)
		sum1 += sum0
	}
	return uint32(sum0) | (uint32(sum1) << 16)
}

const zDigitsEnc = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz~"

func Create(source, target []byte) []byte {
	if len(target) == 0 {
		var buf []byte
		buf = appendInt(buf, 0)
		buf = append(buf, '\n')
		buf = appendInt(buf, uint64(Checksum(target)))
		buf = append(buf, ';')
		return buf
	}

	if len(source) < 16 {
		return createInsertAll(target)
	}

	const nHash = 16
	type hashEntry struct {
		offset int
		next   int
	}

	tableSize := len(source) / nHash
	if tableSize < 64 {
		tableSize = 64
	}
	for tableSize&(tableSize-1) != 0 {
		tableSize &= tableSize - 1
	}
	tableSize <<= 1
	mask := tableSize - 1

	heads := make([]int, tableSize)
	entries := make([]hashEntry, 0, len(source)/nHash)

	for i := 0; i+nHash <= len(source); i += nHash {
		h := rollingHash(source[i : i+nHash])
		idx := int(h) & mask
		entries = append(entries, hashEntry{offset: i, next: heads[idx] - 1})
		heads[idx] = len(entries)
	}

	var buf []byte
	buf = appendInt(buf, uint64(len(target)))
	buf = append(buf, '\n')

	var pendingInsert []byte
	tPos := 0

	flushInsert := func() {
		if len(pendingInsert) > 0 {
			buf = appendInt(buf, uint64(len(pendingInsert)))
			buf = append(buf, ':')
			buf = append(buf, pendingInsert...)
			pendingInsert = pendingInsert[:0]
		}
	}

	for tPos < len(target) {
		bestLen := 0
		bestOff := 0

		if tPos+nHash <= len(target) {
			h := rollingHash(target[tPos : tPos+nHash])
			idx := int(h) & mask
			ei := heads[idx]
			for ei > 0 {
				e := entries[ei-1]
				sOff := e.offset

				if sOff+nHash <= len(source) && matchLen(source[sOff:], target[tPos:]) >= nHash {
					ml := matchLen(source[sOff:], target[tPos:])
					if ml > bestLen {
						bestLen = ml
						bestOff = sOff
					}
				}
				ei = e.next + 1
			}
		}

		if bestLen >= nHash {
			flushInsert()
			buf = appendInt(buf, uint64(bestOff))
			buf = append(buf, '@')
			buf = appendInt(buf, uint64(bestLen))
			buf = append(buf, ',')
			tPos += bestLen
		} else {
			pendingInsert = append(pendingInsert, target[tPos])
			tPos++
		}
	}

	flushInsert()
	buf = appendInt(buf, uint64(Checksum(target)))
	buf = append(buf, ';')
	return buf
}

func createInsertAll(target []byte) []byte {
	var buf []byte
	buf = appendInt(buf, uint64(len(target)))
	buf = append(buf, '\n')
	buf = appendInt(buf, uint64(len(target)))
	buf = append(buf, ':')
	buf = append(buf, target...)
	buf = appendInt(buf, uint64(Checksum(target)))
	buf = append(buf, ';')
	return buf
}

func appendInt(buf []byte, v uint64) []byte {
	if v == 0 {
		return append(buf, '0')
	}
	var tmp [13]byte
	i := len(tmp)
	for v > 0 {
		i--
		tmp[i] = zDigitsEnc[v&0x3f]
		v >>= 6
	}
	return append(buf, tmp[i:]...)
}

func rollingHash(data []byte) uint32 {
	var h uint32
	for _, b := range data {
		h = h*37 + uint32(b)
	}
	return h
}

func matchLen(a, b []byte) int {
	n := len(a)
	if len(b) < n {
		n = len(b)
	}
	for i := 0; i < n; i++ {
		if a[i] != b[i] {
			return i
		}
	}
	return n
}
