#pragma once
#include <Arduino.h>
#ifdef PRISMA_COMBINED
void diag_begin();
void diag_logf(bool periodic,const char *fmt,...);
void diag_crashf(const char *fmt,...);
bool diag_consume(uint8_t c);
bool diag_command_pending();
void diag_poll();
void prisma_diag_status();
bool prisma_diag_bench();
#endif
