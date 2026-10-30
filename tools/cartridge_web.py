#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Serve the cartridge manager (web/) on this computer, with the local builds it installs.

    python3 tools/cartridge_web.py [--port 8737] [--open]

Then open http://127.0.0.1:8737/ in Chrome or Edge (Web Serial and Web MIDI need them; 127.0.0.1 is a secure
context). The server listens on the loopback interface only and serves a fixed set of paths:

  /web/...                                  the page (web/)
  /firmware/build-<profile>/<file>          a slot build's fm1_cartridge.bin, bootloader, partition table,
                                            ota_data_initial.bin and flasher_args.json
  /sloop-fm1/web/fm1ota.js, fm1pkg.js       Sloop's updater, which the FM-1 tab uses
  /sloop-fm1/build/felucca.fwsc             the local Sloop package
  /api/builds, /api/sloop                   what is built here
  POST /api/catalog/candidates              add a candidate firmware to web/catalog.json

It never builds or flashes anything: writing a device is the page's, on a button the user presses."""
import argparse
import hashlib
import json
import re
import sys
import webbrowser
from http import HTTPStatus
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / "web"
CATALOG = WEB / "catalog.json"
BUILD_FILES = {"fm1_cartridge.bin", "bootloader/bootloader.bin", "partition_table/partition-table.bin",
               "ota_data_initial.bin", "flasher_args.json"}
SLOOP_FILES = {"sloop-fm1/web/fm1ota.js", "sloop-fm1/web/fm1pkg.js", "sloop-fm1/build/felucca.fwsc"}
PROFILE = re.compile(r"^[a-z0-9][a-z0-9-]*$")
REPO = re.compile(r"^[\w.-]+/[\w.-]+$")
CANDIDATE_FIELDS = {"id", "name", "status", "repo", "licence", "summary", "effort", "notes"}


def resolve(path):
    """The file a URL path maps to, or None: only the paths listed in the module's docstring."""
    path = unquote(path).lstrip("/")
    if path.startswith("web/"):
        target = (WEB / path[4:]).resolve()
        return target if target.is_relative_to(WEB) and target.is_file() else None
    m = re.fullmatch(r"firmware/build-([^/]+)/(.+)", path)
    if m and PROFILE.match(m[1]) and m[2] in BUILD_FILES:
        target = ROOT / "firmware" / f"build-{m[1]}" / m[2]
        return target if target.is_file() else None
    if path in SLOOP_FILES:
        target = ROOT / path
        return target if target.is_file() else None
    return None


def builds():
    """Every firmware/build-<profile>/ holding a finished slot image."""
    out = []
    for d in sorted((ROOT / "firmware").glob("build-*")):
        image = d / "fm1_cartridge.bin"
        profile = d.name[len("build-"):]
        if not PROFILE.match(profile) or not image.is_file() or not (d / "flasher_args.json").is_file():
            continue
        data = image.read_bytes()
        args = json.loads((d / "flasher_args.json").read_text()).get("write_flash_args", [])
        out.append({"profile": profile, "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                    "mtime": int(image.stat().st_mtime), "flash_args": args})
    return out


def sloop():
    pkg = ROOT / "sloop-fm1" / "build" / "felucca.fwsc"
    if not pkg.is_file():
        return {"exists": False}
    data = pkg.read_bytes()
    return {"exists": True, "size": len(data), "sha256": hashlib.sha256(data).hexdigest(), "mtime": int(pkg.stat().st_mtime)}


def add_candidate(entry):
    """Merge one candidate into web/catalog.json; a ported synth is never replaced from the page."""
    if not isinstance(entry, dict) or set(entry) - CANDIDATE_FIELDS:
        raise ValueError(f"fields allowed: {', '.join(sorted(CANDIDATE_FIELDS))}")
    if entry.get("status") != "candidate" or not REPO.match(str(entry.get("repo", ""))):
        raise ValueError("a candidate needs status 'candidate' and repo 'owner/name'")
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]{0,40}", str(entry.get("id", ""))):
        raise ValueError("id: lower-case letters, digits and dashes")
    for k, v in entry.items():
        if not isinstance(v, str) or len(v) > 400:
            raise ValueError(f"{k}: a string of at most 400 characters")
    catalog = json.loads(CATALOG.read_text())
    for i, s in enumerate(catalog["synths"]):
        if s["id"] == entry["id"] or s.get("repo") == entry["repo"]:
            if s.get("status") != "candidate":
                raise ValueError(f"{s['id']} is already {s.get('status')}")
            catalog["synths"][i] = entry
            break
    else:
        catalog["synths"].append(entry)
    CATALOG.write_text(json.dumps(catalog, indent=2, ensure_ascii=False) + "\n")
    return entry


class Handler(SimpleHTTPRequestHandler):
    server_version = "cartridge-web"
    extensions_map = {**SimpleHTTPRequestHandler.extensions_map, ".js": "text/javascript", ".mjs": "text/javascript",
                      ".json": "application/json", ".bin": "application/octet-stream", ".fwsc": "application/octet-stream"}

    def local_origin(self):
        """A request from a page this server sent: the Host is the loopback address (no DNS rebinding) and a
        POST's Origin is the same."""
        host = self.headers.get("Host", "")
        port = self.server.server_address[1]
        allowed = {f"127.0.0.1:{port}", f"localhost:{port}"}
        if host not in allowed:
            return False
        origin = self.headers.get("Origin")
        return origin is None or origin in {f"http://{h}" for h in allowed}

    def send_json(self, data, status=HTTPStatus.OK):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def refuse(self, status, text):
        body = text.encode()
        self.send_response(status)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_GET(self):
        if not self.local_origin():
            return self.refuse(HTTPStatus.FORBIDDEN, "only http://127.0.0.1 or http://localhost on this port")
        path = urlsplit(self.path).path
        if path in ("/", "/web"):
            self.send_response(HTTPStatus.FOUND)
            self.send_header("Location", "/web/")
            self.end_headers()
            return
        if path == "/web/":
            path = "/web/index.html"
        if path == "/api/builds":
            return self.send_json({"builds": builds()})
        if path == "/api/sloop":
            return self.send_json(sloop())
        target = resolve(path)
        if target is None:
            return self.refuse(HTTPStatus.NOT_FOUND, "not served")
        data = target.read_bytes()
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", self.guess_type(str(target)))
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(data)

    do_HEAD = do_GET

    def do_POST(self):
        if not self.local_origin() or self.headers.get("Origin") is None:
            return self.refuse(HTTPStatus.FORBIDDEN, "a POST comes from the manager's own page")
        if urlsplit(self.path).path != "/api/catalog/candidates":
            return self.refuse(HTTPStatus.NOT_FOUND, "not served")
        if self.headers.get_content_type() != "application/json":
            return self.refuse(HTTPStatus.UNSUPPORTED_MEDIA_TYPE, "application/json")
        length = int(self.headers.get("Content-Length") or 0)
        if not 0 < length <= 16384:
            return self.refuse(HTTPStatus.BAD_REQUEST, "body of 1..16384 bytes")
        try:
            entry = add_candidate(json.loads(self.rfile.read(length)))
        except (ValueError, json.JSONDecodeError) as e:
            return self.refuse(HTTPStatus.BAD_REQUEST, str(e))
        self.send_json(entry)

    def log_message(self, fmt, *args):
        sys.stderr.write(f"{self.address_string()} {fmt % args}\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", type=int, default=8737)
    ap.add_argument("--open", action="store_true", help="open the page in the default browser")
    args = ap.parse_args()
    httpd = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    url = f"http://127.0.0.1:{httpd.server_address[1]}/web/"
    print(f"Cartridge manager: {url}  (Chrome or Edge; Ctrl-C stops)", flush=True)
    if args.open:
        webbrowser.open(url)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
