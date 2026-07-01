#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ohos_init.h"
#include "cmsis_os2.h"
#include "iot_uart.h"
#include "pinctrl.h"
#include "uart.h"
#include "lwip/sockets.h"
#include "lwip/ip4_addr.h"

#include "wifi_connecter.h"

#define SSID "HONOR200"
#define KEY "qwertyuiop12"

#define UART_RECV_SIZE 50
#define DELAY_TIME_MS 1

static osThreadId_t g_tcp_task_id;
static int g_server_fd = -1;
static int g_client_fd = -1;
static uint8_t g_tcp_recv_buf[UART_RECV_SIZE];

static void uart_gpio_init(void)
{
    uapi_pin_set_mode(GPIO_08, PIN_MODE_2);
    uapi_pin_set_mode(GPIO_07, PIN_MODE_2);
}

static int uart_init_config(void)
{
    IotUartAttribute param = {
        .baudRate = 9600,
        .dataBits = IOT_UART_DATA_BIT_8,
        .stopBits = IOT_UART_STOP_BIT_1,
        .parity = IOT_UART_PARITY_NONE,
        .txBlock = S_MGPIO0,
        .rxBlock = S_MGPIO1
    };

    IoTUartDeinit(UART_BUS_2);
    int ret = IoTUartInit(UART_BUS_2, &param);
    if (ret != 0) {
        printf("UART2 init failed, ret=%02x\n", ret);
        return ret;
    }

    printf("UART2 TX ready: GPIO8, 9600 8N1\n");
    return 0;
}

static void *uart_tcp_task(const char *arg)
{
    (void)arg;

    if (ConnectToHotspot(SSID, KEY) != 0) {
        printf("Connect to AP failed!\n");
        return NULL;
    }
    printf("Connect to AP success!\n");

    uart_gpio_init();
    if (uart_init_config() != 0) {
        return NULL;
    }

    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_fd < 0) {
        printf("Socket creation failed!\n");
        return NULL;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(CONFIG_SERVER_PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(g_server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        printf("Bind failed!\n");
        lwip_close(g_server_fd);
        g_server_fd = -1;
        return NULL;
    }

    if (listen(g_server_fd, 5) < 0) {
        printf("Listen failed!\n");
        lwip_close(g_server_fd);
        g_server_fd = -1;
        return NULL;
    }

    printf("TCP Server running on port %d\n", CONFIG_SERVER_PORT);

    while (1) {
        printf("Waiting for connection...\n");
        g_client_fd = accept(g_server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (g_client_fd < 0) {
            printf("Accept failed!\n");
            continue;
        }

        printf("Client connected: %s:%d\n",
               inet_ntoa(client_addr.sin_addr),
               ntohs(client_addr.sin_port));

        while (1) {
            memset(g_tcp_recv_buf, 0, sizeof(g_tcp_recv_buf));
            int recv_len = recv(
                g_client_fd,
                g_tcp_recv_buf,
                sizeof(g_tcp_recv_buf) - 1,
                0
            );
            if (recv_len <= 0) {
                printf("Client disconnected\n");
                break;
            }

            g_tcp_recv_buf[recv_len] = '\0';
            printf("TCP recv: len=%d, data=[%s]\n", recv_len, g_tcp_recv_buf);

            int written = IoTUartWrite(
                UART_BUS_2,
                g_tcp_recv_buf,
                (unsigned int)recv_len
            );
            printf("UART2 write: requested=%d, written=%d, data=[%s]\n",
                   recv_len, written, g_tcp_recv_buf);

            if (written == recv_len) {
                send(
                    g_client_fd,
                    "$ACK:BALANCE_CAR#",
                    strlen("$ACK:BALANCE_CAR#"),
                    0
                );
            } else {
                send(
                    g_client_fd,
                    "$ERR:UART#",
                    strlen("$ERR:UART#"),
                    0
                );
            }

            osDelay(DELAY_TIME_MS);
        }

        lwip_close(g_client_fd);
        g_client_fd = -1;
    }
}

static void uart_tcp_entry(void)
{
    printf("Enter uart_tcp_entry()!\r\n");

    osThreadAttr_t attr = {
        .name = "UartTcpTask",
        .attr_bits = 0U,
        .cb_mem = NULL,
        .cb_size = 0U,
        .stack_mem = NULL,
        .stack_size = 0x1000,
        .priority = osPriorityNormal,
    };

    g_tcp_task_id = osThreadNew((osThreadFunc_t)uart_tcp_task, NULL, &attr);
    if (g_tcp_task_id == NULL) {
        printf("Create UartTcpTask failed!\r\n");
    } else {
        printf("Create UartTcpTask success!\r\n");
    }
}

APP_FEATURE_INIT(uart_tcp_entry);
