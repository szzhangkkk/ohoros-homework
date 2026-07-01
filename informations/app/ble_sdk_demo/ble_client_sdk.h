/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * ble_client_sdk.h — BLE GATT Client / 扫描连接封装（无应用入口）
 */
#ifndef BLE_CLIENT_SDK_H
#define BLE_CLIENT_SDK_H

#include <stdint.h>
#include "errcode.h"

/* Client 写 Server RX 特征时匹配的 UUID */
#define BLE_CLIENT_UUID_RX         0xEFEF

/** Client 链路阶段（供 demo / 实验观察，由 SDK 回调更新） */
typedef enum {
    BLE_CLIENT_PHASE_STACK_DOWN = 0,
    BLE_CLIENT_PHASE_STACK_UP,
    BLE_CLIENT_PHASE_SCANNING,
    BLE_CLIENT_PHASE_CONNECTING,
    BLE_CLIENT_PHASE_CONNECTED,
    BLE_CLIENT_PHASE_GATT_READY,
} ble_client_phase_t;

/**
 * @brief 步骤 0：注册 GAP/GATT 回调并 enable_ble（不启动扫描）
 * @note 完成后进入 BLE_CLIENT_PHASE_STACK_UP；须在 enable 回调就绪后再扫描
 */
errcode_t ble_client_stack_init(void);

/**
 * @brief 设置扫描命中后用于连接的 Server 地址（6 字节，与扫描结果 addr 一致）
 * @note 须在 start_gap_scan 之前调用；默认见 ble_sdk_common.h BLE_SDK_SERVER_ADDR_INIT
 */
void ble_client_set_target_server_addr(const uint8_t addr[6]);

/**
 * @brief 步骤 1：GAP 扫描发现 Server（匹配已设置的 6 字节 MAC）
 * @note 异步：set_scan_param_cb → gap_ble_start_scan → scan_result_cb
 */
errcode_t ble_client_start_gap_scan(void);

/** @brief 一步完成 stack_init + start_gap_scan（兼容旧调用） */
errcode_t ble_client_init(void);

ble_client_phase_t ble_client_get_phase(void);

errcode_t ble_client_write(uint8_t *data, uint16_t len, uint16_t handle);

uint16_t ble_client_get_write_handle(void);
uint8_t ble_client_get_connection_state(void);
uint8_t ble_client_is_transfer_ready(void);

#endif
