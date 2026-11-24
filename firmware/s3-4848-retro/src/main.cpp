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
#include <esp_partition.h>
#include "private_roms.h"
#include "fullscreen.h"

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
static bool nesMode=false,home=true;
static bool playing=false,sdOk=false,wasDown=false;
static uint32_t perfStart=0,perfFrames=0,perfShown=0,perfWork=0,perfOver=0,lastWork=0;
static bool lastPresented=false;
static bool aspectFit=false, gameTouchArmed=false, modeHeld=false;
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
  if(nesMode)for(unsigned i=0;i<private_rom_count();i++)paths.push_back(String("#")+private_rom_name(i));
  for(const auto &g:builtins)if(g.nes==nesMode)paths.push_back(String("@")+g.name);
  size_t builtinCount=paths.size();
  Serial.printf("ROM library %s: %u internal games\n",nesMode?"NES":"GB",(unsigned)builtinCount);
  for(const auto &g:builtins)if(g.nes==nesMode)Serial.printf("  %s: %u bytes\n",g.name,(unsigned)g.size);
  if(!includeSD){status=String(builtinCount)+" juegos internos";return;}
  if(!sdOk){SD.end();sdOk=SD.begin(42,SPI,4000000);}
  if(!sdOk){status=String(paths.size())+" internos / sin SD";return;}
  File dir=SD.open(nesMode?"/roms/nes":"/roms/gb");
  if(!dir||!dir.isDirectory()){status=String(paths.size())+" internos / sin carpeta SD";return;}
  for(File f=dir.openNextFile();f;f=dir.openNextFile()){
    if(!f.isDirectory()){
      String path=f.path();String lower=path;lower.toLowerCase();
      if(lower.endsWith(nesMode?".nes":".gb")&&paths.size()<128)paths.push_back(path);
    }f.close();
  }
  dir.close();
  std::sort(paths.begin()+builtinCount,paths.end(),[](const String&a,const String&b){return a.compareTo(b)<0;});
  status=paths.empty()?"No hay ROM .gb":String(paths.size())+" juegos detectados";
}
static void homeUI(){
  home=true;lcd.fillScreen(BG);textAt(20,20,"PRISMA RETRO",4,ACC);
  textAt(20,65,"BUILD 006 / SELECT SYSTEM",2);
  lcd.fillRoundRect(20,110,440,135,12,0x3265);lcd.drawRoundRect(20,110,440,135,12,0xAEDA);
  textAt(40,140,"GAME BOY",4);textAt(40,194,"TOBU TOBU GIRL + MAS",2);
  lcd.fillRoundRect(20,262,440,135,12,0x5925);lcd.drawRoundRect(20,262,440,135,12,0xF9C8);
  textAt(40,292,"NINTENDO NES",3);textAt(40,346,"MULTICART / NES",2);
  textAt(20,434,"DIO40 / 16MB / SD + FLASH",2,ACC);
}
static void menu(){
  home=false;
  lcd.fillScreen(BG);textAt(18,16,"PRISMA RETRO",3,ACC);
  textAt(18,52,nesMode?"NES / BUILD 006":"GAME BOY / BUILD 006",2);
  button(280,8,86,38,"GB");button(376,8,90,38,"NES");
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
  Fullscreen::configure(nesMode,aspectFit);
  lcd.fillScreen(BG);
  gameTouchArmed=false;modeHeld=false;
}
static void releaseGame(){if(nesMode)nes_core_stop();free(rom);free(ram);free(frame);rom=nullptr;ram=nullptr;frame=nullptr;playing=false;}
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
    if(!f||f.size()<(nesMode?16400:32768)||f.size()>4*1024*1024){status="ROM invalida (32KB-4MB)";return false;}
    bytes=f.size();
  }
  rom=(uint8_t*)ps_malloc(bytes);ram=nesMode?nullptr:(uint8_t*)ps_malloc(128*1024);
  const size_t pixels=nesMode?256*240:160*144;
  frame=(uint16_t*)ps_malloc(pixels*sizeof(uint16_t));
  if(!rom||(!nesMode&&!ram)||!frame){releaseGame();status="Sin PSRAM suficiente";return false;}
  size_t got=bytes;
  if(privateIndex>=0){if(!private_rom_read(privateIndex,rom,bytes))got=0;}
  else if(internal)memcpy(rom,internal->data,bytes);
  else {got=f.read(rom,bytes);f.close();}
  if(got!=bytes||(!nesMode&&rom[0x143]==0xC0)){releaseGame();status="ROM no compatible (solo DMG)";return false;}
  if(ram)memset(ram,0xFF,128*1024);memset(frame,0,pixels*sizeof(uint16_t));
  int err=nesMode?nes_core_start(rom,bytes,frame):core_start(rom,bytes,ram,128*1024,frame);
  if(err){releaseGame();status="Error core: "+String(err);return false;}
  Serial.printf("ROM launched: %s, %u bytes\n",p.c_str(),(unsigned)bytes);
  perfStart=millis();perfFrames=perfShown=perfWork=perfOver=lastWork=0;lastPresented=false;
  playing=true;gameUI();return true;
}
void setup(){
  Serial.begin(115200);delay(200);
  Serial.printf("CPU MHz=%u\n",getCpuFrequencyMhz());
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
        if(hit(p,20,110,440,135)||hit(p,20,262,440,135)){nesMode=p.y>=262;scan();menu();}
      }else if(hit(p,280,0,94,50)){nesMode=false;scan();menu();}
      else if(hit(p,374,0,106,50)){nesMode=true;scan();menu();}
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
  uint8_t keys=0;bool modeNow=false;
  if(!npoints)gameTouchArmed=true;
  if(gameTouchArmed)for(int i=0;i<npoints;i++){
    Point p=points[i];
    if(Fullscreen::contains(p.x,p.y,Fullscreen::controls[0])){
      releaseGame();status="Juego cerrado";menu();wasDown=true;return;
    }
    if(Fullscreen::contains(p.x,p.y,Fullscreen::controls[1]))modeNow=true;
    for(unsigned j=2;j<sizeof(Fullscreen::controls)/sizeof(Fullscreen::controls[0]);j++)
      if(Fullscreen::contains(p.x,p.y,Fullscreen::controls[j]))keys|=Fullscreen::controls[j].key;
  }
  if(modeNow&&!modeHeld){
    aspectFit=!aspectFit;Fullscreen::configure(nesMode,aspectFit);lastPresented=false;
    Serial.printf("VIDEO mode=%s\n",aspectFit?"FIT":"FULL");
  }
  modeHeld=modeNow;
  const uint32_t period=nesMode?16667:16743;
  // Present every frame when possible, alternate only after an expensive frame.
  const bool present=!lastPresented || lastWork+500<period;
  if(nesMode)nes_core_frame(keys,present);else core_frame(keys,present);
  if(present){Fullscreen::render(lcd.getFramebuffer(),frame,keys);lcd.flush(true);perfShown++;}
  lastPresented=present;
  lastWork=micros()-tickStart;
  perfFrames++;perfWork+=lastWork;
  if(lastWork>=period)perfOver++;
  uint32_t elapsedMs=millis()-perfStart;
  if(elapsedMs>=3000){
    Serial.printf("PERF %s emu=%.1f fps display=%.1f fps work=%u us late=%u/%u heap=%u psram=%u\n",
      nesMode?"NES":"GB",perfFrames*1000.0/elapsedMs,perfShown*1000.0/elapsedMs,
      (unsigned)(perfWork/perfFrames),(unsigned)perfOver,(unsigned)perfFrames,
      (unsigned)ESP.getFreeHeap(),(unsigned)ESP.getFreePsram());
    perfStart=millis();perfFrames=perfShown=perfWork=perfOver=0;
  }
  uint32_t elapsed=micros()-tickStart;
  if(elapsed<period){
    uint32_t remaining=period-elapsed;
    if(remaining>2000)delay((remaining-1000)/1000);
    elapsed=micros()-tickStart;
    if(elapsed<period)delayMicroseconds(period-elapsed);
  }else delay(1);
}
