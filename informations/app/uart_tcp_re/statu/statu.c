#include "statu.h"

HomeStatus g_home_status = {0};

void home_status_init(void)
{
    g_home_status.light_bright = 0;
    g_home_status.ac_on = 0;
    g_home_status.curtain_open = 0;
}
