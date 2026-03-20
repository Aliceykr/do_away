/**
 * @file postoffice.h
 * @brief 通信机制头文件（信号量、消息队列等）
 */

#ifndef __POSTOFFICE_H
#define __POSTOFFICE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "cmsis_os.h"

/* WiFi信号量：用于按钮触发串口发送 */
extern osSemaphoreId_t wifiSemaphoreHandle;

/**
 * @brief 通信机制初始化
 * @details 创建信号量、消息队列等
 */
void PostOffice_Init(void);

#ifdef __cplusplus
}
#endif

#endif
