/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * wifi_tcp_common.h — 双板 TCP 联调公共参数
 */
#ifndef WIFI_TCP_COMMON_H
#define WIFI_TCP_COMMON_H

#include <stdint.h>
#include "errcode.h"

/* 双板实验：AP 板起 Server，STA 板 Client 连 AP 的 192.168.43.1 */
#define WIFI_TCP_DEMO_PORT       8888
#define WIFI_TCP_AP_IP           "192.168.43.1"
#define WIFI_TCP_EXCHANGE_MAX    5
#define WIFI_TCP_RECV_BUF_SIZE   128
#define WIFI_TCP_THREAD_STACK    0x3000

#endif
