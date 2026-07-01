/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * sle_client_sdk.h — SLE SSAP Client / 扫描连接封装（无应用入口）
 */
#ifndef SLE_CLIENT_SDK_H
#define SLE_CLIENT_SDK_H

#include <stdint.h>
#include "ohos_sle_common.h"
#include "ohos_sle_errcode.h"
#include "sle_ssap_client.h"

/** Client 链路阶段（供 demo / 实验观察） */
typedef enum {
    SLE_CLIENT_PHASE_STACK_DOWN = 0,
    SLE_CLIENT_PHASE_STACK_UP,
    SLE_CLIENT_PHASE_SEEKING,
    SLE_CLIENT_PHASE_CONNECTING,
    SLE_CLIENT_PHASE_CONNECTED,
    SLE_CLIENT_PHASE_SSAP_DISCOVERING,
    SLE_CLIENT_PHASE_SSAP_READY,
} sle_client_phase_t;

/** 上报给 demo 的事件类型 */
typedef enum {
    SLE_CLIENT_EVT_STACK_ENABLED = 0,
    SLE_CLIENT_EVT_SEEK_STARTED,
    SLE_CLIENT_EVT_SEEK_RESULT,
    SLE_CLIENT_EVT_SEEK_HIT,
    SLE_CLIENT_EVT_SEEK_STOPPED,
    SLE_CLIENT_EVT_CONNECT_START,
    SLE_CLIENT_EVT_CONN_STATE,
    SLE_CLIENT_EVT_MTU_EXCHANGE,
    SLE_CLIENT_EVT_SSAP_SERVICE,
    SLE_CLIENT_EVT_SSAP_PROPERTY,
    SLE_CLIENT_EVT_SSAP_READY,
    SLE_CLIENT_EVT_NOTIFICATION,
    SLE_CLIENT_EVT_WRITE_CFM,
} sle_client_event_t;

/** 单次事件快照（协议栈回调上下文，勿阻塞） */
typedef struct {
    sle_client_event_t event;
    sle_client_phase_t phase;
    ErrCodeType status;
    uint16_t conn_id;
    uint8_t client_id;
    SleAcbStateType conn_state;
    SleDiscReasonType disc_reason;
    int8_t rssi;
    uint16_t ssap_handle;
    uint8_t notify_cmd;
    uint8_t notify_len;
    SleAddr peer_addr;
    uint8_t peer_addr_valid;
    char adv_name_snippet[32];
} sle_client_event_info_t;

typedef void (*sle_client_event_listener_t)(const sle_client_event_info_t *info);

/**
 * @brief 注册 SLE 扫描/连接/SSAP 事件监听（须在 sle_client_stack_init 之前调用）
 */
void sle_client_register_event_listener(sle_client_event_listener_t listener);

/**
 * @brief 步骤 0：注册回调并 EnableSle（异步进入扫描）
 */
void sle_client_stack_init(void);

sle_client_phase_t sle_client_get_phase(void);
uint16_t sle_client_get_conn_id(void);
uint8_t sle_client_get_client_id(void);
/** @return 1 表示 SSAP 发现完成，可收 Server Notify */
uint8_t sle_client_is_ssap_ready(void);

void sle_client_start_seek(void);

ssapc_write_param_t *sle_client_get_write_param(void);
int sle_client_uart_send(uint8_t *data, uint8_t length);

/** 兼容旧名 */
#define sle_uart_client_init       sle_client_stack_init
#define sle_uart_start_scan        sle_client_start_seek
#define get_g_sle_uart_conn_id     sle_client_get_conn_id
#define get_g_sle_uart_send_param  sle_client_get_write_param
#define uart_sle_client_send_data  sle_client_uart_send

#endif
