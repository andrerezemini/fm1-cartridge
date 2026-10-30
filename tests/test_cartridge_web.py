#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tools/cartridge_web.py serves only its fixed paths, to the loopback host, and adds candidates to the catalogue
only from its own page: a traversal, another Host (DNS rebinding), a foreign Origin or a ported synth is refused."""
import http.client
import importlib.util
import json
import shutil
import sys
import tempfile
import threading
from pathlib import Path

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("cartridge_web", root / "tools" / "cartridge_web.py")
web = importlib.util.module_from_spec(spec)
spec.loader.exec_module(web)

tmp = Path(tempfile.mkdtemp())
web.CATALOG = tmp / "catalog.json"                    # writes go to a copy
web.Handler.log_message = lambda *args: None
shutil.copy(root / "web" / "catalog.json", web.CATALOG)
httpd = web.ThreadingHTTPServer(("127.0.0.1", 0), web.Handler)
port = httpd.server_address[1]
threading.Thread(target=httpd.serve_forever, daemon=True).start()
failures = []


def request(method, path, body=None, headers=None):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    h = {"Host": f"127.0.0.1:{port}", **(headers or {})}
    c.request(method, path, body=body, headers=h)
    r = c.getresponse()
    data = r.read()
    c.close()
    return r.status, data, r


def expect(what, got, want):
    if got != want:
        failures.append(f"{what}: {got} (expected {want})")


status, data, r = request("GET", "/web/")
expect("the page", status, 200)
expect("the page is the manager", b"Cartridge Manager" in data, True)
expect("a module's type", request("GET", "/web/manager.js")[2].getheader("Content-Type"), "text/javascript")
expect("the root redirects", request("GET", "/")[0], 302)
for path in ("/web/../tools/cartridge_web.py", "/web/%2e%2e/AGENTS.md", "/AGENTS.md", "/firmware/build-x/../../AGENTS.md",
             "/firmware/build-clipper/sdkconfig", "/firmware/build-../x/fm1_cartridge.bin", "/sloop-fm1/build.sh", "/.git/config"):
    expect(f"GET {path}", request("GET", path)[0], 404)
expect("another Host", request("GET", "/web/", headers={"Host": "evil.example:80"})[0], 403)
status, data, _ = request("GET", "/api/builds")
expect("/api/builds", status, 200)
expect("/api/builds lists", isinstance(json.loads(data).get("builds"), list), True)
expect("/api/sloop", "exists" in json.loads(request("GET", "/api/sloop")[1]), True)

origin = {"Origin": f"http://127.0.0.1:{port}", "Content-Type": "application/json"}
candidate = {"id": "test-x0x", "name": "X0X", "status": "candidate", "repo": "charlesvestal/fm1-x0x", "licence": "GPL-3.0",
             "summary": "909, 808 and two 303s", "effort": "medium", "notes": "host build"}
body = json.dumps(candidate)
expect("POST without Origin", request("POST", "/api/catalog/candidates", body, {"Content-Type": "application/json"})[0], 403)
expect("POST from another site", request("POST", "/api/catalog/candidates", body, {**origin, "Origin": "https://evil.example"})[0], 403)
expect("POST as a form", request("POST", "/api/catalog/candidates", body, {**origin, "Content-Type": "text/plain"})[0], 415)
expect("POST a candidate", request("POST", "/api/catalog/candidates", body, origin)[0], 200)
expect("the candidate is kept", any(s["id"] == "test-x0x" for s in json.loads(web.CATALOG.read_text())["synths"]), True)
expect("replacing a ported synth", request("POST", "/api/catalog/candidates", json.dumps({**candidate, "id": "clipper"}), origin)[0], 400)
expect("an unknown field", request("POST", "/api/catalog/candidates", json.dumps({**candidate, "profiles": ["x"]}), origin)[0], 400)
expect("a ported status", request("POST", "/api/catalog/candidates", json.dumps({**candidate, "status": "ported"}), origin)[0], 400)
expect("the real catalogue is untouched", "test-x0x" in (root / "web" / "catalog.json").read_text(), False)

httpd.shutdown()
shutil.rmtree(tmp)
if failures:
    sys.exit("cartridge_web: " + "; ".join(failures))
print("cartridge_web: fixed paths, loopback Host, same-origin POST and candidate-only catalogue writes hold")
