#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <algorithm>
#include <vector>
#include "gb_core.h"
#include "builtin_roms.h"
#include "nes_core.h"
#include "md_core.h"
#include <esp_partition.h>
#include "private_roms.h"
#include "async_video.h"

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
static bool nesMode=false,mdMode=false,home=true;
static bool playing=false,sdOk=false,wasDown=false;
static uint32_t perfStart=0,perfFrames=0,perfShown=0,perfWork=0,perfOver=0,lastWork=0;
static bool lastPresented=false;
static unsigned mdDiv=2,mdSeq=0,goodWindows=0;
static uint32_t queuedFrames=0,droppedFrames=0,borrowedFrames=0;
static uint32_t frameDeadline=0;
static int shownW=0,shownH=0;
static uint32_t mdDrawUs=0,mdCoreUs=0;
static size_t page=0;
static String status;
struct Point{uint16_t x,y;};
static Point points[5];
static uint8_t npoints=0;
static uint32_t lastTouchReport=0;

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
  if(!(st&0x80)){if(millis()-lastTouchReport>150)npoints=0;return;}
  lastTouchReport=millis();
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
static void scan(bool includeSD=false){
  paths.clear();page=0;
  if(nesMode||mdMode)for(unsigned i=0;i<private_rom_count();i++)if(private_rom_md(i)==mdMode)paths.push_back(String("#")+private_rom_name(i));
  for(const auto &g:builtins)if(!mdMode&&g.nes==nesMode)paths.push_back(String("@")+g.name);
  size_t builtinCount=paths.size();
  Serial.printf("ROM library %s: %u internal games\n",mdMode?"MD":nesMode?"NES":"GB",(unsigned)builtinCount);
  for(const auto &g:builtins)if(g.nes==nesMode)Serial.printf("  %s: %u bytes\n",g.name,(unsigned)g.size);
  if(!includeSD){status=String(builtinCount)+" juegos internos";return;}
  if(!sdOk){SD.end();sdOk=SD.begin(42,SPI,4000000);}
  if(!sdOk){status=String(paths.size())+" internos / sin SD";return;}
  File dir=SD.open(mdMode?"/roms/md":nesMode?"/roms/nes":"/roms/gb");
  if(!dir||!dir.isDirectory()){status=String(paths.size())+" internos / sin carpeta SD";return;}
  for(File f=dir.openNextFile();f;f=dir.openNextFile()){
    if(!f.isDirectory()){
      String path=f.path();String lower=path;lower.toLowerCase();
      if((mdMode?(lower.endsWith(".md")||lower.endsWith(".gen")||lower.endsWith(".bin")):lower.endsWith(nesMode?".nes":".gb"))&&paths.size()<128)paths.push_back(path);
    }f.close();
  }
  dir.close();
  std::sort(paths.begin()+builtinCount,paths.end(),[](const String&a,const String&b){return a.compareTo(b)<0;});
  status=paths.empty()?"No hay ROMs":String(paths.size())+" juegos detectados";
}
static void homeUI(){
  home=true;lcd.fillScreen(BG);textAt(20,20,"PRISMA RETRO",4,ACC);
  textAt(20,65,"BUILD 012 / SELECT SYSTEM",2);
  lcd.fillRoundRect(20,110,440,135,12,0x3265);lcd.drawRoundRect(20,110,440,135,12,0xAEDA);
  textAt(40,140,"GAME BOY",4);textAt(40,194,"TOBU TOBU GIRL + MAS",2);
  lcd.fillRoundRect(20,262,440,135,12,0x5925);lcd.drawRoundRect(20,262,440,135,12,0xF9C8);
  textAt(40,292,"NINTENDO NES",3);textAt(40,346,"MULTICART / NES",2);
  textAt(20,434,"DIO40 / 16MB / SD + FLASH",2,ACC);
}
static void menu(){
  home=false;
  lcd.fillScreen(BG);textAt(18,16,"PRISMA",3,ACC);
  textAt(18,52,mdMode?"MEGA DRIVE / BUILD 012":nesMode?"NES / BUILD 012":"GAME BOY / BUILD 012",2);
  button(206,8,76,38,"GB");button(292,8,82,38,"NES");button(384,8,84,38,"SEGA");
  textAt(18,81,status,2,ACC);
  for(size_t i=0;i<6&&page*6+i<paths.size();i++){
    int y=116+i*48;
    lcd.fillRoundRect(14,y,452,42,5,CARD);
    String p=paths[page*6+i];p=(p.startsWith("@")||p.startsWith("#"))?p.substring(1)+" [FLASH]":p.substring(p.lastIndexOf('/')+1);
    textAt(24,y+12,p.substring(0,34),2);
  }
  button(14,420,110,45,"PREV");button(145,420,170,45,"+ SD ROMS");button(345,420,121,45,"NEXT");
}
static void gameUI(){
  lcd.fillScreen(BG);
  button(10,8,64,45,"EXIT");
  button(64,310,56,48,"UP");button(8,365,56,48,"LT");
  button(64,420,56,48,"DN");button(120,365,56,48,"RT");
  button(190,426,102,42,mdMode?"C":"SELECT");button(300,426,100,42,"START");
  button(335,355,62,60,"B");button(407,320,64,60,"A");
}
static void releaseGame(){AsyncVideo::drain();if(mdMode)md_stop();else if(nesMode)nes_core_stop();free(rom);free(ram);free(frame);rom=nullptr;ram=nullptr;frame=nullptr;playing=false;}
static bool load(const String &p){
  File f;
  const BuiltinROM *internal=nullptr;
  size_t bytes=0;
  int privateIndex=-1;
  if(p.startsWith("#"))privateIndex=private_rom_find(p.substring(1).c_str());
  if(privateIndex>=0){bytes=private_rom_size(privateIndex);}
  else if(p.startsWith("@")){
    for(const auto &g:builtins)if(p.substring(1)==g.name){internal=&g;break;}
    if(!internal){status="ROM interna no encontrada";return false;}
    bytes=internal->size;
  }else{
    f=SD.open(p,FILE_READ);
    if(!f||f.size()<(mdMode?512:nesMode?16400:32768)||f.size()>4*1024*1024){status="ROM invalida (32KB-4MB)";return false;}
    bytes=f.size();
  }
  rom=(uint8_t*)ps_malloc(mdMode?0x400004:bytes);ram=(nesMode||mdMode)?nullptr:(uint8_t*)ps_malloc(128*1024);
  frame=(nesMode||mdMode)?nullptr:(uint16_t*)ps_malloc(320*288*sizeof(uint16_t));
  if(!rom||(!nesMode&&!mdMode&&!ram)||(!nesMode&&!mdMode&&!frame)){releaseGame();status="Sin PSRAM suficiente";return false;}
  if(mdMode)memset(rom,0xff,0x400004);
  size_t got=bytes;
  if(privateIndex>=0){if(!private_rom_read(privateIndex,rom,bytes))got=0;}
  else if(internal)memcpy(rom,internal->data,bytes);
  else {got=f.read(rom,bytes);f.close();}
  if(got!=bytes||(!nesMode&&!mdMode&&rom[0x143]==0xC0)){releaseGame();status="ROM no compatible (solo DMG)";return false;}
  if(ram)memset(ram,0xFF,128*1024);if(frame)memset(frame,0,320*288*sizeof(uint16_t));
  int err=mdMode?md_start(rom,bytes,frame):nesMode?nes_core_start(rom,bytes,frame):core_start(rom,bytes,ram,128*1024,frame);
  if(err){releaseGame();status="Error core: "+String(err);return false;}
  Serial.printf("ROM launched: %s, %u bytes\n",p.c_str(),(unsigned)bytes);
  perfStart=millis();perfFrames=perfShown=perfWork=perfOver=lastWork=0;lastPresented=false;mdDiv=mdMode?3:2;mdSeq=0;goodWindows=0;queuedFrames=droppedFrames=borrowedFrames=0;mdDrawUs=mdCoreUs=0;
  uint32_t u,n;AsyncVideo::stats(u,n);
  playing=true;gameUI();frameDeadline=micros();shownW=shownH=0;return true;
}
void setup(){
  Serial.begin(115200);delay(200);
  Serial.printf("CPU MHz=%u\n",getCpuFrequencyMhz());
  Serial.printf("PRISMA RETRO S3 flash=%u PSRAM=%u\n",(unsigned)ESP.getFlashChipSize(),(unsigned)ESP.getPsramSize());
  pinMode(38,OUTPUT);digitalWrite(38,LOW);
  pinMode(42,OUTPUT);digitalWrite(42,HIGH);
  if(!lcd.begin()){Serial.println("LCD init failed");return;}
  lcd.fillScreen(BG);digitalWrite(38,HIGH);
  Serial.printf("LCD worker core0=%s\n",AsyncVideo::begin(lcd,BG)?"ready":"fallback sync");
  Wire1.begin(19,45,400000);
  for(uint8_t a:{0x5D,0x14}){Wire1.beginTransmission(a);if(!Wire1.endTransmission()){addr=a;break;}}
  Serial.printf("GT911=0x%02x\n",addr);
  pinMode(39,OUTPUT);digitalWrite(39,HIGH);
  SPI.begin(48,41,47,42);
  private_rom_init();nesMode=true;scan();menu();
  Serial.printf("BOOT NES library: %u private ROMs, %u entries\n",private_rom_count(),(unsigned)paths.size());
}
void loop(){
  const uint32_t tickStart=micros();
  if(!addr){delay(100);return;}
  pollTouch();
  if(!playing){
    if(npoints&&!wasDown){
      Point p=points[0];
      Serial.printf("TOUCH x=%u y=%u home=%u nes=%u\n",p.x,p.y,home,nesMode);
      if(home){
        if(hit(p,20,110,440,135)||hit(p,20,262,440,135)){mdMode=false;nesMode=p.y>=262;scan();menu();}
      }else if(hit(p,206,0,76,50)){mdMode=false;nesMode=false;scan();menu();}
      else if(hit(p,292,0,82,50)){mdMode=false;nesMode=true;scan();menu();}
      else if(hit(p,384,0,96,50)){mdMode=true;nesMode=false;scan();menu();}
      else if(hit(p,14,420,110,45)){if(page)page--;menu();}
      else if(hit(p,145,420,170,45)){scan(true);menu();}
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
  const uint32_t period=mdMode?md_period():nesMode?16667:16743;
  // Present every frame when possible, alternate only after an expensive frame.
  const bool due=mdSeq++%mdDiv==0;
  const bool async=(mdMode||nesMode)&&AsyncVideo::enabled();
  const bool present=due&&(!async||AsyncVideo::ready());
  if(due&&!present)droppedFrames++;
  uint32_t coreStart=micros();
  if(mdMode)md_frame(keys,present);else if(nesMode)nes_core_frame(keys,present);else core_frame(keys,present);
  mdCoreUs+=micros()-coreStart;
  if(present){
    uint32_t t=micros();
    if(mdMode||nesMode){
      int w,h,pitch;uint16_t *pal;
      uint8_t *v=mdMode?md_video(&w,&h,&pitch,&pal):nes_core_video(&w,&h,&pitch,&pal);
      if(v&&pal){
        if(async){
          if(mdMode&&md_video_can_flip()){
            if(AsyncVideo::submitBorrowed(v,pal,w,h,pitch)){
              md_video_flip();queuedFrames++;borrowedFrames++;
            }else droppedFrames++;
          }else if(AsyncVideo::submit(v,pal,w,h,pitch))queuedFrames++;else droppedFrames++;
        }
        else {if(w!=shownW||h!=shownH){lcd.fillRect(80,10,320,288,BG);shownW=w;shownH=h;}
          lcd.drawIndexedBitmap((480-w)/2,10+(288-h)/2,v,pal,w,h,pitch-w);}
      }
    }else lcd.draw16bitRGBBitmap(80,10,frame,320,288);
    mdDrawUs+=micros()-t;if(!async)perfShown++;
  }
  lastPresented=present;
  lastWork=micros()-tickStart;
  perfFrames++;perfWork+=lastWork;
  if(lastWork>=period)perfOver++;
  uint32_t elapsedMs=millis()-perfStart;
  if(elapsedMs>=3000){
    uint32_t workerUs=0,workerCount=0;AsyncVideo::stats(workerUs,workerCount);
    if(async)perfShown=workerCount;
    Serial.printf("PERF %s emu=%.1f fps display=%.1f fps work=%u us late=%u/%u heap=%u psram=%u\n",
      mdMode?"MD":nesMode?"NES":"GB",perfFrames*1000.0/elapsedMs,perfShown*1000.0/elapsedMs,
      (unsigned)(perfWork/perfFrames),(unsigned)perfOver,(unsigned)perfFrames,
      (unsigned)ESP.getFreeHeap(),(unsigned)ESP.getFreePsram());
    {
      float fps=perfFrames*1000.0f/elapsedMs,target=1000000.0f/period;
      Serial.printf("VIDEO PERF core=%u us lcd=%u us frameskip=%u target=%.1f\n",(unsigned)(mdCoreUs/perfFrames),(unsigned)(perfShown?mdDrawUs/perfShown:0),mdDiv-1,target);
      if(async)Serial.printf("ASYNC LCD=%u us submit=%u us queued=%u dropped=%u borrowed=%u\n",(unsigned)(workerCount?workerUs/workerCount:0),(unsigned)(queuedFrames?mdDrawUs/queuedFrames:0),queuedFrames,droppedFrames,borrowedFrames);
      if(fps<target-2){goodWindows=0;if(mdDiv<4)mdDiv++;}
      else if(fps>=target-0.5f && droppedFrames==0){if(++goodWindows>=5){if(mdDiv>2)mdDiv--;goodWindows=0;}}
      else goodWindows=0;
      queuedFrames=droppedFrames=borrowedFrames=0;
      mdCoreUs=mdDrawUs=0;
    }
    perfStart=millis();perfFrames=perfShown=perfWork=perfOver=0;
  }
  // Fixed timeline: a costly displayed frame can borrow time from cheaper skipped frames.
  // Limit debt to three frame periods; do not skip CPU emulation.
  frameDeadline+=period;
  int32_t remaining=(int32_t)(frameDeadline-micros());
  if(remaining<-(int32_t)(3*period)){frameDeadline=micros();remaining=0;}
  if(remaining>0){
    if(remaining>2000)delay((remaining-1000)/1000);
    remaining=(int32_t)(frameDeadline-micros());
    if(remaining>0)delayMicroseconds(remaining);
  }else if((mdSeq&7)==0)delay(1);
}
