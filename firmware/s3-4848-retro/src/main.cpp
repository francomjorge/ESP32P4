#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <algorithm>
#include <vector>
#include "gb_core.h"

static Arduino_SWSPI panelBus(GFX_NOT_DEFINED,39,48,47,GFX_NOT_DEFINED);
static Arduino_ESP32RGBPanel rgb(18,17,16,21,
  11,12,13,14,0, 8,20,3,46,9,10, 4,5,6,7,15,
  1,10,8,50,1,10,8,20,0,8000000);
static Arduino_RGB_Display lcd(480,480,&rgb,0,true,&panelBus,GFX_NOT_DEFINED,
  st7701_type9_init_operations,sizeof(st7701_type9_init_operations));

static const uint16_t BG=0x0842,FG=0xFFFF,ACC=0x5E9E,CARD=0x2124;
static uint8_t addr=0, *rom=nullptr,*ram=nullptr;
static uint16_t *frame=nullptr;
static std::vector<String> paths;
static bool playing=false,sdOk=false,wasDown=false;
static size_t page=0;
static String status;
struct Point{uint16_t x,y;};
static Point points[5];
static uint8_t npoints=0;

static bool readReg(uint16_t reg,uint8_t *out,uint8_t n){
  if(!addr)return false;
  Wire1.beginTransmission(addr);Wire1.write(reg>>8);Wire1.write(reg&255);
  if(Wire1.endTransmission(false))return false;
  if(Wire1.requestFrom(addr,n)!=n)return false;
  for(int i=0;i<n;i++)out[i]=Wire1.read();return true;
}
static void clearTouch(){
  Wire1.beginTransmission(addr);Wire1.write(0x81);Wire1.write(0x4E);Wire1.write(0);
  Wire1.endTransmission();
}
static void pollTouch(){
  uint8_t st=0;
  if(!readReg(0x814E,&st,1)){npoints=0;return;}
  if(!(st&0x80))return;
  uint8_t count=st&15; npoints=0;
  if(count<=5){
    uint8_t raw[40];
    if(count && readReg(0x814F,raw,count*8)){
      for(int i=0;i<count;i++){
        uint8_t *p=raw+i*8;
        uint16_t x=p[1]|(p[2]<<8),y=p[3]|(p[4]<<8);
        if(x<480&&y<480)points[npoints++]={x,y};
      }
    }
  }
  clearTouch();
}
static void textAt(int x,int y,const String &s,uint8_t size=2,uint16_t color=FG){
  lcd.setTextColor(color);lcd.setTextSize(size);lcd.setCursor(x,y);lcd.print(s);
}
static void button(int x,int y,int w,int h,const char *s){
  lcd.fillRoundRect(x,y,w,h,8,CARD);lcd.drawRoundRect(x,y,w,h,8,ACC);
  textAt(x+8,y+(h-16)/2,s);
}
static bool hit(Point p,int x,int y,int w,int h){
  return p.x>=x&&p.x<x+w&&p.y>=y&&p.y<y+h;
}
static void scan(){
  paths.clear();page=0;
  if(!sdOk){SD.end();sdOk=SD.begin(42,SPI,4000000);}
  if(!sdOk){status="SD no detectada";return;}
  File dir=SD.open("/roms/gb");
  if(!dir||!dir.isDirectory()){status="Crear /roms/gb en SD";return;}
  for(File f=dir.openNextFile();f;f=dir.openNextFile()){
    if(!f.isDirectory()){
      String path=f.path();String lower=path;lower.toLowerCase();
      if(lower.endsWith(".gb")&&paths.size()<128)paths.push_back(path);
    }f.close();
  }
  dir.close();
  std::sort(paths.begin(),paths.end(),[](const String&a,const String&b){return a.compareTo(b)<0;});
  status=paths.empty()?"No hay ROM .gb":String(paths.size())+" juegos detectados";
}
static void menu(){
  lcd.fillScreen(BG);textAt(18,16,"PRISMA RETRO",3,ACC);
  textAt(18,52,"S3 / GAME BOY / BUILD 001",2);
  textAt(18,81,status,2,ACC);
  for(size_t i=0;i<6&&page*6+i<paths.size();i++){
    int y=116+i*48;
    lcd.fillRoundRect(14,y,452,42,5,CARD);
    String p=paths[page*6+i];p=p.substring(p.lastIndexOf('/')+1);
    textAt(24,y+12,p.substring(0,34),2);
  }
  button(14,420,110,45,"PREV");button(145,420,170,45,"RESCAN");button(345,420,121,45,"NEXT");
}
static void gameUI(){
  lcd.fillScreen(BG);
  button(10,8,64,45,"EXIT");
  button(64,310,56,48,"UP");button(8,365,56,48,"LT");
  button(64,420,56,48,"DN");button(120,365,56,48,"RT");
  button(190,426,102,42,"SELECT");button(300,426,100,42,"START");
  button(335,355,62,60,"B");button(407,320,64,60,"A");
}
static void releaseGame(){free(rom);free(ram);free(frame);rom=nullptr;ram=nullptr;frame=nullptr;playing=false;}
static bool load(const String &p){
  File f=SD.open(p,FILE_READ);
  if(!f||f.size()<32768||f.size()>4*1024*1024){status="ROM invalida (32KB-4MB)";return false;}
  const size_t bytes=f.size();
  rom=(uint8_t*)ps_malloc(bytes);ram=(uint8_t*)ps_malloc(128*1024);
  frame=(uint16_t*)ps_malloc(320*288*sizeof(uint16_t));
  if(!rom||!ram||!frame){releaseGame();status="Sin PSRAM suficiente";return false;}
  size_t got=f.read(rom,bytes);f.close();
  if(got!=bytes||rom[0x143]==0xC0){releaseGame();status="ROM no compatible (solo DMG)";return false;}
  memset(ram,0xFF,128*1024);memset(frame,0,320*288*sizeof(uint16_t));
  int err=core_start(rom,bytes,ram,128*1024,frame);
  if(err){releaseGame();status="Error core: "+String(err);return false;}
  playing=true;gameUI();return true;
}
void setup(){
  Serial.begin(115200);delay(200);
  Serial.printf("PRISMA RETRO S3 flash=%u PSRAM=%u\n",(unsigned)ESP.getFlashChipSize(),(unsigned)ESP.getPsramSize());
  pinMode(38,OUTPUT);digitalWrite(38,LOW);
  pinMode(42,OUTPUT);digitalWrite(42,HIGH);
  if(!lcd.begin()){Serial.println("LCD init failed");return;}
  lcd.fillScreen(BG);digitalWrite(38,HIGH);
  Wire1.begin(19,45,400000);
  for(uint8_t a:{0x5D,0x14}){Wire1.beginTransmission(a);if(!Wire1.endTransmission()){addr=a;break;}}
  Serial.printf("GT911=0x%02x\n",addr);
  pinMode(39,OUTPUT);digitalWrite(39,HIGH);
  SPI.begin(48,41,47,42);
  scan();menu();
}
void loop(){
  if(!addr){delay(100);return;}
  pollTouch();
  if(!playing){
    if(npoints&&!wasDown){
      Point p=points[0];
      if(hit(p,14,420,110,45)){if(page)page--;menu();}
      else if(hit(p,145,420,170,45)){scan();menu();}
      else if(hit(p,345,420,121,45)){if((page+1)*6<paths.size())page++;menu();}
      else if(p.y>=116&&p.y<404){
        size_t i=page*6+(p.y-116)/48;
        if(i<paths.size()&&!load(paths[i]))menu();
      }
    }
    wasDown=npoints>0;delay(12);return;
  }
  uint8_t keys=0;
  for(int i=0;i<npoints;i++){
    Point p=points[i];
    if(hit(p,10,8,64,45)){releaseGame();status="Juego cerrado";menu();wasDown=true;return;}
    if(hit(p,407,320,64,60))keys|=1;
    if(hit(p,335,355,62,60))keys|=2;
    if(hit(p,190,426,102,42))keys|=4;
    if(hit(p,300,426,100,42))keys|=8;
    if(hit(p,120,365,56,48))keys|=16;
    if(hit(p,8,365,56,48))keys|=32;
    if(hit(p,64,310,56,48))keys|=64;
    if(hit(p,64,420,56,48))keys|=128;
  }
  uint32_t start=micros();core_frame(keys);
  static unsigned count=0;
  if((count++&1)==0)lcd.draw16bitRGBBitmap(80,10,frame,320,288);
  uint32_t elapsed=micros()-start;
  if(elapsed<16743)delay((16743-elapsed+999)/1000);
  else delay(1);
}
