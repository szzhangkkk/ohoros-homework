#include "device_pwm.h"
#include <stdint.h>

// 声明uapi_pwm_clear_group函数（用于清除PWM组状态）
extern uint32_t uapi_pwm_clear_group(uint8_t group);

#define SERVO_MIN_US 650
#define SERVO_MAX_US 2350
#define SERVO_PERIOD_US 20000
#define SERVO_CYCLES 8

/**
 * 舵机GPIO初始化
 */
void servo_gpio_init(void)
{
    IoTGpioInit(PWM_GPIO);
    IoTGpioSetDir(PWM_GPIO, IOT_GPIO_DIR_OUT);
    osal_printk("[servo] GPIO%d initialized for software PWM\\n", PWM_GPIO);
}

/**
 * 软件PWM控制舵机
 * @param angle 舵机角度 0-180度
 */
void servo_software_pwm(uint8_t angle)
{
    if (angle > 180) angle = 180;

    uint16_t compare = SERVO_MIN_US +
        (angle * (SERVO_MAX_US - SERVO_MIN_US)) / 180;

    for (int i = 0; i < 8; i++) {
        IoTGpioSetOutputVal(PWM_GPIO, IOT_GPIO_VALUE1);
        osal_udelay(compare);
        IoTGpioSetOutputVal(PWM_GPIO, IOT_GPIO_VALUE0);
        osal_udelay(SERVO_PERIOD_US - compare);
    }
}

/**
 * LED PWM初始化
 */
void led_pwm_init(void)
{
    IoTGpioInit(LED_GPIO);
    uint32_t ret = IoSetFunc(LED_GPIO, IOT_IO_FUNC_GPIO_11_PWM3_OUT);
    osal_printk("[LED_PWM] IoSetFunc(GPIO%d, PWM3_OUT) ret=%u\\n", LED_GPIO, ret);
    
    ret = IoTPwmInit(LED_PWM_PORT);
    osal_printk("[LED_PWM] IoTPwmInit(PORT%d) ret=%u\\n", LED_PWM_PORT, ret);
    
    led_set_brightness(0);
    osal_printk("[LED_PWM] LED PWM initialized on GPIO%d, PORT%d\\n", LED_GPIO, LED_PWM_PORT);
}

/**
 * 设置LED亮度
 * @param brightness 亮度值 0-9
 */
void led_set_brightness(uint8_t brightness)
{
    if (brightness > LED_BRIGHTNESS_MAX) {
        brightness = LED_BRIGHTNESS_MAX;
    }
    
    uint8_t duty;
    
    if (brightness == 0) {
        IoTPwmStop(LED_PWM_PORT);
        uapi_pwm_clear_group(LED_PWM_PORT);
        osal_printk("[LED_PWM] Brightness=0, PWM cleared\\n");
        return;
    }
    
    duty = (brightness * 100) / 9;
    if (duty < 1) duty = 1;
    if (duty > 99) duty = 99;
    
    uapi_pwm_clear_group(LED_PWM_PORT);
    unsigned int ret = IoTPwmStart(LED_PWM_PORT, duty, LED_PWM_FREQ);
    if (ret == 0) {
        osal_printk("[LED_PWM] Brightness=%d, Duty=%d%%, Freq=%dHz\\n", brightness, duty, LED_PWM_FREQ);
    } else {
        osal_printk("[LED_PWM] IoTPwmStart failed: ret=%u\\n", ret);
    }
}

/**
 * 电机GPIO初始化
 */
void motor_gpio_init(void)
{
    IoTGpioInit(MOTOR_GPIO);
    IoTGpioSetDir(MOTOR_GPIO, IOT_GPIO_DIR_OUT);
    IoTGpioSetOutputVal(MOTOR_GPIO, IOT_GPIO_VALUE1);  // 默认关闭（高电平关）
    osal_printk("[MOTOR] GPIO%d initialized\\n", MOTOR_GPIO);
}

/**
 * 设置电机状态
 * @param on 0=关闭, 1=开启
 */
void motor_set_state(uint8_t on)
{
    if (on) {
        IoTGpioSetOutputVal(MOTOR_GPIO, IOT_GPIO_VALUE0);  // 低电平开
        osal_printk("[MOTOR] ON\\n");
    } else {
        IoTGpioSetOutputVal(MOTOR_GPIO, IOT_GPIO_VALUE1);  // 高电平关
        osal_printk("[MOTOR] OFF\\n");
    }
}
