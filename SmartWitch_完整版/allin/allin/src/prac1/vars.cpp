#include <string.h>
#include "vars.h"
#include "ui.h"

int32_t main_power = 100;
int32_t main_step  = 1234;
// 初始为 0 表示"尚未测量"，真实数值由 Task_Heart 写入（避免界面显示假数据）
int32_t health_heartbeat = 0;
int32_t health_oxygen = 0;
float sport_distance = 2.16;
float sport_time = 10;
float sport_pace = 4.63;
int32_t lightness = 119;
float environmrnt_humidity = 0.36;
float environment_temperature = 0.66;
float environment_altitude = 89;
int32_t today_min_temperature = 0;
int32_t today_max_temperature = 15;
char weather_text[10] = { 0 };
char travel_suggestion[50] = { 0 };
int32_t sport_cal =  0 ;
char lightness_text[50] = { 0 };

int32_t environmrnt_currenttemperature = 2;


float get_var_lightness() {
    return lightness;
}

void set_var_lightness(float value) {
    
    lightness = value;

}
int32_t get_var_sport_pace() {
    return sport_pace;
}

void set_var_sport_pace(int32_t value) {
    sport_pace = value;
}

int32_t get_var_sport_time() {
    return sport_time;
}

void set_var_sport_time(int32_t value) {
    sport_time = value;
}

float get_var_sport_distance() {
    return sport_distance;
}

void set_var_sport_distance(float value) {
    sport_distance = value;
}

int32_t get_var_main_power()
{
    return main_power;
}
void set_var_main_power(int32_t value)
{
    main_power = value;
}

int32_t get_var_main_step()
{
    return main_step;
}
void set_var_main_step(int32_t value)
{
    main_step = value;
}


int32_t get_var_health_heartbeat() {
    return health_heartbeat;
}

void set_var_health_heartbeat(int32_t value) {
    health_heartbeat = value;
}

int32_t get_var_health_oxygen() {
    return health_oxygen;
}

void set_var_health_oxygen(int32_t value) {
    health_oxygen = value;
}

char native_date[30] = { 0 };

const char *get_var_native_date() {
    return native_date;
}

void set_var_native_date(const char *value) {
    strncpy(native_date, value, sizeof(native_date) / sizeof(char));
    native_date[sizeof(native_date) / sizeof(char) - 1] = 0;
}

char native_time[10] = { 0 };

const char *get_var_native_time() {
    return native_time;
}

void set_var_native_time(const char *value) {
    strncpy(native_time, value, sizeof(native_time) / sizeof(char));
    native_time[sizeof(native_time) / sizeof(char) - 1] = 0;
}

char native_second[5] = { 0 };

const char *get_var_native_second() {
    return native_second;
}

void set_var_native_second(const char *value) {
    strncpy(native_second, value, sizeof(native_second) / sizeof(char));
    native_second[sizeof(native_second) / sizeof(char) - 1] = 0;
}

int32_t get_var_environmrnt_humidity() {
    return environmrnt_humidity;
}

void set_var_environmrnt_humidity(int32_t value) {
    environmrnt_humidity = value;
}

int32_t get_var_environment_temperature() {
    return environment_temperature;
}

void set_var_environment_temperature(int32_t value) {
    environment_temperature = value;
}

int32_t get_var_environment_altitude() {
    return environment_altitude;
}

void set_var_environment_altitude(int32_t value) {
    environment_altitude = value;
}

int32_t get_var_today_min_temperature() {
    return today_min_temperature;
}

void set_var_today_min_temperature(int32_t value) {
    today_min_temperature = value;
}

int32_t get_var_today_max_temperature() {
    return today_max_temperature;
}

void set_var_today_max_temperature(int32_t value) {
    today_max_temperature = value;
}

const char *get_var_weather_text() {
    return weather_text;
}

void set_var_weather_text(const char *value) {
    strncpy(weather_text, value, sizeof(weather_text) / sizeof(char));
    weather_text[sizeof(weather_text) / sizeof(char) - 1] = 0;
}

const char *get_var_travel_suggestion() {
    return travel_suggestion;
}

void set_var_travel_suggestion(const char *value) {
    strncpy(travel_suggestion, value, sizeof(travel_suggestion) / sizeof(char));
    travel_suggestion[sizeof(travel_suggestion) / sizeof(char) - 1] = 0;
}



int32_t get_var_sport_cal() {
    return sport_cal;
}

void set_var_sport_cal(int32_t value) {
    sport_cal = value;
}

const char *get_var_lightness_text() {
    return lightness_text;
}

void set_var_lightness_text(const char *value) {
    strncpy(lightness_text, value, sizeof(lightness_text) / sizeof(char));
    lightness_text[sizeof(lightness_text) / sizeof(char) - 1] = 0;
}

int32_t get_var_environmrnt_currenttemperature() {
    return environmrnt_currenttemperature;
}

void set_var_environmrnt_currenttemperature(int32_t value) {
    environmrnt_currenttemperature = value;
}
