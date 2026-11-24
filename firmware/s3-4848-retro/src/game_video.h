#pragma once
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <string.h>
namespace GameVideo {
enum Layout:uint8_t { FULL=0,FIT=1,NATIVE=2 };
struct Options {uint8_t layout;bool overlay;bool sega;};
struct Key {int x,y,w,h;const char* label;uint8_t bit;};
static constexpr Key keys[]={{8,8,76,48,"EXIT",0},{65,293,62,62,"^",64},{5,355,62,62,"<",32},{65,417,62,58,"v",128},{125,355,62,62,">",16},{191,424,100,48,"SEL",4},{297,424,100,48,"START",8},{340,346,64,62,"B",2},{410,289,64,62,"A",1}};
static constexpr Key segaC={410,365,64,56,"C",4};
static bool hit(const Key& k,int x,int y){return x>=k.x&&x<k.x+k.w&&y>=k.y&&y<k.y+k.h;}
static uint8_t input(int x,int y,bool sega,bool& exit){
 uint8_t v=0;for(const auto& k:keys){if(sega&&k.bit==4)continue;if(hit(k,x,y)){if(!k.bit)exit=true;else v|=k.bit;}}
 if(sega&&hit(segaC,x,y))v|=4;return v;
}
// Only the display owner calls these routines. Never write into guest framebuffers.
static uint16_t strip[480*8];
static uint16_t xmap[480];
static uint8_t oldLayout=255;
static int oldW=0,oldH=0;
static bool oldOverlay=false;
static void reset(){oldLayout=255;oldW=oldH=0;oldOverlay=false;}
static void geometry(int sw,int sh,uint8_t layout,int& x,int& y,int& w,int& h){
 if(layout==FULL){w=h=480;}
 else if(layout==NATIVE){w=sw;h=sh;}
 else if(sw>=sh){w=480;h=sh*480/sw;}
 else{h=480;w=sw*480/sh;}
 x=(480-w)/2;y=(480-h)/2;
}
static void tint(const Key& k,int dx,int dy,int dw,int rows){
 const int x0=max(dx,k.x),x1=min(dx+dw,k.x+k.w);
 const int y0=max(dy,k.y),y1=min(dy+rows,k.y+k.h);
 if(x0>=x1||y0>=y1)return;
 for(int y=y0;y<y1;y++){uint16_t* p=strip+(y-dy)*dw+x0-dx;
  for(int x=x0;x<x1;x++,p++)*p=(*p&0xf7deu)>>1;
 }
}
static void drawKey(Arduino_RGB_Display& lcd,const Key& k){
 lcd.drawRoundRect(k.x,k.y,k.w,k.h,8,0xdedf);
 lcd.setTextSize(2);lcd.setTextColor(0xffff);lcd.setCursor(k.x+(k.w-12*strlen(k.label))/2,k.y+(k.h-16)/2);lcd.print(k.label);
}
static void draw(Arduino_RGB_Display& lcd,const uint8_t* idx,const uint16_t* pal,const uint16_t* rgb,int sw,int sh,int pitch,Options o){
 if(sw<=0||sh<=0||sw>480||sh>480||pitch<sw||(!rgb&&(!idx||!pal)))return;
 int x,y,w,h;geometry(sw,sh,o.layout,x,y,w,h);
 if(oldLayout!=o.layout||oldW!=w||oldH!=h||(o.layout!=FULL&&oldOverlay&&!o.overlay)){
  lcd.fillScreen(0);oldLayout=o.layout;oldW=w;oldH=h;
 }
 oldOverlay=o.overlay;
 if(o.layout==NATIVE&&!o.overlay){
  if(rgb)lcd.draw16bitRGBBitmap(x,y,const_cast<uint16_t*>(rgb),sw,sh);
  else lcd.drawIndexedBitmap(x,y,const_cast<uint8_t*>(idx),const_cast<uint16_t*>(pal),sw,sh,pitch-sw);
  return;
 }
 for(int j=0;j<w;j++)xmap[j]=(uint32_t)j*sw/w;
 for(int top=0;top<h;top+=8){
  int rows=min(8,h-top);int prev=-1;
  for(int row=0;row<rows;row++){
   int sy=(uint32_t)(top+row)*sh/h;uint16_t* dst=strip+row*w;
   if(row&&sy==prev)memcpy(dst,dst-w,w*2);
   else if(rgb){const uint16_t* src=rgb+sy*pitch;for(int j=0;j<w;j++)dst[j]=src[xmap[j]];}
   else {const uint8_t* src=idx+sy*pitch;for(int j=0;j<w;j++)dst[j]=pal[src[xmap[j]]];}
   prev=sy;
  }
  if(o.overlay){for(const auto& k:keys)if(!o.sega||k.bit!=4)tint(k,x,y+top,w,rows);if(o.sega)tint(segaC,x,y+top,w,rows);}
  lcd.draw16bitRGBBitmap(x,y+top,strip,w,rows);
 }
 if(o.overlay){for(const auto& k:keys)if(!o.sega||k.bit!=4)drawKey(lcd,k);if(o.sega)drawKey(lcd,segaC);}
}
}
