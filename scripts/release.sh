#!/usr/bin/env bash
#
# scripts/release.sh — drive a coordinated EdgeSync release.
#
# What it does, in order:
#   1. Walk each Go submodule (leaf, bridge). For each, find its latest
#      <name>/v* tag and detect whether <name>/ has changes since. If so,
#      compute the next patch version, create the tag, and push it.
#   2. If any submodule was tagged, rewrite the root go.mod's `require`
#      pin for that submodule to the new version, commit, and push to main.
#   3. Detect whether the root module has changes since its latest v* tag
#      (any new submodule pin counts). If so, tag the next root patch and
#      push it. Pushing the root tag fires .github/workflows/release.yml,
#      which runs goreleaser and dispatches the bones bump-upstream.
#
# Usage:
#   scripts/release.sh                   # do the bumps
#   scripts/release.sh --dry-run         # report what it would do
#   scripts/release.sh --bump=minor      # use minor instead of patch (or major)
#   scripts/release.sh --remote=origin   # remote to push to (default origin)
#
# Local invocation uses your git identity and prompts before any push that
# touches main or pushes a tag. Workflow invocation (auto-release.yml)
# passes --ci to suppress prompts and switches to the github-actions bot
# identity.

set -euo pipefail

DRY_RUN=false
CI_MODE=false
ALLOW_NON_MAIN=false
BUMP_KIND=patch
REMOTE=origin
SUBMODULES=("leaf" "bridge")
ROOT_TAG_PREFIX="v"

for arg in "$@"; do
  case "$arg" in
    --dry-run) DRY_RUN=true ;;
    --ci) CI_MODE=true ;;
    --allow-non-main) ALLOW_NON_MAIN=true ;;
    --bump=patch|--bump=minor|--bump=major) BUMP_KIND="${arg#--bump=}" ;;
    --remote=*) REMOTE="${arg#--remote=}" ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "release.sh: unknown arg: $arg" >&2; exit 2 ;;
  esac
done

log()  { printf '\033[1;36m[release]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[release]\033[0m %s\n' "$*" >&2; }
err()  { printf '\033[1;31m[release]\033[0m %s\n' "$*" >&2; }
run()  {
  if $DRY_RUN; then
    printf '\033[2m[dry-run]\033[0m %s\n' "$*"
  else
    eval "$@"
  fi
}

confirm() {
  $CI_MODE && return 0
  $DRY_RUN && return 0
  printf '\033[1;33m[release]\033[0m %s [y/N] ' "$1"
  read -r reply
  [[ "$reply" =~ ^[Yy]$ ]]
}

# ---------- helpers ----------

# latest_tag_version <prefix>  → echoes "0.0.9" for prefix="leaf/v" if
# leaf/v0.0.9 is the latest, "" if no matching tag.
latest_tag_version() {
  local prefix="$1"
  git tag --list "${prefix}*" --sort=-v:refname \
    | grep -E "^${prefix//./\\.}[0-9]+\\.[0-9]+\\.[0-9]+$" \
    | head -n1 \
    | sed "s|^${prefix}||" \
    || true
}

# bump_version <semver> <patch|minor|major>  → echoes the next semver.
bump_version() {
  local v="$1" kind="$2"
  local major minor patch
  IFS='.' read -r major minor patch <<<"$v"
  case "$kind" in
    patch) patch=$((patch + 1)) ;;
    minor) minor=$((minor + 1)); patch=0 ;;
    major) major=$((major + 1)); minor=0; patch=0 ;;
    *) err "unknown bump kind: $kind"; return 1 ;;
  esac
  printf '%s.%s.%s\n' "$major" "$minor" "$patch"
}

# has_path_changes <ref> <path>  → exits 0 if <path> has any diff between
# <ref> and HEAD. Empty diff returns 1.
has_path_changes() {
  local ref="$1" path="$2"
  ! git diff --quiet "$ref" HEAD -- "$path"
}

# update_gomod_pin <module-path> <new-version>  → in-place edit of go.mod.
# Touches only the require directive, not replace.
update_gomod_pin() {
  local module="$1" version="$2"
  GOMOD_MODULE="$module" GOMOD_VERSION="$version" python3 - <<'PY'
import os, pathlib, re, sys
module = os.environ["GOMOD_MODULE"]
version = os.environ["GOMOD_VERSION"]
p = pathlib.Path("go.mod")
src = p.read_text()
pat = re.compile(rf"^(\s*{re.escape(module)}\s+)v[0-9.]+(\s*(?://.*)?)$", re.MULTILINE)
new, n = pat.subn(rf"\g<1>v{version}\g<2>", src)
if n == 0:
    sys.exit(f"release.sh: no require line for {module} in go.mod")
if n > 1:
    sys.exit(f"release.sh: multiple require lines for {module} in go.mod (got {n}, expected 1)")
p.write_text(new)
PY
}

# ---------- preflight ----------

cd "$(git rev-parse --show-toplevel)"

if [[ "$(git symbolic-ref --short HEAD)" != "main" ]]; then
  if $ALLOW_NON_MAIN; then
    warn "running from $(git symbolic-ref --short HEAD) (--allow-non-main set; intended for testing only)"
  else
    err "must run from main; you're on $(git symbolic-ref --short HEAD)"
    exit 1
  fi
fi

if ! $CI_MODE && ! git diff --quiet HEAD -- go.mod; then
  err "go.mod has uncommitted changes — refusing to start"
  exit 1
fi

log "fetching tags from $REMOTE..."
run "git fetch --tags $REMOTE"

if ! git diff --quiet "$REMOTE/main" HEAD; then
  err "local main is not in sync with $REMOTE/main — pull or push first"
  exit 1
fi

# ---------- step 1: tag submodules ----------

declare -a NEW_SUBMODULE_TAGS=()
declare -A NEW_SUBMODULE_VERSIONS=()

for sm in "${SUBMODULES[@]}"; do
  cur=$(latest_tag_version "${sm}/v")
  if [[ -z "$cur" ]]; then
    warn "no ${sm}/v* tag exists; skipping (cut a baseline tag manually)"
    continue
  fi

  if ! has_path_changes "${sm}/v${cur}" "${sm}/"; then
    log "${sm}: no changes since ${sm}/v${cur}; skipping"
    continue
  fi

  next=$(bump_version "$cur" "$BUMP_KIND")
  log "${sm}: ${cur} → ${next} (changes since ${sm}/v${cur})"
  if ! confirm "tag ${sm}/v${next} at HEAD and push?"; then
    warn "${sm}: skipped by user"
    continue
  fi

  run "git tag '${sm}/v${next}' HEAD -m '${sm} v${next}: auto-release'"
  run "git push '$REMOTE' '${sm}/v${next}'"

  NEW_SUBMODULE_TAGS+=("${sm}/v${next}")
  NEW_SUBMODULE_VERSIONS["$sm"]="$next"
done

# ---------- step 2: bump root pins for newly-tagged submodules ----------

PIN_BUMP_NEEDED=false
for sm in "${!NEW_SUBMODULE_VERSIONS[@]}"; do
  module_path="github.com/danmestas/EdgeSync/${sm}"
  new_version="${NEW_SUBMODULE_VERSIONS[$sm]}"
  log "rewriting go.mod: ${module_path} → v${new_version}"
  if ! $DRY_RUN; then
    update_gomod_pin "$module_path" "$new_version"
  fi
  PIN_BUMP_NEEDED=true
done

if $PIN_BUMP_NEEDED; then
  if $DRY_RUN || git diff --quiet HEAD -- go.mod; then
    if ! $DRY_RUN; then
      warn "expected go.mod diff after pin update but found none — skipping commit"
      PIN_BUMP_NEEDED=false
    fi
  fi
fi

if $PIN_BUMP_NEEDED; then
  msg="chore: bump submodule pins ($(IFS=,; echo "${NEW_SUBMODULE_TAGS[*]}"))"
  log "committing: $msg"
  if $CI_MODE; then
    run "git config user.name  'github-actions[bot]'"
    run "git config user.email '41898282+github-actions[bot]@users.noreply.github.com'"
  fi
  if confirm "commit pin bump and push to $REMOTE/main?"; then
    run "git add go.mod"
    run "git commit -m '${msg}'"
    run "git push '$REMOTE' main"
  else
    warn "user declined pin bump push — aborting before root tag"
    exit 1
  fi
fi

# ---------- step 3: tag root ----------

root_cur=$(latest_tag_version "${ROOT_TAG_PREFIX}")
if [[ -z "$root_cur" ]]; then
  err "no v* tag exists on root; cut a baseline tag manually"
  exit 1
fi

root_changes=false
if $PIN_BUMP_NEEDED; then
  # The pin commit itself is a root-relevant change.
  root_changes=true
elif git diff --name-only "${ROOT_TAG_PREFIX}${root_cur}" HEAD -- \
       ':!leaf/' ':!bridge/' ':!docs/' ':!**.md' ':!LICENSE' ':!wrangler.jsonc' \
     | grep -q .; then
  # Detect non-submodule, non-doc changes (e.g. hub/ commits) since last v*.
  root_changes=true
fi

if ! $root_changes; then
  log "root: no relevant changes since ${ROOT_TAG_PREFIX}${root_cur}; nothing to tag"
  exit 0
fi

root_next=$(bump_version "$root_cur" "$BUMP_KIND")
log "root: ${root_cur} → ${root_next}"
if ! confirm "tag ${ROOT_TAG_PREFIX}${root_next} at HEAD and push (fires release.yml + bones dispatch)?"; then
  warn "user declined root tag — submodules tagged + pin bumped but root release skipped"
  exit 1
fi

# Build the annotation body listing the submodule bumps for context.
annotation="${ROOT_TAG_PREFIX}${root_next}: auto-release"
if [[ ${#NEW_SUBMODULE_TAGS[@]} -gt 0 ]]; then
  annotation="${annotation} (includes $(IFS=,; echo "${NEW_SUBMODULE_TAGS[*]}"))"
fi

run "git tag '${ROOT_TAG_PREFIX}${root_next}' HEAD -m '${annotation}'"
run "git push '$REMOTE' '${ROOT_TAG_PREFIX}${root_next}'"

log "done. release.yml is now firing on ${ROOT_TAG_PREFIX}${root_next}."
