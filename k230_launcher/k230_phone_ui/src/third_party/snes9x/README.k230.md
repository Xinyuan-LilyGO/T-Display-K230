# Snes9x 2005 integration

This directory is imported from the Retro-Go Snes9x component at commit
`4ced120669750ca7228fd0414211430c1d923166`:

https://github.com/ducalex/retro-go/tree/4ced120669750ca7228fd0414211430c1d923166/retro-core/components/snes9x

K230-specific changes are intentionally small:

- `src/port.h` skips Retro-Go's `rg_system.h` when `K230_SNES9X_PORT` is set.
- The parent CMake file supplies standard C headers that Retro-Go normally
  pre-includes, and selects the portable unaligned-memory path.
- `ui_snes.cpp` implements the display, audio, controller, ROM, and SRAM port.

The complete upstream license is preserved in `src/LICENSE`. The Snes9x
portion permits personal, educational, and non-commercial use; review that
license before distributing this core in a commercial product.
