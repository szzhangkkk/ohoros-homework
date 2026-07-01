/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * door_access.c — SLE 星闪无线简易门禁系统 主应用
 *
 * ======================== 功能概述 ========================
 *
 * 功能1：本地红外开门
 *   SR602 人体红外传感器（GPIO12 → ADC5）检测人体，
 *   配合 GPIO8 按键按下 → 舵机开锁（GPIO10，90°）。
 *   两个条件同时满足才触发（AND 逻辑），防止误触发。
 *
 * 功能2：SLE 星闪无线管控
 *   门禁板作为 SLE SSAP Server 广播，手机作为 SLE Client
 *   扫描连接后，通过 SSAP Write 发送命令：
 *     0x00 = 闭锁
 *     0x01 = 开锁
 *   门禁板收到后执行，并通过 Notify 回复状态。
 *
 * ======================== 状态机 ========================
 *
 *   LOCKED ──(本地按键+PIR 或 手机开锁)──> UNLOCKING ──> UNLOCKED
 *   UNLOCKED ──(手机闭锁 或 自动落锁超时)──> LOCKED
 *
 *   自动落锁：开锁后若无操作，AUTO_LOCK_TIMEOUT_MS 后自动闭锁。
 *   PIR 持续检测到人时重置计时器。
 *
 * ======================== LED 指示 ========================
 *
 *   LOCKED + SLE已连接   → 灭
 *   LOCKED + SLE未连接   → 慢闪（500ms）
 *   UNLOCKING            → 快闪（200ms）
 *   UNLOCKED + SLE已连接 → 常亮
 *   UNLOCKED + SLE未连接 → 慢闪
 */

#include "door_access.h"

#include <stdio.h>
#include <string.h>
#include "securec.h"
#include "common_def.h"
#include "soc_osal.h"
#include "errcode.h"

/* 外设驱动 */
#include "iot_gpio.h"
#include "iot_watchdog.h"
#include "adc.h"
#include "adc_porting.h"

/* SLE 协议栈 */
#include "sle_uart_server.h"
#include "sle_errcode.h"

/* ======================== 全局状态 ======================== */

/* SLE 远程命令 */
static volatile uint8_t g_door_remote_cmd = DOOR_REMOTE_CMD_NONE;

/* 按键消抖 */
static uint8_t g_button_stable     = 1;
static uint8_t g_button_last_raw   = 1;
static uint8_t g_button_debounce_cnt = 0;

/* PIR 采样 */
static uint32_t g_pir_last_ms = 0;
static uint8_t  g_pir_motion  = 0;

/* 舵机 + 10 秒定时器 */
static unsigned int g_servo_angle = 0;
static unsigned int g_servo_target = 0;
static uint32_t    g_unlock_timer = 0;   /* 开锁倒计时（毫秒）*/

/* LED */
static uint32_t g_led_last_toggle_ms = 0;
static uint8_t  g_led_current_on     = 0;

/* ======================== 日志 ======================== */

#define DOOR_PRINTF(fmt, args...) printf(DOOR_LOG " " fmt "\r\n", ##args)

/* ======================== 舵机控制（拷贝自 pwm_servo.c） ======================== */

static void ServoSendPulseUs(uint32_t pulse_us)
{
    if (pulse_us > SERVO_PWM_PERIOD_US) pulse_us = SERVO_PWM_PERIOD_US;
    IoTGpioSetOutputVal(DOOR_SERVO_GPIO, IOT_GPIO_VALUE1);
    (void)uapi_tcxo_delay_us(pulse_us);
    IoTGpioSetOutputVal(DOOR_SERVO_GPIO, IOT_GPIO_VALUE0);
    (void)uapi_tcxo_delay_us(SERVO_PWM_PERIOD_US - pulse_us);
}

static uint32_t ServoAngleToPulseUs(unsigned int angle_deg)
{
    if (angle_deg > 180) angle_deg = 180;
    return SERVO_PULSE_MIN_US + (angle_deg * SERVO_PULSE_RANGE_US) / 180;
}

/*
 * ServoSetTarget — 设置舵机目标角度
 * 主循环中 ServoMoveStep() 会逐步逼近目标
 */
static void ServoSetTarget(unsigned int angle_deg)
{
    if (angle_deg > 180) angle_deg = 180;
    g_servo_target = angle_deg;
}

/*
 * ServoMoveStep — 向目标移动一步。到位后停发 PWM，舵机不动。
 */
static void ServoMoveStep(void)
{
    if (g_servo_angle == g_servo_target) return;  /* 到位，不发 PWM */

    if (g_servo_angle < g_servo_target) {
        g_servo_angle += 3;
        if (g_servo_angle > g_servo_target) g_servo_angle = g_servo_target;
    } else {
        g_servo_angle -= 3;
        if (g_servo_angle < g_servo_target) g_servo_angle = g_servo_target;
    }

    uint32_t pulse_us = ServoAngleToPulseUs(g_servo_angle);
    ServoSendPulseUs(pulse_us);
}

/* ======================== 按键消抖 ======================== */

static uint8_t DoorButtonRead(void)
{
    IotGpioValue val = IOT_GPIO_VALUE0;
    IoTGpioGetInputVal(DOOR_BUTTON_GPIO, &val);
    uint8_t raw = (uint8_t)val;

    if (raw == g_button_last_raw) {
        if (g_button_debounce_cnt < BUTTON_DEBOUNCE_SAMPLES) {
            g_button_debounce_cnt++;
            if (g_button_debounce_cnt >= BUTTON_DEBOUNCE_SAMPLES)
                g_button_stable = raw;
        }
    } else {
        g_button_debounce_cnt = 0;
        g_button_last_raw = raw;
    }
    return g_button_stable;
}

/* ======================== LED 控制 ======================== */

static void DoorLedInit(void)
{
    IoTGpioInit(DOOR_LED_GPIO);
    IoTGpioSetDir(DOOR_LED_GPIO, IOT_GPIO_DIR_OUT);
    IoTGpioSetOutputVal(DOOR_LED_GPIO, IOT_GPIO_VALUE1); /* 初始灭 */
}

static void DoorLedSet(uint8_t on)
{
    g_led_current_on = on ? 1 : 0;
    IoTGpioSetOutputVal(DOOR_LED_GPIO, on ? IOT_GPIO_VALUE0 : IOT_GPIO_VALUE1);
}

static void DoorLedUpdate(uint32_t elapsed_ms)
{
    g_led_last_toggle_ms += elapsed_ms;
    uint8_t sle_ok = sle_uart_client_is_connected() ? 1 : 0;

    if (g_unlock_timer > 0) {
        /* 开锁状态：SLE 连接时常亮，否则慢闪 */
        if (sle_ok) { DoorLedSet(1); }
        else if (g_led_last_toggle_ms >= LED_SLOW_BLINK_MS) {
            g_led_last_toggle_ms = 0; DoorLedSet(g_led_current_on ? 0 : 1);
        }
    } else {
        /* 闭锁状态：SLE 连接时灭，否则慢闪 */
        if (sle_ok) { DoorLedSet(0); }
        else if (g_led_last_toggle_ms >= LED_SLOW_BLINK_MS) {
            g_led_last_toggle_ms = 0; DoorLedSet(g_led_current_on ? 0 : 1);
        }
    }
}

/* ======================== SLE 回调 ======================== */

/*
 * OnSleMessageReceived — SLE 收到手机 Write 命令
 * 由 sle_server_sdk.c 的 write_request 回调触发，
 * 在 SLE 协议栈上下文执行，只做轻量赋值。
 */
static void OnSleMessageReceived(uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0) return;
    uint8_t cmd = data[0];
    DOOR_PRINTF("SLE recv cmd=0x%02X (%s)", (unsigned)cmd,
                cmd == DOOR_CMD_LOCK ? "LOCK" :
                cmd == DOOR_CMD_UNLOCK ? "UNLOCK" : "?");
    if (cmd == DOOR_CMD_LOCK || cmd == DOOR_CMD_UNLOCK) {
        g_door_remote_cmd = cmd;
    }
}

/*
 * DoorSleNotifyState — 向手机 Notify 当前门禁状态
 */
static void DoorSleNotifyState(void)
{
    if (!sle_uart_client_is_connected()) return;
    uint8_t s = (g_unlock_timer > 0) ? DOOR_CMD_UNLOCK : DOOR_CMD_LOCK;
    errcode_t ret = sle_uart_server_send_report_by_handle(&s, 1);
    if (ret != ERRCODE_SLE_SUCCESS)
        DOOR_PRINTF("Notify fail: 0x%x", (unsigned)ret);
    else
        DOOR_PRINTF("Notify %s -> phone", s == DOOR_CMD_LOCK ? "LOCKED" : "UNLOCKED");
}

/* ======================== PIR 检测 ======================== */

static void DoorPirSample(uint32_t elapsed_ms)
{
    g_pir_last_ms += elapsed_ms;
    if (g_pir_last_ms < 100) return;  /* 每 100ms 采样一次 */
    g_pir_last_ms = 0;

    uint16_t mv = 0;
    if (adc_port_read(DOOR_PIR_ADC_CHANNEL, &mv) != ERRCODE_SUCC) return;

    if (mv >= 1850)      g_pir_motion = 1;   /* 有人 */
    else if (mv <= 750)  g_pir_motion = 0;   /* 无人 */
    /* 中间值保持上一次判定 */
}

/* ======================== 门禁控制 ======================== */

/*
 * DoorControl — 每主循环调用一次
 * PIR 或按键触发 → 开锁 10 秒 → 自动闭锁
 * SLE 命令强制覆盖
 */
static void DoorControl(uint32_t elapsed_ms, uint8_t btn_pressed)
{
    uint8_t cmd = g_door_remote_cmd;

    /* SLE 远程命令优先 */
    if (cmd == DOOR_CMD_UNLOCK) {
        ServoSetTarget(DOOR_UNLOCK_ANGLE_DEG);
        g_unlock_timer = 0;  /* 远程开锁：不自动关 */
        g_door_remote_cmd = DOOR_REMOTE_CMD_NONE;
        return;
    }
    if (cmd == DOOR_CMD_LOCK) {
        ServoSetTarget(DOOR_LOCK_ANGLE_DEG);
        g_unlock_timer = 0;
        g_door_remote_cmd = DOOR_REMOTE_CMD_NONE;
        return;
    }

    /* 本地：PIR 或按键触发 → 重置 10 秒计时器 */
    if (g_pir_motion || btn_pressed) {
        g_unlock_timer = 10000;  /* 10 秒 */
        ServoSetTarget(DOOR_UNLOCK_ANGLE_DEG);
    }

    /* 倒计时 */
    if (g_unlock_timer > 0) {
        if (g_unlock_timer <= elapsed_ms) {
            g_unlock_timer = 0;
            ServoSetTarget(DOOR_LOCK_ANGLE_DEG);  /* 时间到，闭锁 */
        } else {
            g_unlock_timer -= elapsed_ms;
        }
    }
}

/* ======================== 主任务 ======================== */

static void DoorAccessTask(void *arg)
{
    (void)arg;

    DOOR_PRINTF("============================================");
    DOOR_PRINTF("SLE Wireless Door Access System");
    DOOR_PRINTF("  servo: GPIO%d  btn: GPIO%d  LED: GPIO%d",
                DOOR_SERVO_GPIO, DOOR_BUTTON_GPIO, DOOR_LED_GPIO);
    DOOR_PRINTF("  btn pressed -> unlock(90deg)  release -> slow lock(0deg)");
    DOOR_PRINTF("============================================");

    /* ---- 硬件初始化 ---- */
    IoTGpioInit(DOOR_SERVO_GPIO);
    IoTGpioSetDir(DOOR_SERVO_GPIO, IOT_GPIO_DIR_OUT);
    IoTGpioSetOutputVal(DOOR_SERVO_GPIO, IOT_GPIO_VALUE0);

    IoTGpioInit(DOOR_BUTTON_GPIO);
    IoTGpioSetDir(DOOR_BUTTON_GPIO, IOT_GPIO_DIR_IN);

    DoorLedInit();
    uapi_adc_init(ADC_CLOCK_NONE);
    DOOR_PRINTF("hardware init done");

    /* ---- SLE 初始化 ---- */
    osal_msleep(DOOR_START_DELAY_MS);
    sle_uart_server_register_msg(OnSleMessageReceived);

    errcode_t ret = sle_uart_server_init();
    if (ret != ERRCODE_SLE_SUCCESS) {
        DOOR_PRINTF("SLE init FAILED (0x%x) — local-only mode", (unsigned)ret);
    } else {
        DOOR_PRINTF("SLE server OK, waiting for phone...");
    }

    /* ---- 主循环 ---- */
    DOOR_PRINTF("main loop (%ums)", (unsigned)MAIN_LOOP_INTERVAL_MS);
    uint32_t elapsed = MAIN_LOOP_INTERVAL_MS;
    for (;;) {
        DoorPirSample(elapsed);
        uint8_t btn = (DoorButtonRead() == 0) ? 1 : 0;
        DoorControl(elapsed, btn);
        DoorLedUpdate(elapsed);
        ServoMoveStep();
        IoTWatchDogKick();
        osDelay(MS_TO_TICKS(MAIN_LOOP_INTERVAL_MS));
        elapsed = MAIN_LOOP_INTERVAL_MS;
    }
}

/* ======================== 系统入口 ======================== */

static void DoorAccessEntry(void)
{
    osThreadAttr_t attr = {
        .name = "DoorAccessTask", .stack_size = DOOR_TASK_STACK_SIZE,
        .priority = DOOR_TASK_PRIO,
    };
    osal_kthread_lock();
    if (osThreadNew((osThreadFunc_t)DoorAccessTask, NULL, &attr) == NULL)
        printf(DOOR_LOG " create task fail\r\n");
    else
        printf(DOOR_LOG " task started\r\n");
    osal_kthread_unlock();
}

APP_FEATURE_INIT(DoorAccessEntry);
