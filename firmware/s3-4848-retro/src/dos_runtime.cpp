#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_partition.h>
#include <Wire.h>
#include <Adafruit_Faux86.h>
#include <esp_timer.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#if __has_include("dos_art.h")
#include "dos_art.h"
#else
namespace DosArt {static const uint16_t *const small[5]={};static const uint16_t *const large[5]={};}
#endif

static Arduino_RGB_Display *dosLcd=nullptr;
uint16_t *vga_framebuffer=nullptr;
static bool launcher=false;
static uint32_t diskReads=0,diskHits=0,diskReadUs=0;
static bool bootPhase=true,loadingGame=false;
static int loadingIndex=-1;
static uint8_t loadStage=0;
static uint32_t captureBytes=0,benchUntil=0,benchStart=0;
static uint64_t benchInstructions=0;
static bool hideGuest=true;
static bool mouseMode=true,consoleMode=false;
static int activeGame=-1;
static uint8_t panelMode=0; // 0 mouse toolbar, 1 keyboard, 2 gamepad, 3 full, 4 Carmen text
static int panelTop(){const uint8_t p=__atomic_load_n(&panelMode,__ATOMIC_RELAXED);return p==1?228:p==2?324:p==3?480:p==4?290:432;}
static uint32_t overlayKeys=0;
static void drawPanel();
static void setPanel(uint8_t mode);
static uint32_t serialKeys=0,touchClicks=0;
static const char *gameNames[]={"Carmen Sandiego","The Secret of Monkey Island"};
static const char *gameCmds[]={"carmen","monkey"};

static void renderSnapshot();
class PrismaVideo final : public Faux86::FrameBufferInterface {
  Faux86::RenderSurface surface{};
  uint16_t *cache=nullptr;
  uint16_t xmap[480],ymap[480],row[480];
  int oldW=0,oldH=0,jobW=0,jobH=0,oldTop=0,jobTop=480;
  bool invalid=true,jobClear=false;
  int dirtyFirst=0,dirtyLast=0;
  uint32_t viewport=0;
  SemaphoreHandle_t idle=nullptr;
  TaskHandle_t task=nullptr;
  static void worker(void *arg){
    auto *self=static_cast<PrismaVideo*>(arg);
    for(;;){
      ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
      uint32_t t=micros();renderSnapshot();
      uint32_t us=micros()-t;
      uint32_t old=__atomic_load_n(&self->renderMax,__ATOMIC_RELAXED);
      while(us>old&&!__atomic_compare_exchange_n(&self->renderMax,&old,us,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
      xSemaphoreGive(self->idle);
    }
  }
  void present(){
    uint32_t t=micros();
    if(jobClear)dosLcd->fillRect(0,0,480,jobTop,0);
    dosLcd->draw16bitRGBBitmap((480-jobW)/2,(jobTop-jobH)/2+dirtyFirst,cache+dirtyFirst*jobW,jobW,dirtyLast-dirtyFirst+1);
    dosLcd->flush(true);
    __atomic_fetch_add(&frames,1,__ATOMIC_RELAXED);
    __atomic_fetch_add(&work,micros()-t,__ATOMIC_RELAXED);
  }
public:
  uint32_t frames=0, work=0, skipped=0, dropped=0, renderMax=0;
  void invalidate(){__atomic_store_n(&invalid,true,__ATOMIC_RELAXED);__atomic_store_n(&viewport,0,__ATOMIC_RELAXED);}
  bool touchPosition(int x,int y,uint16_t &nx,uint16_t &ny){uint32_t v=__atomic_load_n(&viewport,__ATOMIC_RELAXED);int w=v&511,h=(v>>9)&511,top=(v>>18)&511;int l=(480-w)/2,t=(top-h)/2;if(w<2||h<2||x<l||x>=l+w||y<t||y>=t+h)return false;nx=uint32_t(x-l)*65535/(w-1);ny=uint32_t(y-t)*65535/(h-1);return true;}
  bool asyncReady(){return task!=nullptr;}
  bool acquire(){return idle&&xSemaphoreTake(idle,0)==pdTRUE;}
  void release(){if(idle)xSemaphoreGive(idle);}
  void submit(){xTaskNotifyGive(task);}
  void drain(){if(idle){xSemaphoreTake(idle,portMAX_DELAY);xSemaphoreGive(idle);}}
  bool allocate(){
    cache=(uint16_t*)ps_malloc(480*480*2);
    if(!cache)return false;
    idle=xSemaphoreCreateBinary();
    if(idle){
      xSemaphoreGive(idle);
      if(xTaskCreatePinnedToCore(worker,"dosLCD",4096,this,3,&task,0)!=pdPASS){vSemaphoreDelete(idle);idle=nullptr;}
    }
    Serial.printf("DOS VGA+LCD worker=%s\n",task?"core0":"sync");
    return true;
  }
  Faux86::RenderSurface *getSurface() override {return &surface;}
  void setPalette(Faux86::Palette*) override {}
  void blit(uint16_t *pixels,int w,int h,int stride) override {
    if(!pixels||w<=0||h<=0||w>VGA_FRAMEBUFFER_WIDTH||h>VGA_FRAMEBUFFER_HEIGHT||stride<w*2||!cache)return;
    if(__atomic_load_n(&hideGuest,__ATOMIC_RELAXED)){__atomic_fetch_add(&skipped,1,__ATOMIC_RELAXED);return;}
    int dw=min(w,480),dh=min(h,480);
    if((w==320||w==640)&&h==200){dw=480;dh=360;}
    const int top=panelTop();if(dh>top){dw=max(1,dw*top/dh);dh=top;}
    bool force=__atomic_exchange_n(&invalid,false,__ATOMIC_RELAXED);
    if(w!=oldW||h!=oldH||top!=oldTop){force=true;oldW=w;oldH=h;oldTop=top;for(int x=0;x<dw;x++)xmap[x]=x*w/dw;for(int y=0;y<dh;y++)ymap[y]=y*h/dh;Serial.printf("DOS VIDEO %dx%d -> %dx%d\n",w,h,dw,dh);}
    bool changed=force;dirtyFirst=dh;dirtyLast=-1;
    int prevY=-1;
    for(int y=0;y<dh;y++){
      const uint16_t *src=(const uint16_t*)((const uint8_t*)pixels+ymap[y]*stride);
      if(ymap[y]!=prevY){if(dw==w)memcpy(row,src,dw*2);else for(int x=0;x<dw;x++)row[x]=src[xmap[x]];prevY=ymap[y];}
      uint16_t *prev=cache+y*dw;
      if(force||memcmp(prev,row,dw*2)){memcpy(prev,row,dw*2);changed=true;dirtyFirst=min(dirtyFirst,y);dirtyLast=y;}
    }
    jobW=dw;jobH=dh;jobTop=top;jobClear=force;
    __atomic_store_n(&viewport,uint32_t(dw)|(uint32_t(dh)<<9)|(uint32_t(top)<<18),__ATOMIC_RELAXED);
    if(changed)present();else __atomic_fetch_add(&skipped,1,__ATOMIC_RELAXED);
  }
};
class PrismaTimer final : public Faux86::TimerInterface {
 uint64_t pausedAt=0, offset=0;
 bool paused=false;
 public:
 uint64_t getHostFreq() override {return 10000000ULL;}
 uint64_t getTicks() override {return ((paused?pausedAt:(uint64_t)esp_timer_get_time())-offset)*10ULL;}
 void pause(){if(!paused){pausedAt=esp_timer_get_time();paused=true;}}
 void resume(){if(paused){offset+=(uint64_t)esp_timer_get_time()-pausedAt;paused=false;}}
};
class PrismaHost final : public Faux86::ArduinoHostSystemInterface {
public:
  PrismaVideo video; PrismaTimer timer;
  PrismaHost():ArduinoHostSystemInterface((Adafruit_SPITFT*)nullptr){}
  Faux86::FrameBufferInterface &getFrameBuffer() override {return video;}
  Faux86::TimerInterface &getTimer() override {return timer;}
  void resize(uint32_t,uint32_t) override {video.invalidate();}
};
// Raw read-only disks in a dedicated partition; never mount/format NES storage.
class FlashDisk final : public Faux86::DiskInterface {
 const esp_partition_t *part; uint32_t base,len,pos=0;
 static constexpr uint32_t blockSize=4096,blocks=32;
 uint8_t *cache=nullptr; uint32_t tags[blocks];
public:
 FlashDisk(const esp_partition_t *p,uint32_t b,uint32_t n):part(p),base(b),len(n){cache=(uint8_t*)ps_malloc(blockSize*blocks);for(auto &t:tags)t=UINT32_MAX;}
 ~FlashDisk(){free(cache);}
 bool isValid() override {return part && len && base<=part->size && len<=part->size-base;}
 uint64_t getSize() override {return len;}
 uint64_t seek(uint64_t offset) override {pos=offset>len?len:offset;return pos;}
 int read(uint8_t *buf,unsigned count) override {
   if(!isValid())return 0; count=static_cast<unsigned>(min<uint32_t>(static_cast<uint32_t>(count),len-pos));
   uint32_t t=micros();unsigned done=0;
   while(done<count){
    uint32_t block=pos/blockSize,offset=pos%blockSize,slot=block%blocks;
    unsigned n=min<uint32_t>(count-done,blockSize-offset);
    if(cache){
     if(tags[slot]!=block){
      unsigned fill=min<uint32_t>(blockSize,len-block*blockSize);
      if(esp_partition_read(part,base+block*blockSize,cache+slot*blockSize,fill)!=ESP_OK)break;
      tags[slot]=block;++diskReads;
     }else ++diskHits;
     memcpy(buf+done,cache+slot*blockSize+offset,n);
    }else{if(esp_partition_read(part,base+pos,buf+done,n)!=ESP_OK)break;++diskReads;}
    pos+=n;done+=n;
   }
   diskReadUs+=micros()-t;return done;
 }
 int write(const uint8_t *,unsigned) override {return 0;}
};
struct DiskHeader {char magic[8];uint32_t floppyOffset,floppySize,hardOffset,hardSize;};
static PrismaHost host;
static Faux86::Config cfg(&host);
static Faux86::VM *vm=nullptr;
static void renderSnapshot(){vm->video.prismaRenderSnapshot();}
static uint32_t last=0,steps=0;
static uint64_t lastInstructions=0;
static uint32_t promptCheck=0, launchTime=0;
static bool launchPending=false, promptGone=false;
static uint8_t held=0;
static uint16_t textQueue[512];static unsigned textHead=0,textCount=0;static uint32_t textAccepted=0,textDelivered=0,serialBytes=0;
static uint32_t directAt=0,directBefore=0,directRelease=0;
static bool directQueued=false,directClick=false,directFlightClick=false;
static uint16_t directX=0,directY=0;
static uint32_t directMoves=0,directTimeouts=0;
static bool queueDirect(int x,int y,bool click){
 uint16_t nx,ny;
 if(!host.video.touchPosition(x,y,nx,ny))return false;
 // A release click is never overwritten by a later motion sample.
 if(directQueued&&directClick&&!click)return false;
 directX=nx;directY=ny;directQueued=true;directClick=click;return true;
}
static void cancelDirect(){
 directQueued=directClick=directFlightClick=false;directAt=directRelease=0;
 if(vm&&!vm->cpu.prismaMouseInCall)vm->cpu.prismaMousePending=false;
}
static void clearText(){textHead=textCount=0;}
void dos_key(uint8_t key){
 if(!vm||key==held)return;
 if(held)vm->input.handleKeyUp(held);
 if(key)vm->input.handleKeyDown(key);
 held=key;vm->input.tick();
}
void dos_mouse(int dx,int dy,bool pressed){
 if(!vm)return;
 static bool down=false;
 if(dx||dy)vm->mouse.handleMove(constrain(dx,-127,127),constrain(dy,-127,127));
 if(pressed!=down){if(pressed)vm->mouse.handleButtonDown(Faux86::SerialMouse::ButtonType::Left);else vm->mouse.handleButtonUp(Faux86::SerialMouse::ButtonType::Left);down=pressed;}
}
static uint8_t asciiScan(char c){
 static const char *rows[]={"1234567890","qwertyuiop","asdfghjkl","zxcvbnm"};
 static const uint8_t starts[]={2,16,30,44};
 if(c=='\r')return 0x1c;
 for(int i=0;i<4;i++){const char *p=strchr(rows[i],c);if(p&&*p)return starts[i]+(p-rows[i]);}
 return 0;
}
static bool enqueueText(uint8_t c,uint8_t scan){
 if(textCount>=512)return false;textQueue[(textHead+textCount)%512]=(uint16_t(scan)<<8)|c;++textCount;++textAccepted;return true;
}
static bool textChar(uint8_t c){
 uint8_t k=0;if(c>='A'&&c<='Z')k=asciiScan(c+32);else if((c>='a'&&c<='z')||(c>='0'&&c<='9'))k=asciiScan(c);
 else if(c==13||c==10){k=28;c=13;}else if(c==8||c==127){k=14;c=8;}else if(c==32)k=57;
 else if(c==9)k=15;
 else {const char *plain="-=[]\\;',./",*shifted="_+{}|:\"<>?";const uint8_t keys[]={12,13,26,27,43,39,40,51,52,53};
 const char *p=strchr(plain,c);if(p&&*p)k=keys[p-plain];else{p=strchr(shifted,c);if(p&&*p)k=keys[p-shifted];else{const char *digits="!@#$%^&*()";p=strchr(digits,c);if(p&&*p)k=2+(p-digits);}}}
 return k&&enqueueText(c,k);
}
static void pumpText(){
 if(!textCount)return;auto &m=vm->memory;unsigned head=m.readWord(0x41a),tail=m.readWord(0x41c);
 if(head<0x1e||head>=0x3e||tail<0x1e||tail>=0x3e||(head&1)||(tail&1))return;
 // Never overwrite unread BIOS words. Leave queued characters for a later slice.
 while(textCount){unsigned next=tail+2;if(next>=0x3e)next=0x1e;if(next==head)break;
 m.writeWord(0x400+tail,textQueue[textHead]);textHead=(textHead+1)%512;--textCount;++textDelivered;tail=next;}
 m.writeWord(0x41c,tail);
}
static bool atPrompt(){
 if(!vm)return false;
 auto &m=vm->memory;
 unsigned mode=m.readByte(0x449)&0x7f;
 if(mode>3&&mode!=7)return false;
 unsigned page=m.readByte(0x462),cols=m.readWord(0x44a);
 if(page>7||cols<40||cols>80)return false;
 unsigned pos=m.readWord(0x450+page*2),x=pos&255,y=pos>>8;
 if(x!=4||y>=50)return false;
 uint32_t base=(mode==7?0xb0000:0xb8000)+m.readWord(0x44e)+y*cols*2;
 if(base+8>0xc0000)return false;
 return m.readByte(base)=='C'&&m.readByte(base+2)==':'&&m.readByte(base+4)==92&&m.readByte(base+6)=='>';
}
static bool injectCommand(const char *cmd){
 if(!atPrompt())return false;
 auto &m=vm->memory;
 unsigned head=m.readWord(0x41a),tail=m.readWord(0x41c);
 if(head<0x1e||head>=0x3e||tail<0x1e||tail>=0x3e||(head&1)||(tail&1))return false;
 if(head!=tail)return false;
 for(const char *p=cmd;*p;p++){uint8_t k=asciiScan(*p);if(!k)return false;m.writeWord(0x400+tail,(uint16_t(k)<<8)|uint8_t(*p));tail+=2;if(tail>=0x3e)tail=0x1e;}
 m.writeWord(0x400+tail,0x1c0d);tail+=2;if(tail>=0x3e)tail=0x1e;
 m.writeWord(0x41c,tail);return true;
}
static void centered(const char *s,int y,uint8_t size,uint16_t color=0xffff){dosLcd->setTextSize(size);dosLcd->setTextColor(color);dosLcd->setCursor(max(0,(480-int(strlen(s))*6*size)/2),y);dosLcd->print(s);}
static void loadProgress(uint8_t stage,const char *label){
 if(stage==loadStage)return;loadStage=stage;
 dosLcd->fillRect(30,340,420,96,0);centered(label,345,2);char pct[16];snprintf(pct,sizeof(pct),"%u%%",stage);centered(pct,374,2,0x9cfa);
 dosLcd->drawRect(40,407,400,12,0x7bef);dosLcd->fillRect(42,409,(396*stage)/100,8,0x5e9e);dosLcd->flush(true);Serial.printf("DOS LOAD stage=%u %s\n",stage,label);
}
static void showLoading(int game){
 host.video.drain();__atomic_store_n(&hideGuest,true,__ATOMIC_RELAXED);loadStage=0;
 dosLcd->fillScreen(0);centered(game<0?"PRISMA RETRO / DOS":"PRISMA RETRO",28,2,0x9cfa);
 int art=game<0?2:game;
 if(DosArt::large[art])dosLcd->draw16bitRGBBitmap(96,82,const_cast<uint16_t*>(DosArt::large[art]),288,180);
 else centered(game<0?"MS-DOS 6.22":"DOS GAME",148,4);
 if(game>=0){centered(game==0?"Carmen Sandiego":"The Secret of",280,2);if(game==1)centered("Monkey Island",304,2);}
 loadProgress(10,game<0?"Inicializando DOS":"Preparando juego");
}
static void logGuestText(){
 static uint32_t next=0,oldHash=0;static char oldLines[25][81]={};
 if(!vm||int32_t(millis()-next)<0)return;next=millis()+600;
 auto &m=vm->memory;unsigned mode=m.readByte(0x449)&0x7f,cols=m.readWord(0x44a);
 if((mode>3&&mode!=7)||cols<40||cols>80)return;
 unsigned base=(mode==7?0xb0000:0xb8000)+m.readWord(0x44e);
 if(base+cols*50>0xc0000)return;
 for(unsigned y=0;y<25;y++){char line[81];unsigned n=0;for(unsigned x=0;x<cols;x++){unsigned c=m.readByte(base+2*(y*cols+x));line[n++]=c>=32&&c<127?char(c):' ';}while(n&&line[n-1]==' ')--n;line[n]=0;if(strcmp(line,oldLines[y])){strcpy(oldLines[y],line);if(n)Serial.printf("DOS TEXT %s\n",line);}}
}
static void openLauncher(){
 benchUntil=0;bootPhase=false;loadingGame=false;loadingIndex=-1;consoleMode=false;activeGame=-1;clearText();vm->cpu.prismaMouseEnabled=false;vm->cpu.prismaMousePending=false;cancelDirect();__atomic_store_n(&hideGuest,true,__ATOMIC_RELAXED);
 dos_key(0);dos_mouse(0,0,false);
 host.video.drain();launcher=true;host.timer.pause();
 dosLcd->fillScreen(0x0842);dosLcd->setTextColor(0xffff);
 dosLcd->fillRoundRect(12,10,90,44,8,0x2124);dosLcd->drawRoundRect(12,10,90,44,8,0x5e9e);
 dosLcd->setTextSize(2);dosLcd->setCursor(30,24);dosLcd->print("HOME");
 dosLcd->setTextSize(3);dosLcd->setCursor(114,10);dosLcd->print("PRISMA DOS");
 dosLcd->setTextSize(1);dosLcd->setCursor(114,39);dosLcd->print("2 JUEGOS / ESPANOL");
 for(int i=0;i<2;i++){
  int y=62+i*184;dosLcd->fillRoundRect(12,y,456,180,8,0x2124);dosLcd->drawRoundRect(12,y,456,180,8,0x5e9e);
  if(DosArt::small[i])dosLcd->draw16bitRGBBitmap(220,y+15,const_cast<uint16_t*>(DosArt::small[i]),240,150);
  else{dosLcd->fillRect(220,y+15,240,150,0x0842);dosLcd->setTextSize(1);dosLcd->setCursor(280,y+83);dosLcd->print("SIN PREVIEW");}
  dosLcd->setTextSize(2);dosLcd->setCursor(24,y+40);dosLcd->print(i==0?"Carmen":"The Secret of");
  dosLcd->setCursor(24,y+66);dosLcd->print(i==0?"Sandiego":"Monkey Island");
  dosLcd->setTextSize(1);dosLcd->setCursor(24,y+110);dosLcd->print("MS-DOS / ESPANOL");
  dosLcd->setTextSize(2);dosLcd->setCursor(24,y+140);dosLcd->print("JUGAR >");
 }
 dosLcd->fillRoundRect(12,436,456,32,6,0x2124);dosLcd->setTextSize(2);dosLcd->setCursor(24,444);dosLcd->print("Consola USB");
 dosLcd->flush(true);Serial.println("DOS TOUCH LAUNCHER");
}
bool dos_launcher_touch(unsigned n,int x,int y){
 static bool down=false;static int choice=-1;
 if(!launcher){down=false;choice=-1;return false;}
 if(n==1&&!down){down=true;choice=(x>=12&&x<102&&y>=10&&y<54)?3:(x>=12&&x<468&&y>=436&&y<468)?4:(x>=12&&x<468&&y>=62&&y<426&&(y-62)%184<180)?(y-62)/184:-1;}
 if(n>1){down=true;choice=-1;}
 if(!n&&down){
  down=false;
  if(choice==3){Serial.println("DOS MENU HOME");Serial.flush();ESP.restart();}
  if(choice==4){consoleMode=true;activeGame=-1;clearText();__atomic_store_n(&hideGuest,false,__ATOMIC_RELAXED);host.timer.resume();vm->video.updatedscreen=true;launcher=false;host.video.invalidate();setPanel(1);Serial.println("DOS USB CONSOLE");}
  if(choice>=0&&choice<2){
   const char *cmd=gameCmds[choice];Serial.printf("DOS LAUNCH %s\n",cmd);
   if(!injectCommand(cmd)){Serial.println("DOS command rejected: not at empty prompt");return true;}
   activeGame=choice;clearText();vm->cpu.prismaMouseEnabled=true;vm->cpu.prismaMousePending=false;
   showLoading(choice);loadingGame=true;loadingIndex=choice;loadProgress(35,"Comando enviado");
   mouseMode=choice<2;__atomic_store_n(&panelMode,(uint8_t)(choice<2?0:2),__ATOMIC_RELAXED);host.timer.resume();vm->video.updatedscreen=true;launcher=false;launchPending=true;promptGone=false;launchTime=millis();host.video.invalidate();
  }
  choice=-1;
 }
 return true;
}
void dos_show_launcher(){if(vm&&!launcher&&!launchPending&&atPrompt())openLauncher();}
bool dos_in_launcher(){return launcher;}
void dos_request_exit(){if(!vm||launcher)return;dos_key(0);dos_mouse(0,0,false);if(atPrompt()){openLauncher();return;}vm->input.handleKeyDown(1);vm->input.handleKeyUp(1);Serial.println("DOS EXIT: ESC sent; waiting for DOS prompt");}
static void serialTap(uint8_t k,bool shift=false,bool ctrl=false){
 if(!k)return;
 if(shift)vm->input.handleKeyDown(0x2a);
 if(ctrl)vm->input.handleKeyDown(0x1d);
 vm->input.handleKeyDown(k);vm->input.handleKeyUp(k);
 if(ctrl)vm->input.handleKeyUp(0x1d);
 if(shift)vm->input.handleKeyUp(0x2a);
 ++serialKeys;
}
static void asciiKey(uint8_t c){
 if(c>='a'&&c<='z'){serialTap(asciiScan(c));return;}
 if(c>='A'&&c<='Z'){serialTap(asciiScan(c+32),true);return;}
 if(c>='0'&&c<='9'){serialTap(asciiScan(c));return;}
 if(c==13||c==10){serialTap(0x1c);return;}
 if(c==8||c==127){serialTap(0x0e);return;}
 if(c==9){serialTap(0x0f);return;}
 if(c==27){serialTap(1);return;}
 if(c==32){serialTap(0x39);return;}
 if(c>=1&&c<=26){serialTap(asciiScan(c+96),false,true);return;}
 const char *plain="-=[]\\;',./",*shifted="_+{}|:\"<>?";
 const uint8_t keys[]={12,13,26,27,43,39,40,51,52,53};
 const char *p=strchr(plain,c);
 if(p&&*p){serialTap(keys[p-plain]);return;}
 p=strchr(shifted,c);if(p&&*p){serialTap(keys[p-shifted],true);return;}
 const char *digits="!@#$%^&*()";p=strchr(digits,c);
 if(p&&*p){serialTap(2+(p-digits),true);return;}
 if(c==96||c==126)serialTap(41,c==126);
}
static void ansiKey(char final,int number){
 uint8_t k=0;
 switch(final){case 'A':k=0x48;break;case 'B':k=0x50;break;case 'C':k=0x4d;break;case 'D':k=0x4b;break;case 'H':k=0x47;break;case 'F':k=0x4f;break;case 'P':k=0x3b;break;case 'Q':k=0x3c;break;case 'R':k=0x3d;break;case 'S':k=0x3e;break;
 case '~':switch(number){case 1:case 7:k=0x47;break;case 2:k=0x52;break;case 3:k=0x53;break;case 4:case 8:k=0x4f;break;case 5:k=0x49;break;case 6:k=0x51;break;case 11:k=0x3b;break;case 12:k=0x3c;break;case 13:k=0x3d;break;case 14:k=0x3e;break;case 15:k=0x3f;break;case 17:k=0x40;break;case 18:k=0x41;break;case 19:k=0x42;break;case 20:k=0x43;break;case 21:k=0x44;break;}break;}
 serialTap(k);
}
static void serialInput(){
 static uint8_t state=0;static int number=0;static uint32_t at=0,lastCR=0;
 if(vm->input.freeSlots()<8)return;
 if(state&&millis()-at>80){if(state==1)serialTap(1);state=0;}
 for(int i=0;i<32&&Serial.available()&&vm->input.freeSlots()>=8&&textCount<512;i++){
  uint8_t c=Serial.read();++serialBytes;
  if(c==0x1d&&!bootPhase&&!loadingGame){if(!benchUntil){host.video.drain();benchStart=millis();benchUntil=benchStart+5000;benchInstructions=vm->cpu.totalexec;Serial.println("DOS BENCH video disabled 5s");}continue;}
  if(state==1){if(c=='['||c=='O'){state=2;number=0;at=millis();continue;}serialTap(1);state=0;}
  if(state==2){at=millis();if(c>='0'&&c<='9'){number=min(999,number*10+c-'0');continue;}if(c==';'){number=0;continue;}ansiKey(c,number);state=0;continue;}
  if(c==27){state=1;at=millis();continue;}
  if(c==10&&lastCR&&millis()-lastCR<100){lastCR=0;continue;}
  if(c==13)lastCR=millis();else lastCR=0;
  if((activeGame==0||consoleMode)&&textChar(c))continue;
  asciiKey(c);
 }
}
struct OverlayKey {const char* label;uint8_t scan;};
static const OverlayKey keyboard[7][10]={
 {{"1",2},{"2",3},{"3",4},{"4",5},{"5",6},{"6",7},{"7",8},{"8",9},{"9",10},{"0",11}},
 {{"Q",16},{"W",17},{"E",18},{"R",19},{"T",20},{"Y",21},{"U",22},{"I",23},{"O",24},{"P",25}},
 {{"A",30},{"S",31},{"D",32},{"F",33},{"G",34},{"H",35},{"J",36},{"K",37},{"L",38},{"BKSP",14}},
 {{"Z",44},{"X",45},{"C",46},{"V",47},{"B",48},{"N",49},{"M",50},{",",51},{".",52},{"/",53}},
 {{"SHIFT",240},{"CTRL",241},{"ALT",242},{"TAB",15},{"-",12},{"=",13},{";",39},{"\\",43},{"SPC",57},{"ENT",28}},
 {{"F1",59},{"F2",60},{"F3",61},{"F4",62},{"F5",63},{"F6",64},{"F7",65},{"F8",66},{"F9",67},{"F10",68}},
 {{"ESC",1},{"UP",72},{"DOWN",80},{"LEFT",75},{"RIGHT",77},{"PGUP",73},{"PGDN",81},{"MOUSE",243},{"PAD",244},{"EXIT",245}}
};
static const OverlayKey pad[4][6]={
 {{"K",37},{"UP",72},{"S",31},{"ESC",1},{"LMB",246},{"RMB",247}},
 {{"LEFT",75},{"DOWN",80},{"RIGHT",77},{"ENTER",28},{"SPACE",57},{"CTRL",29}},
 {{"A",30},{"Z",44},{"X",45},{"C",46},{"ALT",56},{"F1",59}},
 {{"KEYS",248},{"MOUSE",243},{"FULL",249},{"TAB",15},{"M",50},{"EXIT",245}}
};
static const OverlayKey toolbar[6]={{"KEYS",248},{"PAD",244},{"LMB",246},{"RMB",247},{"FULL",249},{"EXIT",245}};
static const OverlayKey carmenBar[6]={{"TEXTO",250},{"CLICK",246},{"IZQ",75},{"DER",77},{"ENTER",28},{"SALIR",245}};
static const OverlayKey carmenKeyboard[5][10]={
 {{"1",2},{"2",3},{"3",4},{"4",5},{"5",6},{"6",7},{"7",8},{"8",9},{"9",10},{"0",11}},
 {{"Q",16},{"W",17},{"E",18},{"R",19},{"T",20},{"Y",21},{"U",22},{"I",23},{"O",24},{"P",25}},
 {{"A",30},{"S",31},{"D",32},{"F",33},{"G",34},{"H",35},{"J",36},{"K",37},{"L",38},{"BORR",14}},
 {{"Z",44},{"X",45},{"C",46},{"V",47},{"B",48},{"N",49},{"M",50},{",",51},{".",52},{"-",12}},
 {{"MAY",240},{"ESP",57},{"ENT",28},{"ESC",1},{"IZQ",75},{"DER",77},{"ARR",72},{"ABA",80},{"CLICK",246},{"OCULT",243}}
};
static uint8_t mods=0;
static void cell(const OverlayKey &k,int x,int y,int w,int h){
 bool active=(k.scan==240&&(mods&1))||(k.scan==241&&(mods&2))||(k.scan==242&&(mods&4));
 dosLcd->fillRoundRect(x+2,y+2,w-4,h-4,4,active?0x366e:0x2124);
 dosLcd->drawRoundRect(x+2,y+2,w-4,h-4,4,0x7bef);
 int size=strlen(k.label)*12<w-6?2:1;
 dosLcd->setTextSize(size);dosLcd->setTextColor(0xffff);
 dosLcd->setCursor(x+(w-int(strlen(k.label))*6*size)/2,y+(h-8*size)/2);dosLcd->print(k.label);
}
static void drawPanel(){
 const uint8_t m=__atomic_load_n(&panelMode,__ATOMIC_RELAXED);const int top=panelTop();
 if(m==3)return;
 dosLcd->fillRect(0,top,480,480-top,0x0842);
 if(m==1)for(int r=0;r<7;r++)for(int c=0;c<10;c++)cell(keyboard[r][c],c*48,top+r*36,48,36);
 else if(m==2)for(int r=0;r<4;r++)for(int c=0;c<6;c++)cell(pad[r][c],c*80,top+r*39,80,39);
 else if(m==4)for(int r=0;r<5;r++)for(int c=0;c<10;c++)cell(carmenKeyboard[r][c],c*48,top+r*38,48,38);
 else for(int c=0;c<6;c++)cell(activeGame==0?carmenBar[c]:toolbar[c],c*80,top,80,48);
}
static void setPanel(uint8_t mode){
 host.video.drain();__atomic_store_n(&panelMode,mode,__ATOMIC_RELAXED);mouseMode=mode!=2;mods=0;
 dosLcd->fillScreen(0);drawPanel();dosLcd->flush(true);
 host.video.invalidate();vm->video.updatedscreen=true;vm->video.prismaDrawDue=true;
 Serial.printf("DOS OVERLAY mode=%u\n",mode);
}
static uint8_t hitPanel(int x,int y){
 const uint8_t m=__atomic_load_n(&panelMode,__ATOMIC_RELAXED);const int top=panelTop();
 if(x<0||x>=480||y<top||y>=480)return 0;
 if(m==1)return keyboard[(y-top)/36][x/48].scan;
 if(m==2)return pad[(y-top)/39][x/80].scan;
 if(m==4)return carmenKeyboard[(y-top)/38][x/48].scan;
 if(m==0)return (activeGame==0?carmenBar[x/80]:toolbar[x/80]).scan;
 return 0;
}
static void overlayTap(uint8_t k){
 if(!k||vm->input.freeSlots()<8)return;
 if(activeGame==0||consoleMode){uint8_t c=0;const char *rows[]={"1234567890","qwertyuiop","asdfghjkl","zxcvbnm"};const unsigned starts[]={2,16,30,44};for(int i=0;i<4;i++)if(k>=starts[i]&&k<starts[i]+strlen(rows[i]))c=rows[i][k-starts[i]];if(c>='a'&&c<='z'&&(mods&1))c-=32;if(k==57)c=32;if(k==28)c=13;if(k==14)c=8;if(k==51)c=',';if(k==52)c='.';if(k==12)c='-';if(c&&enqueueText(c,k)){++overlayKeys;if(mods){mods=0;host.video.drain();drawPanel();dosLcd->flush(true);}return;}}

 if(mods&1)vm->input.handleKeyDown(42);
 if(mods&2)vm->input.handleKeyDown(29);
 if(mods&4)vm->input.handleKeyDown(56);
 vm->input.handleKeyDown(k);vm->input.handleKeyUp(k);
 if(mods&4)vm->input.handleKeyUp(56);
 if(mods&2)vm->input.handleKeyUp(29);
 if(mods&1)vm->input.handleKeyUp(42);
 ++overlayKeys;
 if(mods){mods=0;host.video.drain();drawPanel();dosLcd->flush(true);}
}
void dos_touch_game(unsigned n,int x,int y){
 static bool finger=false,moved=false,drag=false,panelTouch=false,rightDown=false;
 static int px=0,py=0,ox=0,oy=0;
 static uint8_t pending=0;
 static uint32_t started=0,releaseAt=0,rightRelease=0;
 if(releaseAt&&int32_t(millis()-releaseAt)>=0){dos_mouse(0,0,false);releaseAt=0;}
 if(rightRelease&&int32_t(millis()-rightRelease)>=0){vm->mouse.handleButtonUp(Faux86::SerialMouse::ButtonType::Right);rightRelease=0;rightDown=false;}
 if(launcher||bootPhase||loadingGame){finger=moved=drag=panelTouch=false;pending=0;dos_key(0);return;}
 const bool absolute=activeGame==0||activeGame==1;
 if(n>1&&absolute){cancelDirect();dos_mouse(0,0,false);}
 if(n!=1){
   if(finger&&panelTouch&&n==0&&pending){
     const uint8_t k=pending;
     if(k==240||k==241||k==242){mods^=1u<<(k-240);host.video.drain();drawPanel();dosLcd->flush(true);}
     else if(k==250)setPanel(4);else if(k==243)setPanel(0);else if(k==244)setPanel(2);else if(k==248)setPanel(1);else if(k==249)setPanel(3);
     else if(k==245)dos_request_exit();
     else if(k==246){dos_mouse(0,0,true);releaseAt=millis()+120;++touchClicks;}
     else if(k==247){if(!rightDown){vm->mouse.handleButtonDown(Faux86::SerialMouse::ButtonType::Right);rightDown=true;}rightRelease=millis()+120;++touchClicks;}
     else if(__atomic_load_n(&panelMode,__ATOMIC_RELAXED)==1||__atomic_load_n(&panelMode,__ATOMIC_RELAXED)==4||activeGame==0)overlayTap(k);
   }else if(finger&&!panelTouch&&!moved&&!drag&&n==0&&millis()-started<350){
     if(absolute){queueDirect(px,py,true);}
     else{dos_mouse(0,0,true);releaseAt=millis()+120;++touchClicks;}
   }
   if(drag)dos_mouse(0,0,false);
   finger=moved=drag=panelTouch=false;pending=0;dos_key(0);return;
 }
 if(!finger){
  finger=true;px=ox=x;py=oy=y;started=millis();moved=drag=false;
  pending=hitPanel(x,y);panelTouch=y>=panelTop();
  if(!panelTouch&&x<56&&y<56&&(!absolute||__atomic_load_n(&panelMode,__ATOMIC_RELAXED)==3)){panelTouch=true;pending=__atomic_load_n(&panelMode,__ATOMIC_RELAXED)==3?243:249;}
  if(panelTouch){if(pending&&pending<240&&__atomic_load_n(&panelMode,__ATOMIC_RELAXED)==2){dos_key(pending);++overlayKeys;}return;}
  if(absolute)queueDirect(x,y,false);
 }
 if(panelTouch)return;
 dos_key(0);
 if(abs(x-ox)+abs(y-oy)>9)moved=true;
 if(absolute){
   // Follow touch using the existing absolute-position request. Keep the
   // original relative drag path for an intentional long press.
   if(!moved&&!drag&&millis()-started>=350&&!directAt&&!directQueued){drag=true;dos_mouse(0,0,true);}
   if(drag){if(x!=px||y!=py)dos_mouse(x-px,y-py,true);px=x;py=y;}
   else if(abs(x-px)+abs(y-py)>=3){if(queueDirect(x,y,false)){px=x;py=y;}}
   return;
 }
 if(!moved&&!drag&&millis()-started>=350){drag=true;dos_mouse(0,0,true);}
 if(moved||drag){dos_mouse(x-px,y-py,drag||releaseAt);px=x;py=y;}
}
static void fail(const char *msg){Serial.println(msg);dosLcd->setCursor(8,8);dosLcd->setTextColor(0xffff);dosLcd->setTextSize(2);dosLcd->println(msg);dosLcd->flush(true);}
bool dos_start(Arduino_RGB_Display &display){
 dosLcd=&display;
 showLoading(-1);
 Serial.printf("DOS ENTRY heap=%u psram=%u\n",ESP.getFreeHeap(),ESP.getFreePsram());
 Serial.println("BUILD 045 DOS: draw cadence 33ms / 30Hz target");
  const auto *part=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_ANY,"dosdata");
  DiskHeader h{};
  if(!part||esp_partition_read(part,0,&h,sizeof(h))!=ESP_OK||memcmp(h.magic,"PRDOS032",8)){fail("Missing DOS data");return false;}
  cfg.diskDriveA=new FlashDisk(part,h.floppyOffset,h.floppySize);
  if(!cfg.diskDriveA->isValid()){fail("Invalid boot disk");return false;}
  if(h.hardSize){cfg.diskDriveC=new FlashDisk(part,h.hardOffset,h.hardSize);if(!cfg.diskDriveC->isValid()){fail("Invalid game disk");return false;}}
  cfg.singleThreaded=true;cfg.slowSystem=false;cfg.cpuSpeed=240; cfg.cpuTiming=127;
  cfg.frameDelay=33;cfg.framebuffer.width=VGA_FRAMEBUFFER_WIDTH;cfg.framebuffer.height=VGA_FRAMEBUFFER_HEIGHT;
  cfg.enableAudio=false;cfg.useDisneySoundSource=false;cfg.useSoundBlaster=false;cfg.useAdlib=false;cfg.usePCSpeaker=false;
  cfg.biosFile=new Faux86::EmbeddedDisk(pcxtbios_bin,pcxtbios_bin_len);
  cfg.romBasicFile=new Faux86::EmbeddedDisk(rombasic_bin,rombasic_bin_len);
  cfg.videoRomFile=new Faux86::EmbeddedDisk(videorom_bin,videorom_bin_len);
  cfg.asciiFile=new Faux86::EmbeddedDisk(asciivga_dat,asciivga_dat_len);
  cfg.setBootDrive("fd0");
  vga_framebuffer=(uint16_t*)ps_calloc(VGA_FRAMEBUFFER_SIZE,sizeof(uint16_t));
  if(!vga_framebuffer){fail("VGA allocation failed");return false;}
  if(!host.video.allocate()){fail("DOS LCD cache allocation failed");return false;}
  vm=new Faux86::VM(cfg);
  if(!vm->init()){vm=nullptr;fail("VM init failed");return false;}
  // Standard BIOS warm-start flag; this BIOS may ignore it.
  vm->memory.writeWord(0x472,0x1234);
  host.init(vm);loadProgress(35,"Maquina virtual lista");last=millis();lastInstructions=vm->cpu.totalexec;Serial.println("DOS VM READY");return true;
}
void dos_tick(){
  if(!vm)return;
  if(!launcher){serialInput();pumpText();}
  if(directAt&&vm->cpu.prismaMouseApplied!=directBefore){
    directAt=0;++directMoves;
    if(directFlightClick){dos_mouse(0,0,true);directRelease=millis()+150;++touchClicks;}
    directFlightClick=false;
  }else if(directAt&&millis()-directAt>800&&!vm->cpu.prismaMouseInCall){
    directAt=0;directFlightClick=false;vm->cpu.prismaMousePending=false;++directTimeouts;
  }
  if(directRelease&&int32_t(millis()-directRelease)>=0){directRelease=0;dos_mouse(0,0,false);}
  if(directQueued&&!directAt&&!directRelease&&!vm->cpu.prismaMouseInCall&&!launcher&&!loadingGame){
    vm->cpu.prismaMouseX=directX;vm->cpu.prismaMouseY=directY;directBefore=vm->cpu.prismaMouseApplied;
    directFlightClick=directClick;directQueued=directClick=false;vm->cpu.prismaMousePending=true;directAt=millis();
  }
  static uint32_t maxSlice=0,maxCapture=0;
  if(!launcher){uint32_t t=micros();vm->simulate();uint32_t elapsed=micros()-t;if(elapsed>maxSlice)maxSlice=elapsed;++steps;
    if(benchUntil&&int32_t(millis()-benchUntil)>=0){Serial.printf("DOS BENCH ips=%.0f elapsed_ms=%u\n",(vm->cpu.totalexec-benchInstructions)*1000.0/(millis()-benchStart),millis()-benchStart);benchUntil=0;host.video.invalidate();vm->video.updatedscreen=true;vm->video.prismaDrawDue=true;}
    if(benchUntil||__atomic_load_n(&hideGuest,__ATOMIC_RELAXED)){vm->video.prismaDrawDue=false;}
    else if(vm->video.prismaDrawDue){
      if(!vm->video.updatedscreen)vm->video.prismaDrawDue=false;
      else if(!host.video.asyncReady()){vm->video.prismaDrawDue=false;vm->video.vga_drawCallback(0);}
      else if(host.video.acquire()){
        vm->video.prismaDrawDue=false;uint32_t rt=micros();
        if(vm->video.prismaCapture()){captureBytes+=vm->video.prismaCaptureBytes();host.video.submit();}
        else{vm->video.vga_drawCallback(0);host.video.release();}
        uint32_t us=micros()-rt;if(us>maxCapture)maxCapture=us;
      }else ++host.video.dropped;
    }
  }
  if(millis()-promptCheck>=100){
   promptCheck=millis();bool prompt=atPrompt();
   if(bootPhase||loadingGame)logGuestText();
   if(bootPhase&&vm->cpu.didbootstrap&&loadStage<75)loadProgress(75,"MS-DOS iniciando");
   if(bootPhase&&prompt)loadProgress(100,"DOS listo");
   if(loadingGame){
     if(!prompt&&loadStage<65)loadProgress(65,"Ejecutando juego");
     unsigned mode=vm->memory.readByte(0x449)&0x7f;
     if((!prompt&&mode>3&&mode!=7)||millis()-launchTime>8000){
       loadingGame=false;host.video.invalidate();vm->video.updatedscreen=true;vm->video.prismaDrawDue=true;__atomic_store_n(&hideGuest,false,__ATOMIC_RELAXED);setPanel(__atomic_load_n(&panelMode,__ATOMIC_RELAXED));Serial.printf("DOS GAME DISPLAY mode=%u elapsed=%u\n",mode,millis()-launchTime);
     }
   }
   if(launchPending){if(!prompt)promptGone=true;if(promptGone||millis()-launchTime>15000)launchPending=false;}
   if(prompt&&!launchPending&&!launcher&&!consoleMode&&millis()-launchTime>1000)openLauncher();
  }
  if(millis()-last>=3000){
    uint32_t elapsed=millis()-last;
    uint64_t nowInstructions=vm->cpu.totalexec;
    uint32_t shown=__atomic_exchange_n(&host.video.frames,0,__ATOMIC_RELAXED),drawUs=__atomic_exchange_n(&host.video.work,0,__ATOMIC_RELAXED);
    uint32_t skipped=__atomic_exchange_n(&host.video.skipped,0,__ATOMIC_RELAXED);
    uint32_t renderMax=__atomic_exchange_n(&host.video.renderMax,0,__ATOMIC_RELAXED);
    Serial.printf("DOS PERF steps=%u ips=%.0f display=%.1f fps blit=%u us skip=%u heap=%u psram=%u\n",steps,(nowInstructions-lastInstructions)*1000.0/elapsed,shown*1000.0/elapsed,shown?drawUs/shown:0,skipped,ESP.getFreeHeap(),ESP.getFreePsram());
    Serial.printf("DOS SLICE max_us=%u capture_max_us=%u render_core0_max_us=%u busy=%u launcher=%d\n",maxSlice,maxCapture,renderMax,host.video.dropped,(int)launcher);maxSlice=maxCapture=host.video.dropped=0;
    Serial.printf("DOS INPUT serial_keys=%u guest_reads=%u touch_clicks=%u queue_free=%d mode=%s\n",serialKeys,vm->input.prismaReadCount,touchClicks,vm->input.freeSlots(),mouseMode?"mouse":"keys");
    Serial.printf("DOS TEXT INPUT bytes=%u queued=%u delivered=%u pending=%u scan_sent=%u guest_reads=%u\n",serialBytes,textAccepted,textDelivered,textCount,vm->input.prismaSentCount,vm->input.prismaReadCount);
    Serial.printf("DOS MOUSE queries=%u positioned=%u in_call=%u touch_moves=%u timeouts=%u\n",vm->cpu.prismaMouseQueries,vm->cpu.prismaMouseApplied,(unsigned)vm->cpu.prismaMouseInCall,directMoves,directTimeouts);
    Serial.printf("DOS OVERLAY keys=%u mode=%u\n",overlayKeys,__atomic_load_n(&panelMode,__ATOMIC_RELAXED));
    Serial.printf("DOS VGA capture_bytes=%u\n",captureBytes);captureBytes=0;
    uint32_t rasterRows=0,rasterPossible=0;vm->video.prismaRasterStats(rasterRows,rasterPossible);
    Serial.printf("DOS RASTER rows=%u possible=%u\n",rasterRows,rasterPossible);
    Serial.printf("DOS DISK reads=%u hits=%u read_us=%u\n",diskReads,diskHits,diskReadUs);diskReads=diskHits=diskReadUs=0;
    lastInstructions=nowInstructions;
    steps=0;last=millis();
  }
  static uint32_t lastYield=0;
  if(launcher||millis()-lastYield>=12){lastYield=millis();delay(1);}
}
