/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * wifi_tcp_client.c — TCP Client：connect AP:port → 收发若干轮 → 关闭
 */
#include "wifi_tcp_client.h"
#include "wifi_tcp_common.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "cmsis_os2.h"
#include "errcode.h"
#include "lwip/sockets.h"

#define WIFI_TCP_CLI_LOG  "[WifiTcpClient]"

static char g_tcp_client_host[32];
static uint16_t g_tcp_client_port;

/**
 * @brief 与 AP 板 TCP Server 进行固定轮次收发
 */
static void WifiTcpClientSession(int sockfd)
{
    char send_buf[WIFI_TCP_RECV_BUF_SIZE];
    char recv_buf[WIFI_TCP_RECV_BUF_SIZE];
    ssize_t n;

    for (uint32_t i = 0; i < WIFI_TCP_EXCHANGE_MAX; i++) {
        (void)snprintf(send_buf, sizeof(send_buf), "STA-MSG-%u", (unsigned)(i + 1));
        n = send(sockfd, send_buf, strlen(send_buf), 0);
        if (n < 0) {
            printf("%s send fail errno=%d\r\n", WIFI_TCP_CLI_LOG, errno);
            break;
        }
        printf("%s sent[%u]: %s (%ld bytes)\r\n",
            WIFI_TCP_CLI_LOG, (unsigned)(i + 1), send_buf, (long)n);

        (void)memset(recv_buf, 0, sizeof(recv_buf));
        n = recv(sockfd, recv_buf, sizeof(recv_buf) - 1, 0);
        if (n <= 0) {
            printf("%s recv fail or closed n=%ld errno=%d\r\n",
                WIFI_TCP_CLI_LOG, (long)n, errno);
            break;
        }
        recv_buf[n] = '\0';
        printf("%s recv[%u]: %s\r\n", WIFI_TCP_CLI_LOG, (unsigned)(i + 1), recv_buf);
        osDelay(500);
    }
}

static void WifiTcpClientTask(void *arg)
{
    int sockfd;
    struct sockaddr_in server_addr;

    (void)arg;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        printf("%s socket fail errno=%d\r\n", WIFI_TCP_CLI_LOG, errno);
        return;
    }

    (void)memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(g_tcp_client_port);

    if (inet_pton(AF_INET, g_tcp_client_host, &server_addr.sin_addr) <= 0) {
        printf("%s inet_pton fail host=%s\r\n", WIFI_TCP_CLI_LOG, g_tcp_client_host);
        lwip_close(sockfd);
        return;
    }

    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        printf("%s connect %s:%u fail errno=%d\r\n",
            WIFI_TCP_CLI_LOG, g_tcp_client_host, (unsigned)g_tcp_client_port, errno);
        lwip_close(sockfd);
        return;
    }

    printf("%s connected to %s:%u\r\n", WIFI_TCP_CLI_LOG,
        g_tcp_client_host, (unsigned)g_tcp_client_port);
    WifiTcpClientSession(sockfd);
    lwip_close(sockfd);
    printf("%s session done\r\n", WIFI_TCP_CLI_LOG);
}

errcode_t WifiTcpClientStartThread(const char *host, uint16_t port)
{
    osThreadAttr_t attr = {
        .name = "WifiTcpClient",
        .stack_size = WIFI_TCP_THREAD_STACK,
        .priority = osPriorityNormal,
    };

    if (host == NULL) {
        return ERRCODE_FAIL;
    }
    (void)memset(g_tcp_client_host, 0, sizeof(g_tcp_client_host));
    (void)snprintf(g_tcp_client_host, sizeof(g_tcp_client_host), "%s", host);
    g_tcp_client_port = port;

    if (osThreadNew(WifiTcpClientTask, NULL, &attr) == NULL) {
        printf("%s create thread fail\r\n", WIFI_TCP_CLI_LOG);
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCC;
}
