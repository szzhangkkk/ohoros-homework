/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * wifi_softap_sdk.h — AP 板热点启动/停止接口（无事件监听）
 */
#ifndef WIFI_SOFTAP_SDK_H
#define WIFI_SOFTAP_SDK_H

#include "errcode.h"
#include "wifi_device_config.h"

/**
 * @brief 启动 SoftAP：enable、配置 ap0、启动 DHCPS
 * @return ERRCODE_SUCC 成功；失败时已尝试 wifi_softap_disable
 */
errcode_t WifiSoftApStart(const char *ssid, const char *psk,
    wifi_security_enum sec_type, uint8_t channel);

/** @brief 关闭热点并停止 ap0 上的 DHCPS */
void WifiSoftApStop(void);

#endif
