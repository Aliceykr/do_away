/**
 * @file wifi.h
 * @brief WiFi连接任务头文件
 */

#ifndef __WIFI_H
#define __WIFI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "cmsis_os.h"
#include "lvgl.h"

/**
 * @brief WiFi凭证结构体
 * @details 用于在UI线程和WiFi任务之间传递SSID和密码
 */
typedef struct {
    char ssid[64];      // WiFi名称，最大63字符+结束符
    char password[64];  // WiFi密码，最大63字符+结束符
} WifiCredentials_t;

/**
 * @brief WiFi消息队列句柄
 * @details 用于传递WiFi凭证从UI到WiFi任务
 */
extern osMessageQueueId_t wifiQueueHandle;

/**
 * @brief WiFi接收消息队列句柄
 * @details 用于传递接收数据从中断到接收任务
 */
extern osMessageQueueId_t wifiRxQueueHandle;

/* UART接收缓冲区（外部可访问） */
#define WIFI_RX_BUF_SIZE 256
extern uint8_t wifiRxBuf[WIFI_RX_BUF_SIZE];
extern uint16_t wifiRxLen;
extern volatile uint32_t uart4_rx_isr_count;
extern volatile uint32_t wifi_rx_task_count; //123123123

/**
 * @brief WiFi任务入口函数
 * @param argument 任务参数（未使用）
 * @details 等待消息队列中的WiFi凭证，收到后通过UART4发送连接命令
 */
void Wifi_StartTask(void *argument);

/**
 * @brief WiFi接收任务入口函数
 * @param argument 任务参数（未使用）
 * @details 等待接收消息队列，处理WiFi模块返回的数据并更新UI
 */
void Wifi_RxStartTask(void *argument);

/**
 * @brief Connect按钮事件处理函数
 * @param e LVGL事件对象
 * @details 按下Connect按钮时，读取输入框内容并发送到消息队列
 */
void ui_event_ConnectButton(lv_event_t * e);
void Wifi_BindUiEvents(void);

/**
 * @brief WiFi模块初始化
 * @details 创建消息队列、启动UART4接收、创建接收任务
 */
void Wifi_Init(void);

/**
 * @brief UART4接收回调函数
 * @details 当接收到一帧数据时调用，将数据发送到接收队列
 */
void UART4_ReceiveCallback(uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif
