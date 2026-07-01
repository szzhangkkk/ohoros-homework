/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * ble_server_sdk.h — BLE GATT Server / GAP 广播封装（无应用入口）
 */
#ifndef BLE_SERVER_SDK_H
#define BLE_SERVER_SDK_H

#include <stdint.h>
#include "errcode.h"

/* GATT 16-bit UUID（与 ble_client_sdk 透传约定一致） */
#define BLE_SERVER_UUID_SERVICE    0xABCD
#define BLE_SERVER_UUID_TX         0xCDEF  /* Notify：Server → Client */
#define BLE_SERVER_UUID_RX         0xEFEF  /* Write：Client → Server */
#define BLE_SERVER_UUID_CCCD       0x2902

/** 链路快照（供 demo 打印 / 监听回调使用） */
typedef struct {
    uint16_t conn_id;
    uint8_t gap_state;       /* gap_ble_conn_state_t：0 断连，1 已连接 */
    uint8_t peer_addr[6];
    uint8_t peer_addr_valid; /* 1 表示 peer_addr 有效 */
    uint8_t disc_reason;
} ble_server_link_info_t;

/** connected：1 表示刚进入 CONNECTED，0 表示 DISCONNECTED */
typedef void (*ble_server_link_listener_t)(const ble_server_link_info_t *info, uint8_t connected);

void ble_server_set_device_name(const uint8_t *name, uint8_t len);
void ble_server_init(void);
errcode_t ble_server_send_notify(uint8_t *data, uint16_t len);

/** @return gap_ble_conn_state_t 原始值 */
uint8_t ble_server_get_connection_state(void);
uint8_t ble_server_is_client_connected(void);
uint16_t ble_server_get_conn_id(void);
void ble_server_get_peer_addr(uint8_t addr_out[6]);

/** 注册连接/断连监听（在 GAP conn_state_change_cb 中调用，运行于协议栈上下文） */
void ble_server_register_link_listener(ble_server_link_listener_t listener);

uint32_t ble_server_get_connect_count(void);
uint32_t ble_server_get_disconnect_count(void);

#endif
