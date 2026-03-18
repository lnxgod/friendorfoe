/**
 * Friend or Foe -- Uplink Battery Monitor Implementation
 *
 * Reads Li-Ion battery voltage through a 2:1 voltage divider connected
 * to ADC1 on GPIO3.  Averages 10 samples and converts to percentage
 * using a 3-segment linear approximation.
 */

#include "battery.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

static const char *TAG = "battery";

/* GPIO3 on ESP32-C3 is ADC1 channel 3 */
#define BATTERY_ADC_UNIT        ADC_UNIT_1
#define BATTERY_ADC_CHANNEL     ADC_CHANNEL_3
#define BATTERY_ADC_ATTEN       ADC_ATTEN_DB_12   /* ~0-3.3V range */
#define BATTERY_ADC_BITWIDTH    ADC_BITWIDTH_12

/* Voltage divider ratio: 2:1 (actual voltage = measured * 2) */
#define VOLTAGE_DIVIDER_RATIO   2.0f

/* Number of samples to average */
#define NUM_SAMPLES             10

/* Li-Ion voltage thresholds */
#define VBAT_FULL               4.2f    /* 100% */
#define VBAT_NOMINAL            3.7f    /* 50%  */
#define VBAT_EMPTY              3.0f    /* 0%   */

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t         s_cali_handle = NULL;
static bool                      s_calibrated  = false;

/* ── Init ──────────────────────────────────────────────────────────────── */

void battery_init(void)
{
    /* Initialize ADC oneshot unit */
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = BATTERY_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle));

    /* Configure channel */
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = BATTERY_ADC_ATTEN,
        .bitwidth = BATTERY_ADC_BITWIDTH,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle,
                                               BATTERY_ADC_CHANNEL,
                                               &chan_cfg));

    /* Try to initialize calibration (curve fitting on C3) */
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = BATTERY_ADC_UNIT,
        .chan     = BATTERY_ADC_CHANNEL,
        .atten    = BATTERY_ADC_ATTEN,
        .bitwidth = BATTERY_ADC_BITWIDTH,
    };
    esp_err_t err = adc_cali_create_scheme_curve_fitting(&cali_cfg,
                                                          &s_cali_handle);
    if (err == ESP_OK) {
        s_calibrated = true;
        ESP_LOGI(TAG, "ADC calibration initialized (curve fitting)");
    } else {
        ESP_LOGW(TAG, "ADC calibration not available: %s",
                 esp_err_to_name(err));
    }
#else
    ESP_LOGW(TAG, "ADC curve fitting calibration not supported on this chip");
#endif

    ESP_LOGI(TAG, "Battery monitor initialized (GPIO3, ADC1_CH3)");
}

/* ── Read raw millivolts ───────────────────────────────────────────────── */

static float read_voltage_mv(void)
{
    int total = 0;
    int valid_samples = 0;

    for (int i = 0; i < NUM_SAMPLES; i++) {
        int raw = 0;
        esp_err_t err = adc_oneshot_read(s_adc_handle, BATTERY_ADC_CHANNEL, &raw);
        if (err != ESP_OK) {
            continue;
        }

        if (s_calibrated && s_cali_handle) {
            int mv = 0;
            err = adc_cali_raw_to_voltage(s_cali_handle, raw, &mv);
            if (err == ESP_OK) {
                total += mv;
                valid_samples++;
                continue;
            }
        }

        /* Fallback: manual conversion (12-bit, 3.3V range) */
        int mv = (raw * 3300) / 4095;
        total += mv;
        valid_samples++;
    }

    if (valid_samples == 0) {
        return 0.0f;
    }

    return (float)total / (float)valid_samples;
}

/* ── Public API ────────────────────────────────────────────────────────── */

float battery_get_voltage(void)
{
    float mv = read_voltage_mv();
    /* Apply voltage divider ratio to get actual battery voltage */
    float voltage = (mv / 1000.0f) * VOLTAGE_DIVIDER_RATIO;
    return voltage;
}

float battery_get_percentage(void)
{
    float v = battery_get_voltage();

    if (v >= VBAT_FULL) {
        return 100.0f;
    }

    if (v <= VBAT_EMPTY) {
        return 0.0f;
    }

    /*
     * 3-segment linear approximation:
     *   3.0V - 3.7V  ->  0% - 50%
     *   3.7V - 4.2V  -> 50% - 100%
     */
    if (v >= VBAT_NOMINAL) {
        /* Upper segment: 3.7V=50%, 4.2V=100% */
        return 50.0f + 50.0f * (v - VBAT_NOMINAL) / (VBAT_FULL - VBAT_NOMINAL);
    } else {
        /* Lower segment: 3.0V=0%, 3.7V=50% */
        return 50.0f * (v - VBAT_EMPTY) / (VBAT_NOMINAL - VBAT_EMPTY);
    }
}
