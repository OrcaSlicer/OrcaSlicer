#!/usr/bin/env python3
"""Preview co-extrusion surface colours directly from OrcaSlicer G-code.

The displayed colour is reconstructed from the physical C-axis angle and the
co-extrusion profile. ``coextrusion_color_id`` comments are deliberately not
used as colour input.

Usage:
    python scripts/coextrusion_gcode_preview.py path/to/file.gcode

No third-party packages are required. With no path, a file chooser is shown.
"""

from __future__ import annotations

import math
import re
import sys
import tkinter as tk
from dataclasses import dataclass
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from typing import Dict, Iterable, List, Optional, Tuple


Point = Tuple[float, float, float]

MOVE_RE = re.compile(r"^\s*(G0|G1|G2|G3)\b", re.IGNORECASE)
WORD_RE = re.compile(r"([A-Z])\s*(-?(?:\d+(?:\.\d*)?|\.\d+))", re.IGNORECASE)
PROFILE_RE = re.compile(r"^;\s*coextrusion_profile_(\d+)\s*=\s*(.+)$", re.IGNORECASE)
CALIBRATION_RE = re.compile(
    r"^;\s*coextrusion_calibration_(\d+)\s*=\s*(-?(?:\d+(?:\.\d*)?|\.\d+))",
    re.IGNORECASE,
)
AXIS_RE = re.compile(r"^;\s*coextrusion_axis\s*=\s*([A-Z])\s*$", re.IGNORECASE)
AXIS_DIRECTION_RE = re.compile(
    r"^;\s*coextrusion_axis_direction\s*=\s*(-?1)\s*$", re.IGNORECASE
)
ZERO_OFFSET_RE = re.compile(
    r"^;\s*coextrusion_axis_zero_offset\s*=\s*(-?(?:\d+(?:\.\d*)?|\.\d+))",
    re.IGNORECASE,
)
LAYER_Z_RE = re.compile(r"^;Z:\s*(-?(?:\d+(?:\.\d*)?|\.\d+))", re.IGNORECASE)
WIDTH_RE = re.compile(r"^;WIDTH:\s*(\d+(?:\.\d*)?|\.\d+)", re.IGNORECASE)
TYPE_RE = re.compile(r"^;TYPE:\s*(.+)$", re.IGNORECASE)
TOOL_RE = re.compile(r"^T(\d+)\b", re.IGNORECASE)

FALLBACK_COLORS = (
    "#e74c3c", "#2ecc71", "#3498db", "#f1c40f", "#9b59b6",
    "#1abc9c", "#e67e22", "#ecf0f1", "#ff66cc", "#8bc34a",
)
NO_SECTOR_COLOR = "#ff4fd8"
UNCONTROLLED_COLOR = "#4a5059"


@dataclass(frozen=True)
class ColorSector:
    color_id: int
    color: str
    center_deg: float
    width_deg: float


@dataclass
class Segment:
    start: Point
    end: Point
    layer: int
    width: float
    feature_type: str
    tool: int
    physical_c_start: Optional[float]
    physical_c_end: Optional[float]
    surface_azimuth_deg: Optional[float] = None

    @property
    def angle_controlled(self) -> bool:
        return self.physical_c_start is not None and self.physical_c_end is not None


@dataclass(frozen=True)
class DrawSegment:
    start: Point
    end: Point
    layer: int
    width: float
    color: str
    angle_controlled: bool


@dataclass
class GCodeModel:
    segments: List[Segment]
    profiles: Dict[int, List[ColorSector]]
    calibrations: Dict[int, float]
    layer_z: Dict[int, float]
    axis: str
    axis_direction: int
    axis_zero_offset_deg: float

    @property
    def layer_ids(self) -> List[int]:
        return sorted({segment.layer for segment in self.segments if segment.layer >= 0})

    @property
    def angle_controlled_count(self) -> int:
        return sum(segment.angle_controlled for segment in self.segments)

    def profile_for_tool(self, tool: int) -> List[ColorSector]:
        if tool in self.profiles:
            return self.profiles[tool]
        if 0 in self.profiles:
            return self.profiles[0]
        return next(iter(self.profiles.values()), [])

    def calibration_for_tool(self, tool: int) -> float:
        if tool in self.calibrations:
            return self.calibrations[tool]
        return self.calibrations.get(0, 0.0)

    def sector_at(
        self, tool: int, physical_c_deg: float, surface_azimuth_deg: float
    ) -> Optional[ColorSector]:
        # Forward model:
        # physical sector azimuth = sector center + calibration + zero
        #                           + axis direction * physical C angle
        material_angle = normalize_angle(
            surface_azimuth_deg
            - self.calibration_for_tool(tool)
            - self.axis_zero_offset_deg
            - self.axis_direction * physical_c_deg
        )
        matches = []
        for sector in self.profile_for_tool(tool):
            distance = circular_distance(material_angle, sector.center_deg)
            if sector.width_deg >= 360.0 or distance <= sector.width_deg * 0.5 + 1e-7:
                matches.append((distance, sector))
        return min(matches, key=lambda item: item[0])[1] if matches else None


def normalize_angle(angle: float) -> float:
    return angle % 360.0


def circular_distance(first: float, second: float) -> float:
    return abs((first - second + 180.0) % 360.0 - 180.0)


def parse_profile(value: str) -> List[ColorSector]:
    fields = value.strip().split(";")
    if not fields or fields[0].strip().lower() != "v1":
        return []
    sectors: List[ColorSector] = []
    for index, encoded_sector in enumerate(fields[1:]):
        parts = [part.strip() for part in encoded_sector.split(",")]
        if len(parts) < 4:
            continue
        try:
            color_id = int(parts[0])
            center_deg = float(parts[2])
            width_deg = max(0.0, float(parts[3]))
        except ValueError:
            continue
        color = parts[1]
        if not re.fullmatch(r"#[0-9a-fA-F]{6}(?:[0-9a-fA-F]{2})?", color):
            color = FALLBACK_COLORS[index % len(FALLBACK_COLORS)]
        sectors.append(ColorSector(color_id, color[:7], center_deg, width_deg))
    return sectors


def _same_path(previous: Segment, current: Segment) -> bool:
    if (
        previous.layer != current.layer
        or previous.tool != current.tool
        or previous.feature_type != current.feature_type
    ):
        return False
    tolerance = max(0.2, previous.width, current.width)
    return math.dist(previous.end, current.start) <= tolerance


def _is_top_feature(feature_type: str) -> bool:
    name = feature_type.lower()
    return "top" in name and ("surface" in name or "solid" in name)


def _is_bottom_feature(feature_type: str) -> bool:
    name = feature_type.lower()
    return "bottom" in name and ("surface" in name or "solid" in name)


def infer_surface_azimuths(segments: List[Segment]) -> None:
    """Infer horizontal surface directions from deposited path geometry."""
    layer_points: Dict[int, List[Point]] = {}
    for segment in segments:
        if segment.angle_controlled:
            layer_points.setdefault(segment.layer, []).extend((segment.start, segment.end))
    layer_centers = {
        layer: (
            sum(point[0] for point in points) / len(points),
            sum(point[1] for point in points) / len(points),
        )
        for layer, points in layer_points.items()
    }

    index = 0
    while index < len(segments):
        if not segments[index].angle_controlled:
            index += 1
            continue
        run = [segments[index]]
        next_index = index + 1
        while (
            next_index < len(segments)
            and segments[next_index].angle_controlled
            and _same_path(run[-1], segments[next_index])
        ):
            run.append(segments[next_index])
            next_index += 1

        points = [run[0].start] + [segment.end for segment in run]
        close_tolerance = max(0.25, max(segment.width for segment in run) * 1.5)
        closed = len(run) >= 3 and math.dist(points[0], points[-1]) <= close_tolerance
        signed_area = 0.0
        if closed:
            for first, second in zip(points, points[1:]):
                signed_area += first[0] * second[1] - second[0] * first[1]

        for segment in run:
            dx = segment.end[0] - segment.start[0]
            dy = segment.end[1] - segment.start[1]
            if abs(dx) + abs(dy) <= 1e-12:
                continue
            tangent = math.degrees(math.atan2(dy, dx))
            if _is_top_feature(segment.feature_type):
                segment.surface_azimuth_deg = normalize_angle(tangent + 180.0)
            elif _is_bottom_feature(segment.feature_type):
                segment.surface_azimuth_deg = normalize_angle(tangent)
            elif closed and abs(signed_area) > 1e-9:
                offset = -90.0 if signed_area > 0.0 else 90.0
                segment.surface_azimuth_deg = normalize_angle(tangent + offset)
            else:
                center = layer_centers.get(segment.layer, (0.0, 0.0))
                midpoint_x = (segment.start[0] + segment.end[0]) * 0.5
                midpoint_y = (segment.start[1] + segment.end[1]) * 0.5
                radial = math.degrees(math.atan2(midpoint_y - center[1], midpoint_x - center[0]))
                left = normalize_angle(tangent + 90.0)
                right = normalize_angle(tangent - 90.0)
                segment.surface_azimuth_deg = (
                    left if circular_distance(left, radial) <= circular_distance(right, radial)
                    else right
                )
        index = next_index


def parse_gcode(path: Path) -> GCodeModel:
    segments: List[Segment] = []
    profiles: Dict[int, List[ColorSector]] = {}
    calibrations: Dict[int, float] = {}
    layer_z: Dict[int, float] = {}
    x = y = z = e = 0.0
    xyz_absolute = True
    e_absolute = True
    current_width = 0.4
    current_layer = -1
    current_type = ""
    current_tool = 0
    axis = "C"
    axis_direction = 1
    axis_zero_offset_deg = 0.0
    logical_c = 0.0
    physical_c = 0.0

    with path.open("r", encoding="utf-8", errors="replace") as stream:
        for raw_line in stream:
            line = raw_line.strip()
            if not line:
                continue
            match = PROFILE_RE.match(line)
            if match:
                profiles[int(match.group(1))] = parse_profile(match.group(2))
                continue
            match = CALIBRATION_RE.match(line)
            if match:
                calibrations[int(match.group(1))] = float(match.group(2))
                continue
            match = AXIS_RE.match(line)
            if match:
                axis = match.group(1).upper()
                continue
            match = AXIS_DIRECTION_RE.match(line)
            if match:
                axis_direction = int(match.group(1))
                continue
            match = ZERO_OFFSET_RE.match(line)
            if match:
                axis_zero_offset_deg = float(match.group(1))
                continue
            match = TYPE_RE.match(line)
            if match:
                current_type = match.group(1).strip()
                continue
            if line.upper().startswith(";LAYER_CHANGE"):
                current_layer += 1
                continue
            match = LAYER_Z_RE.match(line)
            if match and current_layer >= 0:
                layer_z[current_layer] = float(match.group(1))
                continue
            match = WIDTH_RE.match(line)
            if match:
                current_width = float(match.group(1))
                continue

            command = line.split(";", 1)[0].strip()
            upper = command.upper()
            match = TOOL_RE.match(upper)
            if match:
                current_tool = int(match.group(1))
                continue
            if upper.startswith("G90"):
                xyz_absolute = True
                continue
            if upper.startswith("G91"):
                xyz_absolute = False
                continue
            if upper.startswith("M82"):
                e_absolute = True
                continue
            if upper.startswith("M83"):
                e_absolute = False
                continue
            if upper.startswith("G92"):
                words = {name.upper(): float(value) for name, value in WORD_RE.findall(command)}
                x = words.get("X", x)
                y = words.get("Y", y)
                z = words.get("Z", z)
                e = words.get("E", e)
                if axis in words:
                    # No slip ring: G92 changes coordinates, not physical rotation.
                    logical_c = words[axis]
                continue
            if not MOVE_RE.match(command):
                continue

            words = {name.upper(): float(value) for name, value in WORD_RE.findall(command)}

            def next_position(name: str, current: float) -> float:
                if name not in words:
                    return current
                return words[name] if xyz_absolute else current + words[name]

            next_x = next_position("X", x)
            next_y = next_position("Y", y)
            next_z = next_position("Z", z)
            physical_c_start = physical_c
            has_axis_word = axis in words
            if has_axis_word:
                if xyz_absolute:
                    physical_c += words[axis] - logical_c
                    logical_c = words[axis]
                else:
                    logical_c += words[axis]
                    physical_c += words[axis]

            extrusion = 0.0
            if "E" in words:
                extrusion = words["E"] - e if e_absolute else words["E"]
                e = words["E"] if e_absolute else e + words["E"]
            if extrusion > 1e-9 and current_layer >= 0:
                distance = math.dist((x, y, z), (next_x, next_y, next_z))
                if distance > 1e-9:
                    segments.append(Segment(
                        (x, y, z), (next_x, next_y, next_z), current_layer,
                        current_width, current_type, current_tool,
                        physical_c_start if has_axis_word else None,
                        physical_c if has_axis_word else None,
                    ))
            x, y, z = next_x, next_y, next_z

    infer_surface_azimuths(segments)
    return GCodeModel(
        segments, profiles, calibrations, layer_z, axis,
        axis_direction, axis_zero_offset_deg,
    )


def lerp_point(start: Point, end: Point, ratio: float) -> Point:
    return tuple(start[index] + (end[index] - start[index]) * ratio for index in range(3))


def split_at_sector_boundaries(
    model: GCodeModel, segment: Segment, flip_surface: bool
) -> List[DrawSegment]:
    if not segment.angle_controlled or segment.surface_azimuth_deg is None:
        return [DrawSegment(
            segment.start, segment.end, segment.layer, segment.width,
            NO_SECTOR_COLOR, True,
        )]
    assert segment.physical_c_start is not None
    assert segment.physical_c_end is not None
    surface_azimuth = segment.surface_azimuth_deg + (180.0 if flip_surface else 0.0)
    calibration = model.calibration_for_tool(segment.tool)
    local_start = (
        surface_azimuth - calibration - model.axis_zero_offset_deg
        - model.axis_direction * segment.physical_c_start
    )
    local_end = (
        surface_azimuth - calibration - model.axis_zero_offset_deg
        - model.axis_direction * segment.physical_c_end
    )
    cuts = {0.0, 1.0}
    delta = local_end - local_start
    if abs(delta) > 1e-12:
        low, high = sorted((local_start, local_end))
        for sector in model.profile_for_tool(segment.tool):
            for edge in (
                sector.center_deg - sector.width_deg * 0.5,
                sector.center_deg + sector.width_deg * 0.5,
            ):
                first_turn = math.floor((low - edge) / 360.0) - 1
                last_turn = math.ceil((high - edge) / 360.0) + 1
                for turn in range(first_turn, last_turn + 1):
                    ratio = (edge + turn * 360.0 - local_start) / delta
                    if 1e-9 < ratio < 1.0 - 1e-9:
                        cuts.add(ratio)
    ordered_cuts = sorted(cuts)
    result: List[DrawSegment] = []
    for start_ratio, end_ratio in zip(ordered_cuts, ordered_cuts[1:]):
        middle_ratio = (start_ratio + end_ratio) * 0.5
        physical_c = (
            segment.physical_c_start
            + (segment.physical_c_end - segment.physical_c_start) * middle_ratio
        )
        sector = model.sector_at(segment.tool, physical_c, surface_azimuth)
        result.append(DrawSegment(
            lerp_point(segment.start, segment.end, start_ratio),
            lerp_point(segment.start, segment.end, end_ratio),
            segment.layer, segment.width,
            sector.color if sector else NO_SECTOR_COLOR, True,
        ))
    return result


class PreviewApp:
    def __init__(self, root: tk.Tk, initial_path: Optional[Path]) -> None:
        self.root = root
        self.root.title("C-axis co-extrusion angle preview")
        self.root.geometry("1280x820")
        self.root.minsize(800, 520)
        self.model: Optional[GCodeModel] = None
        self.path: Optional[Path] = None
        self.layer_ids: List[int] = []
        self.view_mode = tk.StringVar(value="Layer")
        self.show_uncontrolled = tk.BooleanVar(value=False)
        self.flip_surface = tk.BooleanVar(value=False)
        self.layer_value = tk.DoubleVar(value=0.0)
        self.status = tk.StringVar(value="Open a G-code file")
        self.zoom = 1.0
        self.pan_x = 0.0
        self.pan_y = 0.0
        self.yaw = math.radians(-35.0)
        self.pitch = math.radians(55.0)
        self.drag_origin: Optional[Tuple[int, int]] = None
        self.drag_button = 0
        self._build_ui()
        self._bind_events()
        if initial_path:
            self.load(initial_path)
        else:
            self.root.after(50, self.open_file)

    def _build_ui(self) -> None:
        toolbar = ttk.Frame(self.root, padding=6)
        toolbar.pack(side=tk.TOP, fill=tk.X)
        ttk.Button(toolbar, text="Open G-code", command=self.open_file).pack(side=tk.LEFT)
        ttk.Button(toolbar, text="Fit", command=self.reset_view).pack(side=tk.LEFT, padx=(6, 12))
        ttk.Label(toolbar, text="View").pack(side=tk.LEFT)
        mode = ttk.Combobox(
            toolbar, textvariable=self.view_mode, values=("Layer", "3D"),
            width=7, state="readonly",
        )
        mode.pack(side=tk.LEFT, padx=(4, 12))
        mode.bind("<<ComboboxSelected>>", lambda _event: self.redraw())
        ttk.Checkbutton(
            toolbar, text="Show non-C extrusion", variable=self.show_uncontrolled,
            command=self.redraw,
        ).pack(side=tk.LEFT)
        ttk.Checkbutton(
            toolbar, text="Flip inferred surface", variable=self.flip_surface,
            command=self.redraw,
        ).pack(side=tk.LEFT, padx=(10, 0))
        self.layer_label = ttk.Label(toolbar, text="Layer -")
        self.layer_label.pack(side=tk.RIGHT)
        self.layer_scale = ttk.Scale(
            toolbar, from_=0, to=0, variable=self.layer_value,
            command=lambda _value: self.redraw(), length=280,
        )
        self.layer_scale.pack(side=tk.RIGHT, padx=8)
        body = ttk.Frame(self.root)
        body.pack(fill=tk.BOTH, expand=True)
        self.canvas = tk.Canvas(body, background="#171a1f", highlightthickness=0)
        self.canvas.pack(fill=tk.BOTH, expand=True)
        footer = ttk.Frame(self.root, padding=(8, 4))
        footer.pack(side=tk.BOTTOM, fill=tk.X)
        ttk.Label(footer, textvariable=self.status).pack(side=tk.LEFT)
        ttk.Label(
            footer,
            text="Wheel: zoom | left drag: pan/rotate | right drag: pan | R: reset",
        ).pack(side=tk.RIGHT)

    def _bind_events(self) -> None:
        self.canvas.bind("<Configure>", lambda _event: self.redraw())
        self.canvas.bind("<MouseWheel>", self._mouse_wheel)
        self.canvas.bind("<Button-4>", lambda event: self._zoom_at(event.x, event.y, 1.12))
        self.canvas.bind("<Button-5>", lambda event: self._zoom_at(event.x, event.y, 1.0 / 1.12))
        self.canvas.bind("<ButtonPress-1>", lambda event: self._drag_start(event, 1))
        self.canvas.bind("<ButtonPress-3>", lambda event: self._drag_start(event, 3))
        self.canvas.bind("<B1-Motion>", self._drag_move)
        self.canvas.bind("<B3-Motion>", self._drag_move)
        self.canvas.bind("<ButtonRelease-1>", self._drag_end)
        self.canvas.bind("<ButtonRelease-3>", self._drag_end)
        self.root.bind("r", lambda _event: self.reset_view())
        self.root.bind("R", lambda _event: self.reset_view())
        self.root.bind("<Control-o>", lambda _event: self.open_file())

    def open_file(self) -> None:
        filename = filedialog.askopenfilename(
            title="Open co-extrusion G-code",
            filetypes=(("G-code", "*.gcode *.gco *.gc"), ("All files", "*.*")),
        )
        if filename:
            self.load(Path(filename))

    def load(self, path: Path) -> None:
        try:
            model = parse_gcode(path)
        except OSError as error:
            messagebox.showerror("Read failed", str(error))
            return
        self.model = model
        self.path = path
        self.layer_ids = model.layer_ids
        self.layer_scale.configure(to=max(0, len(self.layer_ids) - 1))
        self.layer_value.set(max(0, len(self.layer_ids) - 1))
        self.root.title(f"{path.name} - C-axis angle preview")
        self.reset_view()
        self.status.set(
            f"{path} | {len(self.layer_ids)} layers | "
            f"{model.angle_controlled_count} angle-controlled / "
            f"{len(model.segments)} extrusion segments | "
            f"axis {model.axis}, direction {model.axis_direction:+d}"
        )
        if not model.profiles:
            messagebox.showwarning(
                "Missing co-extrusion profile",
                "No coextrusion_profile_N header was found; colours cannot be reconstructed.",
            )
        elif model.angle_controlled_count == 0:
            messagebox.showwarning(
                "No angle-controlled extrusion",
                f"No extruding move containing the configured {model.axis} axis was found.",
            )

    def reset_view(self) -> None:
        self.zoom = 1.0
        self.pan_x = 0.0
        self.pan_y = 0.0
        self.yaw = math.radians(-35.0)
        self.pitch = math.radians(55.0)
        self.redraw()

    def _mouse_wheel(self, event: tk.Event) -> None:
        self._zoom_at(event.x, event.y, 1.12 if event.delta > 0 else 1.0 / 1.12)

    def _zoom_at(self, x: int, y: int, factor: float) -> None:
        old_zoom = self.zoom
        self.zoom = min(30.0, max(0.08, self.zoom * factor))
        applied = self.zoom / old_zoom
        center_x = self.canvas.winfo_width() * 0.5
        center_y = self.canvas.winfo_height() * 0.5
        self.pan_x = (self.pan_x + center_x - x) * applied - (center_x - x)
        self.pan_y = (self.pan_y + center_y - y) * applied - (center_y - y)
        self.redraw()

    def _drag_start(self, event: tk.Event, button: int) -> None:
        self.drag_origin = (event.x, event.y)
        self.drag_button = button

    def _drag_move(self, event: tk.Event) -> None:
        if self.drag_origin is None:
            return
        dx = event.x - self.drag_origin[0]
        dy = event.y - self.drag_origin[1]
        self.drag_origin = (event.x, event.y)
        if self.view_mode.get() == "3D" and self.drag_button == 1:
            self.yaw += dx * 0.008
            self.pitch = min(math.radians(88.0), max(math.radians(5.0), self.pitch + dy * 0.008))
        else:
            self.pan_x += dx
            self.pan_y += dy
        self.redraw()

    def _drag_end(self, _event: tk.Event) -> None:
        self.drag_origin = None
        self.drag_button = 0

    def _visible_segments(self) -> List[DrawSegment]:
        if not self.model:
            return []
        segments: Iterable[Segment] = self.model.segments
        if self.view_mode.get() == "Layer" and self.layer_ids:
            index = min(len(self.layer_ids) - 1, max(0, int(round(self.layer_value.get()))))
            layer = self.layer_ids[index]
            segments = (segment for segment in segments if segment.layer == layer)
        result: List[DrawSegment] = []
        for segment in segments:
            if segment.angle_controlled:
                result.extend(split_at_sector_boundaries(
                    self.model, segment, self.flip_surface.get()
                ))
            elif self.show_uncontrolled.get():
                result.append(DrawSegment(
                    segment.start, segment.end, segment.layer, segment.width,
                    UNCONTROLLED_COLOR, False,
                ))
        return result

    def _project_3d(self, point: Point, center: Point) -> Point:
        dx, dy, dz = (point[index] - center[index] for index in range(3))
        cos_yaw, sin_yaw = math.cos(self.yaw), math.sin(self.yaw)
        cos_pitch, sin_pitch = math.cos(self.pitch), math.sin(self.pitch)
        screen_x = cos_yaw * dx - sin_yaw * dy
        depth = sin_yaw * dx + cos_yaw * dy
        screen_y = sin_pitch * depth - cos_pitch * dz
        camera_depth = cos_pitch * depth + sin_pitch * dz
        return screen_x, screen_y, camera_depth

    def redraw(self) -> None:
        self.canvas.delete("all")
        segments = self._visible_segments()
        if not segments:
            self.canvas.create_text(
                self.canvas.winfo_width() / 2, self.canvas.winfo_height() / 2,
                text="No visible angle-controlled extrusion in this view",
                fill="#aab2bd", font=("Arial", 15),
            )
            self.layer_label.configure(text="Layer -")
            return
        if self.view_mode.get() == "Layer":
            index = min(len(self.layer_ids) - 1, max(0, int(round(self.layer_value.get()))))
            layer = self.layer_ids[index]
            z_value = self.model.layer_z.get(layer) if self.model else None
            suffix = f" Z={z_value:g}" if z_value is not None else ""
            self.layer_label.configure(text=f"Layer {layer}{suffix}")
            projected = [
                ((segment.start[0], segment.start[1], 0.0),
                 (segment.end[0], segment.end[1], 0.0), segment)
                for segment in segments
            ]
        else:
            self.layer_label.configure(text=f"All {len(self.layer_ids)} layers")
            points = [point for segment in segments for point in (segment.start, segment.end)]
            center = tuple(
                (min(point[axis] for point in points) + max(point[axis] for point in points)) * 0.5
                for axis in range(3)
            )
            projected = [
                (self._project_3d(segment.start, center),
                 self._project_3d(segment.end, center), segment)
                for segment in segments
            ]
            projected.sort(key=lambda item: (item[0][2] + item[1][2]) * 0.5)

        min_x = min(min(start[0], end[0]) for start, end, _segment in projected)
        max_x = max(max(start[0], end[0]) for start, end, _segment in projected)
        min_y = min(min(start[1], end[1]) for start, end, _segment in projected)
        max_y = max(max(start[1], end[1]) for start, end, _segment in projected)
        canvas_width = max(1, self.canvas.winfo_width())
        canvas_height = max(1, self.canvas.winfo_height())
        fit_scale = min(
            (canvas_width - 50) / max(1e-6, max_x - min_x),
            (canvas_height - 50) / max(1e-6, max_y - min_y),
        )
        scale = fit_scale * self.zoom
        model_center_x = (min_x + max_x) * 0.5
        model_center_y = (min_y + max_y) * 0.5
        for start, end, segment in projected:
            x1 = canvas_width * 0.5 + self.pan_x + (start[0] - model_center_x) * scale
            y1 = canvas_height * 0.5 + self.pan_y - (start[1] - model_center_y) * scale
            x2 = canvas_width * 0.5 + self.pan_x + (end[0] - model_center_x) * scale
            y2 = canvas_height * 0.5 + self.pan_y - (end[1] - model_center_y) * scale
            width = max(2, min(7, segment.width * scale * 0.35)) if segment.angle_controlled else 1
            self.canvas.create_line(
                x1, y1, x2, y2, fill=segment.color, width=width, capstyle=tk.ROUND
            )
        self._draw_legend()

    def _draw_legend(self) -> None:
        if not self.model:
            return
        x, y = 18, 18
        seen = set()
        sectors = []
        for profile in self.model.profiles.values():
            for sector in profile:
                key = (sector.color_id, sector.color, sector.center_deg, sector.width_deg)
                if key not in seen:
                    seen.add(key)
                    sectors.append(sector)
        for sector in sorted(sectors, key=lambda item: item.color_id):
            self.canvas.create_rectangle(
                x, y, x + 18, y + 12, fill=sector.color, outline="#d7dce2"
            )
            self.canvas.create_text(
                x + 25, y + 6,
                text=f"sector {sector.color_id}: {sector.center_deg:g} deg / {sector.width_deg:g} deg",
                fill="#f1f3f5", anchor=tk.W,
            )
            y += 21
        self.canvas.create_rectangle(
            x, y, x + 18, y + 12, fill=NO_SECTOR_COLOR, outline="#d7dce2"
        )
        self.canvas.create_text(
            x + 25, y + 6, text="no matching angular sector",
            fill="#f1f3f5", anchor=tk.W,
        )


def main() -> int:
    initial_path = Path(sys.argv[1]).expanduser() if len(sys.argv) > 1 else None
    if initial_path and not initial_path.is_file():
        print(f"G-code file not found: {initial_path}", file=sys.stderr)
        return 2
    root = tk.Tk()
    PreviewApp(root, initial_path)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
