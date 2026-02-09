# Z Anti-Aliasing (ZAA) — Z Contouring

ZAA eliminates stair-stepping on curved and sloped top surfaces by adjusting the Z height of each extrusion point to follow the actual 3D model surface.

Instead of printing flat horizontal layers, ZAA raycasts each point of the toolpath against the original mesh and micro-adjusts its Z coordinate to match the true surface geometry. The result is visibly smoother surfaces on domes, chamfers, and shallow slopes — without post-processing.

This is a port of the ZAA implementation from [BambuStudio-ZAA](https://github.com/adob/BambuStudio-ZAA) by adob.

## Configuration

ZAA adds five settings under **Print Settings > Quality**:

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `zaa_enabled` | bool | off | Master enable/disable switch |
| `zaa_min_z` | float | 0.06 mm | Minimum Z layer height; also controls the slicing plane offset |
| `zaa_minimize_perimeter_height` | float | 35° | Reduce perimeter heights on slopes below this angle (0 = disabled) |
| `zaa_dont_alternate_fill_direction` | bool | off | Keep fill direction consistent instead of alternating per layer |
| `zaa_region_disable` | bool | off | Disable ZAA for a specific print region/material |

## How It Works

1. The slicer slices normally, then runs a **posContouring** step on each layer.
2. `ContourZ.cpp` raycasts every extrusion point vertically against the source mesh.
3. Each point's Z is adjusted to the mesh intersection, converting flat `Polyline` paths into `Polyline3` paths that carry per-point Z coordinates.
4. The G-code writer emits the adjusted Z values, so the printer follows the true surface.

## Key Implementation Details

- **Core algorithm**: `src/libslic3r/ContourZ.cpp` (~330 lines)
- **3D geometry**: `Point3`, `Line3`, `Polyline3`, `MultiPoint3` extend the existing 2D types
- **Pipeline step**: `posContouring` in `PrintObject.cpp`, runs after perimeter/infill generation
- **G-code output**: `GCode.cpp` writes per-point Z when `path.z_contoured` is set
- **Arc fitting**: Templated to work with both 2D and 3D geometry
- **ExtrusionPath change**: `polyline` field changed from `Polyline` to `Polyline3`

## Testing

1. Load a model with curved top surfaces (spheres, domes, chamfered edges)
2. Enable **Z contouring** in Print Settings > Quality
3. Slice and inspect the G-code — Z values should vary within each layer on contoured surfaces
