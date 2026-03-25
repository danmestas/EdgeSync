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

// HandleOpts configures optional behavior for HandleSync.
type HandleOpts struct {
	Buggify  BuggifyChecker // nil in production.
	Observer Observer       // nil defaults to no-op.
}

// HandleSync processes an incoming xfer request and produces a response.
// Stateless per-round — the client drives convergence.
func HandleSync(ctx context.Context, r *repo.Repo, req *xfer.Message) (*xfer.Message, error) {
	return HandleSyncWithOpts(ctx, r, req, HandleOpts{})
}

// HandleSyncWithOpts processes an incoming xfer request with optional
// fault injection. Used by DST harness; production callers use HandleSync.
func HandleSyncWithOpts(ctx context.Context, r *repo.Repo, req *xfer.Message, opts HandleOpts) (*xfer.Message, error) {
	if r == nil {
		panic("sync.HandleSync: r must not be nil")
	}
	if req == nil {
		panic("sync.HandleSync: req must not be nil")
	}

	obs := resolveObserver(opts.Observer)
	ctx = obs.HandleStarted(ctx, HandleStart{
		Operation: detectOperation(req),
	})

	h := &handler{repo: r, buggify: opts.Buggify}
	resp, err := h.process(ctx, req)
	if err == nil && resp == nil {
		panic("sync.HandleSync: resp must not be nil on success")
	}

	obs.HandleCompleted(ctx, HandleEnd{
		CardsProcessed: len(req.Cards),
		FilesSent:      h.filesSent,
		FilesReceived:  h.filesRecvd,
		Err:            err,
	})
	return resp, err
}

// detectOperation checks request cards to determine if this is a clone or sync.
func detectOperation(req *xfer.Message) string {
	for _, c := range req.Cards {
		if _, ok := c.(*xfer.CloneCard); ok {
			return "clone"
		}
	}
	return "sync"
}

// handler holds per-request state while processing cards.
type handler struct {
	repo          *repo.Repo
	buggify       BuggifyChecker
	resp          []xfer.Card
	pushOK        bool // client sent a valid push card
	pullOK        bool // client sent a valid pull card
	cloneMode     bool // client sent a clone card
	cloneSeq      int  // clone_seqno cursor from client
	uvCatalogSent bool // true after sending UV catalog
	reqClusters   bool // client sent pragma req-clusters
	filesSent     int  // files sent in response (for observer)
	filesRecvd    int  // files received from client (for observer)
}

func (h *handler) process(_ context.Context, req *xfer.Message) (*xfer.Message, error) {
	// First pass: extract control cards.
	for _, card := range req.Cards {
		h.handleControlCard(card)
	}

	// Emit PushCard with project-code/server-code so the clone client can
	// identify the repo. Only in clone mode — sync clients already have
	// codes, and real Fossil treats server-sent "push" as unknown during sync.
	if h.cloneMode {
		var projectCode, serverCode string
		_ = h.repo.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projectCode)
		_ = h.repo.DB().QueryRow("SELECT value FROM config WHERE name='server-code'").Scan(&serverCode)
		if projectCode != "" {
			h.resp = append(h.resp, &xfer.PushCard{
				ProjectCode: projectCode,
				ServerCode:  serverCode,
			})
		}
	}

	// Second pass: handle file cards first so blobs are stored before
	// IGotCard checks blob.Exists. Without this, a request containing
	// both IGotCard and FileCard for the same blob produces a spurious
	// GimmeCard — the IGotCard runs before the FileCard stores it.
	for _, card := range req.Cards {
		switch card.(type) {
		case *xfer.FileCard, *xfer.CFileCard:
			if err := h.handleDataCard(card); err != nil {
				return nil, err
			}
		}
	}
	// Third pass: handle remaining data cards (igot, gimme, etc.).
	for _, card := range req.Cards {
		switch card.(type) {
		case *xfer.FileCard, *xfer.CFileCard:
			continue // Already handled above.
		default:
			if err := h.handleDataCard(card); err != nil {
				return nil, err
			}
		}
	}

	// If pull was requested, emit igot for all non-phantom blobs.
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
		if c.Name == "uv-hash" && len(c.Values) >= 1 {
			if err := h.handlePragmaUVHash(c.Values[0]); err != nil {
				h.resp = append(h.resp, &xfer.ErrorCard{
					Message: fmt.Sprintf("uv-hash: %v", err),
				})
			}
		}
		if c.Name == "req-clusters" {
			h.reqClusters = true
		}
		// Acknowledge client-version, ignore other unknown pragmas.
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
	case *xfer.UVIGotCard:
		return h.handleUVIGot(c)
	case *xfer.UVGimmeCard:
		return h.handleUVGimme(c)
	case *xfer.UVFileCard:
		return h.handleUVFile(c)
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
	// BUGGIFY: 5% chance skip sending a file to test client retry.
	if h.buggify != nil && h.buggify.Check("handler.handleGimme.skip", 0.05) {
		return nil
	}
	rid, ok := blob.Exists(h.repo.DB(), c.UUID)
	if !ok {
		return nil // blob not found — not fatal, skip.
	}
	data, err := content.Expand(h.repo.DB(), rid)
	if err != nil {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("expand %s: %v", c.UUID, err),
		})
		return nil
	}
	h.resp = append(h.resp, &xfer.FileCard{UUID: c.UUID, Content: data})
	h.filesSent++
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
	// BUGGIFY: 3% chance reject a valid file to test client re-push.
	if h.buggify != nil && h.buggify.Check("handler.handleFile.reject", 0.03) {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("buggify: rejected file %s", uuid),
		})
		return nil
	}
	if err := storeReceivedFile(h.repo, uuid, deltaSrc, payload); err != nil {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("storing %s: %v", uuid, err),
		})
		return nil
	}
	h.filesRecvd++
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
	// Emit igot for all non-phantom blobs so the client can discover
	// everything the server has. Cluster generation is a client-side
	// optimization for push; the server always advertises all blobs.
	rows, err := h.repo.DB().Query(
		"SELECT uuid FROM blob WHERE size >= 0",
	)
	if err != nil {
		return fmt.Errorf("handler: listing blobs: %w", err)
	}
	defer rows.Close()

	var uuids []string
	for rows.Next() {
		var uuid string
		if err := rows.Scan(&uuid); err != nil {
			return err
		}
		uuids = append(uuids, uuid)
	}
	if err := rows.Err(); err != nil {
		return err
	}

	// BUGGIFY: 10% chance truncate igot list to test multi-round convergence.
	if h.buggify != nil && h.buggify.Check("handler.emitIGots.truncate", 0.10) && len(uuids) > 1 {
		uuids = uuids[:len(uuids)/2]
	}

	for _, uuid := range uuids {
		h.resp = append(h.resp, &xfer.IGotCard{UUID: uuid})
	}
	return nil
}

// sendAllClusters emits igot cards for all cluster artifacts that are
// not still in unclustered (i.e., already fully clustered themselves).
func (h *handler) sendAllClusters() error {
	rows, err := h.repo.DB().Query(`
		SELECT b.uuid FROM tagxref tx
		JOIN blob b ON tx.rid = b.rid
		WHERE tx.tagid = 7
		  AND NOT EXISTS (SELECT 1 FROM unclustered WHERE rid = b.rid)
		  AND NOT EXISTS (SELECT 1 FROM phantom WHERE rid = b.rid)
		  AND b.size >= 0
	`)
	if err != nil {
		return fmt.Errorf("handler: listing clusters: %w", err)
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
	batchSize := DefaultCloneBatchSize
	// BUGGIFY: 10% chance reduce batch size to 1 to stress pagination.
	if h.buggify != nil && h.buggify.Check("handler.emitCloneBatch.smallBatch", 0.10) {
		batchSize = 1
	}

	rows, err := h.repo.DB().Query(
		"SELECT rid, uuid FROM blob WHERE rid > ? AND size >= 0 ORDER BY rid LIMIT ?",
		h.cloneSeq, batchSize+1,
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
		if count >= batchSize {
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
		h.filesSent++
		lastRID = rid
		count++
	}
	if err := rows.Err(); err != nil {
		return err
	}
	// All blobs sent — signal completion so the client stops requesting.
	h.resp = append(h.resp, &xfer.CloneSeqNoCard{SeqNo: 0})
	return nil
}
