# Claude Code Configuration

## Skills (tracked in repo)

Skills in `.claude/skills/` are tracked and available on any checkout:

- **tigerstyle** — TigerBeetle-style coding discipline (assertions, 70-line limit, safety-first)
- **deterministic-simulation-testing** — DST patterns for sync engine testing
- **linear-cli** — Linear issue management from CLI

## Required Plugins (user-level, install manually)

These plugins are installed via Claude Code settings, not tracked in the repo.
Add to `~/.claude/settings.json` under `enabledPlugins`:

```json
{
  "enabledPlugins": {
    "superpowers@claude-plugins-official": true,
    "gopls-lsp@claude-plugins-official": true,
    "claude-md-management@claude-plugins-official": true,
    "obsidian@obsidian-skills": true,
    "context7@claude-plugins-official": true,
    "code-simplifier@claude-plugins-official": true,
    "explanatory-output-style@claude-plugins-official": true
  }
}
```
