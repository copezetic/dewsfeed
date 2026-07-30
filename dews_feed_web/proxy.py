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
    "earthquake.usgs.gov",      # live quake feed (Earthquake Watch)
    "services.swpc.noaa.gov",   # planetary Kp index (Aurora Watch)
    "gamma-api.polymarket.com", # prediction-market odds (keyless)
    "ll.thespacedevs.com",      # Launch Library 2 — upcoming rocket launches
    "api.g7vrd.co.uk",          # ISS pass predictions over the office
    "api.frankfurter.dev",      # ECB FX reference rates (keyless)
    "air-quality-api.open-meteo.com",  # AQI + UV tiles
    "api.wheretheiss.at",       # ISS position for the SITREP map
    "hacker-news.firebaseio.com",  # HN top stories (tech terminal)
    "freehoroscopeapi.com",     # daily horoscope page
}
# ── Wall radio ("DEWS FM") ─────────────────────────────────────────────
# Curated keyless streams the dashboard's <audio> element can play directly.
# State lives here so the phone (via Tailscale) and the dashboard (localhost)
# see the same switch. Defaults to OFF on every proxy restart so a reboot
# never starts blasting music into the office unasked.
STATIONS = [
    ("Groove Salad — ambient beats",       "https://ice1.somafm.com/groovesalad-256-mp3"),
    ("Secret Agent — spy-movie lounge",    "https://ice1.somafm.com/secretagent-128-mp3"),
    ("DEF CON Radio — hacker electronic",  "https://ice1.somafm.com/defcon-128-mp3"),
    ("Radio Paradise — eclectic rock",     "https://stream.radioparadise.com/mp3-192"),
    ("RP Mellow — chill mix",              "https://stream.radioparadise.com/mellow-192"),
]
MUSIC = {"on": False, "i": 0}

MUSIC_UI = """<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DEWS FM</title><style>
body{background:#0b0e17;color:#e8ecf2;font-family:ui-monospace,Menlo,monospace;
     margin:0;padding:24px;max-width:480px}
h1{font-size:20px;letter-spacing:.25em;color:#ffb400;margin:0 0 4px}
p{color:#7a8699;font-size:12px;letter-spacing:.1em;margin:0 0 20px}
button{display:block;width:100%;margin:10px 0;padding:16px;border-radius:10px;
       border:1px solid #232c44;background:#111624;color:#e8ecf2;font-size:15px;
       font-family:inherit;text-align:left;cursor:pointer}
button.on{border-color:#4ade5b;color:#4ade5b;box-shadow:0 0 14px rgba(74,222,91,.25)}
#pwr{font-size:18px;font-weight:700;text-align:center;letter-spacing:.2em}
#pwr.playing{background:#12240f;border-color:#4ade5b;color:#4ade5b}
</style></head><body>
<h1>DEWS FM</h1><p>OFFICE WALL RADIO · TOGGLE FROM ANYWHERE ON THE TAILNET</p>
<button id="pwr" onclick="hit(st.on?'/music/off':'/music/on')">…</button>
<div id="list"></div>
<script>
let st={on:false,i:0};
async function refresh(){
  st=await (await fetch('/music')).json();
  document.getElementById('pwr').textContent=st.on?'■ STOP':'► PLAY';
  document.getElementById('pwr').className=st.on?'playing':'';
  document.getElementById('list').innerHTML=st.stations.map((s,i)=>
    `<button class="${i===st.i?'on':''}" onclick="hit('/music/set?i=${i}')">`+
    `${i===st.i?'▶ ':''}${s}</button>`).join('');
}
async function hit(p){await fetch(p);refresh();}
refresh();setInterval(refresh,5000);
</script></body></html>"""

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

    def _local(self):
        # The relay + tasks stay localhost-only; the music switch is the one
        # thing the phone may reach now that we bind beyond loopback.
        return self.client_address[0] in ("127.0.0.1", "::1")

    def do_GET(self):
        if self.path.startswith("/ping"):
            self._cors()
            self.wfile.write(b"ok")
            return
        if self.path.startswith("/music"):
            import json as _json
            p = urllib.parse.urlparse(self.path)
            if p.path == "/music/ui":
                self._cors(200, "text/html; charset=utf-8")
                self.wfile.write(MUSIC_UI.encode())
                return
            if p.path == "/music/on":  MUSIC["on"] = True
            if p.path == "/music/off": MUSIC["on"] = False
            if p.path == "/music/next": MUSIC["i"] = (MUSIC["i"]+1) % len(STATIONS)
            if p.path == "/music/set":
                try:
                    i = int(urllib.parse.parse_qs(p.query).get("i", ["0"])[0])
                    if 0 <= i < len(STATIONS): MUSIC["i"] = i
                except Exception:
                    pass
            name, url = STATIONS[MUSIC["i"]]
            self._cors(200, "application/json")
            self.wfile.write(_json.dumps({
                "on": MUSIC["on"], "i": MUSIC["i"], "name": name, "url": url,
                "stations": [s[0] for s in STATIONS]}).encode())
            return
        if not self._local():
            self._cors(403)
            self.wfile.write(b"music endpoints only from off-box")
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
