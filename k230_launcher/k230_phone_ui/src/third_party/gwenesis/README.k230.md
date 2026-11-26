# Gwenesis K230 integration

This directory is based on the Gwenesis snapshot at commit `168e466` carried
by `ducalex/retro-go` commit `4ced120669750ca7228fd0414211430c1d923166`.

K230-specific changes:

- The display renderer writes RGB565 pixels into a Linux/LVGL frame buffer.
- SMD deinterleaving bounds are corrected before processing 16 KiB blocks.
- The bundled non-commercial Z80 implementation is replaced by the
  zlib-licensed `floooh/chips` Z80 core at commit
  `7ef8776f637a6424ac615da64de6b968b06de791`.
- The bundled non-commercial Musashi snapshot is replaced by the MIT-licensed
  upstream Musashi core at commit
  `313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd`, with a Gwenesis timing adapter.

See `LICENSE` for Gwenesis licensing and `../chips/LICENSE` for the replacement
Z80 core license. See `../musashi/LICENSE` for the replacement 68000 core
license and `../musashi/softfloat/README.txt` for its bundled SoftFloat notice.
ROM images are not included.
