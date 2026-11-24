/* PRISMA adapter for Gwenesis, derived from Retro-Go pinned upstream frame loop.
 * GPL/AGPL notices and source retained in corresponding source archive.
 */
#include "md_core.h"
#include <stdlib.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#pragma GCC optimize ("O2")
static unsigned profileFrames,profileDraws;
static uint64_t profileCPU,profileVDP,profileScale;
#include "gwenesis.h"
extern unsigned char *VRAM;
extern unsigned char *ROM_DATA;
extern unsigned char gwenesis_vdp_regs[0x20];
extern unsigned short gwenesis_vdp_status, CRAM565[256];
extern int screen_width,screen_height,hint_pending;
int system_clock,scan_line;
int16_t gwenesis_sn76489_buffer[2048],gwenesis_ym2612_buffer[2048];
int sn76489_index,sn76489_clock,ym2612_index,ym2612_clock;
static uint8_t *video,*spareVideo;
#define MD_VIDEO_BYTES (320*241+320)
static int videoWidth=320,videoHeight=224;
static bool active;
static bool z80Enabled=true,audioTest=true;
int prisma_audio_timers_only=0;
void md_set_audio_timer_only(bool enabled){if(!active)prisma_audio_timers_only=enabled?1:0;}
static uint64_t audioSamples,audioAbsSum,audioEndUs;
static unsigned audioPeak,audioFrames;
static int16_t pcmMix[2048];
void md_set_audio_test(bool enabled){if(!active)audioTest=enabled;}
static uint64_t profile68k,profileZ80;
void md_set_z80(bool enabled){if(!active)z80Enabled=enabled;}
static unsigned period=16667;
static uint32_t diagFrames,diagPCChanges,diagVideoChanges,diagRendered,diagPC,diagHash;
static uint8_t diagKeys;
void md_diag(void){
  if(!active)return;
  printf("MD STATE frames=%u pc=%06x sr=%04x pcChanges=%u rendered=%u imageChanges=%u hash=%08x vdp=%04x keys=%02x PCM=%u\n", (unsigned)diagFrames,(unsigned)diagPC,(unsigned)m68k_get_reg(M68K_REG_SR),(unsigned)diagPCChanges,(unsigned)diagRendered,(unsigned)diagVideoChanges,(unsigned)diagHash,(unsigned)gwenesis_vdp_status,(unsigned)diagKeys,(unsigned)audioTest);
  diagFrames=diagPCChanges=diagVideoChanges=diagRendered=0;
}
void gwenesis_io_get_buttons(void){}
SaveState* saveGwenesisStateOpenForRead(const char* f){(void)f;return 0;}
SaveState* saveGwenesisStateOpenForWrite(const char* f){(void)f;return 0;}
int saveGwenesisStateGet(SaveState* s,const char* t){(void)s;(void)t;return 0;}
void saveGwenesisStateSet(SaveState* s,const char* t,int v){(void)s;(void)t;(void)v;}
void saveGwenesisStateGetBuffer(SaveState* s,const char* t,void* b,int n){(void)s;(void)t;(void)b;(void)n;}
void saveGwenesisStateSetBuffer(SaveState* s,const char* t,void* b,int n){(void)s;(void)t;(void)b;(void)n;}
int md_start(uint8_t *rom,size_t bytes,uint16_t *frame){
  if(bytes<512 || bytes>0x400000 || (bytes&1) || memcmp(rom+0x100,"SEGA",4))return -20;
  VRAM=heap_caps_calloc(1,VRAM_MAX_SIZE,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  if(!VRAM)VRAM=heap_caps_calloc(1,VRAM_MAX_SIZE,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  video=heap_caps_calloc(1,320*241+320,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  if(!video)video=heap_caps_calloc(1,320*241+320,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(!VRAM||!video){md_stop();return -21;}
  // Second framebuffer stays independent until LCD worker completes.
  // Do not exhaust internal heap just to allocate the second framebuffer.
  spareVideo=heap_caps_calloc(1,MD_VIDEO_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  printf("MD doublebuffer=%s\n",spareVideo?"ready":"copy fallback");
  load_cartridge(rom,bytes);
  power_on();reset_emulation();
  (void)frame;active=true;period=16667;
  profileFrames=profileDraws=0;profileCPU=profileVDP=profileScale=profile68k=profileZ80=0;
  audioSamples=audioAbsSum=audioEndUs=0;audioPeak=audioFrames=0;
  diagFrames=diagPCChanges=diagVideoChanges=diagRendered=diagPC=diagHash=0;diagKeys=0;
  printf("MD PROFILE Z80=%s PCM=%s FM=%s output=NONE\n",z80Enabled?"ON":"OFF",audioTest?"ON":"OFF",prisma_audio_timers_only?"TIMERS":"SYNTH");
  printf("MD buffers VRAM=%p video=%p free_internal=%u\n",VRAM,video,(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  return 0;
}
void md_frame(uint8_t k,bool present){
  if(!active)return;
  // Existing touch A/B/SELECT -> Sega A/B/C; START and dpad unchanged.
  static const uint8_t map[8]={64,128,32,16,2,4,1,8};
  for(int i=0;i<8;i++){if(k&map[i])gwenesis_io_pad_press_button(0,i);else gwenesis_io_pad_release_button(0,i);}
  int lines=REG1_PAL?LINES_PER_FRAME_PAL:LINES_PER_FRAME_NTSC;
  period=REG1_PAL?20000:16667;
  screen_width=REG12_MODE_H40?320:256;screen_height=REG1_PAL?240:224;
  if(present){videoWidth=screen_width;videoHeight=screen_height;}
  int hint_counter=gwenesis_vdp_regs[10];
  if(present)memset(video,0,320*241+320);
  gwenesis_vdp_set_buffer((unsigned short*)(video+160));
  gwenesis_vdp_render_config();
  system_clock=0;zclk=z80Enabled?0:0x1000000;
  // Keep Z80 CPU active, mute sample synthesis for this first performance build.
  ym2612_clock=sn76489_clock=audioTest?0:0x1000000;
  ym2612_index=sn76489_index=0;
  scan_line=0;
  uint64_t begin=esp_timer_get_time(),vdp_us=0,cpu68_us=0,z80_us=0;
  const bool measure=(profileFrames%7==0);
  while(scan_line<lines){
    if(measure){
      uint64_t t=esp_timer_get_time();
      m68k_run(system_clock+VDP_CYCLES_PER_LINE);
      uint64_t mid=esp_timer_get_time();
      if(z80Enabled)z80_run(system_clock+VDP_CYCLES_PER_LINE);
      cpu68_us+=mid-t;z80_us+=esp_timer_get_time()-mid;
    }else{
      m68k_run(system_clock+VDP_CYCLES_PER_LINE);
      if(z80Enabled)z80_run(system_clock+VDP_CYCLES_PER_LINE);
    }
    if(present&&scan_line<screen_height){
      if(measure){uint64_t t=esp_timer_get_time();gwenesis_vdp_render_line(scan_line);vdp_us+=esp_timer_get_time()-t;}
      else gwenesis_vdp_render_line(scan_line);
    }
    if(scan_line==0||scan_line>screen_height)hint_counter=REG10_LINE_COUNTER;
    if(--hint_counter<0){
      if(REG0_LINE_INTERRUPT&&scan_line<=screen_height){
        hint_pending=1;
        if(!(gwenesis_vdp_status&STATUS_VIRQPENDING))m68k_update_irq(4);
      }
      hint_counter=REG10_LINE_COUNTER;
    }
    scan_line++;
    if(scan_line==screen_height){
      if(REG1_VBLANK_INTERRUPT){gwenesis_vdp_status|=STATUS_VIRQPENDING;m68k_set_irq(6);}
      z80_irq_line(1);
    }
    if(scan_line==screen_height+1)z80_irq_line(0);
    system_clock+=VDP_CYCLES_PER_LINE;
  }
  if(audioTest){
    uint64_t endStart=esp_timer_get_time();
    // Same accurate register-write synchronization as Retro-Go; complete both chips to frame end.
    ym2612_run(system_clock);
    gwenesis_SN76489_run(system_clock);
    int n=ym2612_index<sn76489_index?ym2612_index:sn76489_index;
    if(n<0)n=0;if(n>2048)n=2048;
    if(!prisma_audio_timers_only)for(int i=0;i<n;i++){
      int v=((int)gwenesis_ym2612_buffer[i]+(int)gwenesis_sn76489_buffer[i])/2;
      pcmMix[i]=(int16_t)v;
      unsigned mag=(unsigned)(v<0?-v:v);
      if(mag>audioPeak)audioPeak=mag;
      audioAbsSum+=mag;
    }
    audioSamples+=n;audioFrames++;
    audioEndUs+=esp_timer_get_time()-endStart;
  }
  m68k.cycles-=system_clock;
  // Sample guest CPU progress and image content independently of LCD presentation.
  uint32_t pc=m68k_get_reg(M68K_REG_PC);
  if(diagFrames&&pc!=diagPC)diagPCChanges++;
  diagPC=pc;diagKeys=k;diagFrames++;
  if(present){
    uint32_t hash=2166136261u;
    for(int y=0;y<videoHeight;y+=16)for(int x=0;x<videoWidth;x+=16){
      unsigned color=CRAM565[video[160+y*320+x]];
      hash=(hash^color)*16777619u;
    }
    if(diagRendered&&hash!=diagHash)diagVideoChanges++;
    diagHash=hash;diagRendered++;
  }
  uint64_t core_us=esp_timer_get_time()-begin;
  if(measure){profileCPU+=core_us-vdp_us;profileVDP+=vdp_us;profile68k+=cpu68_us;profileZ80+=z80_us;if(present)profileDraws++;}
  if(audioFrames>=126){
    printf("MD AUDIO %s samples/frame=%u peak=%u meanabs=%u finalize_mix=%u us output=NONE\n",prisma_audio_timers_only?"TIMERS":"PCM",
      (unsigned)(audioSamples/audioFrames),audioPeak,(unsigned)(audioSamples?audioAbsSum/audioSamples:0),(unsigned)(audioEndUs/audioFrames));
    audioSamples=audioAbsSum=audioEndUs=0;audioPeak=audioFrames=0;
  }
  if(++profileFrames>=126){printf("GEN_PROF cpu=%u us vdp=%u us scale=%u us samples=%u drawn=%u\n",(unsigned)(profileCPU/18),(unsigned)(profileDraws?profileVDP/profileDraws:0),(unsigned)(profileDraws?profileScale/profileDraws:0),18,profileDraws);printf("MD CPU 68k=%u us z80=%u us mode=%s (68k includes bus sync)\n",(unsigned)(profile68k/18),(unsigned)(profileZ80/18),z80Enabled?"ON":"OFF");profileFrames=profileDraws=0;profileCPU=profileVDP=profileScale=profile68k=profileZ80=0;}
}
uint8_t *md_video(int *w,int *h,int *pitch,uint16_t **palette){
  *w=videoWidth;*h=videoHeight;*pitch=320;*palette=CRAM565;
  return active?video+160:NULL;
}
bool md_video_can_flip(void){return active&&video&&spareVideo;}
void md_video_flip(void){if(md_video_can_flip()){uint8_t *old=video;video=spareVideo;spareVideo=old;}}
unsigned md_period(void){return period;}
void md_stop(void){active=false;free(VRAM);free(video);free(spareVideo);VRAM=video=spareVideo=NULL;ROM_DATA=NULL;}
