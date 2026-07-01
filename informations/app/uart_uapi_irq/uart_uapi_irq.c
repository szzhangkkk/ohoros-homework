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
#include <string.h>

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
#define IRQ_UART_BUS             UART_BUS_2//这里根据指导书进行默认使用
#define IRQ_UART_TX_PIN          S_MGPIO8
#define IRQ_UART_RX_PIN          S_MGPIO7
#define IRQ_RX_BUF_SIZE          64
#define IRQ_RW_TIMEOUT           0
#define IRQ_TASK_STACK_SIZE      0x1000
#define IRQ_TASK_PRIO            25
#define TICK_PER_SEC             100
#define MS_TO_TICKS(ms)          (((ms) * TICK_PER_SEC) / 1000)

//设置消息队列
#define MSG_QUEUE_SIZE           16//队列最多存16条消息
#define MSG_DATA_SIZE            32//每条消息最大32字节

static uint8_t s_rx_buf[IRQ_RX_BUF_SIZE];
static osMessageQueueId_t s_msg_queue = NULL;

// 回调函数：在中断上下文执行，仅做快速入队
static void UartUapiIrqCallback(const void *buffer, uint16_t length, bool error)
{
    //若error（表示线路错误）数据可能已损坏/缓冲区为空/长度为0，则返回
    if (error || buffer == NULL || length == 0) {
        return;
    }

    //限制单次处理的数据量，避免在中断中耗时过长
    uint16_t copy_len = (length > MSG_DATA_SIZE) ? MSG_DATA_SIZE : length;
    
    //定义消息结构体
    typedef struct {
        uint8_t data[MSG_DATA_SIZE];
        uint16_t len;
    } UartMsg_t;
    
    UartMsg_t msg;
    memcpy(msg.data, buffer, copy_len);//把UART收到的数据复制到消息结构体
    msg.len = copy_len;//保存长度
    
    //中断上下文中 timeout 必须为 0（非阻塞）
    //放入信息队列
    (void)osMessageQueuePut(s_msg_queue, &msg, 0, 0);
}

//任务线程：创建消息队列，收到数据后回显
static void UartUapiIrqTask(void *arg)
{
    (void)arg;

    uart_pin_config_t pins = {//配置引脚
        .tx_pin = IRQ_UART_TX_PIN,
        .rx_pin = IRQ_UART_RX_PIN,
        .cts_pin = PIN_NONE,
        .rts_pin = PIN_NONE,
    };

    //参数设置，与 sdk uart_demo 一致：8 数据位
    uart_attr_t attr = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_BIT_8,
        .stop_bits = 1,
        .parity = 0,
        .flow_ctrl = 0,
    };

    uart_buffer_config_t buf_cfg = {
        .rx_buffer = s_rx_buf,
        .rx_buffer_size = IRQ_RX_BUF_SIZE,
    };
    //将POLL_UART_BUS对应修改为IRQ_UART_BUS，其余参数不变
    if (uapi_uart_init(IRQ_UART_BUS, &pins, &attr, NULL, &buf_cfg) != ERRCODE_SUCC) {
        printf("[uart_uapi_irq] uapi_uart_init failed\r\n");
        return;
    }

    //创建消息队列
    s_msg_queue = osMessageQueueNew(MSG_QUEUE_SIZE, 
        sizeof(uint8_t) * MSG_DATA_SIZE + sizeof(uint16_t), NULL);
    if (s_msg_queue == NULL) {
        printf("[uart_uapi_irq] Failed to create message queue\r\n");
        return;
    }

    //注册接收回调：条件使用文档建议 FULL_OR_SUFFICIENT_DATA_OR_IDLE，size=1
    //收到串口数据时调用中断函数
    //当缓冲区满/数据足够/UART是空闲的 就需要调用中断函数
    if (uapi_uart_register_rx_callback(IRQ_UART_BUS, 
                                        UART_RX_CONDITION_FULL_OR_SUFFICIENT_DATA_OR_IDLE, 
                                        1, 
                                        UartUapiIrqCallback) != ERRCODE_SUCC) {
        printf("[uart_uapi_irq] uapi_uart_register_rx_callback failed\r\n");
        return;
    }
    //启动提示打印
    printf("[uart_uapi_irq] irq bus %d echo 115200 8N1 (TX=MGPIO8 RX=MGPIO7) [202400203058]\r\n", 
        (int)IRQ_UART_BUS);

    /* 定义消息结构体 */
    typedef struct {
        uint8_t data[MSG_DATA_SIZE];
        uint16_t len;
    } UartMsg_t;
    
    UartMsg_t msg;

    while (1) {
        //从消息队列获取数据，带超时
        osStatus_t ret = osMessageQueueGet(s_msg_queue, &msg, NULL, MS_TO_TICKS(100));
        
        if (ret == osOK && msg.len > 0) {
            //在任务中执行 uapi_uart_write 回显 把收到的数据再发出去
            (void)uapi_uart_write(IRQ_UART_BUS, msg.data, msg.len, IRQ_RW_TIMEOUT);
        }
        
        IoTWatchDogKick();//喂狗
    }
}

static void UartUapiIrqEntry(void)
{
    osThreadAttr_t attr = {
        "UartUapiIrqTask", 0, NULL, 0, NULL, IRQ_TASK_STACK_SIZE, IRQ_TASK_PRIO, 0, 0
    };

    if (osThreadNew(UartUapiIrqTask, NULL, &attr) == NULL) {
        printf("[uart_uapi_irq] Failed to create task\r\n");
    }
}

APP_FEATURE_INIT(UartUapiIrqEntry);
