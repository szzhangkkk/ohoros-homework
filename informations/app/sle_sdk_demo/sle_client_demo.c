/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * sle_client_demo.c — 双板实验 **Client 板** 应用入口
 *
 * 职责：
 *   1. 注册 sle_client_register_event_listener，处理扫描/连接/SSAP/通知事件；
 *   2. 调用 sle_client_stack_init 启动协议栈；
 *   3. 子任务 3：收 Server 自动 Notify 0x00/0x01 控 GPIO10（低电平点亮），否则仅打印。
 *
 * 烧录：BUILD.gn 中 sle_sdk_role = "client"
 */
#include "securec.h"
#include "common_def.h"
#include "soc_osal.h"
#include "cmsis_os2.h"
#include "ohos_init.h"
#include "iot_gpio.h"
#include "sle_connection_manager.h"
#include "sle_errcode.h"
#include "sle_client_sdk.h"
#include "sle_sdk_common.h"

#define SLE_CLIENT_DEMO_LOG       "[SleClientDemo]"
#define SLE_CLIENT_LINK_POLL_MS   5000

static const uint8_t SLE_DEMO_CLIENT_LOCAL_ADDR[6] = SLE_SDK_CLIENT_ADDR_INIT;

#if SLE_SDK_LED_ENABLE
static uint8_t g_sle_client_led_on;
static uint8_t g_sle_client_led_blink;
#endif

static const char *SleClientPhaseStr(sle_client_phase_t phase)
{
    switch (phase) {
        case SLE_CLIENT_PHASE_STACK_DOWN:
            return "STACK_DOWN";
        case SLE_CLIENT_PHASE_STACK_UP:
            return "STACK_UP";
        case SLE_CLIENT_PHASE_SEEKING:
            return "SEEKING";
        case SLE_CLIENT_PHASE_CONNECTING:
            return "CONNECTING";
        case SLE_CLIENT_PHASE_CONNECTED:
            return "CONNECTED";
        case SLE_CLIENT_PHASE_SSAP_DISCOVERING:
            return "SSAP_DISCOVERING";
        case SLE_CLIENT_PHASE_SSAP_READY:
            return "SSAP_READY";
        default:
            return "UNKNOWN";
    }
}

static const char *SleClientEventStr(sle_client_event_t event)
{
    switch (event) {
        case SLE_CLIENT_EVT_STACK_ENABLED:
            return "STACK_ENABLED";
        case SLE_CLIENT_EVT_SEEK_STARTED:
            return "SEEK_STARTED";
        case SLE_CLIENT_EVT_SEEK_RESULT:
            return "SEEK_RESULT";
        case SLE_CLIENT_EVT_SEEK_HIT:
            return "SEEK_HIT";
        case SLE_CLIENT_EVT_SEEK_STOPPED:
            return "SEEK_STOPPED";
        case SLE_CLIENT_EVT_CONNECT_START:
            return "CONNECT_START";
        case SLE_CLIENT_EVT_CONN_STATE:
            return "CONN_STATE";
        case SLE_CLIENT_EVT_MTU_EXCHANGE:
            return "MTU_EXCHANGE";
        case SLE_CLIENT_EVT_SSAP_SERVICE:
            return "SSAP_SERVICE";
        case SLE_CLIENT_EVT_SSAP_PROPERTY:
            return "SSAP_PROPERTY";
        case SLE_CLIENT_EVT_SSAP_READY:
            return "SSAP_READY";
        case SLE_CLIENT_EVT_NOTIFICATION:
            return "NOTIFICATION";
        case SLE_CLIENT_EVT_WRITE_CFM:
            return "WRITE_CFM";
        default:
            return "UNKNOWN";
    }
}

static const char *SleClientConnStateStr(SleAcbStateType state)
{
    switch (state) {
        case SLE_ACB_STATE_NONE:
            return "NONE";
        case SLE_ACB_STATE_CONNECTED:
            return "CONNECTED";
        case SLE_ACB_STATE_DISCONNECTED:
            return "DISCONNECTED";
        default:
            return "OTHER";
    }
}

static const char *SleClientCmdStr(uint8_t cmd)
{
    if (cmd == SLE_SDK_CMD_LED_OFF) {
        return "OFF";
    }
    if (cmd == SLE_SDK_CMD_LED_ON) {
        return "ON";
    }
    return "UNKNOWN";
}

#if SLE_SDK_LED_ENABLE
static void SleClientLedSet(uint8_t on)
{
    g_sle_client_led_on = on ? 1 : 0;
    /* GPIO10 低电平点亮，与 gpio_led / 06-GPIO的使用.md 一致 */
    IoTGpioSetOutputVal(SLE_SDK_LED_GPIO, on ? IOT_GPIO_VALUE0 : IOT_GPIO_VALUE1);
}

static void SleClientLedBlinkEnable(uint8_t enable)
{
    g_sle_client_led_blink = enable ? 1 : 0;
    if (g_sle_client_led_blink) {
        printf("%s LED blink mode ON (interval %u ms)\r\n",
            SLE_CLIENT_DEMO_LOG, (unsigned)SLE_SDK_LED_BLINK_INTERVAL_MS);
    } else {
        printf("%s LED blink mode OFF (Server notify ctrl)\r\n", SLE_CLIENT_DEMO_LOG);
    }
}

static void SleClientLedBlinkTick(void)
{
    if (!g_sle_client_led_blink) {
        return;
    }
    SleClientLedSet((uint8_t)(!g_sle_client_led_on));
    printf("%s LED blink -> %s\r\n", SLE_CLIENT_DEMO_LOG,
        g_sle_client_led_on ? "ON" : "OFF");
}
#endif

/**
 * @brief 处理 Server Notify：始终打印；仅开启 LED 时操作 GPIO
 */
static void SleClientHandleNotify(uint8_t cmd)
{
    printf("%s Server->Client notify cmd=0x%02X (%s)%s\r\n",
        SLE_CLIENT_DEMO_LOG,
        (unsigned)cmd,
        SleClientCmdStr(cmd),
        sle_sdk_led_enabled() ? "" : " [log only, LED disabled]");

#if SLE_SDK_LED_ENABLE
    if (cmd == SLE_SDK_CMD_LED_OFF) {
        SleClientLedSet(0);
    } else if (cmd == SLE_SDK_CMD_LED_ON) {
        SleClientLedSet(1);
    }
#endif
}

/**
 * @brief SDK 事件回调：扫描发现、连接状态、SSAP 发现、Notify 等
 */
static void SleClientOnEvent(const sle_client_event_info_t *info)
{
    if (info == NULL) {
        return;
    }

    printf("%s evt=%s phase=%s status=0x%x conn=%u\r\n",
        SLE_CLIENT_DEMO_LOG,
        SleClientEventStr(info->event),
        SleClientPhaseStr(info->phase),
        (unsigned)info->status,
        (unsigned)info->conn_id);

    switch (info->event) {
        case SLE_CLIENT_EVT_STACK_ENABLED:
            printf("%s step0: SLE stack enabled\r\n", SLE_CLIENT_DEMO_LOG);
            break;
        case SLE_CLIENT_EVT_SEEK_STARTED:
            printf("%s step1: seeking target \"%s\"\r\n", SLE_CLIENT_DEMO_LOG, SLE_SDK_SERVER_NAME);
            break;
        case SLE_CLIENT_EVT_SEEK_RESULT:
            printf("%s scan rssi=%d adv:%s\r\n", SLE_CLIENT_DEMO_LOG,
                (int)info->rssi, info->adv_name_snippet);
            if (info->peer_addr_valid) {
                sle_sdk_print_addr(SLE_CLIENT_DEMO_LOG, info->peer_addr.addr);
            }
            break;
        case SLE_CLIENT_EVT_SEEK_HIT:
            printf("%s *** scan hit \"%s\" ***\r\n", SLE_CLIENT_DEMO_LOG, SLE_SDK_SERVER_NAME);
            break;
        case SLE_CLIENT_EVT_SEEK_STOPPED:
            printf("%s seek stopped, connecting...\r\n", SLE_CLIENT_DEMO_LOG);
            break;
        case SLE_CLIENT_EVT_CONNECT_START:
            printf("%s step2: SleConnectRemoteDevice\r\n", SLE_CLIENT_DEMO_LOG);
            break;
        case SLE_CLIENT_EVT_CONN_STATE:
            printf("%s conn_state=%s disc_reason=0x%x\r\n", SLE_CLIENT_DEMO_LOG,
                SleClientConnStateStr(info->conn_state), (unsigned)info->disc_reason);
            if (info->conn_state == SLE_ACB_STATE_CONNECTED) {
                printf("%s *** CONNECTED ***\r\n", SLE_CLIENT_DEMO_LOG);
            } else if (info->conn_state == SLE_ACB_STATE_DISCONNECTED) {
                printf("%s *** DISCONNECTED *** restart seek\r\n", SLE_CLIENT_DEMO_LOG);
#if SLE_SDK_LED_ENABLE
                SleClientLedBlinkEnable(1);
#endif
            }
            break;
        case SLE_CLIENT_EVT_MTU_EXCHANGE:
            printf("%s step3: MTU exchange, start SSAP find_structure\r\n", SLE_CLIENT_DEMO_LOG);
            break;
        case SLE_CLIENT_EVT_SSAP_SERVICE:
            printf("%s SSAP service hdl=0x%04X\r\n", SLE_CLIENT_DEMO_LOG, (unsigned)info->ssap_handle);
            break;
        case SLE_CLIENT_EVT_SSAP_PROPERTY:
            printf("%s SSAP property hdl=0x%04X\r\n", SLE_CLIENT_DEMO_LOG, (unsigned)info->ssap_handle);
            break;
        case SLE_CLIENT_EVT_SSAP_READY:
            if (info->status == ERRCODE_SLE_SUCCESS) {
                printf("%s *** SSAP_READY *** wait Server auto notify%s\r\n",
                    SLE_CLIENT_DEMO_LOG,
                    sle_sdk_led_enabled() ? " (GPIO10)" : " (log only)");
#if SLE_SDK_LED_ENABLE
                SleClientLedBlinkEnable(0);
                SleClientLedSet(0);
#endif
            }
            break;
        case SLE_CLIENT_EVT_NOTIFICATION:
            SleClientHandleNotify(info->notify_cmd);
            break;
        case SLE_CLIENT_EVT_WRITE_CFM:
            printf("%s write cfm hdl=0x%04X\r\n", SLE_CLIENT_DEMO_LOG, (unsigned)info->ssap_handle);
            break;
        default:
            break;
    }
}

#if SLE_SDK_LED_ENABLE
static void SleClientLedInit(void)
{
    IoTGpioInit(SLE_SDK_LED_GPIO);
    IoTGpioSetDir(SLE_SDK_LED_GPIO, IOT_GPIO_DIR_OUT);
    g_sle_client_led_on = 0;
    g_sle_client_led_blink = 1;
    SleClientLedSet(0);
}
#endif

static void SleClientDumpLinkStatus(const char *tag)
{
    printf("%s %s phase=%s conn_id=%u ssap_ready=%u\r\n",
        SLE_CLIENT_DEMO_LOG,
        tag,
        SleClientPhaseStr(sle_client_get_phase()),
        (unsigned)sle_client_get_conn_id(),
        (unsigned)sle_client_is_ssap_ready());
}

static void *SleClientDemoTask(void *arg)
{
    unused(arg);

    printf("%s ===== SLE Client dual-board =====\r\n", SLE_CLIENT_DEMO_LOG);
    printf("%s seek name: %s, LED ctrl: %s\r\n", SLE_CLIENT_DEMO_LOG, SLE_SDK_SERVER_NAME,
        sle_sdk_led_enabled() ? "ON (GPIO10, blink until connected)" : "OFF (log only)");
    sle_sdk_print_addr(SLE_CLIENT_DEMO_LOG, SLE_DEMO_CLIENT_LOCAL_ADDR);

    (void)osal_msleep(SLE_SDK_START_DELAY_MS);
#if SLE_SDK_LED_ENABLE
    SleClientLedInit();
#endif

    sle_client_register_event_listener(SleClientOnEvent);
    printf("%s step: sle_client_stack_init()\r\n", SLE_CLIENT_DEMO_LOG);
    sle_client_stack_init();

    SleClientDumpLinkStatus("init");
#if SLE_SDK_LED_ENABLE
    SleClientLedBlinkEnable(1);
#endif

    {
        uint32_t monitor_elapsed = 0;

        for (;;) {
            (void)osal_msleep(SLE_SDK_LED_BLINK_INTERVAL_MS);
#if SLE_SDK_LED_ENABLE
            SleClientLedBlinkTick();
#endif
            monitor_elapsed += SLE_SDK_LED_BLINK_INTERVAL_MS;
            if (monitor_elapsed >= SLE_CLIENT_LINK_POLL_MS) {
                monitor_elapsed = 0;
                SleClientDumpLinkStatus("monitor");
            }
        }
    }
    return NULL;
}

static void SleClientDemoEntry(void)
{
    osThreadAttr_t attr = {
        .name = "SleClientDemo",
        .stack_size = SLE_SDK_TASK_STACK_SIZE,
        .priority = SLE_SDK_TASK_PRIO,
    };

    osal_kthread_lock();
    if (osThreadNew((osThreadFunc_t)SleClientDemoTask, NULL, &attr) == NULL) {
        printf("%s create task fail\r\n", SLE_CLIENT_DEMO_LOG);
    } else {
        printf("%s task started\r\n", SLE_CLIENT_DEMO_LOG);
    }
    osal_kthread_unlock();
}

APP_FEATURE_INIT(SleClientDemoEntry);
