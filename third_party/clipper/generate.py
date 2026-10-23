#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Regenerate only the committed CLIPPER_SYNTH=1 tables, offline."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parent
out = root / "generated"
out.mkdir(exist_ok=True)
for script, header in (
    ("gen_tables.py", "felucca_tables.h"),
    ("gen_fm6_patches.py", "felucca_fm6.h"),
    ("gen_drumkits.py", "felucca_drumkits.h"),
    ("gen_wavetables.py", "felucca_wavetables.h"),
):
    subprocess.run([sys.executable, str(root / "tools" / script), str(out / header)], check=True)

sys.path.insert(0, str(root / "tools"))
import gen_samples  # noqa: E402

(out / "felucca_samples.h").write_text(
    "/* Generated from upstream gen_samples.clipper_block(); CLIPPER_SYNTH=1 only. */\n"
    "#pragma once\n" + gen_samples.clipper_block()
)
print("Clipper-only sample tables regenerated.")
