package main

import (
	"database/sql"
	"errors"
	"fmt"

	libfossil "github.com/danmestas/go-libfossil"
	"github.com/danmestas/go-libfossil/bisect"
)

type RepoBisectCmd struct {
	Good   RepoBisectGoodCmd   `cmd:"" help:"Mark version as good"`
	Bad    RepoBisectBadCmd    `cmd:"" help:"Mark version as bad"`
	Next   RepoBisectNextCmd   `cmd:"" help:"Check out midpoint version"`
	Skip   RepoBisectSkipCmd   `cmd:"" help:"Skip current version"`
	Reset  RepoBisectResetCmd  `cmd:"" help:"Clear bisect state"`
	Ls     RepoBisectLsCmd     `cmd:"" help:"Show bisect path"`
	Status RepoBisectStatusCmd `cmd:"" help:"Show bisect state"`
}

// --- good ---

type RepoBisectGoodCmd struct {
	Version string `arg:"" optional:"" help:"Version to mark (default: current checkout)"`
	Dir     string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoBisectGoodCmd) Run(g *Globals) error {
	ckout, repoPath, err := bisectOpenCheckout(g, c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()

	rid, err := bisectResolveVersion(g, ckout, repoPath, c.Version)
	if err != nil {
		return err
	}

	s := bisect.NewSession(ckout)
	if err := s.MarkGood(rid); err != nil {
		return err
	}
	fmt.Printf("marked %d as good\n", rid)

	return bisectAutoNext(g, ckout, c.Dir, s)
}

// --- bad ---

type RepoBisectBadCmd struct {
	Version string `arg:"" optional:"" help:"Version to mark (default: current checkout)"`
	Dir     string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoBisectBadCmd) Run(g *Globals) error {
	ckout, repoPath, err := bisectOpenCheckout(g, c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()

	rid, err := bisectResolveVersion(g, ckout, repoPath, c.Version)
	if err != nil {
		return err
	}

	s := bisect.NewSession(ckout)
	if err := s.MarkBad(rid); err != nil {
		return err
	}
	fmt.Printf("marked %d as bad\n", rid)

	return bisectAutoNext(g, ckout, c.Dir, s)
}

// --- skip ---

type RepoBisectSkipCmd struct {
	Version string `arg:"" optional:"" help:"Version to skip (default: current checkout)"`
	Dir     string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoBisectSkipCmd) Run(g *Globals) error {
	ckout, repoPath, err := bisectOpenCheckout(g, c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()

	rid, err := bisectResolveVersion(g, ckout, repoPath, c.Version)
	if err != nil {
		return err
	}

	s := bisect.NewSession(ckout)
	if err := s.Skip(rid); err != nil {
		return err
	}
	fmt.Printf("skipped %d\n", rid)

	return bisectAutoNext(g, ckout, c.Dir, s)
}

// --- next ---

type RepoBisectNextCmd struct {
	Dir string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoBisectNextCmd) Run(g *Globals) error {
	ckout, _, err := bisectOpenCheckout(g, c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()

	s := bisect.NewSession(ckout)
	return bisectAutoNext(g, ckout, c.Dir, s)
}

// --- reset ---

type RepoBisectResetCmd struct {
	Dir string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoBisectResetCmd) Run(g *Globals) error {
	ckout, err := openCheckout(c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()

	s := bisect.NewSession(ckout)
	s.Reset()
	fmt.Println("bisect state cleared")
	return nil
}

// --- ls ---

type RepoBisectLsCmd struct {
	Dir string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoBisectLsCmd) Run(g *Globals) error {
	ckout, _, err := bisectOpenCheckout(g, c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()

	vid, err := checkoutVid(ckout)
	if err != nil {
		return err
	}

	s := bisect.NewSession(ckout)
	entries, err := s.List(libfossil.FslID(vid))
	if err != nil {
		return err
	}

	for _, e := range entries {
		uuid := e.UUID
		if len(uuid) > 10 {
			uuid = uuid[:10]
		}
		label := ""
		if e.Label != "" {
			label = "  " + e.Label
		}
		fmt.Printf("%s  %s%s\n", uuid, e.Date, label)
	}
	return nil
}

// --- status ---

type RepoBisectStatusCmd struct {
	Dir string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoBisectStatusCmd) Run(g *Globals) error {
	ckout, _, err := bisectOpenCheckout(g, c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()

	s := bisect.NewSession(ckout)
	st := s.Status()

	if st.Good == 0 && st.Bad == 0 {
		fmt.Println("no bisect in progress")
		return nil
	}

	if st.Good != 0 {
		fmt.Printf("good: %d\n", st.Good)
	}
	if st.Bad != 0 {
		fmt.Printf("bad:  %d\n", st.Bad)
	}
	if st.Good != 0 && st.Bad != 0 {
		fmt.Printf("~%d steps remaining\n", st.Steps)
	}
	return nil
}

// --- helpers ---

// bisectOpenCheckout opens the checkout DB and ATTACHes the repo so that
// bisect queries (which need plink, blob, event from the repo AND vvar from
// the checkout) work on a single connection.
func bisectOpenCheckout(g *Globals, dir string) (ckout *sql.DB, repoPath string, err error) {
	ckout, err = openCheckout(dir)
	if err != nil {
		return nil, "", err
	}

	// Determine repo path: prefer -R flag, fall back to vvar.
	repoPath = g.Repo
	if repoPath == "" {
		if err := ckout.QueryRow("SELECT value FROM vvar WHERE name='repository'").Scan(&repoPath); err != nil {
			ckout.Close()
			return nil, "", fmt.Errorf("no repository found (use -R or run 'edgesync repo open')")
		}
	}

	// Attach repo DB so bisect can query plink/blob/event tables.
	if _, err := ckout.Exec("ATTACH DATABASE ? AS repo", repoPath); err != nil {
		ckout.Close()
		return nil, "", fmt.Errorf("attaching repo: %w", err)
	}

	return ckout, repoPath, nil
}

// bisectResolveVersion resolves an optional version string to a RID.
// If version is empty, returns the current checkout vid.
func bisectResolveVersion(g *Globals, ckout *sql.DB, repoPath string, version string) (libfossil.FslID, error) {
	if version == "" {
		vid, err := checkoutVid(ckout)
		if err != nil {
			return 0, err
		}
		return libfossil.FslID(vid), nil
	}

	// Resolve against the repo (which is attached as "repo" schema).
	var rid int64
	switch version {
	case "tip":
		err := ckout.QueryRow(
			"SELECT objid FROM repo.event WHERE type='ci' ORDER BY mtime DESC LIMIT 1",
		).Scan(&rid)
		if err != nil {
			return 0, fmt.Errorf("no checkins found")
		}
	case "trunk":
		err := ckout.QueryRow(`
			SELECT tagxref.rid FROM repo.tagxref
			JOIN repo.tag ON tag.tagid=tagxref.tagid
			WHERE tag.tagname='sym-trunk'
			  AND tagxref.tagtype>0
			ORDER BY tagxref.mtime DESC LIMIT 1`,
		).Scan(&rid)
		if err != nil {
			return bisectResolveVersion(g, ckout, repoPath, "tip")
		}
	default:
		err := ckout.QueryRow(
			"SELECT rid FROM repo.blob WHERE uuid LIKE ?", version+"%",
		).Scan(&rid)
		if err != nil {
			return 0, fmt.Errorf("artifact %q not found", version)
		}
	}
	return libfossil.FslID(rid), nil
}

// bisectAutoNext checks if both good and bad are set, finds the midpoint,
// and checks it out.
func bisectAutoNext(g *Globals, ckout *sql.DB, dir string, s *bisect.Session) error {
	st := s.Status()
	if st.Good == 0 || st.Bad == 0 {
		return nil
	}

	mid, err := s.Next()
	if err != nil {
		if errors.Is(err, bisect.ErrBisectComplete) {
			fmt.Printf("bisect complete: %s\n", err)
			return nil
		}
		return err
	}

	// Look up UUID for the midpoint RID (from attached repo).
	var uuid string
	if err := ckout.QueryRow("SELECT uuid FROM repo.blob WHERE rid=?", mid).Scan(&uuid); err != nil {
		return fmt.Errorf("looking up uuid for rid %d: %w", mid, err)
	}

	fmt.Printf("bisecting: checking out %s (rid %d)\n", uuid[:10], mid)

	// Detach repo before running checkout (which opens its own connections).
	ckout.Exec("DETACH DATABASE repo")
	ckout.Close()

	// Determine repo path for -R flag.
	repoFlag := g.Repo
	if repoFlag == "" {
		// Re-open checkout to read vvar if needed.
		ck2, err := openCheckout(dir)
		if err == nil {
			ck2.QueryRow("SELECT value FROM vvar WHERE name='repository'").Scan(&repoFlag)
			ck2.Close()
		}
	}

	coG := &Globals{Repo: repoFlag, Verbose: g.Verbose}
	co := RepoCoCmd{Dir: dir, Force: true, Version: uuid}
	return co.Run(coG)
}
