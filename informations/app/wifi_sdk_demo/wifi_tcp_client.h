/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * wifi_tcp_client.h — STA 板 TCP 客户端
 */
#ifndef WIFI_TCP_CLIENT_H
#define WIFI_TCP_CLIENT_H

#include <stdint.h>
#include "errcode.h"

/**
 * @brief 创建独立线程连接 AP 板 TCP Server（须在 STA 已获 IP 后调用）
 * @param host  对端 IP，双板实验填 WIFI_TCP_AP_IP（192.168.43.1）
 * @param port  对端端口，与 AP 板 WIFI_TCP_DEMO_PORT 一致
 */
errcode_t WifiTcpClientStartThread(const char *host, uint16_t port);

#endif
