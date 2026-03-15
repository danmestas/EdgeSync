package sync

import (
	"bytes"
	"fmt"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/delta"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

// buildRequest assembles one outbound xfer message for the given cycle.
func (s *session) buildRequest(cycle int) (*xfer.Message, error) {
	// BUGGIFY: shrink send budget to stress multi-round convergence.
	if s.opts.Buggify != nil && s.opts.Buggify.Check("sync.buildRequest.minBudget", 0.10) {
		s.maxSend = 1024
	}

	var cards []xfer.Card

	// 1. Pragma: client-version (every round)
	cards = append(cards, &xfer.PragmaCard{
		Name:   "client-version",
		Values: []string{"go-libfossil/0.1"},
	})

	// 2. Push/Pull cards
	if s.opts.Push {
		cards = append(cards, &xfer.PushCard{
			ServerCode:  s.opts.ServerCode,
			ProjectCode: s.opts.ProjectCode,
		})
	}
	if s.opts.Pull {
		cards = append(cards, &xfer.PullCard{
			ServerCode:  s.opts.ServerCode,
			ProjectCode: s.opts.ProjectCode,
		})
	}

	// 3. Cookie if cached
	if s.cookie != "" {
		cards = append(cards, &xfer.CookieCard{Value: s.cookie})
	}

	// 4. IGot cards from unclustered table, filtered by remoteHas
	igotCards, err := s.buildIGotCards()
	if err != nil {
		return nil, fmt.Errorf("buildRequest igot: %w", err)
	}
	s.igotSentThisRound = len(igotCards)
	cards = append(cards, igotCards...)

	// 5. File cards from pendingSend + unsent table, respecting maxSend budget
	fileCards, err := s.buildFileCards()
	if err != nil {
		return nil, fmt.Errorf("buildRequest file: %w", err)
	}
	cards = append(cards, fileCards...)

	// 6. Gimme cards from phantoms (max = max(MaxGimmeBase, filesRecvdLastRound*2))
	gimmeCards := s.buildGimmeCards()
	cards = append(cards, gimmeCards...)

	// 7. Login card computed LAST, prepended to the front.
	// Nonce = SHA1 of all other cards encoded + random comment.
	if s.opts.User != "" {
		loginCard, err := s.buildLoginCard(cards)
		if err != nil {
			return nil, fmt.Errorf("buildRequest login: %w", err)
		}
		cards = append([]xfer.Card{loginCard}, cards...)
	}

	return &xfer.Message{Cards: cards}, nil
}

// buildIGotCards queries the unclustered table and produces igot cards
// for artifacts the remote doesn't already have.
func (s *session) buildIGotCards() ([]xfer.Card, error) {
	rows, err := s.repo.DB().Query(
		"SELECT b.uuid FROM unclustered u JOIN blob b ON b.rid=u.rid WHERE b.size >= 0",
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var cards []xfer.Card
	for rows.Next() {
		var uuid string
		if err := rows.Scan(&uuid); err != nil {
			return nil, err
		}
		if s.remoteHas[uuid] {
			continue
		}
		cards = append(cards, &xfer.IGotCard{UUID: uuid})
	}
	return cards, rows.Err()
}

// buildFileCards produces file cards from pendingSend and the unsent table,
// respecting the maxSend byte budget.
func (s *session) buildFileCards() ([]xfer.Card, error) {
	if !s.opts.Push {
		return nil, nil
	}

	budget := s.maxSend
	var cards []xfer.Card

	// First: pendingSend
	for uuid := range s.pendingSend {
		if budget <= 0 {
			break
		}
		card, size, err := s.loadFileCard(uuid)
		if err != nil {
			// Skip files we can't load (phantoms, etc.)
			continue
		}
		cards = append(cards, card)
		budget -= size
		delete(s.pendingSend, uuid)
		s.result.FilesSent++
	}

	// Note: files from the unsent table are announced via igot cards (in buildIGotCards).
	// The server will gimme the ones it needs, which populates pendingSend for the next round.
	// We do NOT proactively send unsent files — Fossil's protocol expects igot first, gimme second.

	// BUGGIFY: drop the last file card to simulate partial send.
	if s.opts.Buggify != nil && s.opts.Buggify.Check("sync.buildFileCards.skip", 0.05) && len(cards) > 0 {
		cards = cards[:len(cards)-1]
	}

	return cards, nil
}

// loadFileCard loads a blob by UUID and returns a FileCard plus its payload size.
func (s *session) loadFileCard(uuid string) (*xfer.FileCard, int, error) {
	rid, ok := blob.Exists(s.repo.DB(), uuid)
	if !ok {
		return nil, 0, fmt.Errorf("blob %s not found", uuid)
	}
	data, err := content.Expand(s.repo.DB(), rid)
	if err != nil {
		return nil, 0, err
	}
	return &xfer.FileCard{UUID: uuid, Content: data}, len(data), nil
}

// buildGimmeCards produces gimme cards from the phantoms set.
func (s *session) buildGimmeCards() []xfer.Card {
	if !s.opts.Pull {
		return nil
	}
	maxGimme := MaxGimmeBase
	if alt := s.filesRecvdLastRound * 2; alt > maxGimme {
		maxGimme = alt
	}

	var cards []xfer.Card
	count := 0
	for uuid := range s.phantoms {
		if count >= maxGimme {
			break
		}
		cards = append(cards, &xfer.GimmeCard{UUID: uuid})
		count++
	}
	return cards
}

// buildLoginCard encodes the non-login cards, appends a random comment,
// then computes the login card and returns it.
func (s *session) buildLoginCard(cards []xfer.Card) (*xfer.LoginCard, error) {
	var buf bytes.Buffer
	for _, c := range cards {
		if err := xfer.EncodeCard(&buf, c); err != nil {
			return nil, err
		}
	}
	payload := appendRandomComment(buf.Bytes(), s.env.Rand)
	// BUGGIFY: corrupt the nonce payload to trigger auth failures.
	if s.opts.Buggify != nil && s.opts.Buggify.Check("sync.buildLoginCard.badNonce", 0.02) {
		payload = append(payload, []byte("BUGGIFY")...)
	}
	return computeLogin(s.opts.User, s.opts.Password, s.opts.ProjectCode, payload), nil
}

// processResponse handles all cards in a server response.
// It returns true when the sync has converged (nothing more to do).
func (s *session) processResponse(msg *xfer.Message) (bool, error) {
	filesRecvd := 0
	filesSent := 0 // files the server asked us to send this round

	for _, card := range msg.Cards {
		switch c := card.(type) {
		case *xfer.FileCard:
			if err := s.handleFileCard(c.UUID, c.DeltaSrc, c.Content); err != nil {
				return false, err
			}
			filesRecvd++
			delete(s.phantoms, c.UUID)

		case *xfer.CFileCard:
			if err := s.handleFileCard(c.UUID, c.DeltaSrc, c.Content); err != nil {
				return false, err
			}
			filesRecvd++
			delete(s.phantoms, c.UUID)

		case *xfer.IGotCard:
			s.remoteHas[c.UUID] = true
			if s.opts.Pull {
				_, exists := blob.Exists(s.repo.DB(), c.UUID)
				if !exists {
					s.phantoms[c.UUID] = true
				}
			}

		case *xfer.GimmeCard:
			s.pendingSend[c.UUID] = true
			filesSent++

		case *xfer.CookieCard:
			s.cookie = c.Value

		case *xfer.ErrorCard:
			s.result.Errors = append(s.result.Errors, "error: "+c.Message)

		case *xfer.MessageCard:
			s.result.Errors = append(s.result.Errors, "message: "+c.Message)
		}
	}

	s.result.FilesRecvd += filesRecvd
	s.filesRecvdLastRound = filesRecvd

	// Convergence: done if no files received, no files sent this round,
	// phantoms empty, pendingSend empty, and unsent table empty.
	if filesRecvd > 0 || filesSent > 0 {
		return false, nil
	}
	if len(s.phantoms) > 0 || len(s.pendingSend) > 0 {
		return false, nil
	}

	// If we sent igot cards but the server didn't gimme anything,
	// clear the unsent table — those artifacts have been announced
	// and the server either has them or doesn't want them.
	if s.igotSentThisRound > 0 && filesSent == 0 {
		s.repo.DB().Exec("DELETE FROM unsent")
	}

	// Check unsent table
	var unsentCount int
	err := s.repo.DB().QueryRow("SELECT count(*) FROM unsent").Scan(&unsentCount)
	if err != nil {
		return false, fmt.Errorf("checking unsent: %w", err)
	}
	if unsentCount > 0 {
		return false, nil
	}

	return true, nil
}

// handleFileCard stores a received file (or delta-file) into the repo.
func (s *session) handleFileCard(uuid, deltaSrc string, payload []byte) error {
	var fullContent []byte

	if deltaSrc != "" {
		// Delta: load base via content.Expand, apply delta
		srcRid, ok := blob.Exists(s.repo.DB(), deltaSrc)
		if !ok {
			return fmt.Errorf("delta source %s not found", deltaSrc)
		}
		baseContent, err := content.Expand(s.repo.DB(), srcRid)
		if err != nil {
			return fmt.Errorf("expanding delta source %s: %w", deltaSrc, err)
		}
		applied, err := delta.Apply(baseContent, payload)
		if err != nil {
			return fmt.Errorf("applying delta for %s: %w", uuid, err)
		}
		fullContent = applied
	} else {
		fullContent = payload
	}

	var storedUUID string
	err := s.repo.WithTx(func(tx *db.Tx) error {
		rid, u, err := blob.Store(tx, fullContent)
		if err != nil {
			return err
		}
		storedUUID = u
		// Add to unclustered so we propagate to other remotes
		_, err = tx.Exec("INSERT OR IGNORE INTO unclustered(rid) VALUES(?)", rid)
		return err
	})
	if err != nil {
		return fmt.Errorf("storing file %s: %w", uuid, err)
	}

	if storedUUID != uuid {
		return fmt.Errorf("UUID mismatch for received file: expected %s, got %s", uuid, storedUUID)
	}

	// BUGGIFY: simulate post-store failure to test retry/recovery logic.
	if s.opts.Buggify != nil && s.opts.Buggify.Check("sync.handleFileCard.reject", 0.03) {
		return fmt.Errorf("buggify: simulated storage failure for %s", uuid)
	}

	return nil
}
