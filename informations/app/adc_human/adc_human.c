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

#include "adc.h"
#include "adc_porting.h"
#include "common_def.h"
#include "soc_osal.h"
#include "tcxo.h"
#include "ohos_init.h"
#include "cmsis_os2.h"

/* SR602微型人体红外传感器：GPIO12 → ADC5 */
#define ADC_SAMPLE_CHANNEL        5U
#define ADC_SAMPLE_INTERVAL_MS    100U
#define ADC_SAMPLE_LOOPS        10000U

/*
 * WS63 adc_port_read() returns driver-scaled mV (HAL uses ~0..3600 mV full-scale).
 *
 * SR602人体红外传感器：人体活动越强，ADC 读数越高（同向）。
 * 三档：motion / neutral / idle；中间为 neutral，避免仅两档时 idle 边界不准。
 *
 * 务必 IDLE_LE < MOTION_GE，中间 (IDLE_LE, MOTION_GE) 为 neutral。
 * 实测室内 neutral 约 1000 mV：默认 IDLE_LE=750、MOTION_GE=1850，使 ~1000 落在 neutral。
 * 若 idle 仍偏敏感：降低 IDLE_LE；活动难进 motion：略降 MOTION_GE。
 */
#define ADC_MV_FULL_SCALE         3600U

#define ADC_HUMAN_MOTION_GE_MV    1850U
#define ADC_HUMAN_IDLE_LE_MV       750U

#if ADC_HUMAN_IDLE_LE_MV >= ADC_HUMAN_MOTION_GE_MV
#error "ADC_HUMAN_IDLE_LE_MV must be less than ADC_HUMAN_MOTION_GE_MV (need a neutral band)"
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
        /* 0% = 无活动，100% = 强活动：与实测「活动强则电压高」一致，按满量程线性映射 */
        uint16_t activity_pct = 0U;
        if (voltage >= ADC_MV_FULL_SCALE) {
            activity_pct = 100U;
        } else {
            activity_pct = (uint16_t)(((uint32_t)voltage * 100U) / ADC_MV_FULL_SCALE);
        }

        const char *level;
        if (voltage >= ADC_HUMAN_MOTION_GE_MV) {
            level = "motion";
        } else if (voltage <= ADC_HUMAN_IDLE_LE_MV) {
            level = "idle";
        } else {
            level = "neutral";
        }
        printf("voltage: %4u mV, activity: %u%%, level: %s\r\n", voltage, activity_pct, level);
        osal_msleep(ADC_SAMPLE_INTERVAL_MS);
    }
    /* Voltage may differ from multimeter if a divider is present; that can be expected. */
    uapi_adc_deinit();
    return NULL;
}

static void AdcHumanEntry(void)
{
    osThreadAttr_t attr = {
        .name = "AdcTask",
        .stack_size = 0x1000,
        .priority = osPriorityNormal,
    };

    if (osThreadNew(AdcTask, NULL, &attr) == NULL) {
        printf("[adc_human] Failed to create AdcTask!\n");
    }
}

SYS_RUN(AdcHumanEntry);