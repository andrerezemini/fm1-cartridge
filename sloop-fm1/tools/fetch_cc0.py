#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""Download the CC0 samples of the FLUTE set and the GM kit's hand percussion into
assets/samples-cc0/ (the other built-in sets: tools/gen_builtin_hiphop.py).

The files are already in the tree; this re-creates them from the source:
  https://github.com/sgossner/VSCO-2-CE  (CC0 1.0)
  https://github.com/sgossner/VCSL       (CC0 1.0)
Only the first RANGE bytes of each WAV are downloaded.
"""
import json
import re
import sys
import urllib.parse
import urllib.request
from pathlib import Path

DEST = Path(__file__).resolve().parents[1] / "assets" / "samples-cc0"
RANGE = 420_000

VCSL = "VCSL"
VSCO = "VSCO-2-CE"
# set: (repo, directory, [filename regexes, one file per regex, first match wins])
PICK = {
    "FLUTE": (VSCO, "Woodwinds/Flute/susvib", [r"_C4_v1_1", r"_C5_v1_1", r"_C6_v1_1"]),
    "KIT": (VCSL, None, [
        ("Idiophones/Struck Idiophones/Tambourine 1", r"Tamb1_Hit_v2"),
        ("Idiophones/Struck Idiophones/Shaker, Small", r"ShakerDouble_Down_rr1"),
        ("Membranophones/Struck Membranophones/Conga", r"Conga_HitN_v2_rr1"),
        ("Idiophones/Struck Idiophones/Claves", r"Claves1_Hit_v2"),
        ("Idiophones/Struck Idiophones/Woodblock", r"wood_click_mp"),
        # the acoustic kit (gen_samples.py names them by role and cuts them; ATTRIBUTION.txt lists each)
        ("Membranophones/Struck Membranophones/Bass Drum 1", r"BDrumNew_hit_v5_rr1"),
        ("Membranophones/Struck Membranophones/Snare Drum, Modern 1", r"Snare2_HitSN_v7_rr1"),
        ("Membranophones/Struck Membranophones/Legacy Snares/OldSnare", r"snare_rim"),
        ("Idiophones/Struck Idiophones/Claps", r"Clap_rr1"),
        ("Idiophones/Struck Idiophones/Hi-Hat Cymbal", r"HiHat_HitC_v3_rr1"),
        ("Idiophones/Struck Idiophones/Hi-Hat Cymbal", r"HiHat_HitO_rr1"),
        ("Membranophones/Struck Membranophones/Tom 2/Stick", r"TomL_HitS_v4_rr1"),
        ("Membranophones/Struck Membranophones/Tom 1/Stick", r"TomH_HitS_v4_rr1"),
        ("Idiophones/Struck Idiophones/Suspended Cymbal 2", r"susCymb2_hit_f1"),
        ("Idiophones/Struck Idiophones/Suspended Cymbal 2", r"susCymb2_hit_stick_mf1"),
        ("Idiophones/Struck Idiophones/Cowbells", r"Cowbell1_Normal_v3_rr1"),
    ]),
}


def tree(repo, cache={}):
    if repo not in cache:
        url = f"https://api.github.com/repos/sgossner/{repo}/git/trees/master?recursive=1"
        with urllib.request.urlopen(url) as r:
            cache[repo] = [e["path"] for e in json.load(r)["tree"] if e["path"].lower().endswith(".wav")]
    return cache[repo]


def fetch(repo, path, dest):
    if dest.exists():
        return 0
    url = f"https://raw.githubusercontent.com/sgossner/{repo}/master/" + urllib.parse.quote(path)
    req = urllib.request.Request(url, headers={"Range": f"bytes=0-{RANGE - 1}"})
    with urllib.request.urlopen(req) as r:
        data = r.read()
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)
    return len(data)


def main():
    total, credits = 0, []
    for name, (repo, folder, pats) in PICK.items():
        files = tree(repo)
        for k, p in enumerate(pats):
            d, rx = (folder, p) if folder else p
            cand = sorted(f for f in files if f.rsplit("/", 1)[0] == d and re.search(rx, f.rsplit("/", 1)[1]))
            if not cand:
                print(f"  {name}: no match for {rx} in {d}")
                continue
            src = cand[0]
            dest = DEST / name / f"{k:02d}_{src.rsplit('/', 1)[1]}"
            n = fetch(repo, src, dest)
            total += n
            credits.append(f"{name}/{dest.name}  <-  sgossner/{repo}: {src}")
            print(f"  {name:8s} {dest.name}  {n // 1024} KiB")
    import datetime
    (DEST / "ATTRIBUTION.txt").write_text(
        "Felucca SAMPLE engine - source material\n"
        f"Retrieved: {datetime.date.today().isoformat()} (first ~400 KB of each file)\n"
        "Licence: CC0 1.0 Universal (public domain dedication)\n"
        "  https://creativecommons.org/publicdomain/zero/1.0/\n"
        "Sources: Versilian Studios (Sam Gossner)\n"
        "  VSCO-2 Community Edition  https://github.com/sgossner/VSCO-2-CE  (repository licence: CC0-1.0)\n"
        "  VCSL                      https://github.com/sgossner/VCSL       (repository licence: CC0-1.0)\n\n"
        + "\n".join(credits) + "\n")
    (DEST / "CREDITS.txt").write_text(
        "Samples from Versilian Studios' VSCO-2 Community Edition and VCSL, both released\n"
        "under CC0 1.0 (public domain). Thanks to Versilian Studios / Sam Gossner.\n"
        "Only the first ~1 s of each file is used.\n\n" + "\n".join(credits) + "\n")
    print(f"fetched {total / 1e6:.1f} MB into {DEST}")


if __name__ == "__main__":
    sys.exit(main())
