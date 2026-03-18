package sync

import (
	"fmt"

	"github.com/dmestas/edgesync/go-libfossil/hash"
	"github.com/dmestas/edgesync/go-libfossil/uv"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

func (h *handler) handlePragmaUVHash(clientHash string) {
	if h.uvCatalogSent {
		return
	}
	uv.EnsureSchema(h.repo.DB())

	localHash, err := uv.ContentHash(h.repo.DB())
	if err != nil {
		return // non-fatal
	}
	if localHash == clientHash {
		return // already in sync
	}

	h.uvCatalogSent = true
	h.resp = append(h.resp, &xfer.PragmaCard{Name: "uv-push-ok"})

	entries, err := uv.List(h.repo.DB())
	if err != nil {
		return
	}
	for _, e := range entries {
		hashVal := e.Hash
		if hashVal == "" {
			hashVal = "-"
		}
		h.resp = append(h.resp, &xfer.UVIGotCard{
			Name:  e.Name,
			MTime: e.MTime,
			Hash:  hashVal,
			Size:  e.Size,
		})
	}
}

func (h *handler) handleUVIGot(c *xfer.UVIGotCard) error {
	if c == nil {
		panic("handler.handleUVIGot: c must not be nil")
	}
	uv.EnsureSchema(h.repo.DB())

	// Look up local file.
	_, localMtime, localHash, err := uv.Read(h.repo.DB(), c.Name)
	if err != nil {
		return fmt.Errorf("handler.handleUVIGot: read %q: %w", c.Name, err)
	}

	status := uv.Status(localMtime, localHash, c.MTime, c.Hash)

	switch {
	case status == 0 || status == 1:
		h.resp = append(h.resp, &xfer.UVGimmeCard{Name: c.Name})
	case status == 2:
		h.repo.DB().Exec("UPDATE unversioned SET mtime=? WHERE name=?", c.MTime, c.Name)
		uv.InvalidateHash(h.repo.DB())
	case status == 4 || status == 5:
		h.sendUVFile(c.Name)
	}
	return nil
}

func (h *handler) handleUVGimme(c *xfer.UVGimmeCard) error {
	if c == nil {
		panic("handler.handleUVGimme: c must not be nil")
	}
	if h.buggify != nil && h.buggify.Check("handler.handleUVGimme.skip", 0.05) {
		return nil
	}
	h.sendUVFile(c.Name)
	return nil
}

func (h *handler) sendUVFile(name string) {
	content, mtime, fileHash, err := uv.Read(h.repo.DB(), name)
	if err != nil {
		return
	}

	if fileHash == "" {
		h.resp = append(h.resp, &xfer.UVFileCard{
			Name:  name,
			MTime: mtime,
			Hash:  "-",
			Size:  0,
			Flags: 1,
		})
		return
	}

	h.resp = append(h.resp, &xfer.UVFileCard{
		Name:    name,
		MTime:   mtime,
		Hash:    fileHash,
		Size:    len(content),
		Flags:   0,
		Content: content,
	})
}

func (h *handler) handleUVFile(c *xfer.UVFileCard) error {
	if c == nil {
		panic("handler.handleUVFile: c must not be nil")
	}
	if !h.pushOK {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("uvfile %s rejected: no push card", c.Name),
		})
		return nil
	}

	if h.buggify != nil && h.buggify.Check("handler.handleUVFile.drop", 0.03) {
		return nil
	}

	uv.EnsureSchema(h.repo.DB())

	// Validate hash if content present.
	if c.Flags&0x0005 == 0 && c.Content != nil {
		computed := hash.SHA1(c.Content)
		if len(c.Hash) > 40 {
			computed = hash.SHA3(c.Content)
		}
		if computed != c.Hash {
			h.resp = append(h.resp, &xfer.ErrorCard{
				Message: fmt.Sprintf("uvfile %s: hash mismatch", c.Name),
			})
			return nil
		}
	}

	// Double-check status.
	_, localMtime, localHash, err := uv.Read(h.repo.DB(), c.Name)
	if err != nil {
		return fmt.Errorf("handler.handleUVFile: read %q: %w", c.Name, err)
	}

	status := uv.Status(localMtime, localHash, c.MTime, c.Hash)
	if status >= 2 {
		return nil
	}

	// Apply.
	if c.Hash == "-" {
		return uv.Delete(h.repo.DB(), c.Name, c.MTime)
	}
	if c.Content != nil {
		return uv.Write(h.repo.DB(), c.Name, c.Content, c.MTime)
	}
	return nil
}
