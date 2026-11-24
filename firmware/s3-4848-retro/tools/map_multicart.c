/* SPDX-License-Identifier: LGPL-2.0-only
 * Address-latch multicarts. Register behavior: NESdev INES Mapper 225/227.
 */
#include "nes/nes.h"
static uint8 regs225[4];
static uint8 read225(uint32 a) {return regs225[a&3];}
static void ram225(uint32 a,uint8 v) {regs225[a&3]=v&15;}
static void write225(uint32 a,uint8 v) {
    (void)v;
    unsigned hi=(a&0x4000)>>8;
    unsigned bank=((a>>6)&63)|hi;
    unsigned low=(a&0x1000)?bank:(bank&~1u);
    unsigned high=(a&0x1000)?bank:(low+1);
    mmc_bankrom(16,0x8000,low);
    mmc_bankrom(16,0xc000,high);
    mmc_bankvrom(8,0,(a&63)|hi);
    ppu_setmirroring((a&0x2000)?PPU_MIRROR_HORI:PPU_MIRROR_VERT);
}
static void init225(rom_t *cart) {(void)cart;memset(regs225,0,sizeof(regs225));write225(0x8000,0);}
mapintf_t map225_intf = {
    .number=225,.name="BMC 64-in-1",.init=init225,
    .mem_read={{0x5800,0x5fff,read225}},
    .mem_write={{0x5800,0x5fff,ram225},{0x8000,0xffff,write225}},
};
static bool protect227;
static void write227(uint32 a,uint8 v) {
    (void)v;
    unsigned b=((a>>2)&31)|((a&0x100)>>3);
    bool pair=(a&1)!=0, nrom=(a&0x80)!=0;
    unsigned low=pair?(b&~1u):b;
    unsigned high=nrom?(pair?low+1:low):((b&~7u)|((a&0x200)?7:0));
    mmc_bankrom(16,0x8000,low);
    mmc_bankrom(16,0xc000,high);
    ppu_setmirroring((a&2)?PPU_MIRROR_HORI:PPU_MIRROR_VERT);
    ppu_setchrwrite(!protect227||!nrom);
}
static void init227(rom_t *cart) {
    protect227=!cart->battery;
    mmc_bankchr(8,0,0,CHR_ANY);
    write227(0x8000,0);
}
mapintf_t map227_intf = {
    .number=227,.name="BMC 1200-in-1",.init=init227,
    .mem_write={{0x8000,0xffff,write227}},
};
