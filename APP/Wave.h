/**
 * @file Wave.h
 * @brief 波形控制模块对外接口
 *
 * 当前模块的职责是：
 * 1. 读取并保存 LVGL 页面上的波形选择、频率和 Vpp 参数
 * 2. 对外提供统一的波形控制接口
 * 3. 保留后续接入 AD9833 所需的软件接口
 *
 * 当前约定：
 * - SwitchA 选中：正弦波
 * - SwitchB 选中：三角波
 * - SwitchC 选中：方波
 * - 三个开关都未选中：关闭输出
 *
 * 注意：
 * - 片上 DAC 的波形生成逻辑已经移除
 * - LVGL 侧接口和任务入口仍然保留，供后续 AD9833 复用
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
 * @brief 波形控制任务入口
 * @param argument 线程参数，当前未使用
 *
 * 当前任务不再负责片上 DAC 出波，仅负责：
 * 1. 绑定并轮询 LVGL 控件
 * 2. 解析频率与 Vpp 输入
 * 3. 保存并分发最新的波形参数
 * 4. 为后续 AD9833 后端保留统一入口
 */
void DACStartTask(void *argument);

#ifdef __cplusplus
}
#endif

#endif /* __WAVE_H */
