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
int16_t gwenesis_sn76489_buffer[1100],gwenesis_ym2612_buffer[1100];
int sn76489_index,sn76489_clock,ym2612_index,ym2612_clock;
static uint8_t *video;
static int videoWidth=320,videoHeight=224;
static bool active;
static unsigned period=16667;
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
  load_cartridge(rom,bytes);
  power_on();reset_emulation();
  (void)frame;active=true;period=16667;
  profileFrames=profileDraws=0;profileCPU=profileVDP=profileScale=0;
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
  system_clock=0;zclk=0;
  // Keep Z80 CPU active, mute sample synthesis for this first performance build.
  ym2612_clock=sn76489_clock=0x1000000;
  ym2612_index=sn76489_index=0;
  scan_line=0;
  uint64_t begin=esp_timer_get_time(),vdp_us=0;
  const bool measure=(profileFrames%7==0);
  while(scan_line<lines){
    m68k_run(system_clock+VDP_CYCLES_PER_LINE);
    z80_run(system_clock+VDP_CYCLES_PER_LINE);
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
  m68k.cycles-=system_clock;
  uint64_t core_us=esp_timer_get_time()-begin;
  if(measure){profileCPU+=core_us-vdp_us;profileVDP+=vdp_us;if(present)profileDraws++;}
  if(++profileFrames>=126){printf("GEN_PROF cpu=%u us vdp=%u us scale=%u us samples=%u drawn=%u\n",(unsigned)(profileCPU/18),(unsigned)(profileDraws?profileVDP/profileDraws:0),(unsigned)(profileDraws?profileScale/profileDraws:0),18,profileDraws);profileFrames=profileDraws=0;profileCPU=profileVDP=profileScale=0;}
}
uint8_t *md_video(int *w,int *h,int *pitch,uint16_t **palette){
  *w=videoWidth;*h=videoHeight;*pitch=320;*palette=CRAM565;
  return active?video+160:NULL;
}
unsigned md_period(void){return period;}
void md_stop(void){active=false;free(VRAM);free(video);VRAM=video=NULL;ROM_DATA=NULL;}
