/**
 * @file Wave.h
 * @brief 波形输出模块对外接口
 *
 * 本模块负责：
 * 1. 在 DACTas 线程中启动 TIM6 + DAC + DMA 波形输出
 * 2. 根据 UI 上的 SwitchA / SwitchB / SwitchC 自动选择波形
 * 3. 在正弦波、三角波、方波之间做平滑切换
 *
 * 当前约定：
 * - SwitchA 打开：输出正弦波
 * - SwitchB 打开：输出三角波
 * - SwitchC 打开：输出方波
 *
 * 相关业务逻辑全部收敛在 Wave.c / Wave.h 中，
 * 不需要把波形控制逻辑再写进 LVGL 生成文件。
 */

#ifndef __WAVE_H
#define __WAVE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "cmsis_os.h"

/**
 * @brief 波形类型
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
 * 该接口只是设置“希望切换到什么波形”，
 * 真正的平滑过渡由 Wave.c 内部的 DMA 流式生成逻辑完成。
 */
void Wave_SetWaveType(WaveType_t wave_type);

/**
 * @brief 获取当前目标波形类型
 * @retval 当前目标波形类型
 */
WaveType_t Wave_GetWaveType(void);

/**
 * @brief DACTas 线程入口
 * @param argument 线程参数，当前未使用
 *
 * 线程启动后会：
 * 1. 初始化三种基础波形表
 * 2. 启动 DAC DMA 输出
 * 3. 周期性读取 UI 上三个开关的状态
 * 4. 在需要时平滑切换波形
 */
void DACStartTask(void *argument);

#ifdef __cplusplus
}
#endif

#endif /* __WAVE_H */
