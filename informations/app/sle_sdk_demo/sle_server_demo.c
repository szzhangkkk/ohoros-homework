/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * sle_server_demo.c — 双板实验 **Server 板** 应用入口
 *
 * 烧录：BUILD.gn 中 sle_sdk_role = "server"
 * 九联开发板无 USER 键：Client 连上后 Server 自动 SSAP Notify 发送 0x00/0x01。
 */
#include "securec.h"
#include "common_def.h"
#include "soc_osal.h"
#include "cmsis_os2.h"
#include "ohos_init.h"
#include "sle_server_sdk.h"
#include "sle_errcode.h"
#include "sle_sdk_common.h"

#define SLE_SERVER_DEMO_LOG "[SleServerDemo]"

static const uint8_t SLE_DEMO_SERVER_LOCAL_ADDR[6] = SLE_SDK_SERVER_ADDR_INIT;
static uint8_t g_sle_server_led_cmd = SLE_SDK_CMD_LED_OFF;

static const char *SleServerCmdStr(uint8_t cmd)
{
    if (cmd == SLE_SDK_CMD_LED_OFF) {
        return "OFF";
    }
    if (cmd == SLE_SDK_CMD_LED_ON) {
        return "ON";
    }
    return "UNKNOWN";
}

static void SleServerAutoNotifyLed(void)
{
    errcode_t ret;

    if (sle_uart_client_is_connected() == 0) {
        return;
    }

    g_sle_server_led_cmd = (g_sle_server_led_cmd == SLE_SDK_CMD_LED_ON) ?
        SLE_SDK_CMD_LED_OFF : SLE_SDK_CMD_LED_ON;
    printf("%s auto SSAP notify cmd=0x%02X (%s)\r\n", SLE_SERVER_DEMO_LOG,
        (unsigned)g_sle_server_led_cmd, SleServerCmdStr(g_sle_server_led_cmd));
    ret = sle_uart_server_send_report_by_handle(&g_sle_server_led_cmd, 1);
    if (ret != ERRCODE_SLE_SUCCESS) {
        printf("%s SSAP notify fail status=0x%x\r\n", SLE_SERVER_DEMO_LOG, (unsigned)ret);
    }
}

static void *SleServerDemoTask(void *arg)
{
    unused(arg);

    printf("%s ===== SLE Server dual-board =====\r\n", SLE_SERVER_DEMO_LOG);
    printf("%s announce name: %s, Client LED ctrl: %s\r\n", SLE_SERVER_DEMO_LOG, SLE_SDK_SERVER_NAME,
        sle_sdk_led_enabled() ? "ON" : "OFF (Client log only)");
    sle_sdk_print_addr(SLE_SERVER_DEMO_LOG, SLE_DEMO_SERVER_LOCAL_ADDR);

    (void)osal_msleep(SLE_SDK_START_DELAY_MS);
    printf("%s step: sle_uart_server_init() (SSAP + announce)\r\n", SLE_SERVER_DEMO_LOG);
    if (sle_uart_server_init() != ERRCODE_SLE_SUCCESS) {
        printf("%s stack init fail\r\n", SLE_SERVER_DEMO_LOG);
        return NULL;
    }

    printf("%s ready: wait Client connect, auto notify every %u ms\r\n",
        SLE_SERVER_DEMO_LOG, (unsigned)SLE_SDK_LED_CMD_INTERVAL_MS);
    for (;;) {
        (void)osal_msleep(SLE_SDK_LED_CMD_INTERVAL_MS);
        SleServerAutoNotifyLed();
    }
    return NULL;
}

static void SleServerDemoEntry(void)
{
    osThreadAttr_t attr = {
        .name = "SleServerDemo",
        .stack_size = SLE_SDK_TASK_STACK_SIZE,
        .priority = SLE_SDK_TASK_PRIO,
    };

    osal_kthread_lock();
    if (osThreadNew((osThreadFunc_t)SleServerDemoTask, NULL, &attr) == NULL) {
        printf("%s create task fail\r\n", SLE_SERVER_DEMO_LOG);
    } else {
        printf("%s task started\r\n", SLE_SERVER_DEMO_LOG);
    }
    osal_kthread_unlock();
}

APP_FEATURE_INIT(SleServerDemoEntry);
