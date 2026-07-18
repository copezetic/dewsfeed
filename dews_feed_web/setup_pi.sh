#!/bin/bash
# Dews Feed kiosk setup — run once on the Pi. Idempotent.
set -e

echo "== 1. autostart entries =="
mkdir -p ~/.config/autostart

cat > ~/.config/autostart/dewsproxy.desktop <<'EOF'
[Desktop Entry]
Type=Application
Name=Dews Feed Proxy
Exec=python3 /home/dew/dews_feed_web/proxy.py
EOF

cat > ~/.config/autostart/dewsfeed.desktop <<'EOF'
[Desktop Entry]
Type=Application
Name=Dews Feed Kiosk
Exec=/usr/bin/chromium --kiosk --noerrdialogs --disable-restore-session-state --disable-session-crashed-bubble --autoplay-policy=no-user-gesture-required file:///home/dew/dews_feed_web/index.html
EOF
echo "   proxy + kiosk autostart written"

echo "== 2. disable screen blanking =="
sudo raspi-config nonint do_blanking 1
echo "   display will never sleep"

echo "== 3. office WiFi profile (BankPatriotGuest) =="
if sudo nmcli -t -f NAME connection show | grep -qx "BankPatriotGuest"; then
  echo "   profile already exists"
else
  sudo nmcli connection add type wifi ifname wlan0 con-name BankPatriotGuest \
    ssid BankPatriotGuest wifi-sec.key-mgmt wpa-psk wifi-sec.psk "Patriot02" \
    connection.autoconnect yes >/dev/null
  echo "   profile added — will auto-join at the office"
fi

echo "== 4. quick sanity: proxy starts and answers =="
python3 /home/dew/dews_feed_web/proxy.py &
PROXY_PID=$!
sleep 2
if curl -s --max-time 3 http://localhost:8787/ping >/dev/null; then
  echo "   proxy answers on :8787"
else
  echo "   WARNING: proxy did not answer"
fi
kill $PROXY_PID 2>/dev/null || true

echo "== setup complete =="
