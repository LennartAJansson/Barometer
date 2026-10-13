#include "bme280_handler.h"

#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

namespace {

constexpr const char *TAG = "BME280";

// ---- Hårdvarukoppling -------------------------------------------------------
constexpr gpio_num_t kSdaPin = GPIO_NUM_4;
constexpr gpio_num_t kSclPin = GPIO_NUM_5;
constexpr uint16_t kI2cAddress = 0x76;  // 0x77 om SDO är kopplad till VDDIO
constexpr uint32_t kI2cSpeedHz = 100000;
constexpr int kI2cTimeoutMs = 100;
constexpr int kI2cPortAuto = -1;  // låt drivrutinen välja ledig port
constexpr uint8_t kGlitchIgnoreCount = 7;

// ---- Register enligt Bosch BME280-datablad ----------------------------------
constexpr uint8_t kRegChipId = 0xD0;
constexpr uint8_t kRegCalibTempPress = 0x88;  // 24 byte: T1..T3, P1..P9
constexpr uint8_t kRegCalibH1 = 0xA1;         // 1 byte
constexpr uint8_t kRegCalibH2ToH6 = 0xE1;     // 7 byte
constexpr uint8_t kRegCtrlHum = 0xF2;         // måste skrivas före ctrl_meas
constexpr uint8_t kRegCtrlMeas = 0xF4;
constexpr uint8_t kRegData = 0xF7;            // press[3], temp[3], hum[2]

constexpr uint8_t kChipIdBme280 = 0x60;       // BMP280 (0x58) saknar fuktgivare

constexpr size_t kCalibTempPressLen = 24;
constexpr size_t kCalibH2ToH6Len = 7;
constexpr size_t kDataLen = 8;

constexpr uint8_t kCtrlHumOversampling16 = 0x05;
// ctrl_meas: osrs_t = x2 (010), osrs_p = x16 (101), mode = normal (11)
constexpr uint8_t kCtrlMeasValue = (0x2u << 5) | (0x5u << 2) | 0x3u;

constexpr int32_t kAdcSkipped = 0x80000;      // värde när mätningen är avstängd/ej klar

// Fabrikskalibrering, unik per sensor.
struct CalibData {
    uint16_t dig_T1;
    int16_t dig_T2;
    int16_t dig_T3;
    uint16_t dig_P1;
    int16_t dig_P2;
    int16_t dig_P3;
    int16_t dig_P4;
    int16_t dig_P5;
    int16_t dig_P6;
    int16_t dig_P7;
    int16_t dig_P8;
    int16_t dig_P9;
    uint8_t dig_H1;
    int16_t dig_H2;
    uint8_t dig_H3;
    int16_t dig_H4;
    int16_t dig_H5;
    int8_t dig_H6;
};

CalibData s_calib = {};
i2c_master_bus_handle_t s_bus = nullptr;
i2c_master_dev_handle_t s_dev = nullptr;

// ---- Hjälpfunktioner --------------------------------------------------------
uint16_t le_u16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

int16_t le_s16(const uint8_t *p) {
    return static_cast<int16_t>(le_u16(p));
}

esp_err_t read_regs(uint8_t reg, uint8_t *buf, size_t len) {
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, kI2cTimeoutMs);
}

esp_err_t write_reg(uint8_t reg, uint8_t value) {
    const uint8_t frame[2] = {reg, value};
    return i2c_master_transmit(s_dev, frame, sizeof(frame), kI2cTimeoutMs);
}

esp_err_t load_calibration() {
    uint8_t tp[kCalibTempPressLen];
    ESP_RETURN_ON_ERROR(read_regs(kRegCalibTempPress, tp, sizeof(tp)), TAG, "kalibrering T/P");

    s_calib.dig_T1 = le_u16(&tp[0]);
    s_calib.dig_T2 = le_s16(&tp[2]);
    s_calib.dig_T3 = le_s16(&tp[4]);
    s_calib.dig_P1 = le_u16(&tp[6]);
    s_calib.dig_P2 = le_s16(&tp[8]);
    s_calib.dig_P3 = le_s16(&tp[10]);
    s_calib.dig_P4 = le_s16(&tp[12]);
    s_calib.dig_P5 = le_s16(&tp[14]);
    s_calib.dig_P6 = le_s16(&tp[16]);
    s_calib.dig_P7 = le_s16(&tp[18]);
    s_calib.dig_P8 = le_s16(&tp[20]);
    s_calib.dig_P9 = le_s16(&tp[22]);

    ESP_RETURN_ON_ERROR(read_regs(kRegCalibH1, &s_calib.dig_H1, 1), TAG, "kalibrering H1");

    uint8_t h[kCalibH2ToH6Len];
    ESP_RETURN_ON_ERROR(read_regs(kRegCalibH2ToH6, h, sizeof(h)), TAG, "kalibrering H2-H6");

    s_calib.dig_H2 = le_s16(&h[0]);
    s_calib.dig_H3 = h[2];
    // H4 och H5 delar register 0xE5 (12 bitar vardera, tecken i övre byten).
    s_calib.dig_H4 = static_cast<int16_t>((static_cast<int8_t>(h[3]) * 16) | (h[4] & 0x0F));
    s_calib.dig_H5 = static_cast<int16_t>((static_cast<int8_t>(h[5]) * 16) | (h[4] >> 4));
    s_calib.dig_H6 = static_cast<int8_t>(h[6]);
    return ESP_OK;
}

// ---- Heltalskompensation enligt Boschs referensimplementation ---------------
// Returnerar temperatur i 0.01 °C. t_fine delas med övriga kompenseringar.
int32_t compensate_temperature(int32_t adc_T, int32_t *t_fine) {
    const int32_t t1 = s_calib.dig_T1;
    const int32_t var1 = (((adc_T >> 3) - (t1 * 2)) * s_calib.dig_T2) >> 11;
    const int32_t d = (adc_T >> 4) - t1;
    const int32_t var2 = (((d * d) >> 12) * s_calib.dig_T3) >> 14;
    *t_fine = var1 + var2;
    return (*t_fine * 5 + 128) >> 8;
}

// Returnerar tryck i Pa.
uint32_t compensate_pressure(int32_t adc_P, int32_t t_fine) {
    int32_t var1 = (t_fine >> 1) - 64000;
    int32_t var2 = (((var1 >> 2) * (var1 >> 2)) >> 11) * s_calib.dig_P6;
    var2 += (var1 * s_calib.dig_P5) * 2;
    var2 = (var2 >> 2) + (static_cast<int32_t>(s_calib.dig_P4) * 65536);
    var1 = (((s_calib.dig_P3 * (((var1 >> 2) * (var1 >> 2)) >> 13)) >> 3) +
            ((s_calib.dig_P2 * var1) >> 1)) >> 18;
    var1 = ((32768 + var1) * s_calib.dig_P1) >> 15;
    if (var1 == 0) {
        return 0;  // undvik division med noll
    }

    uint32_t p = (static_cast<uint32_t>(1048576 - adc_P) - static_cast<uint32_t>(var2 >> 12)) * 3125u;
    if (p < 0x80000000u) {
        p = (p << 1) / static_cast<uint32_t>(var1);
    } else {
        p = (p / static_cast<uint32_t>(var1)) * 2u;
    }
    var1 = (s_calib.dig_P9 * static_cast<int32_t>(((p >> 3) * (p >> 3)) >> 13)) >> 12;
    var2 = (static_cast<int32_t>(p >> 2) * s_calib.dig_P8) >> 13;
    return static_cast<uint32_t>(static_cast<int32_t>(p) + ((var1 + var2 + s_calib.dig_P7) >> 4));
}

// Returnerar relativ fuktighet i Q22.10 (dela med 1024 för %RH).
uint32_t compensate_humidity(int32_t adc_H, int32_t t_fine) {
    int32_t v = t_fine - 76800;
    v = (((((adc_H * 16384) - (static_cast<int32_t>(s_calib.dig_H4) * 1048576) -
            (s_calib.dig_H5 * v)) + 16384) >> 15) *
         (((((((v * s_calib.dig_H6) >> 10) * (((v * s_calib.dig_H3) >> 11) + 32768)) >> 10) + 2097152) *
               s_calib.dig_H2 + 8192) >> 14));
    v -= (((((v >> 15) * (v >> 15)) >> 7) * s_calib.dig_H1) >> 4);
    if (v < 0) {
        v = 0;
    } else if (v > 419430400) {
        v = 419430400;
    }
    return static_cast<uint32_t>(v >> 12);
}

esp_err_t attach_and_configure() {
    i2c_device_config_t dev_config = {};
    dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_config.device_address = kI2cAddress;
    dev_config.scl_speed_hz = kI2cSpeedHz;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_config, &s_dev), TAG, "lägg till enhet");

    uint8_t chip_id = 0;
    ESP_RETURN_ON_ERROR(read_regs(kRegChipId, &chip_id, 1), TAG, "läs chip-ID");
    if (chip_id != kChipIdBme280) {
        ESP_LOGE(TAG, "Fel chip-ID 0x%02X (förväntade 0x%02X)", chip_id, kChipIdBme280);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(load_calibration(), TAG, "läs kalibrering");
    ESP_RETURN_ON_ERROR(write_reg(kRegCtrlHum, kCtrlHumOversampling16), TAG, "skriv ctrl_hum");
    ESP_RETURN_ON_ERROR(write_reg(kRegCtrlMeas, kCtrlMeasValue), TAG, "skriv ctrl_meas");
    return ESP_OK;
}

void release_resources() {
    if (s_dev != nullptr) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = nullptr;
    }
    if (s_bus != nullptr) {
        i2c_del_master_bus(s_bus);
        s_bus = nullptr;
    }
}

}  // namespace

esp_err_t bme280_init(void) {
    if (s_dev != nullptr) {
        return ESP_OK;  // redan initierad
    }

    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = kI2cPortAuto;
    bus_config.sda_io_num = kSdaPin;
    bus_config.scl_io_num = kSclPin;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = kGlitchIgnoreCount;
    bus_config.flags.enable_internal_pullup = true;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &s_bus), TAG, "skapa I2C-buss");

    const esp_err_t err = attach_and_configure();
    if (err != ESP_OK) {
        release_resources();
        return err;
    }

    ESP_LOGI(TAG, "BME280 initierad (adress 0x%02X)", kI2cAddress);
    return ESP_OK;
}

esp_err_t bme280_read_data(bme280_measurement_t *out) {
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_dev == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t raw[kDataLen];
    ESP_RETURN_ON_ERROR(read_regs(kRegData, raw, sizeof(raw)), TAG, "läs mätdata");

    const int32_t adc_P = static_cast<int32_t>((static_cast<uint32_t>(raw[0]) << 12) | (raw[1] << 4) | (raw[2] >> 4));
    const int32_t adc_T = static_cast<int32_t>((static_cast<uint32_t>(raw[3]) << 12) | (raw[4] << 4) | (raw[5] >> 4));
    const int32_t adc_H = static_cast<int32_t>((raw[6] << 8) | raw[7]);

    if (adc_P == kAdcSkipped || adc_T == kAdcSkipped) {
        return ESP_ERR_INVALID_STATE;  // första mätningen är inte klar än
    }

    int32_t t_fine = 0;
    out->temperature_c = static_cast<float>(compensate_temperature(adc_T, &t_fine)) / 100.0f;
    out->pressure_hpa = static_cast<float>(compensate_pressure(adc_P, t_fine)) / 100.0f;
    out->humidity_pct = static_cast<float>(compensate_humidity(adc_H, t_fine)) / 1024.0f;
    return ESP_OK;
}
