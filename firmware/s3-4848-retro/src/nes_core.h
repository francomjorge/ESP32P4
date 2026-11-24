#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
int nes_core_start(uint8_t *rom,size_t bytes,uint16_t *frame);
void nes_core_frame(uint8_t keys, bool present);
void nes_core_stop(void);
#ifdef __cplusplus
}
#endif
