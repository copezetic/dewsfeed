# The Dews Feed

A custom DIY ambient information dashboard. Personal project for DeWitt's home in Stamford, CT and lake cabin (Portly Bear Lodge) at Lake Wallenpaupack, PA.

## Hardware

- **Display:** 128×64 LED matrix made from 4× Waveshare P4 HUB75 panels in a 2×2 grid
- **Controller:** ESP32-DevKitC-32E
- **Adapter:** Seengreat RGB Matrix Adapter Board
- **Library:** ESP32-HUB75-MatrixPanel-DMA + VirtualMatrixPanel for the 2×2 chain

### Pin mapping
```
R1=18  G1=25  B1=5
R2=17  G2=33  B2=16
A=4    B=3    C=0    D=21   E=32
CLK=2  LAT=19 OE=15
clkphase=true
```

### Critical hardware constants
- **Chain constant:** `CHAIN_TOP_RIGHT_DOWN` (determined by systematic testing — do not change)
- `VirtualMatrixPanel*` pointer must be typed as `VirtualMatrixPanel*`, never cast from `MatrixPanel_I2S_DMA*`
- `setBrightness8()` must be called on the **hardware** matrix pointer, not the virtual one

## File layout

Currently a single monolithic `.ino` file. As of v9 it's ~3,950 lines and growing. Splitting into headers is a pending v10+ refactor — the team color tables, logo pixel art, and chart drawing functions are good candidates to extract.

Current version: **v9**. Previous versions kept as `the_dews_feed_v5.ino` through `the_dews_feed_v8.ino` for diff/rollback reference.

## Architecture

### Slide system
- Day playlist (~20 slides) and night playlist (~6 slides) defined as `SlideEntry` arrays in `DAY_PLAYLIST[]` and `NIGHT_PLAYLIST[]`
- Each entry is `{ SlideID id, unsigned long durationMs }`
- Mode flips at 6am (day) and 10pm (night). Night cards run at reduced brightness.
- `loop()` advances `playIdx`, dispatches to the renderer for the current slide via switch statement on `SlideID`

### Render strategies (IMPORTANT)

Three rendering patterns. Picking the wrong one causes flickering or trails:

**1. Static cards (news, scores, finance, weather)** — gated to redraw every 2 seconds via `STATIC_REDRAW_MS=2000` + `lastStaticDraw`. First thing in the renderer:
```cpp
if(millis()-lastStaticDraw<STATIC_REDRAW_MS) return;
lastStaticDraw=millis();
cls();
```
Without this gate the card strobes at ~70fps.

**2. Animated scene cards (RFR, Cabin, Tiki)** — draw-once-then-animate-deltas pattern:
```cpp
bool drawBG=(lastStaticDraw==0);
if(drawBG){
  lastStaticDraw=millis();
  cls();
  // draw entire static background ONCE: sky, ground, trees, buildings
}
// animated layer below: only draw moving pixels with proper background restore
```
The Tiki strobing bug in v7 was `cls()` every frame here. Cabin bear walking artifact was the bear's bounding-box erase wiping out trees in its path.

**3. Full-animation cards (sports anim, money anim, game of life, aurora)** — `cls()` every frame is fine because the whole frame is computed from scratch.

### Animation erase pattern
For moving sprites over complex backgrounds (e.g., the cabin bear walking past pine trees):
1. Restore sky/ground colors under the OLD position
2. Re-draw any static elements (trees, buildings) whose bounds intersect the restored band
3. Advance the sprite
4. Draw the sprite at the NEW position

Don't try to read framebuffer state — the lib doesn't support it and `const_cast` workarounds fail on flash (see gotcha below).

## API integrations

| Source | Purpose | Refresh |
|---|---|---|
| OpenWeatherMap (`/forecast` free) | Stamford + Lake Wallenpaupack weather | 10 min |
| ESPN scoreboard | MLB/NBA/NHL/NFL/NCAAB scores | 90 sec |
| Yahoo Finance (`query1.finance.yahoo.com`) | S&P 500 | 5 min |
| CoinGecko | BTC | 5 min |
| alternative.me | Fear & Greed | 5 min |
| EIA.gov | Gas prices | hourly |
| FRED/St. Louis Fed | 10yr Treasury (DGS10) | hourly |
| Google Maps Distance Matrix | Traffic to Stamford + Lake Ariel | 10 min |
| NOAA CO-OPS | Tides, station 8467150 (Stamford) | 30 min |
| NDBC | Buoy 44025 waves | 30 min |
| NWS alerts | Weather warnings | 15 min |
| USGS | Lake water temp + level, site 01427510 | hourly |
| OpenSky Network | Flight tracking, bbox 40.60–41.50N, 74.20–72.80W | 5 min |
| wheretheiss.at | ISS position | 5 min |
| WHOOP via Google Apps Script relay | Recovery/strain | 30 min |
| Google Calendar via relay | Today's events | 15 min |
| NewsAPI | National top headlines + Stamford CT local | 10 min |

**Don't use `/forecast/daily`** — requires paid OWM plan. Parse the free `/forecast` 3-hour endpoint and scan for daily hi/lo manually.

## Critical gotchas (do not relearn the hard way)

### ESP32 flash is read-only at runtime
`const` arrays on ESP32 sit in flash memory. **`const_cast` to mutate them silently fails** — writes don't take effect, the field stays at its original initialized value. This bit us repeatedly with buoy color initialization.

**Wrong:**
```cpp
const BuoyDef LIS_BUOYS[] = { {"44025", 40.97, -72.00, 88, 40, 0, 2500, 2500}, ... };
void initBuoyColors(){
  const_cast<BuoyDef&>(LIS_BUOYS[0]).col = C_WHITE;  // silently no-ops
}
```

**Right:** Hardcode RGB565 literals at compile time:
```cpp
const BuoyDef LIS_BUOYS[] = { {"44025", 40.97, -72.00, 88, 40, 0xFFFF, 2500, 2500}, ... };
```

### `localtime()` uses a shared static buffer
Two `localtime()` calls in the same expression clobber each other. Hit this with sunrise/sunset both showing the same time.

**Wrong:**
```cpp
strftime(up, 8, "%I:%M%p", localtime(&sunriseTime));
strftime(dn, 8, "%I:%M%p", localtime(&sunsetTime));  // both show sunset
```

**Right:** Use `localtime_r()` with separate destination structs:
```cpp
struct tm upTm, dnTm;
localtime_r(&sunriseTime, &upTm);
localtime_r(&sunsetTime, &dnTm);
strftime(up, 8, "%I:%M%p", &upTm);
strftime(dn, 8, "%I:%M%p", &dnTm);
```

### Vertical pixel limits for text rendering
Display clips at y=63. Glyph heights:
- size-1 = 8px, safe max y = 55
- size-2 = 16px, safe max y = 47
- size-3 = 24px, safe max y = 39

### Text width
Each character is `6 * textSize` pixels wide. Size-1 string of N chars is `6*N` pixels. Matters for right-alignment math and catching overflow before it clips off-panel. In v7 "CRYPTO & GAS" was 144px > 128px panel and got cut off — renamed to "CRYPTO/GAS" (120px).

### Heap guard
```cpp
if(!nightMode && WiFi.status()==WL_CONNECTED && ESP.getFreeHeap()>40000){
  // refresh fetches
}
```
ESP32 heap fragmentation will eventually crash JSON parsing if ungated. 40KB minimum free heap is the threshold.

### Color helpers
```cpp
dsp->color565(r, g, b)  // expects 0-255 per channel
```
RGB565 packs as `RRRRRGGGGGGBBBBB` (5/6/5 bits). Common literals:
- `0xFFFF` white
- `0xF800` bright red
- `0x07E0` bright green
- `0x001F` bright blue
- `0xFFE0` yellow
- `0x0000` black

## Rendering helpers (in the .ino)

```cpp
void cls();
void fillRect(int x, int y, int w, int h, uint16_t c);
void txt1(const char *s, int x, int y, uint16_t c); // size 1
void txt2(const char *s, int x, int y, uint16_t c); // size 2
void txt3(const char *s, int x, int y, uint16_t c); // size 3
void ctrTxt1(const char *s, int y, uint16_t c);     // centered
void ctrTxt2(const char *s, int y, uint16_t c);
void ctrTxt3(const char *s, int y, uint16_t c);
void txtOutline(const char *s, int x, int y, uint16_t c); // black outline for busy bgs
void ctrTxtOutline(const char *s, int y, uint16_t c);
String trimTo(const String &s, int maxLen);
```

## Slides currently implemented

### Day playlist
News (AP national) → Local News (Stamford CT) → Sports anim → Scores (MLB/NBA/NHL/NFL/NCAAB) → Money anim → Finance (S&P 500, BTC, gas, treasury, fear & greed) → RFR Scene (Red Fox Rd home, 4 seasonal variants with bees/Benny/leaves/snow) → RFR Weather (2 pages) → RFR Traffic → Cabin Scene (Portly Bear Lodge, 4 seasonal variants with black bear) → Cabin Weather (2 pages) → Cabin Traffic → Cabin Lake (Lake Wallenpaupack, 2 pages: water temp, tides) → Tiki Scene (volcano + lava + tiki masks) → Flights (OpenSky overhead with airline lookup from ICAO callsign) → Moon → Game of Life → Calendar/WHOOP → LIS Chart (Long Island Sound NOAA-style with buoys) → Ipswich/Cape Ann Chart (NOAA-style with buoys)

### Night playlist
Aurora → LIS Chart → Ipswich Chart → Starfield → Moon → Game of Life

## In-progress: v10 plan

These are the next priorities, in order:

### 1. Bespoke 20×20 pixel-art team logos for priority teams
- **All 32 NFL teams** (highest priority — DeWitt is a "big NFL guy")
- **All 32 NHL teams** (second priority)
- **Boston favorites:** Red Sox, Bruins, Celtics
- **NY Giants**
- Other MLB/NBA teams keep the existing colored monogram fallback in `drawTeamLogo()`
- Logos should go in a new `team_logos.h` header (v10 is the right time to split)
- Dispatcher pattern: `drawTeamLogo(ox, oy, abbr, league)` → looks up by abbr+league, falls back to `drawTeamMonogram()` for unmapped teams
- **Size decision:** 20×20 (chose this over 16×16 for better detail despite less score area)
- **Sort priority:** favorites ALWAYS first regardless of game status — Patriots/Bruins/Red Sox/Celtics surface to top (already implemented in v9 `fetchScores()` sort)
- Reference for style: Glance LED commercial product (https://www.glance-led.com/)

Scratch files from v9 attempts are in `team_module.txt` and `nfl_logos.txt` — not yet integrated but have color tables and draft NFL logo functions that can be salvaged.

### 2. S&P 500 sparkline on the Finance card
- Just S&P 500, not BTC/etc (DeWitt's choice)
- Pull intraday close prices from Yahoo Finance with `interval=15m&range=1d`
- Draw as a 60×16 line graph in the lower portion of the finance card
- Green if positive day, red if negative

### 3. Deploy WHOOP relay
- Google Apps Script URL has expired
- Token is in hand
- Need to redeploy and update `WHOOP_RELAY_URL` in `secrets.h`

### 4. Deploy Google Calendar relay
- Not yet deployed
- Calendar card currently shows "Deploy GCal relay" placeholder when `GCAL_RELAY_URL` is empty

### 5. Fill remaining `secrets.h` placeholders
- FRED API key (St. Louis Fed)
- EIA.gov API key
- any others flagged TODO in the file

## Architectural patterns we've settled on

- **No ticker** — full 128×64 cards, never split-screen ticker design
- **Static cards rate-limited to 2s redraw** to eliminate strobing
- **Animated scenes use draw-once-then-deltas** with proper background restoration including any overlapping static elements (trees, buildings)
- **Heap guard** skips fetches when free heap < 40KB
- **Per-league debug logging** in `fetchScores()` so we can diagnose ESPN data issues from serial monitor — look for `[SCORES] mlb d0: N events parsed`, `[SCORES] mlb total: N`, `[SCORES] TOTAL N`
- **Two-location architecture** — Stamford home (RFR scenes/weather/traffic) and Lake Wallenpaupack cabin (Cabin scenes/weather/traffic/lake)
- **Day/night mode** flips at 6am/10pm with reduced brightness and shorter night playlist
- **Favorites always first** in scores sort — priority team = +1000, live = +100, post = +50, pre = +10

## Debugging workflow

1. Flash via `arduino-cli upload` or Arduino IDE
2. Open serial monitor at **115200 baud**
3. Look for `[SCORES]`, `[NEWS]`, `[LOCAL NEWS]`, `[WEATHER]`, etc. prefixed lines
4. **Visual bugs:** photograph the panel, check the layout coordinates in the relevant `render*()` function
5. **Trail / leftover pixel bugs:** the cause is almost always a missing background restore in an animated scene — check the erase logic before suspecting hardware
6. **"All black" rendering bugs:** first suspect is the ESP32 flash const_cast gotcha — verify colors are hardcoded RGB565 literals, not runtime-initialized

## DeWitt's preferences

- Concise communication, big visual improvements per round
- Iterative photo feedback loop — flash, photo, diagnose, fix, reflash
- No video uploads (photo bursts work great)
- Style influence: Glance LED commercial product (https://www.glance-led.com/) — bold colored backgrounds, big bold scores, team logos as recognizable sprites, sparkline graphs for finance
- Reference NOAA charts when designing LIS and Cape Ann maps — match real geography, don't simplify too much
- Two favorite locations: Stamford CT home and Lake Wallenpaupack PA cabin — both get their own scene/weather/traffic cards

## Favorite teams (for scores priority)

Currently defined in `PRIORITY_TEAMS[]` as: `{"BOS","NYM","NYY","NYG","NYJ","NE","BRK","NYK","NYR","NJD","PHI"}` with `PRIORITY_COUNT=11`.

Note that **BOS** is shared across leagues — MLB Red Sox, NBA Celtics, NHL Bruins all use "BOS" as their ESPN abbreviation, so the priority filter catches all three automatically.

## Coding style

- Single .ino, no headers yet (planned v10+ refactor)
- Functions are `camelCase` — match existing style
- Color literals: prefer named globals `C_WHITE`, `C_LIME`, etc. for common colors; use `dsp->color565(r,g,b)` for one-off shades
- Comments explain WHY when non-obvious (esp. flash/timing/aliasing gotchas), not WHAT
- Keep brace counts balanced — a Python sanity check script validates before flashing

## Version history notable events

- **v5:** Loading screen fix, finance labels, scores tightening, RFR/Cabin weather split, calendar rebuilt
- **v6:** House/cabin colors brightened, pine trees brightened, Benny + spring bear brightened
- **v7:** `localtime_r` sunrise/sunset fix, 12-hour format, "CRYPTO/GAS" rename, calendar/WHOOP empty state
- **v8:** Bear background regen fix (tree redraw), Tiki strobing fix (draw-once-then-deltas), buoy "all black" root cause (const_cast flash bug) fixed with RGB565 literals, LIS chart redesigned to match NOAA reference, Cape Ann chart redesigned, local news (Stamford CT) added with fetchLocalNews() + renderLocalNews() + SL_LOCAL_NEWS playlist entry, football sprite replaced stick figure
- **v9:** `fetchLocalNews()` added to refresh loop (was only running at boot), scores status filter loosened (HALFTIME, END_PERIOD, DELAYED, POSTPONED, FINAL_*), per-league debug logs, JSON buffer 12288→16384, favorites-first sort, completely rewritten `renderScores()` with one-game-per-card layout + size-3 scores + 20×20 team color blocks + flashing LIVE indicator + winner gold highlight + game counter dots
