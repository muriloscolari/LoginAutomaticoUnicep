#!/usr/bin/env bash
# NetworkManager Dispatcher Script para auto-login no portal captivo
# Para instalar a nível de sistema (requer root):
# sudo cp 99-captive-portal.sh /etc/NetworkManager/dispatcher.d/99-captive-portal.sh
# sudo chmod +x /etc/NetworkManager/dispatcher.d/99-captive-portal.sh

IFACE="$1"
ACTION="$2"

# Ajuste o caminho abaixo se necessário (ou instale em /usr/local/bin/login_portal.py)
SCRIPT_PATH="/usr/local/bin/login_portal.py"

if [ ! -x "$SCRIPT_PATH" ]; then
    # Tenta localizar o script no diretório do usuário
    SCRIPT_PATH="$(find /home -maxdepth 4 -name login_portal.py -executable 2>/dev/null | head -n 1)"
fi

if [ "$ACTION" = "up" ] || [ "$ACTION" = "connectivity-change" ]; then
    if [ -n "$SCRIPT_PATH" ] && [ -x "$SCRIPT_PATH" ]; then
        "$SCRIPT_PATH" --interface "$IFACE" --quiet > /dev/null 2>&1 &
    fi
fi
