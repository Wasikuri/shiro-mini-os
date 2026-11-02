#!/usr/bin/env python3
"""v1.0 form/cookie/redirect echo server for the QEMU web checks.

Serves on :8083. The /echo endpoint encodes what it received in
the returned page's background color so the harness can assert
round-trips by counting pixels:

  q=hello, agree=yes   -> #55FF55 (85,255,85)
  q=hello only         -> #FFFF55 (255,255,85)
  who=world (POST)     -> #FF55FF (255,85,255)
  anything else        -> #FF5555 (255,85,85)

/cookietest: first visit (no session cookie) sets
Set-Cookie: session=abc123 and paints #FF5555; a later visit
carrying the cookie paints #55FF55.

/ok1 -> /ok2 -> /final: a 2-hop chain that must land.
/r1 -> /r2 -> /r3 -> /r4 -> /final: a 4-hop chain (over the
guest's follow limit, navigation must fail in-browser).
/loop redirects to itself forever.
"""
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

GETFORM = """<html><head><title>GetForm</title></head><body>
<h1>GET form</h1>
<form action="/echo" method="get">
<p>Query: <input type="text" name="q" size="20"></p>
<p><input type="checkbox" name="agree" value="yes"> Agree to terms</p>
<p><input type="submit" value="Send"></p>
</form></body></html>"""

POSTFORM = """<html><head><title>PostForm</title></head><body>
<h1>POST form</h1>
<form action="/echo" method="post">
<p>Who: <input type="text" name="who" size="20"></p>
<p><input type="submit" value="Post"></p>
</form></body></html>"""

FINAL = """<html><head><title>Final</title></head>
<body bgcolor="#55FF55"><h1>Final page</h1></body></html>"""


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _page(self, body, bg=None, extra=None):
        html = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(html)))
        if extra:
            for k, v in extra:
                self.send_header(k, v)
        self.end_headers()
        self.wfile.write(html)

    def _redir(self, to):
        self.send_response(302)
        self.send_header("Location", to)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _handle(self, params):
        path = urllib.parse.urlparse(self.path).path
        if path == "/getform.html":
            self._page(GETFORM)
        elif path == "/postform.html":
            self._page(POSTFORM)
        elif path == "/echo":
            q = params.get("q", [""])[0]
            agree = params.get("agree", [""])[0]
            who = params.get("who", [""])[0]
            if q == "hello" and agree == "yes":
                bg = "#55FF55"
            elif q == "hello":
                bg = "#FFFF55"
            elif who == "world":
                bg = "#FF55FF"
            else:
                bg = "#FF5555"
            rows = "".join(f"<p>{k} is {v[0]}</p>" for k, v in
                           sorted(params.items()))
            self._page(f"<html><head><title>Echo</title></head>"
                       f"<body bgcolor=\"{bg}\"><h1>Echo</h1>"
                       f"{rows}</body></html>")
        elif path == "/cookietest":
            cookie = self.headers.get("Cookie", "")
            if "session=abc123" in cookie:
                self._page("<html><head><title>Cookie</title></head>"
                           "<body bgcolor=\"#55FF55\">"
                           "<h1>cookie seen</h1></body></html>")
            else:
                self._page("<html><head><title>Cookie</title></head>"
                           "<body bgcolor=\"#FF5555\">"
                           "<h1>no cookie</h1></body></html>",
                           extra=[("Set-Cookie", "session=abc123")])
        elif path == "/final":
            self._page(FINAL)
        elif path == "/ok1":
            self._redir("/ok2")
        elif path == "/ok2":
            self._redir("/final")
        elif path == "/r1":
            self._redir("/r2")
        elif path == "/r2":
            self._redir("/r3")
        elif path == "/r3":
            self._redir("/r4")
        elif path == "/r4":
            self._redir("/final")
        elif path == "/loop":
            self._redir("/loop")
        else:
            body = b"<html><body><h1>not here</h1></body></html>"
            self.send_response(404)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    def do_GET(self):
        qs = urllib.parse.urlparse(self.path).query
        self._handle(urllib.parse.parse_qs(qs))

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n).decode("utf-8", "replace")
        self._handle(urllib.parse.parse_qs(body))


if __name__ == "__main__":
    ThreadingHTTPServer(("0.0.0.0", 8083), H).serve_forever()
