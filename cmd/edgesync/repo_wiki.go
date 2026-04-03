package main

import (
	"fmt"
	"os"

	libfossil "github.com/danmestas/go-libfossil"
	"github.com/danmestas/go-libfossil/content"
	"github.com/danmestas/go-libfossil/deck"
)

type RepoWikiCmd struct {
	Ls     RepoWikiLsCmd     `cmd:"" help:"List wiki pages"`
	Export RepoWikiExportCmd `cmd:"" help:"Export a wiki page"`
}

type RepoWikiLsCmd struct{}

func (c *RepoWikiLsCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	// Wiki pages are stored as artifacts with an L-card (page title).
	// Find them by scanning events of type 'w' (wiki), or by looking for
	// blobs that parse as wiki manifests.
	rows, err := r.DB().Query(`
		SELECT DISTINCT b.uuid, b.rid FROM event e
		JOIN blob b ON b.rid=e.objid
		WHERE e.type='w'
		ORDER BY e.mtime DESC`)
	if err != nil {
		return err
	}
	defer rows.Close()

	for rows.Next() {
		var uuid string
		var rid int64
		rows.Scan(&uuid, &rid)

		data, err := content.Expand(r.DB(), libfossil.FslID(rid))
		if err != nil {
			continue
		}
		d, err := deck.Parse(data)
		if err != nil {
			continue
		}
		if d.L != "" {
			fmt.Printf("%-40s %s\n", d.L, uuid[:10])
		}
	}
	return rows.Err()
}

type RepoWikiExportCmd struct {
	Page   string `arg:"" help:"Wiki page name or artifact UUID"`
	Output string `short:"o" help:"Output file (default: stdout)"`
}

func (c *RepoWikiExportCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	// Try as UUID first, then search by page name.
	var data []byte

	rid, err := resolveRID(r, c.Page)
	if err == nil {
		// Resolved as UUID — expand and check if it's a wiki artifact.
		data, err = content.Expand(r.DB(), rid)
		if err != nil {
			return err
		}
	} else {
		// Search by page title in wiki events.
		rows, err := r.DB().Query(`
			SELECT b.rid FROM event e
			JOIN blob b ON b.rid=e.objid
			WHERE e.type='w'
			ORDER BY e.mtime DESC`)
		if err != nil {
			return err
		}
		defer rows.Close()

		found := false
		for rows.Next() {
			var rid int64
			rows.Scan(&rid)
			d, err := content.Expand(r.DB(), libfossil.FslID(rid))
			if err != nil {
				continue
			}
			parsed, err := deck.Parse(d)
			if err != nil {
				continue
			}
			if parsed.L == c.Page {
				data = parsed.W
				found = true
				break
			}
		}
		if !found {
			return fmt.Errorf("wiki page %q not found", c.Page)
		}
	}

	// If we got raw artifact data (UUID path), parse it to extract W card.
	if data != nil && rid > 0 {
		parsed, err := deck.Parse(data)
		if err == nil && len(parsed.W) > 0 {
			data = parsed.W
		}
		// If not parseable as wiki, output raw content.
	}

	if c.Output != "" {
		return os.WriteFile(c.Output, data, 0o644)
	}
	os.Stdout.Write(data)
	return nil
}
