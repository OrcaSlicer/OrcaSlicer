#!/usr/bin/env bash
# Validate every JSON file under resources/profiles/, plus cross-references
# (sub_path / inherits / default_filament_profile / default_print_profile /
# include) from each vendor's index file.
#
# Exits 0 if everything resolves cleanly. Exits 1 with a human-readable report
# of broken / missing / malformed files otherwise.
#
# Usage:
#   tools/validate_profile_json.sh                  # full repo, $(nproc) workers
#   tools/validate_profile_json.sh resources/profiles/BBL  # one vendor
#   JOBS=8 tools/validate_profile_json.sh           # explicit parallelism
#
# Designed to run pre-commit so JSON drift / dangling references / zero-byte
# files (the kind the H2C port has been hit by) are caught before a build.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="${1:-$REPO_ROOT/resources/profiles}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

if [ ! -d "$ROOT" ]; then
    echo "ERR: $ROOT is not a directory" >&2
    exit 2
fi

# Make jq deterministic across locales.
export LC_ALL=C

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Per-category problem files: workers append; main aggregates at the end.
# Each worker writes "<file>\t<detail>" lines.
PARSE_FAILED="$WORK/parse_failed"
EMPTY_FILES="$WORK/empty_files"
MISSING_REFS="$WORK/missing_refs"
DUPLICATE_REFS="$WORK/duplicate_refs"
INHERITS_MISSING="$WORK/inherits_missing"
PROFILE_MISSING="$WORK/profile_missing"
INSTANTIATION_BAD="$WORK/instantiation_bad"
TEMPLATE_INCLUDES="$WORK/template_includes"
for _f in "$PARSE_FAILED" "$EMPTY_FILES" "$MISSING_REFS" \
          "$DUPLICATE_REFS" "$INHERITS_MISSING" "$PROFILE_MISSING" \
          "$INSTANTIATION_BAD" "$TEMPLATE_INCLUDES"; do
    : >"$_f"
done

# ---------------------------------------------------------------------------
# Worker 1 — parse + instantiation literal sanity (per file)
# Functions below are invoked through xargs bash -c '<fn> "$@"' _ <args> so
# the linter cannot see the call site (SC2329 silenced per-fn).
# ---------------------------------------------------------------------------
# shellcheck disable=SC2329
parse_worker() {
    local f="$1"
    if [ ! -s "$f" ]; then
        printf '%s\n' "$f" >>"$EMPTY_FILES"
        return
    fi
    if ! jq empty "$f" 2>/dev/null; then
        printf '%s\n' "$f" >>"$PARSE_FAILED"
        return
    fi
    local inst
    inst=$(jq -r '.instantiation // empty' "$f" 2>/dev/null)
    case "$inst" in
        ''|true|false) ;;
        *) printf '%s :: instantiation=%s (must be "true" or "false")\n' "$f" "$inst" >>"$INSTANTIATION_BAD";;
    esac
}
export PARSE_FAILED EMPTY_FILES INSTANTIATION_BAD
export -f parse_worker

# ---------------------------------------------------------------------------
# Worker 2 — vendor index (sub_path) cross-resolution (per index file)
# ---------------------------------------------------------------------------
# shellcheck disable=SC2329
vendor_index_worker() {
    local vendor_idx="$1"
    local vendor_dir="${vendor_idx%.json}"
    [ -d "$vendor_dir" ] || return 0
    jq empty "$vendor_idx" 2>/dev/null || return 0

    # tab-separated: section, name, sub_path
    local seen_file="$WORK/seen.$$.$RANDOM"
    : >"$seen_file"
    while IFS=$'\t' read -r section name sub_path; do
        [ -z "$sub_path" ] && continue
        local target="$vendor_dir/$sub_path"
        if [ ! -f "$target" ]; then
            printf '%s :: %s/%s -> %s\n' "$vendor_idx" "$section" "$name" "$sub_path" >>"$MISSING_REFS"
            continue
        fi
        if [ ! -s "$target" ]; then
            printf '%s  (referenced from %s)\n' "$target" "$vendor_idx" >>"$EMPTY_FILES"
            continue
        fi
        if ! jq empty "$target" 2>/dev/null; then
            printf '%s  (referenced from %s)\n' "$target" "$vendor_idx" >>"$PARSE_FAILED"
        fi
        local prior
        prior=$(grep -F "${sub_path}=" "$seen_file" 2>/dev/null | cut -d= -f2- | head -n1)
        if [ -n "$prior" ]; then
            printf '%s :: %s appears in both %s and %s\n' "$vendor_idx" "$sub_path" "$prior" "$section" >>"$DUPLICATE_REFS"
        else
            printf '%s=%s\n' "$sub_path" "$section" >>"$seen_file"
        fi
    done < <(jq -r '
        to_entries[]
        | select(.value | type == "array")
        | .key as $sec
        | .value[]?
        | select(type == "object" and has("sub_path"))
        | "\($sec)\t\(.name // "?")\t\(.sub_path)"' "$vendor_idx" 2>/dev/null)
    rm -f "$seen_file"
}
export MISSING_REFS DUPLICATE_REFS WORK
export -f vendor_index_worker

# ---------------------------------------------------------------------------
# Worker 3 — extract `name` from each profile (for VALID_PROFILES table)
# ---------------------------------------------------------------------------
# shellcheck disable=SC2329
name_extractor() {
    local f="$1"
    [ -s "$f" ] || return 0
    local n vendor
    n=$(jq -r '.name // empty' "$f" 2>/dev/null) || return 0
    [ -n "$n" ] || return 0
    # vendor = first path component below $ROOT
    vendor=$(dirname "${f#"$ROOT"/}" | cut -d/ -f1)
    # Two rows: vendor-scoped + global. Cross-vendor inheritance is legitimate
    # (e.g. BBL profiles inheriting `Generic PLA @System` from OrcaFilamentLibrary).
    printf '%s\t%s\n' "$vendor" "$n"
    printf '*\t%s\n' "$n"
}
export -f name_extractor

# ---------------------------------------------------------------------------
# Worker 4 — inherits / default_*_profile / include cross-ref check.
# Reads the VALID_PROFILES file (vendor<TAB>name) via env $VALID_TABLE.
# ---------------------------------------------------------------------------
# shellcheck disable=SC2329
xref_worker() {
    local p="$1"
    [ -s "$p" ] || return 0
    local vendor
    vendor=$(dirname "${p#"$ROOT"/}" | cut -d/ -f1)

    # Resolution order: same-vendor first, then cross-vendor fallback (rows
    # with vendor='*' added by name_extractor for every profile).
    name_known() {
        grep -qF "$(printf '%s\t%s' "$vendor" "$1")" "$VALID_TABLE" \
            || grep -qF "$(printf '*\t%s' "$1")" "$VALID_TABLE"
    }

    local inh
    inh=$(jq -r '.inherits // empty' "$p" 2>/dev/null)
    if [ -n "$inh" ] && ! name_known "$inh"; then
        printf "%s :: inherits='%s' (no profile with this name)\n" "$p" "$inh" >>"$INHERITS_MISSING"
    fi

    # machine profiles: default_*_profile + include
    case "$p" in
        */machine/*)
            while IFS= read -r ref; do
                [ -z "$ref" ] && continue
                if ! name_known "$ref"; then
                    printf "%s :: default_filament_profile -> '%s'\n" "$p" "$ref" >>"$PROFILE_MISSING"
                fi
            done < <(jq -r '.default_filament_profile // empty | if type == "array" then .[] else . end' "$p" 2>/dev/null)
            local pp
            pp=$(jq -r '.default_print_profile // empty' "$p" 2>/dev/null)
            if [ -n "$pp" ] && ! name_known "$pp"; then
                printf "%s :: default_print_profile -> '%s'\n" "$p" "$pp" >>"$PROFILE_MISSING"
            fi
            while IFS= read -r inc; do
                [ -z "$inc" ] && continue
                if ! name_known "$inc"; then
                    printf "%s :: include -> '%s' (no profile with this name)\n" "$p" "$inc" >>"$TEMPLATE_INCLUDES"
                fi
            done < <(jq -r '.include // empty | if type == "array" then .[] else . end' "$p" 2>/dev/null)
            ;;
    esac
}
export INHERITS_MISSING PROFILE_MISSING TEMPLATE_INCLUDES ROOT
export -f xref_worker

# ---------------------------------------------------------------------------
# Step 1: parse + instantiation (per file, parallel)
# ---------------------------------------------------------------------------
echo "==> Step 1: parse every .json under $ROOT (jobs=$JOBS)"
TOTAL=$(find "$ROOT" -name '*.json' -type f | wc -l)
find "$ROOT" -name '*.json' -type f -print0 \
    | xargs -0 -n1 -P "$JOBS" bash -c 'parse_worker "$@"' _

# ---------------------------------------------------------------------------
# Step 2: vendor index sub_path resolution (per index, parallel)
# ---------------------------------------------------------------------------
echo "==> Step 2: vendor index sub_path resolution"
find "$ROOT" -maxdepth 1 -name '*.json' -type f -print0 \
    | xargs -0 -n1 -P "$JOBS" bash -c 'vendor_index_worker "$@"' _

# ---------------------------------------------------------------------------
# Step 3: build VALID_PROFILES table (vendor<TAB>name); each row distinct.
# ---------------------------------------------------------------------------
echo "==> Step 3: build profile name registry"
VALID_TABLE="$WORK/valid_profiles"
find "$ROOT" -mindepth 2 -name '*.json' -type f -print0 \
    | xargs -0 -n1 -P "$JOBS" bash -c 'name_extractor "$@"' _ \
    | sort -u >"$VALID_TABLE"
echo "    registered $(wc -l <"$VALID_TABLE") profile names"
export VALID_TABLE

# ---------------------------------------------------------------------------
# Step 4: cross-reference inherits / default_*_profile / include (parallel)
# ---------------------------------------------------------------------------
echo "==> Step 4: inherits / default_*_profile / include resolution"
find "$ROOT" -mindepth 2 -name '*.json' -type f -print0 \
    | xargs -0 -n1 -P "$JOBS" bash -c 'xref_worker "$@"' _

# ---------------------------------------------------------------------------
# Aggregate + report
# ---------------------------------------------------------------------------
sort -u -o "$PARSE_FAILED"     "$PARSE_FAILED"
sort -u -o "$EMPTY_FILES"      "$EMPTY_FILES"
sort -u -o "$MISSING_REFS"     "$MISSING_REFS"
sort -u -o "$DUPLICATE_REFS"   "$DUPLICATE_REFS"
sort -u -o "$INHERITS_MISSING" "$INHERITS_MISSING"
sort -u -o "$PROFILE_MISSING"  "$PROFILE_MISSING"
sort -u -o "$INSTANTIATION_BAD" "$INSTANTIATION_BAD"
sort -u -o "$TEMPLATE_INCLUDES" "$TEMPLATE_INCLUDES"

count_nonempty() { [ -s "$1" ] && wc -l <"$1" || echo 0; }
n_parse=$(count_nonempty "$PARSE_FAILED")
n_empty=$(count_nonempty "$EMPTY_FILES")
n_missing=$(count_nonempty "$MISSING_REFS")
n_dup=$(count_nonempty "$DUPLICATE_REFS")
n_inherits=$(count_nonempty "$INHERITS_MISSING")
n_profile=$(count_nonempty "$PROFILE_MISSING")
n_inst=$(count_nonempty "$INSTANTIATION_BAD")
n_inc=$(count_nonempty "$TEMPLATE_INCLUDES")
TOTAL_PROBLEMS=$((n_parse + n_empty + n_missing + n_dup + n_inherits + n_profile + n_inst + n_inc))

if [ "$TOTAL_PROBLEMS" -eq 0 ]; then
    echo
    echo "OK: $TOTAL files validated; no broken references."
    exit 0
fi

report_section() {
    local title="$1" file="$2" n
    n=$(count_nonempty "$file")
    [ "$n" -eq 0 ] && return 0
    echo
    echo "### $title ($n)"
    sed 's/^/  - /' "$file"
}

echo
echo "FAILED: $TOTAL_PROBLEMS problem(s) across $TOTAL files"
report_section "Empty / missing files"      "$EMPTY_FILES"
report_section "Files that don't parse"     "$PARSE_FAILED"
report_section "Vendor index dangling refs" "$MISSING_REFS"
report_section "Vendor index duplicate refs" "$DUPLICATE_REFS"
report_section "inherits not found"         "$INHERITS_MISSING"
report_section "default_*_profile dangling" "$PROFILE_MISSING"
report_section "Bad instantiation literals" "$INSTANTIATION_BAD"
report_section "include refs unresolved"    "$TEMPLATE_INCLUDES"
exit 1
