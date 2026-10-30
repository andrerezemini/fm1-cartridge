#!/usr/bin/env python3
"""Audit every vendored upstream subset against its recorded SHA-256 manifest.

third_party/<name>/UPSTREAM_SHA256.json records the hashes of the files as taken from upstream;
PATCHED lists the files each port deliberately changed (documented in the subset's README) and
excludes them from the comparison. Generated tables and READMEs are not in the manifests."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1] / "third_party"
PATCHED = {
    "clipper": {"firmware/hal/fm1_xip.h"},                 # the isolated flash stub
    "felucca": {"firmware/bsp/chip/fm1_xip.h"},            # the same stub, in felucca-bsp: no user sample slots
    "melodee": {"firmware/src/resources.c"},               # the host arena sizes, overridable by the port
}
failures = []
checked = 0
for name, patched in sorted(PATCHED.items()):
    base = root / name
    manifest = json.loads((base / "UPSTREAM_SHA256.json").read_text())
    for file, expected in manifest.items():
        if file in patched:
            if not (base / file).is_file():
                failures.append(f"{name}/{file} (patched file missing)")
            continue
        path = base / file
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            failures.append(f"{name}/{file}")
        checked += 1
if failures:
    raise SystemExit("Unexpected upstream changes: " + ", ".join(failures))
print(f"{checked} upstream files match in {len(PATCHED)} subsets; documented patches excluded")
