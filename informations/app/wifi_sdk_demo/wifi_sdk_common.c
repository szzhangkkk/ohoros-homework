/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * wifi_sdk_common.c — 处理“系统已初始化 Wi-Fi”与“应用首次初始化”两种情况
 */
#include "wifi_sdk_common.h"
#include <stdio.h>
#include "errcode.h"
#include "wifi_device.h"

void WifiSdkEnsureInit(const char *log_tag)
{
    if (wifi_init() == ERRCODE_SUCC) {
        printf("%s wifi_init ok\r\n", log_tag);
        return;
    }
    /*
     * nearlink_dk_3863 启动流程中协议栈常已 wifi_init，
     * 再次调用会打印 Srv:xxx:WiFi has been initialized 并返回 FAIL，属正常。
     */
    printf("%s skip wifi_init (already initialized by system)\r\n", log_tag);
}
