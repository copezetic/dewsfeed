#pragma once
// Copy to secrets.h and fill in. secrets.h is untracked.

#define WIFI_SSID        "YOUR_WIFI_SSID"
#define WIFI_PASSWORD    "YOUR_WIFI_PASSWORD"

#define OWM_API_KEY      "YOUR_OPENWEATHERMAP_KEY"
#define NEWS_API_KEY     "YOUR_NEWSAPI_KEY"
#define ALPHA_VANTAGE_KEY "YOUR_ALPHA_VANTAGE_KEY"
#define FRED_API_KEY    "YOUR_FRED_KEY"
#define EIA_API_KEY     "YOUR_EIA_KEY"
#define GMAPS_API_KEY   "YOUR_GOOGLE_MAPS_KEY"
#define GCAL_RELAY_URL  "YOUR_APPS_SCRIPT_GCAL_RELAY"
#define WHOOP_RELAY_URL "YOUR_APPS_SCRIPT_WHOOP_RELAY"
#define TICKETMASTER_KEY "YOUR_TICKETMASTER_KEY"
#define NY511_API_KEY    "YOUR_511NY_KEY"
#define TRAIN_STOP_ID    "110"           // Stamford CT (New Haven Line)
#define NWS_USER_AGENT   "(your-host.local, you@example.com)"

// ToDew app sync (Cloudflare Worker) — token matches the Worker's
// HEALTH_TOKEN secret and the app's Settings -> sync token.
#define TODEW_SYNC_URL   "https://your-worker.workers.dev"
#define TODEW_SYNC_TOKEN ""

#define HOME_LAT  41.0000
#define HOME_LON -73.0000
#define LAKE_LAT  41.0000
#define LAKE_LON -75.0000
