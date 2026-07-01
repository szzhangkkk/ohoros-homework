/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * ble_server_adv.h — Server 广播数据与 gap_ble_start_adv
 */
#ifndef BLE_SERVER_ADV_H
#define BLE_SERVER_ADV_H

#include <stdint.h>

uint8_t ble_server_set_adv_data(void);
uint8_t ble_server_start_adv(void);

#endif
