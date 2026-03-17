# Mobile porting notes

## Portability constraints

Files under `src/portability/` are part of the platform portability layer and must remain free of desktop UI/graphics dependencies.

Do not include the following from `src/portability/**`:

- `#include <wx/...>`
- Desktop OpenGL headers such as `#include <GL/...>` or `#include <OpenGL/...>`

This rule is enforced at CMake configure-time by `cmake/CheckPortabilityIncludes.cmake`.
