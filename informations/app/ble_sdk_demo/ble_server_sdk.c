/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * ble_server_sdk.c — GATT Server、GAP 连接与广播（协议栈封装，无 APP_FEATURE_INIT）
 */

#include "osal_addr.h"
#include "osal_debug.h"
#include "soc_osal.h"
#include "securec.h"
#include "errcode.h"
#include "bts_def.h"
#include "bts_gatt_stru.h"
#include "bts_gatt_server.h"
#include "uart.h"
#include "bts_le_gap.h"
#include "ble_server_adv.h"
#include "ble_server_sdk.h"
#include "ble_sdk_common.h"

/* uart gatt server id */
#define BLE_UART_SERVER_ID 			1
/* uart ble connect id */
#define BLE_SINGLE_LINK_CONNECT_ID 	1
/* octets of 16 bits uart */
#define UART16_LEN 					2
/* invalid attribute handle */
#define INVALID_ATT_HDL 			0
/* invalid server ID */
#define INVALID_SERVER_ID 			0

#define BLE_SERVER_SDK_LOG "[ble server sdk]"
#define BLE_SERVER_SDK_ERR "[ble server sdk err]"
/* 本 demo 仅注册 1 个 UART GATT 服务；计数须为 1 才会在 start_service 后开广播 */
#define BLE_SDK_SERVER_SERVICE_NUM 1

static uint16_t g_ble_uart_conn_id;
static uint8_t g_ble_uart_name_value[] = BLE_SDK_SERVER_NAME;
static uint8_t g_uart_server_app_uuid[] = { 0x00, 0x00 };
static uint8_t g_ble_uart_server_addr[] = BLE_SDK_SERVER_ADDR_INIT;
static uint8_t g_server_id = INVALID_SERVER_ID;
static uint8_t g_connection_state = 0;
static uint16_t g_notify_indicate_handle = 0;
static uint8_t g_service_num = 0;
static uint8_t g_peer_addr[BD_ADDR_LEN] = { 0 };
static uint8_t g_peer_addr_valid = 0;
static uint32_t g_connect_count = 0;
static uint32_t g_disconnect_count = 0;
static ble_server_link_listener_t g_link_listener = NULL;

static void ble_server_notify_link_listener(uint8_t connected, gap_ble_disc_reason_t disc_reason)
{
    ble_server_link_info_t info = { 0 };

    if (g_link_listener == NULL) {
        return;
    }
    info.conn_id = g_ble_uart_conn_id;
    info.gap_state = g_connection_state;
    info.disc_reason = (uint8_t)disc_reason;
    info.peer_addr_valid = g_peer_addr_valid;
    if (g_peer_addr_valid != 0) {
        if (memcpy_s(info.peer_addr, BD_ADDR_LEN, g_peer_addr, BD_ADDR_LEN) != EOK) {
            info.peer_addr_valid = 0;
        }
    }
    g_link_listener(&info, connected);
}

/* 将uint16的uuid数字转化为bt_uuid_t */
static void bts_data_to_uuid_len2(uint16_t uuid_data, bt_uuid_t *out_uuid)
{
    out_uuid->uuid_len = UART16_LEN;
    out_uuid->uuid[0] = (uint8_t)(uuid_data >> 8); /* 8: octet bit num */
    out_uuid->uuid[1] = (uint8_t)(uuid_data);
}

/* 设置注册服务时的name */
void ble_server_set_device_name(const uint8_t *name, const uint8_t len)
{
    size_t len_name = sizeof(g_ble_uart_name_value);
    if (memcpy_s(g_ble_uart_name_value, len_name, name, len) != EOK) {
        printf("%s memcpy name fail\n", BLE_SERVER_SDK_ERR);
    }
}

/* 创建服务 */
static void ble_server_add_service(void)
{
    bt_uuid_t uart_service_uuid = { 0 };
    bts_data_to_uuid_len2(BLE_SERVER_UUID_SERVICE, &uart_service_uuid);
    gatts_add_service(BLE_UART_SERVER_ID, &uart_service_uuid, true);
}

/* 添加uart发送服务的所有特征和描述符 */
static void ble_server_add_tx_characters_and_descriptors(uint8_t server_id, uint16_t srvc_handle)
{
    printf("%s TX characters:%d srv_handle:%d \n", BLE_SERVER_SDK_LOG, server_id, srvc_handle);
    bt_uuid_t characters_uuid = { 0 };
    uint8_t characters_value[] = { 0x12, 0x34 };
    bts_data_to_uuid_len2(BLE_SERVER_UUID_TX, &characters_uuid);
    gatts_add_chara_info_t character;
    character.chara_uuid = characters_uuid;
    character.properties = GATT_CHARACTER_PROPERTY_BIT_NOTIFY | GATT_CHARACTER_PROPERTY_BIT_READ;
    character.permissions = GATT_ATTRIBUTE_PERMISSION_READ | GATT_ATTRIBUTE_PERMISSION_WRITE;
    character.value_len = sizeof(characters_value);
    character.value = characters_value;
    gatts_add_characteristic(server_id, srvc_handle, &character);
    printf("%s characters_uuid:%2x %2x\n", BLE_SERVER_SDK_LOG, characters_uuid.uuid[0], characters_uuid.uuid[1]);

    static uint8_t ccc_val[] = { 0x01, 0x00 }; // notify
    bt_uuid_t ccc_uuid = { 0 };
    bts_data_to_uuid_len2(BLE_SERVER_UUID_CCCD, &ccc_uuid);
    gatts_add_desc_info_t descriptor;
    descriptor.desc_uuid = ccc_uuid;
    descriptor.permissions = GATT_ATTRIBUTE_PERMISSION_READ | GATT_CHARACTER_PROPERTY_BIT_WRITE |
        GATT_ATTRIBUTE_PERMISSION_WRITE;
    descriptor.value_len = sizeof(ccc_val);
    descriptor.value = ccc_val;
    gatts_add_descriptor(server_id, srvc_handle, &descriptor);
    printf("%s ccc_uuid:%2x %2x\n", BLE_SERVER_SDK_LOG, characters_uuid.uuid[0], characters_uuid.uuid[1]);
}

/* 添加uart接收服务的所有特征和描述符 */
static void ble_server_add_rx_characters_and_descriptors(uint8_t server_id, uint16_t srvc_handle)
{
    printf("%s RX characters:%d srv_handle: %d \n", BLE_SERVER_SDK_LOG, server_id, srvc_handle);
    bt_uuid_t characters_uuid = { 0 };
    uint8_t characters_value[] = { 0x12, 0x34 };
    bts_data_to_uuid_len2(BLE_SERVER_UUID_RX, &characters_uuid);
    gatts_add_chara_info_t character;
    character.chara_uuid = characters_uuid;
    character.properties = GATT_CHARACTER_PROPERTY_BIT_READ | GATT_CHARACTER_PROPERTY_BIT_WRITE_NO_RSP;
    character.permissions = GATT_ATTRIBUTE_PERMISSION_READ | GATT_ATTRIBUTE_PERMISSION_WRITE;
    character.value_len = sizeof(characters_value);
    character.value = characters_value;
    gatts_add_characteristic(server_id, srvc_handle, &character);
    printf("%s characters_uuid:%2x %2x\n", BLE_SERVER_SDK_LOG, characters_uuid.uuid[0], characters_uuid.uuid[1]);

    bt_uuid_t ccc_uuid = { 0 };
    /* uart client characteristic configuration value for test */
    static uint8_t ccc_val[] = { 0x00, 0x00 };
    bts_data_to_uuid_len2(BLE_SERVER_UUID_CCCD, &ccc_uuid);
    gatts_add_desc_info_t descriptor;
    descriptor.desc_uuid = ccc_uuid;
    descriptor.permissions = GATT_ATTRIBUTE_PERMISSION_READ | GATT_ATTRIBUTE_PERMISSION_WRITE;
    descriptor.value_len = sizeof(ccc_val);
    descriptor.value = ccc_val;
    gatts_add_descriptor(server_id, srvc_handle, &descriptor);
    printf("%s ccc_uuid:%2x %2x\n", BLE_SERVER_SDK_LOG, characters_uuid.uuid[0], characters_uuid.uuid[1]);
}

bool bts_uart_compare_uuid(bt_uuid_t *uuid1, bt_uuid_t *uuid2)
{
    if (uuid1->uuid_len != uuid2->uuid_len) {
        return false;
    }
    if (memcmp(uuid1->uuid, uuid2->uuid, uuid1->uuid_len) != 0) {
        return false;
    }
    return true;
}

/* 服务添加回调 */
static void ble_server_service_add_cbk(uint8_t server_id, bt_uuid_t *uuid, uint16_t handle, errcode_t status)
{
    printf("%s add characters_and_descriptors cbk service:%d, srv_handle:%d, uuid_len:%d, status:%d, uuid:",
                BLE_SERVER_SDK_LOG, server_id, handle, uuid->uuid_len, status);
    for (int8_t i = 0; i < uuid->uuid_len; i++) {
        printf("%02x ", uuid->uuid[i]);
    }
    printf("\n");
    ble_server_add_tx_characters_and_descriptors(server_id, handle);
    ble_server_add_rx_characters_and_descriptors(server_id, handle);
    gatts_start_service(server_id, handle);
}

/* 特征添加回调 */
static void ble_server_char_add_cbk(uint8_t server_id, bt_uuid_t *uuid, uint16_t service_handle,
                                                   gatts_add_character_result_t *result, errcode_t status)
{
    printf("%s add character cbk service:%d service_hdl: %d char_hdl: %d char_val_hdl: %d uuid_len: %d \n",
                BLE_SERVER_SDK_LOG, server_id, service_handle, result->handle, result->value_handle, uuid->uuid_len);
    printf("uuid:");
    for (int8_t i = 0; i < uuid->uuid_len; i++) {
        printf("%02x ", uuid->uuid[i]);
    }
    bt_uuid_t characters_cbk_uuid = { 0 };
    bts_data_to_uuid_len2(BLE_SERVER_UUID_TX, &characters_cbk_uuid);
    characters_cbk_uuid.uuid_len = uuid->uuid_len;
    if (bts_uart_compare_uuid(uuid, &characters_cbk_uuid)) {
        g_notify_indicate_handle = result->value_handle;
    }
    printf("%s status:%d indicate_handle:%d\n", BLE_SERVER_SDK_LOG, status, g_notify_indicate_handle);
}

/* 描述符添加回调 */
static void ble_server_desc_add_cbk(uint8_t server_id, bt_uuid_t *uuid, uint16_t service_handle,
                                               uint16_t handle, errcode_t status)
{
    printf("%s service:%d service_hdl: %d desc_hdl: %d uuid_len: %d \n",
                BLE_SERVER_SDK_LOG, server_id, service_handle, handle, uuid->uuid_len);
    printf("uuid:");
    for (int8_t i = 0; i < uuid->uuid_len; i++) {
        printf("%02x ", (uint8_t)uuid->uuid[i]);
    }
    printf("%s status:%d\n", BLE_SERVER_SDK_LOG, status);
}

/* 开始服务回调 */
static void ble_server_service_start_cbk(uint8_t server_id, uint16_t handle, errcode_t status)
{
    g_service_num++;
    if ((g_service_num == BLE_SDK_SERVER_SERVICE_NUM) && (status == 0)) {
        printf("%s start service cbk , start adv\n", BLE_SERVER_SDK_LOG);
        ble_server_set_adv_data();
        ble_server_start_adv();
    }
    printf("%s start service:%2d service_hdl: %d status: %d\n",
                BLE_SERVER_SDK_LOG, server_id, handle, status);
}

static void ble_server_write_req_cbk(uint8_t server_id, uint16_t conn_id, gatts_req_write_cb_t *write_cb_para,
                                           errcode_t status)
{
    printf("%s ble uart write cbk server_id:%d, conn_id:%d, status%d\n",
        BLE_SERVER_SDK_LOG, server_id, conn_id, status);
    printf("%s ble uart write cbk len:%d, data:%.*s\r\n",
        BLE_SERVER_SDK_LOG, write_cb_para->length, (int)write_cb_para->length, write_cb_para->value);
    if ((write_cb_para->length > 0) && write_cb_para->value) {
        uapi_uart_write(BLE_SDK_UART_BUS, (uint8_t *)(write_cb_para->value), write_cb_para->length, 0);
    }
}

static void ble_server_read_req_cbk(uint8_t server_id, uint16_t conn_id, gatts_req_read_cb_t *read_cb_para,
    errcode_t status)
{
    printf("%s ReceiveReadReq--server_id:%d conn_id:%d\n", BLE_SERVER_SDK_LOG, server_id, conn_id);
    printf("%s request_id:%d, att_handle:%d offset:%d, need_rsp:%d, is_long:%d\n",
                BLE_SERVER_SDK_LOG, read_cb_para->request_id, read_cb_para->handle, read_cb_para->offset,
                read_cb_para->need_rsp, read_cb_para->is_long);
    printf("%s status:%d\n", BLE_SERVER_SDK_LOG, status);
}

static void ble_server_mtu_changed_cbk(uint8_t server_id, uint16_t conn_id, uint16_t mtu_size, errcode_t status)
{
    printf("%s MtuChanged--server_id:%d conn_id:%d\n", BLE_SERVER_SDK_LOG, server_id, conn_id);
    printf("%s mtusize:%d, status:%d\n", BLE_SERVER_SDK_LOG, mtu_size, status);
}

static void ble_server_adv_enable_cbk(uint8_t adv_id, adv_status_t status)
{
    printf("%s adv enable cbk adv_id:%d status:%d\n", BLE_SERVER_SDK_LOG, adv_id, status);
}

static void ble_server_adv_disable_cbk(uint8_t adv_id, adv_status_t status)
{
    printf("%s adv disable adv_id: %d, status:%d\n", BLE_SERVER_SDK_LOG, adv_id, status);
}

void ble_server_connect_change_cbk(uint16_t conn_id, bd_addr_t *addr, gap_ble_conn_state_t conn_state,
                                        gap_ble_pair_state_t pair_state, gap_ble_disc_reason_t disc_reason)
{
    unused(pair_state);
    g_ble_uart_conn_id = conn_id;
    g_connection_state = (uint8_t)conn_state;
    if (addr != NULL) {
        if (memcpy_s(g_peer_addr, BD_ADDR_LEN, addr->addr, BD_ADDR_LEN) == EOK) {
            g_peer_addr_valid = 1;
        }
    }
    printf("%s conn_id=%u state=%u pair=%d disc=0x%x\r\n",
        BLE_SERVER_SDK_LOG, (unsigned)conn_id, (unsigned)conn_state, (int)pair_state,
        (unsigned)disc_reason);
    if (addr != NULL) {
        ble_sdk_print_addr(BLE_SERVER_SDK_LOG, addr->addr);
    }

    if (conn_state == GAP_BLE_STATE_CONNECTED) {
        g_connect_count++;
        ble_server_notify_link_listener(1, disc_reason);
        return;
    }
    if (conn_state == GAP_BLE_STATE_DISCONNECTED) {
        g_disconnect_count++;
        ble_server_notify_link_listener(0, disc_reason);
        g_peer_addr_valid = 0;
        ble_server_set_adv_data();
        ble_server_start_adv();
    }
}
void ble_server_pair_result_cb(uint16_t conn_id, const bd_addr_t *addr, errcode_t status)
{
    printf("%s pair result conn_id: %d, status: %d, addr %x \n",
                BLE_SERVER_SDK_LOG, conn_id, status, addr[0]);
}

static errcode_t ble_server_register_callbacks(void)
{

    gap_ble_callbacks_t gap_cb = { 0 };
    gatts_callbacks_t service_cb = { 0 };
    gap_cb.start_adv_cb = ble_server_adv_enable_cbk;
    gap_cb.conn_state_change_cb = ble_server_connect_change_cbk;
    gap_cb.stop_adv_cb = ble_server_adv_disable_cbk;
    gap_cb.pair_result_cb = ble_server_pair_result_cb;
    errcode_t ret = gap_ble_register_callbacks(&gap_cb);


    service_cb.add_service_cb = ble_server_service_add_cbk;
    service_cb.add_characteristic_cb = ble_server_char_add_cbk;
    service_cb.add_descriptor_cb = ble_server_desc_add_cbk;
    service_cb.start_service_cb = ble_server_service_start_cbk;
    service_cb.read_request_cb = ble_server_read_req_cbk;
    service_cb.write_request_cb = ble_server_write_req_cbk;
    service_cb.mtu_changed_cb = ble_server_mtu_changed_cbk;
    ret = gatts_register_callbacks(&service_cb);
    if (ret != ERRCODE_BT_SUCCESS) {
        printf("%s reg service cbk failed ret = %d\n", BLE_SERVER_SDK_ERR, ret);
        return ret;
    }
    return ret;
}

void ble_server_init(void)
{
    (void)osal_msleep(3000); /* 延时3s，等待SLE初始化完毕 */
    ble_server_register_callbacks();
    enable_ble();

    errcode_t ret = 0;
    bt_uuid_t app_uuid = { 0 };
    bd_addr_t ble_addr = { 0 };
    app_uuid.uuid_len = sizeof(g_uart_server_app_uuid);
    if (memcpy_s(app_uuid.uuid, app_uuid.uuid_len, g_uart_server_app_uuid, sizeof(g_uart_server_app_uuid)) != EOK) {
        printf("%s add server app uuid memcpy failed\n", BLE_SERVER_SDK_ERR);
        return;
    }
    ble_addr.type = BT_ADDRESS_TYPE_PUBLIC_DEVICE_ADDRESS;
    if (memcpy_s(ble_addr.addr, BD_ADDR_LEN, g_ble_uart_server_addr, sizeof(g_ble_uart_server_addr)) != EOK) {
        printf("%s add server app addr memcpy failed\n", BLE_SERVER_SDK_ERR);
        return;
    }
    gap_ble_set_local_name(g_ble_uart_name_value, sizeof(g_ble_uart_name_value));
    gap_ble_set_local_addr(&ble_addr);
    ret = gatts_register_server(&app_uuid, &g_server_id);
    if ((ret != ERRCODE_BT_SUCCESS) || (g_server_id == INVALID_SERVER_ID)) {
        printf("%s add server failed\r\n", BLE_SERVER_SDK_ERR);
        return;
    }
    ble_server_add_service(); /* 添加uart服务 */
    printf("%s beginning add service\r\n", BLE_SERVER_SDK_LOG);
    bth_ota_init();

}

/* device向host发送数据：input report */
errcode_t ble_server_send_notify(uint8_t *data, uint16_t len)
{
    gatts_ntf_ind_t param = { 0 };
    uint16_t conn_id = g_ble_uart_conn_id;
    param.attr_handle = g_notify_indicate_handle;
    param.value_len = len;
    param.value = data;
    printf("%s send input report indicate_handle:%d\n", BLE_SERVER_SDK_LOG, g_notify_indicate_handle);
    gatts_notify_indicate(BLE_UART_SERVER_ID, conn_id, &param);
    return ERRCODE_BT_SUCCESS;
}

uint8_t ble_server_get_connection_state(void)
{
    return g_connection_state;
}

uint8_t ble_server_is_client_connected(void)
{
    return (g_connection_state == GAP_BLE_STATE_CONNECTED) ? 1 : 0;
}

uint16_t ble_server_get_conn_id(void)
{
    return g_ble_uart_conn_id;
}

void ble_server_get_peer_addr(uint8_t addr_out[6])
{
    if (addr_out == NULL) {
        return;
    }
    if (g_peer_addr_valid == 0) {
        (void)memset_s(addr_out, BD_ADDR_LEN, 0, BD_ADDR_LEN);
        return;
    }
    (void)memcpy_s(addr_out, BD_ADDR_LEN, g_peer_addr, BD_ADDR_LEN);
}

void ble_server_register_link_listener(ble_server_link_listener_t listener)
{
    g_link_listener = listener;
}

uint32_t ble_server_get_connect_count(void)
{
    return g_connect_count;
}

uint32_t ble_server_get_disconnect_count(void)
{
    return g_disconnect_count;
}