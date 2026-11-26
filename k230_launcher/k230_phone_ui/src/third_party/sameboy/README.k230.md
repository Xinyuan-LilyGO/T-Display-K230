# SameBoy core integration

This directory contains the portable SameBoy core from upstream tag `v1.0.5`
at commit `ea667ad79aa28409f034b1fd6da7304e1844750d`.

Upstream: <https://github.com/LIJI32/SameBoy>

Only the core sources required by the launcher are compiled. The `dmg_boot.c`
and `cgb_boot.c` files are generated from SameBoy's open-source boot ROM
sources at that tag. Desktop, mobile, and libretro frontend code is excluded.
The upstream Expat license is preserved in `LICENSE`.
