package main

import "fmt"

type RepoInfoCmd struct{}

func (c *RepoInfoCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	var blobCount, deltaCount, phantomCount int
	var totalSize int64
	var projectCode, serverCode string

	r.DB().QueryRow("SELECT count(*) FROM blob WHERE size >= 0").Scan(&blobCount)
	r.DB().QueryRow("SELECT count(*) FROM delta").Scan(&deltaCount)
	r.DB().QueryRow("SELECT count(*) FROM phantom").Scan(&phantomCount)
	r.DB().QueryRow("SELECT coalesce(sum(size),0) FROM blob WHERE size >= 0").Scan(&totalSize)
	r.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projectCode)
	r.DB().QueryRow("SELECT value FROM config WHERE name='server-code'").Scan(&serverCode)

	fmt.Printf("project-code:  %s\n", projectCode)
	fmt.Printf("server-code:   %s\n", serverCode)
	fmt.Printf("blobs:         %d\n", blobCount)
	fmt.Printf("deltas:        %d\n", deltaCount)
	fmt.Printf("phantoms:      %d\n", phantomCount)
	fmt.Printf("total size:    %d bytes\n", totalSize)
	return nil
}
