/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * door_access.c — SLE 星闪无线简易门禁系统 主应用
 *
 * ======================== 功能概述 ========================
 *
 * 功能1：本地按键开门
 *   SR602 人体红外传感器（GPIO12 → ADC5）检测人体，
 *   GPIO8 按键按下 → 舵机开锁（GPIO10，90°）。
 *   两个条件同时满足才触发（AND 逻辑），防止误触发：
 *     PIR 检测到人 + 按键按下 → 开锁。
 *
 * 功能2：SLE 星闪无线管控
 *   门禁板作为 SLE SSAP Server 广播，手机作为 SLE Client
 *   扫描连接后，通过 SSAP Write 发送命令：
 *     0x00 = 闭锁  |  0x01 = 开锁
 *   门禁板收到后执行，并通过 Notify 回复状态。
 *
 *   SLE 远程命令优先级高于本地控制。
 *
 * ======================== 自动落锁 ========================
 *
 *   开锁后，PIR 确认无人后 10 秒自动闭锁。
 *   PIR 持续检测到人时，重置定时器保持开锁。
 *   多人流场景：人走后才倒数。
 *
 * ======================== LED 指示 ========================
 *
 *   闭锁 + SLE已连接   → 灭
 *   闭锁 + SLE未连接   → 慢闪（500ms）
 *   运动中              → 快闪（200ms）
 *   开锁 + SLE已连接   → 常亮
 *   开锁 + SLE未连接   → 慢闪
 *
 * ======================== 舵机修复说明 ========================
 *
 *   【原问题】每步只发 1 个 PWM 脉冲，两次脉冲间隔 50ms（其中 30ms 静默），
 *            舵机丢失参考信号 → 抽搐。
 *   【修复】  每步连续发送多个 50Hz 脉冲，舵机获得稳定参考信号。
 *            到位后周期性发送保持 PWM，舵机不丢力矩。
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

/* 舵机 */
static unsigned int g_servo_angle    = 0;   /* 当前角度 */
static unsigned int g_servo_target   = 0;   /* 目标角度 */

/* 门禁 */
static door_state_t g_door_state     = DOOR_STATE_LOCKED;
static uint32_t     g_lock_timer     = 0;   /* PIR无人后自动落锁倒计时（ms） */
static uint32_t     g_max_timer      = 0;   /* 最大开门时间兜底（ms） */
static uint32_t     g_cooldown_timer = 0;   /* 关门后冷却期倒计时（ms） */

/* SLE 远程命令 */
static volatile uint8_t g_remote_cmd = DOOR_REMOTE_CMD_NONE;

/* 按键消抖 */
static uint8_t g_btn_stable       = 1;
static uint8_t g_btn_last_raw     = 1;
static uint8_t g_btn_debounce_cnt = 0;

/* PIR */
static uint32_t g_pir_last_ms  = 0;
static uint8_t  g_pir_motion   = 0;
static uint8_t  g_pir_idle_cnt = 0;   /* 连续无人计数 */

/* LED */
static uint32_t g_led_toggle_ms = 0;
static uint8_t  g_led_on        = 0;

/* ======================== 日志 ======================== */

#define DOOR_PRINTF(fmt, args...) printf(DOOR_LOG " " fmt "\r\n", ##args)

/* ======================== 舵机控制 ======================== */

/*
 * 输出一个完整的 50Hz PWM 周期（20ms）
 *   高电平 pulse_us → 低电平 (20ms - pulse_us)
 */
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
 * ServoMoveStep — 每主循环调用一次，逐步逼近目标角度。
 *
 * 【关键设计】参考 pwm_servo.c:
 *   - 移动中：每个角度发多个连续脉冲，角度之间零间隔（不打 osDelay）
 *   - 静止时：每轮都发保持脉冲（10 个=200ms），舵机始终有力矩
 *
 *   每步 SERVO_STEP_DEG°(6°)，每步 SERVO_MOVE_BURST_CYCLES 个脉冲。
 *   开门 90°/6°=15 步 × ~205ms ≈ 3 秒。
 */
static void ServoMoveStep(void)
{
    if (g_servo_angle == g_servo_target) {
        /* 静止：每轮都发少量保持脉冲，舵机始终有力矩，不攒不阻塞 */
        uint32_t us = ServoAngleToPulseUs(g_servo_angle);
        for (unsigned int i = 0; i < SERVO_HOLD_CYCLES; i++) {
            ServoSendPulseUs(us);
        }
        return;
    }

    /* ---- 移动 ---- */
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
        if (sle_ok) {
            DoorLedSet(0);
        } else if (g_led_toggle_ms >= LED_SLOW_BLINK_MS) {
            g_led_toggle_ms = 0;
            DoorLedSet(g_led_on ? 0 : 1);
        }
        break;

    case DOOR_STATE_UNLOCKING:
    case DOOR_STATE_LOCKING:
        if (g_led_toggle_ms >= LED_FAST_BLINK_MS) {
            g_led_toggle_ms = 0;
            DoorLedSet(g_led_on ? 0 : 1);
        }
        break;

    case DOOR_STATE_UNLOCKED:
        if (sle_ok) {
            DoorLedSet(1);
        } else if (g_led_toggle_ms >= LED_SLOW_BLINK_MS) {
            g_led_toggle_ms = 0;
            DoorLedSet(g_led_on ? 0 : 1);
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
    if (cmd == DOOR_CMD_LOCK || cmd == DOOR_CMD_UNLOCK) {
        g_remote_cmd = cmd;
    }
}

static void DoorSleNotifyState(void)
{
    if (!sle_uart_client_is_connected()) return;
    uint8_t s = (g_door_state == DOOR_STATE_LOCKED) ? DOOR_CMD_LOCK : DOOR_CMD_UNLOCK;
    errcode_t ret = sle_uart_server_send_report_by_handle(&s, 1);
    if (ret != ERRCODE_SLE_SUCCESS)
        DOOR_PRINTF("Notify fail: 0x%x", (unsigned)ret);
    else
        DOOR_PRINTF("Notify %s -> phone", s == DOOR_CMD_LOCK ? "LOCKED" : "UNLOCKED");
}

/* ======================== PIR 检测 ======================== */

/*
 * DoorPirSample — 采样 SR602 PIR 传感器。
 *
 *   PIR 无人时需连续 N 次确认（防短暂掉信号误关）。
 *   PIR 有人时 1 次即响应（保证开门快）。
 *   中间值（750~2200mV）保持上一次判定，滞回防抖。
 */
static void DoorPirSample(uint32_t elapsed_ms)
{
    g_pir_last_ms += elapsed_ms;
    if (g_pir_last_ms < PIR_SAMPLE_INTERVAL_MS) return;
    g_pir_last_ms = 0;

    uint16_t mv = 0;
    if (adc_port_read(DOOR_PIR_ADC_CHANNEL, &mv) != ERRCODE_SUCC) return;

    if (mv >= ADC_HUMAN_MOTION_GE_MV) {
        g_pir_idle_cnt = 0;
        g_pir_motion = 1;   /* 有人：立即响应 */
    } else if (mv <= ADC_HUMAN_IDLE_LE_MV) {
        g_pir_idle_cnt++;
        if (g_pir_idle_cnt >= PIR_IDLE_CONFIRM_SAMPLES) {
            g_pir_motion = 0;
            g_pir_idle_cnt = PIR_IDLE_CONFIRM_SAMPLES;  /* 钳位 */
        }
    }
    /* 中间值保持上一次判定 */
}

/* ======================== 门禁控制 ======================== */

/*
 * DoorControl — 每主循环调用一次。
 *
 * 本地触发（AND）: PIR有人 + 按键按下 → 开锁
 * SLE 远程:        手机直接下发开锁/闭锁命令
 * 自动落锁:        PIR确认无人后倒数 10s → 闭锁
 *
 * 状态转换:
 *   LOCKED   → UNLOCKING:  (PIR+按键) 或 SLE开锁
 *   UNLOCKING→ UNLOCKED:   舵机到达90°
 *   UNLOCKED → LOCKING:    自动落锁超时 或 SLE闭锁
 *   LOCKING  → LOCKED:     舵机到达0°
 *   LOCKING  → UNLOCKING:  (PIR+按键) 或 SLE开锁（中断关门）
 */
static void DoorControl(uint32_t elapsed_ms, uint8_t btn)
{
    uint8_t cmd = g_remote_cmd;

    switch (g_door_state) {

    /* ----- 闭锁态 ----- */
    case DOOR_STATE_LOCKED:
        /* 冷却期倒数 */
        if (g_cooldown_timer > 0) {
            g_cooldown_timer = (g_cooldown_timer <= elapsed_ms)
                ? 0 : (g_cooldown_timer - elapsed_ms);
        }
        /* SLE 远程开锁：不受冷却期限制 */
        if (cmd == DOOR_CMD_UNLOCK) {
            DOOR_PRINTF("SLE UNLOCK");
            ServoSetTarget(DOOR_UNLOCK_ANGLE_DEG);
            g_lock_timer = 0;
            g_cooldown_timer = 0;
            g_door_state = DOOR_STATE_UNLOCKING;
            g_remote_cmd = DOOR_REMOTE_CMD_NONE;
            break;
        }
        /* 本地: PIR+BTN，冷却期内不响应 */
        if (g_cooldown_timer > 0) break;
        if (g_pir_motion && btn) {
            DOOR_PRINTF("local trigger: PIR+BTN → UNLOCKING");
            ServoSetTarget(DOOR_UNLOCK_ANGLE_DEG);
            g_lock_timer = 0;
            g_door_state = DOOR_STATE_UNLOCKING;
        }
        break;

    /* ----- 开锁中 ----- */
    case DOOR_STATE_UNLOCKING:
        if (cmd == DOOR_CMD_LOCK) {
            DOOR_PRINTF("SLE LOCK (abort)");
            ServoSetTarget(DOOR_LOCK_ANGLE_DEG);
            g_lock_timer = 0;
            g_door_state = DOOR_STATE_LOCKING;
            g_remote_cmd = DOOR_REMOTE_CMD_NONE;
            break;
        }
        if (g_servo_angle == g_servo_target &&
            g_servo_target == DOOR_UNLOCK_ANGLE_DEG) {
            DOOR_PRINTF("arrived: UNLOCKED");
            g_lock_timer = AUTO_LOCK_TIMEOUT_MS;
            g_max_timer  = AUTO_LOCK_MAX_MS;
            g_door_state = DOOR_STATE_UNLOCKED;
        }
        break;

    /* ----- 开锁态 ----- */
    case DOOR_STATE_UNLOCKED:
        if (cmd == DOOR_CMD_LOCK) {
            DOOR_PRINTF("SLE LOCK");
            ServoSetTarget(DOOR_LOCK_ANGLE_DEG);
            g_lock_timer = 0;
            g_max_timer  = 0;
            g_door_state = DOOR_STATE_LOCKING;
            g_remote_cmd = DOOR_REMOTE_CMD_NONE;
            break;
        }
        /* PIR有人 → 重置 PIR 定时器 */
        if (g_pir_motion) {
            g_lock_timer = AUTO_LOCK_TIMEOUT_MS;
        }
        /* 自动落锁：PIR 无人 10s 到期 或 最大开门 30s 到期 */
        if (g_lock_timer > 0) {
            if (g_lock_timer <= elapsed_ms) {
                g_lock_timer = 0;
                g_max_timer  = 0;
                DOOR_PRINTF("auto-lock (PIR idle) → LOCKING");
                ServoSetTarget(DOOR_LOCK_ANGLE_DEG);
                g_door_state = DOOR_STATE_LOCKING;
                break;
            }
            g_lock_timer -= elapsed_ms;
        }
        if (g_max_timer > 0) {
            if (g_max_timer <= elapsed_ms) {
                g_max_timer  = 0;
                g_lock_timer = 0;
                DOOR_PRINTF("auto-lock (max time) → LOCKING");
                ServoSetTarget(DOOR_LOCK_ANGLE_DEG);
                g_door_state = DOOR_STATE_LOCKING;
                break;
            }
            g_max_timer -= elapsed_ms;
        }
        break;

    /* ----- 闭锁中 ----- */
    case DOOR_STATE_LOCKING:
        if (cmd == DOOR_CMD_UNLOCK) {
            DOOR_PRINTF("SLE UNLOCK (abort)");
            ServoSetTarget(DOOR_UNLOCK_ANGLE_DEG);
            g_lock_timer = 0;
            g_door_state = DOOR_STATE_UNLOCKING;
            g_remote_cmd = DOOR_REMOTE_CMD_NONE;
            break;
        }
        /* 本地中断关门: PIR有人 AND 按键 */
        if (g_pir_motion && btn) {
            DOOR_PRINTF("PIR+BTN re-trigger → reopen");
            ServoSetTarget(DOOR_UNLOCK_ANGLE_DEG);
            g_door_state = DOOR_STATE_UNLOCKING;
            break;
        }
        if (g_servo_angle == g_servo_target &&
            g_servo_target == DOOR_LOCK_ANGLE_DEG) {
            DOOR_PRINTF("arrived: LOCKED (cooldown %ds)", LOCK_COOLDOWN_MS / 1000);
            g_cooldown_timer = LOCK_COOLDOWN_MS;
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
    DOOR_PRINTF("  local: PIR+BTN(AND) → open, auto-lock: %ds, cooldown: %ds",
                AUTO_LOCK_TIMEOUT_MS / 1000, LOCK_COOLDOWN_MS / 1000);
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
    if (ret != ERRCODE_SLE_SUCCESS) {
        DOOR_PRINTF("SLE init FAILED (0x%x) — local-only", (unsigned)ret);
    } else {
        DOOR_PRINTF("SLE server OK");
    }

    g_door_state = DOOR_STATE_LOCKED;
    DOOR_PRINTF("state: LOCKED, starting main loop");

    /*
     * 启动时立即发一组脉冲把舵机定在 0°，防止前几轮无 PWM 舵机漂移。
     */
    {
        uint32_t us = ServoAngleToPulseUs(0);
        for (unsigned int i = 0; i < SERVO_HOLD_CYCLES; i++) {
            ServoSendPulseUs(us);
        }
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

        /*
         * 静止时：每轮已发 10 个保持脉冲(200ms)，再 osDelay(50ms)=250ms/轮。
         *          舵机始终有力矩，PIR 每轮都检测。
         * 移动时：不打 osDelay，脉冲连续发送无间隙 → 不抽搐。
         */
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
