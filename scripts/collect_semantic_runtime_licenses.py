#!/usr/bin/env python3
"""Collect source notices for the exact offline DLL link inputs and header deps.

Run after the build has generated link-input-manifest.json. This does not ship
build-only Java/Python/LLVM tools or copy the official wheel's telemetry notice.
"""
import argparse
import hashlib
import json
from pathlib import Path


def collect(root, output_base, output):
    external = output_base / "external"
    link_manifest = json.loads((root / "link-input-manifest.json").read_text(encoding="utf-8-sig"))
    linked = set(link_manifest["external_link_repositories"])
    expected = {"XNNPACK", "abseil-cpp~", "bazel_tools", "com_github_gflags_gflags",
                "com_github_glog_glog_windows", "cpuinfo", "easyexif", "farmhash_archive",
                "fft2d", "flatbuffers", "org_tensorflow", "protobuf~", "pthreadpool",
                "ruy", "stblib", "windows_opencv", "zlib"}
    if linked != expected:
        raise ValueError(f"Review changed link dependencies: added {linked-expected}, absent {expected-linked}")
    # Header-only sources do not appear as separate archives in the link list.
    headers = {"eigen", "FP16", "FXdiv", "gemmlowp", "arm_neon_2_x86_sse"}
    entries = []
    files = [("mediapipe", root / "src/LICENSE", "runtime source")]
    for repository in sorted((linked | headers) - {"bazel_tools", "fft2d", "windows_opencv"}):
        licenses = [p for p in (external / repository).iterdir()
                    if p.is_file() and p.name.upper().startswith(("LICENSE", "COPYING", "COPYRIGHT", "NOTICE"))]
        if not licenses:
            raise ValueError(f"Missing notices for {repository}")
        files.extend((repository, p, "linked archive" if repository in linked else "header dependency")
                     for p in sorted(licenses))
    files.extend([
        ("fft2d", external / "fft2d/readme.txt", "linked archive; license in readme"),
        ("fft2d", external / "fft2d/readme2d.txt", "linked archive; license in readme"),
        ("opencv-3.4.11", root / "opencv-3.4.11/LICENSE", "linked archive"),
        ("opencv-zlib", root / "opencv-3.4.11/3rdparty/zlib/README", "linked archive; license in readme"),
        ("minizip", external / "zlib/contrib/minizip/MiniZip64_info.txt", "linked archive; attribution"),
        # Preserve the exact copyright/license header for Bazel's small runtime
        # runfiles library; the shared Apache-2.0 license is also included above.
        ("bazel-runfiles", external / "bazel_tools/tools/cpp/runfiles/runfiles.cc", "linked runtime source notice"),
    ])
    output.mkdir(parents=True, exist_ok=True)
    for repository, source, purpose in files:
        data = source.read_bytes()
        if repository == "bazel-runfiles":
            data = b"\n".join(data.splitlines()[:13]) + b"\n"
        target = output / repository / source.name
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists() and target.read_bytes() != data:
            raise ValueError(f"Refusing to overwrite a different frozen notice: {target}")
        target.write_bytes(data)
        entries.append({"repository": repository, "purpose": purpose, "source": str(source),
                        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                        "file": str(target.relative_to(output)), "sha256": hashlib.sha256(data).hexdigest()})
    result = {"schema": "orca.semantic-runtime-source-notices/v1", "source_commit": "6d31f1ebc3284db74d211d62bdc4f0a0c29ea120",
              "link_inputs_sha256": link_manifest["sha256"], "entries": entries,
              "scope": "Notices from actual linked repositories plus known header dependencies; model licenses are packaged separately."}
    (output / "manifest.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--output-base", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = collect(args.build_root, args.output_base, args.output)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"Source notice collection failed: {error}\n")
    print(f"Collected {len(result['entries'])} source notice files; no build tools packaged.")
