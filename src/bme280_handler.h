#pragma once

#include <cstdint>

#include "esp_err.h"

// Ett komplett mätvärde från BME280, i SI-nära enheter.
struct bme280_measurement_t {
    float temperature_c;  // grader Celsius
    float pressure_hpa;   // hektopascal
    float humidity_pct;   // relativ luftfuktighet, 0..100 %
};

// Initierar I2C-bussen, verifierar chip-ID, läser kalibrering och startar mätning.
// Vid fel frigörs alla resurser och funktionen kan anropas igen.
esp_err_t bme280_init(void);

// Läser och kompenserar ett mätvärde. `out` får inte vara nullptr.
esp_err_t bme280_read_data(bme280_measurement_t *out);
