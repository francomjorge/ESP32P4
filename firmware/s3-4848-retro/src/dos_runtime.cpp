#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_partition.h>
#include <Wire.h>
#include <Adafruit_Faux86.h>
#include <esp_timer.h>

static Arduino_RGB_Display *dosLcd=nullptr;
uint16_t *vga_framebuffer=nullptr;

class PrismaVideo final : public Faux86::FrameBufferInterface {
  Faux86::RenderSurface surface{};
  uint16_t row[480];
public:
  uint32_t frames=0, work=0;
  Faux86::RenderSurface *getSurface() override {return &surface;}
  void setPalette(Faux86::Palette*) override {}
  void blit(uint16_t *pixels,int w,int h,int stride) override {
    if(!pixels || w<=0 || h<=0 || w>VGA_FRAMEBUFFER_WIDTH || h>VGA_FRAMEBUFFER_HEIGHT || stride<w*2) return;
    uint32_t t=micros();
    int dw=min(w,480),dh=min(h,480);
    for(int y=0;y<dh;y++){
      const uint16_t *src=(const uint16_t*)((const uint8_t*)pixels+(y*h/dh)*stride);
      for(int x=0;x<dw;x++) row[x]=src[x*w/dw];
      dosLcd->draw16bitRGBBitmap((480-dw)/2,(480-dh)/2+y,row,dw,1);
    }
    dosLcd->flush(true); ++frames; work+=micros()-t;
  }
};
class PrismaHost final : public Faux86::ArduinoHostSystemInterface {
public:
  PrismaVideo video;
  PrismaHost():ArduinoHostSystemInterface((Adafruit_SPITFT*)nullptr){}
  Faux86::FrameBufferInterface &getFrameBuffer() override {return video;}
  void resize(uint32_t,uint32_t) override {dosLcd->fillScreen(0);}
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
 Serial.println("BUILD 032C DOS");
  const auto *part=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_ANY,"dosdata");
  DiskHeader h{};
  if(!part||esp_partition_read(part,0,&h,sizeof(h))!=ESP_OK||memcmp(h.magic,"PRDOS032",8)){fail("Missing DOS data");return false;}
  cfg.diskDriveA=new FlashDisk(part,h.floppyOffset,h.floppySize);
  if(!cfg.diskDriveA->isValid()){fail("Invalid boot disk");return false;}
  if(h.hardSize){cfg.diskDriveC=new FlashDisk(part,h.hardOffset,h.hardSize);if(!cfg.diskDriveC->isValid()){fail("Invalid game disk");return false;}}
  cfg.singleThreaded=true;cfg.slowSystem=false;cfg.cpuSpeed=12;
  cfg.frameDelay=66;cfg.framebuffer.width=VGA_FRAMEBUFFER_WIDTH;cfg.framebuffer.height=VGA_FRAMEBUFFER_HEIGHT;
  cfg.enableAudio=false;cfg.useDisneySoundSource=false;cfg.useSoundBlaster=false;cfg.useAdlib=false;cfg.usePCSpeaker=false;
  cfg.biosFile=new Faux86::EmbeddedDisk(pcxtbios_bin,pcxtbios_bin_len);
  cfg.romBasicFile=new Faux86::EmbeddedDisk(rombasic_bin,rombasic_bin_len);
  cfg.videoRomFile=new Faux86::EmbeddedDisk(videorom_bin,videorom_bin_len);
  cfg.asciiFile=new Faux86::EmbeddedDisk(asciivga_dat,asciivga_dat_len);
  cfg.setBootDrive("fd0");
  vga_framebuffer=(uint16_t*)ps_calloc(VGA_FRAMEBUFFER_SIZE,sizeof(uint16_t));
  if(!vga_framebuffer){fail("VGA allocation failed");return false;}
  vm=new Faux86::VM(cfg);
  if(!vm->init()){vm=nullptr;fail("VM init failed");return false;}
  host.init(vm);last=millis();Serial.println("DOS VM READY");return true;
}
void dos_tick(){
  if(!vm)return;
  serialInput();vm->simulate();++steps;
  if(millis()-last>=3000){
    uint32_t elapsed=millis()-last;
    Serial.printf("DOS PERF steps=%u display=%.1f fps blit=%u us heap=%u psram=%u\n",steps,host.video.frames*1000.0/elapsed,host.video.frames?host.video.work/host.video.frames:0,ESP.getFreeHeap(),ESP.getFreePsram());
    host.video.frames=host.video.work=steps=0;last=millis();
  }
  delay(1);
}
