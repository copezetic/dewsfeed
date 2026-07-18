# The Dews Feed

DeWitt's ambient information system — one project, three screens:

| Screen | Where | What |
|---|---|---|
| [`dews_feed_web/`](dews_feed_web/) | Office (100 Mason St, Greenwich) | Raspberry Pi 4 kiosk "ticker wall" — 20+ rotating pages, war-room HUD design, alert takeovers, night ambient scenes |
| [`the_dews_feed_v10/`](the_dews_feed_v10/) | Home (Stamford) | ESP32 + 128×64 HUB75 LED matrix firmware — active version |
| [`the_dews_feed_v9/`](the_dews_feed_v9/) | — | Frozen LED firmware kept for rollback reference |

## ⚠️ This repo is PRIVATE for a reason

API keys, WiFi credentials, and sync tokens are embedded in `secrets.h` and
`index.html` (kiosk-style project — the screens are trusted devices). Never
make this repo public and never fork it to a public location.

## Quick orientation

- **Pi dashboard**: single-file app — [`dews_feed_web/index.html`](dews_feed_web/index.html)
  (pages, design system, data fetchers) + [`proxy.py`](dews_feed_web/proxy.py)
  (CORS proxy, host allowlist). Deploys via the Windows scheduled task
  ("DewsFeed Deploy") whenever the Pi is reachable over Tailscale.
- **LED firmware**: [`the_dews_feed_v10/the_dews_feed_v10.ino`](the_dews_feed_v10/the_dews_feed_v10.ino)
  + generated asset headers. Pre-flash check: `awk -f tools/balance.awk *.ino`.
- **Ops knowledge** (root causes, gotchas, network quirks) lives in each
  project's `CLAUDE.md` and in Claude's session memory.

## Notes

Jot anything in [`NOTES.md`](NOTES.md) — ideas, bugs spotted on the wall,
tuning requests — and mention it in a Claude session to action it.
