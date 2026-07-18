# Dews Feed — Raspberry Pi Kiosk Setup

Turns the Pi into a dedicated Dews Feed display: boots straight into the
dashboard fullscreen, no keyboard/mouse needed after setup.

## 1. Flash the SD card (on your PC)

1. Download **Raspberry Pi Imager**: https://www.raspberrypi.com/software/
2. Insert the microSD card into your PC
3. In Imager: **Choose Device** → your Pi model → **Choose OS** →
   **Raspberry Pi OS (64-bit)** (the default, WITH desktop) → **Choose Storage** → the SD card
4. Click **Next → Edit Settings** and set:
   - Hostname: `dewsfeed`
   - Username / password: `dews` / (your pick)
   - **Configure wireless LAN**: your WiFi name + password  ← important
   - Locale: America/New_York
5. Write. When done, move the SD card to the Pi.

## 2. First boot

Connect the Pi to the monitor (micro-HDMI side goes in the Pi), power it on.
It boots to the desktop. Connect a keyboard/mouse for this one-time setup
(or SSH to `dewsfeed.local` from your PC).

## 3. Copy the dashboard onto the Pi

Easiest: put the whole `dews_feed_web` folder on a USB stick, plug it into
the Pi, then in a terminal on the Pi:

    cp -r /media/dews/*/dews_feed_web ~/dews_feed_web

(Or from your PC: `scp -r dews_feed_web dews@dewsfeed.local:~/`)

## 4. Auto-start the proxy + kiosk browser

In a terminal on the Pi, run these four commands:

    mkdir -p ~/.config/autostart

    cat > ~/.config/autostart/dewsproxy.desktop <<'EOF'
    [Desktop Entry]
    Type=Application
    Name=Dews Feed Proxy
    Exec=python3 /home/dews/dews_feed_web/proxy.py
    EOF

    cat > ~/.config/autostart/dewsfeed.desktop <<'EOF'
    [Desktop Entry]
    Type=Application
    Name=Dews Feed Kiosk
    Exec=chromium-browser --kiosk --noerrdialogs --disable-restore-session-state --autoplay-policy=no-user-gesture-required file:///home/dews/dews_feed_web/index.html
    EOF

    sudo raspi-config nonint do_blanking 1

(That last line disables screen blanking so the display never sleeps.
If your username isn't `dews`, adjust the two /home/dews paths.)

## 5. Reboot

    sudo reboot

The Pi now boots directly into the full-screen Dews Feed. The masthead chip
should read **PROXY: CONNECTED** — that means markets, rates, news chyron,
and traffic are all live.

## Day-to-day

- **Update the dashboard**: replace `~/dews_feed_web/index.html`, reboot
  (or press F5 with a keyboard attached).
- **Exit kiosk mode**: plug in a keyboard, press Alt+F4.
- **Everything refreshes itself**: weather 10 min, scores 2 min, markets
  5 min, news 10 min, trains every minute — no interaction needed.

## Troubleshooting

| Symptom | Fix |
|---|---|
| "PROXY OFF" chip in masthead | proxy.py didn't start — check `python3 ~/dews_feed_web/proxy.py` by hand for errors |
| Markets / news / traffic blank | Same as above — those four feeds need the proxy |
| WHOOP card says "relay needs re-auth" | Visit the WHOOP relay `?auth=start` URL (same fix as the LED panel) |
| Screen goes dark after minutes | Re-run: `sudo raspi-config nonint do_blanking 1` and reboot |
| Wrong times | `sudo raspi-config` → Localisation → Timezone → America/New_York |
