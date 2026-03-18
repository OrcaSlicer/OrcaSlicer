# Mobile Porting Docs

Last updated: 2026-03-18

This folder contains planning artifacts for porting OrcaSlicer to iOS in a way that enables later Android support.

- `reference-map.md` — static map of where key modules and coupling points live.
- `ios-android-porting-plan.md` — architecture strategy and phased migration.
- `file-edit-plan.md` — actionable file-level edit list and scope.
- `implementation-status.md` — source-of-truth status: landed pieces, remaining stubs, and pre-UI gates.

## Mobile porting notes

## Portability constraints

Files under `src/portability/` are part of the platform portability layer and must remain free of desktop UI/graphics dependencies.

Do not include the following from `src/portability/**`:

- `#include <wx/...>`
- Desktop OpenGL headers such as `#include <GL/...>` or `#include <OpenGL/...>`

This rule is enforced at CMake configure-time by `cmake/CheckPortabilityIncludes.cmake`.

## Current implementation focus: iOS-first

The active implementation target is iOS portability modules under `src/portability/platform/ios/` and `src/portability/render/ios/`. Android modules should start only after iOS adapters are integrated end-to-end.


## Feature parity matrix

Status legend:
- ✅ Implemented and workflow-validated
- 🟡 Shell/partial (present but not workflow-complete)
- ⚪ Not started

| Capability | Desktop (current) | iOS shell (current) | Android | Notes |
|---|---|---|---|---|
| Project import/export | ✅ | 🟡 | ⚪ | iOS shell routes exist, but end-to-end import/export workflow validation is still pending. |
| Slicing | ✅ | 🟡 | ⚪ | Shared slicing engine is portable; mobile-triggered slice job flow is not yet closed end-to-end. |
| Viewport interaction | ✅ | 🟡 | ⚪ | Root viewport shell exists; interaction parity (selection/gizmos/deep controls) is not yet complete. |
| Preview rendering | ✅ | 🟡 | ⚪ | iOS backend currently validates launch-time viewport rendering but not full renderer parity. |
| Printer connectivity | ✅ | ⚪ | ⚪ | Mobile adapter path exists, but production printer connectivity workflow is not yet implemented. |
| Cloud login | ✅ | ⚪ | ⚪ | Credential storage scaffolding exists on iOS, but full cloud auth/user flow remains pending. |

Important: iOS screenshot CI passing confirms scene bring-up and deterministic capture only; it must not be interpreted as end-to-end feature parity.
