#!/bin/bash

# update_catch2.sh [refspec]

# `./update_catch2 $(cat catch2/COMMIT)` should be a no-op

# Yes, this is kinda reinventing submodule or subtree or FetchContent
# But AFAICT FetchContent is not otherwise used in this repo
# submodules are annoying
# subtree is overkill
# So here you go, if you want to use it.

set -ex

SCRIPTDIR="$(realpath "$(dirname "${BASH_SOURCE[0]}")")"


TDIR="$(mktemp -d)"
trap 'rm -fR "${TDIR}"' EXIT
cd "${TDIR}"
git init -b dontcare
git fetch --depth=1 https://github.com/catchorg/Catch2.git "$1"
git checkout FETCH_HEAD
COMMIT="$(git rev-parse HEAD)"

cd "$SCRIPTDIR"
git rm -f -r --quiet -- catch2
rm -fR catch2 || true
mkdir catch2
mv "${TDIR}/"CMake* catch2
sed -i 's#set_property(GLOBAL PROPERTY USE_FOLDERS ON)##g' catch2/CMakeLists.txt
mv "${TDIR}/src" catch2
mv "${TDIR}/extras" catch2
mv "${TDIR}/LICENSE.txt" catch2
echo "${COMMIT}" > catch2/COMMIT
git add catch2




