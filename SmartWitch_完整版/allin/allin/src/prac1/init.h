#ifndef _INIT_H_
#define _INIT_H_
#include <lvgl.h>


void my_disp_flush( lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p );
void my_touchpad_read( lv_indev_drv_t * indev_driver, lv_indev_data_t * data );

void screen_init(void);
//void clock_init(void);









#endif