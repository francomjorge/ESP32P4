#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>
#include <string.h>
#include "game_video.h"
// One immutable queued frame. LCD belongs exclusively to worker during a job.
// Producer never waits for LCD in the game loop; exit drains it before menu drawing.
namespace AsyncVideo {
static SemaphoreHandle_t idle=nullptr;
static TaskHandle_t task=nullptr;
static Arduino_RGB_Display *display=nullptr;
static uint8_t *snapshot=nullptr;
static uint8_t *jobPixels=nullptr;
static int jobPitch=0;
static uint16_t palette[256];
static int width,height,oldW=0,oldH=0;
static uint32_t drawUs=0,drawCount=0;
static uint16_t bg;
static GameVideo::Options options;
static void worker(void *){
  for(;;){
    ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
    uint32_t start=micros();
    GameVideo::draw(*display,jobPixels,palette,nullptr,width,height,jobPitch,options);
    __atomic_fetch_add(&drawUs,micros()-start,__ATOMIC_RELAXED);
    __atomic_fetch_add(&drawCount,1,__ATOMIC_RELAXED);
    xSemaphoreGive(idle);
  }
}
static bool begin(Arduino_RGB_Display& lcd,uint16_t background){
  if(task)return true;
  display=&lcd;bg=background;
  snapshot=(uint8_t*)heap_caps_malloc(320*240,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  idle=xSemaphoreCreateBinary();
  if(!snapshot||!idle){free(snapshot);snapshot=nullptr;if(idle)vSemaphoreDelete(idle);idle=nullptr;return false;}
  xSemaphoreGive(idle);
  if(xTaskCreatePinnedToCore(worker,"retroLCD",4096,nullptr,3,&task,0)!=pdPASS){
    vSemaphoreDelete(idle);idle=nullptr;free(snapshot);snapshot=nullptr;task=nullptr;return false;
  }
  return true;
}
static bool enabled(){return task!=nullptr;}
static bool ready(){return idle&&uxSemaphoreGetCount(idle)>0;}
static bool submit(const uint8_t* src,const uint16_t* pal,int w,int h,int pitch,GameVideo::Options opts){
  if(!src||!pal||w<1||w>320||h<1||h>240||pitch<w||!idle)return false;
  if(xSemaphoreTake(idle,0)!=pdTRUE)return false;
  options=opts;width=w;height=h;
  for(int y=0;y<h;y++)memcpy(snapshot+y*w,src+y*pitch,w);
  jobPixels=snapshot;jobPitch=w;
  memcpy(palette,pal,sizeof(palette));
  xTaskNotifyGive(task);
  return true;
}
// Caller guarantees that borrowed data is not overwritten until ready()/drain().
static bool submitBorrowed(uint8_t* src,const uint16_t* pal,int w,int h,int pitch,GameVideo::Options opts){
  if(!src||!pal||w<1||w>320||h<1||h>240||pitch<w||!idle)return false;
  if(xSemaphoreTake(idle,0)!=pdTRUE)return false;
  options=opts;width=w;height=h;jobPixels=src;jobPitch=pitch;
  memcpy(palette,pal,sizeof(palette));
  xTaskNotifyGive(task);
  return true;
}
static void drain(){
  if(idle){xSemaphoreTake(idle,portMAX_DELAY);oldW=oldH=0;GameVideo::reset();xSemaphoreGive(idle);}
}
static void stats(uint32_t &us,uint32_t &count){
  us=__atomic_exchange_n(&drawUs,0,__ATOMIC_RELAXED);
  count=__atomic_exchange_n(&drawCount,0,__ATOMIC_RELAXED);
}
}
