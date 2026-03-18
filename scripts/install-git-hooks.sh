#!/usr/bin/env bash
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "${repo_root}"

chmod +x scripts/precommit-validate.sh
chmod +x .githooks/pre-commit

git config core.hooksPath .githooks

echo "Installed git hooks path: $(git config --get core.hooksPath)"
