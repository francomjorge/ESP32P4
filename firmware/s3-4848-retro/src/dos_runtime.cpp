#include <Arduino.h>
#include "prisma_diag.h"
#include <Arduino_GFX_Library.h>
#include <esp_partition.h>
#include <Wire.h>
#include <Adafruit_Faux86.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#if __has_include("dos_art.h")
#include "dos_art.h"
#else
namespace DosArt {static const uint16_t *const small[5]={};static const uint16_t *const large[5]={};}
#endif

// Persist last active phases across watchdog/panic resets. No reset is forced.
RTC_NOINIT_ATTR static uint32_t dosTraceMagic,dosTraceMain,dosTraceWorker,dosTraceMode;
static constexpr uint32_t DOS_TRACE_MAGIC=0x44535452;
static inline void traceMain(uint32_t phase){__atomic_store_n(&dosTraceMain,phase,__ATOMIC_RELAXED);}
static inline void traceWorker(uint32_t phase){__atomic_store_n(&dosTraceWorker,phase,__ATOMIC_RELAXED);}
void dos_report_reset(){
 const auto reason=esp_reset_reason();
 if(reason!=ESP_RST_POWERON&&reason!=ESP_RST_BROWNOUT&&dosTraceMagic==DOS_TRACE_MAGIC)
  diag_crashf("DOS PREVIOUS RESET reason=%d main_phase=%u worker_phase=%u panel=%u\n",(int)reason,dosTraceMain,dosTraceWorker,dosTraceMode);
 dosTraceMagic=0;dosTraceMain=dosTraceWorker=dosTraceMode=0;
}
static Arduino_RGB_Display *dosLcd=nullptr;
uint16_t *vga_framebuffer=nullptr;
// Owned by the snapshot renderer; only valid during its synchronous blit.
uint8_t prismaScaleDirtyRows[VGA_FRAMEBUFFER_HEIGHT]={};
// A nonzero alias identifies an identical RGB565 row produced by the same VGA raster pass.
uint16_t prismaScaleAlias[VGA_FRAMEBUFFER_HEIGHT]={};
bool prismaScaleDirtyValid=false;
static bool launcher=false;
static uint32_t diskReads=0,diskHits=0,diskReadUs=0,diskMappedReads=0,diskMappedBytes=0;
static bool bootPhase=true,loadingGame=false;
static int loadingIndex=-1;
static uint8_t loadStage=0;
static uint32_t captureBytes=0,benchUntil=0,benchStart=0;
static uint64_t benchInstructions=0;
static bool hideGuest=true;
static bool mouseMode=true,consoleMode=false;
static int activeGame=-1;
static uint8_t pendingPanel=255;
static bool startupActive=false;
static uint16_t startupBaseline=0;
static constexpr uint32_t startupNumberAddress=0x349c4;
static void startupReset(bool active);
static void startupTick();
static void startupSearch();
static void startupFinish();
static uint8_t panelMode=0; // 0 mouse toolbar, 1 keyboard, 2 gamepad, 3 full, 4 Carmen text
static int panelTop(){const uint8_t p=__atomic_load_n(&panelMode,__ATOMIC_RELAXED);return p==1||p==4?280:(p==2||p==9)?360:p==3?480:(activeGame==1&&(p==0||p==8))?384:432;}
static uint32_t overlayKeys=0;
static void drawPanel(bool full);
static void setPanel(uint8_t mode);
static uint32_t serialKeys=0,touchClicks=0;
static const char *gameNames[]={"Carmen Sandiego","Monkey Island VGA"};
static int menuGame=0;
static const char *gameCmds[]={"carmen","monkey"};

static void renderSnapshot();
void dos_restart_to_selector();
void dos_restart_home();
class PrismaVideo final : public Faux86::FrameBufferInterface {
  Faux86::RenderSurface surface{};
  uint16_t *cache=nullptr;
  uint16_t *sourceRow=nullptr;
  uint16_t xmap[480],ymap[480],row[480];
  int oldW=0,oldH=0,jobW=0,jobH=0,oldTop=0,jobTop=480;
  bool invalid=true,jobClear=false;
  int dirtyFirst=0,dirtyLast=0;
  uint8_t changedRows[480]={};
  uint32_t viewport=0;
  SemaphoreHandle_t idle=nullptr;
  TaskHandle_t task=nullptr;
  uint32_t profileBlitUs=0;
  static void worker(void *arg){
    auto *self=static_cast<PrismaVideo*>(arg);
    for(;;){
      ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
      traceWorker(1);self->profileBlitUs=0;uint32_t t=micros();renderSnapshot();traceWorker(0);
      uint32_t us=micros()-t;
      __atomic_fetch_add(&self->profileWorkerUs,us,__ATOMIC_RELAXED);
      __atomic_fetch_add(&self->profileRasterUs,us>=self->profileBlitUs?us-self->profileBlitUs:0,__ATOMIC_RELAXED);
      __atomic_fetch_add(&self->profileJobs,1,__ATOMIC_RELAXED);
      uint32_t old=__atomic_load_n(&self->renderMax,__ATOMIC_RELAXED);
      while(us>old&&!__atomic_compare_exchange_n(&self->renderMax,&old,us,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
      xSemaphoreGive(self->idle);
    }
  }
  void present(){
    traceWorker(3);uint32_t t=micros();
    if(jobClear)dosLcd->fillRect(0,0,480,jobTop,0);
    const uint32_t copyStart=micros();
    // Copy disjoint changed row runs, coalescing gaps of up to 2 rows.
    // If heavily fragmented, use the previous bounding rectangle.
    const int left=(480-jobW)/2,top=(jobTop-jobH)/2;
    const unsigned span=unsigned(dirtyLast-dirtyFirst+1);
    int starts[12],ends[12];unsigned runs=0,rows=0;
    bool fallback=jobClear;
    if(!fallback){
      for(int y=dirtyFirst;y<=dirtyLast;++y){
        if(!changedRows[y])continue;
        if(runs&&y-ends[runs-1]<=3){ends[runs-1]=y;continue;}
        if(runs==12){fallback=true;break;}
        starts[runs]=ends[runs]=y;++runs;
      }
    }
    if(fallback||!runs){
      starts[0]=dirtyFirst;ends[0]=dirtyLast;runs=1;
    }
    for(unsigned i=0;i<runs;++i)rows+=unsigned(ends[i]-starts[i]+1);
    // Avoid extra bitmap calls for very small savings.
    if(runs>1&&rows*100>=span*85){starts[0]=dirtyFirst;ends[0]=dirtyLast;runs=1;rows=span;}
    for(unsigned i=0;i<runs;++i)
      dosLcd->draw16bitRGBBitmap(left,top+starts[i],cache+starts[i]*jobW,jobW,ends[i]-starts[i]+1);
    __atomic_fetch_add(&profileSpanPixels,uint32_t(jobW)*span,__ATOMIC_RELAXED);
    __atomic_fetch_add(&profileRuns,runs,__ATOMIC_RELAXED);
    const uint32_t flushStart=micros();
    dosLcd->flush(true);
    const uint32_t done=micros();
    __atomic_fetch_add(&profileClearUs,copyStart-t,__ATOMIC_RELAXED);
    __atomic_fetch_add(&profileCopyUs,flushStart-copyStart,__ATOMIC_RELAXED);
    __atomic_fetch_add(&profileFlushUs,done-flushStart,__ATOMIC_RELAXED);
    __atomic_fetch_add(&profilePixels,uint32_t(jobW)*rows,__ATOMIC_RELAXED);
    __atomic_fetch_add(&profilePresentN,1,__ATOMIC_RELAXED);
    __atomic_fetch_add(&frames,1,__ATOMIC_RELAXED);
    __atomic_fetch_add(&work,micros()-t,__ATOMIC_RELAXED);traceWorker(1);
  }
public:
  uint32_t frames=0, work=0, skipped=0, dropped=0, renderMax=0;
  uint32_t scaleUs=0,scaleCalls=0,scaleRows=0,scalePossible=0,scaleReuse=0;
  uint32_t profileWorkerUs=0,profileRasterUs=0,profileJobs=0;
  uint32_t profileClearUs=0,profileCopyUs=0,profileFlushUs=0,profilePixels=0,profilePresentN=0;
  uint32_t profileSpanPixels=0,profileRuns=0,fusedFrames=0,fusedRows=0;
  void invalidate(){__atomic_store_n(&invalid,true,__ATOMIC_RELAXED);__atomic_store_n(&viewport,0,__ATOMIC_RELAXED);}
  bool touchPosition(int x,int y,uint16_t &nx,uint16_t &ny){uint32_t v=__atomic_load_n(&viewport,__ATOMIC_RELAXED);int w=v&511,h=(v>>9)&511,top=(v>>18)&511;int l=(480-w)/2,t=(top-h)/2;if(w<2||h<2||x<l||x>=l+w||y<t||y>=t+h)return false;nx=uint32_t(x-l)*65535/(w-1);ny=uint32_t(y-t)*65535/(h-1);return true;}
  bool asyncReady(){return task!=nullptr;}
  unsigned stackFree(){return task?(unsigned)uxTaskGetStackHighWaterMark(task):0;}
  bool acquire(){return idle&&xSemaphoreTake(idle,0)==pdTRUE;}
  void release(){if(idle)xSemaphoreGive(idle);}
  void submit(){xTaskNotifyGive(task);}
  void drain(){if(idle){xSemaphoreTake(idle,portMAX_DELAY);xSemaphoreGive(idle);}}
  bool drainForReset(){if(!idle)return true;if(xSemaphoreTake(idle,pdMS_TO_TICKS(750))!=pdTRUE)return false;xSemaphoreGive(idle);return true;}
  bool allocate(){
    cache=(uint16_t*)ps_malloc(480*480*2);
    if(!cache)return false;
    sourceRow=(uint16_t*)heap_caps_malloc(VGA_FRAMEBUFFER_WIDTH*sizeof(uint16_t),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    Serial.printf("DOS SCALE staging=%s\n",sourceRow?"internal":"fallback");
    idle=xSemaphoreCreateBinary();
    if(idle){
      xSemaphoreGive(idle);
      if(xTaskCreatePinnedToCore(worker,"dosLCD",8192,this,3,&task,0)!=pdPASS){vSemaphoreDelete(idle);idle=nullptr;}
    }
    Serial.printf("DOS VGA+LCD worker=%s\n",task?"core0":"sync");
    return true;
  }
  // Native VGA 320x200 data represented by the legacy 640x400 mode.
  // Use the same two-stage nearest-neighbor coordinate mapping exactly,
  // but decode palette colors only once per native source row.
  bool fusedVga(const uint8_t (*planes)[65536],const uint16_t *palette,unsigned start,const uint8_t *dirty,bool full){
    if(!cache||!sourceRow||__atomic_load_n(&hideGuest,__ATOMIC_RELAXED))return false;
    const uint32_t t=micros();
    constexpr int w=640,h=400;
    const int top=panelTop();
    int dw=480,dh=400;
    if(dh>top){dw=max(1,dw*top/dh);dh=top;}
    bool force=__atomic_exchange_n(&invalid,false,__ATOMIC_RELAXED)||full;
    if(w!=oldW||h!=oldH||top!=oldTop){
      force=true;oldW=w;oldH=h;oldTop=top;
      for(int x=0;x<dw;x++)xmap[x]=x*w/dw;
      for(int y=0;y<dh;y++)ymap[y]=y*h/dh;
      Serial.printf("DOS VIDEO %dx%d -> %dx%d fused\n",w,h,dw,dh);
    }
    traceWorker(2);
    dirtyFirst=dh;dirtyLast=-1;memset(changedRows,0,dh);
    int prevY=-1;unsigned processed=0,reused=0,decoded=0;
    uint32_t cooperate=t;
    for(int y=0;y<dh;y++){
      if((y&31)==0&&micros()-cooperate>=15000){vTaskDelay(1);cooperate=micros();}
      unsigned sy=ymap[y]>>1;
      if(!force&&!dirty[sy])continue;
      ++processed;
      if(int(sy)!=prevY){
        const unsigned base=(sy*80+start)&65535;
        for(unsigned x=0;x<320;x+=4){
          unsigned a=(base+(x>>2))&65535;
          sourceRow[x]=palette[planes[0][a]];
          sourceRow[x+1]=palette[planes[1][a]];
          sourceRow[x+2]=palette[planes[2][a]];
          sourceRow[x+3]=palette[planes[3][a]];
        }
        for(int x=0;x<dw;x++)row[x]=sourceRow[xmap[x]>>1];
        prevY=sy;++decoded;
      }else ++reused;
      uint16_t *prev=cache+y*dw;
      if(force||memcmp(prev,row,dw*2)){memcpy(prev,row,dw*2);changedRows[y]=1;dirtyFirst=min(dirtyFirst,y);dirtyLast=y;}
    }
    const uint32_t elapsed=micros()-t;
    __atomic_fetch_add(&scaleUs,elapsed,__ATOMIC_RELAXED);
    __atomic_fetch_add(&scaleCalls,1,__ATOMIC_RELAXED);
    __atomic_fetch_add(&scaleRows,processed,__ATOMIC_RELAXED);
    __atomic_fetch_add(&scaleReuse,reused,__ATOMIC_RELAXED);
    __atomic_fetch_add(&scalePossible,dh,__ATOMIC_RELAXED);
    __atomic_fetch_add(&fusedFrames,1,__ATOMIC_RELAXED);
    __atomic_fetch_add(&fusedRows,decoded,__ATOMIC_RELAXED);
    jobW=dw;jobH=dh;jobTop=top;jobClear=force;
    __atomic_store_n(&viewport,uint32_t(dw)|(uint32_t(dh)<<9)|(uint32_t(top)<<18),__ATOMIC_RELAXED);
    if(dirtyLast>=dirtyFirst)present();else __atomic_fetch_add(&skipped,1,__ATOMIC_RELAXED);
    traceWorker(1);profileBlitUs+=micros()-t;
    return true;
  }
  Faux86::RenderSurface *getSurface() override {return &surface;}
  void setPalette(Faux86::Palette*) override {}
  void blit(uint16_t *pixels,int w,int h,int stride) override {
    if(!pixels||w<=0||h<=0||w>VGA_FRAMEBUFFER_WIDTH||h>VGA_FRAMEBUFFER_HEIGHT||stride<w*2||!cache)return;
    if(__atomic_load_n(&hideGuest,__ATOMIC_RELAXED)){__atomic_store_n(&invalid,true,__ATOMIC_RELAXED);__atomic_fetch_add(&skipped,1,__ATOMIC_RELAXED);return;}
    const uint32_t profileBlitStart=micros();
    int dw=min(w,480),dh=min(h,480);
    if((w==320||w==640)&&h==200){dw=480;dh=360;}
    const int top=panelTop();if(dh>top){dw=max(1,dw*top/dh);dh=top;}
    bool force=__atomic_exchange_n(&invalid,false,__ATOMIC_RELAXED);
    if(w!=oldW||h!=oldH||top!=oldTop){force=true;oldW=w;oldH=h;oldTop=top;for(int x=0;x<dw;x++)xmap[x]=x*w/dw;for(int y=0;y<dh;y++)ymap[y]=y*h/dh;Serial.printf("DOS VIDEO %dx%d -> %dx%d\n",w,h,dw,dh);}
    traceWorker(2);const uint32_t scaleStart=micros();uint32_t lastCooperate=scaleStart;
    bool changed=force;dirtyFirst=dh;dirtyLast=-1;
    memset(changedRows,0,dh);
    int prevY=-1;
    unsigned processed=0,reused=0;
    const bool aliasValid=prismaScaleDirtyValid&&pixels==vga_framebuffer;
    const bool useHints=prismaScaleDirtyValid&&!force&&pixels==vga_framebuffer;
    for(int y=0;y<dh;y++){
      if((y&31)==0&&micros()-lastCooperate>=15000){vTaskDelay(1);lastCooperate=micros();}
      if(useHints&&!prismaScaleDirtyRows[ymap[y]])continue;
      ++processed;
      const unsigned sy=ymap[y];
      // Only use aliases explicitly emitted by the current raster snapshot.
      // No inference from mode/resolution or previous frames.
      const unsigned hint=aliasValid?prismaScaleAlias[sy]:0;
      const unsigned sourceY=hint&&hint<=sy?hint-1:sy;
      const uint16_t *src=(const uint16_t*)((const uint8_t*)pixels+sourceY*stride);
      if(int(sourceY)!=prevY){
        if(dw==w)memcpy(row,src,dw*2);
        else{
          // Read the source row sequentially from PSRAM, then gather
          // nearest-neighbor pixels from internal RAM instead.
          const uint16_t *sample=src;
          if(sourceRow){memcpy(sourceRow,src,w*sizeof(uint16_t));sample=sourceRow;}
          int x=0;
          for(;x+3<dw;x+=4){
            row[x]=sample[xmap[x]];row[x+1]=sample[xmap[x+1]];
            row[x+2]=sample[xmap[x+2]];row[x+3]=sample[xmap[x+3]];
          }
          for(;x<dw;x++)row[x]=sample[xmap[x]];
        }
        prevY=sourceY;
      }else ++reused;
      uint16_t *prev=cache+y*dw;
      if(force||memcmp(prev,row,dw*2)){memcpy(prev,row,dw*2);changed=true;changedRows[y]=1;dirtyFirst=min(dirtyFirst,y);dirtyLast=y;}
    }
    __atomic_fetch_add(&scaleUs,micros()-scaleStart,__ATOMIC_RELAXED);
    __atomic_fetch_add(&scaleCalls,1,__ATOMIC_RELAXED);
    __atomic_fetch_add(&scaleRows,processed,__ATOMIC_RELAXED);
    __atomic_fetch_add(&scaleReuse,reused,__ATOMIC_RELAXED);
    __atomic_fetch_add(&scalePossible,dh,__ATOMIC_RELAXED);
    jobW=dw;jobH=dh;jobTop=top;jobClear=force;
    __atomic_store_n(&viewport,uint32_t(dw)|(uint32_t(dh)<<9)|(uint32_t(top)<<18),__ATOMIC_RELAXED);
    if(changed)present();else __atomic_fetch_add(&skipped,1,__ATOMIC_RELAXED);
    traceWorker(1);profileBlitUs+=micros()-profileBlitStart;
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
// Memory-mapped immutable PC ROM: never allocate a second copy in SRAM.
class FlashROM final : public Faux86::DiskInterface {
 const uint8_t *data;uint64_t length,position=0;
public:
 FlashROM(const uint8_t *p,uint64_t n):data(p),length(n){}
 bool isValid() override {return data&&length;}
 uint64_t getSize() override {return length;}
 uint64_t seek(uint64_t offset) override {position=offset;return position;}
 int read(uint8_t *buffer,unsigned count) override {
   if(position>=length)return 0;
   unsigned n=(unsigned)min<uint64_t>(count,length-position);
   memcpy(buffer,data+position,n);position+=n;return n;
 }
 int write(const uint8_t*,unsigned) override {return 0;}
};
class FlashDisk final : public Faux86::DiskInterface {
 const esp_partition_t *part; uint32_t base,len,pos=0;
 static constexpr uint32_t blockSize=4096,blocks=32;
 uint8_t *cache=nullptr; uint32_t tags[blocks];
 const uint8_t *mapped=nullptr;
 esp_partition_mmap_handle_t mapHandle=0;
public:
 FlashDisk(const esp_partition_t *p,uint32_t b,uint32_t n):part(p),base(b),len(n){
   for(auto &t:tags)t=UINT32_MAX;
   if(!isValid())return;
   const void *ptr=nullptr;
   esp_err_t err=esp_partition_mmap(part,base,len,ESP_PARTITION_MMAP_DATA,&ptr,&mapHandle);
   if(err==ESP_OK){
     mapped=static_cast<const uint8_t*>(ptr);
     // Validate beginning/end and both sides of 64 KiB physical boundaries.
     // The disk is read-only, so mapped contents never require invalidation.
     uint8_t sample[64];
     auto check=[&](uint32_t offset){
       const uint32_t count=min<uint32_t>(sizeof(sample),len-offset);
       return esp_partition_read(part,base+offset,sample,count)==ESP_OK&&
              memcmp(sample,mapped+offset,count)==0;
     };
     bool ok=mapped&&check(0)&&check(len>64?len-64:0);
     uint32_t boundary=(0x10000u-((part->address+base)&0xffffu))&0xffffu;
     if(!boundary)boundary=0x10000u;
     for(uint32_t offset=boundary;ok&&offset<len;offset+=0x10000u){
       ok=check(offset>=32?offset-32:0);
       if((offset&0x7ffffu)<0x10000u)vTaskDelay(1);
     }
     if(ok){
       Serial.printf("DOS DISK mmap OK base=%u bytes=%u psram_saved=%u\n",base,len,blockSize*blocks);
       return;
     }
     esp_partition_munmap(mapHandle);mapped=nullptr;
     Serial.printf("DOS DISK mmap validation failed base=%u; fallback\n",base);
   }else Serial.printf("DOS DISK mmap unavailable base=%u err=%d; fallback\n",base,(int)err);
   cache=(uint8_t*)ps_malloc(blockSize*blocks);
   Serial.printf("DOS DISK backend=%s base=%u bytes=%u\n",cache?"cache":"partition_read",base,len);
 }
 ~FlashDisk(){if(mapped)esp_partition_munmap(mapHandle);free(cache);}
 bool isValid() override {return part && len && base<=part->size && len<=part->size-base;}
 uint64_t getSize() override {return len;}
 uint64_t seek(uint64_t offset) override {pos=offset>len?len:offset;return pos;}
 int read(uint8_t *buf,unsigned count) override {
   if(!isValid())return 0; count=static_cast<unsigned>(min<uint32_t>(static_cast<uint32_t>(count),len-pos));
   if(!count)return 0;
   uint32_t t=micros();unsigned done=0;
   if(mapped){
     memcpy(buf,mapped+pos,count);pos+=count;
     ++diskMappedReads;diskMappedBytes+=count;diskReadUs+=micros()-t;
     return count;
   }
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
bool prismaFusedVga(const uint8_t (*planes)[65536],const uint16_t *palette,unsigned start,const uint8_t *dirty,bool full){return host.video.fusedVga(planes,palette,start,dirty,full);}
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
static bool monkeyTapArmed=false;
static uint32_t monkeyTapAt=0,monkeySingle=0,monkeyDouble=0,monkeyClicksApplied=0,monkeyPositionMaxUs=0;
static int monkeyTapX=0,monkeyTapY=0;
// Experimental per-session opt-in for the validated installed Monkey Island.
// Never apply this address map to other games.
static bool monkeyRamTouch=false,monkeyRamFlight=false,monkeyRamClick=false;
static uint16_t monkeyRamX=0,monkeyRamY=0;
static uint32_t monkeyRamStarted=0,monkeyRamWrites=0,monkeyRamRejects=0;
static uint32_t monkeyRamRecoveries=0,monkeyRamBlockedAt=0;
static bool monkeyRamBlocked=false;
// One independent activation pulse for RAM-touch Monkey Island.
// Do not reuse held: dos_touch_game releases that key on finger-up.
static bool monkeyClickPending=false,monkeyClickDown=false,monkeyClickMouse=false;
static uint32_t monkeyClickAt=0,monkeyClickRead=0,monkeyClickWait=0;
static uint16_t monkeyClickBiosHead=0,monkeyClickBiosTail=0;
static const char *monkeyClickSource="none";
static void monkeyClickCancel();
// Two nearby completed taps produce one guest click. Unsigned subtraction handles millis wrap.
static bool monkeyDoubleTap(bool armed,uint32_t now,uint32_t prior,int x,int y,int oldX,int oldY,bool ramTouch=false){
 return armed && uint32_t(now-prior)<=(ramTouch?500u:350u) && abs(x-oldX)+abs(y-oldY)<=(ramTouch?32:24);
}
static bool queueDirect(int x,int y,bool click){
 uint16_t nx,ny;
 if(!host.video.touchPosition(x,y,nx,ny))return false;
 // A release click is never overwritten by a later motion sample.
 if(directQueued&&directClick&&!click)return false;
 directX=nx;directY=ny;directQueued=true;directClick=click;return true;
}
static void cancelDirect(){
 monkeyClickCancel();
 monkeyTapArmed=false;
 directQueued=directClick=directFlightClick=false;directAt=directRelease=0;monkeyRamFlight=monkeyRamClick=false;
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
static void monkeyClickCancel(){
 if(vm&&monkeyClickDown){
   if(monkeyClickMouse)dos_mouse(0,0,false);
   else{vm->input.handleKeyUp(28);vm->input.tick();}
   Serial.println("DOS ACTION cancelled: released");
 }
 monkeyClickPending=monkeyClickDown=false;
}
static bool monkeyClickRequest(bool mouse,const char *source){
 if(!vm||!monkeyRamTouch||activeGame!=1||launcher||bootPhase||loadingGame||launchPending||benchUntil||pendingPanel!=255)return false;
 if(monkeyClickPending||monkeyClickDown){Serial.println("DOS ACTION busy");return false;}
 monkeyClickPending=true;monkeyClickMouse=mouse;monkeyClickSource=source;monkeyClickWait=millis();
 Serial.printf("DOS ACTION request source=%s method=%s\n",source,mouse?"mouse":"enter");
 return true;
}
static void monkeyClickTick(){
 if(!monkeyClickPending&&!monkeyClickDown)return;
 if(!vm||!monkeyRamTouch||activeGame!=1||launcher||bootPhase||loadingGame||launchPending||benchUntil||pendingPanel!=255){monkeyClickCancel();return;}
 const uint32_t now=millis();
 if(monkeyClickDown){
   const unsigned reads=vm->input.prismaReadCount-monkeyClickRead;
   if(now-monkeyClickAt<180||(!monkeyClickMouse&&!reads&&now-monkeyClickAt<600))return;
   if(monkeyClickMouse)dos_mouse(0,0,false);
   else{vm->input.handleKeyUp(28);vm->input.tick();}
   Serial.printf("DOS ACTION up source=%s method=%s ms=%u guest_reads=%u bios=%u,%u->%u,%u queue=%d\n",monkeyClickSource,monkeyClickMouse?"mouse":"enter",now-monkeyClickAt,reads,monkeyClickBiosHead,monkeyClickBiosTail,vm->memory.readWord(0x41a),vm->memory.readWord(0x41c),vm->input.freeSlots());
   monkeyClickDown=false;return;
 }
 if(held||directQueued||monkeyRamFlight||vm->input.freeSlots()<8){
   if(now-monkeyClickWait>=700){monkeyClickPending=false;Serial.println("DOS ACTION timeout waiting for idle input/position");}
   return;
 }
 monkeyClickPending=false;monkeyClickDown=true;monkeyClickAt=now;monkeyClickRead=vm->input.prismaReadCount;
 monkeyClickBiosHead=vm->memory.readWord(0x41a);monkeyClickBiosTail=vm->memory.readWord(0x41c);
 if(monkeyClickMouse)dos_mouse(0,0,true);
 else{vm->input.handleKeyDown(28);vm->input.tick();}
 ++touchClicks;++monkeyClicksApplied;
 Serial.printf("DOS ACTION down source=%s method=%s queue=%d guest_read=%u\n",monkeyClickSource,monkeyClickMouse?"mouse":"enter",vm->input.freeSlots(),monkeyClickRead);
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
 dosLcd->drawRect(40,407,400,12,0x7bef);dosLcd->fillRect(42,409,(396*stage)/100,8,0x5e9e);dosLcd->flush(true);diag_logf(false, "DOS LOAD stage=%u %s\n",stage,label);
}
static void showLoading(int game){
 host.video.drain();__atomic_store_n(&hideGuest,true,__ATOMIC_RELAXED);loadStage=0;
 dosLcd->fillScreen(0);centered(game<0?"PRISMA RETRO / DOS":"PRISMA RETRO",28,2,0x9cfa);
 int art=game<0?3:game;
 if(DosArt::large[art])dosLcd->draw16bitRGBBitmap(96,82,const_cast<uint16_t*>(DosArt::large[art]),288,180);
 else centered(game<0?"MS-DOS 6.22":"DOS GAME",148,4);
 if(game>=0)centered(gameNames[game],280,2);
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
static void drawDosCarousel(){
 int i=menuGame;dosLcd->fillScreen(0x0842);
 dosLcd->fillRoundRect(12,10,90,44,8,0x2124);
 dosLcd->setTextColor(0xffff);dosLcd->setTextSize(2);dosLcd->setCursor(30,24);dosLcd->print("HOME");
 dosLcd->setTextSize(3);dosLcd->setCursor(114,10);dosLcd->print("PRISMA DOS");
 dosLcd->setTextSize(1);dosLcd->setCursor(114,39);dosLcd->print("2 JUEGOS / ESPANOL");
 dosLcd->fillRoundRect(56,70,368,344,8,0x2124);dosLcd->drawRoundRect(56,70,368,344,8,0x5e9e);
 if(DosArt::large[i]){static uint16_t line[344];for(int y=0;y<215;y++){for(int x=0;x<344;x++)line[x]=DosArt::large[i][(y*180/215)*288+x*288/344];dosLcd->draw16bitRGBBitmap(68,100+y,line,344,1);if((y&31)==31)vTaskDelay(1);}}
 else centered("SIN PREVIEW",202,2);
 centered(gameNames[i],352,2);centered(i==0?"MS-DOS / ESPANOL":"VGA / 256 COLORES",382,1,0x9cfa);
 for(int side=0;side<2;side++){int x=side?430:6;bool enabled=side?i<1:i>0;uint16_t ink=enabled?0xffff:0x4208;dosLcd->fillRoundRect(x,184,44,84,6,enabled?0x39a7:0x18c3);dosLcd->drawRoundRect(x,184,44,84,6,ink);dosLcd->setTextColor(ink);dosLcd->setTextSize(4);dosLcd->setCursor(x+10,211);dosLcd->print(side?">":"<");}
 dosLcd->fillRoundRect(12,436,150,32,6,0x2124);dosLcd->fillRoundRect(318,436,150,32,6,0x366e);
 dosLcd->setTextColor(0xffff);dosLcd->setTextSize(2);dosLcd->setCursor(24,444);dosLcd->print("USB");dosLcd->setCursor(352,444);dosLcd->print("JUGAR");
 char pos[12];snprintf(pos,sizeof(pos),"%d/2",i+1);centered(pos,444,2,0x9cfa);dosLcd->flush(true);
}
static void openLauncher(){
 startupReset(false);
 monkeyRamTouch=false;
 benchUntil=0;bootPhase=false;loadingGame=false;loadingIndex=-1;consoleMode=false;activeGame=-1;clearText();vm->cpu.prismaMouseEnabled=false;vm->cpu.prismaMousePending=false;cancelDirect();__atomic_store_n(&hideGuest,true,__ATOMIC_RELAXED);
 dos_key(0);dos_mouse(0,0,false);
 host.video.drain();launcher=true;host.timer.pause();
 menuGame=0;drawDosCarousel();Serial.println("DOS TOUCH LAUNCHER");
}
bool dos_launcher_touch(unsigned n,int x,int y){
 static bool down=false,blocked=false;static int choice=-1,sx=0,sy=0,ex=0,ey=0;
 if(!launcher){down=blocked=false;choice=-1;return false;}
 if(n>1){blocked=true;down=false;choice=-1;return true;}
 if(blocked){if(!n)blocked=false;return true;}
 if(n==1){if(!down){down=true;sx=x;sy=y;choice=(x>=12&&x<102&&y>=10&&y<54)?3:(x>=12&&x<162&&y>=436&&y<468)?4:(x>=6&&x<50&&y>=184&&y<268)?5:(x>=430&&x<474&&y>=184&&y<268)?6:((x>=56&&x<424&&y>=70&&y<414)||(x>=318&&x<468&&y>=436&&y<468))?menuGame:-1;}ex=x;ey=y;}
 if(!n&&down){
  down=false;int dx=ex-sx,dy=ey-sy;
  if(abs(dx)>55&&abs(dx)>abs(dy))choice=dx<0?6:5;else if(abs(dx)>35||abs(dy)>35)choice=-1;
  if(choice==5||choice==6){int next=menuGame+(choice==5?-1:1);if(next>=0&&next<2){menuGame=next;drawDosCarousel();}return true;}
  if(choice==3){diag_logf(false,"DOS EXIT cause=home\n");dos_restart_home();return true;}
  if(choice==4){consoleMode=true;activeGame=-1;clearText();__atomic_store_n(&hideGuest,false,__ATOMIC_RELAXED);host.timer.resume();vm->video.updatedscreen=true;launcher=false;host.video.invalidate();setPanel(1);Serial.println("DOS USB CONSOLE");}
  if(choice>=0&&choice<2){
   const char *cmd=gameCmds[choice];diag_logf(false, "DOS LAUNCH %s\n",cmd);
   if(!injectCommand(cmd)){Serial.println("DOS command rejected: not at empty prompt");return true;}
   activeGame=choice;clearText();vm->cpu.prismaMouseEnabled=true;vm->cpu.prismaMousePending=false;
   startupReset(choice==1);
   monkeyRamTouch=choice==1;monkeyRamBlocked=false;monkeyRamWrites=monkeyRamRejects=monkeyRamRecoveries=0;
   if(monkeyRamTouch)Serial.println("DOS RAM TOUCH armed at launch; waits for coherent cursor");
   showLoading(choice);loadingGame=true;loadingIndex=choice;loadProgress(35,"Comando enviado");
   mouseMode=true;__atomic_store_n(&panelMode,(uint8_t)(choice==1?8:0),__ATOMIC_RELAXED);host.timer.resume();vm->video.updatedscreen=true;launcher=false;launchPending=true;promptGone=false;launchTime=millis();host.video.invalidate();
  }
  choice=-1;
 }
 return true;
}
void dos_show_launcher(){
 if(vm&&!launcher&&!launchPending&&atPrompt()){
   if(activeGame>=0)return; // Active games use the debounced prompt check in dos_tick.
   openLauncher();
 }
}
bool dos_in_launcher(){return launcher;}
bool dos_prepare_restart(){
 // Called only by the producer/Arduino loop. No further VM slices or
 // LCD jobs are submitted after the restart request.
 __atomic_store_n(&hideGuest,true,__ATOMIC_RELAXED);
 host.timer.pause();
 traceMain(40);
 bool idle=host.video.drainForReset();
 diag_logf(false,"DOS RESTART video_idle=%d\n",(int)idle);
 return idle;
}
void dos_request_exit(){
 if(!vm||launcher)return;
 // Explicit PRISMA EXIT is a full reset, not a guest Escape key.
 // No wait for a hung VM, LCD worker, or keyboard acknowledgement.
 diag_logf(false,"DOS EXIT explicit reset game=%d\n",activeGame);
 dos_restart_to_selector();
}
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
// Read-only, on-demand scan of conventional guest RAM. No guest writes.
// The main DOS producer is paused during each snapshot; LCD worker may finish.
namespace {
constexpr uint32_t CURSOR_RAM_BASE=0x500, CURSOR_RAM_END=0xa0000;
constexpr uint32_t CURSOR_RAM_N=CURSOR_RAM_END-CURSOR_RAM_BASE;
constexpr uint32_t CURSOR_MASK_N=(CURSOR_RAM_N+7)/8;
// Read-only, explicitly requested snapshots of the user's known four digits.
// No game-input injection, no writes and no scan outside the diagnostic.
static uint8_t *keyScanBase=nullptr,*keyScanCurrent=nullptr;
static char keyScanDigits[5]={};
static uint16_t keyScanValue=0,keyScanBcd=0;
static bool keyScanBusy=false,keyScanBefore=false,keyScanReady=false;
static uint32_t keyScanIndex=0,keyScanLast=0,keyScanCount[3][2]={},keyScanAddr[3][2][8]={};
static const char *const keyScanNames[]={"ASCII","U16LE","BCD16LE"};
static void keyScanFree(){
 free(keyScanBase);free(keyScanCurrent);keyScanBase=keyScanCurrent=nullptr;
 keyScanBusy=keyScanReady=false;keyScanDigits[0]=0;keyScanLast=0;keyScanIndex=0;
}
static bool keyScanMatch(const uint8_t *p,unsigned m){
 if(m==0)return memcmp(p,keyScanDigits,4)==0;
 const unsigned n=unsigned(p[0])|(unsigned(p[1])<<8);
 return n==(m==1?keyScanValue:keyScanBcd);
}
static bool keyScanAvailable(){
 return vm&&activeGame==1&&!launcher&&!bootPhase&&!loadingGame&&!launchPending&&!benchUntil;
}
static void keyScanReport(){
 if(keyScanBusy){Serial.printf("DOS KEYSCAN busy %u/%u\n",keyScanIndex,CURSOR_RAM_N);return;}
 if(!keyScanReady){Serial.println("DOS KEYSCAN no completed snapshot");return;}
 Serial.printf("DOS KEYSCAN %s code=%s\n",keyScanBefore?"BEFORE":"AFTER",keyScanDigits);
 for(unsigned m=0;m<3;++m){
   for(unsigned k=0;k<2;++k){
     if(keyScanBefore&&k)continue;
     char out[180];unsigned n=snprintf(out,sizeof(out),"DOS KEYSCAN %s %s count=%u addr=",keyScanNames[m],k?"new":(keyScanBefore?"before":"retained"),keyScanCount[m][k]);
     for(unsigned j=0;j<min<uint32_t>(keyScanCount[m][k],8u)&&n+8<sizeof(out);++j)n+=snprintf(out+n,sizeof(out)-n,"%s%05lx",j?",":"",(unsigned long)keyScanAddr[m][k][j]);
     Serial.println(out);
   }
 }
}
static void keyScanStart(bool before){
 if(!keyScanAvailable()){Serial.println("DOS KEYSCAN wait for Monkey Island screen");return;}
 if(keyScanBusy){Serial.println("DOS KEYSCAN busy; wait for result");return;}
 if(!keyScanBase||!keyScanCurrent){Serial.println("DOS KEYSCAN use :clave ref 1234 first");return;}
 uint32_t t=micros();
 memcpy(before?keyScanBase:keyScanCurrent,vm->memory.RAM+CURSOR_RAM_BASE,CURSOR_RAM_N);
 keyScanBefore=before;keyScanReady=false;keyScanBusy=true;keyScanIndex=0;keyScanLast=millis();
 memset(keyScanCount,0,sizeof(keyScanCount));
 Serial.printf("DOS KEYSCAN captured %s code=%s snapshot_us=%u\n",before?"before":"after",keyScanDigits,micros()-t);
}
static void keyScanTick(){
 if(!keyScanBase)return;
 if(!keyScanAvailable()||millis()-keyScanLast>300000u){keyScanFree();Serial.println("DOS KEYSCAN released");return;}
 if(!keyScanBusy)return;
 const uint8_t *p=keyScanBefore?keyScanBase:keyScanCurrent;
 const uint32_t end=min(CURSOR_RAM_N-3,keyScanIndex+4096u);
 for(uint32_t i=keyScanIndex;i<end;++i){
   for(unsigned m=0;m<3;++m){
     if(!keyScanMatch(p+i,m))continue;
     const unsigned k=(!keyScanBefore&&!keyScanMatch(keyScanBase+i,m))?1:0;
     uint32_t &count=keyScanCount[m][k];
     if(count<8)keyScanAddr[m][k][count]=CURSOR_RAM_BASE+i;
     ++count;
   }
 }
 keyScanIndex=end;
 if(end==CURSOR_RAM_N-3){keyScanBusy=false;keyScanReady=true;keyScanReport();}
}
static void keyScanCommand(const char *arg){
 if(!strcmp(arg,"stop")){keyScanFree();Serial.println("DOS KEYSCAN stopped; buffers released");return;}
 if(!strcmp(arg,"list")){keyScanReport();return;}
 if(!keyScanAvailable()){Serial.println("DOS KEYSCAN unavailable until Monkey Island is displayed");return;}
 if(!strncmp(arg,"ref ",4)){
   const char *p=arg+4;
   if(strlen(p)!=4||strspn(p,"0123456789")!=4){Serial.println("DOS KEYSCAN use :clave ref DDDD");return;}
   keyScanFree();
   keyScanBase=(uint8_t*)ps_malloc(CURSOR_RAM_N);keyScanCurrent=(uint8_t*)ps_malloc(CURSOR_RAM_N);
   if(!keyScanBase||!keyScanCurrent){keyScanFree();Serial.println("DOS KEYSCAN insufficient PSRAM");return;}
   memcpy(keyScanDigits,p,5);keyScanValue=keyScanBcd=0;
   for(unsigned i=0;i<4;++i){keyScanValue=keyScanValue*10+(p[i]-'0');keyScanBcd=(keyScanBcd<<4)|(p[i]-'0');}
   keyScanStart(true);return;
 }
 if(!strcmp(arg,"after")){keyScanStart(false);return;}
 Serial.println("DOS KEYSCAN :clave ref DDDD / after / list / stop");
}
static uint8_t *cursorRamPrev=nullptr,*cursorRamNow=nullptr,*cursorMasks=nullptr;
static unsigned cursorRounds=0;
// These are physical guest addresses observed in the 059R3 experiment.
// They are hypotheses for this specific installed game, not verified offsets.
static bool cursorWatch=false;
static uint8_t cursorWatchKey=0;
static uint32_t cursorWatchNext=0;
// Explicit one-shot experiments only. No touch-driven writes and no persistence.
static uint32_t cursorProbeStart=0;
static unsigned cursorProbePhase=0;
static constexpr uint32_t cursorRenderX=0x2e5f0,cursorRenderY=0x2e5f2,cursorLogicalX=0x30df4,cursorLogicalY=0x30f2a;
static constexpr uint32_t cursorWatchAddresses[]={0x2e5f0,0x2e5f2,0x30df4,0x33fb4,0x2e5fb,0x2ed74};
static unsigned cursorWatchWord(uint32_t a){
 return unsigned(vm->memory.readByte(a))|(unsigned(vm->memory.readByte(a+1))<<8);
}
static bool monkeyRamCoherent(){
 if(!vm||activeGame!=1||launcher||bootPhase||loadingGame||launchPending||benchUntil)return false;
 const unsigned x=cursorWatchWord(cursorLogicalX),y=cursorWatchWord(cursorLogicalY);
 return x<=319&&y<=199&&int16_t(cursorWatchWord(cursorRenderX))==int(x)-8&&cursorWatchWord(cursorRenderY)==y+9;
}
static void monkeyRamReport(const char *event){
 Serial.printf("DOS RAM TOUCH %s enabled=%u coherent=%u logical=%u,%u render=%d,%u target=%u,%u writes=%u rejected=%u recovered=%u flight=%u blocked=%u\n",
 event,unsigned(monkeyRamTouch),unsigned(monkeyRamCoherent()),vm?cursorWatchWord(cursorLogicalX):0,vm?cursorWatchWord(cursorLogicalY):0,
 vm?int(int16_t(cursorWatchWord(cursorRenderX))):0,vm?cursorWatchWord(cursorRenderY):0,
 monkeyRamX,monkeyRamY,monkeyRamWrites,monkeyRamRejects,monkeyRamRecoveries,unsigned(monkeyRamFlight),unsigned(monkeyRamBlocked));
}
static void monkeyRamTick(){
 if(!monkeyRamTouch||monkeyClickDown)return;
 if(!vm||activeGame!=1||launcher){monkeyRamTouch=false;monkeyRamFlight=monkeyRamClick=false;directQueued=directClick=false;return;}
 if(bootPhase||loadingGame||launchPending){monkeyRamFlight=monkeyRamClick=false;directQueued=directClick=false;return;}
 if(held||benchUntil||pendingPanel!=255){monkeyRamFlight=monkeyRamClick=false;directQueued=directClick=false;return;}
 if(monkeyRamFlight){
   const bool matched=monkeyRamCoherent()&&cursorWatchWord(cursorLogicalX)==monkeyRamX&&cursorWatchWord(cursorLogicalY)==monkeyRamY;
   if(matched){
     if(monkeyRamClick){
       if(vm->input.freeSlots()<8){
         if(millis()-monkeyRamStarted<300)return;
         ++monkeyRamRejects;
       }else{monkeyClickRequest(false,"double");}
     }
     ++directMoves;monkeyPositionMaxUs=max(monkeyPositionMaxUs,uint32_t(millis()-monkeyRamStarted)*1000u);
     monkeyRamFlight=monkeyRamClick=false;
   }else if(millis()-monkeyRamStarted>=1000){
     ++monkeyRamRejects;++directTimeouts;monkeyRamFlight=monkeyRamClick=false;
     // Never send a delayed action after an unconfirmed position. Keep the
     // feature armed so the next coherent touch can recover automatically.
     directQueued=directClick=false;
     monkeyRamBlocked=true;monkeyRamBlockedAt=millis();
     monkeyRamReport("readback_timeout");
     return;
   }else return;
 }
 // No RAM checks in an idle frame. A blocked state is checked at 10Hz.
 if(!directQueued){
   if(!monkeyRamBlocked||millis()-monkeyRamBlockedAt<100)return;
   monkeyRamBlockedAt=millis();
 }
 if(!monkeyRamCoherent()){
   if(directQueued){
     ++monkeyRamRejects;directQueued=directClick=false;
     if(!monkeyRamBlocked){monkeyRamBlocked=true;monkeyRamBlockedAt=millis();monkeyRamReport("waiting_coherence");}
   }
   return;
 }
 if(monkeyRamBlocked){
   monkeyRamBlocked=false;++monkeyRamRecoveries;
   monkeyRamReport("recovered");
 }
 if(!directQueued)return;
 const uint16_t x=(uint32_t(directX)*319+32767)/65535;
 const uint16_t y=(uint32_t(directY)*199+32767)/65535;
 monkeyRamX=x;monkeyRamY=y;monkeyRamClick=directClick;directQueued=directClick=false;
 if(cursorWatchWord(cursorLogicalX)!=x||cursorWatchWord(cursorLogicalY)!=y){
   vm->memory.writeWord(cursorLogicalX,x);vm->memory.writeWord(cursorLogicalY,y);++monkeyRamWrites;
 }
 monkeyRamStarted=millis();monkeyRamFlight=true;
}
static void cursorWatchLog(const char *event,uint8_t key){
 unsigned v[6];
 for(unsigned i=0;i<6;++i)v[i]=cursorWatchWord(cursorWatchAddresses[i]);
 Serial.printf("DOS RAM WATCH %s key=%u XY=%d,%u Xalt=%u,%u Yalt=%u,%u nextY=%u\n",event,(unsigned)key,int(int16_t(v[0])),v[1],v[2],v[3],v[4],v[5],cursorWatchWord(0x30df6));
}
static bool cursorProbeSafe(){
 if(!cursorWatch||!vm||activeGame!=1||launcher||bootPhase||loadingGame||benchUntil||launchPending||held||cursorProbePhase)return false;
 const int x=int(int16_t(cursorWatchWord(cursorRenderX)));
 const unsigned y=cursorWatchWord(cursorRenderY),lx=cursorWatchWord(cursorLogicalX);
 // Require the exact relation and limits established by the four-corner test.
 // This is a heuristic guard, not a game version/signature check.
 return x>=-8&&x<=311&&y>=9&&y<=208&&lx<=319&&int(lx)==x+8;
}
static void cursorProbe(int x,int y,bool logicalOnly){
 if(x<0||x>319||(!logicalOnly&&(y<0||y>199))){Serial.println("DOS RAM PROBE rejected: logical range X=0..319 Y=0..199");return;}
 if(!cursorProbeSafe()){Serial.println("DOS RAM PROBE rejected: watch must be active, cursor idle, observed bounds and X relation must match");return;}
 cursorWatchLog("before",0);
 if(logicalOnly){
   vm->memory.writeWord(cursorLogicalX,uint16_t(x));
 }else{
   vm->memory.writeWord(cursorRenderX,uint16_t(int16_t(x-8)));
   vm->memory.writeWord(cursorRenderY,uint16_t(y+9));
 }
 cursorWatchLog(logicalOnly?"writeX":"writeXY",0);
 cursorProbeStart=millis();cursorProbePhase=1;
}
// Find the logical Y by equality with the observed graphical Y minus 9.
// The snapshot is coherent on the guest task; filtering is incremental.
static uint8_t *cursorYSnapshot=nullptr,*cursorYMasks=nullptr;
static uint32_t cursorYIndex=0,cursorYCounts[2]={},cursorYAddr[2][24]={};
static unsigned cursorYRounds=0,cursorYExpected=0,cursorYLast=0;
static bool cursorYBusy=false,cursorYActive=false;
static uint32_t cursorYDue=0;
static bool cursorYSeen[200]={};
static unsigned cursorYDistinct=0;
static uint32_t cursorYProbeAddr=0;
static unsigned cursorYProbeTarget=0,cursorYProbeX=0;
static bool cursorYProbePending=false;
static void cursorYFree(){
 free(cursorYSnapshot);free(cursorYMasks);cursorYSnapshot=cursorYMasks=nullptr;
 cursorYBusy=cursorYActive=false;cursorYRounds=0;cursorYDue=0;cursorYDistinct=0;memset(cursorYSeen,0,sizeof(cursorYSeen));cursorYProbePending=false;
}
static void cursorYList(){
 if(!cursorYActive){Serial.println("DOS YFIND inactive");return;}
 if(cursorYBusy){Serial.printf("DOS YFIND busy %u/%u\n",cursorYIndex,CURSOR_RAM_N-1);return;}
 Serial.printf("DOS YFIND count rounds=%u y=%u Y16=%u Y8=%u\n",cursorYRounds,cursorYLast,cursorYCounts[0],cursorYCounts[1]);
 for(unsigned m=0;m<2;++m)for(unsigned n=0;n<min<uint32_t>(cursorYCounts[m],24u);++n){
   const uint32_t a=cursorYAddr[m][n];
   Serial.printf("DOS YFIND %s addr=%05lx value=%u deltaX=%ld\n",m?"Y8":"Y16",(unsigned long)a,m?unsigned(vm->memory.readByte(a)):cursorWatchWord(a),long(a)-long(cursorLogicalX));
 }
}
static void cursorYSample(){
 if(cursorYBusy||!cursorProbeSafe())return;
 unsigned y=cursorWatchWord(cursorRenderY)-9;
 if(cursorYRounds&&y==cursorYLast)return;
 if(!cursorYSnapshot){
   cursorYSnapshot=(uint8_t*)ps_malloc(CURSOR_RAM_N);
   cursorYMasks=(uint8_t*)ps_malloc(2*CURSOR_MASK_N);
   if(!cursorYSnapshot||!cursorYMasks){cursorYFree();Serial.println("DOS YFIND allocation failed");return;}
   memset(cursorYMasks,0,2*CURSOR_MASK_N);
 }
 const uint32_t start=micros();
 memcpy(cursorYSnapshot,vm->memory.RAM+CURSOR_RAM_BASE,CURSOR_RAM_N);
 cursorYExpected=y;cursorYIndex=0;cursorYCounts[0]=cursorYCounts[1]=0;cursorYBusy=true;
 Serial.printf("DOS YFIND sample y=%u x=%u snapshot_us=%u\n",y,cursorWatchWord(cursorLogicalX),micros()-start);
}
static void cursorYSlice(){
 if(!cursorYBusy)return;
 const uint32_t end=min<uint32_t>(CURSOR_RAM_N-1,cursorYIndex+8192u);
 for(unsigned m=0;m<2;++m){
   uint8_t *mask=cursorYMasks+m*CURSOR_MASK_N;
   for(uint32_t i=cursorYIndex;i<end;++i){
     const uint8_t bit=uint8_t(1u<<(i&7));
     if(cursorYRounds&&!(mask[i>>3]&bit))continue;
     const unsigned val=m?unsigned(cursorYSnapshot[i]):unsigned(cursorYSnapshot[i])|(unsigned(cursorYSnapshot[i+1])<<8);
     if(val!=cursorYExpected){mask[i>>3]&=uint8_t(~bit);continue;}
     mask[i>>3]|=bit;
     if(cursorYCounts[m]<24)cursorYAddr[m][cursorYCounts[m]]=CURSOR_RAM_BASE+i;
     ++cursorYCounts[m];
   }
 }
 cursorYIndex=end;
 if(end!=CURSOR_RAM_N-1)return;
 ++cursorYRounds;cursorYLast=cursorYExpected;cursorYBusy=false;
 if(cursorYLast<200&&!cursorYSeen[cursorYLast]){cursorYSeen[cursorYLast]=true;++cursorYDistinct;}
 // Avoid flooding the terminal while hundreds of candidates remain.
 Serial.printf("DOS YFIND count rounds=%u y=%u Y16=%u Y8=%u\n",cursorYRounds,cursorYLast,cursorYCounts[0],cursorYCounts[1]);
 if(cursorYRounds>=3&&cursorYCounts[0]<=24&&cursorYCounts[1]<=24)cursorYList();
}
// The low byte suffices for 0..199, even if the candidate is a word.
// Never guess an adjacent address or write to multiple candidates at once.
static bool cursorYEligible(uint32_t a,unsigned y){
 if(a<CURSOR_RAM_BASE||a>=CURSOR_RAM_END-1)return false;
 if((a>=cursorRenderX&&a<cursorRenderY+2)||(a>=cursorLogicalX&&a<cursorLogicalX+2))return false;
 const uint32_t i=a-CURSOR_RAM_BASE;
 const uint8_t bit=uint8_t(1u<<(i&7));
 const bool word=(cursorYMasks[i>>3]&bit)&&cursorWatchWord(a)==y;
 const bool byte=(cursorYMasks[CURSOR_MASK_N+(i>>3)]&bit)&&vm->memory.readByte(a)==y;
 return word||byte;
}
static void cursorTestY(int y,uint32_t selected){
 if(y<0||y>199||!cursorProbeSafe()||!cursorYActive||cursorYBusy||!cursorYMasks||cursorYDistinct<3){
   Serial.println("DOS YTEST rejected: watch/yfind, three distinct Y samples, idle cursor, Y=0..199 required");return;
 }
 const unsigned current=cursorWatchWord(cursorRenderY)-9;
 if(unsigned(y)==current){Serial.println("DOS YTEST choose a different Y");return;}
 uint32_t a=selected;
 if(!a){
   // Use the small enumerated candidate lists only if complete.
   if(cursorYCounts[0]>24||cursorYCounts[1]>24){Serial.println("DOS YTEST too many candidates; continue yfind");return;}
   for(unsigned m=0;m<2;++m)for(unsigned j=0;j<cursorYCounts[m];++j){
     uint32_t v=cursorYAddr[m][j];
     if(!cursorYEligible(v,current))continue;
     if(a&&a!=v){Serial.println("DOS YTEST ambiguous; use ylist and continue yfind or specify candidate address");return;}
     a=v;
   }
 }
 if(!a||!cursorYEligible(a,current)){Serial.println("DOS YTEST candidate rejected: not in scan results or current Y mismatch");return;}
 cursorWatchLog("beforeY",0);
 cursorYProbeAddr=a;cursorYProbeTarget=unsigned(y);cursorYProbeX=cursorWatchWord(cursorLogicalX);
 vm->memory.writeByte(a,uint8_t(y));
 Serial.printf("DOS YTEST write addr=%05lx old=%u target=%u read=%u X=%u\n",(unsigned long)a,current,unsigned(y),unsigned(vm->memory.readByte(a)),cursorYProbeX);
 cursorYProbePending=true;cursorProbeStart=millis();cursorProbePhase=1;
}
static void cursorWatchTick(){
 if(!cursorWatch)return;
 if(!vm||activeGame!=1||launcher||bootPhase||loadingGame||benchUntil||launchPending){cursorWatch=false;cursorWatchKey=0;cursorProbePhase=0;cursorYFree();return;}
 if(cursorProbePhase){
   const uint32_t elapsed=millis()-cursorProbeStart;
   const unsigned due=cursorProbePhase==1?100:cursorProbePhase==2?500:1000;
   if(held){cursorProbePhase=0;cursorYProbePending=false;Serial.println("DOS RAM PROBE readback cancelled by input");}
   else if(elapsed>=due){
     cursorWatchLog(cursorProbePhase==1?"after100":cursorProbePhase==2?"after500":"after1000",0);
     if(cursorYProbePending){
       Serial.printf("DOS YTEST after%u addr=%05lx value=%u target=%u renderY=%u X=%u expectedX=%u\n",due,(unsigned long)cursorYProbeAddr,unsigned(vm->memory.readByte(cursorYProbeAddr)),cursorYProbeTarget,cursorWatchWord(cursorRenderY),cursorWatchWord(cursorLogicalX),cursorYProbeX);
     }
     if(++cursorProbePhase>3){cursorProbePhase=0;cursorYProbePending=false;}
   }
 }
 const uint8_t k=held;
 const bool arrow=k==77||k==75||k==80||k==72;
 if(cursorWatchKey&&(!arrow||k!=cursorWatchKey)){
   cursorWatchLog("release",cursorWatchKey);
   if(cursorYActive&&(cursorWatchKey==72||cursorWatchKey==80))cursorYDue=millis()+220;
   cursorWatchKey=0;
 }
 if(arrow&&k!=cursorWatchKey){cursorWatchKey=k;cursorWatchLog("hold",k);cursorWatchNext=millis()+150;}
 else if(arrow&&int32_t(millis()-cursorWatchNext)>=0){cursorWatchLog("live",k);cursorWatchNext=millis()+150;}
 if(cursorYActive){
   cursorYSlice();
   if(cursorYDue&&!held&&!cursorYBusy&&int32_t(millis()-cursorYDue)>=0){
     cursorYDue=0;cursorYSample();
   }
 }
}
static bool cursorAuto=false,cursorAutoFiltered=false;
static uint8_t cursorAutoArrow=0;
static uint32_t cursorAutoAt=0,cursorAutoNext=0;
static bool cursorFilterBusy=false;
static uint32_t cursorFilterIndex=0,cursorFilterCounts[4]={},cursorCaptureUs=0;
static uint8_t cursorFilterArrow=0;
static constexpr unsigned CURSOR_LIVE_N=3;
static uint32_t cursorLiveAddr[4][CURSOR_LIVE_N];
static unsigned cursorLiveCount[4]={};
static const char *cursorAxis(unsigned m){return m==0?"X16":m==1?"Y16":m==2?"X8":"Y8";}
static const char *cursorArrowName(uint8_t k){return k==77?"right":k==75?"left":k==80?"down":"up";}
static bool cursorIsArrow(uint8_t k){return k==77||k==75||k==80||k==72;}
static void cursorRamFree(){
 cursorYFree();
 free(cursorRamPrev);free(cursorRamNow);free(cursorMasks);
 cursorRamPrev=cursorRamNow=cursorMasks=nullptr;cursorRounds=0;cursorAuto=false;cursorAutoArrow=0;cursorWatch=false;cursorWatchKey=0;cursorProbePhase=0;cursorFilterBusy=false;cursorFilterIndex=0;memset(cursorLiveCount,0,sizeof(cursorLiveCount));
}
static bool cursorRamAlloc(){
 cursorRamPrev=(uint8_t*)ps_malloc(CURSOR_RAM_N);
 cursorRamNow=(uint8_t*)ps_malloc(CURSOR_RAM_N);
 cursorMasks=(uint8_t*)ps_malloc(CURSOR_MASK_N*4);
 if(!cursorRamPrev||!cursorRamNow||!cursorMasks){cursorRamFree();return false;}
 return true;
}
static void cursorRamCapture(uint8_t *dst){
 // Guest execution is on this same task. Conventional RAM is directly
 // addressable (same backing memory used by the existing fast CPU reads).
 const uint32_t t=micros();
 memcpy(dst,vm->memory.RAM+CURSOR_RAM_BASE,CURSOR_RAM_N);
 cursorCaptureUs=micros()-t;
}
// mode 0=x16,1=y16,2=x8,3=y8. Keep unaligned words as candidates.
static unsigned cursorValue(const uint8_t *p,uint32_t i,unsigned mode){
 return mode<2?unsigned(p[i])|(unsigned(p[i+1])<<8):unsigned(p[i]);
}
static bool cursorMatch(unsigned before,unsigned after,unsigned limit,int direction){
 if(before>limit||after>limit)return false;
 return direction>0?after>before:direction<0?after<before:after==before;
}
static void cursorReport(){
 if(!cursorMasks){Serial.println("DOS RAM no baseline");return;}
 for(unsigned m=0;m<4;++m){
   uint32_t count=0;cursorLiveCount[m]=0;
   uint8_t *mask=cursorMasks+m*CURSOR_MASK_N;
   for(uint32_t i=0;i<CURSOR_RAM_N-1;++i){
     if(mask[i>>3]&(1u<<(i&7))){
       ++count;
       if(cursorLiveCount[m]<CURSOR_LIVE_N)cursorLiveAddr[m][cursorLiveCount[m]++]=CURSOR_RAM_BASE+i;
       if(count<=10)Serial.printf("DOS RAM %s addr=%05lx value=%u\n",cursorAxis(m),(unsigned long)(CURSOR_RAM_BASE+i),cursorValue(cursorRamPrev,i,m));
     }
     if((i&8191)==8191)vTaskDelay(1);
   }
   Serial.printf("DOS RAM count %s=%lu rounds=%u\n",m==0?"X16":m==1?"Y16":m==2?"X8":"Y8",(unsigned long)count,cursorRounds);
 }
}
static void cursorFilter(int dx,int dy,const char *arg){
 cursorRamCapture(cursorRamNow);
 for(unsigned m=0;m<4;++m){
   uint8_t *mask=cursorMasks+m*CURSOR_MASK_N;
   const unsigned limit=m<2?1023:255;
   const int direction=(m&1)?dy:dx;
   for(uint32_t i=0;i<CURSOR_RAM_N-1;++i){
     const uint8_t bit=uint8_t(1u<<(i&7));
     if((mask[i>>3]&bit)&&!cursorMatch(cursorValue(cursorRamPrev,i,m),cursorValue(cursorRamNow,i,m),limit,direction))
       mask[i>>3]&=uint8_t(~bit);
     if((i&8191)==8191)vTaskDelay(1);
   }
 }
 uint8_t *tmp=cursorRamPrev;cursorRamPrev=cursorRamNow;cursorRamNow=tmp;
 ++cursorRounds;Serial.printf("DOS RAM filtered %s\n",arg);cursorReport();

}
static void cursorAutoBegin(uint8_t k){
 if(!cursorAuto||!cursorMasks||!cursorIsArrow(k)||activeGame!=1)return;
 // Never delay a physical key while a previous scan is being filtered.
 if(cursorFilterBusy){Serial.println("DOS RAM HOLD busy: arrow still forwarded; wait for count");return;}
 cursorRamCapture(cursorRamPrev);
 cursorAutoArrow=k;cursorAutoFiltered=false;cursorAutoAt=millis();cursorAutoNext=cursorAutoAt+350;
 Serial.printf("DOS RAM HOLD begin %s rounds=%u snapshot_us=%u\n",cursorArrowName(k),cursorRounds,cursorCaptureUs);
}
static void cursorAutoLive(){
 // Only preview the bounded list retained from the last differential scan.
 for(unsigned m=0;m<4;++m){
   for(unsigned j=0;j<cursorLiveCount[m];++j){
     uint32_t a=cursorLiveAddr[m][j];
     unsigned val=vm->memory.readByte(a);
     if(m<2)val|=unsigned(vm->memory.readByte(a+1))<<8;
     Serial.printf("DOS RAM LIVE %s %s addr=%05lx value=%u\n",cursorArrowName(cursorAutoArrow),cursorAxis(m),(unsigned long)a,val);
   }
 }
}
static void cursorAutoStartFilter(uint8_t k){
 cursorRamCapture(cursorRamNow);
 cursorFilterArrow=k;cursorFilterIndex=0;cursorFilterBusy=true;
 memset(cursorFilterCounts,0,sizeof(cursorFilterCounts));
 memset(cursorLiveCount,0,sizeof(cursorLiveCount));
 Serial.printf("DOS RAM HOLD snapshot %s duration_ms=%u snapshot_us=%u\n",cursorArrowName(k),millis()-cursorAutoAt,cursorCaptureUs);
}
static void cursorAutoFilterSlice(){
 // Bounded CPU work per loop. Snapshot buffers are immutable until complete.
 const uint32_t end=min(CURSOR_RAM_N-1,cursorFilterIndex+2048u);
 const uint8_t k=cursorFilterArrow;
 const int dx=k==77?1:k==75?-1:0,dy=k==80?1:k==72?-1:0;
 for(unsigned m=0;m<4;++m){
   uint8_t *mask=cursorMasks+m*CURSOR_MASK_N;
   const unsigned limit=m<2?1023:255;
   const int direction=(m&1)?dy:dx;
   for(uint32_t i=cursorFilterIndex;i<end;++i){
     const uint8_t bit=uint8_t(1u<<(i&7));
     if(!(mask[i>>3]&bit))continue;
     if(!cursorMatch(cursorValue(cursorRamPrev,i,m),cursorValue(cursorRamNow,i,m),limit,direction)){mask[i>>3]&=uint8_t(~bit);continue;}
     ++cursorFilterCounts[m];
     if(cursorLiveCount[m]<CURSOR_LIVE_N)cursorLiveAddr[m][cursorLiveCount[m]++]=CURSOR_RAM_BASE+i;
   }
 }
 cursorFilterIndex=end;
 if(end!=CURSOR_RAM_N-1)return;
 uint8_t *tmp=cursorRamPrev;cursorRamPrev=cursorRamNow;cursorRamNow=tmp;
 ++cursorRounds;cursorFilterBusy=false;
 Serial.printf("DOS RAM count X16=%u Y16=%u X8=%u Y8=%u rounds=%u dir=%s\n",cursorFilterCounts[0],cursorFilterCounts[1],cursorFilterCounts[2],cursorFilterCounts[3],cursorRounds,cursorArrowName(k));
 for(unsigned m=0;m<4;++m)for(unsigned n=0;n<cursorLiveCount[m];++n){
   uint32_t addr=cursorLiveAddr[m][n];
   Serial.printf("DOS RAM %s addr=%05lx value=%u\n",cursorAxis(m),(unsigned long)addr,cursorValue(cursorRamPrev,addr-CURSOR_RAM_BASE,m));
 }
}
static void cursorAutoTick(){
 cursorWatchTick();
 if(!cursorAuto||!cursorMasks)return;
 if(activeGame!=1||launcher||bootPhase||loadingGame||benchUntil){cursorAutoArrow=0;return;}
 if(cursorFilterBusy)cursorAutoFilterSlice();
 if(!cursorAutoArrow)return;
 const uint32_t now=millis();
 const bool released=held!=cursorAutoArrow;
 if(!cursorAutoFiltered&&!cursorFilterBusy&&(released||int32_t(now-cursorAutoNext)>=0)){
   // Ignore very short gestures, rather than filtering on a stationary cursor.
   if(now-cursorAutoAt>=150){cursorAutoStartFilter(cursorAutoArrow);cursorAutoFiltered=true;}
 }
 if(released){
   Serial.printf("DOS RAM HOLD end %s sampled=%u duration_ms=%u\n",cursorArrowName(cursorAutoArrow),(unsigned)cursorAutoFiltered,now-cursorAutoAt);
   cursorAutoArrow=0;return;
 }
 if(cursorAutoFiltered&&!cursorFilterBusy&&int32_t(now-cursorAutoNext)>=0){
   cursorAutoLive();cursorAutoNext=now+500;
 }
}
}
void dos_key_scan(const char *arg){keyScanCommand(arg);}
void dos_cursor_scan(const char *arg){
 if(!strcmp(arg,"stop")){cursorRamFree();cancelDirect();Serial.println("DOS RAM scan released; default RAM touch retained");return;}
 if(!strcmp(arg,"click")||!strcmp(arg,"click mouse")){
   if(!monkeyClickRequest(!strcmp(arg,"click mouse"),"serial"))Serial.println("DOS ACTION rejected: enable RAM touch in Monkey Island and wait for idle");
   return;
 }
 if(!strcmp(arg,"touch off")){Serial.println("DOS RAM TOUCH always enabled in Monkey Island");return;}
 if(!strcmp(arg,"touch status")){monkeyRamReport("status");return;}
 if(!strcmp(arg,"touch on")){
   if(!vm||activeGame!=1||launcher){Serial.println("DOS RAM TOUCH only available in Monkey Island");return;}
   if(monkeyRamTouch){monkeyRamReport("already_armed");return;}
   cursorRamFree();cancelDirect();dos_mouse(0,0,false);
   monkeyRamTouch=true;monkeyRamReport("armed");return;
 }
 if(!vm||activeGame!=1||launcher||bootPhase||loadingGame||benchUntil||launchPending){
   Serial.println("DOS RAM unavailable: enter Monkey Island and wait for game");return;
 }
 if(!strcmp(arg,"watch")){
   cursorRamFree();cursorWatch=true;cursorWatchKey=0;
   Serial.println("DOS RAM WATCH addresses XY=2e5f0,2e5f2 Xalt=30df4,33fb4 Yalt=2e5fb,2ed74");
   cursorWatchLog("baseline",0);return;
 }
 if(cursorWatch&&!strcmp(arg,"list")){cursorWatchLog("manual",0);return;}
 if(!strcmp(arg,"yfind")){
   if(!cursorProbeSafe()){Serial.println("DOS YFIND requires :cursor watch and stationary cursor");return;}
   if(!cursorYActive){cursorYActive=true;cursorYSample();Serial.println("DOS YFIND move up/down, release and wait for count");}
   else if(cursorYBusy)cursorYList();
   else cursorYSample();
   return;
 }
 if(!strcmp(arg,"ylist")){cursorYList();return;}
 if(!strncmp(arg,"testy ",6)){
   int y;unsigned long a=0;char tail;
   const int n=sscanf(arg+6,"%d %lx %c",&y,&a,&tail);
   if(n!=1&&n!=2){Serial.println("DOS YTEST use :cursor testy Y [hexaddr]");return;}
   // A second token must be a valid hex address, not silently ignored.
   if(n==1&&sscanf(arg+6,"%d %c",&y,&tail)!=1){Serial.println("DOS YTEST invalid address");return;}
   cursorTestY(y,uint32_t(a));return;
 }
 if(!strncmp(arg,"test ",5)){
   int x,y;char tail;
   if(sscanf(arg+5,"%d %d %c",&x,&y,&tail)!=2){Serial.println("DOS RAM PROBE use :cursor test X Y");return;}
   cursorProbe(x,y,false);return;
 }
 if(!strncmp(arg,"testx ",6)){
   int x;char tail;
   if(sscanf(arg+6,"%d %c",&x,&tail)!=1){Serial.println("DOS RAM PROBE use :cursor testx X");return;}
   cursorProbe(x,0,true);return;
 }
 if(!strcmp(arg,"base")||!strcmp(arg,"auto")){
   const bool automatic=!strcmp(arg,"auto");
   cursorRamFree();
   if(!cursorRamAlloc()){Serial.println("DOS RAM insufficient PSRAM");return;}
   if(!automatic)cursorRamCapture(cursorRamPrev);
   memset(cursorMasks,255,CURSOR_MASK_N*4);
   cursorRounds=0;cursorAuto=automatic;
   if(automatic)Serial.println("DOS RAM AUTO ready: hold PAD arrow 1-2 seconds, release before edge, wait for DOS RAM count; :cursor list / stop");
   else Serial.println("DOS RAM baseline ready; move cursor with PAD, then :cursor right/left/up/down");
   return;
 }
 if(!cursorMasks){Serial.println("DOS RAM use :cursor base first");return;}
 if(!strcmp(arg,"list")){if(cursorFilterBusy){Serial.printf("DOS RAM filtering %u/%u\n",cursorFilterIndex,CURSOR_RAM_N-1);return;}cursorReport();return;}
 if(cursorAuto){Serial.println("DOS RAM AUTO active; use PAD arrows or :cursor stop");return;}
 int dx=0,dy=0;
 if(!strcmp(arg,"right"))dx=1;else if(!strcmp(arg,"left"))dx=-1;
 else if(!strcmp(arg,"down"))dy=1;else if(!strcmp(arg,"up"))dy=-1;
 else{Serial.println("DOS RAM use auto/base/right/left/down/up/list/stop");return;}
 cursorFilter(dx,dy,arg);
}
bool dos_diag_bench(){
 if(!vm||launcher||bootPhase||loadingGame||benchUntil)return false;
 host.video.drain();benchStart=millis();benchUntil=benchStart+5000;benchInstructions=vm->cpu.totalexec;
 diag_logf(false,"DOS BENCH video disabled 5s\n");return true;
}
static void serialInput(){
 static uint8_t state=0;static int number=0;static uint32_t at=0,lastCR=0;
 if(vm->input.freeSlots()<8&&!diag_command_pending()&&Serial.peek()!=':')return;
 if(state&&millis()-at>80){if(state==1)serialTap(1);state=0;}
 for(int i=0;i<32&&Serial.available();i++){
  if((vm->input.freeSlots()<8||textCount>=512)&&!diag_command_pending()&&Serial.peek()!=':')break;
  uint8_t c=Serial.read();if(diag_consume(c))continue;++serialBytes;
  if(c==0x1d&&!bootPhase&&!loadingGame){dos_diag_bench();continue;}
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
// Geometry and touch hitboxes share one table, in physical pixels.
struct PanelKey {OverlayKey key;uint16_t x,y,w,h;};
#define PK(label,scan,x,y,w,h) {{label,scan},x,y,w,h}
static const PanelKey keyboard[]={
 PK("1",2,0,0,40,40),PK("2",3,40,0,40,40),PK("3",4,80,0,40,40),PK("4",5,120,0,40,40),PK("5",6,160,0,40,40),PK("6",7,200,0,40,40),PK("7",8,240,0,40,40),PK("8",9,280,0,40,40),PK("9",10,320,0,40,40),PK("0",11,360,0,40,40),PK("OCULTAR",243,400,0,80,40),
 PK("Q",16,0,40,40,40),PK("W",17,40,40,40,40),PK("E",18,80,40,40,40),PK("R",19,120,40,40,40),PK("T",20,160,40,40,40),PK("Y",21,200,40,40,40),PK("U",22,240,40,40,40),PK("I",23,280,40,40,40),PK("O",24,320,40,40,40),PK("P",25,360,40,40,40),
 PK("A",30,0,80,40,40),PK("S",31,40,80,40,40),PK("D",32,80,80,40,40),PK("F",33,120,80,40,40),PK("G",34,160,80,40,40),PK("H",35,200,80,40,40),PK("J",36,240,80,40,40),PK("K",37,280,80,40,40),PK("L",38,320,80,40,40),PK("MAY",240,360,80,40,40),
 PK("ENTER",28,400,40,80,80),
 PK("Z",44,0,120,40,40),PK("X",45,40,120,40,40),PK("C",46,80,120,40,40),PK("V",47,120,120,40,40),PK("B",48,160,120,40,40),PK("N",49,200,120,40,40),PK("M",50,240,120,40,40),PK("BORRAR",14,280,120,120,40),PK(",",51,400,120,40,40),PK(".",52,440,120,40,40),
 PK("ESPACIO",57,0,160,120,40),PK("ESC",1,120,160,80,40),PK("CTRL",241,200,160,60,40),PK("ALT",242,260,160,60,40),PK("-",12,320,160,40,40),PK("TAB",15,360,160,60,40),PK("PAD",244,420,160,60,40)
};
// Monkey Island: two rows, all 12 original verb shortcuts, ESC, MENU.
static const PanelKey monkeyQuick[]={
 PK("A",24,0,0,68,48),PK("C",46,68,0,69,48),PK("I",17,137,0,68,48),
 PK("R",25,205,0,69,48),PK("M",38,274,0,68,48),PK("H",20,342,0,69,48),PK("ESC",1,411,0,69,48),
 PK("U",22,0,48,68,48),PK("D",34,68,48,69,48),PK("E",31,137,48,68,48),
 PK("T",21,205,48,69,48),PK("EN",49,274,48,68,48),PK("AP",33,342,48,69,48),PK("MENU",251,411,48,69,48)
};
static const char *const monkeyQuickNames[]={"ABRIR","CERRAR","IR A","RECOGER","MIRAR","HABLAR","","USAR","DAR","EMPUJAR","TIRAR","ENCENDER","APAGAR",""};
static_assert(sizeof(monkeyQuickNames)/sizeof(monkeyQuickNames[0])==sizeof(monkeyQuick)/sizeof(monkeyQuick[0]),"quick label count");
static const PanelKey monkeyNumeric[]={
 PK("1",2,0,0,60,60),PK("2",3,60,0,60,60),PK("3",4,120,0,60,60),PK("4",5,180,0,60,60),PK("5",6,240,0,60,60),
 PK("6",7,0,60,60,60),PK("7",8,60,60,60,60),PK("8",9,120,60,60,60),PK("9",10,180,60,60,60),PK("0",11,240,60,60,60),
 PK("^",72,334,0,34,38),PK("<",75,300,38,34,44),PK(">",77,368,38,34,44),PK("v",80,334,82,34,38),
 PK("ENTER",28,402,0,78,40),PK("ESPACIO",57,402,40,78,40),PK("OCULTAR",243,402,80,78,40),
 PK("ESC",1,300,0,34,38),PK("TAB",15,368,82,34,38)
};
static const PanelKey pad[]={
 PK("7",8,8,3,36,36),PK("8",9,44,3,36,36),PK("9",10,80,3,36,36),
 PK("4",5,8,39,36,36),PK("5",6,44,39,36,36),PK("6",7,80,39,36,36),
 PK("1",2,8,75,36,36),PK("2",3,44,75,36,36),PK("3",4,80,75,36,36),PK("0",11,116,75,36,36),
 PK("^",72,208,3,48,48),PK("<",75,160,51,48,60),PK("v",80,208,51,48,60),PK(">",77,256,51,48,60),
 PK("ESC",1,320,3,64,36),PK("OCULTAR",243,384,3,88,36),PK("ENTER",28,320,39,72,72),PK("ESPACIO",57,400,39,72,72)
};
#undef PK
static const OverlayKey toolbar[5]={{"TEXTO",248},{"PAD",244},{"ESC",1},{"ENTER",28},{"MENU",251}};
static const OverlayKey gameMenu[4]={{"GRABAR",0},{"CARGAR",0},{"VOLVER",243},{"SALIR",252}};
static const OverlayKey exitMenu[2]={{"CANCELAR",251},{"CONFIRMAR SALIR",253}};
static const OverlayKey monkeyMenu[5]={{"PARTIDA",254},{"NUMERICO",244},{"TEXTO",248},{"VOLVER",243},{"SALIR",252}};
// F5 is the original game's shared save/load dialog. The disk remains read-only.
static const OverlayKey monkeyGameMenu[5]={{"GUARDAR",63},{"CARGAR",63},{"S",31},{"N",49},{"VOLVER",251}};

static bool panelRepaint=false;
static uint8_t mods=0;
// Startup-only UI. User confirms the displayed number; never patches game RAM.
enum StartupState : uint8_t { START_WAIT,START_SEARCH,START_READY };
static StartupState startupState=START_WAIT;
static uint16_t startupNumber=0,startupSample=0;
static uint32_t startupAt=0,startupNext=0,startupStable=0;
static char startupLabel[5]="----";
static void startupReset(bool active){
 startupActive=active;startupState=START_WAIT;
 startupNumber=startupSample=0;startupAt=startupNext=startupStable=0;
 memcpy(startupLabel,"----",5);
 startupBaseline=active&&vm?vm->memory.readWord(startupNumberAddress):0;
}
static bool startupValid(uint16_t n){return n>=1000&&n<=9999&&n!=startupBaseline;}
static bool startupAvailable(){
 return startupActive&&vm&&activeGame==1&&!launcher&&!bootPhase&&!loadingGame&&!launchPending&&!benchUntil;
}
static void startupReady(uint16_t n){
 startupNumber=n;snprintf(startupLabel,sizeof(startupLabel),"%04u",unsigned(n));
 startupState=START_READY;panelRepaint=true;
 Serial.printf("DOS STARTUP number=%u addr=349c4 candidate; confirm on screen\n",unsigned(n));
}
static void startupSearch(){
 if(!startupAvailable())return;
 const uint16_t n=vm->memory.readWord(startupNumberAddress);
 startupAt=millis();startupNext=startupAt+100;startupStable=startupAt;startupSample=n;
 startupNumber=0;memcpy(startupLabel,"----",5);
 Serial.println("DOS STARTUP search requested; no keyboard input");
 startupState=START_SEARCH;setPanel(9);
}
static void startupEnter(){
 if(!startupAvailable())return;
 if(monkeyClickPending||monkeyClickDown)return;
 cancelDirect();
 if(!monkeyClickRequest(false,"startup-enter"))return;
 startupState=START_WAIT;startupNumber=0;memcpy(startupLabel,"----",5);panelRepaint=true;
}
static void startupFinish(){
 startupActive=false;startupState=START_WAIT;
 keyScanFree();setPanel(0);
 Serial.println("DOS STARTUP manual ENTER; next_panel=0");
}
static void startupTick(){
 if(!startupActive||startupState!=START_SEARCH)return;
 if(!startupAvailable()){startupState=START_WAIT;panelRepaint=true;return;}
 const uint32_t now=millis();
 if(int32_t(now-startupNext)<0)return;
 startupNext=now+100;
 const uint16_t n=vm->memory.readWord(startupNumberAddress);
 if(n!=startupSample){startupSample=n;startupStable=now;}
 if(startupValid(n)&&now-startupStable>=300){startupReady(n);return;}
 if(now-startupAt>=3500){
   startupState=START_WAIT;panelRepaint=true;
   Serial.println("DOS STARTUP no stable reference; tap reference to retry");
 }
}
static void cell(const OverlayKey &k,int x,int y,int w,int h){
 bool active=(k.scan==240&&(mods&1))||(k.scan==241&&(mods&2))||(k.scan==242&&(mods&4));
 dosLcd->fillRoundRect(x+2,y+2,w-4,h-4,4,active?0x366e:0x2124);
 dosLcd->drawRoundRect(x+2,y+2,w-4,h-4,4,0x7bef);
 int size=strlen(k.label)*12<w-6?2:1;
 dosLcd->setTextSize(size);dosLcd->setTextColor(k.scan?0xffff:0x7bef);
 dosLcd->setCursor(x+(w-int(strlen(k.label))*6*size)/2,y+(h-8*size)/2);dosLcd->print(k.label);
}
static void quickCell(const PanelKey &k,const char *name,int top){
 if(!*name){cell(k.key,k.x,top+k.y,k.w,k.h);return;}
 const int x=k.x,y=top+k.y,w=k.w,h=k.h;
 dosLcd->fillRoundRect(x+2,y+2,w-4,h-4,4,0x2124);
 dosLcd->drawRoundRect(x+2,y+2,w-4,h-4,4,0x7bef);
 dosLcd->setTextColor(0xffff);dosLcd->setTextSize(2);
 dosLcd->setCursor(x+(w-int(strlen(k.key.label))*12)/2,y+9);dosLcd->print(k.key.label);
 dosLcd->setTextSize(1);dosLcd->setCursor(x+(w-int(strlen(name))*6)/2,y+31);dosLcd->print(name);
}
static void numericCell(const PanelKey &k,int top){
 if(k.key.scan<2||k.key.scan>11){cell(k.key,k.x,top+k.y,k.w,k.h);return;}
 const int x=k.x,y=top+k.y,w=k.w,h=k.h;
 dosLcd->fillRoundRect(x+2,y+2,w-4,h-4,4,0x2124);dosLcd->drawRoundRect(x+2,y+2,w-4,h-4,4,0x7bef);
 dosLcd->setTextColor(0xffff);dosLcd->setTextSize(4);
 dosLcd->setCursor(x+(w-24)/2,y+(h-32)/2);dosLcd->print(k.key.label);
}
static void drawPanel(bool full){
 const uint8_t m=__atomic_load_n(&panelMode,__ATOMIC_RELAXED);const int top=panelTop();
 if(m==3)return;
 if(m!=9||full)dosLcd->fillRect(0,top,480,480-top,0x0842);
 if(m==1||m==4){unsigned n=0;for(const auto &k:keyboard){cell(k.key,k.x,top+k.y,k.w,k.h);if(++n%10==0)vTaskDelay(1);}}
 else if(m==2){
   unsigned n=0;
   if(activeGame==1){for(const auto &k:monkeyNumeric){numericCell(k,top);if(++n%6==0)vTaskDelay(1);}}
   else{for(const auto &k:pad){cell(k.key,k.x,top+k.y,k.w,k.h);if(++n%6==0)vTaskDelay(1);}}
 }
 else if(activeGame==1&&m==8){
   cell({"ENTER",28},16,top,96,96);
   cell({"CLAVE",245},128,top,96,96);
   cell({"ESC",1},384,top,96,48);
   cell({"MENU",251},384,top+48,96,48);
 }
 else if(activeGame==1&&m==9){
   if(full){
     for(unsigned i=0;i<10;++i)numericCell(monkeyNumeric[i],top);
     cell({"ENTER",28},300,top+60,180,60);
   }
   // Reference only. Touching it retries the lookup, never types.
   dosLcd->fillRoundRect(302,top+2,176,56,4,0x2124);
   dosLcd->drawRoundRect(302,top+2,176,56,4,0x7bef);
   dosLcd->setTextColor(0x7bef);dosLcd->setTextSize(1);
   dosLcd->setCursor(333,top+9);dosLcd->print("CLAVE REFERENCIA");
   dosLcd->setTextSize(3);dosLcd->setTextColor(startupState==START_READY?0xffff:0x7bef);
   dosLcd->setCursor(354,top+25);dosLcd->print(startupLabel);
 }
 else if(activeGame==1&&m==0){unsigned i=0;for(const auto &k:monkeyQuick){quickCell(k,monkeyQuickNames[i++],top);}}
 else if(activeGame==1&&m==5){for(int c=0;c<5;c++){const OverlayKey k=startupActive&&c==0?OverlayKey{"JUGAR",238}:monkeyMenu[c];cell(k,c*96,top,96,48);}}
 else if(activeGame==1&&m==7){for(int c=0;c<5;c++)cell(monkeyGameMenu[c],c*96,top,96,48);}
 else if(m==5){for(int c=0;c<4;c++)cell(gameMenu[c],c*120,top,120,48);}
 else if(m==6){for(int c=0;c<2;c++)cell(exitMenu[c],c*240,top,240,48);}
 else for(int c=0;c<5;c++){OverlayKey k=toolbar[c];if(c==0&&activeGame==0)k.scan=250;cell(k,c*96,top,96,48);}
}
// Requests never block the producer behind an in-flight VGA frame.
static void setPanel(uint8_t mode){
 if(mode>9||!vm||(mode>=7&&activeGame!=1))return;
 pendingPanel=mode;
}
static void servicePanel(){
 if(pendingPanel==255&&!panelRepaint)return;
 if(launcher||bootPhase||loadingGame)return;
 const bool async=host.video.asyncReady();
 if(async&&!host.video.acquire())return;
 const bool change=pendingPanel!=255;
 const uint8_t mode=change?pendingPanel:__atomic_load_n(&panelMode,__ATOMIC_RELAXED);
 const int previousTop=panelTop();
 pendingPanel=255;panelRepaint=false;
 traceMain(20);dosTraceMode=mode;const uint32_t started=micros();
 if(change){
   diag_logf(false,"DOS PANEL begin mode=%u heap=%u stack=%u\n",mode,ESP.getFreeHeap(),(unsigned)uxTaskGetStackHighWaterMark(nullptr));
   __atomic_store_n(&panelMode,mode,__ATOMIC_RELAXED);mouseMode=mode!=2;mods=0;
   cancelDirect();dos_key(0);dos_mouse(0,0,false);
   traceMain(21);if(panelTop()!=previousTop){dosLcd->fillScreen(0);vTaskDelay(1);}
 }
 drawPanel(change);traceMain(22);dosLcd->flush(true);
 if(change&&panelTop()!=previousTop){host.video.invalidate();vm->video.updatedscreen=true;vm->video.prismaDrawDue=true;}
 if(async)host.video.release();
 if(change)diag_logf(false,"DOS PANEL end mode=%u us=%u stack=%u\n",mode,micros()-started,(unsigned)uxTaskGetStackHighWaterMark(nullptr));
 traceMain(0);
}
static uint8_t hitPanel(int x,int y){
 const uint8_t m=__atomic_load_n(&panelMode,__ATOMIC_RELAXED);const int top=panelTop();
 if(x<0||x>=480||y<top||y>=480)return 0;
 y-=top;
 if(m==1||m==4){for(const auto &k:keyboard)if(x>=k.x&&x<k.x+k.w&&y>=k.y&&y<k.y+k.h)return k.key.scan;}
 else if(m==2){
   if(activeGame==1){for(const auto &k:monkeyNumeric)if(x>=k.x&&x<k.x+k.w&&y>=k.y&&y<k.y+k.h)return k.key.scan;}
   else{for(const auto &k:pad)if(x>=k.x&&x<k.x+k.w&&y>=k.y&&y<k.y+k.h)return k.key.scan;}
 }
 else if(activeGame==1&&m==8){
   if(x>=16&&x<112)return 28;
   if(x>=128&&x<224)return 245;
   if(x>=384)return y<48?1:251;
   return 0;
 }
 else if(activeGame==1&&m==9){
   for(unsigned i=0;i<10;++i){const auto &k=monkeyNumeric[i];if(x>=k.x&&x<k.x+k.w&&y>=k.y&&y<k.y+k.h)return k.key.scan;}
   return x>=300?(y<60?245:28):0;
 }
 else if(activeGame==1&&m==0){for(const auto &k:monkeyQuick)if(x>=k.x&&x<k.x+k.w&&y>=k.y&&y<k.y+k.h)return k.key.scan;}
 else if(activeGame==1&&m==5)return startupActive&&x<96?238:monkeyMenu[x/96].scan;
 else if(activeGame==1&&m==7)return monkeyGameMenu[x/96].scan;
 else if(m==0){if(x<96&&activeGame==0)return 250;return toolbar[x/96].scan;}
 else if(m==5)return gameMenu[x/120].scan;
 else if(m==6)return exitMenu[x/240].scan;
 return 0;
}
static void overlayTap(uint8_t k){
 if(!k||vm->input.freeSlots()<8)return;
 if(activeGame==0||consoleMode){uint8_t c=0;const char *rows[]={"1234567890","qwertyuiop","asdfghjkl","zxcvbnm"};const unsigned starts[]={2,16,30,44};for(int i=0;i<4;i++)if(k>=starts[i]&&k<starts[i]+strlen(rows[i]))c=rows[i][k-starts[i]];if(c>='a'&&c<='z'&&(mods&1))c-=32;if(k==57)c=32;if(k==28)c=13;if(k==14)c=8;if(k==51)c=',';if(k==52)c='.';if(k==12)c='-';if(c&&enqueueText(c,k)){++overlayKeys;if(mods){mods=0;panelRepaint=true;}return;}}

 if(mods&1)vm->input.handleKeyDown(42);
 if(mods&2)vm->input.handleKeyDown(29);
 if(mods&4)vm->input.handleKeyDown(56);
 vm->input.handleKeyDown(k);vm->input.handleKeyUp(k);
 if(mods&4)vm->input.handleKeyUp(56);
 if(mods&2)vm->input.handleKeyUp(29);
 if(mods&1)vm->input.handleKeyUp(42);
 ++overlayKeys;
 if(mods){mods=0;panelRepaint=true;}
}
void dos_touch_game(unsigned n,int x,int y){
 static bool finger=false,moved=false,drag=false,panelTouch=false,rightDown=false;
 static int px=0,py=0,ox=0,oy=0;
 static uint8_t pending=0;
 static bool repeatStarted=false;
 static uint32_t repeatAt=0;
 static uint32_t started=0,releaseAt=0,rightRelease=0;
 if(releaseAt&&int32_t(millis()-releaseAt)>=0){dos_mouse(0,0,false);releaseAt=0;}
 if(rightRelease&&int32_t(millis()-rightRelease)>=0){vm->mouse.handleButtonUp(Faux86::SerialMouse::ButtonType::Right);rightRelease=0;rightDown=false;}
 if(launcher||bootPhase||loadingGame||pendingPanel!=255){monkeyTapArmed=false;finger=moved=drag=panelTouch=repeatStarted=false;pending=0;dos_key(0);return;}
 const bool absolute=activeGame>=0&&activeGame<2;
 const bool monkey=activeGame==1;
 if(n>1&&absolute){cancelDirect();dos_mouse(0,0,false);}
 if(n!=1){
   if(finger&&panelTouch&&n==0&&pending&&!repeatStarted){
     const uint8_t k=pending;
     if(k==240||k==241||k==242){mods^=1u<<(k-240);panelRepaint=true;}
     else if(k==250)setPanel(4);else if(k==243)setPanel(monkey&&startupActive?8:0);else if(k==244)setPanel(2);else if(k==248)setPanel(1);else if(k==249)setPanel(3);
     else if(k==251)setPanel(5);else if(k==252)setPanel(6);else if(k==254&&monkey)setPanel(7);
     else if(monkey&&startupActive&&k==28&&__atomic_load_n(&panelMode,__ATOMIC_RELAXED)==8)startupEnter();
     else if(monkey&&startupActive&&k==245)startupSearch();
     else if(monkey&&startupActive&&k==28&&__atomic_load_n(&panelMode,__ATOMIC_RELAXED)==9){dos_key(0);startupFinish();}
     else if(monkey&&startupActive&&k>=2&&k<=11&&__atomic_load_n(&panelMode,__ATOMIC_RELAXED)==9){/* key-up below */}
     else if(monkey&&startupActive&&k==238)startupFinish();
     else if(k==63&&monkey&&__atomic_load_n(&panelMode,__ATOMIC_RELAXED)==7){overlayTap(k);diag_logf(false,"DOS SAVE/LOAD native F5; disk read-only\n");}
     else if(k==253&&__atomic_load_n(&panelMode,__ATOMIC_RELAXED)==6){diag_logf(false,"DOS EXIT cause=confirmed_menu\n");dos_request_exit();}
     else if(k==246){dos_mouse(0,0,true);releaseAt=millis()+120;++touchClicks;}
     else if(k==247){if(!rightDown){vm->mouse.handleButtonDown(Faux86::SerialMouse::ButtonType::Right);rightDown=true;}rightRelease=millis()+120;++touchClicks;}
     else if(monkey&&monkeyRamTouch&&k==28){monkeyClickRequest(false,"panel");++overlayKeys;}
     else if(__atomic_load_n(&panelMode,__ATOMIC_RELAXED)!=2)overlayTap(k);
   }else if(finger&&!panelTouch&&!moved&&!drag&&n==0&&millis()-started<350){
     if(monkey){
       uint16_t nx,ny;const uint32_t now=millis();
       if(host.video.touchPosition(px,py,nx,ny)){
         if(monkeyDoubleTap(monkeyTapArmed,now,monkeyTapAt,px,py,monkeyTapX,monkeyTapY,monkeyRamTouch)){
           monkeyTapArmed=false;
           if(queueDirect(px,py,true))++monkeyDouble;
         }else{
           monkeyTapArmed=true;monkeyTapAt=now;monkeyTapX=px;monkeyTapY=py;++monkeySingle;
         }
       }else monkeyTapArmed=false;
     }else if(absolute){queueDirect(px,py,true);}
     else{dos_mouse(0,0,true);releaseAt=millis()+120;++touchClicks;}
   }
   if(monkey&&finger&&(panelTouch||moved||drag||n!=0||millis()-started>=350))monkeyTapArmed=false;
   if(drag)dos_mouse(0,0,false);
   finger=moved=drag=panelTouch=repeatStarted=false;pending=0;dos_key(0);return;
 }
 if(!finger){
  finger=true;px=ox=x;py=oy=y;started=millis();moved=drag=repeatStarted=false;
  pending=hitPanel(x,y);panelTouch=y>=panelTop();
  if(!panelTouch&&x<56&&y<56&&(!absolute||__atomic_load_n(&panelMode,__ATOMIC_RELAXED)==3)){panelTouch=true;pending=__atomic_load_n(&panelMode,__ATOMIC_RELAXED)==3?243:249;}
  if(panelTouch){monkeyTapArmed=false;const uint8_t m=__atomic_load_n(&panelMode,__ATOMIC_RELAXED);if(m==9&&((pending>=2&&pending<=11)||pending==28)){dos_key(pending);++overlayKeys;}else if(pending&&pending<240&&m==2&&!(monkey&&monkeyRamTouch&&pending==28)){if(monkey)cursorAutoBegin(pending);dos_key(pending);++overlayKeys;}return;}
  if(absolute)queueDirect(x,y,false);
 }
 if(panelTouch){
   const uint8_t currentMode=__atomic_load_n(&panelMode,__ATOMIC_RELAXED);
   if((currentMode==0||currentMode==5||currentMode==6||currentMode==7||currentMode==8||currentMode==9)&&(hitPanel(x,y)!=pending)){if(repeatStarted||currentMode==9)dos_key(0);pending=0;return;}
   // Carmen only: keep the verified short tap path. After a deliberate
   // hold, send physical make/break and bounded typematic make codes.
   if(activeGame==0&&(pending==28||pending==57)&&
      (__atomic_load_n(&panelMode,__ATOMIC_RELAXED)==0||__atomic_load_n(&panelMode,__ATOMIC_RELAXED)==4)&&!mods){
     if(y<panelTop()||hitPanel(x,y)!=pending){
       if(repeatStarted)dos_key(0);
       pending=0;return;
     }
     uint32_t now=millis();
     if(!repeatStarted&&now-started>=350&&vm->input.freeSlots()>=8){
       repeatStarted=true;dos_key(pending);repeatAt=now+125;++overlayKeys;
     }else if(repeatStarted&&int32_t(now-repeatAt)>=0){
       repeatAt=now+125;
       // Do not accumulate repeating keys if the guest is not reading.
       auto &m=vm->memory;
       if(vm->input.freeSlots()==256&&m.readWord(0x41a)==m.readWord(0x41c)){
         vm->input.handleKeyDown(pending);++overlayKeys;
       }
     }
   }
   return;
 }
 dos_key(0);
 if(abs(x-ox)+abs(y-oy)>9)moved=true;
 if(monkey){
   if(moved)monkeyTapArmed=false;
   // Absolute cursor tracking only; no held mouse button on a slide or long press.
   if(abs(x-px)+abs(y-py)>=3 && queueDirect(x,y,false)){px=x;py=y;}
   return;
 }
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
 dosTraceMain=dosTraceWorker=dosTraceMode=0;dosTraceMagic=DOS_TRACE_MAGIC;traceMain(10);
 showLoading(-1);
 Serial.printf("DOS ENTRY heap=%u psram=%u\n",ESP.getFreeHeap(),ESP.getFreePsram());
 Serial.println("BUILD 063 DOS: draw cadence 33ms / 30Hz target");
  const auto *part=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_ANY,"dosdata");
  DiskHeader h{};
  if(!part||esp_partition_read(part,0,&h,sizeof(h))!=ESP_OK||memcmp(h.magic,"PRDOS032",8)){fail("Missing DOS data");return false;}
  cfg.diskDriveA=new FlashDisk(part,h.floppyOffset,h.floppySize);
  if(!cfg.diskDriveA->isValid()){fail("Invalid boot disk");return false;}
  if(h.hardSize){cfg.diskDriveC=new FlashDisk(part,h.hardOffset,h.hardSize);if(!cfg.diskDriveC->isValid()){fail("Invalid game disk");return false;}}
  cfg.singleThreaded=true;cfg.slowSystem=false;cfg.cpuSpeed=240; cfg.cpuTiming=127;
  cfg.frameDelay=33;cfg.framebuffer.width=VGA_FRAMEBUFFER_WIDTH;cfg.framebuffer.height=VGA_FRAMEBUFFER_HEIGHT;
  cfg.enableAudio=false;cfg.useDisneySoundSource=false;cfg.useSoundBlaster=false;cfg.useAdlib=false;cfg.usePCSpeaker=false;
  cfg.biosFile=new FlashROM(pcxtbios_bin,pcxtbios_bin_len);
  cfg.romBasicFile=new FlashROM(rombasic_bin,rombasic_bin_len);
  cfg.videoRomFile=new FlashROM(videorom_bin,videorom_bin_len);
  cfg.asciiFile=new FlashROM(asciivga_dat,asciivga_dat_len);
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
  if(!launcher){serialInput();pumpText();}else diag_poll();
  if(directAt&&vm->cpu.prismaMouseApplied!=directBefore){
    if(activeGame==1)monkeyPositionMaxUs=max(monkeyPositionMaxUs,uint32_t(millis()-directAt)*1000u);
    directAt=0;++directMoves;
    if(directFlightClick){dos_mouse(0,0,true);directRelease=millis()+150;++touchClicks;if(activeGame==1)++monkeyClicksApplied;}
    directFlightClick=false;
  }else if(directAt&&millis()-directAt>800&&!vm->cpu.prismaMouseInCall){
    directAt=0;directFlightClick=false;vm->cpu.prismaMousePending=false;++directTimeouts;
  }
  if(directRelease&&int32_t(millis()-directRelease)>=0){directRelease=0;dos_mouse(0,0,false);}
  monkeyClickTick();
  if(monkeyRamTouch)monkeyRamTick();
  if(!monkeyRamTouch&&directQueued&&!directAt&&!directRelease&&!vm->cpu.prismaMouseInCall&&!launcher&&!loadingGame){
    vm->cpu.prismaMouseX=directX;vm->cpu.prismaMouseY=directY;directBefore=vm->cpu.prismaMouseApplied;
    directFlightClick=directClick;directQueued=directClick=false;vm->cpu.prismaMousePending=true;directAt=millis();
  }
  if(startupActive)startupTick();
  servicePanel();
  static uint32_t maxSlice=0,maxCapture=0,captureTotalUs=0,captureN=0;
  static uint64_t simulateTotalUs=0;
  if(!launcher){traceMain(30);uint32_t t=micros();vm->simulate();traceMain(0);uint32_t elapsed=micros()-t;if(elapsed>maxSlice)maxSlice=elapsed;simulateTotalUs+=elapsed;++steps;
    if(cursorWatch||cursorAuto)cursorAutoTick();
    if(keyScanBase)keyScanTick();
    if(benchUntil&&int32_t(millis()-benchUntil)>=0){diag_logf(false, "DOS BENCH ips=%.0f elapsed_ms=%u\n",(vm->cpu.totalexec-benchInstructions)*1000.0/(millis()-benchStart),millis()-benchStart);benchUntil=0;host.video.invalidate();vm->video.updatedscreen=true;vm->video.prismaDrawDue=true;}
    if(benchUntil||__atomic_load_n(&hideGuest,__ATOMIC_RELAXED)){vm->video.prismaDrawDue=false;}
    else if(vm->video.prismaDrawDue&&pendingPanel==255&&!panelRepaint){
      if(!vm->video.updatedscreen)vm->video.prismaDrawDue=false;
      else if(!host.video.asyncReady()){vm->video.prismaDrawDue=false;vm->video.vga_drawCallback(0);}
      else if(host.video.acquire()){
        vm->video.prismaDrawDue=false;uint32_t rt=micros();
        const bool captured=vm->video.prismaCapture();captureTotalUs+=micros()-rt;++captureN;
        if(captured){captureBytes+=vm->video.prismaCaptureBytes();host.video.submit();}
        else{vm->video.vga_drawCallback(0);host.video.release();}
        uint32_t us=micros()-rt;if(us>maxCapture)maxCapture=us;
      }else ++host.video.dropped;
    }
  }
  static uint32_t gamePromptSince=0;
  if(millis()-promptCheck>=100){
   promptCheck=millis();bool prompt=atPrompt();
   if(bootPhase||loadingGame)logGuestText();
   if(bootPhase&&vm->cpu.didbootstrap&&loadStage<75)loadProgress(75,"MS-DOS iniciando");
   if(bootPhase&&prompt)loadProgress(100,"DOS listo");
   if(loadingGame){
     if(!prompt&&loadStage<65)loadProgress(65,"Ejecutando juego");
     unsigned mode=vm->memory.readByte(0x449)&0x7f;
     if((!prompt&&mode>3&&mode!=7)||millis()-launchTime>8000){
       loadingGame=false;host.video.invalidate();vm->video.updatedscreen=true;vm->video.prismaDrawDue=true;__atomic_store_n(&hideGuest,false,__ATOMIC_RELAXED);setPanel(__atomic_load_n(&panelMode,__ATOMIC_RELAXED));diag_logf(false, "DOS GAME DISPLAY mode=%u elapsed=%u\n",mode,millis()-launchTime);
     }
   }
   if(launchPending){if(!prompt)promptGone=true;if(promptGone||millis()-launchTime>15000)launchPending=false;}
   if(prompt&&!launchPending&&!launcher&&!consoleMode&&millis()-launchTime>1000){
     if(activeGame>=0){
       // Confirm a persistent, empty DOS prompt, not a transient text-mode
       // transition while the game is initializing or switching modes.
       const bool empty=vm->memory.readWord(0x41a)==vm->memory.readWord(0x41c);
       if(!empty||loadingGame){gamePromptSince=0;}
       else if(!gamePromptSince)gamePromptSince=millis();
       else if(millis()-gamePromptSince>=1200){
         diag_logf(false,"DOS EXIT cause=stable_prompt game=%d elapsed=%u\n",activeGame,millis()-launchTime);
         dos_restart_to_selector();return;
       }
     }else{gamePromptSince=0;openLauncher();}
   }else gamePromptSince=0;
  }
  if(millis()-last>=3000){
    uint32_t elapsed=millis()-last;
    uint64_t nowInstructions=vm->cpu.totalexec;
    uint32_t shown=__atomic_exchange_n(&host.video.frames,0,__ATOMIC_RELAXED),drawUs=__atomic_exchange_n(&host.video.work,0,__ATOMIC_RELAXED);
    uint32_t skipped=__atomic_exchange_n(&host.video.skipped,0,__ATOMIC_RELAXED);
    uint32_t renderMax=__atomic_exchange_n(&host.video.renderMax,0,__ATOMIC_RELAXED);
    diag_logf(true, "DOS PERF steps=%u ips=%.0f display=%.1f fps blit=%u us skip=%u heap=%u psram=%u\n",steps,(nowInstructions-lastInstructions)*1000.0/elapsed,shown*1000.0/elapsed,shown?drawUs/shown:0,skipped,ESP.getFreeHeap(),ESP.getFreePsram());
    const uint32_t gfxBusy=host.video.dropped;
    diag_logf(true, "DOS SLICE max_us=%u capture_max_us=%u render_core0_max_us=%u busy=%u launcher=%d\n",maxSlice,maxCapture,renderMax,host.video.dropped,(int)launcher);maxSlice=maxCapture=host.video.dropped=0;
    diag_logf(true,"DOS INPUT serial_keys=%u guest_reads=%u touch_clicks=%u queue_free=%d mode=%s\n",serialKeys,vm->input.prismaReadCount,touchClicks,vm->input.freeSlots(),mouseMode?"mouse":"keys");
    diag_logf(true,"DOS TEXT INPUT bytes=%u queued=%u delivered=%u pending=%u scan_sent=%u guest_reads=%u\n",serialBytes,textAccepted,textDelivered,textCount,vm->input.prismaSentCount,vm->input.prismaReadCount);
    diag_logf(true,"DOS MOUSE queries=%u positioned=%u in_call=%u touch_moves=%u timeouts=%u\n",vm->cpu.prismaMouseQueries,vm->cpu.prismaMouseApplied,(unsigned)vm->cpu.prismaMouseInCall,directMoves,directTimeouts);
    if(activeGame==1){diag_logf(true,"DOS MOUSE TAP singles=%u double_req=%u clicks=%u pos_max_us=%u\n",monkeySingle,monkeyDouble,monkeyClicksApplied,monkeyPositionMaxUs);monkeyPositionMaxUs=0;
     if(monkeyRamTouch)diag_logf(true,"DOS RAM TOUCH writes=%u rejected=%u XY=%u,%u\n",monkeyRamWrites,monkeyRamRejects,cursorWatchWord(cursorLogicalX),cursorWatchWord(cursorLogicalY));}
    diag_logf(true,"DOS OVERLAY keys=%u mode=%u\n",overlayKeys,__atomic_load_n(&panelMode,__ATOMIC_RELAXED));
    diag_logf(true, "DOS STACK main=%u lcd=%u internal_largest=%u\n",(unsigned)uxTaskGetStackHighWaterMark(nullptr),host.video.stackFree(),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    diag_logf(true,"DOS VGA capture_bytes=%u\n",captureBytes);captureBytes=0;
    uint32_t rasterRows=0,rasterPossible=0;vm->video.prismaRasterStats(rasterRows,rasterPossible);
    diag_logf(true,"DOS RASTER rows=%u possible=%u\n",rasterRows,rasterPossible);
    uint32_t scaleTime=__atomic_exchange_n(&host.video.scaleUs,0,__ATOMIC_RELAXED),scaleN=__atomic_exchange_n(&host.video.scaleCalls,0,__ATOMIC_RELAXED);
    uint32_t sr=__atomic_exchange_n(&host.video.scaleRows,0,__ATOMIC_RELAXED),sp=__atomic_exchange_n(&host.video.scalePossible,0,__ATOMIC_RELAXED);
    const uint32_t reused=__atomic_exchange_n(&host.video.scaleReuse,0,__ATOMIC_RELAXED);
    const uint32_t fused=__atomic_exchange_n(&host.video.fusedFrames,0,__ATOMIC_RELAXED),nativeRows=__atomic_exchange_n(&host.video.fusedRows,0,__ATOMIC_RELAXED);
    diag_logf(true, "DOS SCALE avg_us=%u calls=%u rows=%u possible=%u\n",scaleN?scaleTime/scaleN:0,scaleN,sr,sp);
    // Interval counters are sampled independently, as with DOS SCALE.
    const uint32_t jobs=__atomic_exchange_n(&host.video.profileJobs,0,__ATOMIC_RELAXED);
    const uint32_t workerUs=__atomic_exchange_n(&host.video.profileWorkerUs,0,__ATOMIC_RELAXED);
    const uint32_t rasterUs=__atomic_exchange_n(&host.video.profileRasterUs,0,__ATOMIC_RELAXED);
    const uint32_t presents=__atomic_exchange_n(&host.video.profilePresentN,0,__ATOMIC_RELAXED);
    const uint32_t clearUs=__atomic_exchange_n(&host.video.profileClearUs,0,__ATOMIC_RELAXED);
    const uint32_t copyUs=__atomic_exchange_n(&host.video.profileCopyUs,0,__ATOMIC_RELAXED);
    const uint32_t flushUs=__atomic_exchange_n(&host.video.profileFlushUs,0,__ATOMIC_RELAXED);
    const uint32_t pixels=__atomic_exchange_n(&host.video.profilePixels,0,__ATOMIC_RELAXED);
    const uint32_t spanPixels=__atomic_exchange_n(&host.video.profileSpanPixels,0,__ATOMIC_RELAXED);
    const uint32_t runs=__atomic_exchange_n(&host.video.profileRuns,0,__ATOMIC_RELAXED);
    diag_logf(true,"DOS GFX jobs=%u capture_n=%u capture_avg=%u worker_avg=%u raster_est=%u\n",jobs,captureN,captureN?captureTotalUs/captureN:0,jobs?workerUs/jobs:0,jobs?rasterUs/jobs:0);
    diag_logf(true,"DOS GFX LCD n=%u clear_avg=%u copy_avg=%u flush_avg=%u pixels_avg=%u\n",presents,presents?clearUs/presents:0,presents?copyUs/presents:0,presents?flushUs/presents:0,presents?pixels/presents:0);
    // Same interval and existing stage counters; no additional render or scan.
    // Durations are independently sampled core-local work, not additive latency.
    diag_logf(true,"DOS GFX BASE ms=%u game=%d panel=%u fps=%.1f ips=%.0f frames=%u jobs=%u busy=%u\n",elapsed,activeGame,unsigned(__atomic_load_n(&panelMode,__ATOMIC_RELAXED)),shown*1000.0/elapsed,(nowInstructions-lastInstructions)*1000.0/elapsed,shown,jobs,gfxBusy);
    diag_logf(true,"DOS GFX TIME sim_us=%lu cap_us=%u worker_us=%u raster_us=%u scale_us=%u copy_us=%u flush_us=%u clear_us=%u\n",(unsigned long)simulateTotalUs,captureTotalUs,workerUs,rasterUs,scaleTime,copyUs,flushUs,clearUs);
    diag_logf(true,"DOS GFX LOAD raster=%u/%u scale=%u/%u lcd_px=%u presents=%u capture_n=%u\n",rasterRows,rasterPossible,sr,sp,pixels,presents,captureN);
    diag_photo_line(0,"063 GFX t=%us game=%d p=%u",millis()/1000,activeGame,unsigned(__atomic_load_n(&panelMode,__ATOMIC_RELAXED)));
    diag_photo_line(1,"FPS %.1f IPS %.0f dt=%ums",shown*1000.0/elapsed,(nowInstructions-lastInstructions)*1000.0/elapsed,elapsed);
    diag_photo_line(2,"us CAP %u RAS %u SCL %u",captureN?captureTotalUs/captureN:0,jobs?rasterUs/jobs:0,scaleN?scaleTime/scaleN:0);
    diag_photo_line(3,"us COPY %u FLUSH %u WORK %u",presents?copyUs/presents:0,presents?flushUs/presents:0,jobs?workerUs/jobs:0);
    diag_photo_line(4,"frames %u jobs %u busy %u",shown,jobs,gfxBusy);
    diag_photo_line(5,"FUSED %u rows %u reuse %u",fused,nativeRows,reused);
    diag_logf(true,"DOS GFX FUSED jobs=%u native_rows=%u px=%u/%u\n",fused,nativeRows,pixels,spanPixels);
    diag_logf(true,"DOS GFX REUSE rows=%u processed=%u\n",reused,sr);
    diag_logf(true,"DOS GFX SPAN pixels=%u baseline=%u runs=%u\n",pixels,spanPixels,runs);
    diag_photo_line(6,"063 INPUT t=%us game=%d p=%u",millis()/1000,activeGame,unsigned(__atomic_load_n(&panelMode,__ATOMIC_RELAXED)));
    diag_photo_line(7,"key sent %u read %u free %d",vm->input.prismaSentCount,vm->input.prismaReadCount,vm->input.freeSlots());
    diag_photo_line(8,"tap %u double %u click %u",monkeySingle,monkeyDouble,monkeyClicksApplied);
    diag_photo_line(9,"move %u timeout %u reject %u",directMoves,directTimeouts,monkeyRamRejects);
    diag_photo_line(10,"063 MEM t=%us",millis()/1000);
    diag_photo_line(11,"heap %u psram %u",ESP.getFreeHeap(),ESP.getFreePsram());
    diag_photo_line(12,"stack main %u lcd %u",(unsigned)uxTaskGetStackHighWaterMark(nullptr),host.video.stackFree());
    diag_photo_line(13,"internal largest %u",(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    simulateTotalUs=0;
    captureTotalUs=captureN=0;
    diag_logf(true,"DOS DISK reads=%u hits=%u read_us=%u mmap_reads=%u mmap_bytes=%u\n",diskReads,diskHits,diskReadUs,diskMappedReads,diskMappedBytes);diskReads=diskHits=diskReadUs=diskMappedReads=diskMappedBytes=0;
    lastInstructions=nowInstructions;
    steps=0;last=millis();
  }
  static uint32_t lastYield=0;
  if(launcher||millis()-lastYield>=8){lastYield=millis();delay(1);}
}
