# Per-extruder layer heights

The process option `extruder_layer_height` stores a height in millimetres for each physical extruder. Zero inherits the object's scalar `layer_height`. Short vectors follow the configuration system's broadcast convention and repeat the first entry. Existing projects and presets default to zero and retain the ordinary slicing pipeline.

Filaments resolve through manual filament-to-extruder assignments. Conventional independent tools use their tool index. Automatic Bambu filament grouping must be resolved manually because slicing needs to know the extruder before geometry is generated. Different extruders may use different heights even when their nozzle diameters match.

Each process height row stays bound to its named physical tool. Selecting another tool or flow variant does not retarget these rows, and copying variant settings does not overwrite them.

## Nozzle layout changes

Nozzle synchronization and manual printer diameter edits remap process heights when the new layout is a permutation of the same nozzle-size set. Equal-size nozzles that remain in their slots keep their individual heights. Remaining nozzles match in slot order, because diameter telemetry cannot identify individual equal-size nozzles. Zero values move as inheritance markers; compact broadcast vectors remain compact when their effective heights do not change.

Individual manual edits retain the original layout until the permutation is complete. Editing process heights establishes new associations with the current layout. Loading a printer preset or project resets the association, so saved process values are not reordered merely by loading them. Different nozzle-size sets or tool counts retain explicit heights and use slicing validation rather than inferring new heights.

## Geometry and extrusion

All objects use a common fine grid calculated as the greatest common divisor of the heights of tools used by the print, at micrometre precision. The grid must be at least 0.01 mm. For example, 0.12 mm and 0.30 mm use a 0.06 mm grid. Unused nozzle heights neither refine this grid nor activate multi-height restrictions. Object, support, interface, and painted filament assignments contribute through their physical-tool mapping. All objects retain the shared grid for prime-tower compatibility, and changes to the used tool set invalidate cached slicing. The initial layer retains `initial_layer_print_height` for every tool.

A region's walls are planned in bands on that grid. A retained contour is extruded at the band's top with flow calculated for the entire band height. The intermediate contours remain available for surface classification but do not emit duplicate walls. Geometry is partitioned against neighboring materials' occupied vertical spans. Local geometry changes and the object's top can require shorter bands within the extruder's printable height range.

Infill follows wall bands when its extruder uses the same height. Separately assigned infill is planned independently. Solid requirements are gathered across the whole band. The merge runs after surface classification so that it cannot restore extrusion on a swallowed layer. Thin bridges use the full merged height; thick bridges retain their rounded flow when it covers the span.

Shell counts are converted to fine-grid counts during shell discovery. They continue to describe deposited layers, rather than the number of intermediate slicing steps. G-code gap detection uses the extrusion's full vertical span.

The band planning and geometry arbitration are adapted from BambuStudio commit `56a57d00a591360cf41bf21383915ee5bf357be5`. Orca supplies the physical-extruder heights and printer limits instead of deriving heights from nozzle-diameter ratios.

## Settings boundaries

Support and support-interface tools must be assigned explicitly and may use different heights. Support geometry is classified at contact and interface boundaries, then each material's slabs are subdivided to its assigned tool's process height before generating extrusion paths. Interface thickness uses the interface tool's height, with adhesion layers using the base tool's height. Coincident slab tops share a support layer while retaining their own extrusion heights and flow. First-layer and contact-gap requirements still determine the exact support boundary positions. Organic tree support is rejected because its separate planner follows the object grid; normal and slim, strong, or hybrid tree support use independent scheduling.

Inner and outer walls of one part must have matching layer heights. Solid infill, top surfaces, and bottom surfaces of one part must likewise agree. Different parts and painted regions can use different heights. Variable layer-height painting, layer-height range overrides, and spiral vase mode cannot be combined with these fixed extruder schedules.

