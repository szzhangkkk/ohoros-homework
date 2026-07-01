/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * wifi_softap_demo.c — 双板实验 **AP 板** 应用入口
 *
 * 职责：
 *   1. 注册 SoftAP 相关 Wi-Fi 事件（STA 关联/离开），在回调中打印并累计统计；
 *   2. 调用 wifi_softap_sdk 的 WifiSoftApStart 开热点；
 *   3. 周期调用 wifi_softap_get_sta_list 查询当前在线 STA。
 *
 * 烧录：BUILD.gn 的 sources 只保留本文件，注释 wifi_sta_demo.c。
 * 事件与统计逻辑 intentionally 放在 demo，不放在 wifi_softap_sdk.c。
 */
#include <stdio.h>
#include <string.h>
#include "ohos_init.h"
#include "cmsis_os2.h"
#include "errcode.h"
#include "wifi_device.h"
#include "wifi_device_config.h"
#include "wifi_event.h"
#include "wifi_hotspot.h"
#include "wifi_softap_sdk.h"
#include "lwip/netif.h"
#include "lwip/netifapi.h"
#include "lwip/ip4_addr.h"
#if defined(WIFI_SDK_ENABLE_TCP)
#include "wifi_tcp_common.h"
#include "wifi_tcp_server.h"
#endif

/* 与 STA 板 wifi_sta_demo.c 中宏必须一致，否则 STA 无法关联 */
#define WIFI_AP_SSID              "OHOS_AP"
#define WIFI_AP_PSK               "123456789"
/* 2.4G 信道，须与文档 7.2.1 及国家码要求一致 */
#define WIFI_AP_CHANNEL           7
/* SoftAP 在 lwIP 中的网口名，与 wifi_softap_sdk.c 中 WIFI_AP_IFNAME 一致 */
#define WIFI_AP_IFNAME            "ap0"
/* wifi_softap_get_sta_list 缓冲区容量，与 SDK WIFI_DEFAULT_MAX_NUM_STA(8) 对齐 */
#define WIFI_AP_MAX_STA           8
/* 主循环中打印在线列表的周期（毫秒） */
#define WIFI_AP_STATS_PERIOD_MS   5000

/**
 * @brief AP 板侧 STA 关联事件累计（仅统计次数，在线数以 get_sta_list 为准）
 */
typedef struct {
    uint32_t sta_join;   /* wifi_event_softap_sta_join 触发次数 */
    uint32_t sta_leave;  /* wifi_event_softap_sta_leave 触发次数 */
} WifiSoftApStaStats;

static WifiSoftApStaStats g_ap_stats;
static uint8_t g_ap_event_registered;  /* 防止重复 wifi_register_event_cb */

/**
 * @brief 将 lwIP ip4_addr（网络字节序）格式化为 "a.b.c.d"
 */
static void WifiSoftApFormatIpv4(char *buf, size_t buf_len, const ip4_addr_t *ip4)
{
    if (buf == NULL || buf_len < IP4ADDR_STRLEN_MAX || ip4 == NULL) {
        return;
    }
    if (ip4addr_ntoa_r(ip4, buf, (int)buf_len) == NULL) {
        (void)snprintf(buf, buf_len, "?");
    }
}

static void WifiSoftApFormatMac(char *buf, size_t buf_len, const uint8_t *mac)
{
    if (buf == NULL || buf_len < 18 || mac == NULL) {
        return;
    }
    (void)snprintf(buf, buf_len, "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/**
 * @brief 打印单台 STA 的 MAC、信号强度、协商速率
 * @param tag  日志前缀，如 "event" / "  "
 * @param info SDK 下发的 wifi_sta_info_stru，可为 NULL
 */
static void WifiSoftApPrintStaInfo(const char *tag, const wifi_sta_info_stru *info)
{
    char mac_str[18] = {0};

    if (info == NULL) {
        return;
    }
    WifiSoftApFormatMac(mac_str, sizeof(mac_str), info->mac_addr);
    printf("[WifiSoftApDemo] %s MAC=%s RSSI=%d rate=%ukbps\r\n",
        tag, mac_str, (int)info->rssi, (unsigned)info->best_rate);
}

/**
 * @brief 热点有 STA 完成关联时由 Wi-Fi 服务回调（非本线程直接调用）
 *        实现位于 soc_wifi_service_api.c → send_broadcast_sta_connected_for_ap
 */
static void WifiSoftApOnStaJoin(const wifi_sta_info_stru *info)
{
    g_ap_stats.sta_join++;
    printf("[WifiSoftApDemo] event sta_join #%u\r\n", (unsigned)g_ap_stats.sta_join);
    WifiSoftApPrintStaInfo("  ", info);
}

/**
 * @brief STA 从热点断开时回调
 */
static void WifiSoftApOnStaLeave(const wifi_sta_info_stru *info)
{
    g_ap_stats.sta_leave++;
    printf("[WifiSoftApDemo] event sta_leave #%u\r\n", (unsigned)g_ap_stats.sta_leave);
    WifiSoftApPrintStaInfo("  ", info);
}

/**
 * @brief SoftAP 整体状态变化（可用/不可用），一般用于调试
 */
static void WifiSoftApOnStateChanged(int32_t state)
{
    printf("[WifiSoftApDemo] event softap_state=%ld (%s)\r\n", (long)state,
        state == WIFI_STATE_AVALIABLE ? "available" : "not_available");
}

/**
 * @brief 向协议栈注册本 demo 的事件表；须在 STA 连入前完成
 * @return ERRCODE_SUCC 注册成功；失败则后续无法收到 join/leave
 */
static errcode_t WifiSoftApRegisterEvents(void)
{
    static wifi_event_stru s_ev;

    if (g_ap_event_registered) {
        return ERRCODE_SUCC;
    }

    (void)memset(&s_ev, 0, sizeof(s_ev));
    s_ev.wifi_event_softap_sta_join = WifiSoftApOnStaJoin;
    s_ev.wifi_event_softap_sta_leave = WifiSoftApOnStaLeave;
    s_ev.wifi_event_softap_state_changed = WifiSoftApOnStateChanged;

    if (wifi_register_event_cb(&s_ev) != ERRCODE_SUCC) {
        printf("[WifiSoftApDemo] register event fail\r\n");
        return ERRCODE_FAIL;
    }
    g_ap_event_registered = 1;
    return ERRCODE_SUCC;
}

/**
 * @brief 周期统计：打印累计 join/leave，并主动查询当前关联 STA 列表
 *        与事件回调互补——回调反映“变化瞬间”，本函数反映“当前快照”
 */
static void WifiSoftApDumpStats(void)
{
    wifi_sta_info_stru sta_list[WIFI_AP_MAX_STA];
    uint32_t sta_num = WIFI_AP_MAX_STA;
    char mac_str[18];
    char ip_str[16];
    struct netif *ap_netif = netif_find(WIFI_AP_IFNAME);

    (void)memset(sta_list, 0, sizeof(sta_list));

    printf("[WifiSoftApDemo] stats join=%u leave=%u\r\n",
        (unsigned)g_ap_stats.sta_join, (unsigned)g_ap_stats.sta_leave);

    if (ap_netif != NULL && ap_netif->ip_addr.u_addr.ip4.addr != 0) {
        WifiSoftApFormatIpv4(ip_str, sizeof(ip_str), &ap_netif->ip_addr.u_addr.ip4);
        printf("[WifiSoftApDemo] ap0 IPv4=%s", ip_str);
        if (ap_netif->netmask.u_addr.ip4.addr != 0) {
            WifiSoftApFormatIpv4(ip_str, sizeof(ip_str), &ap_netif->netmask.u_addr.ip4);
            printf(" mask=%s", ip_str);
        }
        if (ap_netif->gw.u_addr.ip4.addr != 0) {
            WifiSoftApFormatIpv4(ip_str, sizeof(ip_str), &ap_netif->gw.u_addr.ip4);
            printf(" gw=%s", ip_str);
        }
        printf("\r\n");
    } else {
        printf("[WifiSoftApDemo] ap0 IPv4=(none)\r\n");
    }

    if (wifi_is_softap_enabled() == 0) {
        printf("[WifiSoftApDemo] online=0 (SoftAP disabled)\r\n");
        return;
    }

    /* sta_num 入参为数组容量，出参为实际在线数量 */
    if (wifi_softap_get_sta_list(sta_list, &sta_num) != ERRCODE_SUCC) {
        printf("[WifiSoftApDemo] online=? (get_sta_list fail)\r\n");
        return;
    }

    printf("[WifiSoftApDemo] online=%u\r\n", (unsigned)sta_num);
    for (uint32_t i = 0; i < sta_num; i++) {
        WifiSoftApFormatMac(mac_str, sizeof(mac_str), sta_list[i].mac_addr);
        printf("[WifiSoftApDemo]   [%u] MAC=%s RSSI=%d rate=%ukbps",
            (unsigned)(i + 1), mac_str,
            (int)sta_list[i].rssi, (unsigned)sta_list[i].best_rate);

#if LWIP_DHCPS
        /* 从 AP 侧 DHCPS 租约表按 MAC 查 STA 已分配 IPv4（STA 未完成 DHCP 时可能失败） */
        if (ap_netif != NULL) {
            ip_addr_t cli_ip;
            if (netifapi_dhcps_get_client_ip(ap_netif, sta_list[i].mac_addr,
                    WIFI_MAC_LEN, &cli_ip) == ERR_OK) {
                WifiSoftApFormatIpv4(ip_str, sizeof(ip_str), ip_2_ip4(&cli_ip));
                printf(" IPv4=%s", ip_str);
            } else {
                printf(" IPv4=(pending)");
            }
        }
#endif
        printf("\r\n");
    }
}

/**
 * @brief AP 板主任务：等 Wi-Fi 就绪 → 注册事件 → 开热点 → 周期打印统计
 */
static void WifiSoftApDemoTask(void *arg)
{
    (void)arg;

    /* 系统启动后协议栈异步初始化，未就绪时不可操作 Wi-Fi */
    while (wifi_is_wifi_inited() == 0) {
        osDelay(100);
    }

    (void)memset(&g_ap_stats, 0, sizeof(g_ap_stats));
    if (WifiSoftApRegisterEvents() != ERRCODE_SUCC) {
        return;
    }

    /* 热点参数与 DHCPS 见 wifi_softap_sdk.c */
    if (WifiSoftApStart(WIFI_AP_SSID, WIFI_AP_PSK,
            WIFI_SEC_TYPE_WPA2PSK, WIFI_AP_CHANNEL) != ERRCODE_SUCC) {
        printf("[WifiSoftApDemo] start failed\r\n");
        return;
    }

#if defined(WIFI_SDK_ENABLE_TCP)
    /* BUILD.gn wifi_sdk_enable_tcp=true 时编入 wifi_tcp_server.c 并定义本宏 */
    if (WifiTcpServerStartThread(WIFI_TCP_DEMO_PORT) != ERRCODE_SUCC) {
        printf("[WifiSoftApDemo] TCP server thread fail\r\n");
    }
    printf("[WifiSoftApDemo] hotspot ready, TCP port %u, stats every %ums\r\n",
        (unsigned)WIFI_TCP_DEMO_PORT, (unsigned)WIFI_AP_STATS_PERIOD_MS);
#else
    printf("[WifiSoftApDemo] hotspot ready (TCP disabled), stats every %ums\r\n",
        (unsigned)WIFI_AP_STATS_PERIOD_MS);
#endif
    WifiSoftApDumpStats();

    for (;;) {
        osDelay(WIFI_AP_STATS_PERIOD_MS);
        WifiSoftApDumpStats();
    }
}

/**
 * @brief 应用入口：创建 SoftAP demo 线程（APP_FEATURE_INIT 由启动框架调用）
 */
static void WifiSoftApDemoEntry(void)
{
    osThreadAttr_t attr = {
        .name = "WifiSoftApDemo",
        .stack_size = 0x2000,
        .priority = osPriorityNormal,
    };
    if (osThreadNew(WifiSoftApDemoTask, NULL, &attr) == NULL) {
        printf("[WifiSoftApDemo] create task fail\r\n");
    }
}

APP_FEATURE_INIT(WifiSoftApDemoEntry);
