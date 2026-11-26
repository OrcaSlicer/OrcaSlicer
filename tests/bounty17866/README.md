# Interference travel CLI validation fixtures

This fork-only harness validates generated G-code for the public interference
travel preference patch in OrcaSlicer PR 17868, originally at
`6ad34c04934562f86a2b05762e35d435b974fab0`. The preference implementation
remains its original authors' work. These fixtures do not claim a new feature,
a bounty award, a support defect, or a regression introduced by that PR.

The runner requires an existing executable. It does not download, install,
build, connect to a printer, transmit G-code, or post to GitHub. The separate
fork workflow may build and run it on a GitHub-hosted runner. No application
matrix results are implied by the parser checks or by this README.

## Harness self-checks

From the repository root, using Python's standard library:

```sh
python3 -m unittest discover -s tests/bounty17866 -p 'test_*.py' -v
```

These checks parse synthetic text and construct synthetic fixtures. They do
not invoke OrcaSlicer and must not be reported as application test results.

## Actual application run

On Linux, after building the application in the remote runner:

```sh
python3 tests/bounty17866/run_matrix.py \
  --binary "$ORCA_BINARY" \
  --source "$GITHUB_WORKSPACE" \
  --output "$RUNNER_TEMP/bounty-17866/cli" \
  --timeout 300
```

Use an executable such as `build/src/Release/orca-slicer`. GUI-enabled CLI
builds may need this command inside `xvfb-run -a`; the child processes preserve
`DISPLAY`, `XAUTHORITY` and `LD_LIBRARY_PATH` when the workflow supplies them.
`LIBGL_ALWAYS_SOFTWARE` and `GALLIUM_DRIVER` are also allowlisted for software
rendering. Other unrelated environment values and credentials are omitted.

The same command supports an existing macOS executable when separately
authorized. Linux normal lookup uses an isolated
`XDG_CONFIG_HOME/OrcaSlicer/OrcaSlicer.conf`. macOS uses a child-only
`CFFIXED_USER_HOME` and an isolated
`Library/Application Support/OrcaSlicer/OrcaSlicer.conf`. The real `HOME`
value is preserved; the harness does not open real user configuration files.
A portable `data_dir` beside the executable (beside the `.app` on macOS) is
rejected because it overrides the default lookup.

The output directory must be new; an existing one is rejected. Each slice has
a timeout between 1 and 1,800 seconds, default 300. The runner terminates and
reaps its child process group on timeout or interruption, removes each case's
temporary directory, and retains the small synthetic inputs and evidence in
the specified output directory. It does not remove earlier evidence.

## Twelve configuration cases

The preference is `global_disable_interference_travel`, written into the
`app` category of `OrcaSlicer.conf`. True must yield effective
`reduce_infill_retraction = 0`; false or unset must yield `1` for these inputs.

| Cases | Configuration lookup | Preference sequence or conflict |
| --- | --- | --- |
| `explicit-unset`, `explicit-off`, `explicit-on`, `explicit-off-again` | `--datadir` | Unset, false, true, false using the same isolated directory |
| `default-unset`, `default-off`, `default-on`, `default-off-again` | Normal platform lookup | Unset, false, true, false using the same isolated directory |
| `explicit-off-beats-default-on` | `--datadir` | Explicit false overrides default true |
| `explicit-on-beats-default-off` | `--datadir` | Explicit true overrides default false |
| `import-project-on`, `import-project-off` | `--datadir` and a reimported 3MF | True and false override a project's stored option of `1` |

Each case must exit successfully and generate fresh, loose G-code with the
expected effective option in its configuration block. The reimport input is
the exported project from `explicit-off`; its
`Metadata/project_settings.config` is checked for the stored value `1`
before either reimport slice runs. Cached G-code inside a 3MF is not counted
as fresh output. If the prerequisite export fails, the summary records that
failure and the two dependent application cases cannot run.

The infill fixture is a closed 40 by 40 by 6 mm cube with 15% rectilinear
infill and no top or bottom shells. Infill anchors are limited to produce
separate roads. Presets explicitly request 0.8 mm retraction, a 1 mm minimum
travel, 0.4 mm Normal Lift Z-hop, no wiping and relative extrusion. All presets
are standalone synthetic JSON with no profile inheritance, printer addresses,
account data or keys. Geometry and preset construction reuse the approach in
`tests/cli/test_cli_strict.sh`.

## Actual motion evidence

The parser records a candidate only when a non-extruding XY travel of at least
1 mm lies between two extrusion moves with the same infill/support role and
the same extrusion Z height. Each record contains the original movement line
numbers and commands, start/end coordinates, XY length, E withdrawal and the
Z excursion that returns to the extrusion height. E-only priming is not
treated as deposition. Absolute and relative E/XYZ modes and `G92 E` resets
are handled by the parser. These fixtures use ordinary E retraction; firmware
retraction and arbitrary custom-machine dialects are outside its scope.

The off/on infill pair is matched by role and start/end XYZ coordinates;
repeated occurrences are paired individually. Matched E/Z command sequences
are saved, rather than inferring behavior from total retraction or positive-Z
counts. The application gate requires all twelve configuration cases to pass
and at least one matched infill candidate with added retraction and at least
one with an added returned Z-hop when the global preference is true. A missing
match fails the gate; it does not silently become proof of branch coverage.

These observations demonstrate generated motion for a bounded synthetic
model. G-code role tags and endpoints do not establish containment inside an
internal region or support island. They do not establish physical print
quality, exact internal branch execution, GUI persistence, or the behavior of
an upgraded installation.

## Separate support diagnostics

Four additional slices cover normal and tree support with the preference
false and true. They use a 40 by 40 by 14 mm capital with an 8 mm stem and
an overhanging cap, following the existing CLI strict-test model. Normal
support requests a rectilinear grid; tree support requests Tree Slim with a
lightning base. Support extrusion and matched same-role travel coverage are
reported separately for each type, including an explicit `coverage_observed`
value. Absence of a candidate remains a coverage gap.

The existing `GCode::needs_retraction()` support-island and internal-tree
early returns precede the `reduce_infill_retraction` check. Travels contained
by those support regions can therefore skip retraction in either preference
state. That behavior predates this PR. The support output is diagnostic and
does not assert a new bounty defect or a PR regression. It is excluded from
the twelve-case configuration/infill gate.

## Evidence and interpretation

`summary.json` reports the source checkout SHA, executable hash, fixture hashes,
all configuration outcomes, infill comparison, support outcomes and limits.
The checkout SHA and executable hash are separate evidence; the retained CI
build log establishes which source produced the executable.

Each case retains `command.json`, `console.log`, the harness `result.json`,
`travel-sequences.json`, fresh G-code and its exported 3MF when produced. If
the application writes its own `result.json`, it is preserved as
`cli-result.json` before the harness report is written. Top-level
`infill-matched-travels.json`, `normal-support-matched-travels.json` and
`tree-support-matched-travels.json` retain actual paired E/Z sequences. Reports
contain paths on the machine where the run happened, with no hard-coded Mac
output destination. CI can upload only JSON/log/XML evidence when retaining
application binaries or generated printer files is unnecessary.
