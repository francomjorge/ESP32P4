#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <vector>
#include "gb_core.h"
#include "builtin_roms.h"
#include "nes_core.h"
#include <esp_partition.h>
#include "private_roms.h"
#include "async_video.h"
#include "retro_ui.h"

static Arduino_SWSPI panelBus(GFX_NOT_DEFINED,39,48,47,GFX_NOT_DEFINED);
static Arduino_ESP32RGBPanel rgb(18,17,16,21,
  11,12,13,14,0, 8,20,3,46,9,10, 4,5,6,7,15,
  1,10,8,50,1,10,8,20,0,8000000);
static Arduino_RGB_Display lcd(480,480,&rgb,0,true,&panelBus,GFX_NOT_DEFINED,
  st7701_type9_init_operations,sizeof(st7701_type9_init_operations));

static const uint16_t BG=0x0842,FG=0xFFFF,ACC=0x5E9E,CARD=0x2124;
static uint8_t addr=0, *rom=nullptr,*ram=nullptr;
static uint8_t *frame=nullptr;
static std::vector<String> paths;
static bool nesMode=false,home=true;
static bool splashVisible=true,splashPressed=false;
static bool detail=false,overlay=false,overlayArmed=false,launchRelease=false;
static uint32_t overlayMs=0;
static uint8_t layoutMode=GameVideo::FULL;
static const char* layoutNames[]={"FULL","FIT","NATIVE"};
static size_t selected=0;
static uint32_t phase=0,heartbeat=0;
static uint32_t maxWork=0;
static void markPhase(uint32_t p){__atomic_store_n(&phase,p,__ATOMIC_RELAXED);}
static void stallMonitor(void*){
  uint32_t prev=0;
  for(;;){
    vTaskDelay(pdMS_TO_TICKS(1000));
    uint32_t now=__atomic_load_n(&heartbeat,__ATOMIC_RELAXED);
    uint32_t p=__atomic_load_n(&phase,__ATOMIC_RELAXED);
    if(p&&now==prev)Serial.printf("STALL phase=%u frame=%u lcdBusy=%u\n",p,now,!AsyncVideo::ready());
    prev=now;
  }
}
static bool playing=false,wasDown=false;
static uint32_t perfStart=0,perfFrames=0,perfShown=0,perfWork=0,perfOver=0,lastWork=0;
static bool lastPresented=false;
static constexpr unsigned frameDiv=2;
static unsigned frameSeq=0;
static uint32_t queuedFrames=0,droppedFrames=0;
static uint32_t frameDeadline=0;
static int shownW=0,shownH=0;
static uint32_t drawUs=0,coreUs=0;
static size_t page=0;
static String status;
struct Point{uint16_t x,y;};
static Point points[5],gestureStart{},gestureEnd{};
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
// Fixed five-title benchmark library. No SD and no Mega Drive.
static bool leanPrivate(unsigned i){
 const char* n=private_rom_name(i);
 return !private_rom_md(i)&&(!strcmp(n,"64-in-1")||!strcmp(n,"1200-in-1")||!strcmp(n,"Super Mario Bros.")||!strcmp(n,"Super Mario Bros. 3")||!strcmp(n,"Donkey Kong Classics")||!strcmp(n,"Tetris"));
}
static void scan(){
 paths.clear();page=0;
 if(nesMode)for(unsigned i=0;i<private_rom_count();i++)if(leanPrivate(i))paths.push_back(String("#")+private_rom_name(i));
 for(const auto &g:builtins)if(g.nes==nesMode)paths.push_back(String("@")+g.name);
 status=String(paths.size())+" juegos internos";
 Serial.printf("ROM library %s: %u games\n",nesMode?"NES":"GB",(unsigned)paths.size());
}
static int systemId(){return nesMode?1:0;}
static String displayName(const String& p){return p.startsWith("@")||p.startsWith("#")?p.substring(1):p.substring(p.lastIndexOf('/')+1);}
static String origin(const String& p){return p.startsWith("@")||p.startsWith("#")?"FLASH":"SD";}
static size_t romSize(const String& p){
 if(p.startsWith("#")){int i=private_rom_find(p.substring(1).c_str());return i>=0?private_rom_size(i):0;}
 if(p.startsWith("@")){for(const auto& g:builtins)if(p.substring(1)==g.name)return g.size;return 0;}
 return 0;
}
static void homeUI(){
 home=true;detail=false;unsigned counts[2]={0,0};
 for(const auto& g:builtins)counts[g.nes?1:0]++;
 for(unsigned i=0;i<private_rom_count();i++)if(leanPrivate(i))counts[1]++;
 RetroUI::home(lcd,counts);
}
static void menu(){
 home=false;detail=false;int sys=systemId();const auto&t=RetroUI::themes[sys];
 RetroUI::bg(lcd,sys);
 RetroUI::button(lcd,14,10,94,48,"HOME",t);
 RetroUI::text(lcd,122,15,t.name,3,t.ink);
 RetroUI::text(lcd,122,43,String(t.bits)+" / "+paths.size()+" JUEGOS",2,t.muted);
 RetroUI::text(lcd,16,72,status.substring(0,36),1,t.muted);
 for(size_t i=0;i<4&&page*4+i<paths.size();i++){
  int y=92+i*80;const String& p=paths[page*4+i];String n=displayName(p);
  lcd.fillRoundRect(14,y,452,72,7,t.card);lcd.drawRoundRect(14,y,452,72,7,t.edge);
  RetroUI::cart(lcd,27,y+8,54,56,sys,RetroUI::seed(n));
  RetroUI::text(lcd,94,y+12,n.substring(0,29),2,t.ink);
  RetroUI::text(lcd,94,y+40,String(t.bits)+" / "+origin(p),2,t.muted);
  RetroUI::text(lcd,437,y+27,">",2,t.ink);
 }
 RetroUI::button(lcd,14,424,92,48,"PREV",t);
 RetroUI::button(lcd,119,424,242,48,"LITE / "+String(paths.size())+" ROMS",t);
 RetroUI::button(lcd,374,424,92,48,"NEXT",t);
}
static void detailUI(){
 if(selected>=paths.size()){menu();return;}
 detail=true;home=false;int sys=systemId();const auto&t=RetroUI::themes[sys];const String& p=paths[selected];String n=displayName(p);
 RetroUI::bg(lcd,sys);RetroUI::button(lcd,14,10,100,48,"BACK",t);
 RetroUI::text(lcd,128,20,t.name,3,t.ink);
 lcd.fillRoundRect(16,70,448,202,8,t.card);lcd.drawRoundRect(16,70,448,202,8,t.edge);
 RetroUI::cart(lcd,161,79,158,162,sys,RetroUI::seed(n));
 RetroUI::center(lcd,240,252,"CARTRIDGE / ORIGINAL ART",1,t.muted);
 RetroUI::text(lcd,20,286,n.substring(0,24),3,t.ink);
 if(n.length()>24)RetroUI::text(lcd,20,313,n.substring(24,54),2,t.ink);
 size_t bytes=romSize(p);
 RetroUI::text(lcd,20,340,String(t.bits)+" / "+origin(p)+(bytes?" / "+String((unsigned)(bytes/1024))+" KiB":""),2,t.muted);
 RetroUI::button(lcd,16,369,150,48,layoutNames[layoutMode],t);
 RetroUI::button(lcd,16,426,448,48,"JUGAR",t,true);
}
static void gameUI(){
 lcd.fillScreen(0);GameVideo::reset();overlay=false;overlayArmed=false;launchRelease=true;overlayMs=0;
}
static void releaseGame(){
 markPhase(0);AsyncVideo::drain();if(nesMode)nes_core_stop();
 free(rom);free(ram);free(frame);rom=nullptr;ram=nullptr;frame=nullptr;playing=false;
 Serial.printf("MEM RELEASE heap=%u psram=%u\n",(unsigned)ESP.getFreeHeap(),(unsigned)ESP.getFreePsram());
}
static bool load(const String &p){
 const BuiltinROM *internal=nullptr;size_t bytes=0;int privateIndex=-1;
 if(p.startsWith("#"))privateIndex=private_rom_find(p.substring(1).c_str());
 if(privateIndex>=0&&nesMode&&leanPrivate(privateIndex))bytes=private_rom_size(privateIndex);
 else if(p.startsWith("@")){
  for(const auto &g:builtins)if(p.substring(1)==g.name&&g.nes==nesMode){internal=&g;break;}
  if(internal)bytes=internal->size;
 }
 if(!bytes){status="ROM invalida";return false;}
 Serial.printf("MEM BEFORE romBytes=%u heap=%u psram=%u\n",(unsigned)bytes,(unsigned)ESP.getFreeHeap(),(unsigned)ESP.getFreePsram());
 rom=(uint8_t*)ps_malloc(bytes);
 if(!nesMode){
  ram=(uint8_t*)ps_malloc(128*1024);
  frame=(uint8_t*)heap_caps_malloc(160*144,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  if(!frame)frame=(uint8_t*)ps_malloc(160*144);
 }
 if(!rom||(!nesMode&&(!ram||!frame))){releaseGame();status="Sin memoria";return false;}
 bool ok=false;
 if(privateIndex>=0)ok=private_rom_read(privateIndex,rom,bytes);
 else if(internal){memcpy(rom,internal->data,bytes);ok=true;}
 if(!ok||(!nesMode&&rom[0x143]==0xC0)){releaseGame();status="ROM incompatible";return false;}
 if(ram)memset(ram,0xff,128*1024);if(frame)memset(frame,0,160*144);
 int err=nesMode?nes_core_start(rom,bytes,nullptr):core_start(rom,bytes,ram,128*1024,frame);
 if(err){releaseGame();status="Error core: "+String(err);return false;}
 Serial.printf("ROM launched: %s, %u bytes\n",p.c_str(),(unsigned)bytes);
 Serial.printf("MEM ACTIVE heap=%u psram=%u\n",(unsigned)ESP.getFreeHeap(),(unsigned)ESP.getFreePsram());
 perfStart=millis();perfFrames=perfShown=perfWork=perfOver=lastWork=0;frameSeq=0;
 queuedFrames=droppedFrames=drawUs=coreUs=0;uint32_t us,n;AsyncVideo::stats(us,n);
 playing=true;gameUI();maxWork=0;markPhase(1);
 Serial.printf("RUN BUILD 026 %s divisor=%u layout=%s\n",nesMode?"NES":"GB",frameDiv,layoutNames[layoutMode]);
 frameDeadline=micros();return true;
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
  xTaskCreatePinnedToCore(stallMonitor,"retroMon",3072,nullptr,1,nullptr,0);
  Wire1.begin(19,45,400000);Wire1.setTimeOut(10);
  for(uint8_t a:{0x5D,0x14}){Wire1.beginTransmission(a);if(!Wire1.endTransmission()){addr=a;break;}}
  Serial.printf("GT911=0x%02x\n",addr);
  pinMode(39,OUTPUT);digitalWrite(39,HIGH);
  Serial.println("BOOT 026 private ROM init");private_rom_init();
  Serial.printf("BOOT 026 private ROM count=%u\n",private_rom_count());
  Serial.println("BOOT 026 splash begin");RetroUI::splash(lcd);
  Serial.println("BOOT 026 splash ready / waiting touch");
  Serial.printf("BOOT NES library: %u private ROMs, %u entries\n",private_rom_count(),(unsigned)paths.size());
}
void loop(){
  const uint32_t tickStart=micros();
  if(!addr){delay(100);return;}
  if(playing)markPhase(1);
  pollTouch();
  if(splashVisible){
    if(npoints)splashPressed=true;
    else if(splashPressed){splashVisible=false;wasDown=false;Serial.println("BOOT 026 home begin");homeUI();Serial.println("BOOT 026 home ok");}
    delay(12);return;
  }
  if(!playing){
    if(npoints){if(!wasDown)gestureStart=points[0];gestureEnd=points[0];}
    if(!npoints&&wasDown){
      Point p=gestureStart;int dy=(int)gestureEnd.y-(int)p.y;int dx=(int)gestureEnd.x-(int)p.x;
      if(home){if(abs(dy)<36&&abs(dx)<36&&p.x>=16&&p.x<464&&p.y>=90&&p.y<410){int s=(p.y-90)/164;if(s<2&&(p.y-90)%164<148){nesMode=s==1;scan();menu();}}}
      else if(detail){
        if(abs(dy)<36&&abs(dx)<36){
          if(hit(p,14,10,100,48))menu();
          else if(hit(p,16,369,150,48)){layoutMode=(layoutMode+1)%3;detailUI();}
          else if(hit(p,16,426,448,48)){const auto&t=RetroUI::themes[systemId()];RetroUI::bg(lcd,systemId());RetroUI::center(lcd,240,210,"CARGANDO...",3,t.ink);if(!load(paths[selected]))menu();}
        }
      }else if(abs(dy)>55&&abs(dy)>abs(dx)){
        if(dy<0&&(page+1)*4<paths.size())page++;else if(dy>0&&page)page--;menu();
      }else if(abs(dy)<36&&abs(dx)<36){
        if(hit(p,14,10,94,48))homeUI();
        else if(hit(p,14,424,92,48)){if(page)page--;menu();}
        else if(hit(p,119,424,242,48)){/* LITE fixed library */}
        else if(hit(p,374,424,92,48)){if((page+1)*4<paths.size())page++;menu();}
        else if(p.x>=14&&p.x<466&&p.y>=92&&p.y<404&&(p.y-92)%80<72){size_t i=page*4+(p.y-92)/80;if(i<paths.size()){selected=i;detailUI();}}
      }
    }
    wasDown=npoints>0;delay(12);return;
  }
  uint8_t keys=0;
  bool exit=false;
  if(launchRelease){if(!npoints)launchRelease=false;}
  else if(npoints){
    overlayMs=millis();
    if(!overlay){overlay=true;overlayArmed=false;}
    else if(overlayArmed){for(int i=0;i<npoints;i++)keys|=GameVideo::input(points[i].x,points[i].y,exit);}
  }else if(overlay){overlayArmed=true;if(millis()-overlayMs>=3000){overlay=false;overlayArmed=false;}}
  if(exit){releaseGame();status="Juego cerrado";menu();wasDown=true;gestureStart=gestureEnd={0,0};return;}
  const GameVideo::Options videoOptions{layoutMode,overlay};
  const uint32_t period=nesMode?16667:16743;
  const bool due=frameSeq++%frameDiv==0;
  const bool async=AsyncVideo::enabled();
  const bool present=due&&(!async||AsyncVideo::ready());
  if(due&&!present)droppedFrames++;
  markPhase(2);uint32_t t=micros();
  if(nesMode)nes_core_frame(keys,present);else core_frame(keys,present);
  coreUs+=micros()-t;
  markPhase(3);
  if(present){
    t=micros();int w,h,pitch;uint16_t *pal;
    uint8_t *v=nesMode?nes_core_video(&w,&h,&pitch,&pal):core_video(&w,&h,&pitch,&pal);
    if(v&&pal){
      if(async){if(AsyncVideo::submit(v,pal,w,h,pitch,videoOptions))queuedFrames++;else droppedFrames++;}
      else{GameVideo::draw(lcd,v,pal,nullptr,w,h,pitch,videoOptions);perfShown++;}
    }
    drawUs+=micros()-t;
  }
  lastWork=micros()-tickStart;if(lastWork>maxWork)maxWork=lastWork;
  __atomic_fetch_add(&heartbeat,1,__ATOMIC_RELAXED);markPhase(4);
  perfFrames++;perfWork+=lastWork;if(lastWork>=period)perfOver++;
  uint32_t elapsedMs=millis()-perfStart;
  if(elapsedMs>=3000){
    uint32_t workerUs=0,workerCount=0;AsyncVideo::stats(workerUs,workerCount);
    if(async)perfShown=workerCount;
    Serial.printf("PERF %s emu=%.1f fps display=%.1f fps work=%u us late=%u/%u heap=%u psram=%u\n",nesMode?"NES":"GB",perfFrames*1000.0/elapsedMs,perfShown*1000.0/elapsedMs,(unsigned)(perfWork/perfFrames),(unsigned)perfOver,(unsigned)perfFrames,(unsigned)ESP.getFreeHeap(),(unsigned)ESP.getFreePsram());
    Serial.printf("VIDEO PERF core=%u us lcd=%u us submit=%u us fixed_div=%u queued=%u dropped=%u maxWork=%u\n",(unsigned)(coreUs/perfFrames),(unsigned)(async?(workerCount?workerUs/workerCount:0):(perfShown?drawUs/perfShown:0)),(unsigned)(queuedFrames?drawUs/queuedFrames:0),frameDiv,queuedFrames,droppedFrames,maxWork);
    queuedFrames=droppedFrames=coreUs=drawUs=maxWork=0;
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
  }else if((frameSeq&7)==0)delay(1);
}
