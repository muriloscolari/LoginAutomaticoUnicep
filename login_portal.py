#!/usr/bin/env python3
"""
login_portal.py - Automatização de login no portal captivo UniFi (UNICEP)
Funciona tanto em modo único (CLI) quanto em modo daemon / serviço.
"""

import sys
import os

# Forçar flush de logs imediatamente no systemd / terminal
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(line_buffering=True)
import subprocess
import time
import argparse
import urllib.parse
import re
import select

CHECK_URLS = [
    "http://connectivitycheck.gstatic.com/generate_204",
    "http://detectportal.firefox.com/canonical.html",
]

DEFAULT_FALLBACK_PORTAL = "http://10.0.1.2:8880/guest/s/default/"

def notify(title, message, icon="network-wireless"):
    """Envia notificação gráfica no desktop do usuário."""
    try:
        subprocess.run(
            ["notify-send", "-a", "Portal Wi-Fi", "-i", icon, title, message],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL
        )
    except Exception:
        pass

def get_active_wifi_interface():
    """Detecta a interface Wi-Fi ativa."""
    try:
        out = subprocess.check_output(
            ["nmcli", "-t", "-f", "DEVICE,TYPE,STATE", "device"],
            text=True,
            stderr=subprocess.DEVNULL
        )
        for line in out.strip().splitlines():
            parts = line.split(":")
            if len(parts) >= 3 and parts[1] == "wifi" and "conectado" in parts[2]:
                return parts[0]
    except Exception:
        pass

    try:
        out = subprocess.check_output(["ip", "route", "show", "default"], text=True, stderr=subprocess.DEVNULL)
        m = re.search(r"dev\s+(\S+)", out)
        if m:
            return m.group(1)
    except Exception:
        pass

    return "wlan0"

def get_active_ssid():
    """Obtém o nome (SSID) da rede Wi-Fi conectada."""
    try:
        out = subprocess.check_output(
            ["nmcli", "-t", "-f", "NAME,TYPE", "connection", "show", "--active"],
            text=True,
            stderr=subprocess.DEVNULL
        )
        for line in out.strip().splitlines():
            parts = line.split(":")
            if len(parts) >= 2 and parts[1] in ("802-11-wireless", "wifi"):
                return parts[0]
    except Exception:
        pass
    return None

def is_likely_college_network(ssid):
    """
    Verifica se o SSID atual corresponde à rede da faculdade.
    Exemplos: A5_Alunos_Livre, UNICEP, Alunos, etc.
    """
    if not ssid:
        return True  # Se não puder identificar o SSID, confia na detecção do portal
    keywords = ["alunos", "unicep", "livre", "faculdade"]
    return any(k in ssid.lower() for k in keywords)

def curl_cmd(args, iface=None, timeout=5):
    """
    Executa o curl vinculando à interface Wi-Fi (--interface <iface>).
    Isso é crucial caso o WireGuard / VPN esteja ativo, impedindo que a VPN
    sequestre o tráfego antes de a rede local ter sido liberada.
    """
    cmd = ["curl", "-s"]
    if iface:
        cmd.extend(["--interface", iface])
    cmd.extend(["--connect-timeout", str(timeout), "--max-time", str(timeout * 2)])
    cmd.extend(args)
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

def check_internet(iface):
    """
    Verifica se a internet já está acessível através da interface.
    Retorna (True, '204', url) se o acesso livre foi confirmado.
    """
    for url in CHECK_URLS:
        res = curl_cmd(["-o", "/dev/null", "-w", "%{http_code}", url], iface=iface, timeout=4)
        code = res.stdout.strip()
        if code == "204":
            return True, code, url
    return False, code or "000", CHECK_URLS[0]

def discover_portal(iface):
    """
    Descobre o endereço do portal captivo seguindo os redirecionamentos (302/307).
    No UniFi, o AP redireciona para a porta 8882 (/redirect?ec=...)
    e depois para a porta 8880 (/guest/s/default/?ap=...&ec=...).
    """
    for url in CHECK_URLS:
        res = curl_cmd([
            "-L",
            "-o", "/dev/null",
            "-w", "%{url_effective} | %{http_code}",
            url
        ], iface=iface, timeout=6)

        output = res.stdout.strip()
        if " | " in output:
            final_url, code = output.split(" | ", 1)
            final_url = final_url.strip()
            if "8880" in final_url or "8882" in final_url or "/guest/s/" in final_url:
                return final_url
    return None

def perform_login(portal_url, iface, quiet=False):
    """
    Realiza a requisição POST para autorizar o cliente no portal UniFi.
    """
    parsed = urllib.parse.urlparse(portal_url)

    # Se a URL encontrada for o redirect intermediário na 8882, segue para a 8880
    if ":8882" in portal_url or "/redirect" in parsed.path:
        res = curl_cmd(["-L", "-o", "/dev/null", "-w", "%{url_effective}", portal_url], iface=iface, timeout=5)
        portal_url = res.stdout.strip() or portal_url
        parsed = urllib.parse.urlparse(portal_url)

    # Monta a URL de login mantendo o caminho do site (ex: /guest/s/default/login)
    path = parsed.path
    if not path.endswith("/"):
        path_dir = path[:path.rfind("/") + 1]
    else:
        path_dir = path

    login_endpoint = f"{parsed.scheme}://{parsed.netloc}{path_dir}login"

    if not quiet:
        print(f"[*] Enviando requisição de login para: {login_endpoint}")
        print(f"[*] Referer: {portal_url}")

    # No UniFi com 'auth: none', o login é um POST com body vazio (null)
    # e o Referer apontando para a página onde constam os parâmetros 'ap' e 'ec'
    headers = [
        "-H", f"Referer: {portal_url}",
        "-H", "Accept: */*",
        "-H", "User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
    ]

    res = curl_cmd([
        "-X", "POST",
        "-o", "/dev/null",
        "-w", "%{http_code}",
        *headers,
        login_endpoint
    ], iface=iface, timeout=6)

    return res.stdout.strip()

def attempt_login(iface, force=False, quiet=False):
    """
    Fluxo completo: verifica conectividade, descobre portal, envia POST e valida.
    Retorna True se conectado com sucesso, False caso contrário.
    """
    ssid = get_active_ssid()

    if not force:
        has_internet, code, check_url = check_internet(iface)
        if has_internet:
            if not quiet:
                print(f"[✓] Conexão com a internet já está ativa (HTTP {code} em {check_url}). Login desnecessário.")
            return True
        else:
            if not quiet:
                print(f"[!] Sem internet livre (HTTP {code}). Iniciando login no portal...")
    else:
        if not quiet:
            print("[!] Modo --force: enviando requisição de login...")

    portal_url = discover_portal(iface)
    if not portal_url:
        # Se não detectou portal e o SSID não tem relação com a faculdade, ignora
        if not force and not is_likely_college_network(ssid):
            if not quiet:
                print(f"[i] Rede conectada ('{ssid}') não possui portal captivo da faculdade detectado. Nenhuma ação necessária.")
            return True

        if not quiet:
            print(f"[!] Não foi possível detectar a URL por redirecionamento. Tentando fallback: {DEFAULT_FALLBACK_PORTAL}")
        portal_url = DEFAULT_FALLBACK_PORTAL
    else:
        if not quiet:
            print(f"[✓] Portal detectado em: {portal_url}")

    login_code = perform_login(portal_url, iface, quiet=quiet)
    if not quiet:
        print(f"[i] Resposta do login: HTTP {login_code}")

    # Aguarda 1.5s para aplicação das regras no AP UniFi
    time.sleep(1.5)

    has_internet, code, _ = check_internet(iface)
    if has_internet:
        if not quiet:
            print("[✓] Login efetuado com sucesso! Internet liberada.")
        notify("Wi-Fi Conectado", f"Login efetuado com sucesso na rede {ssid or 'da faculdade'}!")
        return True
    else:
        if not quiet:
            print(f"[✗] Falha na liberação da internet após login (HTTP {code}).")
        notify("Falha no Login Wi-Fi", f"Não foi possível liberar o acesso na rede {ssid or 'da faculdade'}.", icon="dialog-error")
        return False

def run_daemon(iface, interval=60):
    """
    Executa como serviço em background:
    1. Monitora eventos do NetworkManager via 'nmcli monitor'
    2. Executa checagem periódica a cada 'interval' segundos (caso a sessão expire)
    """
    print(f"[+] Iniciando daemon de login automático (intervalo: {interval}s)...")
    last_check_time = 0
    COOLDOWN = 10  # Não tenta logar mais de 1x a cada 10s

    # Testa imediatamente ao iniciar
    active_iface = iface or get_active_wifi_interface()
    attempt_login(active_iface, quiet=False)
    last_check_time = time.time()

    # Inicia o monitor do NetworkManager
    proc = subprocess.Popen(
        ["nmcli", "monitor"],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True
    )

    try:
        while True:
            # Espera por eventos do nmcli ou até o timeout do intervalo periódico
            r, _, _ = select.select([proc.stdout], [], [], interval)
            now = time.time()

            if r:
                line = proc.stdout.readline()
                if not line:
                    # Se o nmcli monitor fechou por algum motivo, reinicia
                    time.sleep(2)
                    proc = subprocess.Popen(["nmcli", "monitor"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
                    continue

                line_str = line.strip()
                # Se for evento de conectividade ou rede
                if any(w in line_str.lower() for w in ["conectado", "conectividade", "connectivity", "connection", "portal"]):
                    if now - last_check_time >= COOLDOWN:
                        time.sleep(2)  # Dá tempo do DHCP estabilizar
                        active_iface = iface or get_active_wifi_interface()
                        print(f"[*] Evento de rede detectado: {line_str}")
                        attempt_login(active_iface, quiet=False)
                        last_check_time = time.time()
            else:
                # Timeout periódico: checagem de saúde
                if now - last_check_time >= interval:
                    active_iface = iface or get_active_wifi_interface()
                    has_internet, _, _ = check_internet(active_iface)
                    if not has_internet:
                        print("[*] Checagem periódica detectou perda de acesso. Efetuando login...")
                        attempt_login(active_iface, quiet=False)
                    last_check_time = time.time()

    except KeyboardInterrupt:
        print("\n[+] Encerrando daemon...")
    finally:
        try:
            proc.terminate()
        except Exception:
            pass

def main():
    parser = argparse.ArgumentParser(description="Auto-login no portal captivo UniFi (UNICEP).")
    parser.add_argument("-i", "--interface", help="Interface de rede Wi-Fi (ex: wlan0)")
    parser.add_argument("-f", "--force", action="store_true", help="Força o login mesmo se a internet parecer ativa")
    parser.add_argument("-q", "--quiet", action="store_true", help="Modo silencioso (sem prints informativos)")
    parser.add_argument("-d", "--daemon", action="store_true", help="Executa continuamente como serviço em background")
    parser.add_argument("--interval", type=int, default=60, help="Intervalo de checagem periódica no modo daemon (padrão: 60s)")
    args = parser.parse_args()

    iface = args.interface or get_active_wifi_interface()

    if args.daemon:
        run_daemon(iface, interval=args.interval)
        return 0

    success = attempt_login(iface, force=args.force, quiet=args.quiet)
    return 0 if success else 1

if __name__ == "__main__":
    sys.exit(main())
