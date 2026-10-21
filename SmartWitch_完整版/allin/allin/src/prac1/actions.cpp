#include "vars.h"
#include "actions.h"
#include <math.h>
#include "ui.h"
#include "screens.h"

extern uint16_t touch_x;

int16_t touchx_begin;
int16_t touchx_end;
int16_t cscreen;


void action_touch_switch_page(lv_event_t *e) {
    
   lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
         touchx_begin = (int16_t)touch_x;
    }
    else if (code == LV_EVENT_RELEASED) { 
        touchx_end = (int16_t)touch_x;
        Serial.printf("touch:(%d,%d)\n",touchx_begin,touchx_end);
        if(fabs(touchx_end-touchx_begin) > 50)
        {
            cscreen = eez_flow_get_current_screen();
            if (touchx_end < touchx_begin){
                cscreen = cscreen==_SCREEN_ID_LAST ?_SCREEN_ID_FIRST : cscreen+1;
                eez_flow_set_screen(cscreen, LV_SCR_LOAD_ANIM_OVER_LEFT, 200, 0);
            }
            else{
                cscreen = cscreen==_SCREEN_ID_FIRST ?_SCREEN_ID_LAST : cscreen-1;
                eez_flow_set_screen(cscreen, LV_SCR_LOAD_ANIM_OVER_RIGHT, 200, 0);
            }
            touchx_begin = 0 , touchx_end = 0;
        }
  
    }    
}

