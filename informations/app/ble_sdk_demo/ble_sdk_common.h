/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * ble_sdk_common.h — 双板 BLE 实验公共参数（名称、MAC）
 */
#ifndef BLE_SDK_COMMON_H
#define BLE_SDK_COMMON_H

#include <stdio.h>
#include <string.h>

/* GAP 本地名与扫描响应中的名称（≤15 字节） */
#define BLE_SDK_SERVER_NAME        "OHOS_BLE"
#define BLE_SDK_CLIENT_NAME        "ble_sdk_client"

/*
 * Server 本地地址（gap_ble_set_local_addr），人类可读常写作 11:22:33:44:55:66。
 * WS63E 扫描回调 scan_result_data->addr.addr 与上表 **字节序相反**（见厂商 19_ble_uart）。
 * Client 默认用 BLE_SDK_SERVER_ADDR_SCAN_MATCH_INIT 做 memcmp；也可用下方匹配函数同时试两种序。
 */
/* Server 本地地址  */
#define BLE_SDK_SERVER_ADDR_INIT            { 0x11, 0x22, 0x33, 0x44, 0x55, 0x05 }
#define BLE_SDK_SERVER_ADDR_SCAN_MATCH_INIT { 0x05, 0x55, 0x44, 0x33, 0x22, 0x11 }

/* Client 本机地址  */
#define BLE_SDK_CLIENT_ADDR_INIT   { 0x12, 0x22, 0x33, 0x44, 0x55, 0x05 }

/* 实验 UART 与任务（demo 层使用） */
#define BLE_SDK_UART_BUS           0
#define BLE_SDK_TASK_STACK_SIZE    0x1200
#define BLE_SDK_TASK_PRIO          28

/** 按 addr[0]..addr[5] 顺序打印，与协议栈数组一致 */
#define BLE_SDK_ADDR_PRINT_FMT     "%02X:%02X:%02X:%02X:%02X:%02X"
#define BLE_SDK_ADDR_PRINT_ARG(a)  (unsigned)(a)[0], (unsigned)(a)[1], (unsigned)(a)[2], \
                                   (unsigned)(a)[3], (unsigned)(a)[4], (unsigned)(a)[5]

static inline void ble_sdk_print_addr(const char *tag, const uint8_t addr[6])
{
    if (tag == NULL || addr == NULL) {
        return;
    }
    printf("%s " BLE_SDK_ADDR_PRINT_FMT "\r\n", tag, BLE_SDK_ADDR_PRINT_ARG(addr));
}

/** 目标地址与扫描上报地址是否一致（含 WS63 扫描逆序） */
static inline int ble_sdk_addr_match_with_reverse(const uint8_t target[6], const uint8_t scan[6])
{
    uint8_t rev[6];
    unsigned int i;

    if (target == NULL || scan == NULL) {
        return 0;
    }
    if (memcmp(target, scan, 6) == 0) {
        return 1;
    }
    for (i = 0; i < 6; i++) {
        rev[i] = target[5 - i];
    }
    return (memcmp(rev, scan, 6) == 0) ? 1 : 0;
}

#endif
