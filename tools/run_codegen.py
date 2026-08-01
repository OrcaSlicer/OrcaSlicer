#!/usr/bin/env python3
"""
Convenience script: runs the codegen pipeline.

1. Compile .proto -> binary descriptor set (protoc)
2. Generate C++ from descriptors (config_codegen.py)
3. Validate output against original

The toolchain (protoc, protobuf, pyyaml) is resolved by codegen_toolchain.py, so
this script is the single entry point every build script and CI job calls -- no
`pip install` lines needed around it.

Usage:
    python tools/run_codegen.py                 # full pipeline
    python tools/run_codegen.py --validate-only # just validate
"""

import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import codegen_toolchain  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
PROTO_DIR = ROOT / "src" / "PrintConfigs"
CODEGEN_OUT = ROOT / "src" / "slic3r" / "GUI" / "generated"
DESC_FILE = ROOT / "config.desc"
LAYOUT_YAML = PROTO_DIR / "layout.yaml"


def run(cmd, **kwargs):
    print(f"  $ {' '.join(str(c) for c in cmd)}")
    result = subprocess.run(cmd, **kwargs)
    if result.returncode != 0:
        print(f"  FAILED (exit code {result.returncode})")
        return False
    return True


def step_compile():
    print("\n=== Step 1: Compile .proto -> descriptor set ===")
    proto_files = [f for f in PROTO_DIR.glob("*.proto") if not f.name.endswith("_gen.proto") and f.name != "config_metadata.proto"]
    if not proto_files:
        print("  ERROR: No .proto files found")
        return False

    protoc = codegen_toolchain.find_protoc()
    if protoc is None:
        return False

    return run(protoc + [
        f"--proto_path={PROTO_DIR}",
        f"--descriptor_set_out={DESC_FILE}",
        "--include_imports",
    ] + [str(f) for f in proto_files])


def step_generate():
    print("\n=== Step 2: Generate C++ from descriptors + layout.yaml ===")
    return run([sys.executable, str(ROOT / "tools" / "config_codegen.py"),
                str(DESC_FILE), str(CODEGEN_OUT)])


def step_lint():
    print("\n=== Lint: proto sanity checks ===")
    return run([sys.executable, str(ROOT / "tools" / "config_codegen.py"),
                str(DESC_FILE), str(CODEGEN_OUT), "--lint-only"])


def step_validate():
    print("\n=== Step 3: Validate ===")
    return run([sys.executable, str(ROOT / "tools" / "validate_codegen.py")])


def main():
    parser = argparse.ArgumentParser(description="Run OrcaSlicer config codegen pipeline")
    parser.add_argument("--validate-only", action="store_true",
                        help="Only run validation")
    parser.add_argument("--no-validate", action="store_true",
                        help="Skip validation step (used by cmake build)")
    args = parser.parse_args()

    # Re-execs into a virtualenv with protobuf/pyyaml if this interpreter lacks them.
    codegen_toolchain.ensure_python_runtime()

    if args.validate_only:
        # Compile + lint the protos, then check the generated files against PrintConfig.cpp.
        sys.exit(0 if (step_compile() and step_lint() and step_validate()) else 1)

    for name, fn in [("Compile", step_compile), ("Generate", step_generate)]:
        if not fn():
            print(f"\n*** Pipeline FAILED at: {name} ***")
            sys.exit(1)

    if not args.no_validate:
        if not step_validate():
            print("\n*** Validate FAILED (run with --no-validate to skip) ***")
            sys.exit(1)

    print("\n=== Pipeline completed successfully ===")


if __name__ == "__main__":
    main()
