//go:build ncruces && js

package db

import (
	"fmt"
	"strings"

	_ "github.com/danmestas/go-sqlite3-opfs"
	_ "github.com/ncruces/go-sqlite3/driver"
)

func driverName() string { return "sqlite3" }

func buildDSN(path string, pragmas map[string]string) string {
	var parts []string
	parts = append(parts, "vfs=opfs")
	for k, v := range pragmas {
		parts = append(parts, fmt.Sprintf("_pragma=%s(%s)", k, v))
	}
	return fmt.Sprintf("file:%s?%s", path, strings.Join(parts, "&"))
}
