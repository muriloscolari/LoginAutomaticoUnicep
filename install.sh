#!/usr/bin/env bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT_PATH="$DIR/login_portal.py"
SERVICE_DIR="$HOME/.config/systemd/user"
SERVICE_FILE="$SERVICE_DIR/unifi-portal.service"

echo "==> Configurando Login Automático UniFi (UNICEP)..."

if [ ! -f "$SCRIPT_PATH" ]; then
    echo "Erro: '$SCRIPT_PATH' não encontrado!" >&2
    exit 1
fi

chmod +x "$SCRIPT_PATH"

mkdir -p "$SERVICE_DIR"

cat > "$SERVICE_FILE" <<EOF
[Unit]
Description=Auto-Login Portal Captivo UniFi (Faculdade)
After=network.target

[Service]
Type=simple
Environment=PYTHONUNBUFFERED=1
ExecStart=$SCRIPT_PATH --daemon
Restart=always
RestartSec=10

[Install]
WantedBy=default.target
EOF

echo "==> Recarregando systemd user daemon..."
systemctl --user daemon-reload

echo "==> Habilitando e iniciando o serviço unifi-portal.service..."
systemctl --user enable --now unifi-portal.service

echo ""
echo "✓ Instalação concluída com sucesso!"
echo "Status do serviço:"
systemctl --user status unifi-portal.service --no-pager
