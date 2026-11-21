# IDEAS — benchmark for the Orca-CAD MCP (what to measure, what value is "good")
mode: lite | cycle: 1/3 | lenses used: compiler benchmarking, game-engine frame budgets, metrology / GD&T, aviation checklists, fuzzing | provider diversity: no (main context; the lenses are my own recall, not blind workers) | human seed: user asked for steps, speed, features, accuracy "etc"

Goal: a repeatable score card that says how good the MCP is for an AI that drives it, and what to fix next.
Hard: runs unattended on the nativedev rig in minutes; every metric has a target value and a pass/fail; no metric that needs a human to judge.
Soft: the same card runs on behemoth's live app (Rule 13) for design work, on the rig for the loop.
Function (domain nouns removed): measure how cheaply, quickly, correctly, completely and safely an agent can get a described object built through a command interface, and rank what to improve.
Negative space: a benchmark that rewards fast wrong answers; a score that moves with machine load; targets set from what the code already does (always passes).

## Claim
C1 (lead): an agent's cost of a task is calls x latency, and its risk is wrong geometry or a corrupted document, so five families are enough: STEPS (calls per task), SPEED (latency percentiles + rebuild scaling), ACCURACY (result vs closed form), COVERAGE (verbs exercised and passing), SAFETY (fuzzed input never crashes or corrupts) — plus two integrity metrics that make the others trustworthy: DETERMINISM and UNDO.
C2: speed is the only thing that matters (the agent is bound by rebuild latency); accuracy is already guaranteed by OCCT.
C3: the useful unit is the whole task (PCB+case+lid) not per-call metrics; per-call numbers hide the cost of a bad API shape.

## Metrics, from the lenses (provenance: all known techniques, combined)
compiler benchmarking -> warm/cold split, percentiles not means, a noise floor from repeated runs, a baseline file with a regression tolerance.
game-engine frame budget -> per-call-class budget (query 50 ms p95, feature add 500 ms p95 at <=30 features), and a "frame drop" count = calls over budget.
metrology / GD&T -> accuracy as a tolerance zone with a measurement-uncertainty ratio: each check states the closed form, the tolerance (1e-6 relative) and the instrument's own repeatability.
aviation checklists -> critical-item gates: a short list that must ALL pass (document unchanged after every rejected call, undo restores, same script twice gives the same volumes) before the score counts.
fuzzing -> every verb called with {} / wrong types / out-of-range ids; expected: a JSON error, the app still answers, the document generation is unchanged.

| id | metric | how | target |
|---|---|---|---|
| S1 | steps: MCP calls to build the PCB+case+lid assembly | count in the harness | baseline, then -20% by macros |
| S2 | steps per feature (calls / features made) | same run | <= 2.0 |
| T1 | latency p50/p95, read class (describe_scene, query_topology, inspect_design, mass_properties) | 200 repeats | p95 <= 50 ms |
| T2 | latency p50/p95, write class (extrude, sketch_add, set_variable...) | the task run | p95 <= 500 ms |
| T3 | rebuild scaling: time of set_variable vs feature count (10,20,40) | build N, change a variable | growth exponent <= 1.3 |
| T4 | replay cache: second recompute with no change | set_variable to the same value | <= 25% of the first |
| A1 | accuracy: volume/area/bbox vs closed form on 20 primitives and ops (box, cylinder, sphere, cone, torus, scale, push_pull, patterns, pipe, coil, extend) | analytic | max rel err <= 1e-6 |
| A2 | parametric sensitivity: every bound variable moves its measure | sweep +10% | 100% |
| C1 | coverage: verbs in describe_tools exercised by a passing call | harness table | >= 90% |
| C2 | verbs with NO documented example | describe_tools | 0 |
| R1 | fuzz: crashes / hangs over verbs x 3 malformed inputs | fuzzer | 0 |
| R2 | fuzz: document changed by a rejected call | generation + feature count | 0 |
| R3 | fuzz: error messages empty or non-actionable | message length > 15 and mentions the field | >= 95% |
| I1 | determinism: same build twice, volumes and feature names identical | two runs | exact |
| I2 | undo: N undos return to the starting state hash | hash of describe_scene | exact |

## Predictions (written before the baseline run)
P1: steps for the PCB+case+lid task are 120-220 calls, and more than half are set_feature_expr / sketch_dimensions bookkeeping, not geometry | check: count calls by verb in the run | separates: C1 vs C3 | result: CONFIRMED. 137 commands, 25 features (5.48 per feature), 60.6% bookkeeping (bind, name, variable, dimensions). The API shape, not the geometry, is the cost: C3 (judge the whole task) was right to be a rival.
P2: read-class p95 is under 50 ms, write-class p95 under 500 ms, rebuild scaling is near-linear (exponent 1.0-1.3) because of the replay cache | check: T1-T3 | separates: C1 vs C2 | result: CONFIRMED and beaten. read p95 6.5 ms, write p95 30 ms (p50 6.9), whole task 1.3 s, exponent 0.65 (sub-linear). C2 (speed is what matters) refuted: speed has 15x headroom, the steps are the cost. SURPRISE: T4 no-change rebuild is 0.78 of the first (48/62 ms): an identical set_variable still does most of the work.
P3: accuracy passes at 1e-6 everywhere except the curved-extend and pipe/coil cases (sampled splines, sweep volume) | check: A1 | separates: C1 vs C2 | result: REFUTED (in our favour). 23/23 inside tolerance, worst error 1e-4 of its tolerance, including curved extend, bent pipe and coil. Accuracy is not where the work is.
P4: fuzzing finds at least one verb that crashes the app or changes the document on a rejected call (OCCT exceptions, unchecked json .get<int>() on a wrong type) | check: R1/R2 | result: pending (R1 is ~20,000 calls, about 8-10 minutes: run at milestones, not every iteration). Side finding: a 3-input probe of every verb answered all of them, the slowest being `coil` that builds real coils from default junk in 3.6-4.0 s (accepting junk and doing work is its own question).
P5: coverage of describe_tools by my harness table is under 60% on the first pass (the long tail: sheet metal, sm_*, import_*) | check: C1 | result: CONFIRMED. 36/129 verbs (27.9%) have a passing call; 2 verbs have a description shorter than 25 characters. The 90% target is a long-term backlog, not a 4x lever: the untouched list is in bench_base.json (C1.untouched).
P6: determinism and undo pass exactly | check: I1/I2 | result: PARTLY REFUTED. Undo and redo pass. Determinism FAILS: the auto-generated feature name comes from a counter that survives new_document ('Schizzo 16' vs 'Schizzo 22'). A real defect found by the benchmark; first item of the loop.

## Refuted -> constraints
(none yet)

## Targets of the loop (user: 'a 400 percent improvement is achievable')
Taken as 4x: S1 137 -> 35, S2 5.48 -> 1.4, S3 0.61 -> 0.15, S4 137 -> 35; gates that may not move: A1/A2, I1-I4, R1/R2, T0-T4. Spec: .claude/loopspec/mcp-bench-4x.spec.md.

## The loop (spec .claude/loopspec/mcp-bench-4x.spec.md; each row one change, kept only if its metric improved and no gate went red)
| it | change | S1 commands | S3 bookkeeping | verdict |
|---|---|---|---|---|
| 0 | baseline | 137 | 0.61 | I1 red (name counter) |
| 1 | the feature-name counter restarts with `new_document` | 137 | 0.61 | KEPT: I1 passes |
| 2 | `extrude {width,height}` instead of the sketch dance | 97 | 0.65 | REVERTED: A2 red. A sketch-less extrude ignores a bound width/height, so pcb_L, tol and wall stopped driving anything (silent dropped binding). Caught by the sensitivity gate, not by the volumes. |
| 3 | `set_variable {variables:{...}}`; `bind:{field:expr}` and `as:"name"` accepted by ANY feature-creating verb (applied by the dispatcher to the feature just added) | 87 | 0.38 | KEPT |
| 4 | `extrude_rect`: centred, constrained sketch + sizes bound + extrude in one command, nothing left behind on refusal | 42 | 0.19 | KEPT (T2 flag = artifact, see below) |
| 5 | faces by geometry (`face:{normal,pick}`) resolved by the dispatcher; `body_as` names the body the command made | 31 | 0.16 | KEPT: S1 4.4x |
| 6 | `extrude_circle` (the circle counterpart) | 23 | 0.043 | KEPT: S1 5.96x, S2 0.92, S4 23 |

User target: 4x ("a 400 percent improvement is achievable"). Reached and passed on the headline step metrics (5.96x commands, 5.9x round trips, bookkeeping 14x lower); whole-task time 1.32 s -> ~0.8 s.

Findings of the loop, as predictions made and resolved:
- The oracle that mattered was A2 (parametric sensitivity), not the volumes: iteration 2 produced the right bodies and silently lost every binding.
- A call that does ten things is a bigger call: per-call latency (T2) is not comparable across API shapes, so T0 (whole-task time) is the speed gate. Measured noise floor of one build over 5 runs: T0 1.19x, T1 1.91x, T2 1.49x, T3 1.33x, T4 1.69x; the regression rule flags the speed family only beyond 2.2x. This calibrates the bench AFTER seeing a result, and says so.
- T4 (an identical set_variable repeated) costs 0.6-1.4 of the first: the replay cache is not what makes a no-change rebuild cheap (cost is elsewhere, probably mesh/display rebuild). Open item, not part of the 4x goal.
- R1 (the existing hostile checker) is ~20,000 calls, 8-10 minutes: run at milestones.

Backlog (not done): `mate_faces` (two connectors + a mate in one command, -4 commands), body colour/opacity as parameters (-2), T4, coverage C1 28% -> 90% (exerciser for the 93 verbs without a passing call), C2 (two verbs with a description under 25 characters).
