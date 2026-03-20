# LVGL 低帧率排查与最小优化记录

## 当前发现的主要原因
- `lvgl-8.4.0/examples/porting/lv_port_disp.c` 使用 `disp_drv.full_refresh = 1`，导致每一帧全屏重绘（800x480），CPU 负载高。
- `lvgl-8.4.0/lv_conf.h` 中 `LV_USE_GPU_STM32_DMA2D` 关闭，绘制和拷贝主要靠 CPU。
- `Core/Src/main.c` 中每一轮循环都调用 `Touch_Scan()`，而触摸为软件 I2C，延时较多，拉长帧周期。
- LVGL 缓冲区在外部 SDRAM（`LV_MEM_ADR` / `LVGL_MemoryAdd`），带宽较慢。

## 最简单、最小风险的性能提升
**降低 `Touch_Scan()` 调用频率**。

### 修改示例（仅改 `Core/Src/main.c`）
```c
// StartDefaultTask 内部
static uint32_t last_touch_tick = 0;

for (;;) {
    osMutexAcquire(mutex_id, osWaitForever);
    lv_task_handler();
    osMutexRelease(mutex_id);

    uint32_t now_tick = osKernelGetTickCount();

    // 触摸扫描降频到 30ms~50ms
    if ((now_tick - last_touch_tick) >= 30U) {
        last_touch_tick = now_tick;
        Touch_Scan();
    }

    osDelay(LV_DISP_DEF_REFR_PERIOD);
}
```

## 后续可选优化方向
1. 关闭 `full_refresh`，改为局部刷新（只刷变动区域）。
2. 启用 DMA2D 加速（`LV_USE_GPU_STM32_DMA2D = 1`）并在 `disp_flush` 中使用 DMA2D。
3. 减少大面积动画/阴影/渐变等高开销效果。

----
记录时间：2026-03-13
