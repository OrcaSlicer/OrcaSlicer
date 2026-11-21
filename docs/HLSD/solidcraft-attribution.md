# SolidCraft attribution

The Design tab's interaction model, MCP surface and oracle test corpus are modelled on
[SolidCraft](https://github.com/tangency-labs/solidcraft) (Copyright (c) 2026 SolidCraft
contributors), licensed **MIT OR Apache-2.0**. We use it under the MIT licence.

What we take, and how:

| Item | Form | Where |
|---|---|---|
| Behaviour rules (focus, picking, sketch inference, dialogs, timeline) | Re-implemented from a written specification; no source copied | `solidcraft-behaviour-spec.md`, `src/slic3r/GUI/CAD/` |
| MCP tool set, batch/rollback semantics, path scope | Re-implemented | `McpControl.cpp`, `tools/orca_cad_mcp_bridge.py` |
| Skills prose (`SKILL.md`) | Adapted text, MIT notice below | `skills/orca-cad-*` |
| Oracle recipes and expected values | Copied verbatim (data) | `tests/cad_oracle/` |
| Persistent-naming grammar, fingerprint-chained rebuild | Re-implemented on OpenCASCADE history | `src/libslic3r/CAD/` |

Not taken: SolidCraft's `truck` kernel code and the vendored `truck-shapeops` (Apache-2.0,
RICOS Co. Ltd.); anything Fusion 360 derived (their ATTRIBUTION forbids Autodesk icons,
strings, templates); the in-app agent chat; fonts (Liberation Sans / BIZ UDPGothic, OFL).
Icons in the ribbon remain FreeCAD (LGPL-2.1, `resources/images/fc_LICENSE.txt`).

The oracle's reference STLs are SolidCraft-owned measurements of SolidCraft recipes computed
with FreeCAD; FreeCAD/OpenCASCADE are not shipped by either project for this purpose.

## MIT notice

Copyright (c) 2026 SolidCraft contributors

Permission is hereby granted, free of charge, to any person obtaining a copy of this software
and associated documentation files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## Added since the first cut

| Item | Form | Where |
|---|---|---|
| Sheet-metal conventions (setback, flange height along the outer face, open corners, hem gap, K-factor unfold) | Re-implemented from the oracle's written method text; no source copied | `src/libslic3r/CAD/SheetMetal.cpp`, `tests/cad_oracle/6[7-9]-*, 7[0-6]-*` |
| Planar-arrangement regions, fillet taper-to-fit, path pattern, loft to a point | Re-implemented from the oracle cases' descriptions | `SketchEngine.cpp`, `GeometryEngine.cpp`, `CadDocument.cpp` |
