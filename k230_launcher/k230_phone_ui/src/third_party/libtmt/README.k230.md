# libtmt for K230 Phone UI

Vendored from `https://github.com/deadpixi/libtmt`.

- Upstream commit: `1da7ba96c459672142949a529e65d730cc5bd6a1`
- Files kept: `tmt.c`, `tmt.h`, `README.rst`
- License: BSD-style license included at the top of `tmt.c` and `tmt.h`

This copy is used by the K230 Phone UI Terminal app as the ANSI terminal
state machine. The LVGL app owns the PTY backend and renders the libtmt screen
buffer.
