# Reference tests (the oracle)

Each directory is one case:

- `recipe.json`: the part, written in SolidCraft's recipe format (sketches in sketch-local
  coordinates, features with their parameters, faces and edges picked by a world-space point).
  `cargo xtask oracle` replays it through SolidCraft's command engine.
- `expected.json`: what the part must measure, and where those numbers come from.
- `reference.stl` (optional): a fine triangulation of the expected part, used to check the
  shape itself (surface distance), not only its totals.

Run every case with `cargo xtask oracle`; it writes `docs/oracle.md`.

## expected.json

```json
{
  "source": "analytic",
  "method": "box 40 x 30 x 20: V = abc, A = 2(ab + bc + ca)",
  "body_count": 1,
  "volume_mm3": 24000.0,
  "area_mm2": 5200.0,
  "center_of_mass": [20.0, 15.0, 10.0],
  "bbox": { "min": [0.0, 0.0, 0.0], "max": [40.0, 30.0, 20.0] },
  "faces": 6
}
```

| Field | Meaning |
|---|---|
| `source` | `"analytic"` (closed-form geometry, worked out by hand; `method` gives the formulas) or `"freecad <version>"` (built from the same recipe by `tools/freecad-oracle/`). |
| `method` | How the numbers were obtained: formulas for analytic cases, the construction for FreeCAD ones. |
| `body_count` | Number of solid bodies. |
| `volume_mm3`, `area_mm2` | Totals over all bodies. |
| `center_of_mass` | Volume-weighted centroid of all bodies (uniform density). |
| `bbox` | Axis-aligned bounding box of all bodies. |
| `faces` | Optional. Only where the geometry fixes the count: a face is a maximal smooth patch of one surface (a full cylinder side is one face; coplanar neighbours are one face). |
| `compare` | Optional. Restricts the comparison to these keys (for example `["volume_mm3", "area_mm2"]` where kernels legitimately build different corner patches); `note` says why. |
| `tolerance` | Optional overrides: `rel` (volume, area; default 1e-3), `abs_mm` (centre of mass and bbox, default 1e-3 × the bbox diagonal), `surface_mm` (STL surface distance). |
| `note` | Optional remarks. |

No value here was measured in, or exported from, any commercial CAD product.
