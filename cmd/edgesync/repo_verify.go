package main

import "fmt"

type RepoVerifyCmd struct{}

func (c *RepoVerifyCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	if err := r.Verify(); err != nil {
		return fmt.Errorf("verification failed: %w", err)
	}
	fmt.Println("repository integrity verified")
	return nil
}
