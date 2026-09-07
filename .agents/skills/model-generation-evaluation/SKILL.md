---
name: model-generation-evaluation
description: Evaluate existing local generated OBJ artifacts or frozen Tripo batches in this OrcaSlicer project using structural, color and evidence checks. Use for model quality review or regression comparison; this workflow does not generate new provider tasks.
---

# Model generation evaluation

Evaluate a supplied artifact or frozen batch and return evidence-backed findings. Read [printing/color boundaries](../../../Docs/domain/printing-color-boundaries.md) for the distinction between physical channels, desired colors, structural checks and print qualification. Paths in commands below are relative to the Git root.

## Identify the evidence

1. Locate the Git root and applicable AGENTS. Record HEAD, dirty state, artifact path/SHA-256 and whether the artifact is raw provider output, processed output or the imported result. For comparisons, record both artifacts and the changed settings; do not compare different stages as if they were the same output.
2. Confirm units, Z-up orientation, intended print size and supplied palette/channel metadata. The OBJ analyzer assumes normalized Z-up geometry and mm-based thresholds. If those facts are missing, report the limitation; do not silently rescale or rewrite the input.
3. Read only the supplied job/manifest/report fields relevant to quality. Check their links to the artifact. Do not collect credentials, environment dumps or unrelated private images. A hash links an artifact to a report; it does not prove visual quality or acceptance.

## Run the applicable local evaluation

For one normalized OBJ, use the existing library from the repository root. The output file must be a new review destination, not an existing accepted report. Replace the two final paths:

```bash
python -c "import sys; from pathlib import Path; sys.path.insert(0, 'tools/ai'); from printable_model_quality import analyze_printable_obj, write_model_quality_report; out=Path(sys.argv[2]); assert not out.exists(), 'Choose a new report path'; out.parent.mkdir(parents=True, exist_ok=True); report=analyze_printable_obj(sys.argv[1]); write_model_quality_report(report, out); print(report['status'])" input.obj output/model-review/model-quality.json
```

Read the resulting status, errors, warnings, thresholds and availability metrics; command exit 0 alone is not a passing quality gate. If an authoritative palette is supplied, pass its exact colors as `target_palette` to `analyze_printable_obj`; do not invent colors or treat its empty default as a palette coverage assessment. Preserve default topology rejection unless the task explicitly calls for a separate repairability analysis, clearly labeled.

For a frozen Tripo batch with the expected approved manifest, validation-state files and already downloaded OBJ assets, reuse:

```bash
python tools/ai/run_tripo_quality_report.py --manifest path/to/manifest.json --tripo-root path/to/review-copy --output output/model-review/batch-quality.json
```

Read its `--help` and manifest loader if the supplied layout differs. This tool writes per-task quality/review files and can reuse an existing output; use a new output and a review copy of the required frozen inputs if originals must remain immutable. It requires Pillow, source images referenced by the manifest and the expected batch layout. Missing evidence is a limitation, not a reason to start a new generation. Do not use this batch command for an arbitrary standalone OBJ.

When validating evaluator changes or a new environment, reuse the relevant existing offline tests:

```bash
python -m unittest tools.ai.test_printable_model_quality tools.ai.test_color_intent tools.ai.test_run_tripo_quality_report -q
```

These include synthetic meshes and report fixtures. Run related palette/input-image tests only when that behavior is in scope. Model import/SmartSlicing regressions additionally use the appropriate C++ suites via [tests/AGENTS.md](../../../tests/AGENTS.md). Python results do not stand in for C++/GUI acceptance.

## Inspect and report

- Inspect available reference images and rendered views with the available image viewer, identifying which artifact/stage they depict. If no views or viewing capability exist, mark visual assessment unperformed. Do not infer identity fidelity from topology metrics.
- Report boundary/non-manifold edges, components/floating geometry, contact, thickness samples/availability, overhang risks and palette coverage where measured. Distinguish warning/review/reject from unavailable and from checks not implemented, such as exhaustive self-intersection or mechanical strength.
- Trace any color-intent handoff only as far as actual consumer code and runtime evidence establish. A sidecar manifest or six-channel DTO is not proof of a solved recipe, valid G-code or calibrated printing.
- Deliver a compact table of artifact SHA, stage, dimensions/assumptions, gate version/thresholds, measured findings and report paths. Separate structural findings, visual findings, import/slicing evidence and real-print evidence. End with actionable defects, comparison limitations and the next verification needed.

Do not regenerate, repair source assets, upload models or execute a paid/remote benchmark merely to complete an evaluation. If a requested extension needs real provider calls, first make the exact input/batch/cost scope concrete and check existing authorization; ask only for a missing scope. Existing pauses and platform-review blocks remain applicable to their specific operations. `run_paid_*`, generic quality benchmarks and remote visual scorers are not interchangeable with this local SOP.
