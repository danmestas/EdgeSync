#!/bin/sh
# Install EdgeSync from a GitHub release: the edgesync CLI and the leaf agent.
#
#   curl -fsSL https://raw.githubusercontent.com/danmestas/EdgeSync/main/install.sh | sh
#
# Name the binaries to install only some of them:
#
#   curl -fsSL https://raw.githubusercontent.com/danmestas/EdgeSync/main/install.sh | sh -s -- leaf
#
# EDGESYNC_VERSION      release tag to install, e.g. v0.0.21 (default: the latest)
# EDGESYNC_INSTALL_DIR  where the binaries go (default: $HOME/.local/bin)
#
# Each archive is checked against the release's checksums.txt before anything
# is installed. A file curl downloads carries no quarantine flag, so macOS runs
# the binaries without the xattr step a browser download needs.
set -eu

repo="danmestas/EdgeSync"
install_dir="${EDGESYNC_INSTALL_DIR:-$HOME/.local/bin}"
binaries="${*:-edgesync leaf}"

fail() {
	echo "edgesync install: $*" >&2
	exit 1
}

case "$(uname -s)" in
Darwin) os=darwin ;;
Linux) os=linux ;;
*) fail "unsupported OS $(uname -s); releases are built for macOS and Linux" ;;
esac

case "$(uname -m)" in
x86_64 | amd64) arch=x86_64 ;;
arm64 | aarch64) arch=arm64 ;;
*) fail "unsupported CPU $(uname -m)" ;;
esac

for binary in $binaries; do
	case "$binary" in
	edgesync | leaf) ;;
	*) fail "unknown binary '$binary'; choose edgesync, leaf or both" ;;
	esac
done

command -v curl >/dev/null || fail "curl is required"
command -v tar >/dev/null || fail "tar is required"
if command -v sha256sum >/dev/null; then
	sha256() { sha256sum "$1" | cut -d' ' -f1; }
elif command -v shasum >/dev/null; then
	sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
	fail "sha256sum or shasum is required"
fi

tag="${EDGESYNC_VERSION:-}"
if [ -z "$tag" ]; then
	# The latest-release page redirects to the tag's page; reading the redirect
	# avoids the GitHub API and its rate limit.
	url=$(curl -fsSL -o /dev/null -w '%{url_effective}' "https://github.com/$repo/releases/latest") ||
		fail "could not reach github.com"
	tag="${url##*/}"
fi
case "$tag" in
v[0-9]*) ;;
*) fail "no release found (got '$tag')" ;;
esac

base="https://github.com/$repo/releases/download/$tag"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

curl -fsSL -o "$tmp/checksums.txt" "$base/checksums.txt" || fail "no checksums.txt in release $tag"
mkdir -p "$install_dir"

for binary in $binaries; do
	# goreleaser names the edgesync archive after the project.
	case "$binary" in
	edgesync) archive="EdgeSync_${tag#v}_${os}_${arch}.tar.gz" ;;
	leaf) archive="leaf_${tag#v}_${os}_${arch}.tar.gz" ;;
	esac

	echo "Downloading $binary $tag for $os/$arch"
	curl -fsSL -o "$tmp/$archive" "$base/$archive" || fail "no $archive in release $tag"
	want=$(awk -v f="$archive" '$2 == f { print $1 }' "$tmp/checksums.txt")
	[ -n "$want" ] || fail "checksums.txt does not list $archive"
	got=$(sha256 "$tmp/$archive")
	[ "$got" = "$want" ] || fail "checksum mismatch for $archive: got $got, want $want"

	mkdir "$tmp/$binary.d"
	tar -xzf "$tmp/$archive" -C "$tmp/$binary.d"
	[ -f "$tmp/$binary.d/$binary" ] || fail "$archive has no $binary binary"
	# Install through a temporary name so a running binary is replaced, not
	# overwritten in place.
	cp "$tmp/$binary.d/$binary" "$install_dir/.$binary.new"
	chmod 755 "$install_dir/.$binary.new"
	mv -f "$install_dir/.$binary.new" "$install_dir/$binary"
	echo "Installed $install_dir/$binary ($tag)"
done

case ":$PATH:" in
*":$install_dir:"*) ;;
*) echo "Add $install_dir to your PATH to run these from anywhere." ;;
esac
