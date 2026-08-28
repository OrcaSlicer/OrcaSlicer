# Rig build traps

The build rig is two long-lived containers, `snaporca-gui` and `orcacad-gui`, one per fork. Each
mounts only its fork's build volume (`snaporca_buildcache` / `orcacad_buildcache`) at
`/OrcaSlicer/build`, its fork's `resources/`, and a shots directory — nothing else. They run the
binary; they do not build it. Rebuild with `scripts/CAD/build-gui.sh`.

| fork repo | project() | deps image | build volume | GUI container | binary |
|---|---|---|---|---|---|
| `snaporca` | `Snapmaker_Orca` | `snaporca-deps` | `snaporca_buildcache` | `snaporca-gui` | `snapmaker-orca` |
| `orca_cad` | `OrcaSlicer` | `orcacad-deps` | `orcacad_buildcache` | `orcacad-gui` | `orca-slicer` |

`scripts/CAD/build-gui.sh` exists alongside `scripts/CAD/build-gui-incremental.sh` for one reason: it does a
target-only `ninja` into the volume the GUI rig launches from, so a session can test a single
change without a full repackage, whereas `build-gui-incremental.sh` runs the full packaged build.
Both start a throwaway container from the deps image with the live repo mounted over the baked
tree — never build inside the GUI container (Trap 1).

Every trap below has already cost about a session to re-derive, once each. They are recorded now
so no fresh session pays them again. Symptoms, causes, and exact recovery commands follow.

---

## Trap 1 — never configure inside the GUI container

**Symptom.** After building inside the GUI container, the fork's targets no longer exist; ninja
reports an unknown target, and `orca-slicer` / `OrcaSlicer` have been replaced by
`snapmaker-orca` / `Snapmaker_Orca`.

**Cause.** The GUI image's baked `/OrcaSlicer` tree is the Jun-13 Snapmaker-derived source
(`project(Snapmaker_Orca)`, executable `snapmaker-orca`). `orcacad-deps` is layered on
`snaporca-deps`, so even on the mainline fork the baked tree is the other fork's. A `cmake .`
there reconfigures the shared build dir under the wrong project name.

**Fix.** Build only via `scripts/CAD/build-gui.sh`, which starts a throwaway container from the deps
image with the live repo mounted over the baked tree — `src`, `resources`, `cmake`, `deps_src`,
`localization`, `CMakeLists.txt`, `version.inc` — and writes into the same volume the rig
launches from.

---

## Trap 2 — stale `NLopt_DIR` in CMakeCache

**Symptom.** Configure fails with `Cannot find NLopt library 'nlopt_cxx' in '<prefix>/lib'`.

**Cause.** `cmake/modules/FindNLopt.cmake:26` is `set(NLopt_DIR $ENV{NLOPT})`. With `NLOPT`
unset that expands to `set(NLopt_DIR)` — zero arguments — which *unsets the normal variable* and
lets a leftover CACHE entry of the same name (e.g. `<prefix>/lib/cmake/nlopt`) show through the
following `if(NOT NLopt_DIR)`. The `else()` branch then searches for `nlopt_cxx` under
`${NLopt_DIR}/lib` with `NO_DEFAULT_PATH`, while the deps prefix ships plain `nlopt`.

**Fix.** From inside the build dir:

    cmake -U NLopt_DIR -U NLopt_LIBS .

Do **not** `sed` the entry out of `CMakeCache.txt` — deleting a line breaks the cache parser.

---

## Trap 3 — the image lacks `deps_src/pybind11`

**Symptom.** Configure aborts with `pybind11 headers not found in /OrcaSlicer/deps_src/pybind11.
Did you initialize submodules?` (the `FATAL_ERROR` guarding `PYBIND11_SOURCE_DIR` in the mainline
fork's root `CMakeLists.txt`, near line 948).

**Cause.** The deps image predates that requirement. Only the mainline (`orca_cad`) fork has
`deps_src/pybind11` and the requirement; snaporca has neither.

**Fix.** Mount `deps_src` over the baked tree — `scripts/CAD/build-gui.sh` does. Corollary: mounting a
snaporca tree into an `orcacad-deps` build reproduces this error exactly.

---

## Trap 4 — `OCCT_LIBS` lags one configure

**Symptom.** A wall of undefined references to `TopOpeBRepBuild` symbols. It reads as a broken
OCCT installation. It is not.

**Cause.** `src/libslic3r/CMakeLists.txt:603` does
`set(OCCT_LIBS "${OCCT_LIBS}" CACHE INTERNAL "OCCT toolkits linked by libslic3r")` at the END of
its own configure, while the consumer in the root `CMakeLists.txt` (`if (NOT OCCT_LIBS)` …
`foreach (_tk IN LISTS OCCT_LIBS)`) reads whatever is already in the cache. The first reconfigure
after the `TKFillet TKOffset` prepend (`src/libslic3r/CMakeLists.txt:599`) therefore links the
previous list and drops `TKBool`/`TKOffset`.

**Fix.** Configure twice. `scripts/CAD/build-gui.sh` runs `cmake .` twice for exactly this reason; if
you ever configure by hand, run it twice.

---

## Trap 5 — `SLIC3R_CAD=ON` in the cache, macro never defined

**Symptom.** The build succeeds and links, but the Design tab is simply absent — or it fails with
`class GLCanvas3D has no member named set_design_sketch_tool`.

**Cause.** The cache carries `SLIC3R_CAD=ON`, but the root `CMakeLists.txt` actually configured is
a stale baked copy that predates the gate and never runs `add_definitions(-DSLIC3R_CAD)` (the
gate is `if (SLIC3R_CAD)` / `add_definitions(-DSLIC3R_CAD)` in the root list — line 179/180 in
snaporca, 319/320 in orca_cad). Every `#ifdef SLIC3R_CAD` block therefore compiles out while the
option still reads ON.

**Fix.** Always mount the live `CMakeLists.txt` and `cmake/` — never inherit them from the image.
This is why `scripts/CAD/build-gui-incremental.sh`, `scripts/CAD/run-kernel-tests.sh` and `scripts/CAD/build-gui.sh`
all mount both.

---

## The binary the rig actually launches

`ninja <target>` writes `/OrcaSlicer/build/src/Release/<binary>`; only `build_linux.sh`
additionally packages to `/OrcaSlicer/build/package/bin/<binary>`. `orca_cad`'s
`scripts/CAD/start-headless-gui.sh` defaults `BIN` to `src/Release/orca-slicer`, but snaporca's defaults to
`package/bin/snapmaker-orca`. So after a target-only rebuild on snaporca, launching
`start-headless-gui.sh` with its default runs the **stale packaged** binary — the change under test is
invisible and the session hunts a phantom. Pass `BIN` explicitly:

    docker exec -e BIN=/OrcaSlicer/build/src/Release/snapmaker-orca snaporca-gui /OrcaSlicer/scripts/CAD/start-headless-gui.sh

`scripts/CAD/build-gui.sh` prints the correct line for the current fork when it finishes.

Note also that the GUI containers do **not** mount `scripts/`: `/OrcaSlicer/scripts` inside them
is the baked copy, so a local edit to `start-headless-gui.sh` has no effect until you
`docker cp scripts/CAD/start-headless-gui.sh <container>:/OrcaSlicer/scripts/`.

---

## Trap 6 — `src/Release/` resolves resources to `build/resources`, which may not exist

The binary derives `resources_dir()` from its own location, so the `src/Release/` one looks in
`/OrcaSlicer/build/resources` while the packaged one looks inside `build/package/`. Only the
packaging step creates the latter; nothing creates the former. Without it the app fails every
`Failed to add custom font ".../build/resources/fonts/…"`, logs `Health check is not running`,
and **exits 255 with nothing on stdout** — which reads exactly like a crash in whatever you just
changed. Measured 2026-08-02: an hour was nearly spent bisecting a GUI change that was fine.

`build/` is the shared cache volume, so one symlink fixes it permanently, and pointing it at the
bind-mounted repo tree means the rig also picks up new `resources/images/*.svg` without a rebuild:

    docker exec <fork>-gui ln -sfn /OrcaSlicer/resources /OrcaSlicer/build/resources

Tell the two apart before debugging: a resource failure dies in the first second with no window;
a real fault in your code gets past the version banner. Compare
`~/.config/<App>/log/<newest>.log.0` against a known-good run — 47 lines versus 340 is the tell.

## Trap 7 — a single-instance app plus a path-matched `pkill`

`start-headless-gui.sh` used to kill by `"$BIN"`, while its own `app_pid()` matched by BASENAME. Launch
with a `BIN` that differs from the running instance's path and the old process survives, keeps
the single-instance lock, and the new one exits seconds after loading fonts — then `status`
reports the *stale* pid as a healthy session. Fixed by killing on the basename; `status` now also
prints `binary : $(readlink -f /proc/<pid>/exe)`. **Read that line before trusting a screenshot.**
