#include "Wave.h"

#include "FreeRTOS.h"
#include "ui.h"

#include <stddef.h>
#include <stdint.h>

/*
 * 波形控制模块当前只保留“UI 参数采集 + 参数状态管理 + 后端占位接口”。
 * 原来的片上 DAC / DMA / TIM6 波形生成逻辑已经移除，后续可以在
 * Wave_ApplyOutputToHardware() 中接入 AD9833 实现，而不需要改动 LVGL 页面接口。
 */
extern osMutexId_t mutex_id;

/* 默认值与输入范围约束，直接对应当前 UI 的使用范围。 */
#define WAVE_DEFAULT_OUTPUT_FREQ_HZ    1000U
#define WAVE_DEFAULT_OUTPUT_VPP_MV     3000U
#define WAVE_MIN_OUTPUT_FREQ_HZ        10U
#define WAVE_MAX_OUTPUT_FREQ_HZ        100000U

#define WAVE_MAX_OUTPUT_MV             3000U

/* 任务调度参数：控制 UI 轮询节奏和任务空闲延时。 */
#define WAVE_UI_POLL_PERIOD_MS         10U
#define WAVE_UI_STARTUP_DELAY_MS       200U
#define WAVE_TASK_IDLE_DELAY_MS        1U

/* 文本框内容的本地缓存大小。 */
#define WAVE_FREQ_TEXT_BUFFER_SIZE     16U
#define WAVE_VPP_TEXT_BUFFER_SIZE      16U

/* 用户刚刚通过 UI 请求的目标波形类型。 */
static volatile WaveType_t wave_requested_type = WAVE_TYPE_NONE;
/* 已经下发到硬件后端的波形类型快照。 */
static WaveType_t wave_active_type = WAVE_TYPE_NONE;

/* 用户通过输入框提交的目标频率和目标 Vpp。 */
static volatile uint32_t wave_requested_freq_hz = WAVE_DEFAULT_OUTPUT_FREQ_HZ;
static volatile uint32_t wave_requested_vpp_mv = WAVE_DEFAULT_OUTPUT_VPP_MV;
/* 已经应用到后端的频率和 Vpp 快照。 */
static uint32_t wave_active_freq_hz = 0U;
static uint32_t wave_active_vpp_mv = 0U;

/* 标记是否存在待应用到硬件的新参数。 */
static volatile uint8_t wave_output_update_pending = 1U;
/* 由 CHANGE 按钮事件置位，表示需要读取并应用输入框内容。 */
static volatile uint8_t wave_ui_apply_request = 0U;

/* 记录已经绑定过的 LVGL 控件，避免重复注册事件和重复设置属性。 */
static lv_obj_t *wave_bound_change_button = NULL;
static lv_obj_t *wave_bound_freq_textarea = NULL;
static lv_obj_t *wave_bound_vpp_textarea = NULL;

/* 安全复制 LVGL 返回的字符串，避免空指针和越界。 */
static void Wave_CopyString(char *dest, size_t dest_size, const char *src)
{
    size_t index = 0U;

    if ((dest == NULL) || (dest_size == 0U)) {
        return;
    }

    if (src == NULL) {
        dest[0] = '\0';
        return;
    }

    while (((index + 1U) < dest_size) && (src[index] != '\0')) {
        dest[index] = src[index];
        index++;
    }

    dest[index] = '\0';
}

/* 标记存在新的输出参数，等待主流程统一下发。 */
static void Wave_RequestOutputUpdate(void)
{
    wave_output_update_pending = 1U;
}

/* 对频率和 Vpp 做范围约束，再更新“用户请求值”。 */
static void Wave_ApplyOutputConfig(uint32_t output_freq_hz, uint32_t output_vpp_mv)
{
    if (output_freq_hz < WAVE_MIN_OUTPUT_FREQ_HZ) {
        output_freq_hz = WAVE_MIN_OUTPUT_FREQ_HZ;
    } else if (output_freq_hz > WAVE_MAX_OUTPUT_FREQ_HZ) {
        output_freq_hz = WAVE_MAX_OUTPUT_FREQ_HZ;
    }

    if (output_vpp_mv > WAVE_MAX_OUTPUT_MV) {
        output_vpp_mv = WAVE_MAX_OUTPUT_MV;
    }

    if ((wave_requested_freq_hz != output_freq_hz) || (wave_requested_vpp_mv != output_vpp_mv)) {
        wave_requested_freq_hz = output_freq_hz;
        wave_requested_vpp_mv = output_vpp_mv;
        Wave_RequestOutputUpdate();
    }
}

/*
 * 统一的硬件输出适配点。
 * 以后接 AD9833 时，建议只在这里补硬件访问逻辑，
 * 这样上层 UI 与参数处理代码都不需要再改。
 */
static void Wave_ApplyOutputToHardware(WaveType_t wave_type,
                                       uint32_t output_freq_hz,
                                       uint32_t output_vpp_mv)
{
    (void)wave_type;
    (void)output_freq_hz;
    (void)output_vpp_mv;

    /* 预留给后续 AD9833 波形输出实现。 */
}

/*
 * 把最新的请求参数同步到“已应用参数”，
 * 然后统一调用底层硬件输出接口。
 */
static void Wave_ProcessPendingOutputUpdate(void)
{
    WaveType_t requested_type;
    uint32_t requested_freq_hz;
    uint32_t requested_vpp_mv;

    requested_type = wave_requested_type;
    requested_freq_hz = wave_requested_freq_hz;
    requested_vpp_mv = wave_requested_vpp_mv;

    if ((wave_output_update_pending == 0U) &&
        (requested_type == wave_active_type) &&
        (requested_freq_hz == wave_active_freq_hz) &&
        (requested_vpp_mv == wave_active_vpp_mv)) {
        return;
    }

    wave_output_update_pending = 0U;

    wave_active_type = requested_type;
    wave_active_freq_hz = requested_freq_hz;
    wave_active_vpp_mv = requested_vpp_mv;

    Wave_ApplyOutputToHardware(requested_type, requested_freq_hz, requested_vpp_mv);
}

/* 初始化模块状态，确保任务刚启动时处于“关闭输出”的安全状态。 */
static void Wave_StartOutput(void)
{
    wave_requested_type = WAVE_TYPE_NONE;
    wave_active_type = WAVE_TYPE_NONE;
    wave_requested_freq_hz = WAVE_DEFAULT_OUTPUT_FREQ_HZ;
    wave_requested_vpp_mv = WAVE_DEFAULT_OUTPUT_VPP_MV;
    wave_active_freq_hz = 0U;
    wave_active_vpp_mv = 0U;
    wave_output_update_pending = 1U;
    wave_ui_apply_request = 0U;

    Wave_ProcessPendingOutputUpdate();
}

/* 从三个波形选择开关中读取当前选中的波形类型。 */
static uint8_t Wave_ReadTypeFromUi(WaveType_t *selected_type)
{
    uint8_t found = 0U;

    if ((selected_type == NULL) || (mutex_id == NULL)) {
        return 0U;
    }

    if ((ui_SwitchA == NULL) || (ui_SwitchB == NULL) || (ui_SwitchC == NULL)) {
        return 0U;
    }

    osMutexAcquire(mutex_id, osWaitForever);

    *selected_type = WAVE_TYPE_NONE;
    found = 1U;

    if (lv_obj_has_state(ui_SwitchA, LV_STATE_CHECKED)) {
        *selected_type = WAVE_TYPE_SINE;
    } else if (lv_obj_has_state(ui_SwitchB, LV_STATE_CHECKED)) {
        *selected_type = WAVE_TYPE_TRIANGLE;
    } else if (lv_obj_has_state(ui_SwitchC, LV_STATE_CHECKED)) {
        *selected_type = WAVE_TYPE_SQUARE;
    }

    osMutexRelease(mutex_id);

    return found;
}

/* 周期性读取开关状态，并更新目标波形类型。 */
static void Wave_PollUiSelection(void)
{
    WaveType_t selected_type;

    if (Wave_ReadTypeFromUi(&selected_type) != 0U) {
        Wave_SetWaveType(selected_type);
    }
}

/* CHANGE 按钮点击后只置位标志，真正处理放到任务上下文。 */
static void Wave_ChangeButtonEvent(lv_event_t *e)
{
    if ((e != NULL) && (lv_event_get_code(e) == LV_EVENT_CLICKED)) {
        wave_ui_apply_request = 1U;
    }
}

/*
 * 绑定波形页面控件，并设置输入框约束。
 * 这些 LVGL 接口后续会直接复用给 AD9833。
 */
static void Wave_BindUiControlsIfNeeded(void)
{
    if (mutex_id == NULL) {
        return;
    }

    if ((ui_ButtonCHANGE == NULL) || (ui_TextAreaFREQ == NULL) || (ui_TextAreaVPP == NULL)) {
        wave_bound_change_button = NULL;
        wave_bound_freq_textarea = NULL;
        wave_bound_vpp_textarea = NULL;
        return;
    }

    if ((wave_bound_change_button == ui_ButtonCHANGE) &&
        (wave_bound_freq_textarea == ui_TextAreaFREQ) &&
        (wave_bound_vpp_textarea == ui_TextAreaVPP)) {
        return;
    }

    osMutexAcquire(mutex_id, osWaitForever);

    if ((ui_ButtonCHANGE != NULL) && (wave_bound_change_button != ui_ButtonCHANGE)) {
        lv_obj_add_event_cb(ui_ButtonCHANGE, Wave_ChangeButtonEvent, LV_EVENT_CLICKED, NULL);
        wave_bound_change_button = ui_ButtonCHANGE;
    }

    if ((ui_TextAreaFREQ != NULL) && (wave_bound_freq_textarea != ui_TextAreaFREQ)) {
        lv_textarea_set_one_line(ui_TextAreaFREQ, true);
        lv_textarea_set_accepted_chars(ui_TextAreaFREQ, "0123456789");
        lv_textarea_set_max_length(ui_TextAreaFREQ, 6);
        lv_textarea_set_placeholder_text(ui_TextAreaFREQ, "10-100000");
        wave_bound_freq_textarea = ui_TextAreaFREQ;
    }

    if ((ui_TextAreaVPP != NULL) && (wave_bound_vpp_textarea != ui_TextAreaVPP)) {
        lv_textarea_set_one_line(ui_TextAreaVPP, true);
        lv_textarea_set_accepted_chars(ui_TextAreaVPP, "0123456789.");
        lv_textarea_set_max_length(ui_TextAreaVPP, 5);
        lv_textarea_set_placeholder_text(ui_TextAreaVPP, "0-3.0");
        wave_bound_vpp_textarea = ui_TextAreaVPP;
    }

    osMutexRelease(mutex_id);
}

/* 解析频率输入框，要求输入纯数字，范围为 10 Hz 到 100 kHz。 */
static uint8_t Wave_ParseFreqText(const char *text, uint32_t *freq_hz)
{
    uint32_t value = 0U;

    if ((text == NULL) || (freq_hz == NULL) || (text[0] == '\0')) {
        return 0U;
    }

    for (size_t i = 0U; text[i] != '\0'; ++i) {
        if ((text[i] < '0') || (text[i] > '9')) {
            return 0U;
        }

        value = value * 10U + (uint32_t)(text[i] - '0');
        if (value > WAVE_MAX_OUTPUT_FREQ_HZ) {
            return 0U;
        }
    }

    if ((value < WAVE_MIN_OUTPUT_FREQ_HZ) || (value > WAVE_MAX_OUTPUT_FREQ_HZ)) {
        return 0U;
    }

    *freq_hz = value;
    return 1U;
}

/* 解析 Vpp 输入框，支持形如 1、1.2、3.000 的文本输入。 */
static uint8_t Wave_ParseVppText(const char *text, uint32_t *vpp_mv)
{
    uint32_t integer_part = 0U;
    uint32_t fractional_part = 0U;
    uint32_t fractional_digits = 0U;
    uint8_t seen_digit = 0U;
    uint8_t seen_dot = 0U;

    if ((text == NULL) || (vpp_mv == NULL) || (text[0] == '\0')) {
        return 0U;
    }

    for (size_t i = 0U; text[i] != '\0'; ++i) {
        const char ch = text[i];

        if ((ch >= '0') && (ch <= '9')) {
            seen_digit = 1U;

            if (seen_dot == 0U) {
                integer_part = integer_part * 10U + (uint32_t)(ch - '0');
                if (integer_part > 3U) {
                    return 0U;
                }
            } else {
                if (fractional_digits >= 3U) {
                    return 0U;
                }

                fractional_part = fractional_part * 10U + (uint32_t)(ch - '0');
                fractional_digits++;
            }
        } else if ((ch == '.') && (seen_dot == 0U)) {
            seen_dot = 1U;
        } else {
            return 0U;
        }
    }

    if (seen_digit == 0U) {
        return 0U;
    }

    while (fractional_digits < 3U) {
        fractional_part *= 10U;
        fractional_digits++;
    }

    *vpp_mv = integer_part * 1000U + fractional_part;

    if (*vpp_mv > WAVE_MAX_OUTPUT_MV) {
        return 0U;
    }

    return 1U;
}

/* 在任务上下文中读取输入框文本，并把合法参数写回模块状态。 */
static void Wave_ProcessUiApplyRequest(void)
{
    char freq_text[WAVE_FREQ_TEXT_BUFFER_SIZE];
    char vpp_text[WAVE_VPP_TEXT_BUFFER_SIZE];
    uint32_t freq_hz = wave_requested_freq_hz;
    uint32_t vpp_mv = wave_requested_vpp_mv;
    uint8_t freq_valid = 0U;
    uint8_t vpp_valid = 0U;

    if (wave_ui_apply_request == 0U) {
        return;
    }

    wave_ui_apply_request = 0U;

    if ((mutex_id == NULL) || (ui_TextAreaFREQ == NULL) || (ui_TextAreaVPP == NULL)) {
        return;
    }

    osMutexAcquire(mutex_id, osWaitForever);
    Wave_CopyString(freq_text, sizeof(freq_text), lv_textarea_get_text(ui_TextAreaFREQ));
    Wave_CopyString(vpp_text, sizeof(vpp_text), lv_textarea_get_text(ui_TextAreaVPP));
    osMutexRelease(mutex_id);

    if (freq_text[0] != '\0') {
        freq_valid = Wave_ParseFreqText(freq_text, &freq_hz);
    }

    if (vpp_text[0] != '\0') {
        vpp_valid = Wave_ParseVppText(vpp_text, &vpp_mv);
    }

    if ((freq_valid == 0U) && (vpp_valid == 0U)) {
        return;
    }

    Wave_ApplyOutputConfig(freq_hz, vpp_mv);
}

/* 对外暴露的波形类型设置接口。 */
void Wave_SetWaveType(WaveType_t wave_type)
{
    switch (wave_type) {
    case WAVE_TYPE_SINE:
    case WAVE_TYPE_TRIANGLE:
    case WAVE_TYPE_SQUARE:
    case WAVE_TYPE_NONE:
        if (wave_requested_type != wave_type) {
            wave_requested_type = wave_type;
            Wave_RequestOutputUpdate();
        }
        break;

    default:
        if (wave_requested_type != WAVE_TYPE_NONE) {
            wave_requested_type = WAVE_TYPE_NONE;
            Wave_RequestOutputUpdate();
        }
        break;
    }
}

/* 返回当前记录的目标波形类型。 */
WaveType_t Wave_GetWaveType(void)
{
    return wave_requested_type;
}

/* 以下 DAC 回调当前保留为空实现，仅用于兼容现有工程连接点。 */
void HAL_DAC_ConvHalfCpltCallbackCh1(DAC_HandleTypeDef *hdac)
{
    (void)hdac;
}

void HAL_DAC_ConvCpltCallbackCh1(DAC_HandleTypeDef *hdac)
{
    (void)hdac;
}

void HAL_DAC_ErrorCallbackCh1(DAC_HandleTypeDef *hdac)
{
    (void)hdac;
}

void HAL_DAC_DMAUnderrunCallbackCh1(DAC_HandleTypeDef *hdac)
{
    (void)hdac;
}

/*
 * 当前任务仍然保留，目的是让现有波形页面继续正常工作：
 * - 绑定控件
 * - 读取开关
 * - 解析输入框
 * - 统一分发参数
 *
 * 真正的硬件输出逻辑留待后续 AD9833 接入时实现。
 */
void DACStartTask(void *argument)
{
    uint32_t startup_deadline;
    uint32_t next_ui_poll_tick;

    (void)argument;

    Wave_StartOutput();

    startup_deadline = osKernelGetTickCount() + WAVE_UI_STARTUP_DELAY_MS;
    while ((int32_t)(osKernelGetTickCount() - startup_deadline) < 0) {
        osDelay(WAVE_TASK_IDLE_DELAY_MS);
    }

    next_ui_poll_tick = osKernelGetTickCount();

    for (;;) {
        /* 到达轮询时机后，处理一次 UI 侧的开关和输入框。 */
        if ((int32_t)(osKernelGetTickCount() - next_ui_poll_tick) >= 0) {
            Wave_BindUiControlsIfNeeded();
            Wave_PollUiSelection();
            Wave_ProcessUiApplyRequest();
            next_ui_poll_tick = osKernelGetTickCount() + WAVE_UI_POLL_PERIOD_MS;
        }

        /* 统一把最新参数同步到硬件后端占位接口。 */
        Wave_ProcessPendingOutputUpdate();
        osDelay(WAVE_TASK_IDLE_DELAY_MS);
    }
}
