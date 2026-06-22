#!/usr/bin/env bash
#
# Transform A — wxGetApp().preset_bundle → orca::session().presets().raw_ptr()
#
# Receiver-redirect rewrite: every GUI call site that reaches the PresetBundle
# through wxWidgets goes through the engine Session instead. Local-pointer
# aliases (`auto preset_bundle = wxGetApp().preset_bundle;`) and member aliases
# (`m_preset_bundle = wxGetApp().preset_bundle;`) work transparently because
# raw_ptr() returns the same PresetBundle* the original expression did, so the
# alias keeps its type and all `->` chains continue to work.
#
# Path-filtered to src/slic3r/ via comby's -d flag. The two engine-side hits
# (Model.cpp, SupportPointGenerator.cpp) are inside `//` comments and don't
# affect compilation either way — left alone by virtue of path filter.
#
# Usage: engine/scripts/comby/transform_a.sh [--dry-run]

set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$REPO_ROOT"

ARGS=(
    'wxGetApp().preset_bundle'
    # Root-qualified namespace because many call sites live inside
    # `namespace Slic3r::GUI` and an unqualified `orca::` would resolve as
    # `Slic3r::GUI::orca::` first, failing to find the engine accessor.
    '::orca::session().presets().raw_ptr()'
    .cpp .hpp .h
    -d src/slic3r
    -matcher .c
)

if [[ "${1:-}" == "--dry-run" ]]; then
    ARGS+=(-diff)
    echo "transform_a: DRY RUN (diff only, no files written)"
else
    ARGS+=(-i)
    echo "transform_a: writing changes in-place"
fi

before=$(grep -rEln 'wxGetApp\(\)\.preset_bundle' src/slic3r 2>/dev/null | wc -l)
echo "transform_a: files matching before: $before"

comby "${ARGS[@]}" 2>&1 | tail -20

after=$(grep -rEln 'wxGetApp\(\)\.preset_bundle' src/slic3r 2>/dev/null | wc -l)
echo "transform_a: files matching after:  $after"
