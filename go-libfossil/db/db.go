package db

import (
	"database/sql"
	"fmt"
)

// DB wraps a SQLite database connection.
type DB struct {
	conn   *sql.DB
	path   string
	driver string
}

// Open opens a SQLite database with the build-tag-selected driver and default pragmas.
func Open(path string) (*DB, error) {
	return OpenWith(path, OpenConfig{})
}

// OpenWith opens a SQLite database with explicit configuration.
func OpenWith(path string, cfg OpenConfig) (*DB, error) {
	driver := cfg.Driver
	if driver == "" {
		driver = driverFromEnv()
	}

	pragmas := defaultPragmas()
	for k, v := range cfg.Pragmas {
		pragmas[k] = v
	}

	dsn := buildDSN(path, pragmas)
	conn, err := sql.Open(driver, dsn)
	if err != nil {
		return nil, fmt.Errorf("db.Open(%s): %w", driver, err)
	}

	return &DB{conn: conn, path: path, driver: driver}, nil
}

func (d *DB) Close() error {
	return d.conn.Close()
}

func (d *DB) Path() string {
	return d.path
}

func (d *DB) Driver() string {
	return d.driver
}

func (d *DB) Exec(query string, args ...any) (sql.Result, error) {
	return d.conn.Exec(query, args...)
}

func (d *DB) QueryRow(query string, args ...any) *sql.Row {
	return d.conn.QueryRow(query, args...)
}

func (d *DB) Query(query string, args ...any) (*sql.Rows, error) {
	return d.conn.Query(query, args...)
}

func (d *DB) SetApplicationID(id int32) error {
	_, err := d.conn.Exec(fmt.Sprintf("PRAGMA application_id=%d", id))
	return err
}

func (d *DB) ApplicationID() (int32, error) {
	var id int32
	err := d.conn.QueryRow("PRAGMA application_id").Scan(&id)
	return id, err
}

type Tx struct {
	tx *sql.Tx
}

func (t *Tx) Exec(query string, args ...any) (sql.Result, error) {
	return t.tx.Exec(query, args...)
}

func (t *Tx) QueryRow(query string, args ...any) *sql.Row {
	return t.tx.QueryRow(query, args...)
}

func (t *Tx) Query(query string, args ...any) (*sql.Rows, error) {
	return t.tx.Query(query, args...)
}

func (d *DB) WithTx(fn func(tx *Tx) error) error {
	sqlTx, err := d.conn.Begin()
	if err != nil {
		return fmt.Errorf("db.WithTx begin: %w", err)
	}
	if err := fn(&Tx{tx: sqlTx}); err != nil {
		sqlTx.Rollback()
		return err
	}
	return sqlTx.Commit()
}
