/*
 * Copyright (c) 2026 SZTU Open Source Organization.
 * 适用平台：九联星闪开发板，海思WS63E 芯片。
 *
 * Licensed under the Apache License, version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include "ohos_init.h"
#include "cmsis_os2.h"

// 线程入口函数：循环打印计数
void thread_entry(void *arg)
{
    (void)arg;  // 消除未使用参数的编译警告
    int count = 0;

    while (1) {
        count++;
        printf("Thread %d\r\n", count);  // 打印当前计数
        osDelay(500);                   // 延时500ms（osDelay单位为10ms，500*10ms=500ms）
    }
}

// 线程创建函数
static void ThreadTestTask(void)
{
    // 定义线程属性结构体
    osThreadAttr_t attr = {
        "thread_sub",    // 线程名称
        0,               // 属性位
        NULL,            // 回调函数内存
        0,               // 回调函数内存大小
        NULL,            // 栈内存
        1024,            // 栈大小（1024字节）
        osPriorityNormal,// 线程优先级（普通优先级）
        0,               // 线程预留参数
        0                // 线程核心亲和性
    };

    // 创建线程
    if (osThreadNew(thread_entry, NULL, &attr) == NULL) {
        printf("osThreadNew failed.\r\n");  // 创建失败打印日志
    }
}

// 注册线程初始化函数，系统启动时自动执行
APP_FEATURE_INIT(ThreadTestTask);



