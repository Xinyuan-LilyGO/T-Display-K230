#ifndef GWENESIS_M68K_H
#define GWENESIS_M68K_H

#include "m68k.h"

#ifdef __cplusplus
extern "C" {
#endif

void gwenesis_m68k_init(void);
void gwenesis_m68k_reset(void);
void gwenesis_m68k_set_irq(unsigned int level);
void gwenesis_m68k_end_frame(unsigned int master_cycles);
int gwenesis_m68k_cycles_run(void);

void m68k_run(unsigned int master_cycles);
int m68k_cycles_master(void);
void m68k_update_irq(unsigned int mask);

void gwenesis_m68k_save_state(void);
void gwenesis_m68k_load_state(void);

#ifdef __cplusplus
}
#endif

#endif
