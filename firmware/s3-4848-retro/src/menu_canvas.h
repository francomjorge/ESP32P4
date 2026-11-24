#pragma once
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
class MenuCanvas : public Arduino_Canvas {
public:
 MenuCanvas():Arduino_Canvas(480,480,nullptr){}
 bool ensure(){
  if(!_framebuffer)_framebuffer=(uint16_t*)heap_caps_aligned_alloc(16,480*480*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  return _framebuffer!=nullptr;
 }
 void release(){free(_framebuffer);_framebuffer=nullptr;}
};
