#!/usr/bin/env python3
"""Stand-in for the bits of GitHub that github_downloader.c uses: the
releases/latest API and release-asset downloads (which redirect, like the real
ones). Usage: mock_github.py <port>. Repos: test/proj (3 assets), noctis1008-wq/*."""
import json, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1])
FILES = {
    "test/proj": {"tool.firm": b"FIRM" + bytes(range(256)) * 40, "script.c": b"int main(void){return 0;}\n", "big.3dsx": b"3DSX" * 5000},
    "noctis1008-wq/checkpoint": {"dropbox.c": b"/* dropbox */\n", "github_downloader.c": b"/* gh */\n"},
    "noctis1008-wq/godmode9-with-cartridge-fixer": {"GodMode9_RefFix.firm": b"FIRMREFFIX" * 100},
}

class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass
    def send(self, code, body=b"", ctype="application/json", extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)
    def do_GET(self):
        if not self.headers.get("User-Agent"):
            return self.send(403, b'{"message":"no user agent"}')
        p = self.path
        if p.startswith("/api/repos/") and p.endswith("/releases/latest"):
            repo = p[len("/api/repos/"):-len("/releases/latest")]
            if repo not in FILES:
                return self.send(404, b'{"message":"Not Found"}')
            assets = [{"name": n, "size": len(b), "browser_download_url": "http://127.0.0.1:%d/%s/releases/download/v1/%s" % (PORT, repo, n)} for n, b in FILES[repo].items()]
            return self.send(200, json.dumps({"tag_name": "v1", "body": "x" * 3000, "assets": assets}).encode())
        for repo, files in FILES.items():
            for name, data in files.items():
                if p in ("/%s/releases/latest/download/%s" % (repo, name), "/%s/releases/download/v1/%s" % (repo, name)):
                    return self.send(302, extra={"Location": "/objects/%s/%s" % (repo, name)})
                if p == "/objects/%s/%s" % (repo, name):
                    return self.send(200, data, "application/octet-stream")
        self.send(404, b"Not Found", "text/plain")

ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
