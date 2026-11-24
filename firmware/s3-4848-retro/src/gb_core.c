#include "gb_core.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#define ENABLE_SOUND 0
#include "peanut_gb.h"
static struct gb_s gb;
static const uint8_t *rom_data;
static size_t rom_size;
static uint8_t *save_ram;
static size_t save_size;
static uint8_t *pixels;
static bool render_frame=true;
static uint16_t palette[256] = {0xE78C,0x9D29,0x4B45,0x1922};
static uint8_t rom_read(struct gb_s *g, uint_fast32_t a) {
  (void)g; return a < rom_size ? rom_data[a] : 0xFF;
}
static uint8_t ram_read(struct gb_s *g, uint_fast32_t a) {
  (void)g; return a < save_size ? save_ram[a] : 0xFF;
}
static void ram_write(struct gb_s *g, uint_fast32_t a, uint8_t v) {
  (void)g; if(a < save_size) save_ram[a]=v;
}
static void core_error(struct gb_s *g, enum gb_error_e e, uint16_t address) {
  (void)g; printf("GB fatal: %d address=%04x\n",(int)e,(unsigned)address); abort();
}
static void lcd_line(struct gb_s *g, const uint8_t *line, uint_fast8_t y) {
  (void)g; if(y>=144||!render_frame) return;
  uint8_t *dst=pixels+(size_t)y*160;
  for(unsigned x=0;x<160;x++)dst[x]=line[x]&3;
}
int core_start(const uint8_t *rom, size_t bytes, uint8_t *ram, size_t ram_bytes, uint8_t *frame){
  memset(&gb,0,sizeof(gb));
  rom_data=rom; rom_size=bytes; save_ram=ram; save_size=ram_bytes; pixels=frame;
  int err=gb_init(&gb,rom_read,ram_read,ram_write,core_error,0);
  if(err!=GB_INIT_NO_ERROR) return err;
  size_t required=0;
  if(gb_get_save_size_s(&gb,&required)!=0 || required>save_size) return -2;
  gb_init_lcd(&gb,lcd_line);
  return 0;
}
void core_frame(uint8_t pressed,bool present){render_frame=present;gb.direct.joypad=(uint8_t)~pressed;gb_run_frame(&gb);}

uint8_t *core_video(int *w,int *h,int *pitch,uint16_t **pal){*w=160;*h=144;*pitch=160;*pal=palette;return pixels;}
