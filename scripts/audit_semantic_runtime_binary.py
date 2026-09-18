#!/usr/bin/env python3
"""Inspect a Windows semantic DLL without loading it or accessing the network.

This is a binary admission check, not a replacement for the real application's
network trace and inference tests. Direct networking imports are rejected even
when they could be unused; dependency DLLs must also be separately inspected.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


REQUIRED_EXPORTS = (
    "MpErrorFree", "MpImageCreateFromUint8Data", "MpImageFree",
    "MpImageGetWidth", "MpImageGetHeight", "MpImageDataFloat32",
    "MpImageSegmenterCreate", "MpImageSegmenterSegmentImage",
    "MpImageSegmenterCloseResult", "MpImageSegmenterClose",
    "MpFaceLandmarkerCreate", "MpFaceLandmarkerDetectImage",
    "MpFaceLandmarkerCloseResult", "MpFaceLandmarkerClose",
)
NETWORK_DLLS = {
    "wininet.dll", "winhttp.dll", "ws2_32.dll", "wsock32.dll",
    "urlmon.dll", "webio.dll", "dnsapi.dll", "rasapi32.dll",
}


def inspect(path):
    blob = path.read_bytes()
    def u16(off): return struct.unpack_from("<H", blob, off)[0]
    def u32(off): return struct.unpack_from("<I", blob, off)[0]
    def u64(off): return struct.unpack_from("<Q", blob, off)[0]
    if blob[:2] != b"MZ": raise ValueError("Missing DOS header")
    pe = u32(0x3C)
    if blob[pe:pe+4] != b"PE\0\0": raise ValueError("Missing PE header")
    machine = u16(pe+4)
    optional = pe+24
    if machine != 0x8664 or u16(optional) != 0x20B:
        raise ValueError("Runtime must be a Windows x64 PE32+ binary")
    base = u64(optional+24)
    directories = optional+112
    section_start = optional+u16(pe+20)
    sections = []
    for i in range(u16(pe+6)):
        off = section_start+i*40
        sections.append((u32(off+12), max(u32(off+8),u32(off+16)),u32(off+20)))
    def raw(rva):
        if rva < u32(optional+60): return rva
        for address, size, offset in sections:
            if address <= rva < address+size: return offset+rva-address
        raise ValueError(f"Unmapped RVA {rva:x}")
    def string(rva):
        off = raw(rva)
        return blob[off:blob.index(0, off)].decode("ascii")
    def directory(index):
        return u32(directories+index*8), u32(directories+index*8+4)
    imports = []
    rva, size = directory(1)
    if rva:
        off = raw(rva)
        for i in range(size//20):
            entry = off+i*20
            if not any(blob[entry:entry+20]): break
            imports.append(string(u32(entry+12)))
    delayed = []
    rva, size = directory(13)
    if rva:
        off = raw(rva)
        for i in range(size//32):
            entry = off+i*32
            if not any(blob[entry:entry+32]): break
            name = u32(entry+4)
            if not (u32(entry)&1): name -= base
            delayed.append(string(name))
    exports = []
    rva, _ = directory(0)
    if rva:
        off = raw(rva)
        names = raw(u32(off+32))
        exports = [string(u32(names+i*4)) for i in range(u32(off+24))]
    telemetry_markers = [marker for marker in ("play.googleapis.com/log", "InternetOpenA", "HttpSendRequestA", "WinHttpSendRequest")
                         if marker.encode() in blob or marker.encode("utf-16-le") in blob]
    missing = sorted(set(REQUIRED_EXPORTS)-set(exports))
    network = sorted(set(name.lower() for name in imports+delayed)&NETWORK_DLLS)
    return {
        "schema": "orca.semantic-runtime-binary-audit/v1",
        "path": str(path.resolve()), "sha256": hashlib.sha256(blob).hexdigest(),
        "bytes": len(blob), "architecture": "windows-x64",
        "imports": sorted(imports), "delay_imports": sorted(delayed),
        "exports_count": len(exports), "missing_required_exports": missing,
        "network_imports": network, "telemetry_markers": telemetry_markers,
        "binary_admission_passed": not (missing or network or telemetry_markers),
        "limitations": "Static direct-import/marker audit only. Does not prove absence of dynamically loaded networking or replace real-process trace and inference validation.",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dll", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        result = inspect(args.dll)
    except (OSError, ValueError, IndexError, struct.error) as exc:
        parser.exit(2, f"Runtime audit failed: {exc}\n")
    rendered = json.dumps(result, ensure_ascii=False, indent=2)+"\n"
    if args.output: args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return 0 if result["binary_admission_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
