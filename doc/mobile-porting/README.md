# Mobile Porting Docs

This folder contains planning artifacts for porting OrcaSlicer to iOS in a way that enables later Android support.

- `reference-map.md` — static map of where key modules and coupling points live.
- `ios-android-porting-plan.md` — architecture strategy and phased migration.
- `file-edit-plan.md` — actionable file-level edit list and scope.
- `implementation-status.md` — tracks scaffolding already landed in code.
# Mobile porting notes

## Portability constraints

Files under `src/portability/` are part of the platform portability layer and must remain free of desktop UI/graphics dependencies.

Do not include the following from `src/portability/**`:

- `#include <wx/...>`
- Desktop OpenGL headers such as `#include <GL/...>` or `#include <OpenGL/...>`

This rule is enforced at CMake configure-time by `cmake/CheckPortabilityIncludes.cmake`.
