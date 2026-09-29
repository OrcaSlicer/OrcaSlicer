#!/usr/bin/env python3
"""
Generate Python .pyi stubs for OrcaSlicer's pybind11 plugin API.

The script creates a local virtual environment, installs pybind11-stubgen,
imports the built `orca` extension module, and writes stubs to ./typings by
default. It intentionally does not update editor settings.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import sysconfig
import venv
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_VENV = REPO_ROOT / ".venv-stubgen"
DEFAULT_OUTPUT = REPO_ROOT / "typings"
DEFAULT_BUILD_DIR = REPO_ROOT / "build"
DEFAULT_CONFIG = "RelWithDebInfo"
MODULE_NAME = "orca"


def log(message: str) -> None:
    print(f"[orca-stubgen] {message}")


def run(command: list[str], *, env: dict[str, str] | None = None, cwd: Path = REPO_ROOT) -> None:
    log(" ".join(command))
    subprocess.run(command, cwd=cwd, env=env, check=True)


def venv_python(venv_dir: Path) -> Path:
    if os.name == "nt":
        return venv_dir / "Scripts" / "python.exe"
    return venv_dir / "bin" / "python"


def ensure_venv(venv_dir: Path) -> Path:
    python = venv_python(venv_dir)
    if not python.exists():
        log(f"creating virtual environment: {venv_dir}")
        venv.EnvBuilder(with_pip=True).create(venv_dir)
    return python


def ensure_stubgen(python: Path) -> None:
    probe = subprocess.run(
        [str(python), "-m", "pybind11_stubgen", "--help"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if probe.returncode == 0:
        return

    run([str(python), "-m", "pip", "install", "--upgrade", "pip"])
    run([str(python), "-m", "pip", "install", "pybind11-stubgen"])


def module_suffixes() -> list[str]:
    suffixes = [".pyd", ".so", ".dylib"]
    ext_suffix = sysconfig.get_config_var("EXT_SUFFIX")
    if ext_suffix:
        suffixes.insert(0, str(ext_suffix))
    return list(dict.fromkeys(suffixes))


def is_orca_extension(path: Path) -> bool:
    if not path.is_file():
        return False
    if path.name == f"{MODULE_NAME}.so" or path.name == f"{MODULE_NAME}.pyd":
        return True
    return any(path.name.startswith(f"{MODULE_NAME}.") and path.name.endswith(suffix) for suffix in module_suffixes())


def find_module_dir(build_dir: Path, config: str) -> Path | None:
    candidates = [
        build_dir / "src" / "slic3r" / config,
        build_dir / "src" / "slic3r",
        REPO_ROOT / "build" / "src" / "slic3r" / config,
        REPO_ROOT / "build" / "src" / "slic3r",
        REPO_ROOT / "build" / "arm64" / "src" / "slic3r" / config,
        REPO_ROOT / "build" / "arm64" / "src" / "slic3r",
    ]

    for candidate in candidates:
        if not candidate.exists():
            continue
        if any(is_orca_extension(path) for path in candidate.iterdir()):
            return candidate

    for root in dict.fromkeys([build_dir, REPO_ROOT / "build", REPO_ROOT / "build" / "arm64"]):
        if not root.exists():
            continue
        for path in root.rglob(f"{MODULE_NAME}*"):
            if is_orca_extension(path):
                return path.parent
    return None


def build_stubgen_module(build_dir: Path, config: str) -> None:
    run([
        "cmake",
        "--build",
        str(build_dir),
        "--config",
        config,
        "--target",
        "orca_stubgen",
        "--",
    ])


def import_env(module_dir: Path) -> dict[str, str]:
    env = os.environ.copy()
    existing = env.get("PYTHONPATH")
    paths = [str(module_dir)]
    if existing:
        paths.append(existing)
    env["PYTHONPATH"] = os.pathsep.join(paths)
    return env


def verify_import(python: Path, module_dir: Path) -> None:
    code = (
        "import orca; "
        "print(orca.__file__); "
        "assert hasattr(orca, 'printer_agent'), 'orca.printer_agent is missing'"
    )
    run([str(python), "-c", code], env=import_env(module_dir))


def clean_output(output_dir: Path) -> None:
    package_dir = output_dir / MODULE_NAME
    module_stub = output_dir / f"{MODULE_NAME}.pyi"
    if package_dir.exists():
        shutil.rmtree(package_dir)
    if module_stub.exists():
        module_stub.unlink()


def generate_stubs(python: Path, module_dir: Path, output_dir: Path, ignore_errors: bool) -> None:
    command = [str(python), "-m", "pybind11_stubgen", MODULE_NAME, "-o", str(output_dir)]
    if ignore_errors:
        command.append("--ignore-all-errors")
    run(command, env=import_env(module_dir))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--venv", type=Path, default=DEFAULT_VENV, help="Virtual environment path.")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT, help="Directory for generated stubs.")
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR, help="CMake build directory.")
    parser.add_argument("--config", default=DEFAULT_CONFIG, help="CMake configuration name.")
    parser.add_argument("--module-dir", type=Path, help="Directory containing the built orca extension module.")
    parser.add_argument("--build-missing", action="store_true", help="Build the orca_stubgen target if orca is missing.")
    parser.add_argument("--clean", action="store_true", help="Remove existing generated orca stubs before writing new ones.")
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Do not pass --ignore-all-errors to pybind11-stubgen.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    venv_dir = args.venv.resolve()
    output_dir = args.output.resolve()
    build_dir = args.build_dir.resolve()
    module_dir = args.module_dir.resolve() if args.module_dir else None

    python = ensure_venv(venv_dir)
    ensure_stubgen(python)

    if module_dir is None:
        module_dir = find_module_dir(build_dir, args.config)

    if module_dir is None and args.build_missing:
        build_stubgen_module(build_dir, args.config)
        module_dir = find_module_dir(build_dir, args.config)

    if module_dir is None:
        print(
            "Could not find a built orca extension module.\n"
            f"Expected something like: {build_dir / 'src' / 'slic3r' / args.config / 'orca.so'}\n"
            "Build it with:\n"
            f"  cmake --build {build_dir} --config {args.config} --target orca_stubgen --\n"
            "Or rerun this script with --build-missing.",
            file=sys.stderr,
        )
        return 1

    output_dir.mkdir(parents=True, exist_ok=True)
    if args.clean:
        clean_output(output_dir)

    log(f"using module directory: {module_dir}")
    verify_import(python, module_dir)
    generate_stubs(python, module_dir, output_dir, ignore_errors=not args.strict)
    log(f"wrote stubs to: {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
