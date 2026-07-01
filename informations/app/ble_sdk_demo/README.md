# ble_sdk_demo

**适用平台**：九联星闪开发板，海思 WS63E 芯片。

**作者**：SZTU鸿蒙课程组 王老师

相关文档：[实验指导书（2 人双板）](../../../docs/18-九联星闪开发板BLE实验指导书（学生版）.md) · [接口说明](../../../docs/18-九联星闪开发板BLE接口与功能说明.md)

双板 **BLE GATT 串口透传**，代码分层与 `wifi_sdk_demo` 一致：**sdk = 协议栈封装**，**demo = 应用入口与实验逻辑**。

## 如何创建本工程

`make_app.py -c` 只生成 HelloWorld 模板，完整 BLE 工程需两步：**先创建目录，再用示例源码替换该目录**。

### 1. 用 make_app 创建并注册工程

在 OpenHarmony 源码**根目录**执行（目录名与 GN 目标名建议一致）：

```bash
python3 applications/make_app.py -c ble_sdk_demo ble_sdk_demo
```

脚本会：

- 在 `applications/sample/wifi-iot/app/ble_sdk_demo/` 下生成模板 `ble_sdk_demo.c` 与简易 `BUILD.gn`
- 自动修改 `app/BUILD.gn`、`config.py`、`ohos.cmake`，使工程参与编译

若目录**已存在**于本仓库（例如已 clone 含 `ble_sdk_demo` 的 ws63_ohos），不必再 `-c`，改用启用即可：

```bash
python3 applications/make_app.py -e ble_sdk_demo ble_sdk_demo
```

查看是否已启用：`python3 applications/make_app.py -l`

### 2. 用示例源码替换

把完整的 **ble_sdk_demo** 源码文件夹（本 README 所在目录）里的文件，**全部复制并覆盖**到第 1 步生成的路径：

`applications/sample/wifi-iot/app/ble_sdk_demo/`

覆盖后删除脚本生成的 `ble_sdk_demo.c`（模板入口，与 `ble_server_demo.c` / `ble_client_demo.c` 冲突）。

本仓库已自带完整工程时，可跳过第 1、2 步，直接执行 `-e` 启用后编译。

### 3. 配置板子角色并编译

编辑 `BUILD.gn` 顶部 `declare_args`（无需再手动注释 `sources`）：

| 烧录板 | `ble_sdk_role` |
| ------------ | -------------- |
| **Server 板** | `"server"` |
| **Client 板** | `"client"`（默认） |

```bash
python3 applications/make_app.py -b
```

Server 与 Client **须分别改 `declare_args`、分别编译、分别烧录**，不能两块板烧同一份固件。

修改 `declare_args` 后若行为异常，可全量重编：`python3 applications/make_app.py -f`

## 双板分工

| 板子 | 入口文件 | BUILD.gn |
| ------------ | ------------------- | ------------------------- |
| **Server 板** | `ble_server_demo.c` | `ble_sdk_role = "server"` |
| **Client 板** | `ble_client_demo.c` | `ble_sdk_role = "client"` |

| 板子 | BLE 角色 | 串口透传方向 |
| ------------ | ------------------ | -------------------------------------------------------- |
| **Server 板** | GATT Server + 广播 | UART RX → **Notify** → Client；Client **Write** → UART TX |
| **Client 板** | GATT Client + 扫描连接 | UART RX → **Write** → Server；Server **Notify** → UART TX |

公共逻辑在 `ble_server_sdk.c`、`ble_server_adv.c`、`ble_client_sdk.c`；`ble_sdk_role` 决定只链接一个 `*_demo.c`（避免两个 `APP_FEATURE_INIT` 冲突）。

**连接监听**：Server 在 `ble_server_demo.c` 注册 `ble_server_register_link_listener`，并周期打印链路状态；Client 在 `ble_client_demo.c` 分步调用 `ble_client_stack_init` / `ble_client_start_gap_scan`，用 `ble_client_get_phase()` 观察阶段。

## 默认参数（`ble_sdk_common.h`）

| 参数 | 默认值 | 说明 |
| ---------------- | -------------------- | ------------------------------------------------------------- |
| 广播名 / Server 本地名 | `OHOS_BLE` | `BLE_SDK_SERVER_NAME` |
| Client 本地名 | `ble_sdk_client` | 不参与扫描匹配 |
| Server MAC | `11:22:33:44:55:66` | `BLE_SDK_SERVER_ADDR_INIT`，用于 `gap_ble_set_local_addr` |
| Client 扫描匹配 | 扫描上报常为 `66:55:…`（逆序） | `BLE_SDK_SERVER_ADDR_SCAN_MATCH_INIT`；SDK 亦按广播名 `OHOS_BLE` 匹配 |
| Client 本机 MAC | `12:22:33:44:55:66` | 避免与 Server 冲突 |
| 串口 | UART0，115200 | `BLE_SDK_UART_BUS = 0` |

改 Server MAC 时只改 `BLE_SDK_SERVER_ADDR_INIT`，**双板重新编译**；串口打印使用 `ble_sdk_print_addr()`，与协议栈数组下标一致。

## 双板联调步骤

1. **Server 板**：`ble_sdk_role = "server"`，编译烧录
2. 串口（115200）确认：`local GAP addr: 11:22:33:44:55:66`、`start adv`、GATT 服务启动日志
3. **Client 板**：`ble_sdk_role = "client"`，编译烧录（建议 **后上电**）
4. Client 串口应出现：
   - `step0: ble_client_stack_init`
   - `step1: ble_client_start_gap_scan`
   - `[ble client sdk] scan hit (mac|name OHOS_BLE) addr …`
   - `phase: GATT_READY`
5. Server 串口应出现：`*** Client CONNECTED ***`
6. 任一侧串口助手发字符，对端串口应收到（已连接且 GATT 就绪后）

**上电顺序**：先 Server 广播稳定，再 Client 扫描；Client 断线后 SDK 会自动重新扫描。

**可选**：手机 nRF Connect 扫描 **OHOS_BLE** 验证 Server 广播是否正常。

## 工程目录说明

| 文件 | 说明 |
| ------------------------- | --------------------------------------- |
| `ble_sdk_common.h` | 名称、MAC、UART/任务参数、`ble_sdk_print_addr()` |
| `ble_server_sdk.c` / `.h` | GATT Server、连接回调、Notify |
| `ble_server_adv.c` / `.h` | 广播与扫描响应 AD 组包 |
| `ble_client_sdk.c` / `.h` | 扫描、连接、GATT 发现、Write |
| `ble_server_demo.c` | Server 入口：初始化、连接监听、UART→Notify |
| `ble_client_demo.c` | Client 入口：分步扫描连接、UART↔GATT |
| `BUILD.gn` | `declare_args("ble_sdk_role")` 切换板型 |

## 按功能编译（BUILD.gn）

在 `ble_sdk_demo/BUILD.gn` 顶部 `declare_args` 中配置：

| 变量 | 取值 | 效果 |
| -------------- | ---------- | ---------------------- |
| `ble_sdk_role` | `"server"` | 编入 `ble_server_demo.c` |
| `ble_sdk_role` | `"client"` | 编入 `ble_client_demo.c` |

`ble_server_sdk.c`、`ble_server_adv.c`、`ble_client_sdk.c` 两种固件都会链接；仅 demo 含 `APP_FEATURE_INIT`。

## 常见问题

| 现象 | 处理 |
| -------------------------------------- | -------------------------------------------------------------------------------------------------- |
| Client 一直 `SCANNING` / `wait SCANNING` | **Server 必须先**出现 `start adv`；先 Server 后 Client 上电 |
| 有扫描但无 `scan hit` | 确认 Server 日志含 `start adv`；MAC 改 `BLE_SDK_SERVER_ADDR_INIT` 后双板重编；`scan hit` 里 addr 为 `66:55:…` 属正常 |
| Client 连上但串口无数据 | 等待 `GATT_READY` 后再发；确认写 handle 已发现 |
| 固件体积过大 | `python3 applications/make_app.py -x` 禁用其它示例应用 |
