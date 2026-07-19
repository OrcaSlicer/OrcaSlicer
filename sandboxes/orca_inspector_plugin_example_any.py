# /// script
# requires-python = ">=3.12"
# dependencies = ["numpy"]
#
# [tool.orcaslicer.plugin]
# name = "Orca Inspector Example"
# description = "An interactive panel that browses the whole orca.host read-only API and demos every orca.host.ui facility."
# author = "OrcaSlicer"
# version = "0.0.1"
# ///
"""Orca Inspector — the worked example for `orca.host` and `orca.host.ui`.

Run it from the Plugins dialog. It opens a NON-MODAL window (OrcaSlicer stays
usable) with a sidebar of sections, each exercising one part of the API:

  Overview   plater state, scene statistics, current printer/process/filaments
  Presets    every PresetCollection, preset flags, any preset's full config
  Config     the complete merged full_config as a searchable table
  Model      Model -> Object -> Volume/Instance tree with lazy mesh geometry
  Assembly   objects grouped by module_name, per-instance assemble transforms
  UI Toolkit live demos of message/create_window/ProgressDialog

The page and the plugin talk JSON through the injected `window.orca` bridge:

  page   --orca.postMessage({command:'fetch', section})-->      plugin.on_message()
  page   --orca.postMessage({command:'mesh', object, volume})-->     "
  page   --orca.postMessage({command:'ui', action, ...})-->          "
  plugin --win.post({command:'section'|'mesh'|..., ok, data})--> page (orca.onMessage)

Everything is fetched lazily: a section is built only when it is first shown,
and heavy mesh geometry (zero-copy numpy arrays, world-space math, the little
point-cloud previews) only when you ask for it. "Refresh" drops the cache and
refetches the visible section.

Style rules the page follows (see the plugin docs):
  - no hardcoded colors — only the host-injected --orca-* theme variables, so
    the panel matches light and dark mode automatically;
  - self-contained HTML (inline CSS/JS, no external resources);
  - on_message runs on the UI thread, so long work (the progress demos) is
    offloaded to a thread and results are pushed back with win.post().

numpy is declared as a dependency for the mesh arrays and 4x4 matrices; every
numpy-dependent feature degrades gracefully if it is missing. The assembly
transforms need bindings added in July 2026 — on older builds the panel shows
what is missing instead of failing.
"""

import re
import threading
import time

import orca

try:
    import numpy as np
except Exception:
    np = None


# Soft caps so a heavy scene cannot produce an unusably large payload. When a
# cap trims output the payload says so explicitly rather than truncating silently.
MAX_OBJECTS = 50
MAX_VOLUMES = 20
MAX_INSTANCES = 20
MAX_PRESET_NAMES = 500
MAX_PREVIEW_POINTS = 4000


# --------------------------------------------------------------------------- #
# small converters — every builder returns plain JSON-able dicts/lists.
# Cast numpy scalars with float()/int(): the bridge serializes unknown types
# with str(), which would silently turn them into strings.
# --------------------------------------------------------------------------- #
def vec(v):
    return [round(float(c), 4) for c in v]


def bbox_dict(bb):
    """host.BoundingBox -> dict (or None when the box is undefined)."""
    if not bb.defined:
        return None
    return {"min": vec(bb.min), "max": vec(bb.max), "size": vec(bb.size),
            "center": vec(bb.center), "radius": round(float(bb.radius), 4)}


def enum_name(value):
    return getattr(value, "name", str(value))


def config_rows(keys, get_value):
    """Sorted [{k, v}] for a config; None values become ''. Complete, not sampled."""
    return [{"k": k, "v": "" if (v := get_value(k)) is None else str(v)}
            for k in sorted(keys)]


def preset_dict(preset):
    """Every documented host.Preset attribute, in one dict."""
    return {
        "name": preset.name,
        "alias": preset.alias,
        "label": preset.label(),
        "type": enum_name(preset.type),
        "file": preset.file,
        "bundle_id": preset.bundle_id,
        "config_key_count": len(preset.config_keys()),
        "is_default": preset.is_default,
        "is_system": preset.is_system,
        "is_user": preset.is_user(),
        "is_external": preset.is_external,
        "is_visible": preset.is_visible,
        "is_dirty": preset.is_dirty,
        "is_compatible": preset.is_compatible,
        "is_project_embedded": preset.is_project_embedded,
        "is_from_bundle": preset.is_from_bundle(),
    }


def split_config_list(value):
    """Split a serialized per-extruder option ("#FF0000;#00FF00") into parts."""
    if value is None:
        return []
    return [part.strip().strip('"') for part in str(value).split(";")]


# --------------------------------------------------------------------------- #
# section builders
# --------------------------------------------------------------------------- #
def build_overview():
    plater = orca.host.plater()
    model = orca.host.model()
    bundle = orca.host.preset_bundle()
    objects = model.objects()

    cfg = bundle.full_config_value
    colors = split_config_list(cfg("filament_colour"))
    types = split_config_list(cfg("filament_type"))
    filaments = []
    for i, preset in enumerate(bundle.current_filament_presets()):
        filaments.append({
            "name": preset.name if preset else "<missing preset>",
            "color": colors[i] if i < len(colors) else "",
            "type": types[i] if i < len(types) else "",
            "is_system": bool(preset.is_system) if preset else False,
            "is_dirty": bool(preset.is_dirty) if preset else False,
        })

    printer = bundle.current_printer_preset()
    process = bundle.current_process_preset()
    return {
        "plater": {
            "project_dirty": plater.is_project_dirty(),
            "presets_dirty": plater.is_presets_dirty(),
            "in_snapshot": plater.inside_snapshot_capture(),
        },
        "stats": {
            "objects": len(objects),
            # derived from instances: ModelObject.printable is only the import-time flag
            "printable_objects": sum(1 for o in objects if printable_instance_count(o)),
            "volumes": sum(o.volume_count() for o in objects),
            "instances": sum(o.instance_count() for o in objects),
            "triangles": sum(o.facets_count() for o in objects),
            "materials": model.material_count(),
            "plate_index": model.current_plate_index(),
            "max_z": round(float(model.max_z()), 3),
            "config_keys": len(bundle.full_config_keys()),
        },
        "bbox": bbox_dict(model.bounding_box()),
        "bbox_approx": bbox_dict(model.bounding_box_approx()),
        "painted": {
            "support": model.is_fdm_support_painted(),
            "seam": model.is_seam_painted(),
            "multimaterial": model.is_mm_painted(),
            "fuzzy_skin": model.is_fuzzy_skin_painted(),
        },
        "design": {"designer": model.designer(), "design_id": model.design_id(),
                   "model_id": model.id()},
        "setup": {
            "printer": {"name": printer.name, "is_system": printer.is_system,
                        "is_dirty": printer.is_dirty},
            "process": {"name": process.name, "is_system": process.is_system,
                        "is_dirty": process.is_dirty},
            "filaments": filaments,
            "highlights": [{"k": key, "v": str(cfg(key))}
                           for key in ("printer_model", "nozzle_diameter",
                                       "printable_height", "layer_height",
                                       "sparse_infill_density", "enable_prime_tower")
                           if cfg(key) is not None],
        },
    }


# label -> how to reach the PresetCollection on the bundle
COLLECTIONS = [
    ("prints", "Process"),
    ("printers", "Printer"),
    ("filaments", "Filament"),
    ("sla_prints", "SLA print"),
    ("sla_materials", "SLA material"),
]


def build_presets():
    bundle = orca.host.preset_bundle()
    collections = []
    for attr, label in COLLECTIONS:
        coll = getattr(bundle, attr)
        names = coll.preset_names()
        selected = coll.selected_preset()
        collections.append({
            "key": attr,
            "label": label,
            "size": coll.size(),
            "selected_name": coll.selected_preset_name(),
            # selected = as saved on disk; edited = selected + unsaved modifications
            "selected": {"name": selected.name, "is_dirty": selected.is_dirty,
                         "config_key_count": len(selected.config_keys())},
            "edited": preset_dict(coll.edited_preset()),
            "names": names[:MAX_PRESET_NAMES],
            "truncated": max(0, len(names) - MAX_PRESET_NAMES),
        })
    return {
        "collections": collections,
        "filament_names": bundle.current_filament_preset_names(),
    }


def build_preset_config(collection_key, name):
    bundle = orca.host.preset_bundle()
    if collection_key not in {attr for attr, _ in COLLECTIONS}:
        raise ValueError(f"unknown collection {collection_key!r}")
    preset = getattr(bundle, collection_key).find_preset(name)
    if preset is None:
        raise ValueError(f"preset {name!r} not found")
    return {"preset": preset_dict(preset),
            "rows": config_rows(preset.config_keys(), preset.config_value)}


def build_config():
    bundle = orca.host.preset_bundle()
    rows = config_rows(bundle.full_config_keys(), bundle.full_config_value)
    return {"rows": rows, "count": len(rows)}


def volume_dict(index, volume):
    return {
        "index": index,
        "id": volume.id(),
        "name": volume.name,
        "type": enum_name(volume.type()),
        "roles": {
            "model_part": volume.is_model_part(),
            "modifier": volume.is_modifier(),
            "negative": volume.is_negative_volume(),
            "support_enforcer": volume.is_support_enforcer(),
            "support_blocker": volume.is_support_blocker(),
        },
        "extruder": volume.extruder_id(),      # 1-based; -1 = none/inherited
        "offset": vec(volume.offset()),
        "rotation": vec(volume.rotation()),    # radians
        "scale": vec(volume.scaling_factor()),
        "mirror": vec(volume.mirror()),
        "facets": volume.facets_count(),
        "volume_mm3": round(float(volume.volume()), 3),
        "manifold": volume.is_manifold(),
        "mesh_errors": volume.mesh_errors_count(),
        "painted": {"support": volume.is_fdm_support_painted(),
                    "seam": volume.is_seam_painted(),
                    "multimaterial": volume.is_mm_painted(),
                    "fuzzy_skin": volume.is_fuzzy_skin_painted()},
        "bbox": bbox_dict(volume.bounding_box()),
        "config": config_rows(volume.config_keys(), volume.config_value),
    }


def assemble_dict(instance):
    """Assemble-view placement of one instance; needs bindings added 2026-07.
    getattr-guarded so the panel still runs on an older OrcaSlicer build."""
    if not hasattr(instance, "assemble_offset"):
        return {"available": False}
    return {
        "available": True,
        "initialized": instance.is_assemble_initialized(),
        "offset": vec(instance.assemble_offset()),
        "rotation": vec(instance.assemble_rotation()),   # radians
        "offset_to_assembly": vec(instance.offset_to_assembly()),
    }


def instance_dict(index, instance):
    return {
        "index": index,
        "id": instance.id(),
        "printable_flag": instance.printable,
        "is_printable": instance.is_printable(),  # also requires inside print volume
        "offset": vec(instance.offset()),
        "rotation": vec(instance.rotation()),      # radians
        "scale": vec(instance.scaling_factor()),
        "mirror": vec(instance.mirror()),
        "left_handed": instance.is_left_handed(),
        "bbox": bbox_dict(instance.bounding_box()),
        "assemble": assemble_dict(instance),
    }


def printable_instance_count(obj):
    """The GUI's printable toggle writes ModelInstance.printable only;
    ModelObject.printable keeps its import-time value. So an object's effective
    printable state must be derived from its instances."""
    return sum(1 for i in range(obj.instance_count()) if obj.instance(i).printable)


def object_dict(index, obj):
    volumes = [volume_dict(i, obj.volume(i))
               for i in range(min(obj.volume_count(), MAX_VOLUMES))]
    instances = [instance_dict(i, obj.instance(i))
                 for i in range(min(obj.instance_count(), MAX_INSTANCES))]
    return {
        "index": index,
        "id": obj.id(),
        "name": obj.name,
        "module_name": obj.module_name,
        "input_file": obj.input_file,
        "printable": obj.printable,       # raw import-time flag, see above
        "counts": {"volumes": obj.volume_count(), "instances": obj.instance_count(),
                   "printable_instances": printable_instance_count(obj),
                   "facets": obj.facets_count(), "parts": obj.parts_count(),
                   "materials": obj.materials_count(),
                   "mesh_errors": obj.mesh_errors_count()},
        "flags": {"multiparts": obj.is_multiparts(), "cut": obj.is_cut(),
                  "custom_layering": obj.has_custom_layering()},
        "painted": {"support": obj.is_fdm_support_painted(),
                    "seam": obj.is_seam_painted(),
                    "multimaterial": obj.is_mm_painted(),
                    "fuzzy_skin": obj.is_fuzzy_skin_painted()},
        "z_range": [round(float(obj.min_z()), 3), round(float(obj.max_z()), 3)],
        "bbox": bbox_dict(obj.bounding_box()),            # world, all instances
        "raw_bbox": bbox_dict(obj.raw_mesh_bounding_box()),  # untransformed meshes
        "config": config_rows(obj.config_keys(), obj.config_value),
        "volumes": volumes,
        "volumes_omitted": max(0, obj.volume_count() - len(volumes)),
        "instances": instances,
        "instances_omitted": max(0, obj.instance_count() - len(instances)),
    }


def build_model():
    model = orca.host.model()
    count = model.object_count()
    shown = min(count, MAX_OBJECTS)
    return {
        # carried along so the extruder swatches never depend on a stale Overview
        "filament_colors": split_config_list(
            orca.host.preset_bundle().full_config_value("filament_colour")),
        "id": model.id(),
        "object_count": count,
        "objects_omitted": count - shown,
        "material_count": model.material_count(),
        "plate_index": model.current_plate_index(),
        "max_z": round(float(model.max_z()), 3),
        "bbox": bbox_dict(model.bounding_box()),
        "bbox_approx": bbox_dict(model.bounding_box_approx()),
        "objects": [object_dict(i, model.object(i)) for i in range(shown)],
    }


def build_assembly():
    """Group objects by module_name (the assembly path recorded in the 3mf) into
    a tree, and report each instance's Assemble-view transform."""
    model = orca.host.model()
    root = {"name": "", "groups": {}, "objects": []}
    count = min(model.object_count(), MAX_OBJECTS)
    has_modules = False

    for i in range(count):
        obj = model.object(i)
        module = (obj.module_name or "").strip()
        has_modules = has_modules or bool(module)

        node = root
        for part in [p for p in re.split(r"[/\\]", module) if p]:
            node = node["groups"].setdefault(part, {"name": part, "groups": {}, "objects": []})

        instances = [{"index": j, "offset": vec(obj.instance(j).offset()),
                      "printable": obj.instance(j).printable,
                      "assemble": assemble_dict(obj.instance(j))}
                     for j in range(min(obj.instance_count(), MAX_INSTANCES))]
        bb = bbox_dict(obj.raw_mesh_bounding_box())
        node["objects"].append({"index": i, "name": obj.name,
                                "printable_instances": printable_instance_count(obj),
                                "instance_count": obj.instance_count(),
                                "size": bb["size"] if bb else None,
                                "instances": instances,
                                "instances_omitted": obj.instance_count() - len(instances)})

    def freeze(node):  # dict-of-groups -> sorted list, for stable JSON
        return {"name": node["name"],
                "groups": [freeze(node["groups"][k]) for k in sorted(node["groups"])],
                "objects": node["objects"]}

    return {"tree": freeze(root),
            "object_count": count,
            "objects_omitted": model.object_count() - count,
            "has_modules": has_modules}


def build_mesh(object_index, volume_index):
    """Heavy on purpose: full TriangleMesh access, numpy views, world-space math
    and a sampled world-space point cloud for the previews."""
    obj = orca.host.model().object(object_index)
    volume = obj.volume(volume_index)
    mesh = volume.mesh()

    data = {
        "vertex_count": mesh.vertex_count(),
        "triangle_count": mesh.triangle_count(),
        "empty": mesh.is_empty(),
        "manifold": mesh.is_manifold(),
        "volume_mm3": round(float(mesh.volume()), 3),
        "bbox": bbox_dict(mesh.bounding_box()),
        # numpy-free element access (always available, bounds-checked)
        "vertex0": vec(mesh.vertex(0)) if not mesh.is_empty() else None,
        "triangle0": list(mesh.triangle(0)) if not mesh.is_empty() else None,
        "numpy": None,
        "preview": None,
    }
    if np is None or mesh.is_empty():
        return data

    V = np.asarray(mesh.vertices())      # (N, 3) float32, read-only, zero-copy
    T = np.asarray(mesh.triangles())     # (M, 3) int32,  read-only, zero-copy
    N = np.asarray(mesh.face_normals())  # (M, 3) float32, computed copy
    tri = V[T].astype(np.float64)        # (M, 3, 3) corner positions
    area = 0.5 * np.linalg.norm(np.cross(tri[:, 1] - tri[:, 0],
                                         tri[:, 2] - tri[:, 0]), axis=1).sum()
    data["numpy"] = {
        "vertices": f"shape={V.shape} dtype={V.dtype} writeable={V.flags.writeable} "
                    f"zero_copy={V.base is not None}",
        "triangles": f"shape={T.shape} dtype={T.dtype}",
        "normals": f"shape={N.shape} dtype={N.dtype}",
        "surface_area_mm2": round(float(area), 3),
    }

    # World-space math under the first instance, using the row-vector convention
    # world = [V 1] @ (instance_matrix @ volume_matrix).T
    if obj.instance_count():
        instance = obj.instance(0)
        homog = np.c_[V.astype(np.float64), np.ones(len(V))]
        world = (homog @ (instance.matrix() @ volume.matrix()).T)[:, :3]
        sample = world[::max(1, len(world) // MAX_PREVIEW_POINTS)]
        data["numpy"]["world_bbox"] = {"min": vec(world.min(0)), "max": vec(world.max(0))}
        data["preview"] = {"points": [vec(p) for p in sample],
                           "sampled": len(sample), "total": len(world)}
        # The same mesh placed by the Assemble view (new 2026-07 API, guarded).
        if hasattr(instance, "assemble_matrix") and instance.is_assemble_initialized():
            asm = (homog @ (instance.assemble_matrix() @ volume.matrix()).T)[:, :3]
            data["numpy"]["assemble_bbox"] = {"min": vec(asm.min(0)), "max": vec(asm.max(0))}
    return data


SECTION_BUILDERS = {
    "overview": build_overview,
    "presets": build_presets,
    "config": build_config,
    "model": build_model,
    "assembly": build_assembly,
}


# --------------------------------------------------------------------------- #
# the page — sidebar app, themed exclusively via the injected --orca-* vars
# --------------------------------------------------------------------------- #
PAGE = r"""<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<style>
  :root { --mono: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; }
  * { box-sizing: border-box; }
  body { margin:0; height:100vh; display:flex; flex-direction:column; overflow:hidden; }

  header { flex:none; display:flex; align-items:center; gap:10px;
           padding:10px 16px; border-bottom:1px solid var(--orca-border); }
  header h1 { font-size:15px; margin:0; }
  #stamp { flex:1; color:var(--orca-muted); font-size:12px; }
  button.secondary { background:transparent; color:var(--orca-fg);
                     border-color:var(--orca-border); }
  button.small { padding:2px 10px; font-size:12px; }

  main { flex:1; display:flex; min-height:0; }
  nav  { flex:none; width:150px; padding:8px 0; overflow-y:auto;
         border-right:1px solid var(--orca-border); }
  nav .item { padding:8px 14px; cursor:pointer; user-select:none;
              color:var(--orca-muted); border-left:3px solid transparent; }
  nav .item:hover  { color:var(--orca-fg); }
  nav .item.active { color:var(--orca-fg); font-weight:600;
                     border-left-color:var(--orca-accent); }
  nav .glyph { display:inline-block; width:1.5em; }
  #content { flex:1; overflow:auto; padding:14px 16px 28px; }

  .tiles { display:grid; grid-template-columns:repeat(auto-fill,minmax(108px,1fr));
           gap:10px; margin-bottom:12px; }
  .tile  { border:1px solid var(--orca-border); border-radius:8px; padding:10px 12px; }
  .tile .v { font-size:20px; font-weight:600; font-variant-numeric:tabular-nums; }
  .tile .l { font-size:11px; color:var(--orca-muted); margin-top:2px; }

  .cards { display:grid; grid-template-columns:repeat(auto-fit,minmax(260px,1fr)); gap:12px; }
  .card  { border:1px solid var(--orca-border); border-radius:8px;
           padding:12px 14px; min-width:0; }
  .card h3 { margin:0 0 8px; font-size:11px; letter-spacing:.07em;
             text-transform:uppercase; color:var(--orca-muted); }
  .card.wide { grid-column:1 / -1; }

  .kv { display:grid; grid-template-columns:minmax(110px,max-content) 1fr;
        gap:3px 14px; font-size:12px; }
  .kv .k { color:var(--orca-muted); }
  .kv .v { font-family:var(--mono); word-break:break-word; }

  .badge { display:inline-block; border:1px solid var(--orca-border);
           color:var(--orca-muted); border-radius:999px; padding:1px 8px;
           font-size:11px; margin:1px 3px 1px 0; white-space:nowrap; }
  .badge.on { background:var(--orca-accent); border-color:var(--orca-accent);
              color:var(--orca-accent-fg); }
  .swatch { display:inline-block; width:12px; height:12px; border-radius:3px;
            border:1px solid var(--orca-border); vertical-align:-2px; margin-right:5px; }

  .toolbar { display:flex; gap:8px; align-items:center; margin-bottom:10px; }
  .toolbar input { flex:1; max-width:340px; }
  .count { color:var(--orca-muted); font-size:12px; }

  table.cfg { width:100%; border-collapse:collapse; font-size:12px; }
  table.cfg td:first-child { font-family:var(--mono); white-space:nowrap;
                             vertical-align:top; }
  table.cfg td.val { font-family:var(--mono); white-space:pre-wrap; word-break:break-word; }
  td.val .more { color:var(--orca-accent); cursor:pointer; }

  .node  { margin:1px 0; }
  .nhead { display:flex; align-items:center; gap:7px; padding:4px 8px;
           border-radius:6px; cursor:pointer; user-select:none; flex-wrap:wrap; }
  .nhead:hover { background:var(--orca-border); }
  .twist { width:1em; flex:none; color:var(--orca-muted); font-size:10px; }
  .nname { font-weight:600; }
  .nbody { display:none; margin-left:14px; padding:4px 0 4px 12px;
           border-left:1px dotted var(--orca-border); }
  .node.open > .nhead .twist { transform:rotate(90deg); }
  .node.open > .nbody { display:block; }
  .subhead { margin:8px 0 4px; font-size:11px; letter-spacing:.07em;
             text-transform:uppercase; color:var(--orca-muted); }

  .banner { border:1px solid var(--orca-border); border-left:3px solid var(--orca-accent);
            border-radius:6px; padding:9px 12px; margin-bottom:12px; font-size:12px; }
  .muted  { color:var(--orca-muted); }
  .mono   { font-family:var(--mono); }

  canvas.preview { width:100%; height:160px; border:1px solid var(--orca-border);
                   border-radius:6px; }
  .previews { display:grid; grid-template-columns:1fr 1fr; gap:10px; margin-top:8px; }
  .previews .cap { font-size:11px; color:var(--orca-muted); text-align:center; margin-top:2px; }

  .log { font-family:var(--mono); font-size:12px; border:1px solid var(--orca-border);
         border-radius:6px; padding:8px 10px; max-height:190px; overflow:auto; }
  .log div { padding:1px 0; }
  fieldset { border:1px solid var(--orca-border); border-radius:6px; margin:0 0 10px; }
  legend { color:var(--orca-muted); font-size:11px; padding:0 6px; }
  .frow { display:flex; gap:8px; align-items:center; flex-wrap:wrap; margin:6px 0; }
  label { font-size:12px; color:var(--orca-muted); }
</style>
</head>
<body>
  <header>
    <h1>Orca Inspector</h1>
    <span id="stamp"></span>
    <button class="small" onclick="refresh()">Refresh</button>
    <button class="small secondary" onclick="orca.close()">Close</button>
  </header>
  <main>
    <nav id="nav"></nav>
    <div id="content"></div>
  </main>

<script>
'use strict';
/* ------------------------------------------------------------------ state */
const SECTIONS = [
  ['overview', '◎', 'Overview'],
  ['presets',  '⚙', 'Presets'],
  ['config',   '≡', 'Config'],
  ['model',    '▦', 'Model'],
  ['assembly', '⬡', 'Assembly'],
  ['uikit',    '▣', 'UI Toolkit'],
];
const S = { current:'overview', cache:{}, busy:{}, colors:[], uiLog:[] };

const $ = id => document.getElementById(id);
const esc = s => String(s == null ? '' : s)
  .replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const num = (x, d=2) => Number(x).toFixed(d).replace(/\.?0+$/, '');
const deg = r => num(r * 180 / Math.PI, 1) + '°';
const vec3 = (v, d=2) => v ? '(' + v.map(c => num(c, d)).join(', ') + ')' : '—';
const yn  = b => b ? 'yes' : 'no';
const badge = (label, on) => '<span class="badge' + (on ? ' on' : '') + '">' + esc(label) + '</span>';
const kv = rows => '<div class="kv">' + rows.map(([k, v]) =>
  '<div class="k">' + esc(k) + '</div><div class="v">' + v + '</div>').join('') + '</div>';
const bboxRows = (label, bb) => bb ? [
  [label + ' min', esc(vec3(bb.min))], [label + ' max', esc(vec3(bb.max))],
  [label + ' size', esc(vec3(bb.size))], [label + ' center', esc(vec3(bb.center))],
] : [[label, '<span class="muted">undefined</span>']];

/* ------------------------------------------------------- nav + fetching */
function buildNav() {
  $('nav').innerHTML = SECTIONS.map(([key, glyph, label]) =>
    '<div class="item" id="nav-' + key + '" onclick="show(\'' + key + '\')">' +
    '<span class="glyph">' + glyph + '</span>' + label + '</div>').join('');
}
function show(key) {
  S.current = key;
  SECTIONS.forEach(([k]) => $('nav-' + k).classList.toggle('active', k === key));
  if (key === 'uikit') { render(); return; }         // local, nothing to fetch
  if (S.cache[key]) render();
  else if (S.busy[key]) $('content').innerHTML = '<p class="muted">Loading ' + esc(key) + '…</p>';
  else fetchSection(key);
}
function fetchSection(key) {
  S.busy[key] = true;
  $('content').innerHTML = '<p class="muted">Loading ' + esc(key) + '…</p>';
  orca.postMessage({ command:'fetch', section:key });
}
function refresh() {
  S.cache = {};
  S.busy = {};
  show(S.current);
}
function stamp() {
  $('stamp').textContent = 'updated ' + new Date().toLocaleTimeString();
}

/* ------------------------------------------------------------ renderers */
function render() {
  const key = S.current, d = S.cache[key];
  if (key === 'uikit') { $('content').innerHTML = renderUikit(); paintUiLog(); return; }
  if (!d) return;
  if (!d.ok) {
    $('content').innerHTML = '<div class="banner">⚠ ' + esc(key) +
      ' failed: <span class="mono">' + esc(d.error) + '</span></div>' +
      '<p class="muted">Is a project open? Click Refresh to retry.</p>';
    return;
  }
  const r = { overview:renderOverview, presets:renderPresets, config:renderConfig,
              model:renderModel, assembly:renderAssembly }[key];
  $('content').innerHTML = r(d.data);
  if (key === 'model') drawPendingPreviews();
}

function renderOverview(d) {
  S.colors = d.setup.filaments.map(f => f.color);   // reused by the model tree
  const t = d.stats;
  const tiles = [
    [t.objects, 'objects'], [t.volumes, 'volumes'], [t.instances, 'instances'],
    [t.triangles.toLocaleString(), 'triangles'], [t.materials, 'materials'],
    [t.plate_index, 'plate index'], [num(t.max_z) + ' mm', 'max Z'],
    [t.config_keys, 'config keys'],
  ].map(([v, l]) => '<div class="tile"><div class="v">' + v + '</div><div class="l">' + l + '</div></div>').join('');

  const filaments = d.setup.filaments.map((f, i) =>
    '<div style="margin:3px 0">' +
    (f.color ? '<span class="swatch" style="background:' + esc(f.color) + '"></span>' : '') +
    '<b>' + (i + 1) + '</b> ' + esc(f.name) +
    (f.type ? ' <span class="muted">' + esc(f.type) + '</span>' : '') +
    badge(f.is_system ? 'system' : 'user', f.is_system) +
    (f.is_dirty ? badge('modified', true) : '') +
    '</div>').join('') || '<span class="muted">none</span>';

  return '<div class="tiles">' + tiles + '</div><div class="cards">' +
    '<div class="card"><h3>Plater · orca.host.plater()</h3>' + kv([
      ['project dirty', yn(d.plater.project_dirty)],
      ['presets dirty', yn(d.plater.presets_dirty)],
      ['in snapshot', yn(d.plater.in_snapshot)],
      ['printable objects', t.printable_objects + ' / ' + t.objects],
    ]) + '</div>' +
    '<div class="card"><h3>Current setup</h3>' + kv([
      ['printer', esc(d.setup.printer.name) + (d.setup.printer.is_dirty ? ' ' + badge('modified', true) : '')],
      ['process', esc(d.setup.process.name) + (d.setup.process.is_dirty ? ' ' + badge('modified', true) : '')],
    ]) + '<div class="subhead">Filaments</div>' + filaments + '</div>' +
    '<div class="card"><h3>Scene bounds · model.bounding_box()</h3>' +
      kv([...bboxRows('exact', d.bbox),
          ['approx size', esc(vec3(d.bbox_approx && d.bbox_approx.size))],
          ['radius', d.bbox ? num(d.bbox.radius) + ' mm' : '—']]) + '</div>' +
    '<div class="card"><h3>Design &amp; painting</h3>' + kv([
      ['designer', esc(d.design.designer) || '<span class="muted">—</span>'],
      ['design id', esc(d.design.design_id) || '<span class="muted">—</span>'],
      ['model id', esc(d.design.model_id)],
      ['painted', badge('support', d.painted.support) + badge('seam', d.painted.seam) +
                  badge('multimaterial', d.painted.multimaterial) + badge('fuzzy skin', d.painted.fuzzy_skin)],
    ]) + '</div>' +
    '<div class="card wide"><h3>Config highlights · full_config_value()</h3>' +
      kv(d.setup.highlights.map(h => [h.k, esc(h.v)])) +
      '<p class="muted" style="margin:8px 0 0">The complete merged configuration (' +
      t.config_keys + ' keys) is on the Config tab.</p></div>' +
    '</div>';
}

function presetBadges(p) {
  return badge(p.is_system ? 'system' : 'user', p.is_system) +
    (p.is_dirty ? badge('modified', true) : '') +
    (p.is_default ? badge('default', false) : '') +
    (p.is_external ? badge('external', false) : '') +
    (p.is_project_embedded ? badge('project', false) : '') +
    (p.is_from_bundle ? badge('bundle', false) : '') +
    badge(p.is_compatible ? 'compatible' : 'incompatible', false) +
    (p.is_visible ? '' : badge('hidden', false));
}
function renderPresets(d) {
  return '<div class="banner">Each card is one <span class="mono">host.PresetCollection</span>. ' +
    '“Edited” is the selected preset plus any unsaved changes. Pick any preset ' +
    'and load its complete config via <span class="mono">find_preset().config_value()</span>.</div>' +
    '<div class="cards">' + d.collections.map(c => {
      const p = c.edited;
      const options = c.names.map(n =>   // explicit value= so odd whitespace in names survives
        '<option value="' + esc(n) + '"' + (n === c.selected_name ? ' selected' : '') + '>' +
        esc(n) + '</option>').join('');
      return '<div class="card"><h3>' + esc(c.label) + ' · ' + c.size + ' presets</h3>' +
        '<div style="margin-bottom:6px"><b>' + esc(p.name) + '</b> ' + presetBadges(p) + '</div>' +
        kv([['label()', esc(p.label)], ['alias', esc(p.alias) || '—'],
            ['type', esc(p.type)], ['bundle', esc(p.bundle_id) || '—'],
            ['edited_preset()', p.config_key_count + ' keys' +
              (p.is_dirty ? ' · has unsaved changes' : ' · same as saved')],
            ['selected_preset()', esc(c.selected.name) + ' · ' + c.selected.config_key_count + ' keys'],
            ['file', '<span class="muted">' + esc(p.file) + '</span>']]) +
        '<div class="frow" style="margin-top:8px">' +
          '<select id="sel-' + c.key + '" style="flex:1;min-width:0">' + options + '</select>' +
          '<button class="small" data-act="pcfg" data-coll="' + c.key + '">View config</button>' +
        '</div>' +
        (c.truncated ? '<p class="muted">list capped, ' + c.truncated + ' more not shown</p>' : '') +
        '<div id="pcfg-' + c.key + '"></div></div>';
    }).join('') + '</div>';
}

function configTable(rows, id) {
  const body = rows.map(r => {
    const long = r.v.length > 160;
    const shown = long ? esc(r.v.slice(0, 160)) + '<span class="more" data-act="expand"> … show all</span>'
                       : esc(r.v);
    return '<tr data-k="' + esc(r.k.toLowerCase()) + '"><td>' + esc(r.k) + '</td>' +
      '<td class="val" data-full="' + esc(r.v) + '">' + shown + '</td></tr>';
  }).join('');
  return '<table class="cfg" id="' + id + '"><tr><th>key</th><th>value</th></tr>' + body + '</table>';
}
function renderConfig(d) {
  return '<div class="toolbar"><input id="cfg-search" placeholder="Filter ' + d.count +
    ' keys…" oninput="filterConfig(this.value)">' +
    '<span class="count" id="cfg-count">' + d.count + ' keys</span></div>' +
    '<div class="banner">The merged result of printer + process + filament presets — ' +
    'exactly what <span class="mono">preset_bundle().full_config_value(key)</span> returns for ' +
    'every key in <span class="mono">full_config_keys()</span>.</div>' +
    configTable(d.rows, 'cfg-table');
}
function filterConfig(q) {
  q = q.trim().toLowerCase();
  let visible = 0;
  document.querySelectorAll('#cfg-table tr[data-k]').forEach(tr => {
    const hit = !q || tr.dataset.k.includes(q) ||
      tr.querySelector('.val').dataset.full.toLowerCase().includes(q);
    tr.style.display = hit ? '' : 'none';
    if (hit) visible++;
  });
  $('cfg-count').textContent = visible + ' keys';
}

function extruderChip(id) {
  if (id == null || id < 1) return '<span class="badge">extruder —</span>';
  const color = S.colors[id - 1];
  return '<span class="badge">' + (color ? '<span class="swatch" style="background:' +
    esc(color) + '"></span>' : '') + 'extruder ' + id + '</span>';
}
const VOL_GLYPH = { ModelPart:'◆', NegativeVolume:'◇', ParameterModifier:'▨',
                    SupportEnforcer:'▲', SupportBlocker:'▽' };

function node(head, body, open) {
  return '<div class="node' + (open ? ' open' : '') + '">' +
    '<div class="nhead" data-act="toggle"><span class="twist">▶</span>' + head + '</div>' +
    '<div class="nbody">' + body + '</div></div>';
}
function paintedBadges(p) {
  const parts = Object.entries(p).filter(([, v]) => v).map(([k]) => badge(k, true)).join('');
  return parts || '<span class="muted">none</span>';
}
function configOverride(rows) {
  if (!rows.length) return [['config overrides', '<span class="muted">none</span>']];
  return [['config overrides', rows.map(r => '<span class="badge">' + esc(r.k) + ' = ' +
    esc(r.v) + '</span>').join('')]];
}
function renderVolume(o, v) {
  const glyph = VOL_GLYPH[v.type] || '◆';
  const head = '<span>' + glyph + '</span><span class="nname">' + esc(v.name || '(volume ' + v.index + ')') +
    '</span>' + badge(v.type, false) + extruderChip(v.extruder) +
    '<span class="muted">' + v.facets.toLocaleString() + ' tris</span>' +
    (v.manifold ? '' : badge('⚠ non-manifold', false)) +
    (v.mesh_errors ? badge('⚠ ' + v.mesh_errors + ' repaired', false) : '');
  const body = kv([
    ['id', v.id], ['offset', esc(vec3(v.offset))],
    ['rotation', esc(v.rotation.map(deg).join(', '))],
    ['scale', esc(vec3(v.scale))], ['mirror', esc(vec3(v.mirror))],
    ['mesh volume', num(v.volume_mm3) + ' mm³'],
    ...bboxRows('local bbox', v.bbox),
    ['painted', paintedBadges(v.painted)],
    ...configOverride(v.config),
  ]) +
  '<div class="subhead">Mesh geometry · volume.mesh()</div>' +
  '<div id="mesh-' + o + '-' + v.index + '">' +
  '<button class="small secondary" data-act="mesh" data-o="' + o + '" data-v="' + v.index + '">' +
  'Load mesh detail</button> <span class="muted">zero-copy numpy views + world-space preview</span></div>';
  return node(head, body, false);
}
function renderInstance(o, i) {
  const a = i.assemble;
  const asmRows = !a.available
    ? [['assemble', '<span class="muted">needs a newer OrcaSlicer build</span>']]
    : [['assemble init', yn(a.initialized)],
       ['assemble offset', esc(vec3(a.offset))],
       ['assemble rotation', esc(a.rotation.map(deg).join(', '))],
       ['offset to assembly', esc(vec3(a.offset_to_assembly))]];
  const head = '<span>⌗</span><span class="nname">Instance ' + i.index + '</span>' +
    badge(i.is_printable ? 'printable' : 'not printable', i.is_printable) +
    '<span class="muted">at ' + esc(vec3(i.offset, 1)) + '</span>';
  const body = kv([
    ['id', i.id], ['printable flag', yn(i.printable_flag)],
    ['rotation', esc(i.rotation.map(deg).join(', '))],
    ['scale', esc(vec3(i.scale))], ['mirror', esc(vec3(i.mirror))],
    ['left-handed', yn(i.left_handed)],
    ...bboxRows('world bbox', i.bbox), ...asmRows,
  ]);
  return node(head, body, false);
}
// Effective printable state comes from the instances; the object flag is only
// the import-time value (the GUI's toggle never writes it).
function printableBadge(printable, total) {
  return badge(printable === 0 ? 'not printable'
    : printable < total ? printable + '/' + total + ' printable'
    : 'printable', printable > 0);
}
function renderObject(o) {
  const c = o.counts;
  const head = '<span>▦</span><span class="nname">' + esc(o.name || '(object ' + o.index + ')') + '</span>' +
    printableBadge(c.printable_instances, c.instances) +
    (o.flags.cut ? badge('cut', false) : '') +
    (o.flags.multiparts ? badge('multipart', false) : '') +
    (o.flags.custom_layering ? badge('custom layers', false) : '') +
    (c.mesh_errors ? badge('⚠ ' + c.mesh_errors + ' repaired', false) : '') +
    '<span class="muted">' + c.volumes + ' vol · ' + c.instances + ' inst · ' +
    c.facets.toLocaleString() + ' tris</span>';
  const body = kv([
    ['id', o.id],
    ['module', esc(o.module_name) || '<span class="muted">—</span>'],
    ['source file', '<span class="muted">' + (esc(o.input_file) || '—') + '</span>'],
    ['printable flag', yn(o.printable) +
      ' <span class="muted">import-time flag — the GUI toggle sets instances only</span>'],
    ['parts / materials', c.parts + ' / ' + c.materials],
    ['Z range', num(o.z_range[0]) + ' → ' + num(o.z_range[1]) + ' mm'],
    ...bboxRows('world bbox', o.bbox),
    ['raw size', esc(vec3(o.raw_bbox && o.raw_bbox.size))],
    ['painted', paintedBadges(o.painted)],
    ...configOverride(o.config),
  ]) +
  '<div class="subhead">Volumes (' + c.volumes + ')</div>' +
  o.volumes.map(v => renderVolume(o.index, v)).join('') +
  (o.volumes_omitted ? '<p class="muted">… ' + o.volumes_omitted + ' more not shown</p>' : '') +
  '<div class="subhead">Instances (' + c.instances + ')</div>' +
  o.instances.map(i => renderInstance(o.index, i)).join('') +
  (o.instances_omitted ? '<p class="muted">… ' + o.instances_omitted + ' more not shown</p>' : '');
  return node(head, body, o.index === 0);
}
function renderModel(d) {
  S.colors = d.filament_colors || S.colors;
  if (!d.object_count)
    return '<div class="banner">The plate is empty — add or open a model, then Refresh.</div>';
  return '<div class="cards" style="margin-bottom:12px"><div class="card wide"><h3>Model · orca.host.model()</h3>' +
    kv([['model id', d.id], ['objects', d.object_count], ['materials', d.material_count],
        ['plate index', d.plate_index], ['max Z', num(d.max_z) + ' mm'],
        ...bboxRows('bbox', d.bbox)]) + '</div></div>' +
    d.objects.map(renderObject).join('') +
    (d.objects_omitted ? '<p class="muted">… ' + d.objects_omitted + ' more object(s) not shown</p>' : '');
}

function renderAssemblyGroup(g, depth) {
  const objects = g.objects.map(o => {
    const inst = o.instances.map(i => {
      const a = i.assemble;
      const detail = !a.available ? '<span class="muted">assemble API needs a newer build</span>'
        : !a.initialized ? '<span class="muted">not initialized — no assemble placement recorded yet</span>'
        : 'offset ' + esc(vec3(a.offset, 1)) + ' · rot ' + esc(a.rotation.map(deg).join(', ')) +
          ' · to-assembly ' + esc(vec3(a.offset_to_assembly, 1));
      return '<div class="kv" style="margin:2px 0"><div class="k">instance ' + i.index +
        '</div><div class="v">' + detail + '</div></div>';
    }).join('') + (o.instances_omitted
      ? '<p class="muted">… ' + o.instances_omitted + ' more instance(s) not shown</p>' : '');
    const head = '<span>▦</span><span class="nname">' + esc(o.name) + '</span>' +
      (o.size ? '<span class="muted">' + esc(vec3(o.size, 1)) + ' mm</span>' : '') +
      printableBadge(o.printable_instances, o.instance_count);
    return node(head, inst || '<span class="muted">no instances</span>', false);
  }).join('');
  const groups = g.groups.map(child => renderAssemblyGroup(child, depth + 1)).join('');
  if (!g.name) return groups + objects;   // root: contents only
  const head = '<span>⬡</span><span class="nname">' + esc(g.name) + '</span>' +
    '<span class="muted">' + (g.objects.length + g.groups.length) + ' item(s)</span>';
  return node(head, groups + objects, depth < 2);
}
function renderAssembly(d) {
  let banner;
  if (!d.object_count)
    banner = 'The plate is empty — add or open a model, then Refresh.';
  else if (!d.has_modules)
    banner = 'No object carries a <span class="mono">module_name</span> — the hierarchy below is flat. ' +
      'Import a 3mf saved from an assembly (e.g. a STEP import) to see real modules.';
  else
    banner = 'Objects grouped by their <span class="mono">module_name</span>, the assembly path ' +
      'recorded in the project. Instances show their Assemble-view transforms.';
  return '<div class="banner">' + banner + '</div>' +
    renderAssemblyGroup(d.tree, 0) +
    (d.objects_omitted ? '<p class="muted">… ' + d.objects_omitted + ' more object(s) not shown</p>' : '');
}

/* --------------------------------------------------------- mesh previews */
const pendingPreviews = [];
const PREVIEWS = {};   // canvas id -> {pts, ax, ay}; repainted when a collapsed node reopens
function renderMeshDetail(container, d) {
  let html = kv([
    ['vertices', d.vertex_count.toLocaleString()],
    ['triangles', d.triangle_count.toLocaleString()],
    ['manifold', yn(d.manifold)], ['mesh volume', num(d.volume_mm3) + ' mm³'],
    ['vertex(0)', esc(vec3(d.vertex0, 3))], ['triangle(0)', esc(String(d.triangle0))],
    ...bboxRows('local bbox', d.bbox),
  ]);
  if (d.numpy) {
    html += '<div class="subhead">numpy arrays</div>' + kv([
      ['vertices()', esc(d.numpy.vertices)], ['triangles()', esc(d.numpy.triangles)],
      ['face_normals()', esc(d.numpy.normals)],
      ['surface area', num(d.numpy.surface_area_mm2) + ' mm²'],
      ...(d.numpy.world_bbox ? [
        ['world bbox min', esc(vec3(d.numpy.world_bbox.min))],
        ['world bbox max', esc(vec3(d.numpy.world_bbox.max))]] : []),
      ...(d.numpy.assemble_bbox ? [
        ['assemble bbox', esc(vec3(d.numpy.assemble_bbox.min)) + ' → ' +
                          esc(vec3(d.numpy.assemble_bbox.max)) + ' via assemble_matrix()']] : []),
    ]);
  } else {
    html += '<p class="muted">numpy not available — arrays, world-space math and previews are skipped.</p>';
  }
  if (d.preview) {
    const id = container.id;
    html += '<div class="previews"><div><canvas class="preview" id="' + id + '-top"></canvas>' +
      '<div class="cap">top (X/Y)</div></div><div><canvas class="preview" id="' + id + '-front"></canvas>' +
      '<div class="cap">front (X/Z)</div></div></div>' +
      '<p class="muted">' + d.preview.sampled.toLocaleString() + ' of ' +
      d.preview.total.toLocaleString() + ' world-space vertices, via zero-copy arrays and ' +
      'instance.matrix() @ volume.matrix()</p>';
    pendingPreviews.push([id, d.preview.points]);
  }
  container.innerHTML = html;
  drawPendingPreviews();
}
function drawPendingPreviews() {
  while (pendingPreviews.length) {
    const [id, pts] = pendingPreviews.pop();
    drawPoints($(id + '-top'), pts, 0, 1);
    drawPoints($(id + '-front'), pts, 0, 2);
  }
}
function drawPoints(canvas, pts, ax, ay) {
  if (!canvas || !pts.length) return;
  PREVIEWS[canvas.id] = { pts, ax, ay };
  const scaleDpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth * scaleDpr, h = canvas.clientHeight * scaleDpr;
  if (!w || !h) return;      // inside a collapsed node; repainted on toggle
  canvas.dataset.painted = '1';
  canvas.width = w; canvas.height = h;
  const ctx = canvas.getContext('2d');
  let minX = 1/0, maxX = -1/0, minY = 1/0, maxY = -1/0;
  for (const p of pts) {
    minX = Math.min(minX, p[ax]); maxX = Math.max(maxX, p[ax]);
    minY = Math.min(minY, p[ay]); maxY = Math.max(maxY, p[ay]);
  }
  const pad = 10 * scaleDpr;
  const scale = Math.min((w - 2 * pad) / Math.max(maxX - minX, 1e-6),
                         (h - 2 * pad) / Math.max(maxY - minY, 1e-6));
  const ox = (w - (maxX - minX) * scale) / 2, oy = (h - (maxY - minY) * scale) / 2;
  const style = getComputedStyle(document.body);
  ctx.strokeStyle = style.getPropertyValue('--orca-accent');
  ctx.strokeRect(ox, oy, (maxX - minX) * scale, (maxY - minY) * scale);
  ctx.fillStyle = style.color;
  ctx.globalAlpha = 0.55;
  const r = Math.max(1, 1.2 * scaleDpr);
  for (const p of pts)
    ctx.fillRect(ox + (p[ax] - minX) * scale - r / 2,
                 h - oy - (p[ay] - minY) * scale - r / 2, r, r);
}

/* ------------------------------------------------------------ UI toolkit */
function renderUikit() {
  return '<div class="banner">Live calls into <span class="mono">orca.host.ui</span>. Results appear in the ' +
    'event log; the plugin runs the progress demos on a worker thread and pushes results back with ' +
    '<span class="mono">win.post()</span>.</div>' +
    '<div class="cards">' +
    '<div class="card"><h3>Native message box · ui.message()</h3>' +
      '<div class="frow"><label>buttons</label><select id="ui-btns"><option>ok</option>' +
      '<option>ok_cancel</option><option selected>yes_no</option><option>yes_no_cancel</option></select>' +
      '<label>icon</label><select id="ui-icon"><option>info</option><option>warning</option>' +
      '<option>error</option><option selected>question</option></select></div>' +
      '<div class="frow"><input id="ui-text" style="flex:1" value="Shall we inspect some hosts?">' +
      '<button class="small" data-act="ui" data-ui="message">Show</button></div></div>' +
    '<div class="card"><h3>Modal HTML dialog · ui.create_window()</h3>' +
      '<p class="muted">Uses WINDOW_MODAL; messages and the final <span class="mono">orca.submit()</span> payload are callbacks.</p>' +
      '<button class="small" data-act="ui" data-ui="modal">Open modal dialog</button></div>' +
    '<div class="card"><h3>Progress dialog · ui.ProgressDialog</h3>' +
      '<p class="muted">Determinate update() loop (cancellable) and start_pulse()/stop_pulse().</p>' +
      '<div class="frow"><button class="small" data-act="ui" data-ui="progress">40-step demo</button>' +
      '<button class="small secondary" data-act="ui" data-ui="pulse">Pulse demo</button></div></div>' +
    '<div class="card"><h3>Second window · ui.create_window()</h3>' +
      '<p class="muted">A plugin can own several windows; the child echoes pings through the plugin.</p>' +
      '<div class="frow"><button class="small" data-act="ui" data-ui="child_open">Open child</button>' +
      '<button class="small secondary" data-act="ui" data-ui="child_close">Close child</button>' +
      '<button class="small secondary" data-act="ui" data-ui="child_state">is_open()?</button></div></div>' +
    '<div class="card wide"><h3>Event log</h3><div class="log" id="ui-log"></div></div>' +
    '</div>';
}
function addLog(text) {
  S.uiLog.push(new Date().toLocaleTimeString() + '  ' + text);
  if (S.uiLog.length > 80) S.uiLog.shift();
  paintUiLog();
}
function paintUiLog() {
  const el = $('ui-log');
  if (!el) return;
  el.innerHTML = S.uiLog.map(esc).map(l => '<div>' + l + '</div>').join('') ||
    '<span class="muted">no events yet</span>';
  el.scrollTop = el.scrollHeight;
}
function uiAction(action) {
  const msg = { command:'ui', action };
  if (action === 'message') {
    msg.text = $('ui-text').value;
    msg.buttons = $('ui-btns').value;
    msg.icon = $('ui-icon').value;
  }
  addLog('→ ' + action);
  orca.postMessage(msg);
}

/* -------------------------------------------------------- event delegation */
$('content').addEventListener('click', e => {
  const t = e.target.closest('[data-act]');
  if (!t) return;
  const act = t.dataset.act;
  if (act === 'toggle') {
    const node = t.parentElement;
    node.classList.toggle('open');
    // Repaint any preview whose canvas was hidden (zero-size) when its data arrived.
    node.querySelectorAll('canvas.preview').forEach(c => {
      const p = PREVIEWS[c.id];
      if (p && !c.dataset.painted) drawPoints(c, p.pts, p.ax, p.ay);
    });
  } else if (act === 'mesh') {
    t.disabled = true;
    t.textContent = 'Loading…';
    orca.postMessage({ command:'mesh', object:+t.dataset.o, volume:+t.dataset.v });
  } else if (act === 'pcfg') {
    const coll = t.dataset.coll;
    orca.postMessage({ command:'preset_config', collection:coll,
                       name:$('sel-' + coll).value });
  } else if (act === 'expand') {
    const td = t.closest('td');
    td.textContent = td.dataset.full;
  } else if (act === 'ui') {
    uiAction(t.dataset.ui);
  }
});

/* ------------------------------------------------------------- messages */
orca.onMessage(msg => {
  if (!msg || !msg.command) return;
  if (msg.command === 'section') {
    S.cache[msg.section] = msg;
    S.busy[msg.section] = false;
    stamp();
    if (msg.section === S.current) render();
  } else if (msg.command === 'mesh') {
    const container = $('mesh-' + msg.object + '-' + msg.volume);
    if (!container) return;
    if (msg.ok) renderMeshDetail(container, msg.data);
    else container.innerHTML = '<span class="muted">⚠ ' + esc(msg.error) + '</span>';
  } else if (msg.command === 'preset_config') {
    const container = $('pcfg-' + msg.collection);
    if (!container) return;
    container.innerHTML = msg.ok
      ? '<div class="subhead">' + esc(msg.name) + ' · ' + msg.data.rows.length +
        ' keys</div>' + configTable(msg.data.rows, 'pcfg-table-' + msg.collection)
      : '<span class="muted">⚠ ' + esc(msg.error) + '</span>';
  } else if (msg.command === 'ui_result') {
    addLog(msg.ok ? '← ' + msg.action + ': ' + msg.result
                  : '← ' + msg.action + ' failed: ' + msg.error);
  }
});

buildNav();
show('overview');
</script>
</body>
</html>
"""


# The modal page demoed from the UI Toolkit tab: create_window(style=WINDOW_MODAL)
# keeps the handle alive for callbacks; orca.submit(payload) invokes on_submit.
MODAL_PAGE = r"""<!DOCTYPE html>
<html><head><meta charset="utf-8"></head>
<body style="padding:18px">
  <h3 style="margin-top:0">Modal dialog</h3>
  <p>create_window(style=WINDOW_MODAL) keeps this page interactive until it submits or closes.</p>
  <p><input id="note" style="width:100%" value="hello from the modal"></p>
  <p style="text-align:right">
    <button style="background:transparent;color:var(--orca-fg);border-color:var(--orca-border)"
            onclick="orca.postMessage({live: document.getElementById('note').value})">Post while open</button>
    <button style="background:transparent;color:var(--orca-fg);border-color:var(--orca-border)"
            onclick="orca.close()">Cancel</button>
    <button onclick="orca.submit({note: document.getElementById('note').value})">Submit</button>
  </p>
</body></html>
"""

CHILD_PAGE = r"""<!DOCTYPE html>
<html><head><meta charset="utf-8"></head>
<body style="padding:18px">
  <h3 style="margin-top:0">Child window</h3>
  <p>Same create_window() API — one plugin, several windows.</p>
  <p><button onclick="orca.postMessage({command:'ping'})">Ping the plugin</button></p>
  <div id="log" style="color:var(--orca-muted)"></div>
  <script>
    var n = 0;
    orca.onMessage(function (msg) {
      if (msg && msg.command === 'pong')
        document.getElementById('log').textContent = 'pong #' + (++n) + ' via child.post()';
    });
  </script>
</body></html>
"""


# --------------------------------------------------------------------------- #
# the plugin
# --------------------------------------------------------------------------- #
class OrcaInspectorPanel(orca.script.ScriptPluginCapabilityBase):
    win = None
    child = None
    modal = None

    def get_name(self):
        return "Orca Inspector"

    def execute(self):
        # Capability objects are instantiated once per plugin load, so a second
        # Run lands on the same instance — close any previous windows first, or
        # the old panel would keep posting into the new one.
        if self.win is not None and self.win.is_open():
            self.win.close()
        if self.child is not None:
            self.child.close()
            self.child = None
        if self.modal is not None:
            self.modal.close()
            self.modal = None
        # Non-modal: returns immediately. The window is host-owned and lives on
        # after execute() returns; on_message keeps firing when the page posts.
        self.win = orca.host.ui.create_window(
            title="Orca Inspector",
            html=PAGE,
            width=940,
            height=680,
            on_message=self.on_message,
            on_close=self.on_close,
        )
        return orca.ExecutionResult.success("Orca Inspector opened.")

    # Called on the UI thread when the page posts a message. Every branch posts
    # a reply carrying ok/error so the page never waits on a silent failure.
    def on_message(self, msg):
        msg = msg or {}
        command = msg.get("command")
        if command == "fetch":
            self.send_section(msg.get("section", ""))
        elif command == "mesh":
            self.send_mesh(int(msg.get("object", -1)), int(msg.get("volume", -1)))
        elif command == "preset_config":
            self.send_preset_config(msg.get("collection", ""), msg.get("name", ""))
        elif command == "ui":
            self.run_ui_action(msg)

    def on_close(self):
        if self.child is not None:
            self.child.close()
            self.child = None
        print("Orca Inspector closed")

    def send_section(self, section):
        builder = SECTION_BUILDERS.get(section)
        reply = {"command": "section", "section": section}
        if builder is None:
            self.win.post({**reply, "ok": False, "error": f"unknown section {section!r}"})
            return
        try:
            self.win.post({**reply, "ok": True, "data": builder()})
        except Exception as exc:
            self.win.post({**reply, "ok": False, "error": str(exc)})

    def send_mesh(self, object_index, volume_index):
        reply = {"command": "mesh", "object": object_index, "volume": volume_index}
        try:
            self.win.post({**reply, "ok": True,
                           "data": build_mesh(object_index, volume_index)})
        except Exception as exc:
            self.win.post({**reply, "ok": False, "error": str(exc)})

    def send_preset_config(self, collection, name):
        reply = {"command": "preset_config", "collection": collection, "name": name}
        try:
            self.win.post({**reply, "ok": True,
                           "data": build_preset_config(collection, name)})
        except Exception as exc:
            self.win.post({**reply, "ok": False, "error": str(exc)})

    # ---------------------------------------------------------------- ui demos
    def run_ui_action(self, msg):
        action = msg.get("action", "")
        reply = {"command": "ui_result", "action": action}

        def report(result):
            self.win.post({**reply, "ok": True, "result": result})

        try:
            if action == "message":
                clicked = orca.host.ui.message(
                    msg.get("text") or "Hello from Orca Inspector",
                    title="Orca Inspector",
                    buttons=msg.get("buttons", "ok"),
                    icon=msg.get("icon", "info"))
                report(f"user clicked {clicked!r}")
            elif action == "modal":
                if self.modal is not None and self.modal.is_open():
                    report("modal already open")
                    return
                # The modal window is created asynchronously on the UI thread,
                # but the handle remains usable for post()/close() and callbacks.
                self.modal = orca.host.ui.create_window(
                    html=MODAL_PAGE, title="Orca Inspector — modal", width=420, height=280,
                    style=orca.host.ui.WINDOW_MODAL,
                    on_message=lambda m: self.win.post(
                        {**reply, "ok": True, "result": f"modal posted {m!r} while open"}),
                    on_submit=lambda result: self.win.post(
                        {**reply, "ok": True, "result": f"modal submitted {result!r}"}),
                    on_close=lambda: self.win.post(
                        {**reply, "ok": True, "result": "modal closed"}))
                report("modal opened")
            elif action == "progress":
                threading.Thread(target=self.progress_demo, daemon=True).start()
                report("started on a worker thread…")
            elif action == "pulse":
                threading.Thread(target=self.pulse_demo, daemon=True).start()
                report("started on a worker thread…")
            elif action == "child_open":
                if self.child is not None and self.child.is_open():
                    report("child already open")
                else:
                    self.child = orca.host.ui.create_window(
                        title="Orca Inspector — child", html=CHILD_PAGE,
                        width=380, height=240, on_message=self.on_child_message,
                        on_close=lambda: self.win.post(
                            {**reply, "ok": True, "result": "child window closed"}))
                    report("child opened")
            elif action == "child_close":
                if self.child is not None:
                    self.child.close()
                else:
                    report("no child window")
            elif action == "child_state":
                is_open = self.child is not None and self.child.is_open()
                report(f"child.is_open() = {is_open}")
            else:
                self.win.post({**reply, "ok": False, "error": f"unknown action {action!r}"})
        except Exception as exc:
            self.win.post({**reply, "ok": False, "error": str(exc)})

    def on_child_message(self, msg):
        if (msg or {}).get("command") == "ping":
            self.child.post({"command": "pong"})
            self.win.post({"command": "ui_result", "action": "child",
                           "ok": True, "result": "child pinged; replied with pong"})

    # Both demos run on a worker thread: ProgressDialog calls marshal to the UI
    # thread internally, and win.post() is thread-safe, so the app stays live.
    def progress_demo(self):
        style = (orca.host.ui.PD_APP_MODAL | orca.host.ui.PD_AUTO_HIDE |
                 orca.host.ui.PD_CAN_ABORT | orca.host.ui.PD_ELAPSED_TIME |
                 orca.host.ui.PD_ESTIMATED_TIME | orca.host.ui.PD_REMAINING_TIME)
        outcome = "completed 40 steps"
        with orca.host.ui.create_progress_dialog(
                "Orca Inspector", "Crunching very important numbers…",
                maximum=40, style=style) as progress:
            for step in range(1, 41):
                time.sleep(0.05)
                if not progress.update(step, f"Step {step}/40"):
                    outcome = f"cancelled at step {step}"
                    break
        self.win.post({"command": "ui_result", "action": "progress",
                       "ok": True, "result": outcome})

    def pulse_demo(self):
        # ui.ProgressDialog(...) is the constructor form of create_progress_dialog().
        with orca.host.ui.ProgressDialog(
                "Orca Inspector", "Waiting for something indeterminate…",
                style=orca.host.ui.PD_APP_MODAL | orca.host.ui.PD_AUTO_HIDE) as progress:
            for _ in range(10):                     # manual single-shot pulses...
                time.sleep(0.1)
                if not progress.pulse("Manual pulse()…"):
                    break
            progress.start_pulse(80, "Host-timed start_pulse()…")   # ...then host-timed
            time.sleep(1.5)
            progress.stop_pulse()
            was_open = progress.is_open()
        self.win.post({"command": "ui_result", "action": "pulse", "ok": True,
                       "result": f"pulsed manually + via timer (is_open while shown: {was_open})"})


@orca.plugin
class OrcaInspectorPlugin(orca.base):
    def register_capabilities(self):
        orca.register_capability(OrcaInspectorPanel)
