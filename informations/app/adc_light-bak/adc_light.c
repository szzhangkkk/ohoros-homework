/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "pinctrl.h"
#include "adc.h"
#include "adc_porting.h"
#include "common_def.h"
#include "soc_osal.h"
#include "tcxo.h"
#include "ohos_init.h"
#include "cmsis_os2.h"

/* 炫彩板光敏 / HS-S20B：GPIO12 → ADC5，见 adc_light README 五、6.6.1 硬件连接 */
#define ADC_SAMPLE_CHANNEL        5U
#define ADC_SAMPLE_INTERVAL_MS    100U
#define ADC_SAMPLE_LOOPS        10000U

/*
 * WS63 adc_port_read() returns driver-scaled mV (HAL uses ~0..3600 mV full-scale).
 *
 * 九联板 + HS-S20B 实测：光照越强，ADC 读数越高（同向）。
 * 三档：bright / normal / dark；中间为 normal，避免仅两档时 dark 边界不准。
 *
 * 务必 DARK_LE < BRIGHT_GE，中间 (DARK_LE, BRIGHT_GE) 为 normal。
 * 实测室内 normal 约 1000 mV：默认 DARK_LE=750、BRIGHT_GE=1850，使 ~1000 落在 normal。
 * 若 dark 仍偏敏感：降低 DARK_LE；强光难进 bright：略降 BRIGHT_GE。
 */
#define ADC_MV_FULL_SCALE         3600U

#define ADC_LIGHT_BRIGHT_GE_MV    1850U
#define ADC_LIGHT_DARK_LE_MV       750U

#if ADC_LIGHT_DARK_LE_MV >= ADC_LIGHT_BRIGHT_GE_MV
#error "ADC_LIGHT_DARK_LE_MV must be less than ADC_LIGHT_BRIGHT_GE_MV (need a normal band)"
#endif

static void *AdcTask(const char *arg)
{
    unused(arg);
    printf("start adc sample (channel %u)\r\n", ADC_SAMPLE_CHANNEL);
    uapi_adc_init(ADC_CLOCK_NONE);

    uint16_t voltage = 0;
    uint32_t n = ADC_SAMPLE_LOOPS;

    while (n--) {
        if (adc_port_read(ADC_SAMPLE_CHANNEL, &voltage) != ERRCODE_SUCC) {
            printf("adc_port_read failed\r\n");
            break;
        }
        /* 0% = 暗，100% = 亮：与实测「亮则电压高」一致，按满量程线性映射 */
        uint16_t brightness_pct = 0U;
        if (voltage >= ADC_MV_FULL_SCALE) {
            brightness_pct = 100U;
        } else {
            brightness_pct = (uint16_t)(((uint32_t)voltage * 100U) / ADC_MV_FULL_SCALE);
        }

        const char *level;
        if (voltage >= ADC_LIGHT_BRIGHT_GE_MV) {
            level = "bright";
        } else if (voltage <= ADC_LIGHT_DARK_LE_MV) {
            level = "dark";
        } else {
            level = "normal";
        }
        printf("voltage: %4u mV, brightness: %u%%, level: %s\r\n", voltage, brightness_pct, level);
        osal_msleep(ADC_SAMPLE_INTERVAL_MS);
    }
    /* Voltage may differ from multimeter if a divider is present; that can be expected. */
    uapi_adc_deinit();
    return NULL;
}

static void AdcLightEntry(void)
{
    osThreadAttr_t attr = {
        .name = "AdcTask",
        .stack_size = 0x1000,
        .priority = osPriorityNormal,
    };

    if (osThreadNew(AdcTask, NULL, &attr) == NULL) {
        printf("[adc_light] Failed to create AdcTask!\n");
    }
}

SYS_RUN(AdcLightEntry);
