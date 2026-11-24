#pragma once
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
// Arduino_GFX menu primitives target only the inactive driver-owned buffer.
// RGB bounce DMA consumes the active buffer; the frame-complete event means
// the driver has finished reading the old framebuffer and selected the next.
class PrismaDisplay : public Arduino_RGB_Display {
 public:
  using Arduino_RGB_Display::Arduino_RGB_Display;
  bool initSwap(){
    handle=_rgbpanel->prismaPanelHandle();
    void *a=nullptr,*b=nullptr;
    if(!handle||esp_lcd_rgb_panel_get_frame_buffer(handle,2,&a,&b)!=ESP_OK||!a||!b||a==b||a!=_framebuffer)return false;
    fbs[0]=(uint16_t*)a;fbs[1]=(uint16_t*)b;
    done=xSemaphoreCreateBinary();if(!done)return false;
    esp_lcd_rgb_panel_event_callbacks_t cb={};
    cb.on_frame_buf_complete=frameDone;
    cb.on_vsync=vsyncDone;
    if(esp_lcd_rgb_panel_register_event_callbacks(handle,&cb,this)!=ESP_OK)return false;
    // Driver initially scans buffer 0. All subsequent drawing uses buffer 1.
    back=1;_framebuffer=fbs[back];return true;
  }
  void present(){
    // Only one caller owns the display. Publish completed cache contents once.
    flush(true);
    if(esp_lcd_panel_draw_bitmap(handle,0,0,480,480,_framebuffer)!=ESP_OK){
      Serial.println("LCD SWAP submit error");ESP.restart();return;
    }
    // Discard any pre-submit event (or a racing event, conservatively waiting
    // one more boundary). Never reuse old scanout before a new completion.
    xSemaphoreTake(done,0);
    unsigned retries=0;
    while(xSemaphoreTake(done,pdMS_TO_TICKS(150))!=pdTRUE){
      Serial.println("LCD SWAP timeout: buffer not recycled");
      if(++retries==5){ESP.restart();return;}
    }
    back^=1;_framebuffer=fbs[back];
  }
  uint32_t vsyncTotal()const{return vsyncCount;}
 private:
  static bool IRAM_ATTR frameDone(esp_lcd_panel_handle_t,const esp_lcd_rgb_panel_event_data_t*,void* ctx){
    auto* self=static_cast<PrismaDisplay*>(ctx);BaseType_t wake=pdFALSE;
    xSemaphoreGiveFromISR(self->done,&wake);return wake==pdTRUE;
  }
  static bool IRAM_ATTR vsyncDone(esp_lcd_panel_handle_t,const esp_lcd_rgb_panel_event_data_t*,void* ctx){
    auto* self=static_cast<PrismaDisplay*>(ctx);self->vsyncCount++;return false;
  }
  esp_lcd_panel_handle_t handle=nullptr;
  SemaphoreHandle_t done=nullptr;
  uint16_t* fbs[2]={nullptr,nullptr};
  unsigned back=1;
  volatile uint32_t vsyncCount=0;
};
