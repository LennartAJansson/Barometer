#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "bme280_handler.h"
#include "nats_client.h"

// WiFi-uppgifter kommer från .env via read_env.py (se .env.example) – ligger inte i git
#if !defined(WIFI_SSID) || !defined(WIFI_PASS)
#error "WIFI_SSID/WIFI_PASS saknas. Skapa en .env i projektroten (se .env.example)."
#endif

// Fälten i wifi_config_t är fasta arrayer; fel längd ska fångas vid kompilering.
static_assert(sizeof(WIFI_SSID) - 1 <= sizeof(wifi_sta_config_t::ssid), "WIFI_SSID är för lång (max 32 tecken)");
static_assert(sizeof(WIFI_PASS) - 1 < sizeof(wifi_sta_config_t::password), "WIFI_PASS är för långt (max 63 tecken)");

namespace {

constexpr const char *TAG = "MAIN";

// ---- Konfiguration ----------------------------------------------------------
constexpr const char *kNatsServer = "192.168.50.254";
constexpr uint16_t kNatsPort = 4222;
constexpr const char *kNatsSubject = "sensors.bme280.raw";

constexpr TickType_t kSampleInterval = pdMS_TO_TICKS(10000);
constexpr TickType_t kWifiPollInterval = pdMS_TO_TICKS(1000);
constexpr TickType_t kNatsRetryDelay = pdMS_TO_TICKS(5000);
constexpr TickType_t kReconnectDelay = pdMS_TO_TICKS(2000);

// Stränghanteringen (snprintf) kräver en större stack än standard.
constexpr uint32_t kTelemetryStackBytes = 8192;
constexpr UBaseType_t kTelemetryPriority = 5;
constexpr BaseType_t kTelemetryCore = 1;

constexpr size_t kMacLen = 6;
constexpr size_t kIpStrLen = 16;  // "255.255.255.255" + '\0'
constexpr size_t kSenderIdSize = 64;
constexpr size_t kSensorValueSize = 128;
constexpr size_t kEnvelopeSize = 384;

// ---- Delat tillstånd (skrivs av event-tasken, läses av telemetry-tasken) ----
std::atomic<bool> s_wifi_connected{false};
esp_netif_t *s_sta_netif = nullptr;

void wifi_event_handler(void * /*arg*/, esp_event_base_t event_base, int32_t event_id, void * /*event_data*/) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        ESP_LOGW(TAG, "WiFi bortkopplat! Försöker ansluta igen...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_connected = true;
        ESP_LOGI(TAG, "WiFi anslutet! IP-adress erhållen.");
    }
}

void wifi_init() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr, nullptr));

    wifi_config_t wifi_config = {};
    std::memcpy(wifi_config.sta.ssid, WIFI_SSID, sizeof(WIFI_SSID) - 1);
    std::memcpy(wifi_config.sta.password, WIFI_PASS, sizeof(WIFI_PASS) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// Skriver "MAC=XX:XX:XX:XX:XX:XX,IP=YYY.YYY.YYY.YYY" till buffern.
bool format_sender_identity(char *out, size_t out_size) {
    uint8_t mac[kMacLen] = {};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) {
        ESP_LOGW(TAG, "Kunde inte läsa MAC-adress");
    }

    char ip_str[kIpStrLen] = "0.0.0.0";
    esp_netif_ip_info_t ip_info;
    if (s_sta_netif != nullptr && esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK) {
        esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
    }

    const int n = std::snprintf(out, out_size, "MAC=%02X:%02X:%02X:%02X:%02X:%02X,IP=%s",
                                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], ip_str);
    return n > 0 && static_cast<size_t>(n) < out_size;
}

// Bygger meddelandet: ett kuvert där "value" är mätvärdes-JSON som escapad sträng
// (formatet konsumenterna redan förväntar sig).
bool build_envelope(char *out, size_t out_size, const char *sender, const bme280_measurement_t &m) {
    char sensor_value[kSensorValueSize];
    int n = std::snprintf(sensor_value, sizeof(sensor_value),
                          "{\\\"hpa\\\":%.2f,\\\"temp\\\":%.2f,\\\"humidity\\\":%.2f}",
                          static_cast<double>(m.pressure_hpa),
                          static_cast<double>(m.temperature_c),
                          static_cast<double>(m.humidity_pct));
    if (n < 0 || static_cast<size_t>(n) >= sizeof(sensor_value)) {
        return false;
    }

    n = std::snprintf(out, out_size, "{\"type\":\"bme280\",\"sender\":\"%s\",\"value\":\"%s\"}", sender, sensor_value);
    return n > 0 && static_cast<size_t>(n) < out_size;
}

void telemetry_task(void * /*pvParameters*/) {
    ESP_LOGI(TAG, "Telemetry-task startad.");

    while (true) {
        if (!s_wifi_connected) {
            vTaskDelay(kWifiPollInterval);
            continue;
        }

        if (nats_connect(kNatsServer, kNatsPort) != ESP_OK) {
            vTaskDelay(kNatsRetryDelay);
            continue;
        }

        while (s_wifi_connected) {
            bme280_measurement_t measurement = {};
            const esp_err_t err = bme280_read_data(&measurement);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Kunde inte läsa BME280: %s", esp_err_to_name(err));
            } else {
                char sender_identity[kSenderIdSize];
                char envelope[kEnvelopeSize];
                if (!format_sender_identity(sender_identity, sizeof(sender_identity)) ||
                    !build_envelope(envelope, sizeof(envelope), sender_identity, measurement)) {
                    ESP_LOGE(TAG, "Meddelandet ryms inte i bufferten – hoppar över mätningen.");
                } else if (nats_publish(kNatsSubject, envelope) != ESP_OK) {
                    ESP_LOGE(TAG, "Publicering misslyckades. Bryter för återanslutning.");
                    break;
                }
            }
            vTaskDelay(kSampleInterval);
        }

        nats_disconnect();
        vTaskDelay(kReconnectDelay);
    }
}

}  // namespace

extern "C" void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init();

    const esp_err_t sensor_err = bme280_init();
    if (sensor_err != ESP_OK) {
        ESP_LOGE(TAG, "Hårdvarufel: kunde inte initiera BME280: %s", esp_err_to_name(sensor_err));
        return;
    }

    const BaseType_t created = xTaskCreatePinnedToCore(telemetry_task, "telemetry_io", kTelemetryStackBytes,
                                                       nullptr, kTelemetryPriority, nullptr, kTelemetryCore);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Kunde inte skapa telemetry-task");
        return;
    }

    ESP_LOGI(TAG, "app_main slutförd.");
}
