//go:build mattn

package db

import (
	"fmt"
	"strings"

	_ "github.com/mattn/go-sqlite3"
)

func driverName() string { return "sqlite3" }

func buildDSN(path string, pragmas map[string]string) string {
	if len(pragmas) == 0 {
		return path
	}
	// mattn uses _name=value syntax (underscore-prefixed pragma names).
	var parts []string
	for k, v := range pragmas {
		parts = append(parts, fmt.Sprintf("_%s=%s", k, v))
	}
	return fmt.Sprintf("file:%s?%s", path, strings.Join(parts, "&"))
}
