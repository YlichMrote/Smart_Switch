#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Screens

enum ScreensEnum {
    _SCREEN_ID_FIRST = 1,
    SCREEN_ID_ENVIRONMENT = 1,
    SCREEN_ID_MAIN = 2,
    SCREEN_ID_HEALTH = 3,
    SCREEN_ID_SPORT = 4,
    SCREEN_ID_LIGHT = 5,
    _SCREEN_ID_LAST = 5
};

typedef struct _objects_t {
    lv_obj_t *environment;
    lv_obj_t *main;
    lv_obj_t *health;
    lv_obj_t *sport;
    lv_obj_t *light;
    lv_obj_t *obj0;
    lv_obj_t *obj1;
    lv_obj_t *obj2;
    lv_obj_t *obj3;
    lv_obj_t *obj4;
    lv_obj_t *obj5;
    lv_obj_t *obj6;
    lv_obj_t *obj7;
    lv_obj_t *obj8;
    lv_obj_t *obj9;
    lv_obj_t *obj10;
    lv_obj_t *obj11;
    lv_obj_t *obj12;
    lv_obj_t *obj13;
    lv_obj_t *obj14;
    lv_obj_t *obj15;
    lv_obj_t *obj16;
    lv_obj_t *obj17;
    lv_obj_t *arc_display_power;
    lv_obj_t *arc_display_step;
    lv_obj_t *arc_display_cal;
    lv_obj_t *label_power;
    lv_obj_t *label_step;
    lv_obj_t *flow_cal;
    lv_obj_t *native_date;
    lv_obj_t *native_time;
    lv_obj_t *native_second;
    lv_obj_t *obj18;
    lv_obj_t *obj19;
    lv_obj_t *obj20;
    lv_obj_t *obj21;
    lv_obj_t *obj22;
    lv_obj_t *obj23;
    lv_obj_t *obj24;
    lv_obj_t *obj25;
    lv_obj_t *obj26;
    lv_obj_t *obj27;
    lv_obj_t *obj28;
    lv_obj_t *obj29;
    lv_obj_t *obj30;
    lv_obj_t *obj31;
    lv_obj_t *obj32;
    lv_obj_t *obj33;
    lv_obj_t *obj34;
    lv_obj_t *obj35;
} objects_t;

extern objects_t objects;

void create_screen_environment();
void tick_screen_environment();

void create_screen_main();
void tick_screen_main();

void create_screen_health();
void tick_screen_health();

void create_screen_sport();
void tick_screen_sport();

void create_screen_light();
void tick_screen_light();

void create_user_widget_data_display(lv_obj_t *parent_obj, void *flowState, int startWidgetIndex);
void tick_user_widget_data_display(void *flowState, int startWidgetIndex);

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/