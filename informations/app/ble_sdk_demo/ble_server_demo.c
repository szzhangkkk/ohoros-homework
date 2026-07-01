/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * ble_server_demo.c — 双板实验 **Server 板** 应用入口
 *
 * 职责：
 *   1. 打印本机广播名、GAP 地址（Client 扫描/连接须与之一致）；
 *   2. 调用 ble_server_init() 注册 GATT 服务并开始广播（见 ble_server_sdk.c）；
 *   3. 注册连接监听：Client 连上/断开时立即打印链路信息；
 *   4. 独立任务周期打印连接统计（监听当前是否可 Notify 透传）；
 *   5. UART0 收到字节且已连接时，经 GATT Notify 发往 Client。
 *
 * 烧录：BUILD.gn 中 ble_sdk_role = "server"。
 *
 * 建议实验顺序：先烧录本板并确认串口出现 start adv → 再烧录 Client 板扫描连接。
 * 串口 115200，UART0（BLE_SDK_UART_BUS = 0）。
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
#include "ble_server_sdk.h"
#include "ble_sdk_common.h"

#define BLE_DEMO_ADDR_LEN           6
/** 周期打印连接状态的间隔（毫秒） */
#define BLE_SERVER_LINK_POLL_MS     5000

/* 本机 GAP 地址，与 ble_sdk_common.h、Client 板 BLE_DEMO_SERVER_TARGET_ADDR 一致 */
static const uint8_t BLE_DEMO_SERVER_LOCAL_ADDR[BLE_DEMO_ADDR_LEN] = BLE_SDK_SERVER_ADDR_INIT;

/** UART 中断与透传任务之间的消息 */
typedef struct {
    uint8_t *value;
    uint16_t value_len;
} BleServerMsg;

static unsigned long g_ble_server_msg_queue;
static unsigned int g_ble_server_msg_size = sizeof(BleServerMsg);

/** 上次周期打印时的连接状态，用于检测变化时多打一行 */
static uint8_t g_ble_server_last_connected;

/**
 * @brief 将 6 字节地址格式化为 XX:XX:XX:XX:XX:XX
 */
/**
 * @brief 打印本机广播参数（实验开始前核对）
 */
static void BleServerDemoPrintLocalInfo(void)
{
    printf("[BleServerDemo] local name: %s\r\n", BLE_SDK_SERVER_NAME);
    ble_sdk_print_addr("[BleServerDemo] local GAP addr (same order as Client scan)", BLE_DEMO_SERVER_LOCAL_ADDR);
}

/**
 * @brief 打印当前链路快照（周期监听或手动查询）
 */
static void BleServerDemoDumpLinkInfo(const char *tag)
{
    uint8_t peer[BLE_DEMO_ADDR_LEN] = { 0 };

    ble_server_get_peer_addr(peer);
    printf("[BleServerDemo] %s connected=%u conn_id=%u notify_ok=%u\r\n",
        tag,
        (unsigned)ble_server_is_client_connected(),
        (unsigned)ble_server_get_conn_id(),
        (unsigned)ble_server_is_client_connected());
    ble_sdk_print_addr("[BleServerDemo] peer addr", peer);
    printf("[BleServerDemo] %s connect#=%u disconnect#=%u gap_state=%u\r\n",
        tag,
        (unsigned)ble_server_get_connect_count(),
        (unsigned)ble_server_get_disconnect_count(),
        (unsigned)ble_server_get_connection_state());
}

/**
 * @brief GAP 连接状态变化监听（由 ble_server_sdk 在 conn_state_change_cb 里调用）
 * @param connected 1=Client 已连接，0=已断开
 * @note 运行在协议栈回调上下文，仅做轻量 printf，勿阻塞
 */
static void BleServerOnLinkChanged(const ble_server_link_info_t *info, uint8_t connected)
{
    if (info == NULL) {
        return;
    }

    if (connected != 0) {
        printf("[BleServerDemo] *** Client CONNECTED *** conn_id=%u\r\n", (unsigned)info->conn_id);
        if (info->peer_addr_valid != 0) {
            ble_sdk_print_addr("[BleServerDemo] peer", info->peer_addr);
        }
        printf("[BleServerDemo] UART->Notify passthrough enabled; Client writes go to SDK write-cbk UART\r\n");
        g_ble_server_last_connected = 1;
    } else {
        printf("[BleServerDemo] *** Client DISCONNECTED *** conn_id=%u disc_reason=0x%02X\r\n",
            (unsigned)info->conn_id, (unsigned)info->disc_reason);
        if (info->peer_addr_valid != 0) {
            ble_sdk_print_addr("[BleServerDemo] peer", info->peer_addr);
        }
        printf("[BleServerDemo] SDK will set_adv_data + start_adv again, wait for next Client\r\n");
        g_ble_server_last_connected = 0;
    }
}

/**
 * @brief 周期监听任务：每 BLE_SERVER_LINK_POLL_MS 打印一次连接统计
 */
static void *BleServerLinkMonitorTask(void *arg)
{
    unused(arg);

    for (;;) {
        osDelay(BLE_SERVER_LINK_POLL_MS);
        uint8_t now = ble_server_is_client_connected();
        if (now != g_ble_server_last_connected) {
            /* 状态与上次周期不同但可能未触发回调时，补打一行 */
            BleServerDemoDumpLinkInfo("poll(edge)");
            g_ble_server_last_connected = now;
        } else {
            BleServerDemoDumpLinkInfo("poll");
        }
    }
    return NULL;
}

/**
 * @brief UART 接收回调：已连接时把字节送入队列，主任务里发 Notify
 */
static void BleServerUartRxHandler(const void *buffer, uint16_t length, bool error)
{
    unused(error);
    if (ble_server_is_client_connected() == 0) {
        return;
    }
    BleServerMsg msg = { 0 };
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
    (void)osal_msg_queue_write_copy(g_ble_server_msg_queue, (void *)&msg, g_ble_server_msg_size, 0);
}

/**
 * @brief 主任务：初始化 Server、处理 UART→Notify 透传
 */
static void *BleServerDemoTask(void *arg)
{
    unused(arg);

    printf("[BleServerDemo] ===== BLE Server dual-board passthrough =====\r\n");
    BleServerDemoPrintLocalInfo();

    /*
     * 协议栈 + GATT 服务 + 广播在 SDK 内完成：
     *   enable_ble → 设本机名/MAC → 添加 UART 服务 → start_service 回调里 start_adv
     * 连接/断连由 ble_server_connect_change_cbk 处理，并通过下方监听通知本 demo。
     */
    ble_server_register_link_listener(BleServerOnLinkChanged);
    printf("[BleServerDemo] step: ble_server_init() (GATT + adv)\r\n");
    ble_server_init();

    if (uapi_uart_register_rx_callback(BLE_SDK_UART_BUS, UART_RX_CONDITION_FULL_OR_IDLE,
        1, BleServerUartRxHandler) != ERRCODE_SUCC) {
        printf("[BleServerDemo] uart callback fail\r\n");
        return NULL;
    }

    printf("[BleServerDemo] uart ready; link monitor every %ums\r\n",
        (unsigned)BLE_SERVER_LINK_POLL_MS);
    BleServerDemoDumpLinkInfo("init");

    for (;;) {
        BleServerMsg msg = { 0 };

        if (osal_msg_queue_read_copy(g_ble_server_msg_queue, &msg, &g_ble_server_msg_size,
            OSAL_WAIT_FOREVER) != OSAL_SUCCESS) {
            if (msg.value != NULL) {
                osal_vfree(msg.value);
            }
            continue;
        }

        if (msg.value != NULL) {
            if (ble_server_is_client_connected() == 0) {
                printf("[BleServerDemo] drop uart %u bytes: no client\r\n", (unsigned)msg.value_len);
            } else {
                (void)ble_server_send_notify(msg.value, msg.value_len);
            }
            osal_vfree(msg.value);
        }
    }
    return NULL;
}

/**
 * @brief 启动：消息队列 + 透传任务 + 连接监听任务
 */
static void BleServerDemoEntry(void)
{
    osThreadAttr_t attr_main = {
        .name = "BleServerDemo",
        .stack_size = BLE_SDK_TASK_STACK_SIZE,
        .priority = BLE_SDK_TASK_PRIO,
    };
    osThreadAttr_t attr_mon = {
        .name = "BleSrvLinkMon",
        .stack_size = 0x800,
        .priority = BLE_SDK_TASK_PRIO + 1,
    };

    if (osal_msg_queue_create("ble_srv_msg", g_ble_server_msg_size, &g_ble_server_msg_queue,
        0, g_ble_server_msg_size) != OSAL_SUCCESS) {
        printf("[BleServerDemo] msg queue create fail\r\n");
        return;
    }

    osal_kthread_lock();
    if (osThreadNew((osThreadFunc_t)BleServerLinkMonitorTask, NULL, &attr_mon) == NULL) {
        printf("[BleServerDemo] link monitor task fail\r\n");
    }
    if (osThreadNew((osThreadFunc_t)BleServerDemoTask, NULL, &attr_main) == NULL) {
        printf("[BleServerDemo] main task fail\r\n");
    } else {
        printf("[BleServerDemo] tasks started\r\n");
    }
    osal_kthread_unlock();
}

APP_FEATURE_INIT(BleServerDemoEntry);
