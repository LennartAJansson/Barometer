#include "nats_client.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "lwip/sockets.h"

namespace {

constexpr const char *TAG = "NATS";

constexpr int kSocketTimeoutSec = 3;
constexpr size_t kInfoBufferSize = 1024;
constexpr size_t kRxBufferSize = 512;
constexpr size_t kHeaderBufferSize = 128;

constexpr char kConnectCmd[] =
    "CONNECT {\"verbose\":false,\"pedantic\":false,\"tls_required\":false,\"name\":\"esp32-s3-barometer\"}\r\n";
constexpr char kPongCmd[] = "PONG\r\n";
constexpr char kCrLf[] = "\r\n";

int s_sock = -1;

bool would_block() {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

// Skickar hela bufferten (send() kan skicka färre byte än begärt).
esp_err_t send_all(const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        const ssize_t n = send(s_sock, data + sent, len - sent, 0);
        if (n < 0) {
            ESP_LOGE(TAG, "send misslyckades: errno %d", errno);
            return ESP_FAIL;
        }
        sent += static_cast<size_t>(n);
    }
    return ESP_OK;
}

// Läser allt som servern skickat sedan sist (icke-blockerande) och svarar på PING.
esp_err_t service_incoming() {
    char buf[kRxBufferSize];
    for (;;) {
        const ssize_t len = recv(s_sock, buf, sizeof(buf) - 1, MSG_DONTWAIT);
        if (len > 0) {
            buf[len] = '\0';
            if (std::strncmp(buf, "PING", 4) == 0) {
                if (send_all(kPongCmd, sizeof(kPongCmd) - 1) != ESP_OK) {
                    return ESP_FAIL;
                }
                ESP_LOGD(TAG, "PING -> PONG");
            } else if (std::strncmp(buf, "-ERR", 4) == 0) {
                ESP_LOGW(TAG, "Serverfel: %s", buf);
            } else {
                ESP_LOGD(TAG, "Mottog: %s", buf);
            }
            continue;
        }
        if (len == 0) {
            ESP_LOGW(TAG, "Servern stängde anslutningen (EOF)");
            return ESP_FAIL;
        }
        if (would_block()) {
            return ESP_OK;  // inget mer att läsa just nu
        }
        ESP_LOGE(TAG, "recv misslyckades: errno %d", errno);
        return ESP_FAIL;
    }
}

}  // namespace

esp_err_t nats_connect(const char *host, uint16_t port) {
    if (host == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_sock >= 0) {
        nats_disconnect();
    }

    sockaddr_in dest_addr = {};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &dest_addr.sin_addr) != 1) {
        ESP_LOGE(TAG, "Ogiltig IPv4-adress: %s", host);
        return ESP_ERR_INVALID_ARG;
    }

    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        ESP_LOGE(TAG, "Kunde inte skapa socket: errno %d", errno);
        return ESP_FAIL;
    }

    timeval timeout = {};
    timeout.tv_sec = kSocketTimeoutSec;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    if (connect(fd, reinterpret_cast<const sockaddr *>(&dest_addr), sizeof(dest_addr)) != 0) {
        ESP_LOGE(TAG, "Kunde inte ansluta till NATS: errno %d", errno);
        close(fd);
        return ESP_FAIL;
    }
    s_sock = fd;

    // Servern skickar "INFO {...}\r\n" direkt efter anslutning.
    char info[kInfoBufferSize];
    const ssize_t len = recv(s_sock, info, sizeof(info) - 1, 0);
    if (len <= 0) {
        ESP_LOGE(TAG, "Fick ingen INFO från servern");
        nats_disconnect();
        return ESP_FAIL;
    }
    info[len] = '\0';
    if (std::strncmp(info, "INFO ", 5) != 0) {
        ESP_LOGE(TAG, "Oväntat svar från servern: %s", info);
        nats_disconnect();
        return ESP_FAIL;
    }

    if (send_all(kConnectCmd, sizeof(kConnectCmd) - 1) != ESP_OK) {
        nats_disconnect();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Ansluten till NATS %s:%u", host, static_cast<unsigned>(port));
    return ESP_OK;
}

esp_err_t nats_publish(const char *subject, const char *payload) {
    if (subject == nullptr || payload == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_sock < 0) {
        ESP_LOGE(TAG, "Publicering misslyckades: ingen aktiv anslutning");
        return ESP_ERR_INVALID_STATE;
    }

    if (service_incoming() != ESP_OK) {
        return ESP_FAIL;
    }

    const size_t payload_len = std::strlen(payload);
    char header[kHeaderBufferSize];
    const int header_len = std::snprintf(header, sizeof(header), "PUB %s %u\r\n", subject,
                                         static_cast<unsigned>(payload_len));
    if (header_len < 0 || static_cast<size_t>(header_len) >= sizeof(header)) {
        ESP_LOGE(TAG, "Subject för långt");
        return ESP_ERR_INVALID_SIZE;
    }

    if (send_all(header, static_cast<size_t>(header_len)) != ESP_OK ||
        send_all(payload, payload_len) != ESP_OK ||
        send_all(kCrLf, sizeof(kCrLf) - 1) != ESP_OK) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Publicerade till [%s]: %u bytes", subject, static_cast<unsigned>(payload_len));
    return ESP_OK;
}

void nats_disconnect(void) {
    if (s_sock < 0) {
        return;
    }
    shutdown(s_sock, SHUT_RDWR);
    close(s_sock);
    s_sock = -1;
    ESP_LOGI(TAG, "Frånkopplad från NATS");
}
