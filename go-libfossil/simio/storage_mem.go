package simio

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// MemStorage is an in-memory Storage implementation for tests and browser fallback.
type MemStorage struct {
	files map[string][]byte
	dirs  map[string]bool
}

// NewMemStorage creates a new in-memory storage.
func NewMemStorage() *MemStorage {
	return &MemStorage{
		files: make(map[string][]byte),
		dirs:  make(map[string]bool),
	}
}

// Stat returns file info for the given path.
func (m *MemStorage) Stat(path string) (os.FileInfo, error) {
	// Check files first
	if data, ok := m.files[path]; ok {
		return &memFileInfo{
			name:  filepath.Base(path),
			size:  int64(len(data)),
			isDir: false,
		}, nil
	}

	// Check directories
	if m.dirs[path] {
		return &memFileInfo{
			name:  filepath.Base(path),
			size:  0,
			isDir: true,
		}, nil
	}

	return nil, fmt.Errorf("stat %s: %w", path, os.ErrNotExist)
}

// Remove deletes the file at the given path.
func (m *MemStorage) Remove(path string) error {
	if _, ok := m.files[path]; !ok {
		return os.ErrNotExist
	}
	delete(m.files, path)
	return nil
}

// MkdirAll creates the directory and all parent directories.
func (m *MemStorage) MkdirAll(path string, perm os.FileMode) error {
	// Mark all parent paths as directories
	parts := strings.Split(path, string(filepath.Separator))
	current := ""
	for _, part := range parts {
		if part == "" {
			continue
		}
		if current == "" {
			current = string(filepath.Separator) + part
		} else {
			current = filepath.Join(current, part)
		}
		m.dirs[current] = true
	}
	return nil
}

// ReadFile reads the file at the given path.
func (m *MemStorage) ReadFile(path string) ([]byte, error) {
	data, ok := m.files[path]
	if !ok {
		return nil, os.ErrNotExist
	}
	// Return a copy to prevent external modification
	result := make([]byte, len(data))
	copy(result, data)
	return result, nil
}

// WriteFile writes data to the file at the given path.
func (m *MemStorage) WriteFile(path string, data []byte, perm os.FileMode) error {
	// Store a copy to prevent external modification
	stored := make([]byte, len(data))
	copy(stored, data)
	m.files[path] = stored
	return nil
}

// memFileInfo implements os.FileInfo for in-memory files.
type memFileInfo struct {
	name  string
	size  int64
	isDir bool
}

func (f *memFileInfo) Name() string       { return f.name }
func (f *memFileInfo) Size() int64        { return f.size }
func (f *memFileInfo) Mode() os.FileMode  { return 0644 }
func (f *memFileInfo) ModTime() time.Time { return time.Time{} }
func (f *memFileInfo) IsDir() bool        { return f.isDir }
func (f *memFileInfo) Sys() any           { return nil }
