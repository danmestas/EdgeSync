package sync

import (
	"encoding/json"
	"fmt"

	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

// SyncedTable caches a table definition along with its metadata.
type SyncedTable struct {
	Info repo.TableInfo
	Def  repo.TableDef
}

// loadSyncedTables loads all registered synced tables into the handler cache.
func (h *handler) loadSyncedTables() error {
	if err := repo.EnsureSyncSchema(h.repo.DB()); err != nil {
		return fmt.Errorf("handler.loadSyncedTables: ensure schema: %w", err)
	}

	tables, err := repo.ListSyncedTables(h.repo.DB())
	if err != nil {
		return fmt.Errorf("handler.loadSyncedTables: list tables: %w", err)
	}

	h.syncedTables = make(map[string]*SyncedTable)
	for _, info := range tables {
		h.syncedTables[info.Name] = &SyncedTable{
			Info: info,
			Def:  info.Def,
		}
	}
	return nil
}

// handleSchemaCard processes a schema declaration from the client.
func (h *handler) handleSchemaCard(c *xfer.SchemaCard) {
	if c == nil {
		panic("handler.handleSchemaCard: c must not be nil")
	}

	// Validate table name.
	if err := repo.ValidateTableName(c.Table); err != nil {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("schema %s: %v", c.Table, err),
		})
		return
	}

	// Unmarshal and validate definition.
	var def repo.TableDef
	if err := json.Unmarshal(c.Content, &def); err != nil {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("schema %s: unmarshal: %v", c.Table, err),
		})
		return
	}

	// Register table.
	if err := repo.RegisterSyncedTable(h.repo.DB(), c.Table, def, c.MTime); err != nil {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("schema %s: register: %v", c.Table, err),
		})
		return
	}

	// Cache for this session.
	h.syncedTables[c.Table] = &SyncedTable{
		Info: repo.TableInfo{
			Name:  c.Table,
			Def:   def,
			MTime: c.MTime,
		},
		Def: def,
	}
}

// handlePragmaXTableHash compares catalog hashes and emits xigots if they differ.
func (h *handler) handlePragmaXTableHash(table, clientHash string) {
	st, ok := h.syncedTables[table]
	if !ok {
		// Table not registered yet — client will send schema card.
		return
	}

	localHash, err := repo.CatalogHash(h.repo.DB(), table, st.Def)
	if err != nil {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("xtable-hash %s: %v", table, err),
		})
		return
	}

	if localHash == clientHash {
		return // already in sync
	}

	// Emit xigot for all rows.
	if err := h.emitXIGotsForTable(table, st); err != nil {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("xtable-hash %s: emit xigots: %v", table, err),
		})
	}
}

// handleXIGot processes a table sync igot card.
func (h *handler) handleXIGot(c *xfer.XIGotCard) error {
	if c == nil {
		panic("handler.handleXIGot: c must not be nil")
	}
	if !h.pullOK {
		return nil
	}

	st, ok := h.syncedTables[c.Table]
	if !ok {
		// Table not registered — skip silently (client may have newer schema).
		return nil
	}

	// Lookup local row.
	localRow, localMtime, err := repo.LookupXRow(h.repo.DB(), c.Table, st.Def, c.PKHash)
	if err != nil {
		return fmt.Errorf("handler.handleXIGot: lookup %s/%s: %w", c.Table, c.PKHash, err)
	}

	if localRow == nil {
		// Missing locally — request it.
		h.resp = append(h.resp, &xfer.XGimmeCard{
			Table:  c.Table,
			PKHash: c.PKHash,
		})
		return nil
	}

	// Compare mtimes: if local is newer, push it; otherwise no-op.
	if localMtime > c.MTime {
		return h.sendXRow(c.Table, st, c.PKHash)
	}

	return nil
}

// handleXGimme processes a table sync gimme card.
func (h *handler) handleXGimme(c *xfer.XGimmeCard) error {
	if c == nil {
		panic("handler.handleXGimme: c must not be nil")
	}

	// BUGGIFY: 5% chance skip sending a row to test client retry.
	if h.buggify != nil && h.buggify.Check("handler.handleXGimme.skip", 0.05) {
		return nil
	}

	st, ok := h.syncedTables[c.Table]
	if !ok {
		// Table not registered — skip silently.
		return nil
	}

	return h.sendXRow(c.Table, st, c.PKHash)
}

// handleXRow processes a table sync row card.
func (h *handler) handleXRow(c *xfer.XRowCard) error {
	if c == nil {
		panic("handler.handleXRow: c must not be nil")
	}
	if !h.pushOK {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("xrow %s/%s rejected: no push card", c.Table, c.PKHash),
		})
		return nil
	}

	st, ok := h.syncedTables[c.Table]
	if !ok {
		// Table not registered — skip silently (client may have newer schema).
		return nil
	}

	// BUGGIFY: 3% chance reject a valid row to test client re-push.
	if h.buggify != nil && h.buggify.Check("handler.handleXRow.reject", 0.03) {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("buggify: rejected xrow %s/%s", c.Table, c.PKHash),
		})
		return nil
	}

	// Unmarshal row data.
	var row map[string]any
	if err := json.Unmarshal(c.Content, &row); err != nil {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("xrow %s/%s: unmarshal: %v", c.Table, c.PKHash, err),
		})
		return nil
	}

	// Verify PK hash.
	pkCols := h.extractPKColumns(st.Def)
	pkValues := make(map[string]any)
	for _, col := range pkCols {
		pkValues[col] = row[col]
	}
	computedPK := repo.PKHash(pkValues)
	if computedPK != c.PKHash {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("xrow %s/%s: pk hash mismatch", c.Table, c.PKHash),
		})
		return nil
	}

	// Conflict resolution.
	localRow, localMtime, err := repo.LookupXRow(h.repo.DB(), c.Table, st.Def, c.PKHash)
	if err != nil {
		return fmt.Errorf("handler.handleXRow: lookup %s/%s: %w", c.Table, c.PKHash, err)
	}

	switch st.Def.Conflict {
	case "mtime-wins":
		if localRow != nil && localMtime > c.MTime {
			// Local is newer — reject incoming.
			return nil
		}
	case "self-write":
		if localRow != nil {
			localOwner, _ := localRow["_owner"].(string)
			if localOwner != "" && localOwner != h.loginUser {
				// Not owner — reject.
				return nil
			}
		}
		// Add _owner if not present (first write or self-write allowed).
		if h.loginUser != "" {
			row["_owner"] = h.loginUser
		}
	case "owner-write":
		if localRow != nil {
			localOwner, _ := localRow["_owner"].(string)
			if localOwner != h.loginUser {
				// Not owner — reject.
				return nil
			}
		}
		// Add _owner.
		if h.loginUser != "" {
			row["_owner"] = h.loginUser
		}
	}

	// Upsert row.
	if err := repo.UpsertXRow(h.repo.DB(), c.Table, row, c.MTime); err != nil {
		return fmt.Errorf("handler.handleXRow: upsert %s/%s: %w", c.Table, c.PKHash, err)
	}

	h.xrowsRecvd++
	return nil
}

// sendXRow sends a single row to the client.
func (h *handler) sendXRow(table string, st *SyncedTable, pkHash string) error {
	row, mtime, err := repo.LookupXRow(h.repo.DB(), table, st.Def, pkHash)
	if err != nil {
		return fmt.Errorf("handler.sendXRow: lookup %s/%s: %w", table, pkHash, err)
	}
	if row == nil {
		return nil // Row not found — skip silently.
	}

	// Marshal row data.
	rowJSON, err := json.Marshal(row)
	if err != nil {
		return fmt.Errorf("handler.sendXRow: marshal %s/%s: %w", table, pkHash, err)
	}

	h.resp = append(h.resp, &xfer.XRowCard{
		Table:   table,
		PKHash:  pkHash,
		MTime:   mtime,
		Content: rowJSON,
	})
	h.xrowsSent++
	return nil
}

// emitXIGots emits xigot cards for all synced tables.
func (h *handler) emitXIGots() error {
	for name, st := range h.syncedTables {
		if err := h.emitXIGotsForTable(name, st); err != nil {
			return err
		}
	}
	return nil
}

// emitXIGotsForTable emits xigot cards for all rows in a table.
func (h *handler) emitXIGotsForTable(table string, st *SyncedTable) error {
	rows, mtimes, err := repo.ListXRows(h.repo.DB(), table, st.Def)
	if err != nil {
		return fmt.Errorf("handler.emitXIGotsForTable: list %s: %w", table, err)
	}

	pkCols := h.extractPKColumns(st.Def)

	for i, row := range rows {
		pkValues := make(map[string]any)
		for _, col := range pkCols {
			pkValues[col] = row[col]
		}
		pkHash := repo.PKHash(pkValues)

		h.resp = append(h.resp, &xfer.XIGotCard{
			Table:  table,
			PKHash: pkHash,
			MTime:  mtimes[i],
		})
	}
	return nil
}

// extractPKColumns returns the names of all primary key columns.
func (h *handler) extractPKColumns(def repo.TableDef) []string {
	var pkCols []string
	for _, col := range def.Columns {
		if col.PK {
			pkCols = append(pkCols, col.Name)
		}
	}
	return pkCols
}
