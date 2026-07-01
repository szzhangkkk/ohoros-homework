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

#include <stdio.h>

#include "ohos_init.h"
#include "cmsis_os2.h"
#include "iot_watchdog.h"

#define UART_PRINTF_TASK_STACK_SIZE 0x1000
#define UART_PRINTF_TASK_PRIO       25

/* WS63 系统 tick 多为 100Hz，1 tick = 10ms；osDelay 参数为 tick 数 */
#define TICK_PER_SEC 100
#define MS_TO_TICKS(ms) (((ms) * TICK_PER_SEC) / 1000)

static void UartPrintfTask(void *arg)
{
    (void)arg;
    uint32_t seq = 1;

    while (1) {
        printf("Hello, Uart Printf! #%u\r\n", seq);
        seq++;
        IoTWatchDogKick();
        osDelay(MS_TO_TICKS(1000));
    }
}

static void UartPrintfEntry(void)
{
    osThreadAttr_t attr = {
        "UartPrintfTask", 0, NULL, 0, NULL, UART_PRINTF_TASK_STACK_SIZE, UART_PRINTF_TASK_PRIO, 0, 0
    };

    if (osThreadNew(UartPrintfTask, NULL, &attr) == NULL) {
        printf("[uart_printf] Failed to create UartPrintfTask!\r\n");
    }
}

APP_FEATURE_INIT(UartPrintfEntry);
