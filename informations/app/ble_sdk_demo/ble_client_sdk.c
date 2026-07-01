/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * ble_client_sdk.c — GATT Client、扫描与连接（协议栈封装，无 APP_FEATURE_INIT）
 */

#include "securec.h"
#include "soc_osal.h"
#include "osal_debug.h"
#include "osal_addr.h"
#include "uart.h"
#include "bts_le_gap.h"
#include "bts_gatt_client.h"
#include "ble_client_sdk.h"
#include "ble_sdk_common.h"

#define UUID16_LEN 2
#define BLE_CLIENT_SDK_LOG "[ble client sdk]"
#define BLE_CLIENT_SDK_ERR "[ble client sdk err]"

/* client id, invalid client id is "0" */
static uint8_t g_uart_client_id = 0;
/* connection id, invalid client id is "0" */
static uint16_t g_uart_conn_id = 0;
/* max transport unit, default is 100 */
static uint16_t g_uart_mtu = 100;
/* characteristic handle */
static uint16_t g_ble_uart_chara_hanle_write_value = 0;
static uint8_t g_ble_client_connected = 0;
static uint8_t g_ble_stack_ready = 0;
static uint8_t g_ble_pending_gap_scan = 0;
static ble_client_phase_t g_ble_client_phase = BLE_CLIENT_PHASE_STACK_DOWN;

/* uart client app uuid for test */
static bt_uuid_t g_client_app_uuid = { UUID16_LEN, { 0 } };

/* 扫描匹配并连接的 Server 地址（可由 demo 通过 ble_client_set_target_server_addr 覆盖） */
static uint8_t g_ble_server_addr_connect[BD_ADDR_LEN] = BLE_SDK_SERVER_ADDR_SCAN_MATCH_INIT;

static uint8_t g_ble_uart_name_value[] = BLE_SDK_CLIENT_NAME;
static uint8_t g_ble_uart_client_addr[] = BLE_SDK_CLIENT_ADDR_INIT;


static uint16_t g_ble_scan_interval = 0x48;
static uint16_t g_ble_scan_window = 0x48;
static uint8_t g_ble_scan_type = 0x00;
static uint8_t g_ble_scan_phy = 0x01;
static uint8_t g_ble_scan_filter_policy = 0x00;

static errcode_t ble_client_set_scan_parameters(void)
{
    errcode_t ret = ERRCODE_BT_SUCCESS;
    gap_ble_scan_params_t params = { 0 };
    params.scan_interval = g_ble_scan_interval;
    params.scan_window = g_ble_scan_window;
    params.scan_type = g_ble_scan_type;
    params.scan_phy = g_ble_scan_phy;
    params.scan_filter_policy = g_ble_scan_filter_policy;
    ret = gap_ble_set_scan_parameters(&params);
    if (ret != ERRCODE_BT_SUCCESS) {
        printf("gap_ble_set_scan_parameters ret = %x\n", ret);
    }
    return ret;
}

static errcode_t ble_client_gap_start_scan(void)
{
    g_ble_client_phase = BLE_CLIENT_PHASE_SCANNING;
    printf("%s phase: SCANNING (GAP scan)\r\n", BLE_CLIENT_SDK_LOG);
    return gap_ble_start_scan();
}

static void bts_data_to_uuid_len2(uint16_t uuid_data, bt_uuid_t *out_uuid)
{
    out_uuid->uuid_len = UUID16_LEN;
    out_uuid->uuid[0] = (uint8_t)(uuid_data >> 8); /* 8: octet bit num */
    out_uuid->uuid[1] = (uint8_t)(uuid_data);
}

/* ble client discover all service */
errcode_t ble_client_discover_all_service(uint16_t conn_id)
{
    bt_uuid_t service_uuid = { 0 }; /* uuid length is zero, discover all service */
    return gattc_discovery_service(g_uart_client_id, conn_id, &service_uuid);
}

/* ble client write data to server */
errcode_t ble_client_write(uint8_t *data, uint16_t len, uint16_t handle)
{
    gattc_handle_value_t uart_handle_value = { 0 };
    uart_handle_value.handle = handle;
    uart_handle_value.data_len = len;
    uart_handle_value.data = data;
    printf("%s ble_client_write len: %d, g_uart_client_id: %x\n",
                BLE_CLIENT_SDK_LOG, len, g_uart_client_id);
    for (uint16_t i = 0; i < len; i++) {
        printf("%02x", data[i]);
    }
    printf("\n");
    errcode_t ret = gattc_write_cmd(g_uart_client_id, g_uart_conn_id, &uart_handle_value);
    if (ret != ERRCODE_BT_SUCCESS) {
        printf("%s gattc_write_cmd failed\n", BLE_CLIENT_SDK_LOG);
        return ERRCODE_BT_FAIL;
    }
    return ERRCODE_BT_SUCCESS;
}

/* ble client set scan param callback */
void ble_client_set_scan_param_cbk(errcode_t status)
{
    printf("%s set scan param status: %d\n", BLE_CLIENT_SDK_LOG, status);
    gap_ble_remove_all_pairs();
    (void)ble_client_gap_start_scan();
}

static int ble_client_adv_has_server_name(const gap_scan_result_data_t *scan)
{
    const uint8_t *adv;
    uint8_t len;
    uint8_t i;
    const size_t name_len = sizeof(BLE_SDK_SERVER_NAME) - 1;

    if (scan == NULL || scan->adv_data == NULL || scan->adv_len == 0) {
        return 0;
    }
    adv = scan->adv_data;
    len = scan->adv_len;
    i = 0;
    while (i + 1 < len) {
        uint8_t field_len = adv[i];

        if (field_len == 0 || (uint16_t)i + 1u + (uint16_t)field_len > len) {
            break;
        }
        if ((adv[i + 1] == 0x08 || adv[i + 1] == 0x09) &&
            field_len >= 1 + name_len &&
            memcmp(&adv[i + 2], BLE_SDK_SERVER_NAME, name_len) == 0) {
            return 1;
        }
        i = (uint8_t)(i + 1 + field_len);
    }
    return 0;
}

/* ble client scan result callback */
void ble_client_scan_result_cbk(gap_scan_result_data_t *scan_result_data)
{
    int addr_hit = ble_sdk_addr_match_with_reverse(g_ble_server_addr_connect, scan_result_data->addr.addr);
    int name_hit = ble_client_adv_has_server_name(scan_result_data);

    if (addr_hit || name_hit) {
        gap_ble_stop_scan();
        g_ble_client_phase = BLE_CLIENT_PHASE_CONNECTING;
        printf("%s phase: CONNECTING (GAP connect)\r\n", BLE_CLIENT_SDK_LOG);
        printf("%s scan hit (%s) addr " BLE_SDK_ADDR_PRINT_FMT " rssi=%d\r\n",
            BLE_CLIENT_SDK_LOG, name_hit ? "name OHOS_BLE" : "mac",
            BLE_SDK_ADDR_PRINT_ARG(scan_result_data->addr.addr), (int)scan_result_data->rssi);
        bd_addr_t bt_uart_client_addr = { 0 };
        bt_uart_client_addr.type = scan_result_data->addr.type;
        if (memcpy_s(bt_uart_client_addr.addr, BD_ADDR_LEN, scan_result_data->addr.addr, BD_ADDR_LEN) != EOK) {
            printf("%s add server app addr memcpy failed\r\n", BLE_CLIENT_SDK_ERR);
            return;
        }
        gap_ble_connect_remote_device(&bt_uart_client_addr);
    }
}

/* ble client connect state change callback */
void ble_client_connect_change_cbk(uint16_t conn_id, bd_addr_t *addr, gap_ble_conn_state_t conn_state,
                                        gap_ble_pair_state_t pair_state, gap_ble_disc_reason_t disc_reason)
{
    bd_addr_t bt_uart_client_addr = { 0 };
    bt_uart_client_addr.type = addr->type;
    g_uart_conn_id = conn_id;
    if (memcpy_s(bt_uart_client_addr.addr, BD_ADDR_LEN, addr->addr, BD_ADDR_LEN) != EOK) {
        printf("%s add server app addr memcpy failed\r\n", BLE_CLIENT_SDK_ERR);
        return;
    }
    printf("%s connect state change conn_id: %d, status: %d, pair_status:%d, disc_reason %x\n",
                BLE_CLIENT_SDK_LOG, conn_id, conn_state, pair_state, disc_reason);

    if (conn_state == GAP_BLE_STATE_CONNECTED  &&  pair_state == GAP_BLE_PAIR_NONE) {
        g_ble_client_connected = 1;
        g_ble_client_phase = BLE_CLIENT_PHASE_CONNECTED;
        printf("%s phase: CONNECTED, MTU then GATT discover\r\n", BLE_CLIENT_SDK_LOG);
        gattc_exchange_mtu_req(g_uart_client_id, g_uart_conn_id, g_uart_mtu);
        //gap_ble_pair_remote_device(addr);
    } else if (conn_state == GAP_BLE_STATE_DISCONNECTED) {
        g_ble_client_connected = 0;
        g_ble_uart_chara_hanle_write_value = 0;
        g_ble_client_phase = BLE_CLIENT_PHASE_STACK_UP;
        printf("%s phase: disconnected, restart GAP scan\r\n", BLE_CLIENT_SDK_LOG);
        (void)ble_client_start_gap_scan();
        return;
    }
}

/* ble client pair result callback */
void ble_client_pair_result_cb(uint16_t conn_id, const bd_addr_t *addr, errcode_t status)
{
    printf("%s pair result conn_id: %d,status: %d \n", BLE_CLIENT_SDK_LOG, conn_id, status);
    printf("addr:\n");
    for (uint8_t i = 0; i < BD_ADDR_LEN; i++) {
        printf("%2x", addr->addr[i]);
    }
    printf("\n");
    gattc_exchange_mtu_req(g_uart_client_id, g_uart_conn_id, g_uart_mtu);
}


/* ble client bt stack enable callback */
void ble_client_enable_cbk(errcode_t status)
{
    printf("ble enable: %d\n", status);
    errcode_t ret = 0;
    bd_addr_t ble_addr = { 0 };
    ble_addr.type = BT_ADDRESS_TYPE_PUBLIC_DEVICE_ADDRESS;
    osal_msleep(500); /* 延时5s，等待SLE初始化完毕 */
    if (memcpy_s(ble_addr.addr, BD_ADDR_LEN, g_ble_uart_client_addr, sizeof(g_ble_uart_client_addr)) != EOK) {
        printf("%s add server app addr memcpy failed\n", BLE_CLIENT_SDK_ERR);
        return;
    }
    ret = gap_ble_set_local_name(g_ble_uart_name_value, sizeof(g_ble_uart_name_value));
    if (ret != ERRCODE_BT_SUCCESS) {
        printf("%s gap_ble_set_local_name ret = %x\n", BLE_CLIENT_SDK_ERR, ret);
    }
    ret = gap_ble_set_local_addr(&ble_addr);
    if (ret != ERRCODE_BT_SUCCESS) {
        printf("%s gap_ble_set_local_addr ret = %x\n", BLE_CLIENT_SDK_ERR, ret);
    }
    ret = gattc_register_client(&g_client_app_uuid, &g_uart_client_id);
    if (ret != ERRCODE_BT_SUCCESS) {
        printf("%s gattc_register_client ret = %x\n", BLE_CLIENT_SDK_ERR, ret);
    }

    g_ble_stack_ready = 1;
    g_ble_client_phase = BLE_CLIENT_PHASE_STACK_UP;
    printf("%s phase: STACK_UP (call ble_client_start_gap_scan in demo)\r\n", BLE_CLIENT_SDK_LOG);
    if (g_ble_pending_gap_scan != 0) {
        (void)ble_client_set_scan_parameters();
    }
    printf("ble enable end: %d\n", status);
}

/* ble client service discovery callback */
static void ble_client_discover_service_cbk(uint8_t client_id, uint16_t conn_id,
                                                 gattc_discovery_service_result_t *service, errcode_t status)
{
    gattc_discovery_character_param_t param = { 0 };
    printf("%s Discovery service callback client:%d conn_id:%d\n", BLE_CLIENT_SDK_LOG, client_id, conn_id);
    printf("%s start handle:%d end handle:%d uuid_len:%d uuid:\n",
                BLE_CLIENT_SDK_LOG, service->start_hdl, service->end_hdl, service->uuid.uuid_len);
    for (uint8_t i = 0; i < service->uuid.uuid_len; i++) {
        printf("%02x", service->uuid.uuid[i]);
    }
    printf("\n %s status:%d\n", BLE_CLIENT_SDK_LOG, status);
    param.service_handle = service->start_hdl;
    param.uuid.uuid_len = service->uuid.uuid_len; /* uuid length is zero, discover all character */
    if (memcpy_s(param.uuid.uuid, param.uuid.uuid_len, service->uuid.uuid, service->uuid.uuid_len) != 0) {
        printf("%s memcpy error\n", BLE_CLIENT_SDK_ERR);
    }
    gattc_discovery_character(g_uart_client_id, conn_id, &param);
}

/* ble client character discovery callback */
static void ble_client_discover_character_cbk(uint8_t client_id, uint16_t conn_id,
                                                   gattc_discovery_character_result_t *character, errcode_t status)
{
    for (uint8_t i = 0; i < character->uuid.uuid_len; i++) {
        printf("%02x", character->uuid.uuid[i]);
    }
    printf("\n%s discover character declare_handle:%d, value_handle:%d, properties:%2x\n",
                BLE_CLIENT_SDK_LOG, character->declare_handle, character->value_handle, character->properties);
    printf("%s client_id:%d, conn_id = %d, status:%d\n", BLE_CLIENT_SDK_LOG, client_id, conn_id, status);
    bt_uuid_t write_uuid = { 0 };
    bts_data_to_uuid_len2(BLE_CLIENT_UUID_RX, &write_uuid);
    write_uuid.uuid_len = BT_UUID_MAX_LEN;
    if (memcmp(character->uuid.uuid, write_uuid.uuid, character->uuid.uuid_len) == 0) {
        g_ble_uart_chara_hanle_write_value = character->value_handle;
        g_ble_client_phase = BLE_CLIENT_PHASE_GATT_READY;
        printf("%s phase: GATT_READY write_hdl=%u (step4 uart ok)\r\n",
            BLE_CLIENT_SDK_LOG, (unsigned)character->value_handle);
    }
    gattc_discovery_descriptor(g_uart_client_id, conn_id, character->declare_handle);
}

/* ble client descriptor discovery callback */
static void ble_client_discover_descriptor_cbk(uint8_t client_id, uint16_t conn_id,
    gattc_discovery_descriptor_result_t *descriptor, errcode_t status)
{
    printf("%s Discovery descriptor----client:%d conn_id:%d uuid len:%d, uuid:\n",
                BLE_CLIENT_SDK_LOG, client_id, conn_id, descriptor->uuid.uuid_len);
    for (uint8_t i = 0; i < descriptor->uuid.uuid_len; i++) {
        printf("%02x", descriptor->uuid.uuid[i]);
    }
    printf("\n%s descriptor handle:%d, status:%d\n", BLE_CLIENT_SDK_LOG, descriptor->descriptor_hdl, status);

    gattc_read_req_by_uuid_param_t paramsss = { 0 };
    paramsss.uuid = descriptor->uuid;
    paramsss.start_hdl = descriptor->descriptor_hdl;
    paramsss.end_hdl = descriptor->descriptor_hdl;
    gattc_read_req_by_uuid(client_id, conn_id, &paramsss);
}

/* ble client compare service uuid */
static void ble_client_discover_service_compl_cbk(uint8_t client_id, uint16_t conn_id, bt_uuid_t *uuid,
                                                       errcode_t status)
{
    printf("%s Discovery service complete----client:%d conn_id:%d uuid len:%d uuid:\n",
                BLE_CLIENT_SDK_LOG, client_id, conn_id, uuid->uuid_len);
    for (uint8_t i = 0; i < uuid->uuid_len; i++) {
        printf("%02x", uuid->uuid[i]);
    }
    printf("\n%s status:%d\n", BLE_CLIENT_SDK_LOG, status);
}

/* ble client character discovery complete callback */
static void ble_client_discover_character_compl_cbk(uint8_t client_id, uint16_t conn_id,
                                                         gattc_discovery_character_param_t *param, errcode_t status)
{
    printf("%s Discovery character complete----client:%d conn_id:%d uuid len:%d uuid: \n",
                BLE_CLIENT_SDK_LOG, client_id, conn_id, param->uuid.uuid_len);
    for (uint8_t i = 0; i < param->uuid.uuid_len; i++) {
        printf("%02x", param->uuid.uuid[i]);
    }
    printf("\n%s service handle:%d status:%d\n", BLE_CLIENT_SDK_LOG, param->service_handle, status);
}

/* ble client descriptor discovery complete callback */
static void ble_client_discover_descriptor_compl_cbk(uint8_t client_id, uint16_t conn_id,
                                                          uint16_t character_handle, errcode_t status)
{
    printf("%s Discovery descriptor complete----client:%d conn_id:%d\n", BLE_CLIENT_SDK_LOG, client_id, conn_id);
    printf("%s charatcer handle:%d, status:%d\n", BLE_CLIENT_SDK_LOG, character_handle, status);
}

/* Callback invoked when receive read response */
static void ble_client_read_cfm_cbk(uint8_t client_id, uint16_t conn_id, gattc_handle_value_t *read_result,
                                         gatt_status_t status)
{
    printf("%s Read result client:%d conn_id:%d\n", BLE_CLIENT_SDK_LOG, client_id, conn_id);
    printf("%s handle:%d data_len:%d\ndata:", BLE_CLIENT_SDK_LOG, read_result->handle, read_result->data_len);
    for (uint8_t i = 0; i < read_result->data_len; i++) {
        printf("%02x", read_result->data[i]);
    }
    printf("\n%s status:%d\n", BLE_CLIENT_SDK_LOG, status);
}

/* Callback invoked when read complete */
static void ble_client_read_compl_cbk(uint8_t client_id, uint16_t conn_id, gattc_read_req_by_uuid_param_t *param,
                                           errcode_t status)
{
    printf("%s Read by uuid complete----client:%d conn_id:%d\n", BLE_CLIENT_SDK_LOG, client_id, conn_id);
    printf("%s start handle:%d end handle:%d uuid len:%d uuid:\n",
                BLE_CLIENT_SDK_LOG, param->start_hdl, param->end_hdl, param->uuid.uuid_len);
    for (uint8_t i = 0; i < param->uuid.uuid_len; i++) {
        printf("%02x", param->uuid.uuid[i]);
    }
    printf("\n%s status:%d\n", BLE_CLIENT_SDK_LOG, status);
}

/* Callback invoked when receive write response */
static void ble_client_write_cfm_cbk(uint8_t client_id, uint16_t conn_id, uint16_t handle, gatt_status_t status)
{
    printf("%s Write result----client:%d conn_id:%d handle:%d\n", BLE_CLIENT_SDK_LOG, client_id, conn_id, handle);
    printf("%s status:%d\n", BLE_CLIENT_SDK_LOG, status);
}

/* Callback invoked when change MTU complete */
static void ble_client_mtu_changed_cbk(uint8_t client_id, uint16_t conn_id, uint16_t mtu_size, errcode_t status)
{
    printf("%s Mtu changed----client:%d conn_id:%d, mtu size:%d, status:%d\n",
                BLE_CLIENT_SDK_LOG, client_id, conn_id, mtu_size, status);
    ble_client_discover_all_service(conn_id);
}

/* Callback invoked when receive server notification */
static void ble_client_notification_cbk(uint8_t client_id, uint16_t conn_id, gattc_handle_value_t *data,
                                             errcode_t status)
{
    printf("%s Receive notification----client:%d conn_id:%d\n", BLE_CLIENT_SDK_LOG, client_id, conn_id);
    printf("%s handle:%d data_len:%d\ndata:", BLE_CLIENT_SDK_LOG, data->handle, data->data_len);
    /* 修复：使用 %.*s 限制长度，避免读到 buffer 外的内存 */
    printf("%s ble_client_notification_cbk %.*s", BLE_CLIENT_SDK_LOG, data->data_len, data->data);
    printf("\n%s status:%d\n", BLE_CLIENT_SDK_LOG, status);
    uapi_uart_write(BLE_SDK_UART_BUS, (uint8_t *)(data->data), data->data_len, 0);
}

/* Callback invoked when receive server indication */
static void ble_client_indication_cbk(uint8_t client_id, uint16_t conn_id, gattc_handle_value_t *data,
                                           errcode_t status)
{
    printf("%s Receive indication----client:%d conn_id:%d\n", BLE_CLIENT_SDK_LOG, client_id, conn_id);
    printf("%s handle:%d data_len:%d\ndata:", BLE_CLIENT_SDK_LOG, data->handle, data->data_len);
    /* Indication 回调使用 hex 打印，更安全（不依赖 \0 终止符） */
    for (uint8_t i = 0; i < data->data_len; i++) {
        printf("%02x ", data->data[i]);
    }
    printf("(len=%u)\n", data->data_len);
    printf("\n%s status:%d\n", BLE_CLIENT_SDK_LOG, status);
}

/* register gatt and gap callback */
errcode_t ble_client_register_callbacks(void)
{
    errcode_t ret = 0;

    gattc_callbacks_t cb = { 0 };
    gap_ble_callbacks_t gap_cb = { 0 };
    gap_cb.ble_enable_cb = ble_client_enable_cbk;
    gap_cb.set_scan_param_cb = ble_client_set_scan_param_cbk;
    gap_cb.scan_result_cb = ble_client_scan_result_cbk;
    gap_cb.conn_state_change_cb = ble_client_connect_change_cbk;
    gap_cb.pair_result_cb = ble_client_pair_result_cb;
    ret |= gap_ble_register_callbacks(&gap_cb);
    if (ret != ERRCODE_BT_SUCCESS) {
        printf("%s reg gap cbk failed ret = %d\n", BLE_CLIENT_SDK_ERR, ret);
    }

    cb.discovery_svc_cb = ble_client_discover_service_cbk;
    cb.discovery_svc_cmp_cb = ble_client_discover_service_compl_cbk;
    cb.discovery_chara_cb = ble_client_discover_character_cbk;
    cb.discovery_chara_cmp_cb = ble_client_discover_character_compl_cbk;
    cb.discovery_desc_cb = ble_client_discover_descriptor_cbk;
    cb.discovery_desc_cmp_cb = ble_client_discover_descriptor_compl_cbk;
    cb.read_cb = ble_client_read_cfm_cbk;
    cb.read_cmp_cb = ble_client_read_compl_cbk;
    cb.write_cb = ble_client_write_cfm_cbk;
    cb.mtu_changed_cb = ble_client_mtu_changed_cbk;
    cb.notification_cb = ble_client_notification_cbk;
    cb.indication_cb = ble_client_indication_cbk;
    ret = gattc_register_callbacks(&cb);
    if (ret != ERRCODE_BT_SUCCESS) {
        printf("%s reg gatt cbk failed ret = %d\n", BLE_CLIENT_SDK_ERR, ret);
    }

    return ret;
}

void ble_client_set_target_server_addr(const uint8_t addr[6])
{
    if (addr == NULL) {
        return;
    }
    if (memcpy_s(g_ble_server_addr_connect, BD_ADDR_LEN, addr, BD_ADDR_LEN) != EOK) {
        printf("%s set_target_server_addr memcpy fail\r\n", BLE_CLIENT_SDK_ERR);
    }
}

errcode_t ble_client_stack_init(void)
{
    errcode_t ret = ERRCODE_BT_SUCCESS;
    (void)osal_msleep(3000); /* 等待 SLE/BLE 共存初始化 */
    g_ble_stack_ready = 0;
    g_ble_pending_gap_scan = 0;
    g_ble_client_phase = BLE_CLIENT_PHASE_STACK_DOWN;
    ret |= ble_client_register_callbacks();
    ret |= enable_ble();
    if (ret != ERRCODE_BT_SUCCESS) {
        printf("%s stack_init enable_ble ret = %x\n", BLE_CLIENT_SDK_ERR, ret);
    }
    return ret;
}

errcode_t ble_client_start_gap_scan(void)
{
    g_ble_pending_gap_scan = 1;
    if (g_ble_stack_ready == 0) {
        printf("%s start_gap_scan: wait stack (enable cbk)\r\n", BLE_CLIENT_SDK_LOG);
        return ERRCODE_BT_SUCCESS;
    }
    return ble_client_set_scan_parameters();
}

errcode_t ble_client_init(void)
{
    errcode_t ret = ble_client_stack_init();
    if (ret != ERRCODE_BT_SUCCESS) {
        return ret;
    }
    return ble_client_start_gap_scan();
}

ble_client_phase_t ble_client_get_phase(void)
{
    return g_ble_client_phase;
}

uint16_t ble_client_get_write_handle(void)
{
    return g_ble_uart_chara_hanle_write_value;
}

uint8_t ble_client_get_connection_state(void)
{
    return g_ble_client_connected;
}

uint8_t ble_client_is_transfer_ready(void)
{
    return (g_ble_client_connected != 0 && g_ble_uart_chara_hanle_write_value != 0) ? 1 : 0;
}
