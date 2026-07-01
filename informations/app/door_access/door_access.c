/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * door_access.c — SLE 星闪无线简易门禁系统 主应用
 *
 * ======================== 功能概述 ========================
 *
 * 功能1：本地按键开门
 *   SR602 人体红外传感器（GPIO12 → ADC5）+ GPIO8 按键按下
 *   → 舵机正向转90°开门 → 等2s → 反向转90°关门 → PIR重开待下次触发
 *   PIR+按键同时满足才触发（AND 逻辑），防止误触发。
 *
 * 功能2：SLE 星闪无线管控
 *   手机通过 SLE 远程发送 0x00=闭锁 / 0x01=开锁
 *
 * ======================== LED 指示 ========================
 *
 *   闭锁 + SLE已连接   → 灭
 *   闭锁 + SLE未连接   → 慢闪（500ms）
 *   运动中              → 快闪（200ms）
 *   开锁 + SLE已连接   → 常亮
 *   开锁 + SLE未连接   → 慢闪
 */

#include "door_access.h"

#include <stdio.h>
#include <string.h>
#include "securec.h"
#include "common_def.h"
#include "soc_osal.h"
#include "errcode.h"

#include "iot_gpio.h"
#include "iot_watchdog.h"
#include "adc.h"
#include "adc_porting.h"

#include "sle_uart_server.h"
#include "sle_errcode.h"

/* ======================== 全局状态 ======================== */

static unsigned int g_servo_angle  = 0;
static unsigned int g_servo_target = 0;

static door_state_t g_door_state = DOOR_STATE_LOCKED;
static uint32_t     g_hold_timer = 0;   /* 开门后等待计时（ms） */

static volatile uint8_t g_remote_cmd = DOOR_REMOTE_CMD_NONE;

/* 按键消抖 */
static uint8_t g_btn_stable       = 1;
static uint8_t g_btn_last_raw     = 1;
static uint8_t g_btn_debounce_cnt = 0;

/* PIR */
static uint32_t g_pir_last_ms  = 0;
static uint8_t  g_pir_motion   = 0;
static uint8_t  g_pir_idle_cnt = 0;

/* LED */
static uint32_t g_led_toggle_ms = 0;
static uint8_t  g_led_on        = 0;

#define DOOR_PRINTF(fmt, args...) printf(DOOR_LOG " " fmt "\r\n", ##args)

/* ======================== 舵机控制 ======================== */

static void ServoSendPulseUs(uint32_t pulse_us)
{
    if (pulse_us > SERVO_PWM_PERIOD_US) pulse_us = SERVO_PWM_PERIOD_US;
    IoTGpioSetOutputVal(DOOR_SERVO_GPIO, IOT_GPIO_VALUE1);
    (void)uapi_tcxo_delay_us(pulse_us);
    IoTGpioSetOutputVal(DOOR_SERVO_GPIO, IOT_GPIO_VALUE0);
    (void)uapi_tcxo_delay_us(SERVO_PWM_PERIOD_US - pulse_us);
}

static uint32_t ServoAngleToPulseUs(unsigned int angle)
{
    if (angle > 180) angle = 180;
    return SERVO_PULSE_MIN_US + (angle * SERVO_PULSE_RANGE_US) / 180;
}

static void ServoSetTarget(unsigned int angle)
{
    if (angle > 180) angle = 180;
    g_servo_target = angle;
}

/*
 * ServoMoveStep — 每主循环调用一次。
 *   移动中：每步发 SERVO_MOVE_BURST_CYCLES 个脉冲，不打 osDelay（零间隙）
 *   静止时：每轮发 SERVO_HOLD_CYCLES 个保持脉冲
 */
static void ServoMoveStep(void)
{
    if (g_servo_angle == g_servo_target) {
        uint32_t us = ServoAngleToPulseUs(g_servo_angle);
        for (unsigned int i = 0; i < SERVO_HOLD_CYCLES; i++) {
            ServoSendPulseUs(us);
        }
        return;
    }

    if (g_servo_angle < g_servo_target) {
        g_servo_angle += SERVO_STEP_DEG;
        if (g_servo_angle > g_servo_target) g_servo_angle = g_servo_target;
    } else {
        if (g_servo_angle < SERVO_STEP_DEG)
            g_servo_angle = 0;
        else
            g_servo_angle -= SERVO_STEP_DEG;
        if (g_servo_angle < g_servo_target) g_servo_angle = g_servo_target;
    }

    uint32_t us = ServoAngleToPulseUs(g_servo_angle);
    for (unsigned int i = 0; i < SERVO_MOVE_BURST_CYCLES; i++) {
        ServoSendPulseUs(us);
    }
}

/* ======================== 按键消抖 ======================== */

static uint8_t DoorButtonRead(void)
{
    IotGpioValue val = IOT_GPIO_VALUE0;
    IoTGpioGetInputVal(DOOR_BUTTON_GPIO, &val);
    uint8_t raw = (uint8_t)val;

    if (raw == g_btn_last_raw) {
        if (g_btn_debounce_cnt < BUTTON_DEBOUNCE_SAMPLES) {
            g_btn_debounce_cnt++;
            if (g_btn_debounce_cnt >= BUTTON_DEBOUNCE_SAMPLES)
                g_btn_stable = raw;
        }
    } else {
        g_btn_debounce_cnt = 0;
        g_btn_last_raw = raw;
    }
    return g_btn_stable;
}

/* ======================== LED ======================== */

static void DoorLedInit(void)
{
    IoTGpioInit(DOOR_LED_GPIO);
    IoTGpioSetDir(DOOR_LED_GPIO, IOT_GPIO_DIR_OUT);
    IoTGpioSetOutputVal(DOOR_LED_GPIO, IOT_GPIO_VALUE1);
}

static void DoorLedSet(uint8_t on)
{
    g_led_on = on ? 1 : 0;
    IoTGpioSetOutputVal(DOOR_LED_GPIO, on ? IOT_GPIO_VALUE0 : IOT_GPIO_VALUE1);
}

static void DoorLedUpdate(uint32_t elapsed_ms)
{
    g_led_toggle_ms += elapsed_ms;
    uint8_t sle_ok = sle_uart_client_is_connected() ? 1 : 0;

    switch (g_door_state) {
    case DOOR_STATE_LOCKED:
        if (sle_ok) DoorLedSet(0);
        else if (g_led_toggle_ms >= LED_SLOW_BLINK_MS) {
            g_led_toggle_ms = 0; DoorLedSet(g_led_on ? 0 : 1);
        }
        break;
    case DOOR_STATE_UNLOCKING:
    case DOOR_STATE_LOCKING:
        if (g_led_toggle_ms >= LED_FAST_BLINK_MS) {
            g_led_toggle_ms = 0; DoorLedSet(g_led_on ? 0 : 1);
        }
        break;
    case DOOR_STATE_UNLOCKED:
        if (sle_ok) DoorLedSet(1);
        else if (g_led_toggle_ms >= LED_SLOW_BLINK_MS) {
            g_led_toggle_ms = 0; DoorLedSet(g_led_on ? 0 : 1);
        }
        break;
    }
}

/* ======================== SLE ======================== */

static void OnSleMessageReceived(uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0) return;
    uint8_t cmd = data[0];
    DOOR_PRINTF("SLE recv cmd=0x%02X (%s)", (unsigned)cmd,
                cmd == DOOR_CMD_LOCK ? "LOCK" :
                cmd == DOOR_CMD_UNLOCK ? "UNLOCK" : "?");
    if (cmd == DOOR_CMD_LOCK || cmd == DOOR_CMD_UNLOCK)
        g_remote_cmd = cmd;
}

/* ======================== PIR 检测 ======================== */

static void DoorPirSample(uint32_t elapsed_ms)
{
    g_pir_last_ms += elapsed_ms;
    if (g_pir_last_ms < PIR_SAMPLE_INTERVAL_MS) return;
    g_pir_last_ms = 0;

    uint16_t mv = 0;
    if (adc_port_read(DOOR_PIR_ADC_CHANNEL, &mv) != ERRCODE_SUCC) return;

    if (mv >= ADC_HUMAN_MOTION_GE_MV) {
        g_pir_idle_cnt = 0;
        g_pir_motion = 1;
    } else if (mv <= ADC_HUMAN_IDLE_LE_MV) {
        g_pir_idle_cnt++;
        if (g_pir_idle_cnt >= PIR_IDLE_CONFIRM_SAMPLES) {
            g_pir_motion = 0;
            g_pir_idle_cnt = PIR_IDLE_CONFIRM_SAMPLES;
        }
    }
}

/* ======================== 门禁控制 ======================== */

/*
 * 简单流程: PIR+按键 → 正向开90° → 等2s → 反向关90° → PIR重开
 * SLE: 手机远程开/关
 */
static void DoorControl(uint32_t elapsed_ms, uint8_t btn)
{
    uint8_t cmd = g_remote_cmd;

    switch (g_door_state) {

    case DOOR_STATE_LOCKED:
        if (cmd == DOOR_CMD_UNLOCK) {
            DOOR_PRINTF("SLE UNLOCK");
            ServoSetTarget(DOOR_UNLOCK_ANGLE_DEG);
            g_door_state = DOOR_STATE_UNLOCKING;
            g_remote_cmd = DOOR_REMOTE_CMD_NONE;
            break;
        }
        if (g_pir_motion && btn) {
            DOOR_PRINTF("PIR+BTN → opening");
            ServoSetTarget(DOOR_UNLOCK_ANGLE_DEG);
            g_door_state = DOOR_STATE_UNLOCKING;
        }
        break;

    case DOOR_STATE_UNLOCKING:
        if (g_servo_angle == DOOR_UNLOCK_ANGLE_DEG) {
            DOOR_PRINTF("opened, wait %dms", UNLOCK_HOLD_MS);
            g_hold_timer = UNLOCK_HOLD_MS;
            g_door_state = DOOR_STATE_UNLOCKED;
        }
        break;

    case DOOR_STATE_UNLOCKED:
        if (cmd == DOOR_CMD_LOCK) {
            DOOR_PRINTF("SLE LOCK");
            ServoSetTarget(DOOR_LOCK_ANGLE_DEG);
            g_door_state = DOOR_STATE_LOCKING;
            g_remote_cmd = DOOR_REMOTE_CMD_NONE;
            break;
        }
        if (g_hold_timer <= elapsed_ms) {
            DOOR_PRINTF("hold done → closing");
            ServoSetTarget(DOOR_LOCK_ANGLE_DEG);
            g_door_state = DOOR_STATE_LOCKING;
        } else {
            g_hold_timer -= elapsed_ms;
        }
        break;

    case DOOR_STATE_LOCKING:
        if (g_servo_angle == DOOR_LOCK_ANGLE_DEG) {
            DOOR_PRINTF("closed, PIR re-armed");
            g_door_state = DOOR_STATE_LOCKED;
        }
        break;
    }
}

/* ======================== 主任务 ======================== */

static void DoorAccessTask(void *arg)
{
    (void)arg;

    DOOR_PRINTF("============================================");
    DOOR_PRINTF("SLE Door Access System");
    DOOR_PRINTF("  GPIO%d=servo GPIO%d=btn GPIO%d=LED ADC%d=PIR",
                DOOR_SERVO_GPIO, DOOR_BUTTON_GPIO,
                DOOR_LED_GPIO, DOOR_PIR_ADC_CHANNEL);
    DOOR_PRINTF("  PIR+BTN → open 90° → wait %ds → close 0°",
                UNLOCK_HOLD_MS / 1000);
    DOOR_PRINTF("============================================");

    /* 硬件初始化 */
    IoTGpioInit(DOOR_SERVO_GPIO);
    IoTGpioSetDir(DOOR_SERVO_GPIO, IOT_GPIO_DIR_OUT);
    IoTGpioSetOutputVal(DOOR_SERVO_GPIO, IOT_GPIO_VALUE0);

    IoTGpioInit(DOOR_BUTTON_GPIO);
    IoTGpioSetDir(DOOR_BUTTON_GPIO, IOT_GPIO_DIR_IN);

    DoorLedInit();
    uapi_adc_init(ADC_CLOCK_NONE);
    DOOR_PRINTF("hardware init done");

    /* SLE 初始化 */
    osal_msleep(DOOR_START_DELAY_MS);
    sle_uart_server_register_msg(OnSleMessageReceived);

    errcode_t ret = sle_uart_server_init();
    if (ret != ERRCODE_SLE_SUCCESS)
        DOOR_PRINTF("SLE init FAILED (0x%x) — local-only", (unsigned)ret);
    else
        DOOR_PRINTF("SLE server OK");

    g_door_state = DOOR_STATE_LOCKED;

    /* 启动时舵机定在 0° */
    {
        uint32_t us = ServoAngleToPulseUs(0);
        for (unsigned int i = 0; i < SERVO_HOLD_CYCLES; i++)
            ServoSendPulseUs(us);
    }

    /* 主循环 */
    uint32_t elapsed = MAIN_LOOP_INTERVAL_MS;

    for (;;) {
        uint8_t moving = (g_servo_angle != g_servo_target) ? 1 : 0;

        DoorPirSample(elapsed);
        uint8_t btn = (DoorButtonRead() == 0) ? 1 : 0;
        DoorControl(elapsed, btn);
        ServoMoveStep();
        DoorLedUpdate(elapsed);
        IoTWatchDogKick();

        if (!moving) {
            osDelay(MS_TO_TICKS(MAIN_LOOP_INTERVAL_MS));
            elapsed = SERVO_HOLD_CYCLES * (SERVO_PWM_PERIOD_US / 1000)
                     + MAIN_LOOP_INTERVAL_MS;
        } else {
            elapsed = SERVO_MOVE_BURST_CYCLES * (SERVO_PWM_PERIOD_US / 1000);
        }
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
