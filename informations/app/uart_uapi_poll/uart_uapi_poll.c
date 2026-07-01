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

#include "errcode.h"
#include "uart.h"
#include "ohos_init.h"
#include "cmsis_os2.h"
#include "iot_watchdog.h"

/*
 * 使用 UART_BUS_2 + MGPIO8(TX)/MGPIO7(RX)，与 SDK BOARD_ASIC 下 UART2 复用一致（见官方 IO 复用表：
 * GPIO_08=UART2_TXD，GPIO_07=UART2_RXD）。勿用 UART_BUS_0：产品与 mconfig 中 CONFIG_DEBUG_UART
 * 等多为 UART0，二次 uapi_uart_init 会与系统调试口争用导致异常/NMI。
 */
#define POLL_UART_BUS           UART_BUS_2
#define POLL_UART_TX_PIN        S_MGPIO8
#define POLL_UART_RX_PIN        S_MGPIO7
#define POLL_RX_BUF_SIZE        64
#define POLL_RW_TIMEOUT         0
#define POLL_TASK_STACK_SIZE    0x1000
#define POLL_TASK_PRIO          25
#define TICK_PER_SEC            100
#define MS_TO_TICKS(ms)         (((ms) * TICK_PER_SEC) / 1000)

static uint8_t s_rx_buf[POLL_RX_BUF_SIZE];

static void UartUapiPollTask(void *arg)
{
    (void)arg;

    uart_pin_config_t pins = {
        .tx_pin = POLL_UART_TX_PIN,
        .rx_pin = POLL_UART_RX_PIN,
        .cts_pin = PIN_NONE,
        .rts_pin = PIN_NONE,
    };

    /* 与 sdk uart_demo 一致：8 数据位 */
    uart_attr_t attr = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_BIT_8,
        .stop_bits = 1,
        .parity = 0,
        .flow_ctrl = 0,
    };

    uart_buffer_config_t buf_cfg = {
        .rx_buffer = s_rx_buf,
        .rx_buffer_size = POLL_RX_BUF_SIZE,
    };

    if (uapi_uart_init(POLL_UART_BUS, &pins, &attr, NULL, &buf_cfg) != ERRCODE_SUCC) {
        printf("[uart_uapi_poll] uapi_uart_init failed\r\n");
        return;
    }

    printf("[uart_uapi_poll] poll bus %d echo 115200 8N1 (TX=MGPIO8 RX=MGPIO7)\r\n", (int)POLL_UART_BUS);

    while (1) {
        uint32_t n = 0;
        while (n < POLL_RX_BUF_SIZE && !uapi_uart_rx_fifo_is_empty(POLL_UART_BUS)) {
            int32_t r = uapi_uart_read(POLL_UART_BUS, s_rx_buf + n, 1, POLL_RW_TIMEOUT);
            if (r != 1) {
                break;
            }
            n++;
        }
        if (n > 0) {
            (void)uapi_uart_write(POLL_UART_BUS, s_rx_buf, n, POLL_RW_TIMEOUT);
        }
        IoTWatchDogKick();
        osDelay(MS_TO_TICKS(10));
    }
}

static void UartUapiPollEntry(void)
{
    osThreadAttr_t attr = {
        "UartUapiPollTask", 0, NULL, 0, NULL, POLL_TASK_STACK_SIZE, POLL_TASK_PRIO, 0, 0
    };

    if (osThreadNew(UartUapiPollTask, NULL, &attr) == NULL) {
        printf("[uart_uapi_poll] Failed to create task\r\n");
    }
}

APP_FEATURE_INIT(UartUapiPollEntry);
