/**
 * @file postoffice.c
 * @brief 通信机制源文件（信号量、消息队列等）
 */

#include "postoffice.h"

/* WiFi信号量：用于按钮触发串口发送 */
osSemaphoreId_t wifiSemaphoreHandle;

/**
 * @brief 通信机制初始化
 * @details 创建信号量、消息队列等
 */
void PostOffice_Init(void)
{
    /* 创建二值信号量，初始值为0（无信号） */
    wifiSemaphoreHandle = osSemaphoreNew(1, 0, NULL);
}
