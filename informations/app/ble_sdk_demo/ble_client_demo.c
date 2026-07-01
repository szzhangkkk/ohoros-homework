/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * ble_client_demo.c — 双板实验 **Client 板** 应用入口
 *
 * 职责：
 *   1. 打印本机 Client GAP 地址、目标 Server 地址；
 *   2. 调用 ble_client_init() 启动 BLE 协议栈 + 扫描（见 ble_client_sdk.c）；
 *   3. 注册连接监听：Server 连上/断开时打印链路信息；
 *   4. 独立任务周期打印链路状态（监听是否可写 RX 特征）；
 *   5. UART0 收到字节且已连接时，经 GATT Write 发往 Server。
 *
 * 烧录：BUILD.gn 中 ble_sdk_role = "client"。
 *
 * 建议实验顺序：先烧录 Server 板并确认串口出现 start adv → 再烧录本板扫描连接。
 * 串口 115200，UART0（BLE_SDK_UART_BUS = 0）。
 * 新增
 */
#include <stdio.h>
#include "securec.h"
#include "soc_osal.h"
#include "osal_debug.h"
#include "osal_addr.h"
#include "uart.h"
#include "ohos_init.h"
#include "cmsis_os2.h"
#include "errcode.h"
#include "bts_le_gap.h"
#include "ble_client_sdk.h"
#include "ble_sdk_common.h"

#define BLE_DEMO_ADDR_LEN           6
/** 周期打印链路状态的间隔（毫秒） */
#define BLE_CLIENT_LINK_POLL_MS     5000

/* 本机 Client GAP 地址 */
static const uint8_t BLE_DEMO_CLIENT_LOCAL_ADDR[BLE_DEMO_ADDR_LEN] = BLE_SDK_CLIENT_ADDR_INIT;

/** UART 中断与透传任务之间的消息 */
typedef struct {
    uint8_t *value;
    uint16_t value_len;
} BleClientMsg;

static unsigned long g_ble_client_msg_queue;
static unsigned int g_ble_client_msg_size = sizeof(BleClientMsg);

/** 上次周期打印时的连接状态，用于检测变化时多打一行 */
static uint8_t g_ble_client_last_connected;

/**
 * @brief 打印本机 Client 参数（实验开始前核对）
 */
static void BleClientDemoPrintLocalInfo(void)
{
    printf("[BleClientDemo] local name: %s\r\n", BLE_SDK_CLIENT_NAME);
    ble_sdk_print_addr("[BleClientDemo] local GAP addr", BLE_DEMO_CLIENT_LOCAL_ADDR);
    printf("[BleClientDemo] target server name: %s\r\n", BLE_SDK_SERVER_NAME);
    ble_sdk_print_addr("[BleClientDemo] target server addr (init)", (const uint8_t[])BLE_SDK_SERVER_ADDR_INIT);
    ble_sdk_print_addr("[BleClientDemo] target server addr (scan match)",
                       (const uint8_t[])BLE_SDK_SERVER_ADDR_SCAN_MATCH_INIT);
}

/**
 * @brief 打印当前链路快照（周期监听或手动查询）
 */
static void BleClientDemoDumpLinkInfo(const char *tag)
{
    printf("[BleClientDemo] %s phase=%d connected=%u ready=%u write_handle=%u\r\n",
        tag,
        (int)ble_client_get_phase(),
        (unsigned)ble_client_get_connection_state(),
        (unsigned)ble_client_is_transfer_ready(),
        (unsigned)ble_client_get_write_handle());
}

/**
 * @brief GAP 连接状态变化监听（由 ble_client_sdk 在协议栈回调里调用）
 * @param connected 1=已连接 Server，0=已断开
 * @note 运行在协议栈回调上下文，仅做轻量 printf，勿阻塞
 */
static void BleClientOnLinkChanged(uint8_t connected, uint16_t conn_id)
{
    if (connected != 0) {
        printf("[BleClientDemo] *** Server CONNECTED *** conn_id=%u\r\n", (unsigned)conn_id);
        printf("[BleClientDemo] wait gatt discover to enable UART->Write passthrough\r\n");
        g_ble_client_last_connected = 1;
    } else {
        printf("[BleClientDemo] *** Server DISCONNECTED *** conn_id=%u\r\n", (unsigned)conn_id);
        printf("[BleClientDemo] SDK will rescan & reconnect on next scan result\r\n");
        g_ble_client_last_connected = 0;
    }
}

/**
 * @brief 周期监听任务：每 BLE_CLIENT_LINK_POLL_MS 打印一次链路状态
 */
static void *BleClientLinkMonitorTask(void *arg)
{
    unused(arg);

    for (;;) {
        osDelay(BLE_CLIENT_LINK_POLL_MS);
        uint8_t now = ble_client_get_connection_state();
        if (now != g_ble_client_last_connected) {
            /* 状态与上次周期不同但可能未触发回调时，补打一行 */
            BleClientDemoDumpLinkInfo("poll(edge)");
            g_ble_client_last_connected = now;
        } else {
            BleClientDemoDumpLinkInfo("poll");
        }
    }
    return NULL;
}

/**
 * @brief UART 接收回调：已连接时把字节送入队列，主任务里发 Write
 */
static void BleClientUartRxHandler(const void *buffer, uint16_t length, bool error)
{
    unused(error);
    if (ble_client_is_transfer_ready() == 0) {
        return;
    }
    BleClientMsg msg = { 0 };
    void *cpy = osal_vmalloc(length);

    if (cpy == NULL) {
        return;
    }
    if (memcpy_s(cpy, length, buffer, length) != EOK) {
        osal_vfree(cpy);
        return;
    }
    msg.value = (uint8_t *)cpy;
    msg.value_len = length;
    (void)osal_msg_queue_write_copy(g_ble_client_msg_queue, (void *)&msg, g_ble_client_msg_size, 0);
}

/**
 * @brief 链接变化时打印（轮询模式，无独立回调时使用）
 */
static void BleClientPollLinkInTask(void)
{
    uint8_t now = ble_client_get_connection_state();
    if (now != g_ble_client_last_connected) {
        BleClientOnLinkChanged(now, 0);
    }
}

/**
 * @brief 主任务：初始化 Client、处理 UART→Write 透传
 */
static void *BleClientDemoTask(void *arg)
{
    unused(arg);

    printf("[BleClientDemo] ===== BLE Client dual-board passthrough =====\r\n");
    BleClientDemoPrintLocalInfo();

    /*
     * 协议栈 + 扫描 + 连接 + GATT 发现在 SDK 内完成：
     *   enable_ble → 设本机名/MAC → start_gap_scan → 命中目标地址 → connect
     * 连接后 GATT 发现写 handle 完成后即可透传。
     */
    printf("[BleClientDemo] step: ble_client_init() (stack + scan)\r\n");
    ble_client_init();

    if (uapi_uart_register_rx_callback(BLE_SDK_UART_BUS, UART_RX_CONDITION_FULL_OR_IDLE,
        1, BleClientUartRxHandler) != ERRCODE_SUCC) {
        printf("[BleClientDemo] uart callback fail\r\n");
        return NULL;
    }

    printf("[BleClientDemo] uart ready; link monitor every %ums\r\n",
        (unsigned)BLE_CLIENT_LINK_POLL_MS);
    BleClientDemoDumpLinkInfo("init");

    for (;;) {
        BleClientMsg msg = { 0 };

        /* 轮询连接状态变化 */
        BleClientPollLinkInTask();

        if (osal_msg_queue_read_copy(g_ble_client_msg_queue, &msg, &g_ble_client_msg_size,
            OSAL_WAIT_FOREVER) != OSAL_SUCCESS) {
            if (msg.value != NULL) {
                osal_vfree(msg.value);
            }
            continue;
        }

        if (msg.value != NULL) {
            if (ble_client_is_transfer_ready() == 0) {
                printf("[BleClientDemo] drop uart %u bytes: gatt not ready\r\n", (unsigned)msg.value_len);
            } else {
                (void)ble_client_write(msg.value, msg.value_len, ble_client_get_write_handle());
            }
            osal_vfree(msg.value);
        }
    }
    return NULL;
}

/**
 * @brief 启动：消息队列 + 透传任务 + 连接监听任务
 */
static void BleClientDemoEntry(void)
{
    osThreadAttr_t attr_main = {
        .name = "BleClientDemo",
        .stack_size = BLE_SDK_TASK_STACK_SIZE,
        .priority = BLE_SDK_TASK_PRIO,
    };
    osThreadAttr_t attr_mon = {
        .name = "BleCliLinkMon",
        .stack_size = 0x800,
        .priority = BLE_SDK_TASK_PRIO + 1,
    };

    if (osal_msg_queue_create("ble_cli_msg", g_ble_client_msg_size, &g_ble_client_msg_queue,
        0, g_ble_client_msg_size) != OSAL_SUCCESS) {
        printf("[BleClientDemo] msg queue create fail\r\n");
        return;
    }

    osal_kthread_lock();
    if (osThreadNew((osThreadFunc_t)BleClientLinkMonitorTask, NULL, &attr_mon) == NULL) {
        printf("[BleClientDemo] link monitor task fail\r\n");
    }
    if (osThreadNew((osThreadFunc_t)BleClientDemoTask, NULL, &attr_main) == NULL) {
        printf("[BleClientDemo] main task fail\r\n");
    } else {
        printf("[BleClientDemo] tasks started\r\n");
    }
    osal_kthread_unlock();
}

APP_FEATURE_INIT(BleClientDemoEntry);
