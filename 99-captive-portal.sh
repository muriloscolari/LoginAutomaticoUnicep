#!/usr/bin/env bash
# NetworkManager Dispatcher Script para auto-login no portal captivo
# Para instalar a nível de sistema (requer root):
# sudo cp 99-captive-portal.sh /etc/NetworkManager/dispatcher.d/99-captive-portal.sh
# sudo chmod +x /etc/NetworkManager/dispatcher.d/99-captive-portal.sh

IFACE="$1"
ACTION="$2"

BIN_PATH="/usr/local/bin/login_portal"

if [ ! -x "$BIN_PATH" ]; then
    BIN_PATH="$(find /home -maxdepth 4 -name login_portal -type f -executable 2>/dev/null | head -n 1)"
fi

if [ ! -x "$BIN_PATH" ]; then
    BIN_PATH="$(find /home -maxdepth 4 -name login_portal.py -type f -executable 2>/dev/null | head -n 1)"
fi

if [ "$ACTION" = "up" ] || [ "$ACTION" = "connectivity-change" ]; then
    if [ -n "$BIN_PATH" ] && [ -x "$BIN_PATH" ]; then
        "$BIN_PATH" --interface "$IFACE" --quiet > /dev/null 2>&1 &
    fi
fi
