package tag

import (
	"container/heap"
	"database/sql"
	"fmt"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/db"
)

// propagate walks the plink DAG from a target artifact to all descendants,
// inserting/deleting tagxref rows to cascade propagating tags.
// This matches Fossil's tag_propagate in tag.c:34-113.
//
// For propagating tags (type 2), it inserts tagxref rows with srcid=0.
// For cancel tags (type 0), it deletes all tagxref entries for that tag.
// Special case: if tagName is "bgcolor", also updates event.bgcolor.
func propagate(q db.Querier, tagid int64, tagType int, origID libfossil.FslID, mtime float64, value string, tagName string, pid libfossil.FslID) error {
	if q == nil {
		panic("tag.propagate: q must not be nil")
	}

	// Priority queue seeded with the target artifact (mtime=0.0 for seed)
	pq := &mtimeQueue{}
	heap.Init(pq)
	heap.Push(pq, &queueItem{rid: pid, mtime: 0.0})

	visited := make(map[libfossil.FslID]bool)

	for pq.Len() > 0 {
		item := heap.Pop(pq).(*queueItem)
		currentRid := item.rid

		if visited[currentRid] {
			continue
		}
		visited[currentRid] = true

		// Query primary children via LEFT JOIN with tagxref.
		// The doit column is 1 if we should propagate/cancel to this child.
		// For propagating: doit = 1 if (srcid=0 AND tagxref.mtime < ?) OR no tagxref entry
		// For cancel: doit = 1 if there's any tagxref entry with srcid=0 (propagated tag)
		var query string
		if tagType == TagPropagating {
			query = `
				SELECT cid, plink.mtime,
				       COALESCE(srcid=0 AND tagxref.mtime < ?, 1) AS doit
				FROM plink
				LEFT JOIN tagxref ON cid=tagxref.rid AND tagxref.tagid=?
				WHERE pid=? AND isprim=1
			`
		} else {
			// For cancel, we want to delete any propagated tag (srcid=0)
			query = `
				SELECT cid, plink.mtime,
				       COALESCE(srcid=0, 0) AS doit
				FROM plink
				LEFT JOIN tagxref ON cid=tagxref.rid AND tagxref.tagid=?
				WHERE pid=? AND isprim=1
			`
		}
		var rows *sql.Rows
		var err error
		if tagType == TagPropagating {
			rows, err = q.Query(query, mtime, tagid, currentRid)
		} else {
			rows, err = q.Query(query, tagid, currentRid)
		}
		if err != nil {
			return fmt.Errorf("query children of %d: %w", currentRid, err)
		}

		children := []struct {
			cid        libfossil.FslID
			childMtime float64
			doit       bool
		}{}

		for rows.Next() {
			var cid int64
			var childMtime float64
			var doitInt int
			if err := rows.Scan(&cid, &childMtime, &doitInt); err != nil {
				rows.Close()
				return fmt.Errorf("scan child: %w", err)
			}
			children = append(children, struct {
				cid        libfossil.FslID
				childMtime float64
				doit       bool
			}{libfossil.FslID(cid), childMtime, doitInt != 0})
		}
		rows.Close()
		if err := rows.Err(); err != nil {
			return fmt.Errorf("rows error: %w", err)
		}

		// Process each child
		for _, child := range children {
			if child.doit {
				if tagType == TagPropagating {
					// Insert propagating tagxref entry
					if _, err := q.Exec(
						`REPLACE INTO tagxref(tagid, tagtype, srcid, origid, value, mtime, rid)
						 VALUES(?, 2, 0, ?, ?, ?, ?)`,
						tagid, origID, value, mtime, child.cid,
					); err != nil {
						return fmt.Errorf("propagate to %d: %w", child.cid, err)
					}

					// Special case: bgcolor updates event table
					if tagName == "bgcolor" {
						if _, err := q.Exec("UPDATE event SET bgcolor=? WHERE objid=?", value, child.cid); err != nil {
							return fmt.Errorf("update event bgcolor for %d: %w", child.cid, err)
						}
					}
				} else {
					// Cancel: delete all tagxref entries for this tag and rid
					if _, err := q.Exec("DELETE FROM tagxref WHERE tagid=? AND rid=?", tagid, child.cid); err != nil {
						return fmt.Errorf("cancel at %d: %w", child.cid, err)
					}

					// Special case: bgcolor cancellation clears event.bgcolor
					if tagName == "bgcolor" {
						if _, err := q.Exec("UPDATE event SET bgcolor=NULL WHERE objid=?", child.cid); err != nil {
							return fmt.Errorf("clear event bgcolor for %d: %w", child.cid, err)
						}
					}
				}

				// Queue child for further processing
				heap.Push(pq, &queueItem{rid: child.cid, mtime: child.childMtime})
			}
		}
	}

	return nil
}

// queueItem represents a node in the priority queue.
type queueItem struct {
	rid   libfossil.FslID
	mtime float64
	index int // heap index
}

// mtimeQueue implements heap.Interface for mtime-ordered priority queue.
type mtimeQueue []*queueItem

func (pq mtimeQueue) Len() int           { return len(pq) }
func (pq mtimeQueue) Less(i, j int) bool { return pq[i].mtime < pq[j].mtime }
func (pq mtimeQueue) Swap(i, j int) {
	pq[i], pq[j] = pq[j], pq[i]
	pq[i].index = i
	pq[j].index = j
}

func (pq *mtimeQueue) Push(x interface{}) {
	n := len(*pq)
	item := x.(*queueItem)
	item.index = n
	*pq = append(*pq, item)
}

func (pq *mtimeQueue) Pop() interface{} {
	old := *pq
	n := len(old)
	item := old[n-1]
	old[n-1] = nil
	item.index = -1
	*pq = old[0 : n-1]
	return item
}
