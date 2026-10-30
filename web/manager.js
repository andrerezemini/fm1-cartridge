// SPDX-License-Identifier: GPL-3.0-only
// The cartridge manager: reads and writes the ESP32-S3's firmware slots over Web Serial (esptool-js), installs
// the FM-1's Sloop over Web MIDI (Sloop's own updater, sloop-fm1/web/fm1ota.js), keeps the catalogue of synths
// (catalog.json) and checks the pinned upstreams on GitHub. Served by tools/cartridge_web.py, or by any static
// server at the repository root (then the local builds are found by probing and the catalogue is read-only).
import * as img from "./espimage.js";

const $ = (id) => document.getElementById(id);
function el(tag, attrs = {}, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (v == null || v === false) continue;
    if (k === "class") e.className = v;
    else if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
    else e.setAttribute(k, v === true ? "" : v);
  }
  for (const k of kids.flat()) if (k != null && k !== false) e.append(k instanceof Node ? k : String(k));
  return e;
}
const bytes = (n) => `${n.toLocaleString("en")} B`;
const percent = (a, b) => `${((100 * a) / b).toFixed(0)}%`;
const short = (rev) => (rev || "").slice(0, 7);
const hex = (n) => `0x${n.toString(16)}`;
const sha256hex = async (data) => Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", data)), (x) => x.toString(16).padStart(2, "0")).join("");
const store = {                                       // per-viewer conveniences only; never required
  get(k) { try { return JSON.parse(localStorage.getItem(k)); } catch { return null; } },
  set(k, v) { try { localStorage.setItem(k, JSON.stringify(v)); } catch { /* private window */ } },
};
function download(data, name) {
  const a = el("a", { href: URL.createObjectURL(new Blob([data])), download: name });
  document.body.append(a);
  a.click();
  setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
}

// ---------------------------------------------------------------- tabs
const TABS = ["slots", "synths", "updates", "fm1"];
function showTab(name) {
  if (!TABS.includes(name)) name = "slots";
  for (const t of TABS) {
    $(`t-${t}`).setAttribute("aria-selected", String(t === name));
    $(`tab-${t}`).hidden = t !== name;
  }
  if (location.hash !== `#${name}`) history.replaceState(null, "", `#${name}`);
}
for (const t of TABS) $(`t-${t}`).addEventListener("click", () => showTab(t));
showTab(location.hash.slice(1));

// ---------------------------------------------------------------- catalogue and local builds
let catalog = { upstreams: [], synths: [] };
let server = false;                                   // tools/cartridge_web.py answers /api/*
const builds = new Map();                             // profile -> { profile, base, size, files: Map, head }
const upstream = (id) => catalog.upstreams.find((u) => u.id === id);

async function loadCatalog() {
  const r = await fetch("catalog.json", { cache: "no-store" });
  catalog = await r.json();
}

async function loadBuilds() {
  let list = null;
  try {
    const r = await fetch("/api/builds", { cache: "no-store" });
    if (r.ok) { list = (await r.json()).builds; server = true; }
  } catch { /* a static server */ }
  if (!list) {                                        // probe the catalogue's profiles next to this page
    const profiles = [...new Set(catalog.synths.flatMap((s) => s.profiles || []))];
    list = (await Promise.all(profiles.map(async (profile) => {
      try {
        const r = await fetch(`../firmware/build-${profile}/fm1_cartridge.bin`, { method: "HEAD", cache: "no-store" });
        return r.ok ? { profile, size: Number(r.headers.get("content-length")) || 0 } : null;
      } catch { return null; }
    }))).filter(Boolean);
  }
  builds.clear();
  for (const b of list) builds.set(b.profile, { ...b, base: `../firmware/build-${b.profile}/`, files: new Map(), head: null });
  await Promise.all([...builds.values()].map((b) => buildHead(b).catch(() => null)));
}

async function buildFile(b, file = "fm1_cartridge.bin") {
  if (!b.files.has(file)) {
    const r = await fetch(b.base + file, { cache: "no-store" });
    if (!r.ok) throw new Error(`${b.base}${file}: HTTP ${r.status}`);
    b.files.set(file, new Uint8Array(await r.arrayBuffer()));
  }
  return b.files.get(file);
}

async function buildHead(b) {
  if (!b.head) {
    const data = await buildFile(b);
    b.size = data.length;
    b.head = img.parseAppHead(data.subarray(0, img.APP_HEAD_SIZE));
  }
  return b.head;
}
const buildName = (b) => (b.head ? img.slotName(b.head) : b.profile.toUpperCase());
const buildStamp = (b) => (b.head && b.head.desc ? `${b.head.desc.date} ${b.head.desc.time}` : "");

// ---------------------------------------------------------------- ESP32-S3 over Web Serial
const esp = { loader: null, transport: null, mac: "", flashSize: "", flashBytes: 0, boot: null, table: null, parts: null, slots: [], ota: null };
const plan = new Map();                               // slot label -> { source, label, data, check, profile }
let busy = false, backupOk = false;
const esptool = import("esptool-js").catch((e) => { throw new Error(`esptool-js did not load (${e.message}); this page needs the network once`); });
esptool.catch(() => {});

const espLog = (s, nl = true) => { const p = $("esp-log"); p.textContent += s + (nl ? "\n" : ""); p.scrollTop = p.scrollHeight; };
const terminal = { clean() {}, writeLine: (s) => espLog(s), write: (s) => espLog(s, false) };

function setBusy(on) {
  busy = on;
  refreshSlotButtons();
  $("chip-esp").classList.toggle("busy", on);
}
window.addEventListener("beforeunload", (e) => { if (busy) { e.preventDefault(); e.returnValue = ""; } });

if (!("serial" in navigator)) {
  $("no-serial").hidden = false;
  $("esp-connect").disabled = true;
}

$("esp-connect").addEventListener("click", async () => {
  if (busy) return;
  let port;
  try {                                               // first, while the click still counts as a user gesture
    port = await navigator.serial.requestPort($("esp-allports").checked ? {} : { filters: [{ usbVendorId: 0x303a }] });
  } catch (e) { if (e.name !== "NotFoundError") espLog(String(e)); return; }
  setBusy(true);
  try {
    const { ESPLoader, Transport } = await esptool;
    const transport = new Transport(port, false);
    const loader = new ESPLoader({ transport, baudrate: 460800, romBaudrate: 460800, terminal });
    const chip = await loader.main("no_reset");
    if (loader.chip.CHIP_NAME !== "ESP32-S3") {
      await transport.disconnect();
      throw new Error(`${chip}: the cartridge is an ESP32-S3`);
    }
    Object.assign(esp, { loader, transport, chip });
    esp.mac = await loader.chip.readMac(loader);
    esp.flashSize = (await loader.detectFlashSize()) || "";
    esp.flashBytes = esp.flashSize ? loader.flashSizeBytes(esp.flashSize) : 0;
    await readLayout();
    const kept = store.get(`fm1cart.backup.${esp.mac}`);
    backupOk = false;
    $("backup-status").textContent = kept ? `Last backup of this board from this browser: ${kept.name} (sha256 ${kept.sha.slice(0, 16)}…).` : "";
    $("chip-esp").textContent = `Cartridge: ${esp.mac}`;
    $("chip-esp").classList.add("on");
  } catch (e) {
    espLog(`error: ${e.message || e}`);
    $("write-status").textContent = "";
    $("backup-status").innerHTML = "";
    $("backup-status").append(el("span", { class: "bad" }, `Could not talk to the bootloader: ${e.message || e}. Is the board in its USB bootloader (BOOT held at RESET), and no other program using the port?`));
    try { await port.close(); } catch { /* already closed */ }
    resetEsp();
  } finally { setBusy(false); renderAll(); }
});

$("esp-disconnect").addEventListener("click", async () => {
  if (busy) return;
  try { await esp.transport.disconnect(); } catch { /* gone already */ }
  resetEsp();
  renderAll();
});
if ("serial" in navigator) navigator.serial.addEventListener("disconnect", () => { if (esp.loader && !busy) { resetEsp(); renderAll(); } });

function resetEsp() {
  Object.assign(esp, { loader: null, transport: null, mac: "", flashSize: "", flashBytes: 0, boot: null, table: null, parts: null, slots: [], ota: null });
  plan.clear();
  backupOk = false;
  $("chip-esp").textContent = "Cartridge: not connected";
  $("chip-esp").classList.remove("on");
}

async function readLayout() {
  const L = esp.loader;
  esp.boot = img.parseAppHead(await L.readFlash(0, 32));
  esp.table = await L.readFlash(img.PARTITION_TABLE_OFFSET, img.PARTITION_TABLE_SIZE);
  esp.parts = img.parsePartitionTable(esp.table);
  esp.slots = [];
  esp.ota = null;
  if (!esp.parts) return;
  for (const part of img.appSlots(esp.parts)) {
    const head = img.parseAppHead(await L.readFlash(part.offset, img.APP_HEAD_SIZE));
    esp.slots.push({ part, head, name: img.slotName(head) });
  }
  const od = esp.parts.find((p) => p.kind === "ota");
  const otaSlots = esp.slots.filter((s) => s.part.slot.startsWith("ota_"));
  if (od) {
    esp.ota = img.parseOtadata(await L.readFlash(od.offset, Math.min(od.size, 0x2000)), otaSlots.length);
    const failed = esp.ota.entries.filter((e) => e.valid && (e.stateName === "INVALID" || e.stateName === "ABORTED"));
    for (const e of failed) { const s = otaSlots[(e.seq - 1) % otaSlots.length]; if (s) s.failed = true; }
    const boot = otaSlots[esp.ota.bootOta ?? 0];
    if (boot) { boot.boots = true; boot.state = esp.ota.state; }
  }
}

// ---------------------------------------------------------------- backup
$("backup-have").addEventListener("change", () => { backupOk = $("backup-have").checked; refreshSlotButtons(); });
$("backup-go").addEventListener("click", async () => {
  if (busy || !esp.loader || !esp.flashBytes) return;
  setBusy(true);
  const bar = $("backup-bar");
  bar.hidden = false; bar.value = 0;
  $("backup-status").textContent = `Reading ${bytes(esp.flashBytes)}…`;
  try {
    const t0 = performance.now();
    const data = await esp.loader.readFlash(0, esp.flashBytes, (_pkt, done, all) => { bar.value = done / all; });
    const sha = await sha256hex(data);
    const stamp = new Date().toISOString().replace(/[:T]/g, "-").slice(0, 19);
    const name = `esp32-backup-${esp.mac.replaceAll(":", "")}-${stamp}.bin`;
    download(data, name);
    store.set(`fm1cart.backup.${esp.mac}`, { name, sha, at: Date.now() });
    backupOk = true;
    $("backup-status").innerHTML = "";
    $("backup-status").append(el("span", { class: "ok" }, `Saved ${name}`), ` — ${bytes(data.length)} in ${((performance.now() - t0) / 1000).toFixed(0)} s, sha256 `, el("span", { class: "mono" }, sha));
  } catch (e) {
    $("backup-status").textContent = `Backup failed: ${e.message || e}`;
  } finally { bar.hidden = true; setBusy(false); }
});

// ---------------------------------------------------------------- the slot plan
function slotLabel(s) { return s.part.slot === "factory" ? "factory" : `slot ${Number(s.part.slot.slice(4)) + 1} · ${s.part.label}`; }

function describeHead(head) {
  if (!head || head.kind === "empty") return [el("span", { class: "empty" }, "empty")];
  if (head.kind !== "image") return [el("span", { class: "bad" }, "not an application image")];
  if (!head.desc) return ["application without a description"];
  const d = head.desc;
  return [el("b", {}, img.slotName(head)), el("div", { class: "small muted" }, `${d.projectName} · ${d.date} ${d.time} · ${d.idfVer}`), el("div", { class: "small muted mono" }, `elf ${d.elfSha256.slice(0, 12)}`)];
}

async function choose(s, value, file) {
  plan.delete(s.part.slot);
  try {
    if (value === "file" && file) {
      const data = new Uint8Array(await file.arrayBuffer());
      plan.set(s.part.slot, { source: "file", label: file.name, data });
    } else if (value && value.startsWith("build:")) {
      const b = builds.get(value.slice(6));
      const data = await buildFile(b);
      plan.set(s.part.slot, { source: "build", label: `build-${b.profile}`, data, profile: b.profile });
    }
    await checkPlan();
  } catch (e) {
    plan.delete(s.part.slot);
    $("write-status").textContent = `${slotLabel(s)}: ${e.message || e}`;
  }
  renderSlots();
}

// Each planned image is checked alone (checkSlotImage), against the board (its flash settings: a 4 MB / quad PSRAM
// build on an 8 MB / octal board never starts) and against the rest of the plan (one partition table).
let planFull = null;                                  // { source build, reasons } when the bootloader, table and otadata go too
async function checkPlan() {
  planFull = null;
  const needFull = [], otherTable = new Set();
  if (plan.has("ota_0")) needFull.push("slot 1 is written");
  let table = null, tableFrom = null;
  for (const p of plan.values()) {
    if (p.source !== "build") continue;
    const b = builds.get(p.profile);
    const t = await buildFile(b, "partition_table/partition-table.bin");
    if (!table) { table = t; tableFrom = b; }
    else if (!img.sameBytes(img.tableEntries(t), img.tableEntries(table))) otherTable.add(p);
  }
  const deviceTableDiffers = table && (!esp.parts || !img.sameBytes(img.tableEntries(table), img.tableEntries(esp.table)));
  if (deviceTableDiffers) needFull.push("the board's partition table differs from the builds'");
  const target = needFull.length && tableFrom ? img.parseAppHead((await buildFile(tableFrom, "bootloader/bootloader.bin")).subarray(0, 32)) : esp.boot;
  const newParts = deviceTableDiffers ? img.appSlots(img.parsePartitionTable(table)) : null;
  for (const [slot, p] of plan) {
    const s = esp.slots.find((x) => x.part.slot === slot);
    p.part = newParts ? newParts.find((x) => x.slot === slot) : s.part;
    const c = img.checkSlotImage(p.data, p.part ? p.part.size : s.part.size);
    if (!p.part) c.problems.push(`build-${tableFrom.profile}'s partition table has no ${slot}`);
    if (otherTable.has(p)) c.problems.push(`another partition table than build-${tableFrom.profile}`);
    if (c.head.kind === "image") {
      if (target && target.kind === "image" && target.flashSize !== c.head.flashSize)
        c.problems.push(`built for ${c.head.flashSize} of flash, the board's bootloader for ${target.flashSize} (another board's profile?)`);
      if (esp.flashBytes && parseInt(c.head.flashSize, 10) * 1048576 > esp.flashBytes)
        c.problems.push(`built for ${c.head.flashSize}, the board has ${esp.flashSize}`);
      if (c.head.desc && s.head.desc && c.head.desc.elfSha256 === s.head.desc.elfSha256) c.warnings.push("the same build the slot holds now");
    }
    p.check = { problems: c.problems, warnings: c.warnings, head: c.head };
  }
  if (needFull.length) {
    if (!tableFrom) {
      for (const p of plan.values()) p.check.problems.push("a full install needs one of this repository's builds for the bootloader and the table");
    } else if (deviceTableDiffers && !plan.has("ota_0")) {
      for (const p of plan.values()) p.check.problems.push("the board's partition table differs from the builds': write slot 1 too (a full install)");
    } else planFull = { from: tableFrom, reasons: needFull };
  }
}

function renderSlots() {
  const tbody = $("slot-table").tBodies[0];
  tbody.replaceChildren();
  if (!esp.loader) {
    tbody.append(el("tr", {}, el("td", { colspan: 4, class: "empty" }, "Connect the cartridge to read its slots.")));
  } else if (!esp.parts) {
    tbody.append(el("tr", {}, el("td", { colspan: 4 }, el("span", { class: "warn" }, "No partition table at 0x8000: write a release image (below) or a full install from a build."))));
  }
  const boardBuilds = [...builds.values()].sort((a, b) => a.profile.localeCompare(b.profile));
  for (const s of esp.slots) {
    const p = plan.get(s.part.slot);
    const select = el("select", { "aria-label": `New firmware for ${slotLabel(s)}`, disabled: busy });
    select.append(el("option", { value: "" }, "Keep"));
    for (const b of boardBuilds) {
      const opt = el("option", { value: `build:${b.profile}` }, `${buildName(b)} — build-${b.profile}${buildStamp(b) ? ` (${buildStamp(b)})` : ""}`);
      if (p && p.profile === b.profile) opt.selected = true;
      select.append(opt);
    }
    const fileOpt = el("option", { value: "file" }, p && p.source === "file" ? `File: ${p.label}` : "From a file…");
    if (p && p.source === "file") fileOpt.selected = true;
    select.append(fileOpt);
    const picker = el("input", { type: "file", accept: ".bin" });
    picker.addEventListener("change", () => { if (picker.files[0]) choose(s, "file", picker.files[0]); else renderSlots(); });
    picker.addEventListener("cancel", () => renderSlots());
    select.addEventListener("change", () => { if (select.value === "file") picker.click(); else choose(s, select.value); });

    const now = el("td", {}, ...describeHead(s.head));
    if (s.boots) now.append(el("span", { class: "badge ok" }, `boots${s.state ? ` · ${s.state.toLowerCase().replace("_", " ")}` : ""}`));
    if (s.failed) now.append(el("span", { class: "badge bad" }, "rolled back"));
    const check = el("td", { class: "hide-sm small" });
    if (p && p.check) {
      const h = p.check.head;
      if (h && h.kind === "image") {
        const size = (p.part || s.part).size;
        check.append(el("div", {}, `${img.slotName(h)} · ${bytes(p.data.length)} (${percent(p.data.length, size)} of the slot)`));
        check.append(el("div", { class: "meter" }, el("i", { style: `width:${Math.min(100, (100 * p.data.length) / size)}%` })));
      }
      for (const m of p.check.problems) check.append(el("div", { class: "bad" }, m));
      for (const m of p.check.warnings) check.append(el("div", { class: "warn" }, m));
      if (p.done) check.append(el("div", { class: "ok" }, "written, MD5 verified"));
    }
    tbody.append(el("tr", {}, el("td", {}, el("b", {}, slotLabel(s)), el("div", { class: "small muted mono" }, `${hex(s.part.offset)} · ${bytes(s.part.size)}`)), now, el("td", {}, select, picker), check));
  }
  const full = $("plan-full");
  full.replaceChildren();
  if (planFull) full.append(el("span", { class: "warn" }, `Full install (${planFull.reasons.join("; ")}): the bootloader, the partition table and a clean otadata from build-${planFull.from.profile} go too, so the board starts from slot 1 with every slot unconfirmed. The kept sounds (nvs) stay.`));
  refreshSlotButtons();
}

function refreshSlotButtons() {
  const connected = !!esp.loader;
  $("esp-connect").disabled = busy || connected || !("serial" in navigator);
  $("esp-disconnect").disabled = busy || !connected;
  $("backup-go").disabled = busy || !connected || !esp.flashBytes;
  $("backup-have").disabled = busy || !connected;
  if (!connected) $("backup-have").checked = false;
  const problems = [...plan.values()].some((p) => !p.check || p.check.problems.length);
  let why = "";
  if (!connected) why = "Connect the cartridge first.";
  else if (!plan.size) why = "Choose a firmware for at least one slot.";
  else if (problems) why = "Fix the problems in the Check column.";
  else if (!backupOk) why = "Back up this board first (step 2).";
  $("write-go").disabled = busy || !!why;
  $("write-why").textContent = busy ? "Working…" : why;
  $("merged-go").disabled = busy || !connected || !merged || !backupOk || merged.problems.length > 0;
  for (const s of document.querySelectorAll("#slot-table select")) s.disabled = busy;
}

$("write-go").addEventListener("click", async () => {
  if (busy || !plan.size) return;
  const files = [];
  if (planFull) {
    const b = planFull.from;
    const table = await buildFile(b, "partition_table/partition-table.bin");
    const od = img.parsePartitionTable(table).find((p) => p.kind === "ota");
    files.push({ address: 0x0, data: await buildFile(b, "bootloader/bootloader.bin"), what: "bootloader" });
    files.push({ address: img.PARTITION_TABLE_OFFSET, data: table, what: "partition table" });
    files.push({ address: od.offset, data: new Uint8Array(od.size).fill(0xff), what: "otadata (clean)" });
  }
  for (const s of esp.slots) {
    const p = plan.get(s.part.slot);
    if (p) files.push({ address: p.part.offset, data: p.data, what: `${slotLabel(s)} ← ${img.slotName(p.check.head)} (${p.label})`, slot: s.part.slot });
  }
  const summary = files.map((f) => `${hex(f.address).padStart(8)}  ${f.what}  ${bytes(f.data.length)}`).join("\n");
  if (!confirm(`Write to the cartridge ${esp.mac}?\n\n${summary}\n\nThe board stays in its bootloader afterwards; press RESET to start it.`)) return;
  setBusy(true);
  const bar = $("write-bar");
  bar.hidden = false; bar.value = 0;
  const weights = files.map((f) => f.data.length), all = weights.reduce((a, b) => a + b, 0);
  const before = (i) => weights.slice(0, i).reduce((a, b) => a + b, 0);
  $("write-status").textContent = "Writing…";
  try {
    await esp.loader.writeFlash({
      fileArray: files.map((f) => ({ data: f.data, address: f.address })),
      flashMode: "keep", flashFreq: "keep", flashSize: "keep", eraseAll: false, compress: true,
      reportProgress: (i, written, total) => { bar.value = (before(i) + (weights[i] * written) / Math.max(1, total)) / all; },
      calculateMD5Hash: img.md5hex,
    });
    for (const f of files) if (f.slot) plan.get(f.slot).done = true;
    $("write-status").innerHTML = "";
    $("write-status").append(el("span", { class: "ok" }, "Written; esptool compared the MD5 of every region with the files."), " Press RESET on the board (or power it off and on), then plug it into the FM-1: GLO → CARTRIDGE shows the firmware, and FIRMWARE (knob 3) picks another slot.");
    const done = new Set(files.filter((f) => f.slot).map((f) => f.slot));
    await readLayout();
    for (const slot of done) plan.delete(slot);
    planFull = null;
  } catch (e) {
    $("write-status").innerHTML = "";
    $("write-status").append(el("span", { class: "bad" }, `Write failed: ${e.message || e}.`), " Nothing is lost while the board stays in its bootloader: connect again and repeat, or put the backup back.");
  } finally { bar.hidden = true; setBusy(false); renderAll(); }
});

// ---------------------------------------------------------------- a whole-flash image (release or backup) at 0x0
let merged = null;
$("merged-file").addEventListener("change", async () => {
  const f = $("merged-file").files[0];
  merged = null;
  const out = $("merged-status");
  out.replaceChildren();
  if (!f) return refreshSlotButtons();
  const data = new Uint8Array(await f.arrayBuffer());
  const problems = [];
  const boot = img.parseAppHead(data.subarray(0, 32));
  if (boot.kind !== "image" || boot.chipId !== img.CHIP_ESP32S3) problems.push("no ESP32-S3 bootloader at 0x0");
  const parts = data.length > img.PARTITION_TABLE_OFFSET ? img.parsePartitionTable(data.subarray(img.PARTITION_TABLE_OFFSET, img.PARTITION_TABLE_OFFSET + img.PARTITION_TABLE_SIZE)) : null;
  if (!parts) problems.push("no partition table at 0x8000");
  if (esp.flashBytes && data.length > esp.flashBytes) problems.push(`${bytes(data.length)} is larger than the board's ${esp.flashSize}`);
  if (esp.boot && boot.kind === "image" && esp.boot.kind === "image" && boot.flashSize !== esp.boot.flashSize) problems.push(`made for ${boot.flashSize} of flash, the board's bootloader says ${esp.boot.flashSize}`);
  const holds = (parts ? img.appSlots(parts) : []).map((p) => `${p.label}: ${p.offset + img.APP_HEAD_SIZE <= data.length ? img.slotName(img.parseAppHead(data.subarray(p.offset, p.offset + img.APP_HEAD_SIZE))) : "EMPTY"}`);
  merged = { name: f.name, data, problems };
  out.append(el("div", {}, `${f.name} · ${bytes(data.length)} · sha256 `, el("span", { class: "mono" }, (await sha256hex(data)).slice(0, 16) + "…")));
  if (holds.length) out.append(el("div", {}, holds.join(" · ")));
  for (const m of problems) out.append(el("div", { class: "bad" }, m));
  refreshSlotButtons();
});
$("merged-go").addEventListener("click", async () => {
  if (busy || !merged || merged.problems.length) return;
  if (!confirm(`Write ${merged.name} (${bytes(merged.data.length)}) at 0x0 of the cartridge ${esp.mac}?\n\nEverything it covers is replaced, the kept sounds (nvs) included.`)) return;
  setBusy(true);
  const out = $("merged-status");
  try {
    await esp.loader.writeFlash({ fileArray: [{ data: merged.data, address: 0 }], flashMode: "keep", flashFreq: "keep", flashSize: "keep", eraseAll: false, compress: true, calculateMD5Hash: img.md5hex,
      reportProgress: (_i, w, t) => { out.dataset.p = `${Math.round((100 * w) / Math.max(1, t))}%`; $("write-why").textContent = `Writing the image… ${out.dataset.p}`; } });
    out.append(el("div", { class: "ok" }, "Written and MD5-verified. Press RESET on the board."));
    await readLayout();
  } catch (e) { out.append(el("div", { class: "bad" }, `Write failed: ${e.message || e}`)); }
  finally { setBusy(false); renderAll(); }
});

function renderEspInfo() {
  const info = $("esp-info");
  info.replaceChildren();
  info.hidden = !esp.loader;
  if (!esp.loader) return;
  const otaText = !esp.ota ? "no otadata partition" : esp.ota.blank ? "blank: slot 1 boots" : `sequence ${esp.ota.seq}, state ${esp.ota.state}`;
  const rows = [
    ["Chip", esp.chip], ["MAC", esp.mac], ["Flash", `${esp.flashSize || "?"}${esp.boot && esp.boot.kind === "image" ? ` (bootloader: ${esp.boot.flashSize}, ${esp.boot.flashMode}, ${esp.boot.flashFreq})` : " (no bootloader at 0x0)"}`],
    ["Partitions", esp.parts ? esp.parts.map((p) => p.label).join(", ") : "none"], ["otadata", otaText],
  ];
  for (const [k, v] of rows) info.append(el("dt", {}, k), el("dd", {}, v));
}

// ---------------------------------------------------------------- synths
function renderSynths() {
  const grid = $("synth-grid");
  grid.replaceChildren();
  for (const s of catalog.synths.filter((x) => x.status === "ported")) {
    const u = upstream(s.upstream) || {};
    const card = el("div", { class: "card synth" },
      el("h3", {}, s.name, el("span", { class: "small muted" }, u.version ? `v${u.version}` : "")),
      el("p", { class: "small", style: "margin:0 0 8px" }, s.summary),
      el("dl", { class: "kv" },
        el("dt", {}, "Upstream"), el("dd", {}, u.repo ? el("a", { href: `https://github.com/${u.repo}/tree/${u.revision}`, target: "_blank", rel: "noopener" }, `${u.repo} @ ${short(u.revision)}`) : "—"),
        el("dt", {}, "Sound"), el("dd", {}, `${s.engines} engines · ${s.presets} presets`),
        el("dt", {}, "Licence"), el("dd", {}, s.licence),
        el("dt", {}, "On the FM-1"), el("dd", {}, s.hardware)));
    const list = el("div", { class: "small", style: "margin-top:10px" });
    const mine = (s.profiles || []).map((p) => builds.get(p)).filter(Boolean);
    if (!mine.length) list.append(el("div", { class: "muted" }, `No local build: ./firmware/build.sh ${(s.profiles || [])[0] || s.id}`));
    for (const b of mine) {
      list.append(el("div", {}, el("span", { class: "mono" }, `build-${b.profile}`), ` · ${bytes(b.size)} (${percent(b.size, 0x140000)} of a slot)${buildStamp(b) ? ` · ${buildStamp(b)}` : ""} `,
        el("button", { class: "link", onclick: () => putInSlot(b) }, "put in a slot")),
        el("div", { class: "meter" }, el("i", { style: `width:${Math.min(100, (100 * b.size) / 0x140000)}%` })));
    }
    card.append(list);
    grid.append(card);
  }
  const tbody = $("cand-table").tBodies[0];
  tbody.replaceChildren();
  const cands = catalog.synths.filter((x) => x.status === "candidate");
  if (!cands.length) tbody.append(el("tr", {}, el("td", { colspan: 4, class: "empty" }, "None listed yet.")));
  for (const c of cands) {
    tbody.append(el("tr", {},
      el("td", {}, el("a", { href: `https://github.com/${c.repo}`, target: "_blank", rel: "noopener" }, c.name), el("div", { class: "small muted" }, c.repo)),
      el("td", { class: licenceClass(c.licence) }, c.licence || "?"),
      el("td", { class: "hide-sm small" }, c.summary || ""),
      el("td", { class: "small" }, c.effort || "?", c.notes ? el("div", { class: "muted" }, c.notes) : null)));
  }
}

function putInSlot(b) {
  showTab("slots");
  if (!esp.loader) { $("write-why").textContent = `Connect the cartridge, then choose build-${b.profile} for a slot.`; return; }
  const name = buildName(b);
  const s = esp.slots.find((x) => x.name === name) || esp.slots.find((x) => x.name === "EMPTY");
  if (!s) { $("write-why").textContent = `Choose a slot for build-${b.profile} in the table.`; return; }
  choose(s, `build:${b.profile}`);
}

// GPL-3.0-only can combine with these; anything else (or no licence at all) cannot be redistributed in a slot.
const GPL3_COMPATIBLE = new Set(["GPL-3.0", "GPL-3.0-only", "GPL-3.0-or-later", "GPL-2.0-or-later", "LGPL-2.1", "LGPL-2.1-or-later", "LGPL-3.0", "LGPL-3.0-only", "LGPL-3.0-or-later", "MIT", "BSD-2-Clause", "BSD-3-Clause", "Apache-2.0", "MPL-2.0", "ISC", "Zlib", "Unlicense", "CC0-1.0", "0BSD", "BSL-1.0"]);
const licenceClass = (l) => (!l || l === "?" ? "warn" : GPL3_COMPATIBLE.has(l.replace(/ .*/, "")) ? "" : "bad");

$("add-check").addEventListener("click", async () => {
  const repo = $("add-repo").value.trim().replace(/^https?:\/\/github\.com\//, "").replace(/\.git$/, "").replace(/\/$/, "");
  const out = $("add-result");
  out.replaceChildren();
  if (!/^[\w.-]+\/[\w.-]+$/.test(repo)) { out.append(el("span", { class: "bad" }, "Write owner/name, e.g. charlesvestal/fm1-x0x.")); return; }
  out.append(el("span", { class: "muted" }, "Reading GitHub…"));
  try {
    const info = await gh(`repos/${repo}`);
    const [release, hostsim, modules] = await Promise.all([
      gh(`repos/${repo}/releases/latest`).catch(() => null),
      gh(`repos/${repo}/contents/tests/hostsim.c`).then(() => true).catch(() => false),
      gh(`repos/${repo}/contents/.gitmodules`).then((r) => atob(r.content.replace(/\n/g, ""))).catch(() => ""),
    ]);
    const licence = info.license && info.license.spdx_id && info.license.spdx_id !== "NOASSERTION" ? info.license.spdx_id : "";
    const entry = {
      id: info.name.toLowerCase().replace(/^fm1-/, "").replace(/[^a-z0-9]+/g, "-"),
      name: info.name.replace(/^fm1-/i, ""),
      status: "candidate",
      repo: info.full_name,
      licence: licence || "?",
      summary: info.description || "",
      effort: hostsim ? "medium" : "high",
      notes: [hostsim ? "host build of the DSP (tests/hostsim.c)" : "no tests/hostsim.c", /felucca-bsp/.test(modules) ? "on felucca-bsp" : "", info.parent ? `fork of ${info.parent.full_name}` : "", release ? `release ${release.tag_name}` : "no release"].filter(Boolean).join("; "),
    };
    const known = catalog.synths.find((s) => (s.repo || (upstream(s.upstream) || {}).repo) === entry.repo);
    out.replaceChildren(
      el("dl", { class: "kv" },
        el("dt", {}, "Repository"), el("dd", {}, el("a", { href: info.html_url, target: "_blank", rel: "noopener" }, info.full_name), ` · ★${info.stargazers_count} · last push ${info.pushed_at.slice(0, 10)}`),
        el("dt", {}, "Description"), el("dd", {}, info.description || "—"),
        el("dt", {}, "Licence"), el("dd", { class: licenceClass(licence) }, licence || "none declared: it cannot be redistributed until its author adds one", licence && !GPL3_COMPATIBLE.has(licence) ? " — not combinable with this GPL-3.0-only project" : ""),
        el("dt", {}, "Port route"), el("dd", {}, entry.notes)),
      el("div", { class: "row", style: "margin-top:10px" },
        known ? el("span", { class: "muted small" }, `Already in the catalogue as ${known.name} (${known.status}).`)
          : el("button", { class: "btn", onclick: () => saveCandidate(entry, out) }, server ? "Add to catalog.json" : "Download catalog.json with it")));
  } catch (e) { out.replaceChildren(el("span", { class: "bad" }, String(e.message || e))); }
});

async function saveCandidate(entry, out) {
  if (server) {
    const r = await fetch("/api/catalog/candidates", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(entry) });
    if (!r.ok) { out.append(el("div", { class: "bad" }, `Not saved: ${await r.text()}`)); return; }
    await loadCatalog();
    renderSynths();
    out.append(el("div", { class: "ok" }, `Added to web/catalog.json as a candidate; review and commit it.`));
  } else {
    const next = { ...catalog, synths: [...catalog.synths, entry] };
    download(JSON.stringify(next, null, 2) + "\n", "catalog.json");
    out.append(el("div", { class: "ok" }, "Replace web/catalog.json with the downloaded file."));
  }
}

// ---------------------------------------------------------------- upstreams on GitHub
const GH_CACHE_MS = 15 * 60 * 1000;
async function gh(path) {
  const key = `fm1cart.gh.${path}`;
  try { const c = JSON.parse(sessionStorage.getItem(key)); if (c && Date.now() - c.at < GH_CACHE_MS) { if (c.status) throw Object.assign(new Error(c.message), { status: c.status }); return c.data; } }
  catch (e) { if (e.status) throw e; }
  const r = await fetch(`https://api.github.com/${path}`, { headers: { Accept: "application/vnd.github+json" } });
  if (r.status === 403 || r.status === 429) {
    const reset = Number(r.headers.get("x-ratelimit-reset"));
    throw new Error(`GitHub's limit for anonymous requests is reached${reset ? `; it resets at ${new Date(reset * 1000).toLocaleTimeString()}` : ""}`);
  }
  if (!r.ok) {
    const err = Object.assign(new Error(`GitHub ${r.status} for ${path}`), { status: r.status });
    if (r.status === 404) try { sessionStorage.setItem(key, JSON.stringify({ at: Date.now(), status: 404, message: err.message })); } catch { /* full */ }
    throw err;
  }
  const data = await r.json();
  try { sessionStorage.setItem(key, JSON.stringify({ at: Date.now(), data })); } catch { /* full */ }
  return data;
}

const vparts = (v) => (v.match(/\d+/g) || []).map(Number);
const vcmp = (a, b) => { const x = vparts(a), y = vparts(b); for (let i = 0; i < Math.max(x.length, y.length); i++) { const d = (x[i] || 0) - (y[i] || 0); if (d) return d; } return 0; };

async function checkUpstream(u) {
  if (u.registry) {
    const r = await fetch(`https://components.espressif.com/api/components/${u.registry}`);
    if (!r.ok) throw new Error(`registry ${r.status}`);
    const versions = ((await r.json()).versions || []).map((v) => v.version).filter((v) => !/-/.test(v)).sort(vcmp);
    const latest = versions[versions.length - 1];
    return { latest, url: `https://components.espressif.com/components/${u.registry}`, behind: latest && vcmp(latest, u.version) > 0 ? "newer version" : 0 };
  }
  if (u.track) {
    const rels = await gh(`repos/${u.repo}/releases?per_page=60`);
    const re = new RegExp(u.track);
    const inTrack = rels.filter((r) => re.test(r.tag_name)).map((r) => r.tag_name).sort(vcmp);
    const stable = rels.filter((r) => !r.prerelease && /^v\d+\.\d+(\.\d+)?$/.test(r.tag_name)).map((r) => r.tag_name).sort(vcmp);
    const latest = inTrack[inTrack.length - 1] || u.version;
    const newest = stable[stable.length - 1];
    return { latest, url: `https://github.com/${u.repo}/releases/tag/${latest}`, behind: vcmp(latest, u.version) > 0 ? "newer in its line" : 0,
      note: newest && vcmp(newest, latest) > 0 ? `${newest} is the newest overall (another release line: a migration)` : "" };
  }
  const release = await gh(`repos/${u.repo}/releases/latest`).catch(() => null);
  let ref = release && release.tag_name, latestLabel = ref, url = release && release.html_url, body = release && release.body;
  if (!ref) {
    const head = await gh(`repos/${u.repo}/commits?per_page=1`);
    ref = head[0].sha; latestLabel = `${short(ref)} (no release)`; url = head[0].html_url;
  }
  const cmp = await gh(`repos/${u.repo}/compare/${u.revision}...${ref}`);
  const res = { latest: latestLabel, url, behind: cmp.ahead_by, diverged: cmp.status === "diverged" || cmp.behind_by > 0, commits: (cmp.commits || []).slice(-12).reverse(), body, compare: cmp.html_url };
  const pre = await gh(`repos/${u.repo}/releases?per_page=5`).catch(() => []);
  const newerPre = pre.find((r) => r.prerelease && (!release || new Date(r.published_at) > new Date(release.published_at)));
  if (newerPre) res.note = `pre-release ${newerPre.tag_name} (${newerPre.published_at.slice(0, 10)})`;
  return res;
}

$("up-check").addEventListener("click", checkUpstreams);
async function checkUpstreams() {
  const tbody = $("up-table").tBodies[0];
  $("up-check").disabled = true;
  $("up-status").textContent = "Asking GitHub…";
  tbody.replaceChildren();
  let n = 0;
  for (const u of catalog.upstreams) {
    const pinned = el("td", {}, u.version ? `${u.version}` : "", u.revision ? el("div", { class: "small muted mono" }, short(u.revision)) : null);
    const latest = el("td", {}, "…"), status = el("td", { class: "small" }, "…");
    const name = el("td", {}, el("b", {}, u.name), el("div", { class: "small muted" }, u.role || ""));
    tbody.append(el("tr", {}, name, pinned, latest, status));
    try {
      const r = await checkUpstream(u);
      latest.replaceChildren(r.url ? el("a", { href: r.url, target: "_blank", rel: "noopener" }, r.latest || "?") : r.latest || "?");
      status.replaceChildren();
      if (!r.behind) status.append(el("span", { class: "ok" }, "up to date"));
      else if (u.follows) {                           // a submodule: its parent's release pins it, not its own main
        status.append(el("span", { class: "muted" }, `${r.behind} commit${r.behind > 1 ? "s" : ""} on its main since`));
        if (r.compare) status.append(" · ", el("a", { href: r.compare, target: "_blank", rel: "noopener" }, "compare"));
      } else {
        n++;
        status.append(el("span", { class: "warn" }, typeof r.behind === "number" ? `${r.behind} commit${r.behind > 1 ? "s" : ""} newer` : r.behind));
        if (r.compare) status.append(" · ", el("a", { href: r.compare, target: "_blank", rel: "noopener" }, "compare"));
      }
      if (r.diverged) status.append(el("div", { class: "warn" }, "the pinned revision is not on the release's history"));
      if (r.note) status.append(el("div", { class: "muted" }, r.note));
      if (r.commits && r.commits.length) {
        status.append(el("details", {}, el("summary", {}, "changes"),
          el("ul", { class: "small", style: "padding-left:18px;margin:6px 0" }, r.commits.map((c) => el("li", {}, c.commit.message.split("\n")[0].slice(0, 220))))));
      }
      if (u.follows) status.append(el("div", { class: "muted" }, `pinned by ${upstream(u.follows).name} ${upstream(u.follows).version}'s release`));
    } catch (e) {
      latest.textContent = "—";
      status.replaceChildren(el("span", { class: "bad" }, String(e.message || e)));
      if (u.registry) status.append(" · ", el("a", { href: `https://components.espressif.com/components/${u.registry}`, target: "_blank", rel: "noopener" }, "registry"));
    }
  }
  $("up-status").textContent = `${n} with newer versions · checked ${new Date().toLocaleTimeString()}`;
  $("up-check").disabled = false;
}

function renderSlotUpdates() {
  const out = $("slot-updates");
  out.replaceChildren();
  if (!esp.loader) { out.append(el("span", { class: "empty" }, "Connect the cartridge on the Slots tab.")); return; }
  const list = el("ul", { style: "padding-left:18px;margin:0" });
  for (const s of esp.slots) {
    const d = s.head.desc;
    const li = el("li", {}, el("b", {}, slotLabel(s)), ` — ${s.name}: `);
    if (!d) { li.append(el("span", { class: "muted" }, "nothing to compare")); list.append(li); continue; }
    const same = [...builds.values()].filter((b) => b.head && b.head.desc && b.head.desc.version === d.version && esp.boot && b.head.flashSize === esp.boot.flashSize);
    if (!same.length) li.append(el("span", { class: "muted" }, `no local build of ${s.name} for this board`));
    for (const b of same) {
      if (b.head.desc.elfSha256 === d.elfSha256) li.append(el("span", { class: "ok" }, `same as build-${b.profile}`));
      else li.append(el("span", { class: "warn" }, `build-${b.profile} differs (${buildStamp(b)}; the slot has ${d.date} ${d.time}) `), el("button", { class: "link", onclick: () => { showTab("slots"); choose(s, `build:${b.profile}`); } }, "plan the update"));
    }
    list.append(li);
  }
  out.append(list);
}

// ---------------------------------------------------------------- FM-1: Sloop's updater over Web MIDI
let pkg = null, fm1Busy = false;
const fm1Log = (s) => { const p = $("fm1-log"); p.textContent += s + "\n"; p.scrollTop = p.scrollHeight; };
const sloop = Promise.all([import("../sloop-fm1/web/fm1ota.js"), import("../sloop-fm1/web/fm1pkg.js")]).then(([ota, pk]) => ({ ...ota, ...pk }));
sloop.catch((e) => fm1Log(`Sloop's updater did not load: ${e.message}`));
if (!navigator.requestMIDIAccess) { $("no-midi").hidden = false; $("fm1-find").disabled = true; }

async function usePackage(data, source) {
  const { productOf, logicalImage } = await sloop;
  const info = $("pkg-info");
  info.replaceChildren();
  info.hidden = false;
  pkg = null;
  try {
    const product = productOf(data);
    if (!/^FM-1_9\d\d$/.test(product)) throw new Error(`the package is ${product}, not a SLOOP / Felucca package for the FM-1`);
    pkg = { product, image: logicalImage(data), size: data.length, source };
    for (const [k, v] of [["Source", source], ["Identity", product], ["Size", bytes(data.length)], ["SHA-256", await sha256hex(data)]]) info.append(el("dt", {}, k), el("dd", { class: k === "SHA-256" ? "mono" : "" }, v));
  } catch (e) { info.append(el("dt", {}, "Package"), el("dd", { class: "bad" }, e.message)); }
  refreshFm1();
}

async function probeSloopBuild() {
  let ok = false;
  try {
    const r = server ? await fetch("/api/sloop", { cache: "no-store" }) : await fetch("../sloop-fm1/build/felucca.fwsc", { method: "HEAD", cache: "no-store" });
    ok = r.ok && (!server || (await r.json()).exists);
  } catch { /* none */ }
  $("pkg-local").disabled = !ok;
  $("pkg-local").title = ok ? "sloop-fm1/build/felucca.fwsc" : "No sloop-fm1/build/felucca.fwsc: cd sloop-fm1 && sh build-cartridge.sh";
}
$("pkg-local").addEventListener("click", async () => {
  const r = await fetch("../sloop-fm1/build/felucca.fwsc", { cache: "no-store" });
  if (r.ok) usePackage(new Uint8Array(await r.arrayBuffer()), "sloop-fm1/build/felucca.fwsc");
});
$("pkg-file").addEventListener("change", async () => { const f = $("pkg-file").files[0]; if (f) usePackage(new Uint8Array(await f.arrayBuffer()), f.name); });
$("fm1-backed").addEventListener("change", refreshFm1);
function refreshFm1() { $("fm1-go").disabled = fm1Busy || !pkg || !$("fm1-backed").checked || !navigator.requestMIDIAccess; }
window.addEventListener("beforeunload", (e) => { if (fm1Busy) { e.preventDefault(); e.returnValue = ""; } });

async function midiAccess() {
  try { return await navigator.requestMIDIAccess({ sysex: true }); }
  catch { throw new Error("Web MIDI with SysEx was refused: allow it for this page"); }
}

$("fm1-find").addEventListener("click", async () => {
  const st = $("fm1-status");
  try {
    const { Updater } = await sloop;
    const up = new Updater(await midiAccess(), fm1Log);
    const dev = await up.find();
    if (!dev) { st.textContent = `No FM-1 answered${up.seen()}. Unplug the cartridge and connect the FM-1 to this computer.`; $("chip-fm1").textContent = "FM-1: not found"; $("chip-fm1").classList.remove("on"); return; }
    dev.link.close();
    st.textContent = `Found ${dev.id.text} on "${dev.name}"${/^ota-/i.test(dev.id.model) ? " — in its update mode: Install finishes the update" : ""}.`;
    $("chip-fm1").textContent = `FM-1: ${dev.id.text}`;
    $("chip-fm1").classList.add("on");
  } catch (e) { st.textContent = e.message || String(e); }
});

$("fm1-go").addEventListener("click", async () => {
  if (fm1Busy || !pkg) return;
  if (!confirm(`Install ${pkg.product} (${pkg.source}) on the FM-1?\n\nThe FM-1 reboots into its update loader, takes the image and starts again.`)) return;
  fm1Busy = true; refreshFm1();
  const bar = $("fm1-bar"), st = $("fm1-status");
  bar.hidden = false; bar.value = 0;
  const writes = Math.ceil(pkg.image.length / 512);
  const step = (k, a) => {
    fm1Log(`${k} ${a ?? ""}`);
    const say = { start: `Starting the update on ${a}…`, verify: "The FM-1 checks the package…", loader: "Waiting for the update loader…", write: "Writing…", reboot: "Restarting the FM-1…", done: `Installed: the FM-1 reports ${a}.` }[k];
    if (say) st.textContent = say;
    if (k === "write") bar.value = Math.min(99, (100 * a) / writes);
    if (k === "done") bar.value = 100;
  };
  try {
    const { Updater } = await sloop;
    const up = new Updater(await midiAccess(), fm1Log);
    const ota = await up.find((id) => /^ota-/i.test(id.model));
    if (ota) { ota.link.close(); st.textContent = (await up.resume(pkg.image, step)) ? "The interrupted update is finished." : "The loader stopped: replug the FM-1 and press Install again."; }
    else await up.install(pkg.image, pkg.product, step);
  } catch (e) { st.textContent = `${e.message || e}${e.detail ? ` (${e.detail})` : ""}`; fm1Log(String(e.stack || e)); }
  finally { fm1Busy = false; refreshFm1(); }
});

// ---------------------------------------------------------------- start
function renderAll() { renderEspInfo(); renderSlots(); renderSynths(); renderSlotUpdates(); }
(async () => {
  try { await loadCatalog(); } catch (e) { $("synth-grid").append(el("div", { class: "bad" }, `catalog.json: ${e.message}`)); }
  try { await loadBuilds(); } catch (e) { espLog(`local builds: ${e.message}`); }
  renderAll();
  probeSloopBuild();
  refreshFm1();
})();
