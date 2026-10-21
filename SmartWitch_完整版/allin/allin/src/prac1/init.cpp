#include <stdint.h>
#include "init.h"
#include <Arduino.h>
#include <SPI.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <bb_captouch.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

/* 与 SW_FreeRTOS.cpp 里的是同一把互斥锁（触摸控制器和传感器共用 I2C 总线） */
extern SemaphoreHandle_t i2c_mutex;

#define TOUCH_SDA 10
#define TOUCH_SCL 11
#define TOUCH_INT -1
#define TOUCH_RST -1

BBCapTouch bbct;
const char *szNames[] = {"Unknown", "FT6x36", "GT911", "CST820"};

static const uint16_t screenWidth  = 240;
static const uint16_t screenHeight = 280;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[ screenWidth * 10 ];
TFT_eSPI tft = TFT_eSPI(screenWidth, screenHeight); /* TFT instance */

uint16_t touch_x;
uint16_t touch_y;

void my_disp_flush( lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p )
{
    uint32_t w = ( area->x2 - area->x1 + 1 );
    uint32_t h = ( area->y2 - area->y1 + 1 );
    tft.startWrite();
    tft.setAddrWindow( area->x1, area->y1, w, h );
    tft.pushColors( ( uint16_t * )&color_p->full, w * h, true );
    tft.endWrite();
    lv_disp_flush_ready( disp );
}
/*Read the touchpad*/
void my_touchpad_read( lv_indev_drv_t * indev_driver, lv_indev_data_t * data )
{
    // uint16_t touchX, touchY;
    TOUCHINFO ti;
    bool got = false;

    /* ★ 触摸控制器和五个传感器共用同一条 I2C 总线（SDA=10 / SCL=11）。
     *   Core0 的传感器任务都在 i2c_mutex 保护下访问总线，而这里跑在 Core1 的
     *   Task_Parc 上下文里 —— 必须用同一把锁串行化，否则跨核并发访问会把
     *   某一方的事务打断（表现为偶发的传感器读失败 / 触摸丢点）。
     *   锁内只有一次读事务（<1 ms），20 ms 超时足够；拿不到就当"未触摸"。 */
    if (i2c_mutex != NULL &&
        xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(20)) == pdTRUE)
    {
        got = bbct.getSamples(&ti);
        xSemaphoreGive(i2c_mutex);
    }

    if(got)
    {
        data->state = LV_INDEV_STATE_PR;
        /*Set the coordinates*/
        data->point.x = ti.x[0];
        data->point.y = ti.y[0];
        touch_x = ti.x[0];
        touch_y = ti.y[0];
        
        // Serial.print( "Data x " );
        // Serial.println( ti.x[0] );
        // Serial.print( "Data y " );
        // Serial.println( ti.y[0] );       
        

    }
    else
    {
        data->state = LV_INDEV_STATE_REL;
    }
}

void screen_init(void)
{

    String LVGL_Arduino = "Hello Arduino! ";
    LVGL_Arduino += String('V') + lv_version_major() + "." + lv_version_minor() + "." + lv_version_patch();
    Serial.println( LVGL_Arduino );
    Serial.println( "I am LVGL_Arduino" );

    lv_init();
    tft.begin();          /* TFT init */
    tft.setRotation( 0 ); /* Landscape orientation, flipped */
    bbct.init(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT);
    int iType = bbct.sensorType();
    Serial.printf("Sensor type = %s\n", szNames[iType]);
    lv_disp_draw_buf_init( &draw_buf, buf, NULL, screenWidth * 10 );
    /*Initialize the display*/
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init( &disp_drv );
    /*Change the following line to your display resolution*/
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register( &disp_drv );
    /*Initialize the (dummy) input device driver*/
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init( &indev_drv );
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register( &indev_drv );

}
/*
#include <NTPClient.h>
#include <WiFi.h>
#include <WiFiUdp.h>

const char *ssid     = "iQOO";
const char *password = "12345678";

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP,"ntp.aliyun.com",28800,86400000);

void clock_init(void)
{
    WiFi.begin(ssid, password);
    while ( WiFi.status() != WL_CONNECTED ) {
    delay ( 500 );
    Serial.print ( "." );
  }
    timeClient.begin();
}

*/
