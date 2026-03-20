package simio

import "os"

// Storage abstracts filesystem operations for WASM portability.
type Storage interface {
	Stat(path string) (os.FileInfo, error)
	Remove(path string) error
	MkdirAll(path string, perm os.FileMode) error
	ReadFile(path string) ([]byte, error)
	WriteFile(path string, data []byte, perm os.FileMode) error
}

// OSStorage delegates to the os package. Used in production and WASI.
type OSStorage struct{}

func (OSStorage) Stat(path string) (os.FileInfo, error)                      { return os.Stat(path) }
func (OSStorage) Remove(path string) error                                    { return os.Remove(path) }
func (OSStorage) MkdirAll(path string, perm os.FileMode) error               { return os.MkdirAll(path, perm) }
func (OSStorage) ReadFile(path string) ([]byte, error)                        { return os.ReadFile(path) }
func (OSStorage) WriteFile(path string, data []byte, perm os.FileMode) error { return os.WriteFile(path, data, perm) }
