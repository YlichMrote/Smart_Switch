#ifndef EEZ_LVGL_UI_VARS_H
#define EEZ_LVGL_UI_VARS_H

#include <stdint.h>
#include <stdbool.h>
#include <lvgl.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif

// enum declarations

// Flow global variables

enum FlowGlobalVariables {
    FLOW_GLOBAL_VARIABLE_NONE
};

// Native global variables

extern int32_t get_var_main_power();
extern void set_var_main_power(int32_t value);
extern int32_t get_var_main_step();
extern void set_var_main_step(int32_t value);
extern const char *get_var_native_date();
extern void set_var_native_date(const char *value);
extern const char *get_var_native_time();
extern void set_var_native_time(const char *value);
extern const char *get_var_native_second();
extern void set_var_native_second(const char *value);
extern int32_t get_var_health_heartbeat();
extern void set_var_health_heartbeat(int32_t value);
extern int32_t get_var_health_oxygen();
extern void set_var_health_oxygen(int32_t value);
extern float get_var_sport_distance();
extern void set_var_sport_distance(float value);
extern int32_t get_var_sport_time();
extern void set_var_sport_time(int32_t value);
extern int32_t get_var_sport_pace();
extern void set_var_sport_pace(int32_t value);
extern float get_var_lightness();
extern void set_var_lightness(float value);
extern int32_t get_var_environmrnt_humidity();
extern void set_var_environmrnt_humidity(int32_t value);
extern int32_t get_var_environment_temperature();
extern void set_var_environment_temperature(int32_t value);
extern int32_t get_var_environment_altitude();
extern void set_var_environment_altitude(int32_t value);
extern int32_t get_var_today_min_temperature();
extern void set_var_today_min_temperature(int32_t value);
extern int32_t get_var_today_max_temperature();
extern void set_var_today_max_temperature(int32_t value);
extern const char *get_var_weather_text();
extern void set_var_weather_text(const char *value);
extern const char *get_var_travel_suggestion();
extern void set_var_travel_suggestion(const char *value);
extern int32_t get_var_sport_cal();
extern void set_var_sport_cal(int32_t value);
extern const char *get_var_lightness_text();
extern void set_var_lightness_text(const char *value);
extern int32_t get_var_environmrnt_currenttemperature();
extern void set_var_environmrnt_currenttemperature(int32_t value);

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_VARS_H*/