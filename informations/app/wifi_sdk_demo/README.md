# wifi_sdk_demo

**适用平台**：九联星闪开发板，海思 WS63E 芯片。

课程实验见：[实验指导书（2 人双板）](../../../docs/16-九联星闪开发板WiFi实验指导书（学生版）.md) · [备课说明](../../../docs/16-九联星闪开发板WiFi实验指导书.md)

## 如何创建本工程

`make_app.py -c` 只生成 HelloWorld 模板，完整 Wi-Fi 工程需两步：**先创建目录，再用示例源码替换该目录**。

### 1. 用 make_app 创建并注册工程

在 OpenHarmony 源码**根目录**执行（目录名与 GN 目标名建议一致）：

```bash
python3 applications/make_app.py -c wifi_sdk_demo wifi_sdk_demo
```

脚本会：

- 在 `applications/sample/wifi-iot/app/wifi_sdk_demo/` 下生成模板 `wifi_sdk_demo.c` 与简易 `BUILD.gn`
- 自动修改 `app/BUILD.gn`、`config.py`、`ohos.cmake`，使工程参与编译

若目录**已存在**于本仓库（例如已 clone 含 `wifi_sdk_demo` 的 ws63_ohos），不必再 `-c`，改用启用即可：

```bash
python3 applications/make_app.py -e wifi_sdk_demo wifi_sdk_demo
```

查看是否已启用：`python3 applications/make_app.py -l`

### 2. 用示例源码替换

把完整的 **`wifi_sdk_demo` 源码文件夹**（本 README 所在目录，或课程/仓库提供的同名目录）里的文件，**全部复制并覆盖**到第 1 步生成的路径：

`applications/sample/wifi-iot/app/wifi_sdk_demo/`

覆盖后删除脚本生成的 `wifi_sdk_demo.c`（模板入口，与 demo 冲突）。

本仓库已自带完整工程时，可跳过第 1、2 步，直接执行 `-e` 启用后编译。

### 3. 配置板子角色并编译

编辑 `BUILD.gn` 顶部 `declare_args`（无需再手动注释 `sources`）：

| 烧录板 | `wifi_sdk_role` | `wifi_sdk_enable_tcp`（可选） |
|--------|-----------------|-------------------------------|
| **AP 板** | `"ap"` | `true` 时编入 `wifi_tcp_server.c` |
| **STA 板** | `"sta"` | `true` 时编入 `wifi_tcp_client.c` |

```bash
python3 applications/make_app.py -b
```

AP 与 STA **须分别改 `declare_args`、分别编译、分别烧录**，不能两块板烧同一份固件。

---

## 双板分工

| 板子 | 入口文件 | BUILD.gn |
|------|----------|----------|
| **AP 板** | `wifi_softap_demo.c` | `wifi_sdk_role = "ap"` |
| **STA 板** | `wifi_sta_demo.c` | `wifi_sdk_role = "sta"` |

| 板子 | Wi-Fi | TCP |
|------|-------|-----|
| **AP 板** | SoftAP `OHOS_AP` | **Server** 监听 `0.0.0.0:8888`（`wifi_tcp_server.c`） |
| **STA 板** | 连接 `OHOS_AP` | **Client** 连接 `192.168.43.1:8888`（`wifi_tcp_client.c`） |

公共逻辑在 `wifi_*_sdk.c`、`wifi_tcp_*.c`；`wifi_sdk_role` 决定只链接一个 `*_demo.c`（避免两个 `APP_FEATURE_INIT` 冲突）。

**事件监听**：`wifi_register_event_cb` 与连接统计写在各 `*_demo.c` 中。

## TCP 联调步骤

1. AP 与 STA 的 `BUILD.gn` 中设 `wifi_sdk_enable_tcp = true`，分别编译烧录  
2. 先烧录 **AP 板**，串口见 `[WIFI_AP] started`、`[WifiTcpServer] listening on 0.0.0.0:8888`  
3. 再烧录 **STA 板**，串口见 `[WIFI_STA] IP 192.168.43.x`、`[WifiTcpClient] connected`  
4. 双方应打印 `STA-MSG-n` / `AP-ACK-n` 收发日志（默认各 5 轮）

端口与 AP IP 在 `wifi_tcp_common.h` 中修改：`WIFI_TCP_DEMO_PORT`、`WIFI_TCP_AP_IP`。

## 按功能编译（BUILD.gn）

在 `wifi_sdk_demo/BUILD.gn` 顶部 `declare_args` 中配置：

| 变量 | 取值 | 效果 |
|------|------|------|
| `wifi_sdk_role` | `"ap"` | 编 `wifi_softap_demo.c` |
| `wifi_sdk_role` | `"sta"` | 编 `wifi_sta_demo.c` |
| `wifi_sdk_enable_tcp` | `true` | 编入 TCP 模块，并定义宏 `WIFI_SDK_ENABLE_TCP`（demo 内启动 TCP） |
| `wifi_sdk_enable_tcp` | `false` | 不编 TCP 源文件，demo 中 `#if defined(WIFI_SDK_ENABLE_TCP)` 为假 |

`wifi_sta_sdk.c` / `wifi_softap_sdk.c` / `wifi_sdk_common.c` 两种板子都会链接（切换角色只需改变量）。

## 编译烧录

```bash
python3 applications/make_app.py -b
```

修改 `declare_args` 后若行为异常，可全量重编：`python3 applications/make_app.py -f`

默认热点：`OHOS_AP` / `123456789`
