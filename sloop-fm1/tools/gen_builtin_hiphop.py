#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The SAMPLE engine's built-in sets, hip-hop edition: writes the WAVs gen_samples.py builds into
the firmware, under assets/samples-cc0/<SET>/ (all CC0; CREDITS.txt next to them).

    python tools/gen_builtin_hiphop.py      (numpy, scipy, soundfile; fetches the sources once)

Sets (gen_samples.py CC0_SETS gives their order and kind; PIANO, the grand, is cut from VCSL as
ATTRIBUTION.txt lists):
  BASS   jazz upright bass, pizz   (VSCO-2 CE Solo Contrabass) - two octaves under the keys
  VIBES  vibraphone, hard mallets  (VCSL)
  HORNS  trumpet + trombone stabs  (VSCO-2 CE)
  STRGS  violin + viola stabs      (VSCO-2 CE)
  FLUTE  kept as it was            (VSCO-2 CE, tools/fetch_cc0.py)
  SCRCH  scratch, backspin, rewind (Sonic Pi sample set, CC0)
Every file is named NN_<note>_m<MIDI root>.wav: gen_samples.py takes the root from _m<n>.
The chain (retune to A440, then the "dusty" record / old sampler colour) is gen_hiphop_pack.py's.
"""
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_hiphop_pack as g  # noqa: E402

CC0 = g.ROOT / "assets" / "samples-cc0"

# set: (zones [(MIDI root it sounds at, seconds)], builder, chain, transpose of the stored root)
SETS = {
    "BASS": ([(28, 0.9), (35, 0.9), (42, 0.9), (49, 0.85)], g.CB, dict(lp=4500, drive=1.8, warm=3.0, wow=0.0), 24),
    "VIBES": ([(53, 1.0), (62, 1.0), (71, 1.0)], g.VIB, dict(lp=10000, drive=1.2, warm=1.0), 0),
    "HORNS": ([(55, 0.6), (62, 0.6), (69, 0.6), (76, 0.6)],
              g.section((g.TPT, 0, 1.0), (g.TBN, 12, 0.75)), dict(lp=9500, drive=2.2, warm=1.5), 0),
    "STRGS": ([(55, 0.6), (64, 0.6), (73, 0.6)],
              g.section((g.VLN, 0, 1.0), (g.VLA, 12, 0.7)), dict(lp=10000, drive=1.8, warm=1.5), 0),
}
SCRATCH = [("vinyl_scratch", 57, 0.8), ("vinyl_backspin", 65, 0.8), ("vinyl_rewind", 72, 1.0)]

CREDITS = """Samples in the SAMPLE engine's built-in sets. All CC0 1.0 (public domain).

PIANO  VCSL (Versilian Community Sample Library), Grand Piano (Steinway B), sustain, close mics
       (until 2.1 the piano was VCSL's Upright Piano (Knight), DUSTY; it is no longer in the tree)
VIBES  VCSL, Vibraphone (hard mallets)                    https://github.com/sgossner/VCSL
BASS   VSCO-2 Community Edition, Solo Contrabass pizzicato
HORNS  VSCO-2 Community Edition, Trumpet + Tenor Trombone staccato
STRGS  VSCO-2 Community Edition, Violin + Viola sections spiccato
FLUTE  VSCO-2 Community Edition, Flute (susvib)           https://github.com/sgossner/VSCO-2-CE
SCRCH  Sonic Pi sample set (freesound.org CC0 recordings): vinyl_scratch (hello_flowers),
       vinyl_backspin (il112), vinyl_rewind (TasmanianPower)
       https://github.com/sonic-pi-net/sonic-pi/tree/main/etc/samples
KIT    VCSL: the acoustic drum kit of the GM map (bass drum, snare, side stick, claps, hi-hat, toms,
       suspended cymbal, cowbell) and hand percussion (tambourine, shaker, conga, claves, woodblock)

Thanks to Versilian Studios / Sam Gossner and to the Sonic Pi project.
Retuned, cut and coloured by tools/gen_builtin_hiphop.py (gen_hiphop_pack.py's chain).
"""


ATT_HEAD = """SLOOP SAMPLE engine - built-in sets: source material
Licence: CC0 1.0 Universal (public domain dedication)
  https://creativecommons.org/publicdomain/zero/1.0/
Sources:
  VSCO-2 Community Edition  https://github.com/sgossner/VSCO-2-CE  (Versilian Studios, CC0-1.0)
  VCSL                      https://github.com/sgossner/VCSL       (Versilian Studios, CC0-1.0)
  Sonic Pi sample set       https://github.com/sonic-pi-net/sonic-pi/tree/main/etc/samples (CC0, freesound.org)
Each file is retuned, cut and coloured (tools/gen_builtin_hiphop.py) from:

"""


def main():
    used, real = [], g.source

    def source(url, nominal, target):                      # (records what each file is made of)
        used.append(url[len(g.RAW):])
        return real(url, nominal, target)
    g.source = source
    att = []
    for old in ("DUSTY", "TRANH", "SAX"):                  # (sets of earlier releases; PIANO is kept)
        shutil.rmtree(CC0 / old, ignore_errors=True)
    total = 0
    for name, (zones, make, chain, tr) in SETS.items():
        d = CC0 / name
        shutil.rmtree(d, ignore_errors=True)
        print(name)
        for k, (root, sec) in enumerate(zones):
            used.clear()
            x = g.shape(g.dusty(make(root), seed=k, **chain), sec, fade=0.25)
            fn = f"{k:02d}_{g.note_name(root).replace('#', 's')}_m{root + tr}.wav"
            g.write(d / fn, x)
            att.append(f"{name}/{fn}  <-  " + " + ".join(used))
            total += len(x)
    d = CC0 / "SCRCH"
    shutil.rmtree(d, ignore_errors=True)
    print("SCRCH")
    for k, (src, root, sec) in enumerate(SCRATCH):
        x, sr = g.load(g.SONICPI + f"{src}.flac")
        x = g.to_rate(x[g.onset(x):], sr)
        x = g.shape(g.dusty(x, lp=11000, drive=1.4, warm=1.5, wow=0.0, seed=k), sec, fade=0.15)
        g.write(d / f"{k:02d}_{src}_m{root}.wav", x)
        att.append(f"SCRCH/{k:02d}_{src}_m{root}.wav  <-  sonic-pi-net/sonic-pi/main/etc/samples/{src}.flac")
        total += len(x)
    (CC0 / "CREDITS.txt").write_text(CREDITS, encoding="utf-8")
    keep = [ln for ln in (CC0 / "ATTRIBUTION.txt").read_text(encoding="utf-8").splitlines()
            if ln.startswith(("FLUTE/", "KIT/", "PIANO/"))] if (CC0 / "ATTRIBUTION.txt").exists() else []
    (CC0 / "ATTRIBUTION.txt").write_text(ATT_HEAD + "\n".join(att + keep) + "\n", encoding="utf-8")
    print(f"built-in sets: {total / g.RATE:.1f} s written (FLUTE, KIT and PIANO kept)")


if __name__ == "__main__":
    main()
