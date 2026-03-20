//go:build wasip1 || js

package db

// wasmPragmaOverrides returns nil on WASM — pragmas are skipped entirely.
func wasmPragmaOverrides() map[string]string {
	return nil
}

// wasmDSNSuffix appends nolock=1 to disable file locking on WASM.
// WASI runtimes don't support POSIX/OFD/BSD file locks.
func wasmDSNSuffix() string {
	return "nolock=1"
}

// wasmClearPragmas signals that WASM builds should skip DSN pragmas entirely.
const wasmClearPragmas = true
