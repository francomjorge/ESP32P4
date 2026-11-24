#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
int core_start(const uint8_t *rom, size_t bytes, uint8_t *ram, size_t ram_bytes, uint8_t *frame);
void core_frame(uint8_t pressed,bool present);
uint8_t *core_video(int *w,int *h,int *pitch,uint16_t **pal);
#ifdef __cplusplus
}
#endif
