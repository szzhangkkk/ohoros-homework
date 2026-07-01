# adc_light（ADC 采样示例）

**适用平台**：九联星闪开发板，基于海思 WS63E 芯片。

---

## 一、硬件


| 类型    | 说明                    |
| ----- | --------------------- |
| 开发板   | **九联星闪开发板**（海思 WS63E） |
| 传感器模块 | **HS-S20B 环境亮度传感器**   |


---

## 二、实验目的

- 在 **九联星闪开发板（海思 WS63E）** 上掌握 **ADC** 采样流程及 `adc_port_read`、`uapi_adc_init` / `uapi_adc_deinit` 等接口用法。
- 将 **HS-S20B 环境亮度传感器** 的模拟输出接入 MCU **ADC 引脚**，理解**光照强弱与采样电压**的对应关系。
- 根据电压划分 **bright / normal / dark** 三档，并将电压**线性映射**为 **brightness（0～100%）**，在串口输出日志。
- 学会按实际模组来**设定阈值**，并了解 WS63 HAL 中电压换算与**满量程 mV**的含义。

---

## 三、实验原理

### 3.1. 传感器与分压

**HS-S20B** 内部通常以光敏元件与固定电阻组成**分压电路**：光照变化 → 阻值变化 → **S（Signal）** 端对地**模拟电压**在约 **0～VCC** 范围内变化。

### 3.2. MCU 侧：ADC 与读数

WS63 的 **ADC** 对该模拟电压采样；本工程通过 `adc_port_read` 取得驱动换算后的 **电压（mV）**，**不是**原始 ADC 码。HAL 中 `hal_adc_fifo_data_2_voltage()` 使用 `**VOLTAGE_UPPER_LIMIT`**（默认 **3600**）将码值换算为电压，文件见：

`device/soc/hisilicon/ws63v100/sdk/drivers/drivers/hal/adc/v154/hal_adc_v154.c`

应用里 `**ADC_MV_FULL_SCALE`** 应与上述常量一致，用于 **brightness** 百分比计算。

### 3.3. 光照与电压

在 **九联板 + HS-S20B** 上实测：**光照越强，`adc_port_read` 读到的 mV 越高**（与主观亮度**同向**）。若硬件表现为「越亮电压越低」，需在程序中**反转**判据或映射。

### 3.4. 三档区域

须满足 `**ADC_LIGHT_DARK_LE_MV` < `ADC_LIGHT_BRIGHT_GE_MV`**，中间为 **normal**。


| 含义         | 条件                                                  |
| ---------- | --------------------------------------------------- |
| **bright** | `voltage >= ADC_LIGHT_BRIGHT_GE_MV`（默认 **1850 mV**） |
| **dark**   | `voltage <= ADC_LIGHT_DARK_LE_MV`（默认 **750 mV**）    |
| **normal** | `DARK_LE` **<** `voltage` **<** `BRIGHT_GE`         |


默认按室内 **normal 约 1000 mV** 对齐 **750～1850 mV** 为 normal 带；强光、全暗不同时请改宏标定。

### 3.5. 亮度百分比

与「亮则电压高」一致：`brightness = voltage * 100 / ADC_MV_FULL_SCALE`，`**ADC_MV_FULL_SCALE`** 默认 **3600**。读数若只落在中间一段 mV，可改用暗端～亮端跨度或 **MIN/MAX** 两点校准。

**原理小结**：光强 → 传感器模拟电压 → **ADC 采样为 mV** → 门限分档 + 线性映射为百分比。

---

## 四、实验内容

### 4.1. 软件功能

- 在独立线程中周期性读取 **ADC 通道**（默认 **5**，对应 **GPIO12 / ADC5**），间隔与循环次数由 `adc_light.c` 中宏配置。
- 串口打印：`voltage`（mV）、`brightness`（%）、`level`（`bright` / `normal` / `dark`）。
- 依赖 SDK：`adc.h`、`adc_porting.h`、`uapi_adc_`* 等；须先按**五、1.** 完成驱动替换。

### 4.2. 工程文件


| 路径            | 说明                                        |
| ------------- | ----------------------------------------- |
| `adc_light.c` | `SYS_RUN` 创建线程，循环采样并打印电压、brightness、档位    |
| `BUILD.gn`    | 目标 `**adc_light_demo`**，配置头文件与 porting 路径 |
| `adc_driver/` | 驱动替换包及 `**apply_adc_driver_patch.sh`**    |


---

## 五、实验步骤

### 5.1. 前置条件 — 替换 SDK 中的 ADC 驱动文件

本示例的 `BUILD.gn` 与源码假定 **WS63 SDK** 已按 HiHope 资料完成 **porting / driver / hal** 及 `**adc.h`** 的配套版本。请先阅读同目录下 `**adc_driver/替换文件步骤.txt`**，并在 **OpenHarmony 源码根目录**执行一键脚本（会覆盖 `device/soc/hisilicon/ws63v100/sdk/...` 下对应文件，执行前请自行备份或提交版本库）：

```bash
cd applications/sample/wifi-iot/app/adc_light/adc_driver
bash apply_adc_driver_patch.sh               # 确认后执行
```

未替换时可能出现编译找不到头文件或链接符号失败。

### 5.2. 新建本项目的常规方式（若从零开始）

在 **OpenHarmony 源码根目录**使用应用管理脚本创建同名工程并写入三处配置（产品默认 **nearlink_dk_3863**）：

```bash
python3 applications/make_app.py -c adc_light adc_light_demo
```

若目录 `**adc_light**` 已存在（如本仓库），只需保证 `**app/BUILD.gn**` 的 `features`、`**config.py**` 的 `ram_component`、`**ohos.cmake**` 的 `COMPONENT_LIST` 中已包含本示例，或使用：

```bash
python3 applications/make_app.py -e adc_light adc_light_demo
```

详细说明见 `**applications/docs/04-创建新项目的方法.md**`。

### 5.3. 编译

在 **OpenHarmony 源码根目录**：

```bash
python3 applications/make_app.py -b
```

或：

```bash
hb set -p nearlink_dk_3863
hb build -f
```

全量清理后编译可使用 `**python3 applications/make_app.py -f**`。环境与烧录见 `**applications/docs/03-环境配置和系统编译烧录文档.md**`。

### 5.4. 目录说明

同 **四、实验内容** 中工程文件表；开发时主要修改 `**adc_light.c`**（通道、门限、采样周期）与 `**ADC_SAMPLE_CHANNEL`**。

### 5.5. ADC 接口使用说明

本示例通过 WS63 SDK 提供的 `**adc.h**`（`uapi_adc_*`）与 `**adc_porting.h**`（`adc_port_read`）完成采样；须先完成**五、1.**，否则可能链接或运行异常。

#### 5.5.1 头文件


| 头文件             | 作用                                  |
| --------------- | ----------------------------------- |
| `adc.h`         | `uapi_adc_init`、`uapi_adc_deinit` 等 |
| `adc_porting.h` | `adc_port_read`：porting 层一次采样       |
| `common_def.h`  | `ERRCODE_SUCC` 等，判断读数是否成功           |
| `pinctrl.h`     | 部分产品需配置引脚；本示例可视板级初始化而定              |


`BUILD.gn` 已配置 ADC 相关路径，新建工程时请与 `**adc_light_demo`** 保持一致。

#### 5.5.2 调用顺序（与 `adc_light.c` 一致）

1. `uapi_adc_init(ADC_CLOCK_NONE)` — 初始化 ADC 子系统。
2. **循环：`adc_port_read(channel, &voltage)`** — `channel` 为逻辑通道（本工程由 `**ADC_SAMPLE_CHANNEL**` 指定，默认 **5**）；成功时 `voltage` 为 **mV**；返回值为 `**ERRCODE_SUCC`** 才可用读数。
3. `uapi_adc_deinit()` — 与 init 成对调用。

建议在**独立线程**中循环采样（本示例 `**osThreadNew`** 创建 `**AdcTask`**）。

#### 5.5.3 ADC接口说明


| 接口                                                         | 说明                                              |
| ---------------------------------------------------------- | ----------------------------------------------- |
| `errcode_t uapi_adc_init(adc_clock_t clock)`               | 初始化；本示例 `**ADC_CLOCK_NONE`**。                   |
| `errcode_t adc_port_read(uint8_t channel, uint16_t *data)` | 单次采样，成功则 `*data` 为 mV（实现见 `**adc_porting.c`**）。 |
| `errcode_t uapi_adc_deinit(void)`                          | 关闭                                              |


#### 5.5.4 使用注意

- **通道号**须与硬件一致（见**五、6.**），修改 `**ADC_SAMPLE_CHANNEL`**。  
- 必须判断 `**adc_port_read`** 是否为 `**ERRCODE_SUCC`**。  
- 读数为驱动换算后的 **mV**，满量程见 **三、实验原理**。  
- 更高采样率或 DMA 需扩展 SDK；本示例仅为周期单次读数。

### 5.6. 硬件连接与参数标定

#### 5.6.1 硬件连接（HS-S20B / 光敏传感器）

本示例搭配 **HS-S20B**；**九联星闪板**上光敏对应核心板 **GPIO12（ADC5）**。模块引脚：


| 引脚标识          | 功能描述                          |
| ------------- | ----------------------------- |
| **V（VCC）**    | 3V3 正极，支持 3.3V / 5V 供电        |
| **G（GND）**    | 接地                            |
| **S（Signal）** | **GPIO12（ADC5）** 模拟输出，电压随光照变化 |


默认 `**ADC_SAMPLE_CHANNEL`** 为 **5**；换引脚时以原理图为准修改 `**adc_light.c`**。

#### 5.6.2 阈值建议

1. 正常室内光下记录串口 **mV**（参考「亮」）。
2. 遮挡或暗处记录 **mV**（参考「暗」）。
3. 调整 `**ADC_LIGHT_BRIGHT_GE_MV`**、`**ADC_LIGHT_DARK_LE_MV`**，使 dark 仅覆盖明确暗环境，bright 覆盖强光，中间为 normal，并保持 `**DARK_LE` < `BRIGHT_GE**`。
4. 电压与万用表差异大时，检查**分压、参考电压与 ADC 通道**是否与原理图一致。

门限与亮度公式见 **三、实验原理**。

### 5.7. 使用与调试说明

- **ADC 通道**默认 **5**（**GPIO12 / ADC5**），其他硬件请改宏。  
- 串口格式：`voltage`、`brightness`、`level`（英文），实测样例见 **六、1.**。  
- 临时禁用本应用：`**python3 applications/make_app.py -x adc_light adc_light_demo`**，详见 `**applications/make_app.py -h`**。

---

## 六、运行结果

### 6.1. 串口输出示例

以下为实测串口打印（不同光照下，**level** 对应 **dark / normal / bright**）：

```
voltage:  340 mV, brightness: 9%, level: dark
voltage: 1077 mV, brightness: 29%, level: normal
voltage: 3139 mV, brightness: 87%, level: bright
```

实际读数随环境光与电路变化，以你板上的串口为准。

---

> 文档更新于 2026-03-12，为本人在开发中总结的经验，如有问题请及时联系王老师（[wangpeizheng@sztu.edu.cn](mailto:wangpeizheng@sztu.edu.cn)）。

