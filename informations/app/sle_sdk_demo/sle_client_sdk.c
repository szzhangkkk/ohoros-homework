/**
# Copyright (C) 2024 HiHope Open Source Organization .
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
 */
#include "string.h"
#include "common_def.h"
#include "osal_debug.h"
#include "osal_task.h"
#include "cmsis_os2.h"
#include "securec.h"
#include "sle_device_discovery.h"
#include "sle_connection_manager.h"
#include "sle_ssap_client.h"
#include "sle_client_sdk.h"
#include "sle_errcode.h"
#include "ohos_sle_common.h"
#include "ohos_sle_errcode.h"
#include "ohos_sle_ssap_server.h"
#include "ohos_sle_ssap_client.h"
#include "ohos_sle_device_discovery.h"
#include "ohos_sle_connection_manager.h"
#include "errcode.h"
#include "sle_sdk_common.h"

#define SLE_CLIENT_SDK_LOG "[sle client sdk]"
#define SLE_MTU_SIZE_DEFAULT 520
#define SLE_SEEK_INTERVAL_DEFAULT 100
#define SLE_SEEK_WINDOW_DEFAULT 100
#define UUID_16BIT_LEN 2
#define UUID_128BIT_LEN 16
#define SLE_UART_TASK_DELAY_MS 1000
#define SLE_UART_WAIT_SLE_CORE_READY_MS 5000
#define UUID_LEN_2     2

static char g_sle_uuid_app_uuid[] = { 0x39, 0xBE, 0xA8, 0x80, 0xFC, 0x70, 0x11, 0xEA,
    0xB7, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static ssapc_find_service_result_t g_sle_uart_find_service_result = { 0 };
static sle_announce_seek_callbacks_t g_sle_uart_seek_cbk = { 0 };
static SleConnectionCallbacks g_sle_uart_connect_cbk = { 0 };
static ssapc_callbacks_t g_sle_uart_ssapc_cbk = { 0 };
static SleAddr g_sle_uart_remote_addr = { 0 };
static ssapc_write_param_t g_sle_uart_send_param = { 0 };
static uint16_t g_sle_uart_conn_id = 0;
static uint8_t g_client_id = 0;
static uint8_t receive_buf[520] = { 0 };

static sle_client_event_listener_t g_sle_client_listener = NULL;
static sle_client_phase_t g_sle_client_phase = SLE_CLIENT_PHASE_STACK_DOWN;
static uint8_t g_sle_client_ssap_ready = 0;

static errcode_t sle_uart_client_send_report_by_handle(const uint8_t *data, uint8_t len);

static void sle_client_set_phase(sle_client_phase_t phase)
{
    g_sle_client_phase = phase;
}

static void sle_client_emit(sle_client_event_info_t *info)
{
    if (info == NULL) {
        return;
    }
    info->phase = g_sle_client_phase;
    if (g_sle_client_listener != NULL) {
        g_sle_client_listener(info);
    }
}

void sle_client_register_event_listener(sle_client_event_listener_t listener)
{
    g_sle_client_listener = listener;
}

sle_client_phase_t sle_client_get_phase(void)
{
    return g_sle_client_phase;
}

uint16_t sle_client_get_conn_id(void)
{
    return g_sle_uart_conn_id;
}

uint8_t sle_client_get_client_id(void)
{
    return g_client_id;
}

uint8_t sle_client_is_ssap_ready(void)
{
    return g_sle_client_ssap_ready;
}

ssapc_write_param_t *sle_client_get_write_param(void)
{
    return &g_sle_uart_send_param;
}

int sle_client_uart_send(uint8_t *data, uint8_t length)
{
    return sle_uart_client_send_report_by_handle(data, length);
}

void sle_client_start_seek(void)
{
    SleSeekParam param = { 0 };
    param.ownaddrtype = 0;
    param.filterduplicates = 0;
    param.seekfilterpolicy = 0;
    param.seekphys = 1;
    param.seekType[0] = 1;
    param.seekInterval[0] = SLE_SEEK_INTERVAL_DEFAULT;
    param.seekWindow[0] = SLE_SEEK_WINDOW_DEFAULT;
    SleSetSeekParam(&param);
    SleStartSeek();
    sle_client_set_phase(SLE_CLIENT_PHASE_SEEKING);
    sle_client_event_info_t evt = { .event = SLE_CLIENT_EVT_SEEK_STARTED, .status = ERRCODE_SLE_SUCCESS };
    sle_client_emit(&evt);
}

static void sle_uart_client_sample_sle_enable_cbk(errcode_t status)
{
    sle_client_event_info_t evt = { .event = SLE_CLIENT_EVT_STACK_ENABLED, .status = status };

    if (status != ERRCODE_SLE_SUCCESS) {
        printf("%s enable sle fail status=0x%x\r\n", SLE_CLIENT_SDK_LOG, status);
        sle_client_emit(&evt);
        return;
    }
    sle_client_set_phase(SLE_CLIENT_PHASE_STACK_UP);
    sle_client_emit(&evt);
    osal_msleep(SLE_UART_TASK_DELAY_MS);
    sle_client_start_seek();
}

static void sle_uart_client_sample_seek_enable_cbk(errcode_t status)
{
    if (status != ERRCODE_SLE_SUCCESS) {
        printf("%s seek enable fail status=0x%x\r\n", SLE_CLIENT_SDK_LOG, status);
    }
}

static void sle_uart_client_sample_seek_result_info_cbk(SleSeekResultInfo *seek_result_data)
{
    sle_client_event_info_t evt = { 0 };
    int hit = 0;

    if (seek_result_data == NULL || seek_result_data->data == NULL) {
        printf("%s seek result null\r\n", SLE_CLIENT_SDK_LOG);
        return;
    }

    evt.event = SLE_CLIENT_EVT_SEEK_RESULT;
    evt.status = ERRCODE_SLE_SUCCESS;
    evt.rssi = (int8_t)seek_result_data->rssi;
    evt.peer_addr = seek_result_data->addr;
    evt.peer_addr_valid = 1;
    if (seek_result_data->dataLength > 0) {
        size_t copy_len = seek_result_data->dataLength;
        if (copy_len >= sizeof(evt.adv_name_snippet)) {
            copy_len = sizeof(evt.adv_name_snippet) - 1;
        }
        if (memcpy_s(evt.adv_name_snippet, sizeof(evt.adv_name_snippet),
            seek_result_data->data, copy_len) == EOK) {
            evt.adv_name_snippet[copy_len] = '\0';
        }
    }
    sle_client_emit(&evt);

    if (strstr((const char *)seek_result_data->data, SLE_UART_SERVER_NAME) != NULL) {
        hit = 1;
        (void)memcpy_s(&g_sle_uart_remote_addr, sizeof(SleAddr),
            &seek_result_data->addr, sizeof(SleAddr));
        evt.event = SLE_CLIENT_EVT_SEEK_HIT;
        sle_client_emit(&evt);
        SleStopSeek();
    }

    printf("%s seek %s rssi=%d data:%.*s\r\n", SLE_CLIENT_SDK_LOG,
        hit ? "HIT" : "skip", (int)evt.rssi,
        (int)seek_result_data->dataLength, seek_result_data->data);
}

static void sle_uart_client_sample_seek_disable_cbk(errcode_t status)
{
    sle_client_event_info_t evt = {
        .event = SLE_CLIENT_EVT_SEEK_STOPPED,
        .status = status,
        .peer_addr = g_sle_uart_remote_addr,
        .peer_addr_valid = 1,
    };

    sle_client_emit(&evt);
    if (status != ERRCODE_SLE_SUCCESS) {
        printf("%s seek stop fail status=0x%x\r\n", SLE_CLIENT_SDK_LOG, status);
        return;
    }
    sle_client_set_phase(SLE_CLIENT_PHASE_CONNECTING);
    evt.event = SLE_CLIENT_EVT_CONNECT_START;
    sle_client_emit(&evt);
    SleConnectRemoteDevice(&g_sle_uart_remote_addr);
}

static void sle_uart_client_sample_seek_cbk_register(void)
{
    g_sle_uart_seek_cbk.sle_enable_cb = sle_uart_client_sample_sle_enable_cbk;
    g_sle_uart_seek_cbk.seek_enable_cb = sle_uart_client_sample_seek_enable_cbk;
    g_sle_uart_seek_cbk.seek_result_cb = sle_uart_client_sample_seek_result_info_cbk;
    g_sle_uart_seek_cbk.seek_disable_cb = sle_uart_client_sample_seek_disable_cbk;
    sle_announce_seek_register_callbacks(&g_sle_uart_seek_cbk);
}

static void sle_uart_client_sample_connect_state_changed_cbk(uint16_t conn_id, const SleAddr *addr,
    SleAcbStateType conn_state, SlePairStateType pair_state, SleDiscReasonType disc_reason)
{
    sle_client_event_info_t evt = {
        .event = SLE_CLIENT_EVT_CONN_STATE,
        .conn_id = conn_id,
        .conn_state = conn_state,
        .disc_reason = disc_reason,
    };

    unused(pair_state);
    g_sle_uart_conn_id = conn_id;
    if (addr != NULL) {
        evt.peer_addr = *addr;
        evt.peer_addr_valid = 1;
    }

    if (conn_state == SLE_ACB_STATE_CONNECTED) {
        sle_client_set_phase(SLE_CLIENT_PHASE_CONNECTED);
        g_sle_client_ssap_ready = 0;
        sle_client_emit(&evt);
        SsapcExchangeInfo info = { 0 };
        info.mtuSize = SLE_MTU_SIZE_DEFAULT;
        info.version = 1;
        SsapcExchangeInfoReq(g_client_id, conn_id, &info);
        if (addr != NULL) {
            SlePairRemoteDevice(addr);
        }
    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        sle_client_set_phase(SLE_CLIENT_PHASE_SEEKING);
        g_sle_client_ssap_ready = 0;
        sle_client_emit(&evt);
        if (addr != NULL) {
            SleRemovePairedRemoteDevice(addr);
        }
        sle_client_start_seek();
    } else {
        sle_client_emit(&evt);
    }
}

static void sle_uart_client_sample_connect_cbk_register(void)
{
    g_sle_uart_connect_cbk.connectStateChangedCb = sle_uart_client_sample_connect_state_changed_cbk;
    SleConnectionRegisterCallbacks(&g_sle_uart_connect_cbk);
}

static void sle_uart_client_sample_exchange_info_cbk(uint8_t client_id, uint16_t conn_id, ssap_exchange_info_t *param,
                                                     errcode_t status)
{
    sle_client_event_info_t evt = {
        .event = SLE_CLIENT_EVT_MTU_EXCHANGE,
        .status = status,
        .client_id = client_id,
        .conn_id = conn_id,
    };

    sle_client_set_phase(SLE_CLIENT_PHASE_SSAP_DISCOVERING);
    sle_client_emit(&evt);
    if (status != ERRCODE_SLE_SUCCESS || param == NULL) {
        return;
    }
    printf("%s mtu=%u version=%u\r\n", SLE_CLIENT_SDK_LOG,
        (unsigned)param->mtu_size, (unsigned)param->version);
    ssapc_find_structure_param_t find_param = { 0 };
    find_param.type = 1;
    find_param.start_hdl = 1;
    find_param.end_hdl = 0xFFFF;
    (void)ssapc_find_structure(client_id, conn_id, &find_param);
}

static void sle_uart_client_sample_find_structure_cbk(uint8_t client_id, uint16_t conn_id,
                                                      ssapc_find_service_result_t *service,
                                                      errcode_t status)
{
    sle_client_event_info_t evt = {
        .event = SLE_CLIENT_EVT_SSAP_SERVICE,
        .status = status,
        .client_id = client_id,
        .conn_id = conn_id,
    };

    if (service != NULL) {
        evt.ssap_handle = service->start_hdl;
        g_sle_uart_find_service_result.start_hdl = service->start_hdl;
        g_sle_uart_find_service_result.end_hdl = service->end_hdl;
        (void)memcpy_s(&g_sle_uart_find_service_result.uuid, sizeof(sle_uuid_t),
            &service->uuid, sizeof(sle_uuid_t));
    }
    sle_client_emit(&evt);
}

static void sle_uart_client_sample_find_property_cbk(uint8_t client_id, uint16_t conn_id,
                                                     ssapc_find_property_result_t *property, errcode_t status)
{
    sle_client_event_info_t evt = {
        .event = SLE_CLIENT_EVT_SSAP_PROPERTY,
        .status = status,
        .client_id = client_id,
        .conn_id = conn_id,
    };

    if (property != NULL) {
        evt.ssap_handle = property->handle;
        g_sle_uart_send_param.handle = property->handle;
        g_sle_uart_send_param.type = SSAP_PROPERTY_TYPE_VALUE;
    }
    sle_client_emit(&evt);
}

static void sle_uart_client_sample_find_structure_cmp_cbk(uint8_t client_id, uint16_t conn_id,
                                                          ssapc_find_structure_result_t *structure_result,
                                                          errcode_t status)
{
    sle_client_event_info_t evt = {
        .event = SLE_CLIENT_EVT_SSAP_READY,
        .status = status,
        .client_id = client_id,
        .conn_id = conn_id,
    };

    unused(structure_result);
    unused(conn_id);
    if (status == ERRCODE_SLE_SUCCESS) {
        sle_client_set_phase(SLE_CLIENT_PHASE_SSAP_READY);
        g_sle_client_ssap_ready = 1;
    }
    sle_client_emit(&evt);
}

static void sle_uart_client_sample_write_cfm_cb(uint8_t client_id, uint16_t conn_id,
                                                ssapc_write_result_t *write_result, errcode_t status)
{
    sle_client_event_info_t evt = {
        .event = SLE_CLIENT_EVT_WRITE_CFM,
        .status = status,
        .client_id = client_id,
        .conn_id = conn_id,
    };

    if (write_result != NULL) {
        evt.ssap_handle = write_result->handle;
    }
    sle_client_emit(&evt);
}

static void sle_uart_client_sample_ssapc_cbk_register(ssapc_notification_callback notification_cb,
                                                      ssapc_indication_callback indication_cb)
{
    g_sle_uart_ssapc_cbk.exchange_info_cb = sle_uart_client_sample_exchange_info_cbk;
    g_sle_uart_ssapc_cbk.find_structure_cb = sle_uart_client_sample_find_structure_cbk;
    g_sle_uart_ssapc_cbk.ssapc_find_property_cbk = sle_uart_client_sample_find_property_cbk;
    g_sle_uart_ssapc_cbk.find_structure_cmp_cb = sle_uart_client_sample_find_structure_cmp_cbk;
    g_sle_uart_ssapc_cbk.write_cfm_cb = sle_uart_client_sample_write_cfm_cb;
    g_sle_uart_ssapc_cbk.notification_cb = notification_cb;
    g_sle_uart_ssapc_cbk.indication_cb = indication_cb;
    ssapc_register_callbacks(&g_sle_uart_ssapc_cbk);
}


static errcode_t sle_uuid_client_register(void)
{
    errcode_t ret;
    SleUuid app_uuid = {0};

    printf("[uuid client] ssapc_register_client \r\n");
    app_uuid.len = sizeof(g_sle_uuid_app_uuid);
    if (memcpy_s(app_uuid.uuid, app_uuid.len, g_sle_uuid_app_uuid, sizeof(g_sle_uuid_app_uuid)) != EOK) {
        return ERRCODE_SLE_FAIL;
    }
    ret = SsapcRegisterClient(&app_uuid, &g_client_id);

    return ret;
}



/* device通过handle向host发送数据：report */
errcode_t sle_uart_client_send_report_by_handle(const uint8_t *data, uint8_t len)
{
    ssapc_write_param_t param = { 0 };
    uint16_t handle = g_sle_uart_send_param.handle;

    if (data == NULL || len == 0) {
        return ERRCODE_SLE_PARAM_ERR;
    }
    if (handle == 0) {
        handle = g_sle_uart_find_service_result.start_hdl;
    }
    if (handle == 0 || g_sle_uart_conn_id == 0) {
        return ERRCODE_SLE_FAIL;
    }

    param.handle = handle;
    param.type = SSAP_PROPERTY_TYPE_VALUE;
    param.data = receive_buf;
    param.data_len = len;
    if (memcpy_s(param.data, sizeof(receive_buf), data, len) != EOK) {
        return ERRCODE_SLE_FAIL;
    }

    return SsapWriteReq(g_client_id, g_sle_uart_conn_id, &param);
}


static void ssapc_notification_callbacks(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data,
    errcode_t status)
{
    sle_client_event_info_t evt = {
        .event = SLE_CLIENT_EVT_NOTIFICATION,
        .status = status,
        .client_id = client_id,
        .conn_id = conn_id,
    };

    if (data != NULL && data->data != NULL && data->data_len > 0) {
        evt.notify_len = data->data_len;
        evt.notify_cmd = data->data[0];
    }
    sle_client_emit(&evt);
}

void ssapc_indication_callbacks(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data,
    errcode_t status){
        (void)client_id;
        (void)conn_id;
        (void)data;
        (void)status;
    }


void sle_client_stack_init(void)
{
    uint8_t local_addr[SLE_ADDR_LEN] = SLE_SDK_CLIENT_ADDR_INIT;
    SleAddr local_address;

    sle_client_set_phase(SLE_CLIENT_PHASE_STACK_DOWN);
    g_sle_client_ssap_ready = 0;
    local_address.type = 0;
    (void)memcpy_s(local_address.addr, SLE_ADDR_LEN, local_addr, SLE_ADDR_LEN);
    (void)sle_uuid_client_register();
    sle_uart_client_sample_seek_cbk_register();
    sle_uart_client_sample_connect_cbk_register();
    sle_uart_client_sample_ssapc_cbk_register(ssapc_notification_callbacks, ssapc_indication_callbacks);
    (void)EnableSle();
    (void)SleSetLocalAddr(&local_address);
}
