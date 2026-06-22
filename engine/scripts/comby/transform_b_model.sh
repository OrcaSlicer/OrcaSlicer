#!/usr/bin/env bash
#
# Transform B (ModelService) — wxGetApp().plater()->model() → ::orca::session().project().raw()
#
# Receiver-redirect for the GUI's Plater::model() callers. Both forms return
# Slic3r::Model& so the rewrite is type-preserving — no syntactic fallout
# expected (no `->` vs `.` flips, no rvalue/lvalue mismatches).
#
# Companion patterns also handle:
#   - `plater_->model()`             (Plater raw pointer member alias, ~1 site)
#   - `wxGetApp().plater()->model()` (the canonical form, ~43 sites)
#
# `plater()->model()` without the wxGetApp() prefix is left alone — those calls
# happen inside Plater itself where the rewrite would self-reference. (Inside
# Plater::method(), the local `p->model` accesses are completely separate from
# the public Plater::model() accessor and are not rewritten.)
#
# Usage: engine/scripts/comby/transform_b_model.sh [--dry-run]

set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$REPO_ROOT"

DRY=0
if [[ "${1:-}" == "--dry-run" ]]; then DRY=1; fi

run_pattern() {
    local match="$1" rewrite="$2"
    local args=("$match" "$rewrite" .cpp .hpp .h -d src/slic3r -matcher .c)
    if (( DRY )); then args+=(-diff); else args+=(-i); fi
    comby "${args[@]}" 2>&1 | tail -5
}

if (( DRY )); then echo "transform_b_model: DRY RUN starting"; else echo "transform_b_model: writing changes in-place"; fi
echo "before: $(grep -rE '(wxGetApp\(\)\.plater\(\)|plater_)->model\(\)' src/slic3r 2>/dev/null | wc -l) total receiver hits"

# Order matters: the wxGetApp form must be rewritten before the bare plater_-> form
# because the wxGetApp pattern is a prefix-superset and we want both to flow to
# the same destination without double-rewriting.
run_pattern 'wxGetApp().plater()->model()' '::orca::session().project().raw()'
run_pattern 'plater_->model()'             '::orca::session().project().raw()'

if (( DRY == 0 )); then
    echo "after:  $(grep -rE '(wxGetApp\(\)\.plater\(\)|plater_)->model\(\)' src/slic3r 2>/dev/null | wc -l) remaining"
fi
