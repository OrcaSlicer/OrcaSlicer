#!/usr/bin/env python3
"""Recordings, as the test_recording entry of tools/mcp/test-entries/gui_tools.md designs them.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_recording.py [--out DIR]

What the machine's ffmpeg can encode decides each encode's checks: an encode it cannot make must be refused with the
app's reason, and the file checks it would have had print as SKIP. The last line names what could be encoded. The
recordings go to --out, else to a new temporary folder.
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import GENERIC_PLA, KLIPPER, KLIPPER_STD, WINDOW_SIZE, Checks, plates, run, start, until, wait  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CUBE = os.path.join(REPO, "resources", "handy_models", "OrcaCube_v2.drc")
PREFERENCES = "orca_command:open_preferences:orca"


class RecordingChecks(Checks):
    """Checks that can also be skipped: a skipped check prints `SKIP <name>: <why>` and is not counted."""

    def __init__(self):
        super().__init__()
        self.skipped = 0

    def skip(self, name, why):
        self.skipped += 1
        print(f"SKIP {name}: {why}", flush=True)


# ---- Reading the files ----

def _vint(data, pos, marker):
    """An EBML variable-length integer at `pos`: (value, length, all value bits set). An element id keeps its
    length marker (`marker`); a size drops it."""
    first, length, mask = data[pos], 1, 0x80
    while length <= 8 and not first & mask:
        length, mask = length + 1, mask >> 1
    value = first if marker else first & (mask - 1)
    for i in range(1, length):
        value = (value << 8) | data[pos + i]
    return value, length, not marker and value == (1 << (7 * length)) - 1


SEGMENT, INFO, TRACKS, TRACK, VIDEO, CLUSTER, GROUP = 0x18538067, 0x1549A966, 0x1654AE6B, 0xAE, 0xE0, 0x1F43B675, 0xA0
MASTERS = {SEGMENT, INFO, TRACKS, TRACK, VIDEO, CLUSTER}


def mkv(path):
    """A Matroska file as the app writes it: {codec, width, height, frame_ms (the default frame duration), blocks,
    seconds (the segment's duration)}."""
    with open(path, "rb") as f:
        data = f.read()
    found = {"blocks": 0}

    def uint(body, size):
        return int.from_bytes(data[body:body + size], "big")

    def walk(pos, end):
        while pos < end:
            eid, n1, _ = _vint(data, pos, True)
            size, n2, unknown = _vint(data, pos + n1, False)
            body = pos + n1 + n2
            stop = end if unknown else min(end, body + size)
            if eid in MASTERS:
                walk(body, stop)
            elif eid in (0xA3, GROUP):  # a SimpleBlock or a BlockGroup: one frame
                found["blocks"] += 1
            elif eid == 0x86:
                found["codec"] = data[body:stop].decode("ascii", "replace")
            elif eid == 0xB0:
                found["width"] = uint(body, size)
            elif eid == 0xBA:
                found["height"] = uint(body, size)
            elif eid == 0x23E383:
                found["frame_ms"] = uint(body, size) / 1e6
            elif eid == 0x2AD7B1:
                found["scale"] = uint(body, size)
            elif eid == 0x4489:
                found["duration"] = struct.unpack(">f" if size == 4 else ">d", data[body:stop])[0]
            pos = stop

    walk(0, len(data))
    seconds = found.pop("duration", 0) * found.pop("scale", 1000000) / 1e9
    return {**found, "seconds": seconds}


def mp4(path):
    """An MP4's first sample entry (hvc1 for H.265, avc1 for H.264) and its movie header's duration in seconds."""
    with open(path, "rb") as f:
        data = f.read()
    found = {}

    def walk(pos, end):
        while pos + 8 <= end:
            size, kind = struct.unpack(">I4s", data[pos:pos + 8])
            head = 8
            if size == 1:
                size, head = struct.unpack(">Q", data[pos + 8:pos + 16])[0], 16
            elif size == 0:
                size = end - pos
            body, stop = pos + head, pos + size
            if kind in (b"moov", b"trak", b"mdia", b"minf", b"stbl"):
                walk(body, stop)
            elif kind == b"mvhd":
                version = data[body]
                if version == 1:
                    scale, duration = struct.unpack(">IQ", data[body + 20:body + 32])
                else:
                    scale, duration = struct.unpack(">II", data[body + 12:body + 20])
                found["seconds"] = duration / scale
            elif kind == b"stsd" and "entry" not in found:
                found["entry"] = data[body + 12:body + 16].decode("ascii", "replace")
            pos = stop

    walk(0, len(data))
    return found


def fitted(w, h):
    """The video size the app makes of a w x h capture: scaled down so its longer side is at most 1920, then each
    side rounded down to even (video_size())."""
    scale = 1920 / max(w, h) if max(w, h) > 1920 else 1.0
    return max(2, int(w * scale) & ~1), max(2, int(h * scale) & ~1)


def ffmpeg():
    """(path, encoders, why): the ffmpeg the app finds, the encoders it lists, and, when there is none, the reason
    the app gives."""
    env = os.environ.get("ORCA_FFMPEG")
    if env:
        if not os.path.isfile(env):
            return None, set(), f"ORCA_FFMPEG names {env}, which is not a file"
        path = env
    else:
        path = shutil.which("ffmpeg")
        if not path and sys.platform == "darwin":
            path = next((p for p in ("/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/opt/local/bin/ffmpeg")
                         if os.path.isfile(p)), None)
        if not path:
            return None, set(), "ffmpeg was not found"
    try:
        out = subprocess.run([path, "-hide_banner", "-encoders"], capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError):
        out = ""
    encoders = {line.split()[1] for line in out.splitlines() if len(line.split()) > 1 and line.startswith(" V")}
    return path, encoders, None


class RecordingTest:
    def __init__(self, client, folder):
        self.c = client
        self.t = RecordingChecks()
        self.folder = folder
        self.ffmpeg, self.encoders, self.no_ffmpeg = ffmpeg()
        self.encoded = {}

    def out(self, name):
        return os.path.join(self.folder, name)

    # ---- Calls and reads ----

    def run(self, tool, **args):
        return run(self.c, tool, **args)

    def recording(self):
        return self.c.call("app_state").get("recording")

    def recording_is(self, label, path=None):
        rec = self.recording()
        if path is None:
            self.t.equal(f"{label}: no recording runs", rec, None)
        else:
            self.t.equal(f"{label}: the recording that runs", os.path.normpath((rec or {}).get("path", "")),
                         os.path.normpath(path))
        return rec

    def capture(self, target):
        reply = self.c.call("window_screenshot", target=target, include_image=False,
                            save_path=self.out(f"capture_{target}.png"), overwrite=True)
        return reply["width"], reply["height"]

    def same_path(self, label, got, want):
        self.t.equal(label, os.path.normpath(got or ""), os.path.normpath(want))

    def start(self, name):
        self.run("project_new", discard_changes=True)
        self.run("view_set", tab="prepare")
        wait(self.c, "idle")
        self.recording_is(f"{name}: the start")
        self.t.equal(f"{name}: the start: the plates", plates(self.c),
                     [{"name": "", "current": True, "locked": False, "objects": 0}])

    def can_encode(self, encoder):
        """Why an encode with `encoder` cannot be made on this machine, or None when it can."""
        if self.no_ffmpeg:
            return self.no_ffmpeg
        if encoder not in self.encoders:
            return f"ffmpeg has no {encoder}"
        return None

    def encode_refused(self, label, encoder, call):
        """The refusal an encode gets on a machine that cannot make it: the app's reason when ffmpeg is missing,
        or that ffmpeg has no `encoder`."""
        words = self.no_ffmpeg if self.no_ffmpeg else f"has no {encoder} encoder"
        self.t.refused(f"{label}: the encode is refused", call, words=words)

    def mkv_holds(self, label, path, size, frame_ms, written, seconds=None):
        info = mkv(path)
        self.t.equal(f"{label}: the file's codec", info.get("codec"), "V_MJPEG")
        self.t.equal(f"{label}: the file's size", (info.get("width"), info.get("height")), size)
        self.t.near(f"{label}: the file's frame duration", info.get("frame_ms"), frame_ms, 0.001)
        self.t.check(f"{label}: the file holds the frames written, or one more",
                     info["blocks"] in (written, written + 1), (info["blocks"], written))
        if seconds is not None:
            self.t.near(f"{label}: the file's duration", info["seconds"], seconds, 0.1)
        return info

    # ---- Scenarios ----

    def driven(self):
        s = "a recording while the GUI is driven"
        self.start(s)
        main = fitted(*self.capture("main"))
        session = self.out("session.mkv")
        reply = self.run("recording_start", save_path=session, overwrite=True, captions=True, pointer=True)
        self.same_path(f"{s} 1: the reply's path", reply.get("path"), session)
        self.t.equal(f"{s} 1: the reply", {k: reply.get(k) for k in ("target", "fps", "captions", "pointer",
                                                                      "max_seconds")},
                     {"target": "main", "fps": 10, "captions": True, "pointer": True, "max_seconds": 3600})
        self.t.equal(f"{s} 1: the video's size", (reply.get("width"), reply.get("height")), main)
        rec = self.recording_is(f"{s} 1", session)
        self.t.equal(f"{s} 1: the recording's target", (rec or {}).get("target"), "main")

        reply = self.run("project_import", paths=[CUBE])
        wait(self.c, "idle")
        self.t.equal(f"{s} 2: the reply's added", reply.get("added"), 1)
        self.t.equal(f"{s} 2: one object", self.c.call("object_list")["total"], 1)
        self.t.equal(f"{s} 2: an import selects what it adds", self.selected(), [0])
        self.run("mouse_gesture", action="click", x=300, y=200)
        self.t.equal(f"{s} 3: a click on empty space deselects", self.selected(), [])
        self.run("mouse_gesture", action="click", at={"object": 0, "spot": "top"})
        self.t.equal(f"{s} 4: a click on the object selects it", self.selected(), [0])

        # The sidebar is on Global, as start() leaves it, so it shows the process's fields.
        self.run("widget_set_value", target={"option": "layer_height"}, value="0.16")
        self.t.equal(f"{s} 5: the layer height", self.process("layer_height"), "0.16")
        first = {"option": "initial_layer_print_height"}
        reply = self.run("keyboard_press", target=first, keys="ctrl+a", text="0.24")
        self.t.equal(f"{s} 6: the reply", reply, {"sent": "ctrl+a", "to": "TextInput", "typed": "0.24"})
        self.run("keyboard_press", target=first, keys="Return")
        wait(self.c, "idle")
        self.t.equal(f"{s} 6: typing leaves a native text field as it was", self.process("initial_layer_print_height"),
                     "0.2")

        job = self.c.call("action_run", id=PREFERENCES)
        self.t.equal(f"{s} 7: the action waits on its dialog",
                     (job.get("status"), [d.get("title") for d in job.get("dialogs", [])]),
                     ("blocked_by_dialog", ["Preferences"]))
        self.c.call("dialog_answer", dialog={"title": "Preferences"}, button="close")
        try:
            self.c.await_job(job, timeout=60)
            self.t.check(f"{s} 7: the action's job ends without an error", True)
        except orca_mcp.McpError as e:
            self.t.check(f"{s} 7: the action's job ends without an error", False, str(e))
        self.t.equal(f"{s} 7: no dialog", self.c.call("app_state")["dialogs"], [])

        reply = self.run("plate_slice")
        self.t.equal(f"{s} 8: the slice's outcome", [p.get("outcome") for p in reply.get("plates", [])], ["ok"])
        gcode = self.out("session.gcode")
        self.run("project_export", path=gcode, overwrite=True)
        with open(gcode, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
        for line in ("; layer_height = 0.16", "; initial_layer_print_height = 0.2"):
            self.t.check(f"{s} 8: the G-code's config holds {line!r}", line in lines, "")
        # The app marks each layer with ;Z:<z>; the printer's own layer-change G-code writes ;<z> without Z:.
        heights = []
        for line in lines:
            if line.startswith(";Z:"):
                z = float(line.split(":", 1)[1])
                if z not in heights:
                    heights.append(z)
                if len(heights) == 2:
                    break
        self.t.near(f"{s} 8: the first two layers' heights", heights, [0.2, 0.36], 1e-6)

        report = self.run("recording_stop", silent=True)
        self.t.equal(f"{s} 9: the report's end", report.get("ended"), "stopped")
        self.same_path(f"{s} 9: the report's path", report.get("path"), session)
        self.t.check(f"{s} 9: at least 5 frames captured and written",
                     report.get("frames", 0) >= 5 and report.get("written", 0) >= 5, report)
        self.t.equal(f"{s} 9: no frame failed", report.get("failed"), 0)
        self.t.check(f"{s} 9: it ran over a second", report.get("seconds", 0) > 1, report.get("seconds"))
        self.t.check(f"{s} 9: a capture took time", report.get("capture_ms", 0) > 0, report.get("capture_ms"))
        self.t.equal(f"{s} 9: the video's size", (report.get("width"), report.get("height")), main)
        self.t.equal(f"{s} 9: the report's size is the file's", report.get("size_bytes"), os.path.getsize(session))
        self.recording_is(f"{s} 9")
        self.mkv_holds(f"{s} 9", session, main, 100, report.get("written", -1), report.get("seconds", 0))

        mp4_path = self.out("session.mp4")
        call = lambda: self.c.call("recording_encode", save_path=mp4_path, overwrite=True, keep_raw_video_file=True)
        why = self.can_encode("libx265")
        if why is None:
            reply = self.c.await_job(call(), timeout=300)
            self.same_path(f"{s} 10: the MP4's path", reply.get("path"), mp4_path)
            self.same_path(f"{s} 10: the source", reply.get("source"), session)
            self.t.equal(f"{s} 10: the encode", {k: reply.get(k) for k in ("codec", "encoder", "raw_video_file_deleted")},
                         {"codec": "h265", "encoder": "libx265", "raw_video_file_deleted": False})
            self.t.equal(f"{s} 10: the reply's size is the file's", reply.get("size_bytes"), os.path.getsize(mp4_path))
            info = mp4(mp4_path)
            self.t.equal(f"{s} 10: the MP4 is H.265", info.get("entry"), "hvc1")
            self.t.near(f"{s} 10: the MP4's duration", info.get("seconds"), report.get("seconds", 0), 0.3)
            self.encoded["h265"] = "yes"
        else:
            self.encode_refused(f"{s} 10", "libx265", call)
            self.t.check(f"{s} 10: no MP4", not os.path.exists(mp4_path), mp4_path)
            for check in ("the MP4's path", "the source", "the encode", "the MP4 is H.265", "the MP4's duration"):
                self.t.skip(f"{s} 10: {check}", why)
            self.encoded["h265"] = f"no ({why})"
        self.t.check(f"{s} 10: the .mkv is still there", os.path.exists(session), session)

    def selected(self):
        return (self.c.call("app_state").get("selection") or {}).get("objects", [])

    def process(self, key):
        return self.c.call("settings_get", type="process", keys=[key])["items"][0]["value"]

    def nested(self):
        s = "a recording a replay starts"
        self.start(s)
        canvas = fitted(*self.capture("canvas"))
        inner = self.out("inner.mkv")
        replay = {"orca_replay_version": "0.2.0", "name": "inner", "steps": [
            {"tool": "recording_start", "args": {"target": "canvas", "fps": 5, "save_path": inner, "overwrite": True},
             "expect": {"/target": {"eq": "canvas"}, "/fps": {"eq": 5}}},
            {"tool": "view_set", "args": {"camera": "iso"}},
            {"tool": "mouse_gesture", "args": {"action": "drag", "x": 400, "y": 300, "to": [500, 340]}}]}
        reply = self.run("replay_run", replay=replay, out_dir=self.out("inner_replay"))
        self.t.equal(f"{s} 1: the replay passed", reply.get("passed"), True)
        self.t.check(f"{s} 1: no unfinished recording", "unfinished_recording" not in reply, reply)
        self.recording_is(f"{s} 1: the recording ended with the replay")

        report = self.run("recording_stop", silent=True)
        self.t.equal(f"{s} 2: the report's end", report.get("ended"), "run_ended")
        self.same_path(f"{s} 2: the report's path", report.get("path"), inner)
        self.t.check(f"{s} 2: at least one frame written", report.get("written", 0) >= 1, report)
        self.t.equal(f"{s} 2: no frame failed", report.get("failed"), 0)
        self.t.equal(f"{s} 2: the video's size", (report.get("width"), report.get("height")), canvas)
        self.mkv_holds(f"{s} 2", inner, canvas, 200, report.get("written", -1))

        self.t.refused(f"{s} 3: an ended recording is reported once",
                       lambda: self.c.call("recording_stop", silent=True), words="No recording is running")
        self.recording_is(f"{s} 3")

        mp4_path = self.out("inner.mp4")
        call = lambda: self.c.call("recording_encode", overwrite=True, codec="h264")
        why = self.can_encode("libx264")
        if why is None:
            reply = self.c.await_job(call(), timeout=300)
            self.same_path(f"{s} 4: the MP4's path", reply.get("path"), mp4_path)
            self.same_path(f"{s} 4: the session's last finished recording is the source", reply.get("source"), inner)
            self.t.equal(f"{s} 4: the encode", {k: reply.get(k) for k in ("encoder", "raw_video_file_deleted")},
                         {"encoder": "libx264", "raw_video_file_deleted": True})
            self.t.check(f"{s} 4: the .mkv is gone", not os.path.exists(inner), inner)
            self.t.equal(f"{s} 4: the MP4 is H.264", mp4(mp4_path).get("entry"), "avc1")
            self.encoded["h264"] = "yes"
        else:
            self.encode_refused(f"{s} 4", "libx264", call)
            self.t.check(f"{s} 4: no MP4", not os.path.exists(mp4_path), mp4_path)
            self.t.check(f"{s} 4: the .mkv is still there", os.path.exists(inner), inner)
            for check in ("the MP4's path", "the source", "the encode", "the .mkv is gone", "the MP4 is H.264"):
                self.t.skip(f"{s} 4: {check}", why)
            self.encoded["h264"] = f"no ({why})"

        reply = self.run("recording_start", save_path=inner, overwrite=True)
        self.t.equal(f"{s} 5: no overlays by default", (reply.get("captions"), reply.get("pointer")), (False, False))
        self.recording_is(f"{s} 5: the replay's end released the file", inner)
        reply = self.run("recording_stop", silent=True)
        self.t.equal(f"{s} 6: the report's end", reply.get("ended"), "stopped")
        self.recording_is(f"{s} 6")

    def overlays(self):
        s = "overlays and the time limit"
        self.start(s)
        plain = self.out("plain.mkv")
        for step, args, overlays in ((1, {"pointer": True}, (False, True)), (3, {"captions": True}, (True, False))):
            reply = self.run("recording_start", save_path=plain, overwrite=True, **args)
            self.t.equal(f"{s} {step}: captions and pointer", (reply.get("captions"), reply.get("pointer")), overlays)
            self.recording_is(f"{s} {step}", plain)
            reply = self.run("recording_stop", silent=True)
            self.t.equal(f"{s} {step + 1}: the report's end", reply.get("ended"), "stopped")
            self.recording_is(f"{s} {step + 1}")

        short = self.out("short.mkv")
        reply = self.run("recording_start", save_path=short, overwrite=True, max_seconds=2)
        self.t.equal(f"{s} 5: the limit", reply.get("max_seconds"), 2)
        self.recording_is(f"{s} 5", short)
        until(lambda: self.recording() is None, "the 2 s recording ended on its own")
        self.recording_is(f"{s} 6: the recording ended on its own")
        report = self.run("recording_stop", silent=True)
        self.t.equal(f"{s} 7: the limit ended it", report.get("ended"), "max_seconds")
        # The timer ends it at its first tick past the limit, which a loaded machine delays: only the lower bound
        # holds on any machine.
        self.t.check(f"{s} 7: it ran at least the limit", report.get("seconds", 0) >= 2, report.get("seconds"))
        self.t.near(f"{s} 7: the file's duration", mkv(short)["seconds"], report.get("seconds", 0), 0.1)

        wrong = self.out("wrong.avi")
        self.t.refused(f"{s} 8: a path that is not .mkv is refused",
                       lambda: self.c.call("recording_start", save_path=wrong), words="save_path must end with .mkv")
        self.recording_is(f"{s} 8")
        self.t.check(f"{s} 8: no file", not os.path.exists(wrong), wrong)

    def encode_job(self, label, report, mp4_path, mkv_path, keep):
        """Steps 3 and 6 of the encode scenario: the encode the stop started, followed to its result."""
        encode = report.get("encode") or {}
        why = self.can_encode("libx264")
        error, result = None, encode
        if encode.get("job") and encode.get("status") in ("running", "blocked_by_dialog"):
            try:
                result = self.c.await_job(encode, timeout=300)
            except orca_mcp.McpError as e:
                error = str(e)
        elif "error" in encode:
            error = str(encode.get("error"))
        if why is None:
            self.t.check(f"{label}: the encode ends without an error", error is None, error)
            self.same_path(f"{label}: the MP4's path", (result or {}).get("path"), mp4_path)
            self.t.equal(f"{label}: the encode", {k: (result or {}).get(k) for k in ("encoder", "raw_video_file_deleted")},
                         {"encoder": "libx264", "raw_video_file_deleted": not keep})
            self.t.check(f"{label}: the .mkv is {'kept' if keep else 'gone'}", os.path.exists(mkv_path) == keep, mkv_path)
            info = mp4(mp4_path) if os.path.exists(mp4_path) else {}
            self.t.equal(f"{label}: the MP4 is H.264", info.get("entry"), "avc1")
            if not keep:
                self.t.near(f"{label}: the MP4's duration", info.get("seconds"), report.get("seconds", 0), 0.3)
        else:
            self.t.check(f"{label}: the encode ends with the encoder's error",
                         error is not None and "has no libx264 encoder" in error, error)
            self.t.check(f"{label}: the .mkv is still there", os.path.exists(mkv_path), mkv_path)
            self.t.check(f"{label}: no MP4", not os.path.exists(mp4_path), mp4_path)
            for check in ("the MP4's path", "the encode", "the MP4 is H.264"):
                self.t.skip(f"{label}: {check}", why)

    def encode_at_start(self):
        s = "an encode asked for at the start"
        self.start(s)
        encoded, encoded_mp4 = self.out("encoded.mkv"), self.out("encoded.mp4")
        args = dict(save_path=encoded, overwrite=True, encode=True, encode_preset="compatible")
        if self.no_ffmpeg:
            self.t.refused(f"{s} 1: the start is refused", lambda: self.c.call("recording_start", crf=30, **args),
                           words="Cannot encode: " + self.no_ffmpeg)
            self.recording_is(f"{s} 1")
            self.t.check(f"{s} 1: no file", not os.path.exists(encoded), encoded)
            for step in range(2, 7):
                self.t.skip(f"{s} {step}", self.no_ffmpeg)
            return
        reply = self.run("recording_start", crf=30, **args)
        self.t.equal(f"{s} 1: the encode asked for", reply.get("encode"),
                     {"codec": "h264", "crf": 30, "speed": "medium", "profile": "high", "keep_raw_video_file": False})
        self.recording_is(f"{s} 1", encoded)
        report = self.c.call("recording_stop", silent=True)
        self.same_path(f"{s} 2: the report's path", report.get("path"), encoded)
        self.t.equal(f"{s} 2: the report's end", report.get("ended"), "stopped")
        self.t.check(f"{s} 2: the report holds the encode", isinstance(report.get("encode"), dict), report)
        self.recording_is(f"{s} 2")
        self.encode_job(f"{s} 3", report, encoded_mp4, encoded, keep=False)

        kept, kept_mp4 = self.out("kept.mkv"), self.out("kept.mp4")
        reply = self.run("recording_start", **{**args, "save_path": kept, "keep_raw_video_file": True})
        self.t.equal(f"{s} 4: the encode keeps the .mkv", (reply.get("encode") or {}).get("keep_raw_video_file"), True)
        self.recording_is(f"{s} 4", kept)
        report = self.c.call("recording_stop", silent=True)
        self.t.check(f"{s} 5: the report holds the encode", isinstance(report.get("encode"), dict), report)
        self.recording_is(f"{s} 5")
        self.encode_job(f"{s} 6", report, kept_mp4, kept, keep=True)

    def run_all(self):
        self.t.equal("the start: the presets", start(self.c),
                     {"printer": KLIPPER, "process": KLIPPER_STD, "filaments": [GENERIC_PLA]})
        # The click at (300, 200) that must land on empty space is placed in the baseline's 1600 x 900 window.
        self.t.equal("the start: the window's size", self.c.call("app_state")["window"]["size"], WINDOW_SIZE)
        for name, scenario in [("a recording while the GUI is driven", self.driven),
                               ("a recording a replay starts", self.nested),
                               ("overlays and the time limit", self.overlays),
                               ("an encode asked for at the start", self.encode_at_start)]:
            self.t.scenario(name, scenario)
        code = self.t.exit_code()
        print("encodes checked: " + ", ".join(f"{codec} {self.encoded.get(codec, 'no (not reached)')}"
                                             for codec in ("h265", "h264")), flush=True)
        return code


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", help="folder for the recordings (default: a new temporary folder)")
    args = ap.parse_args()
    folder = args.out or tempfile.mkdtemp(prefix="orca-recording-")
    os.makedirs(folder, exist_ok=True)
    print(f"output folder: {folder}", flush=True)
    client, _ = orca_mcp.session_client()
    return RecordingTest(client, folder).run_all()


if __name__ == "__main__":
    sys.exit(main())
