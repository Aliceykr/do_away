#include "Wave.h"

#include "FreeRTOS.h"
#include "portable.h"
#include "ui.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

extern DAC_HandleTypeDef hdac1;
extern TIM_HandleTypeDef htim6;
extern osMutexId_t mutex_id;

#define WAVE_DEFAULT_OUTPUT_FREQ_HZ      1000U
#define WAVE_DEFAULT_OUTPUT_VPP_MV       3000U
#define WAVE_MIN_OUTPUT_FREQ_HZ          10U
#define WAVE_MAX_OUTPUT_FREQ_HZ          100000U

#define WAVE_MAX_OUTPUT_MV               3000U
#define WAVE_DAC_VREF_MV                 3300U
#define WAVE_DAC_MAX_CODE                4095U
#define WAVE_TIMER_MAX_COUNT             65536U
#define WAVE_PI_F                        3.14159265358979323846f

#define WAVE_TYPE_COUNT                  3U
#define WAVE_LUT_BITS                    10U
#define WAVE_LUT_SIZE                    (1U << WAVE_LUT_BITS)
#define WAVE_LUT_INDEX_MASK              (WAVE_LUT_SIZE - 1U)
#define WAVE_LUT_FRAC_BITS               16U
#define WAVE_LUT_MAX_VALUE               65535U

/*
 * Rebuild one full cycle in task context and let DMA loop over it.
 * Keep the DAC trigger rate bounded, then adapt samples-per-cycle by frequency.
 */
#define WAVE_DAC_SAMPLE_RATE_MAX_HZ      2000000U
#define WAVE_DAC_IDLE_SAMPLE_RATE_HZ     1000U
#define WAVE_PERIOD_SAMPLES_MIN          16U
#define WAVE_PERIOD_SAMPLES_MAX          1024U
#define WAVE_CACHE_ALIGN_BYTES           32U

#define WAVE_UI_POLL_PERIOD_MS           10U
#define WAVE_UI_STARTUP_DELAY_MS         200U

#define WAVE_FREQ_TEXT_BUFFER_SIZE       16U
#define WAVE_VPP_TEXT_BUFFER_SIZE        16U

static uint16_t wave_shape_lut[WAVE_TYPE_COUNT][WAVE_LUT_SIZE];

static void *wave_dma_buffer_raw = NULL;
static uint16_t *wave_dma_buffer = NULL;

static volatile WaveType_t wave_requested_type = WAVE_TYPE_NONE;
static WaveType_t wave_active_type = WAVE_TYPE_NONE;

static volatile uint32_t wave_requested_freq_hz = WAVE_DEFAULT_OUTPUT_FREQ_HZ;
static volatile uint32_t wave_requested_vpp_mv = WAVE_DEFAULT_OUTPUT_VPP_MV;
static uint32_t wave_active_freq_hz = 0U;
static uint32_t wave_active_vpp_mv = 0U;

static uint32_t wave_output_peak_code = 0U;
static uint32_t wave_period_sample_rate_hz = 0U;
static uint16_t wave_period_sample_count = 0U;

static volatile uint8_t wave_output_update_pending = 1U;
static uint8_t wave_output_started = 0U;

static volatile uint8_t wave_ui_apply_request = 0U;
static volatile uint8_t wave_dma_half_flag = 0U;
static volatile uint8_t wave_dma_full_flag = 0U;

static lv_obj_t *wave_bound_change_button = NULL;
static lv_obj_t *wave_bound_freq_textarea = NULL;
static lv_obj_t *wave_bound_vpp_textarea = NULL;

typedef struct
{
    uint32_t sample_rate_hz;
    uint32_t prescaler_div;
    uint32_t period_counts;
    uint16_t sample_count;
} WaveOutputPlan_t;

static uintptr_t Wave_AlignUp(uintptr_t value, uintptr_t alignment)
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

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

static void Wave_CleanDCacheRange(const void *addr, size_t size_bytes)
{
    uintptr_t start_addr;
    uintptr_t end_addr;

    if ((addr == NULL) || (size_bytes == 0U)) {
        return;
    }

    start_addr = ((uintptr_t)addr) & ~(uintptr_t)(WAVE_CACHE_ALIGN_BYTES - 1U);
    end_addr = (((uintptr_t)addr) + size_bytes + WAVE_CACHE_ALIGN_BYTES - 1U) &
               ~(uintptr_t)(WAVE_CACHE_ALIGN_BYTES - 1U);

    SCB_CleanDCache_by_Addr((uint32_t *)start_addr, (int32_t)(end_addr - start_addr));
}

static uint16_t Wave_ConvertMvToDacCode(uint32_t output_mv)
{
    if (output_mv >= WAVE_DAC_VREF_MV) {
        return (uint16_t)WAVE_DAC_MAX_CODE;
    }

    return (uint16_t)(((uint64_t)output_mv * WAVE_DAC_MAX_CODE + (WAVE_DAC_VREF_MV / 2U)) /
                      WAVE_DAC_VREF_MV);
}

static void Wave_RequestOutputUpdate(void)
{
    wave_output_update_pending = 1U;
}

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

static void Wave_BuildSineLut(uint16_t *table)
{
    for (uint32_t i = 0U; i < WAVE_LUT_SIZE; ++i) {
        const float phase = (2.0f * WAVE_PI_F * (float)i) / (float)WAVE_LUT_SIZE;
        const float normalized = (sinf(phase) + 1.0f) * 0.5f;
        table[i] = (uint16_t)(normalized * (float)WAVE_LUT_MAX_VALUE + 0.5f);
    }
}

static void Wave_BuildSquareLut(uint16_t *table)
{
    const uint32_t half_cycle = WAVE_LUT_SIZE / 2U;

    for (uint32_t i = 0U; i < WAVE_LUT_SIZE; ++i) {
        table[i] = (i < half_cycle) ? (uint16_t)WAVE_LUT_MAX_VALUE : 0U;
    }
}

static void Wave_BuildTriangleLut(uint16_t *table)
{
    const uint32_t half_cycle = WAVE_LUT_SIZE / 2U;

    for (uint32_t i = 0U; i < WAVE_LUT_SIZE; ++i) {
        uint32_t sample;

        if (i < half_cycle) {
            sample = ((uint64_t)i * WAVE_LUT_MAX_VALUE + (half_cycle - 1U) / 2U) /
                     (half_cycle - 1U);
        } else {
            sample = ((uint64_t)(WAVE_LUT_SIZE - 1U - i) * WAVE_LUT_MAX_VALUE +
                      (half_cycle - 1U) / 2U) /
                     (half_cycle - 1U);
        }

        table[i] = (uint16_t)sample;
    }
}

static void Wave_InitBaseLut(void)
{
    Wave_BuildSineLut(wave_shape_lut[WAVE_TYPE_SINE]);
    Wave_BuildTriangleLut(wave_shape_lut[WAVE_TYPE_TRIANGLE]);
    Wave_BuildSquareLut(wave_shape_lut[WAVE_TYPE_SQUARE]);
}

static uint16_t Wave_GetNormalizedSampleFromType(WaveType_t wave_type, uint32_t phase_accumulator)
{
    uint32_t index;

    if (wave_type == WAVE_TYPE_NONE) {
        return 0U;
    }

    if ((uint32_t)wave_type >= WAVE_TYPE_COUNT) {
        wave_type = WAVE_TYPE_SINE;
    }

    index = phase_accumulator >> (32U - WAVE_LUT_BITS);

    if (wave_type == WAVE_TYPE_SQUARE) {
        return wave_shape_lut[(uint32_t)wave_type][index & WAVE_LUT_INDEX_MASK];
    }

    {
        const uint32_t next_index = (index + 1U) & WAVE_LUT_INDEX_MASK;
        const uint32_t frac_shift = 32U - WAVE_LUT_BITS - WAVE_LUT_FRAC_BITS;
        const uint32_t frac = (phase_accumulator >> frac_shift) & 0xFFFFU;
        const int32_t sample_a =
            (int32_t)wave_shape_lut[(uint32_t)wave_type][index & WAVE_LUT_INDEX_MASK];
        const int32_t sample_b = (int32_t)wave_shape_lut[(uint32_t)wave_type][next_index];
        const int32_t interpolated =
            sample_a + (int32_t)(((sample_b - sample_a) * (int32_t)frac + 0x8000L) >> 16);

        return (uint16_t)interpolated;
    }
}

static HAL_StatusTypeDef Wave_AllocateDmaBuffer(void)
{
    const size_t buffer_bytes = WAVE_PERIOD_SAMPLES_MAX * sizeof(uint16_t);
    const size_t alloc_bytes = buffer_bytes + WAVE_CACHE_ALIGN_BYTES - 1U;
    uintptr_t aligned_addr;

    if (wave_dma_buffer != NULL) {
        return HAL_OK;
    }

    wave_dma_buffer_raw = pvPortMalloc(alloc_bytes);
    if (wave_dma_buffer_raw == NULL) {
        return HAL_ERROR;
    }

    aligned_addr = Wave_AlignUp((uintptr_t)wave_dma_buffer_raw, WAVE_CACHE_ALIGN_BYTES);
    wave_dma_buffer = (uint16_t *)aligned_addr;

    return HAL_OK;
}

static uint32_t Wave_GetTim6ClockHz(void)
{
    RCC_ClkInitTypeDef clk_init = {0};
    uint32_t flash_latency = 0U;
    uint32_t pclk1_hz = HAL_RCC_GetPCLK1Freq();

    HAL_RCC_GetClockConfig(&clk_init, &flash_latency);

    if (clk_init.APB1CLKDivider == RCC_APB1_DIV1) {
        return pclk1_hz;
    }

    return (pclk1_hz * 2U);
}

static HAL_StatusTypeDef Wave_ComputeTim6Counts(uint32_t timer_clk_hz,
                                                uint32_t sample_rate_hz,
                                                uint32_t *prescaler_div,
                                                uint32_t *period_counts)
{
    uint64_t prescaler_denominator;
    uint64_t denominator;

    if ((sample_rate_hz == 0U) || (prescaler_div == NULL) || (period_counts == NULL)) {
        return HAL_ERROR;
    }

    prescaler_denominator = (uint64_t)WAVE_TIMER_MAX_COUNT * (uint64_t)sample_rate_hz;
    *prescaler_div = (uint32_t)(((uint64_t)timer_clk_hz + prescaler_denominator - 1ULL) /
                                prescaler_denominator);
    if ((*prescaler_div == 0U) || (*prescaler_div > WAVE_TIMER_MAX_COUNT)) {
        return HAL_ERROR;
    }

    denominator = (uint64_t)sample_rate_hz * (uint64_t)(*prescaler_div);
    *period_counts = (uint32_t)(((uint64_t)timer_clk_hz + (denominator / 2ULL)) / denominator);
    if ((*period_counts == 0U) || (*period_counts > WAVE_TIMER_MAX_COUNT)) {
        return HAL_ERROR;
    }

    return HAL_OK;
}

static HAL_StatusTypeDef Wave_ConfigTim6ForOutputPlan(const WaveOutputPlan_t *output_plan)
{
    TIM_MasterConfigTypeDef master_config = {0};

    if ((output_plan == NULL) || (output_plan->prescaler_div == 0U) ||
        (output_plan->period_counts == 0U)) {
        return HAL_ERROR;
    }

    htim6.Init.Prescaler = output_plan->prescaler_div - 1U;
    htim6.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim6.Init.Period = output_plan->period_counts - 1U;
    htim6.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

    if (HAL_TIM_Base_Init(&htim6) != HAL_OK) {
        return HAL_ERROR;
    }

    master_config.MasterOutputTrigger = TIM_TRGO_UPDATE;
    master_config.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;

    if (HAL_TIMEx_MasterConfigSynchronization(&htim6, &master_config) != HAL_OK) {
        return HAL_ERROR;
    }

    return HAL_OK;
}

static void Wave_DisableRegularDmaInterrupts(void)
{
    if (hdac1.DMA_Handle1 == NULL) {
        return;
    }

    __HAL_DMA_DISABLE_IT(hdac1.DMA_Handle1, DMA_IT_HT);
    __HAL_DMA_DISABLE_IT(hdac1.DMA_Handle1, DMA_IT_TC);
}

static void Wave_StopOutput(void)
{
    (void)HAL_TIM_Base_Stop(&htim6);
    (void)HAL_DAC_Stop_DMA(&hdac1, DAC_CHANNEL_1);
}

static HAL_StatusTypeDef Wave_BuildOutputPlan(WaveType_t wave_type,
                                              uint32_t output_freq_hz,
                                              WaveOutputPlan_t *output_plan)
{
    uint32_t timer_clk_hz;

    if (output_plan == NULL) {
        return HAL_ERROR;
    }

    timer_clk_hz = Wave_GetTim6ClockHz();

    if (wave_type == WAVE_TYPE_NONE) {
        uint64_t actual_divisor;

        output_plan->sample_count = WAVE_PERIOD_SAMPLES_MIN;
        output_plan->sample_rate_hz = WAVE_DAC_IDLE_SAMPLE_RATE_HZ;
        if (Wave_ComputeTim6Counts(timer_clk_hz,
                                   output_plan->sample_rate_hz,
                                   &output_plan->prescaler_div,
                                   &output_plan->period_counts) != HAL_OK) {
            return HAL_ERROR;
        }

        actual_divisor =
            (uint64_t)output_plan->prescaler_div * (uint64_t)output_plan->period_counts;
        output_plan->sample_rate_hz =
            (uint32_t)(((uint64_t)timer_clk_hz + (actual_divisor / 2ULL)) / actual_divisor);

        return HAL_OK;
    }

    if (output_freq_hz < WAVE_MIN_OUTPUT_FREQ_HZ) {
        output_freq_hz = WAVE_MIN_OUTPUT_FREQ_HZ;
    } else if (output_freq_hz > WAVE_MAX_OUTPUT_FREQ_HZ) {
        output_freq_hz = WAVE_MAX_OUTPUT_FREQ_HZ;
    }

    {
        uint8_t best_found = 0U;
        uint64_t best_error_numerator = 0U;
        uint64_t best_error_denominator = 1U;

        for (uint32_t candidate_samples = WAVE_PERIOD_SAMPLES_MIN;
             candidate_samples <= WAVE_PERIOD_SAMPLES_MAX;
             ++candidate_samples) {
            WaveOutputPlan_t candidate_plan;
            uint32_t candidate_target_sample_rate_hz;
            uint64_t candidate_divisor;
            uint64_t candidate_denominator;
            uint64_t target_scaled;
            uint64_t candidate_error_numerator;

            candidate_target_sample_rate_hz = output_freq_hz * candidate_samples;
            if ((candidate_target_sample_rate_hz == 0U) ||
                (candidate_target_sample_rate_hz > WAVE_DAC_SAMPLE_RATE_MAX_HZ)) {
                continue;
            }

            if (Wave_ComputeTim6Counts(timer_clk_hz,
                                       candidate_target_sample_rate_hz,
                                       &candidate_plan.prescaler_div,
                                       &candidate_plan.period_counts) != HAL_OK) {
                continue;
            }

            candidate_plan.sample_count = (uint16_t)candidate_samples;
            candidate_divisor =
                (uint64_t)candidate_plan.prescaler_div * (uint64_t)candidate_plan.period_counts;
            candidate_plan.sample_rate_hz =
                (uint32_t)(((uint64_t)timer_clk_hz + (candidate_divisor / 2ULL)) /
                           candidate_divisor);
            candidate_denominator = candidate_divisor * (uint64_t)candidate_plan.sample_count;
            target_scaled = (uint64_t)output_freq_hz * candidate_denominator;

            if (target_scaled >= (uint64_t)timer_clk_hz) {
                candidate_error_numerator = target_scaled - (uint64_t)timer_clk_hz;
            } else {
                candidate_error_numerator = (uint64_t)timer_clk_hz - target_scaled;
            }

            if ((best_found == 0U) ||
                (candidate_error_numerator * best_error_denominator <
                 best_error_numerator * candidate_denominator) ||
                ((candidate_error_numerator * best_error_denominator ==
                  best_error_numerator * candidate_denominator) &&
                 (candidate_plan.sample_count > output_plan->sample_count))) {
                *output_plan = candidate_plan;
                best_error_numerator = candidate_error_numerator;
                best_error_denominator = candidate_denominator;
                best_found = 1U;
            }
        }

        if (best_found == 0U) {
            return HAL_ERROR;
        }
    }

    return HAL_OK;
}

static HAL_StatusTypeDef Wave_BuildPeriodBuffer(WaveType_t wave_type,
                                                uint32_t output_vpp_mv,
                                                uint16_t sample_count)
{
    const uint16_t peak_code = Wave_ConvertMvToDacCode(output_vpp_mv);

    if ((wave_dma_buffer == NULL) || (sample_count == 0U) ||
        (sample_count > WAVE_PERIOD_SAMPLES_MAX)) {
        return HAL_ERROR;
    }

    for (uint32_t i = 0U; i < sample_count; ++i) {
        const uint32_t phase_accumulator =
            (uint32_t)(((uint64_t)i << 32) / (uint64_t)sample_count);
        const uint16_t normalized_sample =
            Wave_GetNormalizedSampleFromType(wave_type, phase_accumulator);

        wave_dma_buffer[i] = (uint16_t)(((uint32_t)normalized_sample * peak_code +
                                         (WAVE_LUT_MAX_VALUE / 2U)) /
                                        WAVE_LUT_MAX_VALUE);
    }

    Wave_CleanDCacheRange(wave_dma_buffer, (size_t)sample_count * sizeof(uint16_t));
    wave_output_peak_code = peak_code;

    return HAL_OK;
}

static void Wave_ProcessPendingOutputUpdate(void)
{
    WaveType_t requested_type;
    uint32_t requested_freq_hz;
    uint32_t requested_vpp_mv;
    WaveOutputPlan_t output_plan = {0};

    requested_type = wave_requested_type;
    requested_freq_hz = wave_requested_freq_hz;
    requested_vpp_mv = wave_requested_vpp_mv;

    if ((wave_output_started != 0U) &&
        (wave_output_update_pending == 0U) &&
        (requested_type == wave_active_type) &&
        (requested_freq_hz == wave_active_freq_hz) &&
        (requested_vpp_mv == wave_active_vpp_mv)) {
        return;
    }

    wave_output_update_pending = 0U;

    if (Wave_BuildOutputPlan(requested_type, requested_freq_hz, &output_plan) != HAL_OK) {
        Error_Handler();
    }

    Wave_StopOutput();

    if (Wave_BuildPeriodBuffer(requested_type, requested_vpp_mv, output_plan.sample_count) != HAL_OK) {
        Error_Handler();
    }

    if (Wave_ConfigTim6ForOutputPlan(&output_plan) != HAL_OK) {
        Error_Handler();
    }

    if (HAL_DAC_Start_DMA(&hdac1,
                          DAC_CHANNEL_1,
                          (uint32_t *)wave_dma_buffer,
                          output_plan.sample_count,
                          DAC_ALIGN_12B_R) != HAL_OK) {
        Error_Handler();
    }

    Wave_DisableRegularDmaInterrupts();

    if (HAL_TIM_Base_Start(&htim6) != HAL_OK) {
        Error_Handler();
    }

    wave_active_type = requested_type;
    wave_active_freq_hz = requested_freq_hz;
    wave_active_vpp_mv = requested_vpp_mv;
    wave_period_sample_rate_hz = output_plan.sample_rate_hz;
    wave_period_sample_count = output_plan.sample_count;
    wave_output_started = 1U;
    wave_dma_half_flag = 0U;
    wave_dma_full_flag = 0U;
}

static void Wave_ClearDmaTransferFlags(void)
{
    if ((wave_dma_half_flag != 0U) || (wave_dma_full_flag != 0U)) {
        wave_dma_half_flag = 0U;
        wave_dma_full_flag = 0U;
    }
}

static void Wave_StartOutput(void)
{
    Wave_InitBaseLut();

    if (Wave_AllocateDmaBuffer() != HAL_OK) {
        Error_Handler();
    }

    wave_requested_type = WAVE_TYPE_NONE;
    wave_active_type = WAVE_TYPE_NONE;
    wave_requested_freq_hz = WAVE_DEFAULT_OUTPUT_FREQ_HZ;
    wave_requested_vpp_mv = WAVE_DEFAULT_OUTPUT_VPP_MV;
    wave_active_freq_hz = 0U;
    wave_active_vpp_mv = 0U;
    wave_output_peak_code = 0U;
    wave_period_sample_rate_hz = 0U;
    wave_period_sample_count = 0U;
    wave_output_started = 0U;
    wave_output_update_pending = 1U;
    wave_dma_half_flag = 0U;
    wave_dma_full_flag = 0U;

    Wave_ProcessPendingOutputUpdate();
}

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

static void Wave_PollUiSelection(void)
{
    WaveType_t selected_type;

    if (Wave_ReadTypeFromUi(&selected_type) != 0U) {
        Wave_SetWaveType(selected_type);
    }
}

static void Wave_ChangeButtonEvent(lv_event_t *e)
{
    if ((e != NULL) && (lv_event_get_code(e) == LV_EVENT_CLICKED)) {
        wave_ui_apply_request = 1U;
    }
}

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

WaveType_t Wave_GetWaveType(void)
{
    return wave_requested_type;
}

void HAL_DAC_ConvHalfCpltCallbackCh1(DAC_HandleTypeDef *hdac)
{
    if ((hdac != NULL) && (hdac->Instance == DAC1)) {
        wave_dma_half_flag = 1U;
    }
}

void HAL_DAC_ConvCpltCallbackCh1(DAC_HandleTypeDef *hdac)
{
    if ((hdac != NULL) && (hdac->Instance == DAC1)) {
        wave_dma_full_flag = 1U;
    }
}

void HAL_DAC_ErrorCallbackCh1(DAC_HandleTypeDef *hdac)
{
    (void)hdac;
    Error_Handler();
}

void HAL_DAC_DMAUnderrunCallbackCh1(DAC_HandleTypeDef *hdac)
{
    (void)hdac;
    Error_Handler();
}

void DACStartTask(void *argument)
{
    (void)argument;

    Wave_StartOutput();

    osDelay(WAVE_UI_STARTUP_DELAY_MS);

    for (;;) {
        Wave_BindUiControlsIfNeeded();
        Wave_PollUiSelection();
        Wave_ProcessUiApplyRequest();
        Wave_ProcessPendingOutputUpdate();
        Wave_ClearDmaTransferFlags();
        osDelay(WAVE_UI_POLL_PERIOD_MS);
    }
}
