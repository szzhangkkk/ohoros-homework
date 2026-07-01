/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * wifi_sdk_common.h — STA/AP SDK 共用的 Wi-Fi 初始化辅助
 */
#ifndef WIFI_SDK_COMMON_H
#define WIFI_SDK_COMMON_H

/**
 * @brief 保证 Wi-Fi 协议栈可用
 * @param log_tag 串口日志前缀，如 "[WIFI_STA]"
 *
 * 若系统启动阶段已调用 wifi_init()，再次调用会失败并打印
 * “WiFi has been initialized”，本函数不循环重试，避免死锁。
 */
void WifiSdkEnsureInit(const char *log_tag);

#endif
