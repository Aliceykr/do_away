# STM32H750 RGB LCD + LVGL + WiFi + DAC Waveform Project

基于 `STM32H750XBH6` 的嵌入式工程，集成了：

- `FreeRTOS` 多任务调度
- `LVGL 8.4` 图形界面
- `800x480 RGB LCD` 显示
- `GT911` 电容触摸
- `UART4` WiFi 交互
- `DAC1 + TIM6 + DMA` 波形输出

这个仓库更偏向一个完整的板级应用工程，而不是单一 demo。
显示、触摸、UI、串口交互和波形输出都已经接在一起，适合继续做界面联动、仪表类应用或信号源控制类项目。

## 功能概览

- 800x480 RGB LCD 显示，使用 `LTDC` 输出
- 外部 `SDRAM` 作为 LCD framebuffer
- `LVGL` 负责 UI 绘制与输入分发
- `GT911` 触摸采样
- WiFi 凭据输入、发送、状态回显
- 支持 `正弦波 / 三角波 / 方波 / 关闭输出`
- 波形频率范围按当前 UI 约束为 `10 Hz ~ 100 kHz`
- 波形幅值按当前 UI 约束为 `0 ~ 3.0 Vpp`

## 硬件与软件栈

### MCU / 板级资源

- MCU: `STM32H750XBH6`
- LCD: `800x480 RGB`
- Touch: `GT911`
- External RAM: `SDRAM` via `FMC`
- DAC output: `DAC1 Channel 1`
- Waveform trigger: `TIM6`
- WiFi link: `UART4`

### 软件栈

- STM32 HAL
- CMSIS
- FreeRTOS
- LVGL 8.4
- Keil MDK-ARM 工程
- STM32CubeMX 工程文件：`STM32H750XBH6.ioc`

## 工程结构

```text
.
├─ APP/                应用层逻辑
│  ├─ Wave.c/h         波形输出模块
│  └─ wifi.c/h         WiFi 任务与串口收发逻辑
├─ Core/               CubeMX 生成的主工程入口与中断/初始化代码
├─ Drivers/            HAL、CMSIS 与板级驱动
│  └─ User/            LCD、SDRAM、Touch、USART、LED 等用户驱动
├─ lvgl-8.4.0/         LVGL 源码与移植文件
├─ My_LVGL/            当前项目使用的 UI 代码与资源
├─ MDK-ARM/            Keil 工程、scatter file、map 文件
└─ *.md                过程记录、性能说明、问题排查文档
```

## 系统架构

### 1. 显示与 UI 架构

显示链路如下：

`LVGL -> Draw Buffer / Framebuffer -> LTDC -> RGB LCD`

关键点：

- `LVGL` 在 `defaultTask` 中驱动
- 显示显存放在外部 `SDRAM`
- `Touch_Scan()` 周期调用，把触摸输入喂给 LVGL
- LVGL 相关 API 通过互斥锁保护，避免多线程同时访问 UI

### 2. WiFi 架构

WiFi 相关逻辑位于 `APP/wifi.c`，采用“队列解耦 + UI 更新回主线程资源”的方式：

- UI 读取 SSID / Password
- 点击按钮后把凭据放入消息队列
- `wifiTask` 从队列中取出凭据并通过 `UART4` 发送连接命令
- `UART4` 接收中断按字节收包
- 收到一行后投递到接收队列
- `wifiRxTask` 解析返回内容并更新 UI 标签

这套方式把串口收发和 UI 操作分开了，结构比较清晰，也方便后续扩展 AT 命令或状态机。

### 3. 波形输出架构

当前波形模块位于 `APP/Wave.c`，采用的是：

`固定采样率 + NCO/DDS 相位累加 + DAC DMA 双半缓冲流式补数`

核心思路：

- `TIM6` 产生固定 DAC 采样触发
- `DAC1` 使用 DMA 循环输出
- DMA half/full 回调中断里只置标志位，不做重计算
- `DACTas` 任务根据标志位去补对应半区缓冲
- 每个采样点通过相位累加器推进相位，再从 LUT 中取样并插值

这样做的目的：

- 把重活从中断移到任务上下文
- 保持 DMA 中断非常轻
- 提高频率分辨率
- 避免旧方案中“整周期点数 + 定时器整数分频”带来的频率离散误差

### 4. 任务架构

当前主要 FreeRTOS 任务如下：

| 任务名 | 优先级 | 作用 |
| --- | --- | --- |
| `defaultTask` | Normal | LVGL 初始化、UI 刷新、触摸扫描、WiFi 初始化 |
| `ledTask02` | Low | LED 心跳闪烁 |
| `uartTask03` | Low | 预留串口任务 |
| `wifiTask` | Low | 发送 WiFi 连接命令 |
| `wifiRxTask` | Low | 处理 WiFi 返回数据并更新界面 |
| `DACTas` | AboveNormal | 波形输出控制、DMA 缓冲补数、参数更新 |

其中 `DACTas` 的优先级略高，是因为它需要及时处理波形缓冲续填。

## 内存布局

当前工程里几个关键内存区如下：

| 区域 | 地址 | 用途 |
| --- | --- | --- |
| SDRAM | `0xC0000000` | LCD framebuffer / LVGL 显示缓冲 |
| D2 SRAM 专用段 | `0x30040000` | 波形 DMA 流式缓冲 |
| AXI SRAM | `0x24000000` 起 | 一般 RW/ZI、堆、其余运行时数据 |

说明：

- 显示相关数据在 `SDRAM`
- 波形 DMA 缓冲单独放在 `D2 SRAM`
- 这样可以减少波形 DMA 与其他 AXI 访问的冲突

## 关键模块说明

### 波形模块

位置：

- `APP/Wave.c`
- `APP/Wave.h`

支持内容：

- 波形类型切换
- 频率与幅值更新
- DAC 启停
- NCO/DDS 相位步进生成
- DMA 双半缓冲续填

当前实现特征：

- 波形查找表预先建立
- 正弦波和三角波使用 LUT + 插值
- DMA 回调中断仅置 `half/full` 标志位
- 缓冲刷新在任务上下文完成

### WiFi 模块

位置：

- `APP/wifi.c`
- `APP/wifi.h`

支持内容：

- UI 输入读取
- WiFi 凭据消息队列
- UART4 发送连接命令
- UART4 接收回显
- 连接失败 / 成功 IP 的文本回显

### 显示与触摸模块

主要位置：

- `Drivers/User/Src/lcd_rgb.c`
- `Drivers/User/Src/sdram.c`
- `Drivers/User/Src/touch_800x480.c`
- `lvgl-8.4.0/examples/porting/`
- `My_LVGL/`

职责分层：

- `Drivers/User` 负责板级硬件驱动
- `lvgl-8.4.0/examples/porting` 负责 LVGL 显示/输入移植
- `My_LVGL` 负责页面和控件资源

## 构建与下载

### 开发环境

- Keil MDK-ARM
- ARM Compiler
- STM32CubeMX

### 构建步骤

1. 使用 Keil 打开 `MDK-ARM/STM32H750XBH6.uvprojx`
2. 选择目标 `STM32H750XBH6`
3. 编译工程
4. 使用调试器下载到板子

### 使用 CubeMX 重新生成时的注意事项

- 保留 `USER CODE` 区域内的内容
- 注意 `MDK-ARM/STM32H750XBH6/STM32H750XBH6.sct` 中的自定义内存划分
- 波形模块的 D2 SRAM 专用段不要被 CubeMX 覆盖
- UI 业务逻辑建议继续放在 `APP/`，不要把复杂逻辑直接塞进 LVGL 生成文件

## 当前设计取舍

### 为什么波形模块用 NCO/DDS

旧思路是“整周期数组循环播放”，实现简单，但频率精度容易受：

- 每周期采样点数
- `TIM6` 的整数分频
- 不同频点下的离散组合限制

当前版本改成 NCO/DDS 后：

- 长期平均频率分辨率更高
- UI 参数变化时不需要重新规划每周期点数
- 频率控制更像“相位步进”而不是“按周期切块”

但它也带来一个现实代价：

- 需要持续补 DMA 缓冲
- 对任务调度及时性更敏感
- 对总线和缓存行为更敏感

## 已知说明

- README 描述的是仓库当前代码结构，不代表最终产品说明书
- `My_LVGL/` 下的部分代码与资源可能由 UI 工具生成
- 一些根目录文档是开发过程记录，适合继续参考但不一定代表最终结论

## 相关文档

仓库根目录还保留了一些排查记录：

- `FIXES_SUMMARY.md`
- `LVGL_PERF_NOTES.md`
- `PANEL_ISSUES.md`
- `UART_RX_LVGL_ISSUE.md`

如果你要继续维护这个工程，建议先看这些文档，再看 `APP/Wave.c` 和 `APP/wifi.c`。

## 后续可扩展方向

- 波形输出加入更多波形类型
- WiFi 模块扩展为更完整的 AT 状态机
- LVGL 页面与波形参数做更紧密联动
- 给波形模块加入错误统计与 underrun 诊断
- 优化显示刷新策略与 DMA2D 加速路径

---

如果你准备把这个仓库公开到 GitHub，这份 README 可以作为主入口文档；
后续如果你愿意，我还可以继续帮你补一份 `.gitignore` 和英文版 `README_EN.md`。
