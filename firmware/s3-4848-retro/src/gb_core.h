#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int core_start(const uint8_t *rom, size_t bytes, uint8_t *ram, size_t ram_bytes, uint16_t *frame);
void core_frame(uint8_t pressed);
#ifdef __cplusplus
}
#endif
