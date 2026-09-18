#!/usr/bin/env python3
"""Build-time source provenance check for an unchanged pinned tar archive."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import tarfile


def digest_stream(stream):
    value = hashlib.sha256()
    for block in iter(lambda: stream.read(1024*1024), b""): value.update(block)
    return value.hexdigest()


def audit(archive_path, source, prefix, expected_archive, allow_opencv_cache_marker=False):
    with archive_path.open("rb") as stream:
        if digest_stream(stream) != expected_archive: raise ValueError("Source archive hash mismatch")
    source = source.resolve(strict=True)
    original = {}
    with tarfile.open(archive_path, "r:gz") as archive:
        for entry in archive:
            if entry.isdir(): continue
            if not entry.isfile(): raise ValueError(f"Unsupported source archive link: {entry.name}")
            member = PurePosixPath(entry.name)
            if member.is_absolute() or ".." in member.parts or member.parts[0] != prefix:
                raise ValueError(f"Unexpected archive path: {entry.name}")
            relative = PurePosixPath(*member.parts[1:]).as_posix()
            file = source.joinpath(*member.parts[1:])
            if not file.resolve(strict=True).is_relative_to(source): raise ValueError(f"Source escapes root: {relative}")
            with archive.extractfile(entry) as stream: expected = digest_stream(stream)
            with file.open("rb") as stream: actual = digest_stream(stream)
            if actual != expected: raise ValueError(f"Unregistered source modification: {relative}")
            original[relative] = {"path": relative, "sha256": expected, "bytes": entry.size}
    generated = []
    for file in source.rglob("*"):
        if file.is_symlink(): raise ValueError(f"Unexpected source symlink: {file}")
        relative = file.relative_to(source).as_posix()
        if file.is_file() and relative not in original:
            if allow_opencv_cache_marker and relative == ".cache/.gitignore" and file.read_bytes() in (b"*\n", b"*\r\n"):
                generated.append({"path": relative, "sha256": hashlib.sha256(file.read_bytes()).hexdigest(),
                                  "reason": "Exact cache marker written by upstream cmake/OpenCVDownload.cmake"})
            else: raise ValueError(f"Unregistered additional source file: {file}")
    return {"schema": "orca.semantic-tar-source-audit/v1", "archive": str(archive_path.resolve()),
            "archive_sha256": expected_archive, "source": str(source),
            "files": sorted(original.values(), key=lambda item: item["path"]), "generated_files": generated, "unregistered_changes": 0}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--prefix", required=True)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--allow-opencv-cache-marker", action="store_true")
    args = parser.parse_args()
    try: result = audit(args.archive, args.source, args.prefix, args.sha256, args.allow_opencv_cache_marker)
    except (OSError, ValueError, tarfile.TarError) as exc: parser.exit(1, f"Source audit failed: {exc}\n")
    args.output.write_text(json.dumps(result, indent=2)+"\n", encoding="utf-8")
    print(f"Verified {len(result['files'])} pinned source files with no modifications.")


if __name__ == "__main__": main()
