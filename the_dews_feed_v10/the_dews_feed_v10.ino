/*
 * =====================================================================
 *  THE DEWS FEED - v10  (visual refresh: unified card design system)
 *  DeWitt Hutchins | Stamford CT / Lake Wallenpaupack PA
 *
 *  HARDWARE:
 *    ESP32-DevKitC-32E + Seengreat RGB Matrix Adapter Board (E)
 *    4x Waveshare P4 64x32 HUB75 panels in 2x2 arrangement
 *    ALITOVE 5V 15A power supply
 *
 *  PANEL LAYOUT (viewed from front):
 *    [Panel 3 top-left]  [Panel 4 top-right]
 *    [Panel 1 bot-left]  [Panel 2 bot-right]
 *  Chain: Panel1 -> Panel2 -> Panel3 -> Panel4
 *  Virtual: CHAIN_TOP_RIGHT_DOWN
 *
 *  DISPLAY: 128x64 pixels total
 *    Full screen used by all cards (no ticker)
 *
 *  SEQUENCE (Day 6am-10pm):
 *    Boot splash -> News -> Sports anim -> Scores -> Money anim ->
 *    Finance -> Red Fox Road seasonal -> RFR Weather/Traffic ->
 *    Cabin seasonal -> Cabin Weather/Traffic/Lake ->
 *    Tiki volcanic -> Flights -> Moon -> Game of Life ->
 *    Calendar/WHOOP -> LIS Chart -> Ipswich Chart -> repeat
 *
 *  SEQUENCE (Night 10pm-6am):
 *    Aurora -> LIS Chart -> Ipswich Chart -> Starfield/ISS ->
 *    Moon -> Game of Life -> repeat (dim @ 18%)
 *
 *  LIBRARIES:
 *    ESP32-HUB75-MatrixPanel-I2S-DMA
 *    ESP32-VirtualMatrixPanel-I2S-DMA
 *    ArduinoJson v6.x
 *    Adafruit GFX
 *
 *  secrets.h keys:
 *    WIFI_SSID, WIFI_PASSWORD
 *    OWM_API_KEY        (openweathermap.org)
 *    NEWS_API_KEY       (newsapi.org)
 *    FRED_API_KEY       (fred.stlouisfed.org)
 *    EIA_API_KEY        (eia.gov)
 *    GMAPS_API_KEY      (Google Maps Distance Matrix)
 *    NWS_USER_AGENT     ("(display.local, email@email.com)")
 *    WHOOP_RELAY_URL    (Google Apps Script relay)
 *    GCAL_RELAY_URL     (Google Apps Script relay)
 * =====================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <ESP32-VirtualMatrixPanel-I2S-DMA.h>
#include <time.h>
#include <math.h>
#include <esp_task_wdt.h>      // hardware watchdog (esp_task_wdt_init/reset)
#include <esp_system.h>        // exposes esp_random()
#include <ArduinoOTA.h>        // over-the-air firmware updates
#include "secrets.h"

// Auto-generated bitmap team logos (NFL/NHL/MLB/NBA).
// Header is produced by tools/build_team_logos.py — until you run that
// script the file may not exist; we gate the include so the project still
// compiles in that case (drawTeamLogoBmp* will return false and the
// existing colored monogram is used as a fallback).
#if __has_include("team_logos.h")
  #include "team_logos.h"
  #define HAS_TEAM_LOGOS 1
#else
  #define HAS_TEAM_LOGOS 0
#endif

// Auto-generated bitmap airline logos (ICAO-keyed, 16×16).
// Header produced by tools/build_airline_logos.ps1.
#if __has_include("airline_logos.h")
  #include "airline_logos.h"
  #define HAS_AIRLINE_LOGOS 1
#else
  #define HAS_AIRLINE_LOGOS 0
#endif

// Auto-generated bitmap weather icons (OWM icon-code keyed, 16×16 and 24×24).
// Header produced by tools/build_weather_icons.ps1.
#if __has_include("weather_icons.h")
  #include "weather_icons.h"
  #define HAS_WEATHER_ICONS 1
#else
  #define HAS_WEATHER_ICONS 0
#endif

// Auto-generated per-slide anchor icons (Twemoji, keyed by name, 16×16 + 24×24).
// Header produced by tools/build_slide_icons.ps1.
#if __has_include("slide_icons.h")
  #include "slide_icons.h"
  #define HAS_SLIDE_ICONS 1
#else
  #define HAS_SLIDE_ICONS 0
#endif

// Auto-generated pixel-art animations (GIF frames → PROGMEM).
// Header produced by tools/build_animations.ps1 from tools/gifs/*.gif.
#if __has_include("pixel_anims.h")
  #include "pixel_anims.h"
  #define HAS_PIXEL_ANIMS 1
#else
  #define HAS_PIXEL_ANIMS 0
#endif

#define WDT_TIMEOUT_SEC 60     // loop must reset watchdog within this window
                               // 60s is enough for 6 sequential adsbdb calls
                               // + a slow ESPN/Yahoo response in the same pass

// Forward type declarations - needed for Arduino IDE prototype generation
struct NewsItem;
struct GameScore;
struct FinanceData;
struct DayForecast;
struct WeatherData;
struct TrafficData;
struct TidePred;
// Forward declarations (prevents Arduino IDE auto-prototype errors)
struct WeatherData;
struct TrafficData;
struct BuoyDef;
struct MiniAid;
struct RouteCacheEntry;
void fetchWeather(float lat, float lon, WeatherData &wx);
void fetchTraffic(float dlat, float dlon, TrafficData &td);

// =====================================================================
// PANEL CONFIG
// =====================================================================
#define PANEL_WIDTH    128
#define PANEL_HEIGHT    64
#define SAFE_H          32   // Verified safe rendering height (top panels only if bottom not working)
#define PANELS_NUMBER    4
#define PIN_R1  18
#define PIN_G1  25
#define PIN_B1   5
#define PIN_R2  17
#define PIN_G2  33
#define PIN_B2  16
#define PIN_A    4
#define PIN_B    3
#define PIN_C    0
#define PIN_D   21
#define PIN_E   32
#define PIN_CLK  2
#define PIN_LAT 19
#define PIN_OE  15

VirtualMatrixPanel  *dsp    = nullptr;
MatrixPanel_I2S_DMA *matrix = nullptr;

// =====================================================================
// COLORS (filled after display init)
// =====================================================================
uint16_t C_BLACK, C_WHITE, C_GRAY, C_DARKGRAY, C_RED, C_GREEN, C_BLUE,
         C_YELLOW, C_CYAN, C_TEAL, C_LIME, C_GOLD, C_ORANGE, C_PINK,
         C_PURPLE, C_BROWN, C_NAVY, C_AMBER, C_MAROON, C_OLIVE;

// =====================================================================
// TIMING & BRIGHTNESS
// =====================================================================
// Brightness 0-255. 75 was tripping brownouts during news-scroll heavy
// pixel writes on the ALITOVE 5V/15A supply (4 panels × ~2A typical).
// 55 keeps the picture vivid while leaving comfortable PSU headroom.
#define BRIGHTNESS_DAY    55
#define BRIGHTNESS_NIGHT  18
#define NIGHT_HOUR_START  22
#define NIGHT_HOUR_END     6

// Refresh intervals (ms)
#define REFRESH_NEWS     600000UL
#define REFRESH_SCORES    90000UL
#define REFRESH_FINANCE  300000UL
#define REFRESH_WEATHER  600000UL
#define REFRESH_TRAFFIC  300000UL
// OpenSky Network anonymous rate limit: 400 credits/day, 4 credits per
// state-vectors call → 100 calls/day max → must space out by ~15 minutes
// minimum. Earlier 45-second interval was getting throttled (HTTP 429).
#define REFRESH_FLIGHTS   600000UL    // 10 minutes
#define REFRESH_MARINE   600000UL
#define REFRESH_LAKE     300000UL
#define REFRESH_WHOOP   3600000UL
#define REFRESH_GCAL     300000UL
#define REFRESH_CRYPTO   120000UL
#define REFRESH_CONCERTS 7200000UL  // 2 hours — concert listings don't change often
#define REFRESH_TRAINS    120000UL  // 2 min — real-time Metro-North departures
#define REFRESH_ISS        60000UL  // 60 sec — ISS position (only when night card up)
#define REFRESH_TODEW     600000UL  // 10 min — ToDew task sync (KV-cached, cheap)

// =====================================================================
// LOCATIONS
// =====================================================================
#define HOME_LAT         41.0534f
#define HOME_LON        -73.5387f
#define CABIN_LAT        41.4167f
#define CABIN_LON       -75.2167f
#define HOME_ADDR        "299+Red+Fox+Rd+Stamford+CT+06903"
#define WORK_ADDR        "100+Mason+St+Greenwich+CT"
#define WORK_LAT         41.0262f
#define WORK_LON        -73.6255f
#define CABIN_ADDR       "1133+Indian+Dr+Lake+Ariel+PA+18436"
#define NWS_HOME_ZONE    "CTZ009"
#define NWS_MARINE_ZONE  "ANZ332"
#define NOAA_STATION_ID  "8467150"
#define NDBC_BUOY_ID     "44025"

// =====================================================================
// SEQUENCE ENGINE
// =====================================================================
enum SlideID {
  // Title / brand intro (always slide 0)
  SL_TITLE,
  // Transitions
  SL_SPORTS_ANIM, SL_MONEY_ANIM, SL_WORKOUT_ANIM, SL_GAMEOFLIFE,
  // Day cards
  SL_NEWS, SL_LOCAL_NEWS, SL_SCORES, SL_GOLF, SL_TENNIS, SL_FINANCE,
  SL_CONCERTS,
  SL_RFR_SCENE, SL_RFR_WEATHER, SL_RFR_TRAFFIC,
  SL_CABIN_SCENE, SL_CABIN_WEATHER, SL_CABIN_TRAFFIC, SL_CABIN_LAKE,
  SL_TIKI_SCENE, SL_TRAINS, SL_FLIGHTS, SL_MOON, SL_PIXELART,
  SL_CALENDAR, SL_WHOOP, SL_LIS_CHART, SL_IPSWICH_CHART, SL_QUOTE, SL_TODO,
  // Night cards
  SL_AURORA, SL_STARFIELD, SL_SPACE,
  SL_COUNT
};

// Firmware label shown on the title card.
#define DEWS_FEED_VERSION "v10"

struct SlideEntry {
  SlideID id;
  unsigned long dur; // ms
};

// Day playlist
const SlideEntry DAY_PLAYLIST[] = {
  { SL_TITLE,         12000 },   // longer so the icon marquee gets a full pass
  { SL_NEWS,         100000 },   // safety cap; advances early once all stories cycle
  { SL_SPORTS_ANIM,    8000 },
  { SL_SCORES,        45000 },
  { SL_GOLF,          20000 },   // self-skips unless a golf major is live
  { SL_TENNIS,        20000 },   // self-skips unless a tennis Grand Slam is live
  { SL_LOCAL_NEWS,   100000 },   // safety cap; advances early once all stories cycle
  { SL_MONEY_ANIM,     8000 },
  { SL_FINANCE,       60000 },
  { SL_CONCERTS,      30000 },
  { SL_RFR_SCENE,     20000 },
  { SL_RFR_WEATHER,   40000 },
  { SL_RFR_TRAFFIC,   10000 },
  { SL_CABIN_SCENE,   20000 },
  { SL_CABIN_WEATHER, 40000 },
  { SL_CABIN_TRAFFIC, 10000 },
  { SL_CABIN_LAKE,    10000 },
  { SL_TIKI_SCENE,    20000 },
  { SL_TRAINS,        30000 },
  { SL_FLIGHTS,       20000 },
  { SL_MOON,          20000 },
  { SL_PIXELART,      12000 },   // GIF pixel-art; self-skips if none compiled in
  { SL_GAMEOFLIFE,    10000 },
  { SL_CALENDAR,      45000 },
  { SL_TODO,          15000 },   // ToDew urgent tasks; skips fast when unconfigured
  { SL_WORKOUT_ANIM,   6000 },
  { SL_WHOOP,         20000 },
  { SL_LIS_CHART,     30000 },
  { SL_IPSWICH_CHART, 30000 },
  { SL_QUOTE,         15000 },   // split-flap board: ~3s flip + hold to read
};
const int DAY_COUNT = sizeof(DAY_PLAYLIST)/sizeof(SlideEntry);

// Night playlist
const SlideEntry NIGHT_PLAYLIST[] = {
  { SL_AURORA,        60000 },
  { SL_PIXELART,      20000 },   // pixel-art loops are great ambient night fare
  { SL_SPACE,         60000 },   // constellations / solar system / galaxy
  { SL_LIS_CHART,     45000 },
  { SL_IPSWICH_CHART, 45000 },
  { SL_STARFIELD,     30000 },
  { SL_MOON,          30000 },
  { SL_GAMEOFLIFE,    20000 },
};
const int NIGHT_COUNT = sizeof(NIGHT_PLAYLIST)/sizeof(SlideEntry);

int  playIdx       = 0;
bool nightMode     = false;
unsigned long slideStart = 0;
unsigned long lastStaticDraw = 0;  // rate-limit static card redraws
#define STATIC_REDRAW_MS 2000      // redraw static cards every 2s max

SlideID currentSlide() {
  if (nightMode) return NIGHT_PLAYLIST[playIdx % NIGHT_COUNT].id;
  return DAY_PLAYLIST[playIdx % DAY_COUNT].id;
}

unsigned long currentDur() {
  if (nightMode) return NIGHT_PLAYLIST[playIdx % NIGHT_COUNT].dur;
  return DAY_PLAYLIST[playIdx % DAY_COUNT].dur;
}

// True for any slide with continuous motion — scrolling text, walking
// sprites, particles, blinking indicators. HTTP fetches are deferred while
// one of these is on screen because a blocking request freezes the motion.
bool slideIsAnimated(SlideID s){
  switch(s){
    case SL_TITLE:        // letters pop in + pulse
    case SL_SPORTS_ANIM:  // logo card animations
    case SL_MONEY_ANIM:   // falling money
    case SL_WORKOUT_ANIM: // pulsing dumbbell
    case SL_GAMEOFLIFE:   // cellular automaton
    case SL_NEWS:         // scrolling headline
    case SL_LOCAL_NEWS:   // scrolling headline
    case SL_SCORES:       // flashing LIVE dot
    case SL_RFR_SCENE:    // Benny + bees
    case SL_CABIN_SCENE:  // bear / snow / fire
    case SL_TIKI_SCENE:   // lava pulse + torches
    case SL_AURORA:       // full-frame waves
    case SL_STARFIELD:    // twinkle + ISS
    case SL_SPACE:        // sub-view animations
    case SL_PIXELART:     // GIF frame playback
    case SL_QUOTE:        // split-flap tiles clicking to their letters
      return true;
    default:
      return false;       // static data cards — a stall is invisible
  }
}

void advanceSlide() {
  playIdx = nightMode ? (playIdx+1) % NIGHT_COUNT : (playIdx+1) % DAY_COUNT;
  slideStart = millis();
  lastStaticDraw = 0;  // force immediate redraw on new slide
  dsp->clearScreen();
  delay(40);
}

// =====================================================================
// DATA STRUCTURES
// =====================================================================

// --- News ---
#define MAX_NEWS 8
struct NewsItem { String headline; bool valid=false; };
NewsItem news[MAX_NEWS];
NewsItem localNews[MAX_NEWS];  // Stamford Advocate / local Stamford CT news
int newsCount = 0;
int localNewsCount = 0;
static int newsDispIdx = 0;
static unsigned long newsPageStart = 0;

// --- Breaking-news takeover (parity with the office Pi dashboard) ---
// A headline matching the breaking keywords interrupts whatever slide is up
// with a full-screen red card for 45s, once per headline (hash-deduped).
String        breakingHead  = "";
unsigned long breakingUntil = 0;     // millis() deadline; 0 = no takeover
bool          breakingDrawn = false;
uint32_t      seenBreakHash[12] = {0};
int           seenBreakIdx  = 0;

// --- Scores ---
#define MAX_GAMES 16
struct GameScore {
  String away, home, awayScore, homeScore, status, clock, league;
  bool valid=false, priority=false;
  // Live broadcast state (only populated when status indicates LIVE).
  // Sport-specific fields — read only when league matches.
  // ───── MLB ─────
  int8_t  mlbBalls=0, mlbStrikes=0, mlbOuts=0;
  bool    mlbOn1B=false, mlbOn2B=false, mlbOn3B=false;
  bool    mlbTopHalf=false;     // true = top of inning (arrow up)
  // ───── NFL ─────
  int8_t  nflDown=0;            // 1..4, 0 if unknown
  int8_t  nflDistance=0;        // yards to go
  String  nflPossession;        // abbr of team with ball
  // ───── NHL ─────
  int8_t  nhlPeriod=0;          // 1..3 (4=OT, 5=SO)
  bool    nhlPowerPlay=false;
  String  nhlPPTeam;            // abbr of team on PP (when applicable)
  // ───── NBA ─────
  int8_t  nbaQuarter=0;         // 1..4 (5=OT)
  String  nbaPossession;        // abbr of team with ball
};
GameScore games[MAX_GAMES];
int gameCount=0, gameDispIdx=0;

// --- Finance ---
struct FinanceData {
  float sp500=0, sp500Chg=0;
  float btcUSD=0, btcChg=0;
  float gasNat=0;       // national avg gas price
  float treasury10y=0;  // 10yr yield
  int   fearGreed=50;   // 0-100
  // Page 3: small-bank watch + short-rate environment
  float pnbk=0, pnbkChg=0;   // Patriot National Bancorp (NASDAQ: PNBK)
  float sofr=0;              // Secured Overnight Financing Rate (FRED: SOFR)
  float fedFunds=0;          // Effective Federal Funds Rate (FRED: DFF)
  // Page 4: treasury curve
  float treasury3y=0;        // 3yr yield (FRED: DGS3)
  float treasury5y=0;        // 5yr yield (FRED: DGS5)
  bool  valid=false;
};
FinanceData finance;

// S&P 500 intraday sparkline (15-min intervals, current trading day)
#define SP_SPARK_MAX 40
float sp500Spark[SP_SPARK_MAX];
int   sp500SparkLen = 0;
unsigned long lastSP500Spark = 0;

// --- Weather ---
struct DayForecast {
  String label;       // "Mon"
  float  hiF, loF;
  String icon;        // "sun","cloud","rain","snow","thunder"
};
#define MAX_FORECAST 7
struct WeatherData {
  float tempF=0, feelsF=0, windMph=0, humidity=0;
  String condition, icon;
  float sunrise=0, sunset=0;  // unix timestamps
  DayForecast forecast[MAX_FORECAST];
  int forecastCount=0;
  bool valid=false;
};
WeatherData homeWx, cabinWx;

// --- Traffic ---
struct TrafficData {
  String durationNormal;   // "22 min"
  String durationTraffic;  // "38 min"
  bool valid=false;
};
TrafficData trafficWork, trafficCabin;

// --- Marine ---
struct TidePred { String timeStr; float height=0; String type; };
struct MarineData {
  TidePred nextTide, followTide;
  float waveHt=0, waterTempF=0, windKts=0;
  bool tideValid=false, buoyValid=false;
  bool hasAlert=false; String alertType; uint16_t alertColor=0;
};
MarineData marine;

// --- Lake ---
struct LakeData {
  float waterTempF=0, levelFt=0;
  bool valid=false;
};
LakeData lake;

// --- Metro-North trains ---
#define MAX_TRAINS 5
struct TrainDep {
  char time[8];   // "6:12p"
  char dest[22];  // "Grand Central"
  char line[18];  // "New Haven"
  int  minsAway;  // minutes until departure
  bool delayed;
  bool valid;
};
TrainDep trains[MAX_TRAINS];
int trainCount=0;

// --- Flights ---
#define MAX_FLIGHTS 6
// Aircraft category derived heuristically from altitude+speed.
// 0=unknown 1=regional/prop 2=narrowbody 3=widebody/heavy
struct FlightData {
  String callsign;
  String icao24;            // unique ADS-B aircraft hex
  float lat=0, lon=0;       // position
  float altitude=0, speed=0, heading=0, vertRate=0;
  float distMi=0;           // distance from HOME
  String compass;           // "NE", "SW" etc — bearing label from HOME
  String routeFrom;         // "JFK"
  String routeTo;           // "LAX"
  uint8_t category=0;       // size class for silhouette
  bool routeKnown=false;
  bool valid=false;
};
FlightData flights[MAX_FLIGHTS];
int flightCount=0, flightDispIdx=0;

// Route lookup cache — adsbdb.com is free but we don't want to refetch
// every 45s. Cache by callsign across the whole session.
#define ROUTE_CACHE_SIZE 32
struct RouteCacheEntry {
  String callsign;
  String from, to;
  bool found=false;          // true if API replied (route may still be empty)
  bool tried=false;          // we hit the API for this one
  unsigned long lastTouch=0; // for LRU eviction
};
RouteCacheEntry routeCache[ROUTE_CACHE_SIZE];
unsigned long lastFlightRoutes=0;
#define REFRESH_FLIGHT_ROUTES 20000UL  // chase any new callsigns every 20s

// --- Buoys ---
// --- Buoys ---

// --- Moon ---
float moonPhase=0;

// --- WHOOP ---
struct WhoopData {
  float recovery=0, hrv=0, rhr=0, sleepScore=0, strain=0;
  String recoveryLabel;  // "PEAK","GOOD","OK","TIRED"
  uint16_t recoveryColor=0;
  bool valid=false;
};
WhoopData whoop;

// --- Calendar ---
#define MAX_EVENTS 5
struct CalEvent { String time, title; bool valid=false; };
CalEvent calEvents[MAX_EVENTS];
int calCount=0;

// --- Concerts (Ticketmaster Discovery API) ---
#define MAX_CONCERTS 5
struct ConcertEvent {
  String name;    // artist / event name
  String date;    // "2025-04-20"
  String time;    // "20:00:00" (may be empty for TBA)
  String venue;   // venue name
  String city;    // city name
  bool valid=false;
};
ConcertEvent concerts[MAX_CONCERTS];
int concertCount=0;

// --- NOAA Buoys ---
#define MAX_LIS_BUOYS 8
#define MAX_IPW_BUOYS 6
struct Buoy {
  String id, name;
  float  lat, lon;
  float  waveHt=0, waterTemp=0;
  bool   active=false;
  // Light character for blinking
  int    blinkOnMs=500, blinkOffMs=500;
  uint16_t color=0;
};
Buoy lisBuoys[MAX_LIS_BUOYS];
Buoy ipwBuoys[MAX_IPW_BUOYS];

// =====================================================================
// REFRESH TIMESTAMPS
// =====================================================================
unsigned long lastNews=0, lastLocalNews=0, lastScores=0, lastFinance=0;
unsigned long lastHomeWx=0, lastCabinWx=0;
unsigned long lastTrafficW=0, lastTrafficC=0;
unsigned long lastFlights=0, lastMarine=0, lastLake=0;
unsigned long lastWhoop=0, lastGcal=0, lastConcerts=0, lastTrains=0, lastISS=0;
unsigned long lastToDew=0;
unsigned long lastGolf=0, lastTennis=0;
#define REFRESH_GOLF   300000UL   // 5 min — leaderboards move slowly
#define REFRESH_TENNIS 300000UL
unsigned long lastBrightness=0;
unsigned long lastWifiRetry=0;

// =====================================================================
// PRIORITY TEAMS
// =====================================================================
const char* PRIORITY_TEAMS[] = {"BOS","NYM","NYY","NYG","NYJ","NE","BRK","NYK","NYR","NJD","PHI"};
const int   PRIORITY_COUNT   = 11;
bool isPriority(const String &t) {
  for (int i=0;i<PRIORITY_COUNT;i++) if (t==PRIORITY_TEAMS[i]) return true;
  return false;
}

// =====================================================================
// DISPLAY HELPERS
// =====================================================================
void fillRect(int x, int y, int w, int h, uint16_t c) {
  for (int r=y; r<y+h; r++) dsp->drawFastHLine(x,r,w,c);
}

void cls() { dsp->clearScreen(); }

void setTextS(int s) { dsp->setTextSize(s); dsp->setTextWrap(false); }

void txt1(const char *s, int x, int y, uint16_t c) {
  setTextS(1); dsp->setTextColor(c); dsp->setCursor(x,y); dsp->print(s);
}
void txt2(const char *s, int x, int y, uint16_t c) {
  setTextS(2); dsp->setTextColor(c); dsp->setCursor(x,y); dsp->print(s);
  setTextS(1);
}
void txt3(const char *s, int x, int y, uint16_t c) {
  setTextS(3); dsp->setTextColor(c); dsp->setCursor(x,y); dsp->print(s);
  setTextS(1);
}

void ctrTxt1(const char *s, int y, uint16_t c) {
  int x = (PANEL_WIDTH - (int)strlen(s)*6) / 2;
  if (x<0) x=0;
  txt1(s, x, y, c);
}
void ctrTxt2(const char *s, int y, uint16_t c) {
  int x = (PANEL_WIDTH - (int)strlen(s)*12) / 2;
  if (x<0) x=0;
  txt2(s, x, y, c);
}
void ctrTxt3(const char *s, int y, uint16_t c) {
  int x = (PANEL_WIDTH - (int)strlen(s)*18) / 2;
  if (x<0) x=0;
  txt3(s, x, y, c);
}

// Outlined text - draws text with black 1px outline for high contrast
// over any background. Used for scene card titles at top of sky.
void txtOutline(const char *s, int x, int y, uint16_t c) {
  setTextS(1);
  dsp->setTextColor(C_BLACK);
  // 8-direction outline
  for(int dx=-1;dx<=1;dx++) for(int dy=-1;dy<=1;dy++){
    if(dx==0&&dy==0) continue;
    dsp->setCursor(x+dx,y+dy); dsp->print(s);
  }
  dsp->setTextColor(c);
  dsp->setCursor(x,y); dsp->print(s);
}
void txtOutline2(const char *s, int x, int y, uint16_t c) {
  setTextS(2);
  dsp->setTextColor(C_BLACK);
  for(int dx=-1;dx<=1;dx++) for(int dy=-1;dy<=1;dy++){
    if(dx==0&&dy==0) continue;
    dsp->setCursor(x+dx,y+dy); dsp->print(s);
  }
  dsp->setTextColor(c);
  dsp->setCursor(x,y); dsp->print(s);
  setTextS(1);
}
void ctrTxtOutline(const char *s, int y, uint16_t c) {
  int x = (PANEL_WIDTH - (int)strlen(s)*6) / 2;
  if (x<0) x=0;
  txtOutline(s, x, y, c);
}
void ctrTxtOutline2(const char *s, int y, uint16_t c) {
  int x = (PANEL_WIDTH - (int)strlen(s)*12) / 2;
  if (x<0) x=0;
  txtOutline2(s, x, y, c);
}

String trimTo(const String &s, int n) {
  return (s.length()<=(size_t)n) ? s : s.substring(0,n);
}

// Strip all non-printable / non-ASCII bytes from s and replace common
// smart-punctuation with ASCII equivalents. NewsAPI / Yahoo / ESPN routinely
// return UTF-8 multi-byte sequences (curly "" '' em-dash —, accented chars,
// emoji). Adafruit GFX renders bytes 0x80-0xFF as IBM cp437 glyphs (boxes,
// fractions, math symbols) — the "backwards letters and numbers" we see on
// the news card. Worse, partially trimmed multi-byte sequences can also
// confuse the renderer's glyph-width math, occasionally panicking.
//
// This in-place pass guarantees every headline is pure printable ASCII.
void sanitizeAscii(String &s){
  String out;
  out.reserve(s.length());
  for(size_t i=0; i<s.length(); ){
    uint8_t c = (uint8_t)s.charAt(i);
    if(c < 0x80){
      // ASCII pass-through; map control chars to space
      out += (c >= 0x20 && c <= 0x7E) ? (char)c : ' ';
      i++;
    } else {
      // UTF-8 multi-byte. Decode the codepoint length and map common ones,
      // then skip the whole sequence so we don't split it mid-byte.
      int seqLen =
          (c & 0xE0) == 0xC0 ? 2 :
          (c & 0xF0) == 0xE0 ? 3 :
          (c & 0xF8) == 0xF0 ? 4 : 1;
      // Bounds-check: truncated sequence at end of string → skip cleanly
      if(i + (size_t)seqLen > s.length()){
        out += ' ';
        break;   // exit the whole loop, we're done
      }
      // Build the codepoint
      uint32_t cp = 0;
      if(seqLen == 2) cp = ((c & 0x1F) << 6)  | ((uint8_t)s.charAt(i+1) & 0x3F);
      else if(seqLen == 3) cp = ((c & 0x0F) << 12)
                                | (((uint8_t)s.charAt(i+1) & 0x3F) << 6)
                                | ((uint8_t)s.charAt(i+2) & 0x3F);
      // Map common punctuation; otherwise replace whole sequence with space
      switch(cp){
        case 0x2018: case 0x2019: case 0x201A: case 0x2032: out += '\''; break; // ‘ ’ ‚ ′
        case 0x201C: case 0x201D: case 0x201E: case 0x2033: out += '"';  break; // “ ” „ ″
        case 0x2013: case 0x2014: case 0x2212:              out += '-';  break; // – — −
        case 0x2026:                                        out += "..."; break; // …
        case 0x00A0:                                        out += ' ';  break; // nbsp
        case 0x00E9: case 0x00E8: case 0x00EA: case 0x00EB: out += 'e';  break; // é è ê ë
        case 0x00C9: case 0x00C8: case 0x00CA: case 0x00CB: out += 'E';  break; // É È Ê Ë
        case 0x00E1: case 0x00E0: case 0x00E2: case 0x00E4: out += 'a';  break; // á à â ä
        case 0x00C1: case 0x00C0: case 0x00C2: case 0x00C4: out += 'A';  break; // Á À Â Ä
        case 0x00ED: case 0x00EC: case 0x00EE: case 0x00EF: out += 'i';  break; // í ì î ï
        case 0x00F3: case 0x00F2: case 0x00F4: case 0x00F6: out += 'o';  break; // ó ò ô ö
        case 0x00FA: case 0x00F9: case 0x00FB: case 0x00FC: out += 'u';  break; // ú ù û ü
        case 0x00F1:                                        out += 'n';  break; // ñ
        case 0x00D1:                                        out += 'N';  break; // Ñ
        case 0x00DF:                                        out += "ss"; break; // ß
        case 0x00B0:                                        out += "deg"; break;// °
        case 0x00A9:                                        out += "(c)"; break;// ©
        case 0x00AE:                                        out += "(R)"; break;// ®
        case 0x2122:                                        out += "TM"; break; // ™
        case 0x20AC:                                        out += "EUR"; break;// €
        case 0x00A3:                                        out += "GBP"; break;// £
        case 0x00A5:                                        out += "JPY"; break;// ¥
        case 0x00A2:                                        out += 'c';  break; // ¢
        case 0x00B7: case 0x2022:                           out += '*';  break; // · •
        default:                                            out += ' ';  break;
      }
      i += seqLen;
    }
  }
  // Collapse runs of whitespace to a single space
  while(out.indexOf("  ") >= 0) out.replace("  ", " ");
  out.trim();
  s = out;
}

// Shorten Google Maps duration: "2 hours 15 mins" -> "2h 15m"
String shortDur(const String &s) {
  String r=s;
  r.replace(" hours","h"); r.replace(" hour","h");
  r.replace(" mins","m");  r.replace(" min","m");
  r.replace(" days","d");  r.replace(" day","d");
  return r;
}

uint16_t tempColor(float f) {
  if (f<25) return C_PURPLE;
  if (f<32) return C_CYAN;
  if (f<50) return C_BLUE;
  if (f<65) return dsp->color565(100,200,255);
  if (f<75) return C_LIME;
  if (f<85) return C_YELLOW;
  if (f<95) return C_ORANGE;
  return C_RED;
}

uint16_t pctColor(float p) {  // 0-100 green=good red=bad
  if (p>75) return C_LIME;
  if (p>50) return C_YELLOW;
  if (p>25) return C_ORANGE;
  return C_RED;
}

int getHour() { struct tm t; if(!getLocalTime(&t)) return 12; return t.tm_hour; }
bool isNightTime() { int h=getHour(); return (h>=NIGHT_HOUR_START||h<NIGHT_HOUR_END); }

String getSeason() {
  struct tm t; if(!getLocalTime(&t)) return "winter";
  int m = t.tm_mon+1;
  if (m>=3&&m<=5) return "spring";
  if (m>=6&&m<=8) return "summer";
  if (m>=9&&m<=11) return "fall";
  return "winter";
}

// =====================================================================
// MOON PHASE
// =====================================================================
float getMoonPhase() {
  struct tm ti; if(!getLocalTime(&ti)) return 0;
  time_t t = mktime(&ti);
  double days = (t - 1738150560L) / 86400.0;
  double p = fmod(days, 29.53059) / 29.53059;
  if (p<0) p+=1.0;
  return (float)p;
}
const char* moonPhaseName(float p) {
  // Full proper phase names — split on the space when rendered to fit 60px col
  if (p<0.04||p>=0.96) return "New Moon";
  if (p<0.22) return "Waxing Crescent";
  if (p<0.28) return "First Quarter";
  if (p<0.47) return "Waxing Gibbous";
  if (p<0.53) return "Full Moon";
  if (p<0.72) return "Waning Gibbous";
  if (p<0.78) return "Last Quarter";
  return "Waning Crescent";
}
const char* moonPhaseShort(float p) {
  if (p<0.04||p>=0.96) return "NEW";
  if (p<0.22) return "WxC";
  if (p<0.28) return "1st";
  if (p<0.47) return "WxG";
  if (p<0.53) return "FUL";
  if (p<0.72) return "WnG";
  if (p<0.78) return "3rd";
  return "WnC";
}

// =====================================================================
// WIFI & HTTP
// =====================================================================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] connecting");
  int t=0;
  while (WiFi.status()!=WL_CONNECTED && t++<40) { delay(500); Serial.print("."); }
  Serial.println(WiFi.status()==WL_CONNECTED ? " OK" : " FAIL");
}

void ensureWiFi() {
  if (WiFi.status()==WL_CONNECTED) return;
  if (millis()-lastWifiRetry < 30000) return;
  lastWifiRetry = millis();
  WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

// Guarded WDT reset. Boot path calls feedWdt() during splash/fetches
// BEFORE setup() registers the loop task with the watchdog. Calling
// esp_task_wdt_reset() before the task is registered produces hundreds of
// "task not found" lines that drown out real diagnostics. The flag is
// flipped once setup() completes registration.
static volatile bool wdtArmed = false;
static inline void feedWdt(){
  if(wdtArmed) esp_task_wdt_reset();
}

String httpGet(const String &url, int ms=12000) {
  feedWdt();
  HTTPClient h; h.begin(url); h.setTimeout(ms);
  h.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int code = h.GET(); String body="";
  if (code==HTTP_CODE_OK) body=h.getString();
  else Serial.printf("[HTTP] %d %s\n", code, url.substring(0,60).c_str());
  h.end();
  feedWdt();
  return body;
}

String httpGetUA(const String &url, int ms=12000) {
  feedWdt();
  HTTPClient h; h.begin(url); h.setTimeout(ms);
  h.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  h.addHeader("User-Agent", NWS_USER_AGENT);
  int code = h.GET(); String body="";
  if (code==HTTP_CODE_OK) body=h.getString();
  h.end();
  feedWdt();
  return body;
}

// Browser-like UA needed for Yahoo Finance + ESPN (reject default ESP32 UA)
// Following redirects is essential — ESPN sometimes 301s scoreboard URLs to
// site.web.api.espn.com, and Ticketmaster app.* hops through region domains.
String httpGetBrowser(const String &url, int ms=12000) {
  feedWdt();
  HTTPClient h; h.begin(url); h.setTimeout(ms);
  h.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  h.addHeader("User-Agent","Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
  h.addHeader("Accept","application/json,text/plain,*/*");
  h.addHeader("Accept-Language","en-US,en;q=0.9");
  int code = h.GET(); String body="";
  if (code==HTTP_CODE_OK) body=h.getString();
  else Serial.printf("[HTTP] browser %d %s\n", code, url.substring(0,60).c_str());
  h.end();
  feedWdt();
  return body;
}

void syncTime() {
  // POSIX TZ string with explicit US DST rules (2nd Sun Mar → 1st Sun Nov).
  // The simpler configTime(-5*3600, 3600, ...) does NOT auto-toggle DST by
  // date — it just statically applies the daylight offset. Using
  // configTzTime with the rules string makes localtime() correct year-round.
  configTzTime("EST5EDT,M3.2.0/2,M11.1.0/2", "pool.ntp.org", "time.nist.gov");
  struct tm ti; int t=0;
  while (!getLocalTime(&ti) && t++<15) delay(500);
  Serial.printf("[TIME] %02d:%02d (DST=%d)\n", ti.tm_hour, ti.tm_min, ti.tm_isdst);
}

// =====================================================================
// DATA FETCHERS
// =====================================================================

// --- News ---
// ── RSS title extractor — shared by national + local news ──
// Scans <item><title>…</title> pairs out of a Google News RSS body with
// plain string ops (no XML lib). Handles CDATA-wrapped titles. Strips the
// trailing " - Source" suffix Google appends, decodes the common HTML
// entities, then runs the existing sanitizeAscii cleanup.
int parseRssTitles(const String &body, NewsItem *items, int maxItems){
  int count = 0;
  int pos = body.indexOf("<item>");
  while(pos >= 0 && count < maxItems){
    int t0 = body.indexOf("<title>", pos);
    if(t0 < 0) break;
    t0 += 7;
    int t1;
    if(body.startsWith("<![CDATA[", t0)){
      t0 += 9;
      t1 = body.indexOf("]]>", t0);
    } else {
      t1 = body.indexOf("</title>", t0);
    }
    if(t1 < 0) break;
    String t = body.substring(t0, t1);
    // Strip " - Source" suffix
    int dash = t.lastIndexOf(" - ");
    if(dash > 10) t = t.substring(0, dash);
    // Minimal HTML entity decode before ASCII sanitize
    t.replace("&amp;",  "&");
    t.replace("&#39;",  "'");
    t.replace("&apos;", "'");
    t.replace("&quot;", "\"");
    t.replace("&lt;",   "<");
    t.replace("&gt;",   ">");
    t.trim();
    sanitizeAscii(t);
    if(t.length() >= 8){
      if(t.length() > 120) t = t.substring(0, 120);
      items[count].headline = t;
      items[count].valid = true;
      count++;
    }
    pos = body.indexOf("<item>", t1);
  }
  return count;
}

uint32_t breakHash(const String &s){          // FNV-1a — cheap dedupe key
  uint32_t h=2166136261u;
  for(size_t i=0;i<s.length();i++){ h^=(uint8_t)s[i]; h*=16777619u; }
  return h;
}
bool isBreakingHeadline(const String &t){
  String l=t; l.toLowerCase();
  return l.indexOf("breaking")>=0 || l.indexOf("just in")>=0 ||
         l.indexOf("urgent")>=0   || l.indexOf("live updates")>=0 ||
         l.indexOf("developing:")>=0;
}
void checkBreakingNews(){
  for(int i=0;i<newsCount;i++){
    if(!news[i].valid || !isBreakingHeadline(news[i].headline)) continue;
    uint32_t h=breakHash(news[i].headline);
    bool seen=false;
    for(int j=0;j<12;j++) if(seenBreakHash[j]==h){ seen=true; break; }
    if(seen) continue;
    seenBreakHash[seenBreakIdx]=h; seenBreakIdx=(seenBreakIdx+1)%12;
    breakingHead=news[i].headline;
    breakingUntil=millis()+45000UL;
    breakingDrawn=false;
    Serial.printf("[BREAKING] %s\n", breakingHead.c_str());
    break;                             // one takeover at a time
  }
}

void fetchNews() {
  newsCount=0;
  // PRIMARY: Google News RSS — keyless and unlimited. NewsAPI's free tier
  // caps at 100 requests/day; our 10-min refresh blew through that by
  // mid-morning, which is why the card stopped loading. RSS has no cap.
  {
    String body=httpGetBrowser("https://news.google.com/rss?hl=en-US&gl=US&ceid=US:en", 15000);
    if(!body.isEmpty()){
      Serial.printf("[NEWS] RSS %u bytes, heap=%u\n",
        (unsigned)body.length(), (unsigned)ESP.getFreeHeap());
      newsCount = parseRssTitles(body, news, MAX_NEWS);
    }
  }
  // FALLBACK: NewsAPI (key-limited) only if RSS produced nothing.
  if(newsCount == 0){
    String url="https://newsapi.org/v2/top-headlines?country=us&pageSize=5&apiKey=";
    url+=NEWS_API_KEY;
    String body=httpGetBrowser(url);
    if(!body.isEmpty()){
      DynamicJsonDocument doc(10240);
      if(!deserializeJson(doc,body)){
        int idx=0;
        for (JsonObject a : doc["articles"].as<JsonArray>()) {
          if (idx>=MAX_NEWS) break;
          String t=a["title"].as<String>(); t.trim();
          if (t.length()<8||t.indexOf("[Removed]")>=0) continue;
          int dash=t.lastIndexOf(" - "); if(dash>10) t=t.substring(0,dash);
          sanitizeAscii(t);
          if (t.length()<8) continue;
          if (t.length()>120) t = t.substring(0,120);
          news[idx].headline=t; news[idx].valid=true; idx++;
        }
        newsCount=idx;
      }
    }
  }
  Serial.printf("[NEWS] %d headlines\n",newsCount);
  checkBreakingNews();
}

// --- Local News (Stamford CT) ---
void fetchLocalNews() {
  localNewsCount=0;
  // PRIMARY: Google News RSS search for Stamford CT — keyless, unlimited,
  // and actually richer than NewsAPI's everything endpoint for local sources
  // (pulls Stamford Advocate, Patch, CT Insider, etc.).
  {
    String body=httpGetBrowser(
      "https://news.google.com/rss/search?q=%22Stamford%22%20Connecticut&hl=en-US&gl=US&ceid=US:en", 15000);
    if(!body.isEmpty()){
      Serial.printf("[LNEWS] RSS %u bytes, heap=%u\n",
        (unsigned)body.length(), (unsigned)ESP.getFreeHeap());
      localNewsCount = parseRssTitles(body, localNews, MAX_NEWS);
    }
  }
  // FALLBACK: NewsAPI everything endpoint
  if(localNewsCount == 0){
    String url="https://newsapi.org/v2/everything?q=%22Stamford%20CT%22%20OR%20%22Stamford%20Advocate%22&pageSize=5&sortBy=publishedAt&language=en&apiKey=";
    url+=NEWS_API_KEY;
    String body=httpGetBrowser(url);
    if(!body.isEmpty()){
      DynamicJsonDocument doc(10240);
      if(!deserializeJson(doc,body)){
        int idx=0;
        for (JsonObject a : doc["articles"].as<JsonArray>()) {
          if (idx>=MAX_NEWS) break;
          String t=a["title"].as<String>(); t.trim();
          if (t.length()<8||t.indexOf("[Removed]")>=0) continue;
          int dash=t.lastIndexOf(" - "); if(dash>10) t=t.substring(0,dash);
          sanitizeAscii(t);
          if (t.length()<8) continue;
          if (t.length()>120) t = t.substring(0,120);
          localNews[idx].headline=t; localNews[idx].valid=true; idx++;
        }
        localNewsCount=idx;
      }
    }
  }
  Serial.printf("[LOCAL NEWS] %d headlines\n",localNewsCount);
}

// ────────────────────────────────────────────────────────────────────────
// Team name → ESPN-style abbreviation. TheSportsDB returns full names like
// "Boston Red Sox"; the rest of the dashboard (favorites filter, color
// lookups, monogram fallback) expects 2–3 char codes like "BOS".
// First-match wins — if a name appears in multiple leagues the first hit
// is used (BOS = Bruins/Celtics/Red Sox all share "BOS" by design).
// ────────────────────────────────────────────────────────────────────────
struct TeamAbbrEntry { const char* name; const char* abbr; };
const TeamAbbrEntry TEAM_NAME_TO_ABBR[] = {
  // ── MLB ──
  {"Boston Red Sox","BOS"},{"New York Yankees","NYY"},{"New York Mets","NYM"},
  {"Los Angeles Dodgers","LAD"},{"San Francisco Giants","SF"},{"Chicago Cubs","CHC"},
  {"Chicago White Sox","CWS"},{"Atlanta Braves","ATL"},{"Philadelphia Phillies","PHI"},
  {"Tampa Bay Rays","TB"},{"Toronto Blue Jays","TOR"},{"Baltimore Orioles","BAL"},
  {"Detroit Tigers","DET"},{"Cleveland Guardians","CLE"},{"Minnesota Twins","MIN"},
  {"Kansas City Royals","KC"},{"Houston Astros","HOU"},{"Texas Rangers","TEX"},
  {"Oakland Athletics","OAK"},{"Athletics","OAK"},{"Seattle Mariners","SEA"},
  {"Los Angeles Angels","LAA"},{"Miami Marlins","MIA"},{"Washington Nationals","WSH"},
  {"Pittsburgh Pirates","PIT"},{"Cincinnati Reds","CIN"},{"Milwaukee Brewers","MIL"},
  {"St. Louis Cardinals","STL"},{"St Louis Cardinals","STL"},
  {"Colorado Rockies","COL"},{"Arizona Diamondbacks","ARI"},{"San Diego Padres","SD"},
  // ── NBA ──
  {"Boston Celtics","BOS"},{"New York Knicks","NYK"},{"Brooklyn Nets","BKN"},
  {"Philadelphia 76ers","PHI"},{"Toronto Raptors","TOR"},{"Miami Heat","MIA"},
  {"Atlanta Hawks","ATL"},{"Charlotte Hornets","CHA"},{"Orlando Magic","ORL"},
  {"Washington Wizards","WAS"},{"Cleveland Cavaliers","CLE"},{"Detroit Pistons","DET"},
  {"Indiana Pacers","IND"},{"Milwaukee Bucks","MIL"},{"Chicago Bulls","CHI"},
  {"Denver Nuggets","DEN"},{"Minnesota Timberwolves","MIN"},{"Oklahoma City Thunder","OKC"},
  {"Portland Trail Blazers","POR"},{"Utah Jazz","UTA"},{"Golden State Warriors","GSW"},
  {"Los Angeles Lakers","LAL"},{"LA Clippers","LAC"},{"Los Angeles Clippers","LAC"},
  {"Sacramento Kings","SAC"},{"Phoenix Suns","PHX"},{"Dallas Mavericks","DAL"},
  {"Houston Rockets","HOU"},{"Memphis Grizzlies","MEM"},{"New Orleans Pelicans","NO"},
  {"San Antonio Spurs","SA"},
  // ── NHL ──
  {"Boston Bruins","BOS"},{"New York Rangers","NYR"},{"New York Islanders","NYI"},
  {"New Jersey Devils","NJ"},{"Philadelphia Flyers","PHI"},{"Pittsburgh Penguins","PIT"},
  {"Washington Capitals","WSH"},{"Carolina Hurricanes","CAR"},{"Columbus Blue Jackets","CBJ"},
  {"Detroit Red Wings","DET"},{"Buffalo Sabres","BUF"},{"Tampa Bay Lightning","TBL"},
  {"Florida Panthers","FLA"},{"Toronto Maple Leafs","TOR"},{"Montreal Canadiens","MTL"},
  {"Ottawa Senators","OTT"},{"Chicago Blackhawks","CHI"},{"Minnesota Wild","MIN"},
  {"Winnipeg Jets","WPG"},{"Colorado Avalanche","COL"},{"St. Louis Blues","STL"},
  {"St Louis Blues","STL"},{"Dallas Stars","DAL"},{"Nashville Predators","NSH"},
  {"Edmonton Oilers","EDM"},{"Calgary Flames","CGY"},{"Vancouver Canucks","VAN"},
  {"Vegas Golden Knights","VGK"},{"Seattle Kraken","SEA"},{"Anaheim Ducks","ANA"},
  {"Los Angeles Kings","LAK"},{"San Jose Sharks","SJ"},{"Arizona Coyotes","ARI"},
  {"Utah Hockey Club","UTA"},
  // ── NFL ──
  {"New England Patriots","NE"},{"Buffalo Bills","BUF"},{"New York Jets","NYJ"},
  {"Miami Dolphins","MIA"},{"Baltimore Ravens","BAL"},{"Cincinnati Bengals","CIN"},
  {"Cleveland Browns","CLE"},{"Pittsburgh Steelers","PIT"},{"Houston Texans","HOU"},
  {"Indianapolis Colts","IND"},{"Jacksonville Jaguars","JAX"},{"Tennessee Titans","TEN"},
  {"Denver Broncos","DEN"},{"Kansas City Chiefs","KC"},{"Las Vegas Raiders","LV"},
  {"Los Angeles Chargers","LAC"},{"Dallas Cowboys","DAL"},{"New York Giants","NYG"},
  {"Philadelphia Eagles","PHI"},{"Washington Commanders","WSH"},{"Chicago Bears","CHI"},
  {"Detroit Lions","DET"},{"Green Bay Packers","GB"},{"Minnesota Vikings","MIN"},
  {"Atlanta Falcons","ATL"},{"Carolina Panthers","CAR"},{"New Orleans Saints","NO"},
  {"Tampa Bay Buccaneers","TB"},{"Arizona Cardinals","ARI"},{"Los Angeles Rams","LAR"},
  {"San Francisco 49ers","SF"},{"Seattle Seahawks","SEA"},
};
const int TEAM_NAME_COUNT = sizeof(TEAM_NAME_TO_ABBR)/sizeof(TEAM_NAME_TO_ABBR[0]);

// Resolve a full team name to an abbreviation. Falls back to first 3 chars
// uppercase if no mapping exists (covers minor leagues, new franchises).
String teamNameToAbbr(const String &name){
  for(int i=0; i<TEAM_NAME_COUNT; i++){
    if(name == TEAM_NAME_TO_ABBR[i].name) return String(TEAM_NAME_TO_ABBR[i].abbr);
  }
  String fb = name.substring(0, min((int)name.length(), 3));
  fb.toUpperCase();
  return fb;
}

// --- Scores (TheSportsDB) ---
// Switched from ESPN's site.api.espn.com (which intermittently blocks ESP32
// User-Agents and CDN-routes some markets through region-locked endpoints).
// TheSportsDB is free, no API key, returns clean JSON, covers all 5 leagues.
//
//   GET /api/v1/json/3/eventsday.php?d=YYYY-MM-DD&l=<LEAGUE_ID>
//
// Free tier key "3" is the documented public test key.
void fetchScores() {
  gameCount=0;
  // League IDs from thesportsdb.com/api/v1/json/3/all_leagues.php
  struct LeagueDef { const char* tag; const char* id; };
  const LeagueDef leagues[] = {
    { "MLB",  "4424" },   // April → in-season
    { "NBA",  "4387" },   // April → playoffs
    { "NHL",  "4380" },   // April → playoffs
    { "NFL",  "4391" },   // April → off-season (will be empty)
    { "NCB",  "4607" },   // April → off-season (will be empty)
  };

  // Date strings — use localtime_r for safe yesterday/tomorrow calc
  struct tm ti; getLocalTime(&ti);
  char today[11], yest[11], tmw[11];
  strftime(today, 11, "%Y-%m-%d", &ti);
  time_t raw = mktime(&ti) - 86400;
  struct tm yt; localtime_r(&raw, &yt);
  strftime(yest, 11, "%Y-%m-%d", &yt);
  time_t rawT = mktime(&ti) + 86400;
  struct tm tw; localtime_r(&rawT, &tw);
  strftime(tmw, 11, "%Y-%m-%d", &tw);

  for(int i=0; i<5; i++){
    int leagueGames = 0;
    // d=0 today, d=1 yesterday (late finals), d=2 tomorrow (upcoming)
    for(int d=0; d<3; d++){
      esp_task_wdt_reset();
      String url = "https://www.thesportsdb.com/api/v1/json/3/eventsday.php?d=";
      url += (d==0 ? today : d==1 ? yest : tmw);
      url += "&l=";
      url += leagues[i].id;

      String body = httpGet(url, 10000);
      if(body.isEmpty()){
        Serial.printf("[SCORES] %s d%d: HTTP empty\n", leagues[i].tag, d);
        delay(60); continue;
      }
      Serial.printf("[SCORES] %s d%d: got %d bytes\n", leagues[i].tag, d, body.length());

      // TheSportsDB returns "events: null" when no games — handle gracefully
      if(body.indexOf("\"events\":null") >= 0){
        Serial.printf("[SCORES] %s d%d: no games\n", leagues[i].tag, d);
        delay(60); continue;
      }

      // Filter — keeps doc small even though raw response can be 30KB+
      StaticJsonDocument<512> filter;
      filter["events"][0]["strHomeTeam"]    = true;
      filter["events"][0]["strAwayTeam"]    = true;
      filter["events"][0]["intHomeScore"]   = true;
      filter["events"][0]["intAwayScore"]   = true;
      filter["events"][0]["strStatus"]      = true;
      filter["events"][0]["strProgress"]    = true;
      filter["events"][0]["strTime"]        = true;
      filter["events"][0]["strPostponed"]   = true;

      DynamicJsonDocument doc(8192);
      DeserializationError je = deserializeJson(doc, body,
        DeserializationOption::Filter(filter),
        DeserializationOption::NestingLimit(20));
      if(je){
        Serial.printf("[SCORES] %s d%d: parse err %s\n", leagues[i].tag, d, je.c_str());
        delay(60); continue;
      }

      JsonArray evArr = doc["events"].as<JsonArray>();
      int evCnt = evArr.size();

      for(JsonObject ev : evArr){
        if(gameCount >= MAX_GAMES) break;
        GameScore g;
        String homeFull = ev["strHomeTeam"].as<String>();
        String awayFull = ev["strAwayTeam"].as<String>();
        if(homeFull.length()==0 || awayFull.length()==0) continue;

        g.home = teamNameToAbbr(homeFull);
        g.away = teamNameToAbbr(awayFull);

        // Scores: "null" or empty when game hasn't started → "0"
        String hs = ev["intHomeScore"].as<String>();
        String as = ev["intAwayScore"].as<String>();
        if(hs == "null" || hs.length()==0) hs = "0";
        if(as == "null" || as.length()==0) as = "0";
        g.homeScore = hs;
        g.awayScore = as;

        // Status mapping
        String st  = ev["strStatus"].as<String>();
        String pp  = ev["strPostponed"].as<String>();
        String pgs = ev["strProgress"].as<String>();   // e.g. "Top 7th", "2nd Q"

        if(pp == "yes"){
          g.status = "post"; g.clock = "PPD";
        } else if(st.indexOf("Finished") >= 0 || st == "FT" || st.indexOf("Final") >= 0){
          g.status = "post"; g.clock = "FINAL";
        } else if(st.indexOf("Cancelled") >= 0 || st.indexOf("Canceled") >= 0){
          g.status = "post"; g.clock = "CXL";
        } else if(st.indexOf("Play") >= 0 || st.indexOf("Live") >= 0
               || st.indexOf("Progress") >= 0 || st.indexOf("Half") >= 0){
          g.status = "in";
          g.clock = pgs.length() > 0 ? trimTo(pgs, 10) : String("LIVE");
        } else {
          // "Not Started" / null / scheduled → use start time HH:MM.
          // BUT: TheSportsDB lags ~10-15 min on flipping games to "In Play".
          // If the scheduled start has passed AND there's a non-zero score,
          // treat the game as live to avoid showing a "7:10p start" badge
          // on a game that's actually in the middle of the 3rd inning.
          String t = ev["strTime"].as<String>();
          bool overrideLive = false;
          if(t.length() >= 5 && (d == 0)){   // only for today's games
            int hh = t.substring(0,2).toInt();
            int mm = t.substring(3,5).toInt();
            int gameMin = hh*60 + mm;
            int nowMin  = ti.tm_hour*60 + ti.tm_min;
            int sinceStart = nowMin - gameMin;
            int homeScoreInt = hs.toInt();
            int awayScoreInt = as.toInt();
            // Game was scheduled to start more than 5 minutes ago AND any
            // points have been put on the board → almost certainly live
            if(sinceStart > 5 && (homeScoreInt > 0 || awayScoreInt > 0)){
              overrideLive = true;
            }
          }
          if(overrideLive){
            g.status = "in"; g.clock = "LIVE";
          } else {
            g.status = "pre";
            if(t.length() >= 5){
              int hh = t.substring(0,2).toInt();
              int mm = t.substring(3,5).toInt();
              int h12 = hh % 12; if(h12==0) h12 = 12;
              char ab = hh < 12 ? 'a' : 'p';
              char buf[8]; snprintf(buf, 8, "%d:%02d%c", h12, mm, ab);
              g.clock = String(buf);
            } else {
              g.clock = "TBD";
            }
            // Tomorrow's games get a "Tm" prefix so upcoming is obvious
            if(d == 2) g.clock = "Tm " + g.clock;
          }
        }

        g.league = String(leagues[i].tag);
        g.priority = isPriority(g.home) || isPriority(g.away);
        g.valid = true;

        // Dedupe across day pages (today vs yesterday boundaries)
        bool dup = false;
        for(int x=0; x<gameCount; x++){
          if(games[x].home == g.home && games[x].away == g.away){ dup = true; break; }
        }
        if(!dup){ games[gameCount++] = g; leagueGames++; }
      }
      Serial.printf("[SCORES] %s d%d: %d events parsed\n", leagues[i].tag, d, evCnt);
      delay(60);
    }
    Serial.printf("[SCORES] %s total: %d\n", leagues[i].tag, leagueGames);
  }

  // FIFA World Cup — fetched from ESPN (TheSportsDB's WC coverage is spotty).
  // Appended into the same games[] array so WC matches rotate in with the
  // team scores. Only runs while the tournament is in season.
  fetchWorldCup();

  // Sort: favorites > live > priority > others
  for(int i=0;i<gameCount-1;i++)
    for(int j=0;j<gameCount-1-i;j++){
      // Score each game: priority team = 1000, live = 100, post = 50, pre = 10
      auto rank=[&](GameScore &g){
        int r=0;
        if(g.priority) r+=1000;
        if(g.status=="in") r+=100;
        else if(g.status=="post") r+=50;
        else r+=10;
        return r;
      };
      if(rank(games[j+1])>rank(games[j])){
        GameScore tmp=games[j];games[j]=games[j+1];games[j+1]=tmp;
      }
    }
  Serial.printf("[SCORES] TOTAL %d\n",gameCount);

  // Augment LIVE games with broadcast-state fields from ESPN's free scoreboard
  // endpoint. We only hit ESPN if at least one game is currently in progress.
  augmentLiveSituations();
}

// Pull sport-specific live broadcast state (bases/BSO for MLB, down/distance
// for NFL, period/PP for NHL, quarter for NBA) from ESPN's free scoreboard
// JSON and merge it into our existing games[] array by matching team
// abbreviations. Called from fetchScores() after the bulk TheSportsDB fetch.
//
// ESPN's `situation` block is rich:
//   MLB: balls, strikes, outs, onFirst/onSecond/onThird
//   NFL: down, distance, possession (team id), shortDownDistanceText
//   NHL/NBA: lastPlay.team for possession; period from status block
void augmentLiveSituations() {
  bool hasLive[4] = { false, false, false, false };  // MLB, NBA, NHL, NFL
  for(int i = 0; i < gameCount; i++){
    if(games[i].status != "in") continue;
    if(games[i].league == "MLB") hasLive[0] = true;
    else if(games[i].league == "NBA") hasLive[1] = true;
    else if(games[i].league == "NHL") hasLive[2] = true;
    else if(games[i].league == "NFL") hasLive[3] = true;
  }
  if(!hasLive[0] && !hasLive[1] && !hasLive[2] && !hasLive[3]) return;

  struct ESPNLeague { const char* tag; const char* path; };
  const ESPNLeague eps[4] = {
    { "MLB", "baseball/mlb" },
    { "NBA", "basketball/nba" },
    { "NHL", "hockey/nhl" },
    { "NFL", "football/nfl" },
  };

  for(int li = 0; li < 4; li++){
    if(!hasLive[li]) continue;
    esp_task_wdt_reset();
    String url = "https://site.api.espn.com/apis/site/v2/sports/";
    url += eps[li].path;
    url += "/scoreboard";

    String body = httpGet(url, 12000);
    if(body.isEmpty()){
      Serial.printf("[LIVE] %s ESPN fetch empty\n", eps[li].tag);
      continue;
    }

    // Filter to only the situation/status/competitors fields we need.
    StaticJsonDocument<1024> filter;
    JsonObject f = filter["events"][0]["competitions"][0].to<JsonObject>();
    f["status"]["type"]["state"]  = true;
    f["status"]["type"]["detail"] = true;
    f["status"]["period"]         = true;
    f["status"]["displayClock"]   = true;
    f["competitors"][0]["homeAway"]              = true;
    f["competitors"][0]["team"]["abbreviation"]  = true;
    f["situation"]["balls"]                = true;
    f["situation"]["strikes"]              = true;
    f["situation"]["outs"]                 = true;
    f["situation"]["onFirst"]              = true;
    f["situation"]["onSecond"]             = true;
    f["situation"]["onThird"]              = true;
    f["situation"]["down"]                 = true;
    f["situation"]["distance"]             = true;
    f["situation"]["shortDownDistanceText"]= true;
    f["situation"]["possession"]           = true;
    f["situation"]["possessionText"]       = true;

    DynamicJsonDocument doc(16384);
    DeserializationError je = deserializeJson(doc, body,
      DeserializationOption::Filter(filter),
      DeserializationOption::NestingLimit(20));
    if(je){
      Serial.printf("[LIVE] %s parse err %s\n", eps[li].tag, je.c_str());
      continue;
    }

    JsonArray evArr = doc["events"].as<JsonArray>();
    for(JsonObject ev : evArr){
      JsonArray comps = ev["competitions"].as<JsonArray>();
      if(comps.size() == 0) continue;
      JsonObject comp = comps[0];
      JsonObject status = comp["status"];
      String state = status["type"]["state"].as<String>();
      if(state != "in") continue;

      // Map competitors to home/away abbreviations
      String homeAbbr, awayAbbr;
      for(JsonObject c : comp["competitors"].as<JsonArray>()){
        String ha = c["homeAway"].as<String>();
        String abbr = c["team"]["abbreviation"].as<String>();
        if(ha == "home") homeAbbr = abbr;
        else if(ha == "away") awayAbbr = abbr;
      }
      if(homeAbbr.length() == 0 || awayAbbr.length() == 0) continue;

      // Match into our games[] by league + both team abbreviations.
      for(int gi = 0; gi < gameCount; gi++){
        GameScore &g = games[gi];
        if(g.league != eps[li].tag) continue;
        if(g.home != homeAbbr || g.away != awayAbbr) continue;
        // MATCH — populate broadcast state
        JsonObject situ = comp["situation"].as<JsonObject>();
        int period = status["period"] | 0;
        String detail = status["type"]["detail"].as<String>();
        String tagStr = String(eps[li].tag);

        if(tagStr == "MLB"){
          g.mlbBalls   = (int8_t)(situ["balls"]   | 0);
          g.mlbStrikes = (int8_t)(situ["strikes"] | 0);
          g.mlbOuts    = (int8_t)(situ["outs"]    | 0);
          g.mlbOn1B    = situ["onFirst"]  | false;
          g.mlbOn2B    = situ["onSecond"] | false;
          g.mlbOn3B    = situ["onThird"]  | false;
          g.mlbTopHalf = (detail.indexOf("Top") >= 0)
                       || (detail.indexOf("Beginning") >= 0);
        } else if(tagStr == "NFL"){
          g.nflDown     = (int8_t)(situ["down"]     | 0);
          g.nflDistance = (int8_t)(situ["distance"] | 0);
          // possessionText is human-readable team name; we prefer abbr from
          // matching the possession team-id against our two competitors,
          // but ESPN often gives the team-id in situation.possession.
          // Fallback to shortDownDistanceText for display if needed.
        } else if(tagStr == "NHL"){
          g.nhlPeriod = (int8_t)period;
          // ESPN doesn't expose a clean "powerPlay" boolean on the free
          // endpoint; leave nhlPowerPlay default false. Period + clock from
          // status block are sufficient for the basic widget.
        } else if(tagStr == "NBA"){
          g.nbaQuarter = (int8_t)period;
        }
        Serial.printf("[LIVE] %s %s@%s populated\n",
                      eps[li].tag, awayAbbr.c_str(), homeAbbr.c_str());
        break;
      }
    }
    delay(80);
  }
}

// FIFA World Cup — parse ESPN's soccer/fifa.world scoreboard into games[].
// Each match becomes a GameScore with league "WC". Country abbreviations
// (USA, BRA, ARG…) act as the 3-char codes the scores card already expects.
// USA matches are flagged priority so they surface to the top.
void fetchWorldCup() {
  esp_task_wdt_reset();
  String body = httpGet("https://site.api.espn.com/apis/site/v2/sports/soccer/fifa.world/scoreboard", 12000);
  if(body.isEmpty()){ Serial.println("[WC] empty"); return; }

  // Filter to the few fields we need — full WC scoreboard can be 60KB+.
  StaticJsonDocument<768> filter;
  JsonObject f = filter["events"][0]["competitions"][0].to<JsonObject>();
  f["status"]["type"]["state"]  = true;
  f["status"]["type"]["shortDetail"] = true;
  f["competitors"][0]["homeAway"]             = true;
  f["competitors"][0]["score"]                = true;
  f["competitors"][0]["team"]["abbreviation"] = true;
  filter["events"][0]["date"] = true;

  DynamicJsonDocument doc(16384);
  if(deserializeJson(doc, body, DeserializationOption::Filter(filter),
                     DeserializationOption::NestingLimit(20))){
    Serial.println("[WC] parse err"); return;
  }

  int added = 0;
  for(JsonObject ev : doc["events"].as<JsonArray>()){
    if(gameCount >= MAX_GAMES) break;
    JsonArray comps = ev["competitions"].as<JsonArray>();
    if(comps.size()==0) continue;
    JsonObject comp = comps[0];
    GameScore g;
    for(JsonObject c : comp["competitors"].as<JsonArray>()){
      String ha = c["homeAway"].as<String>();
      String abbr = c["team"]["abbreviation"].as<String>();
      String sc = c["score"].as<String>();
      if(ha=="home"){ g.home=abbr; g.homeScore = sc.length()?sc:"0"; }
      else          { g.away=abbr; g.awayScore = sc.length()?sc:"0"; }
    }
    if(g.home.length()==0 || g.away.length()==0) continue;

    String state = comp["status"]["type"]["state"].as<String>();
    String detail= comp["status"]["type"]["shortDetail"].as<String>();
    if(state=="in"){ g.status="in"; g.clock = detail.length()?trimTo(detail,10):String("LIVE"); }
    else if(state=="post"){ g.status="post"; g.clock="FT"; }
    else {
      g.status="pre";
      // Pre-game: ESPN date is ISO UTC; just show a short "WC" tag + detail
      g.clock = detail.length() ? trimTo(detail,8) : String("WC");
    }
    g.league = "WC";
    g.priority = (g.home=="USA" || g.away=="USA");
    g.valid = true;

    bool dup=false;
    for(int x=0;x<gameCount;x++)
      if(games[x].league=="WC" && games[x].home==g.home && games[x].away==g.away){ dup=true; break; }
    if(!dup){ games[gameCount++]=g; added++; }
  }
  Serial.printf("[WC] %d matches\n", added);
}

// ====================================================================
// GOLF MAJORS — leaderboard, only shown during the 4 majors.
// ====================================================================
#define MAX_GOLFERS 7
struct GolfEntry { String pos; String name; String score; };
GolfEntry golfBoard[MAX_GOLFERS];
int    golfCount = 0;
String golfTourName = "";
bool   golfIsMajor = false;

static bool nameIsGolfMajor(const String &n){
  String s = n; s.toLowerCase();
  return (s.indexOf("masters") >= 0)
      || (s.indexOf("pga championship") >= 0)
      || (s.indexOf("u.s. open") >= 0) || (s.indexOf("us open") >= 0)
      || (s.indexOf("open championship") >= 0) || (s.indexOf("the open") >= 0);
}

void fetchGolf(){
  golfCount = 0; golfIsMajor = false; golfTourName = "";
  esp_task_wdt_reset();
  String body = httpGet("https://site.api.espn.com/apis/site/v2/sports/golf/pga/scoreboard", 12000);
  if(body.isEmpty()){ Serial.println("[GOLF] empty"); return; }

  // Filter to leaderboard essentials — the full field is huge.
  StaticJsonDocument<640> filter;
  filter["events"][0]["name"] = true;
  filter["events"][0]["shortName"] = true;
  JsonObject c = filter["events"][0]["competitions"][0]["competitors"][0].to<JsonObject>();
  c["order"] = true;
  c["score"] = true;
  c["athlete"]["shortName"] = true;

  DynamicJsonDocument doc(20480);
  if(deserializeJson(doc, body, DeserializationOption::Filter(filter),
                     DeserializationOption::NestingLimit(20))){
    Serial.println("[GOLF] parse err"); return;
  }
  JsonObject ev = doc["events"][0];
  String name = ev["name"].as<String>();
  if(name.length()==0) name = ev["shortName"].as<String>();
  golfTourName = name;
  if(!nameIsGolfMajor(name)){
    Serial.printf("[GOLF] %s (not a major) — card hidden\n", name.c_str());
    return;
  }
  golfIsMajor = true;
  for(JsonObject p : ev["competitions"][0]["competitors"].as<JsonArray>()){
    if(golfCount >= MAX_GOLFERS) break;
    String nm = p["athlete"]["shortName"].as<String>();
    if(nm.length()==0) continue;
    int order = p["order"] | (golfCount+1);
    String sc = p["score"].as<String>();
    char posb[4]; snprintf(posb,4,"%d",order);
    golfBoard[golfCount].pos   = String(posb);
    golfBoard[golfCount].name  = nm;
    golfBoard[golfCount].score = sc.length()?sc:String("E");
    golfCount++;
  }
  Serial.printf("[GOLF] MAJOR \"%s\" %d golfers\n", name.c_str(), golfCount);
}

// ====================================================================
// TENNIS MAJORS — featured matches, only during the 4 Grand Slams.
// ====================================================================
#define MAX_TENNIS 4
struct TennisMatch { String p1, p2, sets1, sets2; bool p1won, p2won, live; };
TennisMatch tennisM[MAX_TENNIS];
int    tennisCount = 0;
String tennisTour = "";
bool   tennisIsMajor = false;

static bool nameIsTennisMajor(const String &n){
  String s = n; s.toLowerCase();
  return (s.indexOf("australian open") >= 0)
      || (s.indexOf("roland garros") >= 0) || (s.indexOf("french open") >= 0)
      || (s.indexOf("wimbledon") >= 0)
      || (s.indexOf("us open") >= 0) || (s.indexOf("u.s. open") >= 0);
}

// Parse one tour's scoreboard, appending matches. Returns the tournament name.
static String parseTennisTour(const char* url){
  esp_task_wdt_reset();
  String body = httpGet(url, 12000);
  if(body.isEmpty()) return "";

  StaticJsonDocument<768> filter;
  filter["events"][0]["shortName"] = true;
  JsonObject comp = filter["events"][0]["competitions"][0].to<JsonObject>();
  comp["status"]["type"]["state"] = true;
  JsonObject ct = comp["competitors"][0].to<JsonObject>();
  ct["winner"] = true;
  ct["athlete"]["shortName"] = true;
  ct["linescores"][0]["value"] = true;

  DynamicJsonDocument doc(20480);
  if(deserializeJson(doc, body, DeserializationOption::Filter(filter),
                     DeserializationOption::NestingLimit(20))){
    Serial.println("[TENNIS] parse err"); return "";
  }
  JsonArray events = doc["events"].as<JsonArray>();
  if(events.size()==0) return "";
  String tour = events[0]["shortName"].as<String>();
  if(!nameIsTennisMajor(tour)) return tour;   // name returned, but no parse

  for(JsonObject ev : events){
    if(tennisCount >= MAX_TENNIS) break;
    JsonObject cmp = ev["competitions"][0];
    String state = cmp["status"]["type"]["state"].as<String>();
    TennisMatch m; m.p1won=false; m.p2won=false; m.live=(state=="in");
    int pi = 0;
    for(JsonObject c : cmp["competitors"].as<JsonArray>()){
      if(pi > 1) break;
      String nm = c["athlete"]["shortName"].as<String>();
      bool won  = c["winner"] | false;
      String sets = "";
      for(JsonObject ls : c["linescores"].as<JsonArray>()){
        int v = ls["value"] | 0;
        if(sets.length()) sets += " ";
        sets += String(v);
      }
      if(pi==0){ m.p1=nm; m.sets1=sets; m.p1won=won; }
      else     { m.p2=nm; m.sets2=sets; m.p2won=won; }
      pi++;
    }
    if(m.p1.length() && m.p2.length()) tennisM[tennisCount++] = m;
  }
  return tour;
}

void fetchTennis(){
  tennisCount = 0; tennisIsMajor = false; tennisTour = "";
  // ATP (men) first, then WTA (women) — a Grand Slam runs both simultaneously.
  String t1 = parseTennisTour("https://site.api.espn.com/apis/site/v2/sports/tennis/atp/scoreboard");
  String t2 = parseTennisTour("https://site.api.espn.com/apis/site/v2/sports/tennis/wta/scoreboard");
  String tour = nameIsTennisMajor(t1) ? t1 : t2;
  tennisTour = tour;
  tennisIsMajor = (tennisCount > 0) && nameIsTennisMajor(tour);
  Serial.printf("[TENNIS] \"%s\" major=%d matches=%d\n",
                tour.c_str(), tennisIsMajor?1:0, tennisCount);
}

// --- Finance ---
void fetchFinance() {
  finance.valid=false;

  // S&P 500 via Yahoo v8 chart endpoint. The v7 /quote API now returns
  // 401 Unauthorized on every call — Yahoo locked it down to require a
  // session cookie / crumb that's impractical to obtain on ESP32. v8 chart
  // still works without auth and gives us current price + previous close,
  // which is enough to compute the % change.
  {
    bool gotSP = false;
    {
      String body2=httpGetBrowser("https://query1.finance.yahoo.com/v8/finance/chart/%5EGSPC?interval=1d&range=2d");
      if(!body2.isEmpty()){
        DynamicJsonDocument doc2(8192);
        if(!deserializeJson(doc2,body2)){
          JsonArray closes=doc2["chart"]["result"][0]["indicators"]["quote"][0]["close"].as<JsonArray>();
          int n=closes.size();
          if(n>=2){
            float prev=closes[n-2].as<float>(), curr=closes[n-1].as<float>();
            if(curr>0){ finance.sp500=curr; finance.sp500Chg=((curr-prev)/prev)*100.0f; gotSP=true; }
          }
        }
      }
    }
    if(!gotSP) Serial.println("[FINANCE] SP500 v8 chart fetch failed");
  }
  delay(200);

  // Bitcoin via CoinGecko (no key)
  {
    String body=httpGet("https://api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=usd&include_24hr_change=true");
    if(!body.isEmpty()){
      DynamicJsonDocument doc(2048);
      if(!deserializeJson(doc,body)){
        finance.btcUSD=doc["bitcoin"]["usd"].as<float>();
        finance.btcChg=doc["bitcoin"]["usd_24h_change"].as<float>();
      }
    }
  }
  delay(200);

  // Gas prices via EIA
  {
    String url="https://api.eia.gov/v2/petroleum/pri/gnd/data/?api_key=";
    url+=EIA_API_KEY;
    url+="&frequency=weekly&data[0]=value&facets[series][]=EMM_EPMRR_PTE_NUS_DPG&sort[0][column]=period&sort[0][direction]=desc&length=1";
    String body=httpGet(url);
    if(!body.isEmpty()){
      DynamicJsonDocument doc(4096);
      if(!deserializeJson(doc,body)){
        finance.gasNat=doc["response"]["data"][0]["value"].as<float>();
      }
    }
  }
  delay(200);

  // 10yr Treasury via FRED.
  // If FRED_API_KEY is set: use JSON API.
  // Fallback: FRED public CSV endpoint (no key, last 21 days to keep response small).
  {
    bool gotTreasury = false;
    if(String(FRED_API_KEY) != "your_fred_key") {
      String url="https://api.stlouisfed.org/fred/series/observations?series_id=DGS10&api_key=";
      url+=FRED_API_KEY;
      url+="&file_type=json&sort_order=desc&limit=1";
      String body=httpGet(url);
      if(!body.isEmpty()){
        DynamicJsonDocument doc(4096);
        if(!deserializeJson(doc,body)){
          String val=doc["observations"][0]["value"].as<String>();
          if(val!="." && val.length()>0){ finance.treasury10y=val.toFloat(); gotTreasury=true; }
        }
      }
    }
    if(!gotTreasury){
      // Public CSV fallback (no API key required). Filter to last 21 days to cap response size.
      struct tm ti; getLocalTime(&ti);
      time_t t3w=mktime(&ti)-21*86400; struct tm *tp=localtime(&t3w);
      char sd[12]; strftime(sd,12,"%Y-%m-%d",tp);
      String url="https://fred.stlouisfed.org/graph/fredgraph.csv?id=DGS10&observation_start=";
      url+=sd;
      String body=httpGet(url,15000);
      if(!body.isEmpty()){
        // CSV: DATE,DGS10 — scan all lines, keep last non-"." value
        float lastVal=0;
        int s=body.indexOf('\n')+1; // skip header
        while(s<(int)body.length()){
          int e=body.indexOf('\n',s); if(e<0) e=body.length();
          int c=body.indexOf(',',s);
          if(c>s && c<e){
            String val=body.substring(c+1,e); val.trim();
            if(val!="." && val.length()>0) lastVal=val.toFloat();
          }
          s=e+1;
        }
        if(lastVal>0) finance.treasury10y=lastVal;
      }
    }
  }
  delay(200);

  // Fear & Greed via alternative.me (no key)
  {
    String body=httpGet("https://api.alternative.me/fng/?limit=1");
    if(!body.isEmpty()){
      DynamicJsonDocument doc(2048);
      if(!deserializeJson(doc,body)){
        finance.fearGreed=doc["data"][0]["value"].as<int>();
      }
    }
  }
  delay(200);

  // PNBK — Patriot National Bancorp on NASDAQ via Yahoo v8 chart endpoint.
  // Same pattern as S&P 500: prev close + current close → % change.
  {
    String body=httpGetBrowser("https://query1.finance.yahoo.com/v8/finance/chart/PNBK?interval=1d&range=2d");
    if(!body.isEmpty()){
      DynamicJsonDocument doc(8192);
      if(!deserializeJson(doc,body)){
        JsonArray closes=doc["chart"]["result"][0]["indicators"]["quote"][0]["close"].as<JsonArray>();
        int n=closes.size();
        if(n>=2){
          float prev=closes[n-2].as<float>(), curr=closes[n-1].as<float>();
          if(curr>0){
            finance.pnbk=curr;
            finance.pnbkChg=((curr-prev)/prev)*100.0f;
          }
        } else if(n>=1){
          float curr=closes[n-1].as<float>();
          if(curr>0) finance.pnbk=curr;
        }
      }
    }
  }
  delay(200);

  // SOFR + Fed Funds via FRED (CSV fallback works without a key).
  // Series IDs: SOFR = "SOFR", Effective Fed Funds = "DFF".
  // Helper lambda would be nicer; inline two near-identical blocks for clarity.
  auto fetchFredScalar = [](const char* seriesId) -> float {
    // JSON path first if a real FRED key is present
    if(String(FRED_API_KEY) != "your_fred_key"){
      String url="https://api.stlouisfed.org/fred/series/observations?series_id=";
      url+=seriesId; url+="&api_key="; url+=FRED_API_KEY;
      url+="&file_type=json&sort_order=desc&limit=1";
      String body=httpGet(url);
      if(!body.isEmpty()){
        DynamicJsonDocument doc(4096);
        if(!deserializeJson(doc,body)){
          String val=doc["observations"][0]["value"].as<String>();
          if(val!="." && val.length()>0) return val.toFloat();
        }
      }
    }
    // CSV fallback (no key needed). Pull last 21 days, take the last
    // non-"." value.
    struct tm ti; if(!getLocalTime(&ti)) return 0;
    time_t t3w=mktime(&ti)-21*86400; struct tm *tp=localtime(&t3w);
    char sd[12]; strftime(sd,12,"%Y-%m-%d",tp);
    String url="https://fred.stlouisfed.org/graph/fredgraph.csv?id=";
    url+=seriesId; url+="&observation_start="; url+=sd;
    String body=httpGet(url,15000);
    if(body.isEmpty()) return 0;
    float lastVal=0;
    int s=body.indexOf('\n')+1;
    while(s<(int)body.length()){
      int e=body.indexOf('\n',s); if(e<0) e=body.length();
      int c=body.indexOf(',',s);
      if(c>s && c<e){
        String val=body.substring(c+1,e); val.trim();
        if(val!="." && val.length()>0) lastVal=val.toFloat();
      }
      s=e+1;
    }
    return lastVal;
  };
  finance.sofr     = fetchFredScalar("SOFR");
  delay(200);
  finance.fedFunds = fetchFredScalar("DFF");
  delay(200);
  finance.treasury3y = fetchFredScalar("DGS3");
  delay(200);
  finance.treasury5y = fetchFredScalar("DGS5");

  finance.valid=true;
  Serial.printf("[FINANCE] SP=%.0f BTC=%.0f Gas=%.2f 10yr=%.2f F&G=%d PNBK=%.2f SOFR=%.2f FF=%.2f\n",
                finance.sp500,finance.btcUSD,finance.gasNat,finance.treasury10y,finance.fearGreed,
                finance.pnbk,finance.sofr,finance.fedFunds);
}

// S&P 500 intraday sparkline — 15-min intervals, current trading day
// Called separately from the main 5-min finance refresh (heap guard applies)
void fetchSP500Sparkline() {
  String body=httpGetBrowser("https://query1.finance.yahoo.com/v8/finance/chart/%5EGSPC?interval=15m&range=1d");
  if(body.isEmpty()) return;
  DynamicJsonDocument doc(10240);
  if(deserializeJson(doc,body)) return;
  JsonArray closes=doc["chart"]["result"][0]["indicators"]["quote"][0]["close"].as<JsonArray>();
  sp500SparkLen=0;
  for(JsonVariant v : closes){
    if(sp500SparkLen>=SP_SPARK_MAX) break;
    float val=v.as<float>();
    if(!isnan(val) && val>0) sp500Spark[sp500SparkLen++]=val;
  }
  Serial.printf("[SPARK] %d points, last=%.0f\n",sp500SparkLen,sp500SparkLen>0?sp500Spark[sp500SparkLen-1]:0);
}

// --- Weather (7-day forecast) ---
void fetchWeather(float lat, float lon, WeatherData &wx) {
  // Current conditions
  {
    String url="https://api.openweathermap.org/data/2.5/weather?lat=";
    url+=String(lat,4)+"&lon="+String(lon,4)+"&units=imperial&appid="+OWM_API_KEY;
    String body=httpGet(url); if(body.isEmpty()) return;
    DynamicJsonDocument doc(8192);
    if(deserializeJson(doc,body)) return;
    wx.tempF    =doc["main"]["temp"].as<float>();
    wx.feelsF   =doc["main"]["feels_like"].as<float>();
    wx.humidity =doc["main"]["humidity"].as<float>();
    wx.windMph  =doc["wind"]["speed"].as<float>();
    wx.condition=doc["weather"][0]["main"].as<String>();
    wx.icon     =doc["weather"][0]["icon"].as<String>();
    wx.sunrise  =doc["sys"]["sunrise"].as<float>();
    wx.sunset   =doc["sys"]["sunset"].as<float>();
  }
  delay(100);
  // 5-day forecast via free /forecast endpoint (3-hourly, take one entry per day)
  {
    String url="https://api.openweathermap.org/data/2.5/forecast?lat=";
    url+=String(lat,4)+"&lon="+String(lon,4)+"&cnt=40&units=imperial&appid="+OWM_API_KEY;
    String body=httpGet(url); if(body.isEmpty()){wx.valid=true;return;}
    DynamicJsonDocument doc(14336);
    if(deserializeJson(doc,body)){wx.valid=true;return;}
    // Build daily hi/lo by scanning ALL 3-hour entries per day
    const char* dnames[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    int dMday[MAX_FORECAST]; float dHi[MAX_FORECAST]; float dLo[MAX_FORECAST];
    String dIcon[MAX_FORECAST]; String dLabel[MAX_FORECAST];
    int idx=0;
    for(int j=0;j<MAX_FORECAST;j++){dHi[j]=-999.f;dLo[j]=999.f;}
    for(JsonObject d:doc["list"].as<JsonArray>()){
      time_t dt=d["dt"].as<long>();
      struct tm *t2=localtime(&dt);
      int mday=t2->tm_mday;
      float temp=d["main"]["temp"].as<float>();
      int slot=-1;
      for(int j=0;j<idx;j++) if(dMday[j]==mday){slot=j;break;}
      if(slot<0 && idx<MAX_FORECAST){
        slot=idx++;
        dMday[slot]=mday;
        dHi[slot]=-999.f; dLo[slot]=999.f;
        dLabel[slot]=String(dnames[t2->tm_wday]).substring(0,3);
        dIcon[slot]=d["weather"][0]["icon"].as<String>();
      }
      if(slot>=0){
        if(temp>dHi[slot]) dHi[slot]=temp;
        if(temp<dLo[slot]) dLo[slot]=temp;
        if(t2->tm_hour>=11&&t2->tm_hour<=13) dIcon[slot]=d["weather"][0]["icon"].as<String>();
      }
    }
    for(int j=0;j<idx&&j<MAX_FORECAST;j++){
      wx.forecast[j].label=dLabel[j];
      wx.forecast[j].hiF=(dHi[j]>-900)?dHi[j]:0;
      wx.forecast[j].loF=(dLo[j]<900)?dLo[j]:0;
      wx.forecast[j].icon=dIcon[j];
    }
    wx.forecastCount=idx;  }
  wx.valid=true;
  Serial.printf("[WX] %.0fF forecast=%d days\n",wx.tempF,wx.forecastCount);
}

// --- Traffic ---
// OSRM public router (keyless, free). Google Distance Matrix with
// departure_time=now bills on the "Advanced" SKU and was costing real money
// (~$48/mo across the panel + the office Pi) — typical drive time is fine
// for an ambient display. No key, no billing, no live-traffic delta.
void fetchTraffic(float dlat, float dlon, TrafficData &td) {
  String url="https://router.project-osrm.org/route/v1/driving/";
  url+=String(HOME_LON,4)+","+String(HOME_LAT,4)+";";
  url+=String(dlon,4)+","+String(dlat,4)+"?overview=false";
  String body=httpGet(url); if(body.isEmpty()) return;
  DynamicJsonDocument doc(2048);
  if(deserializeJson(doc,body)) return;
  float secs=doc["routes"][0]["duration"] | 0.0f;
  if(secs<=0) return;
  int m=(int)(secs/60.0f+0.5f);
  char buf[16];
  if(m>=60) snprintf(buf,16,"%dh %02dm",m/60,m%60);
  else      snprintf(buf,16,"%dm",m);
  td.durationNormal =String(buf);
  td.durationTraffic=String(buf);   // no live-traffic delta on OSRM
  td.valid=true;
  Serial.printf("[TRAFFIC] osrm %s\n", buf);
}

// --- Marine ---
void fetchTides() {
  struct tm ti; if(!getLocalTime(&ti)) return;
  char dateBuf[9]; strftime(dateBuf,9,"%Y%m%d",&ti);
  String url="https://api.tidesandcurrents.noaa.gov/api/prod/datagetter";
  url+="?product=predictions&application=the_dews_feed";
  url+="&begin_date="; url+=dateBuf;
  url+="&range=48&datum=MLLW&station="; url+=NOAA_STATION_ID;
  url+="&time_zone=lst_ldt&interval=hilo&units=english&format=json";
  String body=httpGet(url,15000); if(body.isEmpty()) return;
  if(body.indexOf("\"error\"")>=0) return;
  DynamicJsonDocument doc(16384);
  if(deserializeJson(doc,body)) return;
  JsonArray preds=doc["predictions"].as<JsonArray>();
  int nowMins=ti.tm_hour*60+ti.tm_min, found=0;
  for(JsonObject p:preds) {
    String t=p["t"].as<String>(); float ht=p["v"].as<float>(); String type=p["type"].as<String>();
    int ph=t.substring(11,13).toInt(),pm=t.substring(14,16).toInt(),pd=t.substring(8,10).toInt();
    int pmo=t.substring(5,7).toInt();
    bool future=(pmo>ti.tm_mon+1)||(pmo==ti.tm_mon+1&&pd>ti.tm_mday)||
                (pmo==ti.tm_mon+1&&pd==ti.tm_mday&&(ph*60+pm)>nowMins);
    if(future){
      char tbuf[10]; int h=ph%12; if(!h) h=12;
      snprintf(tbuf,10,"%d:%02d%s",h,pm,ph>=12?"PM":"AM");
      TidePred tp; tp.height=ht; tp.type=type; tp.timeStr=String(tbuf);

      if(found==0) marine.nextTide=tp;
      if(found==1) marine.followTide=tp;
      found++; if(found>=2) break;
    }
  }
  marine.tideValid=(found>0);
}

void fetchBuoy() {
  String body=httpGet("https://www.ndbc.noaa.gov/data/realtime2/"+String(NDBC_BUOY_ID)+".txt",10000);
  if(body.isEmpty()) return;
  int l1=body.indexOf('\n'),l2=body.indexOf('\n',l1+1),l3=body.indexOf('\n',l2+1);
  if(l2<0||l3<0) return;
  String line=body.substring(l2+1,l3); line.trim();
  String tok[20]; int tc=0,pos=0;
  while(pos<(int)line.length()&&tc<20){
    while(pos<(int)line.length()&&line[pos]==' ') pos++;
    int end=pos; while(end<(int)line.length()&&line[end]!=' ') end++;
    if(end>pos){tok[tc++]=line.substring(pos,end);pos=end;}
  }
  if(tc<15) return;
  auto v=[](String s){return s!="MM"&&s!="99.0"&&s!="999"&&s!="9999";};
  if(v(tok[8]))  marine.waveHt    =tok[8].toFloat()*3.28084f;
  if(v(tok[14])) marine.waterTempF=tok[14].toFloat()*9.0f/5.0f+32.0f;
  if(v(tok[6]))  marine.windKts   =tok[6].toFloat()*1.94384f;
  marine.buoyValid=true;
}

void fetchMarineAlerts() {
  marine.hasAlert=false;
  String url="https://api.weather.gov/alerts/active?zone="+String(NWS_MARINE_ZONE);
  String body=httpGetUA(url); if(body.isEmpty()) return;
  DynamicJsonDocument doc(8192); if(deserializeJson(doc,body)) return;
  for(JsonObject f:doc["features"].as<JsonArray>()){
    String ev=f["properties"]["event"].as<String>(); ev.toUpperCase();
    if(ev.indexOf("GALE")>=0)            {marine.hasAlert=true;marine.alertType="GALE WARN";marine.alertColor=C_RED;break;}
    else if(ev.indexOf("STORM")>=0)      {marine.hasAlert=true;marine.alertType="STORM WARN";marine.alertColor=C_RED;break;}
    else if(ev.indexOf("SMALL CRAFT")>=0){marine.hasAlert=true;marine.alertType="SCA";marine.alertColor=C_ORANGE;}
  }
}

void fetchMarine() {
  marine=MarineData(); fetchTides(); delay(100); fetchBuoy(); delay(100); fetchMarineAlerts();
  Serial.printf("[MARINE] tide=%d buoy=%d\n",marine.tideValid,marine.buoyValid);
}

// --- Lake ---
void fetchLake() {
  // USGS water temp + level - Wallenpaupack Creek at Hawley PA (01427510)
  String url="https://waterservices.usgs.gov/nwis/iv/?sites=01427510&parameterCd=00010,00065&format=json";
  String body=httpGet(url); if(body.isEmpty()) return;
  DynamicJsonDocument doc(10240); if(deserializeJson(doc,body)) return;
  for(JsonObject ts:doc["value"]["timeSeries"].as<JsonArray>()){
    String code=ts["variable"]["variableCode"][0]["value"].as<String>();
    float val=ts["values"][0]["value"][0]["value"].as<String>().toFloat();
    if(code=="00010") lake.waterTempF=val*9.0f/5.0f+32.0f;
    if(code=="00065") lake.levelFt=val;
  }
  lake.valid=true;
  Serial.printf("[LAKE] %.1fF %.2fft\n",lake.waterTempF,lake.levelFt);
}

// --- Metro-North real-time departures via 511NY SIRI StopMonitoring API ---
// Free key at https://511ny.org/developers — returns JSON, no protobuf needed.
// stop_id 110 = Stamford New Haven Line platform.
// ────────────────────────────────────────────────────────────────────────
// METRO-NORTH STATIC SCHEDULE — Stamford CT → Grand Central
// Approximation of the published New Haven Line southbound timetable. Each
// value is HHMM (24-hour, e.g. 1430 = 2:30pm, 30 = 12:30am next day).
// Sorted ascending by time-of-day. Sub-4am entries represent overnight
// service that wraps past midnight.
//
// Realtime SIRI-JSON departures from MTA aren't accessible to ESP32 without
// protobuf decoding (see notes above), so this is the reliable fallback —
// always works, no API key, no failure modes. Refine the values to match
// your exact commute pattern if needed.
// ────────────────────────────────────────────────────────────────────────
const uint16_t SCHED_WEEKDAY_SB[] = {
  // Pre-dawn / early
   430,  530,  600,  615,  630,  645,
  // AM peak — every 10-15 min from 7-9am
   700,  709,  716,  724,  731,  739,  746,  754,
   801,  809,  816,  824,  831,  839,  846,  854,
  // Late morning
   901,  916,  931,  946, 1001, 1031, 1101, 1131,
  // Midday
  1201, 1231, 1301, 1331, 1401, 1431, 1501, 1531,
  // Afternoon (PM peak heads NB out of GCT, fewer SB)
  1555, 1613, 1630, 1700, 1730, 1800, 1830,
  // Evening
  1900, 1930, 2000, 2030, 2100, 2130, 2200, 2230,
  // Late
  2300, 2330,   30,  100,
};
const int SCHED_WEEKDAY_COUNT = sizeof(SCHED_WEEKDAY_SB)/sizeof(SCHED_WEEKDAY_SB[0]);

const uint16_t SCHED_WEEKEND_SB[] = {
   500,  600,  700,  800,  900, 1000, 1100, 1200,
  1300, 1400, 1500, 1600, 1700, 1800, 1900, 2000,
  2100, 2200, 2300,   30,
};
const int SCHED_WEEKEND_COUNT = sizeof(SCHED_WEEKEND_SB)/sizeof(SCHED_WEEKEND_SB[0]);

// Internal — try one URL and parse trains. Returns true on success (any
// trains parsed). Stream-parses to avoid 50-100KB intermediate body String.
// Kept for future reference but no longer called — see fetchTrains below.
bool fetchTrainsURL(const String &url, const char* label){
  Serial.printf("[TRAINS] try %s: %s\n", label, url.substring(0,90).c_str());
  feedWdt();
  HTTPClient h;
  h.begin(url);
  h.setTimeout(15000);
  h.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  h.addHeader("User-Agent","Mozilla/5.0 (compatible; ESP32 Dashboard)");
  h.addHeader("Accept","application/json");

  int code = h.GET();
  Serial.printf("[TRAINS] HTTP %d, free heap=%u\n", code, (unsigned)ESP.getFreeHeap());
  if(code != HTTP_CODE_OK){
    String errBody = h.getString();
    if(errBody.length() > 0){
      String snip = errBody.substring(0, min((int)errBody.length(), 200));
      Serial.printf("[TRAINS] err body: %s\n", snip.c_str());
    }
    h.end();
    return false;
  }

  // Use getString for chunked-encoding-safe body retrieval (same reasoning
  // as fetchConcertsURL — h.getStream() includes raw chunked headers that
  // ArduinoJson cannot parse).
  String body = h.getString();
  h.end();
  feedWdt();
  if(body.length() == 0){ Serial.println("[TRAINS] empty body"); return false; }
  // Strip UTF-8 BOM if 511NY prepended one
  if(body.length()>3 && (uint8_t)body[0]==0xEF && (uint8_t)body[1]==0xBB && (uint8_t)body[2]==0xBF)
    body=body.substring(3);

  // Filter — keep only the SIRI fields we render.
  StaticJsonDocument<512> filt;
  filt["Siri"]["ServiceDelivery"]["StopMonitoringDelivery"][0]
    ["MonitoredStopVisit"][0]["MonitoredVehicleJourney"]["DestinationName"][0] = true;
  filt["Siri"]["ServiceDelivery"]["StopMonitoringDelivery"][0]
    ["MonitoredStopVisit"][0]["MonitoredVehicleJourney"]["PublishedLineName"][0] = true;
  filt["Siri"]["ServiceDelivery"]["StopMonitoringDelivery"][0]
    ["MonitoredStopVisit"][0]["MonitoredVehicleJourney"]["MonitoredCall"]["ExpectedDepartureTime"] = true;
  filt["Siri"]["ServiceDelivery"]["StopMonitoringDelivery"][0]
    ["MonitoredStopVisit"][0]["MonitoredVehicleJourney"]["MonitoredCall"]["DepartureStatus"] = true;

  DynamicJsonDocument doc(12288);
  DeserializationError je = deserializeJson(doc, body,
    DeserializationOption::Filter(filt),
    DeserializationOption::NestingLimit(20));

  if(je){
    Serial.printf("[TRAINS] parse err: %s\n", je.c_str());
    return false;
  }

  struct tm ti; getLocalTime(&ti);
  time_t nowEpoch = mktime(&ti);

  JsonArray visits = doc["Siri"]["ServiceDelivery"]["StopMonitoringDelivery"][0]
                       ["MonitoredStopVisit"].as<JsonArray>();
  if(visits.isNull()){
    Serial.println("[TRAINS] no MonitoredStopVisit array (response shape unexpected)");
    return false;
  }

  int seenAll = 0, kept = 0;
  for(JsonObject v2 : visits){
    seenAll++;
    if(trainCount >= MAX_TRAINS) break;
    auto jrn = v2["MonitoredVehicleJourney"];
    String depStr = jrn["MonitoredCall"]["ExpectedDepartureTime"].as<String>();
    if(depStr.length() < 16) continue;
    String status = jrn["MonitoredCall"]["DepartureStatus"].as<String>();

    // Parse ISO-8601 "YYYY-MM-DDTHH:MM:SS..."
    int yr = depStr.substring(0,4).toInt();
    int mo = depStr.substring(5,7).toInt();
    int dy = depStr.substring(8,10).toInt();
    int hr = depStr.substring(11,13).toInt();
    int mn = depStr.substring(14,16).toInt();
    struct tm dep = {0};
    dep.tm_year = yr-1900; dep.tm_mon = mo-1; dep.tm_mday = dy;
    dep.tm_hour = hr; dep.tm_min = mn; dep.tm_isdst = -1;
    time_t depEpoch = mktime(&dep);
    int minsAway = (int)((depEpoch - nowEpoch) / 60);
    if(minsAway < -2) continue;

    // Filter to southbound NYC-bound only (Stamford → Grand Central / Harlem-125)
    String dst = jrn["DestinationName"][0].as<String>();
    if(dst.indexOf("Grand Central") < 0 && dst.indexOf("Harlem") < 0) continue;
    if(dst.indexOf("Grand Central") >= 0) dst = "Grand Central";
    else if(dst.indexOf("Harlem") >= 0)   dst = "Harlem-125 St";

    int h12 = hr % 12; if(h12 == 0) h12 = 12;
    char ampm = hr < 12 ? 'a' : 'p';
    snprintf(trains[trainCount].time, 8, "%d:%02d%c", h12, mn, ampm);
    dst = dst.substring(0, min((int)dst.length(), 21));
    dst.toCharArray(trains[trainCount].dest, 22);

    String ln = jrn["PublishedLineName"][0].as<String>();
    ln = ln.substring(0, min((int)ln.length(), 17));
    ln.toCharArray(trains[trainCount].line, 18);

    trains[trainCount].minsAway = minsAway;
    trains[trainCount].delayed = (status == "delayed" || status == "Delayed");
    trains[trainCount].valid = true;
    trainCount++; kept++;
  }
  Serial.printf("[TRAINS] %s: %d visits, %d southbound kept\n", label, seenAll, kept);
  return kept > 0;
}

// Populate trains[] from the static published timetable.
// No network call — instant, infallible, no API keys required.
// Picks weekday/weekend table by current day-of-week, finds the next
// MAX_TRAINS departures within 4 hours of the current local clock.
void fetchTrains(){
  trainCount = 0;
  struct tm ti;
  if(!getLocalTime(&ti)){
    Serial.println("[TRAINS] no local time yet");
    return;
  }

  int curMin = ti.tm_hour * 60 + ti.tm_min;
  bool weekend = (ti.tm_wday == 0 || ti.tm_wday == 6);  // Sun=0, Sat=6

  const uint16_t* sched = weekend ? SCHED_WEEKEND_SB : SCHED_WEEKDAY_SB;
  int schedLen          = weekend ? SCHED_WEEKEND_COUNT : SCHED_WEEKDAY_COUNT;

  for(int i = 0; i < schedLen && trainCount < MAX_TRAINS; i++){
    int hhmm = sched[i];
    int trainHr = hhmm / 100;
    int trainMn = hhmm % 100;
    int trainMin = trainHr * 60 + trainMn;

    int diff = trainMin - curMin;
    // Heuristic: if more than 2 hours in the "past", treat as next-day wrap
    // (handles overnight entries like 0030 / 0100). Anything mildly past is
    // skipped — train has already departed.
    if(diff < -120) diff += 1440;
    if(diff < 0)    continue;
    if(diff > 240)  continue;   // don't surface trains > 4h away

    int h12 = trainHr % 12; if(h12 == 0) h12 = 12;
    char ampm = trainHr < 12 ? 'a' : 'p';
    snprintf(trains[trainCount].time, 8, "%d:%02d%c", h12, trainMn, ampm);

    // Static destination — all our schedule entries head to GCT
    strcpy(trains[trainCount].dest, "Grand Central");
    strcpy(trains[trainCount].line, "New Haven");
    trains[trainCount].minsAway = diff;
    trains[trainCount].delayed  = false;   // schedule = no delay info
    trains[trainCount].valid    = true;
    trainCount++;
  }
  Serial.printf("[TRAINS] %d scheduled departures (%s)\n",
    trainCount, weekend ? "weekend" : "weekday");
}

// --- Flights ---
// Haversine distance in miles between two lat/lon (degrees).
float haversineMi(float lat1, float lon1, float lat2, float lon2){
  const float R = 3958.8f;  // earth radius in miles
  float toRad = PI/180.0f;
  float dlat = (lat2-lat1)*toRad;
  float dlon = (lon2-lon1)*toRad;
  float a = sinf(dlat/2)*sinf(dlat/2) +
            cosf(lat1*toRad)*cosf(lat2*toRad)*sinf(dlon/2)*sinf(dlon/2);
  return R * 2 * atan2f(sqrtf(a), sqrtf(1-a));
}

// 8-point compass label for bearing from origin to target (in degrees, 0=N).
const char* compassLabel(float bearingDeg){
  while(bearingDeg<0)    bearingDeg+=360;
  while(bearingDeg>=360) bearingDeg-=360;
  if(bearingDeg<22.5||bearingDeg>=337.5) return "N";
  if(bearingDeg<67.5)  return "NE";
  if(bearingDeg<112.5) return "E";
  if(bearingDeg<157.5) return "SE";
  if(bearingDeg<202.5) return "S";
  if(bearingDeg<247.5) return "SW";
  if(bearingDeg<292.5) return "W";
  return "NW";
}

// Initial-bearing in degrees (0=N) from (lat1,lon1) toward (lat2,lon2).
float bearingDeg(float lat1, float lon1, float lat2, float lon2){
  float toRad = PI/180.0f;
  float dlon = (lon2-lon1)*toRad;
  float y = sinf(dlon)*cosf(lat2*toRad);
  float x = cosf(lat1*toRad)*sinf(lat2*toRad)
          - sinf(lat1*toRad)*cosf(lat2*toRad)*cosf(dlon);
  return atan2f(y,x) * 180.0f/PI;
}

// Heuristic aircraft size category from altitude (ft) + speed (kt).
// Lookup-by-type would be more accurate but doubles API calls per flight;
// alt+speed are >90% predictive for the common cases.
//   1 = regional/prop (CRJ, ERJ, Dash-8)
//   2 = narrowbody    (737, A320, A220)
//   3 = widebody      (777, 787, A330/350/380)
uint8_t categorizeFlight(float altFt, float spdKt){
  if(altFt > 33000 && spdKt > 460) return 3;        // heavy long-haul
  if(altFt < 14000 || spdKt < 230)  return 1;       // regional/prop
  return 2;                                         // narrowbody default
}

void fetchFlights() {
  // NOTE: We don't reset flightCount until we get a successful response —
  // otherwise a temporary OpenSky rate-limit blanks the card.
  String url = "https://opensky-network.org/api/states/all?lamin=40.60&lomin=-74.20&lamax=41.50&lomax=-72.80";
  feedWdt();
  HTTPClient h;
  h.begin(url);
  h.setTimeout(10000);
  h.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  h.addHeader("User-Agent", "Mozilla/5.0 (compatible; ESP32 Dashboard)");
  h.addHeader("Accept", "application/json");

  int code = h.GET();
  Serial.printf("[FLIGHTS] HTTP %d, free heap=%u\n", code, (unsigned)ESP.getFreeHeap());
  if(code != HTTP_CODE_OK){
    String err = h.getString();
    if(err.length() > 0){
      Serial.printf("[FLIGHTS] err: %s\n", err.substring(0, min((int)err.length(), 150)).c_str());
    }
    h.end();
    return;
  }

  String body = h.getString();
  h.end();
  feedWdt();
  Serial.printf("[FLIGHTS] body=%u bytes\n", (unsigned)body.length());
  if(body.length() == 0) return;

  DynamicJsonDocument doc(16384);
  DeserializationError je = deserializeJson(doc, body);
  if(je){
    Serial.printf("[FLIGHTS] parse err: %s\n", je.c_str());
    return;
  }

  // Successful parse — reset count and repopulate
  flightCount = 0;
  for(JsonArray s : doc["states"].as<JsonArray>()){
    if(flightCount >= MAX_FLIGHTS) break;
    if(s[8].as<bool>()) continue;                              // on-ground filter
    String cs = s[1].as<String>(); cs.trim();
    if(cs.isEmpty()) continue;
    FlightData &f = flights[flightCount];
    f.icao24    = s[0].as<String>(); f.icao24.toUpperCase();
    f.callsign  = cs;
    f.lon       = s[5].as<float>();
    f.lat       = s[6].as<float>();
    f.altitude  = s[7].as<float>() * 3.28084f;                 // m → ft
    f.speed     = s[9].as<float>() * 1.94384f;                 // m/s → kt
    f.heading   = s[10].as<float>();
    f.vertRate  = s[11].as<float>() * 196.85f;                 // m/s → ft/min
    f.distMi    = haversineMi(HOME_LAT, HOME_LON, f.lat, f.lon);
    f.compass   = compassLabel(bearingDeg(HOME_LAT, HOME_LON, f.lat, f.lon));
    f.category  = categorizeFlight(f.altitude, f.speed);
    f.routeFrom = ""; f.routeTo = ""; f.routeKnown = false;
    f.valid     = true;
    flightCount++;
  }
  Serial.printf("[FLIGHTS] %d overhead\n", flightCount);
}

// Look up callsign in the route cache. Returns nullptr if not present.
RouteCacheEntry* routeCacheFind(const String &cs){
  for(int i=0;i<ROUTE_CACHE_SIZE;i++){
    if(routeCache[i].tried && routeCache[i].callsign==cs) return &routeCache[i];
  }
  return nullptr;
}
// LRU slot for new entry — evicts least-recently-touched.
RouteCacheEntry* routeCacheSlot(){
  RouteCacheEntry* oldest = &routeCache[0];
  for(int i=1;i<ROUTE_CACHE_SIZE;i++){
    if(!routeCache[i].tried) return &routeCache[i];           // empty slot wins
    if(routeCache[i].lastTouch < oldest->lastTouch) oldest = &routeCache[i];
  }
  return oldest;
}

// Hit adsbdb.com for any new callsigns in the current flight list.
// Free, no key needed. Cache by callsign so we don't refetch on every refresh.
void fetchFlightRoutes(){
  for(int i=0;i<flightCount;i++){
    // Feed the hardware watchdog every iteration — each adsbdb call can take
    // 2-4 seconds (TLS handshake + request); 6 in a row = ~20s, well within
    // the 60s WDT window but defensive feeding prevents any edge case.
    esp_task_wdt_reset();

    FlightData &f = flights[i];
    if(f.callsign.length()==0) continue;
    // Already cached? Copy + mark known.
    RouteCacheEntry* hit = routeCacheFind(f.callsign);
    if(hit){
      hit->lastTouch = millis();
      f.routeFrom = hit->from; f.routeTo = hit->to;
      f.routeKnown = (hit->from.length()>0);
      continue;
    }
    // Not cached — fetch.
    String url = "https://api.adsbdb.com/v0/callsign/" + f.callsign;
    String body = httpGetBrowser(url, 5000);
    RouteCacheEntry* slot = routeCacheSlot();
    slot->callsign = f.callsign;
    slot->lastTouch = millis();
    slot->tried = true;
    slot->found = false;
    slot->from = ""; slot->to = "";
    if(body.length()>0){
      // Filter to just the airport IATA codes — entire response can be ~1KB
      StaticJsonDocument<256> filter;
      filter["response"]["flightroute"]["origin"]["iata_code"]      = true;
      filter["response"]["flightroute"]["destination"]["iata_code"] = true;
      DynamicJsonDocument doc(2048);
      if(!deserializeJson(doc, body, DeserializationOption::Filter(filter))){
        JsonObject fr = doc["response"]["flightroute"];
        if(!fr.isNull()){
          slot->from = fr["origin"]["iata_code"].as<String>();
          slot->to   = fr["destination"]["iata_code"].as<String>();
          slot->found = true;
        }
      }
    }
    f.routeFrom = slot->from; f.routeTo = slot->to;
    f.routeKnown = (slot->from.length()>0);
    delay(100);   // be polite to the free API
  }
  Serial.printf("[ROUTES] cache used; %d flights resolved\n", flightCount);
}

// --- WHOOP ---
void fetchWhoop() {
  if(strlen(WHOOP_RELAY_URL)==0) return;
  // Use browser UA — Apps Script Web Apps can return HTML for default UAs
  String body=httpGetBrowser(String(WHOOP_RELAY_URL),15000);
  if(body.isEmpty()){ Serial.println("[WHOOP] empty body"); return; }
  Serial.printf("[WHOOP] got %u bytes\n", (unsigned)body.length());
  DynamicJsonDocument doc(4096);
  DeserializationError je = deserializeJson(doc, body);
  if(je){ Serial.printf("[WHOOP] parse err: %s\n", je.c_str()); return; }
  whoop.recovery   = doc["recovery_score"].as<float>();
  whoop.hrv        = doc["hrv"].as<float>();
  whoop.rhr        = doc["rhr"].as<float>();
  whoop.sleepScore = doc["sleep_score"].as<float>();
  whoop.strain     = doc["strain"].as<float>();
  if      (whoop.recovery >= 67) { whoop.recoveryLabel = "PEAK";  whoop.recoveryColor = C_LIME; }
  else if (whoop.recovery >= 34) { whoop.recoveryLabel = "GOOD";  whoop.recoveryColor = C_YELLOW; }
  else                            { whoop.recoveryLabel = "TIRED"; whoop.recoveryColor = C_RED; }
  whoop.valid = true;
  Serial.printf("[WHOOP] recovery=%.0f hrv=%.1f rhr=%.0f sleep=%.0f strain=%.1f\n",
    whoop.recovery, whoop.hrv, whoop.rhr, whoop.sleepScore, whoop.strain);
}

// --- Google Calendar ---
void fetchCalendar() {
  if(strlen(GCAL_RELAY_URL)==0) return;  // deploy relay to enable
  calCount=0;
  String body=httpGet(String(GCAL_RELAY_URL)); if(body.isEmpty()) return;
  DynamicJsonDocument doc(8192); if(deserializeJson(doc,body)) return;
  int idx=0;
  for(JsonObject ev:doc["events"].as<JsonArray>()) {
    if(idx>=MAX_EVENTS) break;
    calEvents[idx].time =ev["time"].as<String>();
    calEvents[idx].title=ev["title"].as<String>();
    calEvents[idx].valid=true; idx++;
  }
  calCount=idx;
  Serial.printf("[GCAL] %d events\n",calCount);
}

// --- Concerts (Ticketmaster Discovery API) ---
// Uses ArduinoJson filter to extract only 3 fields per event — keeps doc tiny
// even though the raw TM response can be 30-80KB.
// =====================================================================
// TODEW TASK SYNC — pulls the live task list from the ToDew app's
// Cloudflare Worker (/api/sync, same store the phone app syncs to), scores
// urgency, and keeps the top items for the TODEW card. Requires
// TODEW_SYNC_TOKEN in secrets.h (must match the Worker's HEALTH_TOKEN).
// =====================================================================
#define MAX_TODOS 6
struct TodoItem {
  String  title;
  int     days;   // days until due; 9999 = no due date
  uint8_t pri;    // 2=urgent 1=high 0=normal
};
TodoItem todos[MAX_TODOS];
int  todoCount  = 0;
bool todoLoaded = false;   // distinguishes "syncing..." from "all clear"

// Days from today to a "YYYY-MM-DD" due date. Negative = overdue.
int todewDaysUntil(const char* due){
  if(!due || strlen(due) < 10) return 9999;
  struct tm nowTm;
  if(!getLocalTime(&nowTm)) return 9999;
  struct tm dueTm = {};
  dueTm.tm_year = atoi(due) - 1900;
  dueTm.tm_mon  = atoi(due + 5) - 1;
  dueTm.tm_mday = atoi(due + 8);
  dueTm.tm_hour = 12;                     // noon-to-noon avoids DST off-by-one
  struct tm dayTm = nowTm;
  dayTm.tm_hour = 12; dayTm.tm_min = 0; dayTm.tm_sec = 0;
  return (int)lround(difftime(mktime(&dueTm), mktime(&dayTm)) / 86400.0);
}

void fetchToDew(){
  if(strlen(TODEW_SYNC_TOKEN) == 0) return;
  feedWdt();
  HTTPClient h;
  h.begin(String(TODEW_SYNC_URL) + "/api/sync");
  h.setTimeout(15000);
  h.addHeader("Authorization", String("Bearer ") + TODEW_SYNC_TOKEN);
  h.addHeader("Accept", "application/json");
  h.addHeader("Accept-Encoding", "identity");
  h.useHTTP10(true);   // clean stream for ArduinoJson (see fetchConcertsURL)
  int code = h.GET();
  Serial.printf("[TODEW] HTTP %d, free heap=%u\n", code, (unsigned)ESP.getFreeHeap());
  if(code != HTTP_CODE_OK){ h.end(); return; }

  // The sync payload carries every task incl. completion history — filter
  // down to the five fields we score/display so the parse doc stays small.
  StaticJsonDocument<256> filter;
  filter["tasks"][0]["title"]     = true;
  filter["tasks"][0]["priority"]  = true;
  filter["tasks"][0]["due"]       = true;
  filter["tasks"][0]["completed"] = true;
  filter["tasks"][0]["type"]      = true;
  DynamicJsonDocument doc(24576);
  DeserializationError je = deserializeJson(doc, h.getStream(),
    DeserializationOption::Filter(filter),
    DeserializationOption::NestingLimit(20));
  h.end();
  feedWdt();
  if(je){ Serial.printf("[TODEW] parse err: %s\n", je.c_str()); return; }

  JsonArray tasks = doc["tasks"].as<JsonArray>();
  if(tasks.isNull()){
    Serial.println("[TODEW] no tasks array — has the app synced yet?");
    todoLoaded = true; todoCount = 0;
    return;
  }

  // Score urgency and keep the top MAX_TODOS via insertion into a small
  // sorted list. Routines (daily habit loops) are excluded — they repeat
  // forever and would drown out genuinely urgent one-off tasks.
  TodoItem best[MAX_TODOS];
  int bestScore[MAX_TODOS];
  int n = 0;
  for(JsonObject t : tasks){
    if(t["completed"].as<bool>()) continue;
    const char* ty = t["type"] | "";
    if(strcmp(ty, "routine") == 0) continue;
    const char* ti = t["title"] | "";
    if(!ti[0]) continue;
    const char* pr = t["priority"] | "normal";
    uint8_t pri = (strcmp(pr, "urgent") == 0) ? 2 : (strcmp(pr, "high") == 0) ? 1 : 0;
    int days = todewDaysUntil(t["due"] | "");
    int score = pri * 200;
    if(days < 9000){
      if(days < 0)       score += 180;   // overdue trumps same-tier peers
      else if(days == 0) score += 150;
      else if(days == 1) score += 100;
      else if(days <= 3) score += 60;
      else if(days <= 7) score += 30;
    }
    if(score == 0) continue;             // normal-priority, undated → not "urgent"
    int pos = n;
    for(int i = 0; i < n; i++) if(score > bestScore[i]){ pos = i; break; }
    if(pos >= MAX_TODOS) continue;
    for(int j = min(n, MAX_TODOS-1); j > pos; j--){
      best[j] = best[j-1]; bestScore[j] = bestScore[j-1];
    }
    String s = String(ti); sanitizeAscii(s);
    best[pos].title = s; best[pos].days = days; best[pos].pri = pri;
    bestScore[pos] = score;
    if(n < MAX_TODOS) n++;
  }
  for(int i = 0; i < n; i++) todos[i] = best[i];
  todoCount = n; todoLoaded = true;
  Serial.printf("[TODEW] %d urgent items kept\n", todoCount);
}

// Helper that runs one Ticketmaster query against the given URL and parses
// up to MAX_CONCERTS events into the global `concerts[]`.
// Returns true if it parsed at least one event.
//
// Note: previously this used `deserializeJson(doc, h.getStream(), filter)`
// to avoid an intermediate 80KB body String. But h.getStream() returns the
// RAW socket stream including chunked-transfer-encoding size headers (e.g.
// "1ff0\r\n...chunk...\r\n0\r\n\r\n"). ArduinoJson chokes on those headers
// with "IncompleteInput" because they look like garbage between JSON tokens.
// HTTPClient's getString() handles chunked decoding properly, so we use that
// — the memory cost is acceptable since we cap event count and use a filter.
bool fetchConcertsURL(const String &url, const char* label){
  Serial.printf("[CONCERTS] try %s: %s\n", label, url.substring(0,90).c_str());
  feedWdt();
  HTTPClient h;
  h.begin(url);
  h.setTimeout(15000);
  h.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  h.addHeader("User-Agent","Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
  h.addHeader("Accept","application/json");
  // Force uncompressed response — TM otherwise sometimes returns gzip,
  // which ArduinoJson can't decode and would fail with "InvalidInput".
  h.addHeader("Accept-Encoding", "identity");
  // HEAP FIX: force HTTP/1.0. The server then can't use chunked transfer
  // encoding, which means getStream() yields CLEAN json we can hand straight
  // to ArduinoJson's streaming parser. The old path buffered the whole body
  // via getString() — TM responses run ~60KB even at size=5, and that single
  // contiguous String allocation fails silently on a fragmented heap, which
  // presented as "[CONCERTS] empty body" on-device while the API was fine.
  // Streaming caps peak memory at the 12KB filter doc regardless of body size.
  h.useHTTP10(true);

  int code = h.GET();
  Serial.printf("[CONCERTS] HTTP %d, free heap=%u\n", code, (unsigned)ESP.getFreeHeap());
  if(code != HTTP_CODE_OK){
    String errBody = h.getString();
    if(errBody.length() > 0){
      String snip = errBody.substring(0, min((int)errBody.length(), 200));
      Serial.printf("[CONCERTS] err body: %s\n", snip.c_str());
    }
    h.end();
    return false;
  }

  // Filter — only extract the fields we display.
  StaticJsonDocument<512> filter;
  filter["_embedded"]["events"][0]["name"]                              = true;
  filter["_embedded"]["events"][0]["dates"]["start"]["localDate"]       = true;
  filter["_embedded"]["events"][0]["dates"]["start"]["localTime"]       = true;
  filter["_embedded"]["events"][0]["_embedded"]["venues"][0]["name"]    = true;
  filter["_embedded"]["events"][0]["_embedded"]["venues"][0]["city"]["name"] = true;

  // Stream-parse directly from the socket — no intermediate String.
  DynamicJsonDocument doc(12288);
  DeserializationError je = deserializeJson(doc, h.getStream(),
    DeserializationOption::Filter(filter),
    DeserializationOption::NestingLimit(20));
  h.end();
  feedWdt();

  if(je){
    Serial.printf("[CONCERTS] parse err: %s\n", je.c_str());
    return false;
  }

  int got = 0;
  JsonArray events = doc["_embedded"]["events"].as<JsonArray>();
  if(events.isNull()){
    Serial.println("[CONCERTS] no _embedded.events array in response");
    return false;
  }

  for(JsonObject ev : events){
    if(concertCount >= MAX_CONCERTS) break;
    String nm = ev["name"].as<String>(); nm.trim();
    if(nm.length() == 0) continue;
    sanitizeAscii(nm);
    String venue = ev["_embedded"]["venues"][0]["name"].as<String>();
    String city  = ev["_embedded"]["venues"][0]["city"]["name"].as<String>();
    sanitizeAscii(venue);
    sanitizeAscii(city);

    concerts[concertCount].name  = nm;
    concerts[concertCount].date  = ev["dates"]["start"]["localDate"].as<String>();
    concerts[concertCount].time  = ev["dates"]["start"]["localTime"].as<String>();
    concerts[concertCount].venue = venue;
    concerts[concertCount].city  = city;
    concerts[concertCount].valid = true;
    concertCount++;
    got++;
  }
  Serial.printf("[CONCERTS] %s parsed %d events\n", label, got);
  return got > 0;
}

void fetchConcerts() {
  if(strlen(TICKETMASTER_KEY) == 0){
    Serial.println("[CONCERTS] no TICKETMASTER_KEY");
    return;
  }
  concertCount = 0;

  // RESPONSE-SIZE NOTE: verified the key + latlong query work (HTTP 200)
  // from a desktop — the on-device failure is almost certainly the response
  // body size. TM events can carry kilobyte-long descriptions (season
  // tickets etc.); size=10 responses topped 100KB which starves the ESP32
  // heap mid-parse. All queries now use size=5 + music classification on
  // the primary to keep bodies small, and skip test/TBA listings.

  // ── Primary URL: music near Stamford CT (smallest useful response) ──
  String urlA = "https://app.ticketmaster.com/discovery/v2/events.json";
  urlA += "?latlong=41.0534%2C-73.5387";   // %2C = URL-encoded comma
  urlA += "&radius=30&unit=miles";
  urlA += "&classificationName=music";
  urlA += "&size=5&sort=date,asc&includeTBA=no&includeTest=no";
  urlA += "&apikey="; urlA += TICKETMASTER_KEY;
  if(fetchConcertsURL(urlA, "latlong music")) return;

  // ── Fallback: ALL event types near Stamford, wider radius.
  // (The old postalCode=06903 query verifiably returns zero events from the
  // TM API — desktop-tested — so it was a dead fallback.)
  String urlB = "https://app.ticketmaster.com/discovery/v2/events.json";
  urlB += "?latlong=41.0534%2C-73.5387&radius=40&unit=miles";
  urlB += "&size=5&sort=date,asc&includeTBA=no&includeTest=no";
  urlB += "&apikey="; urlB += TICKETMASTER_KEY;
  if(fetchConcertsURL(urlB, "latlong all")) return;

  // ── Last-resort: any music event in NYC metro market ──
  String urlC = "https://app.ticketmaster.com/discovery/v2/events.json";
  urlC += "?marketId=35";    // 35 = New York/Tri-State market
  urlC += "&classificationName=music";
  urlC += "&size=5&sort=date,asc&includeTBA=no&includeTest=no";
  urlC += "&apikey="; urlC += TICKETMASTER_KEY;
  fetchConcertsURL(urlC, "NYC market");
}

// Format "2025-04-20" + "20:00:00" → "SAT APR 20 • 8PM"
String formatConcertDate(const String &d, const String &t) {
  if(d.length()<10) return d;
  int yr=d.substring(0,4).toInt(), mo=d.substring(5,7).toInt(), dy=d.substring(8,10).toInt();
  struct tm tm2={0}; tm2.tm_year=yr-1900; tm2.tm_mon=mo-1; tm2.tm_mday=dy; mktime(&tm2);
  const char* dnam[]={"SUN","MON","TUE","WED","THU","FRI","SAT"};
  const char* mnam[]={"JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC"};
  char buf[24];
  if(t.length()>=5){
    int h=t.substring(0,2).toInt();
    bool pm=h>=12; if(h>12)h-=12; if(h==0)h=12;
    snprintf(buf,24,"%s %s %d  %d%s",dnam[tm2.tm_wday],mnam[mo-1],dy,h,pm?"PM":"AM");
  } else {
    snprintf(buf,24,"%s %s %d",dnam[tm2.tm_wday],mnam[mo-1],dy);
  }
  return String(buf);
}

// =====================================================================
// PIXEL ART HELPERS
// =====================================================================
void drawStars(int count=25, int maxY=64) {
  uint16_t dim=dsp->color565(50,50,80);
  // Deterministic star field
  int px[]={3,11,19,29,37,45,55,63,71,81,91,97,107,117,125,7,23,41,59,77,95,113,5,33,87};
  int py[]={2, 4, 1, 5, 2, 6, 3, 1, 4,  2, 5,  1,   3,  2,  5,9, 7, 8,10, 6,  9,  8,14,12,11};
  for(int i=0;i<count&&i<25;i++){
    if(py[i]>=maxY) continue;
    bool bright=((millis()/500+i)%4==0);
    dsp->drawPixel(px[i],py[i],bright?C_WHITE:dim);
  }
}

// Stamp maria (dark seas) + craters on a lit moon disc. Positions are
// proportional to radius so the same map works for the big r=20 hero moon
// and the tiny r=4 strip moons (which only get the largest features).
static void moonSurfaceDetail(int cx, int cy, int r){
  uint16_t maria  = dsp->color565(196,188,142);  // darker basalt seas
  uint16_t mariaD = dsp->color565(172,164,120);
  uint16_t crater = dsp->color565(214,206,156);
  if(r >= 12){
    // Mare Imbrium — big blotch upper-left
    dsp->fillCircle(cx - r/3,     cy - r/3,     r/4,  maria);
    dsp->fillCircle(cx - r/2,     cy - r/5,     r/6,  mariaD);
    // Mare Tranquillitatis — mid-right
    dsp->fillCircle(cx + r/4,     cy - r/8,     r/5,  maria);
    // Mare Nubium — lower-center
    dsp->fillCircle(cx - r/8,     cy + r/3,     r/6,  mariaD);
    // Tycho crater + ray hints (bottom)
    dsp->fillCircle(cx + r/8,     cy + (2*r)/3 - r/8, 1, crater);
    dsp->drawPixel(cx + r/8 - 2,  cy + r/2, crater);
    dsp->drawPixel(cx + r/8 + 2,  cy + r/2, crater);
    // Copernicus crater (small, left-center)
    dsp->fillCircle(cx - r/6,     cy,           1, crater);
    // Kepler pinpoint
    dsp->drawPixel(cx - r/2 + 1,  cy + r/6, crater);
  } else if(r >= 4){
    // Tiny moons just get two darker blotches
    dsp->drawPixel(cx - 1, cy - 1, maria);
    dsp->drawPixel(cx + 1, cy,     mariaD);
  }
}

void drawMoonFull(int cx, int cy, int r, float phase) {
  uint16_t moonC=dsp->color565(240,235,180);
  uint16_t darkC=dsp->color565(8,15,35);
  dsp->drawCircle(cx,cy,r+1,dsp->color565(80,75,50));
  dsp->fillCircle(cx,cy,r,C_BLACK);
  if(phase<0.04||phase>=0.96){
    // New moon — faint earthshine ring + barely-visible disc
    dsp->drawCircle(cx,cy,r,dsp->color565(30,30,50));
    if(r >= 12) dsp->fillCircle(cx,cy,r-1,dsp->color565(10,12,22));
    return;
  }
  if(phase>=0.47&&phase<0.53){
    dsp->fillCircle(cx,cy,r,moonC);
    moonSurfaceDetail(cx,cy,r);
    return;
  }
  bool waxing=(phase<0.5);
  float illum=waxing?(phase*2.0f):((1.0f-phase)*2.0f);
  dsp->fillCircle(cx,cy,r,moonC);
  moonSurfaceDetail(cx,cy,r);   // detail first, terminator shadow covers it
  int sw=(int)(r*(1.0f-illum)+0.5f);
  if(sw>0){
    for(int dy=-r;dy<=r;dy++){
      int chord=(int)sqrt((float)(r*r-dy*dy));
      int left =waxing?(cx-chord):(cx+sw-chord);
      int right=waxing?(cx-chord+sw):(cx+chord);
      if(right>left) dsp->drawFastHLine(max(left,cx-r),cy+dy,min(right-left,r*2),darkC);
    }
  }
}

void drawSun(int cx, int cy, int r) {
  dsp->fillCircle(cx,cy,r,C_GOLD);
  // Rays
  for(int a=0;a<8;a++){
    float ang=a*3.14159f/4.0f;
    int x1=(int)(cx+(r+1)*cos(ang)), y1=(int)(cy+(r+1)*sin(ang));
    int x2=(int)(cx+(r+3)*cos(ang)), y2=(int)(cy+(r+3)*sin(ang));
    dsp->drawLine(x1,y1,x2,y2,C_YELLOW);
  }
}

void drawCloud(int x, int y, int w, uint16_t c) {
  int h=w/3;
  fillRect(x,y+h/2,w,h,c);
  dsp->fillCircle(x+w/4,  y+h/2, h/2, c);
  dsp->fillCircle(x+w*3/4,y+h/2, h/2, c);
  dsp->fillCircle(x+w/2,  y,     h/2+1, c);
}

void drawSoftCloud(int x, int y, int w, uint16_t body, uint16_t shade) {
  drawCloud(x, y, w, body);
  int h=w/3;
  dsp->drawFastHLine(x+2, y+h, max(1,w-4), shade);
  dsp->drawFastHLine(x+w/4, y+h/2+1, max(1,w/2), shade);
}

void drawGradientSkyRGB(int y0, int y1, uint8_t r0, uint8_t g0, uint8_t b0,
                        uint8_t r1, uint8_t g1, uint8_t b1) {
  int span=max(1,y1-y0-1);
  for(int y=y0;y<y1;y++){
    uint8_t r=map(y,y0,y0+span,r0,r1);
    uint8_t g=map(y,y0,y0+span,g0,g1);
    uint8_t b=map(y,y0,y0+span,b0,b1);
    dsp->drawFastHLine(0,y,PANEL_WIDTH,dsp->color565(r,g,b));
    if(((y*5)%11)==0) {
      uint16_t glint=dsp->color565(min(255,(int)r+10),min(255,(int)g+10),min(255,(int)b+10));
      for(int x=(y*7)%13;x<PANEL_WIDTH;x+=29) dsp->drawPixel(x,y,glint);
    }
  }
}

uint16_t rfrSkyPixel(const String &season, int y) {
  y=constrain(y,0,43);
  if(season=="winter") return dsp->color565(10,15,map(y,0,43,60,25));
  if(season=="spring") return dsp->color565(map(y,0,43,100,160),map(y,0,43,180,215),255);
  if(season=="summer") return dsp->color565(map(y,0,43,80,160),map(y,0,43,170,215),255);
  return dsp->color565(map(y,0,43,205,140),map(y,0,43,152,98),map(y,0,43,88,48));
}

uint16_t cabinSkyPixel(const String &season, int y) {
  y=constrain(y,0,43);
  if(season=="winter") return dsp->color565(5,10,map(y,0,43,50,20));
  if(season=="summer" && y>=30) return dsp->color565(0,map(y,30,43,80,130),map(y,30,43,150,200));
  if(season=="summer") return dsp->color565(100,180,255);
  if(season=="spring") return dsp->color565(map(y,0,43,100,160),map(y,0,43,180,215),255);
  return dsp->color565(map(y,0,43,180,120),map(y,0,43,130,80),map(y,0,43,60,30));
}

// ────────────────────────────────────────────────────────────────────────
// Full background pixel sampler — returns the EXACT background color the
// scene's BG draw would have placed at (x,y), ignoring any animated sprite.
//
// Used for SMOOTH SPRITE-DELTA rendering: when a sprite (bee, ball, bear,
// puck, flake, lava particle) moves, we erase its OLD position by repainting
// each pixel with the value rfrBgPixel/cabinBgPixel/tikiBgPixel returns at
// that coordinate. Result = aurora-smooth animation with zero flicker
// because no full-screen redraw is needed and per-frame work is tiny.
//
// Coverage: sky (y<44) + procedural ground texture (y>=44). Sprites should
// stay clear of foreground furniture (pine trees, fence, paths, house).
// ────────────────────────────────────────────────────────────────────────
uint16_t rfrBgPixel(int x, int y, const String &season){
  if(y < 44) return rfrSkyPixel(season, y);
  // Ground texture procedurally identical to drawGroundTexture()
  uint16_t baseC, shadeC, fleckC;
  if(season=="winter"){
    baseC  = dsp->color565(210,215,220);
    shadeC = C_WHITE;
    fleckC = dsp->color565(235,240,245);
  } else if(season=="fall"){
    baseC  = dsp->color565(75,105,28);
    shadeC = dsp->color565(45,75,15);
    fleckC = dsp->color565(110,90,38);
  } else {
    baseC  = dsp->color565(55,165,45);
    shadeC = dsp->color565(35,95,30);
    fleckC = dsp->color565(105,185,70);
  }
  uint8_t n = ((x*7 + y*13) & 0x07);
  return (n < 2) ? shadeC : (n < 6) ? baseC : fleckC;
}

uint16_t cabinBgPixel(int x, int y, const String &season){
  if(y < 44) return cabinSkyPixel(season, y);
  uint16_t baseC, shadeC, fleckC;
  if(season=="winter"){
    baseC  = dsp->color565(200,210,215);
    shadeC = C_WHITE;
    fleckC = dsp->color565(235,240,245);
  } else if(season=="fall"){
    baseC  = dsp->color565(80,110,30);
    shadeC = dsp->color565(48,68,22);
    fleckC = dsp->color565(160,92,28);
  } else {
    baseC  = dsp->color565(50,160,40);
    shadeC = dsp->color565(30,92,28);
    fleckC = dsp->color565(100,188,75);
  }
  uint8_t n = ((x*7 + y*13) & 0x07);
  return (n < 2) ? shadeC : (n < 6) ? baseC : fleckC;
}

// Summer lake pixel sampler — matches the procedural water drawn in the
// summer cabin BG block (lake spans y=LAKE_TOP..63). Returns the exact
// color for any (x,y) so the water-ski bear sprite can erase by sampling,
// restoring wave texture instead of scarring a flat blue rectangle.
// LAKE_TOP is the horizon line where the far shore meets the water.
#define CABIN_LAKE_TOP 26
uint16_t cabinLakePixel(int x, int y){
  int ly = y - CABIN_LAKE_TOP;
  if(ly < 0) ly = 0;
  int span = 63 - CABIN_LAKE_TOP;
  // Vertical gradient: bright teal at the horizon → deep navy at the bottom
  uint8_t r = map(ly, 0, span,  28,   0);
  uint8_t g = map(ly, 0, span, 150,  52);
  uint8_t b = map(ly, 0, span, 205, 110);
  // Deterministic wave crests — short bright dashes on a stable lattice so
  // they never flicker and the erase always reproduces them exactly.
  int crest = ((x * 3 + y * 7));
  if((y % 3 == 0) && (((x + (y/3)) % 7) == 0)){
    return dsp->color565(min(255, r + 70), min(255, g + 55), min(255, b + 35));
  }
  if((crest & 0x0F) == 0){
    return dsp->color565(min(255, r + 25), min(255, g + 22), min(255, b + 18));
  }
  return dsp->color565(r, g, b);
}

// Tiki sky pixel sampler — matches the tropical teal gradient drawn in
// the BG block (y=0..27 sky band). Used for sprite-delta erase of lava
// particles arcing across the sky.
uint16_t tikiSkyPixel(int y){
  if(y < 0) y = 0;
  if(y > 27) y = 27;
  uint8_t r = map(y, 0, 27, 8,   18);
  uint8_t g = map(y, 0, 27, 95,  130);
  uint8_t b = map(y, 0, 27, 110, 155);
  return dsp->color565(r, g, b);
}

void drawLowHills(uint16_t farC, uint16_t nearC, int baseY) {
  dsp->fillTriangle(0,baseY, 28,baseY-10, 64,baseY, farC);
  dsp->fillTriangle(40,baseY, 82,baseY-13, 128,baseY, farC);
  dsp->fillTriangle(0,baseY+5, 35,baseY-6, 82,baseY+5, nearC);
  dsp->fillTriangle(55,baseY+5, 104,baseY-8, 128,baseY+5, nearC);
}

void drawFirLine(int baseY, bool far=false) {
  uint16_t dark = far ? dsp->color565(12,55,45) : dsp->color565(10,78,42);
  uint16_t mid  = far ? dsp->color565(18,75,55) : dsp->color565(22,112,52);
  int step = far ? 8 : 10;
  for(int x=-2; x<PANEL_WIDTH+6; x+=step){
    int h = far ? 11 + ((x*7)&5) : 15 + ((x*5)&7);
    uint16_t c = ((x/step)&1) ? dark : mid;
    dsp->fillTriangle(x, baseY, x+step/2, baseY-h, x+step+1, baseY, c);
    fillRect(x+step/2-1, baseY-3, 2, 4, dsp->color565(70,45,24));
  }
}

void drawGroundTexture(int y, uint16_t baseC, uint16_t shadeC, uint16_t fleckC) {
  fillRect(0,y,PANEL_WIDTH,PANEL_HEIGHT-y,baseC);
  dsp->drawFastHLine(0,y,PANEL_WIDTH,shadeC);
  for(int yy=y+3;yy<PANEL_HEIGHT;yy+=5){
    for(int x=(yy*11)%17;x<PANEL_WIDTH;x+=19) dsp->drawPixel(x,yy,fleckC);
  }
}

void drawStonePath(int cx, int topY, int topW, int bottomW, uint16_t stone, uint16_t shade) {
  for(int y=topY;y<PANEL_HEIGHT;y++){
    int w=map(y,topY,PANEL_HEIGHT-1,topW,bottomW);
    int x0=cx-w/2;
    dsp->drawFastHLine(x0,y,w,stone);
    if((y-topY)%4==0) dsp->drawFastHLine(x0+1,y,max(1,w-2),shade);
  }
  for(int y=topY+2;y<PANEL_HEIGHT;y+=6){
    int w=map(y,topY,PANEL_HEIGHT-1,topW,bottomW);
    for(int x=cx-w/2+3;x<cx+w/2;x+=8) dsp->drawPixel(x,y,shade);
  }
}

void drawSplitRailFence(int y, uint16_t railC, uint16_t shadowC) {
  for(int x=0;x<PANEL_WIDTH;x+=18){
    dsp->drawFastVLine(x+2,y-5,9,shadowC);
    dsp->drawFastVLine(x+3,y-5,9,railC);
  }
  for(int x=0;x<PANEL_WIDTH;x++){
    if(x<35 || x>92){
      dsp->drawPixel(x,y-2,railC);
      dsp->drawPixel(x,y+1,shadowC);
    }
  }
}

void drawLakeBand(int y, uint16_t waterC, uint16_t waveC) {
  fillRect(0,y,PANEL_WIDTH,14,waterC);
  for(int x=0;x<PANEL_WIDTH;x+=9){
    dsp->drawFastHLine(x,y+2,5,waveC);
    dsp->drawFastHLine(x+4,y+6,4,dsp->color565(40,120,175));
    dsp->drawFastHLine(x+1,y+10,5,dsp->color565(10,82,145));
  }
}

void drawBamboo(int x, int y, int h) {
  uint16_t a=dsp->color565(156,164,66), b=dsp->color565(88,108,36);
  dsp->drawFastVLine(x,y,h,a);
  dsp->drawFastVLine(x+1,y,h,b);
  for(int yy=y+5; yy<y+h; yy+=8) dsp->drawFastHLine(x,yy,2,dsp->color565(210,190,92));
}

void drawTikiMaskBig(int cx, int cy) {
  uint16_t wood=dsp->color565(148,76,30), dark=dsp->color565(45,20,8);
  uint16_t glow=dsp->color565(255,184,45), cream=dsp->color565(244,224,164);
  uint16_t red=dsp->color565(218,44,25), teal=dsp->color565(26,150,130);
  for(int y=0;y<26;y++){
    int margin = (y<3 || y>22) ? 6 : (y<6 || y>19) ? 3 : 0;
    dsp->drawFastHLine(cx-17+margin, cy+y, 34-margin*2, wood);
    if(y%4==1) dsp->drawFastHLine(cx-15+margin, cy+y, 30-margin*2, dsp->color565(112,55,22));
  }
  dsp->drawFastHLine(cx-14,cy+4,28,dark); dsp->drawFastHLine(cx-13,cy+5,26,teal);
  fillRect(cx-12,cy+8,8,6,dark); fillRect(cx+4,cy+8,8,6,dark);
  dsp->drawPixel(cx-9,cy+10,glow); dsp->drawPixel(cx-8,cy+10,glow);
  dsp->drawPixel(cx+7,cy+10,glow); dsp->drawPixel(cx+8,cy+10,glow);
  fillRect(cx-4,cy+14,8,4,red); dsp->drawFastHLine(cx-5,cy+18,10,dark);
  fillRect(cx-13,cy+20,26,4,dark);
  for(int i=0;i<6;i++) fillRect(cx-11+i*4,cy+20,2,4,cream);
  dsp->drawFastHLine(cx-17,cy+2,34,dark);
  dsp->drawFastHLine(cx-15,cy+25,30,dark);
}

// Weather icon based on OWM icon code
void drawWxIcon(int cx, int cy, int sz, const String &icon) {
  // Prefer the high-fidelity bitmap when available. The bitmap is anchored
  // by top-left so convert from the procedural API's centre+size convention:
  //   sz==3 → 12 px square → use 16-px bitmap centred at (cx, cy)
  //   sz>=4 → 16 px+        → use 24-px bitmap centred at (cx, cy)
  // Bitmap size chosen to stay close to the procedural footprint so no other
  // layout math in the weather card has to change.
  {
    int bmpSize = (sz >= 4) ? 24 : 16;
    int ox = cx - bmpSize/2;
    int oy = cy - bmpSize/2;
    if(drawWxIconBmp(ox, oy, icon, bmpSize)) return;
  }

  bool day=icon.endsWith("d");
  if(icon.startsWith("01"))      drawSun(cx,cy,sz);
  else if(icon.startsWith("02")){drawCloud(cx-sz/2,cy-sz/3,sz*2,C_GRAY);if(day)drawSun(cx-sz,cy-sz,sz/2);}
  else if(icon.startsWith("03")||icon.startsWith("04")) drawCloud(cx-sz,cy-sz/2,sz*2,C_GRAY);
  else if(icon.startsWith("09")||icon.startsWith("10")){
    drawCloud(cx-sz,cy-sz,sz*2,C_GRAY);
    for(int i=0;i<3;i++) dsp->drawFastVLine(cx-sz/2+i*sz/2,cy,sz/2,C_BLUE);
  }
  else if(icon.startsWith("11")){
    drawCloud(cx-sz,cy-sz,sz*2,C_DARKGRAY);
    dsp->drawLine(cx,cy,cx-sz/3,cy+sz,C_YELLOW);
  }
  else if(icon.startsWith("13")){
    drawCloud(cx-sz,cy-sz,sz*2,C_WHITE);
    for(int i=0;i<4;i++) dsp->drawPixel(cx-sz/2+i*sz/3,cy+i%2*(sz/3),C_CYAN);
  }
  else { dsp->fillCircle(cx,cy,sz/2,C_GRAY); }
}

// =====================================================================
// ANIMATION STATE GLOBALS
// =====================================================================

// Snowflakes
#define MAX_FLAKES 35
struct Flake { float x,y,spd,drift; };
static Flake flakes[MAX_FLAKES];
static bool flakesInited=false;
void initFlakes(){
  for(int i=0;i<MAX_FLAKES;i++){
    flakes[i].x=random(0,128); flakes[i].y=random(0,64);
    flakes[i].spd=0.25f+random(0,10)*0.06f;
    flakes[i].drift=(random(0,3)-1)*0.08f;
  }
  flakesInited=true;
}

// Lava particles
#define MAX_LAVA 24
struct LavaPart { float x,y,vx,vy; bool active; uint16_t col; };
static LavaPart lava[MAX_LAVA];
static bool lavaInited=false;
void spawnLava(int i){
  // Crater is now at (95, 12) — volcano on the right side of the new tiki scene
  lava[i].x = 95 + (random(0, 8) - 4);
  lava[i].y = 12;
  lava[i].vx = (random(0, 18) - 9) * 0.25f;
  lava[i].vy = -(random(6, 16) * 0.35f);
  uint8_t r = 200 + random(0, 55), g = random(0, 120);
  lava[i].col = ((uint16_t)(r>>3) << 11) | ((uint16_t)(g>>2) << 5);
  lava[i].active = true;
}
void initLava(){
  for(int i=0;i<MAX_LAVA;i++){spawnLava(i);lava[i].y=35+random(0,15);}
  lavaInited=true;
}

// Campfire particles (cabin fall/winter)
#define MAX_CAMP 12
struct CampPart { float x,y,vy; int life; uint16_t col; bool active; };
static CampPart camp[MAX_CAMP];
static bool campInited=false;
void spawnCamp(int i){
  camp[i].x=82.f+random(-3,4);
  camp[i].y=53.f;
  camp[i].vy=-(0.3f+random(0,8)*0.1f);
  camp[i].life=12+random(0,18);
  uint8_t r=200+random(0,55),g=random(0,100);
  camp[i].col=((uint16_t)(r>>3)<<11)|((uint16_t)(g>>2)<<5);
  camp[i].active=true;
}
void initCamp(){
  for(int i=0;i<MAX_CAMP;i++){spawnCamp(i);camp[i].y=43.f+random(0,11);}
  campInited=true;
}

// Fireflies (cabin summer)
#define MAX_FF 7
struct Firefly { float x,y,dx,dy; int blinkTimer; bool on; };
static Firefly ff[MAX_FF];
static bool ffInited=false;
void initFireflies(){
  for(int i=0;i<MAX_FF;i++){
    ff[i].x=20+random(0,88); ff[i].y=12+random(0,30);
    ff[i].dx=(random(0,3)-1)*0.3f; ff[i].dy=(random(0,3)-1)*0.2f;
    if(ff[i].dx==0) ff[i].dx=0.2f;
    ff[i].blinkTimer=random(0,50); ff[i].on=(random(0,2)==0);
  }
  ffInited=true;
}

// Benny animation
static float ballX=90,ballY=20,ballVX=-1.6f,ballVY=0.9f;
static float bennyX=20,bennyY=20;
static bool  bennyWag=false;

// Conway's Game of Life
#define GOL_W 64
#define GOL_H 32
static uint8_t golGrid[GOL_H][GOL_W];
static uint8_t golNext[GOL_H][GOL_W];
static bool golInited=false;
void initGOL(){
  for(int y=0;y<GOL_H;y++) for(int x=0;x<GOL_W;x++) golGrid[y][x]=random(0,3)==0?1:0;
  golInited=true;
}
void stepGOL(){
  for(int y=0;y<GOL_H;y++) for(int x=0;x<GOL_W;x++){
    int n=0;
    for(int dy=-1;dy<=1;dy++) for(int dx=-1;dx<=1;dx++){
      if(dx==0&&dy==0) continue;
      n+=golGrid[(y+dy+GOL_H)%GOL_H][(x+dx+GOL_W)%GOL_W];
    }
    golNext[y][x]=(golGrid[y][x]?(n==2||n==3):(n==3))?1:0;
  }
  memcpy(golGrid,golNext,sizeof(golGrid));
}

// Aurora
static float auroraPhase=0;

// Firework particles for splash
#define MAX_FW 40
struct FWPart { float x,y,vx,vy; uint16_t col; bool active; int life; };
static FWPart fw[MAX_FW];

// Money particles
#define MAX_MONEY 20
struct MoneyPart { int x,y,spd; char sym; uint16_t col; };
static MoneyPart money[MAX_MONEY];

// ISS — lastISS is declared in the REFRESH TIMESTAMPS block above
static float issLat=0,issLon=0;
static bool issValid=false;

// =====================================================================
// PIXEL ART SCENES (Helper draw functions)
// =====================================================================

void drawPineTree(int x, int base, int scale=1){
  uint16_t lt=dsp->color565(60,200,55),dk=dsp->color565(25,130,25);
  uint16_t outline=dsp->color565(5,30,5);
  // Black outline pass first - draw silhouette slightly larger
  dsp->drawFastVLine(x-1,base-scale,scale*2+1,outline);
  dsp->drawFastVLine(x+1,base-scale,scale*2+1,outline);
  for(int d=0;d<=3*scale;d++){
    dsp->drawFastHLine(x-d-1,base-2*scale-d,d*2+3,outline);
  }
  for(int d=0;d<=2*scale;d++){
    dsp->drawFastHLine(x-d-1,base-7*scale-d,d*2+3,outline);
  }
  dsp->drawPixel(x-1,base-11*scale,outline);
  dsp->drawPixel(x+1,base-11*scale,outline);
  dsp->drawPixel(x,base-12*scale,outline);
  // Now draw the tree on top
  dsp->drawFastVLine(x,base-scale,scale*2,dsp->color565(120,70,30));
  for(int d=0;d<=3*scale;d++) dsp->drawFastHLine(x-d,base-2*scale-d,d*2+1,(d%2==0)?lt:dk);
  for(int d=0;d<=2*scale;d++) dsp->drawFastHLine(x-d,base-7*scale-d,d*2+1,(d%2==0)?lt:dk);
  dsp->drawPixel(x,base-11*scale,lt);
}

void drawPalmTree(int x, int base){
  dsp->drawLine(x,base,x+2,base-14,C_BROWN);
  uint16_t g=dsp->color565(25,140,25);
  dsp->drawLine(x+2,base-14,x-8,base-20,g);
  dsp->drawLine(x+2,base-14,x+10,base-19,g);
  dsp->drawLine(x+2,base-14,x-10,base-15,g);
  dsp->drawLine(x+2,base-14,x+12,base-13,g);
  dsp->drawLine(x+2,base-14,x+1,base-20,g);
}

void drawTorch(int x, int y, bool flick){
  dsp->drawFastVLine(x,y+5,7,C_BROWN);
  uint16_t f=flick?C_YELLOW:C_ORANGE;
  dsp->drawPixel(x,y+2,f);dsp->drawPixel(x-1,y+3,f);
  dsp->drawPixel(x+1,y+3,f);dsp->drawPixel(x,y+4,C_RED);
}

// Benny — side-profile golden retriever, matched to the reference pixel art
// of the real Benny: CREAM coat (not orange), darker golden legs + tail +
// haunch patch, brown floppy ear, dark muzzle with black nose, pink tongue
// out, and his orange collar.
// FOOTPRINT: 16 wide × 10 tall from (ox,oy), facing right. If this grows,
// the erase box in the RFR summer scene MUST grow with it (that mismatch
// was a trail bug with the old 23×20 sprite).
void drawBenny(int ox, int oy, bool wag){
  uint16_t cream=dsp->color565(238,222,160);   // pale golden coat
  uint16_t gold =dsp->color565(210,155,38);    // legs / tail / patch
  uint16_t brown=dsp->color565(118,68,28);     // floppy ear
  uint16_t muzz =dsp->color565(150,105,60);    // muzzle shading
  uint16_t nose =dsp->color565(40,25,15);      // black nose
  uint16_t tong =dsp->color565(250,120,130);   // tongue
  uint16_t coll =dsp->color565(222,88,28);     // orange collar
  uint16_t eye  =dsp->color565(30,20,10);

  // Tail — gold, angled up-left, wags between two poses
  if(wag){ dsp->drawPixel(ox,oy+1,gold); dsp->drawPixel(ox+1,oy+2,gold); }
  else   { dsp->drawPixel(ox,oy+3,gold); dsp->drawPixel(ox+1,oy+3,gold); }

  // Body — cream mass with a sloped back (x2..11, y2..7)
  dsp->drawFastHLine(ox+3,oy+2,8,cream);
  fillRect(ox+2,oy+3,10,4,cream);
  dsp->drawFastHLine(ox+3,oy+7,9,cream);
  // Gold haunch patch (rear hip, like the reference)
  dsp->drawPixel(ox+3,oy+4,gold);
  dsp->drawPixel(ox+4,oy+5,gold);
  dsp->drawPixel(ox+3,oy+5,gold);

  // Head — cream block x11..15, y0..4
  fillRect(ox+11,oy,4,4,cream);
  dsp->drawPixel(ox+15,oy+1,cream);
  dsp->drawPixel(ox+15,oy+2,muzz);      // muzzle shading
  dsp->drawPixel(ox+15,oy+3,nose);      // black nose tip
  dsp->drawPixel(ox+14,oy+3,muzz);
  // Tongue out below the nose
  dsp->drawPixel(ox+15,oy+4,tong);
  // Eye
  dsp->drawPixel(ox+13,oy+1,eye);
  // Brown floppy ear hanging at the back of the head
  dsp->drawPixel(ox+11,oy,brown);
  dsp->drawFastVLine(ox+11,oy+1,3,brown);

  // Orange collar at the neck
  dsp->drawPixel(ox+11,oy+4,coll);
  dsp->drawPixel(ox+12,oy+4,coll);

  // Legs — GOLD (darker than body, per reference), y7..9. Two pairs;
  // inner pair tucks/extends alternately for a trot.
  dsp->drawFastVLine(ox+3, oy+7,3,gold);            // back outer
  dsp->drawFastVLine(ox+10,oy+7,3,gold);            // front outer
  if(wag){
    dsp->drawFastVLine(ox+6, oy+7,2,gold);          // back inner tucked
    dsp->drawFastVLine(ox+12,oy+7,3,gold);          // front inner extended
  } else {
    dsp->drawFastVLine(ox+6, oy+7,3,gold);
    dsp->drawFastVLine(ox+12,oy+7,2,gold);
  }
}

// Benny sleeping — curled up, 22×12px, perfect for night winter scene.
// Palette matched to the reference art: cream coat, gold accents, brown ear.
void drawBennySleep(int ox, int oy){
  uint16_t fur=dsp->color565(238,222,160),drk=dsp->color565(210,155,38);
  uint16_t nose=dsp->color565(40,25,15),wht=dsp->color565(248,238,190);
  // Body oval
  dsp->drawFastHLine(ox+4,oy+3,14,fur);
  dsp->drawFastHLine(ox+2,oy+4,17,fur);dsp->drawFastHLine(ox+2,oy+5,17,fur);
  dsp->drawFastHLine(ox+2,oy+6,17,fur);dsp->drawFastHLine(ox+2,oy+7,17,fur);
  dsp->drawFastHLine(ox+4,oy+8,14,fur);
  // Head at left
  dsp->fillCircle(ox+4,oy+5,4,fur);
  // Closed eye (just a line)
  dsp->drawFastHLine(ox+2,oy+4,3,drk);
  // Ear flop — brown like the reference
  dsp->drawFastVLine(ox+1,oy+2,4,dsp->color565(118,68,28));
  dsp->drawFastVLine(ox+2,oy+2,4,dsp->color565(118,68,28));
  // Nose
  dsp->drawFastHLine(ox+1,oy+6,2,nose);
  // White belly
  dsp->drawFastHLine(ox+6,oy+6,8,wht);dsp->drawFastHLine(ox+6,oy+7,8,wht);
  // Curled tail
  dsp->drawPixel(ox+19,oy+3,fur);dsp->drawPixel(ox+20,oy+4,fur);
  dsp->drawPixel(ox+21,oy+5,drk);dsp->drawPixel(ox+20,oy+6,drk);
  // Tucked legs
  dsp->drawFastHLine(ox+8,oy+8,10,drk);
}

// Tiki mask
void drawTikiMask(int ox, int oy){
  uint16_t wood=dsp->color565(140,80,30),dark=dsp->color565(60,30,8);
  uint16_t lite=dsp->color565(180,110,45),teeth=dsp->color565(240,230,200);
  for(int dy=0;dy<=18;dy++){
    int margin=(dy==0||dy==18)?3:(dy<=2||dy>=16)?2:(dy<=4||dy>=14)?1:0;
    int w=14-margin*2; dsp->drawFastHLine(ox+margin,oy+dy,w,wood);
  }
  dsp->drawFastHLine(ox+1,oy+2,12,dark);dsp->drawFastHLine(ox+1,oy+3,12,lite);
  fillRect(ox+1,oy+4,4,4,dark);
  dsp->drawPixel(ox+2,oy+5,dsp->color565(255,180,0));dsp->drawPixel(ox+3,oy+5,dsp->color565(255,180,0));
  fillRect(ox+9,oy+4,4,4,dark);
  dsp->drawPixel(ox+10,oy+5,dsp->color565(255,180,0));dsp->drawPixel(ox+11,oy+5,dsp->color565(255,180,0));
  dsp->drawFastHLine(ox+4,oy+9,6,dark);dsp->drawFastHLine(ox+3,oy+10,8,dark);
  dsp->drawFastHLine(ox+1,oy+12,12,dark);dsp->drawFastHLine(ox+1,oy+13,12,dark);
  for(int i=0;i<3;i++){dsp->drawFastVLine(ox+2+i*4,oy+12,2,teeth);dsp->drawFastVLine(ox+3+i*4,oy+12,2,teeth);}
  dsp->drawFastHLine(ox+2,oy+15,10,lite);dsp->drawFastHLine(ox+2,oy+16,10,dark);
}

// Tall carved tiki totem pole — W=11, H=46, ox=left edge, oy=top
void drawTikiTotem(int ox, int oy){
  uint16_t wd=dsp->color565(130,85,40);
  uint16_t dk=dsp->color565(50,25,8);
  uint16_t lt=dsp->color565(185,120,55);
  uint16_t th=dsp->color565(240,230,200);
  uint16_t gl=dsp->color565(255,175,0);
  const int W=10;
  // Shaft
  fillRect(ox+1,oy,W-2,46,wd);
  dsp->drawFastVLine(ox,oy,46,dk);
  dsp->drawFastVLine(ox+W,oy,46,dk);
  dsp->drawFastVLine(ox+2,oy,46,lt);
  // Crown (wider top 4 rows)
  dsp->drawFastHLine(ox-1,oy,  W+3,dk);
  dsp->drawFastHLine(ox-1,oy+1,W+3,lt);
  dsp->drawFastHLine(ox-1,oy+2,W+3,wd);
  dsp->drawFastHLine(ox-1,oy+3,W+3,dk);
  // Face 1 (oy+4..oy+15)
  dsp->drawFastHLine(ox+1,oy+4,W-2,dk);
  fillRect(ox+1,oy+5,3,3,dk);  dsp->drawPixel(ox+2,oy+6,gl);
  fillRect(ox+W-4,oy+5,3,3,dk); dsp->drawPixel(ox+W-3,oy+6,gl);
  dsp->drawFastHLine(ox+3,oy+9,4,lt); dsp->drawFastHLine(ox+3,oy+10,4,dk);
  dsp->drawFastHLine(ox+1,oy+12,W-2,dk);
  dsp->drawFastVLine(ox+2,oy+12,3,th); dsp->drawFastVLine(ox+5,oy+12,3,th); dsp->drawFastVLine(ox+8,oy+12,3,th);
  // Divider band
  fillRect(ox,oy+16,W+1,2,dk);
  // Face 2 (oy+19..oy+30)
  dsp->drawFastHLine(ox+1,oy+19,W-2,dk);
  fillRect(ox+1,oy+20,3,3,dk); dsp->drawPixel(ox+2,oy+21,gl);
  fillRect(ox+W-4,oy+20,3,3,dk); dsp->drawPixel(ox+W-3,oy+21,gl);
  dsp->drawFastHLine(ox+3,oy+24,4,dk);
  dsp->drawFastHLine(ox+1,oy+27,W-2,dk);
  dsp->drawFastVLine(ox+2,oy+27,2,th); dsp->drawFastVLine(ox+5,oy+27,2,th); dsp->drawFastVLine(ox+8,oy+27,2,th);
  // Wing flange
  dsp->drawFastHLine(ox-2,oy+31,W+5,dk);
  dsp->drawFastHLine(ox-2,oy+32,W+5,lt);
  // Face 3 (oy+34..oy+41)
  dsp->drawFastHLine(ox+1,oy+34,W-2,dk);
  fillRect(ox+2,oy+35,2,2,dk); fillRect(ox+W-4,oy+35,2,2,dk);
  dsp->drawFastHLine(ox+2,oy+39,W-4,dk);
  dsp->drawFastVLine(ox+3,oy+39,2,th); dsp->drawFastVLine(ox+7,oy+39,2,th);
  // Base flare
  dsp->drawFastHLine(ox-1,oy+42,W+3,dk);
  fillRect(ox,oy+43,W+1,3,wd);
}

// Red Fox Road - colonial house (sage siding, proper peaked roof, red door)
void drawRFRHouse(int ox, int oy){
  uint16_t wall  =dsp->color565(220,225,200); // bright cream siding (was dim sage)
  uint16_t sidng =dsp->color565(180,185,160); // siding shadow lines
  uint16_t roof  =dsp->color565(110,100,95);  // brighter charcoal roof
  uint16_t roofH =dsp->color565(85,75,70);    // darker shingle alternate
  uint16_t eave  =dsp->color565(255,250,235); // bright white eave/trim
  uint16_t brick =dsp->color565(220,90,55);   // bright chimney brick
  uint16_t mortr =dsp->color565(170,140,115); // mortar
  uint16_t win   =dsp->color565(255,235,140); // glowing yellow window (was blue-grey)
  uint16_t winFr =dsp->color565(255,250,235); // bright window frame
  uint16_t door  =dsp->color565(220,40,25);   // bright red door
  uint16_t doorF =dsp->color565(150,25,12);   // door shadow
  uint16_t stone =dsp->color565(190,180,160); // brighter path stone
  uint16_t bush  =dsp->color565(60,180,55);   // brighter shrubs

  // House is 52px wide, body starts at oy+14
  // ROOF: proper triangle - NARROW at top (apex), WIDE at base (eave)
  int roofH2=13; // roof height in pixels
  for(int i=0;i<roofH2;i++){
    int w=(i+1)*4; if(w>52)w=52;
    int lx=ox+26-w/2;
    dsp->drawFastHLine(lx, oy+i, w, (i%2==0)?roof:roofH);
  }
  // Eave line at base of roof
  dsp->drawFastHLine(ox, oy+roofH2, 52, eave);
  dsp->drawFastHLine(ox, oy+roofH2+1, 52, dsp->color565(100,95,90));

  // CHIMNEY - right of center, rises through roof
  fillRect(ox+36, oy-3, 7, roofH2+6, brick);
  // Brick pattern
  for(int by=oy-3;by<oy+roofH2+3;by+=3){
    dsp->drawFastHLine(ox+36,by,7,mortr);
    if((by/3)%2==0) dsp->drawPixel(ox+39,by+1,mortr);
    else { dsp->drawPixel(ox+37,by+1,mortr); dsp->drawPixel(ox+41,by+1,mortr); }
  }
  // Chimney cap
  dsp->drawFastHLine(ox+35,oy-4,9,dsp->color565(90,80,70));

  // MAIN BODY - 2 story (oy+15 to oy+42)
  fillRect(ox, oy+15, 52, 28, wall);
  // Horizontal siding lines
  for(int sy=oy+17;sy<oy+43;sy+=4) dsp->drawFastHLine(ox,sy,52,sidng);
  // Corner trim
  dsp->drawFastVLine(ox,oy+15,28,eave);
  dsp->drawFastVLine(ox+51,oy+15,28,eave);

  // UPPER WINDOWS (3 windows, row at oy+17)
  for(int wi=0;wi<3;wi++){
    int wx=ox+4+wi*16;
    fillRect(wx,oy+17,10,8,win);
    dsp->drawRect(wx,oy+17,10,8,winFr);
    dsp->drawFastVLine(wx+5,oy+17,8,winFr); // mullion
    dsp->drawFastHLine(wx,oy+21,10,winFr);  // rail
  }

  // LOWER WINDOWS (flanking door)
  fillRect(ox+4, oy+28, 10, 10, win);
  dsp->drawRect(ox+4, oy+28, 10, 10, winFr);
  dsp->drawFastVLine(ox+9, oy+28, 10, winFr);
  dsp->drawFastHLine(ox+4, oy+33, 10, winFr);

  fillRect(ox+38, oy+28, 10, 10, win);
  dsp->drawRect(ox+38, oy+28, 10, 10, winFr);
  dsp->drawFastVLine(ox+43, oy+28, 10, winFr);
  dsp->drawFastHLine(ox+38, oy+33, 10, winFr);

  // FRONT DOOR - centered, red
  fillRect(ox+21, oy+28, 10, 15, door);
  dsp->drawRect(ox+21, oy+28, 10, 15, doorF);
  // Door panels
  fillRect(ox+22,oy+29,4,5,dsp->color565(130,25,12));
  fillRect(ox+27,oy+29,3,5,dsp->color565(130,25,12));
  fillRect(ox+22,oy+35,8,5,dsp->color565(130,25,12));
  // Doorknob
  dsp->fillCircle(ox+29,oy+37,1,C_GOLD);
  // Door surround/trim
  dsp->drawFastHLine(ox+20,oy+27,12,eave);
  dsp->drawFastVLine(ox+20,oy+27,16,eave);
  dsp->drawFastVLine(ox+31,oy+27,16,eave);

  // FOUNDATION
  fillRect(ox, oy+43, 52, 3, stone);

  // SHRUBS flanking door
  dsp->fillCircle(ox+14,oy+42,4,bush);
  dsp->fillCircle(ox+38,oy+42,4,bush);
  dsp->fillCircle(ox+10,oy+43,3,dsp->color565(30,90,30));
  dsp->fillCircle(ox+42,oy+43,3,dsp->color565(30,90,30));

  // STONE PATH
  for(int pi=0;pi<4;pi++){
    fillRect(ox+21,oy+46+pi*3,10,2,stone);
    if(pi<3) dsp->drawFastHLine(ox+21,oy+47+pi*3,10,dsp->color565(130,125,110));
  }
}

// Cabin (A-frame log cabin, elevated deck, American flag, from photo)
void drawCabinHouse(int ox, int oy){
  uint16_t log1  =dsp->color565(180,110,50);  // brighter log warm brown
  uint16_t log2  =dsp->color565(140,82,32);   // log shadow
  uint16_t roof1 =dsp->color565(160,80,30);   // brighter roof brown
  uint16_t roof2 =dsp->color565(120,55,18);   // roof shadow
  uint16_t deck  =dsp->color565(200,135,65);  // brighter deck planks
  uint16_t rail  =dsp->color565(160,100,45);  // railing posts
  uint16_t win   =dsp->color565(255,235,140); // glowing yellow window
  uint16_t winFr =dsp->color565(220,190,140); // brighter window frame
  uint16_t stone =dsp->color565(180,170,150); // brighter stone/chimney
  uint16_t flagR =dsp->color565(255,55,55);   // bright flag red
  uint16_t poleC =dsp->color565(220,200,150); // flag pole

  // ROOF: steep A-frame - NARROW at top, WIDE at bottom (correct)
  // At i=0: just the peak pixel; at i=15: full width
  for(int i=0;i<16;i++){
    int w=max(2,i*3+2); if(w>52)w=52;
    int lx=ox+27-w/2;
    dsp->drawFastHLine(lx, oy+i, w, (i%2==0)?roof1:roof2);
  }
  // Roof edge cap
  dsp->drawFastHLine(ox+2, oy+16, 50, dsp->color565(80,38,10));

  // CHIMNEY right side, rises above roof
  fillRect(ox+46, oy+4, 6, 14, stone);
  dsp->drawFastHLine(ox+45, oy+3, 8, dsp->color565(90,82,72)); // cap

  // DORMER - center of roof
  fillRect(ox+20, oy+5, 14, 10, log1);
  fillRect(ox+23, oy+6, 8, 7, win);
  dsp->drawRect(ox+23, oy+6, 8, 7, winFr);
  // Dormer mini-roof
  for(int i=0;i<4;i++) dsp->drawFastHLine(ox+20+i,oy+1+i,14-i*2,roof1);

  // MAIN BODY (log walls, oy+17 to oy+34)
  fillRect(ox+2, oy+17, 52, 17, log1);
  // Log lines
  for(int ly=oy+19;ly<oy+34;ly+=4){
    dsp->drawFastHLine(ox+2,ly,52,log2);
    dsp->drawFastHLine(ox+2,ly+1,52,dsp->color565(112,64,26));
  }
  // Corner logs
  dsp->drawFastVLine(ox+2,oy+17,17,log2);
  dsp->drawFastVLine(ox+53,oy+17,17,log2);

  // WINDOWS (oy+19)
  fillRect(ox+6, oy+19, 10, 9, win);
  dsp->drawRect(ox+6, oy+19, 10, 9, winFr);
  dsp->drawFastVLine(ox+11,oy+19,9,winFr);
  dsp->drawFastHLine(ox+6,oy+23,10,winFr);

  fillRect(ox+40, oy+19, 10, 9, win);
  dsp->drawRect(ox+40, oy+19, 10, 9, winFr);
  dsp->drawFastVLine(ox+45,oy+19,9,winFr);
  dsp->drawFastHLine(ox+40,oy+23,10,winFr);

  // ELEVATED DECK / WRAP-AROUND PORCH (oy+34)
  fillRect(ox, oy+34, 56, 4, deck);
  // Deck boards
  for(int dx=ox;dx<ox+56;dx+=4) dsp->drawFastVLine(dx,oy+34,4,dsp->color565(120,75,32));
  // Porch posts / railing
  for(int pi=0;pi<12;pi++) dsp->drawFastVLine(ox+1+pi*5,oy+34,7,rail);
  // Railing top bar
  dsp->drawFastHLine(ox,oy+34,56,dsp->color565(165,120,65));
  dsp->drawFastHLine(ox,oy+40,56,dsp->color565(108,68,32));

  // DECK SUPPORTS (stilts/posts going down)
  for(int sp=0;sp<4;sp++) dsp->drawFastVLine(ox+6+sp*16,oy+38,6,log2);

  // STONE STEPS
  dsp->drawFastHLine(ox+16,oy+39,12,stone);
  dsp->drawFastHLine(ox+18,oy+41,10,stone);
  dsp->drawFastHLine(ox+20,oy+43,8,stone);

  // FLAG POLE left of door area
  dsp->drawFastVLine(ox+24,oy+8,16,poleC);
  fillRect(ox+25,oy+8,10,5,flagR);
  dsp->drawFastHLine(ox+25,oy+9,10,C_WHITE);
  dsp->drawFastHLine(ox+25,oy+11,10,C_WHITE);

  // HANGING FLOWER BASKETS on porch
  dsp->fillCircle(ox+12,oy+36,3,dsp->color565(40,160,40));
  dsp->fillCircle(ox+44,oy+36,3,dsp->color565(40,160,40));
  // Hanging chains
  dsp->drawFastVLine(ox+12,oy+32,4,dsp->color565(100,80,50));
  dsp->drawFastVLine(ox+44,oy+32,4,dsp->color565(100,80,50));
}

// =====================================================================
// v10 DESIGN SYSTEM — one consistent card chrome across all data cards.
//
// Every data card gets the same anatomy:
//   y=0..12  header band in a DIMMED version of the card's accent color
//            [16px slide icon][title, size-1 white][clock OR page pips, right]
//   y=13     1px accent underline
//   y=15+    card content (unchanged per card)
//
// This replaces a dozen slightly-different hand-rolled headers, which is
// what made v9 feel patchworky. One helper, one look.
// =====================================================================

// Scale an RGB565 color to pct% brightness (0..100).
uint16_t dimColor565(uint16_t c, uint8_t pct){
  uint8_t r = ((c >> 11) & 0x1F) * 255 / 31;
  uint8_t g = ((c >> 5)  & 0x3F) * 255 / 63;
  uint8_t b = ( c        & 0x1F) * 255 / 31;
  return dsp->color565((uint8_t)(r*pct/100), (uint8_t)(g*pct/100), (uint8_t)(b*pct/100));
}

// Unified card header.
//   iconKey    slide_icons.h key ("news", "finance", …); text-only if missing
//   title      size-1, drawn white beside the icon
//   accent     card accent color; band = 30% dim of it, underline = full
//   pages      >1 → page pips top-right; 0 → clock top-right; -1 → nothing
//              (use -1 when the card draws its own right-side content)
//   activePage which pip is lit (0-based)
void drawCardHeader(const char* iconKey, const char* title, uint16_t accent,
                    int pages = 0, int activePage = 0){
  fillRect(0, 0, PANEL_WIDTH, 13, dimColor565(accent, 30));
  dsp->drawFastHLine(0, 13, PANEL_WIDTH, accent);
  int tx = 2;
  if(drawSlideIcon(iconKey, 0, -2, 16)) tx = 18;   // icon hangs 1px over band
  txt1(title, tx, 3, C_WHITE);

  if(pages > 1){
    // Page pips right-aligned in the band
    int x0 = PANEL_WIDTH - pages*6 - 2;
    for(int i = 0; i < pages; i++)
      dsp->fillCircle(x0 + i*6 + 2, 6, 1,
        (i == activePage) ? C_WHITE : dimColor565(accent, 55));
  } else if(pages == 0){
    // Live clock right-aligned
    struct tm ti;
    if(getLocalTime(&ti)){
      char tb[8]; strftime(tb, 8, "%I:%M", &ti);
      char* p = (tb[0] == '0') ? tb + 1 : tb;
      int w = (int)strlen(p) * 6;
      txt1(p, PANEL_WIDTH - w - 2, 3, dimColor565(accent, 85));
    }
  }
  // pages == -1: caller owns the right side of the band
}

// =====================================================================
// SLIDE RENDERERS
// =====================================================================

// ----------------------------------------------------------
// News: one story at a time, headline scrolls across, 10s per story
static int   newsScrollX  = 128;
static unsigned long newsScrollTick = 0;
static int   newsStoryIdx = 0;
static unsigned long newsStoryStart = 0;
// Set true once every story has scrolled by once since the slide opened —
// the main loop reads this to advance early so all headlines are seen.
bool newsCycleDone  = false;
bool lNewsCycleDone = false;
// Scroll speed (px per 33ms tick). 3 = ~50% faster than the old 2.
#define NEWS_SCROLL_STEP 3

// News card — full story scrolls fully off-screen before advancing.
// No "UP NEXT" preview band any more (per user request). Bottom freed up
// for a larger headline scroll zone + page indicators only.
void renderNews() {
  bool newSlide = (lastStaticDraw == 0);
  if(newSlide){
    Serial.printf("[NEWS] entry heap=%u count=%d idx=%d\n",
      (unsigned)ESP.getFreeHeap(), newsCount, newsStoryIdx);
    cls();
    // v10 unified chrome
    drawCardHeader("news", "NEWS", C_BLUE);
    // Source label band y=15..24
    fillRect(0, 15, PANEL_WIDTH, 10, dsp->color565(5,5,25));
    txt1("TOP HEADLINES", 2, 17, dsp->color565(80,100,180));
    // Body zone y=26..56 stays black (will hold scrolling headline)
    fillRect(0, 26, PANEL_WIDTH, 30, C_BLACK);
    // Page-dots band y=58..63
    fillRect(0, 56, PANEL_WIDTH, 8, C_BLACK);
    dsp->drawFastHLine(0, 56, PANEL_WIDTH, C_DARKGRAY);
    lastStaticDraw = millis();
    newsScrollX = 128;
    // Restart from story 0 each visit so we can detect a full cycle.
    newsStoryIdx = 0;
    newsCycleDone = false;
  }

  if(newsCount == 0){
    if(newSlide) ctrTxt1("Loading...", 38, C_GRAY);
    return;
  }

  // Update header counter when story changes — unified chrome, story counter
  // drawn beside the title
  static int lastDrawnIdx = -1;
  if(lastDrawnIdx != newsStoryIdx){
    lastDrawnIdx = newsStoryIdx;
    drawCardHeader("news", "NEWS", C_BLUE);
    char sc[8]; snprintf(sc, 8, "%d/%d", newsStoryIdx+1, newsCount);
    txt1(sc, 46, 3, dimColor565(C_BLUE, 70));
    // Redraw page dots
    fillRect(0, 57, PANEL_WIDTH, 7, C_BLACK);
    int dCount = min(newsCount, 10);
    for(int i = 0; i < dCount; i++){
      dsp->fillCircle(64 - dCount*3 + i*6, 60, 2,
        (i == newsStoryIdx) ? C_WHITE : C_DARKGRAY);
    }
  }

  // Scroll band y=28..49 (bigger now that preview is gone — fits size-2 nicely)
  fillRect(0, 28, PANEL_WIDTH, 22, C_BLACK);

  // Move scroll position
  const String &hRef = news[newsStoryIdx % max(newsCount, 1)].headline;
  int textW = (int)hRef.length() * 12;     // size-2 chars are 12px wide
  if(millis() - newsScrollTick > 33){
    newsScrollX -= NEWS_SCROLL_STEP;
    newsScrollTick = millis();
    // ── ADVANCE only after the FULL headline has scrolled off the left
    // edge. When we wrap past the last story, flag the cycle complete so
    // the main loop can advance the slide — guarantees every headline is
    // seen rather than getting cut off by a fixed slide timer.
    if(newsScrollX < -textW){
      newsStoryIdx++;
      if(newsStoryIdx >= newsCount){ newsStoryIdx = 0; newsCycleDone = true; }
      newsScrollX  = 128;
      newsStoryStart = millis();
    }
  }

  // Render the scrolling headline (size 2) at y=30
  dsp->setTextSize(2); dsp->setTextWrap(false);
  dsp->setTextColor(C_WHITE);
  dsp->setCursor(newsScrollX, 30); dsp->print(hRef.c_str());
  dsp->setTextSize(1);

  // Progress bar: how far through this scroll we are (0..1)
  // Total scroll distance = 128 (entry) + textW (exit) = 128 + textW
  int total = 128 + textW;
  int done  = 128 - newsScrollX;
  if(done < 0) done = 0;
  if(done > total) done = total;
  int barW = total > 0 ? (done * PANEL_WIDTH / total) : 0;
  fillRect(0, 51, barW, 1, C_BLUE);
  fillRect(barW, 51, PANEL_WIDTH-barW, 1, dsp->color565(20,20,40));
}

// ----------------------------------------------------------
// Local News (Stamford CT) - same layout as national news but teal accent
static int   lNewsScrollX  = 128;
static unsigned long lNewsScrollTick = 0;
static int   lNewsStoryIdx = 0;
static unsigned long lNewsStoryStart = 0;

// Stamford News card — same full-scroll-then-advance pattern as national news.
// Header label changed from "STMFD" to full "STAMFORD NEWS" per user request.
// No "UP NEXT" preview band any more — bigger scroll zone instead.
void renderLocalNews() {
  bool newSlide = (lastStaticDraw == 0);
  uint16_t teal = dsp->color565(0, 80, 75);
  if(newSlide){
    Serial.printf("[LNEWS] entry heap=%u count=%d idx=%d\n",
      (unsigned)ESP.getFreeHeap(), localNewsCount, lNewsStoryIdx);
    cls();
    // v10 unified chrome
    drawCardHeader("localnews", "STAMFORD", C_TEAL);
    // Source band y=15..24
    fillRect(0, 15, PANEL_WIDTH, 10, dsp->color565(5, 20, 18));
    txt1("LOCAL CT NEWS", 2, 17, dsp->color565(120, 200, 180));
    // Body y=26..56 black
    fillRect(0, 26, PANEL_WIDTH, 30, C_BLACK);
    // Bottom band
    fillRect(0, 56, PANEL_WIDTH, 8, C_BLACK);
    dsp->drawFastHLine(0, 56, PANEL_WIDTH, C_DARKGRAY);
    lastStaticDraw = millis();
    lNewsScrollX = 128;
    lNewsStoryIdx = 0;
    lNewsCycleDone = false;
  }

  if(localNewsCount == 0){
    if(newSlide){
      ctrTxt1("Loading...", 32, C_GRAY);
      ctrTxt1("(Stamford CT)", 42, C_DARKGRAY);
    }
    return;
  }

  // Update header counter when story changes — unified chrome
  static int lastDrawnLIdx = -1;
  if(lastDrawnLIdx != lNewsStoryIdx){
    lastDrawnLIdx = lNewsStoryIdx;
    drawCardHeader("localnews", "STAMFORD", C_TEAL);
    char sc[8]; snprintf(sc, 8, "%d/%d", lNewsStoryIdx+1, localNewsCount);
    txt1(sc, 70, 3, dimColor565(C_TEAL, 70));
    // Page dots
    fillRect(0, 57, PANEL_WIDTH, 7, C_BLACK);
    int dCount = min(localNewsCount, 10);
    for(int i = 0; i < dCount; i++){
      dsp->fillCircle(64 - dCount*3 + i*6, 60, 2,
        (i == lNewsStoryIdx) ? C_TEAL : C_DARKGRAY);
    }
  }

  // Scroll band y=28..49
  fillRect(0, 28, PANEL_WIDTH, 22, C_BLACK);

  const String &lhRef = localNews[lNewsStoryIdx % max(localNewsCount, 1)].headline;
  int textW = (int)lhRef.length() * 12;
  if(millis() - lNewsScrollTick > 33){
    lNewsScrollX -= NEWS_SCROLL_STEP;
    lNewsScrollTick = millis();
    if(lNewsScrollX < -textW){
      lNewsStoryIdx++;
      if(lNewsStoryIdx >= localNewsCount){ lNewsStoryIdx = 0; lNewsCycleDone = true; }
      lNewsScrollX  = 128;
      lNewsStoryStart = millis();
    }
  }
  dsp->setTextSize(2); dsp->setTextWrap(false);
  dsp->setTextColor(dsp->color565(180, 255, 240));
  dsp->setCursor(lNewsScrollX, 30); dsp->print(lhRef.c_str());
  dsp->setTextSize(1);

  // Progress bar by scroll fraction
  int total = 128 + textW;
  int done  = 128 - lNewsScrollX;
  if(done < 0) done = 0;
  if(done > total) done = total;
  int barW = total > 0 ? (done * PANEL_WIDTH / total) : 0;
  fillRect(0, 51, barW, 1, C_TEAL);
  fillRect(barW, 51, PANEL_WIDTH-barW, 1, dsp->color565(10, 30, 28));
}

// ----------------------------------------------------------
// Sports animation - homerun, TD, basketball, hockey
// Sports transition — bold logo cards for the 4 Boston favorites.
// Cycles through Red Sox / Patriots / Celtics / Bruins, randomized per slide.
// Each is logo-centric with team colors, no busy action scenes.
void drawPixelRing(int cx, int cy, int r, uint16_t c) {
  dsp->drawCircle(cx,cy,r,c);
  dsp->drawCircle(cx,cy,r-1,c);
}

void drawSoxMark(int cx, int cy) {
  uint16_t navy=dsp->color565(12,32,70), red=dsp->color565(215,20,38);
  dsp->fillCircle(cx,cy,13,navy); drawPixelRing(cx,cy,13,C_WHITE);
  fillRect(cx-7,cy-7,5,13,red); fillRect(cx+2,cy-7,5,13,red);
  fillRect(cx-8,cy+5,7,4,red); fillRect(cx+1,cy+5,8,4,red);
  dsp->drawFastHLine(cx-8,cy+9,8,C_WHITE); dsp->drawFastHLine(cx+1,cy+9,9,C_WHITE);
  dsp->drawFastHLine(cx-7,cy-8,5,C_WHITE); dsp->drawFastHLine(cx+2,cy-8,5,C_WHITE);
}

void drawNYMark(int cx, int cy) {
  uint16_t blue=dsp->color565(0,45,110), orange=dsp->color565(255,95,15);
  dsp->fillCircle(cx,cy,13,blue); drawPixelRing(cx,cy,13,orange);
  dsp->drawFastVLine(cx-7,cy-8,16,C_WHITE); dsp->drawLine(cx-7,cy-8,cx,cy+7,C_WHITE); dsp->drawFastVLine(cx,cy-8,16,C_WHITE);
  dsp->drawFastVLine(cx+4,cy-8,8,C_WHITE); dsp->drawLine(cx+4,cy,cx+10,cy-8,C_WHITE); dsp->drawLine(cx+4,cy,cx+10,cy+8,C_WHITE);
  dsp->drawPixel(cx-6,cy-8,orange); dsp->drawPixel(cx+1,cy+7,orange);
}

void drawPatsMark(int cx, int cy) {
  uint16_t navy=dsp->color565(0,24,55), red=dsp->color565(198,12,48), silver=dsp->color565(202,210,218);
  dsp->fillCircle(cx,cy,13,navy); drawPixelRing(cx,cy,13,red);
  dsp->drawFastHLine(cx-10,cy-2,17,silver); dsp->drawFastHLine(cx-8,cy+1,15,silver); dsp->drawFastHLine(cx-5,cy+4,11,silver);
  dsp->drawLine(cx+5,cy-6,cx+11,cy-2,silver); dsp->drawLine(cx+5,cy+5,cx+11,cy+2,silver);
  dsp->drawFastHLine(cx-7,cy-1,12,navy); dsp->drawFastHLine(cx-5,cy+2,10,navy);
  dsp->drawPixel(cx+4,cy-4,C_WHITE); dsp->drawPixel(cx+5,cy-4,C_WHITE); dsp->drawPixel(cx+6,cy-3,red);
}

void drawCelticsMark(int cx, int cy) {
  uint16_t green=dsp->color565(0,105,48), leaf=dsp->color565(0,150,70), stem=dsp->color565(186,150,83);
  dsp->fillCircle(cx,cy,13,green); drawPixelRing(cx,cy,13,C_WHITE);
  dsp->fillCircle(cx,cy-5,4,C_WHITE); dsp->fillCircle(cx-5,cy,4,C_WHITE); dsp->fillCircle(cx+5,cy,4,C_WHITE);
  dsp->fillCircle(cx,cy-5,3,leaf); dsp->fillCircle(cx-5,cy,3,leaf); dsp->fillCircle(cx+5,cy,3,leaf);
  dsp->drawLine(cx,cy+2,cx,cy+9,stem); dsp->drawLine(cx,cy+5,cx-4,cy+8,stem); dsp->drawLine(cx,cy+5,cx+4,cy+8,stem);
}

void drawBruinsMark(int cx, int cy) {
  dsp->fillCircle(cx,cy,13,C_BLACK); drawPixelRing(cx,cy,13,C_GOLD); drawPixelRing(cx,cy,9,C_GOLD);
  for(int s=0;s<8;s++){ float a=s*PI/4.0f; dsp->drawLine(cx+cos(a)*3,cy+sin(a)*3,cx+cos(a)*12,cy+sin(a)*12,C_GOLD); }
  fillRect(cx-4,cy-7,7,15,C_GOLD); fillRect(cx-2,cy-5,4,11,C_BLACK);
  dsp->drawFastHLine(cx-4,cy-7,8,C_GOLD); dsp->drawFastHLine(cx-4,cy,9,C_GOLD); dsp->drawFastHLine(cx-4,cy+7,8,C_GOLD);
  dsp->drawFastVLine(cx+4,cy-5,5,C_GOLD); dsp->drawFastVLine(cx+4,cy+2,5,C_GOLD);
}

// ============================================================
// FULL-PAGE TEAM LOGO RENDERERS
// Each is a 128×64 iconic representation of the team logo. BG drawn ONCE
// on slide entry; per-frame work is limited to one small animated element
// (baseball / star twinkle / basketball / hockey puck) so the screen stays
// smooth like the aurora — small per-frame deltas, no full-screen redraw.
// ============================================================

// Big "B" used by Red Sox + Bruins logos.
// 22px wide × 28px tall, drawn at (cx-11, cy-14) → fills (cx-11..cx+11)
void drawBigLetterB(int cx, int cy, uint16_t fg, uint16_t outline){
  // Vertical spine — 4px wide
  fillRect(cx-9, cy-13, 4, 27, fg);
  // Top loop — outer
  fillRect(cx-5, cy-13, 9, 4, fg);
  fillRect(cx+1, cy-9,  4, 4, fg);
  fillRect(cx-5, cy-5,  9, 4, fg);
  // Bottom loop — outer
  fillRect(cx-5, cy-1,  9, 4, fg);
  fillRect(cx+1, cy+3,  4, 4, fg);
  fillRect(cx-5, cy+7,  9, 4, fg);
  fillRect(cx-9, cy+11, 13, 3, fg);
  // Outline (single px around)
  dsp->drawRect(cx-10, cy-14, 16, 28, outline);
  dsp->drawPixel(cx+5,  cy-13, outline);
  dsp->drawPixel(cx+5,  cy-9,  outline);
  dsp->drawPixel(cx+5,  cy-5,  outline);
  dsp->drawPixel(cx+5,  cy-1,  outline);
  dsp->drawPixel(cx+5,  cy+3,  outline);
  dsp->drawPixel(cx+5,  cy+7,  outline);
}

// Big crossed red baseball socks — Boston Red Sox iconic logo. No B,
// per reference: just the two socks crossed at the cuff. Drawn at scale
// for full-page presentation: sock body ~28 px tall × 8 wide each.
void drawCrossedSocks(int cx, int cy){
  uint16_t soxRed  = dsp->color565(225, 30, 38);
  uint16_t soxRedD = dsp->color565(170, 18, 22);   // shadow on heel
  uint16_t cream   = dsp->color565(245,235,205);

  // ── LEFT SOCK ── Angles down-left from cuff at (cx-2, cy-13) to toe at (cx-13, cy+10)
  // Cuff (white band, 8 wide × 4 tall)
  fillRect(cx-6, cy-15, 8, 4, cream);
  dsp->drawFastHLine(cx-6, cy-11, 8, dsp->color565(200, 195, 170));   // cuff shadow

  // Sock body — angled diagonal from cuff toward toe
  for(int i=0; i<22; i++){
    int sx = cx - 2 - i*0.55f;        // moves left as we go down
    int sy = cy - 11 + i;
    fillRect((int)sx, sy, 8, 1, soxRed);
    // Heel shadow on outside
    dsp->drawPixel((int)sx, sy, soxRedD);
  }
  // Toe — flat horizontal cap
  fillRect(cx-15, cy+11, 9, 3, soxRed);
  dsp->drawFastHLine(cx-15, cy+13, 9, soxRedD);

  // ── RIGHT SOCK ── Mirror — angles down-right from cuff
  fillRect(cx-2, cy-15, 8, 4, cream);
  dsp->drawFastHLine(cx-2, cy-11, 8, dsp->color565(200, 195, 170));

  for(int i=0; i<22; i++){
    int sx = cx - 6 + i*0.55f;
    int sy = cy - 11 + i;
    fillRect((int)sx, sy, 8, 1, soxRed);
    dsp->drawPixel((int)sx + 7, sy, soxRedD);
  }
  fillRect(cx+6, cy+11, 9, 3, soxRed);
  dsp->drawFastHLine(cx+6, cy+13, 9, soxRedD);

  // Crossing point — the two cuffs visually cross at the top
  // Small overlap shadow at cuff intersection
  dsp->drawPixel(cx-1, cy-13, dsp->color565(180,170,150));
  dsp->drawPixel(cx,   cy-13, dsp->color565(180,170,150));
  dsp->drawPixel(cx+1, cy-13, dsp->color565(180,170,150));
}

// Big Patriot face profile facing right (Pat-the-Patriot style).
// 36px wide × 30px tall, centered at (cx, cy)
void drawBigPatFace(int cx, int cy){
  uint16_t neNavy = dsp->color565(0, 34, 68);
  uint16_t neRed  = dsp->color565(198, 12, 48);
  uint16_t skin   = dsp->color565(225, 200, 175);
  uint16_t skinSh = dsp->color565(180, 150, 125);
  uint16_t hatHi  = dsp->color565(40, 60, 100);

  // ── TRICORN HAT (top) ──
  // Triangular shape pointing up-left and up-right with points
  // Hat brim at y=cy-12, crown rises to y=cy-18
  for(int y=cy-18; y<=cy-12; y++){
    int w = 24 - (cy-12-y)*2;        // narrows toward top
    fillRect(cx-w/2, y, w, 1, neNavy);
  }
  // Hat brim — wide horizontal stripe
  fillRect(cx-15, cy-12, 30, 3, neNavy);
  // Hat tip points (tricorn shape) — small triangles top-left + top-right
  dsp->drawPixel(cx-13, cy-13, neNavy); dsp->drawPixel(cx-12, cy-14, neNavy);
  dsp->drawPixel(cx+13, cy-13, neNavy); dsp->drawPixel(cx+12, cy-14, neNavy);
  // Highlight on hat
  dsp->drawFastHLine(cx-10, cy-17, 8, hatHi);
  // Red bandana band under brim
  fillRect(cx-15, cy-9, 30, 3, neRed);
  // White trim line under bandana
  dsp->drawFastHLine(cx-15, cy-6, 30, dsp->color565(240,240,240));

  // ── FACE PROFILE (facing right) ──
  // Forehead/nose/chin profile, drawn as a rightward-facing silhouette
  // Forehead curve
  fillRect(cx-13, cy-5, 4, 5, skin);
  dsp->drawPixel(cx-14, cy-4, skin);
  dsp->drawPixel(cx-14, cy-3, skin);
  // Bridge to nose — pokes out at front
  fillRect(cx-9, cy-4, 8, 4, skin);
  // Nose tip
  dsp->drawPixel(cx-1, cy-3, skin);
  dsp->drawPixel(cx,    cy-3, skin);
  dsp->drawPixel(cx,    cy-2, skin);
  // Mouth/chin
  fillRect(cx-9, cy+0, 8, 4, skin);
  dsp->drawPixel(cx-2, cy+1, skinSh);   // lip line
  dsp->drawPixel(cx-1, cy+1, skinSh);
  // Chin lower
  fillRect(cx-9, cy+4, 6, 3, skin);
  dsp->drawPixel(cx-3, cy+5, skinSh);   // chin shadow
  // Eye dot
  dsp->drawPixel(cx-9, cy-3, dsp->color565(20,20,30));
  // Brow line
  dsp->drawFastHLine(cx-11, cy-5, 4, dsp->color565(40,30,20));

  // ── HAT TASSELS (red, hanging behind hat) ──
  for(int i=0; i<6; i++){
    fillRect(cx+12, cy-15+i*2, 3, 2, neRed);
  }
}

// Boston Bruins iconic spoked-B logo — drawn at 25 px radius for max
// readability on a 64px-tall panel. Reference: gold outer ring, white
// inner disc with 6 thick gold spokes, bold black B with gold trim center.
void drawBigBruinsB(int cx, int cy){
  uint16_t bruGold = dsp->color565(252, 181, 20);
  uint16_t bruBlk  = dsp->color565(8, 8, 12);
  uint16_t bruWhite= dsp->color565(245, 245, 230);
  int R = 25;          // outer rim radius

  // Outer gold ring — fill big disc, then carve out white inner
  dsp->fillCircle(cx, cy, R,    bruGold);
  // White annulus (band of white between two gold rings)
  dsp->fillCircle(cx, cy, R-3,  bruWhite);
  // Inner gold mini-ring around the B
  dsp->fillCircle(cx, cy, R-9,  bruGold);
  dsp->fillCircle(cx, cy, R-10, bruWhite);

  // Six thick gold spokes from inner ring to outer rim, crossing the
  // white annulus. Each spoke is 3 px thick.
  for(int s = 0; s < 6; s++){
    float a = s * (PI / 3.0f);
    float ca = cosf(a), sa = sinf(a);
    // Perpendicular offset for thickness
    float pxn = -sa, pyn = ca;
    for(int r = R-10; r <= R; r++){
      int x = cx + (int)(r * ca);
      int y = cy + (int)(r * sa);
      dsp->drawPixel(x, y, bruGold);
      dsp->drawPixel(x + (int)pxn, y + (int)pyn, bruGold);
      dsp->drawPixel(x - (int)pxn, y - (int)pyn, bruGold);
    }
  }

  // Center black disc that holds the B
  dsp->fillCircle(cx, cy, R-12, bruBlk);
  dsp->drawCircle(cx, cy, R-12, bruGold);

  // Bold black B with gold outline — drawn pixel-art style for clarity.
  // 14 px wide × 18 px tall. Centered at (cx, cy).
  // Outer gold halo first, then black B on top.
  uint16_t fg = bruGold;
  uint16_t b  = bruBlk;
  // Halo (slightly bigger than B)
  fillRect(cx-7, cy-10, 13, 21, fg);

  // Carve out the B shape in black
  // Vertical spine (left side)
  fillRect(cx-5, cy-9, 3, 19, b);
  // Inner negative spaces (left hollow column)
  fillRect(cx-2, cy-7, 4, 4, b);   // top hollow
  fillRect(cx-2, cy+1, 4, 4, b);   // bottom hollow

  // Now repaint just the gold "B" outline pixels (around the carved area)
  // Top loop outer edge
  fillRect(cx-5, cy-9, 9, 2, fg);
  // Bottom loop outer edge
  fillRect(cx-5, cy+7, 9, 3, fg);
  // Middle pinch
  fillRect(cx-5, cy-1, 9, 2, fg);
  // Right edges
  fillRect(cx+2, cy-7, 2, 4, fg);
  fillRect(cx+2, cy+1, 2, 4, fg);
  // Left spine (re-paint)
  fillRect(cx-5, cy-9, 3, 19, fg);
}

// Big shamrock logo for Celtics — 3 leaves + stem, ~26px tall
void drawBigShamrock(int cx, int cy){
  uint16_t leaf    = dsp->color565(40, 175, 80);
  uint16_t leafHi  = dsp->color565(80, 220, 110);
  uint16_t leafSh  = dsp->color565(20, 130, 55);
  uint16_t stem    = dsp->color565(170, 130, 60);

  // Three leaves
  // Top leaf
  dsp->fillCircle(cx,    cy-7, 6, leaf);
  dsp->fillCircle(cx-1,  cy-8, 4, leafHi);
  // Bottom-left leaf
  dsp->fillCircle(cx-7,  cy+1, 6, leaf);
  dsp->fillCircle(cx-8,  cy+0, 4, leafHi);
  // Bottom-right leaf
  dsp->fillCircle(cx+7,  cy+1, 6, leaf);
  dsp->fillCircle(cx+6,  cy+0, 4, leafHi);
  // Center darkening (where leaves overlap)
  dsp->fillCircle(cx, cy-2, 2, leafSh);
  // Stem — slight curve, 3px thick
  fillRect(cx-1, cy+5, 3, 8, stem);
  dsp->drawPixel(cx+2, cy+9, stem);
  dsp->drawPixel(cx+3, cy+11, stem);
  // Leaf vein hints
  dsp->drawPixel(cx,    cy-9, leafSh);
  dsp->drawPixel(cx-9,  cy+1, leafSh);
  dsp->drawPixel(cx+9,  cy+1, leafSh);
}

// ============================================================
// renderSportsAnim — pick a team, draw full-page logo, animate one element
// ============================================================
// ============================================================
// renderTitle — animated "DEWS FEED" brand intro card.
// Slide 0 of the day playlist; runs ~7s on each loop.
//
// Animation phases (driven by ms-since-slide-start):
//   0..1500 ms : sweep gradient backdrop in
//   1500..3500 ms : letters of "DEWS FEED" pop in one at a time, size-3,
//                   each with a brief flash-of-white that settles to gold
//   3500..7000 ms : steady banner with subtitle + version, soft starburst pulse
// ============================================================
// Title backdrop sampler — horizontal day→night gradient. Left edge is warm
// dawn, right edge is deep night. Used both for the once-only backdrop fill
// and for sprite-delta erasing of the sun/moon arc and the icon marquee.
uint16_t titleBgPixel(int x, int y){
  // Horizontal blend factor 0 (left/dawn) → 255 (right/night)
  uint8_t fx = (uint8_t)map(x, 0, 127, 0, 255);
  // Dawn column color (warm navy w/ orange base) vs night column (near black)
  uint8_t r = map(fx, 0, 255, 26,  0);
  uint8_t g = map(fx, 0, 255, 22,  2);
  uint8_t b = map(fx, 0, 255, 64, 22);
  // Vertical darkening toward the bottom
  r = (uint8_t)((r * (96 - y/2)) / 96);
  g = (uint8_t)((g * (96 - y/2)) / 96);
  b = (uint8_t)((b * (110 - y/3)) / 110);
  return dsp->color565(r, g, b);
}

// renderTitle — animated brand intro reflecting the feed's full scope:
//   0.0–7s  : sun rises, arcs across the top, hands off to the moon
//             (the feed's day/night cycle in one gesture)
//   0.8–2.8s: "DEWS FEED" letters pop in one at a time
//   2.8s+   : two-line subtitle (locations) + version
//   3.2s+   : continuous icon marquee across the bottom — news, sports,
//             finance, weather, transit, flights, calendar, health, moon —
//             everything the feed covers, scrolling by like a film strip.
void renderTitle(){
  bool newSlide = (lastStaticDraw == 0);
  static unsigned long startMs = 0;
  static int   lastFrameLetters = -1;
  static bool  subtitleDrawn = false;
  static float marqueeX = 0;
  static int   lastSunX = -99, lastSunY = -99;
  if(newSlide){
    startMs = millis();
    lastFrameLetters = -1;
    subtitleDrawn = false;
    marqueeX = 0;
    lastSunX = -99;
    lastStaticDraw = millis();
    cls();
    // Day→night horizontal gradient backdrop, drawn once via the sampler.
    for(int y=0; y<PANEL_HEIGHT; y++)
      for(int x=0; x<PANEL_WIDTH; x++)
        dsp->drawPixel(x, y, titleBgPixel(x, y));
    // Stars only on the night (right) half — deterministic
    const uint8_t SX[] = {70,82,95,108,118,124, 76,90,103,115};
    const uint8_t SY[] = {5,9,4,8,12,6,         14,11,15,10};
    for(int i=0;i<10;i++){
      uint8_t br = (i&1)?80:45;
      dsp->drawPixel(SX[i],SY[i],dsp->color565(br,br,br+15));
    }
  }

  unsigned long t = millis() - startMs;

  // ── SUN→MOON ARC across the top band (y=2..12, x sweeps 6→121) ──
  // The sun crosses the day half, morphs to a moon crossing the night half.
  static unsigned long lastArc = 0;
  if(millis() - lastArc > 90){
    lastArc = millis();
    // Position along the arc loops every ~7s
    float ph = (t % 7000UL) / 7000.0f;          // 0..1
    int ax = 6 + (int)(ph * 115);
    int ay = 9 - (int)(sinf(ph * PI) * 6);      // gentle arc, peak at center
    // Erase OLD body (5×5) via sampler, restore any stars beneath
    if(lastSunX > -90){
      for(int dy=-2; dy<=2; dy++)
        for(int dx=-2; dx<=2; dx++){
          int xx=lastSunX+dx, yy=lastSunY+dy;
          if(xx<0||xx>=PANEL_WIDTH||yy<0||yy>17) continue;
          dsp->drawPixel(xx, yy, titleBgPixel(xx, yy));
        }
      const uint8_t SX[] = {70,82,95,108,118,124, 76,90,103,115};
      const uint8_t SY[] = {5,9,4,8,12,6,         14,11,15,10};
      for(int i=0;i<10;i++){
        if(abs(SX[i]-lastSunX)<=3 && abs(SY[i]-lastSunY)<=3){
          uint8_t br=(i&1)?80:45;
          dsp->drawPixel(SX[i],SY[i],dsp->color565(br,br,br+15));
        }
      }
    }
    // Draw NEW body: sun on day half, moon on night half
    if(ax < 64){
      dsp->fillCircle(ax, ay, 2, dsp->color565(255,210,80));
      dsp->drawPixel(ax-3, ay, dsp->color565(150,110,30));
      dsp->drawPixel(ax+3, ay, dsp->color565(150,110,30));
      dsp->drawPixel(ax, ay-3, dsp->color565(150,110,30));
    } else {
      dsp->fillCircle(ax, ay, 2, dsp->color565(210,215,230));
      dsp->drawPixel(ax+1, ay-1, dsp->color565(120,125,145)); // crater hint
    }
    lastSunX = ax; lastSunY = ay;
  }

  // ── "DEWS FEED" letters pop in (800..2800ms) ──
  const char* line = "DEWS FEED";
  const int LCHARS = 9;
  const int CHARW = 12;
  const int startX = (PANEL_WIDTH - LCHARS*CHARW) / 2;
  int letters;
  if(t < 800)       letters = 0;
  else if(t < 2800) letters = (int)((t - 800) / 222) + 1;
  else              letters = LCHARS;
  if(letters > LCHARS) letters = LCHARS;

  if(letters != lastFrameLetters){
    // Title strip y=18..35
    for(int y=18; y<36; y++)
      for(int x=0; x<PANEL_WIDTH; x++)
        dsp->drawPixel(x, y, titleBgPixel(x, y));
    dsp->drawFastHLine(8, 35, PANEL_WIDTH-16, dsp->color565(80, 60, 0));
    dsp->setTextSize(2);
    for(int i=0; i<letters; i++){
      bool fresh = (i == letters-1) && (t < 2800);
      dsp->setTextColor(fresh ? C_WHITE : C_GOLD);
      dsp->setCursor(startX + i*CHARW, 19);
      dsp->print(line[i]);
    }
    dsp->setTextSize(1);
    lastFrameLetters = letters;
  }

  // ── Subtitle: the two locations + version (2800ms+) ──
  if(t >= 2800 && !subtitleDrawn){
    ctrTxt1("STAMFORD + THE LAKE", 38, dsp->color565(160,180,220));
    txt1(DEWS_FEED_VERSION, PANEL_WIDTH-(int)strlen(DEWS_FEED_VERSION)*6-2, 38,
         dsp->color565(110,170,190));
    subtitleDrawn = true;
  }

  // ── ICON MARQUEE strip y=47..63 (3200ms+) ──
  // Slide-icon film strip scrolling right→left. Each icon 16px + 6px gap.
  // The whole strip is re-painted from the sampler each tick — 128×17 px of
  // gradient + up to 6 icon blits ≈ small enough to stay smooth at ~12fps.
  if(t >= 3200){
    static unsigned long lastMq = 0;
    if(millis() - lastMq > 80){
      lastMq = millis();
      marqueeX += 1.0f;
      static const char* MQ[] = {
        "news","scores","finance","weather","traffic","train",
        "flights","calendar","whoop","concert","moon","tiki"
      };
      const int MQN = 12, STEP = 22;                  // 16px icon + 6px gap
      int total = MQN * STEP;
      // Repaint strip background
      for(int y=47; y<64; y++)
        for(int x=0; x<PANEL_WIDTH; x++)
          dsp->drawPixel(x, y, titleBgPixel(x, y));
      dsp->drawFastHLine(0, 47, PANEL_WIDTH, dsp->color565(50, 45, 20));
      // Blit visible icons
      int off = (int)marqueeX % total;
      for(int i=0; i<MQN; i++){
        int ix = i*STEP - off;
        if(ix < -16) ix += total;                     // wrap
        if(ix > PANEL_WIDTH) continue;
        drawSlideIcon(MQ[i], ix, 48, 16);
      }
    }
  }
}

// ============================================================
// renderWorkoutAnim — short transition card before WHOOP slide.
// Animated dumbbell + pulsing "WORKOUT" header for ~6s, no API.
// ============================================================
void renderWorkoutAnim(){
  bool newSlide = (lastStaticDraw == 0);
  static unsigned long startMs = 0;
  static uint8_t pulse = 0;

  if(newSlide){
    startMs = millis();
    pulse = 0;
    lastStaticDraw = millis();
    cls();
    // Dark crimson → black gradient backdrop
    for(int y=0; y<PANEL_HEIGHT; y++){
      uint8_t r = map(y, 0, 63, 60, 8);
      uint8_t g = map(y, 0, 63,  6, 0);
      uint8_t b = map(y, 0, 63, 14, 4);
      dsp->drawFastHLine(0,y,PANEL_WIDTH,dsp->color565(r,g,b));
    }
    // "WORKOUT" header (top, size-2 white)
    fillRect(0,0,PANEL_WIDTH,14,dsp->color565(120,12,16));
    ctrTxt2("WORKOUT", 0, C_WHITE);
    dsp->drawFastHLine(0,14,PANEL_WIDTH,C_GOLD);

    // Footer
    ctrTxt1("WHOOP UP NEXT", 56, dsp->color565(255,160,80));
  }

  // Pulse a dumbbell sprite in the middle of the card (centered at x=64, y=36).
  // Base dumbbell is two end-weights + a bar; we draw it once and just shift the
  // highlight pixel each tick to imply motion.
  static unsigned long lastPulse = 0;
  if(millis() - lastPulse > 160){
    lastPulse = millis();
    pulse = (pulse + 1) & 7;
    uint8_t bri = (pulse < 4) ? pulse : (8 - pulse);

    // Restore strip of background under dumbbell area (y=24..48)
    for(int y=24; y<48; y++){
      uint8_t r = map(y, 0, 63, 60, 8);
      uint8_t g = map(y, 0, 63,  6, 0);
      uint8_t b = map(y, 0, 63, 14, 4);
      dsp->drawFastHLine(20, y, 88, dsp->color565(r,g,b));
    }

    // Bar  (y=35..36, x=42..86)
    uint16_t barCol = dsp->color565(180,180,200);
    fillRect(42, 35, 45, 2, barCol);

    // Left weight: stack of plates centered at x=38, height ~16
    auto plate = [&](int cx, int cy, int w, int h, uint16_t c){
      fillRect(cx-w/2, cy-h/2, w, h, c);
    };
    uint16_t darkPlate  = dsp->color565(40,40,55);
    uint16_t lightPlate = dsp->color565(120 + bri*15, 120 + bri*15, 145 + bri*15);
    // Left side
    plate(38, 36, 4, 18, darkPlate);
    plate(34, 36, 3, 14, lightPlate);
    plate(31, 36, 2,  9, darkPlate);
    // Right side
    plate(90, 36, 4, 18, darkPlate);
    plate(94, 36, 3, 14, lightPlate);
    plate(97, 36, 2,  9, darkPlate);

    // Spark at one end — alternates side based on pulse phase
    int sparkX = (pulse & 2) ? 28 : 100;
    uint16_t sparkCol = (bri >= 2) ? C_WHITE : dsp->color565(255,200,80);
    dsp->drawPixel(sparkX,   30, sparkCol);
    dsp->drawPixel(sparkX-1, 31, sparkCol);
    dsp->drawPixel(sparkX+1, 31, sparkCol);
  }
}

// ----------------------------------------------------------
void renderSportsAnim(){
  static int animType = 0;
  static bool animInited = false;
  static float animA = 0, animB = 0;            // generic animation state
  static unsigned long lastAnim = 0;
  // True when the slide was drawn with a real 48×48 bitmap logo. In that
  // case we skip the per-frame animation (baseball/puck/etc.) so it doesn't
  // overpaint the logo. Animations still run when we fell back to the old
  // hand-coded art.
  static bool animUsesBitmap = false;

  bool newSlide = (lastStaticDraw == 0);
  if(!animInited || newSlide){
    animType = random(0,4);
    animInited = true;
    lastStaticDraw = millis();
    cls();

    if(animType == 0){
      // ===== RED SOX — bitmap hero logo card =====
      // Navy background top + bottom bands frame a real Red Sox logo
      // blitted from team_logos.h (falls back to hand-coded socks if the
      // header hasn't been generated yet).
      uint16_t soxNavy  = dsp->color565(13, 27, 62);
      uint16_t cream    = dsp->color565(240, 230, 200);

      fillRect(0, 0, PANEL_WIDTH, PANEL_HEIGHT, C_BLACK);
      fillRect(0, 0, PANEL_WIDTH, 11, soxNavy);
      ctrTxt1("BOSTON RED SOX", 2, cream);
      dsp->drawFastHLine(0, 11, PANEL_WIDTH, dsp->color565(180, 25, 32));

      // 48×48 centered logo at (40, 12)
      animUsesBitmap = drawTeamLogoBmp48((PANEL_WIDTH-48)/2, 12, "BOS", "MLB");
      if(!animUsesBitmap){
        drawCrossedSocks(64, 36);   // fallback to existing pixel art
      }

      fillRect(0, 56, PANEL_WIDTH, 8, soxNavy);
      ctrTxt1("FENWAY  PARK", 57, cream);

      // Init baseball animation in the small space above the logo
      animA = -3;
      animB = 16;
    }
    else if(animType == 1){
      // ===== PATRIOTS — bitmap hero logo card =====
      uint16_t neNavy = dsp->color565(0, 34, 68);
      uint16_t neRed  = dsp->color565(198, 12, 48);
      uint16_t neWhite= dsp->color565(245, 245, 245);

      // Navy field background + red bands top/bottom
      fillRect(0, 0, PANEL_WIDTH, PANEL_HEIGHT, neNavy);
      fillRect(0, 0, PANEL_WIDTH, 4, neRed);
      fillRect(0, 60, PANEL_WIDTH, 4, neRed);
      dsp->drawFastHLine(0, 4, PANEL_WIDTH, neWhite);
      dsp->drawFastHLine(0, 59, PANEL_WIDTH, neWhite);

      // 48×48 centered logo at (40, 8)
      animUsesBitmap = drawTeamLogoBmp48((PANEL_WIDTH-48)/2, 8, "NE", "NFL");
      if(!animUsesBitmap){
        drawBigPatFace(64, 32);   // fallback to existing pixel art
      }

      // Footer
      txtOutline("PATRIOTS", 38, 54, neWhite);

      // Static twinkle stars in top band (frozen pattern, animated below)
      static const uint8_t patStars[][2] = {
        {10,2}, {26,1}, {44,2}, {64,2}, {84,1}, {102,2}, {118,1}
      };
      for(int i=0; i<7; i++){
        int sx = patStars[i][0], sy = patStars[i][1];
        dsp->drawPixel(sx, sy, neWhite);
        if(sy>=1) dsp->drawPixel(sx-1, sy, neWhite);
        dsp->drawPixel(sx+1, sy, neWhite);
      }

      // Init twinkle animation
      animA = 0;
    }
    else if(animType == 2){
      // ===== CELTICS — bitmap hero logo card =====
      uint16_t celtGreen = dsp->color565(0, 122, 51);
      uint16_t cream     = dsp->color565(240, 230, 200);

      fillRect(0, 0, PANEL_WIDTH, PANEL_HEIGHT, C_BLACK);
      // Top green banner with team name
      fillRect(0, 0, PANEL_WIDTH, 11, celtGreen);
      ctrTxt1("BOSTON CELTICS", 2, cream);
      dsp->drawFastHLine(0, 11, PANEL_WIDTH, dsp->color565(0, 80, 30));

      // 48×48 centered logo at (40, 12)
      animUsesBitmap = drawTeamLogoBmp48((PANEL_WIDTH-48)/2, 12, "BOS", "NBA");
      if(!animUsesBitmap){
        // Fallback: simplified pixel-art shamrock-on-green-circle
        int gcx = 64, gcy = 36, gR = 22;
        dsp->fillCircle(gcx, gcy, gR, celtGreen);
        drawBigShamrock(gcx, gcy + 6);
      }

      // Bottom banner
      fillRect(0, 56, PANEL_WIDTH, 8, celtGreen);
      ctrTxt1("TD GARDEN", 57, cream);

      // Init basketball animation
      animA = -4;
      animB = 0;
    }
    else {
      // ===== BRUINS — bitmap hero logo card =====
      uint16_t bruGold = dsp->color565(252, 181, 20);
      uint16_t bruBlk  = dsp->color565(8, 8, 12);

      // Black arena
      fillRect(0, 0, PANEL_WIDTH, PANEL_HEIGHT, bruBlk);

      // Top gold banner
      fillRect(0, 0, PANEL_WIDTH, 11, bruGold);
      ctrTxt1("BOSTON BRUINS", 2, bruBlk);

      // STATIC star pattern in arena rafters (below banner)
      static const uint8_t bruStars[][2] = {
        {7,14},{19,16},{32,13},{47,17},{63,14},{78,16},{93,13},{108,15},{120,17}
      };
      for(int i=0; i<9; i++)
        dsp->drawPixel(bruStars[i][0], bruStars[i][1], bruGold);

      // 48×48 centered logo at (40, 12)
      animUsesBitmap = drawTeamLogoBmp48((PANEL_WIDTH-48)/2, 12, "BOS", "NHL");
      if(!animUsesBitmap){
        drawBigBruinsB(64, 32);   // fallback
      }

      // Ice strip bottom (preserved for puck animation)
      for(int y=54; y<64; y++)
        dsp->drawFastHLine(0, y, PANEL_WIDTH,
          dsp->color565(map(y,54,63,140,90), map(y,54,63,170,120), 200));
      dsp->drawFastHLine(0, 57, PANEL_WIDTH, dsp->color565(220, 30, 30));
      dsp->drawFastHLine(0, 60, PANEL_WIDTH, dsp->color565(50, 100, 200));

      // Init puck animation
      animA = -5;
      animB = 56;
    }
    lastAnim = millis();
    return;
  }

  // ── PER-FRAME ANIMATION (smooth, tiny delta updates) ──
  // Bitmap-logo cards get a SAFE banner animation that never touches the
  // 48×48 logo (x=40..88, y=8..60): pulsing corner gems in the banner bands
  // plus a roaming sparkle along the top edge. Keeps the cards alive without
  // risking overpaint of the logo art.
  if(animUsesBitmap){
    static unsigned long lastPulse2 = 0;
    static uint8_t pp = 0;
    static int sparkleX = 0;
    if(millis() - lastPulse2 > 120){
      lastPulse2 = millis();
      pp = (pp + 1) & 7;
      uint8_t bri = (pp < 4) ? pp : (8 - pp);
      // Team accent color per card
      uint16_t gem;
      if(animType == 0)      gem = dsp->color565(180+bri*15, 25, 32);    // Sox red
      else if(animType == 1) gem = dsp->color565(200+bri*12, 200+bri*12, 215+bri*10); // Pats silver
      else if(animType == 2) gem = dsp->color565(0, 120+bri*30, 50);     // Celtics green
      else                   gem = dsp->color565(210+bri*10, 150+bri*20, 15); // Bruins gold
      // Corner gems — 3px diamonds at banner corners, clear of centered text
      auto gemAt = [&](int gx, int gy){
        dsp->drawPixel(gx,   gy,   gem);
        dsp->drawPixel(gx-1, gy,   gem);
        dsp->drawPixel(gx+1, gy,   gem);
        dsp->drawPixel(gx,   gy-1, gem);
        dsp->drawPixel(gx,   gy+1, gem);
      };
      gemAt(6, 5); gemAt(121, 5);
      // Roaming sparkle along the very top row (y=0) — restore band color
      // behind it. Top banner colors per card:
      uint16_t bandC;
      if(animType == 0)      bandC = dsp->color565(13, 27, 62);
      else if(animType == 1) bandC = dsp->color565(198, 12, 48);
      else if(animType == 2) bandC = dsp->color565(0, 122, 51);
      else                   bandC = dsp->color565(252, 181, 20);
      dsp->drawPixel(sparkleX, 0, bandC);             // erase old
      sparkleX = (sparkleX + 3) % PANEL_WIDTH;
      dsp->drawPixel(sparkleX, 0, C_WHITE);           // new sparkle
    }
    return;
  }
  if(millis() - lastAnim < 40) return;     // ~25fps
  lastAnim = millis();

  if(animType == 0){
    // RED SOX — baseball arcs across the small space y=13..19 between
    // the navy header and the crossed socks. Erase via plain black bg.
    int oldX = (int)animA;
    int oldY = (int)animB;
    if(oldX >= 0 && oldX < PANEL_WIDTH){
      dsp->drawPixel(oldX-1, oldY,   C_BLACK);
      dsp->drawPixel(oldX,   oldY-1, C_BLACK);
      dsp->drawPixel(oldX,   oldY,   C_BLACK);
      dsp->drawPixel(oldX+1, oldY,   C_BLACK);
      dsp->drawPixel(oldX,   oldY+1, C_BLACK);
    }
    animA += 0.8f;
    if(animA > PANEL_WIDTH+3){ animA = -3; animB = 18; }
    // Tight parabolic arc in y=13..19 (between header and socks)
    float t = (animA + 3) / (PANEL_WIDTH + 6);
    animB = 18 - sinf(t * PI) * 4;   // arc dips up at center
    int nx = (int)animA, ny = (int)animB;
    if(nx >= 0 && nx < PANEL_WIDTH && ny >= 13 && ny < 20){
      dsp->drawPixel(nx,   ny-1, dsp->color565(245,240,225));
      dsp->drawPixel(nx-1, ny,   dsp->color565(245,240,225));
      dsp->drawPixel(nx,   ny,   C_WHITE);
      dsp->drawPixel(nx+1, ny,   dsp->color565(245,240,225));
      dsp->drawPixel(nx,   ny+1, dsp->color565(220,35,45));   // stitch hint
    }
  }
  else if(animType == 1){
    // PATRIOTS — twinkle the navy-band stars (top + bottom)
    static const uint8_t patStars[][2] = {
      {10,2}, {26,1}, {44,2}, {64,2}, {84,1}, {102,2}, {118,1}
    };
    int idx = ((int)animA) % 7;
    uint16_t neRed   = dsp->color565(198, 12, 48);
    uint16_t neWhite = dsp->color565(245,245,245);
    // Erase previous twinkle (set back to red band)
    int prevIdx = (idx + 6) % 7;
    int psx = patStars[prevIdx][0], psy = patStars[prevIdx][1];
    fillRect(psx-1, psy-1, 3, 3, neRed);
    dsp->drawPixel(psx, psy, neWhite);
    dsp->drawPixel(psx-1, psy, neWhite);
    dsp->drawPixel(psx+1, psy, neWhite);
    // Brighter twinkle on current star
    int sx = patStars[idx][0], sy = patStars[idx][1];
    dsp->drawPixel(sx, sy-1, C_WHITE);
    dsp->drawPixel(sx-1, sy, C_WHITE);
    dsp->drawPixel(sx, sy, C_WHITE);
    dsp->drawPixel(sx+1, sy, C_WHITE);
    dsp->drawPixel(sx, sy+1, C_WHITE);
    animA += 0.3f;
  }
  else if(animType == 2){
    // CELTICS — basketball bouncing along the bottom edge against black bg.
    uint16_t basketb = dsp->color565(220, 110, 30);
    uint16_t bbDark  = dsp->color565(140, 60, 10);
    int oldX = (int)animA;
    int oldY = 60 - (int)(fabsf(sinf(animB)) * 4.0f);
    // Erase old ball footprint to BLACK (5×5 box)
    if(oldX >= 0 && oldX < PANEL_WIDTH){
      for(int dy=-2; dy<=2; dy++){
        for(int dx=-2; dx<=2; dx++){
          int xx = oldX+dx, yy = oldY+dy;
          if(xx<0 || xx>=PANEL_WIDTH || yy<58 || yy>=64) continue;
          dsp->drawPixel(xx, yy, C_BLACK);
        }
      }
    }
    animA += 1.0f;
    animB += 0.20f;
    if(animA > PANEL_WIDTH+3){ animA = -3; }
    int nx = (int)animA;
    int ny = 60 - (int)(fabsf(sinf(animB)) * 4.0f);
    if(nx >= 0 && nx < PANEL_WIDTH){
      dsp->fillCircle(nx, ny, 2, basketb);
      dsp->drawPixel(nx, ny-2, bbDark);
      dsp->drawPixel(nx, ny+2, bbDark);
      dsp->drawPixel(nx-2, ny, bbDark);
      dsp->drawPixel(nx+2, ny, bbDark);
    }
  }
  else {
    // BRUINS — hockey puck sliding across the ice
    int oldX = (int)animA;
    int puckY = (int)animB;
    // Erase old puck — restore ice gradient
    if(oldX >= -3 && oldX < PANEL_WIDTH){
      for(int dy=-1; dy<=1; dy++){
        int yy = puckY + dy;
        if(yy < 54 || yy >= 64) continue;
        uint16_t c = dsp->color565(map(yy,54,63,140,90), map(yy,54,63,170,120), 200);
        for(int dx=-3; dx<=3; dx++){
          int xx = oldX + dx;
          if(xx<0 || xx>=PANEL_WIDTH) continue;
          dsp->drawPixel(xx, yy, c);
        }
      }
      // Re-paint goal/blue lines
      dsp->drawFastHLine(0, 57, PANEL_WIDTH, dsp->color565(220, 30, 30));
      dsp->drawFastHLine(0, 60, PANEL_WIDTH, dsp->color565(50, 100, 200));
    }
    // Advance puck
    animA += 1.0f;
    if(animA > PANEL_WIDTH+5){ animA = -5; }
    int nx = (int)animA;
    if(nx >= 0 && nx < PANEL_WIDTH){
      // Black puck — flat oval 5×3
      uint16_t puckC = dsp->color565(20, 20, 25);
      fillRect(nx-2, puckY-1, 5, 3, puckC);
      dsp->drawPixel(nx-2, puckY-1, dsp->color565(60,60,70));
      dsp->drawPixel(nx+2, puckY+1, dsp->color565(60,60,70));
      // Ice trail behind puck
      uint16_t trailC = dsp->color565(200, 220, 240);
      for(int t=1; t<=3; t++){
        if(nx-2-t < 0) break;
        dsp->drawPixel(nx-2-t, puckY, trailC);
      }
    }
  }
}

// (no extra helpers needed)

// (Old animation type-4 (American flag) and the type-by-type animation chain
//  below have been retired in favor of the four single-team static cards above.)

// ----------------------------------------------------------
// =====================================================================
// TEAM COLORS - lookup table for ~110 teams across MLB/NBA/NHL/NFL
// Returns RGB565 values. First-match wins for shared abbreviations.
// =====================================================================
struct TeamCol { const char* abbr; uint16_t primary; uint16_t secondary; };

const TeamCol TEAM_COLS[] = {
  // NFL (most important per user)
  {"NE",  0x002A, 0xC100}, {"BUF", 0x00B5, 0xC100}, {"NYJ", 0x0C04, 0xFFFF},
  {"MIA", 0x05F8, 0xFD60}, {"BAL", 0x2008, 0xC8C8}, {"CIN", 0xF800, 0x0000},
  {"CLE", 0x6A00, 0xFD20}, {"PIT", 0xFFE0, 0x0000}, {"HOU", 0x0023, 0xC100},
  {"IND", 0x0098, 0xFFFF}, {"JAX", 0x0408, 0xFD20}, {"TEN", 0x0438, 0xFD60},
  {"DEN", 0xFC00, 0x0848}, {"KC",  0xC8E5, 0xFFE0}, {"LV",  0xC638, 0x0000},
  {"LAC", 0x055F, 0xFFE0}, {"DAL", 0x0011, 0xC618}, {"NYG", 0x0014, 0xC100},
  {"PHI", 0x0408, 0xC638}, {"WSH", 0x6020, 0xFCE0}, {"CHI", 0x10A6, 0xFD20},
  {"DET", 0x055F, 0xC618}, {"GB",  0x1AC5, 0xFFE0}, {"MIN", 0x4810, 0xFD20},
  {"ATL", 0xC0C0, 0x0000}, {"CAR", 0x055F, 0x0000}, {"NO",  0xCDA0, 0x0000},
  {"TB",  0xC100, 0x52A4}, {"ARI", 0x9863, 0xFFFF}, {"LAR", 0x012A, 0xFD60},
  {"SF",  0xA800, 0xCDA0}, {"SEA", 0x012A, 0x6FE0},
  // NHL
  {"BOS", 0xFEC0, 0x0000}, {"TOR", 0x0017, 0xFFFF}, {"MTL", 0xC8C8, 0x0011},
  {"OTT", 0xC8C8, 0x0000}, {"TBL", 0x0017, 0xFFFF}, {"FLA", 0xC100, 0xFD20},
  {"NJD", 0xC8C8, 0x0000}, {"NYI", 0x012A, 0xFD60}, {"NYR", 0x0017, 0xC100},
  {"CBJ", 0x012A, 0xC100}, {"CAR", 0xC8C8, 0x0000}, {"WSH", 0xC100, 0xFFFF},
  {"COL", 0x80E0, 0x012A}, {"STL", 0x0017, 0xFD20}, {"NSH", 0xFD20, 0x0017},
  {"WPG", 0x0408, 0xFFFF}, {"DAL", 0x0408, 0xFFE0}, {"MIN", 0x0408, 0xFFE0},
  {"VGK", 0x9863, 0xC638}, {"VAN", 0x012A, 0x0408}, {"EDM", 0x0017, 0xFC00},
  {"CGY", 0xC8C8, 0xFFE0}, {"ANA", 0xFC00, 0x0000}, {"LAK", 0xC638, 0x0000},
  {"SJS", 0x0408, 0xFD20}, {"UTA", 0x012A, 0xC8C8},
  // MLB
  {"NYY", 0x012A, 0xFFFF}, {"BOS", 0xC8C8, 0x012A}, {"TBR", 0x012A, 0x0408},
  {"TOR", 0x012A, 0xC8C8}, {"BAL", 0xFC00, 0x0000}, {"CWS", 0x0000, 0xFFFF},
  {"CLE", 0xC8C8, 0x012A}, {"DET", 0x012A, 0xFD20}, {"KC",  0x012A, 0xFFFF},
  {"MIN", 0x012A, 0xC8C8}, {"HOU", 0x012A, 0xFD20}, {"LAA", 0xC8C8, 0x012A},
  {"OAK", 0x4408, 0xFD20}, {"SEA", 0x0408, 0xFFFF}, {"TEX", 0x0017, 0xC8C8},
  {"ATL", 0x012A, 0xC8C8}, {"MIA", 0x0017, 0xFC00}, {"NYM", 0x0017, 0xFD20},
  {"PHI", 0xC8C8, 0x012A}, {"WSH", 0xC8C8, 0x012A}, {"CHC", 0x012A, 0xC8C8},
  {"CIN", 0xC8C8, 0x0000}, {"MIL", 0x012A, 0xFD20}, {"PIT", 0xFFE0, 0x0000},
  {"STL", 0xC8C8, 0xFD20}, {"ARI", 0x80E0, 0xFD60}, {"COL", 0x4810, 0xC638},
  {"LAD", 0x012A, 0xFFFF}, {"SD",  0x6A00, 0xFFE0}, {"SF",  0xFC00, 0x0000},
  // NBA
  {"BOS", 0x0408, 0xFD20}, {"BKN", 0x0000, 0xFFFF}, {"NYK", 0x012A, 0xFD20},
  {"PHI", 0x0017, 0xC100}, {"TOR", 0xC8C8, 0x0000}, {"CHI", 0xC8C8, 0x0000},
  {"CLE", 0x80E0, 0xFD20}, {"DET", 0x0017, 0xC100}, {"IND", 0x012A, 0xFD20},
  {"MIL", 0x0408, 0xFFE0}, {"ATL", 0xC8C8, 0x0408}, {"CHA", 0x0017, 0x4810},
  {"MIA", 0xC8C8, 0x0000}, {"ORL", 0x0017, 0xFFFF}, {"WAS", 0x012A, 0xC8C8},
  {"DEN", 0x012A, 0xFD20}, {"OKC", 0x055F, 0xFC00}, {"POR", 0xC8C8, 0x0000},
  {"UTA", 0x012A, 0xFFE0}, {"GSW", 0x012A, 0xFD20}, {"LAC", 0xC100, 0x0017},
  {"LAL", 0x4810, 0xFD20}, {"PHX", 0x4810, 0xFC00}, {"SAC", 0x4810, 0xC638},
  {"DAL", 0x012A, 0x0017}, {"HOU", 0xC8C8, 0xC638}, {"MEM", 0x0017, 0xFD20},
  {"NOP", 0x012A, 0xC8C8}, {"SAS", 0xC638, 0x0000},
};
const int TEAM_COL_COUNT = sizeof(TEAM_COLS)/sizeof(TeamCol);

uint16_t teamColor(const String &abbr){
  for(int i=0;i<TEAM_COL_COUNT;i++) if(abbr==TEAM_COLS[i].abbr) return TEAM_COLS[i].primary;
  return 0x4208;
}
uint16_t teamColorSec(const String &abbr){
  for(int i=0;i<TEAM_COL_COUNT;i++) if(abbr==TEAM_COLS[i].abbr) return TEAM_COLS[i].secondary;
  return 0xFFFF;
}

// ============================================================
// Bitmap team-logo helpers (powered by tools/build_team_logos.py).
// findTeamLogoBmp() returns nullptr when no header has been generated yet
// OR when the (league, abbr) pair isn't in the table — callers should
// fall back to the colored-monogram renderer in that case.
// ============================================================
const uint16_t* findTeamLogoBmp(const char* league, const char* abbr, int size){
#if HAS_TEAM_LOGOS
  for(int i=0; i<TEAM_LOGO_COUNT; i++){
    if(strcmp(TEAM_LOGOS[i].league, league)==0 &&
       strcmp(TEAM_LOGOS[i].abbr,   abbr  )==0){
      return (size >= 48) ? TEAM_LOGOS[i].px48 : TEAM_LOGOS[i].px20;
    }
  }
#endif
  (void)league; (void)abbr; (void)size;
  return nullptr;
}

// Uppercase the league string into a small stack buffer so we can match
// against the all-caps keys in TEAM_LOGOS regardless of what the API hands
// us ("mlb" vs "MLB").
static void leagueUp(const String &league, char *out, size_t outLen){
  size_t i=0;
  for(; i<league.length() && i<outLen-1; i++){
    char c = league.charAt(i);
    out[i] = (c >= 'a' && c <= 'z') ? (c - 32) : c;
  }
  out[i] = 0;
}

// Blit a 20×20 logo at (ox,oy). Skips transparent pixels so the panel
// background shows through. Returns false if the team isn't in the table.
bool drawTeamLogoBmp20(int ox, int oy, const String &abbr, const String &league){
  char lg[8]; leagueUp(league, lg, sizeof(lg));
  const uint16_t* px = findTeamLogoBmp(lg, abbr.c_str(), 20);
  if(!px) return false;
  for(int y=0; y<20; y++){
    for(int x=0; x<20; x++){
      uint16_t c = pgm_read_word(&px[y*20 + x]);
      if(c == LOGO_TRANSPARENT) continue;
      dsp->drawPixel(ox+x, oy+y, c);
    }
  }
  return true;
}

// Blit a 48×48 logo at (ox,oy). Same transparency convention as the 20px
// variant. Used by the sports transition cards for a hero logo display.
bool drawTeamLogoBmp48(int ox, int oy, const String &abbr, const String &league){
  char lg[8]; leagueUp(league, lg, sizeof(lg));
  const uint16_t* px = findTeamLogoBmp(lg, abbr.c_str(), 48);
  if(!px) return false;
  for(int y=0; y<48; y++){
    for(int x=0; x<48; x++){
      uint16_t c = pgm_read_word(&px[y*48 + x]);
      if(c == LOGO_TRANSPARENT) continue;
      dsp->drawPixel(ox+x, oy+y, c);
    }
  }
  return true;
}

// ============================================================
// Airline bitmap-logo helpers (16×16). Powered by tools/build_airline_logos.ps1.
// Falls back gracefully when the generated header is missing.
// ============================================================
bool drawAirlineLogoBmp16(int ox, int oy, const String &callsign){
#if HAS_AIRLINE_LOGOS
  if(callsign.length() < 3) return false;
  String prefix = callsign.substring(0, 3); prefix.toUpperCase();
  for(int i=0; i<AIRLINE_LOGO_COUNT; i++){
    if(prefix == AIRLINE_LOGOS[i].icao){
      const uint16_t* px = AIRLINE_LOGOS[i].px16;
      for(int y=0; y<16; y++){
        for(int x=0; x<16; x++){
          uint16_t c = pgm_read_word(&px[y*16 + x]);
          if(c == AIRLINE_LOGO_TRANSPARENT) continue;
          dsp->drawPixel(ox+x, oy+y, c);
        }
      }
      return true;
    }
  }
#endif
  (void)ox; (void)oy; (void)callsign;
  return false;
}

// ============================================================
// Weather icon bitmap helpers. Powered by tools/build_weather_icons.ps1.
// `code` is an OWM icon string like "01d", "10n", "13d", etc.
// ============================================================
bool drawWxIconBmp(int ox, int oy, const String &code, int size){
#if HAS_WEATHER_ICONS
  for(int i=0; i<WX_ICON_COUNT; i++){
    if(code == WX_ICONS[i].code){
      const uint16_t* px = (size >= 24) ? WX_ICONS[i].px24 : WX_ICONS[i].px16;
      int s = (size >= 24) ? 24 : 16;
      for(int y=0; y<s; y++){
        for(int x=0; x<s; x++){
          uint16_t c = pgm_read_word(&px[y*s + x]);
          if(c == WX_ICON_TRANSPARENT) continue;
          dsp->drawPixel(ox+x, oy+y, c);
        }
      }
      return true;
    }
  }
#endif
  (void)ox; (void)oy; (void)code; (void)size;
  return false;
}

// ============================================================
// Generic RGB565 bitmap blitter. Blits a w×h PROGMEM array at (ox,oy),
// skipping pixels equal to `transparent`. The one primitive behind all
// the bitmap-icon helpers.
// ============================================================
void drawBitmapRGB565(int ox, int oy, int w, int h,
                      const uint16_t* px, uint16_t transparent){
  for(int y=0; y<h; y++){
    for(int x=0; x<w; x++){
      uint16_t c = pgm_read_word(&px[y*w + x]);
      if(c == transparent) continue;
      int dx = ox+x, dy = oy+y;
      if(dx < 0 || dx >= PANEL_WIDTH || dy < 0 || dy >= PANEL_HEIGHT) continue;
      dsp->drawPixel(dx, dy, c);
    }
  }
}

// ============================================================
// Per-slide anchor-icon helper. Looks up a named Twemoji icon and blits it
// at (ox,oy). `size` picks the 16px or 24px variant. Returns false when the
// generated header is missing or the key isn't found (caller can fall back
// to its old text/glyph header). Powered by tools/build_slide_icons.ps1.
// ============================================================
bool drawSlideIcon(const char* key, int ox, int oy, int size){
#if HAS_SLIDE_ICONS
  for(int i=0; i<SLIDE_ICON_COUNT; i++){
    if(strcmp(SLIDE_ICONS[i].key, key)==0){
      if(size >= 24) drawBitmapRGB565(ox, oy, 24, 24, SLIDE_ICONS[i].px24, SLIDE_ICON_TRANSPARENT);
      else           drawBitmapRGB565(ox, oy, 16, 16, SLIDE_ICONS[i].px16, SLIDE_ICON_TRANSPARENT);
      return true;
    }
  }
#endif
  (void)key; (void)ox; (void)oy; (void)size;
  return false;
}

// Draw a 20x20 team logo box at (ox,oy).
// Priority teams get team-specific graphic designs; others use enhanced color monogram.
// `league` is the ESPN league string ("mlb", "nfl", "nhl", "nba", …) — used
// to disambiguate "BOS" (Red Sox / Bruins / Celtics) when we look up the
// bitmap logo. Defaults to empty so older call sites still compile.
void drawTeamLogo(int ox, int oy, const String &abbr, const String &league){
  // Prefer the high-fidelity bitmap if we have one for this team.
  if(drawTeamLogoBmp20(ox, oy, abbr, league)) return;

  // ── Fallback: existing colored monogram / priority-team pixel art ──
  uint16_t bg=teamColor(abbr);
  uint16_t fg=teamColorSec(abbr);

  // ── Base fill (rounded corners via corner pixels) ──
  fillRect(ox,oy,20,20,bg);
  // corner rounding
  dsp->drawPixel(ox,oy,C_BLACK); dsp->drawPixel(ox+19,oy,C_BLACK);
  dsp->drawPixel(ox,oy+19,C_BLACK); dsp->drawPixel(ox+19,oy+19,C_BLACK);
  // outer border in secondary color
  dsp->drawFastHLine(ox+1,oy,18,fg);
  dsp->drawFastHLine(ox+1,oy+19,18,fg);
  dsp->drawFastVLine(ox,oy+1,18,fg);
  dsp->drawFastVLine(ox+19,oy+1,18,fg);

  // ── Team-specific designs for priority teams ──
  if(abbr=="NE"){
    // Patriots: navy + red diagonal shoulder stripe + silver "NE"
    uint16_t red=dsp->color565(200,20,30), silv=dsp->color565(200,205,210);
    for(int i=5;i<=8;i++) dsp->drawFastHLine(ox+1,oy+i,18,red);
    dsp->drawFastVLine(ox+1,oy+1,18,silv); dsp->drawFastVLine(ox+18,oy+1,18,silv);
    setTextS(1); dsp->setTextColor(silv); dsp->setCursor(ox+4,oy+11); dsp->print("NE");
    return;
  }
  if(abbr=="BOS"){
    // Red Sox / Celtics / Bruins: bg + white circular badge outline, dark "BOS" inside
    uint16_t wht=C_WHITE;
    dsp->drawCircle(ox+9,oy+9,7,wht);    // outer white ring
    dsp->drawCircle(ox+9,oy+9,8,wht);    // double-weight ring
    setTextS(1); dsp->setTextColor(wht); dsp->setCursor(ox+4,oy+7); dsp->print("BOS");
    return;
  }
  if(abbr=="NYY"){
    // Yankees: navy bg, white vertical pinstripes, white NY
    uint16_t wht=C_WHITE, stripe=dsp->color565(40,45,70);
    for(int px=ox+3;px<ox+18;px+=4) dsp->drawFastVLine(px,oy+1,18,stripe);
    setTextS(1); dsp->setTextColor(wht); dsp->setCursor(ox+4,oy+7); dsp->print("NY");
    return;
  }
  if(abbr=="NYM"){
    // Mets: blue bg, orange diagonal slash, white NY
    uint16_t org=dsp->color565(255,90,0), wht=C_WHITE;
    for(int i=0;i<4;i++){
      dsp->drawFastHLine(ox+1,oy+10+i,18,org);
    }
    setTextS(1); dsp->setTextColor(wht); dsp->setCursor(ox+4,oy+4); dsp->print("NY");
    setTextS(1); dsp->setTextColor(org); dsp->setCursor(ox+4,oy+14); dsp->print("MET");
    return;
  }
  if(abbr=="NYG"){
    // Giants: dark blue, red center bar, white NY
    uint16_t red=dsp->color565(180,20,20), wht=C_WHITE;
    for(int i=8;i<=11;i++) dsp->drawFastHLine(ox+1,oy+i,18,red);
    setTextS(1); dsp->setTextColor(wht); dsp->setCursor(ox+4,oy+3); dsp->print("NY");
    setTextS(1); dsp->setTextColor(wht); dsp->setCursor(ox+1,oy+13); dsp->print("GIA");
    return;
  }
  if(abbr=="NYJ"){
    // Jets: green bg, white horizontal stripe + diagonal slash, black J
    uint16_t wht=C_WHITE;
    dsp->drawFastHLine(ox+1,oy+5, 18,wht); // top stripe
    dsp->drawFastHLine(ox+1,oy+6, 18,wht);
    dsp->drawFastHLine(ox+1,oy+13,18,wht); // bottom stripe
    dsp->drawFastHLine(ox+1,oy+14,18,wht);
    for(int i=0;i<14;i++) dsp->drawPixel(ox+15-i,oy+3+i,wht); // diagonal slash
    setTextS(1); dsp->setTextColor(C_BLACK); dsp->setCursor(ox+7,oy+8); dsp->print("J");
    return;
  }
  if(abbr=="NYR"){
    // Rangers: blue with red diagonal band + white NYR
    uint16_t red=dsp->color565(200,20,20), wht=C_WHITE;
    for(int i=5;i<12;i++){
      int len=18-abs(i-8); int lx=ox+1+abs(i-8)/2;
      dsp->drawFastHLine(lx,oy+i,len,red);
    }
    setTextS(1); dsp->setTextColor(wht); dsp->setCursor(ox+2,oy+7); dsp->print("NYR");
    return;
  }
  if(abbr=="NJD"){
    // Devils: red bg, black N+J pitchfork hint, white text
    uint16_t blk=C_BLACK, wht=C_WHITE;
    dsp->drawFastVLine(ox+6, oy+3, 14, blk);   // left horn
    dsp->drawFastVLine(ox+13,oy+3, 14, blk);   // right horn
    dsp->drawFastHLine(ox+4,oy+3,  12, blk);   // crossbar
    dsp->fillCircle(ox+6, oy+3, 2, blk);       // left tip
    dsp->fillCircle(ox+13,oy+3, 2, blk);       // right tip
    setTextS(1); dsp->setTextColor(wht); dsp->setCursor(ox+4,oy+12); dsp->print("NJD");
    return;
  }
  if(abbr=="NYK"){
    // Knicks: orange bg, blue stripes, white NY
    uint16_t blu=dsp->color565(0,50,160), wht=C_WHITE;
    dsp->drawFastHLine(ox+1,oy+5, 18,blu);
    dsp->drawFastHLine(ox+1,oy+6, 18,blu);
    dsp->drawFastHLine(ox+1,oy+13,18,blu);
    dsp->drawFastHLine(ox+1,oy+14,18,blu);
    setTextS(1); dsp->setTextColor(wht); dsp->setCursor(ox+4,oy+8); dsp->print("NY");
    return;
  }
  if(abbr=="BRK"||abbr=="BKN"){
    // Nets: black bg, white diagonal slash + BKN
    uint16_t wht=C_WHITE;
    for(int i=0;i<20;i++) dsp->drawPixel(ox+i,oy+i,wht);
    for(int i=0;i<20;i++) if(i>0) dsp->drawPixel(ox+i,oy+i-1,wht);
    setTextS(1); dsp->setTextColor(wht); dsp->setCursor(ox+2,oy+3);  dsp->print("BK");
    setTextS(1); dsp->setTextColor(C_BLACK); dsp->setCursor(ox+7,oy+12); dsp->print("N");
    return;
  }
  if(abbr=="PHI"){
    // Philly: use whatever the bg is, add a diagonal double-stripe
    uint16_t wht=C_WHITE;
    for(int i=3;i<7;i++)  dsp->drawFastHLine(ox+1,oy+i,18,fg);
    for(int i=13;i<17;i++) dsp->drawFastHLine(ox+1,oy+i,18,fg);
    setTextS(1); dsp->setTextColor(wht); dsp->setCursor(ox+2,oy+8); dsp->print("PHI");
    return;
  }

  // ── Generic fallback: monogram with enhanced border ──
  String s=abbr; if(s.length()>3) s=s.substring(0,3);
  // Accent inner stripe
  dsp->drawFastHLine(ox+1,oy+5,18,dsp->color565(
    ((fg>>11)&0x1F)*5,
    ((fg>>5)&0x3F)*3,
    (fg&0x1F)*5));
  dsp->drawFastHLine(ox+1,oy+14,18,dsp->color565(
    ((fg>>11)&0x1F)*5,
    ((fg>>5)&0x3F)*3,
    (fg&0x1F)*5));
  int w=s.length()*6;
  setTextS(1); dsp->setTextColor(fg);
  dsp->setCursor(ox+(20-w)/2, oy+7); dsp->print(s.c_str());
}

// Get a league-specific accent color for the badge
uint16_t leagueColor(const String &lg){
  if(lg=="MLB") return 0xC100;       // baseball red
  if(lg=="NBA") return 0xFD60;       // basketball orange
  if(lg=="NFL") return 0x0014;       // football blue
  if(lg=="NHL") return 0x0017;       // hockey blue
  if(lg=="MEN") return 0x4810;       // NCAAB purple
  if(lg=="WC")  return 0x07E8;       // World Cup green (soccer pitch)
  return 0x4208;
}

// Format game status for sport-specific display
String formatGameStatus(const GameScore &g){
  if(g.status=="post") return "FINAL";
  if(g.status=="pre")  return g.clock;
  // live - return as-is, formatted by ESPN
  return g.clock;
}

// ----------------------------------------------------------
// New scores card: ONE game per card, big size-3 scores, team logo blocks
// ============================================================
// Sport-specific live broadcast state widgets.
// Each draws into a fixed rect at (x, y, w, h) where w≈60, h=8 (the
// footer strip of the scores card). Designed to look like a real
// broadcast scoreboard's lower-third graphic.
// ============================================================

// MLB live widget: small diamond (4 bases) + BSO pip rows
//
// Diamond layout (8×8 starting at x):
//        2B           — top of diamond (filled = runner on 2nd)
//     3B    1B        — left = 3rd, right = 1st
//        HP           — bottom = home plate (unused as base)
//
// Beside it: three tiny pip rows for Balls / Strikes / Outs, each capped
// at the sport's max (4 / 3 / 3 respectively).
void drawMLBState(int x, int y, int w, int h, const GameScore &g){
  // ── 4-base diamond at x..x+8, y..y+7 ──
  int dCx = x + 4, dCy = y + 4;     // diamond center
  uint16_t empty = dsp->color565(60, 70, 100);
  uint16_t onCol = C_YELLOW;
  // 2nd base (top)
  dsp->fillRect(dCx - 1, dCy - 4, 3, 3, g.mlbOn2B ? onCol : empty);
  // 1st base (right)
  dsp->fillRect(dCx + 2, dCy - 1, 3, 3, g.mlbOn1B ? onCol : empty);
  // 3rd base (left)
  dsp->fillRect(dCx - 4, dCy - 1, 3, 3, g.mlbOn3B ? onCol : empty);

  // ── BSO pips: vertical stack of 3 rows starting at x+12 ──
  int px = x + 11;
  int py = y;
  auto drawPipRow = [&](int row_y, int filled, int total, uint16_t col){
    for(int i = 0; i < total; i++){
      uint16_t c = (i < filled) ? col : dsp->color565(40, 40, 60);
      dsp->fillRect(px + i*3, row_y, 2, 2, c);
    }
  };
  drawPipRow(py,     g.mlbBalls,   4, C_LIME);
  drawPipRow(py + 3, g.mlbStrikes, 3, C_YELLOW);
  drawPipRow(py + 6, g.mlbOuts,    3, C_RED);

  // ── Inning indicator on the right: arrow + number (e.g. "▲7") ──
  // arrow up = top of inning, arrow down = bottom
  int ix = px + 4 * 3 + 4;  // right of B-row
  uint16_t arrCol = dsp->color565(180, 200, 240);
  if(g.mlbTopHalf){
    dsp->drawPixel(ix + 1, y,     arrCol);
    dsp->drawPixel(ix,     y + 1, arrCol);
    dsp->drawPixel(ix + 1, y + 1, arrCol);
    dsp->drawPixel(ix + 2, y + 1, arrCol);
  } else {
    dsp->drawPixel(ix,     y,     arrCol);
    dsp->drawPixel(ix + 1, y,     arrCol);
    dsp->drawPixel(ix + 2, y,     arrCol);
    dsp->drawPixel(ix + 1, y + 1, arrCol);
  }
  // Inning number (1-2 chars) at top-right
  int inning = 0;
  // Try to extract inning from clock string ("Top 7th" / "Bot 7th")
  String c = g.clock;
  for(int i = 0; i < (int)c.length(); i++){
    if(c.charAt(i) >= '0' && c.charAt(i) <= '9'){
      inning = c.substring(i, i+2).toInt();
      break;
    }
  }
  if(inning > 0){
    char ib[4]; snprintf(ib, 4, "%d", inning);
    txt1(ib, ix + 4, y, C_WHITE);
  }
}

// NFL live widget: "3rd & 7" or "4 & GL" plus possession indicator
void drawNFLState(int x, int y, int w, int h, const GameScore &g){
  if(g.nflDown >= 1 && g.nflDown <= 4){
    // Down ordinal: 1st, 2nd, 3rd, 4th
    const char* ord[] = { "", "1st", "2nd", "3rd", "4th" };
    char buf[16];
    snprintf(buf, 16, "%s & %d", ord[g.nflDown], (int)g.nflDistance);
    txt1(buf, x, y, C_WHITE);
  } else {
    // Fall back to the human-readable clock from TheSportsDB
    String clk = g.clock; if(clk.length() > 10) clk = clk.substring(0, 10);
    txt1(clk.c_str(), x, y, C_WHITE);
  }
}

// NHL live widget: "PER 2  8:35" plus PP badge if applicable
void drawNHLState(int x, int y, int w, int h, const GameScore &g){
  // Period number
  if(g.nhlPeriod > 0){
    char pb[12];
    if(g.nhlPeriod >= 4)      snprintf(pb, 12, "OT");
    else if(g.nhlPeriod == 5) snprintf(pb, 12, "SO");
    else                      snprintf(pb, 12, "P%d", (int)g.nhlPeriod);
    txt1(pb, x, y, C_CYAN);
  }
  // Clock — extract from the original clock string ("2nd P 8:35" -> "8:35")
  String c = g.clock;
  int colon = c.indexOf(':');
  if(colon > 0){
    int start = colon - 1;
    while(start > 0 && c.charAt(start-1) >= '0' && c.charAt(start-1) <= '9') start--;
    String clk = c.substring(start);
    if(clk.length() > 6) clk = clk.substring(0, 6);
    txt1(clk.c_str(), x + 20, y, C_WHITE);
  }
  if(g.nhlPowerPlay){
    // Tiny "PP" badge at far right of strip — flash to draw the eye
    bool blink = ((millis()/400) % 2) == 0;
    uint16_t ppC = blink ? C_RED : dsp->color565(120, 30, 30);
    fillRect(x + w - 14, y, 12, 8, ppC);
    txt1("PP", x + w - 12, y, C_WHITE);
  }
}

// NBA live widget: "Q3 4:52"
void drawNBAState(int x, int y, int w, int h, const GameScore &g){
  if(g.nbaQuarter > 0){
    char qb[8];
    if(g.nbaQuarter >= 5) snprintf(qb, 8, "OT");
    else                  snprintf(qb, 8, "Q%d", (int)g.nbaQuarter);
    txt1(qb, x, y, C_ORANGE);
  }
  // Clock from the clock string (TheSportsDB gives e.g. "Q3 4:52")
  String c = g.clock;
  int colon = c.indexOf(':');
  if(colon > 0){
    int start = colon - 1;
    while(start > 0 && c.charAt(start-1) >= '0' && c.charAt(start-1) <= '9') start--;
    String clk = c.substring(start);
    if(clk.length() > 6) clk = clk.substring(0, 6);
    txt1(clk.c_str(), x + 18, y, C_WHITE);
  }
}

void renderScores(){
  static unsigned long lastCycle=0;
  static int lastShownIdx=-1;
  bool needsRedraw=false;

  if(millis()-lastCycle>6000){
    if(gameCount>0) gameDispIdx=(gameDispIdx+1)%gameCount;
    lastCycle=millis();
    needsRedraw=true;
  }
  if(lastShownIdx!=gameDispIdx) needsRedraw=true;
  if(!needsRedraw && millis()-lastStaticDraw<STATIC_REDRAW_MS) return;
  lastStaticDraw=millis();
  lastShownIdx=gameDispIdx;

  cls();

  // Empty state
  if(gameCount==0){
    fillRect(0,0,PANEL_WIDTH,14,dsp->color565(20,20,40));
    ctrTxt2("SCORES",0,C_CYAN);
    dsp->drawFastHLine(0,14,PANEL_WIDTH,C_CYAN);
    ctrTxt2("NO GAMES",24,C_GRAY);
    ctrTxt1("Loading...",42,dsp->color565(80,80,100));
    txt1("MLB NBA NHL NFL",18,54,dsp->color565(60,60,90));
    return;
  }

  // GLITCH GUARD: fetchScores() runs in the background and can SHRINK
  // gameCount while this card is up (e.g., yesterday's finals age out).
  // An unclamped index then reads past the array — garbage team names or
  // a crash. Clamp every frame before use.
  if(gameDispIdx >= gameCount) gameDispIdx = 0;
  GameScore &g=games[gameDispIdx];

  bool aWin=g.status=="post" && g.awayScore.toInt()>g.homeScore.toInt();
  bool hWin=g.status=="post" && g.homeScore.toInt()>g.awayScore.toInt();
  uint16_t statC=(g.status=="in")?C_LIME:(g.status=="pre")?C_YELLOW:dsp->color565(150,150,180);

  // ===== HEADER y=0..11 — league badge + status =====
  fillRect(0,0,PANEL_WIDTH,12,dsp->color565(15,15,30));
  dsp->drawFastHLine(0,12,PANEL_WIDTH,dsp->color565(50,50,80));
  // League badge (left)
  {
    String lg = g.league;
    if(lg.length() > 3) lg = lg.substring(0, 3);
    int lw = lg.length() * 6;
    fillRect(2, 1, lw+4, 10, leagueColor(g.league));
    txt1(lg.c_str(), 4, 3, C_BLACK);
  }
  // Status (right side)
  if(g.status == "in"){
    bool blink = ((millis()/500) % 2) == 0;
    if(blink){
      dsp->fillCircle(98, 6, 3, C_LIME);
    }
    txt1("LIVE", 104, 3, C_LIME);
  } else if(g.status == "post"){
    txt1("FINAL", 100, 3, dsp->color565(160,160,200));
  } else {
    // Pre-game — show start time in header right
    String clk = g.clock; if(clk.length() > 8) clk = clk.substring(0,8);
    int cw = clk.length() * 6;
    txt1(clk.c_str(), PANEL_WIDTH - cw - 2, 3, C_YELLOW);
  }
  // Priority asterisk in center if either team is a favorite
  if(isPriority(g.away) || isPriority(g.home)){
    txt1("*", 62, 3, C_GOLD);
  }

  // ===== AWAY ROW y=14..33 (20 px tall) =====
  // Layout: [20×20 logo at x=2] [team abbrev at x=26] [score size-2 right-aligned]
  drawTeamLogo(2, 14, g.away, g.league);
  // Team abbrev as text label between logo and score
  String aA = g.away; if(aA.length() > 3) aA = aA.substring(0, 3);
  txt1(aA.c_str(), 26, 18, aWin ? C_GOLD : C_WHITE);
  // Score size-2 at right (3-digit max)
  String aS = g.awayScore.isEmpty() ? "0" : g.awayScore;
  if(aS.length() > 3) aS = aS.substring(0, 3);
  int aw = aS.length() * 12;       // size-2 = 12 px per char
  int ax = PANEL_WIDTH - aw - 4;
  txt2(aS.c_str(), ax, 18, aWin ? C_GOLD : C_WHITE);
  if(aWin) txt1("W", 26, 26, C_GOLD);

  // Divider y=34
  dsp->drawFastHLine(0, 34, PANEL_WIDTH, dsp->color565(40,40,60));

  // ===== HOME ROW y=36..55 (20 px tall) =====
  drawTeamLogo(2, 36, g.home, g.league);
  String hA = g.home; if(hA.length() > 3) hA = hA.substring(0, 3);
  txt1(hA.c_str(), 26, 40, hWin ? C_GOLD : C_WHITE);
  String hS = g.homeScore.isEmpty() ? "0" : g.homeScore;
  if(hS.length() > 3) hS = hS.substring(0, 3);
  int hw = hS.length() * 12;
  int hx = PANEL_WIDTH - hw - 4;
  txt2(hS.c_str(), hx, 40, hWin ? C_GOLD : C_WHITE);
  if(hWin) txt1("W", 26, 48, C_GOLD);

  // ===== FOOTER y=56..63 =====
  // For LIVE games, replace the bland clock text with a broadcast-style
  // state widget (bases diamond for MLB, down/distance for NFL, etc.).
  // For pre/post games, keep the original simple layout.
  fillRect(0, 56, PANEL_WIDTH, 8, dsp->color565(8,8,18));

  // Page counter on the right (reserved space)
  int pageCountW = 0;
  if(gameCount > 1){
    char pgT[10]; snprintf(pgT, 10, "%d/%d", gameDispIdx+1, gameCount);
    pageCountW = strlen(pgT) * 6;
    txt1(pgT, PANEL_WIDTH - pageCountW - 2, 57, dsp->color565(120,120,150));
  }

  // Live widget zone — everything left of the page counter
  int widgetX = 2;
  int widgetW = PANEL_WIDTH - pageCountW - 6;

  if(g.status == "in"){
    if(g.league == "MLB"){
      drawMLBState(widgetX, 56, widgetW, 8, g);
    } else if(g.league == "NFL"){
      drawNFLState(widgetX, 57, widgetW, 8, g);
    } else if(g.league == "NHL"){
      drawNHLState(widgetX, 57, widgetW, 8, g);
    } else if(g.league == "NBA" || g.league == "NCB"){
      drawNBAState(widgetX, 57, widgetW, 8, g);
    } else if(g.clock.length() > 0 && g.clock != "LIVE"){
      // Unknown league — fall back to the original clock text
      String clk = g.clock; if(clk.length() > 12) clk = clk.substring(0, 12);
      txt1(clk.c_str(), widgetX, 57, statC);
    }
  } else if(g.status == "post"){
    // Final games keep the simple "FINAL" feel; show small dot row only
    if(gameCount > 1){
      int dotCount = min(gameCount, 8);
      int startX = (PANEL_WIDTH - dotCount * 4) / 2;
      for(int i = 0; i < dotCount; i++){
        uint16_t dc = (i == gameDispIdx % dotCount) ? C_CYAN : dsp->color565(40,40,60);
        dsp->fillCircle(startX + i*4 + 1, 60, 1, dc);
      }
    }
  } else {
    // Pre-game — show start time text + page dots
    if(g.clock.length() > 0){
      String clk = g.clock; if(clk.length() > 12) clk = clk.substring(0, 12);
      txt1(clk.c_str(), widgetX, 57, statC);
    }
  }
}

// ----------------------------------------------------------
void renderMoneyAnim(){
  static bool moneyInited=false;
  // Market direction drives entire color scheme
  bool spUp = finance.valid ? (finance.sp500Chg >= 0) : true;
  uint16_t bgBase  = spUp ? dsp->color565(0,18,4)  : dsp->color565(20,4,4);
  uint16_t gridCol = spUp ? dsp->color565(0,45,8)  : dsp->color565(50,8,8);

  if(!moneyInited){
    bool spUpInit = finance.valid ? (finance.sp500Chg >= 0) : true;
    uint16_t cols[8];
    if(spUpInit){
      cols[0]=C_LIME; cols[1]=C_GREEN; cols[2]=C_GOLD; cols[3]=C_YELLOW;
      cols[4]=dsp->color565(0,200,0); cols[5]=dsp->color565(100,255,100);
      cols[6]=C_LIME; cols[7]=C_GOLD;
    } else {
      cols[0]=C_RED; cols[1]=C_ORANGE; cols[2]=dsp->color565(200,50,50);
      cols[3]=dsp->color565(255,100,0); cols[4]=C_RED; cols[5]=dsp->color565(180,30,30);
      cols[6]=C_ORANGE; cols[7]=C_RED;
    }
    for(int i=0;i<MAX_MONEY;i++){
      money[i].x=random(2,120); money[i].y=random(-30,64);
      money[i].spd=1+random(0,4);
      money[i].col=cols[random(0,8)];
    }
    moneyInited=true;
  }

  cls();
  fillRect(0,0,PANEL_WIDTH,PANEL_HEIGHT,bgBase);

  // Draw grid pattern background
  for(int y=0;y<PANEL_HEIGHT;y+=8) dsp->drawFastHLine(0,y,PANEL_WIDTH,gridCol);
  for(int x=0;x<PANEL_WIDTH;x+=16) dsp->drawFastVLine(x,0,PANEL_HEIGHT,gridCol);

  static unsigned long lastM=0;
  if(millis()-lastM>35){
    for(int i=0;i<MAX_MONEY;i++){
      money[i].y+=money[i].spd;
      if(money[i].y>64){money[i].y=-8;money[i].x=random(2,120);}
    }
    lastM=millis();
  }

  for(int i=0;i<MAX_MONEY;i++){
    if(money[i].y<-6||money[i].y>62) continue;
    int sz=1+(i%4==0?1:0)+(i%7==0?1:0);
    dsp->setTextColor(money[i].col); dsp->setTextSize(sz);
    dsp->setCursor(money[i].x,money[i].y); dsp->print("$");
    dsp->setTextSize(1);
  }
  // Big centered overlay — uses real finance data when available
  int phase = (millis()/600)%3;
  if(phase==0){
    ctrTxt3(spUp?"$$":"!!",20,spUp?C_GOLD:C_RED);
    if(finance.valid && finance.sp500>0){
      char spLine[14]; snprintf(spLine,14,"S&P %+.2f%%",finance.sp500Chg);
      ctrTxt1(spLine,46,spUp?C_LIME:C_RED);
    } else {
      ctrTxt1("MARKETS",46,C_GRAY);
    }
  } else if(phase==1){
    ctrTxt3(spUp?"BUY!":"SELL",20,spUp?C_LIME:C_RED);
  } else {
    ctrTxt3("$$",20,C_YELLOW);
    if(finance.valid && finance.btcUSD>0){
      char btcLine[14]; snprintf(btcLine,14,"BTC%+.1f%%",finance.btcChg);
      ctrTxt1(btcLine,46,finance.btcChg>=0?C_GOLD:C_ORANGE);
    } else {
      ctrTxt1("MARKETS",46,C_GOLD);
    }
  }
  // Bottom data strip
  if(finance.valid && finance.sp500>0){
    char strip[22]; snprintf(strip,22,"SP:%.0f  BTC:%.0f",finance.sp500,finance.btcUSD/1000.0f);
    // avoid overflow: BTC in K
    char btcK[8]; snprintf(btcK,8,"%.0fK",finance.btcUSD/1000.0f);
    char spN[8];  snprintf(spN,8,"%.0f",finance.sp500);
    char fullStrip[22]; snprintf(fullStrip,22,"SP:%s  BTC:%s",spN,btcK);
    ctrTxt1(fullStrip,55,C_LIME);
  } else {
    ctrTxt1("S&P  BTC  BONDS",55,C_LIME);
  }
  moneyInited=(millis()-slideStart<7500);
}

// Draw a line-chart sparkline. data[0..len-1], rendered into (x,y,w,h) rect.
// Green if positive==true, red otherwise. Draws a faint zero-line if range spans 0.
void drawSparkline(float* data, int len, int x, int y, int w, int h, bool positive) {
  if(len < 2) return;
  float mn=data[0], mx=data[0];
  for(int i=1;i<len;i++){ if(data[i]<mn)mn=data[i]; if(data[i]>mx)mx=data[i]; }
  float range=mx-mn; if(range<0.5f) range=0.5f; // avoid division by near-zero
  uint16_t lineCol = positive ? C_LIME : C_RED;
  uint16_t fillCol = positive ? dsp->color565(0,25,0) : dsp->color565(25,0,0);
  // Light area fill under the curve
  int ppx=-1, ppy=-1;
  for(int i=0;i<len;i++){
    int sx = x + i*(w-1)/(len-1);
    int sy = y + h-1 - (int)((data[i]-mn)/range*(h-1));
    sy = constrain(sy, y, y+h-1);
    // Vertical fill from sy to bottom
    if(sy<y+h-1) dsp->drawFastVLine(sx, sy, y+h-sy, fillCol);
    if(ppx>=0) dsp->drawLine(ppx, ppy, sx, sy, lineCol);
    ppx=sx; ppy=sy;
  }
  // Zero line (shows open price baseline)
  if(len>0){
    int zy = y+h-1-(int)((data[0]-mn)/range*(h-1));
    zy=constrain(zy,y,y+h-1);
    dsp->drawFastHLine(x, zy, w, dsp->color565(50,50,50));
  }
  // Current price label top-right of sparkline area
  if(len>0){
    char lbl[8]; snprintf(lbl,8,"%.0f",data[len-1]);
    int lx=x+w-(int)strlen(lbl)*6-1;
    txt1(lbl, lx, y+1, lineCol);
  }
}

// ----------------------------------------------------------
// GOLF MAJOR leaderboard card. Self-hides (renders a placeholder that the
// main loop skips past) when no major is active.
void renderGolf(){
  if(millis()-lastStaticDraw<STATIC_REDRAW_MS) return;
  lastStaticDraw=millis();
  cls();

  // v10 unified chrome — fairway green accent, tour name as the title
  uint16_t golfGreen = dsp->color565(60,200,90);
  if(!golfIsMajor || golfCount==0){
    drawCardHeader("golf", "GOLF", golfGreen);
    ctrTxt1("No major this week",30,C_GRAY);
    if(golfTourName.length())
      ctrTxt1(trimTo(golfTourName,20).c_str(),42,dsp->color565(80,90,110));
    return;   // main loop skips this card quickly
  }
  drawCardHeader("golf", trimTo(golfTourName,17).c_str(), golfGreen, -1);

  // Column headers
  txt1("POS",2,16,dsp->color565(120,170,120));
  txt1("PLAYER",26,16,dsp->color565(120,170,120));
  txt1("TO PAR",92,16,dsp->color565(120,170,120));
  dsp->drawFastHLine(0,23,PANEL_WIDTH,dsp->color565(40,70,40));

  // Leaderboard rows y=25.. (7px each, up to 5 to fit nicely)
  int rows = min(golfCount, 5);
  for(int i=0;i<rows;i++){
    int y = 25 + i*7;
    txt1(golfBoard[i].pos.c_str(), 2, y, C_WHITE);
    txt1(trimTo(golfBoard[i].name,10).c_str(), 26, y, i==0?C_GOLD:C_WHITE);
    // Score colored: under par green, over par red, even gray
    const String &sc = golfBoard[i].score;
    uint16_t scC = (sc.startsWith("-")) ? C_LIME
                 : (sc=="E" || sc=="0") ? C_GRAY : C_RED;
    int sw = sc.length()*6;
    txt1(sc.c_str(), PANEL_WIDTH-sw-4, y, scC);
  }
}

// ----------------------------------------------------------
// TENNIS GRAND SLAM card. Self-hides when no major is active.
void renderTennis(){
  if(millis()-lastStaticDraw<STATIC_REDRAW_MS) return;
  lastStaticDraw=millis();
  cls();

  // v10 unified chrome — court blue accent, tournament name as the title
  uint16_t courtBlue = dsp->color565(80,150,255);
  if(!tennisIsMajor || tennisCount==0){
    drawCardHeader("tennis", "TENNIS", courtBlue);
    ctrTxt1("No Slam this week",30,C_GRAY);
    if(tennisTour.length())
      ctrTxt1(trimTo(tennisTour,20).c_str(),42,dsp->color565(90,110,150));
    return;
  }
  drawCardHeader("tennis", trimTo(tennisTour,17).c_str(), courtBlue, -1);

  // Up to 3 matches, each 2 rows (player + sets) with a divider.
  int rows = min(tennisCount, 3);
  for(int i=0;i<rows;i++){
    int y = 17 + i*15;
    // Player 1
    txt1(trimTo(tennisM[i].p1,12).c_str(), 2, y, tennisM[i].p1won?C_GOLD:C_WHITE);
    if(tennisM[i].sets1.length()){
      int sw=tennisM[i].sets1.length()*6;
      txt1(tennisM[i].sets1.c_str(), PANEL_WIDTH-sw-2, y, tennisM[i].p1won?C_GOLD:dsp->color565(180,200,230));
    }
    // Player 2
    txt1(trimTo(tennisM[i].p2,12).c_str(), 2, y+7, tennisM[i].p2won?C_GOLD:C_WHITE);
    if(tennisM[i].sets2.length()){
      int sw=tennisM[i].sets2.length()*6;
      txt1(tennisM[i].sets2.c_str(), PANEL_WIDTH-sw-2, y+7, tennisM[i].p2won?C_GOLD:dsp->color565(180,200,230));
    }
    // LIVE pip
    if(tennisM[i].live){
      bool blink=((millis()/500)%2)==0;
      if(blink) dsp->fillCircle(PANEL_WIDTH-2, y+3, 1, C_LIME);
    }
    if(i<rows-1) dsp->drawFastHLine(0, y+13, PANEL_WIDTH, dsp->color565(30,45,80));
  }
}

// ----------------------------------------------------------
void renderFinance(){
  static int finPage=0;
  static unsigned long finPageStart=0;
  bool newSlide=(lastStaticDraw==0);
  if(newSlide){finPage=0;finPageStart=millis();}
  // Cycle pages every 8 seconds. 4 pages:
  //   0 MARKETS (S&P + 10yr + sparkline)
  //   1 CRYPTO/GAS (BTC + gas + fear&greed)
  //   2 PNBK (Patriot National Bancorp + SOFR + Fed Funds)
  //   3 TREASURIES (3/5/10yr curve)
  if(millis()-finPageStart>8000){finPage=(finPage+1)%4;finPageStart=millis();lastStaticDraw=0;}
  if(millis()-lastStaticDraw<STATIC_REDRAW_MS) return;
  lastStaticDraw=millis();
  cls();

  // ===== v10 UNIFIED HEADER — icon + title swap per page, pips built in =====
  {
    const char* finIcon  = (finPage==0)?"finance":(finPage==1)?"money":(finPage==2)?"rates":"rates";
    const char* finTitle = (finPage==0)?"MARKETS":(finPage==1)?"CRYPTO/GAS":(finPage==2)?"PNBK":"TREASURIES";
    drawCardHeader(finIcon, finTitle, C_GOLD, 4, finPage);
  }

  if(!finance.valid){ctrTxt1("Loading...",35,C_GRAY);return;}

  if(finPage==0){
    // ===== PAGE 1: S&P 500 + 10yr Treasury + Sparkline =====

    // LEFT COLUMN x=0-63: S&P 500
    txt1("S&P 500",2,15,C_GRAY);
    char sp[8]; snprintf(sp,8,"%.0f",finance.sp500);
    txt2(sp,2,23,C_WHITE);               // size-2: y=23..39
    bool spUp=(finance.sp500Chg>=0);
    char spC[10]; snprintf(spC,10,"%+.2f%%",finance.sp500Chg);
    fillRect(2,40,62,8,spUp?dsp->color565(0,40,0):dsp->color565(40,0,0));
    txt1(spC,4,41,spUp?C_LIME:C_RED);   // y=41, safe

    // RIGHT COLUMN x=66-127: 10yr Treasury
    dsp->drawFastVLine(65,15,34,C_DARKGRAY);
    txt1("10-YR",68,15,C_GRAY);
    if(finance.treasury10y>0){
      char tr[8]; snprintf(tr,8,"%.2f%%",finance.treasury10y);
      txt2(tr,68,23,C_CYAN);            // size-2: y=23..39
    } else {
      txt1("-- %",68,28,C_DARKGRAY);
      txt1("Get FRED key",68,37,dsp->color565(50,50,70));
    }
    txt1("T-BOND",68,41,C_DARKGRAY);    // y=41, safe

    // SPARKLINE SECTION y=49-62 (13px tall, full width)
    dsp->drawFastHLine(0,48,PANEL_WIDTH,C_DARKGRAY);
    if(sp500SparkLen>=2){
      drawSparkline(sp500Spark,sp500SparkLen,0,49,PANEL_WIDTH,13,spUp);
      // "S&P TODAY" label on left
      txt1("TODAY",2,50,dsp->color565(60,60,80));
    } else {
      // No sparkline data yet — show F&G gauge compactly in this area
      const char* fgLbl; uint16_t fgC2;
      if(finance.fearGreed>=75){fgLbl="EXT GREED";fgC2=C_LIME;}
      else if(finance.fearGreed>=55){fgLbl="GREED";fgC2=C_GREEN;}
      else if(finance.fearGreed>=45){fgLbl="NEUTRAL";fgC2=C_YELLOW;}
      else if(finance.fearGreed>=25){fgLbl="FEAR";fgC2=C_ORANGE;}
      else{fgLbl="EXT FEAR";fgC2=C_RED;}
      // Gradient bar y=51
      for(int gx=0;gx<PANEL_WIDTH;gx++){
        uint8_t r=(uint8_t)((gx<64)?0:map(gx,64,127,0,220));
        uint8_t g2=(uint8_t)((gx<64)?map(gx,0,63,220,150):map(gx,64,127,150,0));
        dsp->drawPixel(gx,51,dsp->color565(r>>3,g2>>3,0));
        dsp->drawPixel(gx,52,dsp->color565(r>>3,g2>>3,0));
      }
      int fgX=constrain(map(finance.fearGreed,0,100,1,126),1,126);
      fillRect(fgX-1,50,3,4,C_WHITE);
      char fgL[22]; snprintf(fgL,22,"F&G %d %s",finance.fearGreed,fgLbl);
      ctrTxt1(fgL,55,fgC2);             // y=55, safe max for size-1
    }

  } else if(finPage==1){
    // ===== PAGE 2: BTC + Gas + Fear & Greed =====

    // LEFT COLUMN x=0-87: Bitcoin
    txt1("BITCOIN",2,15,C_GRAY);
    char btc[12]; snprintf(btc,12,"$%.0f",finance.btcUSD);
    txt2(btc,2,23,C_GOLD);             // size-2: y=23..39
    bool btcUp=(finance.btcChg>=0);
    char btcC[14]; snprintf(btcC,14,"%+.2f%% 24h",finance.btcChg);
    fillRect(2,40,86,8,btcUp?dsp->color565(0,40,0):dsp->color565(40,0,0));
    txt1(btcC,4,41,btcUp?C_LIME:C_RED);

    // RIGHT COLUMN x=90-127: Gas price
    dsp->drawFastVLine(89,15,34,C_DARKGRAY);
    txt1("GAS",93,15,C_GRAY);
    if(finance.gasNat>0){
      char gas[8]; snprintf(gas,8,"$%.2f",finance.gasNat);
      // Right column x=90-127 is only 38px — use size-1, centered
      int gw=(int)strlen(gas)*6;
      txt1(gas, 90+(38-gw)/2, 26, C_YELLOW);
      txt1("/gal",101,36,C_DARKGRAY);
    } else {
      txt1("N/A",98,28,C_DARKGRAY);
      txt1("EIA key",91,38,dsp->color565(50,50,50));
    }

    // FEAR & GREED y=49-62
    dsp->drawFastHLine(0,48,PANEL_WIDTH,C_DARKGRAY);
    const char* fgLbl; uint16_t fgC2;
    if(finance.fearGreed>=75){fgLbl="EXT GREED";fgC2=C_LIME;}
    else if(finance.fearGreed>=55){fgLbl="GREED";fgC2=C_GREEN;}
    else if(finance.fearGreed>=45){fgLbl="NEUTRAL";fgC2=C_YELLOW;}
    else if(finance.fearGreed>=25){fgLbl="FEAR";fgC2=C_ORANGE;}
    else{fgLbl="EXT FEAR";fgC2=C_RED;}
    // Gradient bar (2px) y=50-51
    for(int gx=0;gx<PANEL_WIDTH;gx++){
      uint8_t r=(uint8_t)((gx<64)?0:map(gx,64,127,0,220));
      uint8_t g2=(uint8_t)((gx<64)?map(gx,0,63,220,150):map(gx,64,127,150,0));
      dsp->drawPixel(gx,50,dsp->color565(r>>3,g2>>3,0));
      dsp->drawPixel(gx,51,dsp->color565(r>>3,g2>>3,0));
    }
    int fgX=constrain(map(finance.fearGreed,0,100,1,126),1,126);
    fillRect(fgX-1,49,3,4,C_WHITE);    // marker above/through bar
    char fgL[22]; snprintf(fgL,22,"F&G %d  %s",finance.fearGreed,fgLbl);
    ctrTxt1(fgL,54,fgC2);              // y=54, ends y=62, safe

  } else if(finPage==2){
    // ===== PAGE 3: PNBK + SOFR + Fed Funds =====

    // ── PNBK row y=15..36 — Patriot National Bancorp ──
    txt1("PNBK", 2, 15, dsp->color565(120,160,200));
    txt1("Patriot Natl Bcp", 32, 15, dsp->color565(70,90,130));

    if(finance.pnbk > 0){
      char pn[10]; snprintf(pn, 10, "$%.2f", finance.pnbk);
      txt2(pn, 2, 23, C_WHITE);                      // size-2 y=23..38
      bool pnUp = (finance.pnbkChg >= 0);
      char pnC[10]; snprintf(pnC, 10, "%+.2f%%", finance.pnbkChg);
      // small chip on right
      int chipW = (int)strlen(pnC)*6 + 4;
      int chipX = PANEL_WIDTH - chipW - 2;
      fillRect(chipX, 24, chipW, 9, pnUp?dsp->color565(0,40,0):dsp->color565(40,0,0));
      txt1(pnC, chipX+2, 25, pnUp?C_LIME:C_RED);
    } else {
      txt1("--", 2, 28, C_DARKGRAY);
    }

    // ── Divider y=39
    dsp->drawFastHLine(0, 39, PANEL_WIDTH, C_DARKGRAY);

    // ── Bottom row: SOFR | Fed Funds, two columns split at x=64 ──
    // Left column x=0..63: SOFR
    txt1("SOFR", 4, 42, dsp->color565(100,120,160));
    char sf[10];
    if(finance.sofr > 0) snprintf(sf, 10, "%.2f%%", finance.sofr);
    else strcpy(sf, "--");
    txt2(sf, 4, 50, C_CYAN);

    // Vertical divider
    dsp->drawFastVLine(64, 41, 22, C_DARKGRAY);

    // Right column x=66..127: Fed Funds
    txt1("FED FUNDS", 68, 42, dsp->color565(100,120,160));
    char ff[10];
    if(finance.fedFunds > 0) snprintf(ff, 10, "%.2f%%", finance.fedFunds);
    else strcpy(ff, "--");
    txt2(ff, 68, 50, C_GOLD);

  } else {
    // ===== PAGE 4: TREASURY CURVE — 3yr / 5yr / 10yr side by side =====

    // Three equal columns at x=0..41, 43..84, 86..127.
    // Each: maturity label (dim), yield size-2 (color), with a mini bar
    // under each scaled to 0..6% for an at-a-glance curve shape.
    struct { const char* lbl; float v; uint16_t c; } trs[3] = {
      { "3-YR",  finance.treasury3y,  C_CYAN   },
      { "5-YR",  finance.treasury5y,  C_YELLOW },
      { "10-YR", finance.treasury10y, C_GOLD   },
    };
    for(int i=0;i<3;i++){
      int colX = i*43;
      int colW = 41;
      // Label centered in column
      int lw = (int)strlen(trs[i].lbl)*6;
      txt1(trs[i].lbl, colX+(colW-lw)/2, 17, dsp->color565(100,120,160));
      // Yield size-2 centered
      char yv[8];
      if(trs[i].v > 0) snprintf(yv, 8, "%.2f", trs[i].v);
      else strcpy(yv, "--");
      int vw = (int)strlen(yv)*12;
      txt2(yv, colX+(colW-vw)/2, 27, trs[i].c);
      // small % suffix
      txt1("%", colX+(colW+vw)/2+1, 31, dsp->color565(90,100,130));
      // Mini bar: height scaled 0..6% → 0..14px, anchored at y=60
      int bh = (trs[i].v > 0) ? constrain((int)(trs[i].v/6.0f*14), 1, 14) : 0;
      fillRect(colX+8, 60-bh, colW-16, bh, trs[i].c);
      dsp->drawFastHLine(colX+4, 60, colW-8, C_DARKGRAY);
      if(i<2) dsp->drawFastVLine(colX+colW+1, 16, 46, dsp->color565(35,35,50));
    }
    // Curve-shape hint: INVERTED if 3yr > 10yr
    if(finance.treasury3y > 0 && finance.treasury10y > 0){
      bool inverted = finance.treasury3y > finance.treasury10y;
      if(inverted) txt1("INV", 2, 17, C_RED);
    }
  }
}

// ----------------------------------------------------------
// Concerts slide — upcoming shows within 30mi of 06903 (Stamford CT)
// Cycles through up to 5 events, one per card, 6 sec each.
void renderConcerts(){
  static int concertDispIdx=0;
  static unsigned long lastCycle=0;

  bool newSlide=(lastStaticDraw==0);
  if(newSlide){ concertDispIdx=0; lastCycle=millis(); }

  // Advance every 6 seconds
  if(concertCount>1 && millis()-lastCycle>6000){
    concertDispIdx=(concertDispIdx+1)%concertCount;
    lastCycle=millis();
    lastStaticDraw=0;   // force redraw
  }

  if(millis()-lastStaticDraw<STATIC_REDRAW_MS) return;
  lastStaticDraw=millis();
  cls();

  // ── v10 unified chrome — guitar icon, purple accent, event counter right
  uint16_t accC=dsp->color565(200,100,255); // bright purple accent
  drawCardHeader("concert", "EVENTS", accC, -1);
  if(concertCount > 1){
    char cc[8]; snprintf(cc, 8, "%d/%d", concertDispIdx+1, concertCount);
    int cw = (int)strlen(cc)*6;
    txt1(cc, PANEL_WIDTH-cw-2, 3, dimColor565(accC, 75));
  }

  // ── No key / no data ──
  if(strlen(TICKETMASTER_KEY)==0){
    ctrTxt1("Add TICKETMASTER_KEY",22,dsp->color565(150,100,200));
    ctrTxt1("developer.ticketmaster",32,C_DARKGRAY);
    ctrTxt1(".com (free)",42,C_DARKGRAY);
    return;
  }
  if(concertCount==0){
    ctrTxt1("Loading...",30,C_GRAY);
    ctrTxt1("(or no shows nearby)",42,C_DARKGRAY);
    return;
  }

  // GLITCH GUARD: a background refetch can SHRINK concertCount while this
  // card is up; an unclamped index then reads past the array (garbage card
  // or crash). Clamp every frame.
  if(concertDispIdx >= concertCount) concertDispIdx = 0;
  ConcertEvent &c=concerts[concertDispIdx];

  // ── Artist/Event name ── (y=15 area)
  // Use size-2 if ≤10 chars, else split across 2 lines at size-1
  String nm=c.name;
  uint16_t nameC=C_WHITE;
  if(nm.length()<=10){
    txt2(nm.c_str(), (PANEL_WIDTH-(int)nm.length()*12)/2, 15, nameC);
  } else {
    // Wrap at nearest space before char 21
    int cut=min((int)nm.length(),21);
    int spacePos=-1;
    for(int i=cut-1;i>0;i--){ if(nm[i]==' '){spacePos=i;break;} }
    String line1 = (spacePos>0) ? nm.substring(0,spacePos) : nm.substring(0,21);
    String line2 = (spacePos>0) ? nm.substring(spacePos+1,min((int)nm.length(),spacePos+22)) : "";
    // Center each line
    int x1=(PANEL_WIDTH-(int)line1.length()*6)/2; if(x1<0)x1=0;
    txt1(line1.c_str(), x1, 16, nameC);
    if(line2.length()>0){
      int x2=(PANEL_WIDTH-(int)line2.length()*6)/2; if(x2<0)x2=0;
      txt1(line2.c_str(), x2, 25, dsp->color565(220,180,255));
    }
  }

  // ── Date + Time ── (y=36)
  dsp->drawFastHLine(4,34,PANEL_WIDTH-8,dsp->color565(60,0,80));
  String dateStr=formatConcertDate(c.date,c.time);
  ctrTxt1(dateStr.c_str(),36,C_GOLD);

  // ── Venue ── (y=45)
  String vn=c.venue; if(vn.length()>21) vn=vn.substring(0,21);
  ctrTxt1(vn.c_str(),45,dsp->color565(180,150,220));

  // ── City ── (y=54) — TM returns city for NY venues too, don't hardcode state
  if(c.city.length()>0){
    String loc=c.city; if(loc.length()>21) loc=loc.substring(0,21);
    ctrTxt1(loc.c_str(),54,C_DARKGRAY);
  }

  // ── Page dots ── (y=60)
  if(concertCount>1){
    int dCount=min(concertCount,MAX_CONCERTS);
    int startX=(PANEL_WIDTH-dCount*6)/2;
    for(int i=0;i<dCount;i++){
      uint16_t dc=(i==concertDispIdx)?accC:dsp->color565(50,0,70);
      dsp->fillCircle(startX+i*6+2,60,2,dc);
    }
  }
}

// ----------------------------------------------------------
// Red Fox Road seasonal scene - BG drawn ONCE, animation delta only
void renderRFRScene(){
  String season=getSeason();
  static String lastSeason="";
  bool drawBG=(lastStaticDraw==0 || season!=lastSeason);
  if(drawBG){
    lastSeason=season; lastStaticDraw=millis();
    cls();
    uint16_t groundC=(season=="winter")?dsp->color565(210,215,220):
                     (season=="fall")  ?dsp->color565(75,105,28):dsp->color565(55,165,45);
    if(season=="winter"){
      drawGradientSkyRGB(0,44,6,10,45,18,24,64);
      drawStars(20,44);
      drawMoonFull(109,13,6,getMoonPhase());
    } else if(season=="spring"){
      drawGradientSkyRGB(0,44,95,178,255,164,218,255);
      drawSoftCloud(9,8,24,dsp->color565(235,245,255),dsp->color565(180,210,235));
      drawSoftCloud(92,13,22,dsp->color565(235,245,255),dsp->color565(180,210,235));
    } else if(season=="summer"){
      drawGradientSkyRGB(0,44,72,162,255,155,214,255);
      drawSun(108,12,7);
      drawSoftCloud(12,10,20,dsp->color565(238,248,255),dsp->color565(185,218,240));
    } else {
      drawGradientSkyRGB(0,44,210,150,86,138,92,48);
      dsp->fillCircle(108,13,6,dsp->color565(255,145,55));
    }
    // Ground fills to bottom
    drawLowHills(season=="winter"?dsp->color565(35,45,75):dsp->color565(75,135,70),
                 season=="fall"?dsp->color565(110,90,38):dsp->color565(45,125,52), 39);
    drawGroundTexture(44,groundC,
                      season=="winter"?C_WHITE:dsp->color565(35,95,30),
                      season=="winter"?dsp->color565(235,240,245):dsp->color565(105,185,70));
    drawStonePath(68,48,10,34,dsp->color565(185,178,158),dsp->color565(118,112,98));
    drawSplitRailFence(51,dsp->color565(135,88,42),dsp->color565(75,45,18));
    // ── POND behind the house (top-left of the lawn) ──
    // Drawn BEFORE the house so the house draws on top, giving the impression
    // the pond sits behind. Small oval, water color tuned to the season.
    {
      uint16_t pondMain, pondHi, pondShade;
      if(season=="winter"){
        pondMain  = dsp->color565(180,200,225);   // ice blue-white
        pondHi    = C_WHITE;
        pondShade = dsp->color565(140,165,200);
      } else if(season=="fall"){
        pondMain  = dsp->color565(45,80,120);
        pondHi    = dsp->color565(100,150,200);
        pondShade = dsp->color565(25,55,90);
      } else {
        pondMain  = dsp->color565(35,115,175);
        pondHi    = dsp->color565(90,180,230);
        pondShade = dsp->color565(15,80,135);
      }
      // Filled oval centered at (95, 41), 14px wide × 5px tall
      int pcx=98, pcy=42, pw=8, ph=3;
      for(int dy=-ph; dy<=ph; dy++){
        for(int dx=-pw; dx<=pw; dx++){
          // Ellipse equation: (dx/pw)² + (dy/ph)² ≤ 1
          int ex = dx*dx*ph*ph, ey = dy*dy*pw*pw;
          if(ex + ey <= pw*pw*ph*ph){
            uint16_t c = (dy < 0) ? pondHi : (dy > ph-1 ? pondShade : pondMain);
            dsp->drawPixel(pcx+dx, pcy+dy, c);
          }
        }
      }
      // White surface highlight pixel (sun glint) — only spring/summer
      if(season=="spring" || season=="summer"){
        dsp->drawPixel(pcx-3, pcy-1, C_WHITE);
        dsp->drawPixel(pcx+4, pcy,   C_WHITE);
      }
      // Reedy grass tufts at pond edge (not in winter — frozen)
      if(season!="winter"){
        uint16_t reed = dsp->color565(60,140,40);
        for(int r=0; r<3; r++){
          int rx = pcx-pw-2 + r*2;
          dsp->drawPixel(rx, pcy-1, reed);
          dsp->drawPixel(rx, pcy,   reed);
        }
        for(int r=0; r<3; r++){
          int rx = pcx+pw+1 + r*2;
          if(rx < 128){
            dsp->drawPixel(rx, pcy-1, reed);
            dsp->drawPixel(rx, pcy,   reed);
          }
        }
      }
    }
    // House - centered, bigger for full 128 width
    drawRFRHouse(42,6);
    // Trees flanking house
    drawPineTree(8,63); drawPineTree(22,63); drawPineTree(108,63); drawPineTree(120,63);
    if(season=="winter") for(int i=0;i<8;i++) dsp->drawFastHLine(42+i,6+i,44-i*2,C_WHITE);
    if(season=="spring"){
      // Flowers along path
      uint16_t fc[]={C_PINK,C_YELLOW,C_WHITE,dsp->color565(255,130,0)};
      for(int i=0;i<8;i++){ dsp->fillCircle(45+i*6,44,2,fc[i%4]); }

      // ── BEEHIVE next to the pond — small wooden Langstroth-style hive
      // Position: just right of the pond at (98, 42), so hive sits at (110, 38..43)
      // The hive entry hole is the dark center where the bees would come/go.
      uint16_t hiveBnd = dsp->color565(170, 105, 30);   // medium honey wood
      uint16_t hiveHi  = dsp->color565(220, 165, 70);   // sunny highlight
      uint16_t hiveSh  = dsp->color565(110, 70, 18);    // shadow
      uint16_t hiveTop = dsp->color565(80, 50, 12);     // peaked roof
      int hx = 110, hy = 39;     // top-left of hive body
      // Roof — pointy slate top (4 wide, 2 tall)
      dsp->drawFastHLine(hx,   hy,   4, hiveTop);
      dsp->drawFastHLine(hx-1, hy+1, 6, hiveTop);
      // Stacked supers (3 horizontal bands)
      fillRect(hx-1, hy+2, 6, 1, hiveBnd);
      fillRect(hx-1, hy+3, 6, 1, hiveHi);
      fillRect(hx-1, hy+4, 6, 1, hiveBnd);
      fillRect(hx-1, hy+5, 6, 1, hiveHi);
      // Side shadows
      dsp->drawPixel(hx+4, hy+2, hiveSh);
      dsp->drawPixel(hx+4, hy+3, hiveSh);
      dsp->drawPixel(hx+4, hy+4, hiveSh);
      dsp->drawPixel(hx+4, hy+5, hiveSh);
      // Landing board
      fillRect(hx-2, hy+6, 8, 1, hiveSh);
      // Entry hole
      dsp->drawPixel(hx+1, hy+5, dsp->color565(20, 12, 4));
      dsp->drawPixel(hx+2, hy+5, dsp->color565(20, 12, 4));
      // Tiny "BEES" sign or just pile of grass at base
      dsp->drawPixel(hx,   hy+7, dsp->color565(60, 140, 30));
      dsp->drawPixel(hx+3, hy+7, dsp->color565(60, 140, 30));
      // STATIC bees at the hive entrance — drawn once, never animated, so
      // they can sit right against the hive with zero streak risk.
      dsp->drawPixel(hx-3, hy+5, C_YELLOW);                    // bee on landing board
      dsp->drawPixel(hx-2, hy+5, dsp->color565(60,40,10));
      dsp->drawPixel(hx+6, hy+3, C_YELLOW);                    // bee on hive side
      dsp->drawPixel(hx+6, hy+4, dsp->color565(60,40,10));
    }
    // Label at TOP of sky with outline for high contrast
    // Title is size-1 ("RED FOX RD" = 10 chars * 6 = 60px, centered)
    uint16_t titleC;
    if(season=="winter")       titleC=C_CYAN;
    else if(season=="spring")  titleC=dsp->color565(255,220,255);
    else if(season=="summer")  titleC=C_YELLOW;
    else                       titleC=C_ORANGE;
    ctrTxtOutline("RED FOX RD",1,titleC);
    if(season=="summer"){ ballX=30;ballY=49;ballVX=1.4f;ballVY=0.7f;bennyX=5;bennyY=47; }
    if(season=="winter" && nightMode) drawBennySleep(68,48); // sleeping Benny by fire
    if(!flakesInited) initFlakes();
  }

  // --- ANIMATION: only update changed pixels, no cls() ---
  if(season=="winter"){
    static unsigned long lastSn=0;
    if(millis()-lastSn>55){
      for(int i=0;i<MAX_FLAKES;i++){
        int ox=(int)flakes[i].x, oy=(int)flakes[i].y;
        if(oy>=10&&oy<44) dsp->drawPixel(ox,oy,rfrSkyPixel(season,oy));
        flakes[i].y+=flakes[i].spd; flakes[i].x+=flakes[i].drift;
        if(flakes[i].y>=44){flakes[i].y=10;flakes[i].x=random(0,128);}
        if(flakes[i].x<0)flakes[i].x=127; if(flakes[i].x>127)flakes[i].x=0;
        if((int)flakes[i].y>=10&&(int)flakes[i].y<44) dsp->drawPixel((int)flakes[i].x,(int)flakes[i].y,C_WHITE);
      }
      lastSn=millis();
    }
    // Night mode: sleeping Benny with drifting Zzz
    if(nightMode){
      static unsigned long lastZzB=0; static int zzYb=46;
      if(millis()-lastZzB>380){
        // Erase old Zzz pixel with sky color
        uint16_t skyE=rfrSkyPixel(season,zzYb);
        dsp->drawPixel(81,zzYb,skyE); dsp->drawPixel(84,zzYb-2,skyE);
        zzYb--; if(zzYb<39) zzYb=46;
        dsp->drawPixel(81,zzYb,dsp->color565(120,120,220));
        dsp->drawPixel(84,zzYb-2,dsp->color565(90,90,180));
        lastZzB=millis();
      }
    }
  } else if(season=="spring"){
    // 2 hover-bees in the PURE-SKY band above the hive.
    // STREAK ROOT CAUSE (finally found): the hills from drawLowHills(...,39)
    // span y≈26..44 across x=100..120 — exactly the old flight zone — but
    // rfrBgPixel() returns plain sky for all y<44. Every erase therefore
    // painted sky-colored streaks straight through the hill triangles.
    // The only band at this x-range that is GUARANTEED pure sky is
    // y=22..31 (below the cloud at y≈13..21, above the hill tops at y≈32).
    // Bees now hover there; two extra STATIC bees sit at the hive entrance
    // (painted in the BG block — zero erase, zero streak risk).
    static float beeX[2] = { 105, 113 };
    static float beeY[2] = { 26, 28 };
    static float beeVX[2]= { 0.35f, -0.3f };
    static float beeVY[2]= { 0.15f, 0.2f };
    static int   beeWing[2] = { 0, 0 };
    static unsigned long lastBee = 0;
    if(millis() - lastBee > 80){
      lastBee = millis();
      for(int i=0; i<2; i++){
        int ox = (int)beeX[i], oy = (int)beeY[i];
        // Erase OLD bee — 4×3 footprint, hard-clamped to the pure-sky band
        for(int dy = -1; dy <= 1; dy++){
          for(int dx = -1; dx <= 2; dx++){
            int xx = ox+dx, yy = oy+dy;
            if(xx<0 || xx>=PANEL_WIDTH || yy<22 || yy>31) continue;
            dsp->drawPixel(xx, yy, rfrBgPixel(xx, yy, season));
          }
        }
        // Advance — bounce inside the safe band only
        beeX[i] += beeVX[i]; beeY[i] += beeVY[i];
        if(beeX[i] < 102 || beeX[i] > 117) beeVX[i] = -beeVX[i];
        if(beeY[i] < 24  || beeY[i] > 30)  beeVY[i] = -beeVY[i];
        beeWing[i] = (beeWing[i] + 1) & 1;
        int nx = (int)beeX[i], ny = (int)beeY[i];
        if(nx>=0 && nx<PANEL_WIDTH-1 && ny>=24 && ny<=30){
          dsp->drawPixel(nx,   ny, C_YELLOW);
          dsp->drawPixel(nx+1, ny, dsp->color565(60,40,10));   // black stripe
          if(beeWing[i]) dsp->drawPixel(nx,   ny-1, dsp->color565(180,200,255));
          else           dsp->drawPixel(nx+1, ny-1, dsp->color565(180,200,255));
        }
      }
    }
  } else if(season=="summer"){
    // Smooth sprite-delta — Benny + ball animated in the SAFE GRASS ZONE
    // y=44..57, x=4..38. This zone has no fence, path, or trees so the
    // bg sampler can cleanly restore pixels behind sprite movement.
    // Ball: 5px footprint (3px diameter circle).
    // Benny: 16 wide × 10 tall side-profile sprite (see drawBenny) — the
    // erase box below is sized to that footprint +1px margin. Keep in sync!
    static unsigned long lastBall = 0;
    if(millis() - lastBall > 60){
      lastBall = millis();

      // ── Erase old Benny (16×10 sprite → 18×12 erase with margin)
      int ox = (int)bennyX, oy = (int)bennyY;
      for(int dy=-1; dy<=10; dy++){
        for(int dx=-1; dx<=16; dx++){
          int xx = ox+dx, yy = oy+dy;
          if(xx<0 || xx>=PANEL_WIDTH || yy<44 || yy>=58) continue;
          dsp->drawPixel(xx, yy, rfrBgPixel(xx, yy, season));
        }
      }
      // ── Erase old ball (5×5 footprint)
      int bx = (int)ballX, by = (int)ballY;
      for(int dy=-3; dy<=3; dy++){
        for(int dx=-3; dx<=3; dx++){
          int xx = bx+dx, yy = by+dy;
          if(xx<0 || xx>=PANEL_WIDTH || yy<44 || yy>=58) continue;
          if(dx*dx + dy*dy <= 9)
            dsp->drawPixel(xx, yy, rfrBgPixel(xx, yy, season));
        }
      }

      // ── Advance ball physics
      ballX += ballVX; ballY += ballVY;
      if(ballX < 6) { ballX = 6;  ballVX =  fabs(ballVX); }
      if(ballX > 36){ ballX = 36; ballVX = -fabs(ballVX); }
      if(ballY < 45){ ballY = 45; ballVY =  fabs(ballVY); }
      if(ballY > 55){ ballY = 55; ballVY = -fabs(ballVY); }

      // ── Benny chases ball
      float dx = ballX-bennyX, dy = ballY-bennyY;
      float dist = sqrtf(dx*dx + dy*dy);
      if(dist > 3){
        bennyX += dx/dist * 0.7f;
        bennyY += dy/dist * 0.4f;
      }
      bennyX = constrain(bennyX, 4, 30);
      // Max y=47: sprite bottom (oy+9) stays inside the erased zone (y<58)
      bennyY = constrain(bennyY, 44, 47);
      bennyWag = !bennyWag;

      // ── Draw new ball + Benny
      dsp->fillCircle((int)ballX, (int)ballY, 2, dsp->color565(180, 230, 50));
      dsp->drawPixel((int)ballX, (int)ballY, dsp->color565(255,255,200));
      drawBenny((int)bennyX, (int)bennyY, bennyWag);
    }
  } else { // fall
    static float lX[10],lY[10],lS[10]; static bool lI=false;
    if(!lI){for(int i=0;i<10;i++){lX[i]=random(0,128);lY[i]=random(10,44);lS[i]=0.3f+random(0,7)*0.1f;}lI=true;}
    uint16_t lC[]={C_ORANGE,C_RED,C_GOLD,dsp->color565(180,100,20)};
    static unsigned long lastL=0;
    if(millis()-lastL>80){
      for(int i=0;i<10;i++){
        int ox=(int)lX[i],oy=(int)lY[i];
        if(ox>=0&&ox<128&&oy>=10&&oy<44){
          dsp->drawPixel(ox,oy,rfrSkyPixel(season,oy));
        }
        lY[i]+=lS[i]; lX[i]+=(float)sin(i*1.1f+lY[i]*0.09f)*0.6f;
        if(lY[i]>=44){lY[i]=10;lX[i]=random(0,128);}
        int nx=(int)lX[i],ny=(int)lY[i];
        if(nx>=0&&nx<128&&ny>=10&&ny<44) dsp->drawPixel(nx,ny,lC[i%4]);
      }
      lastL=millis();
    }
  }
}

// ----------------------------------------------------------
void renderRFRWeather(){
  static int wxPage=0;
  static unsigned long wxPageStart=0;
  bool newSlide=(lastStaticDraw==0);
  if(newSlide){wxPage=0;wxPageStart=millis();}
  if(millis()-wxPageStart>4500){wxPage=(wxPage+1)%2;wxPageStart=millis();lastStaticDraw=0;}
  if(millis()-lastStaticDraw<STATIC_REDRAW_MS) return;
  lastStaticDraw=millis();
  cls();
  // v10 unified chrome
  drawCardHeader("weather", "STAMFORD CT", C_CYAN, 2, wxPage);

  if(!homeWx.valid){ctrTxt1("Loading weather...",32,C_GRAY);return;}

  if(wxPage==0){
    // PAGE 1: current conditions + 5-day forecast
    // LEFT 54px: current conditions
    fillRect(0,14,54,50,dsp->color565(3,8,22));
    dsp->drawFastVLine(53,14,50,C_DARKGRAY);
    // Condition text y=15
    txt1(trimTo(homeWx.condition,9).c_str(),1,15,C_WHITE);
    // Weather icon
    drawWxIcon(44,15,4,homeWx.icon);
    // Big temp y=24 (size-2 ends y=40)
    char tmp2[6]; snprintf(tmp2,6,"%.0fF",homeWx.tempF);
    txt2(tmp2,1,24,tempColor(homeWx.tempF));
    // Separator
    dsp->drawFastHLine(0,42,53,C_DARKGRAY);
    // Feels like y=44 (ends y=52)
    char fls2[12]; snprintf(fls2,12,"FL:%.0fF",homeWx.feelsF);
    txt1(fls2,1,44,C_GRAY);
    // Wind y=54 (ends y=62)
    char wnd2[10]; snprintf(wnd2,10,"%.0fmph",homeWx.windMph);
    txt1(wnd2,1,54,C_CYAN);

    // RIGHT 74px: 5-day forecast x=55-127
    int days2=min(homeWx.forecastCount,5);
    fillRect(55,14,73,50,C_BLACK);
    for(int i=0;i<days2;i++){
      int x=55+i*14; if(x+12>PANEL_WIDTH) break;
      txt1(homeWx.forecast[i].label.substring(0,2).c_str(),x+1,16,C_CYAN);
      drawWxIcon(x+6,28,3,homeWx.forecast[i].icon);
      char hi2[5]; snprintf(hi2,5,"%.0f",homeWx.forecast[i].hiF);
      txt1(hi2,x+1,36,tempColor(homeWx.forecast[i].hiF));
      char lo2[5]; snprintf(lo2,5,"%.0f",homeWx.forecast[i].loF);
      txt1(lo2,x+1,44,tempColor(homeWx.forecast[i].loF));
      int bH=constrain((int)((homeWx.forecast[i].hiF-homeWx.forecast[i].loF)/4),1,5);
      fillRect(x+2,56-bH,9,bH,tempColor(homeWx.forecast[i].hiF));
      fillRect(x+2,56,9,2,tempColor(homeWx.forecast[i].loF));
      if(i<days2-1) dsp->drawFastVLine(x+13,14,50,dsp->color565(20,20,30));
    }
  } else {
    // PAGE 2: extended details + sun times
    // Big temp centered top
    char tmp2[6]; snprintf(tmp2,6,"%.0fF",homeWx.tempF);
    dsp->setTextSize(3); dsp->setTextColor(tempColor(homeWx.tempF));
    int w=strlen(tmp2)*18; dsp->setCursor((PANEL_WIDTH-w)/2,17);
    dsp->print(tmp2);
    dsp->setTextSize(1);
    // Condition centered y=44
    ctrTxt1(homeWx.condition.c_str(),44,C_WHITE);

    // Sun times y=54
    dsp->drawFastHLine(0,52,PANEL_WIDTH,C_DARKGRAY);
    if(homeWx.sunrise>0){
      // localtime() returns a pointer to a SHARED static buffer - calling it
      // twice clobbers the first result. Use localtime_r with our own structs.
      time_t sr=(time_t)homeWx.sunrise,ss=(time_t)homeWx.sunset;
      struct tm srt, sst;
      localtime_r(&sr,&srt);
      localtime_r(&ss,&sst);
      // 12-hour format with am/pm
      char srB[8],ssB[8];
      strftime(srB,8,"%I:%M%p",&srt); if(srB[0]=='0')memmove(srB,srB+1,sizeof(srB)-1);
      strftime(ssB,8,"%I:%M%p",&sst); if(ssB[0]=='0')memmove(ssB,ssB+1,sizeof(ssB)-1);
      // Lower-case the am/pm to save horizontal space
      for(char*p=srB;*p;p++) if(*p=='A'||*p=='P'||*p=='M') *p+=32;
      for(char*p=ssB;*p;p++) if(*p=='A'||*p=='P'||*p=='M') *p+=32;
      txt1("UP",2,55,C_GOLD);
      txt1(srB,18,55,C_WHITE);
      txt1("DN",68,55,C_ORANGE);
      txt1(ssB,84,55,C_WHITE);
    }
  }
}

// ----------------------------------------------------------
void renderRFRTraffic(){
  if(millis()-lastStaticDraw<STATIC_REDRAW_MS) return;
  lastStaticDraw=millis();
  cls();
  // v10 unified chrome
  drawCardHeader("traffic", "OFFICE COMMUTE", C_YELLOW);
  // Destination y=16-24
  txt1("TO: 100 MASON ST",4,16,C_GRAY);
  txt1("GREENWICH CT",4,24,C_GRAY);
  // Divider y=32 (below dest, 1px breathing room before body)
  dsp->drawFastHLine(0,32,PANEL_WIDTH,C_DARKGRAY);
  if(!trafficWork.valid){
    ctrTxt1("Loading...",44,C_GRAY);
  } else {
    // Two-column layout in body (y=34..63) — avoids stacked overlap
    // Left column: WITH TRAFFIC duration (primary, red)
    // Right column: NORMAL duration (reference, green)
    txt1("TRAFFIC",10,34,C_GRAY);                      // y=34..41
    txt1("NORMAL",82,34,C_GRAY);                       // y=34..41
    dsp->drawFastVLine(64,34,28,dsp->color565(40,40,40));
    // Big size-2 durations at y=45..60 — fits with 3px clearance at bottom
    String dur=shortDur(trafficWork.durationTraffic);
    String norm=shortDur(trafficWork.durationNormal);
    int dw=(int)dur.length()*12;
    int nw=(int)norm.length()*12;
    dsp->setTextSize(2);
    dsp->setTextColor(C_RED);
    dsp->setCursor(max(0,(64-dw)/2),45); dsp->print(dur.c_str());
    dsp->setTextColor(C_LIME);
    dsp->setCursor(64+max(0,(64-nw)/2),45); dsp->print(norm.c_str());
    dsp->setTextSize(1);
  }
}

// ----------------------------------------------------------
// Static campfire base — stones + crossed logs + ember bed.
// Drawn ONCE per slide entry. The dancing flames above are animated by
// drawFireFlames() (see below).
void drawFireBase(int cx, int by){
  uint16_t logDark = dsp->color565( 80, 40, 12);
  uint16_t logMid  = dsp->color565(120, 65, 22);
  uint16_t logHi   = dsp->color565(170, 95, 30);
  uint16_t emberHi = dsp->color565(255,140, 30);
  uint16_t deepRed = dsp->color565(180, 25,  0);
  uint16_t stone   = dsp->color565(95, 92, 80);
  uint16_t stoneHi = dsp->color565(160,158,145);

  // Stone ring — slightly recessed bowl shape rather than a flat rectangle
  // (which is what made the old fire read as a "small house").
  // Outer stones (4 angled humps at ground line)
  for(int i=0;i<5;i++){
    int sx = cx - 6 + i*3;
    dsp->drawPixel(sx,   by+1, stone);
    dsp->drawPixel(sx+1, by,   stone);
    dsp->drawPixel(sx+1, by+1, stoneHi);
    dsp->drawPixel(sx-1, by+1, stoneHi);
  }
  // Center bowl darker
  fillRect(cx-4, by, 9, 1, dsp->color565(60, 50, 35));

  // Two crossed logs angled X-pattern (not horizontal bars)
  // Front log: \  bottom-right to top-left
  dsp->drawLine(cx-6, by-1, cx+5, by-4, logMid);
  dsp->drawLine(cx-6, by,   cx+5, by-3, logHi);
  // Back log: /  bottom-left to top-right
  dsp->drawLine(cx-5, by-4, cx+6, by-1, logDark);
  dsp->drawLine(cx-5, by-3, cx+6, by,   logMid);
  // Log knot details
  dsp->drawPixel(cx-3, by-2, dsp->color565(60, 30, 8));
  dsp->drawPixel(cx+3, by-2, dsp->color565(60, 30, 8));

  // Ember bed glowing between logs (irregular, looks live not boxy)
  dsp->drawPixel(cx-2, by-5, emberHi);
  dsp->drawPixel(cx-1, by-5, deepRed);
  dsp->drawPixel(cx,   by-5, emberHi);
  dsp->drawPixel(cx+1, by-5, deepRed);
  dsp->drawPixel(cx+2, by-5, emberHi);
  dsp->drawPixel(cx-1, by-6, deepRed);
  dsp->drawPixel(cx+1, by-6, deepRed);
}

// Animated flickering flames — called every ~150ms by the cabin animation
// block. Tiny redraw zone (~14×14 px = ~200 pixels) so it stays smooth like
// aurora. Uses 4 cycling silhouette frames + a "wind" offset that picks one.
//
// CALLER CONTRACT: bg color must be passed in so this can erase the flame
// zone cleanly between frames (callers know if they're on snow or grass).
void drawFireFlames(int cx, int by, uint16_t bgC, int frame){
  uint16_t deepRed = dsp->color565(160, 20,  0);
  uint16_t orange  = dsp->color565(255, 95,  0);
  uint16_t yellow  = dsp->color565(255,200, 30);
  uint16_t coreHot = dsp->color565(255,255,200);
  uint16_t halo    = dsp->color565(110, 35,  0);

  // ── Erase the flame zone (rect cx-7..cx+7, by-15..by-6 = 15w × 10h)
  for(int y = by-15; y <= by-6; y++){
    for(int x = cx-7; x <= cx+7; x++){
      if(x < 0 || x >= PANEL_WIDTH || y < 0 || y >= PANEL_HEIGHT) continue;
      dsp->drawPixel(x, y, bgC);
    }
  }

  // Frame index 0..3 controls flame "lean" direction
  // 0 = upright, 1 = lean right, 2 = upright tall, 3 = lean left
  int leanX = (frame == 1) ? 1 : (frame == 3) ? -1 : 0;
  int tipExtra = (frame == 2) ? 1 : 0;

  // Halo glow above the embers — irregular, organic
  for(int dx = -6; dx <= 6; dx++){
    int dy = -7 - (6 - abs(dx))/2;
    if(abs(dx) >= 4) dsp->drawPixel(cx+dx, by+dy, halo);
  }

  // Deep-red flame base (5px wide at ember level)
  fillRect(cx-3+leanX, by-7, 7, 1, deepRed);
  fillRect(cx-2+leanX, by-8, 5, 1, deepRed);

  // Orange middle layer (narrower, lean left/right)
  fillRect(cx-2+leanX, by-9,  5, 1, orange);
  fillRect(cx-1+leanX, by-10, 4, 1, orange);
  // Side orange tongues (tiny licks)
  if(frame != 1) dsp->drawPixel(cx-3+leanX, by-9, orange);
  if(frame != 3) dsp->drawPixel(cx+3+leanX, by-9, orange);

  // Yellow upper flame tongue (taller in frame 2)
  fillRect(cx+leanX, by-11, 3, 1, yellow);
  fillRect(cx+leanX, by-12, 2, 1, yellow);
  if(tipExtra){
    dsp->drawPixel(cx+leanX, by-13, yellow);
    dsp->drawPixel(cx+1+leanX, by-13, yellow);
  }

  // Hot white core — single point, varies by frame for shimmer
  int corePx = (frame & 1) ? cx + 1 + leanX : cx + leanX;
  dsp->drawPixel(corePx, by-10, coreHot);
  if(frame == 2) dsp->drawPixel(corePx, by-11, coreHot);

  // Tip — single pixel, varies by frame
  dsp->drawPixel(cx + leanX, by-(13+tipExtra), yellow);

  // Sparks above the fire — 2-3 randomized sparks per frame
  // (cycles through deterministic positions so it's smooth not random)
  static const int8_t sparkPos[][3] = {
    // dx, dy, frame-mask
    { -2, -15, 0b0011 },  // left spark on frames 0,1
    {  3, -16, 0b0110 },  // right spark on frames 1,2
    {  4, -14, 0b1100 },  // far spark on frames 2,3
    { -3, -14, 0b1001 },  // far-left on frames 0,3
  };
  for(int i = 0; i < 4; i++){
    if(sparkPos[i][2] & (1 << frame)){
      int sx = cx + sparkPos[i][0];
      int sy = by + sparkPos[i][1];
      if(sx >= 0 && sx < PANEL_WIDTH && sy >= 0 && sy < PANEL_HEIGHT){
        dsp->drawPixel(sx, sy, (i & 1) ? orange : yellow);
      }
    }
  }
}

// Convenience wrapper that draws both base and a single flame frame —
// kept so existing callers (drawBG block) get a complete fire on first draw.
void drawBigFire(int cx, int by){
  drawFireBase(cx, by);
  // Pick season-appropriate bg for initial flame
  // Default to a medium green (we're in spring/fall lawn or winter snow);
  // the per-frame animation will sample the real bg color via cabinBgPixel.
  drawFireFlames(cx, by, dsp->color565(50, 160, 40), 0);
}

// ----------------------------------------------------------
// Cabin seasonal scene - BG drawn ONCE, animation delta only
void renderCabinScene(){
  String season=getSeason();
  static String lastSeason2="";
  bool drawBG=(lastStaticDraw==0 || season!=lastSeason2);
  if(drawBG){
    lastSeason2=season; lastStaticDraw=millis();
    cls();
    if(season=="winter"){
      drawGradientSkyRGB(0,44,4,8,38,16,20,56);
      drawStars(20,44);
      drawMoonFull(111,12,5,getMoonPhase());
      drawFirLine(43,true); drawFirLine(47,false);
      drawGroundTexture(44,dsp->color565(200,210,215),C_WHITE,dsp->color565(235,240,245));
    } else if(season=="spring"){
      uint16_t logC=dsp->color565(100,55,20);
      dsp->drawLine(76,55,82,51,logC); dsp->drawLine(88,55,82,51,logC);
      fillRect(78,50,8,2,dsp->color565(90,44,14));
      fillRect(80,49,5,3,dsp->color565(255,70,0));
      dsp->fillCircle(82,48,2,dsp->color565(255,160,20));
      dsp->drawPixel(82,46,C_YELLOW);
      drawGradientSkyRGB(0,44,98,178,255,160,216,255);
      drawSoftCloud(8,8,23,dsp->color565(235,245,255),dsp->color565(180,210,235));
      drawFirLine(43,true); drawFirLine(47,false);
      drawGroundTexture(44,dsp->color565(50,160,40),dsp->color565(30,92,28),dsp->color565(100,188,75));
    } else if(season=="summer"){
      // ── SUMMER = big open lake. No cabin, no foreground pines — just
      //    Lake Wallenpaupack: sky, sun, far tree-lined shore, a dock,
      //    and (in the animation layer) a bear water-skiing across.
      // Sky gradient y=0..25 (warm bright summer blue)
      drawGradientSkyRGB(0, CABIN_LAKE_TOP, 78,165,255, 150,210,250);
      // Sun high-right with a soft glow ring
      dsp->fillCircle(108, 9, 6, dsp->color565(255, 240, 170));
      dsp->fillCircle(108, 9, 4, dsp->color565(255, 252, 225));
      dsp->drawCircle(108, 9, 8, dsp->color565(255, 230, 140));
      // A couple of soft clouds for depth
      drawSoftCloud(20, 7, 22, dsp->color565(245,250,255), dsp->color565(195,220,240));
      drawSoftCloud(70, 4, 16, dsp->color565(245,250,255), dsp->color565(195,220,240));

      // Far shore — distant pine silhouette tree-line right at the horizon.
      // Drawn as a thin dark-green band of little triangles so the water has
      // a believable far edge.
      uint16_t farShore = dsp->color565(20, 60, 38);
      uint16_t farShoreHi= dsp->color565(34, 84, 50);
      dsp->drawFastHLine(0, CABIN_LAKE_TOP-1, PANEL_WIDTH, dsp->color565(60,110,80));
      for(int x=-1; x<PANEL_WIDTH+3; x+=5){
        int h = 3 + ((x*5)&3);
        uint16_t c = ((x/5)&1) ? farShore : farShoreHi;
        dsp->fillTriangle(x, CABIN_LAKE_TOP-1, x+2, CABIN_LAKE_TOP-1-h, x+5, CABIN_LAKE_TOP-1, c);
      }

      // Lake body — fill y=CABIN_LAKE_TOP..63 via the sampler so the animated
      // bear can erase by sampling and never scar the water.
      for(int y=CABIN_LAKE_TOP; y<PANEL_HEIGHT; y++)
        for(int x=0; x<PANEL_WIDTH; x++)
          dsp->drawPixel(x, y, cabinLakePixel(x, y));

      // Wooden dock — bottom-left, jutting out toward the water with pilings.
      {
        uint16_t plank  = dsp->color565(140, 95, 50);
        uint16_t plankHi= dsp->color565(175, 125, 72);
        uint16_t plankSh= dsp->color565(95, 62, 30);
        // Deck planks (perspective: wider at bottom)
        for(int y=54; y<64; y++){
          int w = map(y, 54, 63, 16, 30);
          dsp->drawFastHLine(0, y, w, plank);
          if((y & 1)==0) dsp->drawFastHLine(0, y, w, plankSh);  // plank seams
        }
        dsp->drawFastHLine(0, 54, 16, plankHi);                 // sunlit front edge
        // Pilings poking up through the deck
        fillRect(6, 50, 2, 6, plankSh);
        fillRect(20, 52, 2, 6, plankSh);
        dsp->drawPixel(6, 50, plankHi); dsp->drawPixel(20, 52, plankHi);
      }

      // A tiny distant sailboat for scale, far right, sitting right at the
      // horizon line so it stays clear of the water-ski bear's erase band.
      {
        int sx = 96, sy = CABIN_LAKE_TOP - 2;
        dsp->drawFastVLine(sx, sy-5, 5, C_WHITE);              // mast
        dsp->fillTriangle(sx, sy-5, sx, sy-1, sx+4, sy-1, dsp->color565(235,235,245)); // sail
        dsp->drawFastHLine(sx-2, sy, 6, dsp->color565(120,80,40)); // hull
      }
    } else {
      drawGradientSkyRGB(0,44,190,132,68,116,78,32);
      dsp->fillCircle(108,12,5,dsp->color565(255,135,42));
      drawFirLine(43,true); drawFirLine(47,false);
      drawGroundTexture(44,dsp->color565(80,110,30),dsp->color565(48,68,22),dsp->color565(160,92,28));
    }
    // Cabin + woods only for non-summer seasons. Summer is the open-lake
    // scene built entirely in its BG branch above.
    if(season != "summer"){
      drawCabinHouse(40,6);
      // Dense tree line — cabin nestled in the woods. Foreground trees at
      // y=63 (full height), mid-ground at y=50, and tucked behind cabin too.
      // Spring/Fall leave a clear lawn x=8..32 for the walking bear sprite.
      if(season=="spring" || season=="fall"){
        drawPineTree(2,63);                       drawPineTree(34,63);
        drawPineTree(96,63); drawPineTree(104,63);drawPineTree(112,63);drawPineTree(120,63);
        drawPineTree(6,52);                       drawPineTree(28,52);
        drawPineTree(102,52);drawPineTree(114,52);drawPineTree(124,52);
      } else {
        drawPineTree(2,63);  drawPineTree(10,63); drawPineTree(18,63); drawPineTree(26,63); drawPineTree(34,63);
        drawPineTree(96,63); drawPineTree(104,63);drawPineTree(112,63);drawPineTree(120,63);
        drawPineTree(6,52);  drawPineTree(16,52); drawPineTree(28,52);
        drawPineTree(102,52);drawPineTree(114,52);drawPineTree(124,52);
      }
      // Background trees peeking behind/over cabin (smaller silhouettes)
      drawPineTree(38,47); drawPineTree(94,47);
      // Tiny trees in distance behind hills
      if(season!="winter"){
        uint16_t farT = dsp->color565(38,68,42);
        for(int i=0;i<6;i++){
          int tx=46+i*7, ty=39;
          dsp->drawFastVLine(tx,ty,4,farT);
          dsp->drawPixel(tx-1,ty+1,farT); dsp->drawPixel(tx+1,ty+1,farT);
        }
      }
    }
    // Static spring bear — cute, friendly, BLACK. Sits next to cabin
    // looking at the path. Drawn once here; no animation, no flicker.
    if(false && season=="spring"){
      int bx = 22, by = 50;
      uint16_t blk    = dsp->color565(15,15,20);     // proper black bear
      uint16_t blkHi  = dsp->color565(45,45,55);     // highlight
      uint16_t snout  = dsp->color565(120,90,70);    // tan muzzle
      uint16_t pinkT  = dsp->color565(220,150,170);  // tongue / nose pink
      // Body — sitting pose, rounded
      dsp->fillCircle(bx, by, 6, blk);
      dsp->fillCircle(bx, by-1, 5, blkHi);
      // Head — slightly tilted, friendly
      dsp->fillCircle(bx, by-7, 4, blk);
      // Ears
      dsp->fillCircle(bx-3, by-10, 1, blk);
      dsp->fillCircle(bx+3, by-10, 1, blk);
      dsp->drawPixel(bx-3, by-10, blkHi);
      dsp->drawPixel(bx+3, by-10, blkHi);
      // Tan muzzle
      fillRect(bx-1, by-6, 3, 3, snout);
      // Friendly pinpoint eyes (no glowing yellow!)
      dsp->drawPixel(bx-2, by-8, C_WHITE);
      dsp->drawPixel(bx+2, by-8, C_WHITE);
      // Tiny pink nose dot
      dsp->drawPixel(bx, by-5, pinkT);
      // Smile — single curved pixel
      dsp->drawPixel(bx-1, by-4, blk);
      dsp->drawPixel(bx+1, by-4, blk);
      // Front paws peeking from sitting body
      dsp->drawPixel(bx-3, by+5, blk); dsp->drawPixel(bx+3, by+5, blk);
    }
    if(season=="winter"){
      uint16_t glow=dsp->color565(255,200,80);
      fillRect(47,25,8,7,glow); fillRect(77,25,8,7,glow);
      dsp->drawFastHLine(40,18,8,C_WHITE); dsp->drawFastHLine(44,16,8,C_WHITE);
      if(!flakesInited) initFlakes();
      // Big static campfire right of cabin on snowy ground
      drawBigFire(82, 56);
    } else if(season=="spring"){
      drawBigFire(82, 56);
    } else if(season=="summer"){
    } else {
      // Cute black bear sitting next to a leaf pile (fall theme).
      // Same friendly sprite as spring — sitting, no creepy glowing eyes.
      if(false){
      uint16_t blk    = dsp->color565(15,15,20);
      uint16_t blkHi  = dsp->color565(45,45,55);
      uint16_t snout  = dsp->color565(120,90,70);
      uint16_t pinkT  = dsp->color565(220,150,170);
      int bx = 22, by = 50;
      dsp->fillCircle(bx, by, 6, blk);
      dsp->fillCircle(bx, by-1, 5, blkHi);
      dsp->fillCircle(bx, by-7, 4, blk);
      dsp->fillCircle(bx-3, by-10, 1, blk);
      dsp->fillCircle(bx+3, by-10, 1, blk);
      fillRect(bx-1, by-6, 3, 3, snout);
      dsp->drawPixel(bx-2, by-8, C_WHITE);
      dsp->drawPixel(bx+2, by-8, C_WHITE);
      dsp->drawPixel(bx, by-5, pinkT);
      dsp->drawPixel(bx-1, by-4, blk);
      dsp->drawPixel(bx+1, by-4, blk);
      }
      // Leaf pile
      for(int i=0;i<6;i++) dsp->fillCircle(34+i*3, 56, 2, i%2?C_ORANGE:C_RED);
      // Big static campfire right of cabin
      drawBigFire(82, 56);
    }
    // Label at TOP of sky with outline for high contrast
    uint16_t titleC2;
    if(season=="winter")       titleC2=C_CYAN;
    else if(season=="spring")  titleC2=dsp->color565(255,220,255);
    else if(season=="summer")  titleC2=C_YELLOW;
    else                       titleC2=C_ORANGE;
    ctrTxtOutline("PORTLY BEAR LODGE",1,titleC2);
  }

  // ANIMATION ONLY - no cls()
  if(season=="winter"){
    // Window flicker
    static unsigned long lastGlow=0;
    if(millis()-lastGlow>350){
      uint16_t g=((millis()/350)%2)?dsp->color565(255,200,80):dsp->color565(255,170,50);
      fillRect(47,25,8,7,g); fillRect(77,25,8,7,g);
      lastGlow=millis();
    }
    // Zzz from chimney (stays below title at y>=11)
    static unsigned long lastZzz=0; static int zzY=18;
    if(millis()-lastZzz>300){
      dsp->drawPixel(85,zzY,cabinSkyPixel(season,zzY));
      dsp->drawPixel(88,zzY-2,cabinSkyPixel(season,zzY-2));
      zzY--; if(zzY<13) zzY=18;
      dsp->drawPixel(85,zzY,C_GRAY); dsp->drawPixel(88,zzY-2,dsp->color565(100,100,100));
      lastZzz=millis();
    }
    // Snowflakes (stay below title at y>=10)
    static unsigned long lastSn3=0;
    if(millis()-lastSn3>60){
      for(int i=0;i<MAX_FLAKES;i++){
        int ox=(int)flakes[i].x,oy=(int)flakes[i].y;
        if(oy>=10&&oy<44) dsp->drawPixel(ox,oy,cabinSkyPixel(season,oy));
        flakes[i].y+=flakes[i].spd; flakes[i].x+=flakes[i].drift;
        if(flakes[i].y>=44){flakes[i].y=10;flakes[i].x=random(0,128);}
        if(flakes[i].x<0)flakes[i].x=127;if(flakes[i].x>127)flakes[i].x=0;
        if((int)flakes[i].y>=10&&(int)flakes[i].y<44) dsp->drawPixel((int)flakes[i].x,(int)flakes[i].y,C_WHITE);
      }
      lastSn3=millis();
    }
  } else if(season=="spring" || season=="fall"){
    // Smooth WALKING BEAR — sprite-delta on cleared lawn (x=8..32, y=51..62).
    // Bear paces back and forth slowly. Erase with cabinBgPixel sampler so
    // pixels behind the sprite restore properly without ever doing a full
    // BG redraw — same smooth pattern as the snowflakes.
    static float bearX = 12, bearV = 0.5f;
    static unsigned long lastBear = 0;
    static int bearStep = 0;
    if(millis() - lastBear > 80){
      lastBear = millis();
      bearStep++;
      // ── Erase old position (8 wide × 11 tall sprite)
      int ox = (int)bearX;
      for(int dy=-7; dy<=3; dy++){
        for(int dx=-4; dx<=4; dx++){
          int xx = ox+dx, yy = 56+dy;
          if(xx<0 || xx>=PANEL_WIDTH || yy<44 || yy>=64) continue;
          dsp->drawPixel(xx, yy, cabinBgPixel(xx, yy, season));
        }
      }
      // ── Advance
      bearX += bearV;
      if(bearX > 30){ bearX = 30; bearV = -fabs(bearV); }
      if(bearX < 10){ bearX = 10; bearV =  fabs(bearV); }
      // ── Draw new position
      int nx = (int)bearX, ny = 56;
      uint16_t blk    = dsp->color565(15,15,20);
      uint16_t blkHi  = dsp->color565(45,45,55);
      uint16_t snout  = dsp->color565(120,90,70);
      uint16_t pinkT  = dsp->color565(220,150,170);
      // Body — small rounded
      dsp->fillCircle(nx, ny, 3, blk);
      dsp->fillCircle(nx, ny-1, 2, blkHi);
      // Head
      dsp->fillCircle(nx, ny-5, 2, blk);
      // Ears
      dsp->drawPixel(nx-2, ny-7, blk);
      dsp->drawPixel(nx+2, ny-7, blk);
      // Muzzle / nose
      dsp->drawPixel(nx,   ny-4, snout);
      dsp->drawPixel(nx-1, ny-4, snout);
      dsp->drawPixel(nx+1, ny-4, snout);
      // Eye dots (white)
      dsp->drawPixel(nx-1, ny-5, C_WHITE);
      dsp->drawPixel(nx+1, ny-5, C_WHITE);
      // Pink nose
      dsp->drawPixel(nx, ny-3, pinkT);
      // Legs alternate stride for walking effect
      bool stride = (bearStep & 1) == 0;
      if(stride){
        dsp->drawPixel(nx-2, ny+3, blk);
        dsp->drawPixel(nx+2, ny+3, blk);
      } else {
        dsp->drawPixel(nx-2, ny+2, blk);
        dsp->drawPixel(nx+2, ny+2, blk);
        dsp->drawPixel(nx-1, ny+3, blk);
        dsp->drawPixel(nx+1, ny+3, blk);
      }
    }
  } else if(season=="summer"){
    // Bear water-skiing across the lake, towed by a speedboat.
    // Smooth sprite-delta: erase the FULL footprint (bear + skis + rope +
    // boat + spray) by sampling cabinLakePixel, so wave texture is restored
    // and there is no flat-blue scar or boat trail.
    static float skiX = -8;
    static unsigned long lastSki = 0;
    static int wakeFrame = 0;
    if(millis() - lastSki > 70){
      lastSki = millis();
      int ox = (int)skiX;
      // Erase previous footprint via the lake sampler (x: ox-12..ox+34)
      for(int y=CABIN_LAKE_TOP; y<45; y++){
        for(int dx=-12; dx<=34; dx++){
          int xx = ox + dx;
          if(xx < 0 || xx >= PANEL_WIDTH) continue;
          dsp->drawPixel(xx, y, cabinLakePixel(xx, y));
        }
      }
      // Advance and wrap off the right edge
      skiX += 0.7f; if(skiX > PANEL_WIDTH + 14) skiX = -12;
      wakeFrame = (wakeFrame + 1) & 3;
      int bx = (int)skiX;

      uint16_t furD = dsp->color565(28, 22, 18);   // dark brown-black bear
      uint16_t furM = dsp->color565(62, 48, 36);   // mid fur tone
      uint16_t furH = dsp->color565(95, 75, 55);   // highlight fur
      uint16_t snout= dsp->color565(165, 125, 85); // tan muzzle — the bear "tell"
      uint16_t skiC = dsp->color565(225, 60, 40);  // red water-skis
      uint16_t ropeC= dsp->color565(235, 235, 215);
      uint16_t spray= C_WHITE;
      int waterY = 40;                              // skis meet the water here

      // ── Speedboat ahead (to the right), planing ──
      int boatX = bx + 22;
      dsp->fillTriangle(boatX-6, 39, boatX+10, 39, boatX+10, 35, dsp->color565(228,228,238));
      fillRect(boatX-6, 36, 16, 4, dsp->color565(208,208,222));
      dsp->drawFastHLine(boatX-6, 39, 17, dsp->color565(150,150,165));  // waterline shadow
      fillRect(boatX-1, 33, 4, 3, dsp->color565(120,200,235));          // windshield
      dsp->drawPixel(boatX+1, 32, dsp->color565(60,40,30));             // driver head
      dsp->drawPixel(boatX-7, 40, spray);                              // prop wash
      dsp->drawPixel(boatX-8, 41, dsp->color565(200,225,235));

      // ── Tow rope (to the bear's outstretched paws) ──
      dsp->drawLine(boatX-6, 36, bx+5, 33, ropeC);

      // ── BEAR — bigger sprite so the species reads at a glance.
      //    Key bear cues: two round ears on top, tan muzzle, chunky body.
      //    All pixels kept at y>=27 so the full sprite sits inside the
      //    erase band (y starts at CABIN_LAKE_TOP=26).
      // Head: circle at (bx+1, 31)
      dsp->fillCircle(bx+1, 31, 3, furD);
      dsp->drawPixel(bx,   30, furM);                          // forehead light
      // EARS — 2px round nubs clearly above the head outline (rows 27..29)
      dsp->fillCircle(bx-2, 28, 1, furD);
      dsp->fillCircle(bx+4, 28, 1, furD);
      dsp->drawPixel(bx-2, 28, furM);                          // inner-ear hint
      dsp->drawPixel(bx+4, 28, furM);
      // MUZZLE — tan block protruding right (direction of travel)
      fillRect(bx+3, 31, 3, 2, snout);
      dsp->drawPixel(bx+5, 31, dsp->color565(25,15,10));       // black nose tip
      // Eye
      dsp->drawPixel(bx+1, 30, C_WHITE);
      // TORSO — chunky, leaning back against rope pull (rows 34..38)
      fillRect(bx-3, 34, 7, 5, furD);
      fillRect(bx-3, 34, 2, 5, furM);                          // back highlight
      dsp->drawPixel(bx-3, 33, furD);                          // shoulder hump
      // ARMS — both reaching forward to the rope handle
      dsp->drawLine(bx+3, 34, bx+5, 34, furD);
      dsp->drawPixel(bx+5, 33, furD);                          // paw on handle
      // Belly highlight
      fillRect(bx-1, 36, 3, 2, furM);

      // ── Skis + legs ──
      dsp->drawFastHLine(bx-5, waterY,   10, skiC);            // wider ski
      dsp->drawFastHLine(bx-5, waterY+1, 10, dsp->color565(150,40,28));
      dsp->drawPixel(bx+5, waterY, skiC);                      // ski tip up
      dsp->drawPixel(bx+5, waterY-1, skiC);
      // Bent legs (short — torso ends at 38, ski at 40)
      dsp->drawPixel(bx-2, 39, furD);
      dsp->drawPixel(bx+2, 39, furD);

      // ── Rooster-tail spray behind the skis ──
      for(int i=0;i<3;i++){
        int sxp = bx - 5 - i*2;
        int syp = waterY - (i & 1);
        if(sxp >= 0) dsp->drawPixel(sxp, syp, (i&1)?spray:dsp->color565(205,230,240));
      }
      dsp->drawPixel(bx-4, waterY-1, spray);
      dsp->drawPixel(bx+4, waterY-1, dsp->color565(205,230,240));
    }
  }
  // ── Animated flickering campfire flames (smooth — tiny redraw zone) ──
  // The fire base (stones + logs) was drawn once in the BG block.
  // Here we only repaint the 15×10 flame zone above it every ~150 ms,
  // cycling through 4 flame silhouette frames for a live flicker feel.
  if(season != "summer"){   // no campfire in summer (lake scene)
    static unsigned long lastFlame = 0;
    static int flameFrame = 0;
    if(millis() - lastFlame > 140){
      lastFlame = millis();
      flameFrame = (flameFrame + 1) & 3;       // 0..3 cycle
      // Sample bg under flame zone — uniform per row, since fire is small
      uint16_t bgC = cabinBgPixel(82, 50, season);
      drawFireFlames(82, 56, bgC, flameFrame);
    }
  }
}

// ----------------------------------------------------------
void renderCabinWeather(){
  static int cwxPage=0;
  static unsigned long cwxPageStart=0;
  bool newSlide=(lastStaticDraw==0);
  if(newSlide){cwxPage=0;cwxPageStart=millis();}
  if(millis()-cwxPageStart>4500){cwxPage=(cwxPage+1)%2;cwxPageStart=millis();lastStaticDraw=0;}
  if(millis()-lastStaticDraw<STATIC_REDRAW_MS) return;
  lastStaticDraw=millis();
  cls();
  // v10 unified chrome
  drawCardHeader("weather", "THE LAKE PA", C_GOLD, 2, cwxPage);

  if(!cabinWx.valid){ctrTxt1("Loading weather...",32,C_GRAY);return;}

  if(cwxPage==0){
    // PAGE 1: current + forecast
    fillRect(0,14,54,50,dsp->color565(5,4,1));
    dsp->drawFastVLine(53,14,50,C_DARKGRAY);
    txt1(trimTo(cabinWx.condition,9).c_str(),1,15,C_WHITE);
    drawWxIcon(44,15,4,cabinWx.icon);
    char tmp2[6]; snprintf(tmp2,6,"%.0fF",cabinWx.tempF);
    txt2(tmp2,1,24,tempColor(cabinWx.tempF));
    dsp->drawFastHLine(0,42,53,C_DARKGRAY);
    char fls2[12]; snprintf(fls2,12,"FL:%.0fF",cabinWx.feelsF);
    txt1(fls2,1,44,C_GRAY);
    char wnd2[10]; snprintf(wnd2,10,"%.0fmph",cabinWx.windMph);
    txt1(wnd2,1,54,C_CYAN);

    int days2=min(cabinWx.forecastCount,5);
    fillRect(55,14,73,50,C_BLACK);
    for(int i=0;i<days2;i++){
      int x=55+i*14; if(x+12>PANEL_WIDTH) break;
      txt1(cabinWx.forecast[i].label.substring(0,2).c_str(),x+1,16,C_GOLD);
      drawWxIcon(x+6,28,3,cabinWx.forecast[i].icon);
      char hi2[5]; snprintf(hi2,5,"%.0f",cabinWx.forecast[i].hiF);
      txt1(hi2,x+1,36,tempColor(cabinWx.forecast[i].hiF));
      char lo2[5]; snprintf(lo2,5,"%.0f",cabinWx.forecast[i].loF);
      txt1(lo2,x+1,44,tempColor(cabinWx.forecast[i].loF));
      int bH=constrain((int)((cabinWx.forecast[i].hiF-cabinWx.forecast[i].loF)/4),1,5);
      fillRect(x+2,56-bH,9,bH,tempColor(cabinWx.forecast[i].hiF));
      fillRect(x+2,56,9,2,tempColor(cabinWx.forecast[i].loF));
      if(i<days2-1) dsp->drawFastVLine(x+13,14,50,dsp->color565(20,20,30));
    }
  } else {
    // PAGE 2: big temp + sun times
    char tmp2[6]; snprintf(tmp2,6,"%.0fF",cabinWx.tempF);
    dsp->setTextSize(3); dsp->setTextColor(tempColor(cabinWx.tempF));
    int w=strlen(tmp2)*18; dsp->setCursor((PANEL_WIDTH-w)/2,17);
    dsp->print(tmp2);
    dsp->setTextSize(1);
    ctrTxt1(cabinWx.condition.c_str(),44,C_WHITE);
    dsp->drawFastHLine(0,52,PANEL_WIDTH,C_DARKGRAY);
    if(cabinWx.sunrise>0){
      time_t sr=(time_t)cabinWx.sunrise,ss=(time_t)cabinWx.sunset;
      struct tm srt, sst;
      localtime_r(&sr,&srt);
      localtime_r(&ss,&sst);
      char srB[8],ssB[8];
      strftime(srB,8,"%I:%M%p",&srt); if(srB[0]=='0')memmove(srB,srB+1,sizeof(srB)-1);
      strftime(ssB,8,"%I:%M%p",&sst); if(ssB[0]=='0')memmove(ssB,ssB+1,sizeof(ssB)-1);
      for(char*p=srB;*p;p++) if(*p=='A'||*p=='P'||*p=='M') *p+=32;
      for(char*p=ssB;*p;p++) if(*p=='A'||*p=='P'||*p=='M') *p+=32;
      txt1("UP",2,55,C_GOLD);
      txt1(srB,18,55,C_WHITE);
      txt1("DN",68,55,C_ORANGE);
      txt1(ssB,84,55,C_WHITE);
    }
  }
}

// ----------------------------------------------------------
void renderCabinTraffic(){
  if(millis()-lastStaticDraw<STATIC_REDRAW_MS) return;
  lastStaticDraw=millis();
  cls();
  // v10 unified chrome
  drawCardHeader("cabin", "CABIN TRIP", C_GOLD);
  // "LAKE WALLENPAUPACK" is too long for 128px — use shorter form
  txt1("TO: LAKE W'PAUPACK",4,16,C_GRAY);
  txt1("1133 INDIAN DR, PA",4,24,C_GRAY);
  dsp->drawFastHLine(0,32,PANEL_WIDTH,C_DARKGRAY);
  if(!trafficCabin.valid){
    ctrTxt1("Loading...",44,C_GRAY);
  } else {
    // Two-column: TRAFFIC (left, red) vs NORMAL (right, green)
    txt1("TRAFFIC",10,34,C_GRAY);
    txt1("NORMAL",82,34,C_GRAY);
    dsp->drawFastVLine(64,34,28,dsp->color565(40,40,40));
    String dur  = shortDur(trafficCabin.durationTraffic);
    String norm = shortDur(trafficCabin.durationNormal);
    // Times ALWAYS size-1 — user reported overlap twice with adaptive
    // sizing, so columns are now permanently overlap-proof: size-1 caps at
    // 10 chars per 60px column, and we hard-trim to 9 chars regardless.
    if(dur.length()  > 9) dur  = dur.substring(0, 9);
    if(norm.length() > 9) norm = norm.substring(0, 9);
    int dw = (int)dur.length()  * 6;
    int nw = (int)norm.length() * 6;
    dsp->setTextSize(1);
    dsp->setTextColor(C_RED);
    dsp->setCursor(max(0,(64-dw)/2), 46);
    dsp->print(dur.c_str());
    dsp->setTextColor(C_LIME);
    dsp->setCursor(64+max(2,(64-nw)/2), 46);
    dsp->print(norm.c_str());
  }
}

// ----------------------------------------------------------
void renderCabinLake(){
  static int lkPage=0;
  static unsigned long lkPageStart=0;
  bool newSlide=(lastStaticDraw==0);
  if(newSlide){lkPage=0;lkPageStart=millis();}
  if(millis()-lkPageStart>4500){lkPage=(lkPage+1)%2;lkPageStart=millis();lastStaticDraw=0;}
  if(millis()-lastStaticDraw<STATIC_REDRAW_MS) return;
  lastStaticDraw=millis();
  cls();
  // v10 unified chrome — title switches per page
  drawCardHeader("lake", lkPage==0 ? "WATER TEMP" : "TIDES", C_TEAL, 2, lkPage);

  if(lkPage==0){
    // PAGE 1: WATER TEMP + LEVEL (USGS)
    // Big temp centered y=18-34 (size-3, 24px)
    if(lake.valid && lake.waterTempF>0){
      char wt[8]; snprintf(wt,8,"%.1fF",lake.waterTempF);
      int w=strlen(wt)*18;
      dsp->setTextSize(3); dsp->setTextColor(tempColor(lake.waterTempF));
      dsp->setCursor((PANEL_WIDTH-w)/2,18); dsp->print(wt);
      dsp->setTextSize(1);
    } else {
      ctrTxt2("--",22,C_GRAY);
    }
    // Divider y=44
    dsp->drawFastHLine(8,44,PANEL_WIDTH-16,C_DARKGRAY);
    // Source label y=50
    ctrTxt1("Lake Wallenpaupack",50,C_DARKGRAY);
    ctrTxt1("USGS 01427510",58,C_DARKGRAY);

  } else {
    // PAGE 2: TIDES + BUOY (NOAA Stamford) — title lives in the v10 header
    if(marine.tideValid){
      bool rH=(marine.nextTide.type=="H");
      // Big NEXT label
      txt1(rH?"NEXT HIGH":"NEXT LOW",2,18,rH?C_LIME:C_RED);
      // Time and height
      char ht[10]; snprintf(ht,10,"%.1fft",marine.nextTide.height);
      txt2(ht,2,28,C_WHITE);
      // Time
      txt1(trimTo(marine.nextTide.timeStr,8).c_str(),74,30,C_GOLD);
      // Following tide
      if(marine.followTide.timeStr.length()>0){
        bool r2=(marine.followTide.type=="H");
        char ft2[20]; snprintf(ft2,20,"Then %s %.1fft",r2?"HI":"LO",marine.followTide.height);
        txt1(ft2,2,46,C_DARKGRAY);
      }
    } else {
      ctrTxt1("Loading tides...",26,C_DARKGRAY);
    }
    // Buoy waves bottom (y=56 max)
    if(marine.buoyValid){
      char bw[16]; snprintf(bw,16,"Waves %.1fft",marine.waveHt);
      ctrTxt1(bw,56,dsp->color565(80,160,220));
    } else {
      ctrTxt1("Stamford CT",56,C_DARKGRAY);
    }
  }
}

// ----------------------------------------------------------
// Tiki scene — REVAMP to match reference image: tropical teal seascape,
// arcing palm trees in the foreground left, distant island in the middle
// distance, BIG erupting volcano on the right with lava streaming, full
// moon in the sky, two lit tiki torches in the front-right corner.
void renderTikiScene(){
  bool drawBG = (lastStaticDraw == 0);
  if(drawBG){
    lastStaticDraw = millis();
    cls();

    // ── TEAL TROPICAL SKY — gradient deep teal at top to lighter at horizon
    for(int y=0; y<28; y++){
      uint8_t r = map(y, 0, 27, 8,  18);
      uint8_t g = map(y, 0, 27, 95, 130);
      uint8_t b = map(y, 0, 27, 110, 155);
      dsp->drawFastHLine(0, y, PANEL_WIDTH, dsp->color565(r, g, b));
    }
    // Horizon line — ocean meets sky
    dsp->drawFastHLine(0, 28, PANEL_WIDTH, dsp->color565(80, 200, 200));

    // ── FULL MOON — high in the sky, slightly left of center
    dsp->fillCircle(75, 8, 5, dsp->color565(220, 230, 220));
    dsp->fillCircle(74, 7, 4, dsp->color565(255, 250, 230));
    // Moon glow halo
    dsp->drawCircle(75, 8, 6, dsp->color565(120, 180, 180));

    // ── DISTANT ISLAND — middle distance, left-center, dark teal silhouette
    uint16_t farIsland = dsp->color565(8, 60, 75);
    fillRect(28, 23, 22, 6, farIsland);
    // Island peaked top
    for(int i=0; i<8; i++) dsp->drawFastHLine(36+i, 21+i, 8-i, farIsland);
    dsp->drawFastHLine(28, 28, 22, dsp->color565(60, 130, 130));   // shore highlight

    // ── VOLCANO — RIGHT side of panel, big blue cone with lava streaming
    uint16_t volC  = dsp->color565(15, 80, 100);    // teal volcano body
    uint16_t volSh = dsp->color565(8,  55, 75);     // shadow side
    uint16_t volHi = dsp->color565(40, 130, 150);   // highlight
    // Cone shape from peak (95, 12) down to base (78..118, 44)
    for(int y=12; y<45; y++){
      int hw = map(y, 12, 44, 2, 22);   // half-width grows from 2 → 22
      int lx = 95 - hw;
      int rx = 95 + hw;
      // Body
      dsp->drawFastHLine(lx, y, rx-lx+1, volC);
      // Shadow on right face (right 1/3)
      dsp->drawFastHLine(95, y, rx-95+1, volSh);
      // Highlight stripe on left face (couple px)
      if(y > 14) dsp->drawPixel(lx, y, volHi);
    }
    // Crater mouth — small cup at top
    fillRect(92, 12, 7, 3, dsp->color565(60, 30, 10));

    // Lava in crater + flowing down right side (vertical stream)
    fillRect(94, 11, 4, 2, dsp->color565(255, 200, 30));      // hot crater
    fillRect(94, 13, 4, 1, dsp->color565(255, 100, 0));       // mouth lip
    // Lava stream down the right face
    for(int y=15; y<44; y++){
      int hw = map(y, 12, 44, 2, 22);
      int rx = 95 + hw - 2;
      dsp->drawPixel(rx,   y, dsp->color565(255, 130, 0));
      dsp->drawPixel(rx+1, y, dsp->color565(255, 60, 0));
    }
    // Eruption smoke puff above crater
    dsp->fillCircle(95, 7, 3, dsp->color565(140, 140, 130));
    dsp->fillCircle(98, 5, 2, dsp->color565(110, 110, 105));
    dsp->fillCircle(92, 5, 2, dsp->color565(110, 110, 105));

    // ── OCEAN BAND — y=29..52, bright tropical teal with wave highlights
    for(int y=29; y<52; y++){
      uint8_t r = map(y, 29, 51, 12, 30);
      uint8_t g = map(y, 29, 51, 165, 140);
      uint8_t b = map(y, 29, 51, 175, 150);
      dsp->drawFastHLine(0, y, PANEL_WIDTH, dsp->color565(r, g, b));
    }
    // Wave highlights — horizontal cyan dashes
    for(int y=32; y<52; y+=4){
      for(int x=2; x<PANEL_WIDTH; x += 14){
        int wx = x + ((y * 7) % 5);
        if(wx + 4 < PANEL_WIDTH){
          dsp->drawFastHLine(wx, y, 4, dsp->color565(160, 230, 220));
        }
      }
    }
    // Lava reflection on water near volcano base
    for(int y=44; y<50; y++){
      for(int x=84; x<108; x++){
        if((x*y) & 1) dsp->drawPixel(x, y, dsp->color565(120, 90, 30));
      }
    }
    // Foam line where ocean meets foreground beach
    dsp->drawFastHLine(0, 51, PANEL_WIDTH, dsp->color565(220, 240, 230));

    // ── BEACH / FOREGROUND — y=52..63
    fillRect(0, 52, PANEL_WIDTH, 12, dsp->color565(20, 105, 110));   // dark teal foreground
    // Foreground rocks/coral on the right (under volcano shadow)
    dsp->fillCircle(90, 56, 3, dsp->color565(8, 50, 65));
    dsp->fillCircle(95, 58, 2, dsp->color565(8, 50, 65));
    dsp->fillCircle(108, 56, 4, dsp->color565(8, 50, 65));

    // ── BIG ARCING PALM TREES — left side foreground (matches reference)
    // Palm 1: arcs up-left from base (8, 63) to fronds at (4, 14)
    {
      uint16_t trunk = dsp->color565(15, 50, 50);
      // Curved trunk — pixel by pixel, arcing leftward as it goes up
      for(int y=14; y<63; y++){
        int x = 8 + (int)(sinf((y-14)*0.05f) * -3);
        dsp->drawPixel(x,   y, trunk);
        dsp->drawPixel(x-1, y, dsp->color565(8, 40, 40));
      }
      // Fronds — 5 leaves radiating from top of trunk
      uint16_t leaf  = dsp->color565(15, 80, 75);
      uint16_t leafD = dsp->color565(8, 55, 50);
      // Top fronds — angled outward
      dsp->drawLine(4, 14, -2, 4, leaf);
      dsp->drawLine(4, 14, 14, 4, leaf);
      dsp->drawLine(4, 14, -4, 16, leaf);
      dsp->drawLine(4, 14, 18, 14, leaf);
      dsp->drawLine(4, 14, 8, 22, leafD);
      // Frond detail — small dots along each branch
      for(int i=0; i<8; i++){
        dsp->drawPixel(4+i*1.7f,  14-i*1.2f, leaf);
        dsp->drawPixel(4-i*0.7f,  14-i*1.2f, leaf);
        dsp->drawPixel(4-i*1.0f,  14+i*0.3f, leaf);
      }
    }
    // Palm 2: smaller, mid-foreground at (24, 63)
    {
      uint16_t trunk = dsp->color565(20, 60, 60);
      for(int y=24; y<63; y++){
        int x = 24 + (int)(sinf((y-24)*0.04f) * -2);
        dsp->drawPixel(x, y, trunk);
      }
      uint16_t leaf = dsp->color565(20, 95, 85);
      dsp->drawLine(22, 24, 16, 18, leaf);
      dsp->drawLine(22, 24, 30, 18, leaf);
      dsp->drawLine(22, 24, 14, 24, leaf);
      dsp->drawLine(22, 24, 32, 24, leaf);
      dsp->drawLine(22, 24, 22, 30, leaf);
    }

    // ── EXOTICA LAYER — tiki masks, thatched hut, extra torch ──
    // Thatched beach hut, left foreground (x=12..32)
    {
      uint16_t thatch  = dsp->color565(196,150,60);
      uint16_t thatchD = dsp->color565(140,100,35);
      uint16_t wallC   = dsp->color565(110,70,30);
      uint16_t doorC   = dsp->color565(40,22,8);
      // Roof — wide shallow triangle y=47..54
      for(int i=0;i<8;i++){
        dsp->drawFastHLine(14+i, 47+i, 18-i*1, (i&1)?thatch:thatchD);
        dsp->drawFastHLine(14+i, 47+i, 1, thatchD);   // left edge shade
      }
      dsp->fillTriangle(12,54, 22,46, 33,54, thatchD);
      dsp->fillTriangle(14,54, 22,48, 31,54, thatch);
      // Walls y=55..63
      fillRect(15, 55, 15, 9, wallC);
      dsp->drawFastVLine(15, 55, 9, dsp->color565(70,42,16));
      // Door
      fillRect(20, 57, 5, 7, doorC);
      // Bamboo trim posts
      drawBamboo(15, 54, 10);
      drawBamboo(28, 54, 10);
    }
    // Tiki mask #1 — planted at mid-beach on a pole (foreground, in front
    // of the ocean band like a carved sentinel)
    drawTikiMask(38, 42);
    fillRect(44, 61, 2, 3, dsp->color565(60,30,8));   // pole stub to sand
    // Tiki mask #2 — second sentinel, slightly lower/right
    drawTikiMask(58, 45);
    fillRect(64, 63, 2, 1, dsp->color565(60,30,8));
    // Third torch between the masks and the rocks (static flame — the two
    // animated torches are at x=102/116; this one just glows)
    {
      int t3x = 80, t3y = 62;
      fillRect(t3x, t3y-4, 2, 4, dsp->color565(40, 70, 30));
      dsp->drawPixel(t3x,   t3y-5, dsp->color565(160, 70, 0));
      dsp->drawPixel(t3x+1, t3y-5, dsp->color565(160, 70, 0));
      dsp->drawPixel(t3x,   t3y-6, dsp->color565(255, 200, 50));
      dsp->drawPixel(t3x+1, t3y-7, dsp->color565(255, 130, 0));
    }
    // Lei-flower dots scattered on the beach sand
    dsp->drawPixel(50, 60, dsp->color565(255,80,140));
    dsp->drawPixel(72, 58, dsp->color565(255,160,40));
    dsp->drawPixel(91, 61, dsp->color565(255,80,140));

    // ── TIKI TORCHES — bottom-right corner, 2 torches with flames
    // Torch 1
    int t1x = 102, t1y = 60;
    fillRect(t1x, t1y-4, 2, 4, dsp->color565(40, 70, 30));    // bamboo shaft
    dsp->drawPixel(t1x, t1y-5, dsp->color565(160, 70, 0));    // wrap
    dsp->drawPixel(t1x+1, t1y-5, dsp->color565(160, 70, 0));
    // Flame
    dsp->drawPixel(t1x,   t1y-7, dsp->color565(255, 220, 60));
    dsp->drawPixel(t1x+1, t1y-7, dsp->color565(255, 220, 60));
    dsp->drawPixel(t1x,   t1y-8, dsp->color565(255, 140, 0));
    dsp->drawPixel(t1x+1, t1y-8, dsp->color565(255, 140, 0));
    dsp->drawPixel(t1x,   t1y-9, dsp->color565(255, 80, 0));
    // Torch 2
    int t2x = 116, t2y = 62;
    fillRect(t2x, t2y-4, 2, 4, dsp->color565(40, 70, 30));
    dsp->drawPixel(t2x,   t2y-5, dsp->color565(160, 70, 0));
    dsp->drawPixel(t2x+1, t2y-5, dsp->color565(160, 70, 0));
    dsp->drawPixel(t2x,   t2y-7, dsp->color565(255, 220, 60));
    dsp->drawPixel(t2x+1, t2y-7, dsp->color565(255, 220, 60));
    dsp->drawPixel(t2x,   t2y-8, dsp->color565(255, 140, 0));
    dsp->drawPixel(t2x+1, t2y-8, dsp->color565(255, 140, 0));
    dsp->drawPixel(t2x,   t2y-9, dsp->color565(255, 80, 0));

  }   // ← end of drawBG-only block

  // ── SUBTLE PULSING LAVA + TORCHES (no eruption particles, no trails) ──
  // Three independent pulsers, each updating only the small footprint they own:
  //   1) Crater glow — 4×3 patch at (94,11), pulses yellow → orange → red
  //   2) Lava stream — pixels along the right face of the volcano shimmer
  //   3) Torch flames — top pixel of each torch alternates intensity
  static unsigned long lastPulse = 0;
  static uint8_t       pulsePhase = 0;
  if(millis() - lastPulse > 140){
    lastPulse = millis();
    pulsePhase = (pulsePhase + 1) & 7;     // 0..7 sine-ish

    // (1) crater — sin-style brightness sweep
    // Brightness curve: 0,1,2,3,4,3,2,1
    uint8_t bri = (pulsePhase < 4) ? pulsePhase : (8 - pulsePhase);
    uint16_t hot;
    switch(bri){
      case 0: hot = dsp->color565(180, 60,  0);   break;
      case 1: hot = dsp->color565(220, 90,  0);   break;
      case 2: hot = dsp->color565(245,140,  0);   break;
      case 3: hot = dsp->color565(255,180, 20);   break;
      default:hot = dsp->color565(255,225, 60);   break;
    }
    fillRect(94, 11, 4, 2, hot);
    // Mouth lip — slightly cooler than crater
    uint16_t lip = (bri >= 3) ? dsp->color565(255,140, 0) : dsp->color565(220, 90, 0);
    fillRect(94, 13, 4, 1, lip);
    // Smoke puff above crater also gets a faint pulse
    uint8_t sg = 130 + bri*4;
    dsp->fillCircle(95, 7, 3, dsp->color565(sg, sg, sg-15));

    // (2) lava stream shimmer — recolor a few pixels along the stream
    //     stream lives between y=15..43 along the right face. We pick 3 random
    //     y positions each tick and re-stamp them with one of three lava shades,
    //     leaving the rest of the stream stable so there are no trails.
    const uint16_t LAVA_A = dsp->color565(255, 60,  0);
    const uint16_t LAVA_B = dsp->color565(255,110,  0);
    const uint16_t LAVA_C = dsp->color565(255,170, 30);
    for(int k = 0; k < 3; k++){
      int y = 15 + (int)random(0, 29);   // 15..43
      int hw = map(y, 12, 44, 2, 22);
      int rx = 95 + hw - 2;
      uint16_t pickInner = (random(0,2)) ? LAVA_B : LAVA_C;
      uint16_t pickEdge  = (random(0,2)) ? LAVA_A : LAVA_B;
      dsp->drawPixel(rx,   y, pickInner);
      dsp->drawPixel(rx+1, y, pickEdge);
    }

    // (3) torch flames — flicker top flame pixel between two warm shades
    uint16_t flameTop = (pulsePhase & 1) ? dsp->color565(255,100, 0)
                                         : dsp->color565(255,180,30);
    uint16_t flameMid = (pulsePhase & 1) ? dsp->color565(255,160, 0)
                                         : dsp->color565(255,210,60);
    // Torch 1 base x=102, top at y=51..53 area
    dsp->drawPixel(102, 51, flameTop);
    dsp->drawPixel(102, 52, flameMid);
    dsp->drawPixel(103, 52, flameMid);
    // Torch 2 base x=116, top y=53..55
    dsp->drawPixel(116, 53, flameTop);
    dsp->drawPixel(116, 54, flameMid);
    dsp->drawPixel(117, 54, flameMid);
  }
}

// ----------------------------------------------------------
// Lookup airline name from ICAO callsign prefix (first 3 letters)
const char* airlineFromCallsign(const String &cs){
  if(cs.length()<3) return "";
  String p=cs.substring(0,3); p.toUpperCase();
  // Most common US/international carriers seen over Stamford CT
  if(p=="AAL") return "American";
  if(p=="UAL") return "United";
  if(p=="DAL") return "Delta";
  if(p=="JBU") return "JetBlue";
  if(p=="SWA") return "Southwest";
  if(p=="ASA") return "Alaska";
  if(p=="FFT") return "Frontier";
  if(p=="NKS") return "Spirit";
  if(p=="RPA") return "Republic";
  if(p=="SKW") return "SkyWest";
  if(p=="EJA") return "NetJets";
  if(p=="GTI") return "Atlas Air";
  if(p=="FDX") return "FedEx";
  if(p=="UPS") return "UPS";
  if(p=="ACA") return "Air Canada";
  if(p=="JZA") return "Jazz";
  if(p=="BAW") return "Brit Air";
  if(p=="AFR") return "Air France";
  if(p=="DLH") return "Lufthansa";
  if(p=="KLM") return "KLM";
  if(p=="VIR") return "Virgin Atl";
  if(p=="UAE") return "Emirates";
  if(p=="ELY") return "El Al";
  if(p=="EIN") return "Aer Lingus";
  if(p=="QXE") return "Horizon";
  if(p=="ENY") return "Envoy";
  if(p=="PDT") return "Piedmont";
  return "Private";
}

// ---------------------------------------------------------------
// MTA METRO-NORTH DEPARTURES
// ---------------------------------------------------------------

// Draw the MTA circle logo: blue disc, white ring, bold "MTA" text.
// cx,cy = centre, r = radius (use r=13 for a 27px badge).
void drawMTALogo(int cx, int cy, int r){
  uint16_t mBlue  = dsp->color565(0,99,176);    // MTA corporate blue #0063B0
  uint16_t mWhite = C_WHITE;
  uint16_t mRing  = dsp->color565(200,220,245); // soft inner ring

  // Filled disc
  dsp->fillCircle(cx,cy,r,mBlue);
  // White outer ring
  dsp->drawCircle(cx,cy,r,mWhite);
  dsp->drawCircle(cx,cy,r-1,mRing);

  // "MTA" in white, size-1 (18×8 px), centred in the disc
  // Size-1: each char 6px wide, 8px tall → 3 chars = 18px wide
  dsp->setTextColor(mWhite); dsp->setTextSize(1);
  dsp->setCursor(cx-9, cy-4);
  dsp->print("MTA");
  dsp->setTextSize(1); // restore
}

void renderTrains(){
  // ── Mini commuter train gliding through the header's free zone (x=84..126)
  // Runs between the 2s full redraws via sprite-delta erase to the band blue.
  // The full redraw below repaints the header which momentarily clears the
  // train; it re-enters on the next animation tick — no trail, no strobe.
  if(millis()-lastStaticDraw < STATIC_REDRAW_MS){
    static float mtX = 126;
    static unsigned long lastMT = 0;
    if(lastStaticDraw != 0 && millis()-lastMT > 80){
      lastMT = millis();
      uint16_t bandBlue = dsp->color565(0,99,176);
      int ox = (int)mtX;
      // Erase previous sprite footprint (14×6), clamped to the free zone
      for(int y=2; y<8; y++)
        for(int x=ox; x<ox+14 && x<127; x++)
          if(x >= 84) dsp->drawPixel(x, y, bandBlue);
      // Advance leftward, wrap
      mtX -= 1.2f;
      if(mtX < 84) mtX = 113;
      int nx = (int)mtX;
      // Draw train: silver body + nose + red stripe + windows
      if(nx >= 84 && nx <= 113){
        uint16_t silver = dsp->color565(205,210,220);
        uint16_t nose   = dsp->color565(160,168,182);
        uint16_t stripe = dsp->color565(200,30,30);
        fillRect(nx+2, 3, 10, 4, silver);
        dsp->drawPixel(nx+1, 4, nose); dsp->drawPixel(nx+1, 5, nose);
        dsp->drawPixel(nx,   5, nose);                    // sloped nose (leftward)
        dsp->drawFastHLine(nx+2, 6, 10, stripe);          // MNR red stripe
        dsp->drawPixel(nx+4, 4, dsp->color565(30,50,90)); // windows
        dsp->drawPixel(nx+7, 4, dsp->color565(30,50,90));
        dsp->drawPixel(nx+10,4, dsp->color565(30,50,90));
      }
    }
    return;
  }
  lastStaticDraw=millis();
  cls();

  uint16_t mtaBlue = dsp->color565(0,99,176);

  // ── Header band y=0..9 (compact)
  fillRect(0, 0, PANEL_WIDTH, 10, mtaBlue);
  dsp->drawFastHLine(0, 10, PANEL_WIDTH, dsp->color565(80,140,220));
  // Small MTA badge top-left
  dsp->fillCircle(5, 5, 4, dsp->color565(0, 75, 140));
  dsp->drawCircle(5, 5, 4, C_WHITE);
  txt1("M", 3, 2, C_WHITE);
  txt1("METRO-NORTH", 14, 2, C_WHITE);
  // "SCHEDULE" badge top-right — full word fits at size-1 if positioned carefully
  // 8 chars * 6 = 48 px. Anchor at x=PANEL_WIDTH-50 = 78 (won't overlap METRO-NORTH which ends ~80)
  // Better: just show small dot icon with no text to save space, OR drop badge entirely.
  // User reported text cutoff — drop the badge since "STAMFORD STATION" subhead implies schedule context.

  // ── Route subheader: 2 lines so "GRAND CENTRAL" fits fully (size-1 only allows 21 chars/row)
  fillRect(0, 11, PANEL_WIDTH, 17, dsp->color565(8, 25, 60));
  ctrTxt1("STAMFORD STATION",   13, C_GOLD);
  ctrTxt1("> GRAND CENTRAL",    21, dsp->color565(220, 200, 100));
  dsp->drawFastHLine(0, 28, PANEL_WIDTH, dsp->color565(50, 80, 160));

  // No-departures empty state
  if(trainCount == 0){
    drawMTALogo(64, 42, 10);
    ctrTxt1("No service",  54, C_GRAY);
    ctrTxt1("next 4 hours", 60, dsp->color565(80, 90, 120));
    return;
  }

  // ── Column headers y=30..36
  txt1("TIME",  4, 31, dsp->color565(120, 140, 180));
  txt1("MIN",  64, 31, dsp->color565(120, 140, 180));
  txt1("LINE", 96, 31, dsp->color565(120, 140, 180));
  dsp->drawFastHLine(0, 38, PANEL_WIDTH, dsp->color565(40, 55, 100));

  // ── Departure rows y=40..58 (3 rows × 7 px each — was 4, but trimmed to fit
  // the 2-line subheader; user only glances at next-up departures anyway)
  int maxRows = min(trainCount, 3);
  for(int i = 0; i < maxRows; i++){
    TrainDep &t = trains[i];
    int ry = 40 + i*7;
    bool soon = (t.minsAway <= 5);
    bool imminent = (t.minsAway <= 2);

    // Highlight bar for imminent train (entire row width)
    if(imminent){
      bool blink = ((millis()/400) % 2) == 0;
      if(blink) fillRect(0, ry-1, PANEL_WIDTH, 8, dsp->color565(60, 20, 0));
    } else if(soon){
      fillRect(0, ry-1, PANEL_WIDTH, 8, dsp->color565(0, 30, 60));
    }

    // TIME col x=4..56
    uint16_t timeCol = t.delayed ? C_RED : (soon ? C_YELLOW : C_WHITE);
    txt1(t.time, 4, ry, timeCol);

    // MIN col x=64..92
    if(t.minsAway <= 1){
      bool b2 = ((millis()/300) % 2) == 0;
      txt1(b2 ? "NOW" : "   ", 64, ry, C_LIME);
    } else {
      char ms[5]; snprintf(ms, 5, "%dm", t.minsAway);
      txt1(ms, 64, ry, soon ? C_YELLOW : dsp->color565(140, 150, 180));
    }

    // LINE col x=96..124
    if(t.delayed){
      txt1("LATE", 96, ry, C_RED);
    } else {
      const char* lshort = "GCT";
      String ln = String(t.line); ln.toUpperCase();
      if(ln.indexOf("NEW HAVEN") >= 0) lshort = "NH";
      else if(ln.indexOf("HARLEM") >= 0) lshort = "HRL";
      else if(ln.indexOf("HUDSON") >= 0) lshort = "HUD";
      txt1(lshort, 96, ry, dsp->color565(120, 150, 210));
    }
  }

  // ── Footer no longer needed — rows expand to y=58 max with 3 entries.
  // (Removed Upd timestamp to make room for the 2-line subheader above.)
}

// Draw a top-down aircraft silhouette centred at (cx,cy), rotated by heading degrees
// (0=North/up, 90=East/right, 180=South/down). Uses screen-coord CW rotation.
// Verified: heading=0 → nose up, heading=90 → nose right. ✓
void drawPlaneIcon(int cx, int cy, float headingDeg, uint16_t bodyCol, uint16_t wingCol, uint16_t engineCol){
  float rad = headingDeg * PI / 180.0f;
  float cs = cos(rad), sn = sin(rad);
  // Rotate local point (lx,ly) – local "up" (nose) = negative Y
  auto pt = [&](float lx, float ly, int &ox, int &oy){
    ox = cx + (int)(lx*cs - ly*sn + 0.5f);
    oy = cy + (int)(lx*sn + ly*cs + 0.5f);
  };
  int ax,ay,bx,by;
  // Fuselage – double line for thickness
  pt(0,-10,ax,ay); pt(0,8,bx,by); dsp->drawLine(ax,ay,bx,by,bodyCol);
  pt(1,-9, ax,ay); pt(1,7, bx,by); dsp->drawLine(ax,ay,bx,by,bodyCol);
  // Nose highlight pixel
  pt(0,-11,ax,ay); dsp->drawPixel(ax,ay,wingCol);
  // Left wing: root → tip (leading edge)
  pt(0,-1,ax,ay); pt(-12,3,bx,by); dsp->drawLine(ax,ay,bx,by,wingCol);
  // Left wing: tip → fold (trailing edge)
  pt(-12,3,ax,ay); pt(-7,7,bx,by); dsp->drawLine(ax,ay,bx,by,wingCol);
  // Right wing: root → tip
  pt(0,-1,ax,ay); pt(12,3,bx,by); dsp->drawLine(ax,ay,bx,by,wingCol);
  // Right wing: tip → fold
  pt(12,3,ax,ay); pt(7,7,bx,by); dsp->drawLine(ax,ay,bx,by,wingCol);
  // Tail fins
  pt(0,5,ax,ay); pt(-4,9,bx,by); dsp->drawLine(ax,ay,bx,by,wingCol);
  pt(0,5,ax,ay); pt(4,9,bx,by);  dsp->drawLine(ax,ay,bx,by,wingCol);
  // Engine pods (2-pixel dots on wings)
  pt(-8,0,ax,ay); dsp->fillCircle(ax,ay,1,engineCol);
  pt( 8,0,ax,ay); dsp->fillCircle(ax,ay,1,engineCol);
}

// Category-aware plane silhouette dispatcher.
//   cat 1 = regional/prop  (smaller fuselage, narrow wings, no engine pods)
//   cat 2 = narrowbody     (default — calls drawPlaneIcon)
//   cat 3 = widebody/heavy (longer fuselage, 4 engines)
void drawPlaneByCategory(int cx, int cy, float hdg, uint8_t cat,
                         uint16_t bodyCol, uint16_t wingCol, uint16_t engineCol){
  if(cat == 0) cat = 2;   // unknown → narrowbody fallback
  float rad = hdg * PI / 180.0f;
  float cs = cosf(rad), sn = sinf(rad);
  auto pt = [&](float lx, float ly, int &ox, int &oy){
    ox = cx + (int)(lx*cs - ly*sn + 0.5f);
    oy = cy + (int)(lx*sn + ly*cs + 0.5f);
  };
  int ax,ay,bx,by;

  if(cat == 1){
    // ── REGIONAL / PROP ── compact silhouette, ~16px wingspan, prop swirl
    pt(0,-7,ax,ay); pt(0,6,bx,by); dsp->drawLine(ax,ay,bx,by,bodyCol);
    pt(0,-8,ax,ay); dsp->drawPixel(ax,ay,wingCol);
    // Wings (shorter)
    pt(0,-1,ax,ay); pt(-8,2,bx,by); dsp->drawLine(ax,ay,bx,by,wingCol);
    pt(0,-1,ax,ay); pt( 8,2,bx,by); dsp->drawLine(ax,ay,bx,by,wingCol);
    // Tail
    pt(0,4,ax,ay); pt(-3,7,bx,by); dsp->drawLine(ax,ay,bx,by,wingCol);
    pt(0,4,ax,ay); pt( 3,7,bx,by); dsp->drawLine(ax,ay,bx,by,wingCol);
    // Prop spinner — 4-leaf at nose
    pt(-1,-9,ax,ay); dsp->drawPixel(ax,ay,engineCol);
    pt( 1,-9,ax,ay); dsp->drawPixel(ax,ay,engineCol);
    pt(0,-10,ax,ay); dsp->drawPixel(ax,ay,engineCol);
  }
  else if(cat == 3){
    // ── WIDEBODY / HEAVY ── longer fuselage, 4 engines, wider span
    // Fuselage (3px thick)
    pt(-1,-12,ax,ay); pt(-1,10,bx,by); dsp->drawLine(ax,ay,bx,by,bodyCol);
    pt( 0,-13,ax,ay); pt( 0,11,bx,by); dsp->drawLine(ax,ay,bx,by,bodyCol);
    pt( 1,-12,ax,ay); pt( 1,10,bx,by); dsp->drawLine(ax,ay,bx,by,bodyCol);
    // Nose
    pt(0,-14,ax,ay); dsp->drawPixel(ax,ay,wingCol);
    // Wings — wider sweep (15px tip)
    pt(0,-2,ax,ay); pt(-15,4,bx,by); dsp->drawLine(ax,ay,bx,by,wingCol);
    pt(-15,4,ax,ay); pt(-9,9,bx,by); dsp->drawLine(ax,ay,bx,by,wingCol);
    pt(0,-2,ax,ay); pt( 15,4,bx,by); dsp->drawLine(ax,ay,bx,by,wingCol);
    pt( 15,4,ax,ay); pt( 9,9,bx,by); dsp->drawLine(ax,ay,bx,by,wingCol);
    // Tail (taller)
    pt(0,6,ax,ay); pt(-5,11,bx,by); dsp->drawLine(ax,ay,bx,by,wingCol);
    pt(0,6,ax,ay); pt( 5,11,bx,by); dsp->drawLine(ax,ay,bx,by,wingCol);
    // 4 engine pods on wings (inner + outer pair)
    pt(-5,1,ax,ay);  dsp->fillCircle(ax,ay,1,engineCol);
    pt(-11,3,ax,ay); dsp->fillCircle(ax,ay,1,engineCol);
    pt(5,1,ax,ay);   dsp->fillCircle(ax,ay,1,engineCol);
    pt(11,3,ax,ay);  dsp->fillCircle(ax,ay,1,engineCol);
  }
  else {
    // ── NARROWBODY (default) — same as legacy drawPlaneIcon
    drawPlaneIcon(cx, cy, hdg, bodyCol, wingCol, engineCol);
  }
}

// Short label for the category (used in card UI).
const char* categoryLabel(uint8_t c){
  switch(c){
    case 1: return "REGIONAL";
    case 2: return "NARROWBODY";
    case 3: return "WIDEBODY";
    default: return "JET";
  }
}

// ============================================================
// renderFlights — typography-forward flight card inspired by
// AxisNimble/TheFlightWall_OSS. No radar widget, no plane silhouette —
// just three centered text lines (airline / route / context) framed by a
// thin header. Static between cycles, so no per-frame redraw is needed
// (the whole-panel "blink" is gone for free).
//
// Layout (128×64):
//   y=0..11    header band: ✈ + "OVERHEAD" left, "N up" right
//   y=12       horizontal accent line
//   y=14..29   AIRLINE NAME    (size-2, amber, centered)
//   y=31..38   CALLSIGN        (size-1, dim gray, centered)
//   y=40..55   ROUTE "JFK>LAX" (size-2, cyan, centered)
//                — or "FL350" altitude if no route is known
//   y=57..63   stats strip:    ALT  SPD   DIST DIR    page counter
// ============================================================
void renderFlights(){
  static int  lastInfoIdx = -2;          // -2 = nothing drawn yet
  static int  lastFlightCount = -1;
  bool drawBG = (lastStaticDraw == 0);
  if(drawBG){
    lastStaticDraw = millis();
    cls();

    // === BACKGROUND: night-sky navy with a sparse, deterministic starfield
    fillRect(0,0,PANEL_WIDTH,PANEL_HEIGHT,dsp->color565(3,5,22));
    const uint8_t STAR_X[]={7,19,31,44,58,67,78,93,106,118,12,38,53,87,113};
    const uint8_t STAR_Y[]={20,17,44,31,55,22,48,18,36,52,58,40,25,56,27};
    for(int i=0;i<15;i++){
      uint8_t br=(i%3==0)?70:(i%3==1)?45:30;
      dsp->drawPixel(STAR_X[i],STAR_Y[i],dsp->color565(br,br,br+15));
    }

    // === v10 unified chrome — plane icon, sky-blue accent, counter on right
    drawCardHeader("flights", "OVERHEAD", dsp->color565(90,130,255), -1);

    lastInfoIdx     = -2;
    lastFlightCount = -1;   // force "N up" repaint below
  }

  // Refresh the "N up" counter in the header only when flightCount changes.
  // Repaint color MUST match drawCardHeader's band: dim(accent, 30%).
  if(flightCount != lastFlightCount){
    fillRect(80, 1, 47, 10, dimColor565(dsp->color565(90,130,255), 30));
    if(flightCount > 0){
      char hc[8]; snprintf(hc,8,"%d up",flightCount);
      txt1(hc, PANEL_WIDTH-(int)strlen(hc)*6-2, 3, dsp->color565(100,140,220));
    }
    lastFlightCount = flightCount;
  }

  // === EMPTY STATE ===
  if(flightCount==0){
    if(lastInfoIdx != -1){
      fillRect(0, 14, PANEL_WIDTH, 50, dsp->color565(3,5,22));  // start at 14: keep v10 underline
      // Restore a sample of stars over the wiped band
      const uint8_t SX[]={7,19,31,44,58,72,86,98,112,122};
      const uint8_t SY[]={20,30,44,31,55,22,48,52,36,42};
      for(int i=0;i<10;i++){
        uint8_t br=(i&1)?55:30;
        dsp->drawPixel(SX[i],SY[i],dsp->color565(br,br,br+15));
      }
      ctrTxt2("CLEAR SKIES",  22, C_GRAY);
      ctrTxt1("No traffic overhead", 44, dsp->color565(80,90,120));
      lastInfoIdx = -1;
    }
    return;
  }

  // Advance through the flight list once every 5 seconds.
  static unsigned long lastCycle=0;
  if(millis()-lastCycle>5000){
    flightDispIdx = (flightDispIdx+1) % flightCount;
    lastCycle = millis();
  }
  FlightData &f = flights[flightDispIdx % flightCount];

  // Static page — only redraw the content area when the cycled flight changes.
  if(flightDispIdx == lastInfoIdx) return;
  lastInfoIdx = flightDispIdx;

  // ── Wipe content band y=13..63 back to navy + restore stars
  fillRect(0, 14, PANEL_WIDTH, 50, dsp->color565(3,5,22));  // start at 14: keep v10 underline
  const uint8_t SX[]={7,19,31,44,58,72,86,98,112,122};
  const uint8_t SY[]={20,30,44,31,55,22,48,52,36,42};
  for(int i=0;i<10;i++){
    uint8_t br=(i&1)?55:30;
    dsp->drawPixel(SX[i],SY[i],dsp->color565(br,br,br+15));
  }

  // ── LINE 1: AIRLINE LOGO (16×16) + NAME (size-2, amber) y=14..29 ──
  // When we have a bitmap logo for the carrier, anchor it at left and put
  // the airline name + callsign to its right (left-aligned). When we don't,
  // fall back to the original centered-text layout.
  const char* airline = airlineFromCallsign(f.callsign);
  String airlineStr = String(airline);
  if(airlineStr.length() > 10) airlineStr = airlineStr.substring(0, 10);

  bool hasLogo = drawAirlineLogoBmp16(4, 14, f.callsign);
  if(hasLogo){
    // size-2 name + size-1 callsign stacked to the right of the 16×16 logo
    txt2(airlineStr.c_str(), 24, 14, dsp->color565(255,195,0));
    txt1(trimTo(f.callsign, 8).c_str(), 24, 32, dsp->color565(165,180,210));
  } else {
    // Centered fallback when no logo is available
    ctrTxt2(airlineStr.c_str(), 15, dsp->color565(255,195,0));
    ctrTxt1(trimTo(f.callsign, 8).c_str(), 32, dsp->color565(165,180,210));
  }

  // ── LINE 3: ROUTE or ALTITUDE (size-2, cyan, centered) y=41..56
  if(f.routeKnown && f.routeFrom.length()>0 && f.routeTo.length()>0){
    char routeS[14];
    // ">" reads as a flight arrow on the LED panel (no Unicode font)
    snprintf(routeS, 14, "%s>%s", f.routeFrom.c_str(), f.routeTo.c_str());
    if(strlen(routeS) > 10) routeS[10] = 0;
    ctrTxt2(routeS, 41, dsp->color565(120,200,255));
  } else {
    // No route data → big FL number where the route would be
    char altS[10]; snprintf(altS, 10, "FL%.0f", f.altitude/100.0f);
    ctrTxt2(altS, 41, dsp->color565(120,200,255));
  }

  // ── LINE 4: STATS STRIP y=57..63 (size-1) — altitude, speed, climb, dist
  // and page counter at far right.
  const char* vSym = (f.vertRate> 200)?"^":(f.vertRate<-200)?"v":"~";
  uint16_t    vCol = (f.vertRate> 200)?C_LIME:(f.vertRate<-200)?C_RED:dsp->color565(110,140,170);

  // Left chunk: "FL350 482kt"
  char left[16];
  snprintf(left, 16, "FL%.0f %.0fkt", f.altitude/100.0f, f.speed);
  left[12] = 0;
  txt1(left, 1, 57, dsp->color565(190,210,235));

  // Climb/dive arrow just after the kt
  int leftW = strlen(left) * 6;
  txt1(vSym, 1 + leftW + 1, 57, vCol);

  // Right chunk: "9.9mi NE" or "12mi NE"
  char distS[12];
  if(f.distMi < 10) snprintf(distS, 12, "%.1fmi %s", f.distMi, f.compass.c_str());
  else              snprintf(distS, 12, "%.0fmi %s", f.distMi, f.compass.c_str());
  distS[8] = 0;
  // Place distance ending 22px from right edge (leave room for page counter)
  int distW = strlen(distS) * 6;
  txt1(distS, PANEL_WIDTH - distW - 22, 57, dsp->color565(170,190,220));

  // Page counter "3/5" at far right
  if(flightCount > 1){
    char pgS[6]; snprintf(pgS, 6, "%d/%d", flightDispIdx+1, flightCount);
    int pgW = strlen(pgS) * 6;
    txt1(pgS, PANEL_WIDTH - pgW - 2, 57, dsp->color565(90,110,150));
  }
}

// ----------------------------------------------------------
void renderMoon(){
  // Redraw every 5 seconds to avoid constant redraw but no strobe
  static unsigned long lastMoonRender=0;
  moonPhase=getMoonPhase();
  bool redraw=(millis()-lastMoonRender>5000);
  if(!redraw) return;   // skip frame - nothing changed

  cls();
    // Deep space
    fillRect(0,0,PANEL_WIDTH,PANEL_HEIGHT,dsp->color565(2,2,10));
    // Stars - deterministic so they don't flicker
    drawStars(25,64);

    // Moon - left half
    drawMoonFull(34,32,20,moonPhase);

    // RIGHT INFO: x=68-127 = 60px wide
    // Phase name now wraps onto 2 lines so the full word ("Waning Gibbous"
    // rather than "Wan Gibbous") is always visible.
    const char* pname = moonPhaseName(moonPhase);
    String pn = String(pname);
    int sp = pn.indexOf(' ');
    if(sp > 0){
      // Two-word phase: split into 2 lines, each centered in the 60px column
      String w1 = pn.substring(0, sp);
      String w2 = pn.substring(sp + 1);
      int x1 = 68 + (60 - (int)w1.length() * 6) / 2;
      int x2 = 68 + (60 - (int)w2.length() * 6) / 2;
      txt1(w1.c_str(), x1, 2, C_GOLD);
      txt1(w2.c_str(), x2, 11, C_GOLD);
    } else {
      // Single word — just center it
      int x1 = 68 + (60 - (int)pn.length() * 6) / 2;
      txt1(pname, x1, 6, C_GOLD);
    }

    float illum;
    if(moonPhase<0.5) illum=moonPhase*2.0f*100.0f;
    else illum=(1.0f-moonPhase)*2.0f*100.0f;
    char pct[12]; snprintf(pct,12,"%.0f%% lit",illum);
    txt1(pct, 68, 21, C_WHITE);

    // Illumination bar (60px wide)
    dsp->drawRect(68, 28, 60, 4, C_DARKGRAY);
    fillRect(69, 29, (int)(illum * 58 / 100), 2, C_GOLD);

    // "STRIKE-THROUGH" FIX: the Full/New line used to sit at y=35 (glyph rows
    // 35..42) while the date sat at y=42 (rows 42..49) — they SHARED row 42,
    // which rendered as a line struck through the date. Rows now: next-event
    // at y=33 (33..40), date at y=43 (43..50), phase strip pushed to y=57 so
    // its gold highlight ring (±5px → rows 52..62) clears the date entirely.
    float daysToFull=(0.5f-moonPhase); if(daysToFull<0) daysToFull+=1.0f; daysToFull*=29.53f;
    float daysToNew=(1.0f-moonPhase); if(daysToNew>1.0f) daysToNew-=1.0f; daysToNew*=29.53f;
    char next[16];
    if(daysToFull<daysToNew) snprintf(next,16,"Full %.0fd",daysToFull);
    else snprintf(next,16,"New %.0fd",daysToNew);
    txt1(next, 68, 33, dsp->color565(160, 140, 80));

    struct tm ti2; if(getLocalTime(&ti2)){
      char db[10]; strftime(db, 10, "%b %d", &ti2);
      txt1(db, 68, 43, C_DARKGRAY);
    }

    // 8-moon phase strip at the very bottom
    for(int i=0;i<8;i++){
      float ph2=i/8.0f;
      int mx2=8+i*15, my2=57;
      drawMoonFull(mx2,my2,4,ph2);
      if(fabsf(ph2-moonPhase)<0.07f) dsp->drawCircle(mx2,my2,5,C_GOLD);
    }

  lastMoonRender=millis();
}

// ----------------------------------------------------------
// PIXEL ART — plays GIF animations compiled in via tools/build_animations.ps1.
// One animation per slide visit, rotating through the set. Frames are full
// bitmaps at a fixed position, so each frame simply overdraws the last —
// no erase pass, no trails, no strobe.
void renderPixelArt(){
#if HAS_PIXEL_ANIMS
  static int animIdx = 0;
  static int frame = 0;
  static unsigned long lastFrame = 0;
  bool newSlide = (lastStaticDraw == 0);

  if(PIXEL_ANIM_COUNT == 0) return;   // header exists but is empty

  if(newSlide){
    lastStaticDraw = millis();
    animIdx = (animIdx + 1) % PIXEL_ANIM_COUNT;
    frame = 0;
    lastFrame = 0;
    cls();
    // No name tag — frames are full-bleed 128×64 (cover mode) and would
    // immediately paint over it anyway.
  }

  const PixelAnim &a = PIXEL_ANIMS[animIdx];
  // Single-frame "art cards" (converted PNG/JPG stills) draw once and rest.
  if(a.frameCount <= 1 && lastFrame != 0) return;
  if(millis() - lastFrame < a.delayMs) return;
  lastFrame = millis();

  // Centered placement
  int ox = (PANEL_WIDTH  - a.w) / 2;
  int oy = (PANEL_HEIGHT - a.h) / 2;
  const uint16_t* fr = (const uint16_t*)pgm_read_ptr(&a.frames[frame]);
  for(int y=0; y<a.h; y++){
    for(int x=0; x<a.w; x++){
      uint16_t c = pgm_read_word(&fr[y*a.w + x]);
      dsp->drawPixel(ox+x, oy+y, c);
    }
  }
  frame = (frame + 1) % a.frameCount;
#else
  // No animations compiled in — brief placeholder; main loop skips onward.
  if(millis()-lastStaticDraw<STATIC_REDRAW_MS) return;
  lastStaticDraw=millis();
  cls();
  ctrTxt1("Drop GIFs in tools/gifs", 26, C_DARKGRAY);
  ctrTxt1("run build_animations", 36, C_DARKGRAY);
#endif
}

// ----------------------------------------------------------
void renderGameOfLife(){
  static int golStyle=0;
  if(!golInited){ initGOL(); golStyle=random(0,4); }

  static unsigned long lastStep=0;
  if(millis()-lastStep>75){stepGOL();lastStep=millis();}

  // Full screen clear each frame (GOL is fully animated)
  cls();

  // Style cycles: green matrix, blue plasma, red fire, rainbow
  for(int y=0;y<GOL_H;y++) for(int x=0;x<GOL_W;x++){
    if(golGrid[y][x]){
      uint16_t col;
      if(golStyle==0)      col=dsp->color565(0,200+random(0,55),random(0,60));  // green matrix
      else if(golStyle==1) col=dsp->color565(0,random(0,100),200+random(0,55)); // blue electric
      else if(golStyle==2) col=dsp->color565(200+random(0,55),random(0,80),0);  // red fire
      else                  col=dsp->color565(random(0,255),random(0,255),random(0,255)); // rainbow
      fillRect(x*2,y*2,2,2,col);
    } else {
      // Dead cells: faint background
      if(golStyle==0) fillRect(x*2,y*2,2,2,dsp->color565(0,8,0));
      else if(golStyle==1) fillRect(x*2,y*2,2,2,dsp->color565(0,0,10));
      else if(golStyle==2) fillRect(x*2,y*2,2,2,dsp->color565(8,0,0));
      else fillRect(x*2,y*2,2,2,C_BLACK);
    }
  }
  // Reset for next slide
  if(millis()-slideStart>9500) golInited=false;
}

// ----------------------------------------------------------
// US holiday lookup — fixed dates + floating (nth-weekday) holidays.
// Returns the holiday name for a date, or nullptr. Keeps the calendar card
// alive even when the GCal relay is down, and adds color when it's up.

// Day-of-week for a y/m/d (0=Sunday) via Sakamoto's algorithm.
static int dowFor(int y, int m, int d){
  static const int tt[] = {0,3,2,5,0,3,5,1,4,6,2,4};
  if(m < 3) y -= 1;
  return (y + y/4 - y/100 + y/400 + tt[m-1] + d) % 7;
}
// Day-of-month for the nth occurrence of weekday `dow` in month m (1-based n).
static int nthDow(int y, int m, int dow, int n){
  int first = dowFor(y, m, 1);
  int d = 1 + ((dow - first + 7) % 7) + (n-1)*7;
  return d;
}
// Day-of-month for the LAST occurrence of weekday `dow` in month m.
static int lastDow(int y, int m, int dow){
  static const int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  int dim = mdays[m-1];
  if(m==2 && ((y%4==0 && y%100!=0) || y%400==0)) dim = 29;
  int last = dowFor(y, m, dim);
  return dim - ((last - dow + 7) % 7);
}

const char* holidayForDate(int y, int m, int d){
  // Fixed-date holidays
  if(m==1  && d==1)  return "New Years Day";
  if(m==2  && d==14) return "Valentines Day";
  if(m==3  && d==17) return "St Patricks Day";
  if(m==6  && d==19) return "Juneteenth";
  if(m==7  && d==4)  return "July 4th";
  if(m==10 && d==31) return "Halloween";
  if(m==11 && d==11) return "Veterans Day";
  if(m==12 && d==24) return "Christmas Eve";
  if(m==12 && d==25) return "Christmas";
  if(m==12 && d==31) return "New Years Eve";
  // Floating holidays
  if(m==1  && d==nthDow(y,1,1,3))   return "MLK Day";          // 3rd Mon Jan
  if(m==2  && d==nthDow(y,2,1,3))   return "Presidents Day";   // 3rd Mon Feb
  if(m==5  && d==nthDow(y,5,0,2))   return "Mothers Day";      // 2nd Sun May
  if(m==5  && d==lastDow(y,5,1))    return "Memorial Day";     // last Mon May
  if(m==6  && d==nthDow(y,6,0,3))   return "Fathers Day";      // 3rd Sun Jun
  if(m==9  && d==nthDow(y,9,1,1))   return "Labor Day";        // 1st Mon Sep
  if(m==10 && d==nthDow(y,10,1,2))  return "Columbus Day";     // 2nd Mon Oct
  if(m==11 && d==nthDow(y,11,4,4))  return "Thanksgiving";     // 4th Thu Nov
  return nullptr;
}

// Finds the next holiday within `maxDays`, writing name + date into the
// out-params. Returns days-from-today (0 = today), or -1 if none in range.
int nextHoliday(char* nameOut, size_t nameLen, char* dateOut, size_t dateLen, int maxDays){
  struct tm ti;
  if(!getLocalTime(&ti)) return -1;
  time_t now = mktime(&ti);
  for(int i = 0; i <= maxDays; i++){
    time_t t = now + (time_t)i * 86400;
    struct tm dt; localtime_r(&t, &dt);
    const char* h = holidayForDate(dt.tm_year+1900, dt.tm_mon+1, dt.tm_mday);
    if(h){
      snprintf(nameOut, nameLen, "%s", h);
      strftime(dateOut, dateLen, "%b %d", &dt);
      return i;
    }
  }
  return -1;
}

void renderCalendar(){
  if(millis()-lastStaticDraw<STATIC_REDRAW_MS) return;
  lastStaticDraw=millis();
  cls();

  // ── v10 unified chrome — date takes the right slot instead of the clock
  drawCardHeader("calendar", "CALENDAR", C_GREEN, -1);
  struct tm ti; if(getLocalTime(&ti)){
    char db[10]; strftime(db, 10, "%a %d", &ti);
    int dw = strlen(db) * 6;
    txt1(db, PANEL_WIDTH - dw - 2, 3, dimColor565(C_GREEN, 85));
  }

  // ── Holiday lookup — today's holiday gets a gold banner; otherwise the
  // next one within 45 days fills the footer. Works with or without GCal.
  char holName[24] = "", holDate[10] = "";
  int holDays = nextHoliday(holName, sizeof(holName), holDate, sizeof(holDate), 45);
  int bodyY = 17;

  if(holDays == 0){
    // TODAY is a holiday — gold banner directly under the header
    fillRect(0, 16, PANEL_WIDTH, 10, dsp->color565(70, 50, 0));
    char hb[30]; snprintf(hb, 30, "* %s *", holName);
    ctrTxt1(hb, 18, C_GOLD);
    bodyY = 28;
  }

  // ── Empty / unconfigured states (holiday still shows above + below)
  if(calCount == 0){
    if(strlen(GCAL_RELAY_URL) == 0){
      ctrTxt1("Deploy GCal relay", bodyY+12, C_YELLOW);
    } else {
      ctrTxt1("No events today", bodyY+12, C_GRAY);
    }
    // Upcoming holiday as useful filler content
    if(holDays > 0){
      char up[40]; snprintf(up, 40, "%s in %dd", holName, holDays);
      ctrTxt1(up, bodyY+26, dsp->color565(200,170,60));
      ctrTxt1(holDate, bodyY+36, dsp->color565(120,105,50));
    }
    return;
  }

  // ── Event list, full width. Each event = TIME (gray) + TITLE (white).
  // Row height 11px. Rows available shrink by one when a holiday banner is up.
  int maxRows = (holDays == 0) ? 3 : 4;
  int shown = (calCount < maxRows) ? calCount : maxRows;
  for(int i=0; i<shown; i++){
    int ey = bodyY + i * 11;
    // Bullet pip on the left
    fillRect(0, ey+2, 3, 3, C_GREEN);
    // Time (max 7 chars: "11:30AM")
    txt1(trimTo(calEvents[i].time, 7).c_str(), 5, ey, C_GRAY);
    // Title takes remaining width
    txt1(trimTo(calEvents[i].title, 13).c_str(), 50, ey, C_WHITE);
  }

  // Footer: "+N more" or upcoming holiday hint
  if(calCount > shown){
    char more[12]; snprintf(more, 12, "+%d more", calCount - shown);
    int mw = strlen(more) * 6;
    txt1(more, PANEL_WIDTH - mw - 2, 57, C_DARKGRAY);
  } else if(holDays > 0 && shown < 4){
    char up[40]; snprintf(up, 40, "%s %s", holName, holDate);
    up[21] = 0;   // cap at 21 chars (126px)
    ctrTxt1(up, 57, dsp->color565(160,135,50));
  }
}

// ----------------------------------------------------------
// Dedicated WHOOP card — full-page recovery + HRV + RHR + sleep + strain.
// Big colored recovery score on the left, smaller stats on the right.
void renderWhoop(){
  if(millis()-lastStaticDraw<STATIC_REDRAW_MS) return;
  lastStaticDraw=millis();
  cls();

  uint16_t whoopRed   = dsp->color565(220, 30, 50);
  uint16_t whoopRedD  = dsp->color565(120, 15, 28);
  uint16_t labelDim   = dsp->color565(120, 120, 140);

  // ── v10 unified chrome — heart icon, WHOOP red accent, date on the right
  drawCardHeader("whoop", "WHOOP", whoopRed, -1);
  struct tm ti; if(getLocalTime(&ti)){
    char db[10]; strftime(db, 10, "%a %d", &ti);
    int dw = strlen(db) * 6;
    txt1(db, PANEL_WIDTH - dw - 2, 3, dimColor565(whoopRed, 85));
  }

  // No-data state
  if(!whoop.valid){
    if(strlen(WHOOP_RELAY_URL) == 0){
      ctrTxt1("WHOOP relay not set",  26, C_YELLOW);
      ctrTxt1("Add URL to secrets.h",  36, C_DARKGRAY);
    } else {
      ctrTxt1("Loading WHOOP...", 26, C_GRAY);
      ctrTxt1("Check serial monitor", 38, C_DARKGRAY);
    }
    return;
  }

  // ── LEFT COLUMN x=0..56 — RECOVERY (the headline metric)
  // All-size-1 typography across both columns so the card reads as a single
  // compact dashboard rather than competing big-number panels.
  int sw = whoop.recoveryLabel.length() * 6;
  int sx = (58 - sw) / 2;
  if(sx < 0) sx = 0;
  txt1(whoop.recoveryLabel.c_str(), sx, 14, whoop.recoveryColor);

  // Recovery dial — smaller (R=11) so the percentage number sits inside it
  // at size-1, leaving room for the "RECOVERY" caption below.
  int rcx = 28, rcy = 36, rR = 11;
  for(int r = rR-2; r <= rR; r++) dsp->drawCircle(rcx, rcy, r, dsp->color565(35,35,45));
  int sweepDeg = (int)(whoop.recovery * 360 / 100);
  for(int deg = -90; deg < -90 + sweepDeg; deg++){
    float rad = deg * PI / 180.0f;
    for(int r = rR-2; r <= rR; r++){
      int px = rcx + (int)(r * cosf(rad));
      int py = rcy + (int)(r * sinf(rad));
      dsp->drawPixel(px, py, whoop.recoveryColor);
    }
  }
  // Center percentage — size-1, fits comfortably inside R=11 dial
  char rec[6]; snprintf(rec, 6, "%.0f", whoop.recovery);
  int rw = strlen(rec) * 6;
  txt1(rec, rcx - rw/2, rcy - 3, whoop.recoveryColor);
  // Caption below dial
  ctrTxt1("RECOVERY", 52, labelDim);

  // Divider
  dsp->drawFastVLine(58, 14, 50, dsp->color565(40, 40, 50));

  // ── RIGHT COLUMN x=62..127 — 4 single-line stat rows, all size-1.
  // Rows at y=16, 26, 36, 46 — 10 px apart, no more competing size-2 numerals.
  // Layout per row:   LABEL (dim, left)  VALUE (right-aligned, color-coded)
  auto drawRow = [&](int y, const char* label, const String &value, uint16_t valColor){
    txt1(label, 62, y, labelDim);
    int vw = (int)value.length() * 6;
    txt1(value.c_str(), PANEL_WIDTH - vw - 2, y, valColor);
  };

  // Row 1: STRAIN — y=16
  char st[6]; snprintf(st, 6, "%.1f", whoop.strain);
  uint16_t strainC = whoop.strain < 10 ? C_LIME
                   : whoop.strain < 14 ? C_YELLOW
                   : whoop.strain < 18 ? C_ORANGE : C_RED;
  drawRow(16, "STRAIN", String(st), strainC);

  // Row 2: SLEEP — y=26
  char sl[8]; snprintf(sl, 8, "%.0f%%", whoop.sleepScore);
  uint16_t sleepC = whoop.sleepScore >= 85 ? C_LIME
                  : whoop.sleepScore >= 70 ? C_YELLOW : C_RED;
  drawRow(26, "SLEEP", String(sl), sleepC);

  // Row 3: HRV — y=36 (with "ms" unit baked into the value string)
  char hr[10]; snprintf(hr, 10, "%.0fms", whoop.hrv);
  drawRow(36, "HRV", String(hr), C_CYAN);

  // Row 4: RHR — y=46 (with "bpm" unit)
  char rh[10]; snprintf(rh, 10, "%.0fbpm", whoop.rhr);
  drawRow(46, "RHR", String(rh), C_PINK);
}

// ----------------------------------------------------------
// LIS and Ipswich Bay charts
// Hardcoded buoy positions mapped to 128x64 pixel space

struct BuoyDef {
  const char* id;
  const char* name;
  float lat, lon;
  int px, py;
  uint16_t col;
  int onMs, offMs;
};

// LIS buoys — zoomed-out frame: lon -74.0°W → -71.5°W, lat 40.55°N → 41.47°N
// Covers NYC/Throgs Neck on the west, full CT+RI shore on top, full Long Island
// with both forks on the south, and Block Island/Fishers Island on the east.
// 0xF800 = bright red, 0x07E0 = bright green, 0xFFFF = white
const BuoyDef LIS_BUOYS[] = {
  {"44025","44025",  40.97,-72.00,  102,35, 0xFFFF, 2500,2500},  // white Fl 4s mid sound
  {"8461490","NLON", 41.36,-72.09,   98, 8, 0x07E0, 1000,5000},  // green Fl G 2.5s New London
  {"8465705","BPT",  41.17,-73.18,   42,21, 0xF800, 2000,2000},  // red Fl R 4s Bridgeport
  {"8510560","MTK",  41.07,-71.96,  104,28, 0xFFFF, 500,4500},   // white Fl 5s Montauk approach
  {"N/A","ExRk",     41.04,-73.70,   15,30, 0xF800, 1000,3000},  // red Fl R 4s Execution Rocks
  {"N/A","Strat",    41.09,-73.12,   45,26, 0x07E0, 2500,2500},  // green Fl G 5s Stratford
  {"N/A","Saybk",    41.27,-72.34,   85,14, 0xFFFF, 1500,3500},  // white Iso 4s Saybrook
  {"N/A","Faulk",    41.26,-72.65,   69,15, 0xF800, 750,2250},   // red Fl R 3s Faulkner Is
};
const int LIS_COUNT=8;

// Cape Ann buoys — zoomed-out frame: lon -71.10°W → -70.30°W, lat 42.15°N → 42.85°N
// Matches the commercial wood-map reference: Salisbury/Newburyport top,
// Plum Island strip, Ipswich Bay, Cape Ann peninsula (Gloucester/Rockport),
// Salem Sound/Marblehead bottom.
const BuoyDef IPW_BUOYS[] = {
  {"44013","B16",   42.35,-70.65,   72,46, 0xFFFF, 2000,2000},  // white Fl 4s Boston Approach
  {"44018","SShl",  42.19,-70.57,   85,60, 0xFFFF, 2500,2500},  // white Fl 5s SE Shoal (S tip)
  {"N/A","PlmI",    42.76,-70.80,   48, 8, 0x07E0, 1000,5000},  // green Fl G 2.5s Plum Is Sound
  {"N/A","Glcs",    42.58,-70.66,   70,25, 0xF800, 2000,4000},  // red Fl R 4s Gloucester Harbor
  {"N/A","Annq",    42.67,-70.69,   66,17, 0x07E0, 1500,6000},  // green Fl G 3s Annisquam River
  {"N/A","Rkpt",    42.66,-70.61,   78,17, 0xFFFF, 3000,3000},  // white Iso 6s Rockport
};
const int IPW_COUNT=6;

// === Minor NOAA aids — 1-pixel blinkers with real flash characteristics ===
// Each entry is one light from the NOAA Light List; position is real lat/lon
// projected into the chart grid. Flash timing matches the actual light:
//   onMs+offMs = period; onMs = single-flash duration.
// For multi-flash lights (Fl(2), Fl(3)) we use the group period & avg on-time.
struct MiniAid {
  int px, py;
  uint16_t col;    // 0xF800 red, 0x07E0 green, 0xFFFF white/safe-water
  int onMs, offMs;
};

// LIS minor aids — comprehensive NOAA Light List for Long Island Sound.
// Each is one pixel; flash timing matches the real light characteristic.
// 0xF800=red, 0x07E0=green, 0xFFFF=white/safe, 0xFFE0=yellow special-purpose.
const MiniAid LIS_AIDS[] = {
  // ===== WESTERN NARROWS (NYC/Throgs Neck → Greenwich) =====
  { 11,46, 0xFFFF,  250,  750 },  // Throgs Neck Lt       Q W
  { 12,45, 0xFFFF,  500, 3500 },  // Stepping Stones Lt   Iso W 4s
  { 14,42, 0xFFFF,  500, 1500 },  // Sands Point          Fl W 2s
  { 15,41, 0xF800,  500, 9500 },  // Execution Rocks      Fl R 10s (mark)
  { 18,38, 0x07E0,  500, 3500 },  // Hempstead Hbr        Fl G 4s
  { 19,33, 0x07E0,  500, 9500 },  // Great Captain Is     Fl G 10s
  // ===== CT SHORE: Greenwich → New Haven =====
  { 22,32, 0x07E0,  500, 3500 },  // Greenwich Pt         Fl G 4s
  { 24,12, 0x07E0,  500, 3500 },  // Stamford Hbr E Bkwtr Fl G 4s
  { 25,13, 0xF800,  500, 3500 },  // Stamford Hbr W Bkwtr Fl R 4s
  { 28,12, 0xFFFF,  400,11600 },  // Greens Ledge         Fl(3) W 12s
  { 30,11, 0xF800,  400, 5600 },  // Pecks Ledge          Fl(2) R 6s
  { 32,11, 0x07E0,  500, 5500 },  // Cockenoe Hbr (Norw)  Fl G 6s
  { 36,10, 0xF800,  500, 3500 },  // Cable & Anchor Reef  Fl R 4s
  { 40,11, 0xF800,  500, 5500 },  // Penfield Reef        Fl R 6s
  { 42,11, 0x07E0,  500, 3500 },  // Bridgeport Hbr inner Fl G 4s
  { 44,11, 0xF800,  500, 5500 },  // Bridgeport Bkwtr     Fl R 6s
  { 46,22, 0xFFFF,  500, 4500 },  // Stratford Shoal/MG   Fl W 5s (mid sound)
  { 56,11, 0xF800,  500, 5500 },  // New Haven Bkwtr W    Fl R 6s
  { 58,11, 0x07E0,  500, 5500 },  // New Haven Bkwtr E    Fl G 6s
  // ===== CT SHORE: Branford → Old Saybrook =====
  { 65, 9, 0xFFFF,  500, 5500 },  // Branford Reef        Fl W 6s
  { 70, 9, 0xFFFF,  500, 1500 },  // Faulkner Is approach Fl W 2s
  { 80, 9, 0x07E0,  500, 3500 },  // Cornfield Pt         Fl G 4s
  { 85,12, 0xFFFF,  500, 5500 },  // Saybrook Outer Bar   Fl W 6s
  { 87,11, 0xFFFF,  400, 5600 },  // Lynde Point          Fl(2) W 6s
  // ===== CT SHORE: Niantic → Stonington =====
  { 92,10, 0x07E0,  500, 3500 },  // Niantic Bay Bkwtr    Fl G 4s
  { 95,10, 0xFFFF,  500, 5500 },  // Bartlett Reef        Fl W 6s
  { 96, 8, 0xFFFF,  400,29600 },  // New London Ledge     Fl(3) W 30s
  { 99, 7, 0x07E0,  500, 3500 },  // New London Hbr Lt    Iso G 4s
  {103, 8, 0xF800,  500, 5500 },  // Bartlett Reef E      Fl R 6s
  {105, 9, 0xFFFF,  500, 4500 },  // Stonington Hbr Bkwtr Iso W 5s
  {107, 7, 0xF800,  500, 5500 },  // Latimer Reef         Fl R 6s
  // ===== RI / WATCH HILL =====
  {110, 5, 0xFFFF,  500, 4500 },  // Watch Hill (RI)      Alt W R 5s
  {114, 4, 0xF800,  500, 3500 },  // Napatree Pt          Fl R 4s
  // ===== EAST END APPROACHES (Race / Plum / Block) =====
  {100,16, 0xF800,  500, 9500 },  // Race Rock            Fl R 10s
  {103,15, 0xFFFF,  250,  750 },  // Valiant Rock         Q W
  { 91,21, 0xFFFF,  250,  750 },  // Plum Gut Lt          Q W
  { 93,21, 0x07E0,  500, 5500 },  // Plum Island Lt       Fl G 6s
  // ===== LI NORTH SHORE: Nassau → Suffolk =====
  { 22,42, 0xFFFF,  500, 5500 },  // Cold Spring Hbr      Fl W 6s
  { 30,40, 0xFFFF,  500, 4500 },  // Eatons Neck          Fl W 5s
  { 38,39, 0x07E0,  500, 3500 },  // Smithtown Bay        Fl G 4s
  { 45,38, 0x07E0,  500, 9500 },  // Old Field Point      Fl G 10s
  { 50,37, 0xF800,  500, 5500 },  // Port Jeff Mt Sinai   Fl R 6s
  { 60,37, 0x07E0,  500, 5500 },  // Wading River         Fl G 6s
  { 74,37, 0xFFFF,  500, 5500 },  // Mattituck Inlet      Fl W 6s
  { 82,37, 0xF800,  500, 3500 },  // Mattituck E reef     Fl R 4s
  { 88,28, 0x07E0,  500, 5500 },  // Greenport Hbr        Fl G 6s
  { 92,29, 0xFFFF,  500, 9500 },  // Cedar Is Lt (Sag Hbr) Fl W 10s
  { 95,27, 0xF800,  500, 3500 },  // Gardiners Bay        Fl R 4s
  { 99,22, 0xFFFF,  250,  750 },  // Orient Point         Q W
  {103,23, 0xFFFF,  500, 1500 },  // Cerberus Shoal       Fl W 2s
  // ===== SOUTH FORK / MONTAUK =====
  {110,29, 0xFFFF,  400,29600 },  // Montauk Pt           Fl(3) W 30s
  // ===== BLOCK ISLAND =====
  {123,18, 0xFFFF,  500, 4500 },  // Block Is North Lt    Fl W 5s
  {125,23, 0x07E0,  500, 4500 },  // Block Is SE Light    Fl G 5s
};
const int LIS_AIDS_COUNT = sizeof(LIS_AIDS)/sizeof(LIS_AIDS[0]);

// Cape Ann minor aids — comprehensive NOAA Light List for North Shore MA.
// Frame covers Salisbury → Marblehead, includes Plum Island, Cape Ann
// peninsula, and Salem Sound.
const MiniAid IPW_AIDS[] = {
  // ===== MERRIMACK / NEWBURYPORT (top) =====
  { 49, 3, 0x07E0,  500, 3500 },  // Merrimack R N Jetty  Fl G 4s
  { 51, 4, 0xF800,  500, 3500 },  // Merrimack R S Jetty  Fl R 4s
  { 47, 5, 0xFFFF,  500, 5500 },  // Newburyport Hbr      Iso W 6s
  // ===== PLUM ISLAND / IPSWICH BAY =====
  { 47, 8, 0x07E0,  500, 5500 },  // Plum Is N Pt         Fl G 6s
  { 48,14, 0xF800,  500, 3500 },  // Plum Is S (Sandy Pt) Fl R 4s
  { 48,16, 0x07E0,  500, 3500 },  // Ipswich Light        Fl G 4s
  { 52,18, 0x07E0,  500, 5500 },  // Essex Bay entrance   Fl G 6s
  { 56,18, 0xFFFF,  500, 4500 },  // Crane Beach approach Iso W 5s
  // ===== ANNISQUAM RIVER / NW CAPE ANN =====
  { 64,16, 0x07E0,  500, 5500 },  // Squam R Jetty        Fl G 6s
  { 66,16, 0xF800,  400, 5600 },  // Annisquam Hbr Lt     Fl(2) R 6s
  // ===== ROCKPORT / NE CAPE ANN =====
  { 76,14, 0xFFFF,  500, 5500 },  // Halibut Pt           Fl W 6s
  { 78,14, 0x07E0,  500, 3500 },  // Andrews Pt           Fl G 4s
  { 80,16, 0xFFFF,  500, 4500 },  // Sandy Bay Bkwtr      Iso W 5s
  { 84,16, 0xFFFF,  400,11600 },  // Straitsmouth Is      Fl(3) W 12s
  { 85,18, 0xF800,  400, 9600 },  // Thacher Is twin (N)  Fl(2) R 10s
  { 85,19, 0xF800,  400, 9600 },  // Thacher Is twin (S)  Fl(2) R 10s
  { 84,21, 0x07E0,  500, 3500 },  // Milk Is              Fl G 4s
  // ===== GLOUCESTER HARBOR / SE CAPE ANN =====
  { 71,24, 0xF800, 3000, 3000 },  // Ten Pound Is         Iso R 6s
  { 70,25, 0x07E0,  500, 3500 },  // Dog Bar Bkwtr        Fl G 4s
  { 70,27, 0xFFFF,  500, 4500 },  // Eastern Point        Fl W 5s
  { 76,27, 0xF800,  500, 5500 },  // Eastern Pt offshore  Fl R 6s
  { 67,27, 0x07E0,  500, 5500 },  // Norman's Woe         Fl G 6s
  // ===== MAGNOLIA / MANCHESTER / BEVERLY =====
  { 60,28, 0xF800,  500, 3500 },  // Magnolia Hbr         Fl R 4s
  { 54,30, 0x07E0,  500, 3500 },  // Manchester Hbr       Fl G 4s
  { 50,32, 0x07E0,  500, 5500 },  // Misery Is            Fl G 6s
  { 49,34, 0xFFFF,  400,29600 },  // Bakers Is Lt         Fl(2) W 30s
  // ===== SALEM SOUND / MARBLEHEAD / NAHANT =====
  { 53,36, 0xFFFF,  500, 5500 },  // Halfway Rock         Fl W 6s
  { 47,38, 0xF800,  500, 9500 },  // Marblehead Lt        Fl R 10s
  { 45,40, 0x07E0,  500, 3500 },  // Tinkers Is           Fl G 4s
  { 38,46, 0xFFFF,  500, 4500 },  // Cat Is (Salem)       Iso W 5s
  { 32,52, 0xF800,  500, 3500 },  // Egg Rock (Nahant)    Fl R 4s
  // ===== OFFSHORE BUOYS =====
  { 95,40, 0xFFE0,  500, 5500 },  // NOAA Buoy 44090      yellow special
};
const int IPW_AIDS_COUNT = sizeof(IPW_AIDS)/sizeof(IPW_AIDS[0]);

// Initialize buoy colors - no longer needed (colors baked into literals above)
// Kept as no-op so the call from setup() still compiles.
void initBuoyColors(){
  // const_cast to flash-resident structs is undefined behavior on ESP32;
  // colors are now hardcoded as RGB565 literals in the BuoyDef arrays.
}

// Chart static background drawn once, buoys redrawn slowly
static bool lisChartDrawn=false;
static bool ipwChartDrawn=false;

void drawChartBackground(bool isLIS){
  cls();
  // Water gradient
  for(int y=0;y<PANEL_HEIGHT;y++){
    uint16_t wc;
    if(y<22)      wc=dsp->color565(0,40,90);
    else if(y<44) wc=dsp->color565(0,75,145);
    else          wc=dsp->color565(20,115,180);
    dsp->drawFastHLine(0,y,PANEL_WIDTH,wc);
  }

  uint16_t landC =dsp->color565(180,170,80);   // NOAA chart yellow-tan
  uint16_t landS =dsp->color565(140,130,55);   // shadow
  uint16_t sandC =dsp->color565(245,225,150);  // bright sand edge

  if(isLIS){
    // === LONG ISLAND SOUND — ENHANCED SHORELINE DETAIL ===
    // Frame unchanged: lon -74.0°W → -71.5°W, lat 40.55°N → 41.47°N
    // (so existing buoy/aid pixel coordinates stay valid).
    //
    // What's new vs the previous chart:
    //   • CT harbors are now distinct U-shaped indents (Stamford, Norwalk,
    //     Bridgeport, New Haven, CT River, Niantic, New London) rather than
    //     gentle sin-curves.
    //   • Long Island has a recognizable "fish" silhouette — narrow at
    //     Queens, widening through Nassau/Suffolk, two sharp forks east.
    //   • Small islands added: Captains Is, Norwalk Is, Faulkner Is,
    //     Charles Is, Falkner, Plum Is (CT version).
    //   • Faint cyan depth contour line traces the 60-ft contour.

    uint16_t depthC = dsp->color565(40, 110, 175);   // 60-ft depth contour

    // ── CT/NY/RI NORTH SHORE ──
    // Pre-tabulated shoreline: each entry = (xStart, xEnd, baseY, harborDepth)
    // We then poke deeper indents for specific harbors.
    for(int x=0; x<PANEL_WIDTH; x++){
      int sy;
      // NYC / Bronx — the Bronx pokes deep south because of Throgs Neck
      if(x<4)        sy = 32;                             // Bronx tip
      else if(x<10)  sy = 30 - (x-4);                     // Westchester rising
      else if(x<14)  sy = 22 - (x-10);                    // hard climb
      // Greenwich — Cos Cob harbor
      else if(x<20)  sy = 12;
      // Stamford harbor — DEEP U-cut at x=22-24
      else if(x<22)  sy = 11;
      else if(x<25)  sy = 16;                             // ← Stamford harbor mouth
      else if(x<28)  sy = 12;
      // Norwalk Is — a few small dots offshore (drawn separately below)
      else if(x<33)  sy = 11;
      // Norwalk harbor
      else if(x<36)  sy = 14;                             // ← Norwalk harbor cut
      else if(x<40)  sy = 11;
      // Bridgeport peninsula + harbor — Penfield Reef juts south
      else if(x<43)  sy = 12;
      else if(x<46)  sy = 15;                             // ← Bridgeport harbor cut
      else if(x<50)  sy = 11;
      // Charles Is hint — tiny blob (drawn separately)
      else if(x<54)  sy = 10;
      // New Haven harbor — DEEP U-cut, distinctive
      else if(x<58)  sy = 12;
      else if(x<62)  sy = 17;                             // ← New Haven harbor mouth (deepest)
      else if(x<66)  sy = 12;
      // Branford / Madison / Guilford
      else if(x<74)  sy = 9;
      // Connecticut River mouth — Saybrook
      else if(x<78)  sy = 11;
      else if(x<82)  sy = 14;                             // ← CT River cut
      else if(x<86)  sy = 11;
      // Niantic Bay
      else if(x<92)  sy = 12;                             // ← Niantic indent
      else if(x<96)  sy = 9;
      // New London / Thames River
      else if(x<100) sy = 13;                             // ← New London / Thames mouth
      else if(x<104) sy = 8;
      // Stonington / Mystic
      else if(x<110) sy = 10;
      // Watch Hill RI — shore rises into RI corner
      else if(x<118) sy = 6;
      else           sy = 3;
      // ── DE-BLOCKING PASS ──
      // 1. deterministic ±1 jitter breaks the dead-flat plateaus
      if(((x*7 + sy*5) % 5) == 0) sy += 1;
      if(((x*11 + sy*3) % 7) == 0 && sy > 2) sy -= 1;
      // 2. smooth abrupt harbor steps into ≤2px ramps (no more cliffs)
      static int syPrevCT;       // persists across the x loop only
      if(x == 0) syPrevCT = sy;
      if(abs(sy - syPrevCT) > 2) sy = syPrevCT + (sy > syPrevCT ? 2 : -2);
      syPrevCT = sy;
      fillRect(x, 0, 1, sy, landC);
      // Land texture (rocky NE coast feel)
      if((x*7+sy*3)%4 == 0 && sy > 1) dsp->drawPixel(x, sy-1, landS);
      dsp->drawPixel(x, sy, sandC);
      // 3. pale shallow-water band hugging the shore (chart-style tint)
      dsp->drawPixel(x, sy+1, dsp->color565(28, 88, 152));
    }
    // Small islands offshore CT
    fillRect(20, 16, 2, 1, landC); dsp->drawPixel(19, 16, sandC);   // Great Captain Is
    fillRect(33, 14, 3, 1, landC); dsp->drawPixel(36, 14, sandC);   // Norwalk Islands cluster
    fillRect(38, 13, 2, 1, landC);
    fillRect(50, 14, 2, 1, landC);                                  // Charles Is (Milford)
    dsp->drawPixel(67, 15, landC); dsp->drawPixel(68, 15, landC);   // Falkner Is
    dsp->drawPixel(67, 14, sandC);

    // CT label + sound name
    txtOutline("CT",        2, 1, C_LIME);
    txtOutline("L.I. SOUND", 36, 20, C_CYAN);

    // ── LONG ISLAND MAIN BODY — recognisable fish shape ──
    for(int x=0; x<PANEL_WIDTH; x++){
      int sy;
      // Queens — narrow neck
      if(x<5)        sy = 40;
      else if(x<10)  sy = 43;                              // Bayside / Manhasset
      // Manhasset Neck cut
      else if(x<13)  sy = 41;
      // Great Neck peninsula
      else if(x<17)  sy = 39;
      // Cold Spring / Oyster Bay (deep cut)
      else if(x<22)  sy = 41;
      else if(x<25)  sy = 38;                              // ← Oyster Bay deep cut northward
      // Lloyd Neck / Huntington
      else if(x<30)  sy = 42;
      else if(x<34)  sy = 39;                              // ← Huntington Bay cut
      // Smithtown Bay
      else if(x<42)  sy = 40;
      else if(x<48)  sy = 38;                              // ← Smithtown
      // Stony Brook / Port Jefferson harbor
      else if(x<54)  sy = 39;
      else if(x<58)  sy = 36;                              // ← Port Jeff harbor
      // Mt Sinai / Wading River
      else if(x<68)  sy = 38;
      // Suffolk eastern run
      else if(x<82)  sy = 37;
      // Riverhead — peninsula start narrows here
      else if(x<88)  sy = 38;
      else           sy = 200;                             // forks take over
      if(sy < PANEL_HEIGHT){
        // De-blocking: jitter + ramp smoothing (same treatment as CT shore)
        if(((x*5 + sy*7) % 5) == 0) sy -= 1;
        if(((x*13 + sy*3) % 7) == 0) sy += 1;
        static int syPrevLI;
        if(x == 0 || syPrevLI >= 200) syPrevLI = sy;
        if(abs(sy - syPrevLI) > 2) sy = syPrevLI + (sy > syPrevLI ? 2 : -2);
        syPrevLI = sy;
        fillRect(x, sy, 1, PANEL_HEIGHT-sy, landC);
        if((x*5+sy*3)%6 == 0) dsp->drawPixel(x, sy+2, landS);
        dsp->drawPixel(x, sy-1, sandC);
        // Shallow-water tint above the LI shore
        dsp->drawPixel(x, sy-2, dsp->color565(28, 88, 152));
      }
    }
    txtOutline("L.I.", 50, 53, C_LIME);

    // ── NORTH FORK — Riverhead → Greenport → Orient Point ──
    for(int x=88; x<102; x++){
      int sy = 37 - (x-88);
      if(sy < 0) sy = 0;
      dsp->drawPixel(x, sy,   sandC);
      dsp->drawPixel(x, sy+1, landC);
      dsp->drawPixel(x, sy+2, landC);
      dsp->drawPixel(x, sy+3, sandC);
    }
    fillRect(100, 22, 5, 4, landC);     // Orient Point tip
    dsp->drawPixel(105, 23, sandC);

    // ── SHELTER ISLAND ──
    fillRect(90, 30, 5, 4, landC);
    dsp->drawPixel(90, 29, sandC);
    dsp->drawPixel(94, 34, sandC);
    dsp->drawPixel(88, 31, sandC);
    dsp->drawPixel(95, 32, sandC);

    // ── GARDINERS ISLAND — small blob E of North Fork ──
    fillRect(98, 27, 3, 2, landC);
    dsp->drawPixel(101, 28, sandC);

    // ── SOUTH FORK — Hamptons → Montauk ──
    for(int x=88; x<114; x++){
      int sy;
      if(x<92)       sy = 38;
      else if(x<98)  sy = 36;
      else if(x<104) sy = 33;
      else if(x<110) sy = 30;
      else           sy = 28;
      int bot = sy + 6;
      if(bot > PANEL_HEIGHT) bot = PANEL_HEIGHT;
      fillRect(x, sy, 1, bot-sy, landC);
      dsp->drawPixel(x, sy-1, sandC);
      if((x*3+sy*5)%7 == 0) dsp->drawPixel(x, sy+1, landS);
    }
    fillRect(112, 27, 4, 3, landC);     // Montauk Point tip
    dsp->drawPixel(116, 28, sandC);
    txt1("MTK", 108, 23, C_WHITE);

    // ── FISHERS ISLAND — thin strip off SE CT ──
    for(int x=103; x<114; x++){
      dsp->drawPixel(x, 14, sandC);
      dsp->drawPixel(x, 15, landC);
    }
    dsp->drawPixel(108, 13, sandC);

    // ── BLOCK ISLAND — separate island far east ──
    fillRect(121, 17, 5, 6, landC);
    dsp->drawPixel(120, 18, sandC); dsp->drawPixel(126, 19, sandC);
    dsp->drawPixel(122, 23, sandC); dsp->drawPixel(125, 16, sandC);
    txt1("BI", 119, 11, C_WHITE);

    // ── DEPTH CONTOURS ── two faint dotted curves (30-ft nearer the CT
    // shore, 60-ft mid-sound) for a real navigation-chart feel.
    for(int x=14; x<108; x+=3){
      int dy = 26 + (int)(sin(x*0.18f) * 2);
      dsp->drawPixel(x, dy, depthC);
    }
    for(int x=16; x<104; x+=4){
      int dy = 19 + (int)(sin(x*0.22f + 1.3f) * 2);
      dsp->drawPixel(x, dy, dsp->color565(30, 85, 140));
    }

    // ── ENHANCED LAND DETAIL — town markers ──
    uint16_t labC = dsp->color565(255, 200, 80);
    auto town = [&](int x, int y, const char* lbl){
      dsp->drawPixel(x, y, dsp->color565(60,40,5));
      txt1(lbl, x-5, y-7, labC);
    };
    town( 23, 13, "ST");   // Stamford
    town( 44, 13, "BR");   // Bridgeport
    town( 60, 14, "NH");   // New Haven
    town( 99, 11, "NL");   // New London
    // Connecticut River — dark blue thread coming down from above frame
    uint16_t riverC = dsp->color565(20, 95, 165);
    for(int y=0; y<11; y++) dsp->drawPixel(80, y, riverC);
    // Long Island towns (subtle, label below shore)
    town( 25, 41, "OB");   // Oyster Bay
    town( 56, 39, "PJ");   // Port Jeff
    // NYC hint
    txtOutline("NYC", 0, 33, dsp->color565(220,200,255));

  } else {
    // === CAPE ANN / NORTH SHORE MASS — ZOOMED-OUT ===
    // Frame: lon -71.10°W → -70.30°W, lat 42.15°N → 42.85°N
    // Matches the commercial wood-map reference: Salisbury/Newburyport up top,
    // Plum Island barrier, Ipswich Bay, Cape Ann peninsula (Gloucester/Rockport),
    // Essex/Manchester/Beverly/Salem/Marblehead bottom-left.

    // === MAINLAND COAST — proper north-shore Mass coastline ===
    // Step-function with deep indents for the major harbors and bays so the
    // coastline reads like a real chart instead of a sin-wave blob.
    // Geography (reading top to bottom):
    //   • Merrimack River mouth (Salisbury / Newburyport)
    //   • Plum Is Sound entrance — water reaches deep inland
    //   • Ipswich town jut + Crane Beach barrier
    //   • Essex marshes (wide, complex)
    //   • Manchester-by-the-Sea + Magnolia
    //   • Salem Sound (deep U-shape, multiple harbors)
    //   • Marblehead neck (sticks out east as a peninsula)
    //   • Nahant peninsula bottom
    for(int y=0; y<PANEL_HEIGHT; y++){
      int rt;
      // Salisbury / Merrimack
      if(y<3)        rt = 48;
      else if(y<6)   rt = 40;                              // Merrimack mouth opens wide
      else if(y<10)  rt = 35;                              // Newburyport harbor mouth
      // Plum Is Sound — water cuts in DEEP behind Plum Is barrier
      else if(y<18)  rt = 16;                              // ← Plum Is Sound (deep)
      // Rowley / Ipswich
      else if(y<22)  rt = 22;
      else if(y<26)  rt = 30;                              // Ipswich town jut
      else if(y<28)  rt = 38;                              // Castle Neck peninsula
      // Essex Bay — deep marshy opening
      else if(y<32)  rt = 24;                              // ← Essex Bay (deep cut)
      else if(y<37)  rt = 50;                              // Essex marshes jut east
      // Manchester / Magnolia
      else if(y<42)  rt = 46;
      else if(y<46)  rt = 38;                              // ← Manchester Hbr cut
      // Beverly Hbr
      else if(y<50)  rt = 42;
      else if(y<53)  rt = 32;                              // ← Beverly Hbr cut
      // Salem Hbr — deep harbor
      else if(y<57)  rt = 30;                              // ← Salem Hbr (deep U)
      else if(y<60)  rt = 38;                              // Marblehead neck juts east
      else           rt = 34;                              // Nahant peninsula bottom
      if(rt > PANEL_WIDTH) rt = PANEL_WIDTH;
      // ── DE-BLOCKING PASS (same as LIS) ──
      // jitter breaks flat runs; ramp-clamp smooths cliff steps to ≤3px
      if(((y*7 + rt*5) % 5) == 0) rt += 1;
      if(((y*11 + rt*3) % 7) == 0 && rt > 2) rt -= 1;
      static int rtPrev;
      if(y == 0) rtPrev = rt;
      if(abs(rt - rtPrev) > 3) rt = rtPrev + (rt > rtPrev ? 3 : -3);
      rtPrev = rt;
      fillRect(0, y, rt, 1, landC);
      if(rt < PANEL_WIDTH) dsp->drawPixel(rt, y, sandC);
      // Shallow-water tint hugging the shore
      if(rt+1 < PANEL_WIDTH) dsp->drawPixel(rt+1, y, dsp->color565(28, 88, 152));
      // Land texture (lichen-rocky NE feel)
      if((y*7+rt)%5 == 0 && rt > 2) dsp->drawPixel(rt-2, y, landS);
    }

    // ── PLUM ISLAND — long thin barrier island, vertical orientation ──
    // 11-mile sandbar separating Plum Is Sound from Atlantic. Drawn as a
    // distinct yellow strip with sandy edges.
    for(int y=2; y<16; y++){
      dsp->drawPixel(44, y, sandC);
      dsp->drawPixel(45, y, landC);
      dsp->drawPixel(46, y, landC);
      dsp->drawPixel(47, y, sandC);
    }
    // Plum Is N tip
    dsp->drawPixel(45, 1, sandC);
    dsp->drawPixel(46, 1, sandC);
    // Plum Is S tip (Sandy Pt)
    fillRect(43, 14, 6, 2, landC);
    dsp->drawPixel(42, 15, sandC); dsp->drawPixel(49, 15, sandC);

    // ── CAPE ANN PENINSULA — sharper, more recognizable shape ──
    // Annisquam River cuts north at x=62-65, Gloucester Harbor cuts south
    // at x=68-72.  Pigeon Cove / Sandy Bay fills the NE corner.
    for(int y=14; y<32; y++){
      for(int x=54; x<88; x++){
        bool land = true;
        // Annisquam River channel (top-down, x=62-65, y<23)
        if(y<23 && x>=62 && x<=65) land = false;
        // Gloucester Harbor notch (south, x=68-72, y>22)
        if(y>=23 && x>=68 && x<=72){
          int depth = 30 - y;
          if(depth >= 0) land = false;
        }
        // Outer shape — clip corners for recognizable Cape Ann silhouette
        if(y<16 && x<58) land = false;
        if(y<15 && x<62) land = false;
        if(y<14 && x<70) land = false;
        if(y>29 && x<58) land = false;
        if(y>30 && x<62) land = false;
        // Eastern Pt indent (south of Gloucester Harbor)
        if(y>28 && x>=72 && x<=76) land = false;
        if(land) dsp->drawPixel(x, y, landC);
      }
    }
    // Cape Ann coastal rim — sand edges
    for(int y=16; y<30; y++){
      if(y<20 || y>24) dsp->drawPixel(54, y, sandC);
      dsp->drawPixel(87, y, sandC);
    }
    for(int x=58; x<86; x++){
      dsp->drawPixel(x, 14, sandC);
      dsp->drawPixel(x, 31, sandC);
    }
    // Internal texture
    for(int y=15; y<31; y+=3){
      for(int x=55; x<87; x+=4){
        if(((x*3+y*5)%7)==0) dsp->drawPixel(x, y, landS);
      }
    }

    // ── ROCKPORT — distinct NE bump (Halibut Pt + Sandy Bay) ──
    fillRect(82, 12, 6, 3, landC);
    fillRect(85, 14, 3, 2, landC);                          // Andrews/Pigeon Cove tip
    dsp->drawPixel(81, 13, sandC); dsp->drawPixel(88, 13, sandC);
    // Thacher Island twin lights — small offshore dot
    dsp->drawPixel(89, 19, landC);
    dsp->drawPixel(89, 20, landC);

    // ── MILK ISLAND / Salt Is — tiny offshore blobs east of Cape Ann ──
    dsp->drawPixel(85, 22, landC);
    dsp->drawPixel(82, 25, landC);

    // ── SALEM SOUND ISLANDS — Bakers Is, Misery Is, Tinkers Is ──
    // Visible east of the mainland coast in Salem Sound water
    fillRect(43, 33, 3, 2, landC);                          // Misery Is
    dsp->drawPixel(46, 32, sandC);
    fillRect(45, 38, 2, 2, landC);                          // Bakers Is
    dsp->drawPixel(40, 41, landC);                          // Tinkers Is

    // ── MARBLEHEAD NECK — distinctive peninsula juts east ──
    fillRect(38, 56, 8, 5, landC);
    dsp->drawPixel(37, 56, sandC); dsp->drawPixel(46, 60, sandC);
    // Marblehead Lt at the tip
    dsp->drawPixel(45, 58, sandC);

    // ── EGG ROCK / Nahant offshore ──
    dsp->drawPixel(31, 50, landC);
    dsp->drawPixel(28, 53, landC);

    // ── 60-FT DEPTH CONTOUR ── faint cyan curve in Atlantic + Ipswich Bay
    uint16_t depthC = dsp->color565(40, 110, 175);
    for(int x=50; x<128; x+=3){
      int dy;
      if(x<70)      dy = 38 + (int)(sin(x*0.20f)*1);     // Ipswich Bay band
      else if(x<92) dy = 28 + (int)(sin(x*0.18f)*2);     // E of Cape Ann
      else          dy = 50 + (int)(sin(x*0.15f)*2);     // Atlantic deep band
      if(dy >= 0 && dy < PANEL_HEIGHT) dsp->drawPixel(x, dy, depthC);
    }

    // === ENHANCED LAND DETAIL — Merrimack River + town markers ===
    // Merrimack River runs out at top-left through Newburyport into the
    // Atlantic. Drawn as a curved blue ribbon for orientation.
    uint16_t riverC = dsp->color565(20, 95, 165);
    int riverPath[][2] = {
      { 0, 6}, { 6, 6}, {12, 5}, {18, 4}, {25, 4}, {32, 4}, {38, 5}, {44, 5}
    };
    for(int i=0; i+1 < (int)(sizeof(riverPath)/sizeof(riverPath[0])); i++){
      dsp->drawLine(riverPath[i][0], riverPath[i][1],
                    riverPath[i+1][0], riverPath[i+1][1], riverC);
    }
    // Annisquam River — short blue thread cutting north into Cape Ann
    for(int y=14; y<22; y++) dsp->drawPixel(64, y, riverC);

    // Town dots + labels — placed offset from coast so labels fall into water
    // (blue background) where they're readable.
    uint16_t dotC = dsp->color565(60,40,5);
    uint16_t labC = dsp->color565(255,200,80);
    auto town = [&](int x, int y, const char* lbl, int lx, int ly){
      dsp->drawPixel(x, y, dotC);
      txt1(lbl, lx, ly, labC);
    };
    town( 38,  3, "Sal",  46,  1);   // Salisbury
    town( 38,  6, "NBPT", 50,  6);   // Newburyport
    town( 18, 22, "Ipsw",  2, 24);   // Ipswich
    town( 50, 30, "Esx",   2, 30);   // Essex
    town( 70, 25, "Glo",  74, 26);   // Gloucester (in harbor)
    town( 80, 18, "Rkpt", 86, 12);   // Rockport
    town( 35, 56, "Bvly",  2, 50);   // Beverly
    town( 32, 60, "Sal",  16, 60);   // Salem

    // Outer label on existing chart (kept)
    txtOutline("MASS",2,36,C_LIME);
    txt1("C.ANN",62,22,dsp->color565(255,255,150));
    txtOutline("ATLANTIC",90,38,C_CYAN);
    txt1("Ipsw Bay",4,17,C_CYAN);
    txt1("Plum I",30,8,dsp->color565(220,220,180));
  }

  // Tiny compass top-right corner
  int cx=124,cy=63;
  dsp->drawPixel(cx,cy-1,C_WHITE);
  dsp->drawPixel(cx,cy,C_WHITE);
  dsp->drawPixel(cx-1,cy,C_RED);
}

void drawBuoysOnly(const BuoyDef* buoys, int count){
  unsigned long now=millis();
  for(int i=0;i<count;i++){
    int period=max(buoys[i].onMs+buoys[i].offMs,2000);
    bool lit=((now%(unsigned long)period)<(unsigned long)buoys[i].onMs);
    int bx=buoys[i].px, by=buoys[i].py;
    // ALWAYS-BRIGHT 2x2 base square in the buoy's true color
    // (bug in v7: dim base was being computed as 5-bit values fed to color565
    //  which expects 8-bit, so colors came out near-black)
    dsp->drawPixel(bx,by,buoys[i].col);
    dsp->drawPixel(bx+1,by,buoys[i].col);
    dsp->drawPixel(bx,by+1,buoys[i].col);
    dsp->drawPixel(bx+1,by+1,buoys[i].col);
    if(lit){
      // FLASH: full color cross extending one pixel each direction
      dsp->drawPixel(bx-1,by,buoys[i].col);
      dsp->drawPixel(bx-1,by+1,buoys[i].col);
      dsp->drawPixel(bx+2,by,buoys[i].col);
      dsp->drawPixel(bx+2,by+1,buoys[i].col);
      dsp->drawPixel(bx,by-1,buoys[i].col);
      dsp->drawPixel(bx+1,by-1,buoys[i].col);
      dsp->drawPixel(bx,by+2,buoys[i].col);
      dsp->drawPixel(bx+1,by+2,buoys[i].col);
    } else {
      // OFF: erase the cross arms back to water color so flash blink is visible
      // Need to figure out what water color is at this y row
      uint16_t wc;
      if(by<22)      wc=dsp->color565(0,40,90);
      else if(by<44) wc=dsp->color565(0,75,145);
      else           wc=dsp->color565(20,115,180);
      dsp->drawPixel(bx-1,by,wc);
      dsp->drawPixel(bx-1,by+1,wc);
      dsp->drawPixel(bx+2,by,wc);
      dsp->drawPixel(bx+2,by+1,wc);
      dsp->drawPixel(bx,by-1,wc);
      dsp->drawPixel(bx+1,by-1,wc);
      dsp->drawPixel(bx,by+2,wc);
      dsp->drawPixel(bx+1,by+2,wc);
    }
  }
}

// Draw 1-pixel minor aids — simple on/off blink in the light's own color.
// When off, pixel is restored to the water gradient so the blink is visible.
void drawMiniAids(const MiniAid* aids, int count){
  unsigned long now=millis();
  for(int i=0;i<count;i++){
    int period=aids[i].onMs+aids[i].offMs;
    if(period<100) period=2000;
    bool lit=((now%(unsigned long)period)<(unsigned long)aids[i].onMs);
    int bx=aids[i].px, by=aids[i].py;
    if(bx<0||bx>=PANEL_WIDTH||by<0||by>=PANEL_HEIGHT) continue;
    if(lit){
      dsp->drawPixel(bx,by,aids[i].col);
    } else {
      uint16_t wc;
      if(by<22)      wc=dsp->color565(0,40,90);
      else if(by<44) wc=dsp->color565(0,75,145);
      else           wc=dsp->color565(20,115,180);
      dsp->drawPixel(bx,by,wc);
    }
  }
}

void renderLISChart(){
  static unsigned long lastBGDraw=0;
  if(!lisChartDrawn || millis()-lastBGDraw>30000){
    drawChartBackground(true);
    lisChartDrawn=true; lastBGDraw=millis();
  }
  // Only update buoys (no cls - avoids strobing)
  drawBuoysOnly(LIS_BUOYS,LIS_COUNT);
  drawMiniAids(LIS_AIDS,LIS_AIDS_COUNT);
}

void renderIpswichChart(){
  static unsigned long lastBGDraw=0;
  if(!ipwChartDrawn || millis()-lastBGDraw>30000){
    drawChartBackground(false);
    ipwChartDrawn=true; lastBGDraw=millis();
  }
  drawBuoysOnly(IPW_BUOYS,IPW_COUNT);
  drawMiniAids(IPW_AIDS,IPW_AIDS_COUNT);
}

// ----------------------------------------------------------
// Night mode scenes
void renderAurora(){
  cls();
  auroraPhase+=0.02f;
  // Dark sky
  fillRect(0,0,PANEL_WIDTH,PANEL_HEIGHT,dsp->color565(1,3,8));
  drawStars(25,60);

  // Aurora curtains - multiple color bands
  for(int x=0;x<PANEL_WIDTH;x++){
    float wave1=sin(x*0.04f+auroraPhase)*12+sin(x*0.08f+auroraPhase*0.7f)*8;
    float wave2=sin(x*0.05f+auroraPhase*1.2f)*10+sin(x*0.09f+auroraPhase*0.5f)*6;
    float wave3=sin(x*0.03f+auroraPhase*0.8f)*14+sin(x*0.07f+auroraPhase*1.1f)*7;

    int y1=(int)(14+wave1), y2=(int)(22+wave2), y3=(int)(30+wave3);
    y1=constrain(y1,2,50); y2=constrain(y2,8,55); y3=constrain(y3,14,58);

    // Green aurora
    for(int y=y1;y<y1+6;y++){
      uint8_t alpha=255-(abs(y-y1-3)*40);
      dsp->drawPixel(x,y,dsp->color565(0,alpha>>2,0));
    }
    // Purple/blue accent
    for(int y=y2;y<y2+4;y++){
      uint8_t alpha=200-(abs(y-y2-2)*50);
      dsp->drawPixel(x,y,dsp->color565(alpha>>4,0,alpha>>3));
    }
    // Teal shimmer
    for(int y=y3;y<y3+3;y++){
      dsp->drawPixel(x,y,dsp->color565(0,map(y,y3,y3+3,30,0),map(y,y3,y3+3,40,0)));
    }
  }

  // Time in corner
  struct tm ti; if(getLocalTime(&ti)){
    char tbuf[6]; strftime(tbuf,6,"%I:%M",&ti);
    if(tbuf[0]=='0') memmove(tbuf,tbuf+1,sizeof(tbuf)-1);
    dsp->setTextColor(dsp->color565(0,40,0)); setTextS(1);
    dsp->setCursor(0,56); dsp->print(tbuf);
  }
}

// ----------------------------------------------------------
// Pulled out of renderStarfield — never call HTTPClient inside a render
// function (blocks the matrix DMA loop for the duration of the request,
// causing visible freezes / watchdog risk).
void fetchISS(){
  String body=httpGet("https://api.wheretheiss.at/v1/satellites/25544",6000);
  if(body.isEmpty()) return;
  DynamicJsonDocument doc(2048);
  if(deserializeJson(doc,body)) return;
  issLat=doc["latitude"].as<float>();
  issLon=doc["longitude"].as<float>();
  issValid=true;
}

void renderStarfield(){
  // STROBE FIX: the cls() used to run here EVERY frame (~70fps) but the
  // rate-limit return below only lets the starfield redraw every 200ms.
  // Result: screen was cleared to black ~14× for every 1 frame of stars →
  // violent strobing. The fix is to gate FIRST, then clear+draw together so
  // black and stars are always painted in the same frame.
  if(millis()-lastStaticDraw < 200) return;
  lastStaticDraw=millis();

  fillRect(0,0,PANEL_WIDTH,PANEL_HEIGHT,C_BLACK);

  // Rich star field — deterministic LCG so positions are STABLE across frames.
  // Earlier code mixed `random()` with the LCG which caused the milky way band
  // to flicker chaotically. Pure LCG = same starfield every frame.
  uint32_t seed=42;
  auto rnd=[&seed]()->uint32_t{seed=seed*1664525+1013904223;return seed;};
  for(int i=0;i<80;i++){
    int x=rnd()%128, y=rnd()%64;
    uint8_t b=40+rnd()%140;
    bool twinkle=((millis()/300+i)%5==0);
    dsp->drawPixel(x,y,dsp->color565(b,b,b+(twinkle?50:0)));
  }

  // Milky Way band — also LCG-deterministic so it doesn't flicker
  for(int i=0;i<60;i++){
    int x=20 + (rnd()%88), y=15 + (rnd()%33);
    if(abs(x-y-5)<20) dsp->drawPixel(x,y,dsp->color565(15,15,25));
  }

  // Moon phase small top-right
  drawMoonFull(116,8,6,getMoonPhase());
  txt1(moonPhaseShort(getMoonPhase()),108,16,dsp->color565(80,75,50));

  // ISS position
  if(issValid){
    // Check if overhead (within ~500mi of Stamford)
    float dlat=issLat-HOME_LAT, dlon=issLon-HOME_LON;
    float dist=sqrt(dlat*dlat+dlon*dlon);
    bool overhead=(dist<8.0f); // ~8 degrees = ~550mi

    // Map ISS world position to screen
    int ix=(int)((issLon+180)/360.0f*128);
    int iy=(int)((90-issLat)/180.0f*64);
    ix=constrain(ix,2,125); iy=constrain(iy,2,61);

    if(overhead){
      // Flash ISS
      if((millis()/200)%2){
        dsp->fillCircle(ix,iy,3,C_LIME);
        txt1("ISS OVERHEAD!",2,56,C_LIME);
      }
    } else {
      dsp->drawCircle(ix,iy,2,C_YELLOW);
      dsp->drawPixel(ix,iy,C_YELLOW);
    }
    txt1("ISS",ix+4,iy-4,dsp->color565(150,150,0));
  }

  // Time
  struct tm ti; if(getLocalTime(&ti)){
    char tbuf[6]; strftime(tbuf,6,"%I:%M",&ti);
    if(tbuf[0]=='0') memmove(tbuf,tbuf+1,sizeof(tbuf)-1);
    txt1(tbuf,0,56,dsp->color565(30,30,60));
  }
}

// ----------------------------------------------------------
// SPACE — night card cycling 3 sub-views every ~20s:
//   [0] Constellations  – Orion, Big Dipper, Cassiopeia, Cygnus, lines + names
//   [1] Solar System    – sun + planets w/ orbits (Saturn rings)
//   [2] Galaxy / Deep   – animated spiral (M31 Andromeda flavor)
// Each sub-view draws its own background, no shared `cls()` strobing.
// ----------------------------------------------------------

// 80 deterministic background stars (positions only, no flicker)
static const uint8_t SPACE_STAR_X[80] = {
   3,  9, 17, 23, 31, 38, 47, 55, 63, 71, 79, 87, 94,103,111,119,
   2, 12, 20, 27, 35, 42, 51, 58, 66, 75, 83, 91, 99,107,115,124,
   6, 14, 25, 33, 41, 49, 60, 68, 77, 85, 96,108,116,125,  4, 18,
  29, 39, 53, 64, 73, 82, 90,100,109,118,127, 11, 22, 36, 48, 70,
  88, 95,112,121,  8, 16, 28, 45, 56, 78, 92,105, 26, 50, 76,113
};
static const uint8_t SPACE_STAR_Y[80] = {
   2,  5,  3,  6,  4,  7,  2,  6,  3,  5,  4,  7,  2,  4,  6,  3,
  10, 12, 14, 11, 13, 15, 10, 14, 12, 15, 11, 13, 10, 14, 12, 15,
  18, 21, 19, 23, 20, 17, 22, 19, 21, 18, 22, 20, 19, 23, 28, 26,
  29, 27, 25, 28, 30, 26, 29, 27, 25, 30, 28, 32, 36, 33, 38, 35,
  40, 42, 39, 44, 47, 50, 49, 52, 51, 55, 53, 56, 60, 58, 61, 59
};

void drawDeepStars(int yMax){
  for(int i=0;i<80;i++){
    if(SPACE_STAR_Y[i]>yMax) continue;
    // 4 brightness tiers chosen by index hash, twinkle on slow timer
    uint8_t tier = (SPACE_STAR_X[i] + SPACE_STAR_Y[i]) % 4;
    uint8_t base = tier==0?180:tier==1?120:tier==2?70:40;
    bool twinkle = ((millis()/420 + i*7) % 11 == 0);
    if(twinkle) base = base>180?255:base+60;
    // Slight blue tint on brightest stars
    uint8_t bb = base>120?(base+15):base;
    if(bb>255) bb=255;
    dsp->drawPixel(SPACE_STAR_X[i], SPACE_STAR_Y[i], dsp->color565(base,base,bb));
  }
}

// Connect a sequence of (x,y) pixel pairs with thin lines for constellations.
void drawStellarLines(const int8_t* xs, const int8_t* ys, int n, uint16_t col){
  for(int i=0;i<n-1;i++){
    if(xs[i]<0||xs[i+1]<0) continue;   // -1 sentinel = pen up
    dsp->drawLine(xs[i], ys[i], xs[i+1], ys[i+1], col);
  }
}

// Draw a named star: 1px bright + 4 dim cross-arms for a "twinkle" look.
void drawNamedStar(int x, int y, uint16_t col){
  uint16_t dim = dsp->color565(70,70,90);
  dsp->drawPixel(x,y,col);
  dsp->drawPixel(x-1,y,dim); dsp->drawPixel(x+1,y,dim);
  dsp->drawPixel(x,y-1,dim); dsp->drawPixel(x,y+1,dim);
}

void renderSpace(){
  // Sub-view rotation
  unsigned long sinceSlide = millis() - slideStart;
  int subView = (sinceSlide / 20000UL) % 3;

  // 4-second "ramp" between sub-views — only redraw on view change or every 250ms
  static int lastSubView = -1;
  static unsigned long lastDraw = 0;
  bool viewChanged = (subView != lastSubView);
  if(viewChanged){ lastStaticDraw = 0; lastSubView = subView; }
  if(!viewChanged && millis()-lastDraw < 250) return;
  lastDraw = millis();

  cls();
  fillRect(0,0,PANEL_WIDTH,PANEL_HEIGHT,dsp->color565(2,2,12));   // deep space

  if(subView == 0){
    // ===== CONSTELLATIONS =====
    drawDeepStars(64);

    uint16_t lineC = dsp->color565(40,55,90);
    uint16_t starHi = dsp->color565(220,225,255);
    uint16_t starWarm = dsp->color565(255,210,160);   // Betelgeuse-ish
    uint16_t labelC = dsp->color565(110,140,200);

    // ── Big Dipper (top-left), 7 stars ──
    static const int8_t bdX[] = { 4, 9, 14, 18, 22, 27, 32 };
    static const int8_t bdY[] = { 6, 4,  6,  8,  6,  4,  3 };
    drawStellarLines(bdX, bdY, 7, lineC);
    for(int i=0;i<7;i++) drawNamedStar(bdX[i], bdY[i], starHi);
    txt1("BIG DIPPER", 1, 12, labelC);

    // ── Cassiopeia (top-right) — W shape, 5 stars ──
    static const int8_t cX[] = { 76, 84, 92,100,108 };
    static const int8_t cY[] = {  3,  9,  4,  9,  3 };
    drawStellarLines(cX, cY, 5, lineC);
    for(int i=0;i<5;i++) drawNamedStar(cX[i], cY[i], starHi);
    txt1("CASSIOPEIA", 70, 14, labelC);

    // ── Orion (mid-left) — belt + corners ──
    // Hourglass pattern with 3-star belt
    static const int8_t orX[] = {  6, 14, 22, 14, 14,  6, 22, -1 };
    static const int8_t orY[] = { 28, 30, 28, 34, 34, 42, 42, -1 };
    // explicit 3-star belt line
    static const int8_t obX[] = { 10, 14, 18 };
    static const int8_t obY[] = { 34, 34, 34 };
    drawStellarLines(orX, orY, 7, lineC);
    drawStellarLines(obX, obY, 3, dsp->color565(60,75,110));
    drawNamedStar(6,28,starWarm);     // Betelgeuse (red)
    drawNamedStar(22,28,starHi);
    drawNamedStar(6,42,starHi);
    drawNamedStar(22,42,starHi);
    drawNamedStar(10,34,starHi); drawNamedStar(14,34,starHi); drawNamedStar(18,34,starHi);
    txt1("ORION", 4, 46, labelC);

    // ── Cygnus / Northern Cross (mid-right) ──
    static const int8_t cgX[] = { 74, 88,102, 88, 88, -1, 80, 96 };
    static const int8_t cgY[] = { 30, 34, 30, 26, 42, -1, 26, 26 };
    drawStellarLines(cgX, cgY, 8, lineC);
    drawNamedStar(74,30,starHi); drawNamedStar(88,34,starHi); drawNamedStar(102,30,starHi);
    drawNamedStar(88,26,starHi); drawNamedStar(88,42,starHi);
    txt1("CYGNUS", 78, 46, labelC);

    // ── Footer ──
    fillRect(0,55,PANEL_WIDTH,9,dsp->color565(0,0,8));
    dsp->drawFastHLine(0,55,PANEL_WIDTH,dsp->color565(20,30,60));
    txt1("NIGHT SKY",2,57,dsp->color565(120,130,180));
    char tbuf[6]; struct tm ti; if(getLocalTime(&ti)){
      strftime(tbuf,6,"%I:%M",&ti);
      if(tbuf[0]=='0') memmove(tbuf,tbuf+1,sizeof(tbuf)-1);
      int tw=strlen(tbuf)*6;
      txt1(tbuf, PANEL_WIDTH-tw-2, 57, dsp->color565(180,180,220));
    }

  } else if(subView == 1){
    // ===== SOLAR SYSTEM =====
    drawDeepStars(64);

    int sunX=64, sunY=32;
    // Sun corona — pulsing
    int corona = 9 + (int)(sin(millis()*0.004f)*1.5f);
    for(int r=corona+2; r>=corona; r--){
      uint16_t c = dsp->color565(40+(corona+2-r)*15, 30+(corona+2-r)*10, 0);
      dsp->drawCircle(sunX,sunY,r,c);
    }
    // Sun core
    dsp->fillCircle(sunX,sunY,5,dsp->color565(255,200,30));
    dsp->fillCircle(sunX,sunY,3,dsp->color565(255,255,180));

    // Orbital rings (very faint)
    uint16_t orbitC = dsp->color565(15,18,40);
    int orbits[] = { 11, 16, 22, 28, 38, 50 };
    for(int i=0;i<6;i++) dsp->drawCircle(sunX,sunY,orbits[i],orbitC);

    // Planet positions — angle drifts very slowly with millis
    float t = millis()*0.0001f;
    struct PD { int orbit; float speed; uint16_t col; const char* name; };
    PD planets[] = {
      { 11, 4.15f, dsp->color565(160,140,120), "Me" },  // Mercury
      { 16, 1.62f, dsp->color565(230,180,80),  "V"  },  // Venus
      { 22, 1.00f, dsp->color565(80,130,255),  "E"  },  // Earth
      { 28, 0.53f, dsp->color565(220,80,40),   "M"  },  // Mars
      { 38, 0.08f, dsp->color565(220,180,140), "J"  },  // Jupiter
      { 50, 0.034f,dsp->color565(230,210,140), "S"  },  // Saturn
    };
    for(int i=0;i<6;i++){
      float a = t * planets[i].speed + i*1.7f;
      int px = sunX + (int)(planets[i].orbit*cos(a));
      int py = sunY + (int)(planets[i].orbit*sin(a)*0.7f);  // slight ellipse
      if(i==5){
        // SATURN: planet + rings
        // Rings (ellipse) drawn first behind
        for(int dx=-4; dx<=4; dx++){
          int ry = (int)(0.4f * dx);   // tilted ring
          dsp->drawPixel(px+dx, py+ry, dsp->color565(180,165,110));
        }
        dsp->fillCircle(px,py,2,planets[i].col);
        // Brighten ring outer tips
        dsp->drawPixel(px-4,py-1,dsp->color565(220,200,140));
        dsp->drawPixel(px+4,py+1,dsp->color565(220,200,140));
      } else if(i==4){
        dsp->fillCircle(px,py,2,planets[i].col);   // Jupiter (large)
      } else {
        dsp->drawPixel(px,py,planets[i].col);
        dsp->drawPixel(px+1,py,planets[i].col);
        dsp->drawPixel(px,py+1,planets[i].col);
      }
    }

    // Header
    fillRect(0,0,PANEL_WIDTH,9,dsp->color565(8,4,20));
    txt1("SOLAR SYSTEM",2,1,dsp->color565(180,160,220));
    char tbuf[6]; struct tm ti; if(getLocalTime(&ti)){
      strftime(tbuf,6,"%I:%M",&ti);
      if(tbuf[0]=='0') memmove(tbuf,tbuf+1,sizeof(tbuf)-1);
      int tw=strlen(tbuf)*6;
      txt1(tbuf, PANEL_WIDTH-tw-2, 1, dsp->color565(180,180,220));
    }

  } else {
    // ===== GALAXY (M31 Andromeda flavor) =====
    // Nebula glow background
    for(int y=0;y<64;y++){
      for(int x=0;x<128;x++){
        // distance from galactic center
        int dx = x-64, dy = y-32;
        int d2 = dx*dx + dy*dy;
        if(d2 < 50){
          dsp->drawPixel(x,y,dsp->color565(30,15,40));
        } else if(d2 < 350){
          if(((x*7+y*13)%9)==0) dsp->drawPixel(x,y,dsp->color565(15,8,25));
        }
      }
    }

    drawDeepStars(64);

    // Galactic core — bright yellow-white bulge
    int gx=64, gy=32;
    dsp->fillCircle(gx,gy,4,dsp->color565(255,240,180));
    dsp->fillCircle(gx,gy,3,dsp->color565(255,255,220));
    dsp->drawPixel(gx,gy,C_WHITE);

    // Spiral arms — animated rotation
    float rot = millis() * 0.0006f;
    for(int arm=0; arm<2; arm++){
      float armOffset = arm * PI;
      for(int step=0; step<60; step++){
        float r = 5 + step*0.65f;
        if(r > 38) break;
        // Logarithmic spiral
        float a = armOffset + rot + step*0.18f;
        int sx = gx + (int)(r*cos(a));
        int sy = gy + (int)(r*sin(a)*0.55f);  // squashed for face-on look
        if(sx<0||sx>=PANEL_WIDTH||sy<0||sy>=PANEL_HEIGHT) continue;

        // Brightness fades along the arm
        int b = 220 - step*3;
        if(b<40) b=40;
        // Color shifts blue→pink along arm
        uint8_t rC = (step<20)?(180-step*4):(80+step);
        uint8_t gC = (step<20)?(160-step*5):(60+step/2);
        uint8_t bC = (step<20)?(220-step*2):(180-step);
        dsp->drawPixel(sx,sy,dsp->color565(rC,gC,bC));
        // Some arm "clumps" (HII regions) every ~6th step
        if(step%6==0 && step>5){
          dsp->drawPixel(sx+1,sy,dsp->color565(rC,gC,bC));
          dsp->drawPixel(sx,sy+1,dsp->color565(rC>>1,gC>>1,bC));
        }
      }
    }

    // Header
    fillRect(0,0,PANEL_WIDTH,9,dsp->color565(8,4,18));
    txt1("M31 ANDROMEDA",2,1,dsp->color565(190,165,230));
    txt1("2.5 Mly",PANEL_WIDTH-7*6-2,1,dsp->color565(120,110,180));

    // Footer ticker
    static int tickerOff = 0;
    if((millis()/80)%2==0) tickerOff++;
    fillRect(0,56,PANEL_WIDTH,8,dsp->color565(0,0,8));
    const char* tickerMsg = "  THE FARTHEST OBJECT VISIBLE WITHOUT A TELESCOPE  ";
    int msgLen = strlen(tickerMsg);
    int totalW = msgLen * 6;
    int xOff = -((tickerOff/2) % totalW);
    for(int i=0;i<msgLen;i++){
      int charX = xOff + i*6;
      if(charX > -6 && charX < PANEL_WIDTH){
        char one[2]={tickerMsg[i],0};
        txt1(one, charX, 57, dsp->color565(140,120,180));
      }
    }
  }

  // Sub-view dot indicators (top-center, 3 small dots)
  for(int d=0; d<3; d++){
    uint16_t dc = (d==subView) ? dsp->color565(200,180,240) : dsp->color565(40,30,60);
    dsp->drawPixel(60+d*4, 11, dc);
    dsp->drawPixel(61+d*4, 11, dc);
  }
}

// =====================================================================
// SPLASH SCREEN
// =====================================================================
void spawnFirework(int x, int y, uint16_t col){
  int base=0;
  for(int i=0;i<MAX_FW;i++) if(!fw[i].active){base=i;break;}
  for(int i=0;i<8;i++){
    if(base+i>=MAX_FW) break;
    float ang=i*PI/4.0f;
    fw[base+i].x=x; fw[base+i].y=y;
    fw[base+i].vx=cos(ang)*2.5f; fw[base+i].vy=sin(ang)*2.5f;
    fw[base+i].col=col; fw[base+i].active=true; fw[base+i].life=25+random(0,15);
  }
}

// Firework burst for splash
struct SplashFW { float x,y,vx,vy; int life; uint16_t col; bool active; };
static SplashFW sfw[30];

void burstFW(int cx, int cy, uint16_t col){
  int base=0;
  for(int i=0;i<30;i++) if(!sfw[i].active){base=i;break;}
  for(int i=0;i<10;i++){
    if(base+i>=30) break;
    float ang=(float)i*36.0f*PI/180.0f;
    sfw[base+i]={
      (float)cx,(float)cy,
      cos(ang)*2.2f, sin(ang)*2.2f,
      20+random(0,12),col,true
    };
  }
}

// ----------------------------------------------------------
// SPLASH — clean technical boot diagnostic.
// Layout (128×64):
//   y=0-9   : Title bar (navy)  — "DEWS FEED v9"  …  boot phase indicator
//   y=11    : Cyan separator
//   y=13-19 : SYS  | ESP32-32E   240MHz / 290K     [OK]
//   y=21-27 : NET  | RedFoxRoad  -54dBm  192.168.x [OK]
//   y=29-35 : TIME | pool.ntp    14:32 EDT          [OK]
//   y=37-43 : OTA  | dews-feed   ready              [OK]
//   y=45-51 : DATA | <stage> N/8                   ____
//   y=53    : Cyan separator
//   y=55-63 : Footer — current task verbose msg + small spinner
// All status badges color-coded: amber WAIT / green OK / red FAIL.
// No fireworks, no celebration — pure systems telemetry.
// ----------------------------------------------------------

// Color palette for the splash
static const uint16_t SPL_NAVY  = 0x0008;   // very dark navy bg
static const uint16_t SPL_BAR   = 0x0011;   // header bar navy
static const uint16_t SPL_CYAN  = 0x05BF;   // accent cyan
static const uint16_t SPL_GRID  = 0x18C3;   // faint grid color
static const uint16_t SPL_LBL   = 0x4A6F;   // dim label text
static const uint16_t SPL_VAL   = 0xC618;   // bright value text
static const uint16_t SPL_OK    = 0x07E6;   // green OK
static const uint16_t SPL_WAIT  = 0xFD60;   // amber/yellow waiting
static const uint16_t SPL_FAIL  = 0xF800;   // red fail

// Draw a status row at given y. label="SYS", val="<details>".
// State indicator is a 3px colored DOT (no text overflow risk).
//   0=pending(dim gray)  1=active(amber)  2=ok(green)  3=fail(red)
void splashRow(int y, const char* label, const char* val, uint8_t state){
  // Wipe row (safe rect — entirely on-panel)
  fillRect(0, y, PANEL_WIDTH, 7, SPL_NAVY);
  // Label column (x=2)
  txt1(label, 2, y, state==0 ? SPL_GRID : SPL_LBL);
  // Faint divider at x=24
  dsp->drawFastVLine(24, y, 7, SPL_GRID);
  // Value column — clamp to 14 chars max (84px) so we end well before x=120
  char vbuf[15]; strncpy(vbuf, val, 14); vbuf[14] = 0;
  uint16_t valC = state==3 ? dsp->color565(255,80,80)
                : state==0 ? SPL_GRID
                : SPL_VAL;
  txt1(vbuf, 28, y, valC);
  // Status dot at right (x=121, fully on-panel; 3px circle)
  uint16_t dotC = state==0 ? SPL_GRID
                : state==1 ? SPL_WAIT
                : state==2 ? SPL_OK
                :            SPL_FAIL;
  dsp->fillCircle(122, y+3, 2, dotC);
}

// Footer status line — single message, no spinner clutter.
void splashFooter(const char* msg){
  fillRect(0, 55, PANEL_WIDTH, 9, SPL_NAVY);
  dsp->drawFastHLine(0, 53, PANEL_WIDTH, SPL_CYAN);
  // Truncate to 20 chars (120px) so we never overrun
  char buf[21]; strncpy(buf, msg, 20); buf[20] = 0;
  txt1(buf, 4, 56, SPL_VAL);
}

// Big "DEWS FEED" reveal animation that runs after all loading completes.
// Wipes the diagnostic chrome and presents the title centered + size-3, with
// a thin gold underline. ~2 seconds, full-screen, then handoff to first card.
void splashReveal(){
  // Dim brightness for a softer reveal, restore after
  if(matrix) matrix->setBrightness8(60);

  // Fade-from-black wipe (8 steps) — cheap on ESP32 because we just redraw
  for(int s=0; s<6; s++){
    fillRect(0, 0, PANEL_WIDTH, PANEL_HEIGHT, SPL_NAVY);
    // Vignette dots that contract toward center as `s` rises
    int vignette = 10 - s;
    for(int x=0; x<PANEL_WIDTH; x+=4){
      dsp->drawPixel(x, vignette, dsp->color565(20,20,40));
      dsp->drawPixel(x, PANEL_HEIGHT-1-vignette, dsp->color565(20,20,40));
    }
    delay(40);
  }

  // Final clean dark backdrop
  cls();
  fillRect(0, 0, PANEL_WIDTH, PANEL_HEIGHT, SPL_NAVY);

  // ── Big "DEWS FEED" title centered, size 3 ──
  // size-3 char = 18px wide, 24 tall. "DEWS FEED" = 9 chars w/ space = 162px (won't fit at size 3!)
  // Use TWO LINES at size 3:
  //   "DEWS"  → 4 chars * 18 = 72px, x = (128-72)/2 = 28
  //   "FEED"  → 4 chars * 18 = 72px, x = 28
  // y= 6  for "DEWS" (size-3 height 24, ends y=29)
  // y= 32 for "FEED" (ends y=55)
  ctrTxt3("DEWS", 6,  C_GOLD);
  ctrTxt3("FEED", 32, C_GOLD);

  // Thin gold underline + version (from the define — no more stale strings)
  dsp->drawFastHLine(20, 58, PANEL_WIDTH-40, dsp->color565(120,90,0));
  txt1(DEWS_FEED_VERSION,  2, 58, dsp->color565(80,70,30));
  txt1("READY", PANEL_WIDTH-5*6-2, 58, SPL_OK);

  // Hold the big DEWS FEED title for 4 seconds — gives the eye time to
  // settle on the brand reveal before the news card replaces it.
  delay(4000);

  // Restore working brightness
  if(matrix) matrix->setBrightness8(BRIGHTNESS_DAY);
  cls();
}

void showSplash(){
  // Soften brightness during boot so we don't peg the PSU and to make the
  // reveal at the end feel brighter by contrast.
  if(matrix) matrix->setBrightness8(45);

  // ── Background + chrome ──
  cls();
  fillRect(0, 0, PANEL_WIDTH, PANEL_HEIGHT, SPL_NAVY);

  // Title bar — note: "BOOT" badge is short enough to live entirely on-panel
  fillRect(0, 0, PANEL_WIDTH, 10, SPL_BAR);
  txt1("DEWS FEED " DEWS_FEED_VERSION, 2, 1, SPL_VAL);
  txt1("boot", PANEL_WIDTH - 4*6 - 2, 1, SPL_CYAN);
  dsp->drawFastHLine(0, 11, PANEL_WIDTH, SPL_CYAN);

  // Pre-render all rows in pending state (gray dots)
  splashRow(13, "SYS",  "ESP32-32E",     0);
  splashRow(21, "NET",  "scanning",      0);
  splashRow(29, "TIME", "ntp pending",   0);
  splashRow(37, "OTA",  "-",             0);
  splashRow(45, "DATA", "-",             0);
  dsp->drawFastHLine(0, 53, PANEL_WIDTH, SPL_CYAN);
  splashFooter("init");

  // STEP 1: SYS — chip + heap (instant)
  char sysS[16];
  snprintf(sysS, 16, "%dMHz %dK",
    (int)getCpuFrequencyMhz(), (int)(ESP.getFreeHeap()/1024));
  splashRow(13, "SYS", sysS, 2);
  delay(120);

  // STEP 2: NET — WiFi
  splashRow(21, "NET", WIFI_SSID, 1);
  splashFooter("wifi associating");
  connectWiFi();
  if(WiFi.status() == WL_CONNECTED){
    char netS[16];
    snprintf(netS, 16, "%ddBm ok", (int)WiFi.RSSI());
    splashRow(21, "NET", netS, 2);
    splashFooter("link up");
    delay(150);
  } else {
    splashRow(21, "NET", "no signal", 3);
    splashRow(29, "TIME", "no net",   3);
    splashRow(37, "OTA",  "skipped",  3);
    splashRow(45, "DATA", "skipped",  3);
    splashFooter("offline mode");
    // Start TZ + SNTP even with no link: ensureWifi() keeps retrying in
    // loop(), and the background SNTP client syncs the moment a link
    // appears. Without this an offline boot free-runs from the 1970 epoch
    // and the clock shows garbage (e.g. "11:28 PM") until a manual reboot.
    configTzTime("EST5EDT,M3.2.0/2,M11.1.0/2", "pool.ntp.org", "time.nist.gov");
    delay(2000);
    splashReveal();   // still show big DEWS FEED so it boots cleanly
    return;
  }

  // STEP 3: TIME — NTP
  splashRow(29, "TIME", "syncing", 1);
  splashFooter("ntp sync");
  syncTime();
  struct tm ti;
  if(getLocalTime(&ti)){
    char timeS[16];
    strftime(timeS, 16, "%H:%M", &ti);
    splashRow(29, "TIME", timeS, 2);
  } else {
    splashRow(29, "TIME", "ntp failed", 3);
  }
  delay(120);

  // STEP 4: OTA — register over-the-air
  splashRow(37, "OTA", "registering", 1);
  splashFooter("ota init");
  setupOTA();
  IPAddress ip = WiFi.localIP();
  char otaS[16];
  snprintf(otaS, 16, ".%d ready", ip[3]);
  splashRow(37, "OTA", otaS, 2);
  delay(120);

  // STEP 5: DATA — sequential fetches
  const char* stages[] = {
    "news", "local", "scores", "finance", "weather", "marine", "flights"
  };
  const int N = 7;
  for(int i=0; i<N; i++){
    char dS[16];
    snprintf(dS, 16, "%s %d/%d", stages[i], i+1, N);
    splashRow(45, "DATA", dS, 1);
    splashFooter(stages[i]);
    // Inline progress bar in the row 51 — safely within panel
    int barW = (int)((92.0f) * (float)i / N);    // 92 = 120-28
    fillRect(28, 51, 92, 1, SPL_GRID);
    fillRect(28, 51, barW, 1, SPL_OK);

    switch(i){
      case 0: fetchNews();      break;
      case 1: fetchLocalNews(); break;
      case 2: fetchScores();    break;
      case 3: fetchFinance();   break;
      case 4: fetchWeather(HOME_LAT, HOME_LON, homeWx);
              fetchWeather(CABIN_LAT, CABIN_LON, cabinWx); break;
      case 5: fetchMarine();    break;
      case 6: fetchFlights();   break;
    }
  }
  // Final DATA row
  char dF[16]; snprintf(dF, 16, "%d feeds ok", N);
  splashRow(45, "DATA", dF, 2);
  fillRect(28, 51, 92, 1, SPL_OK);

  // Hold the diagnostic for a beat so the user can see all green dots
  splashFooter("ready");
  delay(700);

  // BIG TITLE REVEAL
  splashReveal();
}


void initDisplay(){
  HUB75_I2S_CFG cfg(64,32,PANELS_NUMBER);
  cfg.gpio.r1=PIN_R1;cfg.gpio.g1=PIN_G1;cfg.gpio.b1=PIN_B1;
  cfg.gpio.r2=PIN_R2;cfg.gpio.g2=PIN_G2;cfg.gpio.b2=PIN_B2;
  cfg.gpio.a=PIN_A;cfg.gpio.b=PIN_B;cfg.gpio.c=PIN_C;
  cfg.gpio.d=PIN_D;cfg.gpio.e=PIN_E;
  cfg.gpio.clk=PIN_CLK;cfg.gpio.lat=PIN_LAT;cfg.gpio.oe=PIN_OE;
  cfg.clkphase=true;
  matrix=new MatrixPanel_I2S_DMA(cfg);
  matrix->begin(); matrix->setBrightness8(BRIGHTNESS_DAY); matrix->clearScreen();
  dsp=new VirtualMatrixPanel(*matrix,2,2,64,32,CHAIN_TOP_RIGHT_DOWN);
  C_BLACK  =dsp->color565(0,0,0);      C_WHITE =dsp->color565(255,255,255);
  C_GRAY   =dsp->color565(120,120,120);C_DARKGRAY=dsp->color565(40,40,40);
  C_RED    =dsp->color565(255,30,30);  C_GREEN =dsp->color565(30,200,30);
  C_BLUE   =dsp->color565(30,100,255); C_YELLOW=dsp->color565(255,230,0);
  C_CYAN   =dsp->color565(0,220,220);  C_TEAL  =dsp->color565(0,180,150);
  C_LIME   =dsp->color565(130,255,60); C_GOLD  =dsp->color565(255,185,0);
  C_ORANGE =dsp->color565(255,140,0);  C_PINK  =dsp->color565(255,80,160);
  C_PURPLE =dsp->color565(180,60,255); C_BROWN =dsp->color565(110,60,20);
  C_NAVY   =dsp->color565(10,30,90);   C_AMBER =dsp->color565(255,170,0);
  C_MAROON =dsp->color565(140,0,0);    C_OLIVE =dsp->color565(100,100,0);
}

// =====================================================================
// TODEW CARD — most urgent open tasks from the ToDew app, priority dot on
// the left (red=urgent amber=high gray=normal), due chip on the right.
// Static card: standard 2s redraw gate.
// =====================================================================
void renderToDo(){
  if(millis() - lastStaticDraw < STATIC_REDRAW_MS) return;
  lastStaticDraw = millis();
  cls();
  drawCardHeader("star", "TODEW", C_PURPLE);

  if(strlen(TODEW_SYNC_TOKEN) == 0){
    ctrTxt1("Set TODEW token", 28, C_YELLOW);
    ctrTxt1("in secrets.h", 38, C_GRAY);
    return;
  }
  if(!todoLoaded){ ctrTxt1("Syncing...", 34, C_GRAY); return; }
  if(todoCount == 0){
    ctrTxt1("All clear!", 28, C_LIME);
    ctrTxt1("Nothing urgent", 38, C_GRAY);
    return;
  }

  int y = 17;
  int shown = min(todoCount, 5);
  for(int i = 0; i < shown; i++){
    uint16_t dotC = (todos[i].pri == 2) ? C_RED : (todos[i].pri == 1) ? C_AMBER : C_GRAY;
    fillRect(1, y + 2, 3, 3, dotC);
    char dl[8] = "";
    int d = todos[i].days;
    if(d < 9000){
      if(d < 0)       snprintf(dl, 8, "OVR");
      else if(d == 0) snprintf(dl, 8, "TDY");
      else            snprintf(dl, 8, "%dD", min(d, 99));
    }
    int dlw = strlen(dl) * 6;
    if(dl[0]) txt1(dl, PANEL_WIDTH - dlw - 1, y,
                   (d <= 0) ? C_RED : (d == 1) ? C_AMBER : C_GRAY);
    int maxCh = (PANEL_WIDTH - 6 - (dl[0] ? dlw + 3 : 0) - 2) / 6;
    txt1(trimTo(todos[i].title, maxCh).c_str(), 6, y, C_WHITE);
    y += 8;
  }
  if(todoCount > shown){
    char f[20]; snprintf(f, 20, "+%d more", todoCount - shown);
    ctrTxt1(f, y + 1, C_DARKGRAY);
  }
}

// =====================================================================
// QUOTE CARD — Vestaboard-style split-flap board, ported from the office
// Pi dashboard. Every tile starts blank and clicks forward through the
// character wheel IN ORDER until it lands on its letter (like the real
// hardware), so letters early in the wheel settle first and the board
// ripples to a finish. Quote rotates every 5 minutes.
// =====================================================================
const char VB_WHEEL[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.'-!?&%";
#define VB_COLS 20
#define VB_ROWS 5   // up to 4 wrapped quote lines + 1 author line

// Authored pre-uppercased and short enough to wrap into <=4 lines of 20.
const char* const VB_QUOTES[][2] = {
  {"WHAT STANDS IN THE WAY BECOMES THE WAY",          "MARCUS AURELIUS"},
  {"DO WHAT YOU CAN WITH WHAT YOU HAVE",              "T. ROOSEVELT"},
  {"THE PRICE OF GREATNESS IS RESPONSIBILITY",        "CHURCHILL"},
  {"A GOOD PLAN NOW BEATS A PERFECT PLAN NEXT WEEK",  "PATTON"},
  {"WINNING IS AN ALL THE TIME THING",                "LOMBARDI"},
  {"LEADERS ARE MADE THROUGH HARD WORK",              "LOMBARDI"},
  {"YOU MISS 100% OF SHOTS YOU DONT TAKE",            "GRETZKY"},
  {"THE BEST WAY TO PREDICT THE FUTURE IS CREATE IT", "DRUCKER"},
  {"BE THE MAN IN THE ARENA",                         "T. ROOSEVELT"},
  {"DO THINGS RIGHT ALL THE TIME",                    "LOMBARDI"},
};
const int VB_NUM_QUOTES = sizeof(VB_QUOTES)/sizeof(VB_QUOTES[0]);

// One tile: 6x8 cell on a 9px row pitch (1px black gap between rows).
// Blank tiles get the split-flap seam line; letter tiles stay clean for
// readability at this glyph size.
void drawVBCell(int x0, int boardY, int r, int c, char ch, uint16_t fg){
  int x = x0 + c*6, y = boardY + r*9;
  uint16_t bg = dsp->color565(20, 24, 32);
  if(ch == ' '){
    fillRect(x, y, 6, 8, bg);
    dsp->drawFastHLine(x, y+4, 6, dsp->color565(8, 10, 14));
  } else {
    dsp->drawChar(x, y, ch, fg, bg, 1);  // bg fill = no flicker on redraw
  }
}

void renderQuote(){
  static uint8_t tgt[VB_ROWS][VB_COLS];
  static uint8_t cur[VB_ROWS][VB_COLS];
  static int rowsUsed = 0;
  static int boardY = 16;
  static unsigned long lastTick = 0;
  const int x0 = (PANEL_WIDTH - VB_COLS*6) / 2;

  if(lastStaticDraw == 0){
    lastStaticDraw = millis();
    cls();
    drawCardHeader("star", "GET IT DONE", C_AMBER);   // live clock on right

    int qi = (int)((time(nullptr) / 300) % VB_NUM_QUOTES);
    // Greedy word-wrap the quote into up to VB_ROWS-1 lines
    String lines[VB_ROWS];
    rowsUsed = 0;
    {
      String s = String(VB_QUOTES[qi][0]);
      String line = "";
      int start = 0;
      while(start <= (int)s.length() && rowsUsed < VB_ROWS-1){
        int sp = s.indexOf(' ', start);
        String w = (sp < 0) ? s.substring(start) : s.substring(start, sp);
        if(line.length() == 0) line = w;
        else if((int)(line.length() + 1 + w.length()) <= VB_COLS) line += " " + w;
        else { lines[rowsUsed++] = line; line = w; }
        if(sp < 0) break;
        start = sp + 1;
      }
      if(line.length() && rowsUsed < VB_ROWS-1) lines[rowsUsed++] = line;
      lines[rowsUsed++] = trimTo("- " + String(VB_QUOTES[qi][1]), VB_COLS);
    }
    // Center each line into the tile grid and convert to wheel indices
    for(int r = 0; r < VB_ROWS; r++){
      int pad = (r < rowsUsed) ? (VB_COLS - (int)lines[r].length()) / 2 : 0;
      for(int c = 0; c < VB_COLS; c++){
        char ch = ' ';
        if(r < rowsUsed && c >= pad && c < pad + (int)lines[r].length())
          ch = lines[r][c - pad];
        const char* p = strchr(VB_WHEEL, ch);
        tgt[r][c] = p ? (uint8_t)(p - VB_WHEEL) : 0;
        cur[r][c] = 0;
      }
    }
    // Vertically center the used rows in the content area (y=16..63)
    boardY = 16 + (48 - rowsUsed*9) / 2;
    // Draw the whole board as blank tiles
    for(int r = 0; r < rowsUsed; r++)
      for(int c = 0; c < VB_COLS; c++)
        drawVBCell(x0, boardY, r, c, ' ', C_WHITE);
    lastTick = millis();
    return;
  }

  if(millis() - lastTick < 70) return;   // flap tick rate
  lastTick = millis();
  for(int r = 0; r < rowsUsed; r++){
    uint16_t fg = (r == rowsUsed-1) ? C_AMBER : C_WHITE;  // author in amber
    for(int c = 0; c < VB_COLS; c++)
      if(cur[r][c] < tgt[r][c]){
        cur[r][c]++;
        drawVBCell(x0, boardY, r, c, VB_WHEEL[cur[r][c]], fg);
      }
  }
}

// =====================================================================
// BREAKING NEWS TAKEOVER — full-screen red card, draw-once + flashing dot
// =====================================================================
void renderBreaking(){
  static unsigned long lastFlash=0;
  static bool dotOn=true;
  if(!breakingDrawn){
    breakingDrawn=true;
    cls();
    fillRect(0, 0, PANEL_WIDTH, 14, dsp->color565(170, 8, 8));
    txt1("BREAKING", 15, 3, C_WHITE);
    dsp->drawFastHLine(0, 14, PANEL_WIDTH, C_RED);
    // Wrap the headline into up to 5 lines of 21 chars
    String h=breakingHead;
    if(h.length()>105) h=h.substring(0,105);
    int y=18, start=0;
    for(int line=0; line<5 && start<(int)h.length(); line++){
      int end=start+21;
      if(end<(int)h.length()){
        int sp=h.lastIndexOf(' ', end);        // break at a word boundary
        if(sp>start) end=sp;
      } else end=h.length();
      String seg=h.substring(start,end); seg.trim();
      txt1(seg.c_str(), 2, y, C_WHITE);
      y+=9;
      start=end+ (end<(int)h.length() && h[end]==' ' ? 1 : 0);
    }
    lastFlash=millis();
  }
  if(millis()-lastFlash>500){                  // flashing live dot in the band
    lastFlash=millis(); dotOn=!dotOn;
    dsp->fillCircle(7, 7, 3, dotOn?C_WHITE:dsp->color565(170,8,8));
  }
}

// =====================================================================
// MAIN RENDER DISPATCH
// =====================================================================
void renderCurrentSlide(){
  // Breaking-news takeover overrides every slide while active
  if(breakingUntil){
    if(millis()<breakingUntil){ renderBreaking(); return; }
    breakingUntil=0; breakingDrawn=false;
    lastStaticDraw=0; cls();                   // force full redraw underneath
  }
  switch(currentSlide()){
    case SL_TITLE:         renderTitle();          break;
    case SL_WORKOUT_ANIM:  renderWorkoutAnim();    break;
    case SL_NEWS:          renderNews();          break;
    case SL_LOCAL_NEWS:    renderLocalNews();     break;
    case SL_SPORTS_ANIM:   renderSportsAnim();    break;
    case SL_SCORES:        renderScores();         break;
    case SL_GOLF:          renderGolf();           break;
    case SL_TENNIS:        renderTennis();         break;
    case SL_MONEY_ANIM:    renderMoneyAnim();      break;
    case SL_FINANCE:       renderFinance();        break;
    case SL_CONCERTS:      renderConcerts();       break;
    case SL_RFR_SCENE:     renderRFRScene();       break;
    case SL_RFR_WEATHER:   renderRFRWeather();     break;
    case SL_RFR_TRAFFIC:   renderRFRTraffic();     break;
    case SL_CABIN_SCENE:   renderCabinScene();     break;
    case SL_CABIN_WEATHER: renderCabinWeather();   break;
    case SL_CABIN_TRAFFIC: renderCabinTraffic();   break;
    case SL_CABIN_LAKE:    renderCabinLake();      break;
    case SL_TIKI_SCENE:    renderTikiScene();      break;
    case SL_TRAINS:        renderTrains();         break;
    case SL_FLIGHTS:       renderFlights();        break;
    case SL_MOON:          renderMoon();           break;
    case SL_PIXELART:      renderPixelArt();       break;
    case SL_GAMEOFLIFE:    renderGameOfLife();     break;
    case SL_CALENDAR:      renderCalendar();       break;
    case SL_WHOOP:         renderWhoop();          break;
    case SL_LIS_CHART:     renderLISChart();       break;
    case SL_IPSWICH_CHART: renderIpswichChart();   break;
    case SL_QUOTE:         renderQuote();          break;
    case SL_TODO:          renderToDo();           break;
    case SL_AURORA:        renderAurora();         break;
    case SL_STARFIELD:     renderStarfield();      break;
    case SL_SPACE:         renderSpace();          break;
    default: break;
  }
}

// =====================================================================
// SETUP
// =====================================================================
// Track boot time so the daily-reboot guard doesn't reboot inside the
// first hour of uptime (avoids restart loops if the device boots near 4am).
unsigned long bootMillis = 0;

void setupOTA(){
  // Hostname + password (password optional — empty disables auth, fine for LAN).
  ArduinoOTA.setHostname("dews-feed");
  // ArduinoOTA.setPassword("change-me");  // uncomment to require password
  ArduinoOTA.onStart([](){
    Serial.println("[OTA] start");
    if(matrix){ matrix->clearScreen(); matrix->setBrightness8(40); }
  });
  ArduinoOTA.onEnd([](){ Serial.println("[OTA] done"); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t){
    Serial.printf("[OTA] %u%%\n", (p*100)/t);
  });
  ArduinoOTA.onError([](ota_error_t e){ Serial.printf("[OTA] err %u\n", e); });
  ArduinoOTA.begin();
  Serial.print("[OTA] ready @ "); Serial.println(WiFi.localIP());
}

void setup(){
  Serial.begin(115200); delay(300);
  Serial.println("\n[THE DEWS FEED " DEWS_FEED_VERSION "]");
  bootMillis = millis();

  // Print the previous reset reason so we know WHY we're here. Critical for
  // diagnosing the news-card resets — different reasons point to different
  // root causes:
  //   POWERON / BROWNOUT  → PSU is sagging under load (drop brightness)
  //   PANIC / INT_WDT     → real software crash / watchdog overrun
  //   TASK_WDT            → loop() got stuck > WDT_TIMEOUT_SEC
  //   SW                  → ESP.restart() — daily reboot or heap-watch
  esp_reset_reason_t rr = esp_reset_reason();
  const char* rrName = "UNKNOWN";
  switch(rr){
    case ESP_RST_POWERON:  rrName = "POWERON";   break;
    case ESP_RST_EXT:      rrName = "EXT";       break;
    case ESP_RST_SW:       rrName = "SW";        break;
    case ESP_RST_PANIC:    rrName = "PANIC";     break;
    case ESP_RST_INT_WDT:  rrName = "INT_WDT";   break;
    case ESP_RST_TASK_WDT: rrName = "TASK_WDT";  break;
    case ESP_RST_WDT:      rrName = "WDT";       break;
    case ESP_RST_DEEPSLEEP:rrName = "DEEPSLEEP"; break;
    case ESP_RST_BROWNOUT: rrName = "BROWNOUT";  break;
    case ESP_RST_SDIO:     rrName = "SDIO";      break;
    default: break;
  }
  Serial.printf("[BOOT] reset reason: %s (%d)\n", rrName, (int)rr);

  // True hardware RNG — beats analogRead(0) which is correlated across boots.
  randomSeed(esp_random());

  initDisplay();
  initBuoyColors();
  showSplash();   // ← does WiFi connect + 7 fetches; can take 30-60s

  // Post-splash fetches
  fetchTraffic(WORK_LAT, WORK_LON, trafficWork);
  fetchTraffic(CABIN_LAT, CABIN_LON, trafficCabin);
  fetchLake();
  fetchWhoop();
  fetchCalendar();
  fetchToDew();   // no-ops if TODEW_SYNC_TOKEN is empty
  if(strlen(TICKETMASTER_KEY)>0) { fetchConcerts(); lastConcerts=millis(); }
  // Static schedule — no key needed, always runs
  fetchTrains();   lastTrains=millis();
  if(ESP.getFreeHeap()>60000) { fetchSP500Sparkline(); lastSP500Spark=millis(); }

  unsigned long now=millis();
  lastNews=lastLocalNews=lastScores=lastFinance=lastHomeWx=lastCabinWx=now;
  lastTrafficW=lastTrafficC=lastFlights=lastMarine=lastLake=now;
  lastWhoop=lastGcal=lastConcerts=lastTrains=lastToDew=now;
  // Critical: initialize these too — they default to 0, which makes them
  // fire on the FIRST loop iteration. fetchFlightRoutes can take 18s for 6
  // flights, fetchISS another 2s. Combined = WDT panic on first card.
  lastFlightRoutes=now;
  lastISS=now;

  slideStart=millis();
  playIdx=0;
  nightMode=isNightTime();

  // Register the hardware watchdog AFTER splash + initial fetches — those can
  // take 30-60s on first boot (WiFi DHCP + 7 sequential HTTPS fetches), which
  // would otherwise trip the WDT and force a reboot loop.
  //
  // Arduino-ESP32 core 3.x pre-initializes the TWDT at boot with a default
  // ~5s timeout. Calling esp_task_wdt_init() again returns ESP_ERR_INVALID_STATE.
  // We MUST use esp_task_wdt_reconfigure() to override the default timeout,
  // otherwise our 60s setting is ignored and the loopTask trips the 5s WDT
  // every time a fetch takes more than 5s.
#if ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t wdtConfig = {
    .timeout_ms     = WDT_TIMEOUT_SEC * 1000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic  = true,
  };
  esp_err_t wdtErr = esp_task_wdt_init(&wdtConfig);
  if(wdtErr == ESP_ERR_INVALID_STATE){
    // Already initialized by the Arduino core — switch to our timeout.
    esp_task_wdt_reconfigure(&wdtConfig);
    Serial.println("[WDT] reconfigured timeout to 60s");
  } else if(wdtErr != ESP_OK){
    Serial.printf("[WDT] init failed: %d\n", wdtErr);
  }
#else
  esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
#endif
  // Add (or re-add) loopTask to the watchdog watch list. Returning a non-OK
  // here is fine if the task is already on the list.
  esp_task_wdt_add(NULL);
  wdtArmed = true;   // ← from now on feedWdt() actually pets the dog

  Serial.println("[READY]");
}

// =====================================================================
// LOOP
// =====================================================================
void loop(){
  unsigned long now=millis();

  // ── Watchdog: reset hardware timer each pass through the loop.
  // If we don't reach this line within WDT_TIMEOUT_SEC, ESP32 reboots itself.
  esp_task_wdt_reset();

  // ── Proactive heap-watch: ESP32 String allocations from JSON parsing
  // fragment the heap over time. If free heap drops below 20KB we're at
  // serious risk of allocation failures that look like random crashes —
  // better to restart cleanly. This is a backstop; the daily 4am reboot
  // handles normal cases.
  static unsigned long lastHeapCheck=0;
  if(now-lastHeapCheck>30000){
    lastHeapCheck=now;
    size_t freeH = ESP.getFreeHeap();
    if(freeH < 20000 && (now-bootMillis) > 600000UL){
      Serial.printf("[PANIC] free heap %u bytes — restarting\n",(unsigned)freeH);
      delay(200);
      ESP.restart();
    }
  }

  // ── OTA: handle pending firmware uploads. Returns immediately if none.
  if(WiFi.status()==WL_CONNECTED) ArduinoOTA.handle();

  // ── Clock sanity: if NTP never landed (offline boot, DNS hiccup) the
  // clock free-runs from the 1970 epoch and every time display is garbage.
  // Re-kick SNTP every 5 min until localtime shows a believable year.
  static unsigned long lastTimeKick=0;
  if(now-lastTimeKick>300000UL){
    lastTimeKick=now;
    time_t tnow=time(nullptr);
    struct tm tchk; localtime_r(&tnow,&tchk);
    if(tchk.tm_year+1900<2020 && WiFi.status()==WL_CONNECTED){
      Serial.println("[TIME] epoch clock detected — restarting SNTP");
      configTzTime("EST5EDT,M3.2.0/2,M11.1.0/2","pool.ntp.org","time.nist.gov");
    }
  }

  // ── Daily reboot at 4:00am (router-style stability).
  // ESP32 String allocations from JSON parsing fragment the heap over weeks;
  // a clean restart prevents eventual crashes. Guarded against early-uptime
  // restart loops (must have been up at least 1 hour).
  {
    struct tm ti;
    if(getLocalTime(&ti) && ti.tm_hour==4 && ti.tm_min==0
       && (now - bootMillis) > 3600000UL){
      Serial.println("[REBOOT] daily 4am restart");
      delay(500);
      ESP.restart();
    }
  }

  ensureWiFi();

  // Night mode toggle
  bool shouldBeNight=isNightTime();
  if(shouldBeNight!=nightMode){
    nightMode=shouldBeNight;
    playIdx=0; slideStart=now;
    if(matrix) matrix->setBrightness8(nightMode?BRIGHTNESS_NIGHT:BRIGHTNESS_DAY);
    cls();
  }

  // Brightness
  if(now-lastBrightness>60000){
    if(matrix) matrix->setBrightness8(nightMode?BRIGHTNESS_NIGHT:BRIGHTNESS_DAY);
    lastBrightness=now;
  }

  // Data refreshes (only during day to conserve API calls at night)
  // Guard: skip if heap too low (prevents crash from fragmentation)
  //
  // FREEZE FIX (two rules):
  //  1. Fetches only run while a STATIC card is on screen. Every animated
  //     slide (scenes, transitions, scrolling news, game of life, title…)
  //     visibly freezes when an HTTP call blocks the loop for 5-15s, so we
  //     wait for a static card where a stall is invisible.
  //  2. At most ONE fetch per loop pass (else-if chain) so the display gets
  //     a render between fetches instead of chaining a 30s stall.
  if(!nightMode && WiFi.status()==WL_CONNECTED && ESP.getFreeHeap()>40000
     && !slideIsAnimated(currentSlide())){

    if     (now-lastNews     >REFRESH_NEWS)    {fetchNews();     lastNews=now;}
    else if(now-lastLocalNews>REFRESH_NEWS)    {fetchLocalNews();lastLocalNews=now;}
    else if(now-lastScores   >REFRESH_SCORES)  {fetchScores();   lastScores=now;}
    else if(now-lastGolf     >REFRESH_GOLF)    {fetchGolf();     lastGolf=now;}
    else if(now-lastTennis   >REFRESH_TENNIS)  {fetchTennis();   lastTennis=now;}
    else if(now-lastFinance  >REFRESH_FINANCE) {fetchFinance();  lastFinance=now;}
    // Sparkline: refresh every 15 min (matches 15-min interval data, stays under rate limits)
    else if(now-lastSP500Spark>900000UL && ESP.getFreeHeap()>60000) {fetchSP500Sparkline(); lastSP500Spark=now;}
    else if(now-lastHomeWx   >REFRESH_WEATHER) {fetchWeather(HOME_LAT,HOME_LON,homeWx);lastHomeWx=now;}
    else if(now-lastCabinWx  >REFRESH_WEATHER) {fetchWeather(CABIN_LAT,CABIN_LON,cabinWx);lastCabinWx=now;}
    else if(now-lastTrafficW >REFRESH_TRAFFIC) {fetchTraffic(WORK_LAT,WORK_LON,trafficWork);lastTrafficW=now;}
    else if(now-lastTrafficC >REFRESH_TRAFFIC) {fetchTraffic(CABIN_LAT,CABIN_LON,trafficCabin);lastTrafficC=now;}
    else if(now-lastFlights  >REFRESH_FLIGHTS) {fetchFlights();  lastFlights=now;
      // Resolve any new callsigns immediately after fetch — keeps display fresh
      lastFlightRoutes=0;
    }
    else if(now-lastFlightRoutes>REFRESH_FLIGHT_ROUTES && flightCount>0){
      fetchFlightRoutes(); lastFlightRoutes=now;
    }
    else if(now-lastMarine   >REFRESH_MARINE)  {fetchMarine();   lastMarine=now;}
    else if(now-lastLake     >REFRESH_LAKE)    {fetchLake();     lastLake=now;}
    else if(now-lastWhoop    >REFRESH_WHOOP)   {fetchWhoop();    lastWhoop=now;}
    else if(now-lastGcal     >REFRESH_GCAL)    {fetchCalendar(); lastGcal=now;}
    else if(now-lastToDew    >REFRESH_TODEW && strlen(TODEW_SYNC_TOKEN)>0){fetchToDew(); lastToDew=now;}
    else if(now-lastConcerts >REFRESH_CONCERTS && strlen(TICKETMASTER_KEY)>0) {fetchConcerts(); lastConcerts=now;}
    // Static schedule — refresh every REFRESH_TRAINS interval to advance the
    // displayed list as time passes. No key needed.
    else if(now-lastTrains   >REFRESH_TRAINS)  {fetchTrains(); lastTrains=now;}
  }
  // ISS refresh runs in BOTH day/night (so position is fresh when night card surfaces).
  // Lightweight endpoint, ~1KB JSON, no rate limit.
  if(WiFi.status()==WL_CONNECTED && ESP.getFreeHeap()>30000
     && now-lastISS>REFRESH_ISS){
    fetchISS(); lastISS=now;
  }

  // Advance slide (and skip scores if no games after 5s)
  bool skipEmpty=(currentSlide()==SL_SCORES&&gameCount==0&&now-slideStart>5000);
  // Golf / Tennis cards only linger when a major is actually live; otherwise
  // they flash their "no major this week" state briefly and move on.
  if(currentSlide()==SL_GOLF   && !golfIsMajor   && now-slideStart>3500) skipEmpty=true;
  if(currentSlide()==SL_TENNIS && !tennisIsMajor && now-slideStart>3500) skipEmpty=true;
  // TODEW card lingers only when there's something to read
  if(currentSlide()==SL_TODO && strlen(TODEW_SYNC_TOKEN)==0 && now-slideStart>3500) skipEmpty=true;
  if(currentSlide()==SL_TODO && todoLoaded && todoCount==0  && now-slideStart>5000) skipEmpty=true;
  // Pixel-art slide skips fast when no animations are compiled in
#if !HAS_PIXEL_ANIMS
  if(currentSlide()==SL_PIXELART && now-slideStart>2000) skipEmpty=true;
#endif
  // News cards advance early once every headline has scrolled by once, so all
  // stories are seen regardless of how long the headlines are.
  bool newsDone = (currentSlide()==SL_NEWS      && newsCycleDone  && newsCount>0)
               || (currentSlide()==SL_LOCAL_NEWS && lNewsCycleDone && localNewsCount>0);
  if(now-slideStart>currentDur()||skipEmpty||newsDone){
    advanceSlide();
  }

  // Render
  renderCurrentSlide();
  delay(14);
}
