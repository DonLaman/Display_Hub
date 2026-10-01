// DhNetClient — vedi DhNetClient.h
#include "DhNetClient.h"

#include <errno.h>
#include <fcntl.h>

#include "lwip/sockets.h"

int DhNetClient::connect(IPAddress ip, uint16_t port, int32_t timeout_ms) {
    _path = DhVpn::pathFor(ip, true);
    if (!_path.via_vpn) return WiFiClient::connect(ip, port, timeout_ms);

    String vpn_ip = DhVpn::vpnIp();
    IPAddress local;
    if (!vpn_ip.length() || !local.fromString(vpn_ip)) return WiFiClient::connect(ip, port, timeout_ms);

    // Tunnel verso il dispositivo che trasporta il traffico: attesa entro il
    // timeout di connessione (i primi pacchetti andrebbero persi e la connect
    // fallirebbe subito, vedi DhVpn::prepareTunnel).
    uint32_t budget = timeout_ms > 0 ? (uint32_t)timeout_ms : 5000;
    uint32_t start = millis();
    while (_path.tunnel == DhVpn::Tunnel::Waking && millis() - start < budget) {
        delay(100);
        _path.tunnel = DhVpn::prepareTunnel(_path.peer_ip);
    }
    uint32_t spent = millis() - start;
    int32_t remaining = (int32_t)budget - (int32_t)spent;
    if (remaining < 1000) remaining = 1000;  // prova comunque, come microlink_tcp_connect

    int fd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        log_e("socket: errno %d", errno);
        return 0;
    }

    struct sockaddr_in src;
    memset(&src, 0, sizeof(src));
    src.sin_family = AF_INET;
    src.sin_port = 0;  // porta effimera
    src.sin_addr.s_addr = (uint32_t)local;  // IPAddress -> uint32 in ordine di rete
    if (lwip_bind(fd, (struct sockaddr*)&src, sizeof(src)) != 0) {
        log_e("bind a %s: errno %d", vpn_ip.c_str(), errno);
        lwip_close(fd);
        return 0;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    dst.sin_addr.s_addr = (uint32_t)ip;

    // connect non bloccante con timeout (stesso schema di WiFiClient::connect)
    lwip_fcntl(fd, F_SETFL, lwip_fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int res = lwip_connect(fd, (struct sockaddr*)&dst, sizeof(dst));
    if (res < 0 && errno != EINPROGRESS) {
        log_e("connect %s:%u via VPN: errno %d", ip.toString().c_str(), port, errno);
        lwip_close(fd);
        return 0;
    }
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    struct timeval tv;
    tv.tv_sec = remaining / 1000;
    tv.tv_usec = (remaining % 1000) * 1000;
    res = lwip_select(fd + 1, nullptr, &wfds, nullptr, &tv);
    if (res <= 0) {
        log_w("connect %s:%u via VPN: %s", ip.toString().c_str(), port, res == 0 ? "timeout" : "errore select");
        lwip_close(fd);
        return 0;
    }
    int sock_err = 0;
    socklen_t len = sizeof(sock_err);
    if (lwip_getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_err, &len) != 0 || sock_err != 0) {
        log_w("connect %s:%u via VPN: errno %d", ip.toString().c_str(), port, sock_err);
        lwip_close(fd);
        return 0;
    }

    // Timeout di lettura/scrittura come WiFiClient (poi HTTPClient li regola
    // con setTimeout), e di nuovo bloccante.
    tv.tv_sec = 3;  // come WIFI_CLIENT_DEF_CONN_TIMEOUT_MS del core (definita solo in WiFiClient.cpp)
    tv.tv_usec = 0;
    lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    lwip_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    lwip_fcntl(fd, F_SETFL, lwip_fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);

    // WiFiClient(fd) avvolge un socket già connesso; l'assegnazione copia i
    // puntatori condivisi, e il temporaneo rilascia il suo senza chiudere il socket.
    WiFiClient::operator=(WiFiClient(fd));
    return 1;
}
