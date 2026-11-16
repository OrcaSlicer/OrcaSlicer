`two_color_jpeg.glb` is a generated, closed tetrahedron with four faces, UVs, and
an embedded 8×8 red/blue JPEG base-color texture. Its coordinates span 20 mm.

The core tests load all three fixtures through `Model::read_from_file`, convert
textures or MTL colors to painting with boundary smoothing, and match red and blue
to filaments. They run in CI without a display. A separate layout check compares
the smoothing search-tree type seen by a core consumer with the same type compiled
inside `libslic3r_cgal`. The mismatch is checked directly because undefined behavior
can be masked by which inline template implementations the linker selects.

`two_color_sphere/test_two_color_sphere.obj` and its matching MTL are the original
red/blue material-color sphere supplied by RF47 in
[the PR discussion](https://github.com/OrcaSlicer/OrcaSlicer/pull/16607#issuecomment-6244475095).
They contain solid `Kd` colors and no texture images.

`test_two_color_sphere_textured.obj` is a derived version of that sphere. Its MTL
uses white `Kd` values and references `textures/two_color.jpg` by a relative path;
the two face groups sample different halves of that image through their UVs.
The JPEG is extracted unchanged from `two_color_jpeg.glb`. This case must retain
both colors from the external texture, rather than fall back to MTL colors.
