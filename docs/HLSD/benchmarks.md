# OrcaBench High Level Design

## Why it exists

OrcaBench measures the slicing pipeline. A run records how long each step took, the memory and CPU
it used and how much work it produced, in a document that two builds can be compared through, so a
change that makes slicing slower shows up along with the step that got slower.

## Where it lives

`benchmarks/` builds `orcabench_core`, a static library that never links libslic3r, and
`orcabench_slicer`, an object library of what slices: the run environment and the workload kinds in
`benchmarks/kinds/`. `orca_bench` links both. `tests/orcabench` builds `orcabench_tests` on core
alone, so it builds without the slicer and runs in about a second, and `orcabench_slicer_tests` for
what slices. It is built when `ORCA_BENCHMARKS` is on, which defaults to `BUILD_TESTS` in a new
build directory and which the build scripts turn on with the tests, so their test builds compile it
and run its suites. Turning the option off removes it. `cmake/modules/OrcaSlicerRuntimeDeps.cmake`
copies the runtime libraries the executables that slice need beside them, for the test suites and
`orca_bench` alike.

## Step timestamps

`PrintStateBase::StateWithTimeStamp` holds `started_at` and `done_at`, `steady_clock` times that
`set_started()` and `set_done()` record under the print's state mutex, next to the state they
already write. `set_started()` clears `done_at`, so a span never mixes two runs. Invalidating a
step leaves both alone, which means they describe the step's last run, and that can predate the
current pass because a step only re-runs after being invalidated. A reader dates them against a
time it took before the pass.

## Platform queries

`benchmarks/core/Host.cpp` is the only file in `benchmarks/` with platform `#ifdef`s. The process
queries (resident memory, its peak, CPU time) answer 0 when the platform call fails. The machine
queries (host name, OS, CPU model, logical cores) fall back to something coarser, such as the
architecture in place of a CPU model, and to `"unknown"` only when nothing coarser exists.
`cpu_time_step()` is the step the CPU time moves in. It is 15.625 ms on Windows, where
`GetProcessTimes` charges each thread's time at the clock interrupt, and 4 ms on Linux, where
`getrusage()` brings another running thread's time up to date only at the scheduler tick. That tick
is 4 ms at the usual `CONFIG_HZ` of 250, which user space cannot read. The step is zero on macOS.

## The result model

`Result.hpp` describes a run: the suite's start time and duration, what the run asked for, the
build and the machine, and one entry per workload. A workload ran, was skipped because a fixture
was missing, or failed because it threw or the Runner refused what it reported, and the last two
have a reason. Neither counts as a pass, and the outcome counts are computed from the list
whenever asked, never stored, so no total can disagree with it.

A field is typed when every result has it, such as a span's stage and times or an iteration's
wall time and CPU time. Peak memory, the work stats and the output hash are optional, since a run
may not collect them, and are then absent, never zero. A measurement only some producers record
is stored in a `Metrics` map (name to number) on the span, the iteration or the work stats, keyed
by a constant declared beside its producer, and a missing key means it was not collected. Build
and machine identity have a `Properties` map (name to text) for the same purpose, which holds the
machine's CPU time step where it has one.

An iteration also lists the steps that started and never finished, each with its start, and the
timed steps that never started. With a span that took no time, which is a step that ran below the
clock's resolution, a stage is in one of four states that a reader can always tell apart.

Measurement identity, what the run asked for, is a map compared whole, so two results are
comparable exactly when it is equal. Build and machine identity may differ between comparable
results, since comparing compilers or machines is one of the uses.

## Policies

A policy sets how many threads slice, how many untimed warmup passes run before the timed
iterations, which stages are timed, what is collected, and whether only the workloads marked for
profile training run. Four presets cover the framework's uses.

| Preset    | Threads  | Warmup | Iterations | Collects              | Stages                |
| --------- | -------- | -----: | ---------: | --------------------- | --------------------- |
| `quick`   | hardware |      1 |          3 | wall, rss, hash, work | load, process, export |
| `precise` | 1        |      2 |         10 | wall, rss, hash, work | load, process, export |
| `verify`  | hardware |      0 |          1 | hash                  | load, process, export |
| `pgo`     | hardware |      0 |          1 | none                  | load, process, export |

`quick` is the edit loop. `precise` runs one thread, where the slicing itself takes the largest
share of the wall time and runs vary least, and its numbers are therefore never comparable with
`quick`'s. `verify` exists for the output hash. `pgo` runs each workload exactly once for profile
coverage, and fixes its warmup and iteration counts. Every preset times all three stages, since a
kind times only the stages it has: a slice times nothing under load, and a load nothing under
process or export.

`PolicyOverrides` replaces a preset's values field by field, so a run can be `precise` with four
threads. `Policy::resolve()` applies them, turns 0 threads into the hardware count, and refuses an
unknown preset, more threads than the hardware runs, a run without iterations or stages, and a
warmup or iteration count on a preset that fixes them. The work stats come from the G-code export
writes, and a hash from that G-code or from the file load reads, so a run whose stages leave export
out drops the work stats, one that also leaves load out drops the hash, and one left collecting
nothing, which is `verify` with neither, is refused.

`Policy::identity()` is the only writer of the measurement identity. It names every field of
`Policy` in a structured binding, so a new field does not compile until it is recorded or
explicitly left out. Threads are recorded as the resolved count, and an empty set as `none`. The
corpus and the affinity are recorded as `embedded,handy` and `none`, the only values they have so
far, so the results taken now stay comparable once either can vary.

## The extension seam

A workload kind is code and a catalog entry is data, so one kind runs many benchmarks. A
`CatalogEntry` names a benchmark permanently, since results are keyed by its name, and holds its
kind, its fixture, its tier, whether PGO training may run it, its tags, and the config the kind
applies over its defaults. PGO training may run a macro entry and not a micro one unless the entry
says otherwise. A name has to be printable ASCII without spaces, so a listing holds one name per
line, and building a workload refuses any other.

`Workload` is what a kind implements. `setup()` runs once and returns a reason when the workload
has to be skipped, such as a missing fixture. `prepare()` builds fresh state before each
iteration, so no iteration inherits another's, and `execute()` runs one timed iteration. An
exception from any of them fails the workload. `RunContext` tells a workload which stages to
time, and whatever a timed stage needs runs untimed in `prepare()`.

A workload reports through `Measurement`. It takes spans with their stage, `Scope` (the whole
print or one object) and times, each with its own metrics, the steps that never finished and the
timed ones that never started, then metrics of the whole iteration, and the work stats and the
output hash once each. The tests' fake writes through the same type, so nothing between a workload
and its result depends on slicing. A span that ends before it starts, a stage reported twice for
one scope in any of the three forms, a number that is not finite and a metric reported twice are
refused where they are reported, so one bad value fails only its own workload and never reaches
the document writer, which would refuse the whole result.

`WorkloadKinds` maps kind names to the factories that build a workload from its entry. A kind adds
itself from its own file through a `WorkloadKindRegistrar`, and the kinds compile into
`orcabench_slicer`, an object library, which puts every object in each executable that links it,
where a static library's linker drops registrars that nothing references. The process-wide registry
is a function-local static, so a registrar reaches it during static initialization in any order, and
tests give a registrar their own registry. An exception cannot leave a static initializer without
ending the process, so a registrar keeps a failed registration, such as a kind added twice, for
`require_registered()`, which `orca_bench` calls before anything else.

`orca_bench` reads its arguments with a parser that shares one table of flags with the usage
text, prints the catalog's workloads with `--list`, and prints the usage for `--help`, even beside
`--list`. `--list` refuses `--policy`, `--compare` and `--check-determinism`, which it would
otherwise replace without a word, and a flag given twice is refused, since only one of its values
would count. An argument it does not know exits 2 with the usage, and any other error exits 1
without it.

## The Runner

`run_suite()` in `Runner.cpp` takes the catalog entries a run selected, the resolved policy, the
registry and a `RunEnvironment`, and returns the `Result`. The environment is what a run sets up
around all its workloads, such as the thread cap, the locale, `resources_dir()`, the temporary
directory and logging. Those are libslic3r and TBB calls, so core only declares the interface and
the code that links libslic3r implements it. The Runner enters it once before the first workload
and leaves it after the last. A run ends before any workload starts when an entry name is one
`validate()` refuses or one that repeats, since results are keyed by name, or when the environment
fails to enter.

Each workload is built, set up once, then prepared and executed for every warmup and timed pass.
Under `pgo`, an entry that may not train is left out of the run, the same as one the selection
never named, and a selection with no entry that may train is refused before the run, since a run of
nothing would look like a pass. Nothing a workload throws leaves the Runner. The workload fails with a reason that
names the call and the pass, such as `execute() threw on timed pass 2: bad allocation`, and the
next one runs. An empty reason to skip fails the workload as well, since the document cannot hold
a skip without a reason.

Only `execute()` is timed, between two readings of the process the Runner takes around it, which
give its wall time and the CPU time the process used. The Runner builds each pass's `Measurement`
with the time of the first reading, and a span that starts before it, or ends after it is
reported, is refused. A workload that reuses state from an earlier pass therefore fails, and every
recorded span lies inside its iteration.

Every pass, warmups included, must reproduce the first pass's output hash and work stats, since a
result holds one of each, and a pass that differs fails the workload. The hash therefore leaves out
whatever varies between runs of the same input, such as the time the G-code header records. Every
pass must also agree on which stages finished, stayed unfinished or never ran, in whatever order it
reports them, so no stage's row mixes states. A result keeps the hash and the work stats only when
the policy collects them, and a timed pass becomes an iteration only when wall time is collected,
so `verify` records its hash and no iterations. The sampler adds each iteration's peak memory and
each span's readings.

The Runner reports as it runs through `RunEvents`: the run starting, each workload starting and
finishing, each pass finishing with the time `execute()` took, and the run finishing with the whole
result once the environment is left. A pass's event fires after the pass's last reading and before
the next pass is prepared, never while a pass is timed or sampled, so whatever listens cannot slow a
measurement. A listener that throws after a pass fails that workload, as the workload throwing
would.

## The sampler

`Sampler.cpp` reads the process every 5 ms on its own thread through each timed pass, taking its
resident memory and the CPU time it has used through the `Host` queries. The Runner starts the
thread before the pass's first reading and stops it after the last, and warmup passes, `verify`
and `pgo` never sample. The interval is recorded in the measurement identity, as the corpus and
the affinity are, and Windows rounds the wait up to its clock tick, 15.6 ms unless something has
raised the timer resolution.

An iteration's peak memory is its highest reading, the same way on every platform, so a spike
shorter than the interval can go unseen. The operating system's own high-water mark would be
exact only for an iteration that sets a new record for the process, and only Linux can reset it.
Each span gets its highest reading, and the CPU time used between the first and last readings
inside it with the time between them, so its utilization is exact where it was measured. A span
holding no reading has no memory and one holding fewer than two has no CPU, and `Measurement`
refuses these keys from a workload. A span's readings are the whole process's, so spans that
overlap, such as the same step on two objects, share them.

Windows CPU time moves in 15.6 ms steps, and Microsoft documents its precise cycle counts as not
convertible to time, so a short span there says little about CPU. A `ProcessProbe` takes each
reading, from `Host` in a run and from a script in the tests.

## Build identity

`BuildId.cpp` reports the commit and whether the working copy was dirty, read from
`git_commit_hash.h`, which `cmake/modules/GitCommitHash.cmake` rewrites at the start of every build,
one for the slicer and one for the benchmarks. Dirty means a tracked file changed. Since `orca_bench`
compiles its kinds by glob, the benchmarks' header also counts a kind git does not track yet, which
the slicer's ignores, as the slicer never compiles it. The compiler, its version,
the configuration and the flags are read from a header CMake generates per configuration. The
flags are `CMAKE_CXX_FLAGS`, the configuration's own, and the `add_compile_options` the directory
inherits, so a cache option such as `SLIC3R_ASAN` shows up in them. `BuildId.cpp` is the only file
in `benchmarks/` that includes either generated header, so a new commit recompiles one file there.

## The result document

`Document.cpp` writes a result as JSON and reads it back, and is the only file that includes
nlohmann/json. The document's `schema` is `major.minor`. A minor version adds fields, which a
reader of the same major ignores when it does not know them. A major version changes or removes
fields, and a reader refuses any major but its own. Entries in the open maps (`measurement`,
`properties`, `metrics`) and stage names are kept whatever they are.

- Durations and CPU time are nanoseconds and memory is bytes, so a document reads back exactly and
  whatever displays it does the rounding.
- An iteration's spans and unfinished steps are written as offsets from its earliest start, since
  `steady_clock` readings mean nothing outside the process, and read back onto the epoch.
- `unfinished` and `not_run` list the steps that never finished and the timed ones that never
  started, and are left out when empty.
- `output_hash` is 16 hex digits, because jq and JavaScript read every JSON number as a double.
- An optional value that is absent, such as the hash of a workload that wrote no G-code, is left
  out, so a reader never takes a zero for a measurement.
- The suite's start time is UTC to the millisecond, converted with Howard Hinnant's calendar
  algorithms, so there is no platform call.
- A workload that did not run is its name and a `skipped` or `failed` string holding the reason.

Every document written reads back. The writer refuses a skip or failure without a reason or with
iterations, a workload that ran with a reason, a workload named twice, NaN or infinity, a negative
integer and a start time outside the years 0000 to 9999, all of which the reader would reject. The
reader also refuses an integer that is fractional or too big for its field and a span that would end
past the largest time the clock can hold. Both throw `DocumentError`, and the reader converts the
parser's own exceptions to it, so no caller needs nlohmann to catch them.

## The stage table

`Summary.cpp` turns a workload's iterations into rows of stages and totals. A stage's row sums its
spans across objects within each iteration, then takes the mean, the minimum and the coefficient of
variation over the iterations that have a span of it, since one in which it never finished recorded
none of its time. Its share is its span time over summed work, the sum of the rows, both over all
the iterations. In verbose mode each scope gets its own row. The wall envelope is the mean iteration
wall time, and unaccounted is the mean time no span covers, so objects slicing side by side can take
summed work past the wall envelope but cannot make unaccounted negative. Each row carries its
stage's state, so the four stay apart.

`collapse()` folds the rows under 1% of summed work into one other row, which counts the stages that
never ran. A row whose stage started without finishing at least once never folds, even when it also
ran, since the time it hides lands in unaccounted. A row whose CV exceeds the 3% significance bar is
noisy, since a change that size there cannot be told from noise.

Each row also carries its sampled CPU and peak memory, and is marked shared when its spans
overlapped another row's, since its readings then include that work. A row's peak memory is the
lowest over the iterations of its spans' highest reading, and the workload's the lowest of its
iterations' peaks, the minimum a comparison takes of every figure. A CPU figure is left out when
the worst-case rounding of its readings exceeds a tenth of the CPU time it shows. That rounding is
the CPU time step the result's machine recorded, once per pair of readings for each of the run's
threads and for the sampler's, so CPU from a machine that recorded no step is never left out.

## The reporters

A `Reporter` turns a run into what `orca_bench` prints. `console` is for people, `json` for anything
that reads results, and `null` for a run whose output nobody reads. A reporter is fed the identities
before the first workload, each workload as it finishes and the whole result at the end, so the
console prints each workload's table as the run goes, while `json`, valid only whole, writes the
document at the end. A new reporter is a class and one entry in the table in `Reporters.cpp` that
`make_reporter()` reads.

The console marks a noisy or shared row, and one whose stage also started without finishing, with a
symbol that a legend explains. Its header names the configuration and the machine, and says
`unoptimized` for flags that turn optimization off, as OrcaSlicer's RelWithDebInfo does, or that
name no level, as a GCC or Clang Debug build's do. `json`
writes the result itself, so nothing folds in it.

Both console views can color their text, as a second channel that never carries meaning the words
lack, so a pasted log, a monochrome screenshot or a colorblind reader loses nothing. Bold vermillion
marks worse and bold blue better, never red against green, inverse marks an alarm and dim what did
not clear the bar. Color is on when standard output is a terminal, `TERM` is not `dumb` and
`NO_COLOR` is unset or empty, and `--color` overrides all three. `Host` turns on escape sequences in
a Windows console, and a terminal that hands programs a pipe, as mintty does, gets color only from
`--color always`.

`Progress` shows where a run is on stderr, apart from the report, so piping `json` never carries it.
In a log it prints a line as each workload starts. In a terminal it redraws one line after each
pass, with the time the pass took and the time the workload has left at its pace so far, and erases
it before the workload's table prints. A line wider than 80 columns shortens the workload's name,
since a line that wraps leaves rows the redraw cannot reach.

## Comparing results

`orca_bench --compare a.json b.json` reads two result documents, `a` from before a change and `b`
from after it, and `Compare.cpp` pairs their workloads by name and their stages by stage, or by
stage and scope in verbose mode. Runs compare only when their measurement identities are equal,
since a different thread count or policy changes the times with no change to the code, and
`--allow-mismatch` compares them anyway after listing each difference. Builds and machines may
differ, since telling them apart is what a comparison is for.

Every figure is the minimum over the iterations: each stage, summed work, unaccounted time, the wall
and peak memory. Interference only ever adds time, so the minimum is the steadiest estimate of what
the code costs, and the change of the mean prints beside it to show a spread that moved. A change
counts when it reaches the 3% significance bar and exceeds both runs' CV. One that reaches the bar
inside a CV is marked `~` as possible noise, and a run with one iteration or a mean of zero has no
CV, so a change there is not judged. A stage's change under a microsecond is not judged either, and
a stage that takes under a microsecond on average counts as instant, since its spans are two clock
readings in a row, which differ only by where the clock ticked.

Output comes before time. A workload whose output hash or work stats differ measures different work,
so the comparison lists every hash and stat that changed before any time, and words such a
workload's changes longer or shorter, since slower means the same work cost more. Only there does
the view also show the wall per million moves, per layer and per cm3, and the geometric mean of the
wall covers only the workloads whose output did not change and that timed a stage, since the wall of
one that timed none is overhead alone. A hash or work stats that only one run has went uncollected in
the other, which `--allow-mismatch` allows, and says nothing about the output. A stage whose state
changed, or that only one run has, is significant, and pairs fold into the other row only when small
in both runs and not judged worse or better, so a stage that grew past the noise surfaces however
small it was before. A run with one iteration judges nothing, so there a small stage folds whatever
its change.

Each column of the view holds one unit at one precision, and the view is as wide as its columns, up
to 120. A figure too wide for its column keeps a space before it and pushes the rest of its row
right. A control character in a document's text prints as a space or `?`, so a document cannot send
the terminal escape sequences of its own, and the same holds for an error message that quotes one.
`orca_bench` exits 0 after a comparison, 3 when a workload's output changed, 1 when a workload
failed in b and not in a, when it refuses the runs or when it cannot read a document, and 2 for a
bad command line.

The two runs are timed apart, one after the other, so anything that changes on the machine between
them, such as its temperature or a background job, moves the figures as a code change would, and the
comparison cannot tell the two apart. Runs made back to back on an idle machine keep that small.

## Slicing

`SlicerEnvironment` in `benchmarks/slicer/Environment.cpp` is the `RunEnvironment` of a run that
slices. It reads paths as UTF-8, as libslic3r writes them, so a temporary directory or source tree
under a name outside ASCII works on Windows, where `orca_bench` also takes its arguments in UTF-8.
It turns logging off, since a slice logs at every step and the writes would land inside
timed passes, points `resources_dir()` at the resources it is given, which `orca_bench` takes from
the source tree, and gives the run a temporary directory of its own, which leaving removes. It holds
one `tbb::global_control` at the policy's thread count for the whole run, since building and tearing
down a cap between passes would add time and variance to what is measured. The first
`Print::process()` names the pool's threads by waiting until each of them runs, which a cap below
the arena's size holds back forever, so the environment names them before it caps. Leaving puts back
every setting it changed.

`benchmarks/kinds/SliceWorkload.cpp` is the slice kind. `setup()` loads the entry's fixture once and
builds its config. `prepare()` builds a fresh `Print` for every pass and applies the model and
config to it, since invalidating a print's steps leaves its computed layers in place, so only a new
one is provably clean. A config `validate()` refuses fails the workload with its reason. `execute()`
runs `process()` and `export_gcode()` as the timed stages ask, and an untimed stage a timed one
needs runs in `prepare()` instead. Process holds every object step, `psWipeTower` and `psSkirtBrim`,
Export holds `psGCodeExport`, and loading is the load kind's stage, so a slice times nothing under
it. Every step of a timed stage is reported in one form: a span when it finished, unfinished when it
started and never finished, which `posEstimateCurledExtrusions` always does, and not run otherwise.
Nothing in the tree starts `psConflictCheck` or `posSimplifyWall`, so no stage reports them. The step names come from tables
the build checks against the step enums, so a step upstream adds stops the build until it has a
name.

The G-code goes to the run's temporary directory, or with `--dump-gcode` straight to its file
there, so keeping it adds nothing to the timed pass. Its hash is FNV-1a over the file, read a line
at a time, without the header's `; generated by` line, whose local time differs between passes and
whose version differs between builds. Its work stats count the extrusion and travel moves the G-code
processor read, the layers that hold extrusion, the bytes the hash covers and the extruded volume,
with the moves and volume of each extrusion role in the metrics. Both
are worked out inside the timed pass, where the wall envelope shows them as unaccounted time, small
against the export that wrote the file.

`benchmarks/slicer/Fixtures.cpp` resolves fixtures and builds the hermetic config. `handy:<file>` is
one of the models the app offers under `resources_dir()`'s `handy_models`, and a missing one skips
the workload with the path it looked for, while `procedural:<shape>` is built in code. A handy 3MF
is read with `LoadStrategy::LoadModel`, without which it has no objects, and `fixture_path()` names
the file a handy fixture is read from, for a kind that times the reading itself. A 3MF read also
makes a backup folder for the model. The folder is deleted right after the read, because otherwise
the model's destructor starts libslic3r's backup thread to delete it, and that thread can deadlock
the process's exit. Every fixture is centered on a 350 mm bed and set down on it. The hermetic
config is the full default print config on that bed, with `G92 E0` as the layer change G-code, which
`validate()` requires with the default relative extrusion, and object labels off, since their
comments print an object id that only the cancel-object feature sets, which otherwise differs
between passes. The entry's keys go on top, and one the slicer does not know fails the workload,
since the config's strict setter only records an unknown key. So does a number that does not read
whole, such as `2.5` for a whole number or `200mm/s` in a list, which the slicer's parser would cut
short or read as 0. The defaults are applied option by
option, so each enum list keeps the names it reads and writes, which the copies
`full_print_config()` makes lack.

Catalogs live in `benchmarks/catalog/`, and `core/Catalog.cpp` reads every `.json` file there as one
catalog, in file name order, so a new kind's catalog is found without code. A catalog is an object
holding an `entries` array, and each entry gives its name, kind, fixture, tier, whether PGO training
may run it, its tags and its config. A field the reader does not know is refused, so a misspelled
`config` cannot run a workload on defaults unnoticed, and so is a name that repeats, within a file
or across them. `--list` prints the catalog.

`slicing.json` holds five handy models, 3DBenchy, Stanford_Bunny, Voron_Design_Cube_v7,
ksr_fdmtest_v4 and OrcaCube_v2, each under four configurations, classic and arachne walls, arachne
with tree supports and classic with normal supports, the grid a hand-made before and after of the
Clipper2 migration measured, so that comparison can be run again. The models span both of the
pipeline's cost shapes, from parts like the Voron cube, where exporting the G-code takes most of the
time, to ones like the Benchy, where processing does. Three procedural shapes join them: a smoke
cube small enough for the tests, a plate of 64 pegs, which gives every layer 64 islands to wall and
travel between, and a sphere of about 130,000 triangles, a dense mesh to slice. Names follow
`slice/<fixture>/<variant>` and are permanent, since results are keyed by them.

`benchmarks/kinds/LoadWorkload.cpp` is the load kind. It times `Model::read_from_file()` as Load's
one step, `read_from_file`, reading the model, a 3MF's settings, plates and embedded presets, and
the default instances. A 3MF goes through the importer the app opens a project with, but without the
auxiliary files and the version check the app also asks `read_from_archive()` for. The plates and
presets are freed after the step, and each pass's model is freed in the next `prepare()`, where it
is not timed, after its 3MF backup folder is deleted as the fixtures do. The last pass's model is
freed with the workload, which leaves its folder to go with the run's temporary directory, since a
destructor cannot report failing to delete it. The step carries the facets,
objects and parts it read as its metrics, since the work stats describe G-code, and the hash,
`model_hash()` beside `gcode_hash()` in `slicer/Output.cpp`, covers each part's vertices, triangles
and placement and each instance's placement, so the names and settings a file carries do not count.
An entry reads its fixture's own file, which a procedural fixture does not have, or, with `format`
set to `stl` or `stl-ascii`, an STL that `setup()` writes from the fixture into the run's temporary
directory, so one mesh can be read in several formats. Any other setting is refused.

`loading.json` reads 3DBenchy as Draco, binary STL and ASCII STL, Stanford_Bunny as Draco, the two
3MF projects the app offers, OrcaBadge and OrcaSliced, and the fine sphere as a binary STL. The
handy models that read in a few milliseconds are left out, since a step that short is mostly noise.
Names follow `load/<fixture>/<format>`.

`orca_bench --policy <name>` runs the catalog under that policy, at the thread count TBB runs
without a cap, which follows the process's affinity. `--iterations`, `--warmup`, `--threads` and
`--stages` replace the preset's values, so the run's identity records what it did, and each needs
`--policy`. A count is a whole number. `--filter` takes only the workloads whose names match its
pattern, where `*` stands for any text, `--exclude` leaves out the names its pattern matches, and
`--kind` and `--tag` take the workloads of one kind or with one tag. A workload must meet all of
them, for a listing as well as a run, and a selection that takes nothing is an error naming each
flag, since a run of nothing would look like a pass. A slice timed without export writes no G-code,
so when the run still collects the hash, because it times load, the console shows `hash none` for
it. `--out` writes the result document `--compare` reads. The console prints each workload's table
as it finishes, and the progress line redraws in place when standard error is a terminal and prints
a line per workload otherwise. `--reporter` picks the console, `json` or `null` for standard output,
`--verbose`, `--collapse-below` and `--sort-by` set the console's layout, and `--quiet` drops the
progress line. `--check-determinism` is the verify preset at two passes, so a workload whose second
pass hashes differently fails. It refuses `--stages`, since a slice that leaves out export has no
G-code to hash and would pass having checked nothing. `--dump-gcode <dir>` keeps every pass's G-code
there, as `<workload>/<pass>.gcode`, for diffing. `--out` opens its file before the run, so a path
it cannot write fails at once. A run exits 1 when a workload failed, after printing every
one, and an unknown policy exits 2, as any bad command line does.

## Tests

`tests/data/orcabench/result_v1.json` is written by hand from the schema and is never regenerated
from the writer, which would make the golden test agree with whatever the writer does, and the
expected text of both console views is laid out apart from the code that prints it for the same
reason. The document tests compare text with the whitespace between tokens removed, so a change in
indentation alone does not fail them. Stripping the escape sequences from colored output must give
the plain text exactly. Every refusal above has a test that fails without it. Under ctest each test
that slices has five minutes, so one stuck waiting on TBB fails well inside the unit-test job's own
limit.
