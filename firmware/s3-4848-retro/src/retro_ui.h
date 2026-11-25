#pragma once
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
namespace RetroUI {
constexpr uint16_t rgb(unsigned r,unsigned g,unsigned b){return ((r&248)<<8)|((g&252)<<3)|(b>>3);}
struct Theme {const char* name;const char* sub;const char* bits;uint16_t bg,card,edge,ink,muted;};
static const Theme themes[]={
#ifdef PRISMA_COMBINED
 {"MS-DOS","PC / x86","DOS",rgb(25,42,31),rgb(45,68,48),rgb(126,165,102),rgb(220,235,185),rgb(166,190,150)},
#else
 {"GAME BOY","1989 / PORTABLE","8-BIT",rgb(25,42,31),rgb(45,68,48),rgb(126,165,102),rgb(220,235,185),rgb(166,190,150)},
#endif
 {"NINTENDO NES","1983 / FAMILY COMPUTER","8-BIT",rgb(34,29,32),rgb(66,48,50),rgb(192,92,80),rgb(246,226,202),rgb(209,166,153)}
};
static void text(Arduino_GFX& d,int x,int y,const String& s,int size,uint16_t color){d.setTextSize(size);d.setTextColor(color);d.setCursor(x,y);d.print(s);}
static void center(Arduino_GFX& d,int x,int y,const String& s,int size,uint16_t color){text(d,x-(int)s.length()*3*size,y,s,size,color);}
static void button(Arduino_GFX& d,int x,int y,int w,int h,const String& s,const Theme& t,bool primary=false){
 d.fillRoundRect(x,y,w,h,8,primary?t.edge:t.card);d.drawRoundRect(x,y,w,h,8,t.edge);
 center(d,x+w/2,y+(h-16)/2,s,2,primary?t.bg:t.ink);
}
static void bg(Arduino_GFX& d,int sys){
 d.fillScreen(sys<0?rgb(13,15,25):themes[sys].bg);
}
static void console(Arduino_GFX& d,int x,int y,int s,int sys){
 auto box=[&](int a,int b,int w,int h,uint16_t c){d.fillRect(x+a*s,y+b*s,w*s,h*s,c);};
 auto round=[&](int a,int b,int w,int h,int r,uint16_t c){d.fillRoundRect(x+a*s,y+b*s,w*s,h*s,r*s,c);};
 auto dot=[&](int a,int b,int r,uint16_t c){d.fillCircle(x+a*s,y+b*s,r*s,c);};
 if(sys==0){
#ifdef PRISMA_COMBINED
  box(2,2,30,23,rgb(194,193,177));box(5,5,24,16,rgb(60,71,61));box(12,25,10,4,rgb(126,126,122));box(3,31,28,4,rgb(194,193,177));
#else
  round(4,0,24,36,3,rgb(194,193,177));round(7,3,18,16,1,rgb(60,71,61));box(9,5,14,12,rgb(150,179,105));
  box(8,24,9,3,rgb(45,48,47));box(11,21,3,9,rgb(45,48,47));dot(22,24,2,rgb(143,40,78));dot(26,21,2,rgb(143,40,78));box(14,32,6,1,rgb(95,96,87));
#endif
 }else if(sys==1){
  round(0,9,34,20,2,rgb(198,195,183));box(0,16,34,7,rgb(126,126,122));box(4,11,16,4,rgb(89,86,80));box(21,24,10,3,rgb(30,29,30));box(3,24,4,3,rgb(182,52,48));
 }
}
static void cart(Arduino_GFX& d,int x,int y,int w,int h,int sys,unsigned seed){
 const auto&t=themes[sys];uint16_t body=rgb(151,151,141);
 d.fillRoundRect(x,y,w,h,5,body);d.drawRoundRect(x,y,w,h,5,t.muted);
 int a=x+w/8,b=y+h/8,c=w*3/4,e=h*3/5;
 d.fillRect(a,b,c,e,t.card);
 // deterministic original pixel-art label, not a claimed game cover
 int cell=max(2,c/12);
 for(int i=0;i<12;i++){unsigned v=seed*1103915245u+i*2654435761u;int ht=1+(v>>27)%5;int px=a+i*cell;int ph=ht*cell;if(ph>e/2)ph=e/2;if(px+cell<=a+c)d.fillRect(px,b+e-ph,cell,ph,t.edge);}
 d.fillCircle(a+c*3/4,b+e/4,max(2,c/10),t.muted);
 for(int i=0;i<5;i++)d.drawFastHLine(x+w/5,y+h-5-i*2,w*3/5,t.bg);
}
static unsigned seed(const String& s){unsigned v=2166136261u;for(unsigned i=0;i<s.length();i++)v=(v^(uint8_t)s[i])*16777619u;return v;}
// Chunky 4-pixel lines on a solid dark background, no scanline texture.
static void pixelLine(Arduino_GFX& d,int x0,int y0,int x1,int y1,uint16_t color,int weight=1){
 int dx=abs(x1-x0),sx=x0<x1?1:-1,dy=-abs(y1-y0),sy=y0<y1?1:-1,err=dx+dy;
 for(;;){d.fillRect(x0*4,y0*4,4*weight,4*weight,color);if(x0==x1&&y0==y1)break;int e2=2*err;if(e2>=dy){err+=dy;x0+=sx;}if(e2<=dx){err+=dx;y0+=sy;}}
}
static void splash(Arduino_GFX& d){
#ifdef PRISMA_COMBINED
 d.fillScreen(0);
 center(d,240,186,"PRISMA",6,0xffff);
 center(d,240,246,"RETRO",4,0xffff);
 center(d,240,420,"TOCA PARA INICIAR",2,0x94b2);
 center(d,240,458,"BUILD 044",1,0x94b2);
#else

 bg(d,-1);const uint16_t white=rgb(235,241,252);
 const uint16_t colors[]={rgb(255,83,120),rgb(255,155,70),rgb(255,216,94),rgb(91,221,156),rgb(74,165,255),rgb(166,103,235)};
 pixelLine(d,21,34,54,34,white);
 for(int i=0;i<6;i++)pixelLine(d,65,34,97,20+i*5,colors[i]);
 pixelLine(d,60,19,44,47,white);pixelLine(d,44,47,76,47,white);pixelLine(d,76,47,60,19,white);
 center(d,240,246,"PRISMA",6,white);center(d,240,307,"R E T R O",3,rgb(166,103,235));
 for(int i=0;i<6;i++)d.fillRect(168+i*24,350,20,8,colors[i]);
 center(d,240,380,"8-BIT ENTERTAINMENT",2,rgb(152,165,191));
 center(d,240,418,"TOCA PARA INICIAR",2,white);
 center(d,240,458,
#ifdef PRISMA_COMBINED
 "ESP32-S3 / BUILD 039"
#else
 "ESP32-S3 / BUILD 031"
#endif
,1,rgb(100,115,144));
#endif
}
static void home(Arduino_GFX& d,const unsigned counts[2]){
 bg(d,-1);text(d,20,16,"PRISMA RETRO",3,0xffff);
#ifdef PRISMA_COMBINED
 text(d,403,19,"044",2,0x9cfa);
#else
 text(d,415,19,"031",2,0x9cfa);
#endif
text(d,20,47,"SELECT SYSTEM",2,0x94b2);
 for(int i=0;i<2;i++){const auto&t=themes[i];int y=90+i*164;d.fillRoundRect(16,y,448,148,10,t.card);d.drawRoundRect(16,y,448,148,10,t.edge);console(d,32,y+35,2,i);text(d,126,y+32,t.name,i==1?2:3,t.ink);text(d,126,y+60,t.sub,1,t.muted);text(d,126,y+85,String(t.bits)+" / "+counts[i]+" JUEGOS",2,t.ink);text(d,432,y+62,">",3,t.muted);}
 center(d,240,459,"ELEGIR CONSOLA",1,0x94b2);
}
}
