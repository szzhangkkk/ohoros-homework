#ifndef STATU_H
#define STATU_H

#include <stdio.h>
#include <stdint.h>

typedef struct {
    uint8_t light_bright;   // LED亮度 (0-9)
    uint8_t ac_on;          // 电机/空调开关 (0/1)
    uint8_t curtain_open;   // 窗帘/舵机状态 (0=关/1=开)
} HomeStatus;

extern HomeStatus g_home_status;

void home_status_init(void);

#endif
