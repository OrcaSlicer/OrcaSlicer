# Model artifact fixtures

These are synthetic offline fixtures, not provider-generated models.

The color-projection GLBs are paired with Z-up, millimetre OBJs containing their expected sRGB
vertex colors. The native test compares positions, colors and oriented faces
without depending on the importer's vertex or triangle order.

- `textured`, `transformed`: original texture and scene transform regressions.
- `outward-textured.glb`: `textured.glb` with each triangle's last two indices
  exchanged, leaving all positions, UVs, materials and texture bytes unchanged.
  The native `Model` regression uses it to check that positive winding is retained;
  it is not one of the GLB/OBJ color-projection pairs below.
- `baseline`: embedded 2 by 2 PNG, interleaved accessors and a linear material multiplier.
- `uv-rotation`: quarter-turn texture transform and offset.
- `uv-repeat`, `uv-mirrored-repeat`, `uv-clamp`: negative and above-one UV coordinates.
- `multi-material`: shared texture and geometry with two different material factors.
- `vertex-material-color`: normalized byte vertex colors multiplied by a material factor.
- `ushort-vertex-colors`: normalized unsigned-short vertex colors.
- `nested-negative-nodes`: nested rotation, translation, nonuniform negative scale and winding.
- `nested-negative-nodes-prepared`: the same scene normalized to 100 mm and placed on the bed.

The additional fixtures were generated from `tools/ai/test_glb_artifact.py`'s
tetrahedron fixture during the 2026-09-10 cross-parser verification. Expected
texture colors, color-space conversion, transforms and winding were first
checked against explicit numeric expectations; paired OBJ files were then
written by the Python GLB analysis path. The C++ regression exercises the
independent Assimp-backed native path against those results.

These fixtures cover opaque color sampling. They do not establish real provider
quality, PBR lighting, transparency or full texture fidelity after local editing.

## Embedded JPEG decoding

- `jpeg-textured.glb`: a four-vertex tetrahedron (60 by 40 by 100 mm after import)
  with a 32 by 16, 8-bit baseline JPEG. Its red, green, blue and white quadrants are
  sampled at their centers. The JPEG was encoded with Pillow at quality 100,
  subsampling 0, progressive false and optimize false; it has 667 bytes. Independent
  decoded center RGB values are `(254, 0, 0)`, `(0, 255, 1)`, `(0, 0, 254)` and
  `(255, 255, 255)`. Native assertions allow two code values of JPEG rounding.
- `jpeg-malformed.glb`: identical geometry and material, with a nine-byte JPEG
  containing an invalid APP0 segment length of 1.
- `jpeg-truncated.glb`: identical geometry and material, with the valid JPEG cut
  at 645 bytes, inside its entropy-coded scan. The SOS marker starts at byte 609
  and its header ends at byte 623, so the fixture retains the complete header and
  22 of 42 scan bytes, removing the last 20 scan bytes and the two-byte EOI marker.

The `[ModelArtifact][JPEG]` regressions call the production `load_model_artifact`
and image adapter without GUI initialization. The hidden `[.LocalArtifactProbe]`
test reads one external GLB selected by `ORCASLICER_MODEL_ARTIFACT_FIXTURE`, checks
finite geometry and valid colors, and verifies the source hash is unchanged. Run it
once per explicitly selected asset with `-s` to record loader timing and counts.
It neither generates nor edits assets and is not main-window acceptance evidence.
