#pragma once

#include <cstdint>

#include "esp_err.h"

// Enkel, enkeltrådad NATS-klient. Alla funktioner ska anropas från samma task.
esp_err_t nats_connect(const char *host, uint16_t port);

// Besvarar eventuella server-PING och publicerar sedan `payload` på `subject`.
esp_err_t nats_publish(const char *subject, const char *payload);

void nats_disconnect(void);
