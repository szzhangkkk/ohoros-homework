/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * wifi_sta_demo.c — 双板实验 **STA 板** 应用入口
 *
 * 职责：
 *   1. 注册 STA 模式事件（连接状态变化、扫描完成）；
 *   2. 调用 wifi_sta_sdk 的 WifiStaConnectAp 连接 AP 板热点；
 *   3. 周期查询 wifi_sta_get_ap_info 与 wlan0 IPv4，打印连接统计。
 *
 * 烧录：BUILD.gn 只保留本文件，注释 wifi_softap_demo.c。
 * 事件与统计放在 demo，不放在 wifi_sta_sdk.c。
 */
#include <stdio.h>
#include <string.h>
#include "ohos_init.h"
#include "cmsis_os2.h"
#include "errcode.h"
#include "wifi_device.h"
#include "wifi_event.h"
#include "wifi_linked_info.h"
#include "wifi_sta_sdk.h"
#include "lwip/netif.h"
#if defined(WIFI_SDK_ENABLE_TCP)
#include "wifi_tcp_common.h"
#include "wifi_tcp_client.h"
#endif

/* 须与 AP 板 wifi_softap_demo.c 中 WIFI_AP_SSID / WIFI_AP_PSK 完全一致 */
#define WIFI_STA_SSID              "OHOS_AP"
#define WIFI_STA_PSK               "123456789"
/* STA 关联后 lwIP 网口名，用于读取 DHCP 获得的 IP */
#define WIFI_STA_IFNAME            "wlan0"
#define WIFI_STA_STATS_PERIOD_MS   5000

/**
 * @brief STA 板侧连接过程累计计数（供实验观察重连、扫描次数）
 */
typedef struct {
    uint32_t scan_done;    /* wifi_event_scan_state_changed 次数 */
    uint32_t connect_ok;   /* 连接可用（WIFI_STATE_AVALIABLE）次数 */
    uint32_t disconnect;   /* 断开（WIFI_STATE_NOT_AVALIABLE）次数 */
} WifiStaConnStats;

static WifiStaConnStats g_sta_stats;
static uint8_t g_sta_event_registered;

/**
 * @brief 将 wifi_conn_state_enum 转为可读字符串
 */
static const char *WifiStaConnStateStr(wifi_conn_state_enum state)
{
    switch (state) {
        case WIFI_CONNECTED:
            return "CONNECTED";
        case WIFI_CONNECTING:
            return "CONNECTING";
        case WIFI_DISCONNECTED:
            return "DISCONNECTED";
        default:
            return "UNKNOWN";
    }
}

static void WifiStaFormatMac(char *buf, size_t buf_len, const uint8_t *mac)
{
    if (buf == NULL || buf_len < 18 || mac == NULL) {
        return;
    }
    (void)snprintf(buf, buf_len, "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/**
 * @brief 读取本机 STA 网卡 MAC：优先 wlan0 的 lwIP hwaddr，否则用 Wi-Fi 基础 MAC
 * @return 1 成功；0 失败
 */
static int WifiStaGetOwnMac(uint8_t *mac, size_t len)
{
    struct netif *netif;

    if (mac == NULL || len < WIFI_MAC_LEN) {
        return 0;
    }

    netif = netif_find(WIFI_STA_IFNAME);
    if (netif != NULL && netif->hwaddr_len >= WIFI_MAC_LEN) {
        netif_get_hwaddr(netif, mac, (int)WIFI_MAC_LEN);
        return 1;
    }
    if (wifi_get_base_mac_addr((int8_t *)mac, WIFI_MAC_LEN) == ERRCODE_SUCC) {
        return 1;
    }
    return 0;
}

/**
 * @brief 打印本机 MAC 与当前关联 AP 的链路信息（SSID/BSSID/RSSI 等）
 */
static void WifiStaPrintLinkInfo(const char *tag, const wifi_linked_info_stru *info)
{
    char own_mac_str[18] = {0};
    char bssid_str[18] = {0};
    uint8_t own_mac[WIFI_MAC_LEN];

    if (info == NULL) {
        return;
    }
    if (WifiStaGetOwnMac(own_mac, sizeof(own_mac))) {
        WifiStaFormatMac(own_mac_str, sizeof(own_mac_str), own_mac);
    } else {
        (void)snprintf(own_mac_str, sizeof(own_mac_str), "(unknown)");
    }
    WifiStaFormatMac(bssid_str, sizeof(bssid_str), (const uint8_t *)info->bssid);
    printf("[WifiStaDemo] %s MAC=%s SSID=%s BSSID=%s state=%s rssi=%d snr=%d ch=%d\r\n",
        tag, own_mac_str, (const char *)info->ssid, bssid_str,
        WifiStaConnStateStr(info->conn_state),
        (int)info->rssi, (int)info->snr, (int)info->channel_num);
}

/**
 * @brief STA 与 AP 关联状态变化回调（由 Wi-Fi 服务在关联成功/失败后调用）
 * @param state        WIFI_STATE_AVALIABLE 表示链路可用，NOT_AVALIABLE 表示断开
 * @param info         当前链路信息，可能为 NULL
 * @param reason_code  断开原因码，具体含义见 SDK 文档
 */
static void WifiStaOnConnectionChanged(int32_t state, const wifi_linked_info_stru *info, int32_t reason_code)
{
    if (state == WIFI_STATE_NOT_AVALIABLE) {
        g_sta_stats.disconnect++;
        printf("[WifiStaDemo] event disconnect #%u reason=%d\r\n",
            (unsigned)g_sta_stats.disconnect, (int)reason_code);
        WifiStaPrintLinkInfo("  ", info);
        return;
    }

    g_sta_stats.connect_ok++;
    printf("[WifiStaDemo] event connect #%u\r\n", (unsigned)g_sta_stats.connect_ok);
    WifiStaPrintLinkInfo("  ", info);
}

/**
 * @brief 扫描结束通知；WifiStaConnectAp 内部会触发 wifi_sta_scan
 */
static void WifiStaOnScanStateChanged(int32_t state, int32_t size)
{
    g_sta_stats.scan_done++;
    printf("[WifiStaDemo] event scan_done #%u state=%ld ap_count=%ld\r\n",
        (unsigned)g_sta_stats.scan_done, (long)state, (long)size);
}

/**
 * @brief 注册 STA 事件；建议在 WifiStaConnectAp 之前调用以便收到全程事件
 */
static errcode_t WifiStaRegisterEvents(void)
{
    static wifi_event_stru s_ev;

    if (g_sta_event_registered) {
        return ERRCODE_SUCC;
    }

    (void)memset(&s_ev, 0, sizeof(s_ev));
    s_ev.wifi_event_connection_changed = WifiStaOnConnectionChanged;
    s_ev.wifi_event_scan_state_changed = WifiStaOnScanStateChanged;

    if (wifi_register_event_cb(&s_ev) != ERRCODE_SUCC) {
        printf("[WifiStaDemo] register event fail\r\n");
        return ERRCODE_FAIL;
    }
    g_sta_event_registered = 1;
    return ERRCODE_SUCC;
}

/**
 * @brief 打印累计统计 + 当前 AP 信息 + wlan0 IPv4（DHCP 成功后应有 192.168.43.x）
 */
static void WifiStaDumpStats(void)
{
    wifi_linked_info_stru info = {0};
    struct netif *netif = netif_find(WIFI_STA_IFNAME);

    printf("[WifiStaDemo] stats scan=%u connect=%u disconnect=%u\r\n",
        (unsigned)g_sta_stats.scan_done,
        (unsigned)g_sta_stats.connect_ok,
        (unsigned)g_sta_stats.disconnect);

    if (wifi_sta_get_ap_info(&info) == ERRCODE_SUCC) {
        WifiStaPrintLinkInfo("current", &info);
    } else {
        printf("[WifiStaDemo] current: get_ap_info fail\r\n");
    }

    /* lwIP 2.x：IPv4 存放在 ip_addr.u_addr.ip4.addr，主机字节序 */
    if (netif != NULL && netif->ip_addr.u_addr.ip4.addr != 0) {
        uint32_t ip = netif->ip_addr.u_addr.ip4.addr;
        printf("[WifiStaDemo] IPv4=%u.%u.%u.%u\r\n",
            (unsigned)(ip & 0xff),
            (unsigned)((ip >> 8) & 0xff),
            (unsigned)((ip >> 16) & 0xff),
            (unsigned)((ip >> 24) & 0xff));
    } else {
        printf("[WifiStaDemo] IPv4=(none)\r\n");
    }
}

/**
 * @brief STA 板主任务
 */
static void WifiStaDemoTask(void *arg)
{
    (void)arg;

    while (wifi_is_wifi_inited() == 0) {
        osDelay(100);
    }

    (void)memset(&g_sta_stats, 0, sizeof(g_sta_stats));
    if (WifiStaRegisterEvents() != ERRCODE_SUCC) {
        return;
    }

    /* 扫描、关联、DHCP 细节见 wifi_sta_sdk.c */
    if (WifiStaConnectAp(WIFI_STA_SSID, WIFI_STA_PSK) != ERRCODE_SUCC) {
        printf("[WifiStaDemo] connect failed\r\n");
        WifiStaDumpStats();
        return;
    }

#if defined(WIFI_SDK_ENABLE_TCP)
    /* BUILD.gn wifi_sdk_enable_tcp=true 时编入 wifi_tcp_client.c 并定义本宏 */
    osDelay(2000);
    if (WifiTcpClientStartThread(WIFI_TCP_AP_IP, WIFI_TCP_DEMO_PORT) != ERRCODE_SUCC) {
        printf("[WifiStaDemo] TCP client thread fail\r\n");
    } else {
        printf("[WifiStaDemo] TCP client -> %s:%u\r\n",
            WIFI_TCP_AP_IP, (unsigned)WIFI_TCP_DEMO_PORT);
    }
#endif

    printf("[WifiStaDemo] connect success, periodic stats every %ums\r\n",
        (unsigned)WIFI_STA_STATS_PERIOD_MS);
    WifiStaDumpStats();

    for (;;) {
        osDelay(WIFI_STA_STATS_PERIOD_MS);
        WifiStaDumpStats();
    }
}

static void WifiStaDemoEntry(void)
{
    osThreadAttr_t attr = {
        .name = "WifiStaDemo",
        .stack_size = 0x2000,
        .priority = osPriorityNormal,
    };
    if (osThreadNew(WifiStaDemoTask, NULL, &attr) == NULL) {
        printf("[WifiStaDemo] create task fail\r\n");
    }
}

APP_FEATURE_INIT(WifiStaDemoEntry);
