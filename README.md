# STM32H750 RGB LCD 工程

## 工程概述

这是一个基于STM32H750XBH6嵌入式项目，集成FreeRTOS实时操作系统和LVGL GUI图形界面框架，用于驱动800x480分辨率RGB LCD显示屏和电容触摸屏。

## 硬件配置

| 外设 | 型号 | 接口 |
|------|------|------|
| 主控芯片 | STM32H750XBH6 | - |
| 显示屏 | 800x480 RGB | LTDC |
| 触摸屏 | GT911 | I2C |
| 外部存储器 | SDRAM | FMC |
| 调试串口 | USART1 | 115200 bps |
| 指示灯 | LED1 | GPIO (PC13) |

## 工程目录结构

```
do_away/
├── Core/                          # 核心应用代码
│   ├── Inc/
│   │   ├── main.h                 # 主头文件
│   │   ├── stm32h7xx_hal_conf.h   # HAL库配置
│   │   ├── stm32h7xx_it.h         # 中断头文件
│   │   └── FreeRTOSConfig.h       # FreeRTOS配置
│   └── Src/
│       ├── main.c                 # 主程序入口
│       ├── freertos.c             # FreeRTOS任务
│       ├── stm32h7xx_it.c         # 中断处理函数
│       └── stm32h7xx_hal_msp.c    # MSP初始化
│
├── Drivers/
│   ├── User/                      # 用户驱动代码
│   │   ├── Inc/
│   │   │   ├── led.h               # LED驱动
│   │   │   ├── usart.h             # 串口驱动
│   │   │   ├── sdram.h            # SDRAM驱动
│   │   │   ├── lcd_rgb.h          # LCD RGB驱动
│   │   │   ├── touch_800x480.h    # 触摸屏驱动
│   │   │   ├── touch_iic.h        # I2C触摸驱动
│   │   │   ├── lcd_fonts.h        # 字体定义
│   │   │   └── lcd_image.h        # 图片显示
│   │   └── Src/
│   │       ├── led.c
│   │       ├── usart.c
│   │       ├── sdram.c
│   │       ├── lcd_rgb.c
│   │       ├── touch_800x480.c
│   │       ├── touch_iic.c
│   │       ├── lcd_fonts.c
│   │       └── lcd_image.c
│   │
│   ├── STM32H7xx_HAL_Driver/       # STM32H7 HAL库
│   └── CMSIS/                      # ARM CMSIS库
│
├── MDK-ARM/                       # Keil MDK工程文件
│   └── STM32H750XBXBH6.uvprojx   # Keil工程文件
│
└── keilkilll.bat                  # Keil清理脚本
```

## 软件架构

### FreeRTOS任务

| 任务名称 | 堆栈大小 | 优先级 | 功能描述 |
|----------|----------|--------|----------|
| defaultTask | 2048*4 | Normal | LVGL UI主任务 |
| ledTask02 | 128*4 | Low | LED闪烁 (100ms) |
| uartTask03 | 1024*4 | Low | 串口数据处理 |

### 初始化顺序

1. MPU配置
2. CPU缓存使能 (I-Cache, D-Cache)
3. HAL库初始化
4. 系统时钟配置 (HSE -> PLL -> 400MHz)
5. GPIO初始化
6. USART1初始化 (115200波特率)
7. FMC/SDRAM初始化
8. DMA2D初始化
9. LTDC初始化 (800x480, RGB565)
10. LED初始化
11. SDRAM初始化序列
12. LCD RGB初始化
13. 触摸屏初始化
14. LVGL初始化
15. FreeRTOS调度器启动

### 核心功能

- **显示屏**: 800x480 RGB LCD，通过LTDC接口驱动，16位RGB565色彩格式
- **触摸屏**: GT911电容触摸屏，通过I2C通信，支持最多5点触控
- **外部存储**: 外部SDRAM (32位数据总线) 作为显示帧缓冲区，地址0xC0000000
- **图形界面**: LVGL (轻量级图形库)，包含显示接口和输入接口的移植
- **图形加速**: DMA2D用于2D图形操作加速

## 构建工具

- 开发环境: Keil MDK-ARM
- 目标芯片: STM32H750XBH6
- 编译器: ARM Compiler

## 注意事项

- LCD时钟频率配置为约33MHz
- SDRAM作为显示帧缓冲区 (800×480×2 = 768000字节，RGB565格式)
- LVGL任务使用互斥锁实现线程安全的渲染
- 触摸屏扫描在主循环中调用，实现连续触控检测
