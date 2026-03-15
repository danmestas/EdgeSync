// Package card implements the Fossil xfer card protocol.
//
// The Fossil sync protocol uses newline-separated "cards" — text lines
// like "command arg1 arg2". This package encodes and decodes these cards
// for translation between NATS messages and HTTP /xfer payloads.
//
// Reference: https://fossil-scm.org/home/doc/tip/www/sync.wiki
package card

import (
	"fmt"
	"strings"
)

// Type represents a card command type.
type Type string

const (
	TypeLogin  Type = "login"
	TypePush   Type = "push"
	TypePull   Type = "pull"
	TypeFile   Type = "file"
	TypeCFile  Type = "cfile"
	TypeIGot   Type = "igot"
	TypeGimme  Type = "gimme"
	TypeCookie Type = "cookie"
	TypeClone  Type = "clone"
)

// Card represents a single xfer protocol card.
type Card struct {
	Type Type
	Args []string
	Data []byte // For file/cfile cards, the artifact payload
}

// String encodes the card as a protocol line (without trailing newline).
func (c *Card) String() string {
	if len(c.Args) == 0 {
		return string(c.Type)
	}
	return string(c.Type) + " " + strings.Join(c.Args, " ")
}

// IGot creates an "igot UUID" card.
func IGot(uuid string) *Card {
	return &Card{Type: TypeIGot, Args: []string{uuid}}
}

// Gimme creates a "gimme UUID" card.
func Gimme(uuid string) *Card {
	return &Card{Type: TypeGimme, Args: []string{uuid}}
}

// File creates a "file UUID SIZE" card with artifact data.
func File(uuid string, data []byte) *Card {
	return &Card{
		Type: TypeFile,
		Args: []string{uuid, fmt.Sprintf("%d", len(data))},
		Data: data,
	}
}

// Parse parses a single card line into a Card struct.
// Does not handle the data payload for file/cfile cards.
func Parse(line string) (*Card, error) {
	line = strings.TrimRight(line, "\r\n")
	if line == "" {
		return nil, fmt.Errorf("card: empty line")
	}

	parts := strings.SplitN(line, " ", 2)
	c := &Card{Type: Type(parts[0])}
	if len(parts) > 1 {
		c.Args = strings.Fields(parts[1])
	}
	return c, nil
}
