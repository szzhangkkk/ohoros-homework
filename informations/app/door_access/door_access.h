/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * door_access.h — SLE 星闪无线简易门禁系统 公共定义
 *
 * 功能1：本地按键开门 — PIR 人体感应（确认有人） + 按键（主动开门动作）
 *         两个条件同时满足（AND），防止误触发。
 * 功能2：SLE 星闪无线管控 — 手机通过 SLE 远程发送开锁/闭锁指令
 *
 * 硬件连接：
 *   GPIO10    舵机信号线（软件模拟 50Hz PWM）
 *   GPIO12    SR602 人体红外传感器输出 → ADC5
 *   GPIO8     开门按键（低电平有效）
 *   GPIO11    状态 LED（低电平点亮）
 *   SLE       星闪无线通信（手机 ↔ 门禁板）
 */

#ifndef DOOR_ACCESS_H
#define DOOR_ACCESS_H

#include <stdint.h>
#include "ohos_init.h"
#include "cmsis_os2.h"
#include "iot_gpio.h"
#include "tcxo.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif

/* ======================== 引脚定义 ======================== */

/* 舵机信号线（黄线）接排针 GPIO10 */
#define DOOR_SERVO_GPIO              10

/* SR602 微型人体红外传感器：GPIO12 → ADC5 */
#define DOOR_PIR_ADC_CHANNEL         5U

/* 开门按键 — GPIO8，低电平有效 */
#define DOOR_BUTTON_GPIO             8

/* 状态 LED — GPIO11，低电平点亮 */
#define DOOR_LED_GPIO                11

/* ======================== 舵机 PWM 时序（50Hz） ======================== */

/* 50Hz 周期 = 20ms = 20000us */
#define SERVO_PWM_PERIOD_US          20000

/* 舵机标准脉宽端点（微秒） */
#define SERVO_PULSE_MIN_US           500     /* 0°   对应 0.5ms 高电平 */
#define SERVO_PULSE_MAX_US           2500    /* 180° 对应 2.5ms */
#define SERVO_PULSE_RANGE_US         (SERVO_PULSE_MAX_US - SERVO_PULSE_MIN_US)

/* 到位后每轮保持脉冲数（3 周期 ≈ 60ms），每轮都发，舵机始终有力矩 */
#define SERVO_HOLD_CYCLES            3
/* 舵机移动时每步脉冲数（10 周期 ≈ 200ms），参考 pwm_servo.c */
#define SERVO_MOVE_BURST_CYCLES      10
/* 舵机移动步进角度（每步 6°） */
#define SERVO_STEP_DEG               6

/* ======================== 门锁角度 ======================== */

#define DOOR_LOCK_ANGLE_DEG          0       /* 闭锁：舵机 0° */
#define DOOR_UNLOCK_ANGLE_DEG        90      /* 开锁：舵机 90°（可按锁具结构调整） */

/* ======================== PIR 防抖 ======================== */

/* PIR 需连续 N 次采样为「无人」才算真正无人，防止短暂掉信号误判 */
#define PIR_IDLE_CONFIRM_SAMPLES     5       /* 连续 5 次 ≈ 500ms 确认 */
/* 注：本地采用 AND 逻辑（PIR+按键），关门动作不会单独触发 PIR 开锁，
        因此不需要冷却期机制。*/

/* ======================== 时间参数 ======================== */

/* LiteOS 系统 tick 频率 100Hz，1 tick = 10ms */
#define TICK_PER_SEC                 100
#define MS_TO_TICKS(ms)              (((ms) * TICK_PER_SEC) / 1000)

/* 主循环间隔（毫秒） */
#define MAIN_LOOP_INTERVAL_MS        50

/* PIR 采样间隔（毫秒） */
#define PIR_SAMPLE_INTERVAL_MS       100

/* 按键消抖：需要连续相同读数的次数 */
#define BUTTON_DEBOUNCE_SAMPLES      3

/* 自动落锁超时（毫秒） */
#define AUTO_LOCK_TIMEOUT_MS         10000
/* 关门后冷却期（毫秒）：完成一次开关门后 10s 内不响应本地触发 */
#define LOCK_COOLDOWN_MS             10000

/* LED 闪烁间隔（毫秒） */
#define LED_FAST_BLINK_MS            200
#define LED_SLOW_BLINK_MS            500

/* 任务栈大小与优先级 */
#define DOOR_TASK_STACK_SIZE         0x1000
#define DOOR_TASK_PRIO               25

/* 启动延时（等待 SLE 协议栈就绪） */
#define DOOR_START_DELAY_MS          1000

/* ======================== PIR 阈值 ======================== */

/*
 * WS63 adc_port_read() 返回驱动换算后的 mV，满量程 3600mV。
 * SR602 人体红外传感器：人体越近、活动越强，ADC 读数越高。
 *
 * 阈值设计思路（门禁近距离场景）：
 *   - 有人阈值设得较高（2800mV），只有贴到门前方圆 ~0.5m 内才触发。
 *     远距离路过的人不会误触发。
 *   - 无人阈值设得较低（750mV），确保人走远后能快速归零。
 *   - 中间值（750~2800mV）保持上一次判定，滞回防抖。
 *
 * 如需调整灵敏度：拉高 MOTION_GE 值 = 要更近才能触发。
 */
#define ADC_MV_FULL_SCALE            3600U
#define ADC_HUMAN_MOTION_GE_MV       2800U   /* >= 此值判定为有人（强阈值，仅近距离） */
#define ADC_HUMAN_IDLE_LE_MV         750U    /* <= 此值判定为空闲 */

#if ADC_HUMAN_IDLE_LE_MV >= ADC_HUMAN_MOTION_GE_MV
#error "ADC_HUMAN_IDLE_LE_MV must be less than ADC_HUMAN_MOTION_GE_MV"
#endif

/* ======================== 门禁状态枚举 ======================== */

typedef enum {
    DOOR_STATE_LOCKED = 0,
    DOOR_STATE_UNLOCKING,
    DOOR_STATE_UNLOCKED,
    DOOR_STATE_LOCKING,
} door_state_t;

/* ======================== SLE 无线通信参数 ======================== */

/* 星闪广播名称（手机扫描此名连接门禁） */
#define DOOR_SLE_SERVER_NAME         "OHOS_SLE_DOOR"

/* 本地 MAC 地址（6 字节） */
#define DOOR_SLE_LOCAL_ADDR          { 0x78, 0x70, 0x60, 0x88, 0x96, 0x46 }

/* SSAP 服务 UUID 和属性 UUID */
#define DOOR_SLE_SERVICE_UUID        0x2222
#define DOOR_SLE_PROPERTY_UUID       0x2323

/* 属性权限：可读可写 */
#define DOOR_SLE_PROPERTIES          (SSAP_PERMISSION_READ | SSAP_PERMISSION_WRITE)
#define DOOR_SLE_OPERATION_INDICATION (SSAP_OPERATE_INDICATION_BIT_READ | SSAP_OPERATE_INDICATION_BIT_WRITE)

/* 广播名称最大长度 */
#define DOOR_SLE_NAME_MAX_LEN        16

/* 广播句柄 */
#define DOOR_SLE_ADV_HANDLE          1

/* 广播/连接时间参数（单位 125us） */
#define DOOR_SLE_ADV_INTERVAL_MIN    0xC8    /* 25ms */
#define DOOR_SLE_ADV_INTERVAL_MAX    0xC8    /* 25ms */
#define DOOR_SLE_CONN_INTV_MIN       0x64    /* 12.5ms */
#define DOOR_SLE_CONN_INTV_MAX       0x64    /* 12.5ms */
#define DOOR_SLE_CONN_SUPERVISION_TO 0x1F4   /* 5000ms，单位 10ms */
#define DOOR_SLE_CONN_MAX_LATENCY    0x1F3   /* 4990ms，单位 10ms */

/* 广播发送功率 */
#define DOOR_SLE_ADV_TX_POWER        10

/* 最大广播数据长度 */
#define DOOR_SLE_ADV_DATA_LEN_MAX    251

/* 手机 ↔ 门禁板 命令定义 */
#define DOOR_CMD_LOCK                0x00    /* 闭锁 */
#define DOOR_CMD_UNLOCK              0x01    /* 开锁 */

/* 远程命令（SLE 回调写入，主循环消费） */
#define DOOR_REMOTE_CMD_NONE         0xFF    /* 无待处理命令 */

/* ======================== 日志标签 ======================== */

#define DOOR_LOG                     "[DoorAccess]"

/* ======================== 地址打印宏 ======================== */

#define DOOR_ADDR_PRINT_FMT          "%02X:%02X:%02X:%02X:%02X:%02X"
#define DOOR_ADDR_PRINT_ARG(a)       (unsigned)(a)[0], (unsigned)(a)[1], (unsigned)(a)[2], \
                                     (unsigned)(a)[3], (unsigned)(a)[4], (unsigned)(a)[5]

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif

#endif /* DOOR_ACCESS_H */
