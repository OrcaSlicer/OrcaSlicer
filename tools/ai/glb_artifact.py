"""Local, bounded GLB handling for generation artifacts and OBJ quality evidence.

The textured GLB remains the product artifact. The OBJ is an analysis projection,
in Orca's Z-up millimetres, for the existing offline mesh/visual checks. No network
access or provider conversion is performed here.
"""
from __future__ import annotations

import base64
import copy
import io
import json
import math
from pathlib import Path
import struct
import tempfile
from dataclasses import dataclass

from PIL import Image

MAX_BYTES = 512 * 1024 * 1024
MAX_FACES = 2_000_000
MAX_VERTICES = 6_000_000
_COMPONENTS = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2), 5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4)}
_COUNTS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}
_IDENTITY = (1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1.)


class GlbError(ValueError):
    pass


def _int(value, label, maximum=MAX_BYTES):
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= maximum:
        raise GlbError(f"Invalid GLB {label}.")
    return value


def _item(items, index, label):
    index = _int(index, label)
    if not isinstance(items, list) or index >= len(items):
        raise GlbError(f"Invalid GLB {label} reference.")
    return items[index]


def _numbers(values, count, label):
    if not isinstance(values, (list, tuple)) or len(values) != count:
        raise GlbError(f"Invalid GLB {label}.")
    if any(isinstance(v, bool) or not isinstance(v, (float, int)) or not math.isfinite(v) for v in values):
        raise GlbError(f"Non-finite GLB {label}.")
    return tuple(float(v) for v in values)


def _multiply(a, b):
    # Column-major matrices, as specified by glTF.
    return tuple(sum(a[k * 4 + row] * b[col * 4 + k] for k in range(4)) for col in range(4) for row in range(4))


def _transform(matrix, point):
    return tuple(sum(matrix[col * 4 + row] * point[col] for col in range(3)) + matrix[12 + row] for row in range(3))


def _node_matrix(node):
    if "matrix" in node:
        matrix = _numbers(node["matrix"], 16, "node matrix")
        if any(abs(matrix[i]) > 1e-7 for i in (3, 7, 11)) or abs(matrix[15] - 1) > 1e-7:
            raise GlbError("GLB node matrix is not affine.")
        return matrix
    t = _numbers(node.get("translation", [0, 0, 0]), 3, "translation")
    s = _numbers(node.get("scale", [1, 1, 1]), 3, "scale")
    x, y, z, w = _numbers(node.get("rotation", [0, 0, 0, 1]), 4, "rotation")
    length = math.sqrt(x*x + y*y + z*z + w*w)
    if length < 1e-9:
        raise GlbError("GLB rotation is empty.")
    x, y, z, w = (v / length for v in (x, y, z, w))
    return ((1-2*y*y-2*z*z)*s[0], (2*x*y+2*z*w)*s[0], (2*x*z-2*y*w)*s[0], 0,
            (2*x*y-2*z*w)*s[1], (1-2*x*x-2*z*z)*s[1], (2*y*z+2*x*w)*s[1], 0,
            (2*x*z+2*y*w)*s[2], (2*y*z-2*x*w)*s[2], (1-2*x*x-2*y*y)*s[2], 0,
            *t, 1)


def _linear(v):
    return v / 12.92 if v <= .04045 else ((v + .055) / 1.055) ** 2.4


def _srgb(v):
    v = min(1., max(0., v))
    return 12.92 * v if v <= .0031308 else 1.055 * v ** (1 / 2.4) - .055


@dataclass
class Mesh:
    vertices: list
    colors: list
    faces: list


class Glb:
    def __init__(self, path):
        path = Path(path)
        if not 20 <= path.stat().st_size <= MAX_BYTES:
            raise GlbError("GLB file has an invalid size.")
        data = path.read_bytes()
        magic, version, length = struct.unpack_from("<4sII", data)
        if magic != b"glTF" or version != 2 or length != len(data):
            raise GlbError("Invalid or incomplete GLB 2.0 file.")
        chunks = []
        offset = 12
        while offset < length:
            if offset + 8 > length:
                raise GlbError("Truncated GLB chunk.")
            size, kind = struct.unpack_from("<II", data, offset)
            offset += 8
            if size % 4 or size > length - offset:
                raise GlbError("Invalid GLB chunk length.")
            chunks.append((kind, data[offset:offset + size]))
            offset += size
        if not chunks or chunks[0][0] != 0x4E4F534A or len(chunks[0][1]) > 32 * 1024 * 1024:
            raise GlbError("GLB JSON chunk is missing or too large.")
        try:
            self.doc = json.loads(chunks[0][1].decode("utf-8"))
        except (UnicodeError, ValueError) as exc:
            raise GlbError("Invalid GLB JSON.") from exc
        if not isinstance(self.doc, dict) or not isinstance(self.doc.get("asset"), dict):
            raise GlbError("GLB description must be an object with an asset version.")
        for field in ("nodes", "scenes", "meshes", "accessors", "bufferViews", "buffers", "materials", "textures", "samplers", "images"):
            entries = self.doc.get(field, [])
            if not isinstance(entries, list) or any(not isinstance(entry, dict) for entry in entries):
                raise GlbError(f"Invalid GLB {field} definitions.")
        if self.doc.get("asset", {}).get("version") != "2.0":
            raise GlbError("Only GLB 2.0 is supported.")
        extensions = self.doc.get("extensionsRequired", [])
        if not isinstance(extensions, list) or any(not isinstance(extension, str) for extension in extensions):
            raise GlbError("Invalid GLB required extensions.")
        required = set(extensions)
        unsupported = required - {"KHR_materials_unlit", "KHR_texture_transform", "KHR_mesh_quantization"}
        if unsupported:
            raise GlbError("Unsupported GLB extension: " + ", ".join(sorted(unsupported)))
        binaries = [value for kind, value in chunks[1:] if kind == 0x004E4942]
        if len(binaries) != 1 or len(self.doc.get("buffers", [])) != 1:
            raise GlbError("GLB must contain one embedded geometry buffer.")
        buffer = self.doc["buffers"][0]
        if "uri" in buffer:
            raise GlbError("External GLB buffers are not supported.")
        size = _int(buffer.get("byteLength"), "buffer size")
        if not size <= len(binaries[0]) <= size + 3:
            raise GlbError("GLB buffer is incomplete.")
        self.binary = binaries[0][:size]
        self._images = {}

    def view(self, index):
        view = _item(self.doc.get("bufferViews"), index, "buffer view")
        if view.get("buffer") != 0:
            raise GlbError("GLB buffer view references an external buffer.")
        start = _int(view.get("byteOffset", 0), "view offset")
        size = _int(view.get("byteLength"), "view length")
        if start + size > len(self.binary):
            raise GlbError("GLB buffer view exceeds the file.")
        return view, memoryview(self.binary)[start:start + size]

    def accessor(self, index):
        accessor = _item(self.doc.get("accessors"), index, "accessor")
        if "sparse" in accessor:
            raise GlbError("Sparse GLB accessors are not supported.")
        component = accessor.get("componentType")
        if component not in _COMPONENTS or accessor.get("type") not in _COUNTS:
            raise GlbError("Unsupported GLB accessor type.")
        count = _int(accessor.get("count"), "accessor count", MAX_VERTICES)
        code, width = _COMPONENTS[component]
        dimensions = _COUNTS[accessor["type"]]
        reader = struct.Struct("<" + code * dimensions)
        view, data = self.view(accessor.get("bufferView"))
        stride = _int(view.get("byteStride", reader.size), "accessor stride", 252)
        start = _int(accessor.get("byteOffset", 0), "accessor offset")
        if stride < reader.size or stride % width or start % width or start + max(0, count - 1) * stride + (reader.size if count else 0) > len(data):
            raise GlbError("GLB accessor exceeds its buffer view.")
        values = [reader.unpack_from(data, start + i * stride) for i in range(count)]
        if component == 5126 and any(not math.isfinite(v) for row in values for v in row):
            raise GlbError("GLB accessor contains a non-finite value.")
        if accessor.get("normalized"):
            divisors = {5120: 127, 5121: 255, 5122: 32767, 5123: 65535}
            if component not in divisors:
                raise GlbError("Invalid normalized GLB component.")
            values = [tuple(max(-1., v / divisors[component]) for v in row) for row in values]
        return values

    def roots(self):
        scenes = self.doc.get("scenes", [])
        if scenes:
            return _item(scenes, self.doc.get("scene", 0), "scene").get("nodes", [])
        nodes = self.doc.get("nodes", [])
        children = {child for node in nodes for child in node.get("children", [])}
        return [i for i in range(len(nodes)) if i not in children]

    def primitives(self):
        def visit(index, parent, ancestors):
            if index in ancestors or len(ancestors) > 128:
                raise GlbError("GLB scene contains a cycle or excessive depth.")
            node = _item(self.doc.get("nodes"), index, "node")
            if "skin" in node or node.get("weights"):
                raise GlbError("Animated/skinned GLB must be baked to a static mesh first.")
            matrix = _multiply(parent, _node_matrix(node))
            if "mesh" in node:
                mesh = _item(self.doc.get("meshes"), node["mesh"], "mesh")
                for primitive in mesh.get("primitives", []):
                    yield primitive, matrix
            for child in node.get("children", []):
                yield from visit(child, matrix, ancestors | {index})
        for root in self.roots():
            yield from visit(root, _IDENTITY, set())

    def image(self, index):
        if index not in self._images:
            definition = _item(self.doc.get("images"), index, "image")
            if "bufferView" in definition:
                data = bytes(self.view(definition["bufferView"])[1])
            else:
                uri = definition.get("uri", "")
                if not uri.startswith(("data:image/png;base64,", "data:image/jpeg;base64,")):
                    raise GlbError("GLB textures must be embedded PNG or JPEG images.")
                try:
                    data = base64.b64decode(uri.split(",", 1)[1], validate=True)
                except ValueError as exc:
                    raise GlbError("Invalid embedded GLB texture.") from exc
            try:
                with Image.open(io.BytesIO(data)) as image:
                    if image.format not in {"PNG", "JPEG"} or image.width * image.height > 64 * 1024 * 1024:
                        raise GlbError("Unsupported or oversized GLB texture.")
                    self._images[index] = image.convert("RGB")
            except (OSError, Image.DecompressionBombError) as exc:
                raise GlbError("Invalid embedded GLB texture.") from exc
        return self._images[index]

    def mesh(self, with_colors=True):
        vertices, colors, faces = [], [], []
        for primitive, matrix in self.primitives():
            if primitive.get("mode", 4) != 4 or primitive.get("targets"):
                raise GlbError("GLB must contain static triangle meshes.")
            attributes = primitive.get("attributes", {})
            positions = self.accessor(attributes.get("POSITION"))
            if any(len(row) != 3 for row in positions):
                raise GlbError("Invalid GLB positions.")
            offset = len(vertices)
            if offset + len(positions) > MAX_VERTICES:
                raise GlbError("GLB has too many vertices.")
            vertices.extend(_transform(matrix, p) for p in positions)
            if "indices" in primitive:
                index_accessor = _item(self.doc.get("accessors"), primitive["indices"], "indices")
                if index_accessor.get("type") != "SCALAR" or index_accessor.get("componentType") not in (5121, 5123, 5125) or index_accessor.get("normalized"):
                    raise GlbError("GLB indices must be unsigned scalar integers.")
                indices = [row[0] for row in self.accessor(primitive["indices"])]
            else:
                indices = list(range(len(positions)))
            if len(indices) % 3 or len(faces) + len(indices) // 3 > MAX_FACES:
                raise GlbError("Invalid or excessive GLB triangle count.")
            if any(isinstance(i, float) or i < 0 or i >= len(positions) for i in indices):
                raise GlbError("GLB triangle index is out of range.")
            determinant = (matrix[0]*(matrix[5]*matrix[10]-matrix[9]*matrix[6]) - matrix[4]*(matrix[1]*matrix[10]-matrix[9]*matrix[2]) + matrix[8]*(matrix[1]*matrix[6]-matrix[5]*matrix[2]))
            for i in range(0, len(indices), 3):
                face = tuple(offset + value for value in indices[i:i + 3])
                faces.append((face[0], face[2], face[1]) if determinant < 0 else face)
            if not with_colors:
                continue
            material = _item(self.doc.get("materials"), primitive["material"], "material") if "material" in primitive else {}
            pbr = material.get("pbrMetallicRoughness", {})
            factor = _numbers(pbr.get("baseColorFactor", [1, 1, 1, 1]), 4, "base color")
            vertex_colors = self.accessor(attributes["COLOR_0"]) if "COLOR_0" in attributes else None
            if vertex_colors is not None and (len(vertex_colors) != len(positions) or any(len(c) not in (3, 4) for c in vertex_colors)):
                raise GlbError("Invalid GLB vertex colors.")
            texture_info = pbr.get("baseColorTexture")
            uvs, texture, transform, sampler = None, None, {}, {}
            if texture_info:
                definition = _item(self.doc.get("textures"), texture_info.get("index"), "texture")
                texture = self.image(definition.get("source"))
                transform = texture_info.get("extensions", {}).get("KHR_texture_transform", {})
                channel = transform.get("texCoord", texture_info.get("texCoord", 0))
                if channel != 0:
                    raise GlbError("GLB color textures must use TEXCOORD_0 for local editing.")
                uvs = self.accessor(attributes.get(f"TEXCOORD_{channel}"))
                if len(uvs) != len(positions) or any(len(uv) != 2 for uv in uvs):
                    raise GlbError("Invalid GLB texture coordinates.")
                if "sampler" in definition:
                    sampler = _item(self.doc.get("samplers"), definition["sampler"], "sampler")
            for i in range(len(positions)):
                color = [1., 1., 1.]
                if texture is not None:
                    u, v = uvs[i]
                    sx, sy = _numbers(transform.get("scale", [1, 1]), 2, "texture scale")
                    tx, ty = _numbers(transform.get("offset", [0, 0]), 2, "texture offset")
                    angle = transform.get("rotation", 0.)
                    if not isinstance(angle, (float, int)) or not math.isfinite(angle):
                        raise GlbError("Invalid texture rotation.")
                    u, v = tx + math.cos(angle)*u*sx - math.sin(angle)*v*sy, ty + math.sin(angle)*u*sx + math.cos(angle)*v*sy
                    def wrap(value, mode):
                        if mode == 33071: return max(0., min(1., value))
                        if mode == 33648: return 1 - abs(value % 2 - 1)
                        if mode == 10497: return value % 1
                        raise GlbError("Unsupported texture wrapping mode.")
                    u, v = wrap(u, sampler.get("wrapS", 10497)), wrap(v, sampler.get("wrapT", 10497))
                    px = min(texture.width - 1, int(u * texture.width))
                    py = min(texture.height - 1, int(v * texture.height))
                    color = [_linear(c / 255.) for c in texture.getpixel((px, py))]
                colors.append(tuple(_srgb(color[c] * factor[c] * (vertex_colors[i][c] if vertex_colors else 1)) for c in range(3)))
        if not vertices or not faces or any(not math.isfinite(v) for point in vertices for v in point):
            raise GlbError("GLB contains no valid model geometry.")
        return Mesh(vertices, colors, faces)

    def save(self, path):
        encoded = json.dumps(self.doc, ensure_ascii=True, separators=(",", ":"), allow_nan=False).encode("ascii")
        encoded += b" " * (-len(encoded) % 4)
        binary = self.binary + b"\0" * (-len(self.binary) % 4)
        length = 12 + 8 + len(encoded) + 8 + len(binary)
        if length > MAX_BYTES:
            raise GlbError("Normalized GLB exceeds its size limit.")
        path = Path(path)
        temporary = path.with_name(path.name + ".part")
        with temporary.open("wb") as stream:
            stream.write(struct.pack("<4sII", b"glTF", 2, length))
            stream.write(struct.pack("<II", len(encoded), 0x4E4F534A))
            stream.write(encoded)
            stream.write(struct.pack("<II", len(binary), 0x004E4942))
            stream.write(binary)
        temporary.replace(path)


def write_analysis_obj(source, destination):
    mesh = Glb(source).mesh()
    path = Path(destination)
    temporary = None
    try:
        # Readers may reuse this cache as soon as it exists. Publish only the
        # complete projection, using a unique file on the same filesystem.
        with tempfile.NamedTemporaryFile(mode="w", encoding="ascii", newline="\n",
                                         dir=path.parent, prefix=f".{path.name}.",
                                         suffix=".part", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write("# GLB analysis projection: Z-up, millimetres, sRGB vertex colors\n")
            for (x, y, z), color in zip(mesh.vertices, mesh.colors):
                stream.write("v {:.9g} {:.9g} {:.9g} {:.6f} {:.6f} {:.6f}\n".format(x * 1000, -z * 1000, y * 1000, *color))
            for face in mesh.faces:
                stream.write("f {} {} {}\n".format(*(i + 1 for i in face)))
        temporary.replace(path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return destination


def prepare_generated_glb(source, destination, analysis_path, size_mm=100.):
    glb = Glb(source)
    mesh = glb.mesh(with_colors=False)
    minimum = [min(p[i] for p in mesh.vertices) for i in range(3)]
    maximum = [max(p[i] for p in mesh.vertices) for i in range(3)]
    span = max(maximum[i] - minimum[i] for i in range(3))
    if not math.isfinite(span) or span < 1e-9:
        raise GlbError("GLB model has invalid dimensions.")
    scale = size_mm / 1000 / span
    roots = list(glb.roots())
    glb.doc = copy.deepcopy(glb.doc)
    # Keep embedded geometry, textures and material data intact. Only a scene
    # root transform establishes a consistent print size in glTF's metre units.
    nodes = glb.doc.setdefault("nodes", [])
    root = len(nodes)
    nodes.append({"name": "Orca print placement", "children": roots, "scale": [scale] * 3,
                  "translation": [-(minimum[0] + maximum[0]) * .5 * scale, -minimum[1] * scale,
                                  -(minimum[2] + maximum[2]) * .5 * scale]})
    glb.doc["scenes"] = [{"nodes": [root]}]
    glb.doc["scene"] = 0
    glb.save(destination)
    write_analysis_obj(destination, analysis_path)
    return Path(destination)
