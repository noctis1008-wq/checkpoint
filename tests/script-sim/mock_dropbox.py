#!/usr/bin/env python3
"""A small stand-in for the parts of the Dropbox HTTP API that dropbox.c uses.

It is strict where real Dropbox is strict (and where a script bug would only
show up on the console): ASCII-only Dropbox-API-Arg headers, the content-type
rules of RPC / upload / download endpoints, bearer tokens, 409 for missing
paths, and paginated list_folder (page size 2, so the cursor path is exercised).

Usage: mock_dropbox.py <port> <state.json>
State (the stored files, as {path: hex}) is written to state.json after every
change and read back at start, so a test can seed and inspect it.
"""

import json
import os
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

APP_KEY = "KEY"
APP_SECRET = "SECRET"
GOOD_CODE = "GOODCODE"
REFRESH = "REFRESH1"
PAGE = 2

state_path = sys.argv[2]
files = {}  # lower-case path -> (display path, bytes)
tokens = set()
counter = [0]
cursors = {}
log = []


def save():
    with open(state_path, "w") as f:
        json.dump({"files": {v[0]: v[1].hex() for v in files.values()}, "log": log}, f, ensure_ascii=False, indent=1)


if os.path.exists(state_path):
    with open(state_path) as f:
        for p, h in json.load(f).get("files", {}).items():
            files[p.lower()] = (p, bytes.fromhex(h))


def children(folder):
    folder = folder.rstrip("/").lower()
    out = {}
    for low, (disp, _) in files.items():
        if not low.startswith(folder + "/"):
            continue
        rest = disp[len(folder) + 1:]
        if "/" in rest:
            name = rest.split("/")[0]
            out[name.lower()] = {".tag": "folder", "name": name}
        else:
            out[rest.lower()] = {".tag": "file", "name": rest, "size": len(files[low][1])}
    return list(out.values())


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def reply(self, code, obj=None, raw=None, ctype="application/json"):
        body = raw if raw is not None else (json.dumps(obj).encode() if obj is not None else b"")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def authed(self):
        a = self.headers.get("Authorization", "")
        if not a.startswith("Bearer ") or a[7:] not in tokens:
            self.reply(401, {"error_summary": "invalid_access_token/"})
            return False
        return True

    def api_arg(self):
        raw = self.headers.get("Dropbox-API-Arg")
        if raw is None:
            self.reply(400, raw=b"Error in call: missing Dropbox-API-Arg", ctype="text/plain")
            return None
        if any(ord(c) > 127 for c in raw):
            self.reply(400, raw=b"Error in call: Dropbox-API-Arg must be ASCII (escape with \\u)", ctype="text/plain")
            return None
        try:
            return json.loads(raw)
        except Exception:
            self.reply(400, raw=b"Error in call: Dropbox-API-Arg is not JSON", ctype="text/plain")
            return None

    def do_POST(self):
        path = self.path
        ctype = self.headers.get("Content-Type", "")
        data = self.body()
        log.append(path)
        save()

        if path == "/oauth2/token":
            if not ctype.startswith("application/x-www-form-urlencoded"):
                return self.reply(400, {"error": "bad content type"})
            form = dict(urllib.parse.parse_qsl(data.decode()))
            if form.get("client_id") != APP_KEY or form.get("client_secret") != APP_SECRET:
                return self.reply(400, {"error": "invalid_client"})
            if form.get("grant_type") == "authorization_code":
                if form.get("code") != GOOD_CODE:
                    return self.reply(400, {"error": "invalid_grant"})
                counter[0] += 1
                tok = "sl." + "x" * 1200 + str(counter[0])  # long, like real ones
                tokens.add(tok)
                return self.reply(200, {"access_token": tok, "refresh_token": REFRESH, "expires_in": 14400})
            if form.get("grant_type") == "refresh_token":
                if form.get("refresh_token") != REFRESH:
                    return self.reply(400, {"error": "invalid_grant"})
                counter[0] += 1
                tok = "sl." + "y" * 1200 + str(counter[0])
                tokens.add(tok)
                return self.reply(200, {"access_token": tok, "expires_in": 14400})
            return self.reply(400, {"error": "unsupported_grant_type"})

        if path.startswith("/api/2/"):
            if not self.authed():
                return
            if ctype != "application/json":
                return self.reply(400, raw=b"Error in call: Bad HTTP Content-Type for RPC", ctype="text/plain")
            try:
                arg = json.loads(data.decode() or "null")
            except Exception:
                return self.reply(400, raw=b"Error in call: body is not JSON", ctype="text/plain")
            ep = path[len("/api/2/"):]
            if ep == "users/get_current_account":
                return self.reply(200, {"email": "tester@example.com", "name": {"display_name": "Tester"}})
            if ep == "auth/token/revoke":
                tokens.clear()
                return self.reply(200, None)
            if ep == "files/list_folder":
                folder = arg["path"].rstrip("/")
                if not any(k.startswith(folder.lower() + "/") for k in files):
                    return self.reply(409, {"error_summary": "path/not_found/.."})
                entries = children(folder)
                return self.page(entries, 0)
            if ep == "files/list_folder/continue":
                c = cursors.get(arg.get("cursor"))
                if c is None:
                    return self.reply(409, {"error_summary": "reset/"})
                return self.page(*c)
            return self.reply(404, {"error_summary": "unknown endpoint"})

        if path == "/content/2/files/upload":
            if not self.authed():
                return
            arg = self.api_arg()
            if arg is None:
                return
            if ctype != "application/octet-stream":
                return self.reply(400, raw=b"Error in call: upload needs application/octet-stream", ctype="text/plain")
            if arg.get("mode") != "overwrite":
                return self.reply(400, raw=b"test expects mode overwrite", ctype="text/plain")
            files[arg["path"].lower()] = (arg["path"], data)
            save()
            return self.reply(200, {"name": arg["path"].split("/")[-1], "size": len(data)})

        if path == "/content/2/files/download":
            if not self.authed():
                return
            arg = self.api_arg()
            if arg is None:
                return
            if ctype not in ("", "application/octet-stream", "text/plain; charset=dropbox-cors-hack"):
                return self.reply(400, raw=b"Error in call: Bad HTTP Content-Type header for download", ctype="text/plain")
            if data:
                return self.reply(400, raw=b"Error in call: download takes no body", ctype="text/plain")
            f = files.get(arg["path"].lower())
            if f is None:
                return self.reply(409, {"error_summary": "path/not_found/"})
            return self.reply(200, raw=f[1], ctype="application/octet-stream")

        self.reply(404, {"error_summary": "no such endpoint"})

    def page(self, entries, start):
        chunk = entries[start:start + PAGE]
        more = start + PAGE < len(entries)
        cur = ""
        if more:
            cur = "cur%d" % len(cursors)
            cursors[cur] = (entries, start + PAGE)
        return self.reply(200, {"entries": chunk, "has_more": more, "cursor": cur})


if __name__ == "__main__":
    save()
    ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
