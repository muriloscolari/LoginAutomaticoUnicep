# Login Automático no Portal Captivo UniFi (UNICEP)

Script e serviço de automação para conexão e liberação automática no portal captivo UniFi da faculdade (UNICEP).

---

## 🔍 Como o Login Funciona

Ao analisar o fluxo de rede do portal captivo em roteadores Ubiquiti UniFi:

1. **Detecção e Interceptação**:
   - Ao se conectar ao Wi-Fi (ex: `A5_Alunos_Livre`), a primeira tentativa de navegação (como a verificação de conectividade do sistema operacional em `http://connectivitycheck.gstatic.com/generate_204`) é interceptada pelo AP UniFi.
   - O AP responde com um redirecionamento HTTP 302 para o daemon de redirecionamento interno:
     ```
     http://10.0.1.2:8882/redirect?ec=<token_criptografado>
     ```
   - O daemon na porta 8882 decodifica a sessão e redireciona (HTTP 302) para a interface web na porta 8880:
     ```
     http://10.0.1.2:8880/guest/s/default/?ap=6c:63:f8:9d:76:12&ec=<token>
     ```
     *(Onde `ap` é o endereço MAC do ponto de acesso e `ec` é o token criptografado da sessão).*

2. **Configuração do Portal (`auth: none`)**:
   - A página consulta `GET /guest/s/default/hotspotconfig`, que retorna:
     ```json
     {
       "auth": "none",
       "payment_enabled": false,
       "radius_enabled": false,
       "password_enabled": false,
       "voucher_enabled": false,
       "title": "Rede WiFi UNICEP",
       "button_text": "Login"
     }
     ```
   - Trata-se de uma autenticação **"click-through" (auth: none)**: não há usuário, senha nem vouchers.

3. **A Requisição de Login**:
   - Quando o botão de login é pressionado, a página dispara uma requisição POST:
     ```http
     POST http://10.0.1.2:8880/guest/s/default/login HTTP/1.1
     Referer: http://10.0.1.2:8880/guest/s/default/?ap=<ap_mac>&ec=<token>
     ```
   - **Corpo da requisição**: Vazio (`null`).
   - **Cookies**: Desnecessários (`credentials: omit`).
   - O controlador UniFi recebe a requisição, valida o cliente e o AP via cabeçalho `Referer`, libera o endereço MAC nas tabelas de firewall do AP e responde com redirecionamento HTTP 302. A internet passa a fluir normalmente.

4. **Tratamento Especial para VPN / WireGuard**:
   - Se houver uma VPN ativa (como WireGuard com rota padrão), a VPN tenta rotear todo o tráfego antes da liberação do portal, travando conexões locais.
   - O script resolve isso vinculando todas as requisições de teste e login diretamente à interface Wi-Fi (`--interface wlan0` / `SO_BINDTODEVICE`), contornando a VPN até que a rede local esteja liberada.

---

## 📁 Estrutura do Projeto

* `login_portal.py`: Script principal em Python (suporta modo pontual ou modo `--daemon`).
* `install.sh`: Script de instalação rápida do serviço de usuário no `systemd`.
* `uninstall.sh`: Script para desativar e remover o serviço.
* `unifi-portal.service`: Arquivo de unidade `systemd --user`.
* `99-captive-portal.sh`: Script alternativo para NetworkManager Dispatcher (nível de sistema).

---

## 🚀 Instalação Rápida (Serviço de Usuário)

Clone o repositório e execute o instalador:

```bash
git clone https://github.com/muriloscolari/LoginAutomaticoUnicep.git
cd LoginAutomaticoUnicep
./install.sh
```

O instalador irá:
1. Tornar os scripts executáveis.
2. Configurar o serviço `systemd --user`.
3. Habilitar a inicialização automática junto com seu login no sistema.

### Gerenciando o Serviço

```bash
# Ver status do serviço
systemctl --user status unifi-portal.service

# Acompanhar logs em tempo real
journalctl --user -u unifi-portal.service -f

# Reiniciar ou parar o serviço
systemctl --user restart unifi-portal.service
systemctl --user stop unifi-portal.service
```

---

## 🛠️ Uso Manual (Terminal)

Você também pode executar o script manualmente a qualquer momento:

```bash
# Checa se a internet precisa de liberação e efetua login se necessário
./login_portal.py

# Força o envio do POST mesmo se a internet já parecer ativa
./login_portal.py --force

# Especificar interface manualmente
./login_portal.py -i wlan0
```

---

## 💡 Como a Detecção Funciona

O script possui 3 camadas de segurança:

1. **Teste de Conectividade (`generate_204`)**:
   - No Wi-Fi de casa, o Google responde `HTTP 204 No Content`. O script encerra instantaneamente sem fazer nada.
   - Na faculdade (bloqueado), o AP retorna `HTTP 302` interceptando a conexão.
2. **Identificação da Assinatura UniFi**:
   - Segue os redirecionamentos para confirmar se o destino é um portal UniFi (portas `8880`/`8882` e rota `/guest/s/`).
3. **Filtro de SSID**:
   - Se a internet cair em uma rede residencial (ex: problema na operadora), o script reconhece que o SSID não é da faculdade e que não há portal UniFi, evitando tentativas inválidas.

---

## 📄 Licença

MIT License. Sinta-se livre para usar, modificar e compartilhar.
