# Dependency weight of the Design/CAD subsystem

What the Design tab actually costs a maintainer who merges it. Written to be checkable:
every number below is reproducible with the command that produced it, and the places where
a number is still missing say so instead of guessing.

Measured on Linux x86_64, OCCT V7_6_0, in the `snaporca-deps` build image.

## Summary

| | Cost |
|---|---|
| New third-party dependencies | **none** |
| OCCT build flag | `BUILD_MODULE_ModelingAlgorithms=ON` |
| Extra OCCT toolkits *built* | 3 (TKFillet, TKOffset, TKFeat) |
| Extra OCCT toolkits *linked* | 2 (TKFillet, TKOffset) |
| Vendored code | `src/libslic3r/slvs`, 9,339 lines, 380 KiB, GPLv3 |
| Own object code | 6.79 MiB unstripped `.o` (7.13 MiB with the solver) |

OCCT is **already** an upstream dependency — Orca uses it for STEP import. The Design tab
does not add a library; it turns on one more OCCT module.

## The OCCT module flag

`deps/OCCT/OCCT.cmake` gates the module on `SLIC3R_CAD`:

```cmake
-DBUILD_MODULE_ModelingAlgorithms=${SLIC3R_CAD}   # was hard-coded OFF
```

With `SLIC3R_CAD=OFF` the deps prefix matches upstream exactly.

`ModelingAlgorithms` contains 12 toolkits, but **most were already being built**, because
`DataExchange` — the STEP path upstream already ships — depends on them. The honest delta is
only the toolkits that DataExchange's dependency closure does *not* reach:

```
ModelingAlgorithms = TKGeomAlgo TKTopAlgo TKPrim TKBO TKBool TKHLR
                     TKFillet TKOffset TKFeat TKMesh TKXMesh TKShHealing

already required by DataExchange:  TKBO TKBool TKGeomAlgo TKHLR TKMesh
                                   TKPrim TKShHealing TKTopAlgo
true delta:                        TKFeat TKFillet TKOffset TKXMesh
```

Reproduce by walking `adm/MODULES` and each toolkit's `src/<TK>/EXTERNLIB` in the OCCT
source tree.

### Sizes of the delta toolkits

Static archives in the deps prefix. These are *build artifacts*, not shipped bytes — a
static link pulls in only the objects it references:

| Toolkit | Archive | Referenced by the Design tab? |
|---|---|---|
| TKFillet | 7.40 MiB | yes — `BRepFilletAPI` |
| TKOffset | 5.38 MiB | yes — `BRepOffsetAPI`, `BRepOffset_` |
| TKFeat | 4.42 MiB | **no** |
| TKXMesh | — | not produced at all |

TKFeat is worth calling out: nothing in the Design tab references it, and it is absent from
the `TKFillet`/`TKOffset` dependency closure, so it is built for nothing. OCCT's module flag
is all-or-nothing per module, which is why it comes along. It costs build time and zero
shipped bytes on any platform that links OCCT statically.

**A correction to the record.** The comment in `deps/OCCT/OCCT.cmake` and the earlier
summary both said the delta was "TKFillet + TKOffset — 3.77 MiB, Windows only". The toolkit
list was incomplete: TKFeat is built too. The 3.77 MiB figure covers 2 of the 3 built
toolkits and has not been re-derived here — see the gap below.

## What is not measured yet

Two numbers a maintainer may reasonably ask for are **not** in this document, because
producing them honestly needs a build this machine cannot do:

1. **Windows DLL delta.** OCCT builds shared on Windows, so the shipped cost there is real
   DLL bytes rather than linker-selected objects. That needs a Windows build to size —
   tracked as the cross-platform build proof (`snaporca-gix`).
2. **Clean-build time delta.** Measuring it means building the deps prefix twice, with the
   flag ON and OFF, on the same machine. The incremental figures from day-to-day work do not
   answer the question and are not offered as if they did.

Do not quote a number for either until it has been measured.

## Vendored solver

`src/libslic3r/slvs` — the 2D sketch constraint solver extracted from SolveSpace.

- 19 files: 8 `.cpp`, 11 `.h`, plus `LICENSE`
- 9,339 lines, 380 KiB of source, 0.34 MiB of object code
- **GPLv3**, `LICENSE` preserved verbatim in the vendored directory

The fork is **AGPLv3**. GPLv3 code combines into an AGPLv3 work without difficulty: AGPLv3
§13 provides explicit compatibility in that direction. No licence question to resolve.

It is live code, not a carried corpse — `SketchSolver.cpp` is its only consumer and drives
every sketch constraint in the Design tab.

## Own code

Object sizes from the release build (unstripped, so these include debug information and
overstate the shipped contribution):

| Object | Size |
|---|---|
| DesignPanel.o | 2.22 MiB |
| McpControl.o | 1.69 MiB |
| DesignSketchTool.o | 0.88 MiB |
| CadDocument.o | 0.76 MiB |
| SketchEngine.o | 0.40 MiB |
| DesignCanvas.o | 0.37 MiB |
| GeometryEngine.o | 0.32 MiB |
| SketchSolver.o | 0.15 MiB |
| slvs (all objects) | 0.34 MiB |
| **total** | **7.13 MiB** |

For scale, the linked binary is 137.1 MiB.

## Reproducing

```bash
# toolkit membership and dependency closure
R=<occt-source>
cat $R/adm/MODULES                     # module -> toolkits
cat $R/src/<TK>/EXTERNLIB              # toolkit -> its dependencies

# archive sizes
ls -l <deps-prefix>/lib/libTK{Fillet,Offset,Feat}.a

# what the Design tab actually references
grep -rE 'BRepFilletAPI|BRepOffsetAPI|BRepOffset_|BRepFeat' src/libslic3r/

# vendored solver
wc -l src/libslic3r/slvs/*.cpp src/libslic3r/slvs/**/*.h
head -3 src/libslic3r/slvs/LICENSE
```
