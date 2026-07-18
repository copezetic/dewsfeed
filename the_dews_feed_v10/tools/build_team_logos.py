"""
build_team_logos.py — one-time converter that turns the ChuckBuilds LEDMatrix
team-logo PNGs into a single PROGMEM C header (`team_logos.h`) consumed by
the_dews_feed_v9.ino.

What it does:
  1. Downloads team logos for NFL, NHL, MLB, NBA from
     https://github.com/ChuckBuilds/LEDMatrix/tree/main/assets/sports/
  2. Resizes each PNG to two sizes — 20×20 (scores card) and 48×48
     (transition card hero) — using Lanczos for crisp pixel art.
  3. Quantizes RGBA → RGB565, with alpha < 128 marked as transparent
     (sentinel value 0x0001).
  4. Emits `team_logos.h` next to this script's parent folder (one level up).

Dependencies (run once):
    pip install pillow requests

Usage:
    python build_team_logos.py
    # → writes ../team_logos.h
    # Then re-flash the .ino from Arduino IDE.

If a team PNG 404s in the upstream repo, the script logs it and skips —
those teams just fall back to the existing colored monogram at runtime.
"""

import os
import sys
import urllib.request
from io import BytesIO
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("ERROR: Pillow not installed. Run: pip install pillow", file=sys.stderr)
    sys.exit(1)

# ---------------------------------------------------------------------------
# Teams. ESPN abbreviations match ChuckBuilds filenames in most cases.
# Edit these lists if you want to add or skip teams.
# ---------------------------------------------------------------------------
NFL = ["ARI","ATL","BAL","BUF","CAR","CHI","CIN","CLE","DAL","DEN","DET","GB",
       "HOU","IND","JAX","KC","LAC","LAR","LV","MIA","MIN","NE","NO","NYG",
       "NYJ","PHI","PIT","SEA","SF","TB","TEN","WSH"]

NHL = ["ANA","BOS","BUF","CAR","CBJ","CGY","CHI","COL","DAL","DET","EDM","FLA",
       "LA","MIN","MTL","NJ","NSH","NYI","NYR","OTT","PHI","PIT","SEA","SJ",
       "STL","TB","TOR","UTA","VAN","VGK","WPG","WSH"]

MLB = ["ARI","ATL","BAL","BOS","CHC","CWS","CIN","CLE","COL","DET","HOU","KC",
       "LAA","LAD","MIA","MIL","MIN","NYM","NYY","OAK","PHI","PIT","SD","SF",
       "SEA","STL","TB","TEX","TOR","WSH"]

NBA = ["ATL","BOS","BKN","CHA","CHI","CLE","DAL","DEN","DET","GS","HOU","IND",
       "LAC","LAL","MEM","MIA","MIL","MIN","NO","NYK","OKC","ORL","PHI","PHX",
       "POR","SAC","SA","TOR","UTAH","WAS"]

LEAGUES = [("NFL", NFL), ("NHL", NHL), ("MLB", MLB), ("NBA", NBA)]

UPSTREAM = ("https://raw.githubusercontent.com/ChuckBuilds/LEDMatrix/"
            "main/assets/sports/{league}_logos/{abbr}.png")

SIZES = [20, 48]              # 20×20 for scores card, 48×48 for transitions
ALPHA_THRESHOLD = 128         # below this = transparent
TRANSPARENT = 0x0001          # sentinel value emitted for transparent pixels


def rgb_to_565(r, g, b):
    """Pack 8-8-8 RGB into 5-6-5. Avoid emitting the sentinel 0x0001 for
    real colors by nudging it to 0x0002 if it ever lands there."""
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    if v == TRANSPARENT:
        v = 0x0002
    return v


def fetch(url):
    try:
        with urllib.request.urlopen(url, timeout=20) as r:
            return r.read()
    except Exception as e:
        print(f"  ! fetch failed: {url} ({e})")
        return None


def render_logo(png_bytes, size):
    """Resize PNG to size×size, return flat list of size² RGB565 (or
    TRANSPARENT sentinel) values."""
    img = Image.open(BytesIO(png_bytes)).convert("RGBA")
    img = img.resize((size, size), Image.LANCZOS)
    out = []
    for y in range(size):
        for x in range(size):
            r, g, b, a = img.getpixel((x, y))
            if a < ALPHA_THRESHOLD:
                out.append(TRANSPARENT)
            else:
                out.append(rgb_to_565(r, g, b))
    return out


def emit_array(f, name, values, width):
    """Write `const uint16_t name[N] PROGMEM = { ... };` with 16 values per
    line for readability."""
    f.write(f"const uint16_t {name}[{len(values)}] PROGMEM = {{\n  ")
    for i, v in enumerate(values):
        f.write(f"0x{v:04X}")
        if i != len(values) - 1:
            f.write(",")
            f.write("\n  " if (i + 1) % width == 0 else " ")
    f.write("\n};\n\n")


def main():
    here = Path(__file__).resolve().parent
    out_path = here.parent / "team_logos.h"
    print(f"Output: {out_path}")

    arrays = []          # list of (varname, values, line_width)
    index = []           # list of (league, abbr, name20, name48)
    fetched = 0
    skipped = []

    for league, teams in LEAGUES:
        print(f"\n== {league} ({len(teams)} teams) ==")
        for abbr in teams:
            url = UPSTREAM.format(league=league.lower(), abbr=abbr)
            png = fetch(url)
            if png is None:
                skipped.append((league, abbr))
                continue
            try:
                px20 = render_logo(png, 20)
                px48 = render_logo(png, 48)
            except Exception as e:
                print(f"  ! decode failed for {league} {abbr}: {e}")
                skipped.append((league, abbr))
                continue
            # Sanitize abbreviation for use in a C identifier
            safe = abbr.replace("-", "_").replace(" ", "_")
            n20 = f"LOGO_{league}_{safe}_20"
            n48 = f"LOGO_{league}_{safe}_48"
            arrays.append((n20, px20, 20))
            arrays.append((n48, px48, 24))
            index.append((league, abbr, n20, n48))
            fetched += 1
            print(f"  ok  {league} {abbr}")

    print(f"\nFetched {fetched} teams, skipped {len(skipped)}")
    if skipped:
        print("Skipped (will fall back to monogram at runtime):")
        for league, abbr in skipped:
            print(f"  {league} {abbr}")

    print(f"\nWriting {out_path} ...")
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("// AUTO-GENERATED by tools/build_team_logos.py — do not edit by hand.\n")
        f.write("// Sourced from https://github.com/ChuckBuilds/LEDMatrix (team logos).\n")
        f.write("// Personal-use only; team marks are property of their respective leagues.\n")
        f.write("#pragma once\n#include <Arduino.h>\n#include <pgmspace.h>\n\n")
        f.write(f"#define LOGO_TRANSPARENT 0x{TRANSPARENT:04X}\n")
        f.write("#define LOGO_SIZE_20 20\n#define LOGO_SIZE_48 48\n\n")

        for name, values, width in arrays:
            emit_array(f, name, values, width)

        # Index table
        f.write("struct TeamLogoEntry {\n")
        f.write("  const char* league;\n")
        f.write("  const char* abbr;\n")
        f.write("  const uint16_t* px20;\n")
        f.write("  const uint16_t* px48;\n")
        f.write("};\n\n")
        f.write(f"const int TEAM_LOGO_COUNT = {len(index)};\n")
        f.write("const TeamLogoEntry TEAM_LOGOS[TEAM_LOGO_COUNT] = {\n")
        for league, abbr, n20, n48 in index:
            f.write(f'  {{ "{league}", "{abbr}", {n20}, {n48} }},\n')
        f.write("};\n")

    flash_kb = (fetched * (400 + 2304) * 2) // 1024
    print(f"\nDone. Approx flash use: {flash_kb} KB in PROGMEM (you have 4 MB).")
    print("Now re-flash the .ino from the Arduino IDE.")


if __name__ == "__main__":
    main()
