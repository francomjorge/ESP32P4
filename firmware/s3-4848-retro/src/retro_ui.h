#pragma once
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
namespace RetroUI {
constexpr uint16_t rgb(unsigned r,unsigned g,unsigned b){return ((r&248)<<8)|((g&252)<<3)|(b>>3);}
struct Theme {const char* name;const char* sub;const char* bits;uint16_t bg,card,edge,ink,muted;};
static const Theme themes[]={
 {"GAME BOY","1989 / PORTABLE","8-BIT",rgb(25,42,31),rgb(45,68,48),rgb(126,165,102),rgb(220,235,185),rgb(166,190,150)},
 {"NINTENDO NES","1983 / FAMILY COMPUTER","8-BIT",rgb(34,29,32),rgb(66,48,50),rgb(192,92,80),rgb(246,226,202),rgb(209,166,153)}
};
static void text(Arduino_GFX& d,int x,int y,const String& s,int size,uint16_t color){d.setTextSize(size);d.setTextColor(color);d.setCursor(x,y);d.print(s);}
static void center(Arduino_GFX& d,int x,int y,const String& s,int size,uint16_t color){text(d,x-(int)s.length()*3*size,y,s,size,color);}
static void button(Arduino_GFX& d,int x,int y,int w,int h,const String& s,const Theme& t,bool primary=false){
 d.fillRoundRect(x,y,w,h,8,primary?t.edge:t.card);d.drawRoundRect(x,y,w,h,8,t.edge);
 center(d,x+w/2,y+(h-16)/2,s,2,primary?t.bg:t.ink);
}
static void bg(Arduino_GFX& d,int sys){
 const uint16_t c=sys<0?rgb(13,12,28):themes[sys].bg;
 d.fillScreen(c);
 const uint16_t line=sys<0?rgb(38,29,59):sys==0?rgb(34,51,37):rgb(55,39,45);
 // Wide low-contrast geometric bands; no animated or one-pixel scanline patterns.
 for(int y=0;y<480;y+=40)d.fillRect(0,y,480,4,line);
 if(sys<0){for(int i=0;i<5;i++){int y=385+i*15;d.fillRect(0,y,480,3+i,line);}}
}
static void console(Arduino_GFX& d,int x,int y,int s,int sys){
 auto box=[&](int a,int b,int w,int h,uint16_t c){d.fillRect(x+a*s,y+b*s,w*s,h*s,c);};
 auto round=[&](int a,int b,int w,int h,int r,uint16_t c){d.fillRoundRect(x+a*s,y+b*s,w*s,h*s,r*s,c);};
 auto dot=[&](int a,int b,int r,uint16_t c){d.fillCircle(x+a*s,y+b*s,r*s,c);};
 if(sys==0){
  round(4,0,24,36,3,rgb(194,193,177));round(7,3,18,16,1,rgb(60,71,61));box(9,5,14,12,rgb(150,179,105));
  box(8,24,9,3,rgb(45,48,47));box(11,21,3,9,rgb(45,48,47));dot(22,24,2,rgb(143,40,78));dot(26,21,2,rgb(143,40,78));box(14,32,6,1,rgb(95,96,87));
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
 for(int i=0;i<12;i++){unsigned v=seed*1103515245u+i*2654435761u;int ht=1+(v>>27)%5;int px=a+i*cell;int ph=ht*cell;if(ph>e/2)ph=e/2;if(px+cell<=a+c)d.fillRect(px,b+e-ph,cell,ph,t.edge);}
 d.fillCircle(a+c*3/4,b+e/4,max(2,c/10),t.muted);
 for(int i=0;i<5;i++)d.drawFastHLine(x+w/5,y+h-5-i*2,w*3/5,t.bg);
}
static unsigned seed(const String& s){unsigned v=2166136261u;for(unsigned i=0;i<s.length();i++)v=(v^(uint8_t)s[i])*16777619u;return v;}
static void splash(Arduino_GFX& d){
 bg(d,-1);uint16_t white=0xffff;
 d.drawTriangle(240,84,180,193,300,193,white);d.drawTriangle(240,87,183,191,297,191,white);d.drawFastHLine(95,151,125,white);
 const uint16_t colors[]={0xf9a8,0xfd40,0xff60,0x67e8,0x4dff,0xb39f};for(int i=0;i<6;i++){d.drawLine(259,151,384,114+i*15,colors[i]);d.drawLine(259,152,384,115+i*15,colors[i]);}
 center(d,240,250,"PRISMA",6,white);center(d,240,309,"R E T R O",3,0xb59e);center(d,240,365,"8-BIT ENTERTAINMENT",2,0x94b2);center(d,240,407,"TOCA PARA INICIAR",2,0xffff);center(d,240,455,"ESP32-S3 / BUILD 030",1,0x94b2);
}
static void home(Arduino_GFX& d,const unsigned counts[2]){
 bg(d,-1);text(d,20,16,"PRISMA RETRO",3,0xffff);text(d,415,19,"030",2,0x9cfa);text(d,20,47,"SELECT SYSTEM",2,0x94b2);
 for(int i=0;i<2;i++){const auto&t=themes[i];int y=90+i*164;d.fillRoundRect(16,y,448,148,10,t.card);d.drawRoundRect(16,y,448,148,10,t.edge);console(d,32,y+35,2,i);text(d,126,y+32,t.name,i==1?2:3,t.ink);text(d,126,y+60,t.sub,1,t.muted);text(d,126,y+85,String(t.bits)+" / "+counts[i]+" JUEGOS",2,t.ink);text(d,432,y+62,">",3,t.muted);}
 center(d,240,459,"ELEGIR CONSOLA",1,0x94b2);
}
}
