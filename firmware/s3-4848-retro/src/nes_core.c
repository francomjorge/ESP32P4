#include "nes_core.h"
#include "nofrendo.h"
#include <esp_heap_caps.h>
static uint8_t *video;
static uint16_t *palette,*out;
static bool active;
int nes_core_start(uint8_t *rom,size_t bytes,uint16_t *frame){
  if(bytes<16 || memcmp(rom,"NES\x1a",4) || (rom[7]&12)==8) return -10;
  if((size_t)16+((rom[6]&4)?512:0)+(size_t)rom[4]*16384+(size_t)rom[5]*8192>bytes)return -11;
  video=heap_caps_calloc(NES_SCREEN_PITCH,NES_SCREEN_HEIGHT,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  palette=nofrendo_buildpalette(NES_PALETTE_NESCLASSIC,16);
  if(!video||!palette){nes_core_stop();return -12;}
  if(!nes_init(SYS_NES_NTSC,22050,false,NULL)){nes_core_stop();return -13;}
  if(nes_insertcart(rom_loadmem(rom,bytes))<0){nes_core_stop();return -14;}
  active=true;out=frame;nes_setvidbuf(video);return 0;
}
void nes_core_frame(uint8_t k){
  if(!active)return;
  uint8_t p=k&15;
  if(k&16)p|=NES_PAD_RIGHT;if(k&32)p|=NES_PAD_LEFT;
  if(k&64)p|=NES_PAD_UP;if(k&128)p|=NES_PAD_DOWN;
  input_update(0,p);nes_emulate(true);
  for(unsigned y=0;y<288;y++){
    uint8_t *line=NES_SCREEN_GETPTR(video,0,y*240/288);
    for(unsigned x=0;x<320;x++)out[y*320+x]=palette[line[x*256/320]];
  }
}
void nes_core_stop(void){
  if(active){nes_shutdown();active=false;}
  free(video);free(palette);video=NULL;palette=NULL;out=NULL;
}
