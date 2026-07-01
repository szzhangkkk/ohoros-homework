/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * wifi_sta_sdk.c — STA 模式连接 AP 与 DHCP 客户端封装
 *
 * 流程概览：
 *   WifiSdkEnsureInit → wifi_sta_enable
 *   → [wifi_sta_scan → 等待 → 匹配 SSID → wifi_sta_connect → 等 WIFI_CONNECTED] × 重试
 *   → netifapi_dhcp_start(wlan0) → 等待 bound 与 IPv4
 *
 * 不包含 wifi_register_event_cb；连接过程事件请在 wifi_sta_demo.c 注册。
 */
#include "wifi_sta_sdk.h"
#include "wifi_sdk_common.h"
#include <stdio.h>
#include <string.h>
#include "securec.h"
#include "soc_osal.h"
#include "errcode.h"
#include "wifi_device.h"          /* wifi_init / wifi_sta_* */
#include "wifi_device_config.h"   /* wifi_sta_config_stru、DHCP、WIFI_MAX_KEY_LEN */
#include "wifi_event.h"           /* 可选：wifi_register_event_cb 扫描完成通知 */
#include "wifi_linked_info.h"     /* WIFI_CONNECTED、wifi_sta_get_ap_info */
#include "lwip/netifapi.h"        /* netifapi_dhcp_start 等 */
#include "lwip/nettool/misc.h"    /* dhcp_clients_info_show 调试打印 */

/* 串口日志前缀，便于过滤 */
#define WIFI_STA_LOG           "[WIFI_STA]"
/* STA 模式在 lwIP 中的网口名，关联 AP 后用于 DHCP */
#define WIFI_STA_IFNAME        "wlan0"
#define WIFI_MAX_SSID_LEN      33   /* SSID 最大长度（含结束符空间） */
#define WIFI_MAC_LEN           6
#define WIFI_SCAN_AP_LIMIT     64   /* 单次扫描最多缓存的 AP 数量 */
#define WIFI_STA_RETRY_MAX     5    /* 扫描+连接整轮最大重试次数 */
#define WIFI_STA_SCAN_WAIT_MS  5000 /* 发起扫描后等待时间(ms)，过短可能读不到 AP 板热点 */
#define WIFI_CONN_POLL_TIMES   10   /* 轮询关联状态次数，每次间隔 500ms */
#define WIFI_DHCP_BOUND_TIMES  10   /* 等待 DHCP 绑定次数 */
#define WIFI_STA_IP_POLL_TIMES 50   /* 等待 IPv4 地址非 0，每次 100ms */

/* 保存 wlan0 的 netif 指针，断开时用于 dhcp_stop */
static struct netif *g_sta_netif = NULL;

/*
 * 从最近一次扫描结果中查找与 ssid 完全匹配的 AP，
 * 并填充 wifi_sta_connect() 所需的 cfg（含 BSSID、加密类型、密码）。
 */
static errcode_t WifiStaFindTargetAp(const char *ssid, const char *psk,
    wifi_sta_config_stru *out_cfg)
{
    uint32_t num = WIFI_SCAN_AP_LIMIT;
    uint32_t scan_len = sizeof(wifi_scan_info_stru) * num;
    wifi_scan_info_stru *result = osal_kmalloc(scan_len, OSAL_GFP_ATOMIC);

    if (result == NULL) {
        return ERRCODE_MALLOC;
    }
    (void)memset_s(result, scan_len, 0, scan_len);

    /* num 入参为缓冲区容量，出参为实际 AP 个数 */
    if (wifi_sta_get_scan_info(result, &num) != ERRCODE_SUCC) {
        osal_kfree(result);
        return ERRCODE_FAIL;
    }

    for (uint32_t i = 0; i < num; i++) {
        /* 先比长度再比内容，减少误匹配 */
        if (strlen(ssid) != strlen((const char *)result[i].ssid)) {
            continue;
        }
        if (memcmp(ssid, result[i].ssid, strlen(ssid)) != 0) {
            continue;
        }
        /* 从扫描条目拷贝 SSID、BSSID；加密类型用 AP 广播的值 */
        if (memcpy_s(out_cfg->ssid, WIFI_MAX_SSID_LEN, result[i].ssid, WIFI_MAX_SSID_LEN) != EOK) {
            osal_kfree(result);
            return ERRCODE_MEMCPY;
        }
        if (memcpy_s(out_cfg->bssid, WIFI_MAC_LEN, result[i].bssid, WIFI_MAC_LEN) != EOK) {
            osal_kfree(result);
            return ERRCODE_MEMCPY;
        }
        out_cfg->security_type = result[i].security_type;
        if (memcpy_s(out_cfg->pre_shared_key, WIFI_MAX_KEY_LEN, psk, strlen(psk)) != EOK) {
            osal_kfree(result);
            return ERRCODE_MEMCPY;
        }
        out_cfg->ip_type = DHCP;  /* 由 AP 板 DHCP Server 分配 192.168.43.x */
        osal_kfree(result);
        return ERRCODE_SUCC;
    }

    osal_kfree(result);
    printf("%s target SSID not found: %s\r\n", WIFI_STA_LOG, ssid);
    return ERRCODE_FAIL;
}

/*
 * wifi_sta_connect() 返回成功后，链路层关联可能仍在进行，
 * 通过 wifi_sta_get_ap_info() 轮询直到 conn_state == WIFI_CONNECTED。
 */
static errcode_t WifiStaWaitConnected(void)
{
    wifi_linked_info_stru status;

    for (uint8_t i = 0; i < WIFI_CONN_POLL_TIMES; i++) {
        osal_msleep(500);
        (void)memset_s(&status, sizeof(status), 0, sizeof(status));
        if (wifi_sta_get_ap_info(&status) != ERRCODE_SUCC) {
            continue;
        }
        if (status.conn_state == WIFI_CONNECTED) {
            printf("%s associated with AP\r\n", WIFI_STA_LOG);
            return ERRCODE_SUCC;
        }
    }
    return ERRCODE_FAIL;
}

/*
 * 在 wlan0 上启动 DHCP 客户端，等待获取 IPv4。
 * 双板实验下 STA 板 IP 一般为 192.168.43.2 等，以串口打印为准。
 */
static errcode_t WifiStaStartDhcp(void)
{
    g_sta_netif = netifapi_netif_find(WIFI_STA_IFNAME);
    if (g_sta_netif == NULL) {
        printf("%s netif %s not found\r\n", WIFI_STA_LOG, WIFI_STA_IFNAME);
        return ERRCODE_FAIL;
    }

    if (netifapi_dhcp_start(g_sta_netif) != ERR_OK) {
        printf("%s dhcp_start fail lwip_err\r\n", WIFI_STA_LOG);
        return ERRCODE_FAIL;
    }

    /* 等待 DHCP 状态机进入 bound */
    for (uint8_t i = 0; i < WIFI_DHCP_BOUND_TIMES; i++) {
        osal_msleep(500);
        if (netifapi_dhcp_is_bound(g_sta_netif) == ERR_OK) {
            break;
        }
    }

    /* lwIP 中 IPv4 为小端存储，打印时需按字节拆解 */
    for (uint8_t i = 0; i < WIFI_STA_IP_POLL_TIMES; i++) {
        osal_msleep(100);
        if (g_sta_netif->ip_addr.u_addr.ip4.addr != 0) {
            uint32_t ip = g_sta_netif->ip_addr.u_addr.ip4.addr;
            printf("%s IP %u.%u.%u.%u\r\n", WIFI_STA_LOG,
                (ip & 0xff), (ip >> 8) & 0xff, (ip >> 16) & 0xff, (ip >> 24) & 0xff);
            (void)netifapi_netif_common(g_sta_netif, dhcp_clients_info_show, NULL);
            return ERRCODE_SUCC;
        }
    }

    printf("%s DHCP timeout\r\n", WIFI_STA_LOG);
    return ERRCODE_FAIL;
}

/**
 * @brief 连接指定热点并获取 IPv4（双板实验 STA 板主流程）
 * @param ssid  目标 SSID，须与 AP 板一致
 * @param psk   WPA 密码，须与 AP 板一致
 * @return ERRCODE_SUCC 已关联且 DHCP 拿到 IP；否则重试耗尽后 ERRCODE_FAIL
 */
errcode_t WifiStaConnectAp(const char *ssid, const char *psk)
{
    wifi_sta_config_stru cfg = {0};

    /* 步骤1：协议栈；系统可能已初始化，勿在 FAIL 时死循环重试 wifi_init */
    WifiSdkEnsureInit(WIFI_STA_LOG);

    /* 步骤2：使能 STA 模式（站点，用于连接外部 AP） */
    while (wifi_sta_enable() != ERRCODE_SUCC) {
        printf("%s sta_enable retry\r\n", WIFI_STA_LOG);
        osal_msleep(100);
    }

    /* 步骤3：扫描 → 匹配 → 连接 → DHCP，失败则进入下一轮重试 */
    for (uint8_t retry = 0; retry < WIFI_STA_RETRY_MAX; retry++) {
        printf("%s scan round %u\r\n", WIFI_STA_LOG, retry);

        if (wifi_sta_scan() != ERRCODE_SUCC) {
            osal_msleep(1000);
            continue;
        }
        /*
         * 必须等待扫描完成再调用 wifi_sta_get_scan_info。
         * 进阶：可注册 wifi_event_scan_state_changed，在回调里置标志再读表，无需固定延时。
         */
        osal_msleep(WIFI_STA_SCAN_WAIT_MS);

        if (WifiStaFindTargetAp(ssid, psk, &cfg) != ERRCODE_SUCC) {
            continue;  /* 未扫到 AP 板：AP 未上电、等待时间过短、SSID/密码不一致 */
        }

        if (wifi_sta_connect(&cfg) != ERRCODE_SUCC) {
            printf("%s wifi_sta_connect fail\r\n", WIFI_STA_LOG);
            continue;
        }

        if (WifiStaWaitConnected() != ERRCODE_SUCC) {
            continue;  /* 密码错误或信号弱可能导致关联超时 */
        }

        if (WifiStaStartDhcp() == ERRCODE_SUCC) {
            printf("%s connect success\r\n", WIFI_STA_LOG);
            return ERRCODE_SUCC;
        }
    }

    printf("%s connect failed after %u retries\r\n", WIFI_STA_LOG, WIFI_STA_RETRY_MAX);
    return ERRCODE_FAIL;
}

/**
 * @brief 断开当前 AP 并释放 wlan0 DHCP 租约
 */
void WifiStaDisconnectAp(void)
{
    if (g_sta_netif != NULL) {
        (void)netifapi_dhcp_stop(g_sta_netif);
        g_sta_netif = NULL;
    }
    (void)wifi_sta_disconnect();
    printf("%s disconnected\r\n", WIFI_STA_LOG);
}
