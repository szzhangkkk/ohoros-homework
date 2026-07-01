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

#define DOOR_SERVO_GPIO              10
#define DOOR_PIR_ADC_CHANNEL         5U
#define DOOR_BUTTON_GPIO             8
#define DOOR_LED_GPIO                11

/* ======================== 舵机 PWM 时序（50Hz） ======================== */

#define SERVO_PWM_PERIOD_US          20000       /* 50Hz = 20ms */
#define SERVO_PULSE_MIN_US           500         /* 0° 脉宽 */
#define SERVO_PULSE_MAX_US           2500        /* 180° 脉宽 */
#define SERVO_PULSE_RANGE_US         (SERVO_PULSE_MAX_US - SERVO_PULSE_MIN_US)

/*
 * 舵机转动到目标角度时发送的脉冲数。
 * 50 个周期 × 20ms = 1000ms = 1 秒。
 * SG90 从 0° 转到 90° 约需 0.1~0.2s（空载），
 * 此处 1s 确保带负载也能转到位。
 */
#define SERVO_TRAVEL_CYCLES          50

/* ======================== 门锁角度 ======================== */

#define DOOR_LOCK_ANGLE_DEG          0
#define DOOR_UNLOCK_ANGLE_DEG        90

/* ======================== PIR ======================== */

#define PIR_IDLE_CONFIRM_SAMPLES     5
#define ADC_MV_FULL_SCALE            3600U
#define ADC_HUMAN_MOTION_GE_MV       2200U   /* >= 此值判定为有人 */
#define ADC_HUMAN_IDLE_LE_MV         750U    /* <= 此值判定为空闲 */

#if ADC_HUMAN_IDLE_LE_MV >= ADC_HUMAN_MOTION_GE_MV
#error "ADC_HUMAN_IDLE_LE_MV must be less than ADC_HUMAN_MOTION_GE_MV"
#endif

/* ======================== 时间参数 ======================== */

#define TICK_PER_SEC                 100
#define MS_TO_TICKS(ms)              (((ms) * TICK_PER_SEC) / 1000)

#define MAIN_LOOP_INTERVAL_MS        50
#define PIR_SAMPLE_INTERVAL_MS       100
#define BUTTON_DEBOUNCE_SAMPLES      2

/* 开门后保持时间：开到位后等 2s 再关门 */
#define UNLOCK_HOLD_MS               2000

/* LED 闪烁间隔（毫秒） */
#define LED_FAST_BLINK_MS            200
#define LED_SLOW_BLINK_MS            500

/* 任务栈大小与优先级 */
#define DOOR_TASK_STACK_SIZE         0x1000
#define DOOR_TASK_PRIO               25

#define DOOR_START_DELAY_MS          1000

/* ======================== 门禁状态 ======================== */

typedef enum {
    DOOR_STATE_LOCKED = 0,
    DOOR_STATE_UNLOCKING,
    DOOR_STATE_UNLOCKED,
    DOOR_STATE_LOCKING,
} door_state_t;

/* ======================== SLE 无线通信参数 ======================== */

#define DOOR_SLE_SERVER_NAME         "OHOS_SLE_DOOR"
#define DOOR_SLE_LOCAL_ADDR          { 0x78, 0x70, 0x60, 0x88, 0x96, 0x46 }
#define DOOR_SLE_SERVICE_UUID        0x2222
#define DOOR_SLE_PROPERTY_UUID       0x2323
#define DOOR_SLE_PROPERTIES          (SSAP_PERMISSION_READ | SSAP_PERMISSION_WRITE)
#define DOOR_SLE_OPERATION_INDICATION (SSAP_OPERATE_INDICATION_BIT_READ | SSAP_OPERATE_INDICATION_BIT_WRITE)
#define DOOR_SLE_NAME_MAX_LEN        16
#define DOOR_SLE_ADV_HANDLE          1
#define DOOR_SLE_ADV_INTERVAL_MIN    0xC8
#define DOOR_SLE_ADV_INTERVAL_MAX    0xC8
#define DOOR_SLE_CONN_INTV_MIN       0x64
#define DOOR_SLE_CONN_INTV_MAX       0x64
#define DOOR_SLE_CONN_SUPERVISION_TO 0x1F4
#define DOOR_SLE_CONN_MAX_LATENCY    0x1F3
#define DOOR_SLE_ADV_TX_POWER        10
#define DOOR_SLE_ADV_DATA_LEN_MAX    251

#define DOOR_CMD_LOCK                0x00
#define DOOR_CMD_UNLOCK              0x01
#define DOOR_REMOTE_CMD_NONE         0xFF

/* ======================== 日志 ======================== */

#define DOOR_LOG                     "[DoorAccess]"

#define DOOR_ADDR_PRINT_FMT          "%02X:%02X:%02X:%02X:%02X:%02X"
#define DOOR_ADDR_PRINT_ARG(a)       (unsigned)(a)[0], (unsigned)(a)[1], (unsigned)(a)[2], \
                                     (unsigned)(a)[3], (unsigned)(a)[4], (unsigned)(a)[5]

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif

#endif /* DOOR_ACCESS_H */
