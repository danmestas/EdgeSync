//go:build ncruces && !js

package db

import (
	"fmt"
	"strings"

	_ "github.com/ncruces/go-sqlite3/driver"
)

func driverName() string { return "sqlite3" }

func buildDSN(path string, pragmas map[string]string) string {
	if len(pragmas) == 0 {
		return path
	}
	// ncruces uses same _pragma=name(value) syntax as modernc.
	var parts []string
	for k, v := range pragmas {
		parts = append(parts, fmt.Sprintf("_pragma=%s(%s)", k, v))
	}
	return fmt.Sprintf("file:%s?%s", path, strings.Join(parts, "&"))
}
