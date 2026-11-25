#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_partition.h>
#include <Wire.h>
#include <Adafruit_Faux86.h>
#include <esp_timer.h>

static Arduino_RGB_Display *dosLcd=nullptr;
uint16_t *vga_framebuffer=nullptr;
static bool launcher=false;
static bool bootPhase=true;
static const char *gameNames[]={"Carmen Sandiego","The Incredible Machine","Indiana Jones - Last Crusade","Indiana Jones - Temple of Doom"};
static const char *gameCmds[]={"carmen","tim","indy3","indy2"};

class PrismaVideo final : public Faux86::FrameBufferInterface {
  Faux86::RenderSurface surface{};
  uint16_t row[480], xmap[480];
  uint16_t *cache=nullptr;
  int oldW=0,oldH=0;
  bool invalid=true;
public:
  uint32_t frames=0, work=0, skipped=0;
  void invalidate(){invalid=true;}
  void allocate(){cache=(uint16_t*)ps_malloc(480*480*2);}

  Faux86::RenderSurface *getSurface() override {return &surface;}
  void setPalette(Faux86::Palette*) override {}
  void blit(uint16_t *pixels,int w,int h,int stride) override {
    if(!pixels || w<=0 || h<=0 || w>VGA_FRAMEBUFFER_WIDTH || h>VGA_FRAMEBUFFER_HEIGHT || stride<w*2) return;
    if(launcher){++skipped;return;}
    uint32_t t=micros();
    int dw=min(w,480),dh=min(h,480);
    if(w!=oldW||h!=oldH){invalid=true;oldW=w;oldH=h;for(int x=0;x<dw;x++)xmap[x]=x*w/dw;}
    bool changed=invalid;
    if(invalid)dosLcd->fillScreen(0);
    for(int y=0;y<dh;y++){
      const uint16_t *src=(const uint16_t*)((const uint8_t*)pixels+(y*h/dh)*stride);
      if(dw==w)memcpy(row,src,dw*2);else for(int x=0;x<dw;x++)row[x]=src[xmap[x]];
      uint16_t *prev=cache?cache+y*480:nullptr;
      if(invalid||!prev||memcmp(prev,row,dw*2)){
        if(prev)memcpy(prev,row,dw*2);
        dosLcd->draw16bitRGBBitmap((480-dw)/2,(480-dh)/2+y,row,dw,1);changed=true;
      }
    }
    invalid=false;
    if(changed){dosLcd->flush(true);++frames;work+=micros()-t;}else ++skipped;
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
public:
 FlashDisk(const esp_partition_t *p,uint32_t b,uint32_t n):part(p),base(b),len(n){}
 bool isValid() override {return part && len && base<=part->size && len<=part->size-base;}
 uint64_t getSize() override {return len;}
 uint64_t seek(uint64_t offset) override {pos=offset>len?len:offset;return pos;}
 int read(uint8_t *buf,unsigned count) override {
   if(!isValid())return 0; count=static_cast<unsigned>(min<uint32_t>(static_cast<uint32_t>(count),len-pos));
   if(esp_partition_read(part,base+pos,buf,count)!=ESP_OK)return 0;pos+=count;return count;
 }
 int write(const uint8_t *,unsigned) override {return 0;}
};
struct DiskHeader {char magic[8];uint32_t floppyOffset,floppySize,hardOffset,hardSize;};
static PrismaHost host;
static Faux86::Config cfg(&host);
static Faux86::VM *vm=nullptr;
static uint32_t last=0,steps=0;
static uint64_t lastInstructions=0;
static uint32_t promptCheck=0, launchTime=0;
static bool launchPending=false, promptGone=false;
static uint8_t held=0;
void dos_key(uint8_t key){
 if(!vm||key==held)return;
 if(held)vm->input.handleKeyUp(held);
 if(key)vm->input.handleKeyDown(key);
 held=key;vm->input.tick();
}
void dos_mouse(int dx,int dy,bool pressed){
 if(!vm)return;
 static bool down=false;
 if(dx||dy)vm->mouse.handleMove(dx,dy);
 if(pressed!=down){if(pressed)vm->mouse.handleButtonDown(Faux86::SerialMouse::ButtonType::Left);else vm->mouse.handleButtonUp(Faux86::SerialMouse::ButtonType::Left);down=pressed;}
}
static uint8_t asciiScan(char c){
 static const char *rows[]={"1234567890","qwertyuiop","asdfghjkl","zxcvbnm"};
 static const uint8_t starts[]={2,16,30,44};
 if(c=='\r')return 0x1c;
 for(int i=0;i<4;i++){const char *p=strchr(rows[i],c);if(p&&*p)return starts[i]+(p-rows[i]);}
 return 0;
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
static void openLauncher(){
 bootPhase=false;
 dos_key(0);dos_mouse(0,0,false);
 launcher=true;host.timer.pause();
 dosLcd->fillScreen(0x0842);dosLcd->setTextColor(0xffff);dosLcd->setTextSize(3);
 dosLcd->setCursor(16,18);dosLcd->print("PRISMA DOS");
 dosLcd->setTextSize(1);dosLcd->setCursor(16,56);dosLcd->print("Elegi un juego / 2 dedos para HOME");
 for(int i=0;i<4;i++){
  int y=78+i*88;dosLcd->fillRoundRect(12,y,456,74,8,0x2124);
  dosLcd->drawRoundRect(12,y,456,74,8,0x5e9e);
  dosLcd->setTextSize(2);dosLcd->setCursor(24,y+28);dosLcd->print(gameNames[i]);
 }
 dosLcd->flush(true);Serial.println("DOS TOUCH LAUNCHER");
}
bool dos_launcher_touch(unsigned n,int x,int y){
 static bool down=false;static int choice=-1;
 if(!launcher){down=false;choice=-1;return false;}
 if(n==1&&!down){down=true;choice=(x>=12&&x<468&&y>=78&&y<430&&(y-78)%88<74)?(y-78)/88:-1;}
 if(n>1){down=true;choice=-1;}
 if(!n&&down){
  down=false;
  if(choice>=0&&choice<4){
   const char *cmd=gameCmds[choice];Serial.printf("DOS LAUNCH %s\n",cmd);
   if(!injectCommand(cmd)){Serial.println("DOS command rejected: not at empty prompt");return true;}
   host.timer.resume();vm->video.updatedscreen=true;launcher=false;launchPending=true;promptGone=false;launchTime=millis();host.video.invalidate();
  }
  choice=-1;
 }
 return true;
}
void dos_show_launcher(){if(vm&&!launcher&&!launchPending&&atPrompt())openLauncher();}
static void serialInput(){
 // ASCII terminal commands, including game launcher batch names.
 static const char *rows[]={"1234567890","qwertyuiop","asdfghjkl","zxcvbnm"};
 static const uint8_t starts[]={2,16,30,44};
 if(!Serial.available())return;
 char c=Serial.read();if(c=='\n')return;
 uint8_t k=c=='\r'?0x1c:c==' '?0x39:c==27?1:c==8?14:0;
 for(int i=0;i<4&&!k;i++){const char *p=strchr(rows[i],tolower((unsigned char)c));if(p&&*p)k=starts[i]+(p-rows[i]);}
 if(k){vm->input.handleKeyDown(k);vm->input.tick();vm->input.handleKeyUp(k);vm->input.tick();}
}
static void fail(const char *msg){Serial.println(msg);dosLcd->setCursor(8,8);dosLcd->setTextColor(0xffff);dosLcd->setTextSize(2);dosLcd->println(msg);dosLcd->flush(true);}
bool dos_start(Arduino_RGB_Display &display){
 dosLcd=&display;
 dosLcd->fillScreen(0);dosLcd->flush(true);
 Serial.printf("DOS ENTRY heap=%u psram=%u\n",ESP.getFreeHeap(),ESP.getFreePsram());
 Serial.println("BUILD 035 DOS");
  const auto *part=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_ANY,"dosdata");
  DiskHeader h{};
  if(!part||esp_partition_read(part,0,&h,sizeof(h))!=ESP_OK||memcmp(h.magic,"PRDOS032",8)){fail("Missing DOS data");return false;}
  cfg.diskDriveA=new FlashDisk(part,h.floppyOffset,h.floppySize);
  if(!cfg.diskDriveA->isValid()){fail("Invalid boot disk");return false;}
  if(h.hardSize){cfg.diskDriveC=new FlashDisk(part,h.hardOffset,h.hardSize);if(!cfg.diskDriveC->isValid()){fail("Invalid game disk");return false;}}
  cfg.singleThreaded=true;cfg.slowSystem=false;cfg.cpuSpeed=120; cfg.cpuTiming=63;
  cfg.frameDelay=100;cfg.framebuffer.width=VGA_FRAMEBUFFER_WIDTH;cfg.framebuffer.height=VGA_FRAMEBUFFER_HEIGHT;
  cfg.enableAudio=false;cfg.useDisneySoundSource=false;cfg.useSoundBlaster=false;cfg.useAdlib=false;cfg.usePCSpeaker=false;
  cfg.biosFile=new Faux86::EmbeddedDisk(pcxtbios_bin,pcxtbios_bin_len);
  cfg.romBasicFile=new Faux86::EmbeddedDisk(rombasic_bin,rombasic_bin_len);
  cfg.videoRomFile=new Faux86::EmbeddedDisk(videorom_bin,videorom_bin_len);
  cfg.asciiFile=new Faux86::EmbeddedDisk(asciivga_dat,asciivga_dat_len);
  cfg.setBootDrive("fd0");
  vga_framebuffer=(uint16_t*)ps_calloc(VGA_FRAMEBUFFER_SIZE,sizeof(uint16_t));
  if(!vga_framebuffer){fail("VGA allocation failed");return false;}
  host.video.allocate();
  vm=new Faux86::VM(cfg);
  if(!vm->init()){vm=nullptr;fail("VM init failed");return false;}
  // Standard BIOS warm-start flag; this BIOS may ignore it.
  vm->memory.writeWord(0x472,0x1234);
  host.init(vm);last=millis();lastInstructions=vm->cpu.totalexec;Serial.println("DOS VM READY");return true;
}
void dos_tick(){
  if(!vm)return;
  if(!launcher)serialInput();
  static uint32_t maxSlice=0;
  if(!launcher){uint32_t t=micros();vm->simulate();uint32_t elapsed=micros()-t;if(elapsed>maxSlice)maxSlice=elapsed;++steps;}
  if(millis()-promptCheck>=100){
   promptCheck=millis();bool prompt=atPrompt();
   if(launchPending){if(!prompt)promptGone=true;if(promptGone||millis()-launchTime>15000)launchPending=false;}
   if(prompt&&!launchPending&&!launcher&&millis()-launchTime>1000)openLauncher();
  }
  if(millis()-last>=3000){
    uint32_t elapsed=millis()-last;
    uint64_t nowInstructions=vm->cpu.totalexec;
    Serial.printf("DOS PERF steps=%u ips=%.0f display=%.1f fps blit=%u us skip=%u heap=%u psram=%u\n",steps,(nowInstructions-lastInstructions)*1000.0/elapsed,host.video.frames*1000.0/elapsed,host.video.frames?host.video.work/host.video.frames:0,host.video.skipped,ESP.getFreeHeap(),ESP.getFreePsram());
    Serial.printf("DOS SLICE max_us=%u launcher=%d\n",maxSlice,(int)launcher);maxSlice=0;
    lastInstructions=nowInstructions;host.video.skipped=0;
    host.video.frames=host.video.work=steps=0;last=millis();
  }
  static uint32_t lastYield=0;
  if(launcher||millis()-lastYield>=12){lastYield=millis();delay(1);}
}
