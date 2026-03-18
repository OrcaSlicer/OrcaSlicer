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
