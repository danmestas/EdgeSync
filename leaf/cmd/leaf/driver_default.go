//go:build !wasip1

package main

// Default SQLite driver: pure-Go modernc (CGo not required, no WASM runtime).
// WASI builds use ncruces instead — see driver_wasi.go.
import _ "github.com/danmestas/libfossil/db/driver/modernc"
