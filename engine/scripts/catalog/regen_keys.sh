#!/usr/bin/env bash
#
# regen_keys.sh — regenerate engine/include/orca/ConfigKeys.hpp from
# libslic3r/PrintConfig.cpp's ConfigDef table.
#
# Run this whenever libslic3r gains/removes/renames a config key. The header
# is fully derived — there's no hand-curated content in it.

set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$REPO_ROOT"

CATALOG_DIR="engine/scripts/catalog"
mkdir -p "$CATALOG_DIR"

echo "regen_keys: extracting ConfigDef from PrintConfig.cpp"
grep -nE 'this->add\s*\(\s*"[a-z_0-9]+"\s*,\s*co[A-Za-z]+' src/libslic3r/PrintConfig.cpp \
  | sed -E 's|^([0-9]+):.*this->add\(\s*"([a-z_0-9]+)"\s*,\s*(co[A-Za-z]+).*$|\1\t\2\t\3|' \
  > "$CATALOG_DIR/config_keys.raw.tsv"

count=$(wc -l < "$CATALOG_DIR/config_keys.raw.tsv")
echo "regen_keys: $count ConfigDef entries"

echo "regen_keys: generating engine/include/orca/ConfigKeys.hpp"
python3 "$CATALOG_DIR/gen_keys_header.py" \
  "$CATALOG_DIR/config_keys.raw.tsv" \
  "engine/include/orca/ConfigKeys.hpp"

echo "regen_keys: done"
