# TigerStyle Hardening Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Apply TigerStyle discipline (assertions, function splitting, error handling, bounds checks) across all 20 go-libfossil packages with zero new features.

**Architecture:** Leaf-first processing order ensures each package's dependencies are hardened before it is. Within each package, all 4 categories (assertions, splits, errors, bounds) are applied before moving on. Every change is tested before committing.

**Tech Stack:** Go 1.22+, `go test`, `-buildvcs=false` (dual VCS)

**Spec:** `docs/superpowers/specs/2026-03-17-tigerstyle-hardening-design.md`

**Working directory:** `/Users/dmestas/projects/EdgeSync/.worktrees/feat-tigerstyle`

**Test command:** `go test -buildvcs=false -count=1 ./go-libfossil/...`

**Assertion pattern (precondition):**
```go
if param == nil { panic("pkg.Func: param must not be nil") }
```

**Assertion pattern (postcondition — critical path only):**
```go
func Foo(x int) (result []byte, err error) {
    if x <= 0 { panic("pkg.Foo: x must be positive") }
    defer func() {
        if err == nil && len(result) == 0 { panic("pkg.Foo: result must not be empty on success") }
    }()
    // ...
}
```

---

## Chunk 1: Core Data Path (hash, delta, blob, content)

### Task 1: hash — Assertions

**Files:**
- Modify: `go-libfossil/hash/hash.go`
- Create: `go-libfossil/hash/assert_test.go`

- [ ] **Step 1: Add precondition assertions to hash.go**

```go
// hash.go — add at top of each function:

func SHA1(data []byte) string {
	if data == nil { panic("hash.SHA1: data must not be nil") }
	h := sha1.Sum(data)
	return hex.EncodeToString(h[:])
}

func SHA3(data []byte) string {
	if data == nil { panic("hash.SHA3: data must not be nil") }
	h := sha3.Sum256(data)
	return hex.EncodeToString(h[:])
}

func HashSize(hashType string) int {
	if hashType == "" { panic("hash.HashSize: hashType must not be empty") }
	// ... existing switch
}

func IsValidHash(h string) bool {
	if h == "" { panic("hash.IsValidHash: h must not be empty") }
	// ... existing logic
}
```

- [ ] **Step 2: Write assertion-trigger tests**

```go
// hash/assert_test.go
package hash

import "testing"

func TestSHA1NilPanics(t *testing.T) {
	defer func() {
		if r := recover(); r == nil {
			t.Fatal("expected panic on nil data")
		}
	}()
	SHA1(nil)
}

func TestSHA3NilPanics(t *testing.T) {
	defer func() {
		if r := recover(); r == nil {
			t.Fatal("expected panic on nil data")
		}
	}()
	SHA3(nil)
}

func TestHashSizeEmptyPanics(t *testing.T) {
	defer func() {
		if r := recover(); r == nil {
			t.Fatal("expected panic on empty hashType")
		}
	}()
	HashSize("")
}

func TestIsValidHashEmptyPanics(t *testing.T) {
	defer func() {
		if r := recover(); r == nil {
			t.Fatal("expected panic on empty hash")
		}
	}()
	IsValidHash("")
}
```

- [ ] **Step 3: Run tests**

Run: `go test -buildvcs=false -count=1 ./go-libfossil/hash/`
Expected: All PASS

- [ ] **Step 4: Commit**

```bash
git add go-libfossil/hash/
git commit -m "tigerstyle(hash): add precondition assertions to all functions"
```

---

### Task 2: delta — Assertions, function split, bounds, naming

**Files:**
- Modify: `go-libfossil/delta/delta.go`
- Create: `go-libfossil/delta/assert_test.go`

- [ ] **Step 1: Add C-reference mapping comments and precondition assertions**

At top of `Create()`, add mapping comment:
```go
// Variable naming follows fossil/src/delta.c for cross-reference:
//   nHash = NHASH (rolling hash window size)
//   ei    = (entry index into hash table)
//   ml    = (match length)
//   tPos  = iSrc (target position)
//   sOff  = iSrc (source offset)
```

At top of `Apply()`, add mapping comment:
```go
// Variable naming follows fossil/src/delta.c for cross-reference:
//   cnt    = cnt (byte count for copy/insert)
//   offset = ofst (source offset for copy command)
```

Add preconditions to all functions:
```go
func Apply(source, delta []byte) (result []byte, err error) {
	if source == nil { panic("delta.Apply: source must not be nil") }
	if len(delta) == 0 {
		return nil, fmt.Errorf("%w: empty delta", ErrInvalidDelta)
	}
	// postcondition
	defer func() {
		if err == nil && result == nil { panic("delta.Apply: result must not be nil on success") }
	}()
	// ... existing logic (remove old empty-delta check since precondition now handles nil)
}

func Create(source, target []byte) (result []byte) {
	if source == nil { panic("delta.Create: source must not be nil") }
	if target == nil { panic("delta.Create: target must not be nil") }
	defer func() {
		if len(result) == 0 { panic("delta.Create: result must not be empty") }
	}()
	// ... existing logic
}

func Checksum(data []byte) uint32 {
	if data == nil { panic("delta.Checksum: data must not be nil") }
	// ... existing logic
}

func (r *reader) getInt() (uint64, error) {
	if r == nil { panic("delta.reader.getInt: r must not be nil") }
	// ... existing logic
}

func (r *reader) getChar() (byte, error) {
	if r == nil { panic("delta.reader.getChar: r must not be nil") }
	// ... existing logic
}

func createInsertAll(target []byte) []byte {
	if len(target) == 0 { panic("delta.createInsertAll: target must not be empty") }
	// ... existing logic
}

func rollingHash(data []byte) uint32 {
	if len(data) == 0 { panic("delta.rollingHash: data must not be empty") }
	// ... existing logic
}

func matchLen(a, b []byte) int {
	if a == nil { panic("delta.matchLen: a must not be nil") }
	if b == nil { panic("delta.matchLen: b must not be nil") }
	// ... existing logic
}
```

- [ ] **Step 2: Add bounds assertions in Apply for uint64→int casts**

In `Apply()`, replace:
```go
if int(offset+cnt) > len(source) {
```
with:
```go
if offset > uint64(len(source)) || cnt > uint64(len(source)) {
	return nil, fmt.Errorf("%w: copy offset/count overflow", ErrInvalidDelta)
}
if int(offset+cnt) > len(source) {
```

And replace:
```go
if r.pos+int(cnt) > len(r.data) {
```
with:
```go
if cnt > uint64(len(r.data)) {
	return nil, fmt.Errorf("%w: insert count overflow", ErrInvalidDelta)
}
if r.pos+int(cnt) > len(r.data) {
```

- [ ] **Step 3: Split Create() into buildHashTable + emitMatches**

Extract hash table construction (lines 159-183) and match loop (lines 201-235) into helpers:

```go
type hashEntry struct {
	offset int
	next   int
}

// buildHashTable builds the rolling-hash lookup table for the source.
func buildHashTable(source []byte) (heads []int, entries []hashEntry, mask int) {
	if len(source) < 16 { panic("delta.buildHashTable: source too short for hashing") }
	const nHash = 16

	tableSize := len(source) / nHash
	if tableSize < 64 {
		tableSize = 64
	}
	for tableSize&(tableSize-1) != 0 {
		tableSize &= tableSize - 1
	}
	tableSize <<= 1
	mask = tableSize - 1

	heads = make([]int, tableSize)
	entries = make([]hashEntry, 0, len(source)/nHash)

	for i := 0; i+nHash <= len(source); i += nHash {
		h := rollingHash(source[i : i+nHash])
		idx := int(h) & mask
		entries = append(entries, hashEntry{offset: i, next: heads[idx] - 1})
		heads[idx] = len(entries)
	}
	return heads, entries, mask
}

// emitMatches scans target against the hash table, emitting copy/insert commands.
func emitMatches(source, target []byte, heads []int, entries []hashEntry, mask int) []byte {
	if len(target) == 0 { panic("delta.emitMatches: target must not be empty") }
	const nHash = 16

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
```

Then `Create()` becomes:
```go
func Create(source, target []byte) (result []byte) {
	if source == nil { panic("delta.Create: source must not be nil") }
	if target == nil { panic("delta.Create: target must not be nil") }
	defer func() {
		if len(result) == 0 { panic("delta.Create: result must not be empty") }
	}()

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

	heads, entries, mask := buildHashTable(source)
	return emitMatches(source, target, heads, entries, mask)
}
```

- [ ] **Step 4: Write assertion-trigger tests**

```go
// delta/assert_test.go
package delta

import "testing"

func TestApplyNilSourcePanics(t *testing.T) {
	defer func() {
		if r := recover(); r == nil {
			t.Fatal("expected panic on nil source")
		}
	}()
	Apply(nil, []byte("dummy"))
}

func TestCreateNilSourcePanics(t *testing.T) {
	defer func() {
		if r := recover(); r == nil {
			t.Fatal("expected panic on nil source")
		}
	}()
	Create(nil, []byte("target"))
}

func TestCreateNilTargetPanics(t *testing.T) {
	defer func() {
		if r := recover(); r == nil {
			t.Fatal("expected panic on nil target")
		}
	}()
	Create([]byte("source"), nil)
}

func TestChecksumNilPanics(t *testing.T) {
	defer func() {
		if r := recover(); r == nil {
			t.Fatal("expected panic on nil data")
		}
	}()
	Checksum(nil)
}
```

- [ ] **Step 5: Run tests**

Run: `go test -buildvcs=false -count=1 ./go-libfossil/delta/`
Expected: All PASS (existing + new assertion tests)

- [ ] **Step 6: Commit**

```bash
git add go-libfossil/delta/
git commit -m "tigerstyle(delta): assertions, bounds checks, function split, C-reference comments"
```

---

### Task 3: blob (incl. compress) — Assertions, postconditions, error fix

**Files:**
- Modify: `go-libfossil/blob/blob.go`
- Modify: `go-libfossil/blob/compress.go`
- Create: `go-libfossil/blob/assert_test.go`

- [ ] **Step 1: Add assertions to blob.go with postconditions**

```go
func Store(q db.Querier, content []byte) (rid libfossil.FslID, uuid string, err error) {
	if q == nil { panic("blob.Store: q must not be nil") }
	if len(content) == 0 { panic("blob.Store: content must not be empty") }
	defer func() {
		if err == nil && rid <= 0 { panic("blob.Store: rid must be positive on success") }
		if err == nil && uuid == "" { panic("blob.Store: uuid must not be empty on success") }
	}()
	// ... existing logic (remove old variable declarations, use named returns)
}

func StoreDelta(q db.Querier, content []byte, srcRid libfossil.FslID) (rid libfossil.FslID, uuid string, err error) {
	if q == nil { panic("blob.StoreDelta: q must not be nil") }
	if len(content) == 0 { panic("blob.StoreDelta: content must not be empty") }
	if srcRid <= 0 { panic("blob.StoreDelta: srcRid must be positive") }
	defer func() {
		if err == nil && rid <= 0 { panic("blob.StoreDelta: rid must be positive on success") }
	}()
	// ... existing logic
}

func StorePhantom(q db.Querier, uuid string) (rid libfossil.FslID, err error) {
	if q == nil { panic("blob.StorePhantom: q must not be nil") }
	if uuid == "" { panic("blob.StorePhantom: uuid must not be empty") }
	defer func() {
		if err == nil && rid <= 0 { panic("blob.StorePhantom: rid must be positive on success") }
	}()
	// ... existing logic
}

func Load(q db.Querier, rid libfossil.FslID) (result []byte, err error) {
	if q == nil { panic("blob.Load: q must not be nil") }
	if rid <= 0 { panic("blob.Load: rid must be positive") }
	defer func() {
		if err == nil && result == nil { panic("blob.Load: result must not be nil on success") }
	}()
	// ... existing logic
}

func Exists(q db.Querier, uuid string) (libfossil.FslID, bool) {
	if q == nil { panic("blob.Exists: q must not be nil") }
	if uuid == "" { panic("blob.Exists: uuid must not be empty") }
	// ... existing logic
}
```

- [ ] **Step 2: Fix binary.Write error + add assertions in compress.go**

```go
func Compress(data []byte) (result []byte, err error) {
	if data == nil { panic("blob.Compress: data must not be nil") }
	defer func() {
		if err == nil && len(result) == 0 { panic("blob.Compress: result must not be empty on success") }
	}()
	var buf bytes.Buffer
	if err := binary.Write(&buf, binary.BigEndian, uint32(len(data))); err != nil {
		return nil, fmt.Errorf("write size prefix: %w", err)
	}
	// ... rest of existing logic
}

func Decompress(data []byte) (result []byte, err error) {
	if data == nil { panic("blob.Decompress: data must not be nil") }
	defer func() {
		if err == nil && result == nil { panic("blob.Decompress: result must not be nil on success") }
	}()
	if len(data) < 5 {
		return nil, fmt.Errorf("zlib decompress: data too short (%d bytes)", len(data))
	}
	// ... rest of existing logic
}
```

- [ ] **Step 3: Write assertion-trigger tests**

```go
// blob/assert_test.go
package blob_test

import (
	"testing"
	"github.com/dmestas/edgesync/go-libfossil/blob"
)

func TestStoreNilQuerierPanics(t *testing.T) {
	defer func() {
		if r := recover(); r == nil {
			t.Fatal("expected panic on nil querier")
		}
	}()
	blob.Store(nil, []byte("data"))
}

// Note: TestStoreEmptyContentPanics requires a non-nil Querier mock to get past
// the nil check. Use a testutil helper or the repo.Create() test setup to provide
// a real DB querier, then call blob.Store(q, []byte{}) to trigger the empty-content panic.

func TestLoadNilQuerierPanics(t *testing.T) {
	defer func() {
		if r := recover(); r == nil {
			t.Fatal("expected panic on nil querier")
		}
	}()
	blob.Load(nil, 1)
}

func TestLoadZeroRidPanics(t *testing.T) {
	defer func() {
		if r := recover(); r == nil {
			t.Fatal("expected panic on zero rid")
		}
	}()
	blob.Load(nil, 0)
}

func TestCompressNilPanics(t *testing.T) {
	defer func() {
		if r := recover(); r == nil {
			t.Fatal("expected panic on nil data")
		}
	}()
	blob.Compress(nil)
}

func TestDecompressNilPanics(t *testing.T) {
	defer func() {
		if r := recover(); r == nil {
			t.Fatal("expected panic on nil data")
		}
	}()
	blob.Decompress(nil)
}
```

- [ ] **Step 4: Run tests**

Run: `go test -buildvcs=false -count=1 ./go-libfossil/blob/`
Expected: All PASS

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/blob/
git commit -m "tigerstyle(blob): assertions, postconditions, fix binary.Write error handling"
```

---

### Task 4: content — Assertions, postconditions

**Files:**
- Modify: `go-libfossil/content/content.go`
- Create: `go-libfossil/content/assert_test.go`

- [ ] **Step 1: Add assertions and postconditions**

```go
func Expand(q db.Querier, rid libfossil.FslID) (result []byte, err error) {
	if q == nil { panic("content.Expand: q must not be nil") }
	// rid <= 0 check already exists as error return — upgrade to assertion
	if rid <= 0 { panic("content.Expand: rid must be positive") }
	defer func() {
		if err == nil && result == nil { panic("content.Expand: result must not be nil on success") }
	}()
	// ... remove old rid <= 0 error check, rest of logic unchanged
}

func walkDeltaChain(q db.Querier, rid libfossil.FslID) (chain []libfossil.FslID, err error) {
	if q == nil { panic("content.walkDeltaChain: q must not be nil") }
	if rid <= 0 { panic("content.walkDeltaChain: rid must be positive") }
	defer func() {
		if err == nil && len(chain) == 0 { panic("content.walkDeltaChain: chain must not be empty on success") }
	}()
	// ... existing logic with named returns
}

func Verify(q db.Querier, rid libfossil.FslID) (err error) {
	if q == nil { panic("content.Verify: q must not be nil") }
	if rid <= 0 { panic("content.Verify: rid must be positive") }
	// postcondition: on nil error, hash was verified
	// ... existing logic
}

func IsPhantom(q db.Querier, rid libfossil.FslID) (bool, error) {
	if q == nil { panic("content.IsPhantom: q must not be nil") }
	if rid <= 0 { panic("content.IsPhantom: rid must be positive") }
	// ... existing logic
}
```

Also rename `srcid` to `sourceID` in `walkDeltaChain`:
```go
var sourceID int64
err := q.QueryRow("SELECT srcid FROM delta WHERE rid=?", current).Scan(&sourceID)
```

- [ ] **Step 2: Write assertion-trigger tests**

```go
// content/assert_test.go
package content_test

import (
	"testing"
	"github.com/dmestas/edgesync/go-libfossil/content"
)

func TestExpandNilQuerierPanics(t *testing.T) {
	defer func() {
		if r := recover(); r == nil {
			t.Fatal("expected panic on nil querier")
		}
	}()
	content.Expand(nil, 1)
}

func TestExpandZeroRidPanics(t *testing.T) {
	defer func() {
		if r := recover(); r == nil {
			t.Fatal("expected panic on zero rid")
		}
	}()
	content.Expand(nil, 0)
}

func TestVerifyNilQuerierPanics(t *testing.T) {
	defer func() {
		if r := recover(); r == nil {
			t.Fatal("expected panic on nil querier")
		}
	}()
	content.Verify(nil, 1)
}

func TestIsPhantomNilQuerierPanics(t *testing.T) {
	defer func() {
		if r := recover(); r == nil {
			t.Fatal("expected panic on nil querier")
		}
	}()
	content.IsPhantom(nil, 1)
}
```

- [ ] **Step 3: Run tests**

Run: `go test -buildvcs=false -count=1 ./go-libfossil/content/`
Expected: All PASS

- [ ] **Step 4: Commit**

```bash
git add go-libfossil/content/
git commit -m "tigerstyle(content): assertions, postconditions, rename srcid to sourceID"
```

---

## Chunk 2: Protocol Layer (deck, xfer)

### Task 5: deck — Assertions, function splits (Marshal + parseCard), error fix

**Files:**
- Modify: `go-libfossil/deck/marshal.go`
- Modify: `go-libfossil/deck/parse.go`
- Create: `go-libfossil/deck/assert_test.go`

- [ ] **Step 1: Split Marshal() — extract marshalCards()**

Keep `Marshal()` as the orchestrator (Z-card computation + delegation). Extract the per-card marshaling into `marshalCards()`:

```go
func (d *Deck) Marshal() ([]byte, error) {
	if d == nil { panic("deck.Marshal: d must not be nil") }
	var b strings.Builder
	marshalCards(&b, d)
	body := b.String()
	zHash := computeZ([]byte(body))
	return []byte(fmt.Sprintf("%sZ %s\n", body, zHash)), nil
}

// marshalCards writes all cards in strict ASCII order to b.
func marshalCards(b *strings.Builder, d *Deck) {
	if b == nil { panic("deck.marshalCards: b must not be nil") }
	if d == nil { panic("deck.marshalCards: d must not be nil") }

	if d.A != nil {
		b.WriteString("A ")
		b.WriteString(FossilEncode(d.A.Filename))
		b.WriteString(" ")
		b.WriteString(d.A.Target)
		if d.A.Source != "" {
			b.WriteString(" ")
			b.WriteString(d.A.Source)
		}
		b.WriteString("\n")
	}

	if d.B != "" {
		fmt.Fprintf(b, "B %s\n", d.B)
	}
	if d.C != "" {
		fmt.Fprintf(b, "C %s\n", FossilEncode(d.C))
	}
	if !d.D.IsZero() {
		fmt.Fprintf(b, "D %s\n", d.D.UTC().Format("2006-01-02T15:04:05.000"))
	}
	if d.E != nil {
		fmt.Fprintf(b, "E %s %s\n", d.E.Date.UTC().Format("2006-01-02T15:04:05"), d.E.UUID)
	}

	if len(d.F) > 0 {
		sorted := make([]FileCard, len(d.F))
		copy(sorted, d.F)
		sort.Slice(sorted, func(i, j int) bool { return sorted[i].Name < sorted[j].Name })
		for _, f := range sorted {
			b.WriteString("F ")
			b.WriteString(FossilEncode(f.Name))
			if f.UUID != "" {
				b.WriteString(" ")
				b.WriteString(f.UUID)
				if f.Perm != "" {
					b.WriteString(" ")
					b.WriteString(f.Perm)
				}
			}
			if f.OldName != "" {
				b.WriteString(" ")
				b.WriteString(FossilEncode(f.OldName))
			}
			b.WriteString("\n")
		}
	}

	if d.G != "" { fmt.Fprintf(b, "G %s\n", d.G) }
	if d.H != "" { fmt.Fprintf(b, "H %s\n", FossilEncode(d.H)) }
	if d.I != "" { fmt.Fprintf(b, "I %s\n", d.I) }

	for _, j := range d.J {
		if j.Value != "" {
			fmt.Fprintf(b, "J %s %s\n", FossilEncode(j.Name), j.Value)
		} else {
			fmt.Fprintf(b, "J %s\n", FossilEncode(j.Name))
		}
	}

	if d.K != "" { fmt.Fprintf(b, "K %s\n", d.K) }
	if d.L != "" { fmt.Fprintf(b, "L %s\n", FossilEncode(d.L)) }

	if len(d.M) > 0 {
		sorted := make([]string, len(d.M))
		copy(sorted, d.M)
		sort.Strings(sorted)
		for _, m := range sorted {
			fmt.Fprintf(b, "M %s\n", m)
		}
	}

	if d.N != "" { fmt.Fprintf(b, "N %s\n", d.N) }
	if len(d.P) > 0 { fmt.Fprintf(b, "P %s\n", strings.Join(d.P, " ")) }

	for _, q := range d.Q {
		prefix := "+"
		if q.IsBackout { prefix = "-" }
		if q.Baseline != "" {
			fmt.Fprintf(b, "Q %s%s %s\n", prefix, q.Target, q.Baseline)
		} else {
			fmt.Fprintf(b, "Q %s%s\n", prefix, q.Target)
		}
	}

	if d.R != "" { fmt.Fprintf(b, "R %s\n", d.R) }

	if len(d.T) > 0 {
		sorted := make([]TagCard, len(d.T))
		copy(sorted, d.T)
		sort.Slice(sorted, func(i, j int) bool {
			ki := string(sorted[i].Type) + sorted[i].Name + sorted[i].UUID
			kj := string(sorted[j].Type) + sorted[j].Name + sorted[j].UUID
			return ki < kj
		})
		for _, tag := range sorted {
			fmt.Fprintf(b, "T %c%s %s", tag.Type, tag.Name, tag.UUID)
			if tag.Value != "" {
				fmt.Fprintf(b, " %s", tag.Value)
			}
			b.WriteString("\n")
		}
	}

	if d.U != "" { fmt.Fprintf(b, "U %s\n", FossilEncode(d.U)) }
	if len(d.W) > 0 { fmt.Fprintf(b, "W %d\n%s\n", len(d.W), d.W) }
}
```

- [ ] **Step 2: Split parseCard() into per-type helpers + fix reader.Read error**

Replace the big switch with dispatcher calling per-type helpers. Also fix the `reader.Read()` error at line 40:

```go
func parseCard(d *Deck, card byte, args string) error {
	if d == nil { panic("deck.parseCard: d must not be nil") }
	switch card {
	case 'A': return parseACard(d, args)
	case 'B': d.B = strings.TrimSpace(args); return nil
	case 'C': d.C = FossilDecode(args); return nil
	case 'D': return parseDCard(d, args)
	case 'E': return parseECard(d, args)
	case 'F': return parseFCard(d, args)
	case 'G': d.G = strings.TrimSpace(args); return nil
	case 'H': d.H = FossilDecode(args); return nil
	case 'I': d.I = strings.TrimSpace(args); return nil
	case 'J': return parseJCard(d, args)
	case 'K': d.K = strings.TrimSpace(args); return nil
	case 'L': d.L = FossilDecode(args); return nil
	case 'M': d.M = append(d.M, strings.TrimSpace(args)); return nil
	case 'N': d.N = strings.TrimSpace(args); return nil
	case 'P': d.P = strings.Fields(args); return nil
	case 'Q': return parseQCard(d, args)
	case 'R': d.R = strings.TrimSpace(args); return nil
	case 'T': return parseTCard(d, args)
	case 'U': d.U = FossilDecode(args); return nil
	default:  return fmt.Errorf("unknown card '%c'", card)
	}
}

func parseACard(d *Deck, args string) error { /* existing A-card logic */ }
func parseDCard(d *Deck, args string) error { /* existing D-card logic */ }
func parseECard(d *Deck, args string) error { /* existing E-card logic */ }
func parseFCard(d *Deck, args string) error { /* existing F-card logic */ }
func parseJCard(d *Deck, args string) error { /* existing J-card logic */ }
func parseQCard(d *Deck, args string) error { /* existing Q-card logic */ }
func parseTCard(d *Deck, args string) error { /* existing T-card logic */ }
```

Fix the `reader.Read()` error in `Parse()`:
```go
// Replace line 40:
n, _ := reader.Read(content)
// With:
n, readErr := reader.Read(content)
if readErr != nil && n != size {
	return nil, fmt.Errorf("deck.Parse: W content read: %w", readErr)
}
```

Add assertion to `Parse()`:
```go
func Parse(data []byte) (*Deck, error) {
	if data == nil { panic("deck.Parse: data must not be nil") }
	// VerifyZ already asserts len(data) >= 35
	// ... existing logic
}
```

Add comment noting VerifyZ guard:
```go
// body is safe to slice: VerifyZ above guarantees len(data) >= 35.
body := data[:len(data)-35]
```

- [ ] **Step 3: Add assertions to encode.go and deck.go types**

```go
// encode.go
func FossilEncode(s string) string {
	if s == "" { panic("deck.FossilEncode: s must not be empty") }
	// ... existing logic
}

func FossilDecode(s string) string {
	if s == "" { panic("deck.FossilDecode: s must not be empty") }
	// ... existing logic
}
```

**IMPORTANT:** Before adding these assertions, run `rg 'FossilDecode\(""\)' go-libfossil/` and `rg 'FossilEncode\(""\)' go-libfossil/` to check if empty strings are passed legitimately. If so, skip these assertions — they would be operating errors, not programmer errors.

- [ ] **Step 4: Write assertion tests**

```go
// deck/assert_test.go
package deck

import "testing"

func TestParseNilPanics(t *testing.T) {
	defer func() {
		if r := recover(); r == nil {
			t.Fatal("expected panic on nil data")
		}
	}()
	Parse(nil)
}

func TestMarshalNilPanics(t *testing.T) {
	defer func() {
		if r := recover(); r == nil {
			t.Fatal("expected panic on nil deck")
		}
	}()
	var d *Deck
	d.Marshal()
}
```

- [ ] **Step 5: Run tests**

Run: `go test -buildvcs=false -count=1 ./go-libfossil/deck/`
Expected: All PASS

- [ ] **Step 6: Commit**

```bash
git add go-libfossil/deck/
git commit -m "tigerstyle(deck): split Marshal+parseCard, fix reader.Read error, assertions"
```

---

### Task 6: xfer — Assertions

**Files:**
- Modify: `go-libfossil/xfer/decode.go`
- Modify: `go-libfossil/xfer/encode.go`
- Modify: `go-libfossil/xfer/message.go`

- [ ] **Step 1: Add assertions to decode.go, encode.go, message.go**

```go
// decode.go
func readPayload(r *bufio.Reader, size int) ([]byte, error) {
	if r == nil { panic("xfer.readPayload: r must not be nil") }
	if size < 0 { panic("xfer.readPayload: size must not be negative") }
	// ... existing logic
}

func DecodeCard(r *bufio.Reader) (Card, error) {
	if r == nil { panic("xfer.DecodeCard: r must not be nil") }
	// ... existing logic
}

// encode.go
func EncodeCard(w *bytes.Buffer, c Card) error {
	if w == nil { panic("xfer.EncodeCard: w must not be nil") }
	if c == nil { panic("xfer.EncodeCard: c must not be nil") }
	// ... existing logic
}

// message.go — Encode and Decode
func (m *Message) Encode() ([]byte, error) {
	if m == nil { panic("xfer.Message.Encode: m must not be nil") }
	// ... existing logic
}
```

- [ ] **Step 2: Run tests**

Run: `go test -buildvcs=false -count=1 ./go-libfossil/xfer/`
Expected: All PASS

- [ ] **Step 3: Commit**

```bash
git add go-libfossil/xfer/
git commit -m "tigerstyle(xfer): add precondition assertions to decode/encode/message"
```

---

## Chunk 3: Infrastructure + Manifest (db, repo, manifest)

### Task 7: db — Assertions, rollback error

**Files:**
- Modify: `go-libfossil/db/db.go`
- Modify: `go-libfossil/db/schema.go`

- [ ] **Step 1: Add assertions to db.go and fix Rollback error**

```go
// db.go
func OpenWith(path string, cfg OpenConfig) (*DB, error) {
	if path == "" { panic("db.OpenWith: path must not be empty") }
	// ... existing logic
}

func (d *DB) WithTx(fn func(tx *Tx) error) error {
	if d == nil { panic("db.WithTx: d must not be nil") }
	if fn == nil { panic("db.WithTx: fn must not be nil") }
	// ... existing begin
	defer func() {
		if rbErr := sqlTx.Rollback(); rbErr != nil && rbErr != sql.ErrTxDone {
			// Log rollback failure — can't return, already in error path
			fmt.Fprintf(os.Stderr, "db.WithTx: rollback failed: %v\n", rbErr)
		}
	}()
	// ... existing logic
}

func (d *DB) SqlDB() *sql.DB {
	if d == nil { panic("db.SqlDB: d must not be nil") }
	return d.sqlDB
}
```

- [ ] **Step 2: Add assertions to schema.go**

```go
func CreateRepoSchema(d *DB) error {
	if d == nil { panic("db.CreateRepoSchema: d must not be nil") }
	// ... existing logic
}

func SeedUser(d *DB, login string) error {
	if d == nil { panic("db.SeedUser: d must not be nil") }
	if login == "" { panic("db.SeedUser: login must not be empty") }
	// ... existing logic
}

func SeedConfig(d *DB, rng simio.Rand) error {
	if d == nil { panic("db.SeedConfig: d must not be nil") }
	if rng == nil { panic("db.SeedConfig: rng must not be nil") }
	// ... existing logic
}
```

- [ ] **Step 3: Run tests**

Run: `go test -buildvcs=false -count=1 ./go-libfossil/db/`
Expected: All PASS

- [ ] **Step 4: Commit**

```bash
git add go-libfossil/db/
git commit -m "tigerstyle(db): assertions, fix Rollback error logging"
```

---

### Task 8: repo — Assertions, os.Remove error handling

**Files:**
- Modify: `go-libfossil/repo/repo.go`

- [ ] **Step 1: Add assertions and handle os.Remove errors**

```go
func Create(path string, rng simio.Rand) (*Repo, error) {
	if path == "" { panic("repo.Create: path must not be empty") }
	if rng == nil { panic("repo.Create: rng must not be nil") }
	// ... existing logic
	// Replace silent os.Remove calls with logged cleanup:
	// Line 30: os.Remove(path) → if rmErr := os.Remove(path); rmErr != nil { /* cleanup best-effort, wrap in returned error */ }
}

func Open(path string) (*Repo, error) {
	if path == "" { panic("repo.Open: path must not be empty") }
	// ... existing logic
}

func (r *Repo) Verify() error {
	if r == nil { panic("repo.Verify: r must not be nil") }
	// ... existing logic
}
```

For `os.Remove` in `Create()` cleanup paths, wrap the cleanup error into the primary error:
```go
// Example for one cleanup location:
if err != nil {
	if rmErr := os.Remove(path); rmErr != nil {
		return nil, fmt.Errorf("repo.Create: %w (cleanup failed: %v)", err, rmErr)
	}
	return nil, fmt.Errorf("repo.Create: %w", err)
}
```

- [ ] **Step 2: Run tests**

Run: `go test -buildvcs=false -count=1 ./go-libfossil/repo/`
Expected: All PASS

- [ ] **Step 3: Commit**

```bash
git add go-libfossil/repo/
git commit -m "tigerstyle(repo): assertions, handle os.Remove cleanup errors"
```

---

### Task 9: manifest — Assertions, postconditions, function split, error fixes

**Files:**
- Modify: `go-libfossil/manifest/manifest.go`
- Modify: `go-libfossil/manifest/log.go`

This is the biggest single task — `Checkin()` at 135 lines needs splitting AND has 4 ignored `tx.Exec()` errors.

- [ ] **Step 1: Fix all tx.Exec errors in Checkin (lines 151-158)**

Replace:
```go
tx.Exec("INSERT OR IGNORE INTO leaf(rid) VALUES(?)", manifestRid)
if opts.Parent > 0 {
	tx.Exec("DELETE FROM leaf WHERE rid=?", opts.Parent)
}
tx.Exec("INSERT OR IGNORE INTO unclustered(rid) VALUES(?)", manifestRid)
tx.Exec("INSERT OR IGNORE INTO unsent(rid) VALUES(?)", manifestRid)
```

With:
```go
if _, err := tx.Exec("INSERT OR IGNORE INTO leaf(rid) VALUES(?)", manifestRid); err != nil {
	return fmt.Errorf("leaf insert: %w", err)
}
if opts.Parent > 0 {
	if _, err := tx.Exec("DELETE FROM leaf WHERE rid=?", opts.Parent); err != nil {
		return fmt.Errorf("leaf delete parent: %w", err)
	}
}
if _, err := tx.Exec("INSERT OR IGNORE INTO unclustered(rid) VALUES(?)", manifestRid); err != nil {
	return fmt.Errorf("unclustered: %w", err)
}
if _, err := tx.Exec("INSERT OR IGNORE INTO unsent(rid) VALUES(?)", manifestRid); err != nil {
	return fmt.Errorf("unsent: %w", err)
}
```

- [ ] **Step 2: Split Checkin into helpers**

Extract from the WithTx closure:

```go
func Checkin(r *repo.Repo, opts CheckinOpts) (manifestRid libfossil.FslID, manifestUUID string, err error) {
	if r == nil { panic("manifest.Checkin: r must not be nil") }
	if len(opts.Files) == 0 { panic("manifest.Checkin: opts.Files must not be empty") }
	if opts.User == "" { panic("manifest.Checkin: opts.User must not be empty") }
	defer func() {
		if err == nil && manifestRid <= 0 { panic("manifest.Checkin: manifestRid must be positive on success") }
	}()
	if opts.Time.IsZero() {
		opts.Time = time.Now().UTC()
	}

	err = r.WithTx(func(tx *db.Tx) error {
		fCards, err := storeFileBlobs(tx, opts.Files)
		if err != nil {
			return err
		}
		d, err := buildCheckinDeck(tx, opts, fCards)
		if err != nil {
			return err
		}
		manifestRid, manifestUUID, err = insertCheckinBlob(tx, d)
		if err != nil {
			return err
		}
		if err := insertMlinks(tx, opts, fCards, manifestRid); err != nil {
			return err
		}
		return markLeafAndEvent(tx, opts, manifestRid)
	})
	if err != nil {
		return 0, "", fmt.Errorf("manifest.Checkin: %w", err)
	}
	return manifestRid, manifestUUID, nil
}

func storeFileBlobs(tx *db.Tx, files []File) ([]deck.FileCard, error) { /* store blobs, return fCards */ }
func buildCheckinDeck(tx *db.Tx, opts CheckinOpts, fCards []deck.FileCard) (*deck.Deck, error) { /* parent, tags, delta, R-card */ }
func insertCheckinBlob(tx *db.Tx, d *deck.Deck) (libfossil.FslID, string, error) { /* marshal + store */ }
func insertMlinks(tx *db.Tx, opts CheckinOpts, fCards []deck.FileCard, manifestRid libfossil.FslID) error { /* filename + mlink */ }
func markLeafAndEvent(tx *db.Tx, opts CheckinOpts, manifestRid libfossil.FslID) error { /* leaf, event, plink, unclustered, unsent */ }
```

- [ ] **Step 3: Add assertions to other functions + fix log.go scan error**

```go
func GetManifest(r *repo.Repo, rid libfossil.FslID) (result *deck.Deck, err error) {
	if r == nil { panic("manifest.GetManifest: r must not be nil") }
	if rid <= 0 { panic("manifest.GetManifest: rid must be positive") }
	defer func() {
		if err == nil && result == nil { panic("manifest.GetManifest: result must not be nil on success") }
	}()
	// ... existing logic
}

func ensureFilename(tx *db.Tx, name string) (int64, error) {
	if tx == nil { panic("manifest.ensureFilename: tx must not be nil") }
	if name == "" { panic("manifest.ensureFilename: name must not be empty") }
	// ... existing logic
}
```

In `log.go`, fix the scan error and rename `mtimeRaw` to `mtimeScanned`:

```go
func Log(r *repo.Repo, opts LogOpts) ([]LogEntry, error) {
	if r == nil { panic("manifest.Log: r must not be nil") }
	// ... existing rid check
	// rename mtimeRaw -> mtimeScanned
	var mtimeScanned any
	// ...
	// Fix scan-in-loop:
	for rows.Next() {
		var puuid string
		if err := rows.Scan(&puuid); err != nil {
			continue // skip bad row
		}
		parents = append(parents, puuid)
	}
}
```

- [ ] **Step 4: Run tests**

Run: `go test -buildvcs=false -count=1 ./go-libfossil/manifest/`
Expected: All PASS

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/manifest/
git commit -m "tigerstyle(manifest): split Checkin, fix 4 tx.Exec errors, fix scan error, assertions"
```

---

## Chunk 4: Merge + Sync

### Task 10: merge — Assertions, function split (merge3), error fixes

**Files:**
- Modify: `go-libfossil/merge/threeway.go`
- Modify: `go-libfossil/merge/ancestor.go`
- Modify: `go-libfossil/merge/detect.go`
- Modify: `go-libfossil/merge/resolve.go`
- Modify: `go-libfossil/merge/fork.go`

- [ ] **Step 1: Fix all scan-in-loop errors**

In `ancestor.go:78`:
```go
for rows.Next() {
	var pid int64
	if err := rows.Scan(&pid); err != nil {
		continue
	}
	parents = append(parents, libfossil.FslID(pid))
}
```

In `detect.go:24`:
```go
for rows.Next() {
	var rid int64
	if err := rows.Scan(&rid); err != nil {
		continue
	}
	leaves = append(leaves, libfossil.FslID(rid))
}
```

In `fork.go:73`:
```go
for rows.Next() {
	var name string
	if err := rows.Scan(&name); err != nil {
		continue
	}
	names = append(names, name)
}
```

In `resolve.go:61`, fix `filepath.Match` error:
```go
matched, matchErr := filepath.Match(p.Glob, filepath.Base(filename))
if matchErr != nil {
	continue // skip malformed glob pattern
}
if matched {
	return p.Strategy
}
```

- [ ] **Step 2: Split merge3() — extract hunk processing**

`merge3()` is 93 lines. Extract the hunk processing into a helper:

The function is complex but the split point is clear — the main loop body (lines 136-218) handles three cases: no hunks left, overlapping hunks, non-overlapping hunks. Rather than a deep extraction, we can extract the overlap-handling block (lines 169-206) into `handleOverlappingHunks()`:

```go
// handleOverlappingHunks processes two overlapping hunks and returns the merged lines + optional conflict.
func handleOverlappingHunks(base []string, lh, rh *hunk, bi int) (lines []string, conflict *Conflict) {
	if sameHunkContent(lh, rh) {
		return lh.lines, nil
	}
	c := Conflict{
		StartLine: bi + 1,
		Local:     []byte(strings.Join(lh.lines, "")),
		Remote:    []byte(strings.Join(rh.lines, "")),
	}
	start := min(lh.baseStart, rh.baseStart)
	end := max(lh.baseEnd, rh.baseEnd)
	if start < end {
		c.Base = []byte(strings.Join(base[start:end], ""))
	}
	c.EndLine = bi + max(lh.baseEnd, rh.baseEnd) - bi

	var result []string
	result = append(result, "<<<<<<< LOCAL\n")
	result = append(result, lh.lines...)
	if len(lh.lines) > 0 && !strings.HasSuffix(lh.lines[len(lh.lines)-1], "\n") {
		result = append(result, "\n")
	}
	result = append(result, "=======\n")
	result = append(result, rh.lines...)
	if len(rh.lines) > 0 && !strings.HasSuffix(rh.lines[len(rh.lines)-1], "\n") {
		result = append(result, "\n")
	}
	result = append(result, ">>>>>>> REMOTE\n")
	return result, &c
}
```

Then `merge3()` uses it:
```go
if lh != nil && rh != nil && hunksOverlap(lh, rh) {
	lines, c := handleOverlappingHunks(base, lh, rh, bi)
	result = append(result, lines...)
	if c != nil {
		conflicts = append(conflicts, *c)
	}
	bi = max(lh.baseEnd, rh.baseEnd)
	li++
	ri++
	continue
}
```

- [ ] **Step 3: Add assertions to all merge functions**

```go
// threeway.go
func (t *ThreeWayText) Merge(base, local, remote []byte) (*Result, error) {
	if base == nil { panic("merge.ThreeWayText.Merge: base must not be nil") }
	if local == nil { panic("merge.ThreeWayText.Merge: local must not be nil") }
	if remote == nil { panic("merge.ThreeWayText.Merge: remote must not be nil") }
	// ...
}

// ancestor.go
func FindCommonAncestor(r *repo.Repo, ridA, ridB libfossil.FslID) (libfossil.FslID, error) {
	if r == nil { panic("merge.FindCommonAncestor: r must not be nil") }
	if ridA <= 0 { panic("merge.FindCommonAncestor: ridA must be positive") }
	if ridB <= 0 { panic("merge.FindCommonAncestor: ridB must be positive") }
	// ...
}

// detect.go
func DetectForks(r *repo.Repo) ([]Fork, error) {
	if r == nil { panic("merge.DetectForks: r must not be nil") }
	// ...
}

// fork.go
func EnsureConflictTable(r *repo.Repo) error {
	if r == nil { panic("merge.EnsureConflictTable: r must not be nil") }
	// ...
}
func RecordConflictFork(r *repo.Repo, filename string, baseRID, localRID, remoteRID int64) error {
	if r == nil { panic("merge.RecordConflictFork: r must not be nil") }
	if filename == "" { panic("merge.RecordConflictFork: filename must not be empty") }
	// ...
}
func ResolveConflictFork(r *repo.Repo, filename string) error {
	if r == nil { panic("merge.ResolveConflictFork: r must not be nil") }
	if filename == "" { panic("merge.ResolveConflictFork: filename must not be empty") }
	// ...
}
func ListConflictForks(r *repo.Repo) ([]string, error) {
	if r == nil { panic("merge.ListConflictForks: r must not be nil") }
	// ...
}

// resolve.go
func LoadResolver(r *repo.Repo, tipRid libfossil.FslID) *Resolver {
	if r == nil { panic("merge.LoadResolver: r must not be nil") }
	// ...
}
```

- [ ] **Step 4: Run tests**

Run: `go test -buildvcs=false -count=1 ./go-libfossil/merge/`
Expected: All PASS

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/merge/
git commit -m "tigerstyle(merge): split merge3, fix 4 scan errors + filepath.Match, assertions"
```

---

### Task 11: sync — Assertions, postconditions, error fixes, function split, stub

**Files:**
- Modify: `go-libfossil/sync/session.go`
- Modify: `go-libfossil/sync/client.go`
- Modify: `go-libfossil/sync/auth.go`
- Modify: `go-libfossil/sync/transport.go`
- Modify: `go-libfossil/sync/stubs.go`

- [ ] **Step 1: Fix DELETE FROM unsent error in client.go:274**

Replace:
```go
s.repo.DB().Exec("DELETE FROM unsent")
```
With:
```go
if _, err := s.repo.DB().Exec("DELETE FROM unsent"); err != nil {
	return false, fmt.Errorf("sync: delete unsent: %w", err)
}
```

- [ ] **Step 2: Add UUID hex validation in client.go:316**

Replace:
```go
if len(uuid) > 40 {
```
With:
```go
if !hash.IsValidHash(uuid) {
	return fmt.Errorf("sync: invalid UUID format: %s", uuid)
}
if len(uuid) > 40 {
```

- [ ] **Step 3: Replace stub error with panic in stubs.go**

```go
func Clone(ctx context.Context, r *repo.Repo, t Transport, opts CloneOpts) error {
	panic("sync.Clone: not implemented — planned for Phase G")
}
```

- [ ] **Step 4: Add assertions and postconditions to session.go**

```go
func Sync(ctx context.Context, r *repo.Repo, t Transport, opts SyncOpts) (result *SyncResult, err error) {
	if r == nil { panic("sync.Sync: r must not be nil") }
	if t == nil { panic("sync.Sync: t must not be nil") }
	defer func() {
		if err == nil && result == nil { panic("sync.Sync: result must not be nil on success") }
	}()
	// ... existing logic
}

func newSession(r *repo.Repo, opts SyncOpts) *session {
	if r == nil { panic("sync.newSession: r must not be nil") }
	// ... existing logic
}
```

Add postcondition to `processResponse`:
```go
func (s *session) processResponse(msg *xfer.Message) (done bool, err error) {
	if msg == nil { panic("sync.processResponse: msg must not be nil") }
	// ... existing logic
}
```

Add assertion to `handleFileCard`:
```go
func (s *session) handleFileCard(uuid, deltaSrc string, payload []byte) (err error) {
	if uuid == "" { panic("sync.handleFileCard: uuid must not be empty") }
	if payload == nil { panic("sync.handleFileCard: payload must not be nil") }
	// postcondition: on success, blob exists in DB
	defer func() {
		if err == nil {
			if _, ok := blob.Exists(s.repo.DB(), uuid); !ok {
				panic("sync.handleFileCard: blob must exist in DB after successful store")
			}
		}
	}()
	// ... existing logic
}
```

- [ ] **Step 5: Add assertions to auth.go and transport.go**

```go
// auth.go
func computeLogin(user, password, projectCode string, payload []byte) *xfer.LoginCard {
	if user == "" { panic("sync.computeLogin: user must not be empty") }
	if projectCode == "" { panic("sync.computeLogin: projectCode must not be empty") }
	if payload == nil { panic("sync.computeLogin: payload must not be nil") }
	// ... existing logic
}

// transport.go — Exchange method
func (t *HTTPTransport) Exchange(ctx context.Context, req *xfer.Message) (*xfer.Message, error) {
	if req == nil { panic("sync.HTTPTransport.Exchange: req must not be nil") }
	// ... existing logic
}
```

- [ ] **Step 6: Run tests**

Run: `go test -buildvcs=false -count=1 ./go-libfossil/sync/`
Expected: All PASS

- [ ] **Step 7: Commit**

```bash
git add go-libfossil/sync/
git commit -m "tigerstyle(sync): fix unsent error, UUID validation, stub panic, assertions+postconditions"
```

---

## Chunk 5: Supporting Packages

### Task 12: simio — simclock AdvanceTo behavioral change

**Files:**
- Modify: `go-libfossil/simio/simclock.go`
- Modify: `go-libfossil/simio/buggify.go`

- [ ] **Step 1: Grep AdvanceTo call sites for backwards-time usage**

Run: `rg 'AdvanceTo' go-libfossil/ dst/ sim/ leaf/ bridge/`

Check if any caller passes non-monotonic times. If so, fix the caller before changing the behavior.

- [ ] **Step 2: Replace silent return with panic in simclock.go**

```go
func (c *SimClock) AdvanceTo(t time.Time) {
	if t.Before(c.now) { panic("simclock.AdvanceTo: cannot move time backwards") }
	// ... existing logic
}

func (c *SimClock) After(d time.Duration) <-chan time.Time {
	if d < 0 { panic("simclock.After: duration must not be negative") }
	// ... existing logic
}
```

- [ ] **Step 3: Add assertion to buggify.go**

```go
func (b *Buggify) Check(site string, probability float64) bool {
	if site == "" { panic("buggify.Check: site must not be empty") }
	if probability < 0 || probability > 1 { panic("buggify.Check: probability must be in [0,1]") }
	// ... existing logic
}
```

- [ ] **Step 4: Run tests**

Run: `go test -buildvcs=false -count=1 ./go-libfossil/simio/`
Expected: All PASS

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/simio/
git commit -m "tigerstyle(simio): AdvanceTo panics on backwards time, buggify assertions"
```

---

### Task 13: path, tag — Assertions

**Files:**
- Modify: `go-libfossil/path/path.go`
- Modify: `go-libfossil/tag/tag.go`

- [ ] **Step 1: Add assertions to path.go**

```go
func Shortest(db *sql.DB, from, to libfossil.FslID, directOnly bool, skip map[libfossil.FslID]bool) ([]PathNode, error) {
	if db == nil { panic("path.Shortest: db must not be nil") }
	if from <= 0 { panic("path.Shortest: from must be positive") }
	if to <= 0 { panic("path.Shortest: to must be positive") }
	// ... existing logic
}
```

- [ ] **Step 2: Add assertions to tag.go and fix tx.Exec errors**

```go
func AddTag(r *repo.Repo, opts TagOpts) (libfossil.FslID, error) {
	if r == nil { panic("tag.AddTag: r must not be nil") }
	if opts.TagName == "" { panic("tag.AddTag: opts.TagName must not be empty") }
	// ... existing logic
}

func ensureTag(tx *db.Tx, name string) (int64, error) {
	if tx == nil { panic("tag.ensureTag: tx must not be nil") }
	if name == "" { panic("tag.ensureTag: name must not be empty") }
	// ... existing logic
}
```

Fix unchecked `tx.Exec` calls in tag.go (lines 103-104):
```go
// Replace unchecked calls with:
if _, err := tx.Exec("INSERT OR IGNORE INTO unclustered(rid) VALUES(?)", rid); err != nil {
	return 0, fmt.Errorf("tag.AddTag: unclustered: %w", err)
}
if _, err := tx.Exec("INSERT OR IGNORE INTO unsent(rid) VALUES(?)", rid); err != nil {
	return 0, fmt.Errorf("tag.AddTag: unsent: %w", err)
}
```

- [ ] **Step 3: Run tests**

Run: `go test -buildvcs=false -count=1 ./go-libfossil/path/ ./go-libfossil/tag/`
Expected: All PASS

- [ ] **Step 4: Commit**

```bash
git add go-libfossil/path/ go-libfossil/tag/
git commit -m "tigerstyle(path,tag): assertions, fix tag tx.Exec errors"
```

---

### Task 14: annotate, bisect — Assertions, annotate split

**Files:**
- Modify: `go-libfossil/annotate/annotate.go`
- Modify: `go-libfossil/bisect/bisect.go`

- [ ] **Step 1: Split Annotate() and add assertions**

Extract the parent-walk loop (lines ~60-120) into `walkParentChain()`:

```go
func Annotate(r *repo.Repo, opts Options) ([]Line, error) {
	if r == nil { panic("annotate.Annotate: r must not be nil") }
	if opts.Filename == "" { panic("annotate.Annotate: opts.Filename must not be empty") }
	// ... load initial file, split into lines
	return walkParentChain(r, startRid, lines, opts)
}

func walkParentChain(r *repo.Repo, startRid libfossil.FslID, lines []Line, opts Options) ([]Line, error) {
	if r == nil { panic("annotate.walkParentChain: r must not be nil") }
	// ... parent walk loop extracted from Annotate
}
```

Add assertions to helper functions:
```go
func loadFileAt(r *repo.Repo, rid libfossil.FslID, filename string) ([]byte, error) {
	if r == nil { panic("annotate.loadFileAt: r must not be nil") }
	if rid <= 0 { panic("annotate.loadFileAt: rid must be positive") }
	if filename == "" { panic("annotate.loadFileAt: filename must not be empty") }
	// ... existing logic
}

func primaryParent(r *repo.Repo, rid libfossil.FslID) (libfossil.FslID, error) {
	if r == nil { panic("annotate.primaryParent: r must not be nil") }
	if rid <= 0 { panic("annotate.primaryParent: rid must be positive") }
	// ... existing logic
}
```

- [ ] **Step 2: Add assertions to bisect.go and fix Reset error**

```go
func NewSession(db *sql.DB) (*Session, error) {
	if db == nil { panic("bisect.NewSession: db must not be nil") }
	// ... existing logic
}

func (s *Session) MarkGood(rid libfossil.FslID) error {
	if rid <= 0 { panic("bisect.MarkGood: rid must be positive") }
	// ... existing logic
}

func (s *Session) MarkBad(rid libfossil.FslID) error {
	if rid <= 0 { panic("bisect.MarkBad: rid must be positive") }
	// ... existing logic
}

func (s *Session) Reset() {
	// Fix: check error — panic since Reset() has void return (TigerStyle: programmer error = crash)
	if _, err := s.db.Exec("DELETE FROM vvar WHERE name IN ('bisect-good','bisect-bad','bisect-log')"); err != nil {
		panic(fmt.Sprintf("bisect.Reset: failed to clear bisect state: %v", err))
	}
}
```

- [ ] **Step 3: Run tests**

Run: `go test -buildvcs=false -count=1 ./go-libfossil/annotate/ ./go-libfossil/bisect/`
Expected: All PASS

- [ ] **Step 4: Commit**

```bash
git add go-libfossil/annotate/ go-libfossil/bisect/
git commit -m "tigerstyle(annotate,bisect): split Annotate, fix bisect.Reset error, assertions"
```

---

### Task 15: undo — Assertions, function split (swapState)

**Files:**
- Modify: `go-libfossil/undo/undo.go`

- [ ] **Step 1: Add assertions and split swapState()**

Add preconditions:
```go
func Save(ckout *sql.DB, dir string, pathnames []string) error {
	if ckout == nil { panic("undo.Save: ckout must not be nil") }
	if dir == "" { panic("undo.Save: dir must not be empty") }
	// ... existing logic
}

func Undo(ckout *sql.DB, dir string) error {
	if ckout == nil { panic("undo.Undo: ckout must not be nil") }
	if dir == "" { panic("undo.Undo: dir must not be empty") }
	return swapState(ckout, dir, false)
}

func Redo(ckout *sql.DB, dir string) error {
	if ckout == nil { panic("undo.Redo: ckout must not be nil") }
	if dir == "" { panic("undo.Redo: dir must not be empty") }
	return swapState(ckout, dir, true)
}
```

Split `swapState()` (141 lines) into:
- `swapState()` — orchestrator (~30 lines)
- `swapDiskFiles(tx, dir, label, wantFlag)` — file content swap loop (~50 lines)
- `swapTables(tx, label)` — vfile/vmerge table swap (~25 lines)
- `swapCheckout(tx, label, setAvail)` — vvar checkout swap (~20 lines)

```go
func swapState(ckout *sql.DB, dir string, redo bool) error {
	label, wantAvail, setAvail, wantFlag := "undo", "1", "2", false
	if redo {
		label, wantAvail, setAvail, wantFlag = "redo", "2", "1", true
	}

	var avail string
	err := ckout.QueryRow("SELECT value FROM vvar WHERE name='undo_available'").Scan(&avail)
	if err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return fmt.Errorf("undo.%s: nothing to %s", label, label)
		}
		return fmt.Errorf("undo.%s: query undo_available: %w", label, err)
	}
	if avail != wantAvail {
		return fmt.Errorf("undo.%s: nothing to %s", label, label)
	}

	tx, err := ckout.Begin()
	if err != nil {
		return fmt.Errorf("undo.%s: begin tx: %w", label, err)
	}
	defer tx.Rollback()

	if err := swapDiskFiles(tx, dir, label, wantFlag); err != nil {
		return err
	}
	if err := swapTables(tx, label); err != nil {
		return err
	}
	if err := swapCheckout(tx, label, setAvail); err != nil {
		return err
	}
	return tx.Commit()
}
```

- [ ] **Step 2: Run tests**

Run: `go test -buildvcs=false -count=1 ./go-libfossil/undo/`
Expected: All PASS

- [ ] **Step 3: Commit**

```bash
git add go-libfossil/undo/
git commit -m "tigerstyle(undo): split swapState into 3 helpers, assertions"
```

---

### Task 16: stash — Assertions, function split, error fixes

**Files:**
- Modify: `go-libfossil/stash/stash.go`

- [ ] **Step 1: Add assertions to all public functions**

```go
func EnsureTables(ckout *sql.DB) error {
	if ckout == nil { panic("stash.EnsureTables: ckout must not be nil") }
	// ...
}
func Save(ckout *sql.DB, repoDB *sql.DB, dir string, comment string) error {
	if ckout == nil { panic("stash.Save: ckout must not be nil") }
	if repoDB == nil { panic("stash.Save: repoDB must not be nil") }
	if dir == "" { panic("stash.Save: dir must not be empty") }
	// ...
}
func Apply(ckout *sql.DB, repoDB *sql.DB, dir string, stashID int64) error {
	if ckout == nil { panic("stash.Apply: ckout must not be nil") }
	if repoDB == nil { panic("stash.Apply: repoDB must not be nil") }
	if dir == "" { panic("stash.Apply: dir must not be empty") }
	if stashID <= 0 { panic("stash.Apply: stashID must be positive") }
	// ...
}
func Pop(ckout *sql.DB, repoDB *sql.DB, dir string) error {
	if ckout == nil { panic("stash.Pop: ckout must not be nil") }
	if repoDB == nil { panic("stash.Pop: repoDB must not be nil") }
	if dir == "" { panic("stash.Pop: dir must not be empty") }
	// ...
}
func List(ckout *sql.DB) ([]Entry, error) {
	if ckout == nil { panic("stash.List: ckout must not be nil") }
	// ...
}
func Drop(ckout *sql.DB, stashID int64) error {
	if ckout == nil { panic("stash.Drop: ckout must not be nil") }
	if stashID <= 0 { panic("stash.Drop: stashID must be positive") }
	// ...
}
func Clear(ckout *sql.DB) error {
	if ckout == nil { panic("stash.Clear: ckout must not be nil") }
	// ...
}
```

- [ ] **Step 2: Fix error handling — os.Remove and RowsAffected**

Line 205 `os.Remove`:
```go
if err := os.Remove(fullPath); err != nil && !errors.Is(err, os.ErrNotExist) {
	return fmt.Errorf("stash.Save: remove added %s: %w", f.pathname, err)
}
```

Line 370 `RowsAffected`:
```go
n, raErr := res.RowsAffected()
if raErr != nil {
	return fmt.Errorf("stash.Drop: rows affected: %w", raErr)
}
```

- [ ] **Step 3: Split Save() — extract snapshotVFile + computeAndStoreDeltas**

```go
func Save(ckout *sql.DB, repoDB *sql.DB, dir string, comment string) error {
	// ... assertions, EnsureTables, begin tx, get checkout hash, get stashID, insert header
	files, err := snapshotChangedFiles(tx)
	if err != nil { return err }
	if len(files) == 0 { return fmt.Errorf("stash.Save: no changes to stash") }
	if err := storeAndRevertFiles(tx, repoDB, dir, stashID, files); err != nil { return err }
	return tx.Commit()
}

type changedFile struct { pathname string; rid int64; chnged, deleted int; isExec, isLink bool }

func snapshotChangedFiles(tx *sql.Tx) ([]changedFile, error) { /* query vfile */ }
func storeAndRevertFiles(tx *sql.Tx, repoDB *sql.DB, dir string, stashID int64, files []changedFile) error { /* delta computation + revert loop */ }
```

- [ ] **Step 4: Run tests**

Run: `go test -buildvcs=false -count=1 ./go-libfossil/stash/`
Expected: All PASS

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/stash/
git commit -m "tigerstyle(stash): split Save, fix os.Remove+RowsAffected errors, assertions"
```

---

## Chunk 6: Final Validation

### Task 17: Full test suite

- [ ] **Step 1: Run all go-libfossil tests**

Run: `go test -buildvcs=false -count=1 ./go-libfossil/...`
Expected: All 20 packages PASS

- [ ] **Step 2: Build CLI to verify no compile errors**

Run: `go build -buildvcs=false ./cmd/edgesync/`
Expected: Clean build

### Task 18: DST sweep

- [ ] **Step 1: Run DST full sweep**

Run: `make dst-full` (or equivalent: 16 seeds x 3 severities)
Expected: 48/48 PASS

- [ ] **Step 2: Run DST hostile**

Run: `make dst-hostile`
Expected: PASS

### Task 19: Sim integration

- [ ] **Step 1: Run sim integration test**

Run: `go test -buildvcs=false -count=1 -timeout 120s ./sim/`
Expected: PASS (2 leaves, blobs converge, all invariants)

### Task 20: Final commit and summary

- [ ] **Step 1: Check for any uncommitted changes**

Run: `git status`
Expected: clean working tree

- [ ] **Step 2: Verify commit history**

Run: `git log --oneline origin/main..HEAD`
Expected: ~15-16 commits, one per task
