#!/usr/bin/env bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_FILE="$DIR/login_portal.c"
BIN_PATH="$DIR/login_portal"
SERVICE_DIR="$HOME/.config/systemd/user"
SERVICE_FILE="$SERVICE_DIR/unifi-portal.service"

echo "==> Compilando login_portal em C..."

if command -v make >/dev/null 2>&1 && [ -f "$DIR/Makefile" ]; then
    make -C "$DIR"
else
    gcc -O2 -Wall -Wextra -s -o "$BIN_PATH" "$SRC_FILE"
fi

chmod +x "$BIN_PATH"

mkdir -p "$SERVICE_DIR"

cat > "$SERVICE_FILE" <<EOF
[Unit]
Description=Auto-Login Portal Captivo UniFi (Faculdade) - C Daemon
After=network.target

[Service]
Type=simple
ExecStart=$BIN_PATH --daemon
Restart=always
RestartSec=10

[Install]
WantedBy=default.target
EOF

echo "==> Recarregando systemd user daemon..."
systemctl --user daemon-reload

echo "==> Habilitando e reiniciando o serviço unifi-portal.service..."
systemctl --user enable unifi-portal.service
systemctl --user restart unifi-portal.service

echo ""
echo "✓ Instalação da versão em C concluída com sucesso!"
echo "Status do serviço:"
systemctl --user status unifi-portal.service --no-pager
