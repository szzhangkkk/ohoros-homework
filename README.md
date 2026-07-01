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
