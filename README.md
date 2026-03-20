# STM32H750 RGB LCD + LVGL + WiFi + Wave Control

This repository contains an STM32H750-based embedded application with:

- FreeRTOS task scheduling
- LVGL 8.4 user interface
- 800x480 RGB LCD display
- GT911 touch input
- UART4-based WiFi interaction
- Waveform control UI reserved for an external generator backend

## Current waveform status

The waveform UI path is still present, but the old on-chip DAC waveform generation logic has been removed.

Current `APP/Wave.c` behavior:

- keeps the waveform type selection interface
- keeps the frequency and Vpp text input parsing
- keeps the `CHANGE` button handling
- keeps the `DACStartTask` task entry so the UI flow is unchanged
- leaves a placeholder function for future AD9833 output logic

What is not active now:

- no DAC DMA waveform streaming
- no TIM6-based waveform sample scheduling inside `APP/Wave.c`
- no DMA half/full refill logic
- no waveform LUT/NCO/DDS generation path

Important note:

- CubeMX-generated DAC, DMA, and timer initialization code may still exist in the project
- the related task and public interfaces are intentionally preserved for the later AD9833 migration
- the AD9833 hardware output logic has not been implemented yet

## Main files

- `APP/Wave.c`
  - waveform UI state handling
  - waveform parameter parsing
  - future hardware backend placeholder
- `APP/Wave.h`
  - public waveform interface
- `APP/wifi.c`
  - WiFi task logic and UART interaction
- `Core/Src/main.c`
  - system startup and task creation

## Build environment

- MCU: `STM32H750XBH6`
- IDE/project: `MDK-ARM/STM32H750XBH6.uvprojx`
- Frameworks: STM32 HAL, CMSIS, FreeRTOS, LVGL 8.4

## Development note

If you continue the waveform migration, the intended next step is to implement the AD9833 backend inside `APP/Wave.c` by filling in the placeholder hardware output function, while keeping the existing LVGL-facing interfaces unchanged.
