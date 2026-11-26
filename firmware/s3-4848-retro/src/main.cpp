#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#ifdef PRISMA_COMBINED
#include <freertos/queue.h>
#include "prisma_diag.h"
#endif
#include <vector>
#include "gb_core.h"
#ifdef PRISMA_COMBINED
bool dos_start(Arduino_RGB_Display&); void dos_tick(); void dos_key(uint8_t); void dos_mouse(int,int,bool); bool dos_launcher_touch(unsigned,int,int); void dos_show_launcher(); bool dos_in_launcher(); void dos_request_exit(); void dos_touch_game(unsigned,int,int);
void dos_report_reset();
bool dos_diag_bench();
bool dos_prepare_restart();
static bool dosActive=false;
#endif
#include "builtin_roms.h"
#include "nes_core.h"
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include "private_roms.h"
#include "async_video.h"
#include "retro_ui.h"
#include "menu_canvas.h"
#include "preview_art.h"

static Arduino_SWSPI panelBus(GFX_NOT_DEFINED,39,48,47,GFX_NOT_DEFINED);
static Arduino_ESP32RGBPanel rgb(18,17,16,21,
  11,12,13,14,0, 8,20,3,46,9,10, 4,5,6,7,15,
  1,10,8,50,1,10,8,20,0,8000000);
static Arduino_RGB_Display lcd(480,480,&rgb,0,false,&panelBus,GFX_NOT_DEFINED,
  st7701_type9_init_operations,sizeof(st7701_type9_init_operations));

static MenuCanvas menuCanvas;
static Arduino_GFX& ui(){if(menuCanvas.ensure())return menuCanvas;return lcd;}
struct UIRect {int x,y,w,h;};
static uint16_t uiPatch[252*16];
static uint32_t uiWrites=0,uiLastWrite=0,uiLastIdle=0;
static void showUI(){
 uint32_t t=micros();
 if(menuCanvas.getFramebuffer())lcd.draw16bitRGBBitmap(0,0,menuCanvas.getFramebuffer(),480,480);
 lcd.flush(true);
 uiWrites++;uiLastWrite=millis();
 Serial.printf("UI PRESENT full pixels=230400 canvas=%u us=%u writes=%u\n",menuCanvas.getFramebuffer()!=nullptr,(unsigned)(micros()-t),uiWrites);
}
static void showUIRects(const UIRect* rects,size_t count){
 uint32_t t=micros(),pixels=0;
 uint16_t* src=menuCanvas.getFramebuffer();
 for(size_t i=0;i<count;i++){
  const auto& q=rects[i];
  if(q.x<0||q.y<0||q.w<=0||q.w>252||q.h<=0||q.x+q.w>480||q.y+q.h>480)continue;
  pixels+=q.w*q.h;
  if(!src)continue; // fallback primitives already drawn into LCD
  for(int y=0;y<q.h;y+=16){
   int rows=min(16,q.h-y);
   for(int j=0;j<rows;j++)memcpy(uiPatch+j*q.w,src+(q.y+y+j)*480+q.x,q.w*2);
   lcd.draw16bitRGBBitmap(q.x,q.y+y,uiPatch,q.w,rows);
  }
 }
 if(!pixels)return;
 // Keep the validated 029 cache flush method; only the bitmap copies are regional.
 lcd.flush(true);uiWrites++;uiLastWrite=millis();
 Serial.printf("UI PRESENT dirty rects=%u pixels=%u us=%u writes=%u\n",(unsigned)count,pixels,(unsigned)(micros()-t),uiWrites);
}
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
static unsigned frameSeq=0,nextPresent=0;
static uint32_t imageHash=0,imageChanges=0,imageSamples=0;
static uint8_t diagKeys=0;
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
#ifdef PRISMA_COMBINED
struct DosTouchSample {uint8_t n;Point p;bool rightPair;uint32_t at;};
RTC_NOINIT_ATTR static uint32_t dosReturnMagic;
RTC_NOINIT_ATTR static uint32_t dosReturnCheck;
static constexpr uint32_t DOS_RETURN_MAGIC=0x44534c52u;
static constexpr uint32_t NES_RETURN_MAGIC=0x4e455352u;
static bool rebootTouchGuard=false;
static uint32_t rebootReleaseAt=0;
static void restartToNes(){
 dosReturnMagic=NES_RETURN_MAGIC;dosReturnCheck=~NES_RETURN_MAGIC;
 diag_logf(false,"GAME EXIT RESET target=NES heap=%u psram=%u\n",(unsigned)ESP.getFreeHeap(),(unsigned)ESP.getFreePsram());
 Serial.flush();ESP.restart();
}
static void dos_quiesce_for_restart();
void dos_restart_to_selector(){
 dos_quiesce_for_restart();
 dosReturnMagic=DOS_RETURN_MAGIC;dosReturnCheck=~DOS_RETURN_MAGIC;
 diag_logf(false,"GAME EXIT RESET target=DOS heap=%u psram=%u\n",(unsigned)ESP.getFreeHeap(),(unsigned)ESP.getFreePsram());
 Serial.flush();ESP.restart();
}
// Consume the held exit touch; require a stable released screen on reboot.
static bool guardRebootTouch(unsigned n){
 if(!rebootTouchGuard)return false;
 if(n){rebootReleaseAt=0;return true;}
 if(!rebootReleaseAt)rebootReleaseAt=millis();
 if(millis()-rebootReleaseAt>=250)rebootTouchGuard=false;
 return true;
}
static void restartToDos(bool toDos){dosReturnMagic=toDos?DOS_RETURN_MAGIC:0;dosReturnCheck=toDos?~DOS_RETURN_MAGIC:0;Serial.printf("DOS GESTURE restart target=%s\n",toDos?"DOS selector":"HOME");Serial.flush();ESP.restart();}
static QueueHandle_t dosTouchQueue=nullptr;
static DosTouchSample dosTouchCurrent{};
static uint32_t dosTouchDrops=0;
static bool dosTouchStop=false,dosTouchStopped=false;
static void dosTouchWorker(void*){
  DosTouchSample previous{255,{0,0},false,0};
  uint32_t sentAt=0;
  for(;;){
    if(__atomic_load_n(&dosTouchStop,__ATOMIC_ACQUIRE)){__atomic_store_n(&dosTouchStopped,true,__ATOMIC_RELEASE);vTaskDelete(nullptr);return;}
    pollTouch();
    DosTouchSample sample{npoints,npoints?points[0]:Point{},npoints>=2&&points[0].x>=240&&points[1].x>=240,millis()};
    if(sample.n!=previous.n||sample.p.x!=previous.p.x||sample.p.y!=previous.p.y||sample.rightPair!=previous.rightPair||millis()-sentAt>=40){
      if(xQueueSend(dosTouchQueue,&sample,pdMS_TO_TICKS(5))==pdTRUE){previous=sample;sentAt=millis();}
      else __atomic_fetch_add(&dosTouchDrops,1,__ATOMIC_RELAXED);
    }
    vTaskDelay(pdMS_TO_TICKS(8));
  }
}
static void startDosTouch(){
  __atomic_store_n(&dosTouchStop,false,__ATOMIC_RELEASE);__atomic_store_n(&dosTouchStopped,false,__ATOMIC_RELEASE);
  dosTouchQueue=xQueueCreate(32,sizeof(DosTouchSample));
  if(dosTouchQueue&&xTaskCreatePinnedToCore(dosTouchWorker,"dosTouch",3072,nullptr,4,nullptr,0)!=pdPASS){vQueueDelete(dosTouchQueue);dosTouchQueue=nullptr;}
  Serial.printf("DOS TOUCH worker=%s\n",dosTouchQueue?"core0":"sync");
}
static void dos_quiesce_for_restart(){
 bool touchIdle=true;
 if(dosTouchQueue){
   __atomic_store_n(&dosTouchStop,true,__ATOMIC_RELEASE);
   uint32_t started=millis();
   while(!__atomic_load_n(&dosTouchStopped,__ATOMIC_ACQUIRE)&&millis()-started<400)delay(5);
   touchIdle=__atomic_load_n(&dosTouchStopped,__ATOMIC_ACQUIRE);
 }
 bool videoIdle=dos_prepare_restart();
 diag_logf(false,"DOS RESTART touch_idle=%d video_idle=%d\n",(int)touchIdle,(int)videoIdle);
 if(touchIdle&&videoIdle){
   digitalWrite(38,LOW);
#ifdef PRISMA_RGB_SHUTDOWN
   esp_err_t rc=rgb.prismaStopForReset();
   diag_logf(false,"DOS RESTART rgb_stop=%d\n",(int)rc);
#else
   diag_logf(false,"DOS RESTART rgb_stop unavailable\n");
#endif
   delay(80);
 }else diag_logf(false,"DOS RESTART shutdown timeout; forced software reset\n");
}
void dos_restart_home(){
 dos_quiesce_for_restart();dosReturnMagic=dosReturnCheck=0;
 diag_logf(false,"GAME EXIT RESET target=HOME\n");Serial.flush();ESP.restart();
}
#endif
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
 paths.clear();page=0;selected=0;
 if(nesMode){
 const char* order[]={"64-in-1","Super Mario Bros.","1200-in-1"};
 for(auto name:order){int i=private_rom_find(name);if(i>=0&&leanPrivate(i))paths.push_back(String("#")+name);}
 for(unsigned i=0;i<private_rom_count();i++)if(leanPrivate(i)){bool seen=false;for(auto name:order)if(!strcmp(name,private_rom_name(i)))seen=true;if(!seen)paths.push_back(String("#")+private_rom_name(i));}
 }
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
#ifdef PRISMA_COMBINED
 counts[0]=3;
#endif
 for(unsigned i=0;i<private_rom_count();i++)if(leanPrivate(i))counts[1]++;
 RetroUI::home(ui(),counts);showUI();
}
static UIRect cardRect(size_t row){return {12,76+(int)row*76,192,66};}
static constexpr UIRect previewRect={216,76,252,296},layoutRect={162,420,112,48};
static void drawCartRow(Arduino_GFX& d,size_t row){
 const auto&t=RetroUI::themes[systemId()];auto q=cardRect(row);
 d.fillRect(q.x,q.y,q.w,q.h,t.bg);
 size_t idx=page*4+row;if(idx>=paths.size())return;
 bool on=idx==selected;String n=displayName(paths[idx]);
 uint16_t card=on?t.card:0x2124,ink=on?t.ink:0x9cf3,edge=on?t.edge:0x4a49;
 d.fillRect(q.x,q.y,q.w,q.h,card);d.drawRect(q.x,q.y,q.w,q.h,edge);
 if(on){d.drawRect(q.x+1,q.y+1,q.w-2,q.h-2,edge);d.fillRect(18,q.y+9,4,48,edge);}
 RetroUI::text(d,29,q.y+13,n.substring(0,14),2,ink);
 if(n.length()>14)RetroUI::text(d,29,q.y+36,n.substring(14,28),2,ink);
}
static void drawPreview(Arduino_GFX& d){
 int sys=systemId();const auto&t=RetroUI::themes[sys];
 d.fillRect(216,76,252,296,t.card);d.drawRect(216,76,252,296,t.edge);
 if(paths.empty()||selected>=paths.size())return;
 String n=displayName(paths[selected]);const uint16_t* art=nullptr;
 for(const auto& e:PreviewArt::entries)if(n==e.name){art=e.pixels;break;}
 d.fillRect(222,82,240,180,0x0842);
 if(art)d.draw16bitRGBBitmap(222,82,const_cast<uint16_t*>(art),240,180);
 else RetroUI::cart(d,305,98,74,144,sys,RetroUI::seed(n));
 RetroUI::text(d,226,270,art?"IMAGEN DE REFERENCIA":"SIN PREVIEW",1,t.muted);
 RetroUI::text(d,226,290,n.substring(0,19),2,t.ink);
 if(n.length()>19)RetroUI::text(d,226,311,n.substring(19,38),2,t.ink);
 RetroUI::text(d,226,342,String((unsigned)(romSize(paths[selected])/1024))+" KiB / "+t.bits,1,t.muted);
}
static void drawLayout(Arduino_GFX& d){
 const auto&t=RetroUI::themes[systemId()];
 d.fillRect(layoutRect.x,layoutRect.y,layoutRect.w,layoutRect.h,t.bg);
 RetroUI::button(d,162,420,112,48,layoutNames[layoutMode],t);
}
static size_t pageSize(){
#ifdef PRISMA_COMBINED
 if(nesMode)return 1;
#endif
 return 4;
}
static void fixSelection(){
 const size_t n=pageSize();if(paths.empty())selected=0;else if(selected>=paths.size()||selected/n!=page)selected=page*n;
}
#ifdef PRISMA_COMBINED
static void nesCards(Arduino_GFX& d){
 const auto&t=RetroUI::themes[1];
 d.fillScreen(0x0842);RetroUI::button(d,12,10,90,44,"HOME",t);
 RetroUI::text(d,114,10,"PRISMA NES",3,0xffff);
 RetroUI::text(d,114,39,String(paths.size())+" JUEGOS",1,t.muted);
 if(page>=paths.size())return;
 String n=displayName(paths[page]);const uint16_t* art=nullptr;
 for(const auto&e:PreviewArt::entries)if(n==e.name){art=e.pixels;break;}
 d.fillRoundRect(56,70,368,344,8,0x2124);d.drawRoundRect(56,70,368,344,8,0x5e9e);
 if(art){static uint16_t line[344];for(int y=0;y<258;y++){for(int x=0;x<344;x++)line[x]=art[(y*180/258)*240+x*240/344];d.draw16bitRGBBitmap(68,82+y,line,344,1);}}
 else RetroUI::cart(d,176,95,128,228,1,RetroUI::seed(n));
 RetroUI::center(d,240,352,n,2,0xffff);
 RetroUI::center(d,240,382,String((unsigned)(romSize(paths[page])/1024))+" KiB / NES",1,t.muted);
 for(int side=0;side<2;side++){int x=side?430:6;bool enabled=side?page+1<paths.size():page>0;uint16_t ink=enabled?0xffff:0x4208;
 d.fillRoundRect(x,184,44,84,6,enabled?0x39a7:0x18c3);d.drawRoundRect(x,184,44,84,6,ink);RetroUI::text(d,x+10,211,side?">":"<",4,ink);}
 RetroUI::button(d,12,436,150,32,layoutNames[layoutMode],t);
 RetroUI::center(d,240,444,String(page+1)+"/"+String(paths.size()),2,t.muted);
 RetroUI::button(d,318,436,150,32,"JUGAR",t,true);
}
#endif
static void menu(){
 home=false;detail=false;int sys=systemId();const auto&t=RetroUI::themes[sys];auto& d=ui();
 fixSelection();
#ifdef PRISMA_COMBINED
 if(nesMode){nesCards(d);showUI();return;}
#endif
 RetroUI::bg(d,sys);
 RetroUI::button(d,12,10,90,44,"HOME",t);
 RetroUI::text(d,114,12,t.name,3,t.ink);
 RetroUI::text(d,114,39,String(paths.size())+" CARTUCHOS",1,t.muted);
 for(size_t i=0;i<4;i++)drawCartRow(d,i);
 drawPreview(d);RetroUI::text(d,14,390,status.substring(0,45),1,t.muted);
 RetroUI::button(d,12,420,68,48,"<",t);RetroUI::button(d,86,420,68,48,">",t);
 drawLayout(d);RetroUI::button(d,282,420,186,48,"JUGAR",t,true);
 showUI();
}
static void changeCart(size_t idx){
 if(idx>=paths.size()||idx/4!=page||idx==selected)return;
 size_t old=selected;selected=idx;auto& d=ui();
 UIRect rects[3];size_t n=0;
 if(old/4==page&&old<paths.size()){drawCartRow(d,old%4);rects[n++]=cardRect(old%4);}
 drawCartRow(d,idx%4);rects[n++]=cardRect(idx%4);
 drawPreview(d);rects[n++]=previewRect;showUIRects(rects,n);
}
static void changePage(){
#ifdef PRISMA_COMBINED
 if(nesMode){menu();return;}
#endif
 fixSelection();auto& d=ui();UIRect rects[5];
 for(size_t i=0;i<4;i++){drawCartRow(d,i);rects[i]=cardRect(i);}
 drawPreview(d);rects[4]=previewRect;showUIRects(rects,5);
}
static void changeLayout(){
 layoutMode=(layoutMode+1)%3;
#ifdef PRISMA_COMBINED
 if(nesMode){menu();return;}
#endif
 drawLayout(ui());showUIRects(&layoutRect,1);
}
static void gameUI(){
 lcd.fillScreen(0);lcd.flush(true);GameVideo::reset();overlay=false;overlayArmed=false;launchRelease=true;overlayMs=0;
}
static void releaseGame(){
 markPhase(0);AsyncVideo::drain();
 Serial.printf("HEAP BEFORE STOP ok=%u\n",heap_caps_check_integrity_all(true));
 if(nesMode)nes_core_stop();
 free(rom);free(ram);free(frame);rom=nullptr;ram=nullptr;frame=nullptr;playing=false;
 Serial.printf("HEAP AFTER STOP ok=%u\n",heap_caps_check_integrity_all(true));
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
 if(nesMode)Serial.printf("NES CART mapper=%u prg=%u KiB chr=%u KiB flags=%02x/%02x\n",(rom[6]>>4)|(rom[7]&0xf0),rom[4]*16,rom[5]*8,rom[6],rom[7]);
 int err=nesMode?nes_core_start(rom,bytes,nullptr):core_start(rom,bytes,ram,128*1024,frame);
 if(err){releaseGame();status="Error core: "+String(err);return false;}
 Serial.printf("ROM launched: %s, %u bytes\n",p.c_str(),(unsigned)bytes);
 Serial.printf("MEM ACTIVE heap=%u psram=%u\n",(unsigned)ESP.getFreeHeap(),(unsigned)ESP.getFreePsram());
#ifdef PRISMA_COMBINED
 if(nesMode){int vw,vh,vp;uint16_t *vpal;uint8_t *vpix=nes_core_video(&vw,&vh,&vp,&vpal);
  Serial.printf("NES MEMORY video=%s palette=%s snapshot=%s pitch=%d\n",vpix?(esp_ptr_internal(vpix)?"INTERNAL":"PSRAM"):"NONE",vpal?(esp_ptr_internal(vpal)?"INTERNAL":"PSRAM"):"NONE",AsyncVideo::snapshot?(esp_ptr_internal(AsyncVideo::snapshot)?"INTERNAL":"PSRAM"):"NONE",vp);
 }
 if(nesMode)Serial.printf("NES BASELINE cpu_mhz=%u reset=%d dos_active=%d dos_touch=%d tasks=%u internal_free=%u internal_largest=%u psram_largest=%u\n",getCpuFrequencyMhz(),(int)esp_reset_reason(),(int)dosActive,dosTouchQueue!=nullptr,(unsigned)uxTaskGetNumberOfTasks(),(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
#endif
 perfStart=millis();perfFrames=perfShown=perfWork=perfOver=lastWork=0;frameSeq=nextPresent=0;imageHash=imageChanges=imageSamples=0;diagKeys=0;
 queuedFrames=droppedFrames=drawUs=coreUs=0;uint32_t us,n;AsyncVideo::stats(us,n);
 playing=true;gameUI();maxWork=0;markPhase(1);
 Serial.printf("RUN BUILD 031 %s divisor=%u layout=%s\n",nesMode?"NES":"GB",frameDiv,layoutNames[layoutMode]);
 frameDeadline=micros();return true;
}
#ifdef PRISMA_COMBINED
void prisma_diag_status(){
 Serial.printf("PRISMA STATUS build=059R7 mode=%s playing=%d cpu_mhz=%u uptime_ms=%u reset=%d\n",dosActive?"DOS":playing?(nesMode?"NES":"GB"):home?"HOME":"SELECTOR",playing|| (dosActive&&!dos_in_launcher()),getCpuFrequencyMhz(),millis(),(int)esp_reset_reason());
}
bool prisma_diag_bench(){return dosActive&&dos_diag_bench();}
#endif
void setup(){
#ifdef PRISMA_COMBINED
  Serial.setRxBufferSize(2048);
#endif
  Serial.begin(115200);delay(200);
#ifdef PRISMA_COMBINED
  Serial.printf("BUILD 059R7 RESET reason=%d\n",(int)esp_reset_reason());
  diag_begin();dos_report_reset();
#endif
  Serial.printf("CPU MHz=%u\n",getCpuFrequencyMhz());
  Serial.printf("PRISMA RETRO S3 flash=%u PSRAM=%u\n",(unsigned)ESP.getFlashChipSize(),(unsigned)ESP.getPsramSize());
  pinMode(38,OUTPUT);digitalWrite(38,LOW);
  pinMode(42,OUTPUT);digitalWrite(42,HIGH);
  if(!lcd.begin()){Serial.println("LCD init failed");return;}
  lcd.fillScreen(BG);lcd.flush(true);digitalWrite(38,HIGH);Serial.println("LCD SAFE single framebuffer / manual flush / pclk 8MHz");
  Serial.printf("LCD worker core0=%s\n",AsyncVideo::begin(lcd,BG)?"ready":"fallback sync");
  xTaskCreatePinnedToCore(stallMonitor,"retroMon",3072,nullptr,1,nullptr,0);
  Wire1.begin(19,45,400000);Wire1.setTimeOut(10);
  for(uint8_t a:{0x5D,0x14}){Wire1.beginTransmission(a);if(!Wire1.endTransmission()){addr=a;break;}}
  Serial.printf("GT911=0x%02x\n",addr);
  pinMode(39,OUTPUT);digitalWrite(39,HIGH);
  Serial.println("BOOT 031 private ROM init");private_rom_init();
  Serial.printf("BOOT 031 private ROM count=%u\n",private_rom_count());
#ifdef PRISMA_COMBINED
  const bool returnToDos=esp_reset_reason()==ESP_RST_SW&&dosReturnMagic==DOS_RETURN_MAGIC&&dosReturnCheck==~DOS_RETURN_MAGIC;
  const bool returnToNes=esp_reset_reason()==ESP_RST_SW&&dosReturnMagic==NES_RETURN_MAGIC&&dosReturnCheck==~NES_RETURN_MAGIC;
  dosReturnMagic=dosReturnCheck=0;
  if(returnToNes){
    Serial.println("GAME RESET COMPLETE target=NES");
    rebootTouchGuard=true;splashVisible=false;nesMode=true;scan();menu();return;
  }
  if(returnToDos){
    rebootTouchGuard=true;
    Serial.println("DOS GESTURE boot directly to DOS selector");
    AsyncVideo::drain();menuCanvas.release();dosActive=dos_start(lcd);
    if(dosActive){startDosTouch();return;}
    Serial.println("DOS GESTURE init failed; reboot HOME");Serial.flush();ESP.restart();
  }
#endif
  Serial.println("BOOT 031 splash begin");RetroUI::splash(ui());showUI();
  Serial.println("BOOT 031 splash ready / waiting touch");
  Serial.printf("BOOT NES library: %u private ROMs, %u entries\n",private_rom_count(),(unsigned)paths.size());
}
void loop(){
  const uint32_t tickStart=micros();
  if(!addr){delay(100);return;}
  if(playing)markPhase(1);
  #ifdef PRISMA_COMBINED
  if(!dosActive)diag_poll();
  if(dosActive){
    if(dosTouchQueue){xQueueReceive(dosTouchQueue,&dosTouchCurrent,0);}else{pollTouch();dosTouchCurrent={npoints,npoints?points[0]:Point{},npoints>=2&&points[0].x>=240&&points[1].x>=240,millis()};}
    const bool touchFresh=millis()-dosTouchCurrent.at<=180;
    const unsigned npoints=touchFresh?dosTouchCurrent.n:0;
    const Point points[1]={dosTouchCurrent.p};
    if(guardRebootTouch(touchFresh?npoints:1)){dos_tick();return;}
    static uint32_t touchStats=0;
    if(millis()-touchStats>=3000){touchStats=millis();Serial.printf("DOS TOUCH queue=%u dropped=%u\n",dosTouchQueue?(unsigned)uxQueueMessagesWaiting(dosTouchQueue):0,(unsigned)__atomic_load_n(&dosTouchDrops,__ATOMIC_RELAXED));}
    // A single touch belongs to the game (including the top-right).
    // Exit only through the toolbar or a deliberate two-finger right
    // hold, confirmed by a fresh release. Never reboot on stale input.
    static uint32_t exitSince=0,partialReleaseAt=0;
    static bool gesture=false,gestureValid=false,gestureArmed=false;
    static bool prev=false;static Point last{};
    if(npoints>=2){
      if(!gesture){gesture=true;gestureValid=touchFresh&&dosTouchCurrent.rightPair&&!dos_in_launcher();gestureArmed=false;partialReleaseAt=0;exitSince=dosTouchCurrent.at;}
      if(!touchFresh||!dosTouchCurrent.rightPair||dos_in_launcher()||partialReleaseAt)gestureValid=false;
      if(gestureValid&&!gestureArmed&&uint32_t(dosTouchCurrent.at-exitSince)>=1500){
        gestureArmed=true;diag_logf(false,"DOS EXIT gesture armed; release to confirm\n");
      }
      dos_touch_game(2,0,0);dos_launcher_touch(npoints,0,0);prev=false;dos_tick();return;
    }
    if(gesture){
      if(!touchFresh){gestureValid=false;gestureArmed=false;dos_tick();return;}
      if(npoints){if(!partialReleaseAt)partialReleaseAt=dosTouchCurrent.at;if(!gestureArmed||uint32_t(dosTouchCurrent.at-partialReleaseAt)>250)gestureValid=false;dos_touch_game(2,0,0);dos_tick();return;}
      bool exit=gestureValid&&gestureArmed&&!dos_in_launcher()&&(!partialReleaseAt||uint32_t(dosTouchCurrent.at-partialReleaseAt)<=250);
      gesture=gestureValid=gestureArmed=false;
      if(exit){diag_logf(false,"DOS EXIT cause=two_finger_release\n");dos_request_exit();return;}
      dos_touch_game(0,0,0);dos_launcher_touch(0,0,0);dos_tick();return;
    }
    if(dos_launcher_touch(npoints,npoints?points[0].x:0,npoints?points[0].y:0)){prev=false;dos_tick();return;}
    dos_touch_game(npoints,npoints?points[0].x:0,npoints?points[0].y:0);dos_tick();return;
  }
#endif
  pollTouch();
#ifdef PRISMA_COMBINED
  if(guardRebootTouch(npoints)){delay(12);return;}
#endif
  if(splashVisible){
    if(npoints)splashPressed=true;
    else if(splashPressed){splashVisible=false;wasDown=false;Serial.println("BOOT 031 home begin");homeUI();Serial.println("BOOT 031 home ok");}
    delay(12);return;
  }
  if(!playing){
    if(npoints){if(!wasDown)gestureStart=points[0];gestureEnd=points[0];}
    if(!npoints&&wasDown){
      Point p=gestureStart;int dy=(int)gestureEnd.y-(int)p.y;int dx=(int)gestureEnd.x-(int)p.x;
      if(home){
#ifdef PRISMA_COMBINED
      if(abs(dy)<36&&abs(dx)<36&&hit(p,16,90,448,148)){
        AsyncVideo::drain();menuCanvas.release();
        dosActive=dos_start(lcd);
        if(!dosActive){Serial.println("DOS RESET REQUEST: init failed");Serial.flush();delay(1500);ESP.restart();}
        else startDosTouch();
        wasDown=false;return;
      }
#endif
      if(abs(dy)<36&&abs(dx)<36&&p.x>=16&&p.x<464&&p.y>=90&&p.y<410){int s=(p.y-90)/164;if(s<2&&(p.y-90)%164<148){nesMode=s==1;scan();menu();}}}
#ifdef PRISMA_COMBINED
      else if(nesMode&&abs(dx)>55&&abs(dx)>abs(dy)){
        size_t old=page;if(dx<0&&page+1<paths.size())page++;else if(dx>0&&page)page--;
        if(old!=page)changePage();
      }
#endif
      else if(abs(dy)>55&&abs(dy)>abs(dx)
#ifdef PRISMA_COMBINED
        &&!nesMode
#endif
      ){
        size_t old=page;if(dy<0&&(page+1)*pageSize()<paths.size())page++;else if(dy>0&&page)page--;
        if(old!=page)changePage();
      }else if(abs(dy)<36&&abs(dx)<36){
#ifdef PRISMA_COMBINED
        if(nesMode){
          if(hit(p,12,10,90,44))homeUI();
          else if(hit(p,6,184,44,84)){if(page){page--;changePage();}}
          else if(hit(p,430,184,44,84)){if(page+1<paths.size()){page++;changePage();}}
          else if(hit(p,12,436,150,32))changeLayout();
          else if(hit(p,318,436,150,32)||hit(p,56,70,368,344)){
            if(page<paths.size()){
              selected=page;auto& d=ui();const auto&t=RetroUI::themes[1];RetroUI::bg(d,1);RetroUI::center(d,240,210,"CARGANDO...",3,t.ink);showUI();menuCanvas.release();
              if(!load(paths[selected]))menu();
            }
          }
        }else
#endif
        if(hit(p,12,10,90,44))homeUI();
        else if(hit(p,12,420,68,48)){if(page){page--;changePage();}}
        else if(hit(p,86,420,68,48)){if((page+1)*4<paths.size()){page++;changePage();}}
        else if(hit(p,162,420,112,48)){changeLayout();}
        else if(hit(p,282,420,186,48)&&selected<paths.size()){
          auto& d=ui();const auto&t=RetroUI::themes[systemId()];RetroUI::bg(d,systemId());RetroUI::center(d,240,210,"CARGANDO...",3,t.ink);showUI();menuCanvas.release();
          if(!load(paths[selected]))menu();
        }else if(p.x>=12&&p.x<204&&p.y>=76&&p.y<380&&(p.y-76)%76<66){
          size_t i=page*4+(p.y-76)/76;changeCart(i);
        }
      }
    }
    wasDown=npoints>0;
    if(!npoints&&millis()-uiLastWrite>=5000&&millis()-uiLastIdle>=5000){uiLastIdle=millis();Serial.printf("UI IDLE writes=%u no_draw_ms=%u screen=%s\n",uiWrites,(unsigned)(millis()-uiLastWrite),home?"HOME":"LIBRARY");}
    delay(12);return;
  }
  uint8_t keys=0;
  bool exit=false;
  if(launchRelease){if(!npoints)launchRelease=false;}
  else if(npoints){
    overlayMs=millis();
    if(!overlay){overlay=true;overlayArmed=false;}
    else if(overlayArmed){for(int i=0;i<npoints;i++)keys|=GameVideo::input(points[i].x,points[i].y,exit);}
  }else if(overlay){overlayArmed=true;if(millis()-overlayMs>=3000){overlay=false;overlayArmed=false;}}
  if(exit){
#ifdef PRISMA_COMBINED
    if(nesMode){restartToNes();return;}
#endif
    releaseGame();status="Juego cerrado";menu();wasDown=true;gestureStart=gestureEnd={0,0};return;
  }
  const GameVideo::Options videoOptions{layoutMode,overlay};
  const uint32_t period=nesMode?16667:16743;
  const bool due=(int32_t)(frameSeq-nextPresent)>=0;
  frameSeq++;
  const bool async=AsyncVideo::enabled();
  const bool present=due&&(!async||AsyncVideo::ready());
  // Retry on the very next guest frame when LCD is busy, not another fixed pair.
  if(due&&!present)droppedFrames++;
  if(present)nextPresent=frameSeq+frameDiv-1;
  diagKeys=keys;
  markPhase(2);uint32_t t=micros();
  if(nesMode)nes_core_frame(keys,present);else core_frame(keys,present);
  coreUs+=micros()-t;
  // Sparse independent guest image signature: diagnose apparent freezes vs LCD stalls.
  if(nesMode){int dw,dh,dp;uint16_t* dpal;uint8_t* pix=nes_core_video(&dw,&dh,&dp,&dpal);
   if(pix&&dpal){uint32_t hash=2166136261u;for(int yy=0;yy<dh;yy+=16)for(int xx=0;xx<dw;xx+=16)hash=(hash^dpal[pix[yy*dp+xx]])*16777619u;
    if(imageSamples&&hash!=imageHash)imageChanges++;imageHash=hash;imageSamples++;}
  }
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
#ifdef PRISMA_COMBINED
    diag_logf(true,"PERF %s emu=%.1f fps display=%.1f fps work=%u us late=%u/%u heap=%u psram=%u\n",nesMode?"NES":"GB",perfFrames*1000.0/elapsedMs,perfShown*1000.0/elapsedMs,(unsigned)(perfWork/perfFrames),(unsigned)perfOver,(unsigned)perfFrames,(unsigned)ESP.getFreeHeap(),(unsigned)ESP.getFreePsram());
#else
    Serial.printf("PERF %s emu=%.1f fps display=%.1f fps work=%u us late=%u/%u heap=%u psram=%u\n",nesMode?"NES":"GB",perfFrames*1000.0/elapsedMs,perfShown*1000.0/elapsedMs,(unsigned)(perfWork/perfFrames),(unsigned)perfOver,(unsigned)perfFrames,(unsigned)ESP.getFreeHeap(),(unsigned)ESP.getFreePsram());
#endif
#ifdef PRISMA_COMBINED
    diag_logf(true,"VIDEO PERF core=%u us lcd=%u us submit=%u us fixed_div=%u queued=%u dropped=%u maxWork=%u\n",(unsigned)(coreUs/perfFrames),(unsigned)(async?(workerCount?workerUs/workerCount:0):(perfShown?drawUs/perfShown:0)),(unsigned)(queuedFrames?drawUs/queuedFrames:0),frameDiv,queuedFrames,droppedFrames,maxWork);
#else
    Serial.printf("VIDEO PERF core=%u us lcd=%u us submit=%u us fixed_div=%u queued=%u dropped=%u maxWork=%u\n",(unsigned)(coreUs/perfFrames),(unsigned)(async?(workerCount?workerUs/workerCount:0):(perfShown?drawUs/perfShown:0)),(unsigned)(queuedFrames?drawUs/queuedFrames:0),frameDiv,queuedFrames,droppedFrames,maxWork);
#endif
    if(nesMode)Serial.printf("NES IMAGE samples=%u changes=%u hash=%08x keys=%02x\n",imageSamples,imageChanges,imageHash,diagKeys);
    imageSamples=imageChanges=0;
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
