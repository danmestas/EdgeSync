package sync

import (
	"context"
	"database/sql"
	"errors"
	"fmt"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/xfer"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
)

// DefaultCloneBatchSize is the number of blobs sent per clone round.
const DefaultCloneBatchSize = 200

// HandleFunc is the server-side sync handler signature.
// Transport listeners call this with decoded requests and write back the response.
type HandleFunc func(ctx context.Context, r *repo.Repo, req *xfer.Message) (*xfer.Message, error)

// HandleSync processes an incoming xfer request and produces a response.
// Stateless per-round — the client drives convergence.
func HandleSync(ctx context.Context, r *repo.Repo, req *xfer.Message) (*xfer.Message, error) {
	if r == nil {
		panic("sync.HandleSync: r must not be nil")
	}
	if req == nil {
		panic("sync.HandleSync: req must not be nil")
	}

	h := &handler{repo: r}
	resp, err := h.process(ctx, req)
	if err == nil && resp == nil {
		panic("sync.HandleSync: resp must not be nil on success")
	}
	return resp, err
}

// handler holds per-request state while processing cards.
type handler struct {
	repo      *repo.Repo
	resp      []xfer.Card
	pushOK    bool // client sent a valid push card
	pullOK    bool // client sent a valid pull card
	cloneMode bool // client sent a clone card
	cloneSeq  int  // clone_seqno cursor from client
}

func (h *handler) process(_ context.Context, req *xfer.Message) (*xfer.Message, error) {
	// First pass: extract control cards.
	for _, card := range req.Cards {
		h.handleControlCard(card)
	}

	// Second pass: handle data cards.
	for _, card := range req.Cards {
		if err := h.handleDataCard(card); err != nil {
			return nil, err
		}
	}

	// If pull was requested, emit igot for all our blobs.
	if h.pullOK {
		if err := h.emitIGots(); err != nil {
			return nil, err
		}
	}

	// If clone, emit paginated file cards.
	if h.cloneMode {
		if err := h.emitCloneBatch(); err != nil {
			return nil, err
		}
	}

	return &xfer.Message{Cards: h.resp}, nil
}

func (h *handler) handleControlCard(card xfer.Card) {
	switch c := card.(type) {
	case *xfer.LoginCard:
		_ = c // Accept all logins. Future: verify credentials.
	case *xfer.PragmaCard:
		_ = c // Acknowledge client-version, ignore unknown pragmas.
	case *xfer.PushCard:
		h.pushOK = true
	case *xfer.PullCard:
		h.pullOK = true
	case *xfer.CloneCard:
		h.cloneMode = true
	case *xfer.CloneSeqNoCard:
		h.cloneSeq = c.SeqNo
	}
}

func (h *handler) handleDataCard(card xfer.Card) error {
	switch c := card.(type) {
	case *xfer.IGotCard:
		return h.handleIGot(c)
	case *xfer.GimmeCard:
		return h.handleGimme(c)
	case *xfer.FileCard:
		return h.handleFile(c.UUID, c.DeltaSrc, c.Content)
	case *xfer.CFileCard:
		return h.handleFile(c.UUID, c.DeltaSrc, c.Content)
	case *xfer.ReqConfigCard:
		return h.handleReqConfig(c)
	}
	return nil
}

func (h *handler) handleIGot(c *xfer.IGotCard) error {
	if c == nil {
		panic("handler.handleIGot: c must not be nil")
	}
	if !h.pullOK {
		return nil
	}
	_, exists := blob.Exists(h.repo.DB(), c.UUID)
	if !exists {
		h.resp = append(h.resp, &xfer.GimmeCard{UUID: c.UUID})
	}
	return nil
}

func (h *handler) handleGimme(c *xfer.GimmeCard) error {
	if c == nil {
		panic("handler.handleGimme: c must not be nil")
	}
	rid, ok := blob.Exists(h.repo.DB(), c.UUID)
	if !ok {
		return nil // blob not found — not fatal, skip.
	}
	data, err := content.Expand(h.repo.DB(), rid)
	if err != nil {
		// Expansion failed — report to client rather than silently dropping.
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("expand %s: %v", c.UUID, err),
		})
		return nil
	}
	h.resp = append(h.resp, &xfer.FileCard{UUID: c.UUID, Content: data})
	return nil
}

func (h *handler) handleFile(uuid, deltaSrc string, payload []byte) error {
	if uuid == "" {
		panic("handler.handleFile: uuid must not be empty")
	}
	if !h.pushOK {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("file %s rejected: no push card", uuid),
		})
		return nil
	}
	if err := storeReceivedFile(h.repo, uuid, deltaSrc, payload); err != nil {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("storing %s: %v", uuid, err),
		})
	}
	return nil
}

func (h *handler) handleReqConfig(c *xfer.ReqConfigCard) error {
	if c == nil {
		panic("handler.handleReqConfig: c must not be nil")
	}
	var val string
	err := h.repo.DB().QueryRow(
		"SELECT value FROM config WHERE name = ?", c.Name,
	).Scan(&val)
	if errors.Is(err, sql.ErrNoRows) {
		return nil // config key not found — expected, not fatal.
	}
	if err != nil {
		return fmt.Errorf("handler: config query %q: %w", c.Name, err)
	}
	h.resp = append(h.resp, &xfer.ConfigCard{
		Name:    c.Name,
		Content: []byte(val),
	})
	return nil
}

func (h *handler) emitIGots() error {
	rows, err := h.repo.DB().Query("SELECT uuid FROM blob WHERE size >= 0")
	if err != nil {
		return fmt.Errorf("handler: listing blobs: %w", err)
	}
	defer rows.Close()
	for rows.Next() {
		var uuid string
		if err := rows.Scan(&uuid); err != nil {
			return err
		}
		h.resp = append(h.resp, &xfer.IGotCard{UUID: uuid})
	}
	return rows.Err()
}

func (h *handler) emitCloneBatch() error {
	rows, err := h.repo.DB().Query(
		"SELECT rid, uuid FROM blob WHERE rid > ? AND size >= 0 ORDER BY rid LIMIT ?",
		h.cloneSeq, DefaultCloneBatchSize+1,
	)
	if err != nil {
		return fmt.Errorf("handler: clone batch: %w", err)
	}
	defer rows.Close()

	count := 0
	var lastRID int
	for rows.Next() {
		var rid int
		var uuid string
		if err := rows.Scan(&rid, &uuid); err != nil {
			return err
		}
		if count >= DefaultCloneBatchSize {
			// More blobs remain — emit seqno for continuation.
			// Check rows.Err before early return so DB errors aren't lost.
			if err := rows.Err(); err != nil {
				return err
			}
			h.resp = append(h.resp, &xfer.CloneSeqNoCard{SeqNo: lastRID})
			return nil
		}
		data, err := content.Expand(h.repo.DB(), libfossil.FslID(rid))
		if err != nil {
			return fmt.Errorf("handler: expanding rid %d: %w", rid, err)
		}
		h.resp = append(h.resp, &xfer.FileCard{UUID: uuid, Content: data})
		lastRID = rid
		count++
	}
	return rows.Err()
}
