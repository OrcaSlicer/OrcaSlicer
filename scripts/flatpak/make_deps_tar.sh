#!/bin/bash
# Pack deps/ into scripts/flatpak/deps.tar for the Flatpak manifest's orca_deps
# module.
#
# Usage: make_deps_tar.sh
#        Requires GNU tar. On macOS, brew install gnu-tar.
#
# The archive is byte-reproducible. Member order, mtimes, ownership and the
# group/other write bits are pinned, so identical deps/ contents always produce
# an identical file. deps/build* and deps/DL_CACHE are excluded.
#
# Prints the output path, size and sha256.

set -euo pipefail

if [ "$#" -ne 0 ]; then
    echo "usage: ${0##*/}" >&2
    exit 2
fi

tar_bin=$(command -v gtar || command -v tar || true)
tar_version=$([ -n "$tar_bin" ] && "$tar_bin" --version 2>/dev/null || true)
case $tar_version in
    *"GNU tar"*) ;;
    *) echo "${0##*/}: needs GNU tar; on macOS run 'brew install gnu-tar'" >&2
       exit 2 ;;
esac

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)
out=$script_dir/deps.tar
tmp=$out.tmp
trap 'rm -f "$tmp"' EXIT

"$tar_bin" --format=gnu \
    --sort=name \
    --mtime=@0 \
    --owner=0 --group=0 --numeric-owner \
    --mode=go-w \
    --exclude='deps/build*' \
    --exclude='deps/DL_CACHE' \
    -cf "$tmp" \
    -C "$repo_root" deps

mv -f "$tmp" "$out"

if command -v sha256sum >/dev/null 2>&1; then
    sum=$(sha256sum "$out" | cut -d' ' -f1)
elif command -v shasum >/dev/null 2>&1; then
    sum=$(shasum -a 256 "$out" | cut -d' ' -f1)
else
    sum=unavailable
fi

echo "Wrote $out ($(du -h "$out" | cut -f1), sha256 $sum)"
