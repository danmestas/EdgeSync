# go-libfossil Phase B Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement manifest parsing, serialization, and checkin operations — enabling Go to programmatically create Fossil checkins that `fossil rebuild` accepts and `fossil timeline` displays.

**Architecture:** Two new packages built bottom-up with strict TDD. `deck/` handles pure manifest format (no database) — parsing, serialization, Z-card, R-card, fossil-encoding. `manifest/` combines deck with database storage for checkin creation, file listing, and history walking. A prerequisite `db.Querier` interface refactor enables transactional checkins. All code lives in `~/projects/EdgeSync/go-libfossil/`.

**Tech Stack:** Go 1.23+, `crypto/md5` (Z-card/R-card), `modernc.org/sqlite`, Phase A packages (`db/`, `blob/`, `content/`, `hash/`, `repo/`, `testutil/`)

**Spec:** `docs/superpowers/specs/2026-03-14-go-libfossil-phase-b-design.md`

---

## File Structure

```
go-libfossil/
  db/
    querier.go         # NEW: Querier interface
  blob/
    blob.go            # MODIFY: function signatures accept db.Querier
  content/
    content.go         # MODIFY: function signatures accept db.Querier
  deck/
    deck.go            # NEW: Deck struct, ArtifactType, card subtypes
    encode.go          # NEW: FossilEncode / FossilDecode
    zcard.go           # NEW: VerifyZ, computeZ
    marshal.go         # NEW: Deck.Marshal()
    parse.go           # NEW: Parse()
    rcard.go           # NEW: Deck.ComputeR()
    deck_test.go       # NEW: Unit tests (no database)
  manifest/
    manifest.go        # NEW: Checkin(), GetManifest()
    files.go           # NEW: ListFiles()
    log.go             # NEW: Log()
    manifest_test.go   # NEW: Integration tests with fossil CLI
  testutil/
    testutil.go        # MODIFY: add FossilArtifact, FossilTimeline helpers
  integration_test.go  # MODIFY: add Phase B integration tests
```

---

## Chunk 1: db.Querier Interface and Signature Refactor

### Task 1: Add Querier Interface

**Files:**
- Create: `go-libfossil/db/querier.go`

- [ ] **Step 1: Create the interface file**

Create `go-libfossil/db/querier.go`:

```go
package db

import "database/sql"

// Querier is the common interface satisfied by both *DB and *Tx.
// Functions that need to work inside transactions accept Querier
// instead of *DB.
type Querier interface {
	Exec(query string, args ...any) (sql.Result, error)
	QueryRow(query string, args ...any) *sql.Row
	Query(query string, args ...any) (*sql.Rows, error)
}
```

- [ ] **Step 2: Verify compilation**

Run: `cd ~/projects/EdgeSync/go-libfossil && go build ./db/...`
Expected: success (both *DB and *Tx already satisfy the interface)

- [ ] **Step 3: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/db/querier.go
fossil commit -m "db: add Querier interface for transactional operations"
```

---

### Task 2: Refactor blob Package to Accept Querier

**Files:**
- Modify: `go-libfossil/blob/blob.go`

- [ ] **Step 1: Update all function signatures**

Change every `d *db.DB` parameter to `q db.Querier` in blob.go. All internal `d.Exec`, `d.QueryRow`, `d.Query` calls become `q.Exec`, `q.QueryRow`, `q.Query`. No logic changes.

Functions to update:
- `Store(q db.Querier, content []byte) (libfossil.FslID, string, error)`
- `StoreDelta(q db.Querier, content []byte, srcRid libfossil.FslID) (libfossil.FslID, string, error)`
- `StorePhantom(q db.Querier, uuid string) (libfossil.FslID, error)`
- `Load(q db.Querier, rid libfossil.FslID) ([]byte, error)`
- `Exists(q db.Querier, uuid string) (libfossil.FslID, bool)`

- [ ] **Step 2: Run all tests**

Run: `cd ~/projects/EdgeSync/go-libfossil && go test ./...`
Expected: all pass (*db.DB satisfies Querier, so all call sites work unchanged)

- [ ] **Step 3: Commit**

```bash
cd ~/projects/EdgeSync
fossil commit -m "blob: accept db.Querier instead of *db.DB for transaction support"
```

---

### Task 3: Refactor content Package to Accept Querier

**Files:**
- Modify: `go-libfossil/content/content.go`

- [ ] **Step 1: Update all function signatures**

Change every `d *db.DB` to `q db.Querier`:
- `Expand(q db.Querier, rid libfossil.FslID) ([]byte, error)`
- `walkDeltaChain(q db.Querier, rid libfossil.FslID) ([]libfossil.FslID, error)`
- `Verify(q db.Querier, rid libfossil.FslID) error`
- `IsPhantom(q db.Querier, rid libfossil.FslID) (bool, error)`

Update internal calls: `blob.Load(d, ...)` → `blob.Load(q, ...)` etc.

- [ ] **Step 2: Run full test suite**

Run: `cd ~/projects/EdgeSync/go-libfossil && go test ./...`
Expected: all pass

- [ ] **Step 3: Commit**

```bash
cd ~/projects/EdgeSync
fossil commit -m "content: accept db.Querier instead of *db.DB for transaction support"
```

---

## Chunk 2: deck/ Package — Types, Encoding, Z-card, Marshal

### Task 4: Deck Types

**Files:**
- Create: `go-libfossil/deck/deck.go`
- Create: `go-libfossil/deck/deck_test.go`

- [ ] **Step 1: Write the type assertion test**

Create `go-libfossil/deck/deck_test.go`:

```go
package deck

import (
	"testing"
	"time"
)

func TestArtifactTypeConstants(t *testing.T) {
	if Checkin != 0 {
		t.Fatalf("Checkin = %d, want 0", Checkin)
	}
	if Control != 7 {
		t.Fatalf("Control = %d, want 7", Control)
	}
}

func TestDeckZeroValue(t *testing.T) {
	var d Deck
	if d.Type != Checkin {
		t.Fatalf("zero Deck.Type = %d, want Checkin(0)", d.Type)
	}
	if len(d.F) != 0 {
		t.Fatal("zero Deck.F should be empty")
	}
	if !d.D.IsZero() {
		t.Fatal("zero Deck.D should be zero time")
	}
}

func TestTagTypeConstants(t *testing.T) {
	if TagSingleton != '+' {
		t.Fatalf("TagSingleton = %c, want +", TagSingleton)
	}
	if TagPropagating != '*' {
		t.Fatalf("TagPropagating = %c, want *", TagPropagating)
	}
	if TagCancel != '-' {
		t.Fatalf("TagCancel = %c, want -", TagCancel)
	}
}
```

- [ ] **Step 2: Run test — should fail**

Run: `cd ~/projects/EdgeSync/go-libfossil && go test ./deck/ -v`
Expected: FAIL — types not defined

- [ ] **Step 3: Write deck.go**

Create `go-libfossil/deck/deck.go`:

```go
package deck

import "time"

type ArtifactType int

const (
	Checkin    ArtifactType = iota
	Wiki
	Ticket
	Event
	Cluster
	ForumPost
	Attachment
	Control
)

type Deck struct {
	Type ArtifactType
	A    *AttachmentCard
	B    string
	C    string
	D    time.Time
	E    *EventCard
	F    []FileCard
	G    string
	H    string
	I    string
	J    []TicketField
	K    string
	L    string
	M    []string
	N    string
	P    []string
	Q    []CherryPick
	R    string
	T    []TagCard
	U    string
	W    []byte
	Z    string
}

type FileCard struct {
	Name    string
	UUID    string
	Perm    string
	OldName string
}

type TagCard struct {
	Type  TagType
	Name  string
	UUID  string
	Value string
}

type TagType byte

const (
	TagSingleton   TagType = '+'
	TagPropagating TagType = '*'
	TagCancel      TagType = '-'
)

type CherryPick struct {
	IsBackout bool
	Target    string
	Baseline  string
}

type AttachmentCard struct {
	Filename string
	Target   string
	Source   string
}

type EventCard struct {
	Date time.Time
	UUID string
}

type TicketField struct {
	Name  string
	Value string
}
```

- [ ] **Step 4: Run tests — should pass**

Run: `cd ~/projects/EdgeSync/go-libfossil && go test ./deck/ -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/deck/deck.go go-libfossil/deck/deck_test.go
fossil commit -m "deck: add Deck struct and all card types"
```

---

### Task 5: Fossil Encoding/Decoding

**Files:**
- Create: `go-libfossil/deck/encode.go`
- Modify: `go-libfossil/deck/deck_test.go`

- [ ] **Step 1: Add failing tests to deck_test.go**

```go
func TestFossilEncode(t *testing.T) {
	tests := []struct{ in, want string }{
		{"hello", "hello"},
		{"hello world", `hello\sworld`},
		{"line\nbreak", `line\nbreak`},
		{`back\slash`, `back\\slash`},
		{"a b\nc\\d", `a\sb\nc\\d`},
		{"", ""},
	}
	for _, tt := range tests {
		got := FossilEncode(tt.in)
		if got != tt.want {
			t.Errorf("FossilEncode(%q) = %q, want %q", tt.in, got, tt.want)
		}
	}
}

func TestFossilDecode(t *testing.T) {
	tests := []struct{ in, want string }{
		{"hello", "hello"},
		{`hello\sworld`, "hello world"},
		{`line\nbreak`, "line\nbreak"},
		{`back\\slash`, `back\slash`},
		{`a\sb\nc\\d`, "a b\nc\\d"},
		{"", ""},
	}
	for _, tt := range tests {
		got := FossilDecode(tt.in)
		if got != tt.want {
			t.Errorf("FossilDecode(%q) = %q, want %q", tt.in, got, tt.want)
		}
	}
}

func TestFossilEncodeDecodeRoundTrip(t *testing.T) {
	inputs := []string{"simple", "has spaces", "has\nnewlines", `has\backslash`, ""}
	for _, s := range inputs {
		if got := FossilDecode(FossilEncode(s)); got != s {
			t.Errorf("round-trip(%q) = %q", s, got)
		}
	}
}
```

- [ ] **Step 2: Run — should fail**

Run: `cd ~/projects/EdgeSync/go-libfossil && go test ./deck/ -run TestFossil -v`

- [ ] **Step 3: Implement encode.go**

Create `go-libfossil/deck/encode.go`:

```go
package deck

import "strings"

func FossilEncode(s string) string {
	var b strings.Builder
	b.Grow(len(s))
	for i := 0; i < len(s); i++ {
		switch s[i] {
		case ' ':
			b.WriteString(`\s`)
		case '\n':
			b.WriteString(`\n`)
		case '\\':
			b.WriteString(`\\`)
		default:
			b.WriteByte(s[i])
		}
	}
	return b.String()
}

func FossilDecode(s string) string {
	var b strings.Builder
	b.Grow(len(s))
	for i := 0; i < len(s); i++ {
		if s[i] == '\\' && i+1 < len(s) {
			switch s[i+1] {
			case 's':
				b.WriteByte(' ')
				i++
			case 'n':
				b.WriteByte('\n')
				i++
			case '\\':
				b.WriteByte('\\')
				i++
			default:
				b.WriteByte(s[i])
			}
		} else {
			b.WriteByte(s[i])
		}
	}
	return b.String()
}
```

- [ ] **Step 4: Run — should pass**

- [ ] **Step 5: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/deck/encode.go
fossil commit -m "deck: add FossilEncode/FossilDecode string escaping"
```

---

### Task 6: Z-card Verification and Computation

**Files:**
- Create: `go-libfossil/deck/zcard.go`
- Modify: `go-libfossil/deck/deck_test.go`

- [ ] **Step 1: Add failing tests**

```go
func TestVerifyZ(t *testing.T) {
	body := "D 2024-01-15T10:30:00.000\nU testuser\n"
	h := md5.Sum([]byte(body))
	zLine := fmt.Sprintf("Z %x\n", h)
	manifest := []byte(body + zLine)
	if err := VerifyZ(manifest); err != nil {
		t.Fatalf("VerifyZ failed on valid manifest: %v", err)
	}
}

func TestVerifyZBadChecksum(t *testing.T) {
	bad := []byte("D 2024-01-15T10:30:00.000\nU testuser\nZ 00000000000000000000000000000000\n")
	if err := VerifyZ(bad); err == nil {
		t.Fatal("VerifyZ should fail on bad checksum")
	}
}

func TestVerifyZTooShort(t *testing.T) {
	if err := VerifyZ([]byte("short")); err == nil {
		t.Fatal("VerifyZ should fail on short input")
	}
}
```

(Add `"crypto/md5"` and `"fmt"` to test imports)

- [ ] **Step 2: Run — should fail**

- [ ] **Step 3: Implement zcard.go**

Create `go-libfossil/deck/zcard.go`:

```go
package deck

import (
	"crypto/md5"
	"encoding/hex"
	"fmt"
)

func VerifyZ(data []byte) error {
	if len(data) < 35 {
		return fmt.Errorf("deck.VerifyZ: manifest too short (%d bytes)", len(data))
	}
	tail := data[len(data)-35:]
	if tail[0] != 'Z' || tail[1] != ' ' || tail[34] != '\n' {
		return fmt.Errorf("deck.VerifyZ: invalid Z-card format")
	}
	stated := string(tail[2:34])
	computed := computeZ(data[:len(data)-35])
	if computed != stated {
		return fmt.Errorf("deck.VerifyZ: checksum mismatch: stated=%s computed=%s", stated, computed)
	}
	return nil
}

func computeZ(data []byte) string {
	h := md5.Sum(data)
	return hex.EncodeToString(h[:])
}
```

- [ ] **Step 4: Run — should pass**

- [ ] **Step 5: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/deck/zcard.go
fossil commit -m "deck: add Z-card verification and computation"
```

---

### Task 7: Marshal — Deck to Canonical Bytes

**Files:**
- Create: `go-libfossil/deck/marshal.go`
- Modify: `go-libfossil/deck/deck_test.go`

- [ ] **Step 1: Add failing tests**

```go
func TestMarshalMinimalCheckin(t *testing.T) {
	d := &Deck{
		Type: Checkin,
		C:    "initial commit",
		D:    time.Date(2024, 1, 15, 10, 30, 0, 0, time.UTC),
		F:    []FileCard{{Name: "hello.txt", UUID: "da39a3ee5e6b4b0d3255bfef95601890afd80709"}},
		R:    "d41d8cd98f00b204e9800998ecf8427e",
		T: []TagCard{
			{Type: TagPropagating, Name: "branch", UUID: "*", Value: "trunk"},
			{Type: TagSingleton, Name: "sym-trunk", UUID: "*"},
		},
		U: "testuser",
	}
	data, err := d.Marshal()
	if err != nil {
		t.Fatalf("Marshal: %v", err)
	}
	if err := VerifyZ(data); err != nil {
		t.Fatalf("Z-card failed: %v", err)
	}
	s := string(data)
	if idx := strings.Index(s, "C "); idx > strings.Index(s, "D ") {
		t.Fatal("C after D — card ordering wrong")
	}
}

func TestMarshalDCardFormat(t *testing.T) {
	d := &Deck{Type: Checkin, D: time.Date(2024, 1, 15, 10, 30, 0, 0, time.UTC), U: "test"}
	data, _ := d.Marshal()
	if !strings.Contains(string(data), "D 2024-01-15T10:30:00.000\n") {
		t.Fatalf("D-card format wrong in:\n%s", data)
	}
}

func TestMarshalFossilEncoding(t *testing.T) {
	d := &Deck{
		Type: Checkin,
		C:    "fix the space bug",
		D:    time.Date(2024, 1, 15, 10, 30, 0, 0, time.UTC),
		U:    "test user",
	}
	data, _ := d.Marshal()
	s := string(data)
	if !strings.Contains(s, `C fix\sthe\sspace\sbug`) {
		t.Fatalf("C-card not encoded:\n%s", s)
	}
	if !strings.Contains(s, `U test\suser`) {
		t.Fatalf("U-card not encoded:\n%s", s)
	}
}

func TestMarshalWCard(t *testing.T) {
	d := &Deck{
		Type: Wiki,
		D:    time.Date(2024, 1, 15, 10, 30, 0, 0, time.UTC),
		L:    "TestPage",
		U:    "test",
		W:    []byte("Hello wiki world"),
	}
	data, _ := d.Marshal()
	if !strings.Contains(string(data), "W 16\nHello wiki world\n") {
		t.Fatalf("W-card wrong:\n%s", data)
	}
}
```

(Add `"strings"` to test imports)

- [ ] **Step 2: Run — should fail**

- [ ] **Step 3: Implement marshal.go**

Create `go-libfossil/deck/marshal.go`:

```go
package deck

import (
	"fmt"
	"sort"
	"strings"
)

func (d *Deck) Marshal() ([]byte, error) {
	var b strings.Builder

	// Cards in strict ASCII order: A B C D E F G H I J K L M N P Q R T U W Z

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
		fmt.Fprintf(&b, "B %s\n", d.B)
	}

	if d.C != "" {
		fmt.Fprintf(&b, "C %s\n", FossilEncode(d.C))
	}

	if !d.D.IsZero() {
		fmt.Fprintf(&b, "D %s\n", d.D.UTC().Format("2006-01-02T15:04:05.000"))
	}

	if d.E != nil {
		fmt.Fprintf(&b, "E %s %s\n", d.E.Date.UTC().Format("2006-01-02T15:04:05"), d.E.UUID)
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

	if d.G != "" {
		fmt.Fprintf(&b, "G %s\n", d.G)
	}
	if d.H != "" {
		fmt.Fprintf(&b, "H %s\n", FossilEncode(d.H))
	}
	if d.I != "" {
		fmt.Fprintf(&b, "I %s\n", d.I)
	}

	for _, j := range d.J {
		if j.Value != "" {
			fmt.Fprintf(&b, "J %s %s\n", FossilEncode(j.Name), j.Value)
		} else {
			fmt.Fprintf(&b, "J %s\n", FossilEncode(j.Name))
		}
	}

	if d.K != "" {
		fmt.Fprintf(&b, "K %s\n", d.K)
	}
	if d.L != "" {
		fmt.Fprintf(&b, "L %s\n", FossilEncode(d.L))
	}

	if len(d.M) > 0 {
		sorted := make([]string, len(d.M))
		copy(sorted, d.M)
		sort.Strings(sorted)
		for _, m := range sorted {
			fmt.Fprintf(&b, "M %s\n", m)
		}
	}

	if d.N != "" {
		fmt.Fprintf(&b, "N %s\n", d.N)
	}

	if len(d.P) > 0 {
		fmt.Fprintf(&b, "P %s\n", strings.Join(d.P, " "))
	}

	for _, q := range d.Q {
		prefix := "+"
		if q.IsBackout {
			prefix = "-"
		}
		if q.Baseline != "" {
			fmt.Fprintf(&b, "Q %s%s %s\n", prefix, q.Target, q.Baseline)
		} else {
			fmt.Fprintf(&b, "Q %s%s\n", prefix, q.Target)
		}
	}

	if d.R != "" {
		fmt.Fprintf(&b, "R %s\n", d.R)
	}

	if len(d.T) > 0 {
		sorted := make([]TagCard, len(d.T))
		copy(sorted, d.T)
		sort.Slice(sorted, func(i, j int) bool {
			ki := string(sorted[i].Type) + sorted[i].Name + sorted[i].UUID
			kj := string(sorted[j].Type) + sorted[j].Name + sorted[j].UUID
			return ki < kj
		})
		for _, tag := range sorted {
			fmt.Fprintf(&b, "T %c%s %s", tag.Type, tag.Name, tag.UUID)
			if tag.Value != "" {
				fmt.Fprintf(&b, " %s", tag.Value)
			}
			b.WriteString("\n")
		}
	}

	if d.U != "" {
		fmt.Fprintf(&b, "U %s\n", FossilEncode(d.U))
	}

	if len(d.W) > 0 {
		fmt.Fprintf(&b, "W %d\n%s\n", len(d.W), d.W)
	}

	body := b.String()
	zHash := computeZ([]byte(body))
	return []byte(fmt.Sprintf("%sZ %s\n", body, zHash)), nil
}
```

- [ ] **Step 4: Run — should pass**

- [ ] **Step 5: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/deck/marshal.go
fossil commit -m "deck: add Marshal() for canonical manifest serialization"
```

---

## Chunk 3: deck/ Package — Parse, R-card, Round-trip

### Task 8: Parse — Manifest Bytes to Deck

**Files:**
- Create: `go-libfossil/deck/parse.go`
- Modify: `go-libfossil/deck/deck_test.go`

- [ ] **Step 1: Add failing tests**

```go
func TestParseMinimalCheckin(t *testing.T) {
	d := &Deck{
		Type: Checkin,
		C:    "initial commit",
		D:    time.Date(2024, 1, 15, 10, 30, 0, 0, time.UTC),
		F:    []FileCard{{Name: "hello.txt", UUID: "da39a3ee5e6b4b0d3255bfef95601890afd80709"}},
		R:    "d41d8cd98f00b204e9800998ecf8427e",
		T: []TagCard{
			{Type: TagPropagating, Name: "branch", UUID: "*", Value: "trunk"},
			{Type: TagSingleton, Name: "sym-trunk", UUID: "*"},
		},
		U: "testuser",
	}
	data, _ := d.Marshal()
	parsed, err := Parse(data)
	if err != nil {
		t.Fatalf("Parse: %v", err)
	}
	if parsed.C != "initial commit" {
		t.Fatalf("C = %q", parsed.C)
	}
	if len(parsed.F) != 1 || parsed.F[0].Name != "hello.txt" {
		t.Fatalf("F = %+v", parsed.F)
	}
	if len(parsed.T) != 2 {
		t.Fatalf("T count = %d", len(parsed.T))
	}
}

func TestParseFossilEncodedFields(t *testing.T) {
	d := &Deck{
		Type: Checkin,
		C:    "fix the space bug",
		D:    time.Date(2024, 1, 15, 10, 30, 0, 0, time.UTC),
		U:    "test user",
	}
	data, _ := d.Marshal()
	parsed, _ := Parse(data)
	if parsed.C != "fix the space bug" {
		t.Fatalf("C = %q, want decoded", parsed.C)
	}
	if parsed.U != "test user" {
		t.Fatalf("U = %q, want decoded", parsed.U)
	}
}

func TestParseWikiManifest(t *testing.T) {
	d := &Deck{
		Type: Wiki,
		D:    time.Date(2024, 1, 15, 10, 30, 0, 0, time.UTC),
		L:    "TestPage",
		U:    "admin",
		W:    []byte("Hello wiki content"),
	}
	data, _ := d.Marshal()
	parsed, err := Parse(data)
	if err != nil {
		t.Fatalf("Parse: %v", err)
	}
	if parsed.L != "TestPage" {
		t.Fatalf("L = %q", parsed.L)
	}
	if string(parsed.W) != "Hello wiki content" {
		t.Fatalf("W = %q", parsed.W)
	}
}

func TestParseBadZCard(t *testing.T) {
	_, err := Parse([]byte("D 2024-01-15T10:30:00.000\nU test\nZ 00000000000000000000000000000000\n"))
	if err == nil {
		t.Fatal("should fail on bad Z-card")
	}
}

func TestParseCardOrdering(t *testing.T) {
	body := "U test\nD 2024-01-15T10:30:00.000\n"
	h := md5.Sum([]byte(body))
	manifest := fmt.Sprintf("%sZ %x\n", body, h)
	_, err := Parse([]byte(manifest))
	if err == nil {
		t.Fatal("should reject out-of-order cards")
	}
}
```

- [ ] **Step 2: Run — should fail**

- [ ] **Step 3: Implement parse.go**

Create `go-libfossil/deck/parse.go`:

```go
package deck

import (
	"bytes"
	"fmt"
	"strconv"
	"strings"
	"time"
)

func Parse(data []byte) (*Deck, error) {
	if err := VerifyZ(data); err != nil {
		return nil, fmt.Errorf("deck.Parse: %w", err)
	}

	body := data[:len(data)-35]
	d := &Deck{}
	var lastCard byte
	reader := bytes.NewReader(body)

	for reader.Len() > 0 {
		line, err := readLine(reader)
		if err != nil || len(line) == 0 {
			continue
		}

		card := line[0]

		if card == 'W' {
			if card < lastCard {
				return nil, fmt.Errorf("deck.Parse: card 'W' out of order (after '%c')", lastCard)
			}
			lastCard = card
			sizeStr := strings.TrimSpace(line[2:])
			size, err := strconv.Atoi(sizeStr)
			if err != nil {
				return nil, fmt.Errorf("deck.Parse: bad W size: %w", err)
			}
			content := make([]byte, size)
			n, _ := reader.Read(content)
			if n != size {
				return nil, fmt.Errorf("deck.Parse: W content: got %d, want %d", n, size)
			}
			reader.ReadByte() // trailing newline
			d.W = content
			continue
		}

		if card < lastCard {
			return nil, fmt.Errorf("deck.Parse: card '%c' out of order (after '%c')", card, lastCard)
		}
		lastCard = card

		if len(line) < 2 || line[1] != ' ' {
			return nil, fmt.Errorf("deck.Parse: malformed: %q", line)
		}
		args := line[2:]
		if err := parseCard(d, card, args); err != nil {
			return nil, fmt.Errorf("deck.Parse: %w", err)
		}
	}

	d.Type = inferType(d)
	return d, nil
}

func readLine(r *bytes.Reader) (string, error) {
	var b strings.Builder
	for {
		c, err := r.ReadByte()
		if err != nil {
			return b.String(), nil
		}
		if c == '\n' {
			return b.String(), nil
		}
		b.WriteByte(c)
	}
}

func parseCard(d *Deck, card byte, args string) error {
	switch card {
	case 'A':
		parts := strings.SplitN(args, " ", 3)
		if len(parts) < 2 {
			return fmt.Errorf("A-card needs 2+ fields")
		}
		ac := &AttachmentCard{Filename: FossilDecode(parts[0]), Target: parts[1]}
		if len(parts) == 3 {
			ac.Source = parts[2]
		}
		d.A = ac
	case 'B':
		d.B = strings.TrimSpace(args)
	case 'C':
		d.C = FossilDecode(args)
	case 'D':
		t, err := parseTimestamp(args)
		if err != nil {
			return fmt.Errorf("D-card: %w", err)
		}
		d.D = t
	case 'E':
		parts := strings.SplitN(args, " ", 2)
		if len(parts) != 2 {
			return fmt.Errorf("E-card needs datetime and uuid")
		}
		t, err := parseTimestamp(parts[0])
		if err != nil {
			return fmt.Errorf("E-card: %w", err)
		}
		d.E = &EventCard{Date: t, UUID: parts[1]}
	case 'F':
		parts := strings.Fields(args)
		if len(parts) == 0 {
			return fmt.Errorf("empty F-card")
		}
		fc := FileCard{Name: FossilDecode(parts[0])}
		if len(parts) >= 2 {
			fc.UUID = parts[1]
		}
		if len(parts) >= 3 {
			fc.Perm = parts[2]
		}
		if len(parts) >= 4 {
			fc.OldName = FossilDecode(parts[3])
		}
		d.F = append(d.F, fc)
	case 'G':
		d.G = strings.TrimSpace(args)
	case 'H':
		d.H = FossilDecode(args)
	case 'I':
		d.I = strings.TrimSpace(args)
	case 'J':
		parts := strings.SplitN(args, " ", 2)
		jf := TicketField{Name: FossilDecode(parts[0])}
		if len(parts) == 2 {
			jf.Value = parts[1]
		}
		d.J = append(d.J, jf)
	case 'K':
		d.K = strings.TrimSpace(args)
	case 'L':
		d.L = FossilDecode(args)
	case 'M':
		d.M = append(d.M, strings.TrimSpace(args))
	case 'N':
		d.N = strings.TrimSpace(args)
	case 'P':
		d.P = strings.Fields(args)
	case 'Q':
		if len(args) < 2 {
			return fmt.Errorf("Q-card too short")
		}
		cp := CherryPick{IsBackout: args[0] == '-'}
		rest := args[1:]
		parts := strings.SplitN(rest, " ", 2)
		cp.Target = parts[0]
		if len(parts) == 2 {
			cp.Baseline = parts[1]
		}
		d.Q = append(d.Q, cp)
	case 'R':
		d.R = strings.TrimSpace(args)
	case 'T':
		if len(args) < 2 {
			return fmt.Errorf("T-card too short")
		}
		tc := TagCard{Type: TagType(args[0])}
		parts := strings.SplitN(args[1:], " ", 3)
		if len(parts) < 2 {
			return fmt.Errorf("T-card needs name and uuid")
		}
		tc.Name = parts[0]
		tc.UUID = parts[1]
		if len(parts) == 3 {
			tc.Value = parts[2]
		}
		d.T = append(d.T, tc)
	case 'U':
		d.U = FossilDecode(args)
	default:
		return fmt.Errorf("unknown card '%c'", card)
	}
	return nil
}

func parseTimestamp(s string) (time.Time, error) {
	s = strings.TrimSpace(s)
	for _, layout := range []string{
		"2006-01-02T15:04:05.000",
		"2006-01-02T15:04:05",
	} {
		if t, err := time.Parse(layout, s); err == nil {
			return t, nil
		}
	}
	s = strings.Replace(s, "t", "T", 1)
	for _, layout := range []string{
		"2006-01-02T15:04:05.000",
		"2006-01-02T15:04:05",
	} {
		if t, err := time.Parse(layout, s); err == nil {
			return t, nil
		}
	}
	return time.Time{}, fmt.Errorf("cannot parse timestamp %q", s)
}

func inferType(d *Deck) ArtifactType {
	switch {
	case len(d.M) > 0:
		return Cluster
	case d.G != "" || d.H != "" || d.I != "":
		return ForumPost
	case d.A != nil:
		return Attachment
	case d.K != "":
		return Ticket
	case d.L != "":
		return Wiki
	case d.E != nil:
		return Event
	case len(d.F) > 0 || d.R != "":
		return Checkin
	case len(d.T) > 0:
		return Control
	default:
		return Checkin
	}
}
```

- [ ] **Step 4: Run — should pass**

- [ ] **Step 5: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/deck/parse.go
fossil commit -m "deck: add Parse() for manifest text to Deck conversion"
```

---

### Task 9: R-card Computation

**Files:**
- Create: `go-libfossil/deck/rcard.go`
- Modify: `go-libfossil/deck/deck_test.go`

- [ ] **Step 1: Add failing tests**

```go
func TestComputeREmpty(t *testing.T) {
	d := &Deck{Type: Checkin}
	r, err := d.ComputeR(nil)
	if err != nil {
		t.Fatalf("ComputeR: %v", err)
	}
	if r != "d41d8cd98f00b204e9800998ecf8427e" {
		t.Fatalf("R = %q, want md5('')", r)
	}
}

func TestComputeRSingleFile(t *testing.T) {
	content := []byte("hello world")
	d := &Deck{
		Type: Checkin,
		F:    []FileCard{{Name: "hello.txt", UUID: "abc123"}},
	}
	getContent := func(uuid string) ([]byte, error) {
		if uuid == "abc123" {
			return content, nil
		}
		return nil, fmt.Errorf("unknown: %s", uuid)
	}
	r, err := d.ComputeR(getContent)
	if err != nil {
		t.Fatalf("ComputeR: %v", err)
	}
	h := md5.New()
	h.Write([]byte("hello.txt"))
	h.Write([]byte(fmt.Sprintf(" %d\n", len(content))))
	h.Write(content)
	want := fmt.Sprintf("%x", h.Sum(nil))
	if r != want {
		t.Fatalf("R = %q, want %q", r, want)
	}
}

func TestComputeRSortedByName(t *testing.T) {
	files := map[string][]byte{"uuid-a": []byte("aaa"), "uuid-b": []byte("bbb")}
	d := &Deck{
		Type: Checkin,
		F: []FileCard{
			{Name: "b.txt", UUID: "uuid-b"},
			{Name: "a.txt", UUID: "uuid-a"},
		},
	}
	getContent := func(uuid string) ([]byte, error) { return files[uuid], nil }
	r, _ := d.ComputeR(getContent)
	h := md5.New()
	h.Write([]byte("a.txt"))
	h.Write([]byte(" 3\n"))
	h.Write([]byte("aaa"))
	h.Write([]byte("b.txt"))
	h.Write([]byte(" 3\n"))
	h.Write([]byte("bbb"))
	if r != fmt.Sprintf("%x", h.Sum(nil)) {
		t.Fatalf("R mismatch — files not sorted?")
	}
}
```

- [ ] **Step 2: Run — should fail**

- [ ] **Step 3: Implement rcard.go**

Create `go-libfossil/deck/rcard.go`:

```go
package deck

import (
	"crypto/md5"
	"encoding/hex"
	"fmt"
	"sort"
)

func (d *Deck) ComputeR(getContent func(uuid string) ([]byte, error)) (string, error) {
	if len(d.F) == 0 {
		return "d41d8cd98f00b204e9800998ecf8427e", nil
	}
	sorted := make([]FileCard, len(d.F))
	copy(sorted, d.F)
	sort.Slice(sorted, func(i, j int) bool { return sorted[i].Name < sorted[j].Name })

	h := md5.New()
	for _, f := range sorted {
		if f.UUID == "" {
			continue
		}
		content, err := getContent(f.UUID)
		if err != nil {
			return "", fmt.Errorf("ComputeR: fetching %q: %w", f.UUID, err)
		}
		h.Write([]byte(f.Name))
		h.Write([]byte(fmt.Sprintf(" %d\n", len(content))))
		h.Write(content)
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}
```

- [ ] **Step 4: Run — should pass**

- [ ] **Step 5: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/deck/rcard.go
fossil commit -m "deck: add ComputeR() for R-card MD5 computation"
```

---

### Task 10: Round-trip Tests and Benchmarks

**Files:**
- Modify: `go-libfossil/deck/deck_test.go`

- [ ] **Step 1: Add round-trip and benchmark tests**

```go
func TestRoundTripCheckin(t *testing.T) {
	d := &Deck{
		Type: Checkin,
		C:    "test with spaces and\nnewlines",
		D:    time.Date(2024, 6, 15, 14, 30, 45, 123000000, time.UTC),
		F: []FileCard{
			{Name: "src/main.go", UUID: "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
			{Name: "README.md", UUID: "aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d", Perm: "x"},
		},
		P: []string{"1234567890123456789012345678901234567890"},
		R: "d41d8cd98f00b204e9800998ecf8427e",
		T: []TagCard{{Type: TagPropagating, Name: "branch", UUID: "*", Value: "trunk"}},
		U: "developer",
	}
	data1, _ := d.Marshal()
	parsed, err := Parse(data1)
	if err != nil {
		t.Fatalf("Parse: %v", err)
	}
	data2, _ := parsed.Marshal()
	if !bytes.Equal(data1, data2) {
		t.Fatalf("round-trip mismatch:\n%s\nvs\n%s", data1, data2)
	}
}

func TestRoundTripWiki(t *testing.T) {
	d := &Deck{
		Type: Wiki,
		D:    time.Date(2024, 1, 1, 0, 0, 0, 0, time.UTC),
		L:    "Test Page",
		N:    "text/x-markdown",
		U:    "admin",
		W:    []byte("# Hello\n\nWiki content."),
	}
	data1, _ := d.Marshal()
	parsed, _ := Parse(data1)
	data2, _ := parsed.Marshal()
	if !bytes.Equal(data1, data2) {
		t.Fatalf("wiki round-trip mismatch")
	}
}

func BenchmarkMarshal(b *testing.B) {
	d := &Deck{
		Type: Checkin,
		C:    "benchmark",
		D:    time.Date(2024, 1, 15, 10, 30, 0, 0, time.UTC),
		P:    []string{"1234567890123456789012345678901234567890"},
		R:    "d41d8cd98f00b204e9800998ecf8427e",
		T:    []TagCard{{Type: TagPropagating, Name: "branch", UUID: "*", Value: "trunk"}},
		U:    "benchuser",
	}
	for i := 0; i < 50; i++ {
		d.F = append(d.F, FileCard{
			Name: fmt.Sprintf("src/file%03d.go", i),
			UUID: "da39a3ee5e6b4b0d3255bfef95601890afd80709",
		})
	}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		d.Marshal()
	}
}

func BenchmarkParse(b *testing.B) {
	d := &Deck{
		Type: Checkin,
		C:    "benchmark",
		D:    time.Date(2024, 1, 15, 10, 30, 0, 0, time.UTC),
		P:    []string{"1234567890123456789012345678901234567890"},
		R:    "d41d8cd98f00b204e9800998ecf8427e",
		T:    []TagCard{{Type: TagPropagating, Name: "branch", UUID: "*", Value: "trunk"}},
		U:    "benchuser",
	}
	for i := 0; i < 50; i++ {
		d.F = append(d.F, FileCard{
			Name: fmt.Sprintf("src/file%03d.go", i),
			UUID: "da39a3ee5e6b4b0d3255bfef95601890afd80709",
		})
	}
	data, _ := d.Marshal()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		Parse(data)
	}
}
```

(Add `"bytes"` to test imports)

- [ ] **Step 2: Run all deck tests + benchmarks**

Run: `cd ~/projects/EdgeSync/go-libfossil && go test ./deck/ -v && go test ./deck/ -bench=. -benchmem`
Expected: all pass

- [ ] **Step 3: Commit**

```bash
cd ~/projects/EdgeSync
fossil commit -m "deck: add round-trip tests and benchmarks"
```

---

## Chunk 4: manifest/ Package — Checkin, GetManifest

### Task 11: testutil Helpers

**Files:**
- Modify: `go-libfossil/testutil/testutil.go`

- [ ] **Step 1: Add FossilArtifact and FossilTimeline helpers**

Check existing testutil.go for `FossilArtifact` and `FossilTimeline`. If missing, add:

```go
func FossilArtifact(repoPath, uuid string) ([]byte, error) {
	cmd := exec.Command("fossil", "artifact", uuid, "-R", repoPath)
	return cmd.Output()
}

func FossilTimeline(repoPath string) ([]byte, error) {
	cmd := exec.Command("fossil", "timeline", "-n", "100", "-R", repoPath)
	return cmd.Output()
}
```

- [ ] **Step 2: Verify compilation**

Run: `cd ~/projects/EdgeSync/go-libfossil && go build ./testutil/...`

- [ ] **Step 3: Commit**

```bash
cd ~/projects/EdgeSync
fossil commit -m "testutil: add FossilArtifact and FossilTimeline helpers"
```

---

### Task 12: Checkin and GetManifest

**Files:**
- Create: `go-libfossil/manifest/manifest.go`
- Create: `go-libfossil/manifest/manifest_test.go`

- [ ] **Step 1: Write failing tests**

Create `go-libfossil/manifest/manifest_test.go`:

```go
package manifest

import (
	"path/filepath"
	"testing"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/deck"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/testutil"
)

func setupTestRepo(t *testing.T) *repo.Repo {
	t.Helper()
	path := filepath.Join(t.TempDir(), "test.fossil")
	r, err := repo.Create(path, "testuser")
	if err != nil {
		t.Fatalf("repo.Create: %v", err)
	}
	t.Cleanup(func() { r.Close() })
	return r
}

func TestCheckinBasic(t *testing.T) {
	r := setupTestRepo(t)
	rid, uuid, err := Checkin(r, CheckinOpts{
		Files:   []File{{Name: "hello.txt", Content: []byte("hello world")}},
		Comment: "initial commit",
		User:    "testuser",
		Time:    time.Date(2024, 1, 15, 10, 30, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("Checkin: %v", err)
	}
	if rid <= 0 {
		t.Fatalf("rid = %d", rid)
	}
	if len(uuid) != 40 && len(uuid) != 64 {
		t.Fatalf("uuid len = %d", len(uuid))
	}
	var comment string
	r.DB().QueryRow("SELECT comment FROM event WHERE objid=?", rid).Scan(&comment)
	if comment != "initial commit" {
		t.Fatalf("event comment = %q", comment)
	}
	var leafCount int
	r.DB().QueryRow("SELECT count(*) FROM leaf WHERE rid=?", rid).Scan(&leafCount)
	if leafCount != 1 {
		t.Fatalf("leaf count = %d", leafCount)
	}
}

func TestCheckinFossilRebuild(t *testing.T) {
	if !testutil.HasFossil() {
		t.Skip("fossil not in PATH")
	}
	r := setupTestRepo(t)
	_, _, err := Checkin(r, CheckinOpts{
		Files: []File{
			{Name: "hello.txt", Content: []byte("hello world")},
			{Name: "src/main.go", Content: []byte("package main\n")},
		},
		Comment: "initial commit",
		User:    "testuser",
		Time:    time.Date(2024, 1, 15, 10, 30, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("Checkin: %v", err)
	}
	r.Close()
	if err := testutil.FossilRebuild(r.Path()); err != nil {
		t.Fatalf("fossil rebuild: %v", err)
	}
}

func TestCheckinMultiple(t *testing.T) {
	r := setupTestRepo(t)
	rid1, _, _ := Checkin(r, CheckinOpts{
		Files:   []File{{Name: "file.txt", Content: []byte("v1")}},
		Comment: "first",
		User:    "testuser",
		Time:    time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
	})
	rid2, _, _ := Checkin(r, CheckinOpts{
		Files:   []File{{Name: "file.txt", Content: []byte("v2")}},
		Comment: "second",
		User:    "testuser",
		Parent:  rid1,
		Time:    time.Date(2024, 1, 15, 11, 0, 0, 0, time.UTC),
	})
	var leafCount int
	r.DB().QueryRow("SELECT count(*) FROM leaf WHERE rid=?", rid1).Scan(&leafCount)
	if leafCount != 0 {
		t.Fatal("rid1 still a leaf")
	}
	r.DB().QueryRow("SELECT count(*) FROM leaf WHERE rid=?", rid2).Scan(&leafCount)
	if leafCount != 1 {
		t.Fatal("rid2 not a leaf")
	}
	var plinkCount int
	r.DB().QueryRow("SELECT count(*) FROM plink WHERE pid=? AND cid=?", rid1, rid2).Scan(&plinkCount)
	if plinkCount != 1 {
		t.Fatal("plink missing")
	}
}

func TestGetManifest(t *testing.T) {
	r := setupTestRepo(t)
	rid, _, _ := Checkin(r, CheckinOpts{
		Files:   []File{{Name: "hello.txt", Content: []byte("hello")}},
		Comment: "test commit",
		User:    "testuser",
		Time:    time.Date(2024, 1, 15, 10, 30, 0, 0, time.UTC),
	})
	d, err := GetManifest(r, rid)
	if err != nil {
		t.Fatalf("GetManifest: %v", err)
	}
	if d.C != "test commit" {
		t.Fatalf("C = %q", d.C)
	}
	if d.Type != deck.Checkin {
		t.Fatalf("Type = %d", d.Type)
	}
	if len(d.F) != 1 || d.F[0].Name != "hello.txt" {
		t.Fatalf("F = %+v", d.F)
	}
}
```

- [ ] **Step 2: Run — should fail**

- [ ] **Step 3: Implement manifest.go**

Create `go-libfossil/manifest/manifest.go`:

```go
package manifest

import (
	"fmt"
	"time"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/deck"
	"github.com/dmestas/edgesync/go-libfossil/hash"
	"github.com/dmestas/edgesync/go-libfossil/julian"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

type CheckinOpts struct {
	Files   []File
	Comment string
	User    string
	Parent  libfossil.FslID
	Delta   bool
	Time    time.Time
}

type File struct {
	Name    string
	Content []byte
	Perm    string
}

func Checkin(r *repo.Repo, opts CheckinOpts) (libfossil.FslID, string, error) {
	if opts.Time.IsZero() {
		opts.Time = time.Now().UTC()
	}

	var manifestRid libfossil.FslID
	var manifestUUID string

	err := r.WithTx(func(tx *db.Tx) error {
		// Store file blobs, build F-cards
		fCards := make([]deck.FileCard, len(opts.Files))
		for i, f := range opts.Files {
			_, uuid, err := blob.Store(tx, f.Content)
			if err != nil {
				return fmt.Errorf("storing file %q: %w", f.Name, err)
			}
			fCards[i] = deck.FileCard{Name: f.Name, UUID: uuid, Perm: f.Perm}
		}

		// Build deck
		d := &deck.Deck{
			Type: deck.Checkin,
			C:    opts.Comment,
			D:    opts.Time,
			F:    fCards,
			U:    opts.User,
		}

		// Parent
		if opts.Parent > 0 {
			var parentUUID string
			if err := tx.QueryRow("SELECT uuid FROM blob WHERE rid=?", opts.Parent).Scan(&parentUUID); err != nil {
				return fmt.Errorf("parent uuid: %w", err)
			}
			d.P = []string{parentUUID}
		}

		// Tags for initial checkin
		if opts.Parent == 0 {
			d.T = []deck.TagCard{
				{Type: deck.TagPropagating, Name: "branch", UUID: "*", Value: "trunk"},
				{Type: deck.TagSingleton, Name: "sym-trunk", UUID: "*"},
			}
		}

		// Delta manifest support
		if opts.Delta && opts.Parent > 0 {
			if err := applyDelta(tx, d, fCards, opts.Parent); err != nil {
				return err
			}
		}

		// R-card (always over full file set)
		rDeck := &deck.Deck{F: fCards}
		getContent := func(uuid string) ([]byte, error) {
			rid, ok := blob.Exists(tx, uuid)
			if !ok {
				return nil, fmt.Errorf("blob not found: %s", uuid)
			}
			return content.Expand(tx, rid)
		}
		rHash, err := rDeck.ComputeR(getContent)
		if err != nil {
			return fmt.Errorf("R-card: %w", err)
		}
		d.R = rHash

		// Marshal and store manifest
		manifestBytes, err := d.Marshal()
		if err != nil {
			return fmt.Errorf("marshal: %w", err)
		}
		manifestRid, manifestUUID, err = blob.Store(tx, manifestBytes)
		if err != nil {
			return fmt.Errorf("store manifest: %w", err)
		}

		// filename + mlink
		for _, f := range opts.Files {
			fnid, err := ensureFilename(tx, f.Name)
			if err != nil {
				return fmt.Errorf("filename %q: %w", f.Name, err)
			}
			fileUUID := hash.SHA1(f.Content)
			fileRid, _ := blob.Exists(tx, fileUUID)
			var pmid, pid int64
			if opts.Parent > 0 {
				pmid = int64(opts.Parent)
				tx.QueryRow("SELECT fid FROM mlink WHERE mid=? AND fnid=?", opts.Parent, fnid).Scan(&pid)
			}
			if _, err := tx.Exec(
				"INSERT INTO mlink(mid, fid, pmid, pid, fnid) VALUES(?, ?, ?, ?, ?)",
				manifestRid, fileRid, pmid, pid, fnid,
			); err != nil {
				return fmt.Errorf("mlink: %w", err)
			}
		}

		// plink
		if opts.Parent > 0 {
			if _, err := tx.Exec(
				"INSERT INTO plink(pid, cid, isprim, mtime) VALUES(?, ?, 1, ?)",
				opts.Parent, manifestRid, julian.ToJulian(opts.Time),
			); err != nil {
				return fmt.Errorf("plink: %w", err)
			}
		}

		// event
		if _, err := tx.Exec(
			"INSERT INTO event(type, mtime, objid, user, comment) VALUES('ci', ?, ?, ?, ?)",
			julian.ToJulian(opts.Time), manifestRid, opts.User, opts.Comment,
		); err != nil {
			return fmt.Errorf("event: %w", err)
		}

		// leaf
		tx.Exec("INSERT OR IGNORE INTO leaf(rid) VALUES(?)", manifestRid)
		if opts.Parent > 0 {
			tx.Exec("DELETE FROM leaf WHERE rid=?", opts.Parent)
		}

		// unclustered + unsent
		tx.Exec("INSERT OR IGNORE INTO unclustered(rid) VALUES(?)", manifestRid)
		tx.Exec("INSERT OR IGNORE INTO unsent(rid) VALUES(?)", manifestRid)

		return nil
	})
	if err != nil {
		return 0, "", fmt.Errorf("manifest.Checkin: %w", err)
	}
	return manifestRid, manifestUUID, nil
}

func applyDelta(tx *db.Tx, d *deck.Deck, fullFCards []deck.FileCard, parentRid libfossil.FslID) error {
	parentData, err := content.Expand(tx, parentRid)
	if err != nil {
		return fmt.Errorf("expand parent: %w", err)
	}
	parentDeck, err := deck.Parse(parentData)
	if err != nil {
		return fmt.Errorf("parse parent: %w", err)
	}

	baselineUUID := parentDeck.B
	if baselineUUID == "" {
		var puuid string
		tx.QueryRow("SELECT uuid FROM blob WHERE rid=?", parentRid).Scan(&puuid)
		baselineUUID = puuid
	}

	baseRid, ok := blob.Exists(tx, baselineUUID)
	if !ok {
		return fmt.Errorf("baseline %s not found", baselineUUID)
	}
	baseData, err := content.Expand(tx, baseRid)
	if err != nil {
		return fmt.Errorf("expand baseline: %w", err)
	}
	baseDeck, err := deck.Parse(baseData)
	if err != nil {
		return fmt.Errorf("parse baseline: %w", err)
	}

	baseFiles := make(map[string]string)
	for _, f := range baseDeck.F {
		baseFiles[f.Name] = f.UUID
	}

	var deltaFCards []deck.FileCard
	currentFiles := make(map[string]bool)
	for _, f := range fullFCards {
		currentFiles[f.Name] = true
		if baseUUID, exists := baseFiles[f.Name]; !exists || baseUUID != f.UUID {
			deltaFCards = append(deltaFCards, f)
		}
	}
	for name := range baseFiles {
		if !currentFiles[name] {
			deltaFCards = append(deltaFCards, deck.FileCard{Name: name})
		}
	}

	if len(deltaFCards) < len(fullFCards) {
		d.B = baselineUUID
		d.F = deltaFCards
	}
	return nil
}

func ensureFilename(tx *db.Tx, name string) (int64, error) {
	var fnid int64
	err := tx.QueryRow("SELECT fnid FROM filename WHERE name=?", name).Scan(&fnid)
	if err == nil {
		return fnid, nil
	}
	result, err := tx.Exec("INSERT INTO filename(name) VALUES(?)", name)
	if err != nil {
		return 0, err
	}
	return result.LastInsertId()
}

func GetManifest(r *repo.Repo, rid libfossil.FslID) (*deck.Deck, error) {
	data, err := content.Expand(r.DB(), rid)
	if err != nil {
		return nil, fmt.Errorf("manifest.GetManifest: %w", err)
	}
	return deck.Parse(data)
}
```

- [ ] **Step 4: Run — should pass**

Run: `cd ~/projects/EdgeSync/go-libfossil && go test ./manifest/ -v`

- [ ] **Step 5: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/manifest/manifest.go go-libfossil/manifest/manifest_test.go
fossil commit -m "manifest: add Checkin() and GetManifest()"
```

---

## Chunk 5: manifest/ Package — ListFiles, Log, Delta Tests

### Task 13: ListFiles

**Files:**
- Create: `go-libfossil/manifest/files.go`
- Modify: `go-libfossil/manifest/manifest_test.go`

- [ ] **Step 1: Add failing test**

```go
func TestListFilesBaseline(t *testing.T) {
	r := setupTestRepo(t)
	rid, _, _ := Checkin(r, CheckinOpts{
		Files: []File{
			{Name: "a.txt", Content: []byte("aaa")},
			{Name: "b.txt", Content: []byte("bbb")},
		},
		Comment: "initial",
		User:    "testuser",
		Time:    time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
	})
	files, err := ListFiles(r, rid)
	if err != nil {
		t.Fatalf("ListFiles: %v", err)
	}
	if len(files) != 2 {
		t.Fatalf("count = %d", len(files))
	}
}
```

- [ ] **Step 2: Run — should fail**

- [ ] **Step 3: Implement files.go**

Create `go-libfossil/manifest/files.go`:

```go
package manifest

import (
	"fmt"
	"sort"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/deck"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

type FileEntry struct {
	Name string
	UUID string
	Perm string
}

func ListFiles(r *repo.Repo, rid libfossil.FslID) ([]FileEntry, error) {
	d, err := GetManifest(r, rid)
	if err != nil {
		return nil, fmt.Errorf("manifest.ListFiles: %w", err)
	}
	if d.B == "" {
		return fCardsToEntries(d.F), nil
	}
	baseRid, ok := blob.Exists(r.DB(), d.B)
	if !ok {
		return nil, fmt.Errorf("manifest.ListFiles: baseline %s not found", d.B)
	}
	baseData, err := content.Expand(r.DB(), baseRid)
	if err != nil {
		return nil, fmt.Errorf("manifest.ListFiles: expand baseline: %w", err)
	}
	baseDeck, err := deck.Parse(baseData)
	if err != nil {
		return nil, fmt.Errorf("manifest.ListFiles: parse baseline: %w", err)
	}
	fileMap := make(map[string]FileEntry)
	for _, f := range baseDeck.F {
		fileMap[f.Name] = FileEntry{Name: f.Name, UUID: f.UUID, Perm: f.Perm}
	}
	for _, f := range d.F {
		if f.UUID == "" {
			delete(fileMap, f.Name)
		} else {
			fileMap[f.Name] = FileEntry{Name: f.Name, UUID: f.UUID, Perm: f.Perm}
		}
	}
	entries := make([]FileEntry, 0, len(fileMap))
	for _, e := range fileMap {
		entries = append(entries, e)
	}
	sort.Slice(entries, func(i, j int) bool { return entries[i].Name < entries[j].Name })
	return entries, nil
}

func fCardsToEntries(fCards []deck.FileCard) []FileEntry {
	entries := make([]FileEntry, len(fCards))
	for i, f := range fCards {
		entries[i] = FileEntry{Name: f.Name, UUID: f.UUID, Perm: f.Perm}
	}
	sort.Slice(entries, func(i, j int) bool { return entries[i].Name < entries[j].Name })
	return entries
}
```

- [ ] **Step 4: Run — should pass**

- [ ] **Step 5: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/manifest/files.go
fossil commit -m "manifest: add ListFiles() with delta resolution"
```

---

### Task 14: Log

**Files:**
- Create: `go-libfossil/manifest/log.go`
- Modify: `go-libfossil/manifest/manifest_test.go`

- [ ] **Step 1: Add failing tests**

```go
func TestLogMultiple(t *testing.T) {
	r := setupTestRepo(t)
	rid1, _, _ := Checkin(r, CheckinOpts{
		Files: []File{{Name: "a.txt", Content: []byte("v1")}},
		Comment: "first", User: "testuser",
		Time: time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
	})
	rid2, _, _ := Checkin(r, CheckinOpts{
		Files: []File{{Name: "a.txt", Content: []byte("v2")}},
		Comment: "second", User: "testuser", Parent: rid1,
		Time: time.Date(2024, 1, 15, 11, 0, 0, 0, time.UTC),
	})
	rid3, _, _ := Checkin(r, CheckinOpts{
		Files: []File{{Name: "a.txt", Content: []byte("v3")}},
		Comment: "third", User: "testuser", Parent: rid2,
		Time: time.Date(2024, 1, 15, 12, 0, 0, 0, time.UTC),
	})
	entries, err := Log(r, LogOpts{Start: rid3})
	if err != nil {
		t.Fatalf("Log: %v", err)
	}
	if len(entries) != 3 {
		t.Fatalf("count = %d", len(entries))
	}
	if entries[0].Comment != "third" || entries[2].Comment != "first" {
		t.Fatalf("order: %q %q %q", entries[0].Comment, entries[1].Comment, entries[2].Comment)
	}
}

func TestLogWithLimit(t *testing.T) {
	r := setupTestRepo(t)
	rid1, _, _ := Checkin(r, CheckinOpts{
		Files: []File{{Name: "a.txt", Content: []byte("v1")}},
		Comment: "first", User: "testuser",
		Time: time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
	})
	rid2, _, _ := Checkin(r, CheckinOpts{
		Files: []File{{Name: "a.txt", Content: []byte("v2")}},
		Comment: "second", User: "testuser", Parent: rid1,
		Time: time.Date(2024, 1, 15, 11, 0, 0, 0, time.UTC),
	})
	entries, _ := Log(r, LogOpts{Start: rid2, Limit: 1})
	if len(entries) != 1 {
		t.Fatalf("count = %d", len(entries))
	}
}
```

- [ ] **Step 2: Run — should fail**

- [ ] **Step 3: Implement log.go**

Create `go-libfossil/manifest/log.go`:

```go
package manifest

import (
	"fmt"
	"time"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/julian"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

type LogOpts struct {
	Start libfossil.FslID
	Limit int
}

type LogEntry struct {
	RID     libfossil.FslID
	UUID    string
	Comment string
	User    string
	Time    time.Time
	Parents []string
}

func Log(r *repo.Repo, opts LogOpts) ([]LogEntry, error) {
	if opts.Start <= 0 {
		return nil, fmt.Errorf("manifest.Log: invalid start rid %d", opts.Start)
	}
	var entries []LogEntry
	current := opts.Start
	for {
		if opts.Limit > 0 && len(entries) >= opts.Limit {
			break
		}
		var uuid, user, comment string
		var mtime float64
		err := r.DB().QueryRow(
			"SELECT b.uuid, e.user, e.comment, e.mtime FROM blob b JOIN event e ON e.objid=b.rid WHERE b.rid=?",
			current,
		).Scan(&uuid, &user, &comment, &mtime)
		if err != nil {
			return nil, fmt.Errorf("manifest.Log: rid=%d: %w", current, err)
		}
		var parents []string
		rows, err := r.DB().Query(
			"SELECT b.uuid FROM plink p JOIN blob b ON b.rid=p.pid WHERE p.cid=? ORDER BY p.isprim DESC",
			current,
		)
		if err == nil {
			for rows.Next() {
				var puuid string
				rows.Scan(&puuid)
				parents = append(parents, puuid)
			}
			rows.Close()
		}
		entries = append(entries, LogEntry{
			RID: current, UUID: uuid, Comment: comment,
			User: user, Time: julian.FromJulian(mtime), Parents: parents,
		})
		var parentRid int64
		if err := r.DB().QueryRow(
			"SELECT pid FROM plink WHERE cid=? AND isprim=1", current,
		).Scan(&parentRid); err != nil {
			break
		}
		current = libfossil.FslID(parentRid)
	}
	return entries, nil
}
```

- [ ] **Step 4: Run — should pass**

- [ ] **Step 5: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/manifest/log.go
fossil commit -m "manifest: add Log() for walking checkin history"
```

---

### Task 15: Delta Checkin Tests

**Files:**
- Modify: `go-libfossil/manifest/manifest_test.go`

- [ ] **Step 1: Add delta tests**

```go
func TestCheckinDelta(t *testing.T) {
	r := setupTestRepo(t)
	rid1, _, _ := Checkin(r, CheckinOpts{
		Files: []File{
			{Name: "a.txt", Content: []byte("aaa")},
			{Name: "b.txt", Content: []byte("bbb")},
		},
		Comment: "baseline", User: "testuser",
		Time: time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
	})
	rid2, _, err := Checkin(r, CheckinOpts{
		Files: []File{
			{Name: "a.txt", Content: []byte("aaa-modified")},
			{Name: "b.txt", Content: []byte("bbb")},
		},
		Comment: "delta", User: "testuser", Parent: rid1, Delta: true,
		Time: time.Date(2024, 1, 15, 11, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("Checkin delta: %v", err)
	}
	d, _ := GetManifest(r, rid2)
	if d.B == "" {
		t.Fatal("delta should have B-card")
	}
	if len(d.F) != 1 || d.F[0].Name != "a.txt" {
		t.Fatalf("delta F = %+v, want only a.txt", d.F)
	}
	files, _ := ListFiles(r, rid2)
	if len(files) != 2 {
		t.Fatalf("ListFiles = %d, want 2", len(files))
	}
}

func TestCheckinDeltaFossilRebuild(t *testing.T) {
	if !testutil.HasFossil() {
		t.Skip("fossil not in PATH")
	}
	r := setupTestRepo(t)
	rid1, _, _ := Checkin(r, CheckinOpts{
		Files: []File{
			{Name: "a.txt", Content: []byte("aaa")},
			{Name: "b.txt", Content: []byte("bbb")},
		},
		Comment: "baseline", User: "testuser",
		Time: time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
	})
	_, _, err := Checkin(r, CheckinOpts{
		Files: []File{
			{Name: "a.txt", Content: []byte("aaa-v2")},
			{Name: "b.txt", Content: []byte("bbb")},
			{Name: "c.txt", Content: []byte("new file")},
		},
		Comment: "delta with add", User: "testuser", Parent: rid1, Delta: true,
		Time: time.Date(2024, 1, 15, 11, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("delta: %v", err)
	}
	r.Close()
	if err := testutil.FossilRebuild(r.Path()); err != nil {
		t.Fatalf("rebuild: %v", err)
	}
}
```

- [ ] **Step 2: Run — should pass**

Run: `cd ~/projects/EdgeSync/go-libfossil && go test ./manifest/ -v`

- [ ] **Step 3: Commit**

```bash
cd ~/projects/EdgeSync
fossil commit -m "manifest: add delta checkin tests with fossil rebuild oracle"
```

---

## Chunk 6: Integration Tests, Benchmarks, Validation

### Task 16: Phase B Integration Test

**Files:**
- Modify: `go-libfossil/integration_test.go`

- [ ] **Step 1: Add Phase B integration test**

Add imports for `manifest` package, then:

```go
func TestPhaseBIntegration(t *testing.T) {
	if !testutil.HasFossil() {
		t.Skip("fossil not in PATH")
	}
	path := filepath.Join(t.TempDir(), "integration-b.fossil")
	r, err := repo.Create(path, "integration-user")
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	defer r.Close()

	var lastRid libfossil.FslID
	for i := 1; i <= 3; i++ {
		rid, _, err := manifest.Checkin(r, manifest.CheckinOpts{
			Files: []manifest.File{
				{Name: "file.txt", Content: []byte(fmt.Sprintf("v%d", i))},
				{Name: fmt.Sprintf("file%d.txt", i), Content: []byte(fmt.Sprintf("new-%d", i))},
			},
			Comment: fmt.Sprintf("commit %d", i),
			User:    "integration-user",
			Parent:  lastRid,
			Time:    time.Date(2024, 1, 15, 10+i, 0, 0, 0, time.UTC),
		})
		if err != nil {
			t.Fatalf("Checkin %d: %v", i, err)
		}
		lastRid = rid
	}

	r.Close()
	if err := testutil.FossilRebuild(path); err != nil {
		t.Fatalf("rebuild: %v", err)
	}
	out, err := testutil.FossilTimeline(path)
	if err != nil {
		t.Fatalf("timeline: %v", err)
	}
	for i := 1; i <= 3; i++ {
		if !bytes.Contains(out, []byte(fmt.Sprintf("commit %d", i))) {
			t.Fatalf("timeline missing 'commit %d'", i)
		}
	}
}
```

- [ ] **Step 2: Run**

Run: `cd ~/projects/EdgeSync/go-libfossil && go test -run TestPhaseBIntegration -v`

- [ ] **Step 3: Commit**

```bash
cd ~/projects/EdgeSync
fossil commit -m "integration: add Phase B multi-checkin + timeline test"
```

---

### Task 17: Benchmarks

**Files:**
- Modify: `go-libfossil/manifest/manifest_test.go`

- [ ] **Step 1: Add benchmarks**

```go
func BenchmarkCheckin(b *testing.B) {
	path := filepath.Join(b.TempDir(), "bench.fossil")
	r, _ := repo.Create(path, "bench")
	defer r.Close()
	files := make([]File, 10)
	for i := range files {
		files[i] = File{Name: fmt.Sprintf("src/file%03d.go", i), Content: bytes.Repeat([]byte("x"), 1000)}
	}
	b.ResetTimer()
	var lastRid libfossil.FslID
	for i := 0; i < b.N; i++ {
		rid, _, _ := Checkin(r, CheckinOpts{
			Files: files, Comment: "bench", User: "bench",
			Parent: lastRid, Time: time.Now().UTC(),
		})
		lastRid = rid
	}
}

func BenchmarkListFiles(b *testing.B) {
	path := filepath.Join(b.TempDir(), "bench.fossil")
	r, _ := repo.Create(path, "bench")
	defer r.Close()
	files := make([]File, 50)
	for i := range files {
		files[i] = File{Name: fmt.Sprintf("src/file%03d.go", i), Content: []byte(fmt.Sprintf("c-%d", i))}
	}
	rid, _, _ := Checkin(r, CheckinOpts{
		Files: files, Comment: "bench", User: "bench", Time: time.Now().UTC(),
	})
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		ListFiles(r, rid)
	}
}
```

(Add `"bytes"` and `"fmt"` to test imports)

- [ ] **Step 2: Run benchmarks**

Run: `cd ~/projects/EdgeSync/go-libfossil && go test ./manifest/ -bench=. -benchmem`

- [ ] **Step 3: Commit**

```bash
cd ~/projects/EdgeSync
fossil commit -m "manifest: add Checkin and ListFiles benchmarks"
```

---

### Task 18: Full Validation

- [ ] **Step 1: go vet**

Run: `cd ~/projects/EdgeSync/go-libfossil && go vet ./...`

- [ ] **Step 2: All tests**

Run: `cd ~/projects/EdgeSync/go-libfossil && go test ./...`

- [ ] **Step 3: Race detector**

Run: `cd ~/projects/EdgeSync/go-libfossil && go test -race ./...`

- [ ] **Step 4: Benchmarks**

Run: `cd ~/projects/EdgeSync/go-libfossil && go test -bench=. -benchmem ./...`

- [ ] **Step 5: Coverage**

Run: `cd ~/projects/EdgeSync/go-libfossil && go test -cover ./...`

- [ ] **Step 6: Final commit**

```bash
cd ~/projects/EdgeSync
fossil commit -m "Phase B complete: manifest parsing, serialization, and checkin operations"
```
