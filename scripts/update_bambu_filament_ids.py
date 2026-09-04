#!/usr/bin/env python3
"""
Generate resources/printers/bambu_filament_ids.json: the map from Orca's
content-addressed filament_id ("OF" + 6 base62 chars, see assign_filament_ids.py)
to Bambu Lab's own AMS/RFID catalog id ("GF..." etc.) for the subset of filament
products Bambu ships.

The map is generated from BambuStudio's OWN shipped BBL bundle, never from
Orca's: Orca's BBL bundle is a fork of Bambu's, tuned and extended
independently, so it is not the source of truth for Bambu's catalog ids.
src/slic3r/Utils/BBLPrinterAgent.cpp loads it at runtime and translates an id
only where it crosses to or from a Bambu printer, so the correspondence never
has to be hand-maintained. See docs/HLSD/filament_id.md.

One row per BambuStudio filament PRODUCT: one commercial line = one
"@base"-declared filament_id, shared by every per-printer/per-nozzle
instantiation of it (BambuStudio follows the same one-product-one-id shape
Orca's own filament_id policy does). A row's key reuses whatever OF id Orca
already ships for that same (filament_vendor, filament_type, family) triple;
a triple Orca does not ship anywhere yet gets a freshly generated one.

Map format:
{
  "source": "https://github.com/bambulab/BambuStudio",
  "bambustudio_commit": "66e405477",
  "generated": "2026-09-04",
  "filaments": {
    "OFhuaUQB": {"bambu_id": "GFB00", "vendor": "Bambu Lab", "type": "ABS", "name": "Bambu ABS"}
  }
}

Run from anywhere:  python3 scripts/update_bambu_filament_ids.py
  (default)          shallow-clone BambuStudio's master branch (sparse: just
                     the BBL filament bundle) and regenerate the map
  --bambustudio-dir DIR
                     read DIR (a BambuStudio resources/profiles checkout)
                     instead of cloning
  --ref REF          clone this BambuStudio ref instead of master
  --output PATH      write here instead of resources/printers/bambu_filament_ids.json

After writing, an informational drift report is printed: BambuStudio families
Orca ships nothing with the same identity for, and a count of Orca's own BBL
families that matched no BambuStudio row. Neither blocks the write; both are
for a human to read.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import datetime

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from assign_filament_ids import (  # noqa: E402
    BAMBU_MAP_PATH,
    OFL,
    OF_ID_RE,
    PROFILES_DIR,
    analyze_tree,
    base_name,
    generate_filament_id,
    load_vendor_filaments,
    print_error,
    print_info,
    print_success,
    resolve_filament_id,
    resolve_triple,
)

BAMBUSTUDIO_REPO = "https://github.com/bambulab/BambuStudio"


# ---------------------------------------------------------------------------
# Row derivation
# ---------------------------------------------------------------------------

def derive_rows(bs_filaments, orca_triples):
    """orca_triples: {(vendor, type, family): orca_id} built from analyze_tree()["triples"]."""
    by_family = {}                       # family -> (bambu_id, triple)
    for rec in bs_filaments.values():
        if not rec["instantiation"]:
            continue
        bambu_id, _src, _entry = resolve_filament_id(rec["name"], bs_filaments, {})
        if not bambu_id:
            continue
        family = base_name(rec["name"])
        triple = resolve_triple(rec["name"], bs_filaments, {})
        prev = by_family.setdefault(family, (bambu_id, triple))
        if prev != (bambu_id, triple):
            raise SystemExit(f"BambuStudio family {family!r} is not one product: {prev} vs {(bambu_id, triple)}")
    rows, seen = {}, {}
    for family, (bambu_id, triple) in sorted(by_family.items()):
        if bambu_id in seen:
            raise SystemExit(f"Bambu id {bambu_id} is shared by {seen[bambu_id]!r} and {family!r}")
        seen[bambu_id] = family
        orca_id = orca_triples.get(triple) or generate_filament_id(*triple)   # reuse a salted id if we ship one
        rows[orca_id] = {"bambu_id": bambu_id, "vendor": triple[0], "type": triple[1], "name": triple[2]}
    return rows


def orca_triples_from_analysis(orca_analysis, needed_triples):
    """{(vendor, type, family): orca_id}, inverted from analyze_tree()["triples"]
    (id -> [[vendor, type, family], ...]) and restricted to `needed_triples`
    (the triples BambuStudio's own bundle ships, i.e. the only ones derive_rows
    will ever look up).

    Kept to ids matching the OF format: our own BBL bundle still declares
    Bambu's GF ids today (analyze_tree already excludes those as BBL-island
    declarations), but filtering here too means a future triple shipped under
    a non-OF scheme can never key a map row, which must always be keyed by
    the OF id the BBL bundle receives once it is re-minted onto the OF space.

    A needed triple owned by more than one OF id would make derive_rows's
    reuse step a guess, so that is a hard error rather than a silent pick.
    Restricting the scan to needed_triples matters for that check: elsewhere
    in the tree one triple legitimately resolves two different ids on purpose
    (e.g. Cubicon's xCeler line declares its own id per printer instead of
    inheriting its family's, sanctioned in scripts/filament_id_snapshot.json)
    and that pre-existing, Bambu-unrelated divergence must not block a map
    that never reads it.
    """
    needed_triples = set(needed_triples)
    result = {}
    for fid, triple_list in orca_analysis["triples"].items():
        if not OF_ID_RE.match(fid):
            continue
        for triple in triple_list:
            triple = tuple(triple)
            if triple not in needed_triples:
                continue
            owner = result.get(triple)
            if owner and owner != fid:
                raise SystemExit(
                    f"Orca triple {triple} resolves to more than one filament_id: "
                    f"{owner!r} and {fid!r}")
            result[triple] = fid
    return result


# ---------------------------------------------------------------------------
# Drift report (informational only)
# ---------------------------------------------------------------------------

def drift_report(rows, orca_analysis):
    """Two triple-identity comparisons between the map just derived and Orca's
    own BBL bundle (never id-based: the two bundles assign filament_id
    independently, so only the (vendor, type, name) identity is comparable).

    Returns print-ready lines: one per BambuStudio row triple with no
    same-triple family in Orca's BBL bundle (upstream ships it, we ship
    nothing with that identity there — sometimes a genuinely missing product,
    sometimes a renamed one), then a count of Orca's own BBL families that
    matched no BambuStudio row (Orca-only products, e.g. a name-drifted
    duplicate of one already counted in the first list).
    """
    row_triples = {(r["vendor"], r["type"], r["name"]) for r in rows.values()}

    bbl_filaments = orca_analysis["vendors"].get("BBL", {})
    ofl_filaments = orca_analysis["vendors"].get(OFL, {})
    orca_families = {}
    for rec in bbl_filaments.values():
        if not rec["instantiation"]:
            continue
        triple = resolve_triple(rec["name"], bbl_filaments, ofl_filaments)
        orca_families.setdefault(base_name(rec["name"]), triple)

    lines = []
    for triple in sorted(row_triples - set(orca_families.values())):
        lines.append(f'upstream ships {triple[2]!r} ({triple[0]}/{triple[1]}), '
                     "we ship nothing with that identity")
    orca_only = [family for family, triple in orca_families.items() if triple not in row_triples]
    lines.append(f"Orca BBL families with no row: {len(orca_only)} Orca-only product(s)")
    return lines


# ---------------------------------------------------------------------------
# Map IO
# ---------------------------------------------------------------------------

def write_map(path, rows, commit, date):
    """Write the map: sorted keys, indent 2, LF, trailing newline."""
    payload = {
        "source": BAMBUSTUDIO_REPO,
        "bambustudio_commit": commit,
        "generated": date,
        "filaments": rows,
    }
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(payload, f, indent=2, ensure_ascii=False, sort_keys=True)
        f.write("\n")


# ---------------------------------------------------------------------------
# BambuStudio fetch
# ---------------------------------------------------------------------------

def run(command, cwd=None):
    """Run a command, streaming its output; raise SystemExit on failure."""
    print("+ " + " ".join(command))
    try:
        subprocess.run(command, cwd=cwd, check=True)
    except subprocess.CalledProcessError as e:
        raise SystemExit(f"command failed ({e.returncode}): {' '.join(command)}") from e


def run_out(command, cwd=None):
    """Run a command and return its stripped stdout; raise SystemExit on failure."""
    try:
        result = subprocess.run(command, cwd=cwd, check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as e:
        raise SystemExit(
            f"command failed ({e.returncode}): {' '.join(command)}\n{e.stderr}") from e
    return result.stdout.strip()


def fetch_bambustudio(ref, workdir):
    run(["git", "clone", "--depth=1", "--filter=blob:none", "--sparse", "--branch", ref,
         BAMBUSTUDIO_REPO + ".git", workdir])
    run(["git", "-C", workdir, "sparse-checkout", "set", "--no-cone",
         "resources/profiles/BBL.json", "resources/profiles/BBL/filament"])
    return os.path.join(workdir, "resources", "profiles"), run_out(["git", "-C", workdir, "rev-parse", "--short", "HEAD"])


def local_dir_commit(dir_path):
    """git rev-parse --short HEAD of dir_path, or "local" when it is not a repo."""
    result = subprocess.run(["git", "-C", dir_path, "rev-parse", "--short", "HEAD"],
                            capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else "local"


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Regenerate resources/printers/bambu_filament_ids.json from "
                    "BambuStudio's shipped BBL bundle.")
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--bambustudio-dir", metavar="DIR",
                        help="a BambuStudio resources/profiles directory to read "
                             "instead of cloning")
    source.add_argument("--ref", default="master",
                        help='BambuStudio git ref to shallow-clone when '
                             '--bambustudio-dir is not given (default: "master")')
    parser.add_argument("--output", default=BAMBU_MAP_PATH,
                        help="output path (default: resources/printers/bambu_filament_ids.json)")
    args = parser.parse_args(argv)

    workdir = None
    if args.bambustudio_dir:
        profiles_dir = args.bambustudio_dir
        commit = local_dir_commit(profiles_dir)
    else:
        workdir = tempfile.mkdtemp(prefix="bambustudio_")
        profiles_dir, commit = fetch_bambustudio(args.ref, workdir)

    try:
        bs_filaments, errors = load_vendor_filaments(profiles_dir, "BBL")
        if errors:
            for e in errors:
                print_error(e)
            raise SystemExit("unreadable BambuStudio filament profile(s)")

        orca_analysis = analyze_tree(PROFILES_DIR)
        needed_triples = {resolve_triple(rec["name"], bs_filaments, {})
                          for rec in bs_filaments.values() if rec["instantiation"]}
        orca_triples = orca_triples_from_analysis(orca_analysis, needed_triples)

        rows = derive_rows(bs_filaments, orca_triples)
        write_map(args.output, rows, commit, datetime.date.today().isoformat())
        print_success(f"wrote {len(rows)} row(s) to {args.output} (BambuStudio @ {commit})")

        for line in drift_report(rows, orca_analysis):
            print_info(line)
    finally:
        if workdir:
            shutil.rmtree(workdir, ignore_errors=True)

    return 0


if __name__ == "__main__":
    sys.exit(main())
