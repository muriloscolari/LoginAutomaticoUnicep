#!/usr/bin/env bash
set -e

SERVICE_FILE="$HOME/.config/systemd/user/unifi-portal.service"

echo "==> Desinstalando unifi-portal.service..."

if systemctl --user is-active --quiet unifi-portal.service 2>/dev/null; then
    echo "==> Parando o serviço..."
    systemctl --user stop unifi-portal.service
fi

if systemctl --user is-enabled --quiet unifi-portal.service 2>/dev/null; then
    echo "==> Desabilitando o serviço..."
    systemctl --user disable unifi-portal.service
fi

if [ -f "$SERVICE_FILE" ]; then
    rm -f "$SERVICE_FILE"
    echo "==> Arquivo de serviço removido."
fi

systemctl --user daemon-reload
echo "✓ Desinstalação concluída."
