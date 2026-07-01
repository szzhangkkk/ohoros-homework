/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * wifi_tcp_server.c — TCP Server：bind → listen → accept → 收发若干轮后关闭连接
 * 供 AP 板在 SoftAP 与 DHCPS 就绪后调用；STA 板使用 wifi_tcp_client.c 连接本机 IP。
 */
#include "wifi_tcp_server.h"
#include "wifi_tcp_common.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "cmsis_os2.h"
#include "errcode.h"
#include "lwip/sockets.h"

#define WIFI_TCP_SRV_LOG  "[WifiTcpServer]"

static uint16_t g_tcp_server_port;

/**
 * @brief 处理单次 accept 得到的连接：先收后发，最多 WIFI_TCP_EXCHANGE_MAX 轮
 */
static void WifiTcpServerHandleClient(int connfd)
{
    char recv_buf[WIFI_TCP_RECV_BUF_SIZE];
    char send_buf[WIFI_TCP_RECV_BUF_SIZE];
    ssize_t n;

    for (uint32_t i = 0; i < WIFI_TCP_EXCHANGE_MAX; i++) {
        (void)memset(recv_buf, 0, sizeof(recv_buf));
        n = recv(connfd, recv_buf, sizeof(recv_buf) - 1, 0);
        if (n <= 0) {
            printf("%s client closed or recv fail n=%ld errno=%d\r\n",
                WIFI_TCP_SRV_LOG, (long)n, errno);
            break;
        }
        recv_buf[n] = '\0';
        printf("%s recv[%u]: %s\r\n", WIFI_TCP_SRV_LOG, (unsigned)(i + 1), recv_buf);

        (void)snprintf(send_buf, sizeof(send_buf), "AP-ACK-%u:%s", (unsigned)(i + 1), recv_buf);
        n = send(connfd, send_buf, strlen(send_buf), 0);
        if (n < 0) {
            printf("%s send fail errno=%d\r\n", WIFI_TCP_SRV_LOG, errno);
            break;
        }
        printf("%s sent ACK len=%ld\r\n", WIFI_TCP_SRV_LOG, (long)n);
    }
    lwip_close(connfd);
}

/**
 * @brief TCP Server 主循环：持续 listen，每接受一个 STA 连接则处理一轮
 */
static void WifiTcpServerTask(void *arg)
{
    int listen_fd;
    int connfd;
    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    socklen_t client_len;
    ssize_t ret;
    uint16_t port = g_tcp_server_port;

    (void)arg;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        printf("%s socket fail errno=%d\r\n", WIFI_TCP_SRV_LOG, errno);
        return;
    }

    (void)memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    ret = bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        printf("%s bind port %u fail ret=%ld errno=%d\r\n",
            WIFI_TCP_SRV_LOG, (unsigned)port, (long)ret, errno);
        lwip_close(listen_fd);
        return;
    }

    ret = listen(listen_fd, 1);
    if (ret < 0) {
        printf("%s listen fail errno=%d\r\n", WIFI_TCP_SRV_LOG, errno);
        lwip_close(listen_fd);
        return;
    }

    printf("%s listening on 0.0.0.0:%u (AP board)\r\n", WIFI_TCP_SRV_LOG, (unsigned)port);

    for (;;) {
        client_len = sizeof(client_addr);
        (void)memset(&client_addr, 0, sizeof(client_addr));
        connfd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (connfd < 0) {
            printf("%s accept fail errno=%d\r\n", WIFI_TCP_SRV_LOG, errno);
            osDelay(1000);
            continue;
        }
        printf("%s accepted client %s:%u\r\n", WIFI_TCP_SRV_LOG,
            inet_ntoa(client_addr.sin_addr), (unsigned)ntohs(client_addr.sin_port));
        WifiTcpServerHandleClient(connfd);
        printf("%s wait next client...\r\n", WIFI_TCP_SRV_LOG);
    }
}

errcode_t WifiTcpServerStartThread(uint16_t port)
{
    osThreadAttr_t attr = {
        .name = "WifiTcpServer",
        .stack_size = WIFI_TCP_THREAD_STACK,
        .priority = osPriorityNormal,
    };

    g_tcp_server_port = port;
    if (osThreadNew(WifiTcpServerTask, NULL, &attr) == NULL) {
        printf("%s create thread fail\r\n", WIFI_TCP_SRV_LOG);
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCC;
}
