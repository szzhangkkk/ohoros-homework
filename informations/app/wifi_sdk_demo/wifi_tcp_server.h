/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * wifi_tcp_server.h — AP 板 TCP 服务端（lwIP socket）
 */
#ifndef WIFI_TCP_SERVER_H
#define WIFI_TCP_SERVER_H

#include <stdint.h>
#include "errcode.h"

/**
 * @brief 创建独立线程运行 TCP Server（阻塞 accept，适合 AP 板热点已开之后调用）
 * @param port 监听端口，建议 WIFI_TCP_DEMO_PORT(8888)
 */
errcode_t WifiTcpServerStartThread(uint16_t port);

#endif
