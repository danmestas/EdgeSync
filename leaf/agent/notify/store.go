package notify

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"sort"
	"strings"
	"time"

	libfossil "github.com/danmestas/libfossil"
)

// InitNotifyRepo creates a new notify.fossil repo at the given path.
// Returns the opened *libfossil.Repo — caller owns it and must Close() it.
func InitNotifyRepo(path string) (*libfossil.Repo, error) {
	return libfossil.Create(path, libfossil.CreateOpts{
		User: "notify",
	})
}

// CommitMessage serializes a message to JSON and commits it to the repo.
func CommitMessage(r *libfossil.Repo, msg Message) error {
	data, err := json.MarshalIndent(msg, "", "  ")
	if err != nil {
		return fmt.Errorf("marshal message: %w", err)
	}

	_, _, err = r.Commit(libfossil.CommitOpts{
		Files: []libfossil.FileToCommit{
			{Name: msg.FilePath(), Content: data},
		},
		Comment: "notify: " + msg.ID,
		User:    "notify",
	})
	if err != nil {
		return fmt.Errorf("commit message: %w", err)
	}
	return nil
}

// ReadMessage reads and deserializes a message from the repo by its file path.
// It queries the repo database for the file content across all checkins.
func ReadMessage(r *libfossil.Repo, filePath string) (Message, error) {
	data, err := readFileContent(r, filePath)
	if err != nil {
		return Message{}, err
	}

	var msg Message
	if err := json.Unmarshal(data, &msg); err != nil {
		return Message{}, fmt.Errorf("unmarshal message: %w", err)
	}
	return msg, nil
}

// ThreadSummary is a summary of a thread for the thread list.
type ThreadSummary struct {
	ThreadShort  string
	Project      string
	LastActivity time.Time
	MessageCount int
	LastMessage  Message
	Priority     Priority
}

// ListThreads returns all threads for a project, sorted by last activity (most recent first).
func ListThreads(r *libfossil.Repo, project string) ([]ThreadSummary, error) {
	files, err := allFileNames(r)
	if err != nil {
		return nil, err
	}

	prefix := project + "/threads/"

	// Group files by thread directory.
	threadFiles := make(map[string][]string)
	for _, f := range files {
		if !strings.HasPrefix(f, prefix) {
			continue
		}
		// Extract thread short from path: <project>/threads/<threadShort>/<file>.json
		rest := f[len(prefix):]
		idx := strings.Index(rest, "/")
		if idx < 0 {
			continue
		}
		threadShort := rest[:idx]
		threadFiles[threadShort] = append(threadFiles[threadShort], f)
	}

	var summaries []ThreadSummary
	for threadShort, paths := range threadFiles {
		var messages []Message
		for _, p := range paths {
			data, err := readFileContent(r, p)
			if err != nil {
				return nil, fmt.Errorf("read %s: %w", p, err)
			}
			var msg Message
			if err := json.Unmarshal(data, &msg); err != nil {
				return nil, fmt.Errorf("unmarshal %s: %w", p, err)
			}
			messages = append(messages, msg)
		}

		if len(messages) == 0 {
			continue
		}

		// Sort by timestamp to find the last message.
		sort.Slice(messages, func(i, j int) bool {
			return messages[i].Timestamp.Before(messages[j].Timestamp)
		})

		last := messages[len(messages)-1]
		highest := highestPriority(messages)

		summaries = append(summaries, ThreadSummary{
			ThreadShort:  threadShort,
			Project:      project,
			LastActivity: last.Timestamp,
			MessageCount: len(messages),
			LastMessage:  last,
			Priority:     highest,
		})
	}

	// Sort by last activity, most recent first.
	sort.Slice(summaries, func(i, j int) bool {
		return summaries[i].LastActivity.After(summaries[j].LastActivity)
	})

	return summaries, nil
}

// ReadThread returns all messages in a thread, sorted by timestamp (oldest first).
func ReadThread(r *libfossil.Repo, project, threadShort string) ([]Message, error) {
	files, err := allFileNames(r)
	if err != nil {
		return nil, err
	}

	prefix := project + "/threads/" + threadShort + "/"

	// Filter and sort lexicographically (timestamp prefix ensures chronological order).
	var matching []string
	for _, f := range files {
		if strings.HasPrefix(f, prefix) && strings.HasSuffix(f, ".json") {
			matching = append(matching, f)
		}
	}
	sort.Strings(matching)

	var messages []Message
	for _, f := range matching {
		data, err := readFileContent(r, f)
		if err != nil {
			return nil, fmt.Errorf("read %s: %w", f, err)
		}
		var msg Message
		if err := json.Unmarshal(data, &msg); err != nil {
			return nil, fmt.Errorf("unmarshal %s: %w", f, err)
		}
		messages = append(messages, msg)
	}

	return messages, nil
}

// priorityRank returns a numeric rank for sorting (higher = more urgent).
func priorityRank(p Priority) int {
	switch p {
	case PriorityUrgent:
		return 2
	case PriorityActionRequired:
		return 1
	default:
		return 0
	}
}

// highestPriority returns the highest priority among a set of messages.
func highestPriority(messages []Message) Priority {
	best := PriorityInfo
	for _, m := range messages {
		if priorityRank(m.Priority) > priorityRank(best) {
			best = m.Priority
		}
	}
	return best
}

// allFileNames returns the distinct file names across all checkins in the repo.
func allFileNames(r *libfossil.Repo) ([]string, error) {
	rows, err := r.DB().Query("SELECT DISTINCT name FROM filename ORDER BY name")
	if err != nil {
		return nil, fmt.Errorf("query filenames: %w", err)
	}
	defer rows.Close()

	var names []string
	for rows.Next() {
		var name string
		if err := rows.Scan(&name); err != nil {
			return nil, fmt.Errorf("scan filename: %w", err)
		}
		names = append(names, name)
	}
	return names, rows.Err()
}

// readFileContent reads the latest content of a file by name from the repo database.
// It joins mlink → blob to find the file content for the most recent checkin.
func readFileContent(r *libfossil.Repo, name string) ([]byte, error) {
	var content []byte
	err := r.DB().QueryRow(`
		SELECT b.content
		FROM mlink ml
		JOIN filename fn ON fn.fnid = ml.fnid
		JOIN blob b ON b.rid = ml.fid
		WHERE fn.name = ?
		ORDER BY ml.mid DESC
		LIMIT 1
	`, name).Scan(&content)
	if err != nil {
		return nil, fmt.Errorf("read file %q: %w", name, err)
	}
	// Fossil blob format: [4-byte BE uncompressed size][zlib data].
	decoded, err := decompressBlob(content)
	if err != nil {
		return nil, fmt.Errorf("decompress file %q: %w", name, err)
	}
	return decoded, nil
}

// decompressBlob decodes Fossil's blob format: 4-byte big-endian size prefix + zlib payload.
// If libfossil adds r.ReadFile() in the future, this function and readFileContent should
// migrate to use it and this direct DB access can be removed.
func decompressBlob(data []byte) ([]byte, error) {
	if len(data) < 4 {
		return data, nil // Too small to be compressed.
	}
	uncompSize := binary.BigEndian.Uint32(data[:4])
	zr, err := zlib.NewReader(bytes.NewReader(data[4:]))
	if err != nil {
		return nil, fmt.Errorf("zlib init: %w", err)
	}
	defer zr.Close()

	out := make([]byte, uncompSize)
	if _, err := io.ReadFull(zr, out); err != nil {
		return nil, fmt.Errorf("zlib read: %w", err)
	}
	return out, nil
}
