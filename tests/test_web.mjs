// SPDX-License-Identifier: GPL-3.0-only
// The cartridge manager's flash structures (web/espimage.js) against independent references: Node's MD5 and
// CRC-32, and images laid out the way ESP-IDF 5.3 writes them (partitions.csv, esp_app_desc_t, otadata).
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import * as zlib from "node:zlib";
import * as img from "../web/espimage.js";

const le32 = (v) => [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff];
const ascii = (s, n) => { const b = new Uint8Array(n); b.set(Buffer.from(s, "ascii").subarray(0, n)); return b; };

// --- partition table: firmware/partitions.csv as gen_esp32part.py lays it out
function entry(type, subtype, offset, size, label) {
  const e = new Uint8Array(32);
  e.set([0xaa, 0x50, type, subtype, ...le32(offset), ...le32(size)]);
  e.set(ascii(label, 16), 12);
  return e;
}
const rows = [];
for (const line of readFileSync(new URL("../firmware/partitions.csv", import.meta.url), "utf8").split("\n")) {
  if (!line.trim() || line.startsWith("#")) continue;
  const [name, type, subtype, offset, size] = line.split(",").map((x) => x.trim());
  const sub = type === "app" ? 0x10 + Number(subtype.slice(4)) : { nvs: 2, ota: 0, phy: 1 }[subtype];
  rows.push(entry(type === "app" ? 0 : 1, sub, Number(offset), Number(size), name));
}
const table = new Uint8Array(img.PARTITION_TABLE_SIZE).fill(0xff);
rows.forEach((r, i) => table.set(r, 32 * i));
const md5row = new Uint8Array(32).fill(0xff);
md5row.set([0xeb, 0xeb]);
table.set(md5row, 32 * rows.length);
const parts = img.parsePartitionTable(table);
assert.deepEqual(parts.map((p) => p.label), ["nvs", "otadata", "phy_init", "ota_0", "ota_1", "ota_2"]);
const slots = img.appSlots(parts);
assert.deepEqual(slots.map((p) => [p.slot, p.offset, p.size]), [["ota_0", 0x10000, 0x140000], ["ota_1", 0x150000, 0x140000], ["ota_2", 0x290000, 0x140000]]);
assert.equal(parts.find((p) => p.kind === "ota").offset, 0xd000);
assert.equal(img.tableEntries(table).length, 32 * rows.length);
assert.equal(img.parsePartitionTable(new Uint8Array(64).fill(0xff)), null, "an erased table is no table");
assert.equal(img.parsePartitionTable(new Uint8Array(64)), null, "zeros are no table");

// --- otadata: the bootloader's CRC is esp_rom_crc32_le(UINT32_MAX, &seq, 4), as an ESP32-S3 wrote it (seq 3 and 5
// read from a cartridge's otadata) and as zlib's crc32 seeded with ~0 computes it (Node 22.2 and later)
assert.equal(img.otaSeqCrc(3), 0xed4a5011);
assert.equal(img.otaSeqCrc(5), 0xc8210fcd);
if (zlib.crc32) for (const seq of [1, 2, 0x1234, 0xfffffffe]) assert.equal(img.otaSeqCrc(seq), zlib.crc32(Buffer.from(le32(seq)), 0xffffffff), `crc of ${seq}`);
function otaEntry(seq, state, crc = img.otaSeqCrc(seq)) {
  const e = new Uint8Array(32).fill(0xff);
  e.set(le32(seq), 0);
  e.set(le32(state), 24);
  e.set(le32(crc), 28);
  return e;
}
const ota = new Uint8Array(0x2000).fill(0xff);
assert.deepEqual([img.parseOtadata(ota, 3).blank, img.parseOtadata(ota, 3).bootOta], [true, 0], "blank: the first slot boots");
ota.set(otaEntry(3, 2), 0);
ota.set(otaEntry(5, 1), 0x1000);
let od = img.parseOtadata(ota, 3);
assert.deepEqual([od.bootOta, od.state, od.seq], [1, "PENDING_VERIFY", 5], "seq 5 of 3 slots boots ota_1");
ota.set(otaEntry(7, 4, 0), 0x1000);                    // a torn write: its CRC is wrong, the other sector rules
od = img.parseOtadata(ota, 3);
assert.deepEqual([od.bootOta, od.state, od.entries[1].valid], [2, "VALID", false]);

// --- an application image: header, first segment header, esp_app_desc_t (ESP-IDF 5.3)
function appImage({ chip = img.CHIP_ESP32S3, version = "CLIPPER", project = img.CARTRIDGE_PROJECT, size = 4096, flash = 0x2f } = {}) {
  const b = new Uint8Array(size);
  b.set([0xe9, 4, 2, flash, ...le32(0x40375000), 0xee, 0, 0, 0, chip & 0xff, chip >> 8]);
  b.set([...le32(0x3c000020), ...le32(0x1000)], 24);
  const d = 0x20;
  b.set(le32(0xabcd5432), d);
  b.set(ascii(version, 32), d + 16);
  b.set(ascii(project, 32), d + 48);
  b.set(ascii("20:25:43", 16), d + 80);
  b.set(ascii("Oct 22 2026", 16), d + 96);
  b.set(ascii("v5.3.6", 32), d + 112);
  b.fill(0xab, d + 144, d + 176);
  return b;
}
const head = img.parseAppHead(appImage().subarray(0, img.APP_HEAD_SIZE));
assert.equal(head.kind, "image");
assert.deepEqual([head.chipId, head.flashMode, head.flashSize, head.flashFreq], [img.CHIP_ESP32S3, "dio", "4MB", "80m"]);
assert.deepEqual([head.desc.version, head.desc.projectName, head.desc.idfVer, head.desc.date], ["CLIPPER", "fm1_cartridge", "v5.3.6", "Oct 22 2026"]);
assert.equal(head.desc.elfSha256, "ab".repeat(32));
assert.equal(img.slotName(head), "CLIPPER");
assert.equal(img.slotName(img.parseAppHead(appImage({ version: "a very long name" }))), "AVERYLONGNAM", "12 printable characters, upper case, as slots.c");
assert.equal(img.slotName(img.parseAppHead(new Uint8Array(img.APP_HEAD_SIZE).fill(0xff))), "EMPTY");
assert.equal(img.parseAppHead(new Uint8Array(img.APP_HEAD_SIZE)).kind, "foreign");

let c = img.checkSlotImage(appImage(), 0x140000);
assert.deepEqual([c.problems, c.warnings], [[], []]);
c = img.checkSlotImage(appImage({ chip: 0 }), 0x140000);
assert.match(c.problems[0], /not the ESP32-S3/);
c = img.checkSlotImage(appImage({ project: "blink" }), 0x140000);
assert.deepEqual(c.problems, []);
assert.match(c.warnings[0], /rolls it back/);
c = img.checkSlotImage(appImage({ size: 0x140001 }), 0x140000);
assert.match(c.problems[0], /does not fit/);
c = img.checkSlotImage(new Uint8Array(4096).fill(0x42), 0x140000);
assert.match(c.problems[0], /not an ESP32 application image/);

// --- MD5, against Node's, across the padding boundaries
for (const n of [0, 1, 55, 56, 63, 64, 65, 119, 120, 1000, 65537, 300000]) {
  const data = new Uint8Array(n).map((_, i) => (i * 131 + n) & 0xff);
  assert.equal(img.md5hex(data), createHash("md5").update(data).digest("hex"), `md5 of ${n} bytes`);
}
console.log("web manager: partition table, otadata, app image, slot checks and MD5 pass");
