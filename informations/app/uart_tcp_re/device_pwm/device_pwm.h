#ifndef DEVICE_PWM_H
#define DEVICE_PWM_H

#include <stdio.h>
#include <unistd.h>
#include "cmsis_os2.h"
#include "ohos_init.h"
#include "iot_gpio.h"
#include "soc_osal.h"
#include "pinctrl.h"
#include "osal_debug.h"
#include "../iot_gpio_ex.h"
#include "iot_pwm.h"

// 舵机 PWM 配置 (GPIO10)
#define PWM_GPIO 10
#define SERVO_ANGLE_OPEN    180
#define SERVO_ANGLE_CLOSE   0

// LED PWM 配置 (GPIO11/PWM3)
#define LED_GPIO 11
#define LED_PWM_PORT 3
#define LED_PWM_FREQ 1230
#define LED_BRIGHTNESS_MAX 9
#define LED_BRIGHTNESS_MIN 0

// 电机 GPIO 配置
#define MOTOR_GPIO 2

// 舵机控制函数
void servo_gpio_init(void);
void servo_software_pwm(uint8_t angle);  // 软件PWM控制舵机角度 (0-180)

// LED PWM控制函数
void led_pwm_init(void);
void led_set_brightness(uint8_t brightness);  // 设置LED亮度 (0-9)

// 电机控制函数
void motor_gpio_init(void);
void motor_set_state(uint8_t on);  // 设置电机状态 (0=关, 1=开)

#endif
