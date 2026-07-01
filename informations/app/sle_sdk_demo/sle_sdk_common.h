/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * sle_sdk_common.h — 双板 SLE 实验公共参数
 */
#ifndef SLE_SDK_COMMON_H
#define SLE_SDK_COMMON_H

#include <stdio.h>

/* Client 扫描广播数据时匹配此名称，须与 Server 广播名一致（≤15 字节） */
#define SLE_SDK_SERVER_NAME           "OHOS_SLE"

/* 兼容旧宏名（sdk 内广播模块仍可能引用） */
#ifndef SLE_UART_SERVER_NAME
#define SLE_UART_SERVER_NAME          SLE_SDK_SERVER_NAME
#endif

/*
 * 示例本地地址（6 字节，与 SleSetLocalAddr 一致）。
 * 双板联调须使用不同 MAC；可按实验需要修改，修改后 Server/Client 均须重新编译。
 */
#define SLE_SDK_SERVER_ADDR_INIT      { 0x78, 0x70, 0x60, 0x88, 0x96, 0x46 }
#define SLE_SDK_CLIENT_ADDR_INIT      { 0x13, 0x67, 0x5c, 0x07, 0x00, 0x51 }

/*
 * LED 硬件控制开关（由 BUILD.gn declare_args sle_sdk_led_enable 注入 SLE_SDK_LED_ENABLE）。
 * 0：Client 仅打印 SSAP 交互日志，不初始化/操作 GPIO10。
 * 1：Client 根据 Notify 0x00/0x01 控制 LED（GPIO10 低电平点亮）。
 */
#ifndef SLE_SDK_LED_ENABLE
#define SLE_SDK_LED_ENABLE            0
#endif

/*
 * 子任务 3：Client 自动发 SSAP 写、Server 回 Notify、Client GPIO10 控灯。
 * 九联星闪开发板排针 GPIO10 接 LED，低电平点亮（见 applications/docs/06-GPIO的使用.md）。
 */
#define SLE_SDK_LED_GPIO              10
/* Client 等待连接时 LED 间隔翻转周期（毫秒） */
#define SLE_SDK_LED_BLINK_INTERVAL_MS 500
/* Client 连上后 Server 自动 Notify LED 命令（0x00/0x01）的间隔（毫秒） */
#define SLE_SDK_LED_CMD_INTERVAL_MS   2000

static inline int sle_sdk_led_enabled(void)
{
#if SLE_SDK_LED_ENABLE
    return 1;
#else
    return 0;
#endif
}

/* 串口与 demo 任务（UART0，115200） */
#define SLE_SDK_UART_BUS              0
#define SLE_SDK_UART_TRANSFER_SIZE    256
#define SLE_SDK_TASK_STACK_SIZE       0x800
#define SLE_SDK_TASK_PRIO             25
#define SLE_SDK_START_DELAY_MS        1000

/* SSAP 载荷：Server Notify 0x00/0x01 → Client 控制 LED */
#define SLE_SDK_CMD_LED_OFF           0x00
#define SLE_SDK_CMD_LED_ON            0x01

#define SLE_SDK_ADDR_PRINT_FMT        "%02X:%02X:%02X:%02X:%02X:%02X"
#define SLE_SDK_ADDR_PRINT_ARG(a)     (unsigned)(a)[0], (unsigned)(a)[1], (unsigned)(a)[2], \
                                      (unsigned)(a)[3], (unsigned)(a)[4], (unsigned)(a)[5]

static inline void sle_sdk_print_addr(const char *tag, const uint8_t addr[6])
{
    if (tag == NULL || addr == NULL) {
        return;
    }
    printf("%s " SLE_SDK_ADDR_PRINT_FMT "\r\n", tag, SLE_SDK_ADDR_PRINT_ARG(addr));
}

#endif
