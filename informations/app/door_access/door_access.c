/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * door_access.c — SLE 星闪无线简易门禁系统 主应用
 *
 * 舵机控制参考 servo.c: ServoSetAngle() 直接发 N 个脉冲到目标角度，
 * 不逐步走角度，舵机内部自行平滑转动。完整流程为阻塞式 open-wait-close。
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

/* ======================== 全局 ======================== */

static volatile uint8_t g_remote_cmd = DOOR_REMOTE_CMD_NONE;

static uint8_t g_btn_stable       = 1;
static uint8_t g_btn_last_raw     = 1;
static uint8_t g_btn_debounce_cnt = 0;

static uint32_t g_pir_last_ms  = 0;
static uint8_t  g_pir_motion   = 0;
static uint8_t  g_pir_idle_cnt = 0;

static uint32_t g_led_toggle_ms = 0;
static uint8_t  g_led_on        = 0;

#define DOOR_PRINTF(fmt, args...) printf(DOOR_LOG " " fmt "\r\n", ##args)

/* ======================== 舵机 ======================== */

static uint32_t ServoAngleToPulseUs(unsigned int angle)
{
    if (angle > 180) angle = 180;
    return SERVO_PULSE_MIN_US + (angle * SERVO_PULSE_RANGE_US) / 180;
}

/*
 * 输出一个 50Hz PWM 周期（20ms）
 */
static void ServoSendOnePeriod(uint32_t pulse_us)
{
    if (pulse_us > SERVO_PWM_PERIOD_US) pulse_us = SERVO_PWM_PERIOD_US;
    IoTGpioSetOutputVal(DOOR_SERVO_GPIO, IOT_GPIO_VALUE1);
    (void)uapi_tcxo_delay_us(pulse_us);
    IoTGpioSetOutputVal(DOOR_SERVO_GPIO, IOT_GPIO_VALUE0);
    (void)uapi_tcxo_delay_us(SERVO_PWM_PERIOD_US - pulse_us);
}

/*
 * 参考 servo.c: ServoSetAngle
 * 直接发 TRAVEL_CYCLES 个脉冲到目标角度，阻塞式。
 * 舵机收到连续 50Hz 信号 → 平滑转动到目标 → 不抽搐。
 */
static void ServoSetAngle(unsigned int angle)
{
    if (angle > 180) angle = 180;
    uint32_t us = ServoAngleToPulseUs(angle);

    for (unsigned int i = 0; i < SERVO_TRAVEL_CYCLES; i++) {
        ServoSendOnePeriod(us);
    }

    DOOR_PRINTF("servo -> %u deg, pulse=%u us", angle, us);
}

/*
 * 参考 servo.c: ServoDispenseOnce
 * 开锁 → 保持 2s → 闭锁，阻塞式完成一次开关门。
 */
static void DoorDoOpenClose(void)
{
    DOOR_PRINTF("opening");
    ServoSetAngle(DOOR_UNLOCK_ANGLE_DEG);

    DOOR_PRINTF("hold %ums", UNLOCK_HOLD_MS);
    osDelay(MS_TO_TICKS(UNLOCK_HOLD_MS));

    DOOR_PRINTF("closing");
    ServoSetAngle(DOOR_LOCK_ANGLE_DEG);

    DOOR_PRINTF("done");
}

/* ======================== 按键 ======================== */

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

static void DoorLedUpdate(uint32_t ms, door_state_t st)
{
    g_led_toggle_ms += ms;
    uint8_t sle = sle_uart_client_is_connected() ? 1 : 0;

    switch (st) {
    case DOOR_STATE_LOCKED:
        if (sle) DoorLedSet(0);
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
        if (sle) DoorLedSet(1);
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

/* ======================== PIR ======================== */

static uint8_t DoorPirSample(uint32_t ms)
{
    g_pir_last_ms += ms;
    if (g_pir_last_ms < PIR_SAMPLE_INTERVAL_MS) return g_pir_motion;
    g_pir_last_ms = 0;

    uint16_t mv = 0;
    if (adc_port_read(DOOR_PIR_ADC_CHANNEL, &mv) != ERRCODE_SUCC) return g_pir_motion;

    if (mv >= ADC_HUMAN_MOTION_GE_MV) {
        if (!g_pir_motion) DOOR_PRINTF("PIR: %umV → MOTION", mv);
        g_pir_idle_cnt = 0;
        g_pir_motion = 1;
    } else if (mv <= ADC_HUMAN_IDLE_LE_MV) {
        g_pir_idle_cnt++;
        if (g_pir_idle_cnt >= PIR_IDLE_CONFIRM_SAMPLES) {
            if (g_pir_motion) DOOR_PRINTF("PIR: %umV → IDLE", mv);
            g_pir_motion = 0;
            g_pir_idle_cnt = PIR_IDLE_CONFIRM_SAMPLES;
        }
    }
    return g_pir_motion;
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
    DOOR_PRINTF("  PIR+BTN or SLE → open → hold %ds → close",
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

    /* 舵机归零 */
    ServoSetAngle(0);

    DOOR_PRINTF("ready, waiting for trigger");

    /* 主循环 */
    for (;;) {
        uint8_t pir = DoorPirSample(MAIN_LOOP_INTERVAL_MS);
        uint8_t cmd = g_remote_cmd;

        /* SLE 开锁 */
        if (cmd == DOOR_CMD_UNLOCK) {
            DOOR_PRINTF("SLE trigger → open");
            g_remote_cmd = DOOR_REMOTE_CMD_NONE;
            DoorLedUpdate(0, DOOR_STATE_UNLOCKING);
            DoorDoOpenClose();
            DoorLedUpdate(0, DOOR_STATE_LOCKED);
            continue;
        }

        /* SLE 闭锁 */
        if (cmd == DOOR_CMD_LOCK) {
            DOOR_PRINTF("SLE LOCK → close");
            g_remote_cmd = DOOR_REMOTE_CMD_NONE;
            ServoSetAngle(DOOR_LOCK_ANGLE_DEG);
            continue;
        }

        /* PIR 检测到人 → 开门 */
        if (pir) {
            DOOR_PRINTF("PIR trigger → open");
            DoorLedUpdate(0, DOOR_STATE_UNLOCKING);
            DoorDoOpenClose();
            DoorLedUpdate(0, DOOR_STATE_LOCKED);
            continue;
        }

        DoorLedUpdate(MAIN_LOOP_INTERVAL_MS, DOOR_STATE_LOCKED);
        IoTWatchDogKick();
        osDelay(MS_TO_TICKS(MAIN_LOOP_INTERVAL_MS));
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
