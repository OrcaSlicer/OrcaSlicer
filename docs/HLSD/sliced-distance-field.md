# Sliced distance field

`SlicedDistanceField` approximates signed distance in millimetres from completed
slice slabs, including holes, ledges, floors and ceilings. Distance is negative
inside. Construction uses Orca's planar SDF and two vertical lower-envelope
sweeps, including slices thinner than the output grid spacing.

The cubic grid starts at 1 mm spacing and coarsens by powers of two to stay
within 32 million nodes. A logarithmic distance byte and a sign bitmap retain
zero crossings and distances up to one metre. Queries use trilinear
interpolation and extend positive distance beyond the small exterior halo.
The field is approximate; it does not fall back to mesh geometry.

The API exposes grid metadata, signed node values and interpolation weights
for consumers preparing aligned fields. Construction reports progress and
propagates cancellation; completed fields are read-only.
