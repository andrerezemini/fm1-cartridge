#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Back up the FM-1's exportable objects over USB-MIDI (Sloop editor protocol: INFO, BK_LIST,
BK_GET), in the Sloop editor's sloop-backup JSON format, checking each object's length and CRC.
  python3 tools/fm1_backup.py OUT.json [REFERENCE.json]
With REFERENCE.json, every object is compared byte for byte afterwards and the result is written
next to OUT.json (OUT.comparison.json). The FM-1 must be connected to the computer and running
the firmware (not the update loader). Needs mido with python-rtmidi, as sloop-fm1/tools/fm1_install.py."""
import base64, datetime, json, sys, time, zlib
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'sloop-fm1' / 'tools'))
from fm1_install import MidoBackend, Updater
def unpack7(a):
    # Editor-protocol pack7 (editor.c ed_pack7): groups of a top-bits byte then up to 7 bytes.
    # Not the bit-stream pack7 of the OTA transport in fm1_install.
    out = bytearray()
    for i in range(0, len(a), 8):
        top = a[i]
        for j in range(7):
            if i + 1 + j < len(a):
                out.append(a[i + 1 + j] | ((top >> j) & 1) << 7)
    return bytes(out)
HDR = bytes([0xF0, 0x7D, 0x46, 0x4C])
def u35(b, i): return b[i] | b[i + 1] << 7 | b[i + 2] << 14 | b[i + 3] << 21 | b[i + 4] << 28
def put35(v): return bytes((v >> (7 * k)) & 0x7F for k in range(5))
def req(link, cmd, args=b'', timeout=5.0, retries=2):
    for _ in range(retries + 1):
        link.drain()
        if not link.send(HDR + bytes([cmd]) + bytes(args) + b'\xF7'):
            raise SystemExit('MIDI send failed')
        end = time.monotonic() + timeout
        while (left := end - time.monotonic()) > 0:
            p = link.read(left)
            if p and p[:4] == HDR and len(p) > 5 and p[4] == cmd:
                return p[5:-1]
    raise SystemExit(f'no reply to editor command {cmd}')
out = Path(sys.argv[1])
assert not out.exists(), f'{out} exists'
dev = Updater(MidoBackend(), 'Felucca').find()
assert dev and not dev.id.loader, 'FM-1 not found: connect it and make sure it runs the firmware, not the loader'
link = dev.link
firmware = req(link, 1).split(b'\0')[0].decode('latin-1')
lst = req(link, 34)
assert lst[0] == 0, f'BK_LIST rc {lst[0]}'
count, objects, i = lst[1], [], 2
for _ in range(count):
    objects.append((lst[i], u35(lst, i + 1), u35(lst, i + 6))); i += 11
print(f'Device {dev.id.text}; firmware {firmware}; {count} objects', flush=True)
items = []
for oid, length, crc in objects:
    data = bytearray()
    while len(data) < length:
        n = min(256, length - len(data))
        r = req(link, 35, bytes([oid]) + put35(len(data)) + bytes([n & 0x7F, n >> 7]), timeout=1.5)
        assert r[0] == oid and r[1] == 0, f'BK_GET object {oid} rc {r[1]} (rc 5: list again)'
        assert u35(r, 2) == len(data) and (r[7] | r[8] << 7) == n, 'BK_GET echo mismatch'
        data += bytes(unpack7(r[9:]))[:n]
    assert zlib.crc32(bytes(data)) == crc, f'object {oid}: CRC mismatch'
    print(f'Object {oid}: {length} bytes, CRC {crc:08x} verified', flush=True)
    items.append({'id': oid, 'len': length, 'crc': crc, 'data': base64.b64encode(bytes(data)).decode()})
link.close()
out.write_text(json.dumps({'format': 'sloop-backup', 'version': 1, 'firmware': firmware,
                           'date': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                           'objects': items}, indent=1) + '\n')
print(f'Backup written: {out} ({count} objects)', flush=True)
if len(sys.argv) > 2:
    ref = {o['id']: o for o in json.loads(Path(sys.argv[2]).read_text())['objects']}
    same = [o['id'] for o in items if o['id'] in ref and ref[o['id']]['data'] == o['data']]
    diff = [o['id'] for o in items if o['id'] not in ref or ref[o['id']]['data'] != o['data']]
    print(f'Compared with {sys.argv[2]}: identical {same}; different or new {diff}', flush=True)
    out.with_suffix('.comparison.json').write_text(json.dumps({'reference': sys.argv[2], 'identical': same, 'different': diff}, indent=2) + '\n')
