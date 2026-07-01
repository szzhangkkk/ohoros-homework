# sle_sdk_demo

**适用平台**：九联星闪开发板，海思 WS63E 芯片。

**作者**：SZTU鸿蒙课程组 王老师

相关文档：[实验指导书（2 人双板）](../../../docs/17-九联星闪开发板SLE实验指导书（学生版）.md) · [接口说明](../../../docs/15-九联星闪开发板星闪SLE接口与功能说明.md)

双板 **SLE SSAP 遥控实验**（Server 自动 Notify → Client LED），代码分层与 `ble_sdk_demo` / `wifi_sdk_demo` 一致：**sdk = 协议栈封装**，**demo = 应用入口与实验逻辑**。

九联开发板**无 USER 键**，由 **Server 在 Client 连上后自动间隔发送** `SsapsNotifyIndicate`（`0x00`/`0x01`），Client 收 Notify 控 GPIO10。

## 如何创建本工程

```bash
python3 applications/make_app.py -c sle_sdk_demo sle_sdk_demo
# 或已存在时：
python3 applications/make_app.py -e sle_sdk_demo sle_sdk_demo
python3 applications/make_app.py -l
```

用完整 **sle_sdk_demo** 目录覆盖 `applications/sample/wifi-iot/app/sle_sdk_demo/`。

## 配置板子角色并编译

编辑 `BUILD.gn` 顶部 `declare_args`：

| 变量 | 取值 | 说明 |
| ------------ | -------------- | ------ |
| `sle_sdk_role` | `"server"` / `"client"` | 烧录板角色 |
| `sle_sdk_led_enable` | `true` / `false` | `true`：Client GPIO10 控灯；`false`：仅打印 SLE 交互日志 |

| 烧录板 | `sle_sdk_role` |
| ------------ | -------------- |
| **Server 板** | `"server"` |
| **Client 板** | `"client"` |

```bash
python3 applications/make_app.py -b
```

Server 与 Client **须分别编译、分别烧录**。联调时 **先 Server 后 Client** 上电。

## 双板分工

| 板子 | 入口文件 | BUILD.gn |
| ------------ | ------------------- | ------------------------- |
| **Server 板** | `sle_server_demo.c` | `sle_sdk_role = "server"` |
| **Client 板** | `sle_client_demo.c` | `sle_sdk_role = "client"` |

| 板子 | SLE 角色 | 子任务 3 数据路径 |
| ------------ | ------------------ | -------------------------------------------------------- |
| **Server 板** | SSAP Server + 广播 | Client 连上后自动 `SsapsNotifyIndicate` 发送 `0x00/0x01` |
| **Client 板** | SSAP Client + 扫描连接 | 等待连接时 **GPIO10** 间隔闪烁；收 Server Notify 控灯 |

公共逻辑在 `sle_server_sdk.c`、`sle_server_adv.c`、`sle_client_sdk.c`；`sle_sdk_role` 决定只链接一个 `*_demo.c`。

## 默认参数（`sle_sdk_common.h`）

| 参数 | 默认值 | 说明 |
| ---------------- | -------------------- | ------------------------------------------------------------- |
| 广播名 | `OHOS_SLE` | `SLE_SDK_SERVER_NAME`，Client 扫描按名称匹配 |
| Server 本地地址 | `78:70:60:88:96:46` | `SLE_SDK_SERVER_ADDR_INIT`，**示例 MAC** |
| Client 本地地址 | `13:67:5C:07:00:51` | `SLE_SDK_CLIENT_ADDR_INIT`，**示例 MAC** |
| Client LED | GPIO **10** | `SLE_SDK_LED_GPIO`，排针接 LED、**低电平点亮**（同 [06-GPIO的使用.md](../../../docs/06-GPIO的使用.md)） |
| LED 开关 | `sle_sdk_led_enable` | `false` 时不操作 GPIO，只打串口日志 |
| LED 闪烁间隔 | `500` ms | `SLE_SDK_LED_BLINK_INTERVAL_MS`，等待连接时 Client 自动亮灭循环 |
| 自动 Notify 间隔 | `2000` ms | `SLE_SDK_LED_CMD_INTERVAL_MS`，Client 连上后 Server 自动发 `0x00/0x01` |
| 指示命令 | `0x00` 灭 / `0x01` 亮 | `SLE_SDK_CMD_LED_OFF` / `ON` |

**MAC 地址说明**：表中地址仅为示例，双板联调须配置**不同** MAC；修改 `SLE_SDK_SERVER_ADDR_INIT` / `SLE_SDK_CLIENT_ADDR_INIT` 后 Server 与 Client **均须重新编译**。

## 双板联调步骤

1. **Server 板**：`sle_sdk_role = "server"`，编译烧录
2. 串口（115200）确认：`announce name: OHOS_SLE`、`sle enable callback`、`set announce data success`
3. **Client 板**：`sle_sdk_role = "client"`，编译烧录（建议 **后上电**）
4. Client 串口应出现：`seek` → `CONNECTED` → `SSAP_READY`
5. Server 串口约每 2s 出现 `auto SSAP notify`；Client 收到 `Server->Client notify` 后 **GPIO10** LED 切换

**上电顺序**：先 Server 广播稳定，再 Client 扫描；Client 断线后 SDK 会自动重新扫描。

## 串口日志示例

串口 **UART0，115200**。下列为双板联调成功时的典型输出（`sle_sdk_led_enable = true`）；`...` 表示中间省略的 SDK 细节日志，实际板子上会更长。

### Server 板（`sle_sdk_role = "server"`）

上电后应看到广播名、本地地址与 SSAP 服务注册，最后进入 `set announce data success` 并开始广播：

```
[SleServerDemo] task started
[SleServerDemo] ===== SLE Server dual-board =====
[SleServerDemo] announce name: OHOS_SLE, Client LED ctrl: ON
[SleServerDemo] 78:70:60:88:96:46
[SleServerDemo] step: sle_uart_server_init() (SSAP + announce)
[sle uart server] init ok
[sle uart server] sle enable callback status:0
[sle uart server] sle uart add service in
[sle uart server] sle uart add service, server_id:..., service_handle:..., property_handle:...
[sle uart server] local_name_len = 8
[sle uart server] local_name: 0x4f 0x48 0x4f 0x53 0x5f 0x53 0x4c 0x45
[sle uart server] data.announce_data_len = ...
[sle uart server] set announce data success.
[sle uart server] sle announce enable callback id:01, state:0
[SleServerDemo] ready: wait Client connect, auto notify every 2000 ms
```

Client 连上后 Server 会打印连接与配对回调（Client 示例地址 `13:67:5C:07:00:51`）：

```
[sle uart server] connect state changed callback conn_id:0x01, conn_state:0x2, pair_state:0x0, disc_reason:0x0
[sle uart server] connect state changed callback addr 13:67:5C:07:00:51
[sle uart server] pair complete conn_id:01, status:0
[sle uart server] pair complete addr 13:67:5C:07:00:51
```

Client 连上后 Server 自动发 Notify：

```
[SleServerDemo] auto SSAP notify cmd=0x01 (ON)
[SleServerDemo] auto SSAP notify cmd=0x00 (OFF)
```

### Client 板（`sle_sdk_role = "client"`）

上电后先启动协议栈并扫描；等待连接期间 **GPIO10 LED 每 500ms 闪烁**（`LED blink -> ON/OFF`）：

```
[SleClientDemo] task started
[SleClientDemo] ===== SLE Client dual-board =====
[SleClientDemo] seek name: OHOS_SLE, LED ctrl: ON (GPIO10, blink until SSAP)
[SleClientDemo] 13:67:5C:07:00:51
[SleClientDemo] step: sle_client_stack_init()
[SleClientDemo] init phase=STACK_DOWN conn_id=0 ssap_ready=0
[SleClientDemo] LED blink mode ON (interval 500 ms)
[SleClientDemo] evt=STACK_ENABLED phase=STACK_UP status=0x0 conn=0
[SleClientDemo] step0: SLE stack enabled
[SleClientDemo] evt=SEEK_STARTED phase=SEEKING status=0x0 conn=0
[SleClientDemo] step1: seeking target "OHOS_SLE"
[SleClientDemo] LED blink -> ON
[SleClientDemo] LED blink -> OFF
[sle client sdk] seek skip rssi=-45 data:...OHOS_SLE...
[SleClientDemo] evt=SEEK_RESULT phase=SEEKING status=0x0 conn=0
[SleClientDemo] scan rssi=-45 adv:...
[SleClientDemo] 78:70:60:88:96:46
[sle client sdk] seek HIT rssi=-45 data:...OHOS_SLE...
[SleClientDemo] evt=SEEK_HIT phase=SEEKING status=0x0 conn=0
[SleClientDemo] *** scan hit "OHOS_SLE" ***
[SleClientDemo] evt=SEEK_STOPPED phase=CONNECTING status=0x0 conn=0
[SleClientDemo] seek stopped, connecting...
[SleClientDemo] evt=CONNECT_START phase=CONNECTING status=0x0 conn=0
[SleClientDemo] step2: SleConnectRemoteDevice
[SleClientDemo] evt=CONN_STATE phase=CONNECTED status=0x0 conn=1
[SleClientDemo] conn_state=CONNECTED disc_reason=0x0
[SleClientDemo] *** CONNECTED ***
[SleClientDemo] evt=MTU_EXCHANGE phase=SSAP_DISCOVERING status=0x0 conn=1
[SleClientDemo] step3: MTU exchange, start SSAP find_structure
[sle client sdk] mtu=520 version=1
[SleClientDemo] evt=SSAP_SERVICE phase=SSAP_DISCOVERING status=0x0 conn=1
[SleClientDemo] SSAP service hdl=0x0001
[SleClientDemo] evt=SSAP_PROPERTY phase=SSAP_DISCOVERING status=0x0 conn=1
[SleClientDemo] SSAP property hdl=0x0002
[SleClientDemo] evt=SSAP_READY phase=SSAP_READY status=0x0 conn=1
[SleClientDemo] *** SSAP_READY *** wait Server auto notify (GPIO10)
[SleClientDemo] LED blink mode OFF (Server notify ctrl)
[SleClientDemo] monitor phase=SSAP_READY conn_id=1 ssap_ready=1
```

Server 自动 Notify 后 Client 控灯（`sle_sdk_led_enable = false` 时 Notify 行带 `[log only, LED disabled]`）：

```
[SleClientDemo] evt=NOTIFICATION phase=SSAP_READY status=0x0 conn=1
[SleClientDemo] Server->Client notify cmd=0x01 (ON)
[SleClientDemo] evt=NOTIFICATION phase=SSAP_READY status=0x0 conn=1
[SleClientDemo] Server->Client notify cmd=0x00 (OFF)
```

断线后 Client 会打印 `DISCONNECTED` 并重新扫描，LED 本地闪烁恢复：

```
[SleClientDemo] evt=CONN_STATE phase=SEEKING status=0x0 conn=1
[SleClientDemo] conn_state=DISCONNECTED disc_reason=0x0
[SleClientDemo] *** DISCONNECTED *** restart seek
[SleClientDemo] LED blink mode ON (interval 500 ms)
[SleClientDemo] step1: seeking target "OHOS_SLE"
```

## 工程目录说明

| 文件 | 说明 |
| ------------------------- | --------------------------------------- |
| `sle_sdk_common.h` | 广播名、示例 MAC、GPIO、命令字节 |
| `sle_server_sdk.c` / `.h` | SSAP Server、连接、Notify |
| `sle_server_adv.c` / `.h` | 广播数据与 `SleStartAnnounce` |
| `sle_client_sdk.c` / `.h` | 扫描、连接、SSAP 发现、Write/Notify |
| `sle_server_demo.c` | Server 入口：广播 + 自动 Notify |
| `sle_client_demo.c` | Client 入口：收 Notify + LED |
| `hal_iot_gpio_ex.c` | GPIO 扩展封装 |
| `BUILD.gn` | `declare_args("sle_sdk_role")` |

## 常见问题

| 现象 | 处理 |
| -------------------------------------- | -------------------------------------------------------------------------------------------------- |
| Client 一直扫描 | Server 须先有 `set announce data success`；先 Server 后 Client |
| 连上但 LED 不切换 | 确认 Client `SSAP_READY`、Server 有 `auto SSAP notify`；检查 `sle_sdk_led_enable = true` |
| LED 不亮 | 确认 LED 接排针 **GPIO10**（低电平点亮）；参考 [06-GPIO的使用.md](../../../docs/06-GPIO的使用.md) |
| 双板 MAC 冲突 | 修改 `sle_sdk_common.h` 中示例地址，确保 Server/Client 不同，双板重编 |
