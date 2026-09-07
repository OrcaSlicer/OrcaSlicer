"""Offline, bounded inspection of actual ZIP/NSIS payloads. Never execute installers.

Exit 0: no finding within the recorded scope; 2: findings or incomplete inspection.
This is a publication gate, not proof of absence, validity testing or authorization.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import tempfile
import zipfile


SECRET_FIELD = re.compile(r"(?:api[_-]?key|api[_-]?token|access[_-]?token|auth[_-]?token|secret|password|credential|OPENAI_PRO_API)$", re.I)
PLACEHOLDER = re.compile(r"(?:your[_ -].*|<[^>]+>|\$\{[^}]+\}|placeholder|changeme|dummy|example|test|x+|\*+)", re.I)
ASSIGNMENT = re.compile(r'''(?<![\w.-])["']?([\w.-]{0,128}(?:api_key|api_token|access_token|auth_token|secret|password|OPENAI_PRO_API))["']?\s*[:=]\s*["']([^"'\r\n]*)["']''', re.I)
TOKEN = re.compile(rb"(?:\bsk-[A-Za-z0-9_-]{20,}|\btsk_[A-Za-z0-9_-]{20,}|-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----)")
MAX_FILE = 256 * 1024 * 1024
MAX_TOTAL = 4 * 1024 * 1024 * 1024
MAX_ENTRIES = 100000

# The checked-in tooltip bundle contains the public emoji-name dictionary entry
# secret -> U+3299 U+FE0F. Classify only that entry in these exact audited bytes;
# edits (including appended credentials) invalidate this classification. Keep the
# candidate in config_fields and continue every other scan of the same file.
TOOLTIP_EMOJI_BUNDLE_SHA256 = "a4040f542802a7c939c6823986b239a334fffd4eadbb41961f051052e7ccdfdf"


def category(value):
    if value is None or value == "" or (isinstance(value, str) and PLACEHOLDER.fullmatch(value.strip())):
        return "EMPTY_OR_PLACEHOLDER"
    return "CREDENTIAL_MATERIAL_VALIDITY_UNKNOWN"


def safe_member(name):
    name = name.replace("\\", "/")
    p = PurePosixPath(name)
    return bool(name) and not p.is_absolute() and ".." not in p.parts and ":" not in name


class Inspection:
    def __init__(self):
        self.findings = []
        self.gaps = []
        self.config_fields = []
        self.files = 0
        self.bytes = 0

    def gap(self, member, reason):
        self.gaps.append({"member": member, "reason": reason})

    def field(self, member, key, value):
        classification = category(value)
        entry = {"member": member, "field": key, "category": classification}
        self.config_fields.append(entry)
        if classification == "CREDENTIAL_MATERIAL_VALIDITY_UNKNOWN":
            self.findings.append(entry)

    def scan(self, member, data, depth=0):
        self.files += 1
        self.bytes += len(data)
        if self.files > MAX_ENTRIES or len(data) > MAX_FILE or self.bytes > MAX_TOTAL:
            raise ValueError("inspection_limit")
        if data[:4] in (b"PK\x03\x04", b"PK\x05\x06") or member.lower().endswith(".zip"):
            if depth >= 3:
                self.gap(member, "nested_archive_depth_limit")
                return
            try:
                with zipfile.ZipFile(io.BytesIO(data)) as archive:
                    self.zip(archive, member + "!", depth + 1)
            except (OSError, ValueError, RuntimeError, zipfile.BadZipFile, NotImplementedError):
                self.gap(member, "nested_zip_unreadable")
            return
        if member.lower().endswith((".7z", ".rar", ".gz", ".xz", ".tar", ".bz2")):
            self.gap(member, "unsupported_nested_archive")
        # Scan binary byte strings too; UTF-16 configs are decoded separately.
        if TOKEN.search(data) or TOKEN.search(data.replace(b"\x00", b"")):
            self.findings.append({"member": member, "category": "TOKEN_OR_PRIVATE_KEY_PATTERN_VALIDITY_UNKNOWN"})
        if not member.lower().endswith((".json", ".env", ".ini", ".cfg", ".conf", ".txt", ".ps1", ".bat", ".cmd", ".py", ".js", ".ts", ".yml", ".yaml", ".xml", ".toml", ".properties")):
            return
        encoding = "utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
        text = data.decode(encoding, errors="replace")
        if member.lower().endswith(".json"):
            try:
                parsed = json.loads(text)
            except (ValueError, RecursionError):
                # Broken provider config cannot be silently treated as empty.
                if "defaults" in member.lower() or "credential" in member.lower():
                    self.gap(member, "provider_json_unreadable")
            else:
                def walk(obj):
                    if isinstance(obj, dict):
                        for key, value in obj.items():
                            if SECRET_FIELD.search(key):
                                self.field(member, key, value)
                            elif "orca_ai_internal_defaults.json" in member:
                                self.config_fields.append({"member": member, "field": key, "category": "ORDINARY_CONFIGURATION"})
                            walk(value)
                    elif isinstance(obj, list):
                        for value in obj:
                            walk(value)
                walk(parsed)
        else:
            for match in ASSIGNMENT.finditer(text):
                if (match[1] == "secret" and match[2] == "\u3299\ufe0f"
                        and member.replace("\\", "/").endswith("resources/tooltip/main.js")
                        and hashlib.sha256(data).hexdigest() == TOOLTIP_EMOJI_BUNDLE_SHA256):
                    self.config_fields.append({"member": member, "field": match[1],
                                               "category": "PUBLIC_EMOJI_NAME_MAPPING",
                                               "evidence": "exact_audited_tooltip_bundle_sha256"})
                    continue
                self.field(member, match[1], match[2])

    def zip(self, archive, prefix="", depth=0):
        infos = archive.infolist()
        if len(infos) > MAX_ENTRIES:
            raise ValueError("entry_limit")
        for info in infos:
            name = prefix + info.filename
            if not safe_member(info.filename) or stat.S_ISLNK(info.external_attr >> 16):
                self.gap(name, "unsafe_archive_member")
                continue
            if info.is_dir():
                continue
            if info.file_size > MAX_FILE or self.bytes + info.file_size > MAX_TOTAL:
                self.gap(name, "inspection_size_limit")
                continue
            try:
                self.scan(name, archive.read(info), depth)
            except (OSError, ValueError, RuntimeError, zipfile.BadZipFile, NotImplementedError):
                self.gap(name, "member_unreadable_or_limit")


def seven_zip_path(explicit=None):
    return explicit or shutil.which("7z") or str(Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "7-Zip/7z.exe")


def inspect(path, seven_zip=None):
    path = Path(path)
    check = Inspection()
    report = {"schema_version": 1, "artifact": path.name, "sha256": None,
              "scope": "All readable payload members and nested ZIPs to depth 3. Selected token/private-key byte patterns in all files; credential fields/literal assignments in JSON, env, ini, cfg, conf, txt, ps1, bat, cmd, py, js, ts, yml, yaml, xml, toml and properties files. No endpoint calls.",
              "limitations": "Heuristic scan: encoded, encrypted, obfuscated or unrecognized secrets may escape detection. Credential validity/revocation and authorization are not inferred."}
    try:
        with path.open("rb") as stream:
            report["sha256"] = hashlib.file_digest(stream, "sha256").hexdigest()
        report["size_bytes"] = path.stat().st_size
        if path.suffix.lower() == ".zip":
            with zipfile.ZipFile(path) as archive:
                check.zip(archive)
        elif path.suffix.lower() == ".exe":
            tool = seven_zip_path(seven_zip)
            # List first; never execute an EXE, follow links or extract traversal paths.
            listing = subprocess.run([tool, "l", "-slt", "-ba", str(path)], capture_output=True, timeout=120)
            if listing.returncode:
                raise ValueError("archive_listing_failed")
            listing_text = listing.stdout.decode("utf-8", errors="replace")
            names = re.findall(r"^Path = (.*)$", listing_text, re.M)
            sizes = [int(n) for n in re.findall(r"^Size = (\d+)", listing_text, re.M)]
            if (not names or len(names) > MAX_ENTRIES or any(not safe_member(n.strip()) for n in names)
                    or sum(sizes) > MAX_TOTAL or re.search(r"^(?:Symbolic Link|Hard Link) = .+", listing_text, re.M)):
                raise ValueError("unsafe_archive_listing")
            with tempfile.TemporaryDirectory(prefix="orca-package-inspection-") as temp:
                extraction = subprocess.run([tool, "x", "-y", "-bd", "-bb0", "-o" + temp, str(path)], capture_output=True, timeout=180)
                if extraction.returncode:
                    raise ValueError("archive_extraction_failed")
                for file in Path(temp).rglob("*"):
                    name = file.relative_to(temp).as_posix()
                    if file.is_symlink() or (hasattr(file, "is_junction") and file.is_junction()):
                        check.gap(name, "extracted_link")
                    elif file.is_file():
                        if file.stat().st_size > MAX_FILE:
                            check.gap(name, "inspection_size_limit")
                        else:
                            check.scan(name, file.read_bytes())
        else:
            raise ValueError("unsupported_artifact")
        with path.open("rb") as stream:
            if hashlib.file_digest(stream, "sha256").hexdigest() != report["sha256"]:
                check.gap(path.name, "artifact_changed_during_inspection")
    except (OSError, ValueError, RuntimeError, RecursionError, zipfile.BadZipFile, subprocess.SubprocessError):
        # Exceptions and child output may contain config values; never emit them.
        check.gap(path.name, "inspection_failed_or_extractor_unavailable")
    report.update(files_scanned=check.files, unpacked_bytes_scanned=check.bytes,
                  findings=check.findings, gaps=check.gaps, config_fields=check.config_fields)
    report["status"] = "UNKNOWN" if check.gaps else ("BLOCKED_FINDINGS" if check.findings else "NOT_DETECTED_WITHIN_SCOPE")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--seven-zip")
    args = parser.parse_args()
    if args.artifact.resolve() == args.report.resolve():
        parser.error("Report must not overwrite the artifact")
    report = inspect(args.artifact, args.seven_zip)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: report[key] for key in ("artifact", "sha256", "status", "files_scanned")}))
    return 0 if report["status"] == "NOT_DETECTED_WITHIN_SCOPE" else 2


if __name__ == "__main__":
    raise SystemExit(main())
