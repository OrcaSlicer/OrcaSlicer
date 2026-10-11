# orca_bench

`orca_bench` times OrcaSlicer's slicing pipeline on a fixed set of models and settings. It runs each
workload several times and prints, for every stage of the pipeline, how long it took, how much it
varied and how much CPU and memory it used. It can also save a run to a file and compare two saved
runs, so you can see what a change did. The design is in
[docs/HLSD/benchmarks.md](../docs/HLSD/benchmarks.md).

## Building

The benchmarks build with the unit tests, or alone:

| Platform | Command |
|---|---|
| Windows | `build_win.bat -s --bench` (or `--tests`) |
| Linux | `./build_linux.sh -s -t` |
| macOS | `./build_release_macos.sh -B` (or `-T`) |

The program is `benchmarks/<config>/orca_bench` in the build directory, for example
`build/benchmarks/Release/orca_bench.exe` on Windows. Build Release for timing. The header of every
run says when the build has optimization off.

## Running

```sh
orca_bench --list                                  # every workload in the catalog
orca_bench --policy quick                          # time all of them
orca_bench --policy quick --filter "slice/3dbenchy/*"
orca_bench --policy quick --kind load --tag handy  # selection flags combine
```

Each flag can be given once, so `--filter` and `--exclude` each take one pattern.

A policy decides how a run measures:

| Policy | Threads | Warmup | Timed passes | Collects |
|---|---|---|---|---|
| `quick` | all | 1 | 3 | wall time, memory, output hash, work counts |
| `precise` | 1 | 2 | 10 | the same, on one thread for steadier numbers |
| `verify` | all | 0 | 1 | only the output hash |
| `pgo` | all | 0 | 1 | nothing; it exercises the code for profile-guided builds |

`--iterations`, `--warmup`, `--threads` and `--stages` change the policy's values for one run.
`--stages` chooses which of `load`, `process` and `export` are timed. A stage that a timed stage
needs still runs, untimed. A stage that nothing needs does not run, so `--stages process` writes
no G-code. When a run collects the hash but a slice writes no G-code, the slice shows `hash none`.

`orca_bench --help` lists every flag.

## Comparing two builds

Save a run before your change and one after it, then compare them:

```sh
orca_bench --policy quick --out before.json
# make the change and rebuild
orca_bench --policy quick --out after.json
orca_bench --compare before.json after.json
```

The comparison lists any workload whose output changed first, then the wall times, then a table
per workload. Every figure is the fastest pass. A change counts when it is at least 3% and larger
than both runs' variation between passes. A change of 3% or more inside that variation is marked
`~` as possible noise. Two results compare only when they were measured the same way, with the same
policy, threads, passes and stages. The builds and machines may differ. `--allow-mismatch` compares
them anyway and lists what differs.

`--verbose` gives each object its own rows. `--collapse-below <percent>` folds the stages under
that share of the time into one row, which is 1% unless set. `--sort-by start` orders the stages
as the pipeline runs them instead of by time. These three flags work for a run and for a
comparison.

## Checking that output does not change between passes

```sh
orca_bench --check-determinism --tag procedural
```

This runs every workload twice under the `verify` policy and fails any workload whose second
pass wrote different G-code from its first. With `--dump-gcode <dir>`, each pass's G-code stays in
`<dir>/<workload>/<pass>.gcode`, so you can diff the two files. Passes are numbered from 1, warmup
passes first. `--dump-gcode` also works with `--policy`. `--check-determinism` cannot take
`--stages`, since a slice that does not export has no G-code to compare.

## Output and exit status

The console prints a table per workload as it finishes, and a progress line on standard error.
`--quiet` drops the progress line. `--reporter json` prints the result document instead of the
tables, and `--reporter null` prints nothing. `--out <file>` writes the result document in any
case. `--color` decides whether the tables use color. By default they do in a terminal, unless
`NO_COLOR` is set or `TERM` is `dumb`.

| Exit status | Meaning |
|---|---|
| 0 | done |
| 1 | an error, or a workload failed, or for `--compare` failed in b and not in a |
| 2 | a bad command line |
| 3 | `--compare` found a workload whose output changed |

## Adding a workload

Workloads are data. Each `.json` file in `benchmarks/catalog/` holds an `entries` array, and
`orca_bench` reads every file there. An entry has a `name`, a `kind`, a `fixture` and, when it
needs them, `tags` and a `config`:

```json
{
  "name": "slice/3dbenchy/arachne",
  "kind": "slice",
  "fixture": "handy:3DBenchy.drc",
  "tags": ["handy"],
  "config": {"wall_generator": "arachne"}
}
```

Results are matched by name, so a name must not change after results exist for it. A fixture is
`handy:<file>`, one of the models in `resources/handy_models`, or `procedural:<shape>`, a shape built
in code. The `slice` kind takes print settings in `config`. The `load` kind takes only `format`,
which is `stl` or `stl-ascii`. A new kind is a new file in `benchmarks/kinds/` that registers itself.
`docs/HLSD/benchmarks.md` describes how.

## A pre-push hook

Copy this to `.git/hooks/pre-push` and make it executable. It refuses a push when a procedural
workload's G-code changes between passes, which takes a few seconds. Change the path to match
your build directory.

```sh
#!/bin/sh
exec build/benchmarks/Release/orca_bench --check-determinism --tag procedural --quiet
```

Git for Windows runs the same script. Use the `.exe` path there.
