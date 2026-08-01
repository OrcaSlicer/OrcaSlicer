#!/usr/bin/env python3
"""
Toolchain resolution for the config codegen.

The codegen needs exactly three things: a protoc binary, the protobuf Python
runtime and pyyaml. Every entry point used to install `grpcio-tools` to get
protoc, which drags in the grpcio C extension: it has no Windows/ARM64 wheel and
falls back to building from source there, which is what broke the ARM64 build
("Failed building wheel for grpcio" -> "protoc not found"). Nothing in the
codegen uses gRPC.

Resolution order, so callers only ever run `python tools/run_codegen.py`:

    protoc   $PROTOC -> PATH -> cached download -> grpc_tools (if installed) ->
             pinned, checksum-verified protoc release downloaded into
             .codegen-tools/ (gitignored)
    runtime  the current interpreter, else a cached virtualenv under
             .codegen-tools/venv that the entry point re-execs into

Set PROTOC=/path/to/protoc (or put protoc on PATH) to build offline.
"""

import hashlib
import importlib.util
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CACHE_DIR = ROOT / ".codegen-tools"

# Pinned so every machine generates with the same compiler, and checksummed
# because we execute what we download. To bump: change the version and refresh
# every hash from https://github.com/protocolbuffers/protobuf/releases/tag/v<ver>
PROTOC_VERSION = "28.3"
PROTOC_ARCHIVE_SHA256 = {
    "win64":                "ce64f49bdeddef49ce4bd313a8f59bcf92fcf67b5831efbf66170386d2e66948",
    "linux-x86_64":         "0ad949f04a6a174da83cdcbdb36dee0a4925272a5b6d83f79a6bf9852076d53f",
    "linux-aarch_64":       "1de522032a8b194002fe35cab86d747848238b5e4de4f99648372079f5b46f9a",
    "osx-universal_binary": "52df502b263da20f3311b23b5c6553d10cc25c6ebb85df381d80a2806b6a698b",
}

# pip name -> import name
PYTHON_PACKAGES = {"protobuf": "google.protobuf", "pyyaml": "yaml"}

# Guards against an endless re-exec loop if the virtualenv still can't import.
_BOOTSTRAP_ENV = "ORCA_CODEGEN_BOOTSTRAPPED"

_PROTOC_EXE = "protoc.exe" if os.name == "nt" else "protoc"


def _protoc_dir():
    return CACHE_DIR / f"protoc-{PROTOC_VERSION}"


def _archive_key():
    """Release asset for this host, or None if protobuf ships no build for it."""
    if sys.platform == "win32":
        # There is no win/arm64 release; the x64 build runs under Windows'
        # emulation, which is how the ARM64 CI job gets a protoc.
        return "win64"
    if sys.platform == "darwin":
        return "osx-universal_binary"
    if sys.platform.startswith("linux"):
        machine = platform.machine().lower()
        if machine in ("x86_64", "amd64"):
            return "linux-x86_64"
        if machine in ("aarch64", "arm64"):
            return "linux-aarch_64"
    return None


def _download_protoc():
    """Fetch and unpack the pinned protoc. Returns the binary path, or None."""
    key = _archive_key()
    if key is None:
        print(f"  ERROR: no pinned protoc release for {sys.platform}/{platform.machine()}.")
        print("  Install protoc from your package manager and re-run, or set PROTOC=<path>.")
        return None

    url = (f"https://github.com/protocolbuffers/protobuf/releases/download/"
           f"v{PROTOC_VERSION}/protoc-{PROTOC_VERSION}-{key}.zip")
    print(f"  Downloading protoc {PROTOC_VERSION} ({key})...")
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=CACHE_DIR) as tmp:
        archive = Path(tmp) / "protoc.zip"
        try:
            with urllib.request.urlopen(url, timeout=120) as response:
                archive.write_bytes(response.read())
        except OSError as exc:
            print(f"  ERROR: download failed: {exc}")
            print("  Install protoc manually and re-run, or set PROTOC=<path>.")
            return None

        actual = hashlib.sha256(archive.read_bytes()).hexdigest()
        expected = PROTOC_ARCHIVE_SHA256[key]
        if actual != expected:
            print(f"  ERROR: protoc archive checksum mismatch for {key} {PROTOC_VERSION}:")
            print(f"    expected {expected}")
            print(f"    actual   {actual}")
            return None

        # Unpack the whole archive, not just bin/: protoc resolves the well-known
        # imports (google/protobuf/descriptor.proto) from ../include next to it.
        staging = Path(tmp) / "unpacked"
        with zipfile.ZipFile(archive) as zf:
            zf.extractall(staging)

        target = _protoc_dir()
        if target.exists():
            shutil.rmtree(target)
        # Move into place only once complete, so an interrupted run never leaves
        # a half-extracted toolchain that later runs would happily use.
        shutil.move(str(staging), str(target))

    binary = target / "bin" / _PROTOC_EXE
    if not binary.exists():
        print(f"  ERROR: protoc archive did not contain bin/{_PROTOC_EXE}")
        return None
    binary.chmod(binary.stat().st_mode | 0o755)
    return binary


def find_protoc(allow_download=True):
    """Return the protoc command as a list, or None if it can't be resolved."""
    override = os.environ.get("PROTOC")
    if override:
        return [override]

    on_path = shutil.which("protoc")
    if on_path:
        return [on_path]

    cached = _protoc_dir() / "bin" / _PROTOC_EXE
    if cached.exists():
        return [str(cached)]

    # Honour a pre-existing grpcio-tools install rather than downloading.
    if importlib.util.find_spec("grpc_tools") is not None:
        return [sys.executable, "-m", "grpc_tools.protoc"]

    if allow_download:
        binary = _download_protoc()
        if binary is not None:
            return [str(binary)]
    return None


def missing_packages():
    """pip names of the required Python packages this interpreter can't import."""
    missing = []
    for pip_name, module in PYTHON_PACKAGES.items():
        try:
            found = importlib.util.find_spec(module) is not None
        except (ImportError, ValueError):
            found = False
        if not found:
            missing.append(pip_name)
    return missing


def _venv_python():
    venv_dir = CACHE_DIR / "venv"
    if os.name == "nt":
        return venv_dir / "Scripts" / "python.exe"
    return venv_dir / "bin" / "python"


def bootstrap_python():
    """The cached virtualenv interpreter, if it exists and has the packages."""
    python = _venv_python()
    if not python.exists():
        return None
    probe = subprocess.run([str(python), "-c", "import google.protobuf, yaml"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return python if probe.returncode == 0 else None


def _ensure_venv(missing):
    """Create/reuse .codegen-tools/venv with the required packages installed."""
    python = _venv_python()

    if not python.exists():
        print(f"  Creating codegen virtualenv in {python.parent.parent}...")
        if subprocess.run([sys.executable, "-m", "venv", str(python.parent.parent)]).returncode != 0 \
                or not python.exists():
            return None

    print(f"  Installing {', '.join(missing)} into the codegen virtualenv...")
    result = subprocess.run([str(python), "-m", "pip", "install", "--quiet",
                             "--disable-pip-version-check", *missing])
    return python if result.returncode == 0 else None


def ensure_python_runtime():
    """
    Guarantee protobuf + pyyaml are importable.

    If they aren't, re-run the calling script in a cached virtualenv that has
    them and exit with its status. Keeping the packages out of the caller's
    interpreter is what lets the build scripts work on distros that refuse
    `pip install` into a system Python (PEP 668).
    """
    missing = missing_packages()
    if not missing:
        return

    if os.environ.get(_BOOTSTRAP_ENV):
        # We are already inside the bootstrapped environment: installing again
        # would just fail the same way.
        print(f"  ERROR: {', '.join(missing)} still missing after bootstrap.")
        sys.exit(1)

    # Reuse a complete virtualenv as-is: builds call this every time, and pip
    # would otherwise hit the network on each one.
    python = bootstrap_python() or _ensure_venv(missing)
    if python is None:
        print(f"  ERROR: could not provide {', '.join(missing)}.")
        print(f"  Install them for this interpreter: {sys.executable} -m pip install "
              f"{' '.join(missing)}")
        sys.exit(1)

    env = dict(os.environ, **{_BOOTSTRAP_ENV: "1"})
    sys.exit(subprocess.run([str(python), *sys.argv], env=env).returncode)


def toolchain_ready():
    """
    True if the codegen can run without downloading or installing anything --
    either from this interpreter or by re-execing into an existing bootstrap
    virtualenv. This is what decides whether a build regenerates on proto edits.
    """
    if missing_packages() and bootstrap_python() is None:
        return False
    return find_protoc(allow_download=False) is not None


def main():
    # --check is what ConfigCodegen.cmake probes with: it answers whether this
    # interpreter can regenerate during a build, and must not download, install
    # or create anything while doing so.
    if "--check" in sys.argv[1:]:
        return 0 if toolchain_ready() else 1

    missing = missing_packages()
    print(f"python:  {sys.executable}")
    print(f"packages: {'all present' if not missing else 'missing ' + ', '.join(missing)}")
    protoc = find_protoc()
    print(f"protoc:  {' '.join(protoc) if protoc else 'NOT FOUND'}")
    return 0 if protoc else 1


if __name__ == "__main__":
    sys.exit(main())
