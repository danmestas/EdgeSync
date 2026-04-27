//go:build wasip1

package main

// WASI SQLite driver: ncruces (WASM-based; modernc.org/libc has no wasip1 port).
import _ "github.com/danmestas/libfossil/db/driver/ncruces"
