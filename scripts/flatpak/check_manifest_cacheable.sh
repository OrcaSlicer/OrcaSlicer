#!/bin/bash
# Check a Flatpak manifest for `type: dir` sources at or before the orca_deps
# module.
#
# Usage: check_manifest_cacheable.sh [manifest]
#        Defaults to com.orcaslicer.OrcaSlicer.yml next to this script.
#
# Exits 0 when none are found, 1 when any are, listing them as file:line, and
# 2 when the manifest is missing or has no orca_deps module.

set -euo pipefail

anchor=orca_deps
manifest=${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/com.orcaslicer.OrcaSlicer.yml}

module_re='^  - name: '
dir_re='(^|[-{,[:space:]])type:[[:space:]]*dir([,}[:space:]]|$)'

if [ ! -f "$manifest" ]; then
    echo "$manifest: no such file" >&2
    exit 2
fi

anchor_start=$(grep -n "${module_re}${anchor}[[:space:]]*$" "$manifest" | cut -d: -f1 || true)
if [ -z "$anchor_start" ]; then
    echo "$manifest: no module named '$anchor'; this check needs updating" >&2
    exit 2
fi

# First module header after the anchor, or EOF if the anchor is last.
anchor_end=$(grep -n "$module_re" "$manifest" | cut -d: -f1 |
             awk -v s="$anchor_start" '$1 > s { print $1; exit }')
[ -n "$anchor_end" ] || anchor_end=$(awk 'END { print NR + 1 }' "$manifest")

violations=$(awk -v e="$anchor_end" -v f="$manifest" -v mre="$module_re" -v dre="$dir_re" '
    { sub(/\r$/, "") }
    /^[[:space:]]*#/ { next }
    $0 ~ mre { module = $3 }
    NR < e && $0 ~ dre {
        printf "%s:%d: %s (module %s)\n", f, NR, $0, module
    }' "$manifest")

if [ -n "$violations" ]; then
    printf '%s\n' "$violations" >&2
    echo >&2
    echo "flatpak-builder cannot checksum a directory, so each of these makes" >&2
    echo "$anchor and every module after it rebuild from scratch on every run." >&2
    echo "See the $anchor sources in this manifest for the tarball used instead." >&2
    exit 1
fi

echo "manifest OK: no 'type: dir' source at or before $anchor"
