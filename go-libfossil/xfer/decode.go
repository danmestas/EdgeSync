package xfer

import (
	"bufio"
	"bytes"
	"compress/zlib"
	"fmt"
	"io"
	"strconv"
	"strings"

	"github.com/dmestas/edgesync/go-libfossil/deck"
)

// readPayload reads exactly size bytes from the reader.
func readPayload(r *bufio.Reader, size int) ([]byte, error) {
	if size == 0 {
		return []byte{}, nil
	}
	buf := make([]byte, size)
	_, err := io.ReadFull(r, buf)
	if err != nil {
		return nil, fmt.Errorf("read payload: %w", err)
	}
	return buf, nil
}

// readPayloadWithTrailingNewline reads size bytes then consumes one trailing \n.
func readPayloadWithTrailingNewline(r *bufio.Reader, size int) ([]byte, error) {
	buf, err := readPayload(r, size)
	if err != nil {
		return nil, err
	}
	b, err := r.ReadByte()
	if err == nil && b != '\n' {
		r.UnreadByte()
	}
	return buf, nil
}

// uvFileOmitsContent returns true if the flags indicate no binary payload follows.
func uvFileOmitsContent(flags int) bool {
	return flags&0x0001 != 0 || flags&0x0004 != 0
}

// DecodeCard reads one card from r. It skips comment lines (starting with #)
// and empty lines. Returns io.EOF when the reader is exhausted.
// Unrecognized command words produce an *UnknownCard (not an error).
func DecodeCard(r *bufio.Reader) (Card, error) {
	for {
		line, err := r.ReadString('\n')
		// Handle final line without trailing newline
		if err != nil && err != io.EOF {
			return nil, err
		}
		line = strings.TrimRight(line, "\n\r")

		// EOF with no data left
		if err == io.EOF && line == "" {
			return nil, io.EOF
		}

		// Skip empty lines and comments
		if line == "" || strings.HasPrefix(line, "#") {
			if err == io.EOF {
				return nil, io.EOF
			}
			continue
		}

		card, parseErr := parseLine(r, line)
		if parseErr != nil {
			return nil, parseErr
		}
		return card, nil
	}
}

// parseLine dispatches a trimmed, non-empty line to the appropriate card parser.
// The reader r is passed through for payload cards that need to read binary data.
func parseLine(r *bufio.Reader, line string) (Card, error) {
	fields := strings.Fields(line)
	if len(fields) == 0 {
		return nil, fmt.Errorf("xfer: empty line after split")
	}
	cmd := fields[0]
	args := fields[1:]

	switch cmd {
	case "igot":
		return parseIGot(args)
	case "gimme":
		return parseGimme(args)
	case "push":
		return parsePush(args)
	case "pull":
		return parsePull(args)
	case "cookie":
		return parseCookie(args)
	case "reqconfig":
		return parseReqConfig(args)
	case "private":
		return parsePrivate(args)
	case "clone":
		return parseClone(args)
	case "clone_seqno":
		return parseCloneSeqNo(args)
	case "uvgimme":
		return parseUVGimme(args)
	case "pragma":
		return parsePragma(args)
	case "login":
		return parseLogin(args)
	case "error":
		return parseError(args)
	case "message":
		return parseMessage(args)
	case "uvigot":
		return parseUVIGot(args)
	case "file":
		return parseFile(r, args)
	case "cfile":
		return parseCFile(r, args)
	case "config":
		return parseConfig(r, args)
	case "uvfile":
		return parseUVFile(r, args)
	default:
		return &UnknownCard{Command: cmd, Args: args}, nil
	}
}

func parseIGot(args []string) (Card, error) {
	if len(args) < 1 || len(args) > 2 {
		return nil, fmt.Errorf("xfer: igot requires 1-2 args, got %d", len(args))
	}
	c := &IGotCard{UUID: args[0]}
	if len(args) == 2 && args[1] == "1" {
		c.IsPrivate = true
	}
	return c, nil
}

func parseGimme(args []string) (Card, error) {
	if len(args) != 1 {
		return nil, fmt.Errorf("xfer: gimme requires 1 arg, got %d", len(args))
	}
	return &GimmeCard{UUID: args[0]}, nil
}

func parsePush(args []string) (Card, error) {
	if len(args) != 2 {
		return nil, fmt.Errorf("xfer: push requires 2 args, got %d", len(args))
	}
	return &PushCard{ServerCode: args[0], ProjectCode: args[1]}, nil
}

func parsePull(args []string) (Card, error) {
	if len(args) != 2 {
		return nil, fmt.Errorf("xfer: pull requires 2 args, got %d", len(args))
	}
	return &PullCard{ServerCode: args[0], ProjectCode: args[1]}, nil
}

func parseCookie(args []string) (Card, error) {
	if len(args) != 1 {
		return nil, fmt.Errorf("xfer: cookie requires 1 arg, got %d", len(args))
	}
	return &CookieCard{Value: args[0]}, nil
}

func parseReqConfig(args []string) (Card, error) {
	if len(args) != 1 {
		return nil, fmt.Errorf("xfer: reqconfig requires 1 arg, got %d", len(args))
	}
	return &ReqConfigCard{Name: args[0]}, nil
}

func parsePrivate(args []string) (Card, error) {
	if len(args) != 0 {
		return nil, fmt.Errorf("xfer: private takes 0 args, got %d", len(args))
	}
	return &PrivateCard{}, nil
}

func parseClone(args []string) (Card, error) {
	if len(args) != 0 && len(args) != 2 {
		return nil, fmt.Errorf("xfer: clone requires 0 or 2 args, got %d", len(args))
	}
	c := &CloneCard{}
	if len(args) == 2 {
		v, err := strconv.Atoi(args[0])
		if err != nil {
			return nil, fmt.Errorf("xfer: clone version: %w", err)
		}
		s, err := strconv.Atoi(args[1])
		if err != nil {
			return nil, fmt.Errorf("xfer: clone seqno: %w", err)
		}
		c.Version = v
		c.SeqNo = s
	}
	return c, nil
}

func parseCloneSeqNo(args []string) (Card, error) {
	if len(args) != 1 {
		return nil, fmt.Errorf("xfer: clone_seqno requires 1 arg, got %d", len(args))
	}
	s, err := strconv.Atoi(args[0])
	if err != nil {
		return nil, fmt.Errorf("xfer: clone_seqno: %w", err)
	}
	return &CloneSeqNoCard{SeqNo: s}, nil
}

func parseUVGimme(args []string) (Card, error) {
	if len(args) != 1 {
		return nil, fmt.Errorf("xfer: uvgimme requires 1 arg, got %d", len(args))
	}
	return &UVGimmeCard{Name: args[0]}, nil
}

func parsePragma(args []string) (Card, error) {
	if len(args) < 1 {
		return nil, fmt.Errorf("xfer: pragma requires at least 1 arg, got %d", len(args))
	}
	c := &PragmaCard{Name: args[0]}
	if len(args) > 1 {
		c.Values = args[1:]
	}
	return c, nil
}

func parseLogin(args []string) (Card, error) {
	if len(args) != 3 {
		return nil, fmt.Errorf("xfer: login requires 3 args, got %d", len(args))
	}
	return &LoginCard{
		User:      deck.FossilDecode(args[0]),
		Nonce:     args[1],
		Signature: args[2],
	}, nil
}

func parseError(args []string) (Card, error) {
	if len(args) != 1 {
		return nil, fmt.Errorf("xfer: error requires 1 arg, got %d", len(args))
	}
	return &ErrorCard{Message: deck.FossilDecode(args[0])}, nil
}

func parseMessage(args []string) (Card, error) {
	if len(args) != 1 {
		return nil, fmt.Errorf("xfer: message requires 1 arg, got %d", len(args))
	}
	return &MessageCard{Message: deck.FossilDecode(args[0])}, nil
}

func parseUVIGot(args []string) (Card, error) {
	if len(args) != 4 {
		return nil, fmt.Errorf("xfer: uvigot requires 4 args, got %d", len(args))
	}
	mtime, err := strconv.ParseInt(args[1], 10, 64)
	if err != nil {
		return nil, fmt.Errorf("xfer: uvigot mtime: %w", err)
	}
	size, err := strconv.Atoi(args[3])
	if err != nil {
		return nil, fmt.Errorf("xfer: uvigot size: %w", err)
	}
	return &UVIGotCard{
		Name:  args[0],
		MTime: mtime,
		Hash:  args[2],
		Size:  size,
	}, nil
}

// --- Payload card parsers ---

// parseFile decodes: file UUID SIZE \n CONTENT  or  file UUID DELTASRC SIZE \n CONTENT
// No trailing newline after CONTENT.
func parseFile(r *bufio.Reader, args []string) (Card, error) {
	if len(args) != 2 && len(args) != 3 {
		return nil, fmt.Errorf("xfer: file requires 2-3 args, got %d", len(args))
	}
	c := &FileCard{UUID: args[0]}
	var sizeStr string
	if len(args) == 3 {
		c.DeltaSrc = args[1]
		sizeStr = args[2]
	} else {
		sizeStr = args[1]
	}
	size, err := strconv.Atoi(sizeStr)
	if err != nil {
		return nil, fmt.Errorf("xfer: file size: %w", err)
	}
	content, err := readPayload(r, size)
	if err != nil {
		return nil, fmt.Errorf("xfer: file payload: %w", err)
	}
	c.Content = content
	return c, nil
}

// parseCFile decodes: cfile UUID USIZE CSIZE \n ZCONTENT
// or: cfile UUID DELTASRC USIZE CSIZE \n ZCONTENT
// No trailing newline after ZCONTENT.
func parseCFile(r *bufio.Reader, args []string) (Card, error) {
	if len(args) != 3 && len(args) != 4 {
		return nil, fmt.Errorf("xfer: cfile requires 3-4 args, got %d", len(args))
	}
	c := &CFileCard{UUID: args[0]}
	var usizeStr, csizeStr string
	if len(args) == 4 {
		c.DeltaSrc = args[1]
		usizeStr = args[2]
		csizeStr = args[3]
	} else {
		usizeStr = args[1]
		csizeStr = args[2]
	}
	usize, err := strconv.Atoi(usizeStr)
	if err != nil {
		return nil, fmt.Errorf("xfer: cfile usize: %w", err)
	}
	csize, err := strconv.Atoi(csizeStr)
	if err != nil {
		return nil, fmt.Errorf("xfer: cfile csize: %w", err)
	}
	compressed, err := readPayload(r, csize)
	if err != nil {
		return nil, fmt.Errorf("xfer: cfile payload: %w", err)
	}
	// Decompress zlib
	zr, err := zlib.NewReader(bytes.NewReader(compressed))
	if err != nil {
		return nil, fmt.Errorf("xfer: cfile zlib init: %w", err)
	}
	defer zr.Close()
	decompressed, err := io.ReadAll(zr)
	if err != nil {
		return nil, fmt.Errorf("xfer: cfile zlib decompress: %w", err)
	}
	if len(decompressed) != usize {
		return nil, fmt.Errorf("xfer: cfile usize mismatch: header says %d, got %d", usize, len(decompressed))
	}
	c.USize = usize
	c.Content = decompressed
	return c, nil
}

// parseConfig decodes: config NAME SIZE \n CONTENT \n
// Note the trailing \n after CONTENT.
func parseConfig(r *bufio.Reader, args []string) (Card, error) {
	if len(args) != 2 {
		return nil, fmt.Errorf("xfer: config requires 2 args, got %d", len(args))
	}
	name := args[0]
	size, err := strconv.Atoi(args[1])
	if err != nil {
		return nil, fmt.Errorf("xfer: config size: %w", err)
	}
	content, err := readPayloadWithTrailingNewline(r, size)
	if err != nil {
		return nil, fmt.Errorf("xfer: config payload: %w", err)
	}
	return &ConfigCard{Name: name, Content: content}, nil
}

// parseUVFile decodes: uvfile NAME MTIME HASH SIZE FLAGS \n CONTENT
// FLAGS bits: 0x0001 = deleted, 0x0004 = content omitted. When set, no payload follows.
func parseUVFile(r *bufio.Reader, args []string) (Card, error) {
	if len(args) != 5 {
		return nil, fmt.Errorf("xfer: uvfile requires 5 args, got %d", len(args))
	}
	mtime, err := strconv.ParseInt(args[1], 10, 64)
	if err != nil {
		return nil, fmt.Errorf("xfer: uvfile mtime: %w", err)
	}
	size, err := strconv.Atoi(args[3])
	if err != nil {
		return nil, fmt.Errorf("xfer: uvfile size: %w", err)
	}
	flags, err := strconv.Atoi(args[4])
	if err != nil {
		return nil, fmt.Errorf("xfer: uvfile flags: %w", err)
	}
	c := &UVFileCard{
		Name:  args[0],
		MTime: mtime,
		Hash:  args[2],
		Size:  size,
		Flags: flags,
	}
	if !uvFileOmitsContent(flags) {
		content, err := readPayload(r, size)
		if err != nil {
			return nil, fmt.Errorf("xfer: uvfile payload: %w", err)
		}
		c.Content = content
	}
	return c, nil
}
