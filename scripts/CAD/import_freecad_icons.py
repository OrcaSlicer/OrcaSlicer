#!/usr/bin/env python3
"""Copy the FreeCAD (LGPL-2.1) icon for every ribbon verb into resources/images/fc_<verb id>.svg.

The ribbon (DesignImGuiUi.cpp) looks for fc_<id>.svg first, then the verb's own design_* icon, then
draws text only. Re-run after adding a verb to DesignOffer.hpp and extending MAP.
Colours are NOT baked in: the ribbon re-tints near-neutral strokes for the active light/dark theme at load.

  import_freecad_icons.py [freecad_src_dir]   # default ~/projects/apps/snaporca_freecad/freecad/src
"""
import os, shutil, sys

SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser('~/projects/apps/snaporca_freecad/freecad/src')
DST = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'resources', 'images')

PD  = 'Mod/PartDesign/Gui/Resources/icons/PartDesign_%s.svg'
PT  = 'Mod/Part/Gui/Resources/icons/tools/Part_%s.svg'
PB  = 'Mod/Part/Gui/Resources/icons/booleans/Part_%s.svg'
SKG = 'Mod/Sketcher/Gui/Resources/icons/geometry/Sketcher_%s.svg'
SKT = 'Mod/Sketcher/Gui/Resources/icons/tools/Sketcher_%s.svg'
SKC = 'Mod/Sketcher/Gui/Resources/icons/constraints/Constraint_%s.svg'
GUI = 'Gui/Icons/%s.svg'
MEA = 'Mod/Measure/Gui/Resources/icons/%s.svg'

MAP = {
    # model
    'sketch': 'Mod/Sketcher/Gui/Resources/icons/general/Sketcher_NewSketch.svg',
    'sketch3d': 'Mod/Sketcher/Gui/Resources/icons/Sketcher_Sketch.svg',
    'extrude': PD % 'Pad', 'revolve': PD % 'Revolution', 'sweep': PD % 'AdditivePipe', 'loft': PD % 'AdditiveLoft',
    'rib': PD % 'AdditivePrism', 'thicken': PT % 'Thickness', 'shell': PD % 'Thickness',
    'boolean': PB % 'Fuse', 'bool_subtract': PB % 'Cut', 'bool_intersect': PB % 'Common',
    'hole': PD % 'Hole', 'thread': PD % 'SubtractiveHelix', 'helix': PD % 'AdditiveHelix',
    'cut': PD % 'Pocket', 'split': PB % 'Slice',
    'fillet': PD % 'Fillet', 'chamfer': PD % 'Chamfer', 'draft': PD % 'Draft',
    'pattern': PD % 'LinearPattern', 'pattern_circular': PD % 'PolarPattern', 'mirror': PD % 'Mirrored',
    'pat_curve': PD % 'MultiTransform',
    'transform': GUI % 'Std_Placement', 'mate': SKC % 'PointOnPoint', 'align': GUI % 'align-to-selection',
    'surf_extrude': PT % 'Extrude', 'surf_revolve': PT % 'Revolve', 'surf_loft': PT % 'Loft',
    'surf_fill': PT % 'RuledSurface', 'surf_offset': PT % 'Offset', 'thicken_surf': PT % 'Thickness',
    'plane': PD % 'Plane', 'axis': PD % 'Line', 'coordsys_v': PD % 'CoordinateSystem',
    'project': SKG % 'Projection',
    'measure': MEA % 'Measurement-Distance', 'mass_props': MEA % 'MassPropertiesIcon', 'interference': PB % 'Section',
    'edit_feature': GUI % 'edit-edit', 'rename': GUI % 'document-properties', 'colour': GUI % 'Std_RandomColor',
    'zoom_to': GUI % 'zoom-fit-best',
    'delete': GUI % 'edit-delete', 'delete_face': GUI % 'edit-delete', 'delete_body': GUI % 'edit-delete',
    # sketch mode
    'sk_line_t': SKG % 'CreateLine', 'sk_polyline': SKG % 'CreatePolyline',
    'sk_rect': SKG % 'CreateRectangle', 'sk_rect_center': SKG % 'CreateRectangle_Center',
    'sk_rect_oblique': SKG % 'CreateRectangle3Points', 'sk_rect_rounded': SKG % 'CreateOblong',
    'sk_circle': SKG % 'CreateCircle', 'sk_circle_2pt': SKG % 'Create3PointCircle', 'sk_circle_3pt': SKG % 'Create3PointCircle',
    'sk_arc_t': SKG % 'Create3PointArc', 'sk_arc_tangent': SKG % 'Create3PointArc', 'sk_arc_center': SKG % 'CreateArc',
    'sk_slot': SKG % 'CreateSlot', 'sk_slot_arc': SKG % 'CreateArcSlot',
    'sk_ellipse': SKG % 'CreateEllipseByCenter', 'sk_ellipse_arc': SKG % 'CreateElliptical_Arc',
    'sk_spline': SKG % 'CreateBSpline',
    'sk_poly_3': SKG % 'CreateTriangle', 'sk_poly_4': SKG % 'CreateSquare', 'sk_poly_5': SKG % 'CreatePentagon',
    'sk_polygon': SKG % 'CreateHexagon', 'sk_poly_8': SKG % 'CreateOctagon', 'sk_poly_12': SKG % 'CreateRegularPolygon',
    'sk_poly_inscribed': SKG % 'CreateRegularPolygon', 'sk_poly_circumscribed': SKG % 'CreateRegularPolygon',
    'sk_point_t': SKG % 'CreatePoint', 'sk_text': SKG % 'CreateText', 'sk_svg': SKG % 'CarbonCopy',
    'sk_offset': SKT % 'Offset', 'sk_trim': SKG % 'Trimming', 'sk_fillet': SKG % 'CreateFillet',
    'sk_chamfer': SKG % 'CreateChamfer', 'sk_extend': SKG % 'Extend',
    'sk_array': SKT % 'RectangularArray', 'sk_array_polar': PD % 'PolarPattern', 'sk_mirror': SKT % 'Symmetry',
    'sk_move': SKT % 'Move', 'sk_rotate': SKT % 'Rotate', 'sk_scale': SKT % 'Scale',
    'sk_dimension': SKC % 'Dimension', 'sk_constrain': SKC % 'Group', 'constrain': SKC % 'Group',
    'sk_construct': SKG % 'ToggleConstruction', 'sk_delete': SKT % 'DeleteGeometry',
    'sk_length': SKC % 'Length', 'sk_radius': SKC % 'Radius', 'sk_angdist': SKC % 'InternalAngle',
}

missing = []
for verb, rel in sorted(MAP.items()):
    src = os.path.join(SRC, rel)
    if not os.path.isfile(src):
        missing.append((verb, rel)); continue
    shutil.copyfile(src, os.path.join(DST, 'fc_%s.svg' % verb))
print('copied %d, missing %d' % (len(MAP) - len(missing), len(missing)))
for v, r in missing: print('  MISSING', v, r)
sys.exit(1 if missing else 0)
