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
 *   PIR 检测到人 或 按键按下 即触发开锁（OR 逻辑）。
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
 *   LOCKED ──(本地按键/PIR 或 手机开锁)──> UNLOCKING ──> UNLOCKED
 *   UNLOCKED ──(手机闭锁 或 自动落锁超时)──> LOCKING ──> LOCKED
 *   LOCKING  ──(PIR 又重新检测到人)──────> UNLOCKING（重新开锁）
 *
 *   自动落锁：PIR 真正无人后 10 秒自动闭锁（PIR 持续有人时重置）。
 *   PIR 防抖：需连续 N 次无人采样才确认无人，防止短暂掉信号误关。
 *   PIR 冷却期：从有人→无人后 3 秒内 PIR 重新触发会被忽略，
 *     防止关门动作误触发 PIR 造成"开→关→开→关"无限振荡。
 *
 * ======================== LED 指示 ========================
 *
 *   LOCKED + SLE已连接   → 灭
 *   LOCKED + SLE未连接   → 慢闪（500ms）
 *   UNLOCKING / LOCKING  → 快闪（200ms）
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

/* 舵机 */
static unsigned int g_servo_angle       = 0;    /* 当前跟踪角度 */
static unsigned int g_servo_target      = 0;    /* 目标角度 */
static uint8_t      g_servo_hold_ctr    = 0;    /* 到位后保持 PWM 计数器 */

/* 门禁状态机 */
static door_state_t g_door_state        = DOOR_STATE_LOCKED;
static uint32_t     g_unlock_timer      = 0;    /* 开锁剩余时间（ms） */

/* SLE 远程命令 */
static volatile uint8_t g_door_remote_cmd = DOOR_REMOTE_CMD_NONE;

/* 按键消抖 */
static uint8_t g_button_stable     = 1;
static uint8_t g_button_last_raw   = 1;
static uint8_t g_button_debounce_cnt = 0;

/* PIR 采样 + 防抖 */
static uint32_t g_pir_last_ms      = 0;
static uint8_t  g_pir_motion       = 0;    /* 当前 PIR 判定：0=无人 1=有人 */
static uint8_t  g_pir_idle_cnt     = 0;    /* 连续无人采样次数（防抖） */
static uint32_t g_pir_cooldown_ms  = 0;    /* PIR 冷却期剩余时间（ms） */

/* LED */
static uint32_t g_led_last_toggle_ms = 0;
static uint8_t  g_led_current_on     = 0;

/* ======================== 日志 ======================== */

#define DOOR_PRINTF(fmt, args...) printf(DOOR_LOG " " fmt "\r\n", ##args)

/* ======================== 舵机控制 ======================== */

/*
 * ServoSendPulseUs — 输出一个完整的 50Hz PWM 周期
 *
 * 时序：拉高 pulse_us → 拉低 (20ms - pulse_us)
 * 舵机根据高电平宽度判断目标角度。
 * 注意：uapi_tcxo_delay_us() 是忙等（busy-wait），会阻塞 CPU。
 */
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
 * ServoSetTarget — 设置舵机目标角度。主循环中 ServoMoveStep() 逐步逼近。
 */
static void ServoSetTarget(unsigned int angle_deg)
{
    if (angle_deg > 180) angle_deg = 180;
    g_servo_target = angle_deg;
}

/*
 * ServoMoveStep — 每主循环调用一次。
 *
 * 【关键修复】
 * 原代码每步只发 1 个 PWM 脉冲，两次脉冲之间间隔 50ms（其中 30ms 静默）。
 * 舵机在静默期丢失参考信号，下一个脉冲到达时角度突变 → 抽搐。
 *
 * 修复：每步连续发送 HOLD_CYCLES 个 PWM 脉冲（15 个 ≈ 300ms），
 * 提供稳定连续的 50Hz 参考信号，舵机平滑过渡。
 *
 * 到位后：每隔 HOLD_REFRESH_LOOPS 次循环发送一组保持 PWM，
 * 使舵机持续有保持力矩，不会因外力偏离位置。
 * 每步移动 3°，兼顾速度与平滑度。
 */
static void ServoMoveStep(void)
{
    /* ---- 到位：周期性发送保持 PWM，维持舵机力矩 ---- */
    if (g_servo_angle == g_servo_target) {
        g_servo_hold_ctr++;
        if (g_servo_hold_ctr >= SERVO_HOLD_REFRESH_LOOPS) {
            g_servo_hold_ctr = 0;
            uint32_t pulse_us = ServoAngleToPulseUs(g_servo_angle);
            for (unsigned int i = 0; i < SERVO_HOLD_CYCLES; i++) {
                ServoSendPulseUs(pulse_us);
            }
        }
        return;
    }

    g_servo_hold_ctr = 0;  /* 移动中，重置保持计数器 */

    /* ---- 向目标移动 3° ---- */
    if (g_servo_angle < g_servo_target) {
        g_servo_angle += 3;
        if (g_servo_angle > g_servo_target) g_servo_angle = g_servo_target;
    } else {
        if (g_servo_angle < 3) g_servo_angle = 0;
        else g_servo_angle -= 3;
        if (g_servo_angle < g_servo_target) g_servo_angle = g_servo_target;
    }

    /*
     * 发送 SERVO_MOVE_BURST_CYCLES 个连续 50Hz 脉冲（5 个 ≈ 100ms）。
     * 比 SERVO_HOLD_CYCLES（15 个 = 300ms）更短，保证主循环对
     * PIR/按键/SLE 的响应速度。5 个连续脉冲足够舵机获得稳定参考信号。
     * 开锁全程：90° / 3° × (100ms + 50ms osDelay) ≈ 4.5 秒。
     */
    uint32_t pulse_us = ServoAngleToPulseUs(g_servo_angle);
    for (unsigned int i = 0; i < SERVO_MOVE_BURST_CYCLES; i++) {
        ServoSendPulseUs(pulse_us);
    }
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

/*
 * DoorLedUpdate — 根据门禁状态机控制 LED 闪烁模式
 *
 *   LOCKED + SLE已连接   → 灭
 *   LOCKED + SLE未连接   → 慢闪（500ms）
 *   UNLOCKING / LOCKING  → 快闪（200ms）
 *   UNLOCKED + SLE已连接 → 常亮
 *   UNLOCKED + SLE未连接 → 慢闪
 */
static void DoorLedUpdate(uint32_t elapsed_ms)
{
    g_led_last_toggle_ms += elapsed_ms;
    uint8_t sle_ok = sle_uart_client_is_connected() ? 1 : 0;

    switch (g_door_state) {
    case DOOR_STATE_LOCKED:
        if (sle_ok) {
            DoorLedSet(0);  /* 已连接 → 灭 */
        } else if (g_led_last_toggle_ms >= LED_SLOW_BLINK_MS) {
            g_led_last_toggle_ms = 0;
            DoorLedSet(g_led_current_on ? 0 : 1);
        }
        break;

    case DOOR_STATE_UNLOCKING:
    case DOOR_STATE_LOCKING:
        /* 运动中 → 快闪 */
        if (g_led_last_toggle_ms >= LED_FAST_BLINK_MS) {
            g_led_last_toggle_ms = 0;
            DoorLedSet(g_led_current_on ? 0 : 1);
        }
        break;

    case DOOR_STATE_UNLOCKED:
        if (sle_ok) {
            DoorLedSet(1);  /* 已连接 → 常亮 */
        } else if (g_led_last_toggle_ms >= LED_SLOW_BLINK_MS) {
            g_led_last_toggle_ms = 0;
            DoorLedSet(g_led_current_on ? 0 : 1);
        }
        break;
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
    uint8_t s = (g_door_state == DOOR_STATE_LOCKED) ? DOOR_CMD_LOCK : DOOR_CMD_UNLOCK;
    errcode_t ret = sle_uart_server_send_report_by_handle(&s, 1);
    if (ret != ERRCODE_SLE_SUCCESS)
        DOOR_PRINTF("Notify fail: 0x%x", (unsigned)ret);
    else
        DOOR_PRINTF("Notify %s -> phone", s == DOOR_CMD_LOCK ? "LOCKED" : "UNLOCKED");
}

/* ======================== PIR 检测（带防抖 + 冷却期） ======================== */

/*
 * DoorPirSample — 每 ~100ms 采样一次 SR602 PIR 传感器。
 *
 * 防抖策略：
 *   1. 连续 PIR_IDLE_CONFIRM_SAMPLES 次（默认 5 次 ≈ 500ms）读数为「无人」
 *      才确认 g_pir_motion = 0。防止 PIR 短暂掉信号导致误关。
 *   2. 从有人→无人后启动冷却期（3 秒），冷却期内 PIR 重新触发会被忽略。
 *      这个冷却期是防止"关门动作触发PIR→重新开锁→又关门→又触发"的无限振荡。
 *   3. PIR 有人时立即响应（g_pir_motion 拉到 1 只需 1 次采样），保证开锁反应快。
 */
static void DoorPirSample(uint32_t elapsed_ms)
{
    g_pir_last_ms += elapsed_ms;
    if (g_pir_last_ms < PIR_SAMPLE_INTERVAL_MS) return;
    g_pir_last_ms = 0;

    /* 冷却期倒数 */
    if (g_pir_cooldown_ms > 0) {
        if (g_pir_cooldown_ms <= PIR_SAMPLE_INTERVAL_MS) {
            g_pir_cooldown_ms = 0;
        } else {
            g_pir_cooldown_ms -= PIR_SAMPLE_INTERVAL_MS;
        }
    }

    uint16_t mv = 0;
    if (adc_port_read(DOOR_PIR_ADC_CHANNEL, &mv) != ERRCODE_SUCC) return;

    if (mv >= ADC_HUMAN_MOTION_GE_MV) {
        /* 检测到人体活动 */
        g_pir_idle_cnt = 0;
        if (g_pir_cooldown_ms == 0) {
            /* 不在冷却期 → 立即响应 */
            g_pir_motion = 1;
        }
        /* 冷却期内 PIR 触发被忽略，防止关门动作造成振荡 */
    } else if (mv <= ADC_HUMAN_IDLE_LE_MV) {
        /* 无人：需要连续多次确认才真正判为无人 */
        g_pir_idle_cnt++;
        if (g_pir_idle_cnt >= PIR_IDLE_CONFIRM_SAMPLES) {
            if (g_pir_motion) {
                /* 从有人→无人，启动冷却期 */
                g_pir_cooldown_ms = PIR_COOLDOWN_AFTER_MOTION_MS;
            }
            g_pir_motion = 0;
            g_pir_idle_cnt = PIR_IDLE_CONFIRM_SAMPLES;  /* 钳位 */
        }
    }
    /* 中间值保持上一次判定（SAR ADC 噪声区滞回） */
}

/* ======================== 门禁控制（状态机 + 防抖） ======================== */

/*
 * DoorControl — 每主循环调用一次，实现门禁状态机。
 *
 * 状态转换：
 *   LOCKED    → UNLOCKING: PIR 有人 或 按键按下 或 SLE 开锁命令
 *   UNLOCKING → UNLOCKED:  舵机到达 90° 开锁位
 *   UNLOCKED  → LOCKING:   自动落锁超时 或 SLE 闭锁命令
 *   LOCKING   → LOCKED:    舵机到达 0° 闭锁位
 *   LOCKING   → UNLOCKING: PIR 重新检测到人（中断闭锁过程，重新开锁）
 *
 * 防抖/防振荡：
 *   - PIR 需经过冷却期后才允许重新触发（DoorPirSample 中实现）
 *   - 无人确认需连续多次采样（DoorPirSample 中实现）
 *   - 定时器仅在真正无人时倒数，PIR 有人时重置
 */
static void DoorControl(uint32_t elapsed_ms, uint8_t btn_pressed)
{
    uint8_t cmd = g_door_remote_cmd;

    switch (g_door_state) {

    /* ===== 闭锁态：等待开锁触发 ===== */
    case DOOR_STATE_LOCKED:
        /* SLE 远程开锁 */
        if (cmd == DOOR_CMD_UNLOCK) {
            DOOR_PRINTF("SLE remote UNLOCK");
            ServoSetTarget(DOOR_UNLOCK_ANGLE_DEG);
            g_unlock_timer = 0;  /* 远程开锁不自动关 */
            g_door_state = DOOR_STATE_UNLOCKING;
            g_door_remote_cmd = DOOR_REMOTE_CMD_NONE;
            break;
        }
        /* 本地：PIR 有人 或 按键按下 → 开锁 */
        if (g_pir_motion || btn_pressed) {
            DOOR_PRINTF("local trigger: PIR=%d BTN=%d → UNLOCKING", g_pir_motion, btn_pressed);
            ServoSetTarget(DOOR_UNLOCK_ANGLE_DEG);
            g_unlock_timer = 0;  /* PIR 有人时不倒数，由下方逻辑重置 */
            g_door_state = DOOR_STATE_UNLOCKING;
        }
        break;

    /* ===== 开锁中：舵机正在转到 90° ===== */
    case DOOR_STATE_UNLOCKING:
        /* SLE 闭锁命令可在中途打断 */
        if (cmd == DOOR_CMD_LOCK) {
            DOOR_PRINTF("SLE remote LOCK (abort unlock)");
            ServoSetTarget(DOOR_LOCK_ANGLE_DEG);
            g_unlock_timer = 0;
            g_door_state = DOOR_STATE_LOCKING;
            g_door_remote_cmd = DOOR_REMOTE_CMD_NONE;
            break;
        }
        /* 到达目标角度 → 进入开锁态 */
        if (g_servo_angle == g_servo_target && g_servo_target == DOOR_UNLOCK_ANGLE_DEG) {
            DOOR_PRINTF("unlocked");
            g_unlock_timer = AUTO_LOCK_TIMEOUT_MS;  /* 启动自动落锁倒计时 */
            g_door_state = DOOR_STATE_UNLOCKED;
        }
        break;

    /* ===== 开锁态：等待自动落锁 或 SLE 远程闭锁 ===== */
    case DOOR_STATE_UNLOCKED:
        /* SLE 远程闭锁 */
        if (cmd == DOOR_CMD_LOCK) {
            DOOR_PRINTF("SLE remote LOCK");
            ServoSetTarget(DOOR_LOCK_ANGLE_DEG);
            g_unlock_timer = 0;
            g_door_state = DOOR_STATE_LOCKING;
            g_door_remote_cmd = DOOR_REMOTE_CMD_NONE;
            break;
        }
        /* PIR 持续检测到人 → 重置定时器，保持开锁 */
        if (g_pir_motion) {
            g_unlock_timer = AUTO_LOCK_TIMEOUT_MS;
        }
        /* 自动落锁倒计时 */
        if (g_unlock_timer > 0) {
            if (g_unlock_timer <= elapsed_ms) {
                g_unlock_timer = 0;
                DOOR_PRINTF("auto-lock timeout → LOCKING");
                ServoSetTarget(DOOR_LOCK_ANGLE_DEG);
                g_door_state = DOOR_STATE_LOCKING;
            } else {
                g_unlock_timer -= elapsed_ms;
            }
        }
        break;

    /* ===== 闭锁中：舵机正在转到 0°。允许 PIR 重新触发 ===== */
    case DOOR_STATE_LOCKING:
        /* SLE 开锁可在中途打断 */
        if (cmd == DOOR_CMD_UNLOCK) {
            DOOR_PRINTF("SLE remote UNLOCK (abort lock)");
            ServoSetTarget(DOOR_UNLOCK_ANGLE_DEG);
            g_unlock_timer = 0;
            g_door_state = DOOR_STATE_UNLOCKING;
            g_door_remote_cmd = DOOR_REMOTE_CMD_NONE;
            break;
        }
        /* PIR 重新检测到人 → 中断闭锁，重新开锁 */
        if (g_pir_motion) {
            DOOR_PRINTF("PIR re-triggered during LOCKING → reopen");
            ServoSetTarget(DOOR_UNLOCK_ANGLE_DEG);
            g_door_state = DOOR_STATE_UNLOCKING;
            break;
        }
        /* 到达目标角度 → 进入闭锁态 */
        if (g_servo_angle == g_servo_target && g_servo_target == DOOR_LOCK_ANGLE_DEG) {
            DOOR_PRINTF("locked");
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
    DOOR_PRINTF("SLE Wireless Door Access System (v2-fixed)");
    DOOR_PRINTF("  servo: GPIO%d  btn: GPIO%d  LED: GPIO%d  PIR: ADC%d",
                DOOR_SERVO_GPIO, DOOR_BUTTON_GPIO, DOOR_LED_GPIO,
                DOOR_PIR_ADC_CHANNEL);
    DOOR_PRINTF("  PIR debounce: %d samples, cooldown: %d ms",
                PIR_IDLE_CONFIRM_SAMPLES, PIR_COOLDOWN_AFTER_MOTION_MS);
    DOOR_PRINTF("  auto-lock: %d s, servo hold refresh: %d loops",
                AUTO_LOCK_TIMEOUT_MS / 1000, SERVO_HOLD_REFRESH_LOOPS);
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

    /* 初始状态 */
    g_door_state = DOOR_STATE_LOCKED;
    g_servo_angle = 0;
    g_servo_target = 0;
    DOOR_PRINTF("state: LOCKED, angle: 0");

    /* ---- 主循环 ---- */
    DOOR_PRINTF("main loop (%ums)", (unsigned)MAIN_LOOP_INTERVAL_MS);
    uint32_t elapsed = MAIN_LOOP_INTERVAL_MS;

    for (;;) {
        /* 1. 采样 PIR（带防抖 + 冷却期） */
        DoorPirSample(elapsed);

        /* 2. 读按键（消抖后） */
        uint8_t btn = (DoorButtonRead() == 0) ? 1 : 0;

        /* 3. 门禁状态机 */
        DoorControl(elapsed, btn);

        /* 4. 舵机运动控制（发送连续 PWM 或保持脉冲） */
        ServoMoveStep();

        /* 5. LED 状态指示 */
        DoorLedUpdate(elapsed);

        /* 6. 喂狗 */
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
