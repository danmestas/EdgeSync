package sync

import (
	"context"
	"fmt"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

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
	return h.process(ctx, req)
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
		// Accept all logins. Future: verify credentials.
		_ = c
	case *xfer.PragmaCard:
		// Acknowledge client-version, ignore unknown pragmas.
		_ = c
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
	card, _, err := LoadBlob(h.repo.DB(), c.UUID)
	if err != nil {
		return nil // blob not found — not fatal, skip
	}
	h.resp = append(h.resp, card)
	return nil
}

func (h *handler) handleFile(uuid, deltaSrc string, content []byte) error {
	if !h.pushOK {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("file %s rejected: no push card", uuid),
		})
		return nil
	}
	if err := StoreBlob(h.repo.DB(), uuid, deltaSrc, content); err != nil {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("storing %s: %v", uuid, err),
		})
	}
	return nil
}

func (h *handler) handleReqConfig(c *xfer.ReqConfigCard) error {
	var val string
	err := h.repo.DB().QueryRow(
		"SELECT value FROM config WHERE name = ?", c.Name,
	).Scan(&val)
	if err != nil {
		return nil // config not found — not fatal
	}
	h.resp = append(h.resp, &xfer.ConfigCard{
		Name:    c.Name,
		Content: []byte(val),
	})
	return nil
}

func (h *handler) emitIGots() error {
	uuids, err := ListBlobUUIDs(h.repo.DB())
	if err != nil {
		return fmt.Errorf("handler: listing blobs: %w", err)
	}
	for _, uuid := range uuids {
		h.resp = append(h.resp, &xfer.IGotCard{UUID: uuid})
	}
	return nil
}

func (h *handler) emitCloneBatch() error {
	cards, lastRID, more, err := ListBlobsFromRID(
		h.repo.DB(), h.cloneSeq, DefaultCloneBatchSize,
	)
	if err != nil {
		return fmt.Errorf("handler: clone batch: %w", err)
	}
	for i := range cards {
		h.resp = append(h.resp, &cards[i])
	}
	if more {
		h.resp = append(h.resp, &xfer.CloneSeqNoCard{SeqNo: lastRID})
	}
	return nil
}
