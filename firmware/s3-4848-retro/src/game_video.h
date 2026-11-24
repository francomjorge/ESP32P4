#pragma once
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <string.h>
namespace GameVideo {
enum Layout:uint8_t { FULL=0,FIT=1,NATIVE=2 };
struct Options {uint8_t layout;bool overlay;};
struct Key {int x,y,w,h;const char* label;uint8_t bit;};
static constexpr Key keys[]={{8,8,76,48,"EXIT",0},{65,293,62,62,"^",64},{5,355,62,62,"<",32},{65,417,62,58,"v",128},{125,355,62,62,">",16},{191,424,100,48,"SEL",4},{297,424,100,48,"START",8},{340,346,64,62,"B",2},{410,289,64,62,"A",1}};
static bool hit(const Key& k,int x,int y){return x>=k.x&&x<k.x+k.w&&y>=k.y&&y<k.y+k.h;}
static uint8_t input(int x,int y,bool& exit){
 uint8_t v=0;for(const auto& k:keys){if(hit(k,x,y)){if(!k.bit)exit=true;else v|=k.bit;}}return v;
}
// Only the display owner calls these routines. Never write into guest framebuffers.
static constexpr int STRIP_ROWS=16;
static uint16_t strip[480*STRIP_ROWS];
static uint16_t xmap[480],ymap[480],scaledRow[480];
static int mapSW=0,mapSH=0,mapW=0,mapH=0;
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
// Compose every control into the same RGB strip as the game. No later LCD
// primitive calls; black margins and disappearing controls are redrawn too.
// Five-by-seven fixed font: only characters actually used by touch keys.
static const uint8_t* glyph(char c){
 static const uint8_t g[][7]={
 {14,17,17,31,17,17,17}, // A
 {30,17,17,30,17,17,30}, // B
 {31,16,16,30,16,16,31}, // E
 {31,4,4,4,4,4,31}, // I
 {16,16,16,16,16,16,31}, // L
 {30,17,17,30,20,18,17}, // R
 {15,16,16,14,1,1,30}, // S
 {31,4,4,4,4,4,4}, // T
 {17,17,10,4,10,17,17}, // X
 {4,14,21,4,4,4,4}, // ^
 {4,4,4,4,21,14,4}, // v
 {4,8,16,31,16,8,4}, // <
 {4,2,1,31,1,2,4} // >
 };
 static const char names[]="ABEILRSTX^v<>";
 for(unsigned i=0;i<sizeof(names)-1;i++)if(c==names[i])return g[i];
 return nullptr;
}
static void compositeKey(const Key& k,int top,int rows){
 const int y0=max(top,k.y),y1=min(top+rows,k.y+k.h);
 if(y0>=y1)return;
 const int len=(int)strlen(k.label),tx=k.x+(k.w-(len*12-2))/2,ty=k.y+(k.h-14)/2;
 for(int y=y0;y<y1;y++){
  uint16_t* row=strip+(y-top)*480;
  for(int x=k.x;x<k.x+k.w;x++){
   int lx=x-k.x,ly=y-k.y;
   // clipped corners, two-pixel outline, half-bright translucent interior.
   if((lx<4||lx>=k.w-4)&&(ly<4||ly>=k.h-4))continue;
   bool border=lx<2||lx>=k.w-2||ly<2||ly>=k.h-2;
   bool letter=false;
   if(y>=ty&&y<ty+14&&x>=tx&&x<tx+len*12){
    int col=(x-tx)%12;
    const uint8_t* bits=glyph(k.label[(x-tx)/12]);
    if(bits&&col<10)letter=(bits[(y-ty)/2]&(1u<<(4-col/2)))!=0;
   }
   row[x]=(border||letter)?0xffff:((row[x]&0xf7deu)>>1);
  }
 }
}
static void scaleIndexed(uint16_t* dst,const uint8_t* src,const uint16_t* pal,int sw,int w){
 if(sw==160&&w==480){for(int j=0;j<160;j++){uint16_t c=pal[src[j]];dst[3*j]=dst[3*j+1]=dst[3*j+2]=c;}}
 else for(int j=0;j<w;j++)dst[j]=pal[src[xmap[j]]];
}
static void draw(Arduino_RGB_Display& lcd,const uint8_t* idx,const uint16_t* pal,const uint16_t* rgb,int sw,int sh,int pitch,Options o){
 if(sw<=0||sh<=0||sw>480||sh>480||pitch<sw||(!rgb&&(!idx||!pal)))return;
 int x,y,w,h;geometry(sw,sh,o.layout,x,y,w,h);
 if(mapSW!=sw||mapSH!=sh||mapW!=w||mapH!=h){
  for(int j=0;j<w;j++)xmap[j]=(uint32_t)j*sw/w;
  for(int j=0;j<h;j++)ymap[j]=(uint32_t)j*sh/h;
  mapSW=sw;mapSH=sh;mapW=w;mapH=h;
 }
 // When overlay is visible, compose a complete 480-wide image including margins.
 // Without overlay, retain the same safe library transfer and narrow FIT/NATIVE.
 const int outX=0,outY=0,outW=480,outH=480;
 uint16_t* target=lcd.getFramebuffer();
 int prev=-1;
 for(int top=0;top<outH;top+=STRIP_ROWS){
  int rows=min(STRIP_ROWS,outH-top);
  for(int row=0;row<rows;row++){
   int screenY=outY+top+row;
   uint16_t* dst=strip+row*outW;
   if(screenY<y||screenY>=y+h){memset(dst,0,outW*2);continue;}
   if(w<480){memset(dst,0,outW*2);dst+=x;}
   int sy=ymap[screenY-y];
   if(sy!=prev){
    if(rgb){const uint16_t* src=rgb+sy*pitch;for(int j=0;j<w;j++)scaledRow[j]=src[xmap[j]];}
    else scaleIndexed(scaledRow,idx+sy*pitch,pal,sw,w);
    prev=sy;
   }
   memcpy(dst,scaledRow,w*2);
  }
  if(o.overlay)for(const auto& k:keys)compositeKey(k,top,rows);
  // Full-width bounded copy into inactive DMA framebuffer only.
  memcpy(target+top*480,strip,rows*480*sizeof(uint16_t));
 }
}
}
