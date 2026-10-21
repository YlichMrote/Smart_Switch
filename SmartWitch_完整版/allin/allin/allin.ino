#include "src/SW_FreeRTOS/SW_FreeRTOS.h"
#include "Arduino.h"

void setup() 
{
  Multitask_Init();
  
  FreeRTOS_Start();
}

void loop() 
{
  
}
