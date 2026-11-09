# Fin support — High Level Design

## Purpose and scope

Fin support stabilizes tall, narrow or leaning FFF models with sheets rising
from the build plate and intermittent contacts along the model's sides.
Users select the Fin support type and paint the model to supply placement
input. Fin dimensions, outward expansion and contact-band spacing control
its shape and attachment.

Fin follows the object's layers. It does not generate underside support or
top interface layers, and it does not support rafts. Its output uses ordinary
`SupportLayer` records, so preview and G-code export use the existing support
pipeline.

## Placement modes

| Mode | Placement | Paint authorization |
| --- | --- | --- |
| Simple | Selects sites along painted model contours, respecting placement spacing. | Required at the seed and each lower contact. |
| Simulated (default) | Plans upward from the model's printed shape and directional demand; revises plans when generated fins fail. | Required at the seed and each lower contact. |
| Painted Shape | Interprets painted islands as points, finite paths or branches, with separate ordinary and crease handling. | Authorizes the initial intent; later contacts can follow unpainted walls. |

Simulated uses geometric heuristics derived from the printed portion of the
object. It does not model material strength, nozzle forces or bed adhesion.
Its replanning loop owns retries and responds to realized geometry and contact
coverage. Simple selects a batch and omits candidates that cannot be generated.

Painted Shape extracts source geometry, corrects directions and connectivity,
and resolves source heights before physical generation. Point/line
classification belongs to this preparation: a later collision or contact
failure does not reinterpret a line as a point. Creases retain their original
3D source for contact queries.

## Intent input

The implementation is in [FinSupport.cpp](../../src/libslic3r/Support/FinSupport.cpp),
entered through `generate_fin_support()`. Painted Shape publishes
`FinIntentInput`, which owns source regions, resolved points, line evidence
and topology. Correction reads it without modifying the source and produces
`FinCorrectionResult`; preparation and height/root resolution then produce
`FinCandidateProposal`. Simple and Simulated use their own placement routes
and do not publish this intent input.

Tracing and contact construction own their working geometry. Completed
Painted Shape member contours are published before component assembly;
assembly can remove unreachable contributions without modifying that result.
Diagnostics observe these boundaries and do not supply generation inputs.

Graph junctions describe connectivity; generation groups define activation,
clipping and failure scope. A finite group may contain interior junctions.
Clipping must retain the group's original start and a connection to a surviving
root. An interior junction cannot become a replacement root after a failure.

## Physical generation

Geometry is sampled on the object's `slice_z`; physical layer tops and bottoms
are used for vertical clearance. These coordinates remain distinct because
slice sampling need not be at the center of a printed layer.

Side contacts use the physical coverage of generated perimeter paths and their
actual widths, including variable-width walls. Fin generation therefore requires
completed perimeters, including in support preview. Perimeter invalidation also
invalidates Fin support. Source-model anchors remain separate from wall hits;
a missing wall hit does not fall back to the nominal model contour.

Ordinary Painted Shape contacts query the nearby model wall with a bounded
turn from the body direction and validate coverage across the contact span.
Crease contacts intersect the source crease at the sampling height and query
the wall along the section's exterior bisector. Crease lines use this rule
throughout; crease points use ordinary contacts for their lower periodic bands.
Configured penetration moves the contact front relative to the wall using the
owning model region's external-perimeter width as its reference.

Each fin has a highest contact band and periodic lower bands. In Painted Shape,
a failed highest band rejects its generating member; a failed periodic-band
top omits that band. Below an established band top, a failed contact query can
continue the last accepted front. A stopped band can retain its accepted upper
part, with a downward taper below it. Contact and body construction share
attachment endpoints so their contours join consistently.

## Clearance and assembly

Bodies are clipped against model clearance and the printable bed. The upper
model query uses physical layer bounds and the configured top gap, including
model layers above the fin's seed. Upper-wall coverage can terminate a contact
band without restoring body geometry already removed by clearance.

Painted Shape constructs body, contact and taper contours in printable-center
space. Actual contacts and transitions have their own clearance exemptions;
those exemptions do not authorize new contacts or revive rejected members.
Completed contributions are assembled and jointly clipped before toolpaths are
generated. Intersecting fins can merge, while finite groups retain their root
reachability and lifetime rules.

Simple and Simulated instead validate individual fins and reject positive-area
overlap above the bed. Simple omits a rejected candidate; Simulated can replan.
All modes unite their first-layer feet.

## Toolpaths and diagnostics

Painted Shape's first outline follows the completed center contour directly.
Additional walls are offset inside the physical envelope; inability to fit an
optional inner wall retains the preceding walls. Failure to generate the
required first outline is reported and skips the affected merged layer region.
It does not reconstruct that region from individual source fins. Foot regions
use the existing support fill machinery.

Logs use the `Fin support` prefix and identify preparation, contact, assembly
and output outcomes. Optional SVG diagnostics show intermediate intent and
geometry. An accepted source or an intermediate drawing does not establish
that printable support was emitted; final outlines and support layers are the
output boundary. Regression cases are in
[test_support_material.cpp](../../tests/fff_print/test_support_material.cpp).
