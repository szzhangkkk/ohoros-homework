# ohoros-homework

基于**九联星闪开发板**（海思 WS63E 芯片）的 OpenHarmony（开源鸿蒙）综合实验作业，涵盖外设驱动、无线通信、系统服务等示例项目。

## 硬件平台

- **开发板:** 九联星闪 NearLink DK-3863
- **芯片:** 海思 HiSilicon WS63E
- **系统:** OpenHarmony (mini 系统)

## 项目结构

```
ohoros-homework/
├── informations/                  # 实验资料与示例代码
│   ├── app/                       # 应用示例项目
│   │   ├── helloworld/            # Hello World 入门
│   │   ├── gpio_led/              # GPIO 控制 LED
│   │   ├── adc_light/             # ADC 光敏传感器
│   │   ├── adc_human/             # ADC 人体感应
│   │   ├── pwm_servo/             # PWM 舵机控制
│   │   ├── thread/                # 多线程示例
│   │   ├── uart_printf/           # UART 打印输出
│   │   ├── uart_uapi_irq/         # UART 中断模式
│   │   ├── uart_uapi_poll/        # UART 轮询模式
│   │   ├── uart_tcp_re/           # UART ↔ TCP 透传
│   │   ├── ble_sdk_demo/          # BLE 蓝牙通信
│   │   ├── sle_sdk_demo/          # SLE 星闪通信
│   │   ├── wifi_sdk_demo/         # Wi-Fi 通信 (STA/AP/TCP)
│   │   ├── easy_wifi/             # Wi-Fi 连接封装库
│   │   ├── door_access/           # 门禁系统综合案例 (SLE + UART)
│   │   ├── oled/                  # OLED 显示屏驱动
│   │   ├── iothardware/           # IoT 硬件 LED 示例
│   │   ├── demolink/              # DemoSDK 适配层示例
│   │   ├── samgr/                 # 系统服务管理 (Samgr) 示例
│   │   └── startup/               # 启动配置
│   ├── make_app.py                # 应用项目管理脚本
│   └── *.pdf / *.pptx             # 实验指导书与课件
└── ws63_ohos/                     # 工作目录 (预留)
```

## 实验列表

| 编号 | 实验名称 | 示例项目 | 说明 |
|------|---------|---------|------|
| 01 | 环境搭建与 Hello World | `helloworld` | 开发环境配置、系统编译烧录 |
| 02 | GPIO 控制 | `gpio_led` | GPIO 引脚控制 LED 亮灭 |
| 03 | ADC 采样 | `adc_light`, `adc_human` | 模拟数字转换，光敏/人体感应 |
| 04 | PWM 输出 | `pwm_servo` | PWM 信号控制舵机角度 |
| 05 | UART 串口通信 | `uart_printf`, `uart_uapi_irq`, `uart_uapi_poll` | 串口轮询/中断/UAPI 驱动 |
| 06 | Thread 多线程 | `thread` | 线程创建与调度 |
| 07 | Wi-Fi 通信 | `wifi_sdk_demo`, `easy_wifi` | WiFi STA/AP/TCP 通信 |
| 08 | BLE 蓝牙通信 | `ble_sdk_demo` | BLE 客户端/服务端通信 |
| 09 | SLE 星闪通信 | `sle_sdk_demo` | 星闪 SLE 客户端/服务端 |
| 10 | 综合案例 | `door_access`, `uart_tcp_re` | 门禁系统、UART-TCP 透传 |
| — | 系统服务 | `samgr` | 系统能力管理、IPC 通信 |
| — | OLED 显示 | `oled` | SSD1306 OLED 驱动与显示 |

## 门禁系统实现逻辑 (`door_access`)

### 硬件连接

| 引脚 | 外设 | 说明 |
|------|------|------|
| GPIO10 | SG90 舵机 | 信号线，软件模拟 50Hz PWM |
| GPIO12 → ADC5 | SR602 人体红外传感器 | 模拟电压输出，有人时电压升高 |
| GPIO8 | 开门按键 | 低电平有效，内部上拉 |
| GPIO11 | 状态 LED | 低电平点亮 |
| SLE | 星闪无线 | 手机 ↔ 门禁板远程管控 |

### PIR 人体检测状态机

SR602 是数字型 PIR 模块，但通过 ADC5 读取模拟电压来判断人体靠近程度。
系统使用**双阈值 + 迟滞**的状态机，避免临界抖动造成误触发：

```
                    mv ≥ MOTION (600mV)
     ┌──────────┐  ──────────────────→  ┌──────────┐
     │  IDLE    │                        │  MOTION  │
     │ (空闲)   │  ←──────────────────  │ (有人)   │
     └──────────┘    连续5次 ≤ IDLE      └──────────┘
                             (550mV)
```

**阈值定义 (`door_access.h`):**

| 宏 | 值 | 含义 |
|---|---|---|
| `ADC_MIN_VALID_MV` | 100mV | 低于此值视为 ADC 采样异常，直接丢弃 |
| `ADC_HUMAN_IDLE_LE_MV` | 550mV | ≤ 此值累计空闲计数 |
| `ADC_HUMAN_MOTION_GE_MV` | 600mV | ≥ 此值立即判定有人 |

**判断区间:**

```
< 100mV    → 丢弃（周期性 ADC 毛刺过滤）
100-550mV  → 空闲区间，累计 idle_cnt
550-600mV  → 死区（迟滞带），维持上一状态不变
≥ 600mV    → 立即判定有人，idle_cnt 清零
```

**空闲确认机制:**

当电压连续 ≤ 550mV 达到 `PIR_IDLE_CONFIRM_SAMPLES`（5 次）时，才从 MOTION 切换为 IDLE。这防止了传感器在阈值附近抖动导致的反复翻转。

**采样周期:**

- PIR 采样间隔: 100ms（`PIR_SAMPLE_INTERVAL_MS`）
- 每秒打印一次当前 mV 和阈值范围，方便调试

**ADC 异常过滤:**

实际运行中观察到周期性的 4mV 尖峰（约每 5 次采样出现一次），可能是 ADC 采样与传感器内部刷新周期碰撞导致。`ADC_MIN_VALID_MV = 100mV` 将这些异常读数过滤掉，避免干扰空闲计数。

### 本地开门流程

```
PIR 检测到人 (mv ≥ 600mV)
       │
       ▼
  舵机转 90° (开锁)
  发送 50 个 50Hz PWM 脉冲 (~1s)
       │
       ▼
  保持开锁状态 2s (UNLOCK_HOLD_MS)
       │
       ▼
  舵机转 0° (闭锁)
  发送 50 个 50Hz PWM 脉冲 (~1s)
```

整个开-等-关流程是**阻塞式**的，期间不响应新的 PIR 触发（防止重复开门）。

### 舵机控制

采用 `servo.c` 的设计模式——直接向目标角度连续发送 50 个 PWM 脉冲：

- **周期:** 20ms（50Hz）
- **脉宽范围:** 500μs (0°) ~ 2500μs (180°)
- **角度换算:** `pulse_us = 500 + (angle × 2000) / 180`
- **行程脉冲数:** `SERVO_TRAVEL_CYCLES = 50`，约 1 秒完成转动

不使用逐度步进，舵机内部自行平滑转动，不会抽搐。

### 按键消抖

按键采样使用**连续一致计数**的去抖策略：

```
读取 GPIO → 与上次相同？
  ├─ 相同 → debounce_cnt++
  │         └─ cnt ≥ 2 (BUTTON_DEBOUNCE_SAMPLES) → 确认稳定值
  └─ 不同 → debounce_cnt = 0，重新计数
```

注意：当前版本（PIR-only 模式）按键逻辑保留但未接入开门判断，开门仅由 PIR 和 SLE 触发。

### LED 状态指示

| 门禁状态 | SLE 已连接 | SLE 未连接 |
|----------|-----------|------------|
| LOCKED（已锁） | 常灭 | 慢闪 (500ms) |
| UNLOCKING/LOCKING（转动中） | 快闪 (200ms) | 快闪 (200ms) |
| UNLOCKED（已开） | 常亮 | 慢闪 (500ms) |

### SLE 星闪远程控制

手机通过 SLE 星闪连接发送单字节指令：

| 指令 | 值 | 行为 |
|------|-----|------|
| `DOOR_CMD_UNLOCK` | `0x01` | 执行完整开-等-关流程 |
| `DOOR_CMD_LOCK` | `0x00` | 仅闭锁（不自动再开） |

SLE 消息通过 `sle_uart_server` 透传，回调 `OnSleMessageReceived` 写入 `g_remote_cmd`，主循环轮询执行。

### 主循环逻辑

```
for (;;) {
    1. DoorPirSample(50ms) → 更新 PIR 状态
    2. 检查 SLE 远程命令
       ├─ UNLOCK → 执行开门流程
       └─ LOCK   → 仅闭锁
    3. PIR 检测到人？→ 执行开门流程
    4. DoorLedUpdate() → 更新 LED 闪烁
    5. 喂狗 + osDelay(50ms)
}
```

## 环境准备

### 1. 获取 OpenHarmony 源码

请参考 OpenHarmony 官方文档获取源码，推荐使用 repo 工具：

```bash
repo init -u https://gitee.com/openharmony/manifest.git -b master --no-repo-verify
repo sync -c
repo forall -c 'git lfs pull'
```

### 2. 安装编译工具

```bash
# 安装 hb 工具
pip3 install --user build/hb
```

### 3. 配置产品

```bash
hb set -p nearlink_dk_3863
```

## 应用管理

使用 `make_app.py` 脚本管理应用项目（需在 OpenHarmony 源码根目录执行）：

```bash
# 查看当前应用列表
python3 applications/make_app.py -l

# 创建新项目
python3 applications/make_app.py -c my_project

# 启用项目（参与编译）
python3 applications/make_app.py -e gpio_led

# 禁用项目（不编译）
python3 applications/make_app.py -x gpio_led

# 删除项目
python3 applications/make_app.py --delete my_project

# 编译
python3 applications/make_app.py -b

# 全量编译
python3 applications/make_app.py -f
```

## 编译与烧录

```bash
# 编译
hb build -f

# 烧录 (通过串口)
# 请参考文档: informations/环境配置和系统编译烧录文档.pdf
```

编译产物位于 `out/nearlink_dk_3863/` 目录。

## 相关资料

`informations/` 目录下包含完整的实验指导书和课件：

- **实验指导书:** 综合实验指导书、ADC/GPIO/UART/PWM 各实验指导手册
- **课件:** ADC 案例、PWM 舵机、实验1-2 课件 (PPTX)
- **接口文档:** UART 驱动接口、WiFi 接口、SLE 接口说明
- **编码规范:** 开源鸿蒙编码规范
- **环境配置:** 系统编译烧录文档

## 许可证

本项目代码基于 Apache License 2.0 许可。

---

*深圳技术大学 (SZTU) 开源组织 - 2026*
