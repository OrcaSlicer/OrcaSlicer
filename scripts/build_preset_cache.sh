#!/usr/bin/env bash
# Build the per-vendor system preset caches (one <vendor>.opc per vendor) by
# running the generate_system_cache dev tool against a profiles directory, and
# make every profiles directory named on the command line ship-ready: install
# the caches into it and delete the preset JSONs they replace, so a build ships
# one copy of its presets instead of two.
#
#   ./scripts/build_preset_cache.sh                     # caches into resources/profiles
#   ./scripts/build_preset_cache.sh -b build/arm64      # search this build tree for the tool
#   ./scripts/build_preset_cache.sh <dir> [<dir> ...]   # and ship into these profiles dirs
#
# Caches are generated into the source tree's resources/profiles, which is what
# every packaging step copies from. Shipping deletes, so it is a CI packaging
# step: pass packaged output directories, or the checkout of a build that is
# about to be packaged from it.
#
# A vendor's own <vendor>.json goes along with its preset JSONs: the cache
# carries the vendor profile and the version it was built at, so discovery,
# version checks and installing all read it there. A shipped vendor is its cache
# and nothing else. Only a vendor that has a cache is pruned, so an ungenerated
# vendor keeps its JSONs and is simply parsed at startup; non-vendor JSONs
# (blacklist.json) are left alone, as are the vendor directories themselves —
# thumbnails, covers and bed models still live there.
#
#   -b <dir>    build tree holding the tool
#               (default: build/arm64, build/x86_64, or build — first that exists)
#   -p <dir>    profiles directory to generate caches into
#               (default: <repo>/resources/profiles)
#   -c <cfg>    build config for multi-config generators
#               (default: the config of the tool already in the build tree, else
#               the build tree's CMAKE_BUILD_TYPE)
#   -n          skip the rebuild and run the tool already in the build tree
#   -l <level>  tool log level (default: 2)
#   --prune-source
#               allow a target that is the directory the caches were generated
#               into (resources/profiles). Pruning it deletes the checkout's own
#               preset JSONs, which is a packaging step - not something a build
#               should do to a working tree by surprise.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd -P)"
build_dir=""
profiles_dir=""
config=""
build_tool=1
log_level=2
prune_source=0

# getopts does not do long options; pull this one out first.
args=()
for arg in "$@"; do
    if [ "$arg" = "--prune-source" ]; then prune_source=1; else args+=("$arg"); fi
done
set -- ${args+"${args[@]}"}

while getopts "b:p:c:l:nh" opt; do
    case $opt in
        b) build_dir="$OPTARG" ;;
        p) profiles_dir="$OPTARG" ;;
        c) config="$OPTARG" ;;
        n) build_tool=0 ;;
        l) log_level="$OPTARG" ;;
        h) sed -n '2,${/^#/!q;p;}' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) exit 1 ;;
    esac
done
shift $((OPTIND - 1))

if [ -z "$build_dir" ]; then
    for candidate in "$repo_root/build/arm64" "$repo_root/build/x86_64" "$repo_root/build"; do
        if [ -d "$candidate" ]; then build_dir="$candidate"; break; fi
    done
fi
if [ -z "$build_dir" ] || [ ! -d "$build_dir" ]; then
    echo "ERROR: build tree not found (pass -b <build_dir>)" >&2
    exit 1
fi

# Newest match wins: multi-config trees keep one binary per config, and a stale
# one silently produces a stale cache layout.
find_tool() {
    local best="" f
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        if [ -z "$best" ] || [ "$f" -nt "$best" ]; then best="$f"; fi
    done < <(find "$build_dir" -name generate_system_cache -type f 2>/dev/null)
    printf '%s' "$best"
}

tool=$(find_tool)
if [ -z "$config" ]; then
    case "$tool" in
        */Debug/*)          config=Debug ;;
        */Release/*)        config=Release ;;
        */RelWithDebInfo/*) config=RelWithDebInfo ;;
        */MinSizeRel/*)     config=MinSizeRel ;;
        *) config=$(sed -n 's/^CMAKE_BUILD_TYPE:[A-Z]*=\(.\+\)$/\1/p' "$build_dir/CMakeCache.txt" 2>/dev/null | head -1 || true) ;;
    esac
fi

if [ "$build_tool" = 1 ]; then
    echo "Building generate_system_cache in $build_dir${config:+ ($config)}"
    build_args=(--build "$build_dir" --target generate_system_cache)
    if [ -n "$config" ]; then build_args+=(--config "$config"); fi
    if ! cmake "${build_args[@]}"; then
        echo "ERROR: could not build generate_system_cache — configure the build tree with -DORCA_TOOLS=ON:" >&2
        echo "       cmake -S \"$repo_root\" -B \"$build_dir\" -DORCA_TOOLS=ON" >&2
        exit 1
    fi
    tool=$(find_tool)
fi

if [ -z "$tool" ]; then
    echo "ERROR: generate_system_cache not found under $build_dir — build with -DORCA_TOOLS=ON" >&2
    exit 1
fi

if [ -z "$profiles_dir" ]; then profiles_dir="$repo_root/resources/profiles"; fi
if [ ! -d "$profiles_dir" ]; then
    echo "ERROR: profiles directory not found: $profiles_dir" >&2
    exit 1
fi
profiles_dir=$(cd "$profiles_dir" && pwd -P)

# Start clean so vendors that went away — and caches written by older tool
# versions — don't linger next to the freshly generated ones.
echo "Generating per-vendor preset caches in $profiles_dir"
rm -f "$profiles_dir"/*.opc "$profiles_dir"/*.cache
"$tool" --path "$profiles_dir" --log_level "$log_level"

for target in "$@"; do
    resolved=$(cd "$target" 2>/dev/null && pwd -P) || {
        echo "ERROR: profiles directory not found: $target" >&2
        exit 1
    }
    if [ "$resolved" = "$profiles_dir" ] && [ "$prune_source" -eq 0 ]; then
        echo "$resolved: skipped - this is where the caches were generated."
        echo "  Pass --prune-source to prune it; that deletes this checkout's preset JSONs."
        continue
    fi
    if [ "$resolved" != "$profiles_dir" ]; then
        cp "$profiles_dir"/*.opc "$resolved"/
    fi

    pruned=0
    shipped=0
    for cache in "$profiles_dir"/*.opc; do
        vendor=$(basename "$cache" .opc)
        shipped=$(( shipped + 1 ))
        if [ -f "$resolved/$vendor.json" ]; then
            rm -f "$resolved/$vendor.json"
            pruned=$(( pruned + 1 ))
        fi
        [ -d "$resolved/$vendor" ] || continue
        n=$(find "$resolved/$vendor" -name '*.json' | wc -l)
        find "$resolved/$vendor" -name '*.json' -delete
        find "$resolved/$vendor" -type d -empty -delete
        pruned=$(( pruned + n ))
    done
    echo "$resolved: $shipped caches, dropped $pruned preset JSONs"
done
