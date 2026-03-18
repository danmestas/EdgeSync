package repo

import (
	"fmt"
	"os"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/simio"
)

type Repo struct {
	db   *db.DB
	path string
}

func Create(path string, user string, rng simio.Rand) (*Repo, error) {
	if path == "" {
		panic("repo.Create: path must not be empty")
	}
	if rng == nil {
		panic("repo.Create: rng must not be nil")
	}
	if _, err := os.Stat(path); err == nil {
		return nil, fmt.Errorf("repo.Create: file already exists: %s", path)
	}

	d, err := db.Open(path)
	if err != nil {
		return nil, fmt.Errorf("repo.Create open: %w", err)
	}

	if err := db.CreateRepoSchema(d); err != nil {
		d.Close()
		if rmErr := os.Remove(path); rmErr != nil {
			return nil, fmt.Errorf("repo.Create: %w (cleanup failed: %v)", err, rmErr)
		}
		return nil, fmt.Errorf("repo.Create schema: %w", err)
	}

	if err := db.SeedUser(d, user); err != nil {
		d.Close()
		if rmErr := os.Remove(path); rmErr != nil {
			return nil, fmt.Errorf("repo.Create: %w (cleanup failed: %v)", err, rmErr)
		}
		return nil, fmt.Errorf("repo.Create seed user: %w", err)
	}

	if err := db.SeedConfig(d, rng); err != nil {
		d.Close()
		if rmErr := os.Remove(path); rmErr != nil {
			return nil, fmt.Errorf("repo.Create: %w (cleanup failed: %v)", err, rmErr)
		}
		return nil, fmt.Errorf("repo.Create seed config: %w", err)
	}

	return &Repo{db: d, path: path}, nil
}

func Open(path string) (*Repo, error) {
	if path == "" {
		panic("repo.Open: path must not be empty")
	}
	if _, err := os.Stat(path); err != nil {
		return nil, fmt.Errorf("repo.Open: %w", err)
	}

	d, err := db.Open(path)
	if err != nil {
		return nil, fmt.Errorf("repo.Open: %w", err)
	}

	id, err := d.ApplicationID()
	if err != nil {
		d.Close()
		return nil, fmt.Errorf("repo.Open application_id: %w", err)
	}
	if id != libfossil.FossilApplicationID {
		d.Close()
		return nil, fmt.Errorf("repo.Open: not a fossil repo (application_id=%d, want %d)",
			id, libfossil.FossilApplicationID)
	}

	return &Repo{db: d, path: path}, nil
}

func (r *Repo) Close() error {
	return r.db.Close()
}

func (r *Repo) Path() string {
	return r.path
}

func (r *Repo) DB() *db.DB {
	return r.db
}

func (r *Repo) WithTx(fn func(tx *db.Tx) error) error {
	return r.db.WithTx(fn)
}

func (r *Repo) Verify() error {
	if r == nil {
		panic("repo.Verify: receiver must not be nil")
	}
	rows, err := r.db.Query("SELECT rid FROM blob WHERE size >= 0")
	if err != nil {
		return fmt.Errorf("repo.Verify query: %w", err)
	}
	defer rows.Close()

	for rows.Next() {
		var rid int64
		if err := rows.Scan(&rid); err != nil {
			return fmt.Errorf("repo.Verify scan: %w", err)
		}
		if err := content.Verify(r.db, libfossil.FslID(rid)); err != nil {
			return err
		}
	}
	return rows.Err()
}
