package main

import "fmt"

type RepoConfigCmd struct {
	Ls  RepoConfigLsCmd  `cmd:"" help:"List all config entries"`
	Get RepoConfigGetCmd `cmd:"" help:"Get a config value"`
	Set RepoConfigSetCmd `cmd:"" help:"Set a config value"`
}

type RepoConfigLsCmd struct{}

func (c *RepoConfigLsCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	rows, err := r.DB().Query("SELECT name, value FROM config ORDER BY name")
	if err != nil {
		return err
	}
	defer rows.Close()

	for rows.Next() {
		var name, value string
		rows.Scan(&name, &value)
		fmt.Printf("%-20s %s\n", name, value)
	}
	return rows.Err()
}

type RepoConfigGetCmd struct {
	Key string `arg:"" help:"Config key to get"`
}

func (c *RepoConfigGetCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	var value string
	err = r.DB().QueryRow("SELECT value FROM config WHERE name=?", c.Key).Scan(&value)
	if err != nil {
		return fmt.Errorf("config key %q not found", c.Key)
	}
	fmt.Println(value)
	return nil
}

type RepoConfigSetCmd struct {
	Key   string `arg:"" help:"Config key"`
	Value string `arg:"" help:"Config value"`
}

func (c *RepoConfigSetCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	_, err = r.DB().Exec(
		"INSERT OR REPLACE INTO config(name, value, mtime) VALUES(?, ?, strftime('%s','now'))",
		c.Key, c.Value,
	)
	if err != nil {
		return err
	}
	fmt.Printf("%s = %s\n", c.Key, c.Value)
	return nil
}
