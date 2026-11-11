#include "board.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t calibration;
static int filteredMv;

void boardBatteryInit(void)
{
    adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    esp_err_t err = adc_oneshot_new_unit(&unit, &adc);
    if (err == ESP_OK)
    {
        adc_oneshot_chan_cfg_t channel = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
        err = adc_oneshot_config_channel(adc, ADC_CHANNEL_3, &channel);
    }
    if (err == ESP_OK)
    {
        adc_cali_curve_fitting_config_t config = {.unit_id = ADC_UNIT_1, .chan = ADC_CHANNEL_3, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
        err = adc_cali_create_scheme_curve_fitting(&config, &calibration);
    }
    if (err != ESP_OK)
        ESP_LOGW("battery", "battery voltage not found: %s", esp_err_to_name(err));
}

bool boardBatteryRead(unsigned *millivolts, int *percent)
{
    *millivolts = 0;
    *percent = -1;
    if (!adc || !calibration)
        return false;
    int sum = 0;
    for (unsigned i = 0; i < 16; ++i)
    {
        int raw, mv;
        if (adc_oneshot_read(adc, ADC_CHANNEL_3, &raw) != ESP_OK ||
            adc_cali_raw_to_voltage(calibration, raw, &mv) != ESP_OK)
            return false;
        sum += mv;
    }
    int measured = (sum * 2 + 8) / 16;
    *millivolts = measured;
    if (measured < 2800 || measured > 4350)
    {
        filteredMv = 0;
        return false;
    }
    filteredMv = filteredMv ? (3 * filteredMv + measured + 2) / 4 : measured;
    *millivolts = filteredMv;
    static const int levels[][2] = {{3300, 0}, {3500, 5}, {3600, 12}, {3700, 25}, {3800, 45}, {3900, 65}, {4000, 80}, {4100, 90}, {4200, 100}};
    *percent = 100;
    if (filteredMv <= levels[0][0])
        *percent = 0;
    else
        for (unsigned i = 1; i < sizeof(levels) / sizeof(levels[0]); ++i)
        {
            if (filteredMv > levels[i][0])
                continue;
            *percent = levels[i - 1][1] + (filteredMv - levels[i - 1][0]) *
                                              (levels[i][1] - levels[i - 1][1]) / (levels[i][0] - levels[i - 1][0]);
            break;
        }
    return true;
}
