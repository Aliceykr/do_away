/**
 * @file wifi.c
 * @brief WiFi 任务与串口收发处理
 * @details 通过消息队列在线程之间传递 WiFi 凭据与串口接收数据
 */

#include "wifi.h"
#include "usart.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* 外部 UART 句柄 */
extern UART_HandleTypeDef huart4;
/* LVGL 互斥锁（在 main.c 中定义） */
extern osMutexId_t mutex_id;

/* 外部 UI 对象（在 ui_Screen1.c 中定义） */
extern lv_obj_t * ui_TextArea1;  // SSID 输入框
extern lv_obj_t * ui_TextArea2;  // 密码输入框
extern lv_obj_t * ui_ConnectButton; // Connect 按钮
extern lv_obj_t * ui_InforLabel; // 信息显示标签

/* WiFi 消息队列句柄 */
osMessageQueueId_t wifiQueueHandle = NULL;
osMessageQueueId_t wifiRxQueueHandle = NULL;

/* UART 接收缓冲区 */
#define WIFI_RX_BUF_SIZE 256
uint8_t wifiRxBuf[WIFI_RX_BUF_SIZE];
uint16_t wifiRxLen = 0;
volatile uint32_t uart4_rx_isr_count = 0;
volatile uint32_t wifi_rx_task_count = 0;

/* WiFi 接收任务句柄 */
#if 0 /* Disabled: wifiRxTask handle managed by CubeMX */
static osThreadId_t wifiRxTaskHandle = NULL;
#endif

static int wifi_extract_ipv4(const char *s, char *out, size_t out_len)
{
    const char *p = s;

    while (*p != '\0') {
        if (!isdigit((unsigned char)*p)) {
            p++;
            continue;
        }

        const char *start = p;
        int parts = 0;
        const char *q = p;

        while (parts < 4) {
            int num = 0;
            int digits = 0;
            while (isdigit((unsigned char)*q)) {
                num = (num * 10) + (*q - '0');
                if (num > 255) {
                    break;
                }
                q++;
                digits++;
            }
            if (digits == 0 || num > 255) {
                break;
            }
            parts++;
            if (parts == 4) {
                break;
            }
            if (*q != '.') {
                break;
            }
            q++;
        }

        if (parts == 4) {
            size_t len = (size_t)(q - start);
            if (len >= out_len) {
                len = out_len - 1;
            }
            memcpy(out, start, len);
            out[len] = '\0';
            return 1;
        }

        p = start + 1;
    }

    return 0;
}

static int wifi_is_fail_message(const char *s)
{
    const char *start = s;
    const char *end;
    size_t len;

    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }

    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }

    len = (size_t)(end - start);
    if (len != 4) {
        return 0;
    }

    return (toupper((unsigned char)start[0]) == 'F' &&
            toupper((unsigned char)start[1]) == 'A' &&
            toupper((unsigned char)start[2]) == 'I' &&
            toupper((unsigned char)start[3]) == 'L');
}

void Wifi_BindUiEvents(void)
{
    if (ui_ConnectButton == NULL) {
        return;
    }

    lv_obj_remove_event_cb(ui_ConnectButton, ui_event_ConnectButton);
    lv_obj_add_event_cb(ui_ConnectButton, ui_event_ConnectButton, LV_EVENT_CLICKED, NULL);
}

/**
 * @brief WiFi 模块初始化
 * @details 创建消息队列、启动 UART4 接收、创建接收任务
 */
void Wifi_Init(void)
{
    Wifi_BindUiEvents();

    /* 创建发送消息队列：深度 1，消息大小为 WifiCredentials_t */
    if (wifiQueueHandle == NULL) {
        wifiQueueHandle = osMessageQueueNew(1, sizeof(WifiCredentials_t), NULL);
    }

    /* 创建接收消息队列：深度 1，消息大小为接收缓冲区大小 */
    if (wifiRxQueueHandle == NULL) {
        wifiRxQueueHandle = osMessageQueueNew(1, WIFI_RX_BUF_SIZE, NULL);
    }

#if 0 /* Disabled: wifiRxTask will be created by CubeMX */
    osThreadAttr_t taskAttr;

    /* 创建 WiFi 接收任务 */
    taskAttr.name = "wifiRxTask";
    taskAttr.stack_size = 2048;
    taskAttr.priority = osPriorityNormal;
    taskAttr.attr_bits = 0;
    taskAttr.cb_mem = NULL;
    taskAttr.cb_size = 0;
    taskAttr.stack_mem = NULL;
    osThreadNew(Wifi_RxStartTask, NULL, &taskAttr);
#endif

    /* 启动 UART4 中断接收：一次接收 1 字节 */
    HAL_UART_Receive_IT(&huart4, &wifiRxBuf[0], 1);
}

/**
 * @brief WiFi 任务入口函数
 * @param argument 任务参数（未使用）
 * @details 等待消息队列中的 WiFi 凭据，收到后通过 UART4 发送连接命令
 */
void Wifi_StartTask(void *argument)
{
    WifiCredentials_t cred;  // WiFi 凭据缓冲区
    char msg[150];           // 发送消息缓冲区

    for (;;) {
        if (wifiQueueHandle == NULL) {
            osDelay(10);
            continue;
        }
        /* 阻塞等待消息队列，直到收到 WiFi 凭据 */
        if (osMessageQueueGet(wifiQueueHandle, &cred, NULL, osWaitForever) == osOK) {
            /* 格式化发送消息：connect_ssid_password */
            snprintf(msg, sizeof(msg), "connect_%s_%s\r\n", cred.ssid, cred.password);

            /* 通过 UART4 发送连接命令，超时 1000ms */
            HAL_UART_Transmit(&huart4, (uint8_t *)msg, strlen(msg), 1000);
        }
    }
}

/**
 * @brief Connect 按钮事件处理函数
 * @param e LVGL 事件对象
 * @details 点击 Connect 按钮时：
 *          1. 读取 SSID
 *          2. 读取密码
 *          3. 组装凭据结构体
 *          4. 发送到消息队列
 */
void ui_event_ConnectButton(lv_event_t * e)
{
    lv_event_code_t event_code = lv_event_get_code(e);

    /* 只处理点击事件 */
    if(event_code == LV_EVENT_CLICKED) {
        WifiCredentials_t cred;

        /* 从 LVGL 输入框获取文本 */
        const char* ssid = lv_textarea_get_text(ui_TextArea1);
        const char* pwd = lv_textarea_get_text(ui_TextArea2);

        /* 安全拷贝到结构体，防止缓冲区溢出 */
        strncpy(cred.ssid, ssid, sizeof(cred.ssid) - 1);
        cred.ssid[sizeof(cred.ssid) - 1] = '\0';
        strncpy(cred.password, pwd, sizeof(cred.password) - 1);
        cred.password[sizeof(cred.password) - 1] = '\0';

        /* 将 WiFi 凭据发送到消息队列（不等待） */
        osMessageQueuePut(wifiQueueHandle, &cred, 0, 0);
    }
}

/**
 * @brief UART4 接收回调函数
 * @details 每接收到一个字节调用一次，存入缓冲区；遇到换行符时将完整数据送入队列
 */
void UART4_ReceiveCallback(uint8_t *data, uint16_t len)
{
    (void)len;  // 每次接收 1 个字节
    uint8_t ch = data[0];
    uart4_rx_isr_count++;

    /* 遇到 '\n' 或缓冲满则结束本帧 */
    if (ch == '\n' || wifiRxLen >= WIFI_RX_BUF_SIZE - 1) {
        /* 追加字符串结束符 */
        wifiRxBuf[wifiRxLen] = '\0';

        /* 过滤空消息 */
        if (wifiRxLen > 0 && wifiRxQueueHandle != NULL) {
            /* 发送到接收消息队列 */
            osMessageQueuePut(wifiRxQueueHandle, wifiRxBuf, 0, 0);
        }

        /* 重置缓冲区长度 */
        wifiRxLen = 0;
    } else {
        /* 存储字符（忽略回车符） */
        if (ch != '\r') {
            wifiRxBuf[wifiRxLen++] = ch;
        }
    }

    /* 继续启动下一次接收 */
    HAL_UART_Receive_IT(&huart4, &wifiRxBuf[wifiRxLen], 1);
}

/**
 * @brief WiFi 接收任务入口函数
 * @details 等待接收消息队列，获取串口数据并更新 UI
 */
void Wifi_RxStartTask(void *argument)
{
    static uint8_t rxData[WIFI_RX_BUF_SIZE];

    for (;;) {
        if (wifiRxQueueHandle == NULL) {
            osDelay(10);
            continue;
        }
        /* 阻塞等待接收消息队列 */
        if (osMessageQueueGet(wifiRxQueueHandle, rxData, NULL, osWaitForever) == osOK) {
            wifi_rx_task_count++;
            /* 确保字符串结尾 */
            rxData[WIFI_RX_BUF_SIZE - 1] = '\0';

            /* 在同一互斥锁内调用 LVGL API */
            osMutexAcquire(mutex_id, osWaitForever);
            {
                char ip[16];
                if (wifi_is_fail_message((char *)rxData)) {
                    lv_label_set_text(ui_InforLabel, "Fail\nplease again");
                } else if (wifi_extract_ipv4((char *)rxData, ip, sizeof(ip))) {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "successful connect\n%s", ip);
                    lv_label_set_text(ui_InforLabel, msg);
                } else {
                    lv_label_set_text(ui_InforLabel, (char *)rxData);
                }
            }
            osMutexRelease(mutex_id);
        }
    }
}
