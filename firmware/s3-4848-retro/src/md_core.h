#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void md_set_z80(bool enabled);
void md_set_audio_test(bool enabled);
int md_start(uint8_t *rom,size_t bytes,uint16_t *out);
void md_frame(uint8_t keys,bool present);
uint8_t *md_video(int *w,int *h,int *pitch,uint16_t **palette);
bool md_video_can_flip(void);
void md_video_flip(void);
unsigned md_period(void);
void md_stop(void);
#ifdef __cplusplus
}
#endif
