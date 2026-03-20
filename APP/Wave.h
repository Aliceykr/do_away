/**
 * @file Wave.h
 * @brief 波形控制模块对外接口
 *
 * 当前模块职责：
 * 1. 读取并保存 LVGL 页面上的波形类型、频率和 Vpp 参数
 * 2. 对外提供统一的波形控制接口
 * 3. 为后续接入 AD9833 保留稳定的软件接口
 *
 * 当前约定：
 * - SwitchA 选中：正弦波
 * - SwitchB 选中：三角波
 * - SwitchC 选中：方波
 * - 三个开关都未选中：关闭输出
 *
 * 注意：
 * - 本分支已经移除片上 DAC 出波逻辑
 * - 模块不再创建独立波形线程
 * - 上层任务需要周期性调用 Wave_Service()
 */

#ifndef __WAVE_H
#define __WAVE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "cmsis_os.h"

/**
 * @brief 波形类型定义
 */
typedef enum
{
    WAVE_TYPE_SINE = 0,
    WAVE_TYPE_TRIANGLE,
    WAVE_TYPE_SQUARE,
    WAVE_TYPE_NONE
} WaveType_t;

/**
 * @brief 设置目标波形类型
 * @param wave_type 目标波形类型
 *
 * 该接口只负责记录“用户希望切换成什么波形”，
 * 真正的硬件应用动作由 Wave.c 内部统一处理。
 */
void Wave_SetWaveType(WaveType_t wave_type);

/**
 * @brief 获取当前请求的目标波形类型
 * @retval 当前请求的目标波形类型
 */
WaveType_t Wave_GetWaveType(void);

/**
 * @brief 初始化波形控制模块
 *
 * 该接口只负责初始化模块状态，不会启动片上 DAC。
 * 当前分支中，实际硬件输出逻辑预留给后续 AD9833 接入。
 */
void Wave_Init(void);

/**
 * @brief 周期性执行波形模块服务
 *
 * 建议在上层常驻任务中周期性调用。
 * 当前主要负责：
 * 1. 绑定并轮询 LVGL 控件
 * 2. 解析频率和 Vpp 输入
 * 3. 将参数同步到硬件输出适配占位接口
 */
void Wave_Service(void);

#ifdef __cplusplus
}
#endif

#endif /* __WAVE_H */
