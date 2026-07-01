/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * wifi_softap_sdk.c — SoftAP（热点）能力封装
 *
 * 流程：WifiSdkEnsureInit → wifi_softap_enable → 等待 ap0 → 静态 IP
 *       → 停止其它网口 DHCPS → netifapi_dhcps_start（为 STA 分配 192.168.43.x）
 *
 * 不包含 STA 上下线监听；事件与统计由 wifi_softap_demo.c 注册。
 */
#include "wifi_softap_sdk.h"
#include "wifi_sdk_common.h"
#include <stdio.h>
#include <string.h>
#include "securec.h"
#include "soc_osal.h"
#include "errcode.h"
#include "wifi_device.h"
#include "wifi_hotspot.h"
#include "wifi_hotspot_config.h"
#include "wifi_event.h"
#include "lwip/err.h"
#include "lwip/netif.h"
#include "lwip/netifapi.h"
#if LWIP_DHCPS
#include "lwip/dhcps.h"
#endif

#define WIFI_AP_LOG              "[WIFI_AP]"
#define WIFI_AP_IFNAME           "ap0"       /* SoftAP 在 lwIP 中的网口名 */
#define WIFI_AP_NETIF_WAIT_MS    100         /* 轮询 ap0 间隔 */
#define WIFI_AP_NETIF_RETRY      50          /* 最多等待约 5s */
#define WIFI_AP_SOFTAP_RETRY     20          /* wifi_softap_enable 重试次数 */
#define WIFI_AP_AFTER_ENABLE_MS  1000        /* enable 后等待驱动创建 netif */

static struct netif *g_ap_netif = NULL;

/**
 * @brief 停止除 ap0 外所有网口上的 DHCP Server
 * @note  lwIP DHCPS 绑定 UDP 67，全局通常只能有一个 Server，否则 dhcps_start 返回 ERR_MEM(-1)
 */
static void WifiApStopDhcpsOnOtherNetifs(struct netif *ap_netif)
{
    struct netif *n = NULL;

    for (n = netif_list; n != NULL; n = n->next) {
        if (n == ap_netif) {
            continue;
        }
        (void)netifapi_dhcps_stop(n);
    }
}

/**
 * @brief 注册空事件表，避免部分固件打印 “cd is not init” 类告警
 *        业务回调请在 wifi_softap_demo.c 中单独 register
 */
static void WifiApRegisterEventStub(void)
{
    static wifi_event_stru s_ev = {0};
    static uint8_t s_registered;

    if (s_registered) {
        return;
    }
    if (wifi_register_event_cb(&s_ev) == ERRCODE_SUCC) {
        s_registered = 1;
    }
}

/**
 * @brief 轮询 netif_find("ap0") 直到驱动创建 SoftAP 网口
 */
static struct netif *WifiApWaitNetif(void)
{
    for (uint8_t i = 0; i < WIFI_AP_NETIF_RETRY; i++) {
        struct netif *netif = netif_find(WIFI_AP_IFNAME);
        if (netif != NULL) {
            return netif;
        }
        osal_msleep(WIFI_AP_NETIF_WAIT_MS);
    }
    return NULL;
}

/**
 * @brief 启动 SoftAP 并完成 ap0 地址与 DHCP Server
 * @param ssid      热点名称
 * @param psk       WPA2 密码
 * @param sec_type  一般为 WIFI_SEC_TYPE_WPA2PSK
 * @param channel   2.4G 信道号（SDK 常用 0～14，见文档 7.2.1）
 */
errcode_t WifiSoftApStart(const char *ssid, const char *psk,
    wifi_security_enum sec_type, uint8_t channel)
{
    softap_config_stru hapd = {0};
    ip4_addr_t ip = {0};
    ip4_addr_t mask = {0};
    ip4_addr_t gw = {0};
    err_t lwip_ret;

    while (wifi_is_wifi_inited() == 0) {
        osal_msleep(WIFI_AP_NETIF_WAIT_MS);
    }

    WifiSdkEnsureInit(WIFI_AP_LOG);
    WifiApRegisterEventStub();

    if (memcpy_s(hapd.ssid, sizeof(hapd.ssid), ssid, strlen(ssid)) != EOK) {
        return ERRCODE_MEMCPY;
    }
    if (memcpy_s(hapd.pre_shared_key, WIFI_MAX_KEY_LEN, psk, strlen(psk)) != EOK) {
        return ERRCODE_MEMCPY;
    }
    hapd.security_type = sec_type;
    hapd.channel_num = channel;

    for (uint8_t retry = 0; retry < WIFI_AP_SOFTAP_RETRY; retry++) {
        if (wifi_softap_enable(&hapd) == ERRCODE_SUCC) {
            break;
        }
        printf("%s softap_enable retry %u\r\n", WIFI_AP_LOG, retry);
        osal_msleep(200);
        if (retry == WIFI_AP_SOFTAP_RETRY - 1) {
            return ERRCODE_FAIL;
        }
    }

    osal_msleep(WIFI_AP_AFTER_ENABLE_MS);
    g_ap_netif = WifiApWaitNetif();
    if (g_ap_netif == NULL) {
        printf("%s netif %s not found\r\n", WIFI_AP_LOG, WIFI_AP_IFNAME);
        (void)wifi_softap_disable();
        return ERRCODE_FAIL;
    }

    /* 与厂商 softap_sample 一致：接口 IP=.1，网关字段=.2，STA 由 DHCPS 分配 .x */
    IP4_ADDR(&ip, 192, 168, 43, 1);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 192, 168, 43, 2);

    lwip_ret = netifapi_netif_set_addr(g_ap_netif, &ip, &mask, &gw);
    if (lwip_ret != ERR_OK) {
        printf("%s set_addr fail lwip_err=%d\r\n", WIFI_AP_LOG, (int)lwip_ret);
        (void)wifi_softap_disable();
        return ERRCODE_FAIL;
    }

    WifiApStopDhcpsOnOtherNetifs(g_ap_netif);
    (void)netifapi_dhcps_stop(g_ap_netif);

#if LWIP_DHCPS
    if (g_ap_netif->dhcps != NULL) {
        printf("%s dhcps already running on ap0\r\n", WIFI_AP_LOG);
    } else
#endif
    {
        lwip_ret = netifapi_dhcps_start(g_ap_netif, NULL, 0);
        if (lwip_ret != ERR_OK) {
            printf("%s dhcps_start fail lwip_err=%d (ERR_MEM=-1 多为67端口占用)\r\n",
                WIFI_AP_LOG, (int)lwip_ret);
            (void)wifi_softap_disable();
            return ERRCODE_FAIL;
        }
    }

    printf("%s started SSID=%s channel=%u IP=192.168.43.1\r\n",
        WIFI_AP_LOG, ssid, channel);
    return ERRCODE_SUCC;
}

/**
 * @brief 停止 DHCPS 并关闭 SoftAP
 */
void WifiSoftApStop(void)
{
    if (g_ap_netif != NULL) {
        (void)netifapi_dhcps_stop(g_ap_netif);
        g_ap_netif = NULL;
    }
    (void)wifi_softap_disable();
    printf("%s stopped\r\n", WIFI_AP_LOG);
}
