# Painted Modifiers — High Level Design

## Purpose and scope

A painted modifier overrides print settings like a modifier volume does, but
its shape is painted on the surface of a part instead of being a mesh of its
own. The user paints the area, and the modifier covers the part of the object
behind the paint, up to a chosen depth.

Painted modifiers are listed in the object list together with the modifier
volumes and share their priority rule: where several modifiers overlap and set
the same option, the one lower in the list wins. This holds between painted
modifiers, between painted and mesh modifiers, and in any mix of the two.

Color painting and fuzzy-skin painting are not part of this order. They are
applied after all modifiers, as before.

## Data model

A painted modifier is a `ModelVolume` of type
`ModelVolumeType::PAINTED_MODIFIER` in `ModelObject::volumes`, so it takes part
in everything the volume list already provides: its position in the list, its
name and settings (`ModelVolume::config`), undo/redo and change detection.

| Member | Meaning |
| --- | --- |
| `painted_modifier_facets` | The painted facets, `ENFORCER` state only. |
| `painted_modifier_depth` | How far the paint reaches into the part, in mm. |
| `painted_modifier_host` | `ObjectID` of the model part it is painted on. |

The volume shares the mesh (`std::shared_ptr<const TriangleMesh>`) and the
transformation of its host part, so the painted facets index the host's
triangles and the volume costs no geometry memory. One painted modifier is
painted on one part; painting several parts takes one painted modifier per part.

The facets live on the painted modifier rather than on its host because one
facet holds a single paint state: overlapping painted modifiers need a mask
each.

### Host link

`ModelObject::painted_modifier_host()` resolves the link by `ObjectID`. The ID
survives copies made with `assign_copy` and undo/redo;
`ModelObject::assign_new_unique_ids_recursive()` remaps it when an object is
cloned with new IDs, and `convert_units()` remaps it to the converted volumes.
Mesh edits such as simplify, repair or smoothing give a part a new ID through
`ModelVolume::set_new_unique_id()`, which moves the links of its painted
modifiers along. Mesh identity is not used as the link, since pasting a part
inside its object shares the mesh between two parts.

`ModelObject::sync_painted_modifiers()` makes each painted modifier follow its
host:

- it copies the host's transformation, because a part moved alone in the scene
  moves without its painted modifiers, which have no scene geometry;
- when the host's mesh was replaced, it carries the paint over with
  `TriangleSelector::remap_painting()` and shares the new mesh.

Operations that replace the meshes of all volumes keep the paint of painted
modifiers through `ModelVolume::save_painting()` and `restore_painting()`, like
any other paint. Repair skips painted modifiers, which follow their repaired host.

The GUI calls it before every `Print::apply()`, before saving a project and when
the paint tool opens. A painted modifier whose host was deleted keeps its own
mesh and transformation; the object list deletes the painted modifiers of a part
together with it.

`ModelObject::sort_volumes()` sorts painted modifiers as modifiers, so the
stable sort keeps the user's order within the modifier group.

## Slicing

### Shape

`PrintObject::slice_volumes()` slices a painted modifier like any other volume,
which yields its host's cross-section on every layer, then replaces these slices
with `painted_modifier_segmentation()` (`MultiMaterialSegmentation.cpp`):

1. **Walls.** The painted facets are projected onto the contours of the
   cross-section and every layer is split by the paint state of the nearest
   contour, with the same Voronoi segmentation as color painting
   (`segment_layers_by_painted_contours()`). With a depth above 0 the painted
   part is cut to a band of that width along the contour.
2. **Top and bottom.** Painted facets facing up or down are projected down or up
   through the cross-sections, as long as they stay inside the part and within
   the depth. A depth of 0 reaches through as many layers as the top and bottom
   shells, like color painting does.

A depth of 0 therefore means: across the whole part up to the nearest unpainted
surface, and as deep as the shells below and above painted faces.

The shape is computed from the host's own slices before region assignment,
because it has to enter the modifier chain. Color painting, in contrast,
segments the merged regions after assignment.

### Precedence

`generate_print_object_regions()` walks the volume list in order and treats a
painted modifier exactly like a mesh modifier (`ModelVolume::is_region_modifier()`):
it becomes a child region of every earlier part or modifier region whose bounding
box it overlaps, with that region's settings plus its own. `slices_to_regions()`
then moves the overlap of its slices out of each parent region into the child.
A later modifier is applied on top of the result of the earlier ones, so it wins
on shared options and inherits the others.

### Change detection

`Print::apply()` reslices an object when its painted modifiers are added,
removed, reordered or moved (`model_volume_list_changed()`), or when their paint
or depth change (`model_painted_modifier_data_changed()`). Setting changes go
through `verify_update_print_object_regions()` like those of mesh modifiers.

## Project files

`store_bbs_3mf()` writes a painted modifier so that older versions load it as a
modifier without effect, never as a printable part:

- the `<part>` has `subtype="modifier_part"`;
- `painted_modifier_depth` marks it as painted and `painted_modifier_host` holds
  the index of the host in the object's volumes;
- its settings are prefixed with `painted_modifier_config:`, which older readers
  drop;
- the paint is the triangle attribute `paint_modifier` of its own copy of the
  host mesh.

The reader restores the type, depth and settings, links the host by index and
calls `sync_painted_modifiers()`, which shares the host's mesh again. The
PrusaSlicer-format writer `store_3mf()` stores no paint for them and writes them
as modifiers with prefixed settings only.

## Application

- **Object list.** Painted modifiers are added from the object's "Add Painted
  Modifier" menu item or a part's context menu, appear among the modifiers with
  their own icon, and can be dragged among mesh modifiers. Their type cannot be
  changed, and their context menu offers rename, delete, process settings and
  filament.
- **Scene.** They have no `GLVolume`; `Selection` counts only the volumes shown
  in the scene to decide whether a whole instance is selected. Selecting a
  painted modifier in the list selects its instance, shows its settings and opens
  its paint tool.
- **Paint tool.** `GLGizmoPaintedModifier` paints one painted modifier at a time
  and has no toolbar button. Only its host can be painted; a hit on another part
  counts as a miss, so other parts still occlude
  (`GLGizmoPainterBase::is_mesh_paintable()`). With "New painted modifier"
  chosen, the first stroke creates one on the part it paints. The tool also edits
  the depth.

## Known limitations

- The paint tool shows the paint of the edited painted modifier only.
- Cutting a host part or reloading it from disk replaces it with new volumes,
  so its painted modifiers lose their host; they keep their shape but no longer
  follow the part.
- Splitting a host part into parts keeps its painted modifiers on the first
  piece; paint on the other pieces is dropped.
- The paint is not shown outside the paint tool.
