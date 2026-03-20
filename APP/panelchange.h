#ifndef __PANELCHANGE_H
#define __PANELCHANGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include <stdbool.h>

typedef enum {
    PANEL_ID_NONE = 0,
    PANEL_ID_WIFI,
    PANEL_ID_WAVE,
    /* Add new panel IDs here. */
    PANEL_ID_MAX
} PanelId_t;

void PanelChange_Init(void);
bool PanelChange_RegisterPanel(PanelId_t id, lv_obj_t *panel);
bool PanelChange_BindOpenButton(lv_obj_t *button, lv_event_cb_t old_cb, PanelId_t target);
bool PanelChange_BindCloseButton(lv_obj_t *button, lv_event_cb_t old_cb);
void PanelChange_Request(PanelId_t target);
void PanelChange_CloseActive(void);
PanelId_t PanelChange_GetActive(void);
bool PanelChange_IsBusy(void);

#ifdef __cplusplus
}
#endif

#endif
