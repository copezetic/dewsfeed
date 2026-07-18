#!/usr/bin/env python3
"""
Dews Feed CORS proxy — runs on the Raspberry Pi alongside the dashboard.

A handful of data sources (Yahoo Finance, FRED, Google News RSS, Google Maps,
OpenSky) don't send CORS headers, so the browser can't fetch them directly.
This tiny proxy forwards those requests and adds the header. Whitelisted
hosts only.

Also serves /tasks — the office WORK_TASKS.md, synced from the PC — so the
dashboard's TO-DO page can read it.

Run:  python3 proxy.py         (listens on http://localhost:8787)
"""
import http.server
import urllib.request
import urllib.parse
import socketserver
import os

PORT = 8787
TASKS_FILE = os.path.expanduser("~/dews_feed_web/work_tasks.md")
ALLOWED_HOSTS = {
    "query1.finance.yahoo.com",
    "www.alphavantage.co",      # PNBK fallback — Yahoo 429s bot-ish IPs
    "fred.stlouisfed.org",
    "api.stlouisfed.org",
    "news.google.com",
    "maps.googleapis.com",
    "opensky-network.org",      # flights radar
    "www.ndbc.noaa.gov",        # LIS buoy 44025 live waves/water-temp
    "script.google.com",        # WHOOP relay (Apps Script)
    "script.googleusercontent.com",  # Apps Script redirect target
    "api.planespotters.net",    # aircraft photos for the flights radar
    "cdn.cboe.com",             # delayed intraday quotes — Yahoo 429s the Pi
                                # and Alpha Vantage free tier is EOD-only
    "router.project-osrm.org",  # keyless drive times — Google key IP-denies office
    "todew.dewitt-bf3.workers.dev",  # ToDew task sync (Work-category to-do page)
    "api.nasa.gov",             # APOD daily space photo
    "api.artic.edu",            # Art Institute of Chicago — painting of the day
}
UA = ("Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36")
# Planespotters 403s browser-like UAs; their API wants one that identifies
# the application instead.
UA_OVERRIDES = {
    "api.planespotters.net":
        "DewsFeed/1.0 (dewitt.hutchins@gmail.com; personal dashboard)",
}
EXTRA_HEADERS = {
    "Accept": "application/json,text/csv,text/plain,*/*",
    "Accept-Language": "en-US,en;q=0.9",
}


class Handler(http.server.BaseHTTPRequestHandler):
    def _cors(self, code=200, ctype="text/plain"):
        self.send_response(code)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Type", ctype)
        self.end_headers()

    def do_OPTIONS(self):
        # CORS preflight — the browser sends one whenever a proxied request
        # carries an Authorization header (ToDew sync).
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "Authorization, Content-Type")
        self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
        self.end_headers()

    def do_GET(self):
        if self.path.startswith("/ping"):
            self._cors()
            self.wfile.write(b"ok")
            return
        if self.path.startswith("/tasks"):
            try:
                with open(TASKS_FILE, "rb") as f:
                    body = f.read()
                self._cors(200, "text/markdown; charset=utf-8")
                self.wfile.write(body)
            except Exception:
                self._cors(404)
                self.wfile.write(b"no tasks file")
            return
        if not self.path.startswith("/p?"):
            self._cors(404)
            self.wfile.write(b"not found")
            return
        qs = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        target = qs.get("u", [""])[0]
        host = urllib.parse.urlparse(target).hostname or ""
        if host not in ALLOWED_HOSTS:
            self._cors(403)
            self.wfile.write(b"host not allowed")
            return
        try:
            hdrs = {"User-Agent": UA_OVERRIDES.get(host, UA)}
            hdrs.update(EXTRA_HEADERS)
            auth = self.headers.get("Authorization")
            if auth:                      # pass through bearer tokens (ToDew)
                hdrs["Authorization"] = auth
            req = urllib.request.Request(target, headers=hdrs)
            with urllib.request.urlopen(req, timeout=20) as r:
                body = r.read()
                ctype = r.headers.get("Content-Type", "text/plain")
            self._cors(200, ctype)
            self.wfile.write(body)
        except Exception as e:
            self._cors(502)
            self.wfile.write(str(e).encode())

    def log_message(self, *args):
        pass  # quiet


if __name__ == "__main__":
    # Threaded: a single slow upstream (20s timeout) must not block every
    # other dashboard fetch queued behind it — the old single-threaded
    # TCPServer stalled the whole dashboard whenever one API hung.
    with http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler) as httpd:
        print(f"Dews Feed proxy on http://localhost:{PORT}")
        httpd.serve_forever()
