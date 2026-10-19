/*
 * login_portal.c - Daemon e CLI ultra-leve em C para auto-login no portal captivo UniFi (UNICEP)
 *
 * Recursos utilizados:
 *  - Sockets POSIX puros com SO_BINDTODEVICE (contorna VPN/WireGuard sem subprocessos)
 *  - Netlink Route Socket (AF_NETLINK) para escutar eventos de rede direto do Kernel Linux
 *  - ioctl Wireless Extensions (SIOCGIWESSID) para ler o SSID sem invocar nmcli
 *  - Zero bibliotecas externas (apenas libc padrão do Linux)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <dirent.h>
#include <getopt.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <linux/wireless.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#define CHECK_HOST "connectivitycheck.gstatic.com"
#define CHECK_PATH "/generate_204"
#define CHECK_FALLBACK_IP "142.251.150.120"

#define DEFAULT_FALLBACK_PORTAL "http://10.0.1.2:8880/guest/s/default/"
#define MAX_URL_LEN 2048
#define MAX_REDIRECTS 6

static volatile sig_atomic_t g_running = 1;

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

/* Envia notificação desktop via notify-send (chamado apenas quando efetua login) */
static void notify_desktop(const char *title, const char *message, const char *icon) {
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("notify-send", "notify-send", "-a", "Portal Wi-Fi", "-i", icon, title, message, (char *)NULL);
        _exit(0);
    } else if (pid > 0) {
        waitpid(pid, NULL, 0);
    }
}

/* Verifica se a interface possui endereço IPv4 atribuído */
static bool iface_has_ipv4(const char *iface) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    bool has_ip = (ioctl(fd, SIOCGIFADDR, &ifr) == 0);
    close(fd);
    return has_ip;
}

/* Obtém o SSID da interface Wi-Fi usando ioctl SIOCGIWESSID direto no Kernel */
static bool get_active_ssid(const char *iface, char *out_ssid, size_t maxlen) {
    out_ssid[0] = '\0';
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;

    struct iwreq wreq;
    memset(&wreq, 0, sizeof(wreq));
    strncpy(wreq.ifr_name, iface, IFNAMSIZ - 1);

    char buf[IW_ESSID_MAX_SIZE + 1];
    memset(buf, 0, sizeof(buf));
    wreq.u.essid.pointer = buf;
    wreq.u.essid.length = IW_ESSID_MAX_SIZE;

    bool ok = false;
    if (ioctl(fd, SIOCGIWESSID, &wreq) == 0 && buf[0] != '\0') {
        snprintf(out_ssid, maxlen, "%s", buf);
        ok = true;
    }
    close(fd);
    return ok;
}

/* Detecta automaticamente a interface Wi-Fi ativa varrendo /sys/class/net */
static void get_active_wifi_interface(char *out_iface, size_t maxlen) {
    snprintf(out_iface, maxlen, "wlan0");
    DIR *dir = opendir("/sys/class/net");
    if (!dir) return;

    char fallback[IFNAMSIZ] = {0};
    char candidate_with_ip[IFNAMSIZ] = {0};
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        /* Ignora interfaces virtuais conhecidas de hotspot/p2p */
        if (strncmp(entry->d_name, "p2p", 3) == 0) continue;

        char path1[512], path2[512];
        snprintf(path1, sizeof(path1), "/sys/class/net/%s/wireless", entry->d_name);
        snprintf(path2, sizeof(path2), "/sys/class/net/%s/phy80211", entry->d_name);

        struct stat st;
        if (stat(path1, &st) == 0 || stat(path2, &st) == 0) {
            char ssid[IW_ESSID_MAX_SIZE + 1];
            bool has_ssid = get_active_ssid(entry->d_name, ssid, sizeof(ssid));
            bool has_ip = iface_has_ipv4(entry->d_name);

            /* Interface conectada a uma rede Wi-Fi como cliente com IP ativo */
            if (has_ssid && has_ip) {
                snprintf(out_iface, maxlen, "%.*s", (int)(maxlen - 1), entry->d_name);
                closedir(dir);
                return;
            }

            if (has_ip && candidate_with_ip[0] == '\0') {
                snprintf(candidate_with_ip, sizeof(candidate_with_ip), "%.*s", (int)(sizeof(candidate_with_ip) - 1), entry->d_name);
            }

            if (fallback[0] == '\0') {
                snprintf(fallback, sizeof(fallback), "%.*s", IFNAMSIZ - 1, entry->d_name);
            }
        }
    }
    closedir(dir);

    /* Se wlan0 estiver presente, prefere ela a interfaces de tethering (como ap0) */
    if (fallback[0] != '\0') {
        if (candidate_with_ip[0] != '\0' && strncmp(candidate_with_ip, "ap", 2) != 0) {
            snprintf(out_iface, maxlen, "%.*s", (int)(maxlen - 1), candidate_with_ip);
        } else {
            snprintf(out_iface, maxlen, "%.*s", (int)(maxlen - 1), fallback);
        }
    }
}

/* Verifica se o SSID tem relação com a rede da faculdade */
static bool is_likely_college_network(const char *ssid) {
    if (!ssid || ssid[0] == '\0') return true;
    const char *keywords[] = {"alunos", "unicep", "livre", "faculdade", NULL};
    for (int i = 0; keywords[i] != NULL; i++) {
        if (strcasestr(ssid, keywords[i]) != NULL) {
            return true;
        }
    }
    return false;
}

/* Estrutura simples para URL HTTP */
typedef struct {
    char host[256];
    int port;
    char path[MAX_URL_LEN];
} parsed_url_t;

static bool parse_http_url(const char *url, parsed_url_t *out) {
    memset(out, 0, sizeof(*out));
    out->port = 80;

    const char *p = url;
    if (strncasecmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (strncasecmp(p, "https://", 8) == 0) {
        return false; /* Portal UniFi e generate_204 operam em HTTP */
    }

    const char *slash = strchr(p, '/');
    size_t host_port_len = slash ? (size_t)(slash - p) : strlen(p);
    if (host_port_len == 0 || host_port_len >= sizeof(out->host)) return false;

    char host_port[256];
    memcpy(host_port, p, host_port_len);
    host_port[host_port_len] = '\0';

    char *colon = strchr(host_port, ':');
    if (colon) {
        *colon = '\0';
        out->port = atoi(colon + 1);
        if (out->port <= 0 || out->port > 65535) out->port = 80;
    }
    snprintf(out->host, sizeof(out->host), "%s", host_port);

    if (slash) {
        snprintf(out->path, sizeof(out->path), "%s", slash);
    } else {
        snprintf(out->path, sizeof(out->path), "/");
    }
    return true;
}

/* Abre conexão TCP não-bloqueante vinculada à interface Wi-Fi (SO_BINDTODEVICE) */
static int connect_bound_tcp(const char *host, int port, const char *iface, const char *fallback_ip, int timeout_ms) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    /* Se já for IP literal, converte direto sem precisar de DNS */
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        bool resolved = false;
        /* Se temos fallback_ip para o connectivitycheck, podemos tentar DNS rápido ou usar o IP direto */
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(host, NULL, &hints, &res) == 0 && res != NULL) {
            struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
            addr.sin_addr = sin->sin_addr;
            resolved = true;
            freeaddrinfo(res);
        } else if (fallback_ip && inet_pton(AF_INET, fallback_ip, &addr.sin_addr) == 1) {
            resolved = true;
        }
        if (!resolved) return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    if (iface && iface[0] != '\0') {
        if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, iface, (socklen_t)strlen(iface) + 1) < 0) {
            /* Ignora erro caso não permitido, mas no Linux 5.7+ funciona sem root */
        }
    }

    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    if (rc < 0) {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        if (poll(&pfd, 1, timeout_ms) <= 0) {
            close(fd);
            return -1;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
            close(fd);
            return -1;
        }
    }

    /* Restaura para modo bloqueante com timeout de leitura/escrita */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    if (tv.tv_sec == 0 && tv.tv_usec == 0) tv.tv_sec = 3;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    return fd;
}

/* Executa uma requisição HTTP e extrai o código de status e o header Location (se houver) */
static int http_request(
    const char *method,
    const char *url,
    const char *referer,
    const char *iface,
    const char *fallback_ip,
    int timeout_ms,
    char *out_location,
    size_t loc_size
) {
    if (out_location && loc_size > 0) out_location[0] = '\0';

    parsed_url_t u;
    if (!parse_http_url(url, &u)) return 0;

    int fd = connect_bound_tcp(u.host, u.port, iface, fallback_ip, timeout_ms);
    if (fd < 0) return 0;

    char req[4096];
    int len;
    if (u.port != 80) {
        len = snprintf(req, sizeof(req),
            "%s %s HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36\r\n"
            "Accept: */*\r\n"
            "%s%s%s"
            "%s"
            "Connection: close\r\n\r\n",
            method, u.path, u.host, u.port,
            referer ? "Referer: " : "", referer ? referer : "", referer ? "\r\n" : "",
            strcmp(method, "POST") == 0 ? "Content-Length: 0\r\n" : ""
        );
    } else {
        len = snprintf(req, sizeof(req),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36\r\n"
            "Accept: */*\r\n"
            "%s%s%s"
            "%s"
            "Connection: close\r\n\r\n",
            method, u.path, u.host,
            referer ? "Referer: " : "", referer ? referer : "", referer ? "\r\n" : "",
            strcmp(method, "POST") == 0 ? "Content-Length: 0\r\n" : ""
        );
    }

    if (send(fd, req, (size_t)len, MSG_NOSIGNAL) < 0) {
        close(fd);
        return 0;
    }

    char resp[4096];
    size_t total = 0;
    while (total < sizeof(resp) - 1) {
        ssize_t n = recv(fd, resp + total, sizeof(resp) - 1 - total, 0);
        if (n <= 0) break;
        total += (size_t)n;
        resp[total] = '\0';
        if (strstr(resp, "\r\n\r\n") != NULL) break;
    }
    close(fd);

    if (total == 0) return 0;
    resp[total] = '\0';

    int status_code = 0;
    if (strncmp(resp, "HTTP/1.", 7) == 0) {
        const char *sp = strchr(resp, ' ');
        if (sp) status_code = atoi(sp + 1);
    }

    if (out_location && loc_size > 0) {
        char *line = resp;
        while (line && *line) {
            char *next = strstr(line, "\r\n");
            if (next) *next = '\0';

            if (strncasecmp(line, "Location:", 9) == 0) {
                const char *val = line + 9;
                while (*val == ' ' || *val == '\t') val++;
                snprintf(out_location, loc_size, "%s", val);
                break;
            }
            if (!next) break;
            line = next + 2;
            if (*line == '\r' || *line == '\n' || *line == '\0') break;
        }
    }

    return status_code;
}

/*
 * Verifica conectividade e já descobre o portal seguindo redirecionamentos se houver.
 * Retorna o código HTTP inicial do generate_204 (204 = internet livre).
 */
static int check_and_discover(const char *iface, char *out_portal_url, size_t portal_size) {
    if (out_portal_url && portal_size > 0) out_portal_url[0] = '\0';

    char current_url[MAX_URL_LEN];
    snprintf(current_url, sizeof(current_url), "http://%s%s", CHECK_HOST, CHECK_PATH);

    char location[MAX_URL_LEN];
    int first_code = http_request("GET", current_url, NULL, iface, CHECK_FALLBACK_IP, 3000, location, sizeof(location));
    if (first_code == 204) {
        return 204;
    }

    /* Se houve redirecionamento (301/302/303/307/308), segue a cadeia até encontrar o portal */
    int code = first_code;
    for (int hop = 0; hop < MAX_REDIRECTS && (code >= 300 && code < 400) && location[0] != '\0'; hop++) {
        snprintf(current_url, sizeof(current_url), "%s", location);
        if (strstr(current_url, "8880") != NULL || strstr(current_url, "/guest/s/") != NULL) {
            if (out_portal_url) snprintf(out_portal_url, portal_size, "%s", current_url);
            return first_code;
        }
        code = http_request("GET", current_url, NULL, iface, NULL, 3000, location, sizeof(location));
    }

    if (strstr(current_url, "8880") != NULL || strstr(current_url, "8882") != NULL || strstr(current_url, "/guest/s/") != NULL) {
        if (out_portal_url) snprintf(out_portal_url, portal_size, "%s", current_url);
    }

    return first_code;
}

/* Executa o POST de login no portal UniFi */
static int perform_login(const char *portal_url, const char *iface, bool quiet) {
    char working_url[MAX_URL_LEN];
    snprintf(working_url, sizeof(working_url), "%s", portal_url);

    /* Se ainda estiver no redirect da porta 8882, segue mais um salto para obter a URL da 8880 */
    if (strstr(working_url, ":8882") != NULL || strstr(working_url, "/redirect") != NULL) {
        char next_loc[MAX_URL_LEN];
        int code = http_request("GET", working_url, NULL, iface, NULL, 4000, next_loc, sizeof(next_loc));
        if (code >= 300 && code < 400 && next_loc[0] != '\0') {
            snprintf(working_url, sizeof(working_url), "%s", next_loc);
        }
    }

    parsed_url_t u;
    if (!parse_http_url(working_url, &u)) return 0;

    /* Remove query string do path para achar o diretório base (/guest/s/default/) */
    char base_path[MAX_URL_LEN];
    snprintf(base_path, sizeof(base_path), "%s", u.path);
    char *q = strchr(base_path, '?');
    if (q) *q = '\0';

    char *last_slash = strrchr(base_path, '/');
    if (last_slash) {
        *(last_slash + 1) = '\0';
    } else {
        snprintf(base_path, sizeof(base_path), "/");
    }

    char login_url[MAX_URL_LEN + 512];
    snprintf(login_url, sizeof(login_url), "http://%s:%d%slogin", u.host, u.port, base_path);

    if (!quiet) {
        printf("[*] Enviando requisição de login para: %s\n", login_url);
        printf("[*] Referer: %s\n", working_url);
    }

    return http_request("POST", login_url, working_url, iface, NULL, 5000, NULL, 0);
}

/* Fluxo completo de verificação e login */
static bool attempt_login(const char *iface, bool force, bool quiet) {
    char ssid[IW_ESSID_MAX_SIZE + 1];
    bool has_ssid = get_active_ssid(iface, ssid, sizeof(ssid));

    if (!quiet) {
        printf("[i] Interface: %s (SSID: %s)\n", iface, has_ssid ? ssid : "Desconhecido");
    }

    char portal_url[MAX_URL_LEN] = {0};

    if (!force) {
        int code = check_and_discover(iface, portal_url, sizeof(portal_url));
        if (code == 204) {
            if (!quiet) {
                printf("[✓] Conexão com a internet já está ativa (HTTP 204). Login desnecessário.\n");
            }
            return true;
        }
        if (!quiet) {
            printf("[!] Sem internet livre (HTTP %d). Verificando portal captivo...\n", code);
        }
    } else {
        if (!quiet) {
            printf("[!] Modo --force: enviando requisição de login...\n");
        }
        check_and_discover(iface, portal_url, sizeof(portal_url));
    }

    if (portal_url[0] == '\0') {
        if (!force && !is_likely_college_network(has_ssid ? ssid : NULL)) {
            if (!quiet) {
                printf("[i] Rede '%s' não possui portal captivo da faculdade detectado. Ignorando.\n",
                       has_ssid ? ssid : iface);
            }
            return true;
        }
        if (!quiet) {
            printf("[!] Redirecionamento não detectado. Usando fallback: %s\n", DEFAULT_FALLBACK_PORTAL);
        }
        snprintf(portal_url, sizeof(portal_url), "%s", DEFAULT_FALLBACK_PORTAL);
    } else {
        if (!quiet) {
            printf("[✓] Portal detectado em: %s\n", portal_url);
        }
    }

    int login_code = perform_login(portal_url, iface, quiet);
    if (!quiet) {
        printf("[i] Resposta do login: HTTP %d\n", login_code);
    }

    /*
     * Aguarda e verifica com retentativas se o AP UniFi liberou o tráfego.
     * Em redes corporativas com múltiplos APs, a controladora pode levar entre 2 a 6 segundos
     * para propagar as regras de firewall (ebtables/iptables) para o rádio físico.
     */
    int after_code = 0;
    bool liberated = false;
    for (int attempt = 1; attempt <= 5; attempt++) {
        usleep(1200000); /* 1.2s entre cada checagem (total de até 6s) */
        after_code = check_and_discover(iface, NULL, 0);
        if (after_code == 204) {
            liberated = true;
            break;
        }
        if (!quiet) {
            printf("[...] Aguardando propagação no AP (tentativa %d/5: HTTP %d)...\n", attempt, after_code);
        }
    }

    if (liberated) {
        if (!quiet) {
            printf("[✓] Login efetuado com sucesso! Internet liberada.\n");
        }
        char msg[256];
        snprintf(msg, sizeof(msg), "Login efetuado com sucesso na rede %s!", has_ssid ? ssid : "da faculdade");
        notify_desktop("Wi-Fi Conectado", msg, "network-wireless");
        return true;
    } else {
        if (!quiet) {
            printf("[✗] Falha na liberação da internet após login (HTTP %d).\n", after_code);
        }
        char msg[256];
        snprintf(msg, sizeof(msg), "Não foi possível liberar o acesso na rede %s.", has_ssid ? ssid : "da faculdade");
        notify_desktop("Falha no Login Wi-Fi", msg, "dialog-error");
        return false;
    }
}

/*
 * Modo Daemon ultra-leve:
 * Escuta eventos de rede diretamente do Kernel Linux via Netlink (RTMGRP_LINK | RTMGRP_IPV4_IFADDR)
 * sem precisar de nenhum subprocesso como 'nmcli monitor'.
 */
static int run_daemon(const char *user_iface, int interval_sec) {
    printf("[+] Iniciando daemon C ultra-leve (intervalo periódico: %ds)...\n", interval_sec);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    char active_iface[IFNAMSIZ];
    if (user_iface) {
        snprintf(active_iface, sizeof(active_iface), "%s", user_iface);
    } else {
        get_active_wifi_interface(active_iface, sizeof(active_iface));
    }

    if (iface_has_ipv4(active_iface)) {
        attempt_login(active_iface, false, false);
    }
    time_t last_check = time(NULL);

    int nl_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (nl_fd >= 0) {
        struct sockaddr_nl sa;
        memset(&sa, 0, sizeof(sa));
        sa.nl_family = AF_NETLINK;
        sa.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR;
        if (bind(nl_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
            close(nl_fd);
            nl_fd = -1;
        }
    }

    while (g_running) {
        struct pollfd pfd = { .fd = nl_fd, .events = POLLIN };
        int rc = (nl_fd >= 0) ? poll(&pfd, 1, interval_sec * 1000) : 0;
        if (!g_running) break;

        if (nl_fd < 0) {
            sleep((unsigned int)interval_sec);
        }

        time_t now = time(NULL);

        if (rc > 0 && (pfd.revents & POLLIN)) {
            char buf[4096];
            bool addr_event = false;
            ssize_t len;
            while ((len = recv(nl_fd, buf, sizeof(buf), 0)) > 0) {
                struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
                for (; NLMSG_OK(nlh, (unsigned int)len); nlh = NLMSG_NEXT(nlh, len)) {
                    if (nlh->nlmsg_type == RTM_NEWADDR || nlh->nlmsg_type == RTM_NEWLINK) {
                        addr_event = true;
                    }
                }
            }

            /* Cooldown de 8 segundos para evitar disparos repetidos durante negociação DHCP */
            if (addr_event && (now - last_check >= 8)) {
                usleep(1500000); /* Aguarda 1.5s para rota e IP estabilizarem */
                if (!user_iface) {
                    get_active_wifi_interface(active_iface, sizeof(active_iface));
                }
                if (iface_has_ipv4(active_iface)) {
                    printf("[*] Evento de rede detectado no kernel (%s). Verificando...\n", active_iface);
                    attempt_login(active_iface, false, false);
                    last_check = time(NULL);
                }
            }
        } else if (rc == 0) {
            /* Timeout periódico */
            if (!user_iface) {
                get_active_wifi_interface(active_iface, sizeof(active_iface));
            }
            if (iface_has_ipv4(active_iface)) {
                char portal_url[MAX_URL_LEN] = {0};
                int code = check_and_discover(active_iface, portal_url, sizeof(portal_url));
                if (code != 204) {
                    printf("[*] Checagem periódica detectou bloqueio (HTTP %d). Autenticando...\n", code);
                    attempt_login(active_iface, false, false);
                }
            }
            last_check = time(NULL);
        }
    }

    if (nl_fd >= 0) close(nl_fd);
    printf("\n[+] Daemon encerrado.\n");
    return 0;
}

static void print_usage(const char *prog) {
    printf("Uso: %s [OPÇÕES]\n\n"
           "Auto-login ultra-leve em C para o portal captivo UniFi (UNICEP).\n\n"
           "Opções:\n"
           "  -i, --interface IFACE   Interface de rede Wi-Fi (ex: wlan0)\n"
           "  -f, --force             Força o envio do login mesmo se a internet parecer ativa\n"
           "  -q, --quiet             Modo silencioso (sem logs informativos)\n"
           "  -d, --daemon            Executa continuamente em background via Netlink socket\n"
           "      --interval SEG      Intervalo de checagem periódica no modo daemon (padrão: 60)\n"
           "  -h, --help              Exibe esta mensagem de ajuda\n", prog);
}

int main(int argc, char *argv[]) {
    /* Line-buffering para logs imediatos no systemd/journalctl */
    setvbuf(stdout, NULL, _IOLBF, 0);

    const char *user_iface = NULL;
    bool force = false;
    bool quiet = false;
    bool daemon_mode = false;
    int interval = 60;

    static struct option long_opts[] = {
        {"interface", required_argument, 0, 'i'},
        {"force",     no_argument,       0, 'f'},
        {"quiet",     no_argument,       0, 'q'},
        {"daemon",    no_argument,       0, 'd'},
        {"interval",  required_argument, 0, 't'},
        {"help",      no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "i:fqdh", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'i': user_iface = optarg; break;
            case 'f': force = true; break;
            case 'q': quiet = true; break;
            case 'd': daemon_mode = true; break;
            case 't': interval = atoi(optarg); if (interval < 5) interval = 5; break;
            case 'h': print_usage(argv[0]); return 0;
            default:  print_usage(argv[0]); return 1;
        }
    }

    if (daemon_mode) {
        return run_daemon(user_iface, interval);
    }

    char active_iface[IFNAMSIZ];
    if (user_iface) {
        snprintf(active_iface, sizeof(active_iface), "%s", user_iface);
    } else {
        get_active_wifi_interface(active_iface, sizeof(active_iface));
    }

    return attempt_login(active_iface, force, quiet) ? 0 : 1;
}
