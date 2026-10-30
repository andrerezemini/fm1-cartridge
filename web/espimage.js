// SPDX-License-Identifier: GPL-3.0-only
// ESP32-S3 flash structures the cartridge manager reads and checks: the partition table, the
// bootloader's otadata, and the header and esp_app_desc_t of an application image. Pure functions
// over Uint8Array, shared by the page and its tests (tests/test_web.mjs).

export const PARTITION_TABLE_OFFSET = 0x8000;
export const PARTITION_TABLE_SIZE = 0xc00;
export const APP_HEAD_SIZE = 0x120;            // image header 24 + first segment header 8 + esp_app_desc_t 256
export const CHIP_ESP32S3 = 0x0009;            // esp_image_header_t.chip_id
export const CARTRIDGE_PROJECT = "fm1_cartridge";   // esp_app_desc_t.project_name of every slot firmware here

const u16 = (b, o) => b[o] | (b[o + 1] << 8);
const u32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
const text = (b, o, n) => {
  let s = "";
  for (let i = o; i < o + n && b[i]; i++) s += String.fromCharCode(b[i]);
  return s;
};
const hex = (b) => Array.from(b, (x) => x.toString(16).padStart(2, "0")).join("");

export const APP_SUBTYPE_FACTORY = 0x00, APP_SUBTYPE_OTA_MIN = 0x10, APP_SUBTYPE_OTA_MAX = 0x1f;
const DATA_SUBTYPES = { 0x00: "ota", 0x01: "phy", 0x02: "nvs", 0x03: "coredump", 0x04: "nvs_keys", 0x05: "efuse", 0x80: "esphttpd", 0x81: "fat", 0x82: "spiffs", 0x83: "littlefs" };

// The partition table at 0x8000: 32-byte entries (magic 0xAA50), an optional MD5 entry (0xEBEB), then 0xFF.
// Returns null when the first entry is not a partition (an erased or foreign flash).
export function parsePartitionTable(b) {
  const parts = [];
  for (let o = 0; o + 32 <= b.length; o += 32) {
    const magic = u16(b, o);
    if (magic === 0xebeb || magic === 0xffff) break;
    if (magic !== 0x50aa) return parts.length ? parts : null;
    const type = b[o + 2], subtype = b[o + 3];
    const p = { type, subtype, offset: u32(b, o + 4), size: u32(b, o + 8), label: text(b, o + 12, 16), flags: u32(b, o + 28) };
    if (type === 0x00) {
      p.kind = "app";
      p.slot = subtype === APP_SUBTYPE_FACTORY ? "factory" : subtype >= APP_SUBTYPE_OTA_MIN && subtype <= APP_SUBTYPE_OTA_MAX ? `ota_${subtype - APP_SUBTYPE_OTA_MIN}` : `app_${subtype}`;
    } else if (type === 0x01) p.kind = DATA_SUBTYPES[subtype] || `data_${subtype}`;
    else p.kind = `type_${type}`;
    parts.push(p);
  }
  return parts.length ? parts : null;
}

// The app slots in the order the cartridge lists them (factory first, then ota_0..), as slots.c does.
export function appSlots(parts) {
  return (parts || []).filter((p) => p.kind === "app").sort((a, b) => a.subtype - b.subtype);
}

// The CRC the bootloader keeps beside each otadata entry: esp_rom_crc32_le(UINT32_MAX, &ota_seq, 4).
export function otaSeqCrc(seq) {
  let crc = 0;                                   // ~UINT32_MAX: the ROM routine inverts the seed
  for (let i = 0; i < 4; i++) {
    crc ^= (seq >>> (8 * i)) & 0xff;
    for (let k = 0; k < 8; k++) crc = crc & 1 ? (crc >>> 1) ^ 0xedb88320 : crc >>> 1;
  }
  return ~crc >>> 0;
}

export const OTA_STATES = { 0: "NEW", 1: "PENDING_VERIFY", 2: "VALID", 3: "INVALID", 4: "ABORTED", 0xffffffff: "UNDEFINED" };

// otadata: two 4 KB sectors, each an esp_ota_select_entry_t {ota_seq, seq_label[20], ota_state, crc}. The valid
// entry with the highest sequence names the slot the bootloader boots: (seq - 1) % number of OTA slots. Both
// sectors blank (after a full flash) means the first app slot boots.
export function parseOtadata(b, otaCount) {
  const entries = [0, 0x1000].map((o) => {
    if (o + 32 > b.length) return null;
    const seq = u32(b, o), state = u32(b, o + 24), crc = u32(b, o + 28);
    if (seq === 0xffffffff) return { sector: o / 0x1000, blank: true };
    return { sector: o / 0x1000, seq, state, stateName: OTA_STATES[state] || `0x${state.toString(16)}`, valid: crc === otaSeqCrc(seq) };
  }).filter(Boolean);
  const live = entries.filter((e) => !e.blank && e.valid);
  if (!live.length) return { entries, blank: entries.every((e) => e.blank), bootOta: otaCount ? 0 : null, state: null };
  const top = live.reduce((a, e) => (e.seq > a.seq ? e : a));
  return { entries, blank: false, bootOta: otaCount ? (top.seq - 1) % otaCount : null, state: top.stateName, seq: top.seq };
}

// The image header and esp_app_desc_t from the first APP_HEAD_SIZE bytes of a slot or a .bin. kind: "empty"
// (erased), "foreign" (not an ESP image), "image" (an ESP image; desc may be null when it carries none).
export function parseAppHead(b) {
  if (!b || b.length < 24) return { kind: "foreign", reason: "too short" };
  if (b.subarray(0, 32).every((x) => x === 0xff)) return { kind: "empty" };
  if (b[0] !== 0xe9) return { kind: "foreign", reason: `magic 0x${b[0].toString(16)}` };
  const head = {
    kind: "image",
    segments: b[1],
    flashMode: ["qio", "qout", "dio", "dout"][b[2]] || `mode ${b[2]}`,
    flashSize: ["1MB", "2MB", "4MB", "8MB", "16MB", "32MB", "64MB", "128MB"][b[3] >> 4] || "?",
    flashFreq: { 0x0: "40m", 0x1: "26m", 0x2: "20m", 0xf: "80m" }[b[3] & 0x0f] || "?",
    entry: u32(b, 4),
    chipId: u16(b, 12),
    hashAppended: b[23] === 1,
    desc: null,
  };
  if (b.length >= APP_HEAD_SIZE && u32(b, 0x20) === 0xabcd5432) {
    const d = 0x20;
    head.desc = {
      secureVersion: u32(b, d + 4),
      version: text(b, d + 16, 32),
      projectName: text(b, d + 48, 32),
      time: text(b, d + 80, 16),
      date: text(b, d + 96, 16),
      idfVer: text(b, d + 112, 32),
      elfSha256: hex(b.subarray(d + 144, d + 176)),
    };
  }
  return head;
}

// The name the FM-1 shows for a slot (slots.c describe()): the profile's version string, else the project name,
// printable characters upper-cased, at most 12.
export function slotName(head) {
  if (!head || head.kind === "empty") return "EMPTY";
  if (head.kind !== "image" || !head.desc) return "APP";
  const src = head.desc.version || head.desc.projectName;
  let n = "";
  for (const c of src) if (c > " " && c < "\x7f" && n.length < 12) n += c.toUpperCase();
  return n || "APP";
}

// What may go into an app slot of `slotSize` bytes on this chip; problems block the write, warnings do not.
export function checkSlotImage(bytes, slotSize) {
  const head = parseAppHead(bytes.subarray(0, APP_HEAD_SIZE));
  const problems = [], warnings = [];
  if (head.kind !== "image") problems.push(head.kind === "empty" ? "the file is blank" : `not an ESP32 application image (${head.reason})`);
  else {
    if (head.chipId !== CHIP_ESP32S3) problems.push(`built for chip id ${head.chipId}, not the ESP32-S3`);
    if (!head.desc) problems.push("no application description (esp_app_desc_t): not an ESP-IDF application");
    else if (head.desc.projectName !== CARTRIDGE_PROJECT)
      warnings.push(`project "${head.desc.projectName}" is not a cartridge firmware: it never answers the FM-1, so the bootloader rolls it back after one boot`);
  }
  if (bytes.length > slotSize) problems.push(`${bytes.length.toLocaleString("en")} B does not fit the ${slotSize.toLocaleString("en")} B slot`);
  return { head, problems, warnings };
}

export function sameBytes(a, b) {
  if (!a || !b || a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

// MD5 of a whole buffer, as hex: the stub's SPI_FLASH_MD5 reply is compared with it after every write.
const MD5_S = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
const MD5_K = Uint32Array.from({ length: 64 }, (_, i) => Math.floor(Math.abs(Math.sin(i + 1)) * 2 ** 32));
export function md5hex(bytes) {
  const n = bytes.length, total = Math.ceil((n + 9) / 64) * 64;
  const buf = new Uint8Array(total);
  buf.set(bytes);
  buf[n] = 0x80;
  const dv = new DataView(buf.buffer);
  dv.setUint32(total - 8, (n * 8) >>> 0, true);
  dv.setUint32(total - 4, Math.floor(n / 2 ** 29), true);
  let a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
  for (let off = 0; off < total; off += 64) {
    let a = a0, b = b0, c = c0, d = d0;
    for (let i = 0; i < 64; i++) {
      let f, g;
      if (i < 16) { f = (b & c) | (~b & d); g = i; }
      else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
      else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
      else { f = c ^ (b | ~d); g = (7 * i) & 15; }
      f = (f + a + MD5_K[i] + dv.getUint32(off + 4 * g, true)) >>> 0;
      const s = MD5_S[(i >> 4) * 4 + (i & 3)];
      a = d; d = c; c = b;
      b = (b + ((f << s) | (f >>> (32 - s)))) >>> 0;
    }
    a0 = (a0 + a) >>> 0; b0 = (b0 + b) >>> 0; c0 = (c0 + c) >>> 0; d0 = (d0 + d) >>> 0;
  }
  const out = new Uint8Array(16), ov = new DataView(out.buffer);
  [a0, b0, c0, d0].forEach((v, i) => ov.setUint32(4 * i, v, true));
  return hex(out);
}

// The partition table as the device holds it, up to its MD5 entry: tables are compared over the entries only.
export function tableEntries(b) {
  for (let o = 0; o + 32 <= b.length; o += 32) {
    const magic = u16(b, o);
    if (magic !== 0x50aa) return b.subarray(0, o);
  }
  return b;
}
