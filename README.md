# Login Automático no Portal Captivo UniFi (UNICEP)

Daemon e utilitário CLI ultra-leve em **C puro** (consumindo apenas **~312 KB de RAM** e zero subprocessos) para autenticação automática no portal captivo UniFi da faculdade (UNICEP). Também inclui uma versão de referência em Python.

---

## ⚡ Destaques da Implementação em C (`login_portal.c`)

* **Consumo Mínimo de Recursos:** Usa cerca de **312 KB de RAM** (97% menos memória que a versão em Python) em um único processo de **27 KB**.
* **Netlink Socket Nativo (`AF_NETLINK`):** Em vez de manter processos pesados como `nmcli monitor` abertos, escuta eventos de interface e atribuição de IP (`RTMGRP_LINK | RTMGRP_IPV4_IFADDR`) diretamente do Kernel Linux via `poll()` com 0% de uso de CPU.
* **Leitura de SSID via Kernel (`ioctl`):** Consulta o nome da rede Wi-Fi conectada diretamente via `SIOCGIWESSID` (`<linux/wireless.h>`), sem invocar processos externos.
* **Sockets POSIX com `SO_BINDTODEVICE`:** Todas as conexões HTTP são vinculadas diretamente à placa Wi-Fi (`wlan0`), contornando túneis de VPN/WireGuard que poderiam bloquear a rota antes da autenticação no portal.
* **Zero Dependências Externas:** Compila apenas com `gcc` e a `libc` padrão do Linux.

---

## 🔍 Como o Login Funciona

Ao analisar o fluxo de rede do portal captivo em roteadores Ubiquiti UniFi:

1. **Detecção e Interceptação**:
   - Ao se conectar ao Wi-Fi (ex: `A5_Alunos_Livre`), qualquer tentativa de acessar a internet em HTTP (como `http://connectivitycheck.gstatic.com/generate_204`) é interceptada pelo AP UniFi.
   - O AP responde com um redirecionamento HTTP 302 para o daemon de redirecionamento interno na porta `8882`:
     ```
     http://10.0.1.2:8882/redirect?ec=<token_criptografado>
     ```
   - O daemon na porta 8882 decodifica a sessão e redireciona (HTTP 302) para a interface web na porta `8880`:
     ```
     http://10.0.1.2:8880/guest/s/default/?ap=6c:63:f8:9d:76:12&ec=<token>
     ```
     *(Onde `ap` é o endereço MAC do ponto de acesso e `ec` é o token criptografado da sessão).*

2. **Configuração do Portal (`auth: none`)**:
   - O endpoint `GET /guest/s/default/hotspotconfig` indica que a rede utiliza autenticação **"click-through" (`auth: none`)**: não exige usuário, senha nem vouchers.

3. **A Requisição de Login**:
   - Quando o botão de login é pressionado, o portal dispara:
     ```http
     POST http://10.0.1.2:8880/guest/s/default/login HTTP/1.1
     Referer: http://10.0.1.2:8880/guest/s/default/?ap=<ap_mac>&ec=<token>
     Content-Length: 0
     ```
   - O controlador UniFi recebe a requisição, valida o cliente e o AP via cabeçalho `Referer`, libera o endereço MAC nas tabelas de firewall do AP e responde com redirecionamento HTTP 302. A internet passa a fluir imediatamente.

---

## 📁 Estrutura do Projeto

* `login_portal.c`: Implementação principal em C puro (CLI + Daemon Netlink).
* `Makefile`: Script de compilação (`make`, `make clean`, `make install`).
* `install.sh`: Compila o binário em C e instala/ativa o serviço `systemd --user`.
* `uninstall.sh`: Desativa e remove o serviço.
* `unifi-portal.service`: Arquivo de unidade `systemd --user`.
* `99-captive-portal.sh`: Script alternativo para NetworkManager Dispatcher (nível de sistema).
* `login_portal.py`: Versão alternativa/referência escrita em Python.

---

## 🚀 Instalação Rápida

Clone o repositório e execute o instalador (necessita apenas do `gcc` instalado):

```bash
git clone https://github.com/muriloscolari/LoginAutomaticoUnicep.git
cd LoginAutomaticoUnicep
./install.sh
```

O instalador irá:
1. Compilar `login_portal.c` gerando o binário otimizado `login_portal`.
2. Configurar e ativar o serviço `systemd --user` (`unifi-portal.service`).

### Gerenciando o Serviço

```bash
# Ver status e consumo de memória do serviço
systemctl --user status unifi-portal.service

# Acompanhar logs em tempo real
journalctl --user -u unifi-portal.service -f

# Reiniciar ou parar o serviço
systemctl --user restart unifi-portal.service
systemctl --user stop unifi-portal.service
```

---

## 🛠️ Uso Manual (Terminal)

Você pode executar o binário compilado diretamente a qualquer momento:

```bash
# Compilar manualmente (se ainda não compilou)
make

# Checa se a internet precisa de liberação e efetua login se necessário
./login_portal

# Força o envio do POST mesmo se a internet já parecer ativa
./login_portal --force

# Especificar interface manualmente
./login_portal -i wlan0
```

---

## 💡 Como a Detecção em 3 Camadas Funciona

1. **Teste de Conectividade (`generate_204`)**:
   - No Wi-Fi de casa, o servidor responde `HTTP 204 No Content`. O programa encerra em poucos milissegundos sem fazer nada.
   - Na faculdade (bloqueado), o AP intercepta a conexão e retorna `HTTP 302`.
2. **Identificação da Assinatura UniFi**:
   - Na mesma verificação, segue os cabeçalhos `Location:` para confirmar se o destino é o portal UniFi (portas `8880`/`8882` e rota `/guest/s/`).
3. **Filtro de SSID via `ioctl`**:
   - Se a internet cair em uma rede residencial (ex: problema na operadora), o programa lê o SSID via kernel e reconhece que não é a rede da faculdade nem há portal UniFi, ignorando silenciosamente.

---

## 📄 Licença

MIT License. Sinta-se livre para usar, modificar e compartilhar.
