/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思 WS63E 芯片。
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * 【进阶实验】舵机角度控制：将角度（0～180°）转换为脉宽
 *
 * 背景：
 *   - 舵机要求周期 20ms（50Hz），高电平脉宽 0.5～2.5ms 表示角度。
 *   - WS63 硬件 PWM 最低约 1230Hz，无法直接输出 50Hz。
 *   - 本程序用 GPIO 手动拉高/拉低，配合 uapi_tcxo_delay_us() 构造脉宽。
 *
 * 行为：
 *   - 使用 ServoSetAngle(angle_deg) 函数，按角度直接计算脉宽。
 *   - 脉宽公式：pulse_us = 500 + angle_deg × 2000 / 180
 *   - 依次转到 0° → 18° → 36° → … → 180°，每档约 300ms，再反向返回。
 */

#include <stdio.h>

#include "ohos_init.h"
#include "cmsis_os2.h"
#include "iot_gpio.h"
#include "iot_watchdog.h"
#include "tcxo.h"

/* ======================== 引脚与任务配置 ======================== */

/* 舵机信号线（黄线）接排针 GPIO10 */
#define SERVO_GPIO              10
/* 舵机任务栈大小（字节） */
#define SERVO_TASK_STACK_SIZE   0x1000
/* 任务优先级，与 gpio_led 等示例同级 */
#define SERVO_TASK_PRIO         25

/* ======================== 舵机 PWM 时序（50Hz） ======================== */

/*
 * 一个 PWM 周期的总时长（微秒）。
 * 50Hz → T = 1/50 s = 20ms = 20000us
 */
#define SERVO_PWM_PERIOD_US     20000

/* 舵机标准脉宽端点（单位：微秒） */
#define SERVO_PULSE_MIN_US      500     /* 0°   对应 0.5ms 高电平 */
#define SERVO_PULSE_MAX_US      2500    /* 180° 对应 2.5ms */
#define SERVO_PULSE_RANGE_US    (SERVO_PULSE_MAX_US - SERVO_PULSE_MIN_US)

/* 每个角度保持的 PWM 周期数（15个周期 ≈ 300ms） */
#define SERVO_HOLD_CYCLES       15

/*
 * LiteOS 系统 tick 频率为 100Hz，即 1 tick = 10ms。
 * osDelay(n) 的参数是 tick 数，不是毫秒；500ms 应写 MS_TO_TICKS(500)=50。
 */
#define TICK_PER_SEC            100
#define MS_TO_TICKS(ms)         (((ms) * TICK_PER_SEC) / 1000)

/* ======================== 底层：单周期输出 ======================== */

/*
 * ServoSendPulseUs — 输出一个完整的 50Hz PWM 周期
 *
 * @param pulse_us  本周期高电平宽度（0.5ms～2.5ms 范围内有效）
 *
 * 时序：先拉高 pulse_us → 再拉低 (20ms - pulse_us) → 周期结束。
 * 舵机根据「高电平有多宽」判断目标角度，低电平段仅为填充周期。
 */
static void ServoSendPulseUs(uint32_t pulse_us)
{
    if (pulse_us > SERVO_PWM_PERIOD_US) {
        pulse_us = SERVO_PWM_PERIOD_US;
    }

    IoTGpioSetOutputVal(SERVO_GPIO, IOT_GPIO_VALUE1);
    (void)uapi_tcxo_delay_us(pulse_us);

    IoTGpioSetOutputVal(SERVO_GPIO, IOT_GPIO_VALUE0);
    (void)uapi_tcxo_delay_us(SERVO_PWM_PERIOD_US - pulse_us);
}

/* ======================== 新增函数 ServoSetAngle(unsigned int angle_deg) ======================== */

/*
 * ServoAngleToPulseUs — 角度到脉宽的线性换算
 *
 * @param angle_deg  角度（0～180°）
 * @return           高电平宽度（us）
 *
 * 脉宽公式：pulse_us = 500 + angle_deg × 2000 / 180
 * 当 angle_deg=0   时，pulse_us = 500us（0.5ms）
 * 当 angle_deg=90  时，pulse_us = 1500us（1.5ms）
 * 当 angle_deg=180 时，pulse_us = 2500us（2.5ms）
 */
//将角度值线性转换为对应的PWM脉宽
static uint32_t ServoAngleToPulseUs(unsigned int angle_deg)
{
    if (angle_deg > 180) {
        angle_deg = 180;
    }
    return SERVO_PULSE_MIN_US + (angle_deg * SERVO_PULSE_RANGE_US) / 180;
}

/*
 * ServoSetAngle — 设置舵机到指定角度
 *
 * @param angle_deg  目标角度（0～180°，超出范围会被钳位）
 *
 * 在该角度下连续输出 SERVO_HOLD_CYCLES 个完整 PWM 周期（约 300ms），然后返回。
 * 每调用一次，舵机转到指定角度并稳定后返回。
 */
static void ServoSetAngle(unsigned int angle_deg)
{
    /* 钳位角度到有效范围 */
    if (angle_deg > 180) {
        angle_deg = 180;
    }

    /* 计算对应脉宽 */
    uint32_t pulse_us = ServoAngleToPulseUs(angle_deg);

    /* 连续输出多个 PWM 周期，确保舵机稳定到位 */
    for (unsigned int i = 0; i < SERVO_HOLD_CYCLES; i++) {
        ServoSendPulseUs(pulse_us);
    }

    /* 打印角度和脉宽信息 */
    printf("[pwm_servo] angle=%3u deg, pulse=%4u us [202400203058]\r\n", angle_deg, pulse_us);
}

/* ======================== 任务入口 ======================== */

static void ServoTask(void *arg)
{
    (void)arg;

    /* 初始化 GPIO10 为输出模式 */
    IoTGpioInit(SERVO_GPIO);
    IoTGpioSetDir(SERVO_GPIO, IOT_GPIO_DIR_OUT);
    IoTGpioSetOutputVal(SERVO_GPIO, IOT_GPIO_VALUE0);  /* 空闲低电平 */

    printf("[pwm_servo] 50Hz software PWM on GPIO%d, hold %d cycles/step\r\n",
           SERVO_GPIO, SERVO_HOLD_CYCLES);
    printf("[pwm_servo] pulse: %uus(0deg) ~ %uus(180deg)\r\n",
           SERVO_PULSE_MIN_US, SERVO_PULSE_MAX_US);

    while (1) {//支持任意角度值
        /* 正向扫描：0° → 18° → 36° → … → 180°（步进 18°） */
        for (unsigned int angle = 0; angle <= 180; angle += 18) {
            ServoSetAngle(angle);
            IoTWatchDogKick();
        }

        osDelay(MS_TO_TICKS(500));   /* 两端扫描之间稍作停顿 */

        /* 反向扫描：180° → 162° → … → 0°（步进 -18°） */
        for (unsigned int angle = 180; angle > 0; angle -= 18) {
            ServoSetAngle(angle);
            IoTWatchDogKick();
        }
        ServoSetAngle(0);  /* 回到 0° */
        IoTWatchDogKick();

        osDelay(MS_TO_TICKS(500));   /* 完成一轮扫描后停顿 */

        IoTWatchDogKick();
    }
}

/* 创建舵机控制任务 */
static void ServoEntry(void)
{
    osThreadAttr_t attr = {
        "ServoTask", 0, NULL, 0, NULL, SERVO_TASK_STACK_SIZE, SERVO_TASK_PRIO, 0, 0
    };

    if (osThreadNew(ServoTask, NULL, &attr) == NULL) {
        printf("[pwm_servo] Failed to create ServoTask!\r\n");
    }
}

APP_FEATURE_INIT(ServoEntry);
