// Faceted ridge key — asymmetric male/female alignment feature, flat facets only.
//
// 6 vertices, 7 faces, one closed manifold. Euler check: V - E + F = 6 - 11 + 7 = 2.
// No spheres, no cylinders, no splines, no fillets.
//
// THE FLANKS ARE TRIANGULATED EXPLICITLY, and that is not cosmetic. Written as quads
// [0,3,5,4] and [1,4,5,2] they are NOT planar — the base edge and the ridge edge are
// skew, so the four corners do not share a plane. A checker caught this after the first
// draft claimed the opposite. Left as quads, the tessellator picks the fold direction for
// you, which means the "flat facet" promise is broken by an unspecified crease and two
// exporters can disagree about the shape. Splitting them here fixes the crease at
// back-bottom -> front-ridge, which keeps the rear peak's triangle large and clean.
//
// FRAME CONVENTION (matches the CAD mate connector it is derived from):
//   +Z  the mating axis   — the feature protrudes along it
//   +X  the roll reference — the ridge runs along it, low end forward
//   +Y  completes the right-handed frame
//
// WHAT BREAKS WHICH SYMMETRY
//   rotational about Z ....... the ridge (elongation along X)
//   180 deg about Z .......... the ridge SLOPE: tall steep back, long shallow front
//   mirror across XZ ......... deliberately NOT broken. Handedness is fixed by convention,
//                              so +Y is implied once Z and X are known. Breaking it would
//                              add a facet and buy nothing.
//
// KNOWN AMBIGUITY, stated rather than hidden: viewed exactly ALONG the ridge (+/-X,
// orthographic), the silhouette is the same isoceles triangle from front and back. Front
// and back are then distinguished by SHADING only — the long shallow front face catches
// light differently from the steep back face. If the target renderer is flat-shaded with a
// single headlight, verify this case before committing to the shape.

// ---------------------------------------------------------------- parameters
L   = 12.0;   // overall length along the ridge (X)
W   = 4.0;    // half-width at the BACK
tf  = 0.45;   // front taper: front half-width = W * tf
H   = 4.5;    // peak height at the rear   <-- the single dimension controlling asymmetry
pr  = 0.22;   // rear ridge position, fraction of L from the back
pf  = 0.62;   // front ridge position, fraction of L from the back
hf  = 0.35;   // front ridge height, fraction of H

// Clearance is a PHYSICAL quantity and only means anything if this is a printed part.
// See the note at the bottom: for a viewport glyph it is meaningless.
clr    = 0.20;  // per-face clearance, mm
depth  = 0.40;  // extra pocket depth so the male never bottoms out before it seats

Wf  = W * tf;
xr0 = -L/2 + L * pr;
xr1 = -L/2 + L * pf;
Hf  = H * hf;

// ---------------------------------------------------------------- geometry
// Vertex order is fixed and referenced by the face table; do not reorder.
//   0 back-left    1 back-right    2 front-right    3 front-left
//   4 REAR PEAK (tall)             5 front ridge (low)
function ridge_pts(l, w, wf, h, hfr, x0, x1) = [
    [-l/2, -w,   0  ],   // 0
    [-l/2,  w,   0  ],   // 1
    [ l/2,  wf,  0  ],   // 2
    [ l/2, -wf,  0  ],   // 3
    [ x0,   0,   h  ],   // 4  rear peak
    [ x1,   0,   hfr]    // 5  front ridge, low
];

// OpenSCAD wants each face wound CLOCKWISE seen from OUTSIDE. The right-hand-rule
// outward-normal (CCW) form is given in the comment for anyone porting to STL/OCC,
// where the opposite convention is the usual one.
RIDGE_FACES = [
    [3, 2, 1, 0],   // base       (CCW-outward: [0,1,2,3])   planar, all z=0
    [1, 4, 0],      // back       (CCW-outward: [0,4,1])     steep
    [5, 3, 0],      // flank -Y a (CCW-outward: [0,3,5])
    [4, 5, 0],      // flank -Y b (CCW-outward: [0,5,4])
    [5, 4, 1],      // flank +Y a (CCW-outward: [1,4,5])
    [2, 5, 1],      // flank +Y b (CCW-outward: [1,5,2])
    [5, 2, 3]       // front      (CCW-outward: [3,2,5])     long, shallow
];

module ridge_key(l = L, w = W, wf = Wf, h = H, hfr = Hf, x0 = xr0, x1 = xr1) {
    polyhedron(points = ridge_pts(l, w, wf, h, hfr, x0, x1),
               faces  = RIDGE_FACES,
               convexity = 3);
}

// MALE: the protrusion, nominal size.
module ridge_key_male() { ridge_key(); }

// FEMALE: the pocket. Grown by `clr` on every side and sunk `depth` deeper.
//
// HONEST LIMITATION: this grows the key by scaling its defining dimensions, which is NOT a
// true uniform surface offset — on the shallow front face the normal clearance comes out
// smaller than `clr`, because that face is far from perpendicular to every axis it is
// scaled along. A true offset needs minkowski() with a small cube, which is exact and slow,
// or an explicit per-face plane push, which is exact and fiddly. For a keying feature whose
// job is angular registration rather than a press fit, the approximation is the right trade
// — but do not quote this pocket as holding 0.2 mm everywhere, because it does not.
module ridge_key_female() {
    translate([0, 0, -depth])
        ridge_key(l   = L + 2*clr,
                  w   = W + clr,
                  wf  = Wf + clr,
                  h   = H + clr + depth,
                  hfr = Hf + clr + depth,
                  x0  = xr0,
                  x1  = xr1);
}

// ---------------------------------------------------------------- demo
// Left: the male key on its plate. Right: the plate with the pocket cut.
PLATE = [30, 18, 3];

module plate_with_male() {
    translate([-PLATE[0]/2, -PLATE[1]/2, -PLATE[2]]) cube(PLATE);
    ridge_key_male();
}

module plate_with_female() {
    difference() {
        translate([-PLATE[0]/2, -PLATE[1]/2, -PLATE[2]]) cube(PLATE);
        ridge_key_female();
    }
}

translate([-20, 0, 0]) plate_with_male();
translate([ 20, 0, 0]) plate_with_female();

// ---------------------------------------------------------------- note on the two readings
// This file is written for the PHYSICAL reading: a printable alignment key, where `clr` and
// `depth` are real millimetres and flat facets genuinely help — they slice without the
// stair-stepping a tessellated curve produces, and they print without support on the
// shallow front face.
//
// If the intent is instead the VIEWPORT GLYPH for a CAD mate connector, then:
//   - `clr` and `depth` are meaningless: a symbol does not mate with anything;
//   - all dimensions must become SCREEN PIXELS scaled by upp = 1/zoom, because every gizmo
//     in that viewport is screen-constant and must not shrink with the model;
//   - "low-poly for rendering performance" is not a real reason at ~2-20 glyphs per frame.
//     The real reason to keep flat facets there is LEGIBILITY: hard normals give distinct
//     value steps between facets, and that is what lets a 22-px solid read as an oriented
//     object instead of a grey blob.
// The vertex logic above is identical under both readings. Only the units and the clearance
// change.
