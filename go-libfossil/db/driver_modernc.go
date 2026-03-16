//go:build !ncruces && !mattn

package db

import (
	"fmt"
	"strings"

	_ "modernc.org/sqlite"
)

func driverName() string { return "sqlite" }

func buildDSN(path string, pragmas map[string]string) string {
	if len(pragmas) == 0 {
		return path
	}
	var parts []string
	for k, v := range pragmas {
		parts = append(parts, fmt.Sprintf("_pragma=%s(%s)", k, v))
	}
	return fmt.Sprintf("file:%s?%s", path, strings.Join(parts, "&"))
}
