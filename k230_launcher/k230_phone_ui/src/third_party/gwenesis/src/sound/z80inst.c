/*
 * K230 Gwenesis Z80 adapter.
 *
 * The retro-go Gwenesis snapshot bundled a Z80 core restricted to
 * non-commercial use. This adapter uses floooh/chips instead, under the
 * zlib/libpng license, while preserving the Gwenesis bus interface.
 */
#include <stdint.h>

#define CHIPS_IMPL
#include "z80.h"

#include "gwenesis_bus.h"
#include "gwenesis_savestate.h"
#include "gwenesis_sn76489.h"
#include "gwenesis_m68k.h"
#include "ym2612.h"
#include "z80inst.h"

static int bus_ack;
static int reset_asserted;
static int reset_released;
static int irq_asserted;
static int initialized;
static int current_clock;
static unsigned char *z80_ram;
static uint64_t z80_pins;
static z80_t z80_cpu;

int zclk;
int Z80_BANK;

static unsigned int zbank_mem_r8(unsigned int address)
{
    address = (address & 0x7FFFU) | ((unsigned int)Z80_BANK << 15);
    return m68k_read_memory_8(address);
}

static void zbank_mem_w8(unsigned int address, unsigned int value)
{
    address = (address & 0x7FFFU) | ((unsigned int)Z80_BANK << 15);
    m68k_write_memory_8(address, value);
}

static unsigned int z80_memory_read(unsigned int address)
{
    if(address < 0x4000U) {
        return z80_ram[address & 0x1FFFU];
    }
    if(address < 0x6000U) {
        return YM2612Read(current_clock);
    }
    if(address >= 0x8000U) {
        return zbank_mem_r8(address);
    }
    return 0xFFU;
}

static void z80_memory_write(unsigned int address, unsigned int value)
{
    if(address < 0x4000U) {
        z80_ram[address & 0x1FFFU] = (unsigned char)value;
    } else if(address < 0x6000U) {
        YM2612Write(address & 3U, value, current_clock);
    } else if(address == 0x6000U) {
        Z80_BANK = (Z80_BANK >> 1) | ((value & 1U) << 8);
    } else if(address == 0x7F11U) {
        gwenesis_SN76489_Write((int)value, current_clock);
    } else if(address >= 0x8000U) {
        zbank_mem_w8(address, value);
    }
}

void z80_start(void)
{
    z80_pins = z80_init(&z80_cpu);
    bus_ack = 0;
    reset_asserted = 1;
    reset_released = 0;
    irq_asserted = 0;
    Z80_BANK = 0;
    zclk = 0;
}

void z80_pulse_reset(void)
{
    z80_pins = z80_reset(&z80_cpu);
    irq_asserted = 0;
}

void z80_set_memory(unsigned char *buffer)
{
    z80_ram = buffer;
    initialized = buffer != NULL;
}

void z80_run(int target)
{
    int cycles;

    if(!initialized || zclk >= target) {
        return;
    }
    cycles = (target - zclk) / Z80_FREQ_DIVISOR;
    if(!reset_released || bus_ack) {
        zclk = target;
        return;
    }

    for(int cycle = 0; cycle < cycles; cycle++) {
        z80_pins = z80_tick(&z80_cpu, z80_pins);
        current_clock = zclk + (cycle + 1) * Z80_FREQ_DIVISOR;

        if(z80_pins & Z80_MREQ) {
            unsigned int address = Z80_GET_ADDR(z80_pins);
            if(z80_pins & Z80_RD) {
                Z80_SET_DATA(z80_pins, z80_memory_read(address));
            } else if(z80_pins & Z80_WR) {
                z80_memory_write(address, Z80_GET_DATA(z80_pins));
            }
        } else if(z80_pins & Z80_IORQ) {
            if(z80_pins & Z80_M1) {
                Z80_SET_DATA(z80_pins, 0xFF);
            } else if(z80_pins & Z80_RD) {
                Z80_SET_DATA(z80_pins, 0xFF);
            }
        }

        if(irq_asserted) {
            z80_pins |= Z80_INT;
        } else {
            z80_pins &= ~Z80_INT;
        }
    }
    zclk += cycles * Z80_FREQ_DIVISOR;
}

static void z80_sync(void)
{
    z80_run((int)m68k_cycles_master());
}

void z80_write_ctrl(unsigned int address, unsigned int value)
{
    z80_sync();
    if(address == 0x1100U) {
        bus_ack = value ? 1 : 0;
    } else if(address == 0x1200U) {
        if(value == 0) {
            reset_asserted = 1;
        } else {
            z80_pulse_reset();
            reset_asserted = 0;
            reset_released = 1;
        }
    }
}

unsigned int z80_read_ctrl(unsigned int address)
{
    z80_sync();
    if(address == 0x1100U) {
        return bus_ack ? 0U : 1U;
    }
    if(address == 0x1200U) {
        return reset_asserted ? 1U : 0U;
    }
    if(address == 0x1101U || address == 0x1201U) {
        return 0U;
    }
    return 0xFFU;
}

void z80_irq_line(unsigned int value)
{
    if(reset_released) {
        irq_asserted = value ? 1 : 0;
    }
}

void z80_write_memory_8(unsigned int address, unsigned int value)
{
    z80_memory_write(address, value);
}

void z80_write_memory_16(unsigned int address, unsigned int value)
{
    z80_memory_write(address, value >> 8);
    z80_memory_write(address + 1, value);
}

unsigned int z80_read_memory_8(unsigned int address)
{
    return z80_memory_read(address);
}

unsigned int z80_read_memory_16(unsigned int address)
{
    return (z80_memory_read(address) << 8) | z80_memory_read(address + 1);
}

void gwenesis_z80inst_save_state(void)
{
    SaveState *state = saveGwenesisStateOpenForWrite("z80inst");
    saveGwenesisStateSetBuffer(state, "cpu", &z80_cpu, sizeof(z80_cpu));
    saveGwenesisStateSetBuffer(state, "pins", &z80_pins, sizeof(z80_pins));
    saveGwenesisStateSet(state, "bus_ack", bus_ack);
    saveGwenesisStateSet(state, "reset_asserted", reset_asserted);
    saveGwenesisStateSet(state, "reset_released", reset_released);
    saveGwenesisStateSet(state, "irq_asserted", irq_asserted);
    saveGwenesisStateSet(state, "zclk", zclk);
    saveGwenesisStateSet(state, "Z80_BANK", Z80_BANK);
}

void gwenesis_z80inst_load_state(void)
{
    SaveState *state = saveGwenesisStateOpenForRead("z80inst");
    saveGwenesisStateGetBuffer(state, "cpu", &z80_cpu, sizeof(z80_cpu));
    saveGwenesisStateGetBuffer(state, "pins", &z80_pins, sizeof(z80_pins));
    bus_ack = saveGwenesisStateGet(state, "bus_ack");
    reset_asserted = saveGwenesisStateGet(state, "reset_asserted");
    reset_released = saveGwenesisStateGet(state, "reset_released");
    irq_asserted = saveGwenesisStateGet(state, "irq_asserted");
    zclk = saveGwenesisStateGet(state, "zclk");
    Z80_BANK = saveGwenesisStateGet(state, "Z80_BANK");
}
