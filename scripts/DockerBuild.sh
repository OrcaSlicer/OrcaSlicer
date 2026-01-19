#!/bin/bash
SCRIPT_DIR=$(cd -P -- "$(dirname -- "$0")" && printf '%s\n' "$(pwd -P)")
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

set -x
# Wishlist hint:  For developers, creating a Docker Compose 
# setup with persistent volumes for the build & deps directories
# would speed up recompile times significantly.  For end users,
# the simplicity of a single Docker image and a one-time compilation
# seems better.
docker build -t orcaslicer \
  --build-arg USER="$USER" \
  --build-arg UID="$(id -u)" \
  --build-arg GID="$(id -g)" \
  --build-arg NCORES="$NCORES" \
  -f "$SCRIPT_DIR/Dockerfile" \
  "$PROJECT_ROOT"
