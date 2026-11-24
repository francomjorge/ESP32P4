#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
int md_start(uint8_t *rom,size_t bytes,uint16_t *out);
void md_frame(uint8_t keys,bool present);
unsigned md_period(void);
void md_stop(void);
#ifdef __cplusplus
}
#endif
