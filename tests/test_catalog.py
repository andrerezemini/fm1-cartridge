#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""web/catalog.json says what the repository builds: each ported synth's upstream revision is the one its adapter
(synth/<id>_synth.c) and CMakeLists.txt carry, Sloop's is the one integrations/sloop-fm1/manifest.json patches,
every profile it names is one firmware/build.sh knows, and candidates are only listed, never built."""
import json
import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
catalog = json.loads((root / "web" / "catalog.json").read_text())
failures = []
upstreams = {u["id"]: u for u in catalog["upstreams"]}
cmake = (root / "CMakeLists.txt").read_text()
ported_in_cmake = dict(re.findall(r"^fm1_port_targets\((\w+) \d+ ([0-9a-f]{40})\)", cmake, re.M))
build_sh = (root / "firmware" / "build.sh").read_text()
profiles = set(re.findall(r"^\s+([a-z0-9-]+)\) defaults=", build_sh, re.M))

ported = [s for s in catalog["synths"] if s["status"] == "ported"]
if {s["id"] for s in ported} != set(ported_in_cmake):
    failures.append(f"ported synths {sorted(s['id'] for s in ported)} != CMake ports {sorted(ported_in_cmake)}")
for s in ported:
    u = upstreams.get(s.get("upstream"))
    if not u:
        failures.append(f"{s['id']}: no upstream entry")
        continue
    adapter = (root / "synth" / f"{s['id']}_synth.c").read_text()
    m = re.search(r'\.revision = "([0-9a-f]{40})"', adapter)
    for where, rev in (("CMakeLists.txt", ported_in_cmake.get(s["id"])), (f"synth/{s['id']}_synth.c", m and m[1])):
        if rev != u["revision"]:
            failures.append(f"{s['id']}: catalog revision {u['revision']} != {where} {rev}")
    for p in s.get("profiles", []):
        if p not in profiles:
            failures.append(f"{s['id']}: profile {p} unknown to firmware/build.sh")

manifest = json.loads((root / "integrations" / "sloop-fm1" / "manifest.json").read_text())
if upstreams["sloop"]["revision"] != manifest["upstream_commit"]:
    failures.append(f"sloop: catalog {upstreams['sloop']['revision']} != manifest {manifest['upstream_commit']}")
lock = (root / "firmware" / "dependencies.lock").read_text()
block = re.search(r"^  espressif/usb_host_uac:\n((?:    .*\n)+)", lock, re.M)   # the component's own keys: 4 spaces
m = block and re.search(r"^    version: (\S+)$", block[1], re.M)
if not m or m[1] != upstreams["usb_host_uac"]["version"]:
    failures.append(f"usb_host_uac: catalog {upstreams['usb_host_uac']['version']} != dependencies.lock {m and m[1]}")
for s in catalog["synths"]:
    if s["status"] not in ("ported", "candidate"):
        failures.append(f"{s['id']}: status {s['status']}")
    if s["status"] == "candidate" and not re.fullmatch(r"[\w.-]+/[\w.-]+", s.get("repo", "")):
        failures.append(f"{s['id']}: a candidate needs repo owner/name")
if len({s["id"] for s in catalog["synths"]}) != len(catalog["synths"]):
    failures.append("duplicate synth ids")

page = (root / "web" / "index.html").read_text()
imports = json.loads(re.search(r'<script type="importmap">(.*?)</script>', page, re.S)[1])
url = imports["imports"]["esptool-js"]
if not re.search(r"@\d+\.\d+\.\d+/", url) or not imports.get("integrity", {}).get(url, "").startswith("sha384-"):
    failures.append(f"esptool-js must be pinned to an exact version with an integrity hash: {url}")

if failures:
    sys.exit("catalog: " + "; ".join(failures))
print(f"catalog: {len(ported)} ported synths and {len(catalog['upstreams'])} upstreams match the build")
