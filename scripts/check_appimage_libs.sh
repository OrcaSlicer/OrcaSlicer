#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=/dev/null
source "${SCRIPT_DIR}/appimage_lib_policy.sh"

usage() {
    echo "Usage: $0 <appdir> [entrypoint]"
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
    usage
    exit 1
fi

APPDIR="$1"
ENTRYPOINT="${2:-}"

if [[ ! -d "$APPDIR" ]]; then
    echo "Error: AppDir does not exist: $APPDIR"
    exit 1
fi

APPDIR="$(cd -- "$APPDIR" && pwd)"

if [[ -n "$ENTRYPOINT" ]]; then
    if [[ ! -e "$ENTRYPOINT" ]]; then
        echo "Error: entrypoint does not exist: $ENTRYPOINT"
        exit 1
    fi
    ENTRYPOINT="$(cd -- "$(dirname -- "$ENTRYPOINT")" && pwd)/$(basename -- "$ENTRYPOINT")"
fi

declare -a lib_paths=(
    "$APPDIR/lib/orca-runtime"
    "$APPDIR/lib"
    "$APPDIR/bin"
)

for candidate in \
    "$APPDIR/lib/gstreamer-1.0" \
    "$APPDIR/lib/gio/modules" \
    "$APPDIR/lib/gdk-pixbuf-2.0/2.10.0/loaders"; do
    if [[ -d "$candidate" ]]; then
        lib_paths+=("$candidate")
    fi
done

audit_ld_library_path="$(IFS=:; printf '%s' "${lib_paths[*]}")"

declare -a targets=()
declare -A seen_unresolved=()
declare -A seen_host=()

if [[ -n "$ENTRYPOINT" ]]; then
    targets+=("$ENTRYPOINT")
fi

while IFS= read -r -d '' file; do
    if appimage_is_elf_file "$file"; then
        targets+=("$file")
    fi
done < <(find "$APPDIR" -type f -print0)

for target in "${targets[@]}"; do
    while IFS= read -r dep; do
        if [[ "$dep" == MISSING:* ]]; then
            seen_unresolved["$target -> ${dep#MISSING:}"]=1
            continue
        fi

        dep="$(readlink -f "$dep" 2>/dev/null || printf '%s' "$dep")"
        if [[ "$dep" == "$APPDIR"* ]]; then
            continue
        fi

        if appimage_is_host_library "$dep"; then
            continue
        fi

        seen_host["$target -> $dep"]=1
    done < <(LD_LIBRARY_PATH="$audit_ld_library_path${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" appimage_list_direct_dependencies "$target")
done

if (( ${#seen_unresolved[@]} > 0 )); then
    echo "AppImage dependency audit failed: unresolved runtime libraries detected"
    printf '%s\n' "${!seen_unresolved[@]}" | LC_ALL=C sort
    exit 1
fi

if (( ${#seen_host[@]} > 0 )); then
    echo "AppImage dependency audit failed: unexpected host libraries are still required"
    printf '%s\n' "${!seen_host[@]}" | LC_ALL=C sort
    exit 1
fi

echo "AppImage dependency audit passed: $APPDIR"
