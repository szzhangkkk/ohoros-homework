#include <stdio.h>              
#include "ohos_init.h"          
#include "cmsis_os2.h"          
#include "iot_gpio.h"           
#include "iot_watchdog.h"       // 看门狗接口

#define LED_GPIO 10             
#define LED_TASK_STACK_SIZE 0x1000   // 设置线程栈大小
#define LED_TASK_PRIO 25             // 线程优先级

#define LED_ON_TIME_MS  500     
#define LED_OFF_TIME_MS 500     

//该宏用于将毫秒转换为tick
#define MS_TO_TICK(ms) ((ms) / 10)

// LED任务函数
static void GpioLedTask(void *arg)
{
    (void)arg;  
    //初始化GPIO
    IoTGpioInit(LED_GPIO);
    IoTGpioSetDir(LED_GPIO, IOT_GPIO_DIR_OUT); // 设置为输出模式

    while (1) {
        /* 低电平点亮 LED，500ms */
        IoTGpioSetOutputVal(LED_GPIO, IOT_GPIO_VALUE0);
        osDelay(MS_TO_TICK(LED_ON_TIME_MS));   // 延时500ms

        /* 高电平点亮 LED，500ms */
        IoTGpioSetOutputVal(LED_GPIO, IOT_GPIO_VALUE1);
        osDelay(MS_TO_TICK(LED_OFF_TIME_MS));  // 延时500ms

        /* 喂狗 */
        IoTWatchDogKick();
    }
}

static void GpioLedEntry(void)
{
    /* 定义线程属性 */
    osThreadAttr_t attr = {
        "GpioLedTask", 0, NULL, 0, NULL, LED_TASK_STACK_SIZE, LED_TASK_PRIO, 0, 0
    };

    /* 创建线程 */
    if (osThreadNew(GpioLedTask, NULL, &attr) == NULL) {
        printf("[GpioLed] Failed to create GpioLedTask!\r\n");
    }
}

/* 应用初始化宏：系统启动时自动调用 */
APP_FEATURE_INIT(GpioLedEntry);