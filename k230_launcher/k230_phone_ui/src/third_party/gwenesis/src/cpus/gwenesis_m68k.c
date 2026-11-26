#include "gwenesis_m68k.h"

#include "gwenesis_savestate.h"

#include <stdint.h>
#include <stdlib.h>

#define M68K_MASTER_CYCLES_PER_CPU_CYCLE 7U

static unsigned int master_clock;
static unsigned int run_target;
static unsigned int run_base;
static unsigned int irq_level;
static int running;

static unsigned int current_master_clock(void)
{
    if(running) {
        int cpu_cycles = m68k_cycles_run();
        if(cpu_cycles > 0) {
            return run_base + (unsigned int)cpu_cycles *
                M68K_MASTER_CYCLES_PER_CPU_CYCLE;
        }
    }
    return master_clock;
}

void gwenesis_m68k_init(void)
{
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    master_clock = 0;
    run_target = 0;
    run_base = 0;
    irq_level = 0;
    running = 0;
}

void gwenesis_m68k_reset(void)
{
    m68k_pulse_reset();
    master_clock = 0;
    run_target = 0;
    run_base = 0;
    irq_level = 0;
    running = 0;
}

void m68k_run(unsigned int target)
{
    unsigned int delta;
    int budget;
    int executed;

    if(master_clock >= target) {
        return;
    }

    delta = target - master_clock;
    budget = (int)((delta + M68K_MASTER_CYCLES_PER_CPU_CYCLE - 1U) /
                   M68K_MASTER_CYCLES_PER_CPU_CYCLE);
    run_base = master_clock;
    run_target = target;
    running = 1;
    executed = m68k_execute(budget);
    running = 0;
    if(executed > 0) {
        master_clock += (unsigned int)executed *
            M68K_MASTER_CYCLES_PER_CPU_CYCLE;
    }
}

int m68k_cycles_master(void)
{
    return (int)current_master_clock();
}

int gwenesis_m68k_cycles_run(void)
{
    unsigned int current = current_master_clock();

    if(!running || current >= run_target) {
        return 0;
    }
    return (int)(run_target - current);
}

void gwenesis_m68k_set_irq(unsigned int level)
{
    irq_level = level & 7U;
    m68k_set_irq(irq_level);
}

void m68k_update_irq(unsigned int mask)
{
    irq_level = (irq_level | mask) & 7U;
    m68k_set_irq(irq_level);
}

void gwenesis_m68k_end_frame(unsigned int frame_master_cycles)
{
    if(master_clock >= frame_master_cycles) {
        master_clock -= frame_master_cycles;
    } else {
        master_clock = 0;
    }
}

void gwenesis_m68k_save_state(void)
{
    unsigned int context_size = m68k_context_size();
    void *context = malloc(context_size);
    SaveState *state;

    if(!context) {
        return;
    }
    m68k_get_context(context);
    state = saveGwenesisStateOpenForWrite("m68k");
    saveGwenesisStateSetBuffer(state, "context", context, (int)context_size);
    saveGwenesisStateSet(state, "master_clock", (int)master_clock);
    saveGwenesisStateSet(state, "irq_level", (int)irq_level);
    free(context);
}

void gwenesis_m68k_load_state(void)
{
    unsigned int context_size = m68k_context_size();
    void *context = malloc(context_size);
    SaveState *state;

    if(!context) {
        return;
    }
    state = saveGwenesisStateOpenForRead("m68k");
    saveGwenesisStateGetBuffer(state, "context", context, (int)context_size);
    m68k_set_context(context);
    master_clock = (unsigned int)saveGwenesisStateGet(state, "master_clock");
    irq_level = (unsigned int)saveGwenesisStateGet(state, "irq_level") & 7U;
    run_target = master_clock;
    run_base = master_clock;
    running = 0;
    m68k_set_irq(irq_level);
    free(context);
}
