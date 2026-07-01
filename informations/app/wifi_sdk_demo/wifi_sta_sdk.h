/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * wifi_sta_sdk.h — STA 板连接 AP 与 DHCP 接口（无事件监听）
 */
#ifndef WIFI_STA_SDK_H
#define WIFI_STA_SDK_H

#include "errcode.h"

/**
 * @brief 扫描、关联指定 SSID 并在 wlan0 上 DHCP 获取 IP
 * @param ssid  须与 AP 板热点名一致（默认 OHOS_AP）
 * @param psk   须与 AP 板密码一致
 */
errcode_t WifiStaConnectAp(const char *ssid, const char *psk);

/** @brief 停止 DHCP 并 wifi_sta_disconnect */
void WifiStaDisconnectAp(void);

#endif
