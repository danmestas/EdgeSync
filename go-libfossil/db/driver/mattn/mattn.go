package mattn

import (
	"fmt"
	"strings"

	"github.com/dmestas/edgesync/go-libfossil/db"
	_ "github.com/mattn/go-sqlite3"
)

func init() {
	db.Register(db.DriverConfig{
		Name:     "sqlite3",
		BuildDSN: buildDSN,
	})
}

func buildDSN(path string, pragmas map[string]string) string {
	if len(pragmas) == 0 {
		return path
	}
	var parts []string
	for k, v := range pragmas {
		parts = append(parts, fmt.Sprintf("_%s=%s", k, v))
	}
	return fmt.Sprintf("file:%s?%s", path, strings.Join(parts, "&"))
}
