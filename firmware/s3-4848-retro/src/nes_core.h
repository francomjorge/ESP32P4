#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
int nes_core_start(uint8_t *rom,size_t bytes,uint16_t *frame);
void nes_core_frame(uint8_t keys, bool present);
uint8_t *nes_core_video(int *w,int *h,int *pitch,uint16_t **pal);
void nes_core_stop(void);
#ifdef __cplusplus
}
#endif
