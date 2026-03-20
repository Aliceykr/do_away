#include "panelchange.h"

#include "../My_LVGL/ui.h"

#include <stdint.h>
#include <string.h>

#define PANEL_ANIM_TIME_MS 350U

typedef enum {
    PANEL_PHASE_IDLE = 0,
    PANEL_PHASE_HIDING,
    PANEL_PHASE_SHOWING
} PanelPhase_t;

typedef struct {
    lv_obj_t *panel;
    lv_coord_t hidden_x;
    lv_coord_t shown_x;
} PanelSlot_t;

static PanelSlot_t s_panels[PANEL_ID_MAX];
static PanelId_t s_active_panel = PANEL_ID_NONE;
static PanelId_t s_requested_panel = PANEL_ID_NONE;
static PanelId_t s_transition_target = PANEL_ID_NONE;
static PanelPhase_t s_phase = PANEL_PHASE_IDLE;

static bool panel_is_valid_id(PanelId_t id)
{
    return (id > PANEL_ID_NONE) && (id < PANEL_ID_MAX);
}

static bool panel_is_registered(PanelId_t id)
{
    return panel_is_valid_id(id) && (s_panels[id].panel != NULL);
}

static void panel_try_process(void);

static void panel_anim_ready_cb(lv_anim_t *a)
{
    (void)a;

    if (s_phase == PANEL_PHASE_HIDING) {
        s_active_panel = PANEL_ID_NONE;
        s_phase = PANEL_PHASE_IDLE;
        panel_try_process();
        return;
    }

    if (s_phase == PANEL_PHASE_SHOWING) {
        s_active_panel = s_transition_target;
        s_phase = PANEL_PHASE_IDLE;

        if (s_requested_panel != s_active_panel) {
            panel_try_process();
        }
    }
}

static void panel_start_anim(PanelId_t id, lv_coord_t target_x)
{
    PanelSlot_t *slot;
    lv_coord_t from_x;
    lv_anim_t anim;

    if (!panel_is_registered(id)) {
        return;
    }

    slot = &s_panels[id];
    from_x = lv_obj_get_x(slot->panel);

    if (from_x == target_x) {
        panel_anim_ready_cb(NULL);
        return;
    }

    lv_anim_del(slot->panel, (lv_anim_exec_xcb_t)lv_obj_set_x);

    lv_anim_init(&anim);
    lv_anim_set_var(&anim, slot->panel);
    lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_values(&anim, from_x, target_x);
    lv_anim_set_time(&anim, PANEL_ANIM_TIME_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_set_ready_cb(&anim, panel_anim_ready_cb);
    lv_anim_start(&anim);
}

static void panel_try_process(void)
{
    if (s_phase != PANEL_PHASE_IDLE) {
        return;
    }

    if (s_requested_panel == s_active_panel) {
        return;
    }

    if (s_active_panel != PANEL_ID_NONE) {
        s_phase = PANEL_PHASE_HIDING;
        panel_start_anim(s_active_panel, s_panels[s_active_panel].hidden_x);
        return;
    }

    if (s_requested_panel != PANEL_ID_NONE) {
        if (!panel_is_registered(s_requested_panel)) {
            s_requested_panel = PANEL_ID_NONE;
            return;
        }

        s_transition_target = s_requested_panel;
        lv_obj_move_foreground(s_panels[s_transition_target].panel);
        s_phase = PANEL_PHASE_SHOWING;
        panel_start_anim(s_transition_target, s_panels[s_transition_target].shown_x);
    }
}

static void panel_open_button_cb(lv_event_t *e)
{
    PanelId_t target = (PanelId_t)(uintptr_t)lv_event_get_user_data(e);
    PanelChange_Request(target);
}

static void panel_close_button_cb(lv_event_t *e)
{
    (void)e;
    PanelChange_CloseActive();
}

void PanelChange_Init(void)
{
    memset(s_panels, 0, sizeof(s_panels));
    s_active_panel = PANEL_ID_NONE;
    s_requested_panel = PANEL_ID_NONE;
    s_transition_target = PANEL_ID_NONE;
    s_phase = PANEL_PHASE_IDLE;

    /* Register built-in panels here, then bind their open/close buttons. */
    PanelChange_RegisterPanel(PANEL_ID_WIFI, ui_WifiPanel);
    PanelChange_RegisterPanel(PANEL_ID_WAVE, ui_wavePanel);

    PanelChange_BindOpenButton(ui_wifiButton, ui_event_wifiButton, PANEL_ID_WIFI);
    PanelChange_BindOpenButton(ui_waveButton, ui_event_waveButton, PANEL_ID_WAVE);
    PanelChange_BindCloseButton(ui_backbutton, ui_event_backbutton);
    PanelChange_BindCloseButton(ui_backbutton2, ui_event_backbutton2);
}

bool PanelChange_RegisterPanel(PanelId_t id, lv_obj_t *panel)
{
    if (!panel_is_valid_id(id) || (panel == NULL)) {
        return false;
    }

    s_panels[id].panel = panel;
    /* The SquareLine initial x position is treated as the hidden position. */
    s_panels[id].hidden_x = lv_obj_get_x(panel);
    s_panels[id].shown_x = 0;

    if (lv_obj_get_x(panel) == s_panels[id].shown_x) {
        s_active_panel = id;
        s_requested_panel = id;
    }

    return true;
}

bool PanelChange_BindOpenButton(lv_obj_t *button, lv_event_cb_t old_cb, PanelId_t target)
{
    if ((button == NULL) || !panel_is_valid_id(target)) {
        return false;
    }

    if (old_cb != NULL) {
        lv_obj_remove_event_cb(button, old_cb);
    }

    lv_obj_add_event_cb(button, panel_open_button_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)target);
    return true;
}

bool PanelChange_BindCloseButton(lv_obj_t *button, lv_event_cb_t old_cb)
{
    if (button == NULL) {
        return false;
    }

    if (old_cb != NULL) {
        lv_obj_remove_event_cb(button, old_cb);
    }

    lv_obj_add_event_cb(button, panel_close_button_cb, LV_EVENT_CLICKED, NULL);
    return true;
}

void PanelChange_Request(PanelId_t target)
{
    if ((target != PANEL_ID_NONE) && !panel_is_registered(target)) {
        return;
    }

    s_requested_panel = target;
    panel_try_process();
}

void PanelChange_CloseActive(void)
{
    PanelChange_Request(PANEL_ID_NONE);
}

PanelId_t PanelChange_GetActive(void)
{
    return s_active_panel;
}

bool PanelChange_IsBusy(void)
{
    return s_phase != PANEL_PHASE_IDLE;
}
