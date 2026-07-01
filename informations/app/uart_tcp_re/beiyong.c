#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ohos_init.h"
#include "cmsis_os2.h"
#include "iot_uart.h"
#include "pinctrl.h"
#include "uart.h"
#include "lwip/netifapi.h"
#include "wifi_hotspot.h"
#include "wifi_hotspot_config.h"
#include "lwip/nettool/misc.h"
#include "wifi_device.h"
#include "wifi_event.h"
#include "lwip/sockets.h"
#include "lwip/ip4_addr.h"
#include "ws63_wifi_linked_info.h"
#include "ws63_wifi_event.h"
#include "soc_osal.h"

#include "wifi_connecter.h"
#include "device_pwm.h"
#include "statu.h"

// WiFi配置
#define SSID "HONOR200"
#define KEY "qwertyuiop12"


// 舵机角度定义
#define SERVO_ANGLE_OPEN    180
#define SERVO_ANGLE_CLOSE   0

#define CMD_OK 0
#define CMD_ERR -1

osThreadId_t task1_ID;
#define DELAY_TIME_MS 1
#define UART_RECV_SIZE 50

uint8_t recv_buf[UART_RECV_SIZE] = {0};
int server_fd, client_fd;

// 前向声明
static void tcp_send_reply(const char *msg);

/**
 * UART GPIO初始化（复用为串口）
 */
void uart_gpio_init(void)
{
    uapi_pin_set_mode(GPIO_08, PIN_MODE_2);  // UART2_TXD
    uapi_pin_set_mode(GPIO_07, PIN_MODE_2);  // UART2_RXD
}

/**
 * UART初始化配置
 */
void uart_init_config(void)
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
        printf("UART init failed ret = %02x\n", ret);
    } else {
        printf("UART init success\n");
    }
}

/**
 * 检查是否为平衡车命令（格式：$X,0,0,0,0,0,0,0,0,0#）
 */
static int is_balance_car_cmd(const char *buf)
{
    if (buf == NULL) return 0;
    int len = strlen(buf);
    // 平衡车命令长度固定为21: $X,0,0,0,0,0,0,0,0,0#
    if (len < 21) return 0;
    if (buf[0] != '$' || buf[len-1] != '#') return 0;
    // 检查是否包含多个逗号分隔的数字
    int comma_count = 0;
    for (int i = 0; i < len; i++) {
        if (buf[i] == ',') comma_count++;
    }
    return (comma_count >= 9) ? 1 : 0;
}

/**
 * 通过UART发送命令到平衡车
 */
static void send_to_balance_car(const char *cmd, int len)
{
    if (IoTUartWrite(UART_BUS_2, (const unsigned char *)cmd, len) == len) {
        printf("UART send to balance car: %s\n", cmd);
        tcp_send_reply("$ACK:BALANCE_CAR#");
    } else {
        printf("UART send failed\n");
        tcp_send_reply("$ERR:UART#");
    }
}

/**
 * 发送TCP回复
 */
static void tcp_send_reply(const char *msg)
{
    if (client_fd < 0 || msg == NULL) {
        return;
    }
    send(client_fd, msg, strlen(msg), 0);
}

/**
 * 处理命令
 */
static int handle_command(const char *cmd)
{
    if (!cmd) {
        return CMD_ERR;
    }

    /* ---------- 灯：100:x ---------- */
    if (strncmp(cmd, "100:", 4) == 0) {
        if (strlen(cmd) != 5) {
            return CMD_ERR;
        }

        int bright = cmd[4] - '0';
        if (bright < 0 || bright > 9) {
            return CMD_ERR;
        }

        g_home_status.light_bright = bright;
        printf("Light brightness = %d\n", bright);
        led_set_brightness(bright);
        return CMD_OK;
    }

    /* ---------- 空调/电机 ---------- */
    if (strcmp(cmd, "210") == 0) {
        g_home_status.ac_on = 1;
        motor_set_state(1);
        printf("AC ON\n");
        return CMD_OK;
    }

    if (strcmp(cmd, "200") == 0) {
        g_home_status.ac_on = 0;
        motor_set_state(0);
        printf("AC OFF\n");
        return CMD_OK;
    }

    /* ---------- 窗帘/舵机 ---------- */
    if (strcmp(cmd, "310") == 0) {
        g_home_status.curtain_open = 1;
        printf("Curtain OPEN\n");
        return CMD_OK;
    }

    if (strcmp(cmd, "300") == 0) {
        g_home_status.curtain_open = 0;
        printf("Curtain CLOSE\n");
        return CMD_OK;
    }

    return CMD_ERR;
}

/**
 * 解析TCP数据
 */
static void parse_tcp_data(char *buf)
{
    if (!buf) {
        return;
    }

    int len = strlen(buf);
    if (len < 3) {
        tcp_send_reply("$ERR:LEN#");
        return;
    }

    /* 校验包头包尾 */
    if (buf[0] != '$' || buf[len - 1] != '#') {
        tcp_send_reply("$ERR:FORMAT#");
        return;
    }

    /* 检查是否为平衡车命令，如果是则通过UART转发 */
    if (is_balance_car_cmd(buf)) {
        send_to_balance_car(buf, len);
        return;
    }

    /* 取出命令体 */
    char cmd[16] = {0};
    int cmd_len = len - 2;

    if (cmd_len <= 0 || cmd_len >= (int)sizeof(cmd)) {
        tcp_send_reply("$ERR:CMDLEN#");
        return;
    }

    memcpy(cmd, &buf[1], cmd_len);
    cmd[cmd_len] = '\0';

    printf("CMD = %s\n", cmd);

    int ret = handle_command(cmd);

    char reply[64];
    if (ret == CMD_OK) {
        snprintf(reply, sizeof(reply),
                 "$ACK:BRIGHT:%d,AC:%d,CU:%d#",
                 g_home_status.light_bright,
                 g_home_status.ac_on,
                 g_home_status.curtain_open);
    } else {
        snprintf(reply, sizeof(reply), "$ERR:%s#", cmd);
    }

    tcp_send_reply(reply);
}

/**
 * 舵机控制任务
 * 只在角度变化时发送PWM，避免持续发送导致抖动
 */
static void *servo_task(const char *arg)
{
    (void)arg;

    printf("[servo_task] Starting...\r\n");
    servo_gpio_init();
    printf("[servo_task] Initialized, entering main loop\r\n");

    int16_t last_angle = -1;  // 记录上次角度，-1表示初始状态

    while (1) {
        uint8_t angle;

        if (g_home_status.curtain_open) {
            angle = SERVO_ANGLE_OPEN;
        } else {
            angle = SERVO_ANGLE_CLOSE;
        }

        // 只有角度变化时才发送PWM信号
        if (angle != last_angle) {
            printf("[servo_task] Angle changed: %d -> %d\r\n", last_angle, angle);
            
            // 发送多次PWM周期确保舵机到位
            for (int i = 0; i < 5; i++) {
                servo_software_pwm(angle);
            }
            
            last_angle = angle;
            printf("[servo_task] Move complete\r\n");
        }

        osDelay(10);  // 检测间隔放长，减少CPU占用
    }
}

/**
 * 主任务 - TCP服务器
 */
void *main_task(const char *arg)
{
    unused(arg);

    // 连接WiFi
    if (ConnectToHotspot(SSID, KEY) != 0) {
        printf("Connect to AP failed!\n");
    } else {
        printf("Connect to AP success!\n");
    }

    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    // 创建TCP socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        printf("Socket creation failed!\n");
        return NULL;
    }

    // 绑定端口
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(CONFIG_SERVER_PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        printf("Bind failed!\n");
        lwip_close(server_fd);
        return NULL;
    }

    // 开始监听
    if (listen(server_fd, 5) < 0) {
        printf("Listen failed!\n");
        lwip_close(server_fd);
        return NULL;
    }
    printf("TCP Server running on port %d\n", CONFIG_SERVER_PORT);

    while (1) {
        printf("Waiting for connection...\n");
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            printf("accept failed!\r\n");
            continue;
        }
        printf("Client connected: %s:%d\n",
               inet_ntoa(client_addr.sin_addr),
               ntohs(client_addr.sin_port));

        while (1) {
            memset(recv_buf, 0, sizeof(recv_buf));

            int recv_len = recv(client_fd, recv_buf, sizeof(recv_buf), 0);
            if (recv_len <= 0) {
                printf("Client disconnected\n");
                break;
            }
            recv_buf[recv_len] = '\0';
            printf("tcp recv: %s\n", recv_buf);

            parse_tcp_data((char *)recv_buf);

            osal_msleep(1);
        }
        lwip_close(client_fd);
    }
    lwip_close(server_fd);
    return NULL;
}

static void uart_entry(void)
{
    printf("Enter uart_entry()!\r\n");

    // 初始化状态
    home_status_init();

    // 初始化LED PWM
    led_pwm_init();

    // 初始化电机GPIO
    motor_gpio_init();

    // 初始化UART（平衡车通信）
    uart_gpio_init();
    uart_init_config();

    // 设置GPIO13为高电平，当作额外的VCC使用
    IoTGpioInit(13);
    IoTGpioSetDir(13, IOT_GPIO_DIR_OUT);
    IoTGpioSetOutputVal(13, IOT_GPIO_VALUE1);
    printf("GPIO13 set HIGH as VCC\n");

    /* ---------- TCP / WiFi 线程 ---------- */
    osThreadAttr_t tcp_attr = {
        .name = "main_task",
        .attr_bits = 0U,
        .cb_mem = NULL,
        .cb_size = 0U,
        .stack_mem = NULL,
        .stack_size = 0x1000,
        .priority = osPriorityNormal,
    };

    task1_ID = osThreadNew((osThreadFunc_t)main_task, NULL, &tcp_attr);
    if (task1_ID != NULL) {
        printf("main_task created OK!\r\n");
    }

    /* ---------- 舵机 PWM 线程 ---------- */
    osThreadAttr_t servo_attr = {
        .name = "servo_task",
        .attr_bits = 0U,
        .cb_mem = NULL,
        .cb_size = 0U,
        .stack_mem = NULL,
        .stack_size = 0x1000,
        .priority = osPriorityLow3,
    };

    osThreadId_t servo_id = osThreadNew((osThreadFunc_t)servo_task, NULL, &servo_attr);
    if (servo_id != NULL) {
        printf("servo_task created OK!\r\n");
    } else {
        printf("servo_task created FAILED!\r\n");
    }
}

/* Run the uart_entry. */
APP_FEATURE_INIT(uart_entry);